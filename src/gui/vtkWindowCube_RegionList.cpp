// The cube viewer's region list (issue #341) — partial class of vtkWindowCube.
//
// What the user keeps: every region drawn, imported, restored from Session
// Data or typed in, until they delete it. Drawing still runs through the draft
// members of _Region.cpp (regionMode, regionAnchorVoxel, …); a finished draft
// joins m_regions and the draft is cleared. Everything here draws, selects,
// edits and analyses the list, on the 2-D slice and on the moment window, with
// the image viewer's model, dialog and rules (RegionModel.h,
// RegionPropertiesDialog, RegionListOverlay).
//
// Coordinates. The list is in the cube's full-resolution 0-based pixels. The
// 2-D slice and the moment window are served at full resolution (origin 0,
// spacing 1), so a region is drawn there at its own numbers; the displayed
// grid is still read from each image (regionDisplayGrid) so a reduced image
// would be measured right. The 3-D view's preview is reached through the
// display sampling (cubeToDisplay / displayToCube, CubeLodGeometry::Sampling).

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "Pane2DView.h"
#include <vtkLookupTable.h>
#include "ProductOrigin.h"
#include "ProfileWidget.h"
#include "RegionCubeSampling.h"
#include <QVTKOpenGLNativeWidget.h>
#include "RegionFullRes.h"
#include "RegionListExport.h"
#include "RegionPropertiesDialog.h"
#include "RegionSessionController.h"
#include "WorkspaceChrome.h"

#include <QVTKOpenGLNativeWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkTrivialProducer.h>

#include <QAction>
#include <QCursor>
#include <QDockWidget>   // WorkspaceChrome.h holds a QPointer to one
#include <QGuiApplication>
#include <QJsonObject>
#include <QKeySequence>
#include <QMenu>
#include <QPointer>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace Qt::StringLiterals;
using visivo::regions::Hit;
using visivo::regions::Region;
using visivo::regions::Vec2;

namespace {

/// How far the rotation knob stands off a box or an ellipse, and how close a
/// press has to land on a handle or an outline, in screen pixels (as in the
/// image viewer).
constexpr double kKnobPx = 22.0;
constexpr double kPickPx = 7.0;

struct ListRegionStatistics
{
    int totalCount{ 0 };
    int validCount{ 0 };
    int blankedCount{ 0 };
    double minValue{ 0. };
    double maxValue{ 0. };
    double mean{ 0. };
    double median{ 0. };
    double stddev{ 0. };
    bool valid{ false };
};

/// The statistics of the displayed pixels of @p image whose centres are in
/// @p region (the backend's _region_mask rules, RegionModel::contains).
ListRegionStatistics statisticsOnImage(vtkImageData *image, const visivo::DisplayGrid &grid,
                                       const Region &region)
{
    ListRegionStatistics stats;
    if (!image)
        return stats;
    int extent[6];
    image->GetExtent(extent);
    const auto b = visivo::displayedVoxelBounds(region, grid);
    std::vector<double> values;
    double sum = 0.0, sumSq = 0.0;
    stats.minValue = std::numeric_limits<double>::infinity();
    stats.maxValue = -std::numeric_limits<double>::infinity();
    for (int y = std::max(extent[2], b[2]); y <= std::min(extent[3], b[3]); ++y) {
        for (int x = std::max(extent[0], b[0]); x <= std::min(extent[1], b[1]); ++x) {
            if (!visivo::displayedVoxelIn(region, grid, x, y))
                continue;
            ++stats.totalCount;
            const double v = image->GetScalarComponentAsDouble(x, y, extent[4], 0);
            if (!std::isfinite(v)) {
                ++stats.blankedCount;
                continue;
            }
            ++stats.validCount;
            stats.minValue = std::min(stats.minValue, v);
            stats.maxValue = std::max(stats.maxValue, v);
            sum += v;
            sumSq += v * v;
            values.push_back(v);
        }
    }
    if (stats.validCount <= 0)
        return stats;
    stats.mean = sum / stats.validCount;
    stats.stddev = std::sqrt(std::max(0.0, sumSq / stats.validCount - stats.mean * stats.mean));
    std::sort(values.begin(), values.end());
    const int mid = stats.validCount / 2;
    stats.median = (stats.validCount % 2 == 0) ? 0.5 * (values[mid - 1] + values[mid]) : values[mid];
    stats.valid = true;
    return stats;
}

}   // namespace

// ── Geometry of the panes ────────────────────────────────────────────────────

int vtkWindowCube::regionPaneSlot(vtkRenderWindow *win) const
{
    if (!win)
        return -1;
    for (int i = 0; i < 4; ++i) {
        Pane2DView *pv = m_paneViews[i];
        if (pv && pv->renderer() && pv->renderer()->GetRenderWindow() == win)
            return i;
    }
    return -1;
}

std::optional<visivo::regions::MapPlacement>
vtkWindowCube::regionPlacement(vtkRenderWindow *win) const
{
    if (!win)
        return std::nullopt;
    vtkImageData *image = nullptr;
    std::optional<std::array<int, 2>> crop;
    bool skyMap = false;
    if (win == this->sliceWin.GetPointer()) {
        image = vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
    } else if (win == this->momentWin.GetPointer()) {
        image = vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    } else {
        const int slot = this->regionPaneSlot(win);
        if (slot < 0 || m_slots[slot].view.kind != ViewId::Product)
            return std::nullopt;
        const auto it = m_products2d.constFind(m_slots[slot].view.productId);
        if (it == m_products2d.constEnd() || !m_paneViews[slot])
            return std::nullopt;
        // The pane's own image: the one it draws, whatever the cache holds now.
        image = m_paneViews[slot]->imageData();
        if (it->hasRegionOrigin)
            crop = std::array<int, 2>{ it->regionOrigin.x(), it->regionOrigin.y() };
        skyMap = true;
    }
    if (!image)
        return std::nullopt;
    double origin[3], spacing[3];
    int extent[6];
    image->GetOrigin(origin);
    image->GetSpacing(spacing);
    image->GetExtent(extent);
    // extent[2], not extent[1]: the Y index has bitten before.
    const std::array<int, 2> extent0{ extent[0], extent[2] };
    if (!skyMap)
        return visivo::regions::worldIsCube({ origin[0], origin[1] }, { spacing[0], spacing[1] },
                                            extent0);
    return visivo::regions::skyMapPlacement(
            { extent[1] - extent[0] + 1, extent[3] - extent[2] + 1 }, { origin[0], origin[1] },
            { spacing[0], spacing[1] }, extent0,
            { this->remoteDatasetWidth, this->remoteDatasetHeight }, crop);
}

