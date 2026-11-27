#ifndef SESSIONDATATREE_H
#define SESSIONDATATREE_H

// SessionDataTree — the left "SESSION DATA" panel of Design 1d (tappa 2). A
// QTreeView over a QStandardItemModel that shows each open dataset as a root and
// every derived product (from ProductRegistry) nested beneath the dataset it came
// from. Double-clicking a product raises its result window. Modelled on
// VLKBInventoryTree's QStandardItemModel usage.

#include "app/SessionRemoval.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class QEvent;
class QMenu;
class QTreeView;
class QStandardItemModel;
class QStandardItem;
class QModelIndex;
struct AnalysisProduct;
namespace visivo::regions {
struct RegionTreeView;
}

class SessionDataTree : public QWidget
{
    Q_OBJECT
public:
    explicit SessionDataTree(QWidget *parent = nullptr);

    /// Bind this tree to its owning viewer window's product scope. The ProductRegistry is
    /// session-wide, so without this a second viewer on the same dataset would show the first's
    /// products (whose render widgets live in the OTHER window). With it, the tree ignores any
    /// product whose ownerId differs. Empty scope (default) = show everything (back-compat).
    void setOwnerScope(const QString &ownerScope) { m_ownerScope = ownerScope; }

    /// (#580) Say what removing a row means for this window. Without a handler
    /// the tree offers no removal. See app/SessionRemoval.h: the tree owns the
    /// gesture and the question, the handler what happens to what the window
    /// holds, so every viewer gets the same Delete key, menu entry, multi-
    /// selection and confirmation.
    void setRemovalHandler(visivo::removal::Handler handler) { m_removal = std::move(handler); }

    /// (#580) The product rows selected now, in tree order (never a dataset
    /// root, a view row or a region).
    QStringList selectedProductIds() const;

    /// (#580) Add "Remove from Session" to a row's context menu: for the
    /// selection when @p productId is part of it, else for @p productId alone.
    /// Does nothing without a handler, or for a row that is not a product.
    void appendRemoveAction(QMenu *menu, const QString &productId);

    /// (#580) Remove these products after the confirmation the plan calls for
    /// (the Delete key and the menu entry end here; a window can call it for
    /// a removal that starts elsewhere, such as its layer list).
    void requestRemoval(const QStringList &productIds);

    /// Ensure a root exists for this dataset (and select it). Safe to call once
    /// per viewer window at startup. @p kind drives the small badge ("CUBE"/…).
    void setCurrentDataset(const QString &parentKey, const QString &label,
                           const QString &kind);

    /// Select the product row with this id (emits productSelected → drives the
    /// Inspector's Provenance). No-op if the id is empty or not present.
    void selectProduct(const QString &productId);

    /// Reverse sync: highlight the row for the pane that just became active WITHOUT
    /// re-triggering activation (guarded), so tree-selection and pane-selection stay in
    /// step whichever the user drove. @p viewTag "view3d"/"slice2d" or @p productId.
    void selectForView(const QString &viewTag, const QString &productId);

private:
    /// True while the code, not the user, moves the selection: such a move must
    /// stay passive (a product registering itself must never steal a pane).
    bool m_selectingProgrammatically = false;

public:

    /// (#4) Which pane(s) currently SHOW each product → small pane-number badges on the
    /// right of the row (a product duplicated in two panes shows two). Products absent
    /// from the map get no badge. Repaints the tree.
    void setProductPaneAssignments(const QHash<QString, QList<int>> &assignments);

    /// (#388) Show the window's region list as a "Regions" node under the
    /// current dataset: one row per region with a visibility check box, the
    /// regions of an import grouped under the file's name. Called after every
    /// change of the list; the node goes when the list is empty. Never emits
    /// the region signals below: the list is the source, the tree its view.
    void setRegions(const visivo::regions::RegionTreeView &view);

signals:
    /// (#388) The user selected a region row: select that region on the image.
    void regionSelected(int regionId);
    /// (#388) The user ticked or cleared a check box: of a region, of an
    /// import's group, or of the Regions node (every region).
    void regionVisibilityRequested(const QList<int> &regionIds, bool visible);
    /// (#388) Double-click on a region row: its properties.
    void regionActivated(int regionId);
    /// (#388) Right-click on a region row.
    void regionContextMenuRequested(int regionId, const QPoint &globalPos);
    /// (#388) Right-click on an import's group (@p batch > 0) or on the
    /// Regions node (@p batch -1: every region).
    void regionGroupContextMenuRequested(int batch, const QPoint &globalPos);

