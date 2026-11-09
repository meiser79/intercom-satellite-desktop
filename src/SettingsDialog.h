#pragma once

#include <QDialog>

class Config;
class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;
class Switch;

/** Name, key and how calls behave; the intercom restarts when name, key, port or TLS change. */
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(Config &config, QWidget *parent = nullptr);
    /** Saves; true when the intercom must restart for the change. */
    bool save();

private:
    void validate();

    Config &config;
    QLineEdit *name, *key;
    QSpinBox *port;
    Switch *tls, *video, *announcements, *pushToTalk, *startHidden;
    QComboBox *camera;
    QDialogButtonBox *buttons;
};
