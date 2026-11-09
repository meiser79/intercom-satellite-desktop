#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include "CallView.h"
#include "Discovery.h"
#include "Engine.h"
#include "HistoryDialog.h"
#include "Icons.h"
#include "Ringer.h"
#include "SettingsDialog.h"
#include "Switch.h"

namespace {
QString describe(Peer::Status s) {
    switch (s) {
    case Peer::Status::Checking: return MainWindow::tr("Checking …");
    case Peer::Status::Ready: return MainWindow::tr("Ready");
    case Peer::Status::Unreachable: return MainWindow::tr("Not reachable");
    case Peer::Status::Off: return MainWindow::tr("Intercom is off");
    case Peer::Status::Dnd: return MainWindow::tr("Do not disturb");
    case Peer::Status::KeyMismatch: return MainWindow::tr("Different intercom key");
    case Peer::Status::TlsMismatch: return MainWindow::tr("TLS setting differs");
    case Peer::Status::ConfirmCert: return MainWindow::tr("Confirm certificate");
    }
    return {};
}

/**
 * Online, Do not disturb or not reachable, drawn like the Android app's badges: a green dot, a
 * red dot with a bar, a grey ring, set off by a ring in the card's color. The tooltip tells the
 * reason.
 */
void paintBadge(QPainter &painter, const Peer &p, const Theme::Colors &c, QPointF center) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(c.card);
    painter.drawEllipse(center, 8, 8);
    if (p.ready) {
        painter.setBrush(c.online);
        painter.drawEllipse(center, 6, 6);
    } else if (p.status == Peer::Status::Dnd) {
        painter.setBrush(c.dnd);
        painter.drawEllipse(center, 6, 6);
        painter.setBrush(c.card);
        painter.drawRoundedRect(QRectF(center.x() - 3.25, center.y() - 0.8, 6.5, 1.6), 0.8, 0.8);
    } else {
        painter.setPen(QPen(c.offline, 1.8));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, 5.1, 5.1);
    }
}

/** The room symbols, as in the Android app. */
const QStringList ROOM_ICONS = {"home",    "living", "kitchen", "dining",   "bedroom", "kids",
                                "office",  "bath",   "entrance", "garage",  "garden",  "stairs"};

QString roomLabel(const QString &icon) {
    if (icon == "living") return MainWindow::tr("Living room");
    if (icon == "kitchen") return MainWindow::tr("Kitchen");
    if (icon == "dining") return MainWindow::tr("Dining room");
    if (icon == "bedroom") return MainWindow::tr("Bedroom");
    if (icon == "kids") return MainWindow::tr("Children's room");
    if (icon == "office") return MainWindow::tr("Office");
    if (icon == "bath") return MainWindow::tr("Bathroom");
    if (icon == "entrance") return MainWindow::tr("Entrance");
    if (icon == "garage") return MainWindow::tr("Garage");
    if (icon == "garden") return MainWindow::tr("Garden");
    if (icon == "stairs") return MainWindow::tr("Stairs");
    return MainWindow::tr("Home");
}
}  // namespace

