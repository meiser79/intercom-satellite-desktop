#!/usr/bin/env python3
"""A minimal Kiosk Satellite intercom on Linux, for testing Intercom Satellite.

It speaks the kiosk side of the intercom protocol: the identity probe, signed
call requests and signals, and the audio WebSocket (80 ms of PCM16 mono
16 kHz per frame). It announces itself over mDNS so the phone finds it, and
learns phones from their mDNS announcements and requests. Audio goes through
PulseAudio or PipeWire (pacat); --tone sends a test tone instead of the
microphone. Video calls send a camera, or a test pattern, through ffmpeg and
show the phone's camera in an ffplay window.

Standard library only. Type `help` at the prompt for the commands.
"""

import argparse
import base64
import hashlib
import hmac
import http.server
import json
import math
import os
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import uuid

VERSION = "kiosk-sim/1.0"
SERVICE = "_kiosk-satellite._tcp.local"
FRAME_BYTES = 2560  # 80 ms of 16 kHz mono PCM16
RING_SECONDS = 45


def log(*parts):
    print("\r" + " ".join(str(p) for p in parts), flush=True)
    print("> ", end="", flush=True)


# ---------------------------------------------------------------- tokens


class Wire:
    """Signed one-call tokens, as Wire.java on the phone."""

    def __init__(self, key):
        self.key = key
        self.used = {}

    def fingerprint(self):
        return hashlib.sha256(self.key.encode()).hexdigest()[:8] if self.key else ""

    def _sign(self, payload):
        mac = hmac.new(("intercom:" + self.key).encode(), payload.encode(), hashlib.sha256)
        return base64.urlsafe_b64encode(mac.digest()).decode()

    def token(self, call, sender):
        claims = {"intercom": call, "from": sender, "n": str(uuid.uuid4()),
                  "exp": int(time.time() * 1000) + 60000}
        payload = base64.urlsafe_b64encode(json.dumps(claims).encode()).decode()
        return payload + "." + self._sign(payload)

    def verify(self, token, call):
        try:
            payload, signature = token.split(".")
            if not hmac.compare_digest(self._sign(payload), signature):
                return None
            claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
            now = time.time() * 1000
            if not (now < claims["exp"] <= now + 90000) or claims.get("intercom") != call:
                return None
            self.used = {t: e for t, e in self.used.items() if e > now}
            if token in self.used or not claims.get("from"):
                return None
            self.used[token] = claims["exp"]
            return claims
        except Exception:
            return None


# ---------------------------------------------------------------- WebSocket


class Socket:
    """One WebSocket connection, either side; the client masks what it sends."""

    def __init__(self, sock, client, pending=b""):
        self.sock = sock
        self.client = client
        self.pending = pending  # what came after the handshake
        self.lock = threading.Lock()
        self.closed = False

    def send(self, opcode, data):
        if self.closed:
            return
        head = bytes([0x80 | opcode])
        mask = 0x80 if self.client else 0
        n = len(data)
        if n < 126:
            head += bytes([mask | n])
        elif n < 65536:
            head += bytes([mask | 126]) + struct.pack(">H", n)
        else:
            head += bytes([mask | 127]) + struct.pack(">Q", n)
        if self.client:
            key = os.urandom(4)
            head += key
            data = bytes(b ^ key[i % 4] for i, b in enumerate(data))
        try:
            with self.lock:
                self.sock.sendall(head + data)
        except OSError:
            self.closed = True

    def text(self, obj):
        self.send(0x1, json.dumps(obj).encode())

    def binary(self, data):
        self.send(0x2, data)

    def _exact(self, n):
        buf, self.pending = self.pending[:n], self.pending[n:]
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def receive(self):
        """The next (opcode, payload) of a text or binary message; None once closed."""
        message, kind = b"", None
        while True:
            try:
                b1, b2 = self._exact(2)
                n = b2 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._exact(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._exact(8))[0]
                key = self._exact(4) if b2 & 0x80 else None
                data = self._exact(n)
            except (EOFError, OSError):
                self.closed = True
                return None
            if key:
                data = bytes(b ^ key[i % 4] for i, b in enumerate(data))
            opcode = b1 & 0x0F
            if opcode == 0x8:
                self.close()
                return None
            if opcode == 0x9:
                self.send(0xA, data)
                continue
            if opcode == 0xA:
                continue
            if opcode in (0x1, 0x2):
                kind = opcode
            message += data
            if b1 & 0x80:
                return kind, message

    def close(self):
        if not self.closed:
            self.send(0x8, struct.pack(">H", 1000) + b"ended")
        self.closed = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


