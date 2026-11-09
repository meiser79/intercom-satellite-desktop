#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSslConfiguration>
#include <QTcpServer>
#include <QWebSocketServer>

#include <functional>
#include <memory>

class QTcpSocket;
class QWebSocket;

/**
 * The intercom's own endpoint, as Kiosk Satellite and the Android app serve it: the identity,
 * call requests and signals over HTTP, and the audio and video WebSockets of a call.
 */
class HttpServer : public QObject {
    Q_OBJECT
public:
    struct Handlers {
        std::function<QJsonObject()> identity;
        /** Replies may carry "code" for the HTTP status; it stays in the body as on Android. */
        std::function<QJsonObject(const QJsonObject &body, const QString &token, const QString &host)>
            incoming;
        std::function<QJsonObject(const QString &id, const QJsonObject &body, const QString &token,
                                  const QString &host)>
            signal;
        std::function<bool(const QString &id, const QString &token, const QString &host)> allowAudio,
            allowVideo;
        std::function<void(const QString &id, QWebSocket *socket)> audio, video;
    };

    enum class Mode {
        Plain,
        /** The plain port beside an encrypted one: the identity only, the rest is refused. */
        IdentityOnly,
        Encrypted,
    };

    /** ssl is the certificate for Mode::Encrypted. */
    HttpServer(Handlers handlers, Mode mode, const QSslConfiguration &ssl = {},
               QObject *parent = nullptr);
    bool listen(quint16 port);
    QString errorString() const { return tcp->errorString(); }

private:
    void accept();
    void read(QTcpSocket *socket);
    void respond(QTcpSocket *socket, int code, const QJsonObject &body);
    void upgraded();

    Handlers handlers;
    Mode mode;
    std::unique_ptr<QTcpServer> tcp;
    QWebSocketServer sockets;
};
