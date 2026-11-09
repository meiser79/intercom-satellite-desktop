#include "Config.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>

namespace {
QString defaultFile() {
    QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        "/intercom-satellite";
    QDir().mkpath(dir);
    return dir + "/settings.conf";
}
}  // namespace

Config::Config(const QString &file)
    : store(std::make_unique<QSettings>(file.isEmpty() ? defaultFile() : file,
                                        QSettings::IniFormat)),
      settings(*store) {
    if (!settings.contains("id"))
        settings.setValue("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    prune();
    protect();
}

void Config::protect() {
    settings.sync();
    QFile::setPermissions(settings.fileName(), QFile::ReadOwner | QFile::WriteOwner);
}

QString Config::fileName() const { return settings.fileName(); }

QString Config::id() const { return settings.value("id").toString(); }

QString Config::name() const {
    QString host = QHostInfo::localHostName();
    return settings.value("name", host.isEmpty() ? QStringLiteral("Linux") : host).toString();
}

QString Config::key() const { return settings.value("key").toString(); }

void Config::setIdentity(const QString &name, const QString &key) {
    settings.setValue("name", name);
    settings.setValue("key", key);
    protect();
}

int Config::port() const { return settings.value("port", 2324).toInt(); }
void Config::setPort(int port) { settings.setValue("port", port); }

bool Config::video() const { return settings.value("video", true).toBool(); }
void Config::setVideo(bool on) { settings.setValue("video", on); }

QString Config::camera() const { return settings.value("camera").toString(); }
void Config::setCamera(const QString &camera) { settings.setValue("camera", camera); }

bool Config::acceptAnnouncements() const {
    return settings.value("acceptAnnouncements", true).toBool();
}
void Config::setAcceptAnnouncements(bool on) { settings.setValue("acceptAnnouncements", on); }

bool Config::pushToTalk() const { return settings.value("pushToTalk", true).toBool(); }
void Config::setPushToTalk(bool on) { settings.setValue("pushToTalk", on); }

bool Config::dnd() const { return settings.value("dnd", false).toBool(); }
void Config::setDnd(bool on) { settings.setValue("dnd", on); }

bool Config::startHidden() const { return settings.value("startHidden", false).toBool(); }
void Config::setStartHidden(bool on) { settings.setValue("startHidden", on); }

QByteArray Config::windowGeometry() const { return settings.value("window/geometry").toByteArray(); }
void Config::setWindowGeometry(const QByteArray &geometry) {
    settings.setValue("window/geometry", geometry);
}

bool Config::tls() const { return settings.value("tls", false).toBool(); }
void Config::setTls(bool on) { settings.setValue("tls", on); }

QByteArray Config::certificate() const { return settings.value("tls/certificate").toByteArray(); }
QByteArray Config::certificateKey() const { return settings.value("tls/key").toByteArray(); }

void Config::setCertificate(const QByteArray &certificate, const QByteArray &key) {
    settings.setValue("tls/certificate", QString::fromLatin1(certificate));
    settings.setValue("tls/key", QString::fromLatin1(key));
    protect();
}

namespace {
QString pinGroup(const QString &host, int port) {
    return "pins/" + host + ":" + QString::number(port);
}
}  // namespace

QString Config::pin(const QString &host, int port) const {
    return settings.value(pinGroup(host, port) + "/pin").toString();
}

void Config::setPin(const QString &host, int port, const QString &pin, const QString &id) {
    if (!id.isEmpty()) {
        settings.beginGroup("pins");
        const QStringList addresses = settings.childGroups();
        for (const QString &address : addresses)
            if (address.left(address.lastIndexOf(':')) != host &&
                settings.value(address + "/id").toString() == id)
                settings.remove(address);
        settings.endGroup();
    }
    QString group = pinGroup(host, port);
    settings.setValue(group + "/pin", pin);
    settings.setValue(group + "/id", id);
    settings.setValue(group + "/seen", QDateTime::currentMSecsSinceEpoch());
}

void Config::pinUsed(const QString &host, int port) { touch(pinGroup(host, port)); }

void Config::touch(const QString &group) {
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - settings.value(group + "/seen").toLongLong() > 3600 * 1000)
        settings.setValue(group + "/seen", now);
}

void Config::prune() {
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QString &section : {QStringLiteral("known"), QStringLiteral("pins")}) {
        settings.beginGroup(section);
        for (const QString &entry : settings.childGroups())
            if (now - settings.value(entry + "/seen").toLongLong() > EXPIRY_MS)
                settings.remove(entry);
        settings.endGroup();
    }
}

void Config::forget(const Peer &p) {
    settings.remove(pinGroup(p.host, p.adminPort));
    settings.remove(pinGroup(p.host, p.port));
    if (p.id.isEmpty()) return;
    settings.remove("known/" + p.id);
    settings.beginGroup("pins");
    const QStringList addresses = settings.childGroups();
    for (const QString &address : addresses)
        if (settings.value(address + "/id").toString() == p.id) settings.remove(address);
    settings.endGroup();
}

QList<Peer> Config::manualPeers() const {
    QList<Peer> peers;
    const QJsonArray saved =
        QJsonDocument::fromJson(settings.value("peers", "[]").toByteArray()).array();
    for (const auto &v : saved) {
        QJsonObject j = v.toObject();
        Peer p(j.value("host").toString(), j.value("port").toInt(2324), j.value("tls").toBool());
        p.manual = true;
        if (!p.host.isEmpty()) peers.append(p);
    }
    return peers;
}

void Config::setManualPeers(const QList<Peer> &peers) {
    QJsonArray a;
    for (const Peer &p : peers)
        a.append(QJsonObject{{"host", p.host}, {"port", p.adminPort}, {"tls", p.adminTls}});
    settings.setValue("peers", QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
}

QString Config::knownName(const QString &id) const {
    return settings.value("known/" + id + "/name").toString();
}

QJsonArray Config::history() const {
    return QJsonDocument::fromJson(settings.value("history").toString().toUtf8()).array();
}

void Config::addHistory(const QJsonObject &entry) {
    QJsonArray items = history();
    items.prepend(entry);
    while (items.size() > 100) items.removeLast();
    settings.setValue("history",
                      QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact)));
}

void Config::clearHistory() { settings.remove("history"); }

QString Config::alias(const QString &id) const {
    return id.isEmpty() ? QString() : settings.value("known/" + id + "/alias").toString();
}

void Config::setAlias(const QString &id, const QString &alias) {
    if (id.isEmpty()) return;
    if (alias.isEmpty()) settings.remove("known/" + id + "/alias");
    else settings.setValue("known/" + id + "/alias", alias);
    touch("known/" + id);
}

QString Config::roomIcon(const QString &id) const {
    return settings.value("known/" + id + "/icon", QStringLiteral("home")).toString();
}

void Config::setRoomIcon(const QString &id, const QString &icon) {
    if (id.isEmpty()) return;
    settings.setValue("known/" + id + "/icon", icon);
    touch("known/" + id);
}

void Config::remember(const Peer &p) {
    if (p.id.isEmpty() || p.name.isEmpty()) return;
    if (knownName(p.id) != p.name) settings.setValue("known/" + p.id + "/name", p.name);
    touch("known/" + p.id);
}
