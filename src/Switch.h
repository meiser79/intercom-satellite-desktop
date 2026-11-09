#pragma once

#include <QAbstractButton>
#include <QColor>

/** An on/off switch drawn like the Android app's (Material 3). */
class Switch : public QAbstractButton {
    Q_OBJECT
public:
    explicit Switch(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {52, 32}; }
    /** Track and thumb when on; track, outline and thumb when off. */
    void setColors(const QColor &on, const QColor &onThumb, const QColor &offTrack,
                   const QColor &outline);

protected:
    void focusInEvent(QFocusEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QColor on, onThumb, offTrack, outline;
    bool keyboardFocus = false;
};
