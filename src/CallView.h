#pragma once

#include <QImage>
#include <QWidget>

#include "Theme.h"

class Engine;
class LevelDots;
class QFrame;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

/**
 * A call or announcement: who, its state and time, the pictures, and its controls, laid out like
 * the Android app's. Space held down talks, like the talk button.
 */
class CallView : public QWidget {
    Q_OBJECT
public:
    explicit CallView(QWidget *parent = nullptr);
    void setEngine(Engine *engine);
    /** The Android app's colors, light or dark. */
    void setColors(const Theme::Colors &colors);
    /** The icon shown for the other side while there's no picture, like room_kitchen. */
    void setSymbol(const QString &icon);
    void render();

signals:
    /** The caller's certificate is to be confirmed before answering. */
    void trustRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void tick();
    void showFar(const QImage &image);
    void showSelf(const QImage &image);
    void placeSelf();
    void paintAvatar();
    void tintIcons();

    Engine *engine = nullptr;
    Theme::Colors colors;
    QString symbol = "room_home";
    QVBoxLayout *layout;
    QFrame *card;
    QLabel *overline, *avatar, *name, *status, *timer, *hint, *far, *self;
    QWidget *video;
    LevelDots *meter;
    QPushButton *trust, *answer, *answerVideo, *decline, *talk, *pushToTalk, *camera, *end;
    QTimer *ticker;
    QImage farImage;
};
