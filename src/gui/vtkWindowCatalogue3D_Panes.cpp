// vtkWindowCatalogue3D_Panes.cpp
// Compare Quantity as panes of one window (#604): a 1 / 2 / 4 pane grid, an active
// pane, pop-out and dock-back, a quantity per pane from the header, and the Session
// Data rows that list what the panes show. The grid is the cube window's (PaneGrid);
// the linking of the views is the comparison group's (vtkWindowCatalogue3D_Compare.cpp),
// which is why a pane is a group member whose view sits here.

#include "vtkWindowCatalogue3D.h"

#include "PaneGrid.h"
#include "WorkspaceChrome.h"
#include "app/ProductRegistry.h"
#include "app/DatasetName.h"
#include "theme/SegmentedToggle.h"
#include "theme/VisivoTheme.h"
#include "ui_vtkWindowCatalogue3D.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <vtkGenericOpenGLRenderWindow.h>

#include <algorithm>
#include <memory>

using namespace Qt::StringLiterals;
using namespace visivo::panes;

namespace {

// Takes a view out of the layout it sits in, so it can be added to another.
void takeOut(QWidget *view)
{
    if (!view)
        return;
    if (QWidget *p = view->parentWidget())
        if (QLayout *l = p->layout())
            l->removeWidget(view);
}

QString quantityText(const QComboBox *combo)
{
    return combo ? combo->currentText().remove(u"raw:"_s) : QString();
}

} // namespace

vtkWindowCatalogue3D *vtkWindowCatalogue3D::paneHostOrSelf()
{
    return this->m_paneHost ? this->m_paneHost.data() : this;
}

vtkWindowCatalogue3D *vtkWindowCatalogue3D::activePaneWindow()
{
    if (this->m_paneGrid && this->m_activePane >= 0 && this->m_activePane < kSlots
        && this->m_paneWindows[this->m_activePane])
        return this->m_paneWindows[this->m_activePane];
    return this;
}

int vtkWindowCatalogue3D::paneSlotOf(const vtkWindowCatalogue3D *window) const
{
    for (int i = 0; i < kSlots; ++i)
        if (this->m_paneWindows[i] == window)
            return i;
    return -1;
}

bool vtkWindowCatalogue3D::paneStacked() const
{
    // Narrow grid: two panes one above the other (as the cube window decides).
    const int w = this->m_paneRoot ? this->m_paneRoot->width() : 0;
    return w > 0 ? (w < 900) : (this->width() < 1100);
}

