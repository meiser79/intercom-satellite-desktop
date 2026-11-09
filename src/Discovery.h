#pragma once

#include <QHash>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>

#include <functional>

/**
 * Finding kiosks and phones, and being found: DNS-SD over multicast DNS for
 * "_kiosk-satellite._tcp", announced and browsed without avahi. The service is announced as
 * "ks-<id>" with the id, name, version and port in its TXT record, as the Android app does.
 */
class Discovery : public QObject {
    Q_OBJECT
public:
    Discovery(QString id, QString name, QString version, int port, QObject *parent = nullptr);
    /** Says goodbye, so others notice at once that this intercom left or restarts elsewhere. */
    ~Discovery() override;
    /** False when the mDNS port can't be used; adding kiosks by address still works. */
    bool start();
    QString errorString() const { return error; }

    /** This computer's address on the network its default route goes to. */
    static QString localIp();

signals:
    void found(const QString &host, int port, bool tls, const QString &id, const QString &name);

private:
    /** A network to be found on, with this computer's address there. */
    struct Link {
        QNetworkInterface interface;
        QHostAddress ip;
    };
    static QList<Link> links();

    struct Service {
        QString target;
        int port = 0;
        QHash<QString, QString> txt;
        bool hasTxt = false;
    };

    /** Joins the mDNS group on networks not joined yet; false when on none. */
    bool join();
    /** Records with no time to live are a goodbye. */
    void announce(bool goodbye = false);
    void query();
    void read();
    void handle(const QByteArray &packet);
    void sendQuery(const QList<QPair<QString, quint16>> &questions);
    /** On every network, or the default one when none is known. */
    void send(const std::function<QByteArray(const QHostAddress &ip)> &packet);
    QByteArray records(const QHostAddress &ip, bool goodbye) const;

    QString id, name, version, instance, hostName, error;
    int port;
    QUdpSocket socket;
    QTimer timer;
    int ticks = 0;
    QHash<QString, Service> services;  // by instance name, lower case
    QHash<QString, QString> addresses; // host name (lower case) to IPv4 address
    QSet<QString> joined;              // interface names
};
