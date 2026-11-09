#include "Discovery.h"

#include <QFile>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QVariant>
#include <QtEndian>

namespace {
const QString SERVICE = "_kiosk-satellite._tcp.local";
const QHostAddress GROUP("224.0.0.251");
constexpr quint16 MDNS_PORT = 5353;
enum : quint16 { TYPE_A = 1, TYPE_PTR = 12, TYPE_TXT = 16, TYPE_SRV = 33 };

QByteArray encodeName(const QString &name) {
    QByteArray out;
    for (const QString &label : name.split('.', Qt::SkipEmptyParts)) {
        QByteArray l = label.toUtf8().left(63);
        out.append(char(l.size())).append(l);
    }
    return out.append('\0');
}

void put16(QByteArray &out, quint16 v) {
    out.append(char(v >> 8)).append(char(v & 0xff));
}

void put32(QByteArray &out, quint32 v) {
    put16(out, quint16(v >> 16));
    put16(out, quint16(v & 0xffff));
}

QByteArray record(const QString &name, quint16 type, const QByteArray &data, quint32 ttl,
                  bool flush) {
    QByteArray out = encodeName(name);
    put16(out, type);
    put16(out, flush ? 0x8001 : 0x0001);
    put32(out, ttl);
    put16(out, quint16(data.size()));
    return out + data;
}

/** A possibly compressed name at offset; offset moves past it. Empty on a broken packet. */
QString readName(const QByteArray &d, int &offset) {
    QStringList labels;
    int pos = offset, end = -1;
    for (int jumps = 0; jumps < 32; jumps++) {
        if (pos >= d.size()) return {};
        quint8 n = quint8(d[pos]);
        if (n == 0) {
            pos++;
            break;
        }
        if ((n & 0xc0) == 0xc0) {
            if (pos + 1 >= d.size()) return {};
            if (end < 0) end = pos + 2;
            pos = ((n & 0x3f) << 8) | quint8(d[pos + 1]);
            continue;
        }
        if (pos + 1 + n > d.size()) return {};
        labels << QString::fromUtf8(d.mid(pos + 1, n));
        pos += 1 + n;
    }
    offset = end >= 0 ? end : pos;
    return labels.join('.');
}

quint16 get16(const QByteArray &d, int at) {
    return quint16((quint8(d[at]) << 8) | quint8(d[at + 1]));
}
}  // namespace

Discovery::Discovery(QString id, QString name, QString version, int port, QObject *parent)
    : QObject(parent),
      id(std::move(id)),
      name(std::move(name)),
      version(std::move(version)),
      port(port) {
    instance = "ks-" + this->id + "." + SERVICE;
    hostName = "intercom-" + this->id.left(8) + ".local";
    connect(&socket, &QUdpSocket::readyRead, this, &Discovery::read);
    // Right away, after 1 and 3 seconds, then every 30 seconds.
    connect(&timer, &QTimer::timeout, this, [this] {
        ticks++;
        if (ticks == 1 || ticks == 3 || ticks % 30 == 0) {
            if (ticks % 30 == 0) join();  // a network that came up since
            announce();
            query();
        }
    });
}

Discovery::~Discovery() {
    if (!joined.isEmpty()) announce(true);
}

bool Discovery::start() {
    if (!socket.bind(QHostAddress::AnyIPv4, MDNS_PORT,
                     QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        error = socket.errorString();
        return false;
    }
    if (!join() && !socket.joinMulticastGroup(GROUP)) {
        error = socket.errorString();
        return false;
    }
    socket.setSocketOption(QAbstractSocket::MulticastTtlOption, QVariant(255));
    announce();
    query();
    timer.start(1000);
    return true;
}

namespace {
bool virtualBridge(const QString &n) {
    return n.startsWith("docker") || n.startsWith("br-") || n.startsWith("virbr") ||
           n.startsWith("veth") || n.startsWith("lxc") || n.startsWith("vnet");
}

QHostAddress ipv4(const QNetworkInterface &i) {
    for (const QNetworkAddressEntry &e : i.addressEntries())
        if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) return e.ip();
    return {};
}

/** The interface of the IPv4 default route with the lowest metric, from the kernel's table. */
QString defaultInterface() {
    QFile routes("/proc/net/route");
    if (!routes.open(QIODevice::ReadOnly)) return {};
    QString best;
    long bestMetric = -1;
    routes.readLine();  // the header
    while (!routes.atEnd()) {
        QList<QByteArray> f = routes.readLine().simplified().split(' ');
        // Iface Destination Gateway Flags RefCnt Use Metric Mask ...
        if (f.size() < 8 || f[1] != "00000000" || f[7] != "00000000") continue;
        if (!(f[3].toUInt(nullptr, 16) & 0x1)) continue;  // RTF_UP
        long metric = f[6].toLong();
        if (bestMetric < 0 || metric < bestMetric) {
            best = QString::fromLatin1(f[0]);
            bestMetric = metric;
        }
    }
    return best;
}
}  // namespace