void vtkWindowCatalogue3D::ensurePaneGrid()
{
    if (this->m_paneGrid || this->m_paneHost || !this->m_renderStack)
        return;
    QWidget *container = this->m_renderStack->parentWidget();
    QLayout *layout = container ? container->layout() : nullptr;
    if (!layout)
        return;

    this->m_paneRoot = new QWidget(container);
    auto *column = new QVBoxLayout(this->m_paneRoot);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    // A thin strip above the grid: the layout (1 / 2 / 4) and a way to add a pane.
    auto *strip = new QWidget(this->m_paneRoot);
    strip->setObjectName(u"paneStrip"_s);
    strip->setFixedHeight(30);
    auto *row = new QHBoxLayout(strip);
    row->setContentsMargins(10, 0, 10, 0);
    row->setSpacing(8);
    auto *caption = new QLabel(u"Panes"_s, strip);
    caption->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                                   .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    row->addWidget(caption);
    this->m_paneCountToggle = new SegmentedToggle(strip);
    this->m_paneCountToggle->setItems({ u"1"_s, u"2"_s, u"4"_s });
    this->m_paneCountToggle->setItemToolTip(0, u"One pane"_s);
    this->m_paneCountToggle->setItemToolTip(
            1, u"Two panes. A pane taken out of the layout keeps its quantity."_s);
    this->m_paneCountToggle->setItemToolTip(2, u"Four panes"_s);
    row->addWidget(this->m_paneCountToggle);
    row->addStretch(1);
    auto *add = new QPushButton(u"Compare another quantity"_s, strip);
    add->setToolTip(u"Add a pane showing another quantity of these particles. Camera and "
                    "selection stay linked; quantity, colour map, range and scale are its own."_s);
    row->addWidget(add);
    column->addWidget(strip);

    auto *splitter = new QSplitter(Qt::Horizontal, this->m_paneRoot);
    splitter->setChildrenCollapsible(false);
    column->addWidget(splitter, 1);
    layout->replaceWidget(this->m_renderStack, this->m_paneRoot);

    this->m_paneGrid = new PaneGrid(splitter, this, this);
    this->m_paneGrid->setTitleProvider([this](int slot) {
        const vtkWindowCatalogue3D *w = this->m_paneWindows[slot];
        return u"%1 · %2"_s.arg(this->shownName(), quantityText(w ? w->comboMapColor : nullptr));
    });
    this->m_paneGrid->build();
    // A press anywhere in a pane (its view too) makes it the active one.
    this->m_paneGrid->setPressActivates(true);

    for (int i = 0; i < kSlots; ++i) {
        PaneGrid::Slot &slot = this->m_paneGrid->slot(i);
        // The quantity: a dropdown of the particles' columns, plus the pane's own actions.
        auto *menu = new QMenu(slot.titleBtn);
        QObject::connect(menu, &QMenu::aboutToShow, this,
                         [this, i, menu]() { this->populatePaneMenu(i, menu); });
        slot.titleBtn->setMenu(menu);
        // Linked: this pane's camera and selection follow the group (and relinking
        // adopts the group's) — the comparison card's toggle, in the header.
        auto *link = new QToolButton();
        link->setObjectName(u"paneLink"_s);
        link->setCheckable(true);
        link->setToolButtonStyle(Qt::ToolButtonTextOnly);
        link->setCursor(Qt::PointingHandCursor);
        link->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:1px solid #2A3238;border-radius:2px;"
                "color:%1;font-size:10px;padding:1px 6px;}"
                "QToolButton:checked{border-color:%2;color:%2;}")
                                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()),
                                         QLatin1StringView(VisivoTheme::kPrimary())));
        QObject::connect(link, &QToolButton::clicked, this, [this, i](bool linked) {
            if (vtkWindowCatalogue3D *w = this->m_paneWindows[i])
                w->setComparisonLinked(linked);
            this->refreshPaneHeaders();
        });
        slot.controls->addWidget(link);
    }
    QObject::connect(this->m_paneGrid, &PaneGrid::headerPressed, this,
                     [this](int slot) { this->setActivePane(slot); });
    QObject::connect(this->m_paneGrid, &PaneGrid::maximiseToggled, this, [this](int slot) {
        // In-workspace maximise: the pane fills the grid; the same button restores.
        if (this->m_paneGrid->isMaximised()) {
            this->m_paneGrid->resetMaximiseButtons();
            this->setPaneCount(this->m_paneCount);
        } else {
            this->setActivePane(slot);
            this->m_paneGrid->showOnly(slot);
        }
    });
    QObject::connect(this->m_paneGrid, &PaneGrid::paneDetached, this,
                     [this](int slot) { this->setActivePane(slot); });
    QObject::connect(this->m_paneGrid, &PaneGrid::paneDocked, this, [this](int slot) {
        // A view moved between top-level windows: draw it again where it is now.
        if (vtkWindowCatalogue3D *w = this->m_paneWindows[slot])
            if (w->renderWindow)
                w->renderWindow->Render();
    });
    QObject::connect(this->m_paneCountToggle, &SegmentedToggle::currentIndexChanged, this,
                     [this](int index) { this->setPaneCount(index == 0 ? 1 : index == 1 ? 2 : 4); });
    QObject::connect(add, &QPushButton::clicked, this,
                     &vtkWindowCatalogue3D::openQuantityComparison);

    this->m_paneWindows[0] = this;
    this->m_paneControlsPage = this->m_pageViz;
    QObject::connect(this->comboMapColor, &QComboBox::currentIndexChanged, this,
                     [this]() { this->refreshPaneHeaders(); });
    this->mountPane(0);
    this->setPaneCount(1);
    this->setActivePane(0);
}

