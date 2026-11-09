#include "CallView.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include "Engine.h"
#include "Icons.h"

/** The voice level as a row of dots, lit from the left, as in the Android app. */
class LevelDots : public QWidget {
public:
    using QWidget::QWidget;
    QSize sizeHint() const override { return {DOTS * 19, 14}; }
    void setLevel(qreal value) {
        int lit = qBound(0, qRound(value * DOTS * 1.5), DOTS);
        if (lit == this->lit) return;
        this->lit = lit;
        update();
    }
    void setColors(const QColor &on, const QColor &off) {
        this->on = on;
        this->off = off;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        qreal x = (width() - DOTS * 19) / 2.0 + 9.5;
        for (int i = 0; i < DOTS; i++) {
            p.setBrush(i < lit ? on : off);
            p.drawEllipse(QPointF(x + i * 19, height() / 2.0), 6, 6);
        }
    }

private:
    static constexpr int DOTS = 12;
    int lit = 0;
    QColor on, off;
};

namespace {
QPushButton *button(const QString &object, const QString &text, QWidget *parent) {
    auto *b = new QPushButton(text, parent);
    b->setObjectName(object);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}
}  // namespace

CallView::CallView(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    overline = new QLabel;
    overline->setObjectName("overline");
    overline->setAlignment(Qt::AlignCenter);
    QFont small = overline->font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    small.setWeight(QFont::DemiBold);
    small.setLetterSpacing(QFont::PercentageSpacing, 110);
    overline->setFont(small);
    avatar = new QLabel;
    avatar->setAlignment(Qt::AlignCenter);
    name = new QLabel;
    QFont big = name->font();
    big.setPointSizeF(big.pointSizeF() * 1.7);
    name->setFont(big);
    name->setAlignment(Qt::AlignCenter);
    name->setWordWrap(true);
    status = new QLabel;
    status->setObjectName("variant");
    status->setAlignment(Qt::AlignCenter);
    status->setWordWrap(true);
    QFont medium = status->font();
    medium.setPointSizeF(medium.pointSizeF() * 1.1);
    status->setFont(medium);
    timer = new QLabel;
    timer->setAlignment(Qt::AlignCenter);
    QFont mono = timer->font();
    mono.setPointSizeF(mono.pointSizeF() * 1.1);
    mono.setWeight(QFont::DemiBold);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    mono.setFeature("tnum", 1);  // digits of one width: the timer doesn't jitter
#endif
    timer->setFont(mono);
    meter = new LevelDots;
    meter->setAccessibleName(tr("Voice level"));

    video = new QWidget;
    video->setMinimumHeight(240);
    // The picture on the card, letterboxed in the card's color.
    video->setStyleSheet("background: transparent;");
    auto *videoLayout = new QVBoxLayout(video);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    far = new QLabel;
    far->setAlignment(Qt::AlignCenter);
    far->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    videoLayout->addWidget(far);
    self = new QLabel(video);
    self->setStyleSheet("border: 2px solid white; background: black;");
    self->hide();

    hint = new QLabel(tr("Hold the button or Space to talk."));
    hint->setObjectName("variant");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);

    trust = button("tonal", tr("Confirm certificate"), this);
    answer = button("primary", tr("Answer"), this);
    answerVideo = button("tonal", tr("Answer with video"), this);
    decline = button("danger", tr("Decline"), this);
    talk = button("talk", tr("Hold to talk"), this);
    QFont large = talk->font();
    large.setPointSizeF(large.pointSizeF() * 1.35);
    talk->setFont(large);
    pushToTalk = button("toggle", tr("Push to talk"), this);
    pushToTalk->setCheckable(true);
    camera = button("toggle", tr("Camera"), this);
    camera->setCheckable(true);
    end = button("danger", tr("End"), this);

    connect(trust, &QPushButton::clicked, this, &CallView::trustRequested);
    connect(answer, &QPushButton::clicked, this, [this] { engine->answer(false); });
    connect(answerVideo, &QPushButton::clicked, this, [this] { engine->answer(true); });
    connect(decline, &QPushButton::clicked, this, [this] { engine->decline(); });
    connect(end, &QPushButton::clicked, this, [this] { engine->hangUp(); });
    connect(talk, &QPushButton::pressed, this, [this] { engine->talk(true); });
    connect(talk, &QPushButton::released, this, [this] {
        if (!engine->handsFree()) engine->talk(false);
    });
    connect(pushToTalk, &QPushButton::clicked, this,
            [this](bool on) { engine->setHandsFree(!on); });
    connect(camera, &QPushButton::clicked, this, [this](bool on) { engine->camera(on); });
    for (QPushButton *b : {pushToTalk, camera})
        connect(b, &QPushButton::toggled, this, &CallView::tintIcons);

    card = new QFrame;
    card->setObjectName("card");
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 20, 16, 24);
    cardLayout->setSpacing(6);
    cardLayout->addWidget(overline);
    cardLayout->addSpacing(6);
    cardLayout->addWidget(avatar);
    cardLayout->addWidget(video, 1);
    cardLayout->addSpacing(6);
    cardLayout->addWidget(name);
    cardLayout->addWidget(status);
    cardLayout->addWidget(timer);
    cardLayout->addSpacing(8);
    cardLayout->addWidget(meter, 0, Qt::AlignHCenter);

    auto *ringRow = new QHBoxLayout;
    ringRow->setSpacing(10);
    ringRow->addWidget(answer, 1);
    ringRow->addWidget(answerVideo, 1);
    auto *controls = new QHBoxLayout;
    controls->setSpacing(10);
    controls->addWidget(camera, 1);
    controls->addWidget(pushToTalk, 1);

    layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    layout->addWidget(card);
    layout->addWidget(hint);
    layout->addWidget(talk);
    layout->addWidget(trust);
    layout->addLayout(ringRow);
    layout->addWidget(decline);
    layout->addLayout(controls);
    layout->addWidget(end);
    layout->addStretch(1);

    // Shown by render(); hidden until then, they'd make the window as tall as all of them.
    for (QWidget *w : std::initializer_list<QWidget *>{video, hint, talk, trust, answer, answerVideo,
                                                        decline, pushToTalk, camera})
        w->hide();

    ticker = new QTimer(this);
    connect(ticker, &QTimer::timeout, this, &CallView::tick);
    ticker->start(100);
}

