#ifndef DataHubWidget_h
#define DataHubWidget_h

#include "app/RecentDatasetsManager.h"

#include <QWidget>
#include <QPointer>

class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;

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

    // Rebuilds the Recent Datasets panel from the current manager state.
    // Called automatically when manager emits changed().
    void refreshRecentDatasets();

signals:
    void openDataRequested();
    void openCatalogueRequested();
    void openVbtRequested();
    void openHiPSRequested();

    // Emitted when the user clicks a recent dataset item.
    // MainWindow handles this by reopening the dataset using the stored path.
    void reopenDatasetRequested(const RecentDataset &dataset);

private:
    // ── Builder helpers ───────────────────────────────────────────────────────
    void buildUi();
    QFrame *buildBackendCard();
    QFrame *buildActionGrid();
    QFrame *buildRecentDatasetsPanel();
    QFrame *buildSystemInsightsPanel();

    // Creates a single interactive action card (icon symbol + title + desc).
    // Installs an event filter so the frame emits the given signal on click.
QFrame *makeActionCard(const QString &iconKind, const QString &title,
                       const QString &desc, QWidget *parent = nullptr);
                       
    QString backendUrl;
    QString backendToken;

    // Backend card widgets
    QPointer<QLabel>       labelBackendUrl;
    QPointer<QLabel>       labelStatusBadge;
    QPointer<QLabel>       labelStatusDetail;
    QPointer<QPushButton>  btnCheckConnection;

    // System insights panel
    QPointer<QLabel> labelWorkers;
    QPointer<QLabel> labelSessions;
    QPointer<QLabel> labelTaskRegistry;

    // Recent datasets — manager pointer (not owned), layout for dynamic items.
    RecentDatasetsManager *recentMgr{ nullptr };
    QPointer<QVBoxLayout>  recentItemsLayout;
};

#endif // DataHubWidget_h