bool vtkWindowCube::isRegionMapPane(int slot) const
{
    if (slot < 0 || slot > 3 || !m_paneViews[slot] || m_slots[slot].view.kind != ViewId::Product
        || !m_slots[slot].frame || !m_slots[slot].frame->isVisible()
        || m_paneProductId[slot] != m_slots[slot].view.productId)
        return false;
    auto *ren = m_paneViews[slot]->renderer();
    return ren && this->regionPlacement(ren->GetRenderWindow()).has_value();
}

bool vtkWindowCube::anyRegionMapPaneVisible() const
{
    for (int i = 0; i < 4; ++i)
        if (this->isRegionMapPane(i))
            return true;
    return false;
}

bool vtkWindowCube::anyRegionToolArmed() const
{
    const auto armed = [](const QAction *a) { return a && a->isChecked(); };
    return armed(actionBoxRegion.data()) || armed(actionCircleRegion.data())
            || armed(actionPolygonRegion.data()) || armed(actionAnnulusRegion.data());
}

QWidget *vtkWindowCube::regionViewWidget(vtkRenderWindow *win) const
{
    if (!win)
        return nullptr;
    if (win == this->sliceWin.GetPointer())
        return ui->vtkImage;
    if (win == this->momentWin.GetPointer())
        return m_vtkMoment.data();
    const int slot = this->regionPaneSlot(win);
    return slot >= 0 ? static_cast<QWidget *>(m_paneViews[slot]->widget()) : nullptr;
}

vtkImageData *vtkWindowCube::regionPaneImage(vtkRenderWindow *win) const
{
    if (!this->regionPlacement(win))
        return nullptr;
    if (win == this->sliceWin.GetPointer())
        return vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
    if (win == this->momentWin.GetPointer())
        return vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    const int slot = this->regionPaneSlot(win);
    return slot >= 0 ? m_paneViews[slot]->imageData() : nullptr;
}

visivo::DisplayGrid vtkWindowCube::regionDisplayGrid(vtkRenderWindow *win) const
{
    const auto placement = this->regionPlacement(win);
    return placement ? placement->displayGrid() : visivo::DisplayGrid{};
}

bool vtkWindowCube::regionDisplayToWorld(vtkRenderWindow *win, int displayX, int displayY,
                                         Vec2 *world) const
{
    auto *renderer = win ? win->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!renderer)
        return false;
    vtkNew<vtkCoordinate> c;
    c->SetCoordinateSystemToDisplay();
    c->SetValue(displayX, displayY);
    const double *w = c->GetComputedWorldValue(renderer);
    if (!w)
        return false;
    *world = { w[0], w[1] };
    return true;
}

bool vtkWindowCube::regionDisplayToList(vtkRenderWindow *win, int displayX, int displayY,
                                        Vec2 *list) const
{
    const auto placement = this->regionPlacement(win);
    Vec2 w{};
    if (!placement || !this->regionDisplayToWorld(win, displayX, displayY, &w))
        return false;
    *list = placement->toCube(w[0], w[1]);
    return true;
}

double vtkWindowCube::regionWorldPerDisplayPixel(vtkRenderWindow *win) const
{
    Vec2 a{}, b{};
    if (!this->regionDisplayToWorld(win, 0, 0, &a) || !this->regionDisplayToWorld(win, 100, 0, &b))
        return 1.0;
    const double d = std::hypot(b[0] - a[0], b[1] - a[1]) / 100.0;
    return d > 0.0 && std::isfinite(d) ? d : 1.0;
}

double vtkWindowCube::regionListPerDisplayPixel(vtkRenderWindow *win) const
{
    const auto placement = this->regionPlacement(win);
    const double perWorld = placement ? placement->cubePerWorld() : 1.0;
    const double d = this->regionWorldPerDisplayPixel(win) * perWorld;
    return d > 0.0 && std::isfinite(d) ? d : 1.0;
}

vtkRenderWindow *vtkWindowCube::regionDraftWin() const
{
    // A Product pane the draft was started on, while it still shows a sky map;
    // the slice, where the drawing tools have always drawn, when the draft is
    // its own. A draft never moves to another view: its voxels are counted on
    // the grid it started on, so a pane that left the screen or changed map
    // gives null and the draft is dropped (regionDraftOrphaned).
    if (m_regionDraftWin && m_regionDraftWin != this->sliceWin.GetPointer()) {
        const int slot = this->regionPaneSlot(m_regionDraftWin);
        if (m_regionDraftWin == this->momentWin.GetPointer() || this->isRegionMapPane(slot))
            return m_regionDraftWin;
        return nullptr;
    }
    return this->active2dWin();
}

bool vtkWindowCube::regionDraftOrphaned() const
{
    return m_regionDraftWin && this->regionDraftWin() == nullptr;
}

vtkRenderWindow *vtkWindowCube::regionAnalysisWin() const
{
    const auto paneWin = [this](int slot) -> vtkRenderWindow * {
        if (slot < 0 || slot > 3)
            return nullptr;
        if (m_slots[slot].view.kind == ViewId::Slice2D && m_slots[slot].frame
            && m_slots[slot].frame->isVisible())
            return this->sliceWin.GetPointer();
        if (this->isRegionMapPane(slot))
            return m_paneViews[slot]->renderer()->GetRenderWindow();
        return nullptr;
    };
    // The pane the user is working in decides.
    if (vtkRenderWindow *w = paneWin(m_activePane))
        return w;
    // Then the view the region was last drawn or handled in, while on screen.
    if (m_regionFocusWin) {
        if (m_regionFocusWin == this->sliceWin.GetPointer() && this->slice2dVisible())
            return m_regionFocusWin;
        if (vtkRenderWindow *w = paneWin(this->regionPaneSlot(m_regionFocusWin)))
            return w;
    }
    if (this->slice2dVisible())
        return this->sliceWin.GetPointer();
    for (int i = 0; i < 4; ++i)
        if (vtkRenderWindow *w = paneWin(i))
            return w;
    return nullptr;
}

// ── Draft → list ─────────────────────────────────────────────────────────────

std::optional<Region> vtkWindowCube::draftRegion() const
{
    if (this->regionMode == RegionMode::None || !this->regionValid)
        return std::nullopt;
    QString shape;
    switch (this->regionMode) {
    case RegionMode::Box:     shape = u"box"_s; break;
    case RegionMode::Circle:  shape = u"circle"_s; break;
    case RegionMode::Polygon: shape = u"polygon"_s; break;
    case RegionMode::Annulus: shape = u"annulus"_s; break;
    case RegionMode::None:    return std::nullopt;
    }
    // Drawn in the voxels of the pane shown: those are what the anchor, the
    // current point and the vertices count.
    return visivo::regionFromDisplayed(shape, this->regionDisplayGrid(this->regionDraftWin()),
                                       this->regionAnchorVoxel, this->regionCurrentVoxel,
                                       this->regionPolygonVertices, this->regionAnnulusInnerRadius);
}

