#ifndef DataHubWidget_h
#define DataHubWidget_h

#include "app/RecentDatasetsManager.h"

#include <QWidget>
#include <QPointer>

class QFrame;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;
struct BackendHealthResult;

// ---------------------------------------------------------------------------
// DataHubWidget  —  Digital Observatory landing page
//
// Sections (matching code.html reference):
//   1. Header title + subtitle
//   2. Operational Backend card  (URL, status badge, Check Connection)
//   3. Primary Operations grid   (4 action cards)
//   4. Recent Datasets panel     + System Insights panel
//   5. Footer note
//
// All business logic lives in MainWindow; this widget emits signals only.
// Backend status is updated via setBackendUrl() + refreshStatus().
// ---------------------------------------------------------------------------
class DataHubWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DataHubWidget(QWidget *parent = nullptr);

    // Called by MainWindow whenever settings change.
    void setBackendUrl(const QString &url, const QString &token);

    // Called by MainWindow after construction to wire the recent-datasets panel.
    // The manager must outlive DataHubWidget. DataHubWidget does NOT take ownership.
    void setRecentDatasetsManager(RecentDatasetsManager *manager);

public slots:
    // Synchronous health-check → updates status badge + system-insights panel.
    void refreshStatus();
    /// Re-fetch the Workspace Exports list from the backend asynchronously
    /// and rebuild the panel rows. Public so cube viewers can request an
    /// immediate refresh after pushing a new export, bypassing the 4-s
    /// background poll.
    void refreshWorkspaceExports();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    // Rebuilds the Recent Datasets panel from the current manager state.
    // Called automatically when manager emits changed().
    void refreshRecentDatasets();

signals:
    void openDataRequested();
    void openCatalogueRequested();
    void openVbtRequested();
    void openHiPSRequested();
    /// Switch to the existing VLKB tab in MainWindow.
    void openVLKBRequested();

    /// User clicked the global search bar — host should open the command palette.
    void commandPaletteRequested();

    // Emitted when the user clicks a recent dataset item.
    // MainWindow handles this by reopening the dataset using the stored path.
    void reopenDatasetRequested(const RecentDataset &dataset);

    // Emitted when the user clicks "Open in VisIVO" in the Workspace
    // Exports panel; the host opens @p absolutePath as a new dataset.
    void openWorkspaceExportRequested(const QString &absolutePath);

private:
    // ── Builder helpers ───────────────────────────────────────────────────────
    void buildUi();
    QFrame *buildBackendCard();
    QFrame *buildActionGrid();
    QFrame *buildRecentDatasetsPanel();
    QFrame *buildSystemInsightsPanel();
    QFrame *buildStatusBar();

    // ── "Mission Control" Data Hub (new design) helpers ─────────────────────
    QFrame *buildSearchBar();             // global ⌘K search bar at top
    QFrame *buildQuickActionsList();      // compact vertical list (replaces 4-card grid)
    QFrame *buildLast5JobsPanel();        // recent-tasks feed extracted from old insights
    QFrame *buildWorkspaceExportsPanel(); // persistent FITS exports from cube viewers
    // refreshWorkspaceExports() is now a public slot (see above) so external
    // signals (vtkWindowCube::workspaceExportsChanged) can hit it directly.

    // Creates a single interactive action card (icon symbol + title + desc).
    // Installs an event filter so the frame emits the given signal on click.
QFrame *makeActionCard(const QString &iconKind, const QString &title,
                       const QString &desc, QWidget *parent = nullptr);

    // System Insights live-activity helpers.
    void updateSystemInsights(const BackendHealthResult &health);
    void clearRecentTasksUi();

    // Tear down + rebuild the entire hub UI when ThemeManager reports a
    // theme switch — needed because most widgets cache theme hex values
    // in inline stylesheets at construction time.
    void rebuildOnThemeChange();

    QString backendUrl;
    QString backendToken;

    // Backend card widgets
    QPointer<QLabel>       labelBackendUrl;
    QPointer<QLabel>       labelStatusBadge;
    QPointer<QLabel>       labelStatusDetail;
    QPointer<QPushButton>  btnCheckConnection;

    // System insights panel — live activity readouts.
    QPointer<QLabel>   labelRunningTasks;
    QPointer<QLabel>   labelRunningCaption;
    QPointer<QLabel>   labelRecentHeader;
    QPointer<QWidget>  recentTasksContainer;
    QPointer<QLabel>   labelRecentEmpty;
    QPointer<QLabel>   labelCapacity;

    // Bottom status bar (Layout B): health dot + workers + session + sanity.
    QPointer<QLabel>   statusBarHealth;
    QPointer<QLabel>   statusBarWorkers;
    QPointer<QLabel>   statusBarSession;
    QPointer<QLabel>   statusBarSanity;

    // Periodic refresh of System Insights while the widget is visible.
    QPointer<QTimer> insightsTimer;

    // Recent datasets — manager pointer (not owned), layout for dynamic items.
    RecentDatasetsManager *recentMgr{ nullptr };
    QPointer<QVBoxLayout>  recentItemsLayout;

    // Workspace Exports — populated by refreshWorkspaceExports().
    QPointer<QFrame>      m_exportsCard;
    QPointer<QVBoxLayout> m_exportsItemsLayout;
    QPointer<QLabel>      m_exportsCountLabel;
    QPointer<QLabel>      m_exportsEmptyLabel;
};

#endif // DataHubWidget_h