MainWindow::MainWindow(Config &config, Options options)
    : config(config), options(std::move(options)) {
    setWindowTitle(QStringLiteral("Intercom Satellite"));
    // The installed icon by name, so tray hosts that take icon names show it too; the built-in
    // copy when not installed.
    setWindowIcon(QIcon::fromTheme("intercom-satellite", QIcon(":/icons/intercom-satellite.svg")));
    // Never narrower than two kiosk tiles side by side.
    setMinimumWidth(530);
    resize(530, 840);
    // Where it was left; restoreGeometry() keeps it on a screen that is still there.
    restoreGeometry(config.windowGeometry());

    // Home page, call screen and dialogs look like the Android app's.
    home = new QWidget;
    home->setObjectName("page");
    statusTitle = new QLabel;
    statusTitle->setObjectName("statusTitle");
    QFont title = statusTitle->font();
    title.setPointSizeF(title.pointSizeF() * 1.45);
    title.setWeight(QFont::DemiBold);
    statusTitle->setFont(title);
    statusText = new QLabel;
    statusText->setObjectName("variant");
    statusText->setWordWrap(true);
    statusText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dnd = new Switch;
    dnd->setAccessibleName(tr("Do not disturb"));
    connect(dnd, &Switch::toggled, this, [this](bool on) {
        this->config.setDnd(on);
        render();
    });
    auto *dndLabel = new QLabel(tr("Do not disturb"));
    auto *dndRow = new QHBoxLayout;
    dndRow->addWidget(dndLabel, 1);
    dndRow->addWidget(dnd);
    notice = new QLabel;
    notice->setObjectName("variant");
    notice->setWordWrap(true);

    auto *statusCard = new QFrame;
    statusCard->setObjectName("card");
    auto *statusLayout = new QVBoxLayout(statusCard);
    statusLayout->setContentsMargins(20, 16, 16, 16);
    statusLayout->setSpacing(6);
    // Quits, as the Android app's power button turns availability off.
    power = new QToolButton;
    power->setObjectName("tonal");
    power->setToolTip(tr("Quit"));
    power->setAccessibleName(tr("Quit"));
    power->setIconSize(QSize(20, 20));
    power->setFixedSize(40, 40);  // round: the radius is half of it
    connect(power, &QToolButton::clicked, this, &MainWindow::quit);
    auto *titleRow = new QHBoxLayout;
    titleRow->addWidget(statusTitle, 1);
    titleRow->addWidget(power, 0, Qt::AlignTop);
    statusLayout->addLayout(titleRow);
    statusLayout->addWidget(statusText);
    statusLayout->addSpacing(4);
    statusLayout->addLayout(dndRow);

    auto *kiosksTitle = new QLabel(tr("Kiosks"));
    QFont section = kiosksTitle->font();
    section.setPointSizeF(section.pointSizeF() * 1.15);
    section.setWeight(QFont::DemiBold);
    kiosksTitle->setFont(section);
    refresh = new QToolButton;
    refresh->setToolTip(tr("Refresh"));
    refresh->setAccessibleName(tr("Refresh"));
    refresh->setIconSize(QSize(22, 22));
    connect(refresh, &QToolButton::clicked, this, [this] {
        if (engine) engine->refresh();
    });
    auto *kiosksHeader = new QHBoxLayout;
    kiosksHeader->setContentsMargins(4, 8, 0, 0);
    kiosksHeader->addWidget(kiosksTitle, 1);
    kiosksHeader->addWidget(refresh);

    auto *list = new QWidget;
    kiosks = new QGridLayout(list);
    kiosks->setContentsMargins(0, 0, 0, 0);
    kiosks->setSpacing(10);
    auto *listColumn = new QWidget;
    auto *listLayout = new QVBoxLayout(listColumn);
    listLayout->setContentsMargins(0, 0, 0, 0);
    empty = new QLabel;
    empty->setObjectName("variant");
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    listLayout->addWidget(empty);
    listLayout->addWidget(list);
    listLayout->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidget(listColumn);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    announce = new QPushButton(tr("Start announcement"));
    announce->setObjectName("primary");
    announce->setMinimumHeight(44);
    connect(announce, &QPushButton::clicked, this, [this] {
        if (engine) engine->announce();
    });
    add = new QPushButton(tr("Add kiosk"));
    add->setObjectName("outlined");
    connect(add, &QPushButton::clicked, this, &MainWindow::addKiosk);

    // The top bar as the Android app's: its name, then the call history and the settings.
    auto *appTitle = new QLabel(QStringLiteral("Intercom Satellite"));
    QFont appFont = appTitle->font();
    appFont.setPointSizeF(appFont.pointSizeF() * 1.5);
    appTitle->setFont(appFont);
    auto barButton = [this](const QString &text) {
        auto *b = new QToolButton;
        b->setToolTip(text);
        b->setAccessibleName(text);
        b->setIconSize(QSize(24, 24));
        b->setFixedSize(40, 40);  // round: the radius is under half of it
        return b;
    };
    historyButton = barButton(tr("Calls"));
    connect(historyButton, &QToolButton::clicked, this, &MainWindow::showHistory);
    settings = barButton(tr("Settings"));
    connect(settings, &QToolButton::clicked, this, &MainWindow::showSettings);
    auto *appBar = new QHBoxLayout;
    appBar->setContentsMargins(4, 0, 0, 0);
    appBar->setSpacing(4);
    appBar->addWidget(appTitle, 1);
    appBar->addWidget(historyButton);
    appBar->addWidget(settings);

    auto *homeLayout = new QVBoxLayout(home);
    homeLayout->setContentsMargins(16, 16, 16, 16);
    homeLayout->setSpacing(10);
    homeLayout->addLayout(appBar);
    homeLayout->addWidget(statusCard);
    homeLayout->addLayout(kiosksHeader);
    homeLayout->addWidget(notice);
    homeLayout->addWidget(scroll, 1);
    homeLayout->addWidget(announce);
    homeLayout->addWidget(add);

    callView = new CallView;
    callView->setObjectName("page");
    callView->setAttribute(Qt::WA_StyledBackground);
    pages = new QStackedWidget;
    pages->addWidget(home);
    pages->addWidget(callView);
    setCentralWidget(pages);

    ringer = new Ringer(this);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray = new QSystemTrayIcon(windowIcon(), this);
        // The call's buttons while there is one, then the window's actions. The tray host draws
        // it, so only the icons follow the app's theme. Each icon's name is kept for applyTheme();
        // they are shown also where the desktop leaves icons out of menus.
        auto *menu = new QMenu(this);
        auto icon = [](QAction *a, const char *name) { a->setProperty("iconName", name); };
        trayAnswer = menu->addAction(tr("Answer"), this, [this] {
            if (engine) engine->answer(false);
            bringToFront();
        });
        icon(trayAnswer, "call");
        trayDecline = menu->addAction(tr("Decline"), this, [this] {
            if (engine) engine->decline();
        });
        icon(trayDecline, "call_end");
        trayEnd = menu->addAction(tr("End"), this, [this] {
            if (engine) engine->hangUp();
        });
        icon(trayEnd, "call_end");
        trayCallSeparator = menu->addSeparator();
        icon(menu->addAction(tr("Open window"), this, &MainWindow::bringToFront), "visibility");
        trayAnnounce = menu->addAction(tr("Start announcement"), this, [this] {
            if (engine) engine->announce();
            bringToFront();
        });
        icon(trayAnnounce, "volume_up");
        icon(menu->addAction(tr("Calls"), this, &MainWindow::showHistory), "history");
        // A switch as its icon, not a check box, to line up with the other icons.
        trayDnd = menu->addAction(tr("Do not disturb"), this, [this] { dnd->toggle(); });
        icon(menu->addAction(tr("Settings"), this, [this] {
                 bringToFront();
                 showSettings();
             }),
             "settings");
        menu->addSeparator();
        icon(menu->addAction(tr("Quit"), this, &MainWindow::quit), "power");
        for (QAction *a : menu->actions()) a->setIconVisibleInMenu(true);
        tray->setContextMenu(menu);
        connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r != QSystemTrayIcon::Trigger) return;
            if (isVisible() && isActiveWindow()) hide();
            else bringToFront();
        });
        connect(tray, &QSystemTrayIcon::messageClicked, this, &MainWindow::bringToFront);
        tray->show();
    }

    dnd->setChecked(config.dnd());
    applyTheme();
    startEngine();
}

