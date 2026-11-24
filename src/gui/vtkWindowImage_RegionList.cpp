#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

#include "ImageLayerController.h"
#include "LayerListModel.h"
#include "RegionFlux.h"
#include "RegionFluxUi.h"
#include "RegionFullRes.h"
#include "RegionModel.h"
#include "RegionListExport.h"
#include "RegionMeasure.h"
#include "RegionPropertiesDialog.h"
#include "RegionSessionController.h"
#include "WorkspaceChrome.h"

#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCommand.h>
#include <vtkContourTriangulator.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkUnsignedCharArray.h>

#include <QAction>
#include <QColor>
#include <QCursor>
#include <QDockWidget>   // WorkspaceChrome.h holds a QPointer to one
#include <QGuiApplication>
#include <QKeySequence>
#include <QMenu>
#include <QPointer>
#include <QStatusBar>
#include <QTimer>

#include <cmath>

using namespace Qt::StringLiterals;
using visivo::regions::Hit;
using visivo::regions::Region;
using visivo::regions::Vec2;

// ── The region list (issue #341) ─────────────────────────────────────────────
// What the user keeps: every region drawn, restored from Session Data or typed
// in, until they delete it. Drawing a region still runs through the draft
// members of _Region.cpp (regionMode, regionAnchorVoxel, …); when it is
// finished it joins m_regions and the draft is cleared. Everything here draws,
// selects and edits the list.

namespace {

/// How far the rotation knob stands off a box or an ellipse, and how close a
/// press has to land on a handle or an outline, in screen pixels.
constexpr double kKnobPx = 22.0;
constexpr double kPickPx = 7.0;

}   // namespace

visivo::DisplayGrid vtkWindowImage::displayGrid() const
{
    visivo::DisplayGrid grid;
    auto *imageData =
            this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (imageData) {
        double origin[3], spacing[3];
        int extent[6];
        imageData->GetOrigin(origin);
        imageData->GetSpacing(spacing);
        imageData->GetExtent(extent);
        // extent[2], not extent[1]: the Y index has bitten before.
        grid = { origin[0], origin[1], spacing[0], spacing[1], extent[0], extent[2] };
    }
    return grid;
}

std::optional<Region> vtkWindowImage::draftRegion() const
{
    if (this->regionMode == RegionMode::None || !this->regionValid)
        return std::nullopt;
    // A point or a line is where the cursor was, in source pixels (not the
    // displayed voxel's), so that a drawing on a decimated image keeps its place.
    if (this->regionMode == RegionMode::Point)
        return visivo::measure::pointRegion(this->regionAnchorWorld[0], this->regionAnchorWorld[1]);
    if (this->regionMode == RegionMode::Line)
        return visivo::measure::lineRegion(this->regionAnchorWorld[0], this->regionAnchorWorld[1],
                                           this->regionCurrentWorld[0], this->regionCurrentWorld[1]);
    QString shape;
    switch (this->regionMode) {
    case RegionMode::Box:     shape = u"box"_s; break;
    case RegionMode::Circle:  shape = u"circle"_s; break;
    case RegionMode::Polygon: shape = u"polygon"_s; break;
    case RegionMode::Annulus: shape = u"annulus"_s; break;
    case RegionMode::Point:
    case RegionMode::Line:
    case RegionMode::None:    return std::nullopt;
    }
    auto vertices = this->regionPolygonVertices;
    // While a polygon is drawn the cursor is its next vertex, on screen and in
    // the numbers, though not in the list yet.
    if (this->regionMode == RegionMode::Polygon && this->regionDragging
        && (vertices.empty() || vertices.back() != this->regionCurrentVoxel))
        vertices.push_back(this->regionCurrentVoxel);
    return visivo::regionFromDisplayed(shape, this->displayGrid(), this->regionAnchorVoxel,
                                       this->regionCurrentVoxel, vertices,
                                       this->regionAnnulusInnerRadius);
}

int vtkWindowImage::commitDraftRegion()
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
    this->statusBar()->showMessage(
            u"%1 added. Click a region to select it, drag it to move it, drag its handles to "
            u"reshape it; right-click for Properties, Analyse and Delete."_s
                    .arg(this->m_regions.find(id)->name),
            8000);
    return id;
}

