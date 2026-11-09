#include "Theme.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QWidget>

namespace {
QString css(const QColor &c) { return c.name(QColor::HexArgb); }

QColor alpha(QColor c, qreal a) {
    c.setAlphaF(a);
    return c;
}
}  // namespace

namespace Theme {

Colors colors(const QPalette &palette) {
    // The Android app's colors where the system gives it no wallpaper colors.
    if (palette.color(QPalette::Window).lightness() < 128)
        return {true,
                QColor("#121318"), QColor("#33343a"), QColor("#b7caff"), QColor("#1e325b"),
                QColor("#8a90a5"), QColor("#121318"), QColor("#e3e2e9"), QColor("#c5c6d0"),
                QColor("#8f9099"), QColor("#ffb4ab"), QColor("#690005"),
                QColor("#81c995"), QColor("#ffb4ab"), QColor("#8f9099")};
    return {false,
            QColor("#faf8ff"), QColor("#e2e2e9"), QColor("#2a4174"), QColor("#ffffff"),
            QColor("#6e7488"), QColor("#ffffff"), QColor("#1a1b20"), QColor("#44464f"),
            QColor("#75777f"), QColor("#8c0009"), QColor("#ffffff"),
            QColor("#2e7d32"), QColor("#ba1a1a"), QColor("#75777f")};
}

QColor mix(const QColor &base, const QColor &over, qreal amount) {
    return QColor::fromRgbF(base.redF() + (over.redF() - base.redF()) * amount,
                            base.greenF() + (over.greenF() - base.greenF()) * amount,
                            base.blueF() + (over.blueF() - base.blueF()) * amount);
}

QString styleSheet(const Colors &c) {
    // Material's state layers: 8 % of the content color on hover, 12 % when pressed.
    QString s = QStringLiteral(R"(
#page { background: %page; }
#page QScrollArea, #page QScrollArea > QWidget > QWidget { background: transparent; }
#page QLabel { color: %text; }
#page QLabel#variant { color: %variant; }
#page QLabel#statusTitle, #page QLabel#overline { color: %primary; }
#page QLabel#qt_msgbox_label { min-width: 320px; }
#page QFrame#card { background: %card; border-radius: 16px; }
#page QPushButton { border: none; border-radius: 20px; padding: 9px 20px; min-height: 22px; }
#page QPushButton#primary { background: %primary; color: %onPrimary; }
#page QPushButton#primary:hover { background: %primaryHover; }
#page QPushButton#primary:pressed { background: %primaryPressed; }
#page QPushButton#talk { background: %primary; color: %onPrimary; border-radius: 36px; min-height: 54px; }
#page QPushButton#talk:hover { background: %primaryHover; }
#page QPushButton#talk:pressed { background: %primaryPressed; }
#page QPushButton#tonal, #page QPushButton#toggle:checked { background: %tonal; color: %onTonal; }
#page QPushButton#tonal:hover, #page QPushButton#toggle:checked:hover { background: %tonalHover; }
#page QPushButton#tonal:pressed, #page QPushButton#toggle:checked:pressed { background: %tonalPressed; }
#page QPushButton#toggle { background: %card; color: %text; }
#page QPushButton#toggle:hover { background: %cardHover; }
#page QPushButton#toggle:pressed { background: %cardPressed; }
#page QPushButton#danger { background: %danger; color: %onDanger; }
#page QPushButton#danger:hover { background: %dangerHover; }
#page QPushButton#danger:pressed { background: %dangerPressed; }
#page QPushButton#outlined { background: transparent; color: %primary; border: 1px solid %outline; }
#page QPushButton#outlined:hover, #page QPushButton#text:hover { background: %primaryLayer; }
#page QPushButton#text { background: transparent; color: %primary; padding: 9px 16px; }
#page QPushButton:disabled { background: %disabledFill; color: %disabledText; }
#page QPushButton#outlined:disabled { background: transparent; border-color: %disabledFill; }
#page QPushButton#text:disabled { background: transparent; }
#page QToolButton { border: none; border-radius: 18px; padding: 6px; background: transparent; }
#page QToolButton:hover { background: %textLayer; }
#page QToolButton::menu-indicator { image: none; width: 0; }
#page QToolButton#tonal { background: %tonal; border-radius: 20px; padding: 8px; }
#page QToolButton#tonal:hover { background: %tonalHover; }
#page QToolButton#tonal:pressed { background: %tonalPressed; }
#page QToolButton#tonal:disabled { background: %disabledFill; }
#page QToolButton#symbol { border: 1px solid %outline; border-radius: 22px; padding: 0; }
#page QToolButton#symbol:checked { background: %tonal; border-color: %tonal; }
#page QLineEdit, #page QSpinBox, #page QComboBox {
    background: transparent; color: %text; border: 1px solid %outline; border-radius: 6px;
    padding: 8px 10px; selection-background-color: %primary; selection-color: %onPrimary; }
#page QLineEdit:focus, #page QSpinBox:focus, #page QComboBox:focus {
    border: 2px solid %primary; padding: 7px 9px; }
#page QLineEdit:disabled, #page QSpinBox:disabled, #page QComboBox:disabled {
    color: %disabledText; border-color: %disabledFill; }
