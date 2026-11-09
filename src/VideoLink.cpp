#include "VideoLink.h"

#include <QJsonDocument>

VideoLink::VideoLink(QWebSocket *socket, QObject *parent) : QObject(parent), ws(socket) {
    connect(ws, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray &data) {
        send({{"type", "ack"}, {"n", ++received}});
        emit frame(data);
    });
    connect(ws, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
        QJsonObject m = QJsonDocument::fromJson(text.toUtf8()).object();
        if (m.value("type").toString() == "ack") {
            int n = m.value("n").toInt(-1);
            if (n > acked && n <= sent) acked = n;
            return;
        }
        if (!m.isEmpty()) emit message(m);
    });
    connect(ws, &QWebSocket::disconnected, this, &VideoLink::closed);
}

VideoLink::~VideoLink() {
    if (ws) {
        ws->disconnect(this);
        ws->close();
        ws->deleteLater();
    }
}

void VideoLink::send(const QJsonObject &message) {
    if (ws && ws->state() == QAbstractSocket::ConnectedState)
        ws->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

bool VideoLink::sendFrame(const QByteArray &frame, bool key) {
    if (!ws || ws->state() != QAbstractSocket::ConnectedState) return true;
    if (sent - acked >= MAX_IN_FLIGHT || (waitingKey && !key)) {
        waitingKey = true;
        return false;
    }
    waitingKey = false;
    sent++;
    ws->sendBinaryMessage(frame);
    return true;
}

void VideoLink::close() {
    if (ws) ws->close();
}
