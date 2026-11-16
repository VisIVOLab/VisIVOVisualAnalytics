#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

#include "ImageLayerController.h"
#include "LayerListModel.h"
#include "RegionFullRes.h"
#include "RegionModel.h"
#include "RegionPropertiesDialog.h"

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

/// The cyan the drawing tools have always used.
constexpr unsigned char kDefaultColour[3] = { 51, 230, 255 };

/// How far the rotation knob stands off a box or an ellipse, and how close a
/// press has to land on a handle or an outline, in screen pixels.
constexpr double kKnobPx = 22.0;
constexpr double kPickPx = 7.0;

std::array<unsigned char, 3> regionColour(const Region &r)
{
    const QColor c(r.color);
    if (r.color.isEmpty() || !c.isValid())
        return { kDefaultColour[0], kDefaultColour[1], kDefaultColour[2] };
    return { static_cast<unsigned char>(c.red()), static_cast<unsigned char>(c.green()),
             static_cast<unsigned char>(c.blue()) };
}

/// Append closed loops as polylines to `points`/`lines`, colouring each cell.
void appendLoops(const std::vector<std::vector<Vec2>> &loops, vtkPoints *points, vtkCellArray *lines,
                 vtkUnsignedCharArray *colours, const std::array<unsigned char, 3> &rgb)
{
    for (const auto &loop : loops) {
        if (loop.size() < 2)
            continue;
        const vtkIdType first = points->GetNumberOfPoints();
        for (const Vec2 &p : loop)
            points->InsertNextPoint(p[0], p[1], 0.0);
        lines->InsertNextCell(static_cast<vtkIdType>(loop.size() + 1));
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(loop.size()); ++i)
            lines->InsertCellPoint(first + i);
        lines->InsertCellPoint(first);   // closed
        if (colours)
            colours->InsertNextTypedTuple(rgb.data());
    }
}

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
    QString shape;
    switch (this->regionMode) {
    case RegionMode::Box:     shape = u"box"_s; break;
    case RegionMode::Circle:  shape = u"circle"_s; break;
    case RegionMode::Polygon: shape = u"polygon"_s; break;
    case RegionMode::Annulus: shape = u"annulus"_s; break;
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
    const auto lineActor = [renderer](vtkPolyData *data, vtkActor *actor, double width) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(data);
        mapper->SetScalarModeToUseCellData();
        mapper->SetColorModeToDirectScalars();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetLineWidth(width);
        actor->PickableOff();
        renderer->AddActor(actor);
    };
    lineActor(this->m_regionListData, this->m_regionListActor, 2.0);
    lineActor(this->m_regionSelectedData, this->m_regionSelectedActor, 3.5);

    this->m_regionSelectedTriangulator->SetInputData(this->m_regionSelectedFillData);
    vtkNew<vtkPolyDataMapper> fillMapper;
    fillMapper->SetInputConnection(this->m_regionSelectedTriangulator->GetOutputPort());
    fillMapper->ScalarVisibilityOff();
    this->m_regionSelectedFillActor->SetMapper(fillMapper);
    this->m_regionSelectedFillActor->GetProperty()->SetOpacity(0.15);
    this->m_regionSelectedFillActor->PickableOff();
    this->m_regionSelectedFillActor->VisibilityOff();
    renderer->AddActor(this->m_regionSelectedFillActor);

    // Point regions: a marker of fixed size on screen, in the region's colour.
    lineActor(this->m_regionPointData, this->m_regionPointActor, 1.0);
    this->m_regionPointActor->GetProperty()->SetPointSize(11.0);

    // Handles of the selected region: white squares of fixed size on screen.
    vtkNew<vtkPolyDataMapper> handleMapper;
    handleMapper->SetInputData(this->m_regionHandleData);
    handleMapper->ScalarVisibilityOff();
    this->m_regionHandleActor->SetMapper(handleMapper);
    this->m_regionHandleActor->GetProperty()->SetColor(1.0, 1.0, 1.0);
    this->m_regionHandleActor->GetProperty()->SetPointSize(9.0);
    this->m_regionHandleActor->PickableOff();
    renderer->AddActor(this->m_regionHandleActor);

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
}

