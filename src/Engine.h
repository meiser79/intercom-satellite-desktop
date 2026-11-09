#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWebSocket>

#include <functional>
#include <memory>

#include "Audio.h"
#include "Camera.h"
#include "Config.h"
#include "Peer.h"
#include "VideoCodec.h"
#include "Wire.h"

class Discovery;
class HttpServer;
class VideoLink;

/**
 * The intercom: finds kiosks, places and takes calls and announcements, and runs their sound and
 * pictures. The protocol and its timings follow Kiosk Satellite and the Android app's Engine.
 *
 * States: idle; calling (this side rings a kiosk); ringing (a kiosk rings here); connecting
 * (answered, the audio socket opens); in_call; listening (an announcement plays here);
 * broadcasting (this side makes an announcement).
 */
class Engine : public QObject {
    Q_OBJECT
public:
    static constexpr int MAX_ANNOUNCEMENT_TARGETS = 32;
    /** A found device that hasn't answered for this long is dropped; mDNS brings it back. */
    static constexpr qint64 STALE_MS = 120000;

    explicit Engine(Config &config, QObject *parent = nullptr);
    ~Engine() override;

    bool start(QString *error, bool mdns = true);
    void stop();
    QString discoveryProblem() const { return mdnsProblem; }

    // ---- kiosks
    QList<Peer> peerList() const;
    bool add(const QString &address, QString *error);
    void remove(const QString &addressKey);
    void refresh();
    QString display(const Peer &p) const;
    /** Accepts the certificate the kiosk showed, as its Peer::candidatePin. */
    void trust(const QString &addressKey);

    // ---- calls
    void call(const QString &addressKey, bool withCamera);
    /** To the kiosks with these ids, or to every reachable one when the list is empty. */
    void announce(const QStringList &ids = {});
    void answer(bool withCamera);
    void decline();
    void hangUp();
    void talk(bool on);
    void setHandsFree(bool on);
    bool camera(bool on);
    /** Accepts the certificate of the kiosk calling here. */
    void trustCaller();

    QString state() const { return currentState; }
    bool active() const { return currentState != "idle"; }
    bool outgoing() const { return isOutgoing; }
    bool broadcast() const { return isBroadcast; }
    bool sending() const { return isSending; }
    bool handsFree() const { return isHandsFree; }
    bool peerVideo() const { return videoAgreed; }
    bool cameraOn() const { return cameraIsOn; }
    bool farCamera() const { return farIsOn; }
    bool microphoneAvailable() const { return audio.microphoneAvailable(); }
    float level() const { return audio.level(); }
    /** The other side's name, or the announcement's title. */
    QString peerName() const;
    /** The other side of the call: its candidatePin asks for the certificate to be confirmed. */
    const Peer &callPeer() const { return peer; }
    QString detail() const { return detailText; }
    /** When the call connected, 0 before. */
    qint64 since() const { return connectedAt; }

signals:
    void changed();
    void incomingCall(const QString &name, bool video);
    void missedCall(const QString &name);
    /** A call or announcement ended and went into the history. */
    void historyChanged();
    void farPicture(const QImage &image);
    void selfPicture(const QImage &image);

private:
    using Reply = std::function<void(const QJsonObject &reply, const QString &error)>;

    QJsonObject identity() const;
    QJsonObject self() const;
    /** Where calls reach this app: the port after the plain one when encrypted. */
    int endpointPort() const;
    void found(const QString &host, int port, bool tls, const QString &id, const QString &name);
    void probe(const QString &key);
    void fetchIdentity(const QString &key);
    void settle(const QString &key, Peer::Status status, const QString &problem = {});
    /** Whether the certificate shown at port is the confirmed one; else asks to confirm it. */
    bool confirmed(Peer &p, int port, const QString &shown, const QString &error);
    QWebSocket *socketTo(const Peer &p);
    void savePeers();
    void post(const Peer &p, const QString &path, const QJsonObject &body, const QString &id,
              Reply done = nullptr);
    QString reason(const QString &status) const;

    QJsonObject incoming(const QJsonObject &body, const QString &token, const QString &host);
    QJsonObject signal(const QString &id, const QJsonObject &body, const QString &token,
                       const QString &host);
    bool allowAudio(const QString &id, const QString &token);
    void connectAudio(const Peer &p, const QString &id);
    void attach(const QString &id, QWebSocket *socket);
    void message(const QString &text);

    void connectAnnouncement(const Peer &target, const QString &id);
    void announcementFailed(const QString &id, const QString &peerId, const QString &status);
    void updateAnnouncementDetail();

    bool videoLive() const;
    bool allowVideo(const QString &id, const QString &token);
    void attachVideo(const QString &id, QWebSocket *socket);
    void connectVideo(const Peer &p, const QString &id);
    void setVideoLink(QWebSocket *socket);
    void picture(const QImage &image);
    void farFrame(const QByteArray &frame);
    void resetVideo();

    void arm(int ms, const QString &reason);
    void endIf(const QString &id, const QString &reason, bool notify);
    void finish(const QString &reason, bool notify);
    void sendAudio(const QByteArray &frame);

    Config &config;
    Wire wire;
    QNetworkAccessManager net;
    Audio audio;
    Camera cameraSource;
    VideoEncoder encoder;
    VideoDecoder decoder;
    std::unique_ptr<HttpServer> server, secureServer;
    std::unique_ptr<Discovery> discovery;
    QString mdnsProblem;
    bool running = false;

    QMap<QString, Peer> peers;  // by address
    QTimer refreshTimer, pingTimer, timeout;
    QString timeoutCall, timeoutReason;

    QString currentState = "idle", callId, detailText, announcementTitle;
    Peer peer;
    bool isOutgoing = false, isBroadcast = false, isSending = false, isHandsFree = false;
    qint64 startedAt = 0, connectedAt = 0;
    QPointer<QWebSocket> link;

    QHash<QString, Peer> announcementPeers;               // by peer id
    QHash<QString, QPointer<QWebSocket>> announcementLinks;
    QMap<QString, QString> announcementNames, announcementStates;

    bool videoAgreed = false, cameraIsOn = false, farIsOn = false;
    VideoLink *videoLink = nullptr;
    QElapsedTimer clock;
    qint64 lastKeyRequest = -10000;
};
