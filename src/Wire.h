#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>

/**
 * The intercom's signed tokens: each request and socket of a call carries one, made with the
 * shared key, valid for a minute and used once. The same format as Kiosk Satellite and the
 * Android app.
 */
class Wire {
public:
    static constexpr int FRAME_BYTES = 2560;  // 80 ms of 16 kHz mono PCM16

    explicit Wire(QString key, std::function<qint64()> clock = nullptr);

    QString token(const QString &call, const QString &from) const;
    /** The token's claims, or an empty object when it is not valid for this call. */
    QJsonObject verify(const QString &token, const QString &call);

    /** The first 8 hex digits of the key's SHA-256: tells a wrong key without revealing it. */
    static QString fingerprint(const QString &key);
    static bool validCallId(const QString &id);

private:
    QString sign(const QByteArray &payload) const;

    QString key;
    std::function<qint64()> clock;
    QHash<QString, qint64> used;
};