    /// Emitted when the selection lands on a product row (empty string when the
    /// selection is a dataset root or cleared).
    void productSelected(const QString &productId);

    /// Emitted when the USER moves the selection (not on a programmatic one), so the
    /// viewer can make the pane already showing that view the active one. Unlike
    /// rowActivated this must never mount anything or move panes around: selecting a
    /// row is a statement about what you are looking at, not a request to rearrange.
    /// It exists because the left dock's DISPLAY section follows the ACTIVE PANE, so
    /// a row highlighted in the tree while another pane stayed active meant the
    /// colour map you then changed was applied to a different view than the one
    /// selected (user report).
    void rowSelected(const QString &viewTag, const QString &productId);

    /// Emitted when a row is ACTIVATED (double-click / Enter, or the Show-in menu)
    /// so the viewer can make the matching pane the active one, mounting the view
    /// if no pane shows it yet. @p viewTag is "view3d"/"slice2d" for the intrinsic
    /// view rows; @p productId is set for a derived-product row. Both empty for a
    /// dataset root. Plain selection goes through rowSelected above, which never
    /// mounts (§ tree-selection = pane-selection).
    void rowActivated(const QString &viewTag, const QString &productId);

    /// (#4) A pane-number badge was clicked → the viewer activates that pane (§5.1).
    void paneBadgeActivated(int paneNumber);

    /// Right-click on a product row → the viewer builds the "Show in ▸ Pane N / New pane"
    /// menu at @p globalPos (it knows the pane state + renderability).
    /// Right-click on any actionable row: an intrinsic view (@p viewTag
    /// "view3d"/"slice2d", @p productId empty) or a product (@p productId set).
    /// The dataset root emits nothing. Both are offered a "Show in ▸" menu — a
    /// view is as mountable as a product, and used to be the one row you could
    /// not place from here.
    void rowContextMenuRequested(const QString &viewTag, const QString &productId,
                                 const QPoint &globalPos);

    /// Double-click on a product that has NO window to raise (its tool window was
    /// closed, or it never had one) and no pane renderer. The viewer decides what
    /// that means — for a product stored as a backend FITS it means reopening it.
    /// Without this the row was simply inert (user report).
    void productReopenRequested(const QString &productId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;   // badge click on the viewport

private slots:
    void onProductAdded(const AnalysisProduct &product);
    void onProductUpdated(const AnalysisProduct &product);
    void onProductRemoved(const QString &productId);
    void onActivated(const QModelIndex &index);

private:
    /// True when @p product belongs to this tree's owner window (or the tree is unscoped).
    bool ownsProduct(const AnalysisProduct &product) const;
    QStandardItem *datasetItem(const QString &parentKey, const QString &label,
                               const QString &kind, bool createIfMissing);
    void upsertProduct(const AnalysisProduct &product);
    /// Size the tree to exactly its visible rows (no dead space above LAYERS),
    /// growing as products are added. Called after every content/expansion change.
    void updateHeightToContents();
    int visibleRowHeight(const QModelIndex &parent) const;

    /// (#388) The Regions node under the current dataset; created when asked.
    QStandardItem *regionsNode(bool createIfMissing);
    QStandardItem *regionItem(int regionId) const;
    /// The ids of the regions at or below @p item.
    QList<int> regionIdsUnder(const QStandardItem *item) const;
    /// A check box the user changed: sent to the window, after this event.
    void onRegionCheckChanged(QStandardItem *item);

    QTreeView *m_tree = nullptr;
    QString m_currentDatasetKey;       ///< the root the Regions node hangs under
    /// True while setRegions() writes the region rows: their check boxes and
    /// the selection it moves are the list's state, not a user's gesture.
    bool m_updatingRegions = false;
    QString m_regionSignature;         ///< rows shown now (RegionTreeView::signature)
    int m_lastRegionSelected = -1;     ///< the list's selection setRegions() last showed
    QStandardItemModel *m_model = nullptr;
    QString m_ownerScope;              ///< this tree's window scope; empty = unscoped (show all)
    visivo::removal::Handler m_removal;   ///< (#580) what the window does on removal
    bool m_removing = false;              ///< (#580) a removal (and its question) is under way
};

#endif // SESSIONDATATREE_H
