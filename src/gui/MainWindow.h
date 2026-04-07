#ifndef MainWindow_h
#define MainWindow_h

#include <QMainWindow>
#include <QPointer>

#include <memory>

class AuthWrapper;
class DataHubWidget;
class DiagnosticsWindow;
class Settings;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void setApplicationTheme();
    void openData();
    void openCatalogue3D();
    void openVbt();
    void openHiPS();
    void openSettingsDialog();
    void openAboutDialog();
    void loadPanoramicView();
    void changeViewSelectionMode();
    void skyRegionSelected(const QString &point, const QString &area);

private:
    void buildDataMenu();
    void syncDataHubSettings();

    Ui::MainWindow *ui;
    QPointer<Settings>        settings;
    QPointer<AuthWrapper>     auth;
    QPointer<DiagnosticsWindow> diagnosticsWindow;
    QPointer<DataHubWidget>   dataHub;

    void openDiagnosticsWindow();
};

#endif