def accept_key(key):
    magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    return base64.b64encode(hashlib.sha1((key + magic).encode()).digest()).decode()


def connect_ws(host, port, path):
    sock = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(
        (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
         f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        .encode())
    response = b""
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(1024)
        if not chunk:
            raise OSError("connection closed during handshake")
        response += chunk
    head, _, rest = response.partition(b"\r\n\r\n")
    status = head.split(b"\r\n", 1)[0]
    if b" 101 " not in status:
        raise OSError(status.decode(errors="replace"))
    sock.settimeout(None)
    # The server may send its first message right behind the handshake.
    return Socket(sock, client=True, pending=rest)


# ---------------------------------------------------------------- audio


class Audio:
    """Plays the other side and sends the microphone, or a tone, while talking."""

    def __init__(self, mode):
        self.mode = mode  # "pulse", "tone" or "none"
        self.player = None
        self.recorder = None
        self.running = False

    def start(self, sink):
        self.running = True
        args = ["--format=s16le", "--rate=16000", "--channels=1", "--raw", "--latency-msec=80"]
        if self.mode != "none":
            try:
                self.player = subprocess.Popen(["pacat", "--playback", *args], stdin=subprocess.PIPE,
                                               bufsize=0)
            except OSError as e:
                log("No playback:", e)
        if self.mode == "pulse":
            try:
                self.recorder = subprocess.Popen(["pacat", "--record", *args], stdout=subprocess.PIPE,
                                                 stdin=subprocess.DEVNULL)
            except OSError as e:
                log("No microphone:", e)
        threading.Thread(target=self._capture, args=(sink,), daemon=True).start()

    def _capture(self, sink):
        phase, start, frames = 0.0, time.monotonic(), 0
        while self.running:
            if self.recorder:
                data = self.recorder.stdout.read(FRAME_BYTES)
                if not data:
                    return
            else:
                samples = []
                for _ in range(FRAME_BYTES // 2):
                    samples.append(int(6000 * math.sin(phase)) if self.mode == "tone" else 0)
                    phase += 2 * math.pi * 440 / 16000
                data = struct.pack("<%dh" % len(samples), *samples)
                frames += 1
                time.sleep(max(0.0, start + frames * 0.08 - time.monotonic()))
            sink(data)

    def play(self, data):
        if self.player and self.player.stdin:
            try:
                self.player.stdin.write(data)
                self.player.stdin.flush()
            except (BrokenPipeError, ValueError):
                pass

    def stop(self):
        self.running = False
        for p in (self.player, self.recorder):
            if p:
                p.kill()
        self.player = self.recorder = None


# ---------------------------------------------------------------- video


def nal_units(stream):
    """Splits an Annex B byte stream into NAL units, without their start codes."""
    buf = b""
    while True:
        chunk = stream.read1(65536)
        if not chunk:
            return
        buf += chunk
        while True:
            first = buf.find(b"\0\0\1")
            if first < 0:
                break
            second = buf.find(b"\0\0\1", first + 3)
            if second < 0:
                buf = buf[first:]
                break
            yield buf[first + 3:second].rstrip(b"\0")
            buf = buf[second:]


class Video:
    """The video socket of a call: H.264 frames behind a 9 byte header, each acknowledged.

    The camera is ffmpeg reading a V4L2 device, or a test pattern; the phone's picture shows
    in an ffplay window.
    """

    MAX_IN_FLIGHT = 6
    WIDTH, HEIGHT, FPS = 640, 480, 15

    def __init__(self, sim, link):
        self.sim = sim
        self.link = link
        self.lock = threading.Lock()
        self.sent = self.acked = self.received = 0
        self.waiting_key = False
        self.encoder = self.player = None
        self.camera = False

    def run(self):
        self.link.text({"type": "camera", "on": False})
        if self.sim.camera_wanted():
            self.camera_on(True)
        while True:
            message = self.link.receive()
            if message is None:
                break
            kind, data = message
            if kind == 0x2:
                with self.lock:
                    self.received += 1
                    n = self.received
                self.link.text({"type": "ack", "n": n})
                self._show(data)
                continue
            try:
                text = json.loads(data)
            except ValueError:
                continue
            if text.get("type") == "ack":
                with self.lock:
                    if self.acked < text.get("n", -1) <= self.sent:
                        self.acked = text["n"]
            elif text.get("type") == "camera":
                log("Phone camera on" if text.get("on") else "Phone camera off")
                if not text.get("on"):
                    self._close_player()
        self.close()
        self.sim.video_closed(self)

    def camera_on(self, on):
        if on == self.camera:
            return
        self.camera = on
        self.link.text({"type": "camera", "on": on})
        if on:
            self._start_encoder()
        elif self.encoder:
            self.encoder.kill()
            self.encoder = None

    def _start_encoder(self, test=False):
        source = self.sim.video_source
        if not test and source != "test" and not os.path.exists(source):
            log(f"No camera at {source}; sending a test pattern")
        test = test or source == "test" or not os.path.exists(source)
        if test:
            src = ["-re", "-f", "lavfi", "-i",
                   f"testsrc2=size={self.WIDTH}x{self.HEIGHT}:rate={self.FPS}"]
        else:
            # The camera's own mode: not every camera does 640x480 at 15 fps.
            src = ["-f", "v4l2", "-i", source]
        w, h = self.WIDTH, self.HEIGHT
        fit = (f"scale={w}:{h}:force_original_aspect_ratio=decrease,"
               f"pad={w}:{h}:(ow-iw)/2:(oh-ih)/2,fps={self.FPS}")
        # -nostdin: ffmpeg would otherwise take the commands typed at the prompt.
        args = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", *src, "-an",
                "-vf", fit, "-c:v", "libx264", "-preset", "ultrafast",
                "-tune", "zerolatency", "-profile:v", "baseline", "-pix_fmt", "yuv420p",
                "-g", str(self.FPS), "-b:v", "800k",
                "-x264-params", "sliced-threads=0:aud=0:repeat-headers=1", "-f", "h264", "-"]
        try:
            self.encoder = subprocess.Popen(args, stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
        except OSError as e:
            return log("No camera:", e)
        threading.Thread(target=self._send_frames, args=(self.encoder, test), daemon=True).start()

    def _send_frames(self, encoder, test):
        sps = pps = None
        started = False
        for unit in nal_units(encoder.stdout):
            started = True
            kind = unit[0] & 0x1F if unit else -1
            if kind == 7:
                sps = unit
            elif kind == 8:
                pps = unit
            elif kind in (1, 5) and self.camera and sps and pps:
                key = kind == 5
                header = struct.pack(">BHHI", 1 if key else 0, self.WIDTH, self.HEIGHT,
                                     int(time.monotonic() * 1000) & 0xFFFFFFFF)
                body = (b"\0\0\0\1" + sps + b"\0\0\0\1" + pps if key else b"") + b"\0\0\0\1" + unit
                self._send(key, header + body)
        # A camera that is busy or refuses to stream: the call gets the test pattern instead.
        if not started and not test and self.camera and self.encoder is encoder:
            log(f"The camera at {self.sim.video_source} doesn't stream (in use?); sending a test pattern")
            self._start_encoder(test=True)

    def _send(self, key, frame):
        with self.lock:
            if self.sent - self.acked >= self.MAX_IN_FLIGHT or (self.waiting_key and not key):
                self.waiting_key = True
                return
            self.waiting_key = False
            self.sent += 1
        self.link.binary(frame)

    def _show(self, frame):
        if len(frame) <= 9:
            return
        if not self.player:
            if not frame[0] & 1:
                return  # a decoder starts at a key frame
            try:
                self.player = subprocess.Popen(
                    ["ffplay", "-hide_banner", "-loglevel", "error", "-window_title", "Phone",
                     "-fflags", "nobuffer", "-flags", "low_delay", "-framedrop",
                     "-probesize", "32", "-analyzeduration", "0", "-f", "h264", "-i", "-"],
                    stdin=subprocess.PIPE, bufsize=0)
            except OSError as e:
                log("Can't show the phone's camera:", e)
                return
        try:
            self.player.stdin.write(frame[9:])
            self.player.stdin.flush()
        except (BrokenPipeError, ValueError):
            self.player = None

    def _close_player(self):
        if self.player:
            self.player.kill()
            self.player = None

    def close(self):
        self.camera = False
        if self.encoder:
            self.encoder.kill()
            self.encoder = None
        self._close_player()
        self.link.close()


# ---------------------------------------------------------------- mDNS


def dns_name(name):
    out = b""
    for part in name.rstrip(".").split("."):
        out += bytes([len(part)]) + part.encode()
    return out + b"\0"


def read_name(data, offset):
    labels, jumped, end = [], False, offset
    for _ in range(64):
        n = data[offset]
        if n == 0:
            offset += 1
            break
        if n & 0xC0 == 0xC0:
            if not jumped:
                end = offset + 2
            offset = ((n & 0x3F) << 8) | data[offset + 1]
            jumped = True
            continue
        labels.append(data[offset + 1:offset + 1 + n].decode(errors="replace"))
        offset += 1 + n
    return ".".join(labels), (end if jumped else offset)


class Mdns:
    """Announces this kiosk and answers queries for it; notes phones it hears."""

    def __init__(self, sim):
        self.sim = sim
        self.instance = f"ks-{sim.id}.{SERVICE}"
        self.host = f"kiosk-sim-{sim.id[:8]}.local"
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        self.sock.bind(("", 5353))
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                             socket.inet_aton("224.0.0.251") + socket.inet_aton(sim.address))
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(sim.address))
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        self.services, self.targets, self.addresses = {}, {}, {}

    def records(self):
        def rr(name, rtype, data, ttl=120, flush=False):
            return (dns_name(name) + struct.pack(">HHIH", rtype, 0x8001 if flush else 1, ttl, len(data))
                    + data)
        txt = b""
        for k, v in {"id": self.sim.id, "name": self.sim.name, "version": VERSION,
                     "port": str(self.sim.port), "tls": "0"}.items():
            item = f"{k}={v}".encode()
            txt += bytes([len(item)]) + item
        return [
            rr(SERVICE, 12, dns_name(self.instance), 4500),
            rr(self.instance, 33, struct.pack(">HHH", 0, 0, self.sim.port) + dns_name(self.host), flush=True),
            rr(self.instance, 16, txt, 4500, flush=True),
            rr(self.host, 1, socket.inet_aton(self.sim.address), flush=True),
        ]

    def announce(self):
        records = self.records()
        packet = struct.pack(">HHHHHH", 0, 0x8400, 0, len(records), 0, 0) + b"".join(records)
        self.sock.sendto(packet, ("224.0.0.251", 5353))

    def query(self):
        packet = struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0) + dns_name(SERVICE) + struct.pack(">HH", 12, 1)
        self.sock.sendto(packet, ("224.0.0.251", 5353))

    def run(self):
        threading.Thread(target=self._announcer, daemon=True).start()
        while True:
            try:
                data, _ = self.sock.recvfrom(9000)
                self._handle(data)
            except Exception:
                continue

    def _announcer(self):
        self.query()
        while True:
            self.announce()
            time.sleep(30)

    def _handle(self, data):
        _, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", data[:12])
        offset = 12
        asked = False
        for _ in range(qd):
            name, offset = read_name(data, offset)
            offset += 4
            if name.lower() in (SERVICE.lower(), self.instance.lower(), self.host.lower()):
                asked = True
        if asked and not flags & 0x8000:
            self.announce()
        for _ in range(an + ns + ar):
            name, offset = read_name(data, offset)
            rtype, _, _, length = struct.unpack(">HHIH", data[offset:offset + 10])
            offset += 10
            body = data[offset:offset + length]
            if rtype == 33:
                port = struct.unpack(">H", body[4:6])[0]
                self.targets[name] = (read_name(data, offset + 6)[0], port)
            elif rtype == 16:
                txt, i = {}, 0
                while i < len(body):
                    item = body[i + 1:i + 1 + body[i]].decode(errors="replace")
                    i += 1 + body[i]
                    k, _, v = item.partition("=")
                    txt[k] = v
                self.services[name] = txt
            elif rtype == 1 and length == 4:
                self.addresses[name] = socket.inet_ntoa(body)
            offset += length
        self._note_phones()

    def _note_phones(self):
        for instance, txt in self.services.items():
            if instance == self.instance or txt.get("id") == self.sim.id:
                continue
            target = self.targets.get(instance)
            address = self.addresses.get(target[0]) if target else None
            if address and txt.get("id"):
                self.sim.note_phone(txt["id"], txt.get("name", ""), address, target[1])


