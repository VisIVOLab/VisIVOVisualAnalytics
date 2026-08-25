#ifndef SESSIONDATATREE_H
#define SESSIONDATATREE_H

// SessionDataTree — the left "SESSION DATA" panel of Design 1d (tappa 2). A
// QTreeView over a QStandardItemModel that shows each open dataset as a root and
// every derived product (from ProductRegistry) nested beneath the dataset it came
// from. Double-clicking a product raises its result window. Modelled on
// VLKBInventoryTree's QStandardItemModel usage.

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class QEvent;
class QTreeView;
class QStandardItemModel;
class QStandardItem;
class QModelIndex;
struct AnalysisProduct;

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

    /// (#4) Which pane(s) currently SHOW each product → small pane-number badges on the
    /// right of the row (a product duplicated in two panes shows two). Products absent
    /// from the map get no badge. Repaints the tree.
    void setProductPaneAssignments(const QHash<QString, QList<int>> &assignments);

signals:
    /// Emitted when the selection lands on a product row (empty string when the
    /// selection is a dataset root or cleared).
    void productSelected(const QString &productId);

    /// Emitted on ANY selection change so the viewer can make the matching pane the
    /// active one (or mount it). @p viewTag is "view3d"/"slice2d" for the intrinsic
    /// view rows; @p productId is set for a derived-product row. Both empty for a
    /// dataset root (§ tree-selection = pane-selection).
    void rowActivated(const QString &viewTag, const QString &productId);

    /// (#4) A pane-number badge was clicked → the viewer activates that pane (§5.1).
    void paneBadgeActivated(int paneNumber);

    /// Right-click on a product row → the viewer builds the "Show in ▸ Pane N / New pane"
    /// menu at @p globalPos (it knows the pane state + renderability).
    void productContextMenuRequested(const QString &productId, const QPoint &globalPos);

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

    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    bool m_suppressActivate = false;   ///< true while a programmatic reverse-sync selection runs
    QString m_ownerScope;              ///< this tree's window scope; empty = unscoped (show all)
};

#endif // SESSIONDATATREE_H