void vtkWindowCatalogue3D::mountPane(int slot)
{
    if (!this->m_paneGrid || slot < 0 || slot >= kSlots)
        return;
    vtkWindowCatalogue3D *w = this->m_paneWindows[slot];
    QWidget *view = w ? w->m_renderStack.data() : nullptr;
    QHBoxLayout *host = this->m_paneGrid->slot(slot).viewHostLayout;
    if (!host)
        return;
    // Whatever else sits in this slot goes home first: another view's page, or the
    // placeholder of an empty slot.
    while (host->count()) {
        QLayoutItem *item = host->takeAt(0);
        QWidget *old = item ? item->widget() : nullptr;
        delete item;
        if (!old || old == view)
            continue;
        bool returned = false;
        for (int i = 0; i < kSlots && !returned; ++i) {
            vtkWindowCatalogue3D *other = this->m_paneWindows[i];
            if (other && other->m_renderStack == old) {
                other->returnPaneView();
                returned = true;
            }
        }
        if (!returned)
            old->deleteLater();   // an empty slot's placeholder
    }
    if (view) {
        takeOut(view);
        host->addWidget(view);
        view->show();
    }
}

void vtkWindowCatalogue3D::returnPaneView()
{
    // Back to the page of this window it was taken from, hidden: the window's own
    // view is not shown in its central area while a grid holds the panes.
    if (!this->m_renderStack || !this->m_viewHome)
        return;
    takeOut(this->m_renderStack);
    if (QLayout *l = this->m_viewHome->layout())
        l->addWidget(this->m_renderStack);
    else
        this->m_renderStack->setParent(this->m_viewHome);
    this->m_renderStack->hide();
}

bool vtkWindowCatalogue3D::addPaneWindow(vtkWindowCatalogue3D *peer, int slot)
{
    this->ensurePaneGrid();
    if (!this->m_paneGrid || !peer)
        return false;
    std::array<bool, kSlots> taken{};
    for (int i = 0; i < kSlots; ++i)
        taken[i] = this->m_paneWindows[i] != nullptr;
    int count = this->m_paneCount;
    // The layout as it is drawn (its orientation was decided when it was last laid out).
    if (slot < 0)
        slot = slotForNewPane(this->m_paneCount, this->m_paneStacked, taken, &count);
    if (slot < 0 || taken[slot])
        return false;

    this->m_paneWindows[slot] = peer;
    peer->m_paneHost = this;
    peer->m_ownerScope = this->m_ownerScope;
    // The pane's quantity names its header and its Session Data row (the host lists them).
    QObject::connect(peer->comboMapColor, &QComboBox::currentIndexChanged, this, [this]() {
        this->refreshPaneHeaders();
        this->refreshPaneProducts();
    });
    if (count != this->m_paneCount || this->m_paneGrid->isMaximised())
        this->setPaneCount(count);   // shows the slot, mounts every pane (and leaves maximise)
    else
        this->mountPane(slot);
    // A new layout may have moved the pane to a slot it shows.
    const int placedAt = this->paneSlotOf(peer);
    this->refreshPaneProducts();
    this->setActivePane(placedAt >= 0 ? placedAt : slot);
    this->savePaneSession();
    return true;
}

void vtkWindowCatalogue3D::setPaneCount(int panes)
{
    if (!this->m_paneGrid)
        return;
    panes = normalisedCount(panes);
    // Out of a maximised pane first: a layout change leaves maximise mode.
    this->m_paneGrid->resetMaximiseButtons();
    // Shrinking to one pane keeps the active pane's view: it moves to slot 0.
    const int swap = survivorSwapSlot(panes, this->m_paneCount, this->m_activePane);
    if (swap >= 0)
        std::swap(this->m_paneWindows[0], this->m_paneWindows[swap]);
    // A layout change re-negotiates the grid: popped-out panes come back first.
    for (int i = 0; i < kSlots; ++i)
        this->m_paneGrid->dock(i);
    this->m_paneCount = panes;
    if (panes == 1)
        this->m_activePane = 0;
    // The orientation is decided here, from the width now; everything else that asks which
    // slots are shown uses this answer (m_paneStacked), so a window widened after opening
    // does not hide a pane.
    const bool stacked = panes == 2 && this->paneStacked();
    this->m_paneStacked = stacked;
    // A pane in a slot this layout hides moves into one it shows (4 -> 2, or a stacked pair
    // that is now side by side), with its view, before the views are mounted.
    {
        std::array<bool, kSlots> taken{};
        for (int i = 0; i < kSlots; ++i)
            taken[i] = this->m_paneWindows[i] != nullptr;
        const std::array<int, kSlots> to = slotsForLayout(taken, visibleSlots(panes, stacked));
        std::array<QPointer<vtkWindowCatalogue3D>, kSlots> moved;
        int active = this->m_activePane;
        for (int i = 0; i < kSlots; ++i) {
            if (!taken[i])
                continue;
            moved[to[i]] = this->m_paneWindows[i];
            if (i == this->m_activePane)
                active = to[i];
        }
        this->m_paneWindows = moved;
        this->m_activePane = active;
    }
    for (int i = 0; i < kSlots; ++i)
        this->mountPane(i);
    this->m_paneGrid->applyVisibility(visibleSlots(panes, stacked));
    this->m_paneGrid->equaliseSplits(panes, stacked);
    if (this->m_paneCountToggle) {
        const QSignalBlocker block(this->m_paneCountToggle);
        this->m_paneCountToggle->setCurrentIndex(panes == 1 ? 0 : panes == 2 ? 1 : 2);
    }
    if (!visibleSlots(panes, stacked)[this->m_activePane])
        this->setActivePane(0);
    else
        this->setActivePane(this->m_activePane);
    this->refreshPaneHeaders();
    this->savePaneSession();
}

