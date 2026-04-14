#ifndef MainWindow_h
#define MainWindow_h

#include "app/RecentDatasetsManager.h"

#include <QMainWindow>
#include <QPointer>

#include <memory>

class AuthWrapper;
class DataHubWidget;
class DiagnosticsWindow;
class HiPSWindow;
class QTabWidget;
class Settings;
class VLKBInventoryTree;
class vtkWindowImage;

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
    void reopenDataset(const RecentDataset &dataset);
    void queryVlkb();   ///< Triggered by btnQuery in the VLKB panel

private:
    void buildDataMenu();
    void syncDataHubSettings();

    // Path-based open helpers (skip the file browser dialog, used for reopen).
    void doOpenDataPath(const QString &path);
    void doOpenCataloguePath(const QString &path);
    void doOpenVbtPath(const QString &path);

    /// Open a VLKB 2D cutout (backend path) in a new vtkWindowImage, label its
    /// master layer with @p displayName, and associate the window with @p tree
    /// so subsequent 2D items from the same inventory session are added as layers.
    void openVlkbImageLayer(const QString &backendPath, const QString &displayName,
                             VLKBInventoryTree *tree);

    Ui::MainWindow *ui;
    QPointer<Settings>               settings;
    QPointer<AuthWrapper>            auth;
    QPointer<DiagnosticsWindow>      diagnosticsWindow;
    QPointer<DataHubWidget>          dataHub;
    QPointer<QTabWidget>             m_tabs;
    QPointer<RecentDatasetsManager>  recentMgr;

    void openDiagnosticsWindow();


};


#endif
