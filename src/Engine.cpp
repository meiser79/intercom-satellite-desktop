#include "Engine.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QUrl>
#include <QUuid>

#include <algorithm>

#include "Discovery.h"
#include "HttpServer.h"
#include "Tls.h"
#include "VideoLink.h"

namespace {
QString text(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

QJsonObject parse(const QByteArray &data) { return QJsonDocument::fromJson(data).object(); }

/** A socket of the call at the peer: ws:// where its requests go to http://. */
QUrl socketUrl(const Peer &p, const QString &path, const QString &token) {
    QUrl url(p.base(false) + path);
    url.setScheme(url.scheme() == "https" ? "wss" : "ws");
    url.setQuery("token=" + QUrl::toPercentEncoding(token));
    return url;
}
}  // namespace

Engine::Engine(Config &config, QObject *parent)
    : QObject(parent), config(config), wire(config.key()) {
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, this, [this] { endIf(timeoutCall, timeoutReason, true); });
    connect(&refreshTimer, &QTimer::timeout, this, &Engine::refresh);
    connect(&pingTimer, &QTimer::timeout, this, [this] {
        if (link) link->ping();
        for (const auto &l : std::as_const(announcementLinks))
            if (l) l->ping();
    });
    connect(&audio, &Audio::frame, this, &Engine::sendAudio);
    connect(&cameraSource, &Camera::picture, this, &Engine::picture);
    connect(&cameraSource, &Camera::failed, this, [this](const QString &problem) {
        detailText = tr("Camera unavailable (%1); sending a test pattern").arg(problem);
        emit changed();
    });
    clock.start();
}

Engine::~Engine() { stop(); }

bool Engine::start(QString *error, bool mdns) {
    if (config.key().isEmpty()) {
        *error = tr("Enter the intercom key in Settings.");
        return false;
    }
    HttpServer::Handlers h;
    h.identity = [this] { return identity(); };
    h.incoming = [this](const QJsonObject &b, const QString &t, const QString &host) {
        return incoming(b, t, host);
    };
    h.signal = [this](const QString &id, const QJsonObject &b, const QString &t,
                      const QString &host) { return signal(id, b, t, host); };
    h.allowAudio = [this](const QString &id, const QString &t, const QString &) {
        return allowAudio(id, t);
    };
    h.allowVideo = [this](const QString &id, const QString &t, const QString &) {
        return allowVideo(id, t);
    };
    h.audio = [this](const QString &id, QWebSocket *s) { attach(id, s); };
    h.video = [this](const QString &id, QWebSocket *s) { attachVideo(id, s); };
    server = std::make_unique<HttpServer>(
        h, config.tls() ? HttpServer::Mode::IdentityOnly : HttpServer::Mode::Plain);
    if (!server->listen(quint16(config.port()))) {
        *error = tr("Port %1 is in use: %2").arg(config.port()).arg(server->errorString());
        server.reset();
        return false;
    }
    if (config.tls()) {
        QSslConfiguration ssl;
        if (!Tls::server(config, &ssl, error)) {
            server.reset();
            return false;
        }
        secureServer = std::make_unique<HttpServer>(h, HttpServer::Mode::Encrypted, ssl);
        if (!secureServer->listen(quint16(endpointPort()))) {
            *error =
                tr("Port %1 is in use: %2").arg(endpointPort()).arg(secureServer->errorString());
            secureServer.reset();
            server.reset();
            return false;
        }
    }
    running = true;
    for (Peer p : config.manualPeers()) peers.insert(p.addressKey(), p);
    if (mdns) {
        discovery = std::make_unique<Discovery>(config.id(), config.name(), APP_VERSION,
                                                config.port());
        connect(discovery.get(), &Discovery::found, this, &Engine::found);
        if (!discovery->start()) {
            mdnsProblem = discovery->errorString();
            discovery.reset();
        }
    }
    refreshTimer.start(20000);
    pingTimer.start(15000);
    refresh();
    emit changed();
    return true;
}

void Engine::stop() {
    if (!running) return;
    finish(tr("Intercom turned off"), true);
    running = false;
    refreshTimer.stop();
    pingTimer.stop();
    discovery.reset();
    server.reset();
    secureServer.reset();
    emit changed();
}

// ---------------------------------------------------------------- identity

QJsonObject Engine::identity() const {
    return {{"id", config.id()},
            {"name", config.name()},
            {"version", APP_VERSION},
            {"enabled", running && !config.key().isEmpty()},
            {"key", Wire::fingerprint(config.key())},
            {"dnd", config.dnd()},
            {"video", config.video()},
            {"endpoint", QJsonObject{{"port", endpointPort()}, {"tls", config.tls()}}}};
}