void vtkWindowCatalogue3D::setActivePane(int slot)
{
    if (!this->m_paneGrid || slot < 0 || slot >= kSlots || !this->m_paneWindows[slot])
        return;
    this->m_activePane = slot;
    for (int i = 0; i < kSlots; ++i)
        this->m_paneGrid->styleSlot(i, i == slot);
    vtkWindowCatalogue3D *w = this->m_paneWindows[slot];
    this->showPaneControls(w);
    // The Session Data row of the pane's quantity follows (without re-activating it).
    if (this->m_workspaceChrome)
        this->m_workspaceChrome->selectSessionProduct(this->paneProductId(w));
}

void vtkWindowCatalogue3D::showPaneControls(vtkWindowCatalogue3D *window)
{
    if (!this->m_workspaceChrome || !window || !window->m_pageViz)
        return;
    QWidget *next = window->m_pageViz;
    if (this->m_paneControlsPage == next)
        return;
    // The host's left dock shows the controls of the ACTIVE pane's window: its quantity,
    // colour map, range and scale, shape, ray-traced light and the comparison card.
    if (QWidget *current = this->m_paneControlsPage) {
        // Back to the window it belongs to, hidden (it is deleted with that window).
        vtkWindowCatalogue3D *owner = this;
        for (int i = 0; i < kSlots; ++i)
            if (this->m_paneWindows[i] && this->m_paneWindows[i]->m_pageViz == current)
                owner = this->m_paneWindows[i];
        this->m_workspaceChrome->removeSessionLayers(current, owner);
    }
    next->show();
    this->m_workspaceChrome->addSessionLayers(next);
    this->m_paneControlsPage = next;
}

void vtkWindowCatalogue3D::populatePaneMenu(int slot, QMenu *menu)
{
    menu->clear();
    vtkWindowCatalogue3D *w = this->m_paneWindows[slot];
    if (!w) {
        menu->addAction(u"Add a quantity here"_s, this, [this]() { this->openQuantityComparison(); });
        return;
    }
    // Colour by: the same choices as the pane's own Colour by control.
    if (w->comboMapColor) {
        const int current = w->comboMapColor->currentIndex();
        for (int i = 0; i < w->comboMapColor->count(); ++i) {
            QAction *a = menu->addAction(w->comboMapColor->itemText(i).remove(u"raw:"_s));
            a->setCheckable(true);
            a->setChecked(i == current);
            QPointer<vtkWindowCatalogue3D> target = w;
            QObject::connect(a, &QAction::triggered, this, [target, i]() {
                if (target && target->comboMapColor)
                    target->comboMapColor->setCurrentIndex(i);
            });
        }
        menu->addSeparator();
    }
    const bool inGroup = !w->m_comparisonGroup.isEmpty();
    QAction *link = menu->addAction(u"Linked to the other panes"_s);
    link->setCheckable(true);
    link->setChecked(w->m_comparisonLinked && inGroup);
    link->setEnabled(inGroup);
    QObject::connect(link, &QAction::triggered, this, [this, slot](bool on) {
        if (vtkWindowCatalogue3D *p = this->m_paneWindows[slot])
            p->setComparisonLinked(on);
        this->refreshPaneHeaders();
    });
    menu->addAction(this->m_paneGrid->isDetached(slot) ? u"Dock back into the grid"_s
                                                        : u"Pop out to a window"_s,
                    this, [this, slot]() {
                        if (this->m_paneGrid->isDetached(slot))
                            this->m_paneGrid->dock(slot);
                        else
                            this->m_paneGrid->detach(slot);
                    });
    QAction *close = menu->addAction(u"Close this pane"_s, this,
                                     [this, slot]() { this->removePane(slot); });
    // The window that holds the grid is the first pane; closing it is closing the window.
    close->setEnabled(w != this);
}

