#include "VideoCodec.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {
constexpr int MAX_W = 640, MAX_H = 480, FPS = 15;
const QByteArray START("\0\0\0\1", 4);
}  // namespace

// ---------------------------------------------------------------- wire format

bool VideoWire::isKey(const QByteArray &f) { return !f.isEmpty() && (f[0] & 1); }

int VideoWire::width(const QByteArray &f) {
    return f.size() < HEADER ? 0 : (quint8(f[1]) << 8) | quint8(f[2]);
}

int VideoWire::height(const QByteArray &f) {
    return f.size() < HEADER ? 0 : (quint8(f[3]) << 8) | quint8(f[4]);
}

QList<QByteArray> VideoWire::nals(const QByteArray &data) {
    QList<QByteArray> units;
    int start = -1, i = 0, end = data.size();
    while (i + 2 < end) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (start >= 0) {
                int to = i;
                while (to > start && data[to - 1] == 0) to--;  // a 4 byte start code's zero
                units << data.mid(start, to - start);
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start >= 0 && start < end) units << data.mid(start);
    else if (start < 0 && end > 0) units << data;
    return units;
}

QByteArray VideoWire::pack(bool key, int width, int height, quint32 timeMs, const QByteArray &sps,
                           const QByteArray &pps, const QList<QByteArray> &units) {
    QByteArray out;
    out.append(char(key ? 1 : 0));
    out.append(char(width >> 8)).append(char(width));
    out.append(char(height >> 8)).append(char(height));
    for (int shift = 24; shift >= 0; shift -= 8) out.append(char(timeMs >> shift));
    if (key) out += START + sps + START + pps;
    for (const QByteArray &u : units) {
        int type = u.isEmpty() ? -1 : u[0] & 0x1f;
        if (type < 0 || type == 7 || type == 8) continue;
        out += START + u;
    }
    return out;
}

// ---------------------------------------------------------------- encoder

VideoEncoder::~VideoEncoder() { close(); }

void VideoEncoder::close() {
    if (ctx) avcodec_free_context(&ctx);
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (sws) sws_freeContext(sws);
    sws = nullptr;
    w = h = srcW = srcH = 0;
}

bool VideoEncoder::open(int width, int height) {
    close();
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        error = "No H.264 encoder in FFmpeg";
        return false;
    }
    ctx = avcodec_alloc_context3(codec);
    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = {1, FPS};
    ctx->framerate = {FPS, 1};
    ctx->gop_size = FPS * 2;
    ctx->max_b_frames = 0;
    ctx->bit_rate = 800'000;
    ctx->thread_count = 1;
    ctx->profile = AV_PROFILE_H264_BASELINE;
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(ctx->priv_data, "profile", "baseline", 0);
    av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        error = "The H.264 encoder did not start";
        avcodec_free_context(&ctx);
        return false;
    }
    frame = av_frame_alloc();
    frame->format = ctx->pix_fmt;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);
    packet = av_packet_alloc();
    w = width;
    h = height;
    keyRequested = true;
    return true;
}

QByteArray VideoEncoder::encode(const QImage &image, quint32 timeMs) {
    if (image.isNull()) return {};
    QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    // Fits the picture into 640x480 with its shape, in even sizes as YUV 4:2:0 needs.
    QSize size = rgb.size().scaled(MAX_W, MAX_H, Qt::KeepAspectRatio);
    if (size.width() > rgb.width()) size = rgb.size();
    int width = std::max(2, size.width() & ~1), height = std::max(2, size.height() & ~1);
    if (width != w || height != h || !ctx) {
        if (!open(width, height)) return {};
    }
    if (rgb.width() != srcW || rgb.height() != srcH || !sws) {
        if (sws) sws_freeContext(sws);
        srcW = rgb.width();
        srcH = rgb.height();
        sws = sws_getContext(srcW, srcH, AV_PIX_FMT_BGRA, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                             nullptr, nullptr, nullptr);
    }
    if (av_frame_make_writable(frame) < 0) return {};
    const uint8_t *src[] = {rgb.constBits()};
    const int stride[] = {int(rgb.bytesPerLine())};
    sws_scale(sws, src, stride, 0, srcH, frame->data, frame->linesize);
    frame->pts = pts++;
    frame->pict_type = keyRequested ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    keyRequested = false;
    if (avcodec_send_frame(ctx, frame) < 0) return {};
    QByteArray result;
    while (avcodec_receive_packet(ctx, packet) == 0) {
        QList<QByteArray> units =
            VideoWire::nals(QByteArray(reinterpret_cast<const char *>(packet->data), packet->size));
        bool key = packet->flags & AV_PKT_FLAG_KEY;
        for (const QByteArray &u : units) {
            int type = u.isEmpty() ? -1 : u[0] & 0x1f;
            if (type == 7) sps = u;
            else if (type == 8) pps = u;
        }
        av_packet_unref(packet);
        if (sps.isEmpty() || pps.isEmpty()) continue;
        result = VideoWire::pack(key, w, h, timeMs, sps, pps, units);
    }
    return result;
}

// ---------------------------------------------------------------- decoder

VideoDecoder::~VideoDecoder() {
    reset();
    if (packet) av_packet_free(&packet);
}

void VideoDecoder::reset() {
    if (ctx) avcodec_free_context(&ctx);
    if (frame) av_frame_free(&frame);
    if (sws) sws_freeContext(sws);
    sws = nullptr;
    started = false;
}

QImage VideoDecoder::decode(const QByteArray &wireFrame) {
    if (wireFrame.size() <= VideoWire::HEADER) return {};
    if (!started && !VideoWire::isKey(wireFrame)) return {};
    if (!ctx) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) return {};
        ctx = avcodec_alloc_context3(codec);
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->thread_count = 1;
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return {};
        }
        frame = av_frame_alloc();
    }
    if (!packet) packet = av_packet_alloc();
    started = true;
    QByteArray payload = wireFrame.mid(VideoWire::HEADER);
    if (av_new_packet(packet, payload.size()) < 0) return {};
    memcpy(packet->data, payload.constData(), payload.size());
    int sent = avcodec_send_packet(ctx, packet);
    av_packet_unref(packet);
    if (sent < 0) {
        // A broken stream: start over at the next key frame.
        reset();
        return {};
    }
    QImage picture;
    while (avcodec_receive_frame(ctx, frame) == 0) {
        sws = sws_getCachedContext(sws, frame->width, frame->height, AVPixelFormat(frame->format),
                                   frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr);
        if (!sws) continue;
        picture = QImage(frame->width, frame->height, QImage::Format_RGB32);
        uint8_t *dst[] = {picture.bits()};
        const int stride[] = {int(picture.bytesPerLine())};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, stride);
    }
    return picture;
}
