# Kiosk simulator

A small stand-in for a Kiosk Satellite intercom on Linux, for testing Intercom Satellite, the Android app and the Linux app, without a kiosk. Python 3 standard library only; audio through PulseAudio or PipeWire (`pacat`), video through `ffmpeg` and `ffplay`.

```sh
python3 tools/kiosk-sim/kiosk_sim.py --key '<intercom key>'
```

Use the same intercom key as in the app. The simulator announces itself over mDNS as "Linux Kiosk", so it shows up in the app's kiosk list. If it doesn't (firewall, another mDNS responder holding the port), start it with `--no-mdns` and add it in the app by address, port 2324.

On the same computer as the Linux app, give one of them another port (`--port 2325` here, or **Port** in the app's settings): both use 2324.

Commands at the prompt:

| Command | |
|---|---|
| `call [phone]` | call the phone (its address, or its name; nothing if only one phone is known) |
| `video [phone]` | make a video call |
| `announce [phone]` | make an announcement |
| `answer`, `decline` | a call from the phone |
| `hangup` | end the call or announcement |
| `mute`, `unmute` | stop or resume sending |
| `camera on`, `camera off` | show this kiosk in video calls, or not |
| `dnd on`, `dnd off` | refuse calls with Do not disturb |
| `status` | this kiosk, the call, the phones it knows |

The simulator learns phones from their mDNS announcements and from their requests, so call it from the phone once, or give `call` the phone's address.

In video calls the simulator sends its camera (`--camera`, default `/dev/video0`; a test pattern when there is none, or with `--camera test`) and shows the phone's camera in an ffplay window. As the caller its picture shows on the phone while it rings; as the callee once it answers.

Options: `--name`, `--port`, `--address` (when it picks the wrong network), `--tone` (send a 440 Hz tone instead of the microphone), `--no-audio`, `--no-video`, `--auto-answer`.
