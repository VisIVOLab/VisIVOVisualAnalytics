#ifndef SESSIONDATATREE_H
#define SESSIONDATATREE_H

// SessionDataTree — the left "SESSION DATA" panel of Design 1d (tappa 2). A
// QTreeView over a QStandardItemModel that shows each open dataset as a root and
// every derived product (from ProductRegistry) nested beneath the dataset it came
// from. Double-clicking a product raises its result window. Modelled on
// VLKBInventoryTree's QStandardItemModel usage.

#include <QWidget>

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

signals:
    /// Emitted when the selection lands on a product row (empty string when the
    /// selection is a dataset root or cleared).
    void productSelected(const QString &productId);

    /// Emitted on ANY selection change so the viewer can make the matching pane the
    /// active one (or mount it). @p viewTag is "view3d"/"slice2d" for the intrinsic
    /// view rows; @p productId is set for a derived-product row. Both empty for a
    /// dataset root (§ tree-selection = pane-selection).
    void rowActivated(const QString &viewTag, const QString &productId);

private slots:
    void onProductAdded(const AnalysisProduct &product);
    void onProductUpdated(const AnalysisProduct &product);
    void onActivated(const QModelIndex &index);

private:
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
};

#endif // SESSIONDATATREE_H
