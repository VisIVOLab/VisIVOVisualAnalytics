// Sky link to an image window (issue #522) — partial class of vtkWindowCube.
//
// A cube and an image are linked in sky coordinates, as CARTA's XY matching
// does: the slice's view follows the image's by sky centre and angular scale
// (not pixels), the cursor of one is a crosshair in the other, regions drawn on
// the image arrive on the slice (where their spectra work as for any region),
// and a line drawn on the image becomes the path of a PV cut here. The matching
// lives in SkyWcs.h, the pairs and the dialogs in SkyLinkHub; this file is what
// the cube window says about itself. It is the slice that is linked: the view
// and the cursor are the slice's, and the Product panes follow the view the
// same way they follow the slice's pan and zoom within the window.
//
// This is separate from "Link Views" (camera, channel and colour across cube
// viewers). The two can be on together: a cube that takes an image's view
// hands it on to the cubes it is Link-Viewed with.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "PvController.h"
#include "RegionImportResult.h"
#include "SkyLinkHub.h"
#include "app/BackendClient.h"
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

QString vtkWindowCube::skyLinkLabel() const
{
    return u"%1 (cube)"_s.arg(visivo::datasetDisplayName(this->filepath));
}

void vtkWindowCube::skyMessage(const QString &text)
{
    this->statusBar()->showMessage(text, 7000);
}

const visivo::skylink::SkyWcs *vtkWindowCube::skyWcs()
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

bool vtkWindowCube::skyViewRead(visivo::skylink::View &view)
{
    if (this->sliceSlot() < 0)
        return false;   // no slice on screen: nothing to follow
    double center[2] = { 0.0, 0.0 };
    double height = 0.0;
    // The slice's world is the cube's 0-based pixels; the height is that of the
    // data area, whatever gutters the colour bar and the axes take.
    if (!this->sliceCameraNormalized(center, height) || !std::isfinite(center[0])
        || !std::isfinite(center[1]) || !(height > 0.0) || !std::isfinite(height))
        return false;
    view = { center[0], center[1], height };
    return true;
}

void vtkWindowCube::skyViewApply(const visivo::skylink::View &view)
{
    if (this->sliceSlot() < 0 || !(view.height > 0.0))
        return;
    const double center[2] = { view.cx, view.cy };
    // The slice and the Product panes, each adopting the camera before it draws
    // it, so none sends it back.
    this->applyNormalizedTo2DPanes(center, view.height, /*skipSlot=*/-1);
    // On to the cubes this one is Link-Viewed with, as for a pan made here.
    if (this->m_linkViews)
        this->propagate2DCamera(this->sliceSlot());
    // The slice's own frame sees no move (the camera was adopted), so the finer
    // detail for the new zoom is asked for here.
    this->scheduleSliceDetail();
}

void vtkWindowCube::skyLinkSliceMoved()
{
    if (SkyLinkHub::instance().isLinked(this))
        SkyLinkHub::instance().notifyViewMoved(this);
}

void vtkWindowCube::skyLinkCursorFromDisplay(int displayX, int displayY)
{
    if (!SkyLinkHub::instance().isLinked(this) || !this->sliceWin
        || this->active2dWin() != this->sliceWin.GetPointer())
        return;
    auto *rc = this->sliceWin->GetRenderers();
    auto *renderer = rc ? rc->GetFirstRenderer() : nullptr;
    if (!renderer)
        return;
    this->coordinate->SetValue(displayX, displayY);
    const double *world = this->coordinate->GetComputedWorldValue(renderer);
    if (world && std::isfinite(world[0]) && std::isfinite(world[1]))
        SkyLinkHub::instance().cursorMoved(this, world[0], world[1]);
}

void vtkWindowCube::skyCursorShow(double px, double py)
{
    if (!this->sliceWin || this->sliceSlot() < 0)
        return;
    auto *rc = this->sliceWin->GetRenderers();
    auto *renderer = rc ? rc->GetFirstRenderer() : nullptr;
    if (!renderer)
        return;
    this->m_skyCursor.attach(renderer);
    this->m_skyCursor.show(px, py, -0.5, this->remoteDatasetWidth - 0.5, -0.5,
                           this->remoteDatasetHeight - 0.5);
    this->sliceWin->Render();
}