MainWindow::~MainWindow() {
    ringer->stop();
    engine.reset();
}

void MainWindow::present() {
    if (!config.startHidden() || !tray || config.key().isEmpty()) show();
    if (config.key().isEmpty()) QTimer::singleShot(0, this, &MainWindow::showSettings);
}

void MainWindow::startEngine() {
    callView->setEngine(nullptr);
    engine.reset();
    startProblem.clear();
    engine = std::make_unique<Engine>(config);
    callView->setEngine(engine.get());
    connect(engine.get(), &Engine::changed, this, &MainWindow::render);
    connect(engine.get(), &Engine::incomingCall, this, &MainWindow::incoming);
    connect(callView, &CallView::trustRequested, engine.get(), [this] {
        if (engine && confirmCertificate(engine->callPeer())) engine->trustCaller();
    });
    connect(engine.get(), &Engine::missedCall, this, [this](const QString &name) {
        if (tray) tray->showMessage(tr("Missed call"), name, QSystemTrayIcon::Information, 15000);
    });
    QString error;
    if (!engine->start(&error, options.mdns)) {
        startProblem = error;
        engine.reset();
        callView->setEngine(nullptr);
    }
    lastFingerprint.clear();
    render();
}

QString MainWindow::fingerprint() const {
    QString f = engine ? engine->state() : "off";
    f += startProblem + QString::number(config.dnd()) + QString::number(config.video());
    if (engine)
        for (const Peer &p : engine->peerList())
            f += "|" + p.addressKey() + p.id + p.name + QString::number(int(p.status)) +
                 QString::number(p.video) + p.candidatePin + p.problem;
    return f;
}

