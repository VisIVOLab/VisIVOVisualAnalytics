#include "SessionDataTree.h"

#include "app/ProductRegistry.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"

#include <QHash>
#include <QHeaderView>

#include <functional>
#include <QIcon>
#include <QItemSelectionModel>
#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardItem>
#include <QScopedValueRollback>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

namespace {
using namespace Qt::StringLiterals;
constexpr int kParentKeyRole = Qt::UserRole;       // dataset roots
constexpr int kProductIdRole = Qt::UserRole + 1;   // product children
constexpr int kViewTagRole = Qt::UserRole + 2;     // intrinsic view rows ("view3d"/"slice2d")
constexpr int kPaneNumbersRole = Qt::UserRole + 3; // (#4) QVariantList of pane numbers (1..4)

// (#4) The pane-number badges are laid out 16×16 from the RIGHT edge of the row, in
// ascending pane order. Shared by the delegate (paint) and the viewport filter (hit-test)
// so a click lands exactly on what's drawn.
QList<QRect> paneBadgeRects(const QRect &itemRect, int count)
{
    QList<QRect> rects;
    constexpr int sz = 16, gap = 4, padRight = 8;
    int right = itemRect.right() - padRight;
    for (int i = 0; i < count; ++i) {
        const int top = itemRect.top() + (itemRect.height() - sz) / 2;
        rects.prepend(QRect(right - sz, top, sz, sz));   // build right→left, store ascending
        right -= (sz + gap);
    }
    return rects;
}

// Paints the default row, then the pane-number badges (outline box + muted number).
class PaneBadgeDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt,
               const QModelIndex &idx) const override
    {
        const QVariantList nums = idx.data(kPaneNumbersRole).toList();
        // Reserve a right-side band so the base delegate ELIDES the label before the badges
        // (never draws text under them), then draw the badges at the ORIGINAL rect.
        QStyleOptionViewItem o = opt;
        if (!nums.isEmpty()) {
            const int n = static_cast<int>(nums.size());
            const int band = n * 16 + (n - 1) * 4 + 8 + 6;   // badges + right pad + gap
            o.rect.setRight(o.rect.right() - band);
        }
        QStyledItemDelegate::paint(p, o, idx);
        if (nums.isEmpty())
            return;
        const QList<QRect> rects = paneBadgeRects(opt.rect, static_cast<int>(nums.size()));
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        QFont f = opt.font;
        f.setPointSizeF(f.pointSizeF() * 0.82);
        p->setFont(f);
        const QColor border(QString::fromLatin1(VisivoTheme::kOutline()));
        const QColor text(QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()));
        for (int i = 0; i < rects.size() && i < nums.size(); ++i) {
            p->setPen(QPen(border, 1));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(QRectF(rects[i]).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            p->setPen(text);
            p->drawText(rects[i], Qt::AlignCenter, QString::number(nums[i].toInt()));
        }
        p->restore();
    }
};

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
            // (§3.2), so a QSS image replaces the system indicator.
            //
            // Its own icon rather than the shared chevron_*.svg the combo boxes
            // use: Qt draws a ::branch image at the SVG's intrinsic size and
            // ignores width/height on the sub-control (measured), so the combo
            // glyph — 12x8 at stroke-width 2, sized for a drop-down — came out
            // heavy against these 34 px thumbnail rows. The tree pair is 9x6 at
            // stroke-width 1.4, in the muted foreground rather than near-white.
            "QTreeView::branch:has-children:!has-siblings:closed,"
            "QTreeView::branch:closed:has-children:has-siblings {"
            "  image: url(:/icons/tree_chevron_right%3.svg);"
            "}"
            "QTreeView::branch:open:has-children:!has-siblings,"
            "QTreeView::branch:open:has-children:has-siblings {"
            "  image: url(:/icons/tree_chevron_down%3.svg);"
            "}")
            .arg(QLatin1StringView(VisivoTheme::kPrimary()),
                 QLatin1StringView(VisivoTheme::kOnSurface()),
                 // A QSS `image:` is a resource URL, so it cannot carry a
                 // colour — the stroke is baked into the file and the pair has
                 // to be chosen here. The dark stroke (#9FA6AC) sits at ~2.4:1
                 // on the light panel, which is not a visible glyph.
                 VisivoTheme::isDark() ? QLatin1StringView("")
                                       : QLatin1StringView("_light")));
    m_tree->setItemDelegate(new PaneBadgeDelegate(m_tree));   // (#4) paints pane-number badges
    m_tree->viewport()->installEventFilter(this);             // (#4) badge clicks → activate pane
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);      // right-click → Show in ▸ menu
    connect(m_tree, &QTreeView::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex idx = m_tree->indexAt(pos);
        const QString pid = idx.data(kProductIdRole).toString();
        const QString viewTag = idx.data(kViewTagRole).toString();
        if (!pid.isEmpty() || !viewTag.isEmpty())
            emit rowContextMenuRequested(viewTag, pid, m_tree->viewport()->mapToGlobal(pos));
    });
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
    // Selecting a row INSPECTS it, and focuses the pane that already shows it —
    // it never MOUNTS anything or rearranges panes. Mounting is the explicit
    // gesture: double-click (onActivated), or right-click ▸ Show in.
    //
    // Selection used to mount into the active pane, which had two consequences.
    // It replaced whatever that pane was showing just because the user clicked a
    // row to read its provenance — and since Qt makes the pressed row current on
    // ANY button, a RIGHT-click mounted the product before its own "Show in ▸"
    // menu had even opened, which made the menu pointless (user report).
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &cur, const QModelIndex &) {
                QStandardItem *it = m_model->itemFromIndex(cur);
                const QString productId = it ? it->data(kProductIdRole).toString() : QString();
                emit productSelected(productId);                 // → Inspector › Provenance
                // A selection the USER made also moves the focus to the pane already
                // showing that row, so the dock's DISPLAY section is about the thing
                // that is highlighted. A programmatic one stays passive.
                if (!m_selectingProgrammatically) {
                    const QString viewTag = it ? it->data(kViewTagRole).toString() : QString();
                    if (!viewTag.isEmpty() || !productId.isEmpty())
                        emit rowSelected(viewTag, productId);
                }
            });

    ProductRegistry &reg = ProductRegistry::instance();
    connect(&reg, &ProductRegistry::productAdded, this, &SessionDataTree::onProductAdded);
    connect(&reg, &ProductRegistry::productUpdated, this, &SessionDataTree::onProductUpdated);
    connect(&reg, &ProductRegistry::productRemoved, this, &SessionDataTree::onProductRemoved);
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

    // The cube's intrinsic views (3D Volume, 2D Slice) are first-class rows under the
    // dataset — selecting one activates its pane and points DISPLAY at its LUT. They are
    // NOT products (no id in ProductRegistry); the derived products nest below them. Only
    // a cube has these views — a flat 2-D image (vtkWindowImage) has neither.
    if (kind.compare(u"cube"_s, Qt::CaseInsensitive) == 0) {
        auto addViewRow = [&](const QString &text, const QString &tag) {
            auto *v = new QStandardItem(text);
            v->setData(tag, kViewTagRole);
            v->setToolTip(QStringLiteral("Double-click to show it in a pane, "
                                         "right-click for “Show in ▸”"));
            it->appendRow(v);
        };
        addViewRow(u"3D Volume"_s, u"view3d"_s);
        addViewRow(u"2D Slice"_s, u"slice2d"_s);
    }

    m_tree->expand(it->index());
    return it;
}