int Engine::endpointPort() const { return config.tls() ? config.port() + 1 : config.port(); }

QJsonObject Engine::self() const {
    return {{"id", config.id()},
            {"name", config.name()},
            {"address", Discovery::localIp()},
            {"port", endpointPort()},
            {"version", APP_VERSION},
            {"tls", config.tls()}};
}

// ---------------------------------------------------------------- kiosks

QList<Peer> Engine::peerList() const {
    QList<Peer> list = peers.values();
    std::sort(list.begin(), list.end(), [this](const Peer &a, const Peer &b) {
        if (a.ready != b.ready) return a.ready;
        return display(a).localeAwareCompare(display(b)) < 0;
    });
    return list;
}

QString Engine::display(const Peer &p) const {
    QString alias = config.alias(p.id);
    if (!alias.isEmpty()) return alias;
    if (!p.name.isEmpty()) return p.name;
    QString known = config.knownName(p.id);
    return known.isEmpty() ? tr("Unknown kiosk") : known;
}

void Engine::found(const QString &host, int port, bool tls, const QString &id,
                   const QString &name) {
    if (id == config.id()) return;
    Peer p(host, port, tls);
    p.id = id;
    p.name = name;
    p.seen = now();
    if (!peers.contains(p.addressKey())) {
        peers.insert(p.addressKey(), p);
        emit changed();
    }
    probe(p.addressKey());
}

bool Engine::add(const QString &address, QString *error) {
    QUrl url = QUrl::fromUserInput(address.contains("://") ? address : "http://" + address);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        (url.path() != "" && url.path() != "/") || url.hasQuery()) {
        *error = tr("Enter an address like 192.168.1.50 or kitchen.local, optionally with :port.");
        return false;
    }
    int port = url.port(2324);
    if (port == 80 || port == 443) port = 2324;
    Peer p(url.host(), port, url.scheme() == "https");
    p.manual = true;
    peers.insert(p.addressKey(), p);
    savePeers();
    probe(p.addressKey());
    emit changed();
    return true;
}

void Engine::remove(const QString &addressKey) {
    auto it = peers.find(addressKey);
    if (it == peers.end()) return;
    config.forget(*it);
    peers.erase(it);
    savePeers();
    emit changed();
}

void Engine::savePeers() {
    QList<Peer> manual;
    for (const Peer &p : std::as_const(peers))
        if (p.manual) manual << p;
    config.setManualPeers(manual);
}

void Engine::refresh() {
    if (!running) return;
    for (const QString &key : peers.keys()) probe(key);
}

void Engine::probe(const QString &key) {
    auto it = peers.find(key);
    if (it == peers.end() || !running) return;
    if (now() - it->checked < 1500) return;
    it->checked = now();
    if (!it->adminTls) return fetchIdentity(key);
    Tls::inspect(it->host, it->adminPort, this,
                 [this, key](const QString &shown, const QString &error) {
        auto it = peers.find(key);
        if (it == peers.end()) return;
        if (confirmed(*it, it->adminPort, shown, error)) fetchIdentity(key);
        else settle(key, it->status, it->problem);
    });
}

void Engine::fetchIdentity(const QString &key) {
    const Peer &target = peers[key];
    QNetworkRequest request(QUrl(target.base(true) + "/api/intercom/identity"));
    request.setTransferTimeout(8000);
    if (target.adminTls) request.setSslConfiguration(Tls::client());
    QNetworkReply *reply = net.get(request);
    if (target.adminTls) Tls::guard(reply, config.pin(target.host, target.adminPort));
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
        reply->deleteLater();
        if (!peers.contains(key)) return;
        QJsonObject j = parse(reply->readAll());
        QString id = j.value("id").toString();
        if (!id.isEmpty()) {
            // The same device found at another address before, such as after a new IP address.
            for (auto other = peers.begin(); other != peers.end();) {
                if (other.key() != key && !other->manual && other->id == id)
                    other = peers.erase(other);
                else
                    ++other;
            }
        }
        Peer &p = peers[key];
        QString where = tr("Identity at port %1").arg(p.adminPort);
        if (reply->error() != QNetworkReply::NoError)
            return settle(key, Peer::Status::Unreachable, where + ": " + reply->errorString());
        if (j.value("id").toString().isEmpty())
            return settle(key, Peer::Status::Unreachable, where + ": " + tr("not an intercom"));
        p.id = id;
        p.seen = now();
        p.name = j.value("name").toString();
        p.version = j.value("version").toString();
        p.video = j.value("video").toBool();
        if (p.id == config.id()) {
            peers.remove(key);
            emit changed();
            return;
        }
        QJsonObject endpoint = j.value("endpoint").toObject();
        p.port = endpoint.isEmpty() ? p.adminPort : endpoint.value("port").toInt();
        p.tls = endpoint.isEmpty() ? p.adminTls : endpoint.value("tls").toBool();
        if (p.port < 1 || p.port > 65535)
            return settle(key, Peer::Status::Unreachable, tr("Invalid port %1").arg(p.port));
        if (!j.value("enabled").toBool()) return settle(key, Peer::Status::Off);
        if (Wire::fingerprint(config.key()) != j.value("key").toString())
            return settle(key, Peer::Status::KeyMismatch);
        if (p.tls != config.tls()) return settle(key, Peer::Status::TlsMismatch);
        if (j.value("dnd").toBool()) return settle(key, Peer::Status::Dnd);
        if (!p.tls) return settle(key, Peer::Status::Ready);
        Tls::inspect(p.host, p.port, this, [this, key](const QString &shown, const QString &error) {
            auto it = peers.find(key);
            if (it == peers.end()) return;
            if (confirmed(*it, it->port, shown, error)) settle(key, Peer::Status::Ready);
            else settle(key, it->status, it->problem);
        });
    });
}

