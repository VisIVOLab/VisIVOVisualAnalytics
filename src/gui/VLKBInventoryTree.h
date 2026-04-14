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
class QModelIndex;
class QTreeView;

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

signals:
    /// Emitted when the user double-clicks a leaf item.
    /// @param ivoID       obs_publisher_did of the selected cutout.
    /// @param posCutout   POS string to pass to soda/sync.
    /// @param displayName Human-readable label (survey + band/transition).
    /// @param type        "image" or "cube".
    void cutoutRequested(const QString &ivoID, const QString &posCutout,
                         const QString &displayName, const QString &type);

private slots:
    void onDoubleClicked(const QModelIndex &idx);

private:
    struct WavelengthGroup { QString name; double lambdaMin; double lambdaMax; };

    void buildUi(const QString &posCutout);
    void setupWavelengthGroups();
    void setupTopLevels();
    void parseVOTable(const QByteArray &votable);
    void applyStyleHints(const QModelIndex &parent);
    int  groupIndexOf(double lambda) const;

    QTreeView *m_tree { nullptr };
    QList<WavelengthGroup>       m_groups;
    QHash<QUuid, Cutout>         m_cutouts;
    QString                      m_posCutout;
    QPointer<QMainWindow>        m_linkedImageWindow; ///< actual type: vtkWindowImage
};
