#include "SessionDataTree.h"

#include "app/ProductRegistry.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"

#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

namespace {
using namespace Qt::StringLiterals;
constexpr int kParentKeyRole = Qt::UserRole;       // dataset roots
constexpr int kProductIdRole = Qt::UserRole + 1;   // product children

// A short uppercase chip per product kind — mirrors the dataset's "CUBE" badge so
// a product row reads as a typed child, not a bare label (§3.0c).
QString productBadge(const QString &kind)
{
    static const QHash<QString, QString> map{
        { u"moment"_s, u"MOM0"_s },   { u"baseline"_s, u"BASE"_s },
        { u"channelmaps"_s, u"CHAN"_s }, { u"linewidth"_s, u"LINE"_s },
        { u"pv"_s, u"PV"_s },         { u"noise"_s, u"NOISE"_s },
        { u"stack"_s, u"STACK"_s },   { u"spectrum"_s, u"SPEC"_s },
    };
    const QString b = map.value(kind);
    return b.isEmpty() ? kind.left(4).toUpper() : b;
}

// Moment products share kind "moment"; their badge must carry the ORDER (MOM0, MOM1, …)
// or every order reads as MOM0 (§3.2). The order lives in the id ("moment<order>@…").
QString momentBadge(const AnalysisProduct &p)
{
    const int at = p.productId.indexOf(u'@');
    if (p.productId.startsWith(u"moment"_s) && at > 6)
        return u"MOM"_s + p.productId.mid(6, at - 6);
    return u"MOM"_s;
}

QString productText(const AnalysisProduct &p)
{
    const QString chip = (p.kind == u"moment"_s) ? momentBadge(p) : productBadge(p.kind);
    const QString badge = p.kind.isEmpty() ? QString() : QStringLiteral("%1  ").arg(chip);
    QString t = badge + (p.label.isEmpty() ? p.kind : p.label);
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
    m_tree->setIndentation(14);                // tight indent; branch chevron is 10px (§3.2)
    m_tree->setExpandsOnDoubleClick(false);   // double-click activates products
    m_tree->header()->setStretchLastSection(true);
    // Soft selection (§3.0c): a full-width solid-cyan band erased the parent/child
    // hierarchy — use a 16%-alpha tint + a 2 px brand-blue left marker instead.
    m_tree->setStyleSheet(QStringLiteral(
            "QTreeView { background: transparent; border: none; outline: none; }"
            "QTreeView::item { padding: 4px 2px; border-left: 2px solid transparent; }"
            "QTreeView::item:selected {"
            "  background: rgba(4, 138, 191, 0.16);"
            "  border-left: 2px solid %1;"
            "  color: %2;"
            "}"
            // Themed branch chevron — the default triangle stays black in dark mode
            // (§3.2). Same baked SVG the combos use (§3.0c), not the system indicator.
            "QTreeView::branch:has-children:!has-siblings:closed,"
            "QTreeView::branch:closed:has-children:has-siblings {"
            "  image: url(:/icons/chevron_right.svg);"
            "}"
            "QTreeView::branch:open:has-children:!has-siblings,"
            "QTreeView::branch:open:has-children:has-siblings {"
            "  image: url(:/icons/chevron_down.svg);"
            "}")
            .arg(QLatin1StringView(VisivoTheme::kPrimary()),
                 QLatin1StringView(VisivoTheme::kOnSurface())));
    layout->addWidget(m_tree);

    connect(m_tree, &QTreeView::doubleClicked, this, &SessionDataTree::onActivated);
    // Keep the tree exactly as tall as its visible rows (no dead space above the
    // LAYERS section below it), growing as products/datasets appear.
    connect(m_tree, &QTreeView::expanded, this, [this]() { updateHeightToContents(); });
    connect(m_tree, &QTreeView::collapsed, this, [this]() { updateHeightToContents(); });
    connect(m_model, &QStandardItemModel::rowsInserted, this,
            [this]() { updateHeightToContents(); });
    connect(m_model, &QStandardItemModel::rowsRemoved, this,
            [this]() { updateHeightToContents(); });
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
    // Defer once so row heights are realised by the layout before we measure.
    updateHeightToContents();
    QTimer::singleShot(0, this, [this]() { updateHeightToContents(); });
}

void SessionDataTree::onProductAdded(const AnalysisProduct &product)
{
    upsertProduct(product);
}

void SessionDataTree::selectProduct(const QString &productId)
{
    if (productId.isEmpty())
        return;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        QStandardItem *ds = m_model->item(r);
        if (!ds)
            continue;
        for (int i = 0; i < ds->rowCount(); ++i) {
            QStandardItem *child = ds->child(i);
            if (child && child->data(kProductIdRole).toString() == productId) {
                m_tree->setCurrentIndex(child->index());   // → currentChanged → productSelected
                return;
            }
        }
    }
}

void SessionDataTree::onProductUpdated(const AnalysisProduct &product)
{
    upsertProduct(product);
}

int SessionDataTree::visibleRowHeight(const QModelIndex &parent) const
{
    int h = 0;
    const int n = m_model->rowCount(parent);
    for (int i = 0; i < n; ++i) {
        const QModelIndex idx = m_model->index(i, 0, parent);
        const int rh = m_tree->visualRect(idx).height();          // public; 0 if not shown
        h += rh > 0 ? rh : m_tree->sizeHintForIndex(idx).height();
        if (m_tree->isExpanded(idx))
            h += visibleRowHeight(idx);
    }
    return h;
}

void SessionDataTree::updateHeightToContents()
{
    int h = 2 * m_tree->frameWidth() + visibleRowHeight(QModelIndex());
    if (h <= 0)
        h = m_tree->frameWidth() * 2 + 24;       // at least one row's worth
    m_tree->setFixedHeight(h);
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
