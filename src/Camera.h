#pragma once

#include <QCamera>
#include <QElapsedTimer>
#include <QImage>
#include <QMediaCaptureSession>
#include <QObject>
#include <QTimer>
#include <QVideoSink>

#include <memory>

/**
 * This computer's picture for video calls, about 15 times a second: a camera, or a test
 * pattern where there is none.
 */
class Camera : public QObject {
    Q_OBJECT
public:
    explicit Camera(QObject *parent = nullptr);
    /** {id, description} of each camera; the id "test" is the test pattern. */
    static QList<QPair<QString, QString>> devices();
    static bool available(const QString &device);

    void start(const QString &device);
    void stop();
    bool running() const { return active; }

signals:
    void picture(const QImage &image);
    void failed(const QString &problem);

private:
    void frame(const QVideoFrame &frame);
    void testFrame();

    std::unique_ptr<QCamera> camera;
    QMediaCaptureSession session;
    QVideoSink sink;
    QTimer pattern;
    QElapsedTimer since;
    qint64 last = -1000;
    int tick = 0;
    bool active = false;
};
