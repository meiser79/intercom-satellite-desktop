#include "Tls.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QNetworkReply>
#include <QSslKey>
#include <QSslSocket>
#include <QTimer>
#include <QWebSocket>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <memory>

#include "Config.h"

namespace {
QByteArray pem(const std::function<int(BIO *)> &write) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio || write(bio.get()) != 1) return {};
    char *data = nullptr;
    long size = BIO_get_mem_data(bio.get(), &data);
    return QByteArray(data, int(size));
}

/** A self-signed RSA certificate like Android's: CN=Intercom Satellite, ten years. */
bool make(QByteArray *certificate, QByteArray *key) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(EVP_RSA_gen(2048), EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> x509(X509_new(), X509_free);
    if (!pkey || !x509) return false;
    X509_set_version(x509.get(), 2);
    ASN1_INTEGER_set_int64(X509_get_serialNumber(x509.get()), QDateTime::currentMSecsSinceEpoch());
    X509_gmtime_adj(X509_getm_notBefore(x509.get()), -86400L);
    X509_gmtime_adj(X509_getm_notAfter(x509.get()), 10L * 365 * 86400);
    X509_set_pubkey(x509.get(), pkey.get());
    X509_NAME *name = X509_get_subject_name(x509.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                               reinterpret_cast<const unsigned char *>("Intercom Satellite"), -1,
                               -1, 0);
    X509_set_issuer_name(x509.get(), name);
    if (X509_sign(x509.get(), pkey.get(), EVP_sha256()) == 0) return false;
    *certificate = pem([&](BIO *b) { return PEM_write_bio_X509(b, x509.get()); });
    *key = pem([&](BIO *b) {
        return PEM_write_bio_PrivateKey(b, pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
    });
    return !certificate->isEmpty() && !key->isEmpty();
}

/** The certificate the peer showed, from the errors it caused or the connection. */
QSslCertificate shown(const QList<QSslError> &errors, const QSslConfiguration &connection) {
    for (const QSslError &e : errors)
        if (!e.certificate().isNull()) return e.certificate();
    return connection.peerCertificate();
}
}  // namespace

namespace Tls {

QString pin(const QSslCertificate &certificate) {
    if (certificate.isNull()) return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(certificate.toDer(), QCryptographicHash::Sha256).toHex());
}

bool server(Config &config, QSslConfiguration *out, QString *error) {
    if (!QSslSocket::supportsSsl()) {
        *error = QCoreApplication::translate("Tls", "TLS is not available on this system");
        return false;
    }
    QSslCertificate certificate(config.certificate());
    QSslKey key(config.certificateKey(), QSsl::Rsa);
    if (certificate.isNull() || key.isNull()) {
        QByteArray c, k;
        if (!make(&c, &k)) {
            *error = QCoreApplication::translate("Tls", "Could not create a certificate");
            return false;
        }
        config.setCertificate(c, k);
        certificate = QSslCertificate(c);
        key = QSslKey(k, QSsl::Rsa);
    }
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setLocalCertificate(certificate);
    ssl.setPrivateKey(key);
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    *out = ssl;
    return true;
}

QSslConfiguration client() {
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    // No authorities at all: every certificate raises errors, and only a pinned one is let past.
    ssl.setCaCertificates({});
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    return ssl;
}

void guard(QNetworkReply *reply, const QString &expected) {
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply, expected](const QList<QSslError> &errors) {
                         if (!expected.isEmpty() &&
                             pin(shown(errors, reply->sslConfiguration())) == expected)
                             reply->ignoreSslErrors();
                     });
    // Before any data goes out: a certificate that got through some other way still has to match.
    QObject::connect(reply, &QNetworkReply::encrypted, reply, [reply, expected] {
        QSslCertificate c = reply->sslConfiguration().peerCertificate();
        if (!c.isNull() && pin(c) != expected) reply->abort();
    });
}

void guard(QWebSocket *socket, const QString &expected) {
    socket->setSslConfiguration(client());
    QObject::connect(socket, &QWebSocket::sslErrors, socket,
                     [socket, expected](const QList<QSslError> &errors) {
                         if (!expected.isEmpty() &&
                             pin(shown(errors, socket->sslConfiguration())) == expected)
                             socket->ignoreSslErrors();
                     });
}

void inspect(const QString &host, int port, QObject *context,
             std::function<void(const QString &pin, const QString &error)> done) {
    auto *socket = new QSslSocket(context);
    socket->setPeerVerifyMode(QSslSocket::VerifyNone);
    auto finish = [socket, done, called = std::make_shared<bool>(false)](const QString &result,
                                                                         const QString &error) {
        if (*called) return;
        *called = true;
        socket->abort();
        socket->deleteLater();
        done(result, error);
    };
    QObject::connect(socket, &QSslSocket::encrypted, context, [socket, finish] {
        QString shown = pin(socket->peerCertificate());
        finish(shown, shown.isEmpty() ? QCoreApplication::translate("Tls", "No certificate")
                                      : QString());
    });
    QObject::connect(socket, &QSslSocket::errorOccurred, context,
                     [socket, finish](QAbstractSocket::SocketError) {
                         finish({}, socket->errorString());
                     });
    QTimer::singleShot(4000, socket, [finish] {
        finish({}, QCoreApplication::translate("Tls", "No answer within 4 seconds"));
    });
    socket->connectToHostEncrypted(host, quint16(port));
}

}  // namespace Tls