void vtkWindowCatalogue3D::removePane(int slot)
{
    if (!this->m_paneGrid || slot < 0 || slot >= kSlots)
        return;
    vtkWindowCatalogue3D *w = this->m_paneWindows[slot];
    if (!w || w == this)
        return;
    w->close();   // closeEvent hands the view back and leaves the grid (forgetPane)
}

void vtkWindowCatalogue3D::forgetPane(vtkWindowCatalogue3D *window)
{
    if (!window)
        return;
    const int slot = this->paneSlotOf(window);
    if (slot < 0)
        return;
    if (this->m_paneGrid)
        this->m_paneGrid->dock(slot);
    if (this->m_paneControlsPage && this->m_paneControlsPage == window->m_pageViz
        && this->m_workspaceChrome && !this->m_closingPanes)
        this->showPaneControls(this);
    window->returnPaneView();
    this->m_paneWindows[slot] = nullptr;
    window->m_paneHost = nullptr;
    if (this->m_closingPanes)
        return;

    // The layout shrinks to what is left: the panes keep their order in the lowest slots.
    QList<vtkWindowCatalogue3D *> alive;
    for (int i = 0; i < kSlots; ++i)
        if (this->m_paneWindows[i])
            alive << this->m_paneWindows[i].data();
    const int count = alive.size() <= 1 ? 1 : alive.size() == 2 ? 2 : 4;
    const bool stacked = count == 2 && this->paneStacked();
    const std::array<bool, kSlots> visible = visibleSlots(count, stacked);
    this->m_paneWindows = {};
    int next = 0;
    for (vtkWindowCatalogue3D *w : alive) {
        while (next < kSlots && !visible[next])
            ++next;
        this->m_paneWindows[std::min(next, kSlots - 1)] = w;
        ++next;
    }
    this->m_activePane = 0;
    this->setPaneCount(count);
    this->refreshPaneProducts();
}

void vtkWindowCatalogue3D::refreshPaneHeaders()
{
    if (!this->m_paneGrid)
        return;
    for (int i = 0; i < kSlots; ++i) {
        PaneGrid::Slot &slot = this->m_paneGrid->slot(i);
        const vtkWindowCatalogue3D *w = this->m_paneWindows[i];
        if (!slot.titleBtn)
            continue;
        if (!w) {
            slot.titleBtn->setText(u"Empty pane ▾"_s);
            slot.qualifier->setText(QString());
            if (QToolButton *link = slot.controls->parentWidget()
                        ? slot.controls->parentWidget()->findChild<QToolButton *>(u"paneLink"_s)
                        : nullptr)
                link->hide();
            // The slot says how to fill it.
            if (slot.viewHostLayout && slot.viewHostLayout->count() == 0) {
                auto *hint = new QLabel(u"No quantity in this pane.\nUse “Compare another quantity”."_s);
                hint->setAlignment(Qt::AlignCenter);
                hint->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
                slot.viewHostLayout->addWidget(hint);
            }
            continue;
        }
        slot.titleBtn->setText(quantityText(w->comboMapColor) + u" ▾"_s);
        QStringList shape;
        if (w->m_rtLightCheck && w->m_rtLightCheck->isChecked())
            shape << u"ray-traced"_s;
        else if (w->comboGeometry)
            shape << w->comboGeometry->currentText().toLower();
        if (w->drawnOnBackend())
            shape << u"on backend"_s;
        slot.qualifier->setText(shape.join(u" · "_s));
        if (QToolButton *link = slot.controls->parentWidget()
                    ? slot.controls->parentWidget()->findChild<QToolButton *>(u"paneLink"_s)
                    : nullptr) {
            const bool inGroup = !w->m_comparisonGroup.isEmpty();
            const QSignalBlocker block(link);
            link->setVisible(inGroup);
            link->setChecked(w->m_comparisonLinked && inGroup);
            link->setText(w->m_comparisonLinked ? u"Linked"_s : u"Unlinked"_s);
            link->setToolTip(u"Camera and selection follow the other panes. Click to make this "
                             "pane autonomous; click again to join them (it adopts their view)."_s);
        }
    }
}