#page QSpinBox::up-button, #page QSpinBox::down-button { width: 0; border: none; }
#page QComboBox::drop-down { border: none; width: 28px; }
#page QComboBox QAbstractItemView, #page QMenu {
    background: %card; color: %text; border: 1px solid %disabledFill; padding: 4px 0;
    selection-background-color: %textLayer; selection-color: %text; outline: none; }
#page QMenu::item { padding: 8px 24px 8px 16px; }
#page QMenu::item:selected { background: %textLayer; }
#page QMenu::item:disabled { color: %disabledText; }
#page QMenu::separator { height: 1px; background: %disabledFill; margin: 4px 0; }
#page QComboBox::down-arrow { image: url(:/icons/arrow_drop_down.svg); width: 24px; height: 24px; }
#page QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
#page QScrollBar::handle:vertical { background: %outline; border-radius: 4px; min-height: 32px; }
#page QScrollBar::add-line, #page QScrollBar::sub-line { height: 0; }
#page QScrollBar::add-page, #page QScrollBar::sub-page { background: transparent; }
)");
    // Longer names first: %textLayer before %text, %cardHover before %card.
    return s.replace("%page", css(c.page))
        .replace("%textLayer", css(alpha(c.text, 0.08)))
        .replace("%text", css(c.text))
        .replace("%variant", css(c.textVariant))
        .replace("%primaryHover", css(mix(c.primary, c.onPrimary, 0.08)))
        .replace("%primaryPressed", css(mix(c.primary, c.onPrimary, 0.12)))
        .replace("%primaryLayer", css(alpha(c.primary, 0.08)))
        .replace("%primary", css(c.primary))
        .replace("%onPrimary", css(c.onPrimary))
        .replace("%cardHover", css(mix(c.card, c.text, 0.08)))
        .replace("%cardPressed", css(mix(c.card, c.text, 0.12)))
        .replace("%card", css(c.card))
        .replace("%tonalHover", css(mix(c.tonal, c.onTonal, 0.08)))
        .replace("%tonalPressed", css(mix(c.tonal, c.onTonal, 0.12)))
        .replace("%tonal", css(c.tonal))
        .replace("%onTonal", css(c.onTonal))
        .replace("%dangerHover", css(mix(c.danger, c.onDanger, 0.08)))
        .replace("%dangerPressed", css(mix(c.danger, c.onDanger, 0.12)))
        .replace("%danger", css(c.danger))
        .replace("%onDanger", css(c.onDanger))
        .replace("%outline", css(c.outline))
        .replace("%disabledFill", css(alpha(c.text, 0.12)))
        .replace("%disabledText", css(alpha(c.text, 0.38)));
}

void apply(QWidget *widget) {
    widget->setObjectName("page");
    widget->setAttribute(Qt::WA_StyledBackground);
    widget->setStyleSheet(styleSheet(colors(widget->palette())));
}

void style(QDialogButtonBox *buttons) {
    // As on the home page: filled and outlined, wide enough not to look like round badges. Text
    // only, as in the Android app: the icons some desktop styles add vanish on the filled color.
    for (QAbstractButton *b : buttons->buttons()) {
        b->setIcon(QIcon());
        b->setObjectName(buttons->buttonRole(b) == QDialogButtonBox::AcceptRole ? "primary"
                                                                               : "outlined");
        b->setMinimumWidth(120);
    }
}

void style(QMessageBox *box) {
    box->setIcon(QMessageBox::NoIcon);
    // The margins of the other dialogs; the text sets the width, as QMessageBox ignores its own.
    if (auto *layout = qobject_cast<QGridLayout *>(box->layout())) {
        layout->setContentsMargins(24, 22, 24, 18);
        layout->setVerticalSpacing(18);
        layout->setHorizontalSpacing(0);  // no gap for the icon that isn't there
    }
    for (QAbstractButton *b : box->buttons()) {
        QMessageBox::ButtonRole role = box->buttonRole(b);
        b->setObjectName(role == QMessageBox::AcceptRole || role == QMessageBox::YesRole
                             ? "primary"
                             : "outlined");
        b->setIcon(QIcon());
        b->setMinimumWidth(120);
    }
}

bool message(QWidget *parent, const QString &title, const QString &text, const QString &accept,
             bool selectable) {
    QMessageBox box(QMessageBox::NoIcon, title, text, QMessageBox::NoButton, parent);
    QAbstractButton *chosen = nullptr;
    if (accept.isEmpty()) {
        box.addButton(QMessageBox::Ok);
    } else {
        chosen = box.addButton(accept, QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
    }
    if (selectable) box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    style(&box);
    apply(&box);
    box.exec();
    return chosen && box.clickedButton() == chosen;
}

}  // namespace Theme