void CallView::setEngine(Engine *e) {
    engine = e;
    if (!e) return;
    connect(e, &Engine::farPicture, this, &CallView::showFar);
    connect(e, &Engine::selfPicture, this, &CallView::showSelf);
}

void CallView::setColors(const Theme::Colors &c) {
    colors = c;
    meter->setColors(c.primary, c.outline);
    far->setStyleSheet("color: " + c.textVariant.name() + ";");
    tintIcons();
    paintAvatar();
}

void CallView::setSymbol(const QString &icon) {
    if (icon == symbol) return;
    symbol = icon;
    paintAvatar();
}

void CallView::paintAvatar() {
    // A 72 px circle in a lighter primary, as on the Android app's call screen.
    const qreal size = 72;
    qreal ratio = devicePixelRatioF();
    QPixmap pixmap(qCeil(size * ratio), qCeil(size * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Theme::mix(colors.primary, colors.card, 0.3));
    painter.drawEllipse(QRectF(0, 0, size, size));
    QColor ink = colors.dark ? colors.onPrimary : colors.page;
    QPixmap icon = Icons::tinted(symbol, ink, ink).pixmap(QSize(36, 36), ratio);
    painter.drawPixmap(QPointF((size - 36) / 2, (size - 36) / 2), icon);
    avatar->setPixmap(pixmap);
}

void CallView::tintIcons() {
    Icons::set(answer, "call", colors.onPrimary);
    Icons::set(answerVideo, "videocam", colors.onTonal);
    Icons::set(decline, "call_end", colors.onDanger);
    Icons::set(end, "call_end", colors.onDanger);
    Icons::set(talk, "mic", colors.onPrimary);
    talk->setIconSize(QSize(26, 26));
    Icons::set(pushToTalk, "touch_app", pushToTalk->isChecked() ? colors.onTonal : colors.text);
    Icons::set(camera, "videocam", camera->isChecked() ? colors.onTonal : colors.text);
}

void CallView::render() {
    if (!engine) return;
    const QString state = engine->state();
    bool ringing = state == "ringing";
    bool live = state == "in_call" || state == "broadcasting";
    bool videoCall = engine->peerVideo();
    bool announcement = state == "listening" || state == "broadcasting";
    overline->setText((ringing ? tr("Incoming call")
                       : announcement ? tr("Announcement")
                                      : tr("Intercom call"))
                          .toUpper());
    name->setText(engine->peerName());
    QString what;
    if (state == "calling") what = tr("Calling …");
    else if (ringing) what = videoCall ? tr("Video call") : tr("wants to talk to you");
    else if (state == "connecting") what = tr("Connecting …");
    else if (state == "broadcasting") what = engine->since() ? QString() : tr("Connecting …");
    QString detail = engine->detail();
    status->setText(what.isEmpty() ? detail : detail.isEmpty() ? what : what + "\n" + detail);
    status->setVisible(!status->text().isEmpty());

    bool pictures = engine->cameraOn() || engine->farCamera();
    video->setVisible(pictures);
    avatar->setVisible(!pictures);
    // The picture takes the room left; without one, the buttons follow the card.
    layout->setStretchFactor(card, pictures ? 1 : 0);
    layout->setStretch(layout->count() - 1, pictures ? 0 : 1);
    bool canTalk = live && !engine->handsFree() && engine->microphoneAvailable();
    hint->setVisible(canTalk);
    talk->setVisible(canTalk);
    trust->setVisible(ringing && !engine->callPeer().candidatePin.isEmpty());
    answer->setVisible(ringing);
    answerVideo->setVisible(ringing && videoCall);
    decline->setVisible(ringing);
    pushToTalk->setVisible(state == "in_call" && engine->microphoneAvailable());
    pushToTalk->setChecked(!engine->handsFree());
    camera->setVisible(videoCall && state != "ringing");
    camera->setChecked(engine->cameraOn());
    end->setVisible(!ringing);
    end->setText(state == "broadcasting" ? tr("End announcement") : tr("End"));
    tick();
}

void CallView::tick() {
    if (!engine || !engine->active()) return;
    qint64 since = engine->since();
    if (since > 0) {
        qint64 s = (QDateTime::currentMSecsSinceEpoch() - since) / 1000;
        timer->setText(QString("%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')));
    } else {
        timer->setText("00:00");
    }
    meter->setLevel(engine->level());
    talk->setDown(engine->sending() && !engine->handsFree());
}

void CallView::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() && engine && talk->isVisible()) {
        engine->talk(true);
        return;
    }
    QWidget::keyPressEvent(event);
}

void CallView::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() && engine && talk->isVisible()) {
        if (!engine->handsFree()) engine->talk(false);
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void CallView::showFar(const QImage &image) {
    farImage = image;
    if (image.isNull()) {
        far->setPixmap({});
        far->setText(engine && engine->peerVideo() && !engine->farCamera() ? tr("Camera off")
                                                                           : QString());
        return;
    }
    far->setPixmap(QPixmap::fromImage(
        image.scaled(far->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void CallView::showSelf(const QImage &image) {
    if (image.isNull()) {
        self->hide();
        return;
    }
    QSize box = QSize(160, 160).boundedTo(video->size() / 3);
    // Mirrored, as people expect to see themselves.
    QImage scaled = image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    self->setPixmap(QPixmap::fromImage(scaled.flipped(Qt::Horizontal)));
#else
    self->setPixmap(QPixmap::fromImage(scaled.mirrored(true, false)));
#endif
    self->adjustSize();
    placeSelf();
    self->show();
    self->raise();
}

void CallView::placeSelf() {
    self->move(video->width() - self->width() - 8, video->height() - self->height() - 8);
}

void CallView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    placeSelf();
    if (!farImage.isNull()) showFar(farImage);
}
