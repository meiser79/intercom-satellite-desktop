#include "Ringer.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QUrl>
#include <QtEndian>

#include <cmath>

namespace {
/** One ring cycle as a WAV file in the cache, written once. */
QString ringFile() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    QString path = dir + "/ring.wav";
    if (QFile::exists(path)) return path;
    constexpr int rate = 22050, seconds = 6;
    QByteArray samples;
    for (int i = 0; i < rate * seconds; i++) {
        double t = double(i) / rate;
        double v = 0;
        if (t < 2.0) {
            double fade = std::min({1.0, t * 50, (2.0 - t) * 50});
            v = fade * 0.3 * (std::sin(2 * M_PI * 440 * t) + std::sin(2 * M_PI * 480 * t));
        }
        qint16 s = qToLittleEndian(qint16(v * 32767 / 2));
        samples.append(reinterpret_cast<const char *>(&s), 2);
    }
    auto le32 = [](quint32 v) {
        v = qToLittleEndian(v);
        return QByteArray(reinterpret_cast<const char *>(&v), 4);
    };
    auto le16 = [](quint16 v) {
        v = qToLittleEndian(v);
        return QByteArray(reinterpret_cast<const char *>(&v), 2);
    };
    QByteArray wav = "RIFF" + le32(36 + samples.size()) + "WAVEfmt " + le32(16) + le16(1) +
                     le16(1) + le32(rate) + le32(rate * 2) + le16(2) + le16(16) + "data" +
                     le32(samples.size()) + samples;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(wav);
    return path;
}
}  // namespace

Ringer::Ringer(QObject *parent) : QObject(parent) {
    effect.setSource(QUrl::fromLocalFile(ringFile()));
    effect.setLoopCount(QSoundEffect::Infinite);
    effect.setVolume(0.8f);
}

void Ringer::start() {
    if (!effect.isPlaying()) effect.play();
}

void Ringer::stop() { effect.stop(); }
