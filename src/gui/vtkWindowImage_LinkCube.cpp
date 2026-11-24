// Link Views with a cube window (issue #522) — partial class of vtkWindowImage.
//
// An image window joins the cube viewers' Link Views group (vtkWindowCube_Link.
// cpp) as a peer of another kind. Its grid is not the cube's, so what the group
// shares goes across by sky: the view follows by sky centre and angular scale
// (AstroUtils::matchView, whatever the projection, pixel size or rotation), the
// cursor of one is a crosshair in the other, regions drawn here are placed on
// the cube's slice by the backend (RegionImport.h), and a line drawn here is the
// path of a PV cut in the cube. vtkWindowCube_LinkImage.cpp is the cube's side.

#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

#include "ImageLayerController.h"
#include "LayerListModel.h"
#include "app/DatasetName.h"
#include "vtkWindowCube.h"

#include <vtkCamera.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

#include <QAction>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>

#include <cmath>

using namespace Qt::StringLiterals;
using visivo::regions::Region;
using visivo::regions::Shape;

LinkedWindowRegistry<vtkWindowImage> vtkWindowImage::s_registry;

namespace {

vtkRenderer *firstRenderer(vtkRenderWindow *win)
{
    auto *rc = win ? win->GetRenderers() : nullptr;
    return rc ? rc->GetFirstRenderer() : nullptr;
}

void bringForward(QWidget *w)
{
    if (!w)
        return;
    if (w->isMinimized())
        w->showNormal();
    w->raise();
}

}   // namespace

QList<QPointer<vtkWindowImage>> vtkWindowImage::linkedPeers()
{
    QList<QPointer<vtkWindowImage>> out;
    for (const auto &w : s_registry.snapshot())
        if (w && w->m_linkViews)
            out.append(w);
    return out;
}

int vtkWindowImage::openCount()
{
    return s_registry.size();
}

void vtkWindowImage::refreshLinkTargets()
{
    for (const auto &w : s_registry.snapshot())
        if (w)
            w->refreshLinkCubeActions();
}

AstroUtils *vtkWindowImage::linkSky()
{
    return this->m_linkSky.get(this->remoteBackendUrl, this->remoteBackendToken,
                               this->remoteSessionId, this->filepath);
}

visivo::regions::ExportContext vtkWindowImage::regionContext() const
{
    return { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
             this->remoteDatasetId, this->filepath };
}

void vtkWindowImage::setLinkViews(bool on)
{
    this->m_linkViews = on;
    if (this->m_actLinkViews && this->m_actLinkViews->isChecked() != on) {
        const QSignalBlocker blocker(this->m_actLinkViews);
        this->m_actLinkViews->setChecked(on);
    }
    this->m_linkCameraTracker.reset();
    if (!on) {
        // The crosshairs of this window's cursor in the linked cubes, and theirs here.
        this->hideLinkedCursor();
        for (const auto &cube : vtkWindowCube::linkedPeers())
            if (cube)
                cube->hideLinkedCursor();
        this->statusBar()->showMessage(u"Linked views: off."_s, 3000);
        return;
    }
    // This view is the shared one: the cubes already in the group snap to it, and
    // frames at it are not a move.
    this->broadcastLinkedView();
    this->m_linkCameraTracker.adopt(this->imageCameraPose());
    const auto cubes = vtkWindowCube::linkedPeers();
    this->statusBar()->showMessage(
            cubes.isEmpty()
                    ? u"Linked views: on, enable it on a cube to sync them."_s
                    : u"Linked views: on (%1 linked cube(s) synced)."_s.arg(cubes.size()),
            5000);
}

void vtkWindowImage::onLinkedCameraFrame()
{
    if (!this->m_linkViews)
        return;
    if (!this->m_linkCameraTracker.frame(this->imageCameraPose(), this->m_applyingLinkedView))
        return;
    // Sent on the next turn of the event loop: a drag's burst of frames joins one
    // send, and the cubes do not draw while this one is still finishing its frame.
    QTimer::singleShot(0, this, [this]() {
        if (this->m_linkCameraTracker.takePending() && this->m_linkViews)
            this->broadcastLinkedView();
    });
}

