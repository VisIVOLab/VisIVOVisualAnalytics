// Link Views with an image window (issue #522) — partial class of vtkWindowCube.
//
// An image window joins the same Link Views group as the cube viewers, as a
// peer of another kind (vtkWindowCube_Link.cpp has the group). Its grid is not
// the cube's, so what the group shares goes across by sky: the slice's pan and
// zoom follow the image's by sky centre and angular scale (AstroUtils::
// matchView, whatever the projection, pixel size or rotation), the cursor of
// one is a crosshair in the other, regions drawn on one are placed on the other
// by the backend (RegionImport.h), and a line drawn on the image is the path of
// a PV cut here. The slice is what is linked: the Product panes follow its view
// the way they do for a pan made in this window.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "PvController.h"
#include "vtkWindowImage.h"
#include "app/DatasetName.h"

#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

#include <QAction>
#include <QStatusBar>

#include <cmath>

using namespace Qt::StringLiterals;
using visivo::regions::Region;

AstroUtils *vtkWindowCube::linkSky()
{
    return this->m_linkSky.get(this->remoteBackendUrl, this->remoteBackendToken,
                               this->remoteSessionId, this->filepath);
}

visivo::regions::ExportContext vtkWindowCube::regionContext() const
{
    return { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
             this->remoteDatasetId, this->filepath };
}

void vtkWindowCube::applyLinkedImageView(AstroUtils &from, const AstroUtils::View &view)
{
    if (!this->m_linkViews || !this->m_linkCamera)
        return;
    AstroUtils *own = this->linkSky();
    AstroUtils::View out;
    if (!own || !from.matchView(*own, view, out)) {
        this->statusBar()->showMessage(
                own ? u"Linked views: the image's view is not on the sky this cube covers."_s
                    : u"Linked views: this cube has no celestial WCS, so it cannot follow."_s,
                4000);
        return;
    }
    // The slice's world is the cube's 0-based pixels at any preview stride, as in the
    // cube-to-cube path, but its view is the data area's, not the viewport the image
    // window showed: out is the viewport's, brought to the data area's here.
    // With no 2-D view drawn yet there is no frame: the view is kept as the image's own, and
    // the panes re-adopt it (reassertLinked2DCamera) once they have one.
    visivo::linkview::Frame frame;
    const auto shared = this->linkFrame(-1, frame)
            ? visivo::linkview::normalizedOf(frame, out)
            : visivo::linkview::Normalized{ out.cx, out.cy, out.height };
    // The panes adopt the camera before drawing it, so none sends it back.
    const double center[2] = { shared.cx, shared.cy };
    this->applyLinked2DCamera(center, shared.height);
    // The slice's own frame sees no move (the camera was adopted), so the finer
    // detail for the new zoom is asked for here.
    if (this->sliceSlot() >= 0)
        this->scheduleSliceDetail();
}

void vtkWindowCube::sendLinkedCursor(int displayX, int displayY)
{
    if (!this->m_linkViews || !this->m_linkCamera || !this->sliceWin
        || this->active2dWin() != this->sliceWin.GetPointer())
        return;
    const auto images = vtkWindowImage::linkedPeers();
    if (images.isEmpty())
        return;
    // A peer redraws for every position: the mouse reports far more of them
    // than anyone can follow.
    if (this->m_linkCursorClock.isValid() && this->m_linkCursorClock.elapsed() < 25)
        return;
    this->m_linkCursorClock.start();
    auto *rc = this->sliceWin->GetRenderers();
    auto *renderer = rc ? rc->GetFirstRenderer() : nullptr;
    AstroUtils *sky = renderer ? this->linkSky() : nullptr;
    if (!sky)
        return;
    this->coordinate->SetValue(displayX, displayY);
    const double *world = this->coordinate->GetComputedWorldValue(renderer);
    if (!world || !std::isfinite(world[0]) || !std::isfinite(world[1]))
        return;
    for (const auto &img : images)
        if (img)
            img->showLinkedCursor(*sky, world[0], world[1]);
}

