// Sky link to a cube window (issue #522) — partial class of vtkWindowImage.
//
// An image and a cube are linked in sky coordinates, as CARTA's XY matching
// does: the view follows by sky centre and angular scale (not pixels), the
// cursor of one is a crosshair in the other, regions are sent across through
// both WCS, and a line drawn here is the path of a PV cut in the cube. The
// matching lives in SkyWcs.h, the pairs and the dialogs in SkyLinkHub; this
// file is what the image window says about itself.

#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

#include "ImageLayerController.h"
#include "LayerListModel.h"
#include "SkyLinkHub.h"
#include "app/BackendClient.h"
#include "app/DatasetName.h"

#include <vtkCamera.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

#include <QAction>
#include <QStatusBar>

#include <cmath>

using namespace Qt::StringLiterals;
using visivo::regions::Region;
using visivo::regions::Shape;
using visivo::regions::Vec2;

namespace {

vtkRenderer *firstRenderer(vtkRenderWindow *win)
{
    auto *rc = win ? win->GetRenderers() : nullptr;
    return rc ? rc->GetFirstRenderer() : nullptr;
}

}   // namespace

QString vtkWindowImage::skyLinkLabel() const
{
    return u"%1 (image)"_s.arg(visivo::datasetDisplayName(this->filepath));
}

void vtkWindowImage::skyMessage(const QString &text)
{
    this->statusBar()->showMessage(text, 7000);
}

const visivo::skylink::SkyWcs *vtkWindowImage::skyWcs()
{
    if (!this->m_skyWcsRead) {
        // The header cards of the dataset: only the WCS of the file is needed,
        // and the backend serves it whether the file is on this machine or not.
        BackendClient client(this->remoteBackendUrl, this->remoteBackendToken);
        if (!this->remoteSessionId.isEmpty())
            client.setSessionId(this->remoteSessionId);
        const auto header = client.fileHeader(this->filepath);
        if (header.valid)
            this->m_skyWcs = visivo::skylink::SkyWcs::fromCards(header.cards);
        // Read once, found or not: a header with no solution does not grow one.
        this->m_skyWcsRead = header.valid;
    }
    return this->m_skyWcs && this->m_skyWcs->valid() ? &*this->m_skyWcs : nullptr;
}

bool vtkWindowImage::skyViewRead(visivo::skylink::View &view)
{
    auto *renderer = firstRenderer(ui->vtk ? ui->vtk->renderWindow() : nullptr);
    auto *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!camera || !(camera->GetParallelScale() > 0.0))
        return false;
    double focal[3];
    camera->GetFocalPoint(focal);
    // The world of this window is the master image's 0-based pixels, at every
    // level of detail; the parallel scale is half the height shown.
    view = { focal[0], focal[1], 2.0 * camera->GetParallelScale() };
    return std::isfinite(view.cx) && std::isfinite(view.cy) && std::isfinite(view.height);
}

void vtkWindowImage::skyViewApply(const visivo::skylink::View &view)
{
    auto *win = ui->vtk ? ui->vtk->renderWindow() : nullptr;
    auto *renderer = firstRenderer(win);
    auto *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!camera || !(view.height > 0.0))
        return;
    double focal[3], position[3];
    camera->GetFocalPoint(focal);
    camera->GetPosition(position);
    const double dx = position[0] - focal[0], dy = position[1] - focal[1];
    camera->SetFocalPoint(view.cx, view.cy, focal[2]);
    camera->SetPosition(view.cx + dx, view.cy + dy, position[2]);
    camera->SetParallelScale(0.5 * view.height);
    renderer->ResetCameraClippingRange();
    // The view of a peer is not this window's move: not sent back. The tile
    // streaming and the axis labels still follow it (their own watchers).
    this->m_skyViewWatch.adopt(this->imageCameraPose());
    win->Render();
}

void vtkWindowImage::skyLinkCameraFrame()
{
    // Followed on every frame even when unlinked, so the pose is never stale
    // when a link is made.
    const bool moved = this->m_skyViewWatch.moved(this->imageCameraPose());
    if (moved && SkyLinkHub::instance().isLinked(this))
        SkyLinkHub::instance().notifyViewMoved(this);
}