// ── Session Data ─────────────────────────────────────────────────────────────

QString vtkWindowCatalogue3D::paneProductId(const vtkWindowCatalogue3D *window) const
{
    return window ? u"quantity@%1"_s.arg(window->m_copilotRouteKey) : QString();
}

void vtkWindowCatalogue3D::refreshPaneProducts()
{
    vtkWindowCatalogue3D *host = this->paneHostOrSelf();
    if (host != this) {
        host->refreshPaneProducts();
        return;
    }
    // A peer not yet in a grid has no Session Data of its own to fill.
    if (this->m_ownerScope.isEmpty() || !this->m_pointTable || this->remoteDatasetId.isEmpty()
        || (this->m_madeAsPeer && !this->m_paneHost && !this->m_paneGrid))
        return;
    ProductRegistry &registry = ProductRegistry::instance();
    const auto base = [this]() {
        AnalysisProduct p;
        p.parentKey = this->filepath;
        p.parentLabel = this->shownName();
        p.ownerId = this->m_ownerScope;
        p.sourceDatasetId = this->remoteDatasetId;
        p.sessionId = this->remoteSessionId;
        return p;
    };

    // The particle type this window draws.
    AnalysisProduct component = base();
    component.productId = this->m_componentProductId =
            u"particles@%1"_s.arg(this->m_copilotRouteKey);
    component.kind = u"component"_s;
    const QString type = this->m_componentMetadata.value(u"name"_s).toString();
    component.label = u"%1 · %L2 rows"_s.arg(type.isEmpty() ? u"particles"_s : type)
                              .arg(this->m_tableRows);
    registry.registerProduct(component);
    QStringList ids{ component.productId };

    // One row per pane: the quantity it shows. A pane the tree row is activated for comes
    // to the front; the number badge on the row says which pane it is.
    QHash<QString, QList<int>> assignments;
    for (int i = 0; i < kSlots; ++i) {
        const vtkWindowCatalogue3D *w = this->m_paneWindows[i];
        if (!w && i == 0)
            w = this;   // before any pane is added the window itself is the only view
        if (!w)
            continue;
        AnalysisProduct p = base();
        p.productId = this->paneProductId(w);
        p.kind = u"quantity"_s;
        p.renderer = u"particles"_s;   // showable: activating the row focuses its pane
        p.label = quantityText(w->comboMapColor);
        p.params.insert(u"column"_s, w->comboMapColor ? w->comboMapColor->currentData().toString()
                                                      : QString());
        registry.registerProduct(p);
        ids << p.productId;
        assignments.insert(p.productId, { i + 1 });
    }
    if (this->m_workspaceChrome)
        this->m_workspaceChrome->setSessionProductPaneAssignments(assignments);

    // The SubFind catalogue of this backend session, when one is open: a row that
    // raises its window (the catalogue is its own table, with its own window).
    for (QWidget *top : QApplication::topLevelWidgets()) {
        auto *other = qobject_cast<vtkWindowCatalogue3D *>(top);
        if (!other || other == this || other->remoteSessionId != this->remoteSessionId
            || other->m_componentMetadata.value(u"catalogue_kind"_s).toString() != u"subfind"_s
            || !other->isVisible())
            continue;
        AnalysisProduct p = base();
        p.productId = u"subfind@%1"_s.arg(other->m_copilotRouteKey);
        p.kind = u"subfind"_s;
        p.label = other->shownName();
        p.window = other;
        registry.registerProduct(p);
        ids << p.productId;
    }
    // A pane closed (or a catalogue window gone) since the last refresh takes its row with it.
    for (const QString &old : std::as_const(this->m_paneProductIds))
        if (!ids.contains(old))
            registry.removeProduct(old);
    this->m_paneProductIds = ids;
}

// paneRemovalRefusal and removePaneProducts are the two halves of a Session Data removal
// handler (#580). They are not installed yet: SessionDataTree gets setRemovalHandler with
// PR #605, and wiring them is then one line (see the header). Until it is merged nothing
// calls them, on purpose, so they are kept here and covered by that line's test.
QString vtkWindowCatalogue3D::paneRemovalRefusal(const AnalysisProduct &product) const
{
    if (product.productId == this->m_componentProductId)
        return u"these are the particles of this window; close the window instead"_s;
    if (product.kind == u"subfind"_s)
        return u"the catalogue has its own window; close that window"_s;
    if (this->paneProductId(this) == product.productId)
        return u"it is this window's own pane; close the window instead"_s;
    return QString();
}