int vtkWindowCube::commitDraftRegion()
{
    const auto draft = this->draftRegion();
    this->clearRegion();
    if (!draft) {
        this->statusBar()->showMessage(
                u"The region has no area (drag to give it a size); nothing was added."_s, 4000);
        this->onRegionListChanged();
        return -1;
    }
    const int id = this->m_regions.add(*draft);
    this->onRegionListChanged();
    return id;
}

// ── Setup ────────────────────────────────────────────────────────────────────

void vtkWindowCube::setupRegionList()
{
    for (auto *win : { this->sliceWin.GetPointer(), this->momentWin.GetPointer() }) {
        auto *renderer = win ? win->GetRenderers()->GetFirstRenderer() : nullptr;
        if (!renderer)
            continue;
        (win == this->sliceWin.GetPointer() ? this->m_sliceRegionOverlay
                                            : this->m_momentRegionOverlay)
                .attach(renderer);
        // The rotation knob stands a fixed distance off the edge ON SCREEN: a
        // zoom has to move it, and the hit test reads the distance it was drawn at.
        if (auto *camera = renderer->GetActiveCamera())
            camera->AddObserver(vtkCommand::ModifiedEvent, this, &vtkWindowCube::onRegionCameraChanged);
    }
    // The cutting plane of the 3-D view (#343): drawn, not edited there. The
    // lines are pulled towards the camera so the textured plane they lie on
    // never hides them; no fill, which would tint the slice under it.
    if (auto *cubeWin = ui->vtkCube ? ui->vtkCube->renderWindow() : nullptr) {
        auto *renderers = cubeWin->GetRenderers();
        renderers->InitTraversal();
        while (vtkRenderer *r = renderers->GetNextItem()) {
            if (r->HasViewProp(this->remoteCuttingPlaneActor)) {
                RegionListOverlay::Style style;
                style.fill = false;
                style.lineOffsetUnits = 4.0;
                this->m_planeRegionOverlay.attach(r, style);
                break;
            }
        }
    }
    // Pressing on a region selects it, dragging moves or reshapes it: such a
    // press must neither pan the slice, nor pin the probe, nor start a new
    // region under it. The callback runs ahead of the interactor style and the
    // tools' observers (higher priority) and aborts the events it uses.
    this->m_regionEditCommand = vtkSmartPointer<vtkCallbackCommand>::New();
    this->m_regionEditCommand->SetClientData(this);
    this->m_regionEditCommand->SetCallback(&vtkWindowCube::regionEditCallback);
    for (auto *win : { this->sliceWin.GetPointer(), this->momentWin.GetPointer() }) {
        if (!win || !win->GetInteractor())
            continue;
        for (unsigned long event : { vtkCommand::LeftButtonPressEvent, vtkCommand::MouseMoveEvent,
                                     vtkCommand::LeftButtonReleaseEvent,
                                     vtkCommand::LeftButtonDoubleClickEvent,
                                     vtkCommand::RightButtonPressEvent })
            win->GetInteractor()->AddObserver(event, this->m_regionEditCommand, 10.0f);
    }
    this->refreshRegionListOverlay();
}

void vtkWindowCube::setupRegionListActions()
{
    const auto make = [this](const QString &text, const QString &tip) {
        auto *a = new QAction(text, this);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        a->setEnabled(false);
        return a;
    };
    m_actRegionProperties = make(u"Region Properties…"_s,
            u"Set the selected region by numbers: centre, sizes, angle or vertices, in image "
            u"pixels or a sky frame (double-clicking a region does the same)."_s);
    m_actAnalyseRegion = make(u"Analyse Region"_s,
            u"Statistics of the selected region on the slice shown, and its mean spectrum "
            u"over the cube."_s);
    m_actDeleteRegion = make(u"Delete Region"_s,
            u"Remove the selected region (Delete or Backspace on the slice)."_s);
    m_actUndoRegion = make(u"Undo Region Edit"_s,
            u"Undo the last change to this window's regions: draw, move, reshape, delete "
            u"(Cmd+Z on the slice)."_s);
    m_actRedoRegion = make(u"Redo Region Edit"_s,
            u"Redo the region change just undone (Shift+Cmd+Z on the slice)."_s);
    // Delete acts while a 2-D view has the focus, so it never takes the key
    // away from a text field or a table elsewhere in the window. Undo and Redo
    // have no shortcut of their own: Cmd+Z already undoes the copilot's display
    // changes window-wide, and two actions on one key would both stay silent.
    // eventFilter() gives the key to the region list while a 2-D view has the
    // focus and the list has something to undo.
    m_actDeleteRegion->setShortcuts({ QKeySequence::Delete, QKeySequence(Qt::Key_Backspace) });
    m_actDeleteRegion->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    for (QWidget *w : { static_cast<QWidget *>(ui->vtkImage), static_cast<QWidget *>(m_vtkMoment.data()) })
        if (w)
            w->addAction(m_actDeleteRegion);
    QObject::connect(m_actRegionProperties, &QAction::triggered, this,
                     [this]() { this->openRegionProperties(); });
    QObject::connect(m_actAnalyseRegion, &QAction::triggered, this, [this]() {
        // On the focused 2-D view when it shows the sky, a moment map included
        // (#355): measured where it is, never turned into the slice first.
        vtkRenderWindow *win = this->regionAnalysisWin();
        if (!win) {
            this->statusBar()->showMessage(
                    u"Show the 2D Slice or a moment map in a pane to measure the region."_s, 5000);
            return;
        }
        this->analyzeRegion(-1, win);
    });
    QObject::connect(m_actDeleteRegion, &QAction::triggered, this,
                     &vtkWindowCube::deleteSelectedRegion);
    QObject::connect(m_actUndoRegion, &QAction::triggered, this, &vtkWindowCube::undoRegionEdit);
    QObject::connect(m_actRedoRegion, &QAction::triggered, this, &vtkWindowCube::redoRegionEdit);
}

// ── Drawing the list ─────────────────────────────────────────────────────────

