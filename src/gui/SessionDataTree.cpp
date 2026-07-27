#include "SessionDataTree.h"

#include "app/ProductRegistry.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"

#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

namespace {
using namespace Qt::StringLiterals;
constexpr int kParentKeyRole = Qt::UserRole;       // dataset roots
constexpr int kProductIdRole = Qt::UserRole + 1;   // product children

QString productText(const AnalysisProduct &p)
{
    QString t = p.label.isEmpty() ? p.kind : p.label;
    if (p.status == u"running"_s)
        t += QStringLiteral(" · %1%").arg(int(p.progress * 100.0 + 0.5));
    else if (p.status == u"failed"_s)
        t += QStringLiteral(" · failed");
    return t;
}
} // namespace

SessionDataTree::SessionDataTree(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_model = new QStandardItemModel(this);
    m_tree = new QTreeView(this);
    m_tree->setModel(m_model);
    m_tree->setHeaderHidden(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setIconSize(QSize(40, 26));
    m_tree->setExpandsOnDoubleClick(false);   // double-click activates products
    m_tree->header()->setStretchLastSection(true);
    layout->addWidget(m_tree);

    connect(m_tree, &QTreeView::doubleClicked, this, &SessionDataTree::onActivated);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &cur, const QModelIndex &) {
                QStandardItem *it = m_model->itemFromIndex(cur);
                emit productSelected(it ? it->data(kProductIdRole).toString() : QString());
            });

    ProductRegistry &reg = ProductRegistry::instance();
    connect(&reg, &ProductRegistry::productAdded, this, &SessionDataTree::onProductAdded);
    connect(&reg, &ProductRegistry::productUpdated, this, &SessionDataTree::onProductUpdated);
}

QStandardItem *SessionDataTree::datasetItem(const QString &parentKey, const QString &label,
                                            const QString &kind, bool createIfMissing)
{
    QStandardItem *root = m_model->invisibleRootItem();
    for (int i = 0; i < root->rowCount(); ++i) {
        QStandardItem *it = root->child(i);
        if (it && it->data(kParentKeyRole).toString() == parentKey)
            return it;
    }
    if (!createIfMissing)
        return nullptr;

    const QString badge = kind.isEmpty() ? QString() : QStringLiteral("%1  ").arg(kind.toUpper());
    auto *it = new QStandardItem(badge + (label.isEmpty() ? parentKey : label));
    it->setData(parentKey, kParentKeyRole);
    it->setToolTip(parentKey);
    const QPixmap thumb = ThumbnailCache::load(parentKey, 40, 26);
    if (!thumb.isNull())
        it->setIcon(QIcon(thumb));
    root->appendRow(it);
    m_tree->expand(it->index());
    return it;
}

void SessionDataTree::upsertProduct(const AnalysisProduct &p)
{
    QStandardItem *ds = datasetItem(p.parentKey, p.parentLabel, QString(), /*create=*/true);
    if (!ds)
        return;
    for (int i = 0; i < ds->rowCount(); ++i) {
        QStandardItem *child = ds->child(i);
        if (child && child->data(kProductIdRole).toString() == p.productId) {
            child->setText(productText(p));
            return;
        }
    }
    auto *child = new QStandardItem(productText(p));
    child->setData(p.productId, kProductIdRole);
    child->setToolTip(p.kind);
    ds->appendRow(child);
    m_tree->expand(ds->index());
}

void SessionDataTree::setCurrentDataset(const QString &parentKey, const QString &label,
                                        const QString &kind)
{
    if (parentKey.isEmpty())
        return;
    QStandardItem *ds = datasetItem(parentKey, label, kind, /*create=*/true);
    // Load any products already registered for this dataset (e.g. reopened).
    const QList<AnalysisProduct> existing = ProductRegistry::instance().productsFor(parentKey);
    for (const AnalysisProduct &p : existing)
        upsertProduct(p);
    if (ds)
        m_tree->setCurrentIndex(ds->index());
}

void SessionDataTree::onProductAdded(const AnalysisProduct &product)
{
    upsertProduct(product);
}

void SessionDataTree::onProductUpdated(const AnalysisProduct &product)
{
    upsertProduct(product);
}

void SessionDataTree::onActivated(const QModelIndex &index)
{
    QStandardItem *it = m_model->itemFromIndex(index);
    if (!it)
        return;
    const QString productId = it->data(kProductIdRole).toString();
    if (productId.isEmpty())
        return;   // a dataset root — nothing to focus
    const AnalysisProduct p = ProductRegistry::instance().product(productId);
    if (p.window) {
        p.window->show();
        p.window->raise();
        p.window->activateWindow();
    }
}