void MainWindow::render() {
    bool calling = engine && engine->active();
    pages->setCurrentWidget(calling ? static_cast<QWidget *>(callView) : home);
    if (calling) {
        const Peer &peer = engine->callPeer();
        callView->setSymbol(engine->state() == "broadcasting" ? QStringLiteral("volume_up")
                            : "room_" + (peer.id.isEmpty() ? QStringLiteral("home")
                                                           : config.roomIcon(peer.id)));
        callView->render();
        callView->setFocus();
    }
    if (engine && engine->state() == "ringing") ringer->start();
    else ringer->stop();
    if (tray) tray->setToolTip(windowTitle() + (calling ? " – " + engine->peerName() : QString()));

    if (!engine) {
        statusTitle->setText(tr("You are offline"));
        statusText->setText(startProblem);
    } else {
        statusTitle->setText(config.dnd() ? tr("Do not disturb") : tr("You are reachable"));
        QString ip = Discovery::localIp();
        QString where = tr("%1 · %2, port %3")
                            .arg(config.name(), ip.isEmpty() ? tr("no network") : ip)
                            .arg(config.port());
        if (config.tls()) where += " · " + tr("encrypted (TLS) on port %1").arg(config.port() + 1);
        statusText->setText(where);
    }
    dnd->setEnabled(engine != nullptr);
    announce->setEnabled(engine && !engine->active());
    add->setEnabled(engine != nullptr);
    refresh->setEnabled(engine != nullptr);

    QStringList notes;
    if (engine && !engine->discoveryProblem().isEmpty())
        notes << tr("Kiosks can't be found automatically (%1); add them by address.")
                     .arg(engine->discoveryProblem());
    notice->setText(notes.join('\n'));
    notice->setVisible(!notes.isEmpty());

    QString current = fingerprint();
    if (current != lastFingerprint) {
        lastFingerprint = current;
        renderKiosks();
    }
    renderTray();

    // A call asked for on the command line starts once its kiosk is reachable.
    if (engine && !options.call.isEmpty() && !engine->active()) {
        for (const Peer &p : engine->peerList()) {
            if (!p.ready) continue;
            if (options.call == p.host || options.call == p.addressKey() || options.call == p.name) {
                QString key = p.addressKey();
                options.call.clear();
                QTimer::singleShot(0, this, [this, key] {
                    if (engine) engine->call(key, options.video);
                });
                break;
            }
        }
    }
}

