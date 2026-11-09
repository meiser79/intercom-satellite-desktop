#include "Camera.h"

#include <QCameraDevice>
#include <QDateTime>
#include <QMediaDevices>
#include <QPainter>
#include <QVideoFrame>

Camera::Camera(QObject *parent) : QObject(parent) {
    session.setVideoSink(&sink);
    connect(&sink, &QVideoSink::videoFrameChanged, this, &Camera::frame);
    connect(&pattern, &QTimer::timeout, this, &Camera::testFrame);
}

QList<QPair<QString, QString>> Camera::devices() {
    QList<QPair<QString, QString>> list;
    for (const QCameraDevice &d : QMediaDevices::videoInputs())
        list << qMakePair(QString::fromUtf8(d.id()), d.description());
    list << qMakePair(QStringLiteral("test"), tr("Test pattern"));
    return list;
}

bool Camera::available(const QString &device) {
    return device == "test" || !QMediaDevices::videoInputs().isEmpty();
}

void Camera::start(const QString &device) {
    stop();
    active = true;
    since.start();
    last = -1000;
    QCameraDevice chosen = QMediaDevices::defaultVideoInput();
    for (const QCameraDevice &d : QMediaDevices::videoInputs())
        if (QString::fromUtf8(d.id()) == device) chosen = d;
    if (device == "test" || chosen.isNull()) {
        pattern.start(1000 / 15);
        return;
    }
    camera = std::make_unique<QCamera>(chosen);
    connect(camera.get(), &QCamera::errorOccurred, this, [this](QCamera::Error, const QString &e) {
        // A camera in use elsewhere, or one that won't stream: the call gets the test pattern.
        camera.reset();
        if (active) pattern.start(1000 / 15);
        emit failed(e);
    });
    session.setCamera(camera.get());
    camera->start();
}

void Camera::stop() {
    active = false;
    pattern.stop();
    if (camera) camera->stop();
    session.setCamera(nullptr);
    camera.reset();
}

void Camera::frame(const QVideoFrame &f) {
    if (!active || !f.isValid()) return;
    qint64 now = since.elapsed();
    if (now - last < 1000 / 15 - 5) return;  // 15 pictures a second are plenty
    last = now;
    QImage image = f.toImage();
    if (!image.isNull()) emit picture(image);
}

void Camera::testFrame() {
    QImage image(640, 480, QImage::Format_RGB32);
    QPainter p(&image);
    static const QColor bars[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green,
                                  Qt::magenta, Qt::red, Qt::blue, Qt::black};
    for (int i = 0; i < 8; i++) p.fillRect(i * 80, 0, 80, 480, bars[i]);
    int x = (tick++ * 8) % 640;
    p.fillRect(x, 200, 40, 80, Qt::darkGray);
    p.fillRect(0, 400, 640, 80, QColor(0, 0, 0, 180));
    p.setPen(Qt::white);
    QFont font = p.font();
    font.setPixelSize(36);
    p.setFont(font);
    p.drawText(QRect(0, 400, 640, 80), Qt::AlignCenter,
               QDateTime::currentDateTime().toString("HH:mm:ss.zzz").left(10));
    p.end();
    emit picture(image);
}