QList<Discovery::Link> Discovery::links() {
    QList<Link> out;
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
        auto f = i.flags();
        if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) ||
            !(f & QNetworkInterface::CanMulticast) || (f & QNetworkInterface::IsLoopBack) ||
            virtualBridge(i.name()))
            continue;
        QHostAddress ip = ipv4(i);
        if (!ip.isNull()) out.append({i, ip});
    }
    return out;
}

bool Discovery::join() {
    bool any = false;
    for (const Link &l : links()) {
        if (joined.contains(l.interface.name())) {
            any = true;
        } else if (socket.joinMulticastGroup(GROUP, l.interface)) {
            joined.insert(l.interface.name());
            any = true;
        }
    }
    return any;
}

QString Discovery::localIp() {
    QNetworkInterface route = QNetworkInterface::interfaceFromName(defaultInterface());
    QHostAddress ip = route.isValid() ? ipv4(route) : QHostAddress();
    if (!ip.isNull()) return ip.toString();
    // No default route: a local network without internet; any real interface will do.
    QString fallback;
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
        auto f = i.flags();
        if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) ||
            (f & QNetworkInterface::IsLoopBack) || virtualBridge(i.name()))
            continue;
        QHostAddress a = ipv4(i);
        if (a.isNull()) continue;
        QString n = i.name();
        if (n.startsWith("en") || n.startsWith("eth") || n.startsWith("wl")) return a.toString();
        if (fallback.isEmpty()) fallback = a.toString();
    }
    return fallback;
}

QByteArray Discovery::records(const QHostAddress &ip, bool goodbye) const {
    quint32 shortTtl = goodbye ? 0 : 120, longTtl = goodbye ? 0 : 4500;
    QByteArray txt;
    const QList<QPair<QString, QString>> items = {
        {"id", id}, {"name", name}, {"version", version},
        {"port", QString::number(port)}, {"tls", "0"}};
    for (const auto &[k, v] : items) {
        QByteArray item = (k + "=" + v).toUtf8().left(255);
        txt.append(char(item.size())).append(item);
    }
    QByteArray srv;
    put16(srv, 0);
    put16(srv, 0);
    put16(srv, quint16(port));
    srv += encodeName(hostName);
    QByteArray out;
    out += record(SERVICE, TYPE_PTR, encodeName(instance), longTtl, false);
    out += record(instance, TYPE_SRV, srv, shortTtl, true);
    out += record(instance, TYPE_TXT, txt, longTtl, true);
    if (!ip.isNull()) {
        QByteArray a;
        put32(a, ip.toIPv4Address());
        out += record(hostName, TYPE_A, a, shortTtl, true);
    }
    return out;
}

void Discovery::send(const std::function<QByteArray(const QHostAddress &ip)> &packet) {
    const QList<Link> all = links();
    if (all.isEmpty()) {
        socket.writeDatagram(packet(QHostAddress(localIp())), GROUP, MDNS_PORT);
        return;
    }
    // Each network hears this computer at its own address there.
    for (const Link &l : all) {
        socket.setMulticastInterface(l.interface);
        socket.writeDatagram(packet(l.ip), GROUP, MDNS_PORT);
    }
}

void Discovery::announce(bool goodbye) {
    send([this, goodbye](const QHostAddress &ip) {
        QByteArray packet;
        put16(packet, 0);
        put16(packet, 0x8400);
        put16(packet, 0);
        put16(packet, quint16(ip.isNull() ? 3 : 4));
        put16(packet, 0);
        put16(packet, 0);
        return packet + records(ip, goodbye);
    });
}

