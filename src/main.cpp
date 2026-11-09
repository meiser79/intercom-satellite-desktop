#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QLockFile>
#include <QStandardPaths>
#include <QTranslator>

#include <cstdio>

extern "C" {
#include <libavutil/log.h>
}

#include "Config.h"
#include "MainWindow.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    av_log_set_level(AV_LOG_ERROR);
    QApplication::setApplicationName("intercom-satellite");
    QApplication::setApplicationDisplayName("Intercom Satellite");
    QApplication::setOrganizationName("meiser79");
    QApplication::setApplicationVersion(APP_VERSION);
    QApplication::setDesktopFileName("intercom-satellite");
    // The tray keeps the intercom reachable with the window closed.
    QApplication::setQuitOnLastWindowClosed(false);

    QTranslator qt, own;
    if (qt.load(QLocale(), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qt);
    if (own.load(QLocale(), "intercom", "_", ":/i18n")) app.installTranslator(&own);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QApplication::translate("main", "Intercom for Kiosk Satellite kiosks and phones."));
    QCommandLineOption help = parser.addHelpOption();
    QCommandLineOption version = parser.addVersionOption();
    QCommandLineOption config("config", QApplication::translate("main", "Settings file to use."),
                              "file");
    QCommandLineOption noMdns("no-mdns",
                              QApplication::translate("main", "Don't announce or look for kiosks."));
    QCommandLineOption call("call",
                            QApplication::translate("main", "Call a kiosk once it is reachable."),
                            "address");
    QCommandLineOption video("video",
                             QApplication::translate("main", "With --call or --auto-answer: video."));
    QCommandLineOption autoAnswer("auto-answer",
                                  QApplication::translate("main", "Answer calls after a second."));
    parser.addOptions({config, noMdns, call, video, autoAnswer});
    // Answered here, not by process(): its exit() leaves Qt's threads running and they complain.
    if (!parser.parse(app.arguments())) {
        std::fprintf(stderr, "%s\n", qPrintable(parser.errorText()));
        return 1;
    }
    if (parser.isSet(help) || parser.isSet("help-all")) {
        std::fputs(qPrintable(parser.helpText()), stdout);
        return 0;
    }
    if (parser.isSet(version)) {
        std::printf("%s %s\n", qPrintable(QApplication::applicationName()),
                    qPrintable(QApplication::applicationVersion()));
        return 0;
    }

    Config settings(parser.value(config));
    // One app per settings file: a second start brings the running one's window up and ends. The
    // lock decides who runs, the local socket passes the request on.
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    QString name = "intercom-satellite-" +
                   QCryptographicHash::hash(settings.fileName().toUtf8(), QCryptographicHash::Sha1)
                       .toHex()
                       .left(16);
    QLockFile lock(base + "/" + name + ".lock");
    if (!lock.tryLock(0)) {
        QLocalSocket other;
        other.connectToServer(name);
        if (other.waitForConnected(2000)) {
            other.write("show\n");
            other.waitForBytesWritten(2000);
        }
        std::fprintf(stderr, "%s\n",
                     qPrintable(QApplication::translate("main", "Intercom Satellite is already running.")));
        return 0;
    }
    QLocalServer::removeServer(name);  // left behind by a crash
    QLocalServer instance;
    instance.setSocketOptions(QLocalServer::UserAccessOption);
    instance.listen(name);

    MainWindow::Options options;
    options.mdns = !parser.isSet(noMdns);
    options.call = parser.value(call);
    options.video = parser.isSet(video);
    options.autoAnswer = parser.isSet(autoAnswer);
    MainWindow window(settings, options);
    window.present();
    QObject::connect(&instance, &QLocalServer::newConnection, &window, [&] {
        while (QLocalSocket *other = instance.nextPendingConnection()) {
            QObject::connect(other, &QLocalSocket::disconnected, other, &QObject::deleteLater);
            window.bringToFront();
        }
    });
    return app.exec();
}
