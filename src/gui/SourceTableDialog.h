#ifndef SourceTableDialog_h
#define SourceTableDialog_h

// SourceTableDialog.h
// VLKB cutouts for a list of sources, from a tab-separated table.
//
// Ported from the legacy client's UserTableWindow (ViaLactea Visual Analytics).
// Three tabs, as there:
//   * Sources — open the table, choose the identifier, l and b columns and the
//     region around each source (a cone or a box), tick the sources, Query;
//   * Images, Cubes — the preview: for every source, how many datasets the
//     archive has and, per survey, whether it covers the source fully (green),
//     partly (yellow) or not at all (red); the species/transitions to request
//     are ticked on the left.
// "Request cutouts" then sends the best dataset of every ticked product for
// every ticked source as ONE asynchronous archive job (McutoutJobDialog).
//
// The logic is in vlkb/SourceTable.h; the per-source search is the same
// VlkbQueryService the VLKB panel uses.

#include "app/BackendClient.h"
#include "vlkb/SourceTable.h"

#include <QDialog>
#include <QList>
#include <QPointer>

class AuthWrapper;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QDragEnterEvent;
class QDropEvent;
class QProgressBar;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class VlkbQueryService;

class SourceTableDialog : public QDialog
{
    Q_OBJECT

public:
    SourceTableDialog(const QString &vlkbUrl, AuthWrapper *auth, QWidget *parent = nullptr);
    ~SourceTableDialog() override;

    /// Load a table from @p path (the Open button calls this too).
    bool loadFile(const QString &path);

signals:
    /// The user asked for the cutouts: one item per (source, product), with a
    /// label for each, ready for McutoutJobDialog.
    void cutoutsRequested(const QVector<BackendMcutoutItem> &items, const QStringList &labels);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void openFile();
    void startQuery();
    void cancelQuery();
    void requestCutouts();

private:
    struct QueriedSource
    {
        visivo::vlkb::UserSource source;
        QString pos;                                  ///< the region queried, sent to SODA
        QList<visivo::vlkb::InventoryEntry> entries;  ///< what the archive answered
        QString error;                                ///< non-empty if the search failed
    };

    void buildUi();
    void showTable();
    void updateRegionControls();
    QString regionFor(const visivo::vlkb::UserSource &s) const;
    void queryNext();
    void finishQuery();
    void buildPreview(const QString &type, QTableWidget *table, QTreeWidget *productTree,
                      QList<QPair<visivo::vlkb::ProductKey, QTreeWidgetItem *>> &productChecks);
    void setBusy(bool busy);
    /// The (item, label) pairs the current ticks would request.
    void collectRequest(QVector<BackendMcutoutItem> &items, QStringList &labels) const;
    /// Keep "Request N cutouts…" in step with the ticks.
    void updateRequestCount();

    QString m_vlkbUrl;
    AuthWrapper *m_auth{ nullptr };   ///< not owned
    visivo::vlkb::SourceTable m_table;
    QList<visivo::vlkb::UserSource> m_pending;
    QList<QueriedSource> m_results;
    int m_total{ 0 };
    QPointer<VlkbQueryService> m_service;
    QPointer<QTimer> m_watchdog;

    QPointer<QTabWidget> m_tabs;
    QPointer<QPushButton> m_openButton;
    QPointer<QLabel> m_fileLabel;
    QPointer<QComboBox> m_idBox;
    QPointer<QComboBox> m_lonBox;
    QPointer<QComboBox> m_latBox;
    QPointer<QComboBox> m_selectionBox;
    QPointer<QStackedWidget> m_regionStack;
    QPointer<QDoubleSpinBox> m_radius;
    QPointer<QDoubleSpinBox> m_dl;
    QPointer<QDoubleSpinBox> m_db;
    QPointer<QPushButton> m_queryButton;
    QPointer<QPushButton> m_cancelButton;
    QPointer<QLabel> m_status;
    QPointer<QProgressBar> m_progress;
    QPointer<QTableWidget> m_sourcesTable;
    QPointer<QTableWidget> m_imagesTable;
    QPointer<QTableWidget> m_cubesTable;
    QPointer<QTreeWidget> m_imageProducts;
    QPointer<QTreeWidget> m_cubeProducts;
    QPointer<QPushButton> m_requestButton;
    QList<QPair<visivo::vlkb::ProductKey, QTreeWidgetItem *>> m_imageChecks;
    QList<QPair<visivo::vlkb::ProductKey, QTreeWidgetItem *>> m_cubeChecks;
};

#endif   // SourceTableDialog_h
