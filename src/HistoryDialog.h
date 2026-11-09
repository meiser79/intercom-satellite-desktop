#pragma once

#include <QDialog>
#include <QPointer>

#include "Theme.h"

class Config;
class Engine;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QWidget;

/**
 * The calls and announcements, newest first, as the Android app's call history: who, when, which
 * way, how long and how each ended, with a button to call a kiosk again. Only that is stored,
 * never audio. It can be cleared.
 */
class HistoryDialog : public QDialog {
    Q_OBJECT
public:
    HistoryDialog(Config &config, Engine *engine, QWidget *parent = nullptr);

private:
    void render();
    void clear();

    Config &config;
    QPointer<Engine> engine;  // replaced when settings restart the intercom
    Theme::Colors colors;
    QScrollArea *scroll;
    QWidget *list, *empty;
    QVBoxLayout *rows;
    QPushButton *clearButton;
};