void SessionDataTree::upsertProduct(const AnalysisProduct &p)
{
    QStandardItem *ds = datasetItem(p.parentKey, p.parentLabel, QString(), /*create=*/true);
    if (!ds)
        return;
    // A TRANSITORY "live" entry (e.g. the Extract-Spectrum live preview) reads distinct from
    // a stable product: italic label in the muted foreground.
    const bool live = (p.status == u"live"_s);
    auto styleItem = [&](QStandardItem *item) {
        QFont f = item->font();
        f.setItalic(live);
        item->setFont(f);
        // Both branches must name a real colour. A default-constructed QBrush is
        // an INVALID brush, and the model stores it as a set ForegroundRole all
        // the same — the delegate then installs it over the palette's Text
        // entry and paints the row black, which on the dark panel is an
        // unreadable product name (measured on MOM0 / SPEC rows). The dataset
        // rows above them looked right only because they set no brush at all.
        item->setForeground(QBrush(QColor(QString::fromLatin1(
                live ? VisivoTheme::kOnSurfaceVariant() : VisivoTheme::kOnSurface()))));
    };
    for (int i = 0; i < ds->rowCount(); ++i) {
        QStandardItem *child = ds->child(i);
        if (child && child->data(kProductIdRole).toString() == p.productId) {
            child->setText(productText(p));
            styleItem(child);
            return;
        }
    }
    auto *child = new QStandardItem(productText(p));
    child->setData(p.productId, kProductIdRole);
    // Selecting a row only inspects it, so the row itself says how to show it.
    child->setToolTip(QStringLiteral("%1 - double-click to show it in a pane, "
                                     "right-click for “Show in ▸”").arg(p.kind));
    styleItem(child);
    ds->appendRow(child);
    m_tree->expand(ds->index());
}

