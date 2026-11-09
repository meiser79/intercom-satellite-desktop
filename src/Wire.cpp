#include "Wire.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QUuid>

Wire::Wire(QString key, std::function<qint64()> clock)
    : key(std::move(key)),
      clock(clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); }) {}

QString Wire::sign(const QByteArray &payload) const {
    QByteArray mac = QMessageAuthenticationCode::hash(
        payload, ("intercom:" + key).toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(mac.toBase64(QByteArray::Base64UrlEncoding));
}

QString Wire::token(const QString &call, const QString &from) const {
    QJsonObject claims{{"intercom", call},
                       {"from", from},
                       {"n", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                       {"exp", clock() + 60000}};
    QByteArray payload = QJsonDocument(claims)
                             .toJson(QJsonDocument::Compact)
                             .toBase64(QByteArray::Base64UrlEncoding);
    return QString::fromLatin1(payload) + "." + sign(payload);
}

QJsonObject Wire::verify(const QString &token, const QString &call) {
    if (key.isEmpty() || token.size() > 8192) return {};
    QStringList parts = token.split('.');
    if (parts.size() != 2) return {};
    QByteArray payload = parts[0].toLatin1();
    QByteArray expected = sign(payload).toLatin1(), given = parts[1].toLatin1();
    // Compared in full whatever the first difference, like MessageDigest.isEqual.
    if (expected.size() != given.size()) return {};
    char difference = 0;
    for (int i = 0; i < expected.size(); i++) difference |= expected[i] ^ given[i];
    if (difference) return {};
    QJsonObject claims =
        QJsonDocument::fromJson(QByteArray::fromBase64(payload, QByteArray::Base64UrlEncoding))
            .object();
    qint64 now = clock(), exp = claims.value("exp").toInteger();
    if (exp <= now || exp > now + 90000 || claims.value("intercom").toString() != call ||
        claims.value("from").toString().isEmpty())
        return {};
    for (auto it = used.begin(); it != used.end();) {
        if (it.value() < now) it = used.erase(it);
        else ++it;
    }
    if (used.contains(token) || used.size() > 4096) return {};
    used.insert(token, exp);
    return claims;
}

QString Wire::fingerprint(const QString &key) {
    if (key.isEmpty()) return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
}

bool Wire::validCallId(const QString &id) {
    static const QRegularExpression valid("^[A-Za-z0-9_-]{1,160}$");
    return valid.match(id).hasMatch();
}
