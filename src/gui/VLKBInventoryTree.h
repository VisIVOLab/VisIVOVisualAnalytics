#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QUuid>
#include <QWidget>

class vtkWindowImage;

// ---------------------------------------------------------------------------
// Cutout — one row returned by the VLKB SIAv2 inventory query.
// ---------------------------------------------------------------------------
struct Cutout
{
    QString type;         ///< "image" | "cube"
    QString survey;       ///< obs_collection, e.g. "Hi-GAL Tiles"
    QString species;      ///< first part of obs_title, e.g. "Continuum"
    QString transition;   ///< second part of obs_title, e.g. "500 um"
    QString pubDID;       ///< bib_reference (used as tooltip)
    QString ivoID;        ///< obs_publisher_did — key for SODA cutout request
    double  spectrum_min  { 0. };
    double  spectrum_max  { 0. };
    int     overlap       { 4 };   ///< 3 = full overlap (green highlight)
    QString region_gal;   ///< s_region_galactic (space-separated polygon)
    QString pos_cutout;   ///< POS string passed to SODA, set by caller
};

// ---------------------------------------------------------------------------
// VLKBInventoryTree
//
// Displays the VOTable returned by the SIAv2 inventory search as a tree
// grouped by wavelength regime and data product type.
//
// When the user double-clicks a leaf item the widget emits
//   cutoutRequested(ivoID, pos)
// and the caller (MainWindow) is responsible for triggering the cutout
// download and opening the resulting FITS file.
// ---------------------------------------------------------------------------
class QStandardItem;
class QStandardItemModel;
class QModelIndex;
class QTreeView;
class QLineEdit;
class QLabel;
class QPushButton;
class QCheckBox;

class VLKBInventoryTree : public QWidget
{
    Q_OBJECT
public:
    /// @param votable   Raw VOTable bytes from siav2/query.
    /// @param posCutout POS string to pass verbatim to soda/sync (CIRCLE … or RANGE …).
    explicit VLKBInventoryTree(const QByteArray &votable,
                                const QString    &posCutout,
                                QWidget          *parent = nullptr);
    ~VLKBInventoryTree() override = default;

    /// Returns the vtkWindowImage that was opened from this inventory session,
    /// or nullptr if it has never been set or has since been closed.
    vtkWindowImage *linkedImageWindow() const;

    /// Called by MainWindow after it creates a new vtkWindowImage for this session.
    void setLinkedImageWindow(vtkWindowImage *win);

public slots:
    /// Called by whoever performs the fetch, so the dialog can stop saying it
    /// is waiting. @p ok false keeps the failed dataset's name visible.
    void noteCutoutFinished(const QString &displayName, bool ok);

signals:
    /// Emitted when the user double-clicks a leaf item.
    /// @param ivoID       obs_publisher_did of the selected cutout.
    /// @param posCutout   POS string to pass to soda/sync.
    /// @param displayName Human-readable label (survey + band/transition).
    /// @param type        "image" or "cube".
    void cutoutRequested(const QString &ivoID, const QString &posCutout,
                         const QString &displayName, const QString &type);

    /// The user asked for the selection as ONE asynchronous archive job rather
    /// than N synchronous cutouts. Parallel lists, one entry per dataset.
    void batchRequested(const QStringList &ivoIDs, const QString &posCutout,
                        const QStringList &displayNames);

private slots:
    void onDoubleClicked(const QModelIndex &idx);
    void onSelectionChanged();
    void downloadSelected();
    void requestBatch();
    void applyFilter(const QString &text);
    /// Re-run the current filter. Bound to the coverage checkbox.
    void reapplyFilter();

private:
    /// Hide the datasets that only partly overlap the queried region.
    void hidePartialCoverage();
    /// Note that a cutout request has just gone out, and refresh the status.
    void noteCutoutStarted(const QString &displayName);
    void updateStatus();

private slots:

private:
    struct WavelengthGroup { QString name; double lambdaMin; double lambdaMax; };

    void buildUi(const QString &posCutout);
    void setupWavelengthGroups();
    void setupTopLevels();
    void parseVOTable(const QByteArray &votable);
    void applyStyleHints(const QModelIndex &parent);
    void annotateCounts();                       ///< append "(N)" to branch nodes, grey empties
    int  countAndTag(QStandardItem *node);        ///< recursive leaf count + tagging
    bool filterNode(QStandardItem *node, const QString &needle);  ///< recursive hide/show
    void requestCutout(const QModelIndex &idx);
    /// Let category rows use the full width instead of being elided in column 0.
    void spanGroupRows();   ///< shared by double-click + button
    int  groupIndexOf(double lambda) const;

    QTreeView          *m_tree        { nullptr };
    QStandardItemModel *m_model       { nullptr };
    QLineEdit          *m_filter      { nullptr };
    QLabel             *m_totalLabel  { nullptr };
    QCheckBox          *m_coverageOnly{ nullptr };
    QPushButton        *m_openButton  { nullptr };
    QPushButton        *m_batchButton { nullptr };
    QLabel             *m_status      { nullptr };  ///< "Requesting …" while fetches are out
    int                 m_pending     { 0 };        ///< cutout requests still in flight
    bool                m_busyCursorSet { false };
    QString             m_lastRequested;
    QString             m_lastFailed;
    QList<WavelengthGroup>       m_groups;
    QHash<QUuid, Cutout>         m_cutouts;
    QString                      m_posCutout;
    QPointer<QMainWindow>        m_linkedImageWindow; ///< actual type: vtkWindowImage
};
