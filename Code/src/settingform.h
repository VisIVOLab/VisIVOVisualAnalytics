#ifndef SETTINGFORM_H
#define SETTINGFORM_H

#include "authwrapper.h"

#include <QWidget>

namespace Ui {
class SettingForm;
}

class SettingForm : public QWidget
{
    Q_OBJECT

public:
    explicit SettingForm(QWidget *parent = 0);
    ~SettingForm();
    void readSettingsFromFile();

private slots:
    void on_TilePushButton_clicked();
    void on_checkBox_clicked(bool checked);
    void on_workdirButton_clicked();
    void on_pushButton_clicked();
    void on_OkPushButton_clicked();
    void on_publicVLKB_radioButton_toggled(bool checked);
    void on_privateVLKB_radioButton_toggled(bool checked);
    void on_neaniasVLKB_radioButton_toggled(bool checked);
    void on_vlkbLoginButton_clicked();
    void on_vlkbLogoutButton_clicked();
    void on_caesarLoginButton_clicked();
    void on_caesarLogoutButton_clicked();
    void vlkb_loggedin();
    void vlkb_loggedout();
    void caesar_loggedin();
    void caesar_loggedout();

private:
    Ui::SettingForm *ui;
    QString m_sSettingsFile;
    AuthWrapper *m_vlkbAuth;
    AuthWrapper *m_caesarAuth;
    bool m_termsAccepted = false;
    const QString m_privacyPolicyUrl = "https://thematic.neanias.eu/SPACE/ViaLactea/privacy.html";
    const QString m_termsOfUseUrl = "https://thematic.neanias.eu/SPACE/ViaLactea/terms.html";
};

#endif // SETTINGFORM_H