void MainWindow::renderKiosks() {
    // Later, not at once: this may run while Qt still delivers an event to an old tile.
    while (QLayoutItem *item = kiosks->takeAt(0)) {
        if (QWidget *tile = item->widget()) {
            tile->hide();
            tile->deleteLater();
        }
        delete item;
    }
    QList<Peer> peers = engine ? engine->peerList() : QList<Peer>();
    empty->setVisible(peers.isEmpty());
    empty->setText(engine ? tr("Looking for kiosks …\n\nIf none shows up, add one by its address.")
                          : QString());
    bool anyReady = false;
    int columns = kioskColumns(), index = 0;
    shownColumns = columns;
    for (const Peer &p : peers) {
        anyReady |= p.ready;
        auto *tile = new QFrame;
        tile->setObjectName("card");
        tile->setMinimumWidth(200);
        auto *v = new QVBoxLayout(tile);
        v->setContentsMargins(16, 14, 8, 16);
        v->setSpacing(2);
        QString why = p.problem.isEmpty() ? describe(p.status) : describe(p.status) + "\n" + p.problem;
        tile->setToolTip(why);
        auto *top = new QHBoxLayout;
        auto *icon = new QLabel;
        icon->setPixmap(avatar(p));
        icon->setAccessibleName(describe(p.status));
        auto *more = new QToolButton;
        Icons::set(more, "more_vert", colors.text);
        more->setToolTip(tr("More"));
        more->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(more);
        QString id = p.id, key = p.addressKey();
        QAction *announceOne = menu->addAction(tr("Announcement"), this, [this, id] {
            if (engine) engine->announce({id});
        });
        announceOne->setEnabled(p.ready && !id.isEmpty());
        if (!id.isEmpty()) {
            Peer shown = p;
            menu->addAction(tr("Edit"), this, [this, shown] { editKiosk(shown); });
        }
        if (p.manual) {
            menu->addAction(tr("Remove"), this, [this, key] {
                if (Theme::message(this, tr("Remove kiosk"),
                                   tr("Remove this kiosk from the list?"), tr("Remove")) &&
                    engine)
                    engine->remove(key);
            });
        }
        more->setMenu(menu);
        top->addWidget(icon, 0, Qt::AlignTop);
        top->addStretch();
        top->addWidget(more, 0, Qt::AlignTop);
        v->addLayout(top);
        v->addSpacing(8);
        auto *label = new QLabel(engine->display(p));
        QFont name = label->font();
        name.setPointSizeF(name.pointSizeF() * 1.25);
        label->setFont(name);
        label->setWordWrap(true);
        v->addWidget(label);
        auto *sub = new QLabel(p.ready ? p.host : p.host + " · " + describe(p.status));
        sub->setObjectName("variant");
        sub->setWordWrap(true);
        v->addWidget(sub);
        v->addSpacing(10);
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 8, 0);
        row->setSpacing(10);
        if (p.status == Peer::Status::ConfirmCert) {
            auto *trust = new QPushButton(tr("Confirm certificate"));
            trust->setObjectName("tonal");
            Peer shown = p;
            connect(trust, &QPushButton::clicked, this, [this, shown] {
                if (engine && confirmCertificate(shown)) engine->trust(shown.addressKey());
            });
            row->addWidget(trust);
        } else if (!p.ready) {
            // Like the Android app: why the kiosk can't be called, instead of a dead button.
            auto *info = new QPushButton(tr("Info"));
            info->setObjectName("tonal");
            Peer shown = p;
            connect(info, &QPushButton::clicked, this, [this, shown] { showInfo(shown); });
            row->addWidget(info);
        } else {
            auto *callButton = new QPushButton;
            callButton->setObjectName("primary");
            callButton->setToolTip(tr("Call", "button"));
            callButton->setAccessibleName(tr("Call", "button"));
            Icons::set(callButton, "call", colors.onPrimary);
            connect(callButton, &QPushButton::clicked, this, [this, key] {
                if (engine) engine->call(key, false);
            });
            row->addWidget(callButton, 1);
            if (p.video && config.video()) {
                auto *videoButton = new QPushButton;
                videoButton->setObjectName("tonal");
                videoButton->setToolTip(tr("Video"));
                videoButton->setAccessibleName(tr("Video"));
                Icons::set(videoButton, "videocam", colors.onTonal);
                connect(videoButton, &QPushButton::clicked, this, [this, key] {
                    if (engine) engine->call(key, true);
                });
                row->addWidget(videoButton, 1);
            }
        }
        v->addLayout(row);
        kiosks->addWidget(tile, index / columns, index % columns);
        index++;
    }
    // Equal columns; those left from a wider window take no room.
    for (int c = 0; c < kiosks->columnCount(); c++) kiosks->setColumnStretch(c, c < columns ? 1 : 0);
    announce->setEnabled(engine && !engine->active() && anyReady);
}

void MainWindow::renderTray() {
    if (!tray) return;
    QString state = engine ? engine->state() : QString();
    bool ringing = state == "ringing";
    bool calling = engine && engine->active();
    trayAnswer->setVisible(ringing);
    trayDecline->setVisible(ringing);
    trayEnd->setVisible(calling && !ringing);
    trayCallSeparator->setVisible(calling);
    trayEnd->setText(state == "broadcasting" ? tr("End announcement") : tr("End"));
    trayAnnounce->setEnabled(announce->isEnabled());
    trayDnd->setEnabled(engine != nullptr);
    // Repainted only when it changes, as each change goes to the tray host.
    QString dndIcon = config.dnd() ? "toggle_on" : "toggle_off";
    QColor dndColor = config.dnd() ? colors.primary : colors.text;
    QString key = dndIcon + dndColor.name();
    if (trayDnd->property("iconKey").toString() != key) {
        trayDnd->setProperty("iconKey", key);
        trayDnd->setIcon(Icons::tinted(dndIcon, dndColor, colors.outline));
    }
}

