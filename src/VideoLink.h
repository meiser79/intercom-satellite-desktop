#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QWebSocket>

/**
 * The video socket of a call, in both directions. A WebSocket buffers whatever it is given, so
 * on a slow link video would fall further and further behind. The far side acknowledges each
 * frame: past MAX_IN_FLIGHT unacknowledged frames this side drops, then waits for a key frame so
 * the decoder never sees a gap.
 */
class VideoLink : public QObject {
    Q_OBJECT
public:
    static constexpr int MAX_IN_FLIGHT = 6;

    explicit VideoLink(QWebSocket *socket, QObject *parent = nullptr);
    ~VideoLink() override;

    void send(const QJsonObject &message);
    /** Sends a frame unless the link has fallen behind; a key frame is due when it returns false. */
    bool sendFrame(const QByteArray &frame, bool key);
    void close();
    QWebSocket *socket() const { return ws; }

signals:
    void frame(const QByteArray &frame);
    void message(const QJsonObject &message);
    void closed();

private:
    QPointer<QWebSocket> ws;
    int sent = 0, acked = 0, received = 0;
    bool waitingKey = false;
};
