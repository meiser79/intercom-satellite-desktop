#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QAbstractButton;
class QWidget;

/**
 * The Android app's Material icons, in the color of the text beside them. Icon themes draw some
 * names, like audio-volume-high, for the panel: light on dark, or not at all.
 */
namespace Icons {

/** The icon `name` (resources/icons/<name>.svg) in `color`, dimmed in `disabled`. */
QIcon tinted(const QString &name, const QColor &color, const QColor &disabled);

/** Gives `button` the icon in its text color, or in `color` on a colored button. */
void set(QAbstractButton *button, const QString &name, const QColor &color = {});

/** Colors the icons under `root` anew, after the palette changed. */
void refresh(QWidget *root);

}  // namespace Icons
