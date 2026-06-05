#ifndef MainWindow_h
#define MainWindow_h

#include "app/RecentDatasetsManager.h"

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QTimer>

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
class QLabel;
class QMenu;
class QTabWidget;
class Settings;
class VLKBInventoryTree;
class vtkWindowImage;
struct BackendNode;

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
    /// Resolve a remote URL → download via VisIVO backend → open. Used by
    /// the SKAVA Discovery tab after a DataLink lookup. ``obsId`` is the
    /// SKAVA-side identifier, used as a filename hint in the cache and as
    /// the display name for the Recent Datasets panel.
    void doOpenDataFromUrl(const QString &url, const QString &obsId);
    /// Backend-aware open driven by a full SKAVA DataLink response. When
    /// SKAVA advertises a ``visivo-backend`` service descriptor that
    /// matches a registered backend, route the open through that backend
    /// (compute next to data, no download). When no descriptor matches,
    /// fall back to ``doOpenDataFromUrl`` (download via local backend).
    /// See docs/distributed-backend-design.md §4.2.
    void doOpenSkavaDataset(const QString &obsId,
                              const struct SkavaDatalinkResponse &datalink);
    /// Shared viewer-creation logic used by both ``doOpenDataPath`` (local
    /// path open) and ``doOpenDataFromUrl`` (SKAVA HTTP open). ``path`` is
    /// the *display* identifier — window title, Recent Datasets entry,
    /// SAMP metadata — and may be a SKAVA obs_id rather than a real file
    /// path when the dataset came in through ``openDatasetFromUrl``.
    void openViewerFromOpenedDataset(const QString &path,
                                       const struct BackendOpenDatasetResult &opened,
                                       const class BackendClient &client);
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

    // Backend diagnostics mirror — polls /v1/diagnostics/events every few
    // seconds and re-publishes each new entry into DiagnosticsManager, so
    // the desktop Diagnostics window shows backend log lines next to the
    // client-side ones. ``m_backendDiagSince`` is the high-water-mark.
    QTimer  m_backendDiagPollTimer;
    qint64  m_backendDiagSince{ 0 };
    bool    m_backendDiagPollInFlight{ false };
    void    pollBackendDiagnosticsOnce();

    // Build/refresh the command palette's action set. Called on demand each
    // time ⌘K is pressed so dynamic groups (recent datasets) stay current.
    void rebuildCommandPalette();
    // Q_INVOKABLE so child windows (vtkWindowCube, vtkWindowImage, …) can
    // open the palette via QMetaObject::invokeMethod without linking against
    // MainWindow's class definition.
    Q_INVOKABLE void openCommandPalette();

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

    // ── "Served by" backend badge ─────────────────────────────────────────
    // Permanent widget in the status bar showing which backend is currently
    // active. Updated by ``setActiveBackendBadge()`` whenever a dataset
    // open routes through a remote SRC backend (``Backend: <name>`` in
    // accent colour) or falls back to the local backend (``Backend:
    // Local`` in muted colour).
    QPointer<QLabel> m_backendBadge;
    void setActiveBackendBadge(const BackendNode &node);


};


#endif