void vtkWindowImage::broadcastLinkedView()
{
    const auto cubes = vtkWindowCube::linkedPeers();
    if (cubes.isEmpty())
        return;
    auto *renderer = firstRenderer(ui->vtk ? ui->vtk->renderWindow() : nullptr);
    auto *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!camera || !(camera->GetParallelScale() > 0.0))
        return;
    double focal[3];
    camera->GetFocalPoint(focal);
    // The world of this window is the master image's 0-based pixels, at every
    // level of detail; the parallel scale is half the height shown.
    const AstroUtils::View view{ focal[0], focal[1], 2.0 * camera->GetParallelScale() };
    AstroUtils *sky = this->linkSky();
    if (!sky || !std::isfinite(view.cx) || !std::isfinite(view.cy) || !std::isfinite(view.height))
        return;
    for (const auto &cube : cubes)
        if (cube)
            cube->applyLinkedImageView(*sky, view);
}

void vtkWindowImage::applyLinkedView(AstroUtils &from, const AstroUtils::View &view)
{
    if (!this->m_linkViews)
        return;
    auto *win = ui->vtk ? ui->vtk->renderWindow() : nullptr;
    auto *renderer = firstRenderer(win);
    auto *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    AstroUtils *own = camera ? this->linkSky() : nullptr;
    AstroUtils::View out;
    if (!own || !from.matchView(*own, view, out)) {
        this->statusBar()->showMessage(
                own ? u"Linked views: the cube's view is not on the sky this image covers."_s
                    : u"Linked views: this image has no celestial WCS, so it cannot follow."_s,
                4000);
        return;
    }
    double focal[3], position[3];
    camera->GetFocalPoint(focal);
    camera->GetPosition(position);
    const double dx = position[0] - focal[0], dy = position[1] - focal[1];
    camera->SetFocalPoint(out.cx, out.cy, focal[2]);
    camera->SetPosition(out.cx + dx, out.cy + dy, position[2]);
    camera->SetParallelScale(0.5 * out.height);
    renderer->ResetCameraClippingRange();
    // The view of a peer is not this window's move: not sent back. The tile
    // streaming and the axis labels still follow it (their own watchers).
    const QScopedValueRollback<bool> guard(this->m_applyingLinkedView, true);
    this->m_linkCameraTracker.adopt(this->imageCameraPose());
    win->Render();
}

void vtkWindowImage::sendLinkedCursor(int displayX, int displayY)
{
    if (!this->m_linkViews)
        return;
    const auto cubes = vtkWindowCube::linkedPeers();
    if (cubes.isEmpty())
        return;
    // A peer redraws for every position: the mouse reports far more of them
    // than anyone can follow.
    if (this->m_linkCursorClock.isValid() && this->m_linkCursorClock.elapsed() < 25)
        return;
    this->m_linkCursorClock.start();
    auto *renderer = firstRenderer(ui->vtk ? ui->vtk->renderWindow() : nullptr);
    AstroUtils *sky = renderer ? this->linkSky() : nullptr;
    if (!sky)
        return;
    this->coordinate->SetValue(displayX, displayY);
    const double *world = this->coordinate->GetComputedWorldValue(renderer);
    if (!world || !std::isfinite(world[0]) || !std::isfinite(world[1]))
        return;
    for (const auto &cube : cubes)
        if (cube)
            cube->showLinkedCursor(*sky, world[0], world[1]);
}

void vtkWindowImage::showLinkedCursor(AstroUtils &from, double px, double py)
{
    if (!this->m_linkViews)
        return;
    auto *win = ui->vtk ? ui->vtk->renderWindow() : nullptr;
    auto *renderer = firstRenderer(win);
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    AstroUtils *own = renderer && imageData ? this->linkSky() : nullptr;
    double tx = 0.0, ty = 0.0;
    if (!own || !from.pixelToPixel(*own, px, py, tx, ty)) {
        this->hideLinkedCursor();
        return;
    }
    double bounds[6];
    imageData->GetBounds(bounds);
    this->m_linkCursor.attach(renderer);
    this->m_linkCursor.show(tx, ty, bounds[0], bounds[1], bounds[2], bounds[3]);
    win->Render();
}

