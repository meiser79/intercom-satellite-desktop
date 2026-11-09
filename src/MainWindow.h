#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QSystemTrayIcon>

#include <memory>

#include "Config.h"
#include "Theme.h"

class CallView;
class Engine;
class HistoryDialog;
class QGridLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QToolButton;
class Ringer;
class Switch;

/**
 * The kiosks and what to do with them, and the call screen while a call is on. Closing the
 * window keeps the intercom running in the system tray.
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    struct Options {
        bool mdns = true;
        QString call;        // an address to call once it is reachable
        bool video = false;  // that call with the camera on
        bool autoAnswer = false;
    };

    MainWindow(Config &config, Options options);
    ~MainWindow() override;
    /** Shows the window unless set to start in the tray. */
    void present();
    /** Shows the window over the others, also from the tray. */
    void bringToFront();

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void startEngine();
    void render();
    void renderKiosks();
    /** The Android app's colors, light or dark after the system theme. */
    void applyTheme();
    /** The kiosk's room symbol in a circle, with its status badge. */
    QPixmap avatar(const Peer &p) const;
    void showInfo(const Peer &p);
    void showSettings();
    /** The tray menu after the intercom's state: its call's buttons. */
    void renderTray();
    /** The call history, beside the window; raised when already open. */
    void showHistory();
    /** How many kiosk tiles fit side by side. */
    int kioskColumns() const;
    void addKiosk();
    /** The kiosk's name and symbol in this app, as the Android app's Edit; no name: its own. */
    void editKiosk(const Peer &p);
    /** Shows a certificate's fingerprint; true when the user trusts it. */
    bool confirmCertificate(const Peer &p);
    void incoming(const QString &name, bool video);
    void quit();
    QString fingerprint() const;

    Config &config;
    Options options;
    std::unique_ptr<Engine> engine;
    QString startProblem, lastFingerprint;
    int shownColumns = 0;  // of the kiosk tiles
    bool quitting = false;

    QStackedWidget *pages;
    QWidget *home;
    CallView *callView;
    QLabel *statusTitle, *statusText, *notice, *empty;
    Switch *dnd;
    QGridLayout *kiosks;
    QPushButton *announce, *add;
    QToolButton *refresh, *power, *historyButton, *settings;
    QPointer<HistoryDialog> history;
    Theme::Colors colors;
    bool theming = false, themePending = false;
    QSystemTrayIcon *tray = nullptr;
    QAction *trayDnd = nullptr, *trayCallSeparator = nullptr, *trayAnswer = nullptr,
            *trayDecline = nullptr, *trayEnd = nullptr,
            *trayAnnounce = nullptr;
    Ringer *ringer;
};
