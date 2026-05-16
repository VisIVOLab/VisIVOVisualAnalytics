#ifndef MainWindow_h
#define MainWindow_h

#include "app/RecentDatasetsManager.h"

#include <QMainWindow>
#include <QPointer>
#include <QString>

#include <memory>

class AuthWrapper;
class BackendClient;
class CommandPalette;
class DataHubWidget;
class DiagnosticsWindow;
class HiPSWindow;
class SampClient;
class SampMessageHandler;
class QAction;
class QMenu;
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
    void buildSampMenu();
    void buildScienceMenu();
    void syncDataHubSettings();
    void syncSampSettings();
    void updateSampActions();
    void handleSampFitsDataset(const QString &backendPath, const QString &mtype);
    void handleSampTableLoad(const QString &url, const QString &localPath);
    void handleSampSkyCoord(double raDeg, double decDeg);
    void sendCurrentFitsViaSamp();
    void sendCurrentCatalogueViaSamp();

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
    QPointer<HiPSWindow>             m_lastHiPSWindow;
    QPointer<QTabWidget>             m_tabs;
    QPointer<RecentDatasetsManager>  recentMgr;
    // VLKB tab is closeable but the widget itself is preserved across
    // close/reopen so the inventory state is not lost. Held here so we can
    // distinguish it from genuinely-disposable tabs in the close handler.
    QPointer<QWidget>                m_vlkbPanel;
    QPointer<CommandPalette>         m_commandPalette;

    // Build/refresh the command palette's action set. Called on demand each
    // time ⌘K is pressed so dynamic groups (recent datasets) stay current.
    void rebuildCommandPalette();
    void openCommandPalette();

    // Brings the VLKB panel to focus. Re-adds the tab if the user previously
    // closed it. Used by both the Data Hub Quick action and the command
    // palette entry.
    void revealVLKBTab();
    std::unique_ptr<BackendClient>   m_sampBackendClient;
    QPointer<SampClient>             m_sampClient;
    QPointer<SampMessageHandler>     m_sampHandler;

    // Science menu — tracks the most recently opened backend dataset.
    QString  m_sciBackendUrl;
    QString  m_sciSessionId;
    QString  m_sciToken;
    QString  m_sciDatasetId;
    int      m_sciDepth{ 1 };
    double   m_sciThreshold{ 0.0 };
    QString  m_sciSpectralAxisType;  ///< Backend CTYPE3, e.g. "VRAD", "FREQ".
    QString  m_sciSpectralAxisUnit;  ///< Backend CUNIT3, e.g. "km/s", "GHz".

    QPointer<QMenu>   m_scienceMenu;
    QPointer<QAction> m_actLinewidth;
    QPointer<QAction> m_actBaseline;
    QPointer<QAction> m_actStack;
    QPointer<QAction> m_actSourceFind;
    QPointer<QMenu>   m_sampMenu;
    QPointer<QAction> m_actSampConnect;
    QPointer<QAction> m_actSampDisconnect;
    QPointer<QAction> m_actSampSendCurrentFits;
    QPointer<QAction> m_actSampSendCurrentCatalogue;
    QString           m_sampCurrentFitsPath;
    QString           m_sampCurrentFitsName;
    QString           m_sampCurrentCataloguePath;
    QString           m_sampCurrentCatalogueName;
    QString           m_sampCurrentCatalogueFormat;

    void openDiagnosticsWindow();


};


#endif