void vtkWindowImage::skyLinkCursorFromDisplay(int displayX, int displayY)
{
    if (!SkyLinkHub::instance().isLinked(this))
        return;
    auto *renderer = firstRenderer(ui->vtk ? ui->vtk->renderWindow() : nullptr);
    if (!renderer)
        return;
    this->coordinate->SetValue(displayX, displayY);
    const double *world = this->coordinate->GetComputedWorldValue(renderer);
    if (world && std::isfinite(world[0]) && std::isfinite(world[1]))
        SkyLinkHub::instance().cursorMoved(this, world[0], world[1]);
}

void vtkWindowImage::skyCursorShow(double px, double py)
{
    auto *win = ui->vtk ? ui->vtk->renderWindow() : nullptr;
    auto *renderer = firstRenderer(win);
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!renderer || !imageData)
        return;
    double bounds[6];
    imageData->GetBounds(bounds);
    this->m_skyCursor.attach(renderer);
    this->m_skyCursor.show(px, py, bounds[0], bounds[1], bounds[2], bounds[3]);
    win->Render();
}

void vtkWindowImage::skyCursorHide()
{
    if (!this->m_skyCursor.shown())
        return;
    this->m_skyCursor.hide();
    if (ui->vtk && ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

std::vector<Region> vtkWindowImage::skyRegions(bool selectedOnly) const
{
    if (!selectedOnly)
        return this->m_regions.regions();
    std::vector<Region> out;
    const int id = this->m_regions.selectedId();
    // The rings of an annulus drawn as one move and travel together.
    for (int ring : this->m_regions.ringGroup(id))
        if (const Region *r = this->m_regions.find(ring))
            out.push_back(*r);
    return out;
}

int vtkWindowImage::skyAddRegions(const std::vector<Region> &regions, const QString &label)
{
    // A drag still running ends here: the arrival is its own change.
    if (this->m_regionDrag.active) {
        this->m_regionDrag.active = false;
        this->m_regions.endGesture();
    }
    const int added = static_cast<int>(this->m_regions.addAll(regions, label).size());
    if (added > 0)
        this->onRegionListChanged();
    return added;
}

std::vector<Vec2> vtkWindowImage::skyPvPath() const
{
    const Region *r = this->m_regions.selected();
    if (r && r->shape == Shape::Line && r->points.size() >= 2)
        return r->points;
    return {};
}

void vtkWindowImage::setupSkyLinkActions()
{
    const auto make = [this](const QString &text, const QString &tip) {
        auto *a = new QAction(text, this);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        return a;
    };
    m_actSkyLink = make(u"Link to Cube…"_s,
            u"Link this image to a cube window in sky coordinates: panning and zooming one moves "
            u"the other to the same sky (centre and angular scale, whatever their projection or "
            u"pixel size), and the cursor of one is a crosshair in the other."_s);
    m_actSkyUnlink = make(u"Unlink from Cube"_s, u"End the sky link with the cube window."_s);
    m_actSendSelectedRegion = make(u"Send Selected Region to Cube…"_s,
            u"Copy the selected region to a cube window, placed on the same sky: a region drawn "
            u"on the radio map appears on the cube's slice, where its spectrum can be taken."_s);
    m_actSendAllRegions = make(u"Send All Regions to Cube…"_s,
            u"Copy every region of this image to a cube window, placed on the same sky."_s);
    m_actExtractPvInCube = make(u"Extract PV in Cube…"_s,
            u"Cut a position-velocity diagram in a cube along the selected line (its vertices, "
            u"placed on the cube by the sky). Draw a line with the Line tool and add vertices "
            u"from its right-click menu."_s);
    QObject::connect(m_actSkyLink, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().chooseAndLink(this); });
    QObject::connect(m_actSkyUnlink, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().unlink(this); });
    QObject::connect(m_actSendSelectedRegion, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().sendRegions(this, /*selectedOnly=*/true); });
    QObject::connect(m_actSendAllRegions, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().sendRegions(this, /*selectedOnly=*/false); });
    QObject::connect(m_actExtractPvInCube, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().extractPv(this); });
    this->refreshSkyLinkActions();
}

void vtkWindowImage::refreshSkyLinkActions()
{
    if (!m_actSkyLink)
        return;
    m_actSkyUnlink->setEnabled(SkyLinkHub::instance().isLinked(this));
    m_actSendSelectedRegion->setEnabled(this->m_regions.selected() != nullptr);
    m_actSendAllRegions->setEnabled(!this->m_regions.isEmpty());
    m_actExtractPvInCube->setEnabled(!this->skyPvPath().empty());
}