void vtkWindowCatalogue3D::removePaneProducts(const QStringList &productIds)
{
    // A quantity row is a pane: closing the pane hands its view back, shrinks the layout
    // and takes the row out of the registry (forgetPane > refreshPaneProducts).
    for (const QString &id : productIds)
        for (int i = 0; i < kSlots; ++i) {
            const vtkWindowCatalogue3D *w = this->m_paneWindows[i];
            if (w && w != this && this->paneProductId(w) == id) {
                this->removePane(i);
                break;
            }
        }
}

void vtkWindowCatalogue3D::onSessionRowActivated(const QString &, const QString &productId)
{
    // A quantity row: its pane becomes the active one.
    for (int i = 0; i < kSlots; ++i) {
        const vtkWindowCatalogue3D *w = this->m_paneWindows[i];
        if (!w && i == 0 && !this->m_paneGrid)
            w = this;
        if (w && this->paneProductId(w) == productId) {
            if (this->m_paneGrid) {
                // A pane hidden by a smaller layout comes back with the layout it needs.
                const std::array<bool, kSlots> visible =
                        visibleSlots(this->m_paneCount, this->m_paneStacked);
                if (!visible[i])
                    this->setPaneCount(4);
                this->setActivePane(i);
            }
            return;
        }
    }
}

// ── Session ──────────────────────────────────────────────────────────────────

visivo::particlepanes::PaneSession vtkWindowCatalogue3D::paneSession() const
{
    std::array<visivo::particlepanes::PaneSlotState, kSlots> cells;
    for (int i = 0; i < kSlots; ++i) {
        const vtkWindowCatalogue3D *w = this->m_paneWindows[i];
        if (!w)
            continue;
        QString name;
        if (w->comboMapColor) {
            const QString key = w->comboMapColor->currentData().toString();
            if (key.startsWith(u"raw:"_s))
                name = w->rawHeaders.value(key.mid(4).toInt());
        }
        cells[i] = { true, name, w->m_comparisonLinked || w->m_comparisonGroup.isEmpty() };
    }
    // The host's pane first: a restore starts from the host's own window, wherever the
    // pane sat in the grid when the session was saved.
    return visivo::particlepanes::makeSession(this->m_paneCount, this->m_activePane,
                                              this->paneSlotOf(this), cells);
}

void vtkWindowCatalogue3D::savePaneSession() const
{
    // A restore in progress saves once, at its end: a half-built layout is not the session.
    if (this->m_paneHost || this->m_restoringPanes || !this->m_pointTable
        || this->filepath.isEmpty())
        return;
    // By table, so the next window on it opens with the same comparison.
    QSettings settings;
    settings.beginGroup(u"layout_v2/particlePanes"_s);
    const QString key = QString::fromLatin1(
            QCryptographicHash::hash(this->filepath.toUtf8(), QCryptographicHash::Sha1).toHex());
    if (!this->m_paneGrid || this->paneSession().panes.size() < 2)
        settings.remove(key);
    else
        settings.setValue(key, QString::fromUtf8(QJsonDocument(
                                  visivo::particlepanes::toJson(this->paneSession()))
                                                         .toJson(QJsonDocument::Compact)));
    settings.endGroup();
}