void vtkWindowCube::showLinkedCursor(AstroUtils &from, double px, double py)
{
    if (!this->m_linkViews || !this->m_linkCamera || !this->sliceWin || this->sliceSlot() < 0)
        return;
    auto *rc = this->sliceWin->GetRenderers();
    auto *renderer = rc ? rc->GetFirstRenderer() : nullptr;
    AstroUtils *own = renderer ? this->linkSky() : nullptr;
    double tx = 0.0, ty = 0.0;
    if (!own || !from.pixelToPixel(*own, px, py, tx, ty)) {
        this->hideLinkedCursor();
        return;
    }
    this->m_linkCursor.attach(renderer);
    this->m_linkCursor.show(tx, ty, -0.5, this->remoteDatasetWidth - 0.5, -0.5,
                            this->remoteDatasetHeight - 0.5);
    this->sliceWin->Render();
}

void vtkWindowCube::hideLinkedCursor()
{
    if (!this->m_linkCursor.shown())
        return;
    this->m_linkCursor.hide();
    if (this->sliceWin && this->sliceSlot() >= 0)
        this->sliceWin->Render();
}

void vtkWindowCube::sendRegionsToImages(bool selectedOnly)
{
    const auto regions = visivo::regions::regionsToSend(this->m_regions, selectedOnly);
    if (regions.empty()) {
        this->statusBar()->showMessage(
                selectedOnly ? u"Select a region first."_s : u"There are no regions to send."_s, 5000);
        return;
    }
    const auto images = vtkWindowImage::linkedPeers();
    if (images.isEmpty()) {
        this->statusBar()->showMessage(
                u"No image window is in this Link Views group: turn Link Views on here and in an "
                u"image window."_s,
                7000);
        return;
    }
    const QString label = u"from %1 (cube)"_s.arg(visivo::datasetDisplayName(this->filepath));
    for (const auto &img : images) {
        if (!img)
            continue;
        visivo::regions::sendRegionsToDataset(
                this, regions, this->regionContext(), img->regionContext(), label,
                [img](const visivo::regions::ImportResult &result) {
                    if (img)
                        img->applyImportedRegions(result);
                });
    }
}

bool vtkWindowCube::startPvAlongPath(const std::vector<std::array<double, 2>> &path)
{
    if (path.size() < 2)
        return false;
    const auto say = [this](const QString &text) { this->statusBar()->showMessage(text, 7000); };
    if (this->remoteDatasetId.isEmpty() || !this->pvController) {
        say(u"PV needs a dataset open on the backend."_s);
        return false;
    }
    if (this->pvController->isBusy()) {
        say(u"A PV diagram is being computed here: send the path again when it is done."_s);
        return false;
    }
    if (!this->actionExtractPvDiagram || !this->m_pvBaselineEnabled) {
        say(u"PV extraction is not available for this cube yet."_s);
        return false;
    }
    // The slice is the view the path is drawn on.
    if (!this->ensureSliceForTools())
        return false;
    // Arming the tool clears the path (unless a diagram is open, which the new
    // path then replaces), so the path is set after.
    if (!this->actionExtractPvDiagram->isChecked())
        this->actionExtractPvDiagram->setChecked(true);
    this->m_pvEdit = PvEdit::None;
    this->pvDragging = false;
    this->pvCursorValid = false;
    this->pvPolylineVertices = path;
    this->pvValid = true;
    this->refreshPvOverlay();
    this->extractCurrentPvDiagram();
    // A finished path shows its handles: it is editable like one drawn here.
    this->refreshPvOverlay();
    this->statusBar()->showMessage(
            u"PV along the path from the image. Drag a vertex or the path to change it; the "
            u"diagram follows."_s,
            8000);
    if (auto *win = this->active2dWin())
        win->Render();
    return true;
}

void vtkWindowCube::setupLinkImageActions()
{
    const auto make = [this](const QString &text, const QString &tip) {
        auto *a = new QAction(text, this);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        return a;
    };
    m_actSendSelectedRegion = make(u"Send Selected Region to Linked Image"_s,
            u"Copy the selected region to the image windows in this Link Views group, placed on "
            u"the same sky."_s);
    m_actSendAllRegions = make(u"Send All Regions to Linked Image"_s,
            u"Copy every region of this cube to the image windows in this Link Views group, "
            u"placed on the same sky."_s);
    QObject::connect(m_actSendSelectedRegion, &QAction::triggered, this,
                     [this]() { this->sendRegionsToImages(/*selectedOnly=*/true); });
    QObject::connect(m_actSendAllRegions, &QAction::triggered, this,
                     [this]() { this->sendRegionsToImages(/*selectedOnly=*/false); });
}
