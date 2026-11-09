#include "HttpServer.h"

#include <QJsonDocument>
#include <QSslServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWebSocket>

#include "Wire.h"

namespace {
constexpr auto AUDIO_PREFIX = "/api/intercom/audio/";
constexpr auto VIDEO_PREFIX = "/api/intercom/video/";
constexpr int MAX_HEADER = 16 * 1024, MAX_BODY = 64 * 1024;

QString reason(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 426: return "Upgrade Required";
    default: return "Intercom";
    }
}
}  // namespace

HttpServer::HttpServer(Handlers handlers, Mode mode, const QSslConfiguration &ssl,
                       QObject *parent)
    : QObject(parent),
      handlers(std::move(handlers)),
      mode(mode),
      // The TLS server hands over connections already decrypted, so the WebSocket server reads
      // them like plain ones.
      sockets("Intercom Satellite", QWebSocketServer::NonSecureMode) {
    if (mode == Mode::Encrypted) {
        auto *secure = new QSslServer;
        secure->setSslConfiguration(ssl);
        tcp.reset(secure);
    } else {
        tcp = std::make_unique<QTcpServer>();
    }
    // Emitted once a connection is ready: for TLS after its handshake.
    connect(tcp.get(), &QTcpServer::pendingConnectionAvailable, this, &HttpServer::accept);
    connect(&sockets, &QWebSocketServer::newConnection, this, &HttpServer::upgraded);
}

bool HttpServer::listen(quint16 port) { return tcp->listen(QHostAddress::Any, port); }

void HttpServer::accept() {
    while (QTcpSocket *socket = tcp->nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        // A request that never completes doesn't hold its connection.
        QTimer::singleShot(10000, socket, [socket] {
            if (socket->property("handled").isNull()) socket->abort();
        });
    }
}

void HttpServer::read(QTcpSocket *socket) {
    if (!socket->property("handled").isNull()) return;
    // Peeked, not read: a WebSocket handshake goes on to QWebSocketServer untouched.
    QByteArray data = socket->peek(socket->bytesAvailable());
    int end = data.indexOf("\r\n\r\n");
    if (end < 0) {
        if (data.size() > MAX_HEADER) socket->abort();
        return;
    }
    QList<QByteArray> lines = data.left(end).split('\n');
    QList<QByteArray> request = lines.value(0).trimmed().split(' ');
    if (request.size() < 3) return respond(socket, 400, {{"error", "invalid request"}});
    QString method = QString::fromLatin1(request[0]);
    QUrl url(QString::fromLatin1(request[1]));
    QString path = url.path();
    QHash<QString, QString> headers;
    for (int i = 1; i < lines.size(); i++) {
        int colon = lines[i].indexOf(':');
        if (colon > 0)
            headers.insert(QString::fromLatin1(lines[i].left(colon)).trimmed().toLower(),
                           QString::fromUtf8(lines[i].mid(colon + 1)).trimmed());
    }
    QString host = socket->peerAddress().toString();
    if (socket->peerAddress().protocol() == QAbstractSocket::IPv6Protocol) {
        bool mapped = false;
        QHostAddress v4(socket->peerAddress().toIPv4Address(&mapped));
        if (mapped) host = v4.toString();
    }

    if (method == "GET" && path == "/api/intercom/identity") {
        socket->read(end + 4);
        return respond(socket, 200, handlers.identity());
    }
    if (mode == Mode::IdentityOnly) {
        socket->read(end + 4);
        return respond(socket, 426, {{"status", "tls"}});
    }

    bool audio = path.startsWith(AUDIO_PREFIX), video = path.startsWith(VIDEO_PREFIX);
    if (audio || video) {
        QString id = path.mid(QString(audio ? AUDIO_PREFIX : VIDEO_PREFIX).size());
        QString token = QUrlQuery(url).queryItemValue("token", QUrl::FullyDecoded);
        bool allowed = Wire::validCallId(id) &&
                       headers.value("upgrade").compare("websocket", Qt::CaseInsensitive) == 0 &&
                       (audio ? handlers.allowAudio(id, token, host)
                              : handlers.allowVideo(id, token, host));
        if (!allowed) {
            socket->read(end + 4);
            return respond(socket, 403, {{"error", "refused"}});
        }
        socket->setProperty("handled", true);
        disconnect(socket, nullptr, this, nullptr);
        disconnect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        sockets.handleConnection(socket);
        return;
    }

    int length = headers.value("content-length", "0").toInt();
    if (length < 0 || length > MAX_BODY) {
        socket->read(end + 4);
        return respond(socket, 413, {{"error", "invalid body"}});
    }
    if (data.size() < end + 4 + length) return;  // the rest of the body is on its way
    socket->read(end + 4);
    QByteArray bodyBytes = socket->read(length);
    if (method != "POST" ||
        (path != "/api/intercom/call" && !path.startsWith("/api/intercom/call/")))
        return respond(socket, 404, {{"error", "not found"}});
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(bodyBytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return respond(socket, 400, {{"error", "invalid request"}});
    QString auth = headers.value("authorization");
    QString token = auth.startsWith("Bearer ") ? auth.mid(7) : QString();
    QJsonObject answer =
        path == "/api/intercom/call"
            ? handlers.incoming(doc.object(), token, host)
            : handlers.signal(path.mid(QString("/api/intercom/call/").size()), doc.object(), token,
                              host);
    respond(socket, answer.value("code").toInt(200), answer);
}

void HttpServer::respond(QTcpSocket *socket, int code, const QJsonObject &body) {
    socket->setProperty("handled", true);
    QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " " + reason(code).toLatin1() +
                      "\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(json.size()) +
                      "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    socket->write(head + json);
    socket->disconnectFromHost();
}

void HttpServer::upgraded() {
    while (QWebSocket *socket = sockets.nextPendingConnection()) {
        QString path = socket->requestUrl().path();
        if (path.startsWith(AUDIO_PREFIX))
            handlers.audio(path.mid(QString(AUDIO_PREFIX).size()), socket);
        else if (path.startsWith(VIDEO_PREFIX))
            handlers.video(path.mid(QString(VIDEO_PREFIX).size()), socket);
        else
            socket->close();
    }
}