void Discovery::sendQuery(const QList<QPair<QString, quint16>> &questions) {
    QByteArray packet;
    put16(packet, 0);
    put16(packet, 0);
    put16(packet, quint16(questions.size()));
    put16(packet, 0);
    put16(packet, 0);
    put16(packet, 0);
    for (const auto &[n, type] : questions) {
        packet += encodeName(n);
        put16(packet, type);
        put16(packet, 1);
    }
    send([packet](const QHostAddress &) { return packet; });
}

void Discovery::query() { sendQuery({{SERVICE, TYPE_PTR}}); }

void Discovery::read() {
    while (socket.hasPendingDatagrams()) handle(socket.receiveDatagram().data());
}

void Discovery::handle(const QByteArray &d) {
    if (d.size() < 12) return;
    quint16 flags = get16(d, 2), qd = get16(d, 4);
    int records = get16(d, 6) + get16(d, 8) + get16(d, 10);
    int offset = 12;
    bool asked = false;
    for (int i = 0; i < qd; i++) {
        QString q = readName(d, offset).toLower();
        if (q.isEmpty() || offset + 4 > d.size()) return;
        offset += 4;
        if (q == SERVICE.toLower() || q == instance.toLower() || q == hostName.toLower())
            asked = true;
    }
    if (asked && !(flags & 0x8000)) announce();

    QStringList touched;
    for (int i = 0; i < records; i++) {
        QString n = readName(d, offset).toLower();
        if (n.isEmpty() || offset + 10 > d.size()) return;
        quint16 type = get16(d, offset);
        quint32 ttl = (quint32(get16(d, offset + 4)) << 16) | get16(d, offset + 6);
        int length = get16(d, offset + 8);
        offset += 10;
        if (offset + length > d.size()) return;
        int data = offset;
        offset += length;
        if (ttl == 0) continue;  // a goodbye; a peer that left just stops answering
        if (type == TYPE_PTR && n == SERVICE.toLower()) {
            int at = data;
            QString target = readName(d, at).toLower();
            if (!target.isEmpty() && !services.contains(target)) services.insert(target, {});
            touched << target;
        } else if (type == TYPE_SRV && length >= 7 && n.endsWith(SERVICE.toLower())) {
            int at = data + 6;
            Service &s = services[n];
            s.port = get16(d, data + 4);
            s.target = readName(d, at).toLower();
            touched << n;
        } else if (type == TYPE_TXT && n.endsWith(SERVICE.toLower())) {
            Service &s = services[n];
            s.hasTxt = true;
            for (int at = data; at < data + length;) {
                int len = quint8(d[at]);
                QString item = QString::fromUtf8(d.mid(at + 1, len));
                at += 1 + len;
                int eq = item.indexOf('=');
                if (eq > 0) s.txt.insert(item.left(eq), item.mid(eq + 1));
            }
            touched << n;
        } else if (type == TYPE_A && length == 4) {
            addresses.insert(n, QHostAddress(quint32((quint32(get16(d, data)) << 16) |
                                                     get16(d, data + 2)))
                                    .toString());
            for (auto it = services.cbegin(); it != services.cend(); ++it)
                if (it->target == n) touched << it.key();
        }
    }

    QList<QPair<QString, quint16>> missing;
    touched.removeDuplicates();
    for (const QString &key : touched) {
        if (key == instance.toLower()) continue;
        const Service &s = services.value(key);
        if (s.txt.value("id") == id) continue;
        if (s.target.isEmpty() || !s.hasTxt) {
            missing << qMakePair(key, quint16(TYPE_SRV)) << qMakePair(key, quint16(TYPE_TXT));
            continue;
        }
        QString address = addresses.value(s.target);
        if (address.isEmpty()) {
            missing << qMakePair(s.target, quint16(TYPE_A));
            continue;
        }
        emit found(address, s.port, s.txt.value("tls") == "1", s.txt.value("id"),
                   s.txt.value("name"));
    }
    if (!missing.isEmpty() && missing.size() <= 16) sendQuery(missing);
}