void vtkWindowImage::setupRegionList(vtkRenderer *renderer)
{
    this->m_regionOverlay.attach(renderer);

    // The rotation knob stands a fixed distance off the edge ON SCREEN: a zoom
    // has to move it, and the hit test reads the distance it was drawn at.
    if (auto *camera = renderer->GetActiveCamera())
        camera->AddObserver(vtkCommand::ModifiedEvent, this, &vtkWindowImage::onRegionCameraChanged);
}

void vtkWindowImage::onRegionCameraChanged()
{
    if (this->m_regions.selected() == nullptr || m_regionDrag.active)
        return;
    const double knob = kKnobPx * this->worldPerDisplayPixel();
    // Pans and clipping-range updates also modify the camera; only a change of
    // scale moves the knob.
    if (std::abs(knob - this->m_regionKnobOffset) > 1e-3 * std::max(knob, 1e-12))
        this->refreshRegionListOverlay();
}

void vtkWindowImage::setupRegionListActions()
{
    const auto make = [this](const QString &text, const QString &tip) {
        auto *a = new QAction(text, this);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        return a;
    };
    m_actRegionProperties = make(u"Region Properties…"_s,
            u"Set the selected region by numbers: centre, sizes, angle or vertices, in image "
            u"pixels or a sky frame (double-click a region does the same)."_s);
    m_actAnalyseRegion = make(u"Analyse Region"_s,
            u"Statistics of the selected region."_s);
    m_actDeleteRegion = make(u"Delete Region"_s, u"Remove the selected region."_s);
    m_actUndoRegion = make(u"Undo Region Edit"_s,
            u"Undo the last change to this window's regions (draw, move, reshape, delete)."_s);
    m_actRedoRegion = make(u"Redo Region Edit"_s, u"Redo the region change just undone."_s);
    // The keys act while the image has the focus, so they never take Delete or
    // Undo away from a text field or a table elsewhere in the window.
    m_actDeleteRegion->setShortcuts({ QKeySequence::Delete, QKeySequence(Qt::Key_Backspace) });
    m_actUndoRegion->setShortcut(QKeySequence::Undo);
    m_actRedoRegion->setShortcut(QKeySequence::Redo);
    for (QAction *a : { m_actDeleteRegion, m_actUndoRegion, m_actRedoRegion }) {
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        ui->vtk->addAction(a);
    }
    QObject::connect(m_actRegionProperties, &QAction::triggered, this,
                     [this]() { this->openRegionProperties(); });
    QObject::connect(m_actAnalyseRegion, &QAction::triggered, this,
                     [this]() { this->analyzeRegion(); });
    QObject::connect(m_actDeleteRegion, &QAction::triggered, this,
                     &vtkWindowImage::deleteSelectedRegion);
    QObject::connect(m_actUndoRegion, &QAction::triggered, this, &vtkWindowImage::undoRegionEdit);
    QObject::connect(m_actRedoRegion, &QAction::triggered, this, &vtkWindowImage::redoRegionEdit);
    for (QAction *a : { m_actRegionProperties, m_actAnalyseRegion, m_actDeleteRegion,
                        m_actUndoRegion, m_actRedoRegion })
        a->setEnabled(false);
    // Always available: the distance is a setting, not an action on a region.
    m_actHiDistance = make(u"HI Mass Distance…"_s,
            u"The distance (Mpc, or a redshift) at which Analyse gives the HI mass of a region "
            u"on a moment-0 map."_s);
    QObject::connect(m_actHiDistance, &QAction::triggered, this,
                     [this]() { visivo::flux::askHiDistance(this); });
}