void vtkWindowCube::onRegionListChanged()
{
    // Shown before the change: the last region deleted still needs the 3-D
    // view drawn again, to take it off.
    const bool planeWasShown = this->m_planeRegionOverlay.visible();
    this->refreshRegionListOverlay();
    this->applyMomentToolGate();   // Properties / Analyse / Delete / Undo / Export

    // The properties dialog follows the selection, and goes when its region does.
    if (m_regionPropertiesDialog) {
        if (const Region *sel = this->m_regions.selected())
            m_regionPropertiesDialog->setRegion(*sel);
        else if (const Region *shown = this->m_regions.find(m_regionPropertiesDialog->regionId()))
            m_regionPropertiesDialog->setRegion(*shown);
        else
            m_regionPropertiesDialog->close();
    }
    for (auto *win : { this->sliceWin.GetPointer(), this->momentWin.GetPointer() })
        if (win && win->GetInteractor() && win->GetInteractor()->GetInitialized())
            win->Render();
    this->renderRegionPanes();
    // The Remote 3-D view's session draws them on its own plane (#356): sent
    // once per change too, and only when the drawing changed.
    this->pushRegionsToRemote();
    // The 3-D view too when the plane shows them: once per change, not per
    // mouse move of a drag (a volume render is not free).
    if ((planeWasShown || this->m_planeRegionOverlay.visible()) && ui->vtkCube
        && ui->vtkCube->renderWindow()
        && ui->vtkCube->isVisible())
        ui->vtkCube->renderWindow()->Render();
    this->refreshRegionSession();
}

void vtkWindowCube::refreshRegionSession()
{
    if (!m_regionSession && m_workspaceChrome && m_workspaceChrome->sessionTree()) {
        RegionSessionController::Hooks hooks;
        hooks.editable = [this]() { return !this->regionDragRunning(); };
        hooks.changed = [this]() { this->onRegionListChanged(); };
        hooks.properties = [this](int id) { this->openRegionProperties(id); };
        hooks.analyse = [this](int) {
            // Through the action, as the region's own menu does: its gate and
            // its "show the slice first" step. The region is selected already.
            if (m_actAnalyseRegion && m_actAnalyseRegion->isEnabled())
                m_actAnalyseRegion->trigger();
            else
                this->statusBar()->showMessage(
                        u"Show the 2D Slice or a moment map in a pane to measure the region."_s, 5000);
        };
        hooks.exportRegions = [this](const std::vector<int> &ids) {
            visivo::regions::exportRegionList(
                    this, this->m_regions,
                    { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
                      this->remoteDatasetId, this->filepath },
                    ids);
        };
        m_regionSession = new RegionSessionController(this, m_workspaceChrome->sessionTree(),
                                                      &this->m_regions, std::move(hooks));
    }
    if (m_regionSession)
        m_regionSession->refresh();
}

void vtkWindowCube::refreshRegionListOverlay()
{
    // The list is in full-resolution pixels, which is the world frame of both
    // 2-D renderers (origin 0, spacing 1): no transform. The knob distance the
    // hit test will use is the one drawn now, per pane (each has its own zoom).
    this->m_regionKnobOffset[0] = kKnobPx * this->regionWorldPerDisplayPixel(this->sliceWin);
    this->m_regionKnobOffset[1] = kKnobPx * this->regionWorldPerDisplayPixel(this->momentWin);
    this->m_sliceRegionOverlay.update(this->m_regions, this->m_regionKnobOffset[0]);
    this->m_momentRegionOverlay.update(this->m_regions, this->m_regionKnobOffset[1]);
    this->refreshPaneRegionOverlays();
    this->refreshPlaneRegionOverlay();
}

void vtkWindowCube::refreshPaneRegionOverlays()
{
    for (int i = 0; i < 4; ++i) {
        Pane2DView *pv = m_paneViews[i];
        if (!pv || !pv->renderer())
            continue;
        // Hooked once the edit callback and the Delete action exist: a pane
        // seen earlier would be left with neither.
        if (m_paneRegionHooked[i] != pv && m_regionEditCommand && m_actDeleteRegion) {
            // First time this view is seen: the overlays go on its renderer and
            // the edit callback on its interactor, ahead of its interactor
            // style, whose LEFT drag is window/level: a press the regions use
            // is aborted so the contrast does not move with it.
            m_paneRegionOverlays[i].detach();
            m_paneDraftOverlays[i].detach();
            m_paneRegionOverlays[i].attach(pv->renderer());
            RegionListOverlay::Style draft;
            draft.lineWidth = 2.0;
            draft.selectedLineWidth = 2.0;
            m_paneDraftOverlays[i].attach(pv->renderer(), draft);
            vtkRenderWindow *win = pv->renderer()->GetRenderWindow();
            if (win && win->GetInteractor())
                for (unsigned long event :
                     { vtkCommand::LeftButtonPressEvent, vtkCommand::MouseMoveEvent,
                       vtkCommand::LeftButtonReleaseEvent, vtkCommand::LeftButtonDoubleClickEvent,
                       vtkCommand::RightButtonPressEvent })
                    win->GetInteractor()->AddObserver(event, m_regionEditCommand, 10.0f);
            if (auto *camera = pv->renderer()->GetActiveCamera())
                camera->AddObserver(vtkCommand::ModifiedEvent, this,
                                    &vtkWindowCube::onRegionCameraChanged);
            // Delete while the pane has the focus, as on the slice.
            if (pv->widget())
                pv->widget()->addAction(m_actDeleteRegion);
            m_paneRegionHooked[i] = pv;
        }
        vtkRenderWindow *win = pv->renderer()->GetRenderWindow();
        const auto placement = this->regionPlacement(win);
        // Not tied to the frame being visible: a layout pass mounts a pane
        // before it shows its frame, and nothing redraws the list after that.
        const bool shown = placement && m_slots[i].view.kind == ViewId::Product
                && m_paneProductId[i] == m_slots[i].view.productId;
        m_paneRegionOverlays[i].setVisible(shown && !this->m_regions.isEmpty());
        if (!shown) {
            m_paneDraftOverlays[i].setVisible(false);
            continue;
        }
        const visivo::regions::MapPlacement place = *placement;
        const auto toWorld = [place](const Vec2 &p) { return place.toWorld(p); };
        m_paneKnobOffset[i] = kKnobPx * this->regionListPerDisplayPixel(win);
        m_paneRegionOverlays[i].update(this->m_regions, m_paneKnobOffset[i], toWorld);
    }
    if (this->regionDraftOrphaned())
        this->clearRegion();   // its pane left the screen or shows another map
    this->refreshRegionOverlay();   // the draft, wherever it is drawn
}