void Engine::settle(const QString &key, Peer::Status status, const QString &problem) {
    auto it = peers.find(key);
    if (it == peers.end()) return;
    // Ready stays as it was while the check runs, so the list doesn't flicker offline.
    if (status == Peer::Status::Unreachable && !it->manual && now() - it->seen > STALE_MS) {
        peers.erase(it);
        emit changed();
        return;
    }
    it->status = status;
    it->problem = problem;
    it->ready = status == Peer::Status::Ready;
    if (it->ready) config.remember(*it);
    emit changed();
}

bool Engine::confirmed(Peer &p, int port, const QString &shown, const QString &error) {
    if (shown.isEmpty()) {
        p.status = Peer::Status::Unreachable;
        p.problem = tr("TLS at port %1: %2").arg(port).arg(error);
        p.ready = false;
        return false;
    }
    p.problem.clear();
    if (shown != config.pin(p.host, port)) {
        p.candidatePin = shown;
        p.candidatePort = port;
        p.status = Peer::Status::ConfirmCert;
        p.ready = false;
        return false;
    }
    p.candidatePin.clear();
    config.pinUsed(p.host, port);
    return true;
}

void Engine::trust(const QString &addressKey) {
    auto it = peers.find(addressKey);
    if (it == peers.end() || it->candidatePin.isEmpty()) return;
    config.setPin(it->host, it->candidatePort, it->candidatePin, it->id);
    it->candidatePin.clear();
    it->checked = 0;
    net.clearConnectionCache();
    probe(addressKey);
}

void Engine::trustCaller() {
    if (peer.candidatePin.isEmpty()) return;
    config.setPin(peer.host, peer.candidatePort, peer.candidatePin, peer.id);
    peer.candidatePin.clear();
    detailText.clear();
    net.clearConnectionCache();
    emit changed();
}

QWebSocket *Engine::socketTo(const Peer &p) {
    auto *ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    if (p.tls) Tls::guard(ws, config.pin(p.host, p.port));
    return ws;
}

void Engine::post(const Peer &p, const QString &path, const QJsonObject &body, const QString &id,
                  Reply done) {
    if (p.tls != config.tls()) {
        if (done) done({}, "tls");
        return;
    }
    QNetworkRequest request(QUrl(p.base(false) + path));
    if (p.tls) request.setSslConfiguration(Tls::client());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
    request.setRawHeader("Authorization", ("Bearer " + wire.token(id, config.id())).toUtf8());
    request.setTransferTimeout(10000);
    QNetworkReply *reply = net.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (p.tls) Tls::guard(reply, config.pin(p.host, p.port));
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject j = parse(reply->readAll());
        if (!done) return;
        if (code == 403) done({{"status", "key"}}, {});
        else if (code >= 200 && code < 300) done(j, {});
        // Refusals like busy or dnd come with 200; other statuses are failures.
        else done(j, reply->errorString().isEmpty() ? QString("HTTP %1").arg(code)
                                                    : reply->errorString());
    });
}

