#include "HistoryDialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "Config.h"
#include "Engine.h"
#include "Icons.h"

HistoryDialog::HistoryDialog(Config &config, Engine *engine, QWidget *parent)
    : QDialog(parent), config(config), engine(engine), colors(Theme::colors(palette())) {
    setWindowTitle(tr("Calls"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto *privacy =
        new QLabel(tr("Only the time and outcome are stored on this computer. Audio is never "
                      "recorded."));
    privacy->setObjectName("variant");
    privacy->setWordWrap(true);

    list = new QWidget;
    rows = new QVBoxLayout(list);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(8);
    scroll = new QScrollArea;
    scroll->setWidget(list);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    empty = new QWidget;
    auto *emptyLayout = new QVBoxLayout(empty);
    auto *emptyTitle = new QLabel(tr("No calls yet"));
    QFont big = emptyTitle->font();
    big.setPointSizeF(big.pointSizeF() * 1.25);
    emptyTitle->setFont(big);
    auto *emptyBody = new QLabel(tr("Your conversations and announcements will appear here."));
    emptyBody->setObjectName("variant");
    emptyBody->setWordWrap(true);
    for (QLabel *l : {emptyTitle, emptyBody}) l->setAlignment(Qt::AlignCenter);
    emptyLayout->addStretch();
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyBody);
    emptyLayout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    clearButton = buttons->addButton(tr("Clear history"), QDialogButtonBox::ActionRole);
    connect(clearButton, &QPushButton::clicked, this, &HistoryDialog::clear);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    Theme::style(buttons);
    buttons->button(QDialogButtonBox::Close)->setObjectName("primary");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    layout->addWidget(privacy);
    layout->addWidget(scroll, 1);
    layout->addWidget(empty, 1);
    layout->addWidget(buttons);
    Theme::apply(this);
    resize(480, 600);

    if (engine) connect(engine, &Engine::historyChanged, this, &HistoryDialog::render);
    render();
}

void HistoryDialog::render() {
    while (QLayoutItem *item = rows->takeAt(0)) {
        if (QWidget *row = item->widget()) {
            row->hide();
            row->deleteLater();
        }
        delete item;
    }
    const QJsonArray items = config.history();
    empty->setVisible(items.isEmpty());
    scroll->setVisible(!items.isEmpty());
    clearButton->setEnabled(!items.isEmpty());

    QList<Peer> peers = engine ? engine->peerList() : QList<Peer>();
    for (const QJsonValue &value : items) {
        QJsonObject item = value.toObject();
        QString id = item.value("id").toString(), alias = config.alias(id);
        QString name = item.value("name").toString();
        bool announcement = item.value("announcement").toBool();
        if (announcement)
            name = name.isEmpty() ? tr("Announcement") : tr("Announcement · %1").arg(name);
        else if (!alias.isEmpty())
            name = alias;
        else if (name.isEmpty())
            name = tr("Unknown kiosk");

        auto *row = new QFrame;
        row->setObjectName("card");
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(16, 10, 10, 10);
        h->setSpacing(14);

        // Which way, as in the Android app: out to the upper right, in to the lower left.
        auto *icon = new QLabel;
        QString symbol = announcement                 ? QStringLiteral("volume_up")
                         : item.value("out").toBool() ? QStringLiteral("north_east")
                                                      : QStringLiteral("south_west");
        icon->setPixmap(Icons::tinted(symbol, colors.textVariant, colors.textVariant)
                            .pixmap(QSize(22, 22), devicePixelRatioF()));
        h->addWidget(icon);

        auto *text = new QVBoxLayout;
        text->setSpacing(1);
        auto *title = new QLabel(name);
        QFont titleFont = title->font();
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.1);
        title->setFont(titleFont);
        QString when = QLocale().toString(
            QDateTime::fromMSecsSinceEpoch(qint64(item.value("at").toDouble())),
            QLocale::ShortFormat);
        QString result = item.value("result").toString();
        qint64 seconds = qint64(item.value("seconds").toDouble());
        if (seconds > 0)
            result += QStringLiteral("  ·  %1:%2\u00a0min")
                          .arg(seconds / 60)
                          .arg(seconds % 60, 2, 10, QChar('0'));
        auto *detail = new QLabel(result.isEmpty() ? when : when + "  ·  " + result);
        detail->setObjectName("variant");
        detail->setWordWrap(true);
        text->addWidget(title);
        text->addWidget(detail);
        h->addLayout(text, 1);

        if (!announcement && !id.isEmpty()) {
            auto *again = new QToolButton;
            again->setObjectName("tonal");
            again->setToolTip(tr("Call again"));
            again->setAccessibleName(tr("Call again"));
            again->setIconSize(QSize(20, 20));
            again->setFixedSize(40, 40);  // round: the radius is half of it
            Icons::set(again, "call", colors.onTonal);
            QString key;
            for (const Peer &p : peers)
                if (p.id == id && p.ready) key = p.addressKey();
            again->setEnabled(!key.isEmpty() && engine && !engine->active());
            connect(again, &QToolButton::clicked, this, [this, key] {
                if (engine) engine->call(key, false);
                close();
            });
            h->addWidget(again);
        }
        rows->addWidget(row);
    }
    rows->addStretch();
}

void HistoryDialog::clear() {
    if (Theme::message(this, tr("Clear history"), tr("Clear the call history?"), tr("Delete"))) {
        config.clearHistory();
        render();
    }
}
