#pragma once

#include <QJsonObject>
#include <QString>

/** A kiosk, or another Intercom Satellite, as this app knows it. */
struct Peer {
    enum class Status { Checking, Ready, Unreachable, Off, Dnd, KeyMismatch, TlsMismatch,
                        ConfirmCert };

    QString host;
    int adminPort = 2324;  // where it was found or added; its identity answers there
    int port = 2324;       // where its intercom answers, from the identity
    bool adminTls = false, tls = false;
    QString id, name, version;
    /** A certificate seen at candidatePort that the user has yet to confirm. */
    QString candidatePin;
    int candidatePort = 0;
    /** Why the last check failed, for the user; empty when it didn't. */
    QString problem;
    Status status = Status::Checking;
    bool ready = false;
    bool manual = false;
    bool video = false;
    qint64 checked = 0;
    /** When it last answered, or was found; found devices that stop answering are dropped. */
    qint64 seen = 0;

    Peer() = default;
    Peer(QString host, int port, bool tls)
        : host(std::move(host)), adminPort(port), port(port), adminTls(tls), tls(tls) {}

    QString addressKey() const { return host + ":" + QString::number(adminPort); }

    QString base(bool admin) const {
        bool secure = admin ? adminTls : tls;
        QString h = host.contains(':') ? "[" + host + "]" : host;
        return QString(secure ? "https://" : "http://") + h + ":" +
               QString::number(admin ? adminPort : port);
    }

    /** The caller of a request, at the address the request really came from. */
    static Peer from(const QJsonObject &j, const QString &actualHost) {
        Peer p(actualHost, j.value("port").toInt(2324), j.value("tls").toBool());
        p.id = j.value("id").toString();
        p.name = j.value("name").toString();
        p.version = j.value("version").toString();
        return p;
    }
};