void SessionDataTree::onProductRemoved(const QString &productId)
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
                // Removing the current row makes a NEIGHBOUR current, and that
                // is not a selection gesture: without the guard, deleting a
                // product would move the focus (and the dock's DISPLAY section)
                // to whatever row happened to take its place.
                {
                    QScopedValueRollback<bool> guard(m_selectingProgrammatically, true);
                    ds->removeRow(i);
                }
                updateHeightToContents();
                return;
            }
        }
    }
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
        if (ownsProduct(p))
            upsertProduct(p);
    if (ds) {
        // A dataset root carries neither a view tag nor a product id, so this
        // cannot emit rowSelected today; guarded anyway, because "the code moved
        // the selection" is the rule, not a property of this particular row.
        QScopedValueRollback<bool> guard(m_selectingProgrammatically, true);
        m_tree->setCurrentIndex(ds->index());
    }
    // Defer once so row heights are realised by the layout before we measure.
    updateHeightToContents();
    QTimer::singleShot(0, this, [this]() { updateHeightToContents(); });
}

bool SessionDataTree::ownsProduct(const AnalysisProduct &product) const
{
    // Session-wide registry: only surface products this window produced. An empty scope means
    // the tree was never bound to a window (back-compat) → show everything.
    return m_ownerScope.isEmpty() || product.ownerId == m_ownerScope;
}

void SessionDataTree::onProductAdded(const AnalysisProduct &product)
{
    if (!ownsProduct(product))
        return;   // a sibling window's product — its render widgets live over there
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
                // Programmatic selection (e.g. registerAnalysisProduct populating
                // Provenance): drive productSelected but NOT rowActivated — a passive
                // registration must never mount/steal a pane.
                // Passive: a product registering itself must not steal a pane.
                QScopedValueRollback<bool> guard(m_selectingProgrammatically, true);
                m_tree->setCurrentIndex(child->index());   // → currentChanged → productSelected
                return;
            }
        }
    }
}

void SessionDataTree::onProductUpdated(const AnalysisProduct &product)
{
    if (!ownsProduct(product))
        return;
    upsertProduct(product);
    // …and if this row is the one the Inspector is showing, tell it again. The
    // Provenance panel is fed by productSelected, which only ever came from
    // currentChanged, so a product that gained its parameters WHILE selected
    // kept displaying the old ones: a baseline run watched from its own row sat
    // at "Status: running…" after it finished, and selecting away and back was
    // the only way to see the result.
    if (const QStandardItem *cur = m_model->itemFromIndex(m_tree->currentIndex())) {
        if (cur->data(kProductIdRole).toString() == product.productId)
            emit productSelected(product.productId);
    }
}