void vtkWindowImage::onRegionListChanged()
{
    this->refreshRegionListOverlay();
    this->updateRegionReadout();

    const bool haveSelected = this->m_regions.selected() != nullptr;
    // A hidden region (#388) is edited and deleted from its Session Data row
    // only; it can still be measured.
    const bool shownSelected = this->m_regions.selectedVisible() != nullptr;
    if (m_actRegionProperties) {
        m_actRegionProperties->setEnabled(shownSelected);
        m_actAnalyseRegion->setEnabled(haveSelected);
        m_actDeleteRegion->setEnabled(shownSelected);
        m_actUndoRegion->setEnabled(this->m_regions.canUndo());
        m_actRedoRegion->setEnabled(this->m_regions.canRedo());
    }

    // The properties dialog follows the selection, and goes when its region does.
    if (m_regionPropertiesDialog) {
        if (const Region *sel = this->m_regions.selected())
            m_regionPropertiesDialog->setRegion(*sel);
        else if (const Region *shown = this->m_regions.find(m_regionPropertiesDialog->regionId()))
            m_regionPropertiesDialog->setRegion(*shown);
        else
            m_regionPropertiesDialog->close();
    }
    if (ui->vtk && ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
    this->refreshRegionSession();
}

void vtkWindowImage::refreshRegionSession()
{
    if (!m_regionSession && m_workspaceChrome && m_workspaceChrome->sessionTree()) {
        RegionSessionController::Hooks hooks;
        hooks.editable = [this]() { return !m_regionDrag.active; };
        hooks.changed = [this]() { this->onRegionListChanged(); };
        hooks.properties = [this](int id) { this->openRegionProperties(id); };
        hooks.analyse = [this](int id) { this->analyzeRegion(id); };
        // A line is read from the file: its profile.
        hooks.canAnalyse = [](const Region &r) {
            return visivo::regions::hasArea(r.shape) || r.shape == visivo::regions::Shape::Line;
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

void vtkWindowImage::refreshRegionListOverlay()
{
    // Positions are full-resolution pixels, which is the world frame of this
    // renderer at every level of detail (world = origin + (voxel - extent0) *
    // spacing): no per-actor transform, unlike the draft's actors.
    // The knob distance the hit test will use is the one drawn now.
    this->m_regionKnobOffset = kKnobPx * this->worldPerDisplayPixel();
    this->m_regionOverlay.update(this->m_regions, this->m_regionKnobOffset);
}

bool vtkWindowImage::displayToWorld(int displayX, int displayY, Vec2 *world) const
{
    auto *win = ui->vtk ? ui->vtk->renderWindow() : nullptr;
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

double vtkWindowImage::worldPerDisplayPixel() const
{
    Vec2 a{}, b{};
    if (!this->displayToWorld(0, 0, &a) || !this->displayToWorld(100, 0, &b))
        return 1.0;
    const double d = std::hypot(b[0] - a[0], b[1] - a[1]) / 100.0;
    return d > 0.0 && std::isfinite(d) ? d : 1.0;
}

bool vtkWindowImage::regionEditingAllowed() const
{
    // Every armed tool that owns the next click keeps it: a ruler, an
    // annotation waiting to be placed, the VLKB box, the frozen-or-live probe,
    // and a polygon being drawn (its clicks are vertices).
    return !this->m_regions.isEmpty() && !this->isBusy() && !this->m_vlkbSelectActive
            && this->m_annotPlaceMode == AnnotPlaceMode::None
            && this->m_measMode == MeasurementMode::None && !this->probeModeActive
            && !(this->regionMode != RegionMode::None && this->regionDragging)
            && this->layers && this->layers->getImageData(this->layers->getMasterIndex());
}

void vtkWindowImage::regionEditCallback(vtkObject *, unsigned long eventId, void *clientData,
                                        void *)
{
    auto *self = static_cast<vtkWindowImage *>(clientData);
    if (self && self->handleRegionEditEvent(eventId))
        self->m_regionEditCommand->SetAbortFlag(1);   // nobody after us sees this event
}

bool vtkWindowImage::handleRegionEditEvent(unsigned long eventId)
{
    auto *win = ui->vtk ? ui->vtk->renderWindow() : nullptr;
    auto *iren = win ? win->GetInteractor() : nullptr;
    if (!iren)
        return false;
    const int *pos = iren->GetEventPosition();
    Vec2 p{};
    if (!pos || !this->displayToWorld(pos[0], pos[1], &p))
        return false;

    if (eventId == vtkCommand::MouseMoveEvent) {
        if (!m_regionDrag.active)
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
        this->onRegionListChanged();
        return true;
    }
    if (eventId == vtkCommand::LeftButtonReleaseEvent) {
        if (!m_regionDrag.active)
            return false;
        m_regionDrag.active = false;
        this->m_regions.endGesture();
        this->onRegionListChanged();
        return true;
    }

    if (!this->regionEditingAllowed())
        return false;
    const double tolerance = kPickPx * this->worldPerDisplayPixel();
    Hit hit;
    const int id = this->m_regions.regionAt(p, tolerance, this->m_regionKnobOffset, &hit);
    // A catalogue source under the cursor keeps its click: it is selected by
    // clicking on it, and a region drawn around it must not swallow that.
    if (id >= 0 && hit.kind == Hit::Body && this->catalogueOverlayLoaded
        && this->catalogueSourceIndexNearDisplayPosition(pos[0], pos[1], win->GetRenderers()->GetFirstRenderer())
                   >= 0)
        return false;

    if (eventId == vtkCommand::LeftButtonPressEvent) {
        if (id < 0) {
            // Empty image: let go of the selection, and let the click do what it
            // does (pan, or start a new region with a drawing tool armed).
            if (this->m_regions.selectedId() != -1) {
                this->m_regions.select(-1);
                this->onRegionListChanged();
            }
            return false;
        }
        this->m_regions.select(id);
        m_regionDrag.active = true;
        m_regionDrag.id = id;
        m_regionDrag.hit = hit;
        m_regionDrag.start = p;
        m_regionDrag.original = *this->m_regions.find(id);
        m_regionDrag.companions.clear();
        for (int ring : this->m_regions.ringGroup(id))
            if (ring != id)
                m_regionDrag.companions.push_back(*this->m_regions.find(ring));
        this->m_regions.beginGesture();
        this->onRegionListChanged();
        if (ui->vtk)
            ui->vtk->setFocus(Qt::MouseFocusReason);   // so Delete and Undo reach the image
        return true;
    }
    if (eventId == vtkCommand::LeftButtonDoubleClickEvent) {
        if (id < 0)
            return false;
        this->m_regions.select(id);
        this->onRegionListChanged();
        this->openRegionProperties(id);
        return true;
    }
    if (eventId == vtkCommand::RightButtonPressEvent) {
        if (id < 0)
            return false;
        this->m_regions.select(id);
        this->onRegionListChanged();
        // Out of the VTK callback: a menu runs its own event loop.
        QPointer<vtkWindowImage> self(this);
        QTimer::singleShot(0, this, [self, id]() {
            if (self)
                self->showRegionContextMenu(id);
        });
        return true;
    }
    return false;
}

void vtkWindowImage::showRegionContextMenu(int id)
{
    const Region *r = this->m_regions.find(id);
    if (!r)
        return;
    // No parent: a stack menu owned by the window would be deleted twice if the
    // window went away while the menu runs its loop.
    QMenu menu;
    menu.setToolTipsVisible(true);
    QAction *props = menu.addAction(u"Properties…"_s);
    QAction *analyse = menu.addAction(u"Analyse"_s);
    QAction *background = menu.addAction(u"Background Region"_s);
    background->setCheckable(true);
    background->setChecked(visivo::regions::isBackground(*r));
    background->setEnabled(visivo::regions::hasArea(r->shape));
    background->setToolTip(u"Analyse subtracts the mean of the nearest background region from a "
                           u"region's flux and takes its scatter as the noise."_s);
    menu.addSeparator();
    QAction *remove = menu.addAction(u"Delete"_s);
    menu.setTitle(r->name);
    QPointer<vtkWindowImage> self(this);
    QAction *chosen = menu.exec(QCursor::pos());
    if (!self || !chosen || !this->m_regions.find(id))
        return;
    this->m_regions.select(id);
    if (chosen == background) {
        if (!m_regionDrag.active && this->m_regions.setBackground(id, background->isChecked()))
            this->onRegionListChanged();
    } else if (chosen == props) {
        this->openRegionProperties(id);
    } else if (chosen == analyse) {
        this->onRegionListChanged();
        this->analyzeRegion(id);
    } else if (chosen == remove) {
        this->deleteSelectedRegion();
    }
}

void vtkWindowImage::openRegionProperties(int id)
{
    if (id < 0)
        id = this->m_regions.selectedId();
    const Region *r = this->m_regions.find(id);
    if (!r)
        return;
    if (!m_regionPropertiesDialog) {
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

void vtkWindowImage::deleteSelectedRegion()
{
    if (m_regionDrag.active)
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

void vtkWindowImage::undoRegionEdit()
{
    if (!m_regionDrag.active && this->m_regions.undo())
        this->onRegionListChanged();
}

void vtkWindowImage::redoRegionEdit()
{
    if (!m_regionDrag.active && this->m_regions.redo())
        this->onRegionListChanged();
}