void vtkWindowCatalogue3D::scheduleRestorePanes()
{
    if (this->m_paneHost || !this->m_pointTable || this->m_paneRestoreTimer)
        return;
    QSettings settings;
    settings.beginGroup(u"layout_v2/particlePanes"_s);
    const QString key = QString::fromLatin1(
            QCryptographicHash::hash(this->filepath.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString saved = settings.value(key).toString();
    settings.endGroup();
    if (saved.isEmpty())
        return;
    const visivo::particlepanes::PaneSession session = visivo::particlepanes::fromJson(
            QJsonDocument::fromJson(saved.toUtf8()).object());
    if (session.panes.size() < 2)
        return;
    // The rows have to be here before a pane can be made from them: wait, a minute at most.
    auto *timer = new QTimer(this);
    this->m_paneRestoreTimer = timer;
    auto waited = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, this, [this, timer, session, waited]() {
        const bool ready = !this->filterBusy && !this->m_sceneBuilding
                && !this->m_awaitingHydration && !this->entries.empty();
        if (!ready && ++*waited < 120)
            return;
        timer->stop();
        timer->deleteLater();
        if (!ready || this->m_paneGrid || this->m_closed)
            return;
        // The first pane is this window's own: its quantity, then a pane for each other.
        const auto select = [](vtkWindowCatalogue3D *w, const QString &name) {
            if (!w || !w->comboMapColor || name.isEmpty())
                return;
            const int column = w->rawHeaders.indexOf(name);
            const int item = column < 0 ? -1 : w->comboMapColor->findData(u"raw:%1"_s.arg(column));
            if (item >= 0)
                w->comboMapColor->setCurrentIndex(item);
        };
        const auto paneCount = [this]() {
            int n = 0;
            for (const QPointer<vtkWindowCatalogue3D> &w : this->m_paneWindows)
                n += w ? 1 : 0;
            return std::max(n, 1);
        };
        // The panes are rebuilt one after another, and the saved layout is only complete at
        // the end: until then nothing overwrites it.
        this->m_restoringPanes = true;
        select(this, session.panes.first().quantity);
        QList<QPointer<vtkWindowCatalogue3D>> restored{ this };   // in session order
        for (int i = 1; i < session.panes.size(); ++i) {
            const int before = paneCount();
            this->openQuantityComparison();
            if (paneCount() <= before)
                break;
            restored << this->activePaneWindow();
            select(this->activePaneWindow(), session.panes[i].quantity);
        }
        const int made = static_cast<int>(restored.size()) - 1;
        if (made > 0) {
            // What the session kept besides the quantities: the layout, which pane was
            // active, and which panes were unlinked (the first one too).
            if (session.count > this->m_paneCount)
                this->setPaneCount(session.count);
            for (int i = 0; i < restored.size(); ++i)
                if (restored[i] && !session.panes[i].linked)
                    restored[i]->setComparisonLinked(false);
            if (session.active >= 0 && session.active < restored.size() && restored[session.active]) {
                const int slot = this->paneSlotOf(restored[session.active]);
                if (slot >= 0)
                    this->setActivePane(slot);
            }
            this->statusBar()->showMessage(
                    u"Comparison of your last session restored: %1 panes. Close the ones you do not need."_s
                            .arg(made + 1),
                    10000);
        }
        this->m_restoringPanes = false;
        this->refreshPaneProducts();
        this->refreshPaneHeaders();
        this->savePaneSession();
    });
    timer->start(500);
}

void vtkWindowCatalogue3D::rebuildPanesFor(const QList<QPointer<vtkWindowCatalogue3D>> &windows,
                                           const QList<int> &paneSlots, int count, int active)
{
    // A timeline step replaced every window of the group: this new host takes the
    // new windows into the slots their predecessors had, in the layout they had.
    this->ensurePaneGrid();
    if (!this->m_paneGrid)
        return;
    // The host sits where the old host sat; the others follow into theirs.
    int hostSlot = 0;
    for (int i = 0; i < windows.size() && i < paneSlots.size(); ++i)
        if (windows.at(i) == this && paneSlots.at(i) >= 0 && paneSlots.at(i) < kSlots)
            hostSlot = paneSlots.at(i);
    this->m_paneWindows = {};
    this->m_paneWindows[hostSlot] = this;
    for (int i = 0; i < windows.size() && i < paneSlots.size(); ++i) {
        vtkWindowCatalogue3D *w = windows.at(i);
        const int slot = paneSlots.at(i);
        if (w && w != this && slot >= 0 && slot < kSlots)
            this->addPaneWindow(w, slot);   // a step's windows start hidden: the grid shows them
    }
    this->setPaneCount(count);
    // The pane that was active, wherever the layout put it now.
    int activeSlot = std::clamp(active, 0, kSlots - 1);
    for (int i = 0; i < windows.size() && i < paneSlots.size(); ++i)
        if (paneSlots.at(i) == active && windows.at(i))
            if (const int moved = this->paneSlotOf(windows.at(i)); moved >= 0)
                activeSlot = moved;
    this->setActivePane(activeSlot);
    this->refreshPaneProducts();
}