void vtkWindowCube::renderRegionPanes()
{
    for (int i = 0; i < 4; ++i)
        if (m_paneViews[i] && m_paneRegionHooked[i] == m_paneViews[i] && m_slots[i].frame
            && m_slots[i].frame->isVisible() && m_slots[i].view.kind == ViewId::Product)
            m_paneViews[i]->render();
}

void vtkWindowCube::refreshPlaneRegionOverlay()
{
    if (!this->m_planeRegionOverlay.attached())
        return;
    // With the plane: hidden with it, and before it has a channel.
    const bool shown = this->m_planeChannel >= 0 && this->remoteCuttingPlaneActor
            && this->remoteCuttingPlaneActor->GetVisibility();
    this->m_planeRegionOverlay.setVisible(shown && !this->m_regions.isEmpty());
    if (!shown)
        return;
    const int channel = this->m_planeChannel;
    const CubeLodGeometry::Sampling sampling = this->m_displaySampling;
    this->m_planeRegionOverlay.update(
            this->m_regions, 0.0,
            [channel, sampling](const Vec2 &p) {
                return visivo::regions::planePoint(p, channel, sampling);
            },
            /*withHandles=*/false);
}

void vtkWindowCube::onRegionCameraChanged(vtkObject *, unsigned long, void *)
{
    if (this->m_regions.selected() == nullptr || m_regionDrag.active)
        return;
    // Pans and clipping-range updates also modify the camera; only a change of
    // scale moves the knob.
    const double slice = kKnobPx * this->regionWorldPerDisplayPixel(this->sliceWin);
    const double moment = kKnobPx * this->regionWorldPerDisplayPixel(this->momentWin);
    const auto moved = [](double now, double drawn) {
        return std::abs(now - drawn) > 1e-3 * std::max(now, 1e-12);
    };
    bool paneMoved = false;
    for (int i = 0; i < 4 && !paneMoved; ++i)
        if (this->isRegionMapPane(i))
            paneMoved = moved(kKnobPx
                                      * this->regionListPerDisplayPixel(
                                              m_paneViews[i]->renderer()->GetRenderWindow()),
                              m_paneKnobOffset[i]);
    if (moved(slice, m_regionKnobOffset[0]) || moved(moment, m_regionKnobOffset[1]) || paneMoved)
        this->refreshRegionListOverlay();
}

// ── Editing ──────────────────────────────────────────────────────────────────

bool vtkWindowCube::regionEditingAllowed(vtkRenderWindow *win) const
{
    // Every armed tool that owns the next click keeps it: the probe (live or
    // pinned), a PV path, the lasso's 2-D mask edit, and a polygon being drawn
    // (its clicks are vertices). A region drawing tool does not: a press on a
    // region edits it, a press on empty slice draws a new one.
    return !this->m_regions.isEmpty() && !this->isBusy() && !this->probeModeActive
            && !this->pvModeActive && !this->m_lassoEditActive
            && !(this->regionMode != RegionMode::None && this->regionDragging)
            && this->regionPaneImage(win) != nullptr;
}

void vtkWindowCube::regionEditCallback(vtkObject *caller, unsigned long eventId, void *clientData,
                                       void *)
{
    auto *self = static_cast<vtkWindowCube *>(clientData);
    if (!self)
        return;
    vtkRenderWindow *win = nullptr;
    for (auto *w : { self->sliceWin.GetPointer(), self->momentWin.GetPointer() })
        if (w && w->GetInteractor() == caller)
            win = w;
    int paneSlot = -1;
    if (!win) {
        for (int i = 0; i < 4; ++i) {
            Pane2DView *pv = self->m_paneViews[i];
            auto *w = pv && pv->renderer() ? pv->renderer()->GetRenderWindow() : nullptr;
            if (w && w->GetInteractor() == caller) {
                win = w;
                paneSlot = i;
            }
        }
    }
    if (!win)
        return;
    if (self->handleRegionEditEvent(win, eventId)) {
        self->m_regionEditCommand->SetAbortFlag(1);   // nobody after us sees this event
        return;
    }
    // A Product pane has no tool observers of its own: the drawing tools take
    // their presses, moves and releases here (the slice's come through
    // toggleProbeFreeze / mouseCallback / finishRegionInteraction).
    if (paneSlot < 0 || self->regionMode == RegionMode::None || !self->isRegionMapPane(paneSlot))
        return;
    auto *iren = win->GetInteractor();
    const int *pos = iren ? iren->GetEventPosition() : nullptr;
    if (!pos)
        return;
    if (eventId == vtkCommand::LeftButtonPressEvent) {
        self->regionDraftPress(win, pos[0], pos[1]);
        self->m_regionEditCommand->SetAbortFlag(1);
    } else if (eventId == vtkCommand::MouseMoveEvent) {
        self->regionDraftMove(win, pos[0], pos[1]);
    } else if (eventId == vtkCommand::LeftButtonReleaseEvent) {
        if (self->regionDraftWin() == win && !self->isBusy() && !self->m_lassoEditActive)
            self->finishRegionDraft();
        self->m_regionEditCommand->SetAbortFlag(1);
    }
}