void vtkWindowCube::skyCursorHide()
{
    if (!this->m_skyCursor.shown())
        return;
    this->m_skyCursor.hide();
    if (this->sliceWin && this->sliceSlot() >= 0)
        this->sliceWin->Render();
}

std::vector<Region> vtkWindowCube::skyRegions(bool selectedOnly) const
{
    if (!selectedOnly)
        return this->m_regions.regions();
    std::vector<Region> out;
    // The rings of an annulus drawn as one move and travel together.
    for (int ring : this->m_regions.ringGroup(this->m_regions.selectedId()))
        if (const Region *r = this->m_regions.find(ring))
            out.push_back(*r);
    return out;
}

int vtkWindowCube::skyAddRegions(const std::vector<Region> &regions, const QString &label)
{
    // Joins the list as an import does: one change for the lot (one Undo takes
    // it back), the first with an area measured on the slice.
    visivo::regions::ImportResult result;
    result.regions = regions;
    result.fileName = label;
    const int before = this->m_regions.count();
    this->applyImportedRegions(result);
    return this->m_regions.count() - before;
}

bool vtkWindowCube::skyRunPv(const std::vector<std::array<double, 2>> &path, const QString &label)
{
    Q_UNUSED(label);
    if (path.size() < 2)
        return false;
    if (this->remoteDatasetId.isEmpty() || !this->pvController) {
        this->skyMessage(u"PV needs a dataset open on the backend."_s);
        return false;
    }
    if (this->pvController->isBusy()) {
        this->skyMessage(u"A PV diagram is being computed here: send the path again when it is done."_s);
        return false;
    }
    if (!this->actionExtractPvDiagram || !this->m_pvBaselineEnabled) {
        this->skyMessage(u"PV extraction is not available for this cube yet."_s);
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

void vtkWindowCube::setupSkyLinkActions()
{
    const auto make = [this](const QString &text, const QString &tip) {
        auto *a = new QAction(text, this);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        return a;
    };
    m_actSkyLink = make(u"Link to Image…"_s,
            u"Link this cube's slice to an image window in sky coordinates: panning and zooming "
            u"one moves the other to the same sky (centre and angular scale, whatever their "
            u"projection or pixel size), and the cursor of one is a crosshair in the other. "
            u"Regions drawn on the image can be sent here, and a line drawn on it cuts a PV "
            u"diagram of this cube."_s);
    m_actSkyUnlink = make(u"Unlink from Image"_s, u"End the sky link with the image window."_s);
    m_actSendSelectedRegion = make(u"Send Selected Region to Image…"_s,
            u"Copy the selected region to an image window, placed on the same sky."_s);
    m_actSendAllRegions = make(u"Send All Regions to Image…"_s,
            u"Copy every region of this cube to an image window, placed on the same sky."_s);
    QObject::connect(m_actSkyLink, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().chooseAndLink(this); });
    QObject::connect(m_actSkyUnlink, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().unlink(this); });
    QObject::connect(m_actSendSelectedRegion, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().sendRegions(this, /*selectedOnly=*/true); });
    QObject::connect(m_actSendAllRegions, &QAction::triggered, this,
                     [this]() { SkyLinkHub::instance().sendRegions(this, /*selectedOnly=*/false); });
    this->refreshSkyLinkActions();
}

void vtkWindowCube::refreshSkyLinkActions()
{
    if (!m_actSkyLink)
        return;
    m_actSkyLink->setEnabled(this->m_cubeActionsEnabled);
    m_actSkyUnlink->setEnabled(SkyLinkHub::instance().isLinked(this));
    m_actSendSelectedRegion->setEnabled(this->m_regions.selected() != nullptr);
    m_actSendAllRegions->setEnabled(!this->m_regions.isEmpty());
}