QString Engine::reason(const QString &status) const {
    if (status == "key") return tr("Different intercom key");
    if (status == "tls") return tr("TLS setting differs");
    if (status == "busy") return tr("Busy");
    if (status == "dnd") return tr("Do not disturb");
    if (status == "off") return tr("Intercom is off");
    if (status == "decline") return tr("Call declined");
    if (status == "missed") return tr("Not answered");
    if (status == "cancel") return tr("Caller hung up");
    if (status == "hangup") return tr("Call ended");
    return tr("Not available");
}

// ---------------------------------------------------------------- outgoing call

void Engine::call(const QString &addressKey, bool withCamera) {
    auto it = peers.find(addressKey);
    if (!running || active() || it == peers.end() || !it->ready) return;
    resetVideo();
    Peer target = *it;
    bool camera = withCamera && target.video && config.video();
    callId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    peer = target;
    isOutgoing = true;
    isBroadcast = false;
    connectedAt = 0;
    startedAt = now();
    isHandsFree = false;
    isSending = false;
    currentState = "calling";
    detailText.clear();
    QString id = callId;
    arm(65000, tr("No answer"));
    emit changed();
    QJsonObject body{{"call", id}, {"kind", "call"}, {"from", self()}};
    if (config.video()) body.insert("video", QJsonObject{{"camera", camera}});
    post(target, "/api/intercom/call", body, id,
         [this, id, target, camera](const QJsonObject &r, const QString &error) {
             if (id != callId) return;
             if (!error.isEmpty() && r.value("status").toString().isEmpty())
                 return endIf(id, tr("Kiosk not reachable"), false);
             QString status = r.value("status").toString();
             if (status != "ringing" && status != "auto") return endIf(id, reason(status), false);
             // The kiosk rings; when both take video the caller opens the video socket.
             videoAgreed = config.video() && r.value("video").toBool();
             if (!videoAgreed || !videoLive()) return;
             if (camera) this->camera(true);
             connectVideo(target, id);
         });
}

// ---------------------------------------------------------------- announcements

void Engine::announce(const QStringList &ids) {
    if (!running || active()) return;
    QMap<QString, Peer> available;
    for (const Peer &p : std::as_const(peers))
        if (!p.id.isEmpty() && (ids.isEmpty() || ids.contains(p.id)))
            if (!available.contains(p.id) || (!available[p.id].ready && p.ready))
                available.insert(p.id, p);
    announcementNames.clear();
    announcementStates.clear();
    QList<Peer> targets;
    for (const Peer &p : std::as_const(available)) {
        if (!p.ready || targets.size() >= MAX_ANNOUNCEMENT_TARGETS) {
            if (!ids.isEmpty()) {
                announcementNames.insert(p.id, display(p));
                announcementStates.insert(p.id, tr("Not reachable"));
            }
            continue;
        }
        targets << p;
        announcementNames.insert(p.id, display(p));
        announcementStates.insert(p.id, tr("Connecting …"));
    }
    if (targets.isEmpty()) {
        detailText = tr("No kiosk reachable for an announcement");
        emit changed();
        return;
    }
    callId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    peer = Peer();
    isOutgoing = true;
    isBroadcast = true;
    connectedAt = 0;
    startedAt = now();
    isHandsFree = false;
    isSending = false;
    currentState = "broadcasting";
    announcementTitle = targets.size() == 1 ? display(targets.first()) : tr("Announcement");
    updateAnnouncementDetail();
    QString id = callId;
    arm(12000, tr("No kiosk connected"));
    emit changed();
    for (const Peer &target : std::as_const(targets)) {
        post(target, "/api/intercom/call",
             {{"call", id}, {"kind", "broadcast"}, {"from", self()}}, id,
             [this, id, target](const QJsonObject &r, const QString &error) {
                 QString status = r.value("status").toString();
                 if (id != callId || currentState != "broadcasting") {
                     if (status == "listening")
                         post(target, "/api/intercom/call/" + id, {{"action", "hangup"}}, id);
                     return;
                 }
                 if (status == "listening") {
                     announcementPeers.insert(target.id, target);
                     connectAnnouncement(target, id);
                 } else {
                     announcementFailed(id, target.id,
                                        error.isEmpty() || !status.isEmpty()
                                            ? reason(status)
                                            : tr("Not reachable"));
                 }
             });
    }
}

void Engine::updateAnnouncementDetail() {
    QStringList lines;
    lines << tr("%1 of %2 connected").arg(announcementLinks.size()).arg(announcementNames.size());
    for (auto it = announcementNames.cbegin(); it != announcementNames.cend(); ++it)
        lines << it.value() + " · " + announcementStates.value(it.key());
    detailText = lines.join('\n');
}