void SessionDataTree::setProductPaneAssignments(const QHash<QString, QList<int>> &assignments)
{
    // (#4) Stamp each product row with the pane numbers that currently SHOW it (empty →
    // no badge). Walk every dataset's product children; a productId absent from the map
    // clears its badges.
    for (int r = 0; r < m_model->rowCount(); ++r) {
        QStandardItem *ds = m_model->item(r);
        if (!ds)
            continue;
        for (int i = 0; i < ds->rowCount(); ++i) {
            QStandardItem *child = ds->child(i);
            if (!child)
                continue;
            const QString pid = child->data(kProductIdRole).toString();
            if (pid.isEmpty())
                continue;   // a view row (3D Volume / 2D Slice), not a product
            QVariantList nums;
            for (int n : assignments.value(pid))
                nums << n;
            child->setData(nums, kPaneNumbersRole);
        }
    }
    if (m_tree && m_tree->viewport())
        m_tree->viewport()->update();
}

bool SessionDataTree::eventFilter(QObject *watched, QEvent *event)
{
    // (#4) A click on a pane-number badge activates that pane (§5.1) — consume it so it
    // doesn't also change the tree selection.
    if (m_tree && watched == m_tree->viewport()
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonDblClick)) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const QPoint pos = me->position().toPoint();
            const QModelIndex idx = m_tree->indexAt(pos);
            const QVariantList nums = idx.data(kPaneNumbersRole).toList();
            if (!nums.isEmpty()) {
                const QList<QRect> rects =
                        paneBadgeRects(m_tree->visualRect(idx), static_cast<int>(nums.size()));
                for (int i = 0; i < rects.size() && i < nums.size(); ++i)
                    if (rects[i].contains(pos)) {
                        // Emit once (on press); swallow the dbl-click too so it neither
                        // re-emits nor reaches onActivated().
                        if (event->type() == QEvent::MouseButtonPress)
                            emit paneBadgeActivated(nums[i].toInt());
                        return true;   // swallow — do not select/activate the row
                    }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SessionDataTree::selectForView(const QString &viewTag, const QString &productId)
{
    // Match either an intrinsic view row (by tag) or a product row (by id), anywhere in
    // the tree, and select it without re-emitting rowActivated (would loop pane→tree→pane).
    QStandardItem *match = nullptr;
    std::function<void(QStandardItem *)> walk = [&](QStandardItem *parent) {
        if (match || !parent)
            return;
        for (int i = 0; i < parent->rowCount() && !match; ++i) {
            QStandardItem *it = parent->child(i);
            if (!it)
                continue;
            const bool hitView = !viewTag.isEmpty()
                    && it->data(kViewTagRole).toString() == viewTag;
            const bool hitProd = !productId.isEmpty()
                    && it->data(kProductIdRole).toString() == productId;
            if (hitView || hitProd) {
                match = it;
                return;
            }
            walk(it);
        }
    };
    walk(m_model->invisibleRootItem());
    if (!match)
        return;
    // Passive: this is the pane→tree direction, so it must not emit rowSelected and
    // bounce straight back as tree→pane.
    QScopedValueRollback<bool> guard(m_selectingProgrammatically, true);
    m_tree->setCurrentIndex(match->index());   // passive: selection no longer mounts
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
    const QString viewTag = it->data(kViewTagRole).toString();
    if (productId.isEmpty()) {
        // An intrinsic view row (3D Volume / 2D Slice): mount it. These used to
        // arrive in a pane through the selection side effect, so double-click did
        // nothing for them; with selection made passive they need the same
        // explicit path as a product.
        if (!viewTag.isEmpty())
            emit rowActivated(viewTag, QString());
        return;   // else a dataset root — nothing to focus
    }
    const AnalysisProduct p = ProductRegistry::instance().product(productId);
    if (!p.renderer.isEmpty()) {
        // A pane-renderable product (image / spectrum / PV diagram): (re)mount it in a pane
        // rather than raising a window. Emitting rowActivated forces activation even when the
        // row is already current — currentChanged wouldn't re-fire, so the parked no-free-pane
        // product would otherwise be unreachable from its own already-selected row.
        emit rowActivated(viewTag, productId);
        return;
    }
    if (p.window) {
        p.window->show();
        p.window->raise();
        p.window->activateWindow();
        return;
    }
    // No window (closed, or never created): not nothing — ask the viewer, which
    // knows whether this product can be reopened from a file it left behind.
    emit productReopenRequested(productId);
}
