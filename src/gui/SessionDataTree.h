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

signals:
    /// Emitted when the selection lands on a product row (empty string when the
    /// selection is a dataset root or cleared).
    void productSelected(const QString &productId);

private slots:
    void onProductAdded(const AnalysisProduct &product);
    void onProductUpdated(const AnalysisProduct &product);
    void onActivated(const QModelIndex &index);

private:
    QStandardItem *datasetItem(const QString &parentKey, const QString &label,
                               const QString &kind, bool createIfMissing);
    void upsertProduct(const AnalysisProduct &product);

    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
};

#endif // SESSIONDATATREE_H
