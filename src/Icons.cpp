#include "Icons.h"

#include <QAbstractButton>
#include <QApplication>
#include <QPainter>
#include <QPixmap>

namespace {
QPixmap paint(const QPixmap &shape, const QColor &color) {
    QPixmap out(shape.size());
    out.setDevicePixelRatio(shape.devicePixelRatio());
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.drawPixmap(0, 0, shape);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRectF(QPointF(), out.deviceIndependentSize()), color);
    return out;
}
}  // namespace

namespace Icons {

QIcon tinted(const QString &name, const QColor &color, const QColor &disabled) {
    QIcon shape(":/icons/" + name + ".svg");
    QIcon icon;
    qreal ratio = qApp->devicePixelRatio();
    for (int size : {16, 20, 24, 32, 48}) {
        QPixmap pixmap = shape.pixmap(QSize(size, size), ratio);
        icon.addPixmap(paint(pixmap, color), QIcon::Normal);
        icon.addPixmap(paint(pixmap, disabled), QIcon::Disabled);
    }
    return icon;
}

void set(QAbstractButton *button, const QString &name, const QColor &color) {
    button->setProperty("iconName", name);
    button->setProperty("iconColor", color);
    const QPalette &palette = button->palette();
    QColor normal = color.isValid() ? color : palette.color(QPalette::Active, QPalette::ButtonText);
    QColor disabled = color.isValid() ? QColor(color.red(), color.green(), color.blue(), 110)
                                      : palette.color(QPalette::Disabled, QPalette::ButtonText);
    button->setIcon(tinted(name, normal, disabled));
}

void refresh(QWidget *root) {
    for (QAbstractButton *b : root->findChildren<QAbstractButton *>()) {
        QString name = b->property("iconName").toString();
        if (!name.isEmpty()) set(b, name, b->property("iconColor").value<QColor>());
    }
}

}  // namespace Icons