bool vtkWindowCube::handleRegionEditEvent(vtkRenderWindow *win, unsigned long eventId)
{
    // The release ends a drag wherever it lands, even where no world
    // position can be read: a drag left running would block Delete and Undo.
    if (eventId == vtkCommand::LeftButtonReleaseEvent) {
        if (!m_regionDrag.active)
            return false;
        m_regionDrag.active = false;
        this->m_regions.endGesture();
        this->onRegionListChanged();
        return true;
    }
    auto *iren = win->GetInteractor();
    const int *pos = iren ? iren->GetEventPosition() : nullptr;
    Vec2 p{};
    if (!pos || !this->regionDisplayToList(win, pos[0], pos[1], &p))
        return false;

    if (eventId == vtkCommand::MouseMoveEvent) {
        if (!m_regionDrag.active || m_regionDrag.window != win)
            return false;
        // The release can be lost (a modal box raised mid-drag takes it): with
        // no button down the drag is over, or hovering would carry the region.
        if (!(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
            m_regionDrag.active = false;
            this->m_regions.endGesture();
            this->onRegionListChanged();
            return false;
        }
        const double dx = p[0] - m_regionDrag.start[0], dy = p[1] - m_regionDrag.start[1];
        const Region next =
                m_regionDrag.hit.kind == Hit::Body
                        ? visivo::regions::moved(m_regionDrag.original, dx, dy)
                        : visivo::regions::withHandleAt(m_regionDrag.original,
                                                        m_regionDrag.hit.handle, p);
        this->m_regions.replace(next);
        if (m_regionDrag.hit.kind == Hit::Body)
            for (const Region &ring : m_regionDrag.companions)
                this->m_regions.replace(visivo::regions::moved(ring, dx, dy));
        this->refreshRegionListOverlay();
        if (m_regionPropertiesDialog)
            m_regionPropertiesDialog->setRegion(next);
        win->Render();
        return true;
    }
    if (!this->regionEditingAllowed(win))
        return false;
    // The knob distance each view drew the selection with, in list pixels.
    const int paneSlot = this->regionPaneSlot(win);
    const double knobOffset = paneSlot >= 0 ? m_paneKnobOffset[paneSlot]
            : this->m_regionKnobOffset[win == this->sliceWin.GetPointer() ? 0 : 1];
    const double tolerance = kPickPx * this->regionListPerDisplayPixel(win);
    Hit hit;
    const int id = this->m_regions.regionAt(p, tolerance, knobOffset, &hit);
    // A catalogue source under the cursor keeps its click: it is selected by
    // clicking on it, and a region drawn around it must not swallow that.
    if (id >= 0 && hit.kind == Hit::Body && this->catalogueOverlayLoaded
        && paneSlot < 0
        && this->catalogueSourceIndexNearDisplayPosition(pos[0], pos[1],
                                                         win->GetRenderers()->GetFirstRenderer())
                   >= 0)
        return false;

    if (eventId == vtkCommand::LeftButtonPressEvent) {
        if (id < 0) {
            // Empty slice: let go of the selection, and let the click do what it
            // does (pan, or start a new region with a drawing tool armed).
            if (this->m_regions.selectedId() != -1) {
                this->m_regions.select(-1);
                this->onRegionListChanged();
            }
            return false;
        }
        this->m_regions.select(id);
        m_regionFocusWin = win;
        m_regionDrag.active = true;
        m_regionDrag.window = win;
        m_regionDrag.hit = hit;
        m_regionDrag.start = p;
        m_regionDrag.original = *this->m_regions.find(id);
        m_regionDrag.companions.clear();
        for (int ring : this->m_regions.ringGroup(id))
            if (ring != id)
                m_regionDrag.companions.push_back(*this->m_regions.find(ring));
        this->m_regions.beginGesture();
        this->onRegionListChanged();
        // So Delete and Undo reach the view the region was picked in.
        if (QWidget *w = this->regionViewWidget(win))
            w->setFocus(Qt::MouseFocusReason);
        return true;
    }
    if (eventId == vtkCommand::LeftButtonDoubleClickEvent) {
        if (id < 0)
            return false;
        this->m_regions.select(id);
        m_regionFocusWin = win;
        this->onRegionListChanged();
        this->openRegionProperties(id);
        return true;
    }
    if (eventId == vtkCommand::RightButtonPressEvent) {
        if (id < 0)
            return false;
        this->m_regions.select(id);
        m_regionFocusWin = win;
        this->onRegionListChanged();
        // Out of the VTK callback: a menu runs its own event loop.
        QPointer<vtkWindowCube> self(this);
        QTimer::singleShot(0, this, [self, id]() {
            if (self)
                self->showRegionContextMenu(id);
        });
        return true;
    }
    return false;
}

void vtkWindowCube::showRegionContextMenu(int id)
{
    const Region *r = this->m_regions.find(id);
    if (!r)
        return;
    // No parent: a stack menu owned by the window would be deleted twice if the
    // window went away while the menu runs its loop.
    QMenu menu;
    QAction *props = menu.addAction(u"Properties…"_s);
    QAction *analyse = menu.addAction(u"Analyse"_s);
    analyse->setEnabled(m_actAnalyseRegion && m_actAnalyseRegion->isEnabled());
    menu.addSeparator();
    QAction *remove = menu.addAction(u"Delete"_s);
    menu.setTitle(r->name);
    QPointer<vtkWindowCube> self(this);
    QAction *chosen = menu.exec(QCursor::pos());
    if (!self || !chosen || !this->m_regions.find(id))
        return;
    this->m_regions.select(id);
    if (chosen == props) {
        this->openRegionProperties(id);
    } else if (chosen == analyse) {
        this->onRegionListChanged();
        // Through the action: its gate and its "show the slice first" step.
        if (m_actAnalyseRegion)
            m_actAnalyseRegion->trigger();
    } else if (chosen == remove) {
        this->deleteSelectedRegion();
    }
}

void vtkWindowCube::openRegionProperties(int id)
{
    if (id < 0)
        id = this->m_regions.selectedId();
    const Region *r = this->m_regions.find(id);
    if (!r)
        return;
    if (!m_regionPropertiesDialog) {
        // The cube's own WCS: the backend converts with the celestial part of it.
        RegionPropertiesDialog::Backend backend{ this->remoteBackendUrl, this->remoteBackendToken,
                                                 this->remoteSessionId, this->remoteDatasetId };
        m_regionPropertiesDialog = new RegionPropertiesDialog(backend, this);
        QObject::connect(m_regionPropertiesDialog, &RegionPropertiesDialog::regionEdited, this,
                         [this](const Region &edited) {
                             if (!this->m_regions.find(edited.id))
                                 return;   // deleted while the dialog was open
                             // Typed numbers win over a drag still running: it
                             // ends here, or its next move would undo them.
                             if (m_regionDrag.active) {
                                 m_regionDrag.active = false;
                                 this->m_regions.endGesture();
                             }
                             this->m_regions.replace(edited);   // one undo step
                             this->m_regions.select(edited.id);
                             this->onRegionListChanged();
                         });
    }
    this->m_regions.select(id);
    m_regionPropertiesDialog->setRegion(*r);
    m_regionPropertiesDialog->show();
    m_regionPropertiesDialog->raise();
    m_regionPropertiesDialog->activateWindow();
    this->onRegionListChanged();
}

bool vtkWindowCube::regionDragRunning()
{
    // A drag whose release never arrived (a modal box took it, or it landed
    // off every view) is over once no button is down.
    if (m_regionDrag.active && !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        m_regionDrag.active = false;
        this->m_regions.endGesture();
        // The gate last ran with the drag on (Undo greyed): run it again, and
        // redraw the end of the drag.
        this->onRegionListChanged();
    }
    return m_regionDrag.active;
}

void vtkWindowCube::deleteSelectedRegion()
{
    if (this->regionDragRunning())
        return;
    // The image side never deletes what it does not show (#388): a hidden
    // region goes from its Session Data row.
    const Region *r = this->m_regions.selectedVisible();
    if (!r)
        return;
    const QString name = r->name;
    this->m_regions.remove(r->id);
    this->onRegionListChanged();
    this->statusBar()->showMessage(u"%1 deleted (Undo brings it back)."_s.arg(name), 5000);
}

void vtkWindowCube::undoRegionEdit()
{
    if (!this->regionDragRunning() && this->m_regions.undo())
        this->onRegionListChanged();
}

void vtkWindowCube::redoRegionEdit()
{
    if (!this->regionDragRunning() && this->m_regions.redo())
        this->onRegionListChanged();
}

std::optional<std::array<int, 4>> vtkWindowCube::selectedRegionPixelBox() const
{
    const Region *r = this->m_regions.selected();
    const int maxX = std::max(0, this->remoteDatasetWidth - 1);
    const int maxY = std::max(0, this->remoteDatasetHeight - 1);
    // A line, a vector or a text has no pixels to seed a box with.
    if (!r || !visivo::regions::hasArea(r->shape) || this->remoteDatasetWidth <= 0
        || this->remoteDatasetHeight <= 0)
        return std::nullopt;
    // The pixels whose centres the region's bounds hold (a box from pixel 4 to
    // pixel 10 gives 4..10), clamped to the cube.
    const auto b = visivo::displayedVoxelBounds(*r, visivo::DisplayGrid{});
    if (b[1] < 0 || b[3] < 0 || b[0] > maxX || b[2] > maxY || b[0] > b[1] || b[2] > b[3])
        return std::nullopt;   // entirely off the cube, or narrower than a pixel
    const int x0 = std::clamp(b[0], 0, maxX);
    const int y0 = std::clamp(b[2], 0, maxY);
    return std::array<int, 4>{ x0, std::clamp(b[1], x0, maxX), y0, std::clamp(b[3], y0, maxY) };
}

// ── Analysis ─────────────────────────────────────────────────────────────────

void vtkWindowCube::analyzeRegion(int id, vtkRenderWindow *win)
{
    if (id < 0)
        id = this->m_regions.selectedId();
    const Region *found = this->m_regions.find(id);
    if (!found)
        return;
    const Region region = *found;   // a copy: the list may change below
    if (!visivo::regions::hasArea(region.shape)) {
        this->statusBar()->showMessage(
                u"%1 is a %2: it has no pixels to measure."_s.arg(region.name,
                                                                    visivo::regions::shapeName(region.shape)),
                5000);
        return;
    }
    if (!win)
        win = this->regionAnalysisWin();
    if (!win)
        win = this->active2dWin();
    vtkImageData *imageData = this->regionPaneImage(win);
    const visivo::DisplayGrid grid = this->regionDisplayGrid(win);
    const ListRegionStatistics stats = statisticsOnImage(imageData, grid, region);
    const auto fmt = [](double v) { return QString::number(v, 'g', 8); };
    const QString label = region.name;
    // A Product pane measures its own map: named, so the numbers say which.
    const int paneSlot = this->regionPaneSlot(win);
    const QString paneProduct = paneSlot >= 0 ? m_slots[paneSlot].view.productId : QString();
    const bool onChannels = !paneProduct.isEmpty() && paneProduct == channelMirrorId();
    const QString mapLabel = paneProduct.isEmpty() ? QString()
                                                   : m_products2d.value(paneProduct).label;
    const QString statsScope = paneProduct.isEmpty()
            ? (this->viewingSlice() ? u"current 2D slice"_s : u"current moment map"_s)
            : onChannels ? u"current channel (Channels pane)"_s
                         : u"%1 (pane %2)"_s.arg(mapLabel).arg(paneSlot + 1);
    // The measured region goes into the product, so opening its Session Data
    // row puts it back in the list (restoreProductOrigin).
    QVariantMap origin = visivo::encodeListRegionOrigin(
            visivo::regions::toCanonical(region, 0.0).toVariantMap());
    if (paneProduct.isEmpty()) {
        origin = this->stampRegionSurface(std::move(origin));
    } else if (onChannels) {
        // The Channels pane shows the channel the slice would (the slice
        // itself shows a map as its background then).
        origin = visivo::withMeasuredSurface(std::move(origin), u"slice"_s,
                                             this->m_displayedSliceIndex);
    } else {
        // Which map, and which version of it: the id survives a recompute.
        origin = visivo::withMeasuredSurface(std::move(origin), u"moment"_s, -1);
        origin.insert(u"momentProduct"_s, paneProduct);
        origin.insert(u"momentGeneration"_s, this->m_momentProductGeneration.value(paneProduct, 0));
    }

    // ── Mean spectrum over the cube shown in 3-D ───────────────────────────
    // The 3-D view's cube is the full cube, a region of it, or a preview that
    // samples it (RegionCubeSampling.h). Each sample whose cube position is in
    // the region counts once: with a preview the spectrum is the mean of the
    // samples the preview has inside the region, not of every pixel of it.
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    QVector<double> spectral, meanValues;
    int samples = 0;
    bool haveSpectrum = false;
    if (cubeImage && stats.valid) {
        int extent[6];
        double o[3], sp[3];
        cubeImage->GetExtent(extent);
        cubeImage->GetOrigin(o);
        cubeImage->GetSpacing(sp);
        const int zCount = extent[5] - extent[4] + 1;
        spectral.resize(zCount);
        meanValues.fill(0.0, zCount);
        QVector<int> counts(zCount, 0);
        // Every channel keeps its spectral coordinate, blank or not: a fully
        // NaN plane must plot as a gap (NaN value), and QCustomPlot only gaps
        // on NaN VALUES — a NaN key broke the line into a spike instead.
        for (int i = 0; i < zCount; ++i) {
            const double datasetZ = this->displayToCube(o[2] + i * sp[2]);
            bool ok = false;
            spectral[i] = this->spectralAxisValue(datasetZ, &ok);
            if (!ok)
                spectral[i] = datasetZ;
        }
        const visivo::regions::AxisGrid gx{ extent[0], extent[1], o[0], sp[0] };
        const visivo::regions::AxisGrid gy{ extent[2], extent[3], o[1], sp[1] };
        const auto inside =
                visivo::regions::samplesInRegion(region, gx, gy, this->m_displaySampling);
        samples = int(inside.size());
        // Blanks were replaced by a finite sentinel below the visible minimum
        // when the cube was sanitised; averaging it turned a fully blank
        // channel into a deep negative spike.
        const double blankBelow = this->currentCubeVisibleRange[0];
        for (const auto &k : inside) {
            for (int z = extent[4]; z <= extent[5]; ++z) {
                const double v = cubeImage->GetScalarComponentAsDouble(k[0], k[1], z, 0);
                if (!std::isfinite(v) || v < blankBelow)
                    continue;
                meanValues[z - extent[4]] += v;
                counts[z - extent[4]] += 1;
            }
        }
        for (int i = 0; i < zCount; ++i) {
            if (counts[i] > 0) {
                meanValues[i] /= counts[i];
                haveSpectrum = true;
            } else {
                meanValues[i] = std::numeric_limits<double>::quiet_NaN();
            }
        }
    }

    QVariantMap params;
    params.insert(u"Region"_s, label);
    params.insert(u"2D stats from"_s, statsScope);
    params.insert(u"Valid pixels"_s, u"%1 / %2"_s.arg(stats.validCount).arg(stats.totalCount));
    if (stats.valid) {
        params.insert(u"Blanked / NaN"_s, QString::number(stats.blankedCount));
        params.insert(u"Min"_s, fmt(stats.minValue));
        params.insert(u"Max"_s, fmt(stats.maxValue));
        params.insert(u"Mean"_s, fmt(stats.mean));
        params.insert(u"Median"_s, fmt(stats.median));
        params.insert(u"Std Dev"_s, fmt(stats.stddev));
    }
    params.insert(u"Measured on"_s, visivo::describeOrigin(origin));

    if (!stats.valid) {
        this->statusBar()->showMessage(
                u"%1: no valid pixels in the region on the %2 (all blanked / NaN, or outside "
                u"the image)."_s.arg(label, statsScope),
                6000);
        return;
    }

    if (haveSpectrum) {
        const QString plotTitle = u"Mean per channel, %1"_s.arg(label);
        // The 3-D view's sampling, said where the numbers are read: a preview
        // spectrum is not the mean of every pixel of the region.
        const double scale = this->m_displaySampling.scale;
        const QString spectrumScope =
                scale > 1.0 ? u"spectrum: 3-D preview, 1 pixel in %1 per axis (%2 samples)"_s
                                      .arg(scale, 0, 'g', 3)
                                      .arg(samples)
                            : u"spectrum: full resolution (%1 pixels)"_s.arg(samples);
        // The region spectrum is a first-class 1-D product (SPEC in Session
        // Data, openable in any pane); the 2-D statistics are in its Provenance.
        const int rn = ++this->m_pinnedSpectrumCount;
        const QString pid = this->scopedProductId(QStringLiteral("spectrum%1").arg(rn));
        const QString productLabel = u"%1 spectrum"_s.arg(label);
        Spectrum1D sp;
        sp.key = spectral;
        sp.values = meanValues;
        sp.xLabel = this->spectralAxisTitle().isEmpty() ? u"Spectral axis"_s
                                                        : this->spectralAxisTitle();
        sp.yLabel = u"Mean value"_s;
        sp.label = productLabel;
        this->m_products1d.insert(pid, sp);
        params.insert(u"Spectrum from"_s, spectrumScope.mid(QStringLiteral("spectrum: ").size()));
        this->registerAnalysisProduct(u"spectrum"_s, productLabel, params, this, pid,
                                      u"spectrum1d"_s, /*revealProvenance=*/true, origin);
        // SHOW it: free pane → grow the layout → else it waits in Session Data.
        // Never borrowed: a region spectrum is a permanent product.
        const int pane = this->acquirePaneForProduct(pid, /*mayBorrow=*/false);
        this->refreshPaneChrome();
        // The shared region plot (the Spectrum view's widget) follows every
        // region analysis, as it always has. With no pane free the spectrum
        // would only be a row in Session Data: then, and only then, the plot
        // is raised as a window of its own; with a pane it is shown there,
        // and a window on top of the slice would hide the regions being edited.
        if (pane < 0 || this->probePlotWidget) {
            const bool freshWidget = !this->probePlotWidget;
            if (freshWidget) {
                this->probePlotWidget = new ProfileWidget(this);
                this->probePlotWidget->setBackend(this->remoteBackendUrl, this->remoteBackendToken);
                this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::RegionStatic, plotTitle);
                this->probePlotWidget->setupSpectrumPlot(this->spectralAxisTitle(), u"Mean value"_s);
                this->probePlotWidget->applyPlotTheme(this->m_darkPlots);
                this->pushSpectralAxisContext(this->probePlotWidget);
                // QPointer is not nulled before destroyed() fires.
                QObject::connect(this->probePlotWidget, &QObject::destroyed, this,
                                 [this]() { this->probePlotWidget = nullptr; });
            } else {
                this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::RegionStatic, plotTitle);
            }
            this->probePlotWidget->setRegionContext(
                    label, stats.validCount, stats.totalCount,
                    u"2D stats: %1  ·  "_s.arg(statsScope) + spectrumScope);
            this->probePlotWidget->updateSpectrumPlot(spectral, meanValues, plotTitle, /*live=*/false);
            if (pane < 0) {
                this->probePlotWidget->show();
                this->probePlotWidget->raise();
            }
        }
        this->statusBar()->showMessage(
                (pane >= 0 ? u"%1: mean %2 · median %3 · %4/%5 px - spectrum in pane %6, full "
                             u"stats in Inspector › Parameters."_s
                           : u"%1: mean %2 · median %3 · %4/%5 px - spectrum in its own "
                             u"window (no free pane), full stats in Inspector › Parameters.%6"_s)
                        .arg(label, fmt(stats.mean), fmt(stats.median))
                        .arg(stats.validCount)
                        .arg(stats.totalCount)
                        .arg(pane >= 0 ? QString::number(pane + 1) : QString()),
                6000);
        return;
    }

    // Valid statistics but no cube spectrum to hang them on (a region on a
    // moment map outside the loaded block): a statistics product of its own,
    // so the row can be reopened and the region put back.
    const int rn = ++this->m_regionStatsCount;
    const QString pid = this->scopedProductId(QStringLiteral("regionstats%1").arg(rn));
    params.insert(u"Status"_s, u"No cube spectrum for this region."_s);
    // No result WINDOW: passing `this` would make the Session Data row raise
    // the cube window instead of replaying the recorded numbers.
    this->registerAnalysisProduct(u"region"_s, u"%1 stats"_s.arg(label), params,
                                  /*window=*/nullptr, pid, QString(),
                                  /*revealProvenance=*/true, origin);
    this->statusBar()->showMessage(
            u"%1: mean %2 · median %3 · %4/%5 px (no cube spectrum; stats saved in Session Data)."_s
                    .arg(label, fmt(stats.mean), fmt(stats.median))
                    .arg(stats.validCount)
                    .arg(stats.totalCount),
            6000);
}
