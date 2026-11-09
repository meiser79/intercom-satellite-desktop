#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSettings>
#include <QString>

#include <memory>

#include "Peer.h"

/**
 * Settings in ~/.config/intercom-satellite/settings.conf, readable by this user only since it
 * holds the intercom key. A different file can be given for running a second instance.
 */
class Config {
public:
    explicit Config(const QString &file = {});
    /** The settings file; one app runs per file. */
    QString fileName() const;

    QString id() const;
    QString name() const;
    QString key() const;
    void setIdentity(const QString &name, const QString &key);

    int port() const;
    void setPort(int port);

    bool video() const;
    void setVideo(bool on);
    /** A camera's QCameraDevice id, "test" for a test pattern or empty for the default one. */
    QString camera() const;
    void setCamera(const QString &camera);

    bool acceptAnnouncements() const;
    void setAcceptAnnouncements(bool on);
    /** Push to talk; off means the microphone opens with the call. */
    bool pushToTalk() const;
    void setPushToTalk(bool on);
    bool dnd() const;
    void setDnd(bool on);
    bool startHidden() const;
    void setStartHidden(bool on);
    /** Where the window was last, as QWidget::saveGeometry() gives it; empty at first. */
    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);

    /** Encrypted calls; must match "Encrypt communications" on the kiosks. */
    bool tls() const;
    void setTls(bool on);
    /** This app's certificate and its key, as PEM. */
    QByteArray certificate() const;
    QByteArray certificateKey() const;
    void setCertificate(const QByteArray &certificate, const QByteArray &key);
    /** The confirmed certificate of the peer at host:port, empty when none is. */
    QString pin(const QString &host, int port) const;
    /** Confirms a certificate; the device's pins on other hosts go, it has moved. */
    void setPin(const QString &host, int port, const QString &pin, const QString &id);
    /** The pin at host:port matched again, which keeps it from expiring. */
    void pinUsed(const QString &host, int port);

    QList<Peer> manualPeers() const;
    void setManualPeers(const QList<Peer> &peers);
    /** The name a kiosk had when last seen, for calls from kiosks not found right now. */
    QString knownName(const QString &id) const;
    void remember(const Peer &p);
    /** The room symbol picked for a kiosk (resources/icons/room_<name>.svg); home by default. */
    QString roomIcon(const QString &id) const;
    /** The name the user gave a kiosk, shown instead of its own; empty when none. */
    QString alias(const QString &id) const;
    void setAlias(const QString &id, const QString &alias);
    void setRoomIcon(const QString &id, const QString &icon);
    /**
     * Calls and announcements, newest first, as in the Android app: who, when, which way, how
     * long and how they ended. Never audio. The last 100 are kept.
     */
    QJsonArray history() const;
    void addHistory(const QJsonObject &entry);
    void clearHistory();
    /** Drops what is kept about a device the user removed: its name and its pins. */
    void forget(const Peer &p);

    /** Known names and pins unused for this long are dropped at start. */
    static constexpr qint64 EXPIRY_MS = 30LL * 24 * 3600 * 1000;

private:
    /** Writes now, readable by this user only. */
    void protect();
    /** Records a use of the group, at most once an hour so checks don't keep writing. */
    void touch(const QString &group);
    /** Drops expired known names and pins. */
    void prune();

    std::unique_ptr<QSettings> store;
    QSettings &settings;
};
