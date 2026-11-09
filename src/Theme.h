#pragma once

#include <QColor>
#include <QString>

class QDialogButtonBox;
class QMessageBox;
class QPalette;
class QWidget;

/**
 * The Android app's look: Material 3 in its default blue, light or dark after the system theme.
 */
namespace Theme {

struct Colors {
    bool dark;
    QColor page, card, primary, onPrimary, tonal, onTonal, text, textVariant, outline;
    QColor danger, onDanger;
    QColor online, dnd, offline;
};

Colors colors(const QPalette &palette);

/** `over` laid on `base` by `amount` (0 to 1). */
QColor mix(const QColor &base, const QColor &over, qreal amount);

/**
 * For widgets named "page" and what they hold: cards, labels, fields, and filled, tonal,
 * toggle, danger, text and outlined buttons (by object name).
 */
QString styleSheet(const Colors &c);

/** Names `widget` "page" and gives it the style sheet in the colors of its palette. */
void apply(QWidget *widget);

/** The dialog's OK as a filled button, the others as outlined ones, as on the home page. */
void style(QDialogButtonBox *buttons);
/** The same for a message box, which also loses its icon; then apply() it. */
void style(QMessageBox *box);

/**
 * A message in the app's style. With `accept`, it asks, and is true when the user chose it;
 * without, it only informs.
 */
bool message(QWidget *parent, const QString &title, const QString &text,
             const QString &accept = {}, bool selectable = false);

}  // namespace Theme