int MainWindow::kioskColumns() const {
    // Tiles of about 240 pixels with their 10 between, at least two.
    return qMax(2, (width() - 32 + 10) / 250);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if (kioskColumns() != shownColumns) {
        lastFingerprint.clear();
        render();
    }
}

void MainWindow::addKiosk() {
    // As wide as the Settings window, with the explanation wrapping above the field.
    // Buttons are named before the style sheet is set, which styles them by name.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add kiosk"));
    auto *label = new QLabel(
        tr("Address of the kiosk, for example 192.168.1.50 or kitchen.local. Port 2324 is used; "
           "for another port, add it after a colon, for example 192.168.1.50:2400."));
    label->setWordWrap(true);
    auto *field = new QLineEdit;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label);
    layout->addWidget(field);
    layout->addWidget(buttons);
    Theme::style(buttons);
    Theme::apply(&dialog);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(12);
    dialog.setFixedWidth(SettingsDialog(config, this).width());
    if (dialog.exec() != QDialog::Accepted) return;
    QString address = field->text();
    if (address.trimmed().isEmpty() || !engine) return;
    QString error;
    if (!engine->add(address.trimmed(), &error)) Theme::message(this, tr("Add kiosk"), error);
}

void MainWindow::editKiosk(const Peer &p) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Edit"));
    auto *label = new QLabel(tr("Room name"));
    auto *field = new QLineEdit(config.alias(p.id));
    field->setMaxLength(40);  // as in the Android app
    QString own = p.name.isEmpty() ? config.knownName(p.id) : p.name;
    field->setPlaceholderText(own);
    auto *hint = new QLabel(tr("Leave it empty for the kiosk's own name."));
    hint->setObjectName("variant");
    hint->setWordWrap(true);

    // The room symbols as round buttons, the chosen one filled, as the Android app's chips.
    auto *symbolLabel = new QLabel(tr("Symbol"));
    auto *symbols = new QButtonGroup(&dialog);
    auto *grid = new QGridLayout;
    grid->setSpacing(8);
    QString current = config.roomIcon(p.id);
    auto tint = [this](QToolButton *b) {
        Icons::set(b, "room_" + b->property("room").toString(),
                   b->isChecked() ? colors.onTonal : colors.text);
    };
    for (int i = 0; i < ROOM_ICONS.size(); i++) {
        auto *b = new QToolButton;
        b->setObjectName("symbol");
        b->setProperty("room", ROOM_ICONS[i]);
        b->setCheckable(true);
        b->setChecked(ROOM_ICONS[i] == current);
        b->setToolTip(roomLabel(ROOM_ICONS[i]));
        b->setAccessibleName(roomLabel(ROOM_ICONS[i]));
        b->setFixedSize(44, 44);  // round: the radius is half of it
        b->setIconSize(QSize(24, 24));
        tint(b);
        connect(b, &QToolButton::toggled, b, [tint, b] { tint(b); });
        symbols->addButton(b);
        grid->addWidget(b, i / 6, i % 6);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label);
    layout->addWidget(field);
    layout->addWidget(hint);
    layout->addSpacing(4);
    layout->addWidget(symbolLabel);
    layout->addLayout(grid);
    layout->addSpacing(4);
    layout->addWidget(buttons);
    Theme::style(buttons);
    Theme::apply(&dialog);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(12);
    dialog.setFixedWidth(SettingsDialog(config, this).width());
    if (dialog.exec() != QDialog::Accepted) return;
    QString alias = field->text().simplified();
    config.setAlias(p.id, alias == own ? QString() : alias);
    if (QAbstractButton *chosen = symbols->checkedButton())
        config.setRoomIcon(p.id, chosen->property("room").toString());
    lastFingerprint.clear();
    render();
}

bool MainWindow::confirmCertificate(const Peer &p) {
    if (p.candidatePin.isEmpty()) return false;
    QString name = p.name.isEmpty() ? p.host : p.name;
    return Theme::message(
        this, tr("Verify kiosk certificate"),
        tr("%1\n%2:%3\n\nSHA-256:\n%4\n\nCompare this fingerprint with the kiosk’s certificate. "
           "After you confirm, only this certificate is accepted.")
            .arg(name, p.host)
            .arg(p.candidatePort)
            .arg(p.candidatePin),
        tr("Trust"), true);
}

