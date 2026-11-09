#include "Switch.h"

#include <QFocusEvent>
#include <QPainter>

Switch::Switch(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
}

void Switch::setColors(const QColor &on, const QColor &onThumb, const QColor &offTrack,
                       const QColor &outline) {
    this->on = on;
    this->onThumb = onThumb;
    this->offTrack = offTrack;
    this->outline = outline;
    update();
}

void Switch::focusInEvent(QFocusEvent *event) {
    // The ring only for keyboard focus, not on start or a click.
    keyboardFocus = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason;
    QAbstractButton::focusInEvent(event);
}

void Switch::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) p.setOpacity(0.38);
    QRectF track(1, 1, width() - 2, height() - 2);
    qreal radius = track.height() / 2;
    if (isChecked()) {
        p.setPen(Qt::NoPen);
        p.setBrush(on);
        p.drawRoundedRect(track, radius, radius);
        // The thumb grows when on, as on Android.
        p.setBrush(onThumb);
        p.drawEllipse(QPointF(track.right() - radius, track.center().y()), 12, 12);
    } else {
        p.setPen(QPen(outline, 2));
        p.setBrush(offTrack);
        p.drawRoundedRect(track.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
        p.setPen(Qt::NoPen);
        p.setBrush(outline);
        p.drawEllipse(QPointF(track.left() + radius, track.center().y()), 8, 8);
    }
    if (keyboardFocus && hasFocus()) {
        p.setOpacity(1);
        p.setPen(QPen(on, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), height() / 2.0, height() / 2.0);
    }
}
