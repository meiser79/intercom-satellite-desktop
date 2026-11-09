#include "SettingsDialog.h"

#include <QAction>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Camera.h"
#include "Config.h"
#include "Icons.h"
#include "Switch.h"
#include "Theme.h"

namespace {
/** A section: its title above a card, as in the Android app's settings. */
QVBoxLayout *section(QVBoxLayout *page, const QString &title) {
    auto *label = new QLabel(title);
    label->setObjectName("overline");
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    label->setContentsMargins(4, 6, 0, 0);
    page->addWidget(label);
    auto *card = new QFrame;
    card->setObjectName("card");
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(6);
    page->addWidget(card);
    return layout;
}

QLabel *hint(const QString &text) {
    auto *label = new QLabel(text);
    label->setObjectName("variant");
    label->setWordWrap(true);
    return label;
}

/** A field with its name above it. */
void field(QVBoxLayout *card, const QString &name, QWidget *widget) {
    if (card->count()) card->addSpacing(6);
    card->addWidget(new QLabel(name));
    card->addWidget(widget);
}

/** A switch at the right of its name, and of a hint below the name when given. */
Switch *toggle(QVBoxLayout *card, const QString &name, bool on, const QString &note = {}) {
    if (card->count()) card->addSpacing(6);
    auto *text = new QVBoxLayout;
    text->setSpacing(2);
    auto *label = new QLabel(name);
    label->setWordWrap(true);
    text->addWidget(label);
    if (!note.isEmpty()) text->addWidget(hint(note));
    auto *s = new Switch;
    s->setAccessibleName(name);
    s->setChecked(on);
    auto *row = new QHBoxLayout;
    row->setSpacing(16);
    row->addLayout(text, 1);
    row->addWidget(s, 0, Qt::AlignVCenter);
    card->addLayout(row);
    return s;
}

/** A fact about the app: its icon and name at the left, its value at the right. */
void info(QVBoxLayout *card, const QString &icon, const QString &name, QWidget *value,
          const QColor &color) {
    if (card->count()) card->addSpacing(4);
    auto *mark = new QLabel;
    mark->setPixmap(
        Icons::tinted(icon, color, color).pixmap(QSize(20, 20), mark->devicePixelRatioF()));
    auto *row = new QHBoxLayout;
    row->setSpacing(12);
    row->addWidget(mark);
    row->addWidget(new QLabel(name), 1);
    row->addWidget(value);
    card->addLayout(row);
}
}  // namespace

SettingsDialog::SettingsDialog(Config &config, QWidget *parent)
    : QDialog(parent), config(config) {
    setWindowTitle(tr("Settings"));
    auto *page = new QVBoxLayout(this);
    page->setContentsMargins(16, 12, 16, 16);
    page->setSpacing(8);

    QVBoxLayout *connection = section(page, tr("Connection"));
    name = new QLineEdit(config.name());
    name->setMaxLength(48);
    field(connection, tr("Name on the network"), name);

    key = new QLineEdit(config.key());
    key->setEchoMode(QLineEdit::Password);
    QAction *show = key->addAction(
        Icons::tinted("visibility", key->palette().color(QPalette::Active, QPalette::Text),
                      key->palette().color(QPalette::Disabled, QPalette::Text)),
        QLineEdit::TrailingPosition);
    show->setCheckable(true);
    show->setToolTip(tr("Show the key"));
    connect(show, &QAction::toggled, this, [this](bool on) {
        key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    field(connection, tr("Intercom key"), key);
    connection->addWidget(hint(tr("The same key as on the kiosks (Kiosk Satellite → Intercom).")));

    port = new QSpinBox;
    port->setRange(1024, 65534);  // the encrypted endpoint takes the next one
    port->setValue(config.port());
    field(connection, tr("Port"), port);

    tls = toggle(connection, tr("TLS encryption"), config.tls(),
                 tr("Must match “Encrypt communications” on the kiosk. Calls then come in on the "
                    "port after the one above."));

    QVBoxLayout *calls = section(page, tr("Calls"));
    announcements = toggle(calls, tr("Receive announcements"), config.acceptAnnouncements());
    pushToTalk = toggle(calls, tr("Push to talk"), config.pushToTalk(),
                        tr("Off: the microphone opens when a call connects."));
    video = toggle(calls, tr("Video calls"), config.video());

    camera = new QComboBox;
    camera->addItem(tr("Default camera"), QString());
    for (const auto &[id, description] : Camera::devices()) camera->addItem(description, id);
    camera->setCurrentIndex(std::max(0, camera->findData(config.camera())));
    camera->setEnabled(video->isChecked());
    connect(video, &Switch::toggled, camera, &QWidget::setEnabled);
    field(calls, tr("Camera"), camera);

    QVBoxLayout *desktop = section(page, tr("Desktop"));
    startHidden = toggle(desktop, tr("Start in the system tray"), config.startHidden());

    Theme::Colors colors = Theme::colors(palette());
    QVBoxLayout *about = section(page, tr("Info"));
    info(about, "info", tr("Version"), hint(QStringLiteral(APP_VERSION)), colors.textVariant);
    info(about, "person", tr("Maintainer"), hint(QStringLiteral("Peter Meiser")),
         colors.textVariant);
    // The source and issues, opened in the browser.
    const QString repo = QStringLiteral("github.com/meiser79/intercom-satellite-desktop");
    auto *link = new QLabel(QStringLiteral("<a href=\"https://%1\" style=\"color:%2; "
                                           "text-decoration:none\">%1</a>")
                                .arg(repo, colors.primary.name()));
    link->setOpenExternalLinks(true);
    link->setToolTip(QStringLiteral("https://") + repo);
    info(about, "github", QStringLiteral("GitHub"), link, colors.textVariant);

    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(name, &QLineEdit::textChanged, this, &SettingsDialog::validate);
    connect(key, &QLineEdit::textChanged, this, &SettingsDialog::validate);
    page->addSpacing(8);
    page->addWidget(buttons);

    Theme::style(buttons);
    Theme::apply(this);
    for (Switch *s : {tls, announcements, pushToTalk, video, startHidden})
        s->setColors(colors.primary, colors.onPrimary, colors.card, colors.outline);
    // As high as the wrapped hints need at this width.
    setMinimumWidth(530);
    resize(530, std::max(sizeHint().height(), heightForWidth(530)));
    validate();
}

void SettingsDialog::validate() {
    bool ok = !name->text().trimmed().isEmpty() && !key->text().trimmed().isEmpty();
    buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
}

bool SettingsDialog::save() {
    QString n = name->text().trimmed(), k = key->text().trimmed();
    bool restart = n != config.name() || k != config.key() || port->value() != config.port() ||
                   tls->isChecked() != config.tls();
    config.setIdentity(n, k);
    config.setPort(port->value());
    config.setTls(tls->isChecked());
    config.setVideo(video->isChecked());
    config.setCamera(camera->currentData().toString());
    config.setAcceptAnnouncements(announcements->isChecked());
    config.setPushToTalk(pushToTalk->isChecked());
    config.setStartHidden(startHidden->isChecked());
    return restart;
}