void MainWindow::showHistory() {
    if (!history) {
        history = new HistoryDialog(config, engine.get(), this);
        history->show();
    }
    history->raise();
    history->activateWindow();
}

void MainWindow::showSettings() {
    SettingsDialog dialog(config, this);
    if (dialog.exec() != QDialog::Accepted) return;
    bool restart = dialog.save();
    if (restart || !engine) {
        if (engine && engine->active() &&
            !Theme::message(this, tr("Settings"),
                            tr("The intercom restarts with the new settings, which ends the "
                               "call. Restart now?"),
                            tr("Restart")))
            return;
        startEngine();
    } else {
        lastFingerprint.clear();
        render();
    }
}

void MainWindow::incoming(const QString &name, bool video) {
    if (history) history->close();  // not over the call screen
    bringToFront();
    if (tray)
        tray->showMessage(video ? tr("Video call") : tr("Call", "incoming"), name, QSystemTrayIcon::Information,
                          45000);
    if (options.autoAnswer)
        QTimer::singleShot(1000, this, [this] {
            if (engine) engine->answer(options.video);
        });
}

void MainWindow::bringToFront() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::quit() {
    quitting = true;
    close();
    QApplication::quit();
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    // Minimized, it goes to the tray, off the taskbar; the tray icon brings it back.
    if (event->type() == QEvent::WindowStateChange && isMinimized() && tray) {
        QTimer::singleShot(0, this, &QWidget::hide);
        return;
    }
    // A light or dark theme: icons and status badges take the new colors. Once Qt has told every
    // widget: applyTheme() rebuilds the kiosk tiles, which Qt may not have reached yet.
    if ((event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange) ||
        theming || themePending)
        return;
    themePending = true;
    QTimer::singleShot(0, this, [this] {
        themePending = false;
        Icons::refresh(this);
        applyTheme();
    });
}

void MainWindow::applyTheme() {
    theming = true;
    colors = Theme::colors(palette());
    QString sheet = Theme::styleSheet(colors);
    home->setStyleSheet(sheet);
    callView->setStyleSheet(sheet);
    callView->setColors(colors);
    dnd->setColors(colors.primary, colors.onPrimary, colors.card, colors.outline);
    Icons::set(announce, "volume_up", colors.onPrimary);
    Icons::set(add, "add", colors.primary);
    Icons::set(settings, "settings", colors.text);
    Icons::set(refresh, "refresh", colors.text);
    Icons::set(historyButton, "history", colors.text);
    Icons::set(power, "power", colors.onTonal);
    if (tray)
        for (QAction *a : tray->contextMenu()->actions())
            if (a->property("iconName").isValid())
                a->setIcon(Icons::tinted(a->property("iconName").toString(), colors.text, colors.outline));
    theming = false;
    lastFingerprint.clear();
    if (engine) render();
}

QPixmap MainWindow::avatar(const Peer &p) const {
    // A 44 px circle, the badge on its lower right as in the Android app.
    const qreal size = 48, circle = 44;
    qreal ratio = devicePixelRatioF();
    QPixmap pixmap(qCeil(size * ratio), qCeil(size * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.tonal);
    painter.drawEllipse(QRectF(0, 0, circle, circle));
    QString room = p.id.isEmpty() ? QStringLiteral("home") : config.roomIcon(p.id);
    QPixmap symbol = Icons::tinted("room_" + room, colors.card, colors.card).pixmap(QSize(24, 24), ratio);
    painter.drawPixmap(QPointF((circle - 24) / 2, (circle - 24) / 2), symbol);
    paintBadge(painter, p, colors, QPointF(circle / 2 + 17, circle / 2 + 17));
    return pixmap;
}

void MainWindow::showInfo(const Peer &p) {
    QString text = describe(p.status);
    if (!p.problem.isEmpty()) text += "\n\n" + p.problem;
    Theme::message(this, engine ? engine->display(p) : p.host, text);
}

void MainWindow::hideEvent(QHideEvent *event) {
    // Closed to the tray, hidden from it or quit: all hide the window.
    if (!event->spontaneous()) config.setWindowGeometry(saveGeometry());
    QMainWindow::hideEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!quitting && tray) {
        // The intercom stays reachable from the tray.
        hide();
        event->ignore();
        return;
    }
    if (engine) engine->stop();
    event->accept();
}