void Engine::announcementFailed(const QString &id, const QString &peerId, const QString &status) {
    if (id != callId || currentState != "broadcasting") return;
    announcementPeers.remove(peerId);
    announcementStates.insert(peerId, status);
    updateAnnouncementDetail();
    emit changed();
    bool pending = false;
    for (const QString &s : std::as_const(announcementStates))
        pending |= s == tr("Connecting …");
    if (announcementLinks.isEmpty() && !pending) {
        QString recipients = detailText;
        finish(tr("No kiosk is listening any more"), false);
        detailText = recipients;
        emit changed();
    }
}

void Engine::connectAnnouncement(const Peer &target, const QString &id) {
    QWebSocket *ws = socketTo(target);
    QString peerId = target.id;
    connect(ws, &QWebSocket::connected, this, [this, ws, id, peerId] {
        if (id != callId || currentState != "broadcasting" ||
            !announcementPeers.contains(peerId)) {
            ws->close();
            return;
        }
        announcementLinks.insert(peerId, ws);
        if (connectedAt == 0) {
            if (!audio.start(true)) detailText = audio.problem();
            timeout.stop();
            connectedAt = now();
        }
        announcementStates.insert(peerId, tr("Connected"));
        updateAnnouncementDetail();
        ws->sendTextMessage(text({{"type", "talk"}, {"on", isSending}}));
        emit changed();
    });
    connect(ws, &QWebSocket::textMessageReceived, this, [ws](const QString &t) {
        if (parse(t.toUtf8()).value("type").toString() == "end") ws->close();
    });
    connect(ws, &QWebSocket::disconnected, this, [this, ws, id, peerId] {
        ws->deleteLater();
        if (id != callId || currentState != "broadcasting") return;
        if (announcementLinks.value(peerId) == ws) announcementLinks.remove(peerId);
        else if (announcementLinks.contains(peerId)) return;
        announcementFailed(id, peerId, tr("Connection ended"));
    });
    ws->open(socketUrl(target, "/api/intercom/audio/" + id, wire.token(id, config.id())));
}

// ---------------------------------------------------------------- incoming call

QJsonObject Engine::incoming(const QJsonObject &body, const QString &token, const QString &host) {
    QString id = body.value("call").toString();
    QJsonObject from = body.value("from").toObject();
    if (!Wire::validCallId(id) || from.isEmpty()) return {{"code", 400}, {"status", "invalid"}};
    QJsonObject claims = wire.verify(token, id);
    if (claims.isEmpty() || claims.value("from").toString() != from.value("id").toString())
        return {{"code", 403}, {"status", "key"}};
    if (!running) return {{"status", "off"}};
    if (from.value("tls").toBool() != config.tls()) return {{"code", 409}, {"status", "tls"}};
    if (config.dnd()) return {{"status", "dnd"}};
    if (active()) return {{"status", "busy"}};
    Peer caller = Peer::from(from, host);
    if (caller.port < 1 || caller.port > 65535) return {{"code", 400}, {"status", "invalid"}};
    QString kind = body.value("kind").toString("call");
    if (kind != "call" && kind != "broadcast") return {{"code", 400}, {"status", "invalid"}};
    if (kind == "broadcast" && !config.acceptAnnouncements()) return {{"status", "dnd"}};
    resetVideo();
    QJsonObject offer = body.value("video").toObject();
    videoAgreed = kind == "call" && config.video() && body.value("video").isObject();
    if (videoAgreed && offer.value("camera").toBool()) farIsOn = true;
    if (caller.name.isEmpty()) caller.name = config.knownName(caller.id);
    peer = caller;
    callId = id;
    isOutgoing = false;
    isBroadcast = kind == "broadcast";
    isHandsFree = false;
    isSending = false;
    connectedAt = 0;
    startedAt = now();
    detailText.clear();
    if (isBroadcast) {
        currentState = "listening";
        arm(12000, tr("The announcement did not connect"));
    } else {
        currentState = "ringing";
        arm(45000, tr("Missed call"));
        emit incomingCall(peerName(), farIsOn);
        if (caller.tls)
            Tls::inspect(caller.host, caller.port, this,
                         [this, id](const QString &shown, const QString &error) {
                if (id != callId || currentState != "ringing") return;
                confirmed(peer, peer.port, shown, error);
                emit changed();
            });
    }
    emit changed();
    return {{"status", isBroadcast ? "listening" : "ringing"}, {"video", config.video()}};
}

