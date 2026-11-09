#pragma once

#include <QAudioSink>
#include <QAudioSource>
#include <QByteArray>
#include <QObject>

#include <memory>

/**
 * The call's sound: the other side through the speakers, the microphone in 80 ms frames of
 * 16 kHz mono PCM16. Without a microphone a call still works, listening only.
 */
class Audio : public QObject {
    Q_OBJECT
public:
    explicit Audio(QObject *parent = nullptr);
    ~Audio() override;

    /** Starts playback, and the microphone when asked; false when nothing can play. */
    bool start(bool microphone);
    void stop();
    void setSending(bool on);
    void play(const QByteArray &frame);

    bool microphoneAvailable() const { return source != nullptr; }
    QString problem() const { return error; }
    /** How loud the other side or the microphone is, 0 to 1, for a level meter. */
    float level() const { return lastLevel; }

signals:
    void frame(const QByteArray &frame);

private:
    void captured();
    void measure(const QByteArray &data);

    std::unique_ptr<QAudioSink> sink;
    std::unique_ptr<QAudioSource> source;
    QIODevice *out = nullptr, *in = nullptr;
    QByteArray pending;
    bool sending = false;
    float lastLevel = 0;
    QString error;
};