void vtkWindowImage::onRegionListChanged()
{
    this->refreshRegionListOverlay();
    this->updateRegionReadout();

    const bool haveSelected = this->m_regions.selected() != nullptr;
    if (m_actRegionProperties) {
        m_actRegionProperties->setEnabled(haveSelected);
        m_actAnalyseRegion->setEnabled(haveSelected);
        m_actDeleteRegion->setEnabled(haveSelected);
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
}

void vtkWindowImage::refreshRegionListOverlay()
{
    // Positions are full-resolution pixels, which is the world frame of this
    // renderer at every level of detail (world = origin + (voxel - extent0) *
    // spacing): no per-actor transform, unlike the draft's actors.
    vtkNew<vtkPoints> listPoints;
    vtkNew<vtkCellArray> listLines;
    vtkNew<vtkUnsignedCharArray> listColours;
    listColours->SetNumberOfComponents(3);
    vtkNew<vtkPoints> selPoints;
    vtkNew<vtkCellArray> selLines;
    vtkNew<vtkUnsignedCharArray> selColours;
    selColours->SetNumberOfComponents(3);
    vtkNew<vtkPoints> markerPoints;
    vtkNew<vtkCellArray> markerCells;
    vtkNew<vtkUnsignedCharArray> markerColours;
    markerColours->SetNumberOfComponents(3);
    vtkNew<vtkPoints> handlePoints;
    vtkNew<vtkCellArray> handleCells;

    // The knob distance the hit test will use is the one drawn now.
    this->m_regionKnobOffset = kKnobPx * this->worldPerDisplayPixel();

    const int selectedId = this->m_regions.selectedId();
    for (const Region &r : this->m_regions.regions()) {
        const auto rgb = regionColour(r);
        const bool selected = r.id == selectedId;
        if (r.shape == visivo::regions::Shape::Point) {
            const vtkIdType id = markerPoints->InsertNextPoint(r.center[0], r.center[1], 0.0);
            markerCells->InsertNextCell(1, &id);
            markerColours->InsertNextTypedTuple(rgb.data());
        }
        if (selected)
            appendLoops(visivo::regions::outlines(r), selPoints, selLines, selColours, rgb);
        else
            appendLoops(visivo::regions::outlines(r), listPoints, listLines, listColours, rgb);
    }

    this->m_regionListData->Initialize();
    this->m_regionListData->SetPoints(listPoints);
    this->m_regionListData->SetLines(listLines);
    this->m_regionListData->GetCellData()->SetScalars(listColours);
    this->m_regionPointData->Initialize();
    this->m_regionPointData->SetPoints(markerPoints);
    this->m_regionPointData->SetVerts(markerCells);
    this->m_regionPointData->GetCellData()->SetScalars(markerColours);

    const Region *sel = this->m_regions.selected();
    if (sel) {
        // The knob hangs on a short stem from the edge it turns.
        const auto hs = visivo::regions::handles(*sel, this->m_regionKnobOffset);
        for (const auto &h : hs) {
            const vtkIdType id = handlePoints->InsertNextPoint(h.pos[0], h.pos[1], 0.0);
            handleCells->InsertNextCell(1, &id);
            if (h.kind == visivo::regions::HandleKind::Rotate) {
                const double edge = sel->shape == visivo::regions::Shape::Box ? sel->size[1] / 2.0
                                                                              : sel->semiAxes[1];
                const Vec2 base = visivo::regions::toWorld(*sel, { 0.0, edge });
                appendLoops({ { base, h.pos } }, selPoints, selLines, selColours, regionColour(*sel));
            }
        }
        if (sel->shape == visivo::regions::Shape::Point) {
            const vtkIdType id = handlePoints->InsertNextPoint(sel->center[0], sel->center[1], 0.0);
            handleCells->InsertNextCell(1, &id);
        }
    }
    this->m_regionSelectedData->Initialize();
    this->m_regionSelectedData->SetPoints(selPoints);
    this->m_regionSelectedData->SetLines(selLines);
    this->m_regionSelectedData->GetCellData()->SetScalars(selColours);
    this->m_regionHandleData->Initialize();
    this->m_regionHandleData->SetPoints(handlePoints);
    this->m_regionHandleData->SetVerts(handleCells);

    // A light fill says which region is selected; an annulus is filled between
    // its rings (the inner loop runs the other way round, a hole).
    this->m_regionSelectedFillData->Initialize();
    bool fill = false;
    if (sel && sel->shape != visivo::regions::Shape::Point) {
        auto loops = visivo::regions::outlines(*sel);
        if (loops.size() > 1)
            std::reverse(loops[1].begin(), loops[1].end());
        vtkNew<vtkPoints> fp;
        vtkNew<vtkCellArray> fl;
        appendLoops(loops, fp, fl, nullptr, {});
        this->m_regionSelectedFillData->SetPoints(fp);
        this->m_regionSelectedFillData->SetLines(fl);
        const auto rgb = regionColour(*sel);
        this->m_regionSelectedFillActor->GetProperty()->SetColor(rgb[0] / 255.0, rgb[1] / 255.0,
                                                                 rgb[2] / 255.0);
        fill = fl->GetNumberOfCells() > 0;
    }
    this->m_regionSelectedFillData->Modified();
    this->m_regionSelectedFillActor->SetVisibility(fill);
    this->m_regionListData->Modified();
    this->m_regionSelectedData->Modified();
    this->m_regionPointData->Modified();
    this->m_regionHandleData->Modified();
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
        const Region next =
                m_regionDrag.hit.kind == Hit::Body
                        ? visivo::regions::moved(m_regionDrag.original, p[0] - m_regionDrag.start[0],
                                                 p[1] - m_regionDrag.start[1])
                        : visivo::regions::withHandleAt(m_regionDrag.original,
                                                        m_regionDrag.hit.handle, p);
        this->m_regions.replace(next);
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
    menu.addSeparator();
    QAction *remove = menu.addAction(u"Delete"_s);
    menu.setTitle(r->name);
    QPointer<vtkWindowImage> self(this);
    QAction *chosen = menu.exec(QCursor::pos());
    if (!self || !chosen || !this->m_regions.find(id))
        return;
    this->m_regions.select(id);
    if (chosen == props) {
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
    const Region *r = this->m_regions.selected();
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