void Engine::answer(bool withCamera) {
    if (currentState != "ringing") return;
    if (peer.tls && (config.pin(peer.host, peer.port).isEmpty() || !peer.candidatePin.isEmpty())) {
        detailText = tr("Please confirm the certificate first");
        emit changed();
        return;
    }
    QString id = callId;
    currentState = "connecting";
    arm(12000, tr("The audio connection failed"));
    if (withCamera) camera(true);
    emit changed();
    post(peer, "/api/intercom/call/" + id, {{"action", "answer"}}, id,
         [this, id](const QJsonObject &r, const QString &) {
             if (!r.value("ok").toBool()) endIf(id, tr("The kiosk did not take the answer"), true);
         });
}

void Engine::decline() {
    if (currentState == "ringing") finish(tr("Call declined"), true);
}

void Engine::hangUp() {
    // Before the kiosk answered, the call was cancelled rather than ended, as the kiosk is told.
    finish(currentState == "calling" ? tr("Call cancelled") : tr("Call ended"), true);
}

QJsonObject Engine::signal(const QString &id, const QJsonObject &body, const QString &token,
                           const QString &) {
    QJsonObject claims = wire.verify(token, id);
    QString action = body.value("action").toString();
    if (id == callId && currentState == "broadcasting" && !claims.isEmpty()) {
        QString member = claims.value("from").toString();
        if (announcementPeers.contains(member) &&
            (action == "hangup" || action == "decline" || action == "cancel")) {
            QPointer<QWebSocket> l = announcementLinks.take(member);
            if (l) {
                l->disconnect(this);
                l->close();
                l->deleteLater();
            }
            announcementFailed(id, member, tr("Closed by the kiosk"));
            return {{"ok", true}};
        }
    }
    if (claims.isEmpty() || id != callId || peer.id.isEmpty() ||
        peer.id != claims.value("from").toString())
        return {{"code", 403}, {"ok", false}};
    if (action == "answer") {
        if (!isOutgoing || currentState != "calling") return {{"code", 409}, {"ok", false}};
        currentState = "connecting";
        arm(12000, tr("The audio connection failed"));
        emit changed();
        Peer p = peer;
        QTimer::singleShot(0, this, [this, p, id] { connectAudio(p, id); });
        return {{"ok", true}};
    }
    if (action == "decline" || action == "missed" || action == "cancel" || action == "hangup") {
        // After the reply: finishing closes sockets the request may still share.
        QTimer::singleShot(0, this, [this, id, action] { endIf(id, reason(action), false); });
        return {{"ok", true}};
    }
    return {{"code", 400}, {"ok", false}};
}

// ---------------------------------------------------------------- audio socket

bool Engine::allowAudio(const QString &id, const QString &token) {
    QJsonObject claims = wire.verify(token, id);
    return !claims.isEmpty() && id == callId && !isOutgoing && !peer.id.isEmpty() &&
           peer.id == claims.value("from").toString() &&
           (currentState == "connecting" || currentState == "listening") && !link;
}

void Engine::connectAudio(const Peer &p, const QString &id) {
    if (id != callId) return;
    QWebSocket *ws = socketTo(p);
    connect(ws, &QWebSocket::connected, this, [this, ws, id] { attach(id, ws); });
    connect(ws, &QWebSocket::errorOccurred, this, [this, ws, id](QAbstractSocket::SocketError) {
        if (link != ws) endIf(id, tr("Audio connection failed: %1").arg(ws->errorString()), false);
    });
    ws->open(socketUrl(p, "/api/intercom/audio/" + id, wire.token(id, config.id())));
}

void Engine::attach(const QString &id, QWebSocket *socket) {
    if (!socket->parent()) socket->setParent(this);
    if (id != callId || !(currentState == "connecting" || currentState == "listening") || link) {
        socket->close();
        socket->deleteLater();
        return;
    }
    link = socket;
    connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket](const QByteArray &b) {
        if (socket == link) audio.play(b);
    });
    connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &t) {
        if (socket == link) message(t);
    });
    connect(socket, &QWebSocket::disconnected, this, [this, socket, id] {
        socket->deleteLater();
        if (socket == link) endIf(id, tr("Connection ended"), false);
    });
    if (!audio.start(!isBroadcast)) detailText = audio.problem();
    timeout.stop();
    connectedAt = now();
    currentState = isBroadcast ? "listening" : "in_call";
    if (!isBroadcast && !audio.microphoneAvailable())
        detailText = tr("Listening only: %1").arg(audio.problem());
    if (!isBroadcast && !config.pushToTalk() && audio.microphoneAvailable()) {
        // Push to talk is off in Settings: the microphone opens with the call.
        isHandsFree = true;
        talk(true);
    } else {
        socket->sendTextMessage(text({{"type", "talk"}, {"on", false}}));
    }
    emit changed();
}

