#include "Audio.h"

#include <QAudioDevice>
#include <QMediaDevices>

#include <cmath>
#include <cstdlib>

#include "Wire.h"

namespace {
QAudioFormat intercomFormat() {
    QAudioFormat f;
    f.setSampleRate(16000);
    f.setChannelCount(1);
    f.setSampleFormat(QAudioFormat::Int16);
    return f;
}
}  // namespace

Audio::Audio(QObject *parent) : QObject(parent) {}

Audio::~Audio() { stop(); }

bool Audio::start(bool microphone) {
    stop();
    error.clear();
    QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull()) {
        error = tr("No audio output");
        return false;
    }
    sink = std::make_unique<QAudioSink>(output, intercomFormat());
    // Half a second of room: frames arrive in bursts over Wi-Fi.
    sink->setBufferSize(16000);
    out = sink->start();
    if (!out) {
        error = tr("Audio output unavailable");
        sink.reset();
        return false;
    }
    if (microphone) {
        QAudioDevice input = QMediaDevices::defaultAudioInput();
        if (input.isNull()) {
            error = tr("No microphone");
        } else {
            source = std::make_unique<QAudioSource>(input, intercomFormat());
            source->setBufferSize(Wire::FRAME_BYTES * 4);
            in = source->start();
            if (!in) {
                error = tr("Microphone unavailable");
                source.reset();
            } else {
                connect(in, &QIODevice::readyRead, this, &Audio::captured);
            }
        }
    }
    return true;
}

void Audio::stop() {
    if (source) source->stop();
    if (sink) sink->stop();
    source.reset();
    sink.reset();
    in = out = nullptr;
    pending.clear();
    sending = false;
    lastLevel = 0;
}

void Audio::setSending(bool on) {
    sending = on && source;
    pending.clear();
}

void Audio::play(const QByteArray &frame) {
    if (!out || !sink) return;
    // Past a full buffer the call would fall behind; the frame is dropped instead.
    if (sink->bytesFree() >= frame.size()) out->write(frame);
    if (!sending) measure(frame);
}

void Audio::captured() {
    if (!in) return;
    QByteArray data = in->readAll();
    if (!sending) return;
    pending += data;
    while (pending.size() >= Wire::FRAME_BYTES) {
        QByteArray f = pending.left(Wire::FRAME_BYTES);
        pending.remove(0, Wire::FRAME_BYTES);
        measure(f);
        emit frame(f);
    }
}

void Audio::measure(const QByteArray &data) {
    const auto *s = reinterpret_cast<const qint16 *>(data.constData());
    int n = data.size() / 2, peak = 0;
    for (int i = 0; i < n; i++) peak = std::max(peak, std::abs(int(s[i])));
    lastLevel = std::min(1.0f, float(peak) / 20000.0f);
}