void vtkWindowImage::hideLinkedCursor()
{
    if (!this->m_linkCursor.shown())
        return;
    this->m_linkCursor.hide();
    if (ui->vtk && ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::sendRegionsToCubes(bool selectedOnly)
{
    const auto regions = visivo::regions::regionsToSend(this->m_regions, selectedOnly);
    if (regions.empty()) {
        this->statusBar()->showMessage(
                selectedOnly ? u"Select a region first."_s : u"There are no regions to send."_s, 5000);
        return;
    }
    const auto cubes = vtkWindowCube::linkedPeers();
    if (cubes.isEmpty()) {
        this->statusBar()->showMessage(
                u"No cube window is in this Link Views group: turn Link Views on here and in a "
                u"cube window."_s,
                7000);
        return;
    }
    const QString label = u"from %1 (image)"_s.arg(visivo::datasetDisplayName(this->filepath));
    for (const auto &cube : cubes) {
        if (!cube)
            continue;
        visivo::regions::sendRegionsToDataset(
                this, regions, this->regionContext(), cube->regionContext(), label,
                [cube](const visivo::regions::ImportResult &result) {
                    if (!cube)
                        return;
                    cube->applyImportedRegions(result);
                    bringForward(cube);
                });
    }
}

void vtkWindowImage::extractPvInCubes()
{
    const Region *line = this->m_regions.selected();
    if (!line || line->shape != Shape::Line || line->points.size() < 2) {
        this->statusBar()->showMessage(
                u"Select a line region first: its vertices are the path of the PV cut."_s, 6000);
        return;
    }
    const auto cubes = vtkWindowCube::linkedPeers();
    if (cubes.isEmpty()) {
        this->statusBar()->showMessage(
                u"No cube window is in this Link Views group: turn Link Views on here and in a "
                u"cube window."_s,
                7000);
        return;
    }
    const QString label = u"PV path from %1 (image)"_s.arg(visivo::datasetDisplayName(this->filepath));
    for (const auto &cube : cubes) {
        if (!cube)
            continue;
        // The path goes to the cube's pixels as a line region does: through the sky.
        visivo::regions::sendRegionsToDataset(
                this, { *line }, this->regionContext(), cube->regionContext(), label,
                [cube](const visivo::regions::ImportResult &result) {
                    if (!cube || result.regions.empty())
                        return;
                    std::vector<std::array<double, 2>> path;
                    for (const auto &p : result.regions.front().points)
                        path.push_back({ p[0], p[1] });
                    if (cube->startPvAlongPath(path))
                        bringForward(cube);
                });
    }
}

void vtkWindowImage::setupLinkCubeActions()
{
    s_registry.add(this);
    const auto make = [this](const QString &text, const QString &tip) {
        auto *a = new QAction(text, this);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        return a;
    };
    m_actLinkViews = make(u"Link Views"_s,
            u"Join the Link Views group of the cube viewers: panning and zooming this image moves "
            u"the cube's slice to the same sky, and the other way round (centre and angular "
            u"scale, whatever their projection or pixel size), and the cursor of one is a "
            u"crosshair in the other. Needs a cube window with Link Views on."_s);
    m_actLinkViews->setCheckable(true);
    m_actSendSelectedRegion = make(u"Send Selected Region to Linked Cube"_s,
            u"Copy the selected region to the cube windows in this Link Views group, placed on the "
            u"same sky: a region drawn on the radio map appears on the cube's slice, where its "
            u"spectrum can be taken."_s);
    m_actSendAllRegions = make(u"Send All Regions to Linked Cube"_s,
            u"Copy every region of this image to the cube windows in this Link Views group, placed "
            u"on the same sky."_s);
    m_actExtractPvInCube = make(u"Extract PV in Linked Cube"_s,
            u"Cut a position-velocity diagram in the cubes of this Link Views group along the "
            u"selected line (its vertices, placed on the cube by the sky). Draw a line with the "
            u"Line tool and add vertices from its right-click menu."_s);
    QObject::connect(m_actLinkViews, &QAction::toggled, this, &vtkWindowImage::setLinkViews);
    QObject::connect(m_actSendSelectedRegion, &QAction::triggered, this,
                     [this]() { this->sendRegionsToCubes(/*selectedOnly=*/true); });
    QObject::connect(m_actSendAllRegions, &QAction::triggered, this,
                     [this]() { this->sendRegionsToCubes(/*selectedOnly=*/false); });
    QObject::connect(m_actExtractPvInCube, &QAction::triggered, this,
                     [this]() { this->extractPvInCubes(); });
    this->refreshLinkCubeActions();
    // This window is a new target for the cubes' Link Views.
    vtkWindowCube::refreshLinkTargets();
}

void vtkWindowImage::refreshLinkCubeActions()
{
    if (!m_actLinkViews)
        return;
    // Linking needs something to link to, as a cube's does: an open cube window.
    const bool haveCube = vtkWindowCube::openCount() > 0;
    m_actLinkViews->setEnabled(haveCube || m_linkViews);
    m_actSendSelectedRegion->setEnabled(this->m_regions.selected() != nullptr);
    m_actSendAllRegions->setEnabled(!this->m_regions.isEmpty());
    const Region *selected = this->m_regions.selected();
    m_actExtractPvInCube->setEnabled(selected && selected->shape == Shape::Line
                                     && selected->points.size() >= 2);
}