void Engine::message(const QString &t) {
    QJsonObject j = parse(t.toUtf8());
    QString type = j.value("type").toString();
    if (type == "end") {
        endIf(callId, tr("Call ended"), false);
    } else if (type == "talk") {
        detailText = j.value("on").toBool() ? tr("%1 is speaking").arg(peerName()) : tr("Connected");
        emit changed();
    }
}

void Engine::talk(bool on) {
    if (currentState != "in_call" && currentState != "broadcasting") return;
    isSending = on && audio.microphoneAvailable();
    audio.setSending(isSending);
    QString m = text({{"type", "talk"}, {"on", isSending}});
    if (currentState == "broadcasting") {
        for (const auto &l : std::as_const(announcementLinks))
            if (l) l->sendTextMessage(m);
    } else if (link) {
        link->sendTextMessage(m);
    }
    emit changed();
}

void Engine::setHandsFree(bool on) {
    isHandsFree = on;
    talk(on);
}

void Engine::sendAudio(const QByteArray &frame) {
    if (currentState == "broadcasting") {
        for (const auto &l : std::as_const(announcementLinks))
            if (l) l->sendBinaryMessage(frame);
    } else if (link && currentState == "in_call") {
        link->sendBinaryMessage(frame);
    }
}

QString Engine::peerName() const {
    if (isBroadcast && isOutgoing) return announcementTitle;
    QString alias = config.alias(peer.id);
    if (!alias.isEmpty()) return alias;
    return peer.name.isEmpty() ? (peer.host.isEmpty() ? tr("Unknown kiosk") : peer.host)
                               : peer.name;
}

// ---------------------------------------------------------------- video

bool Engine::videoLive() const {
    return currentState == "calling" || currentState == "ringing" ||
           currentState == "connecting" || currentState == "in_call";
}

bool Engine::allowVideo(const QString &id, const QString &token) {
    QJsonObject claims = wire.verify(token, id);
    return !claims.isEmpty() && id == callId && !isOutgoing && videoAgreed && videoLive() &&
           peer.id == claims.value("from").toString();
}

void Engine::attachVideo(const QString &id, QWebSocket *socket) {
    if (!socket->parent()) socket->setParent(this);
    if (id != callId || !videoAgreed || !videoLive()) {
        socket->close();
        socket->deleteLater();
        return;
    }
    setVideoLink(socket);
}

void Engine::setVideoLink(QWebSocket *socket) {
    delete videoLink;
    videoLink = new VideoLink(socket, this);
    QString id = callId;
    connect(videoLink, &VideoLink::frame, this, &Engine::farFrame);
    connect(videoLink, &VideoLink::message, this, [this, id](const QJsonObject &m) {
        if (id != callId) return;
        QString type = m.value("type").toString();
        if (type == "camera") {
            bool on = m.value("on").toBool();
            if (on == farIsOn) return;
            farIsOn = on;
            decoder.reset();
            if (!on) emit farPicture({});
            emit changed();
        } else if (type == "key") {
            encoder.forceKey();
        }
    });
    VideoLink *current = videoLink;
    connect(videoLink, &VideoLink::closed, this, [this, current, id] {
        if (videoLink != current) return;
        videoLink->deleteLater();
        videoLink = nullptr;
        // The voice carries on without video; the caller keeps trying while the call lasts.
        if (id == callId && videoLive() && isOutgoing && videoAgreed) {
            Peer p = peer;
            QTimer::singleShot(2000, this, [this, p, id] {
                if (id == callId && !videoLink) connectVideo(p, id);
            });
        }
    });
    videoLink->send({{"type", "camera"}, {"on", cameraIsOn}});
    if (cameraIsOn) encoder.forceKey();
}

void Engine::connectVideo(const Peer &p, const QString &id) {
    if (id != callId || !videoLive() || videoLink) return;
    QWebSocket *ws = socketTo(p);
    connect(ws, &QWebSocket::connected, this, [this, ws, id] {
        if (id != callId || !videoLive() || videoLink) {
            ws->close();
            ws->deleteLater();
            return;
        }
        setVideoLink(ws);
    });
    connect(ws, &QWebSocket::errorOccurred, this, [this, ws, p, id](QAbstractSocket::SocketError) {
        if (videoLink && videoLink->socket() == ws) return;
        ws->deleteLater();
        QTimer::singleShot(2000, this, [this, p, id] { connectVideo(p, id); });
    });
    ws->open(socketUrl(p, "/api/intercom/video/" + id, wire.token(id, config.id())));
}

