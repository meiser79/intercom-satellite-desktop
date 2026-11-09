#pragma once

#include <QSslConfiguration>
#include <QString>

#include <functional>

class Config;
class QNetworkReply;
class QObject;
class QWebSocket;

/**
 * Encryption as Kiosk Satellite and the Android app use it: self-signed certificates, each
 * peer's pinned by its SHA-256 once the user confirmed it.
 */
namespace Tls {

/** The certificate's SHA-256 in lowercase hex, as Android shows and stores it. */
QString pin(const QSslCertificate &certificate);

/** This app's certificate and key, made on first use and kept in the settings. */
bool server(Config &config, QSslConfiguration *out, QString *error);

/** For connecting to a peer: no certificate is trusted unless its pin matches, see guard(). */
QSslConfiguration client();

/** Lets the request through only to the certificate with this pin; nothing is sent otherwise. */
void guard(QNetworkReply *reply, const QString &pin);
void guard(QWebSocket *socket, const QString &pin);

/**
 * The pin of the certificate at host:port, or empty and why not. Only a handshake: no key,
 * token or sound is sent.
 */
void inspect(const QString &host, int port, QObject *context,
             std::function<void(const QString &pin, const QString &error)> done);

}  // namespace Tls
