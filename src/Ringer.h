#pragma once

#include <QObject>
#include <QSoundEffect>

/** Rings for an incoming call: a two-tone ring, two seconds on and four off. */
class Ringer : public QObject {
    Q_OBJECT
public:
    explicit Ringer(QObject *parent = nullptr);
    void start();
    void stop();

private:
    QSoundEffect effect;
};