# ---------------------------------------------------------------- the kiosk


class Sim:
    def __init__(self, args):
        self.key = args.key
        self.wire = Wire(args.key)
        self.name = args.name
        self.port = args.port
        self.id = args.id or str(uuid.uuid5(uuid.NAMESPACE_DNS, socket.gethostname() + args.name))
        self.address = args.address or local_address()
        self.audio_mode = "none" if args.no_audio else ("tone" if args.tone else "pulse")
        self.auto_answer = args.auto_answer
        self.video_enabled = not args.no_video
        self.video_source = args.camera
        self.camera = True  # sends its picture in video calls; `camera off` stops it
        self.dnd = False
        self.phones = {}  # id -> {name, address, port}
        self.lock = threading.RLock()
        self.reset()

    def reset(self):
        self.state = "idle"  # idle, ringing, calling, in_call, listening, announcing
        self.call = ""
        self.peer = None
        self.outgoing = False
        self.link = None
        self.audio = None
        self.muted = False
        self.video = None  # the video socket
        self.peer_video = False  # both sides take video in this call

    def note_phone(self, pid, name, address, port):
        with self.lock:
            known = self.phones.get(pid)
            self.phones[pid] = {"name": name or (known or {}).get("name", ""), "address": address,
                                "port": port}
            if not known:
                log(f"Found {name or pid} at {address}:{port}")

    def identity(self):
        return {"id": self.id, "name": self.name, "version": VERSION, "enabled": True,
                "key": self.wire.fingerprint(), "dnd": self.dnd, "video": self.video_enabled,
                "endpoint": {"port": self.port, "tls": False}}

    def me(self):
        return {"id": self.id, "name": self.name, "address": self.address, "port": self.port,
                "version": VERSION, "tls": False}

    # -- requests from the phone

    def incoming(self, body, token, host):
        call, sender = body.get("call", ""), body.get("from") or {}
        claims = self.wire.verify(token, call)
        if not claims or claims.get("from") != sender.get("id"):
            return 403, {"status": "key"}
        kind = body.get("kind", "call")
        with self.lock:
            if self.dnd:
                return 200, {"status": "dnd"}
            if self.state != "idle":
                return 200, {"status": "busy"}
            self.note_phone(sender.get("id"), sender.get("name", ""), host, int(sender.get("port", 2324)))
            self.call, self.outgoing = call, False
            self.peer = {"id": sender.get("id"), "name": sender.get("name", ""), "address": host,
                         "port": int(sender.get("port", 2324))}
            who = self.peer["name"] or host
            if kind == "broadcast":
                self.state = "listening"
                log(f"Announcement from {who}")
                return 200, {"status": "listening", "video": False}
            self.state = "ringing"
            self.peer_video = self.video_enabled and isinstance(body.get("video"), dict)
            threading.Timer(RING_SECONDS, self._ring_timeout, args=(call,)).start()
        what = "Video call" if self.peer_video else "Call"
        if self.auto_answer:
            threading.Timer(1, self.answer).start()
            log(f"{what} from {who}, answering automatically")
            return 200, {"status": "auto", "video": self.peer_video}
        log(f"{what} from {who}: type `answer` or `decline`")
        return 200, {"status": "ringing", "video": self.peer_video}

    def signal(self, call, body, token):
        if not self.wire.verify(token, call) or call != self.call:
            return 403, {"ok": False}
        action = body.get("action")
        if action == "answer" and self.outgoing and self.state == "calling":
            log("Answered")
            threading.Thread(target=self._open_audio, daemon=True).start()
        elif action in ("decline", "missed", "cancel", "hangup"):
            log({"decline": "Declined", "missed": "Not answered", "cancel": "Caller gave up",
                 "hangup": "Hung up"}[action])
            self.end(notify=False)
        return 200, {"ok": True}

    def attach(self, call, token, link):
        """The phone, as caller, opens the audio socket."""
        if not self.wire.verify(token, call) or call != self.call or self.link:
            return False
        if self.state not in ("in_call", "listening"):
            return False
        self._run_link(link)
        return True

    # -- what the user does

    def answer(self):
        with self.lock:
            if self.state != "ringing":
                return log("Nothing is ringing.")
            self.state = "in_call"
        if not self._post("answer"):
            log("The phone did not take the answer.")
            self.end(notify=False)
        else:
            log("Connecting …")
            if self.video and self.camera:
                self.video.camera_on(True)

    def decline(self):
        if self.state != "ringing":
            return log("Nothing is ringing.")
        self._post("decline")
        self.end(notify=False)

    def place(self, target, kind):
        phone = self._phone(target)
        if not phone:
            return
        with self.lock:
            if self.state != "idle":
                return log("Already in a call.")
            self.call, self.outgoing = uuid.uuid4().hex, True
            self.peer = phone
            self.state = "announcing" if kind == "broadcast" else "calling"
        video = kind == "video" and self.video_enabled
        kind = "broadcast" if kind == "broadcast" else "call"
        body = {"call": self.call, "kind": kind, "from": self.me()}
        if video:
            body["video"] = {"camera": self.camera}
        try:
            answer = self._request("/api/intercom/call", body)
            status = answer.get("status")
        except Exception as e:
            log("Phone not reachable:", e)
            return self.end(notify=False)
        if video and status in ("ringing", "auto") and answer.get("video"):
            # The caller opens the video socket; the phone sees this kiosk while it rings.
            self.peer_video = True
            threading.Thread(target=self._open_video, args=(self.call,), daemon=True).start()
        elif video:
            log("The phone takes no video; calling without")
        if kind == "broadcast":
            if status != "listening":
                log("Announcement refused:", status)
                return self.end(notify=False)
            log("Announcing; type `hangup` when done")
            threading.Thread(target=self._open_audio, daemon=True).start()
        elif status in ("ringing", "auto"):
            log("Ringing …")
            threading.Timer(RING_SECONDS + 15, self._calling_timeout, args=(self.call,)).start()
        else:
            log("Call refused:", status)
            self.end(notify=False)

    def end(self, notify=True):
        with self.lock:
            if self.state == "idle":
                return
            state, link, audio, video = self.state, self.link, self.audio, self.video
            if notify:
                action = ("cancel" if state == "calling" else "decline" if state == "ringing"
                          else "hangup")
                threading.Thread(target=self._post, args=(action,), daemon=True).start()
            self.reset()
        if link:
            link.text({"type": "end"})
            link.close()
        if audio:
            audio.stop()
        if video:
            video.close()
        log("Call ended")

    def mute(self, on):
        self.muted = on
        if self.link:
            self.link.text({"type": "talk", "on": not on})
        log("Muted" if on else "Microphone on")

    def set_camera(self, on):
        self.camera = on
        if self.video:
            self.video.camera_on(on and self.camera_wanted())
        log("Camera on" if on else "Camera off")

    # -- video

    def camera_wanted(self):
        """This kiosk shows itself from the start of a call it makes, or once it answers."""
        return self.camera and (self.outgoing or self.state == "in_call")

    def attach_video(self, call, token, link):
        """The phone, as caller, opens the video socket."""
        with self.lock:
            if (not self.wire.verify(token, call) or call != self.call or self.outgoing
                    or not self.peer_video or self.state not in ("ringing", "in_call")):
                return False
            if self.video:
                self.video.close()
            self.video = Video(self, link)
        self.video.run()
        return True

    def _open_video(self, call):
        while self.call == call and self.state in ("calling", "in_call") and not self.video:
            peer = self.peer
            token = urllib.parse.quote(self.wire.token(call, self.id))
            try:
                link = connect_ws(peer["address"], peer["port"], f"/api/intercom/video/{call}?token={token}")
            except Exception as e:
                log("Video connection failed:", e)
                time.sleep(2)
                continue
            with self.lock:
                if self.call != call:
                    link.close()
                    return
                self.video = Video(self, link)
            log("Video connected")
            self.video.run()
            # The caller tries again while the call lasts.
            time.sleep(2)

    def video_closed(self, video):
        with self.lock:
            if self.video is video:
                self.video = None

    # -- plumbing

    def _phone(self, target):
        with self.lock:
            if not target:
                if len(self.phones) == 1:
                    return dict(next(iter(self.phones.values())))
                log("Name a phone: `call <address>`; known:", self._known())
                return None
            for p in self.phones.values():
                if target in (p["address"], p["name"]):
                    return dict(p)
        host, _, port = target.partition(":")
        return {"name": "", "address": host, "port": int(port or 2324)}

    def _known(self):
        return ", ".join(f"{p['name']} ({p['address']})" for p in self.phones.values()) or "none yet"

    def _request(self, path, body):
        url = f"http://{self.peer['address']}:{self.peer['port']}{path}"
        request = urllib.request.Request(
            url, data=json.dumps(body).encode(), method="POST",
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer " + self.wire.token(self.call, self.id)})
        try:
            with urllib.request.urlopen(request, timeout=10) as r:
                return json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return json.loads(e.read() or b"{}")

    def _post(self, action):
        if not self.peer:
            return False
        try:
            return self._request(f"/api/intercom/call/{self.call}", {"action": action}).get("ok", False)
        except Exception:
            return False

    def _ring_timeout(self, call):
        if self.call == call and self.state == "ringing":
            self._post("missed")
            log("Missed")
            self.end(notify=False)

    def _calling_timeout(self, call):
        if self.call == call and self.state == "calling":
            log("Nobody answered")
            self.end()

    def _open_audio(self):
        """As caller, this side opens the audio socket once the call is taken."""
        with self.lock:
            if self.state == "calling":
                self.state = "in_call"
            call, peer = self.call, self.peer
        if not peer:
            return
        token = urllib.parse.quote(self.wire.token(call, self.id))
        try:
            link = connect_ws(peer["address"], peer["port"], f"/api/intercom/audio/{call}?token={token}")
        except Exception as e:
            log("Audio connection failed:", e)
            return self.end(notify=True)
        self._run_link(link)

    def _run_link(self, link):
        with self.lock:
            self.link = link
            self.audio = Audio(self.audio_mode)
            sending = self.state in ("in_call", "announcing")
            call = self.call
        if sending:
            link.text({"type": "talk", "on": not self.muted})
        self.audio.start(lambda data: (link.binary(data) if sending and not self.muted
                                       and not link.closed else None))
        log("Connected" + (" (sending a test tone)" if self.audio_mode == "tone" and sending else ""))
        while True:
            message = link.receive()
            if message is None:
                break
            kind, data = message
            if kind == 0x2:
                if self.audio:
                    self.audio.play(data)
                continue
            try:
                text = json.loads(data)
            except ValueError:
                continue
            if text.get("type") == "end":
                break
            if text.get("type") == "talk":
                log("Phone talking" if text.get("on") else "Phone quiet")
        if self.call == call and self.state != "idle":
            self.end(notify=False)

    def status(self):
        who = self.peer and (self.peer.get("name") or self.peer.get("address"))
        log(f"{self.name} ({self.address}:{self.port}), id {self.id}, key {self.wire.fingerprint()}, "
            f"{'Do not disturb, ' if self.dnd else ''}state {self.state}"
            + (f" with {who}" if who else "")
            + (f", video frames {self.video.sent} sent, {self.video.received} received" if self.video else "")
            + f"; phones: {self._known()}")


# ---------------------------------------------------------------- HTTP server


def make_handler(sim):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def _json(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = urllib.parse.urlparse(self.path)
            if path.path == "/api/intercom/identity":
                return self._json(200, sim.identity())
            upgrade = self.headers.get("Upgrade", "").lower() == "websocket"
            audio = path.path.startswith("/api/intercom/audio/")
            if upgrade and (audio or path.path.startswith("/api/intercom/video/")):
                call = path.path.rsplit("/", 1)[1]
                token = urllib.parse.parse_qs(path.query).get("token", [""])[0]
                if audio:
                    allowed = call == sim.call and not sim.link and sim.wire.verify(token, call)
                else:
                    allowed = (call == sim.call and not sim.outgoing and sim.peer_video
                               and sim.state in ("ringing", "in_call"))
                if not allowed:
                    return self._json(403, {"error": "forbidden"})
                self.send_response(101)
                self.send_header("Upgrade", "websocket")
                self.send_header("Connection", "Upgrade")
                self.send_header("Sec-WebSocket-Accept", accept_key(self.headers["Sec-WebSocket-Key"]))
                self.end_headers()
                self.wfile.flush()
                self.close_connection = True
                link = Socket(self.connection, client=False)
                if audio:
                    sim._run_link(link)
                elif not sim.attach_video(call, token, link):
                    link.close()
                return
            self._json(404, {"error": "not found"})

        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except ValueError:
                return self._json(400, {"error": "invalid JSON"})
            auth = self.headers.get("Authorization", "")
            token = auth[7:] if auth.startswith("Bearer ") else ""
            host = self.client_address[0]
            if self.path == "/api/intercom/call":
                code, answer = sim.incoming(body, token, host)
            elif self.path.startswith("/api/intercom/call/"):
                code, answer = sim.signal(self.path.rsplit("/", 1)[1], body, token)
            else:
                code, answer = 404, {"error": "not found"}
            self._json(code, answer)

    return Handler


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def local_address():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    finally:
        s.close()


HELP = """Commands:
  status                 this kiosk, the call and the phones it knows
  call [phone]           call a phone (its address or name; alone if only one is known)
  video [phone]          make a video call
  announce [phone]       make an announcement to a phone
  answer | decline       a call ringing here
  hangup                 end the call or announcement
  mute | unmute          stop or resume sending
  camera on | camera off show this kiosk in video calls, or not
  dnd on | dnd off       Do not disturb: refuse calls
  quit"""


def main():
    parser = argparse.ArgumentParser(description="A Kiosk Satellite intercom for testing Intercom Satellite.")
    parser.add_argument("--key", default=os.environ.get("INTERCOM_KEY"),
                        help="the intercom key, the same as in the app (or INTERCOM_KEY)")
    parser.add_argument("--name", default="Linux Kiosk", help="the name the phone shows")
    parser.add_argument("--port", type=int, default=2324)
    parser.add_argument("--address", help="this computer's address on the network (found by itself)")
    parser.add_argument("--id", help="a fixed kiosk id (default: derived from host and name)")
    parser.add_argument("--tone", action="store_true", help="send a 440 Hz tone instead of the microphone")
    parser.add_argument("--no-audio", action="store_true", help="neither play nor record")
    parser.add_argument("--camera", default="/dev/video0",
                        help="the camera for video calls, or `test` for a test pattern (the default without one)")
    parser.add_argument("--no-video", action="store_true", help="take voice calls only")
    parser.add_argument("--auto-answer", action="store_true", help="answer calls after a second")
    parser.add_argument("--no-mdns", action="store_true", help="do not announce; add it in the app by address")
    args = parser.parse_args()
    if not args.key:
        parser.error("--key is required")

    sim = Sim(args)
    server = Server(("", args.port), make_handler(sim))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    if not args.no_mdns:
        try:
            threading.Thread(target=Mdns(sim).run, daemon=True).start()
        except OSError as e:
            print("mDNS unavailable (" + str(e) + "); add the kiosk in the app by address.")
    print(f"{sim.name} listening on {sim.address}:{sim.port}, key {sim.wire.fingerprint()}")
    print(HELP)
    print("> ", end="", flush=True)

    for line in sys.stdin:
        cmd, _, rest = line.strip().partition(" ")
        rest = rest.strip()
        if cmd == "status":
            sim.status()
        elif cmd == "call":
            threading.Thread(target=sim.place, args=(rest, "call"), daemon=True).start()
        elif cmd == "video":
            threading.Thread(target=sim.place, args=(rest, "video"), daemon=True).start()
        elif cmd == "camera":
            sim.set_camera(rest != "off")
        elif cmd == "announce":
            threading.Thread(target=sim.place, args=(rest, "broadcast"), daemon=True).start()
        elif cmd == "answer":
            threading.Thread(target=sim.answer, daemon=True).start()
        elif cmd == "decline":
            sim.decline()
        elif cmd in ("hangup", "end"):
            sim.end()
        elif cmd in ("mute", "unmute"):
            sim.mute(cmd == "mute")
        elif cmd == "dnd":
            sim.dnd = rest == "on"
            log("Do not disturb", "on" if sim.dnd else "off")
        elif cmd in ("quit", "exit"):
            break
        elif cmd in ("help", "?"):
            log(HELP)
        elif cmd:
            log("Unknown command; type `help`.")
        else:
            print("> ", end="", flush=True)
    sim.end()


if __name__ == "__main__":
    main()