bool Engine::camera(bool on) {
    if (on == cameraIsOn) return true;
    if (on) {
        if (isBroadcast || !videoAgreed || !videoLive() || !config.video()) return false;
        cameraIsOn = true;
        encoder.forceKey();
        cameraSource.start(config.camera());
    } else {
        cameraIsOn = false;
        cameraSource.stop();
        emit selfPicture({});
    }
    if (videoLink) videoLink->send({{"type", "camera"}, {"on", cameraIsOn}});
    emit changed();
    return true;
}

void Engine::picture(const QImage &image) {
    if (!cameraIsOn) return;
    emit selfPicture(image);
    if (!videoLink) return;
    QByteArray frame = encoder.encode(image, quint32(clock.elapsed()));
    if (!frame.isEmpty()) videoLink->sendFrame(frame, VideoWire::isKey(frame));
}

void Engine::farFrame(const QByteArray &frame) {
    if (!farIsOn) return;
    QImage image = decoder.decode(frame);
    if (!image.isNull()) {
        emit farPicture(image);
    } else if (!VideoWire::isKey(frame) && videoLink && clock.elapsed() - lastKeyRequest > 1000) {
        // Joined mid-stream or lost a frame: a key frame lets the decoder start again.
        lastKeyRequest = clock.elapsed();
        videoLink->send({{"type", "key"}});
    }
}

void Engine::resetVideo() {
    delete videoLink;
    videoLink = nullptr;
    if (cameraIsOn) cameraSource.stop();
    videoAgreed = cameraIsOn = farIsOn = false;
    decoder.reset();
    emit farPicture({});
    emit selfPicture({});
}

// ---------------------------------------------------------------- ending

void Engine::arm(int ms, const QString &reason) {
    timeoutCall = callId;
    timeoutReason = reason;
    timeout.start(ms);
}

void Engine::endIf(const QString &id, const QString &reason, bool notify) {
    if (!id.isEmpty() && id == callId) finish(reason, notify);
}

void Engine::finish(const QString &reason, bool notify) {
    if (!active()) return;
    QString id = callId, oldState = currentState;
    Peer old = peer;
    // The call history, as the Android app keeps it; an announcement sent is one entry for all,
    // named after its kiosk only when it went to one.
    bool sentAnnouncement = oldState == "broadcasting" && isOutgoing;
    bool toAll = sentAnnouncement && announcementTitle == tr("Announcement");
    // A call that stopped ringing without being taken here is missed, also when the kiosk gave up
    // first, as in the Android app.
    bool missed = oldState == "ringing" && !isOutgoing && (!notify || reason == tr("Missed call"));
    config.addHistory({
        {"id", sentAnnouncement ? QString() : old.id},
        {"name", toAll ? QString() : sentAnnouncement ? announcementTitle
                 : old.name.isEmpty() ? config.knownName(old.id) : old.name},
        {"host", sentAnnouncement ? QString() : old.host},
        {"out", isOutgoing},
        {"announcement", isBroadcast},
        {"video", videoAgreed},
        {"at", startedAt},
        {"seconds", connectedAt ? qMax<qint64>(0, (now() - connectedAt) / 1000) : 0},
        {"result", missed ? tr("Missed call") : reason},
    });
    emit historyChanged();
    if (missed) emit missedCall(peerName());
    if (oldState == "broadcasting") {
        for (auto it = announcementPeers.cbegin(); it != announcementPeers.cend(); ++it)
            if (notify) post(it.value(), "/api/intercom/call/" + id, {{"action", "hangup"}}, id);
        for (const auto &l : std::as_const(announcementLinks)) {
            if (!l) continue;
            l->disconnect(this);
            if (notify) l->sendTextMessage(text({{"type", "end"}}));
            l->close();
            l->deleteLater();
        }
        announcementLinks.clear();
        announcementPeers.clear();
    }
    QPointer<QWebSocket> oldLink = link;
    currentState = "idle";
    callId.clear();
    link = nullptr;
    peer = Peer();
    isSending = isHandsFree = false;
    connectedAt = 0;
    detailText = reason;
    timeout.stop();
    audio.stop();
    resetVideo();
    if (oldLink) {
        oldLink->disconnect(this);
        if (notify) oldLink->sendTextMessage(text({{"type", "end"}}));
        oldLink->close();
        oldLink->deleteLater();
    }
    if (notify && !old.id.isEmpty()) {
        QString action = oldState == "ringing"
                             ? (reason == tr("Missed call") ? "missed" : "decline")
                         : oldState == "calling" ? "cancel"
                                                 : "hangup";
        post(old, "/api/intercom/call/" + id, {{"action", action}}, id);
    }
    emit changed();
}
