#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * One video frame on the wire: a 9 byte header (flags, width, height, capture time in ms, all
 * big endian) followed by one H.264 access unit in Annex B form. Bit 0 of the flags marks a key
 * frame, which carries its SPS and PPS so any frame after a gap can start a decoder.
 */
namespace VideoWire {
constexpr int HEADER = 9;
bool isKey(const QByteArray &frame);
int width(const QByteArray &frame);
int height(const QByteArray &frame);
/** NAL units of Annex B data, without their start codes. */
QList<QByteArray> nals(const QByteArray &data);
QByteArray pack(bool key, int width, int height, quint32 timeMs, const QByteArray &sps,
                const QByteArray &pps, const QList<QByteArray> &units);
}  // namespace VideoWire

/** The camera's pictures as H.264 frames, at most 640x480 and with low delay. */
class VideoEncoder {
public:
    ~VideoEncoder();
    /** A packed frame, or an empty one while the encoder fills its pipeline or on failure. */
    QByteArray encode(const QImage &image, quint32 timeMs);
    /** The next frame is a key frame, for a decoder starting or recovering. */
    void forceKey() { keyRequested = true; }
    QString problem() const { return error; }

private:
    bool open(int width, int height);
    void close();

    AVCodecContext *ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *sws = nullptr;
    int w = 0, h = 0, srcW = 0, srcH = 0;
    qint64 pts = 0;
    bool keyRequested = true;
    QByteArray sps, pps;
    QString error;
};

/** The other side's frames as pictures. */
class VideoDecoder {
public:
    ~VideoDecoder();
    /** The decoded picture, or a null image while waiting for a key frame or on a bad frame. */
    QImage decode(const QByteArray &frame);
    void reset();

private:
    AVCodecContext *ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *sws = nullptr;
    bool started = false;
};
