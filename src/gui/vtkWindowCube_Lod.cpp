// vtkWindowCube_Lod.cpp
// Level of detail of the 3-D view of a large cube (issue #321). Finer levels
// of the cube are drawn over the preview in two ways:
//
//   - by the camera: each time the view settles after a rotation or a zoom,
//     the part of the cube in view is planned again (CubeLodPlan::planFocus)
//     and, when that asks for other blocks than those shown, they are read
//     and swapped in (Settings ▸ General ▸ Large Cubes ▸ Refine automatically);
//   - by hand: Tools ▸ Views ▸ Refine Selected Box reads a chosen box at the
//     finest level the detail budget allows. That box stays, whatever the
//     camera does, until Clear Refined Box.
//
// The preview keeps covering the rest of the cube as at most six coarse
// pieces around the box. The view's world frame stays the preview's, so every
// overlay, region, the lasso, the WCS and the camera stay where they are; only
// the volume layer changes. Its blocks share the window's volume property
// (colour map, threshold, scale and opacity apply to all of them at once) and
// are drawn by CubeLodVolumes. The geometry is CubeLodGeometry /
// CubeLodAssembly, the choice of level CubeLodPlan, the camera's part and the
// fetch decisions CubeLodAuto, all pure and unit-tested; when it may be shown
// at all is CubeLodGate.
//
// The blocks are read in the background, a few at a time, and the view
// changes only once every block of the box has arrived, so it never shows a
// hole. Blocks already read are kept in a cache of up to twice the budget
// (blocks on screen share their samples with it, so the most this holds is
// the cache and the set shown, three budgets), so
// turning back to a part of the cube seen before reads nothing. Anything that
// replaces the displayed cube or changes how it is drawn (full resolution, a
// region, a new preview, the backend rendering, MIP or MinIP, the isosurface,
// a headset) ends the refined view.

#include "vtkWindowCube.h"

#include "ui_vtkWindowCube.h"
#include "CubeLodAssembly.h"
#include "CubeLodAuto.h"
#include "CubeLodGate.h"
#include "CubeLodPlan.h"
#include "CubeLodStatus.h"
#include "CubeLodVolumes.h"
#include "CubeVRController.h"
#include "CubeViewportGeometry.h"
#include "HostMemory.h"
#include "Settings.h"

#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkFloatArray.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkTextureObject.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QStatusBar>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>

#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

namespace {

/// Node side, in level voxels, of the blocks a box is read in: a multiple of
/// the pyramid's 64-voxel bricks, so every read is whole chunks, and 258^3
/// samples with their ghosts, under any GPU's 3-D texture limit.
constexpr int kNodeSize = 256;
/// GL_MAX_3D_TEXTURE_SIZE every desktop OpenGL 4.1 driver offers, used until
/// the render window has been asked. The plan halves the node side below it;
/// at 256 it never has to.
constexpr int kMaxTextureSide = 2048;
/// At most this many fine blocks: each is one ray-casting pass per frame.
constexpr int kMaxBlocks = 16;
/// How long the camera must stay still before the view is planned again: a
/// wheel zoom is a burst of short interactions, and only the last one counts.
constexpr int kSettleMs = 200;

/// The workers that read the blocks of a refined box, shared by every cube
/// window and bounded, so a large box does not open dozens of connections.
/// Never destroyed before the application: a request still running when its
/// window closes finishes on its own (it holds only copies), and one not
/// started yet sees the window's generation moved and returns at once.
QThreadPool *lodPool()
{
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool(qApp);
        p->setMaxThreadCount(4);
        return p;
    }();
    return pool;
}

QString settingsDir()
{
    return QDir::home().absoluteFilePath(qApp->applicationName());
}

/// Bytes the fine blocks may take: the setting, or min(512 MB, memory / 16).
std::int64_t lodBudgetBytes()
{
    const int mb = Settings(settingsDir()).getLodBudgetMb();
    if (mb > 0)
        return std::int64_t(mb) << 20;
    const std::int64_t automatic = std::int64_t(512) << 20;
    const std::uint64_t physical = hostPhysicalMemoryBytes();
    return physical > 0 ? std::min<std::int64_t>(automatic, std::int64_t(physical / 16)) : automatic;
}

/// The cache key of a coarse piece: its boxes and the preview it is cut from
/// (the image and its modification time), so a piece of another preview, or
/// of the same one changed in place, is never taken for it.
std::string coarseKey(const CubeLodAssembly::Block &b, const vtkImageData *preview, vtkMTimeType mtime)
{
    const auto box = [](const CubeLodGeometry::Box &x) {
        return std::to_string(x.lo[0]) + ',' + std::to_string(x.lo[1]) + ',' + std::to_string(x.lo[2])
                + '-' + std::to_string(x.hi[0]) + ',' + std::to_string(x.hi[1]) + ','
                + std::to_string(x.hi[2]);
    };
    return "c:" + std::to_string(reinterpret_cast<std::uintptr_t>(preview)) + ':'
            + std::to_string(mtime) + ':' + box(b.owned) + ':' + box(b.samples);
}

std::string fineKey(const CubeLodGeometry::Node &n, int nodeSize)
{
    return "f:" + std::to_string(n.level) + ':' + std::to_string(nodeSize) + ':' + std::to_string(n.ix)
            + ',' + std::to_string(n.iy) + ',' + std::to_string(n.iz);
}

}   // namespace

/// One block as read: the backend's answer (its data moved out) and its
/// samples, blanks already set to the preview's invisible value.
struct vtkWindowCube::LodBlock
{
    BackendRegionLevelResult meta;   ///< data empty
    vtkSmartPointer<vtkFloatArray> array;
    float sentinel{ 0.f };           ///< the invisible value the blanks took
};

struct vtkWindowCube::LodCache
    : CubeLodAuto::LruCache<CubeLodAuto::BlockKey, vtkWindowCube::LodBlock>
{
    using LruCache::LruCache;
};

/// A refine in flight: what was planned and what has arrived.
struct vtkWindowCube::LodPending
{
    quint64 generation{ 0 };
    bool automatic{ false };          ///< planned by the camera, not by Refine Selected Box
    CubeLodAuto::Signature signature;
    CubeLodPlan::Plan plan;
    CubeLodGeometry::Dims cubeShape{ 0, 0, 0 };
    CubeLodGeometry::Sampling preview;
    /// The preview image the coarse pieces will be cut from: if the window
    /// shows another one by the time the blocks arrive, the set is dropped.
    vtkImageData *previewImage{ nullptr };
    std::vector<CubeLodAssembly::Block> fine;   ///< one per node, before reading
    std::vector<LodBlock> results;
    std::vector<bool> arrived;
    int remaining{ 0 };
    int fromCache{ 0 };
    QElapsedTimer timer;
};

void vtkWindowCube::setupLevelOfDetail()
{
    this->m_lodLiveGeneration = std::make_shared<std::atomic<quint64>>(0);
    this->m_lodCache = std::make_shared<LodCache>(2 * lodBudgetBytes());
    this->m_actRefineBox = new QAction(u"Refine Selected Box…"_s, ui->menuTools);
    QObject::connect(this->m_actRefineBox, &QAction::triggered, this,
                     &vtkWindowCube::showRefineBoxDialog);
    ui->menuTools->addAction(this->m_actRefineBox);
    this->m_actClearRefinedBox = new QAction(u"Clear Refined Box"_s, ui->menuTools);
    QObject::connect(this->m_actClearRefinedBox, &QAction::triggered, this, [this]() {
        this->clearLod(u"Refined box cleared: the view refines where the camera looks again."_s);
        if (auto *rw = ui->vtkCube->renderWindow())
            rw->Render();
        if (this->m_lodSettleTimer)
            this->m_lodSettleTimer->start();
    });
    ui->menuTools->addAction(this->m_actClearRefinedBox);

    this->m_lodSettleTimer = new QTimer(this);
    this->m_lodSettleTimer->setSingleShot(true);
    this->m_lodSettleTimer->setInterval(kSettleMs);
    QObject::connect(this->m_lodSettleTimer, &QTimer::timeout, this, &vtkWindowCube::onLodSettleTimeout);
    this->installLodCameraObservers();
    this->updateRefineBoxAction();
}

void vtkWindowCube::installLodCameraObservers()
{
    if (this->m_lodTornDown || this->m_lodFrameTag != 0)
        return;   // there already, or the window is going
    auto *rw = ui->vtkCube ? ui->vtkCube->renderWindow() : nullptr;
    vtkRenderer *renderer = rw ? rw->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!renderer)
        return;
    // Every frame of the 3-D view, whatever drew it: a drag, a wheel step, a
    // Camera preset, a linked view, the headset. The interactor's
    // Start/EndInteractionEvent never came (VTK raises them on the style),
    // and the render window's own EndEvent did not either in the Qt widget:
    // the renderer's is raised by every vtkRenderer::Render.
    this->m_lodFrameTag = renderer->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::onLodFrameDrawn);
    this->m_lodFrameRenderer = renderer;
    this->m_lodMotion.reset();
    qDebug().noquote() << u"[lod] camera watched on renderer %1"_s.arg(quintptr(renderer), 0, 16);
}

void vtkWindowCube::onLodFrameDrawn()
{
    vtkRenderer *renderer = this->m_lodFrameRenderer.GetPointer();
    vtkCamera *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!camera)
        return;
    CubeLodAuto::CameraPose pose;
    camera->GetPosition(pose.position.data());
    camera->GetFocalPoint(pose.focalPoint.data());
    camera->GetViewUp(pose.viewUp.data());
    pose.viewAngle = camera->GetViewAngle();
    pose.parallelScale = camera->GetParallelScale();
    pose.parallel = camera->GetParallelProjection() != 0;
    const CubeLodAuto::MotionTracker::Frame frame = this->m_lodMotion.frame(pose);
    if (frame == CubeLodAuto::MotionTracker::Frame::Started) {
        qDebug().noquote() << u"[lod] camera moving"_s;
        this->m_lodFrames = LodFrameStats{};
        this->m_lodFrames.sinceStart.start();
    }
    if (!this->m_lodMotion.moving())
        return;
    // Frames drawn while the camera moves, for the log line that closes each
    // motion: what a refined view costs to rotate.
    const double t = renderer->GetLastRenderTimeInSeconds();
    ++this->m_lodFrames.frames;
    this->m_lodFrames.renderSeconds += t;
    this->m_lodFrames.worstRenderSeconds = std::max(this->m_lodFrames.worstRenderSeconds, t);
    // Only a frame that moved the camera puts the settling off.
    if (frame != CubeLodAuto::MotionTracker::Frame::Still && this->m_lodSettleTimer)
        this->m_lodSettleTimer->start();
}

void vtkWindowCube::onLodSettleTimeout()
{
    const bool held = QGuiApplication::mouseButtons() != Qt::NoButton;
    if (CubeLodAuto::settleStep(this->m_lodMotion.moving(), held) == CubeLodAuto::SettleStep::Wait) {
        if (this->m_lodSettleTimer)
            this->m_lodSettleTimer->start();   // a drag paused, not ended
        return;
    }
    if (this->m_lodMotion.settle()) {
        const LodFrameStats &f = this->m_lodFrames;
        const double elapsed = f.sinceStart.isValid() ? f.sinceStart.elapsed() / 1000.0 : 0.0;
        // Only drags say anything about the frame rate (a wheel step draws one frame).
        if (f.frames >= 5 && this->m_lod && this->m_lod->active()) {
            qDebug().noquote() << u"[lod] interaction frames=%1 blocks=%2 render mean=%3 ms worst=%4 ms; "
                                  u"%5 frames/s over %6 s"_s
                                          .arg(f.frames)
                                          .arg(this->m_lod->blockCount())
                                          .arg(1000.0 * f.renderSeconds / f.frames, 0, 'f', 1)
                                          .arg(1000.0 * f.worstRenderSeconds, 0, 'f', 1)
                                          .arg(elapsed > 0 ? f.frames / elapsed : 0.0, 0, 'f', 1)
                                          .arg(elapsed, 0, 'f', 2);
        } else {
            qDebug().noquote() << u"[lod] camera moved: %1 frames over %2 s"_s.arg(f.frames).arg(
                    elapsed, 0, 'f', 2);
        }
    }
    this->maybeRefineLod();
}

void vtkWindowCube::teardownLevelOfDetail()
{
    this->m_lodTornDown = true;
    if (this->m_lodSettleTimer)
        this->m_lodSettleTimer->stop();
    if (this->m_lodFrameRenderer && this->m_lodFrameTag)
        this->m_lodFrameRenderer->RemoveObserver(this->m_lodFrameTag);
    this->m_lodFrameTag = 0;
    // The refined box's blocks leave the renderer while it still exists.
    this->clearLod(QString(), true);
}

vtkWindowCube::LodGateResult vtkWindowCube::lodGate() const
{
    CubeLodGate::Inputs in;
    in.settingEnabled = Settings(settingsDir()).getLodEnabled();
    in.remoteDataset = !this->remoteDatasetId.isEmpty() && !this->remoteBackendUrl.isEmpty();
    in.showingPreview = !this->usingHighResCube;
    in.previewStride = int(this->m_displaySampling.scale);
    in.localRender = this->m_effectiveRenderMode == RenderMode::Local;
    in.compositeBlend = this->m_volumeBlendMode == 0;
    in.isosurfaceShown = this->m_isosurfaceDrawn;
    in.localVrActive = this->m_vrController && this->m_vrController->isActive();
    const CubeLodGate::Decision d = CubeLodGate::evaluate(in);
    if (!d.active)
        return { false, d.reason };
    // What the backend can read, once the gate itself would allow it.
    if (!this->m_lodPyramid.valid)
        return { false, this->m_lodPyramid.httpStatus == 404
                                ? u"This backend cannot read a box of the cube at a finer level: "
                                  u"update it."_s
                                : u"The cube's levels of detail are not known yet."_s };
    if (this->m_lodPyramid.state == u"unsupported"_s)
        return { false, u"This cube has no levels of detail (its last three axes are not one "
                        u"cube)."_s };
    return { true, QString() };
}

void vtkWindowCube::updateRefineBoxAction()
{
    this->installLodCameraObservers();   // a no-op once they are there
    const LodGateResult gate = this->lodGate();
    if (this->m_actRefineBox) {
        const QString base =
                u"Show a box of the cube at a finer level over the 3-D preview: the box is read "
                u"from the backend at the finest level the detail budget allows (Settings ▸ "
                u"General ▸ Large Cubes) and drawn in place of the preview there. It stays until "
                u"Clear Refined Box."_s;
        this->m_actRefineBox->setEnabled(gate.active);
        this->m_actRefineBox->setToolTip(gate.active ? base : gate.reason);
        this->m_actRefineBox->setStatusTip(gate.active ? base : gate.reason);
    }
    if (this->m_actClearRefinedBox) {
        const bool held = this->m_lodShownExplicit || (this->m_lodPending && !this->m_lodPending->automatic);
        const QString tip = held ? u"Remove the box chosen with Refine Selected Box: the view goes "
                                   u"back to refining where the camera looks."_s
                                 : u"No box chosen with Refine Selected Box is shown."_s;
        this->m_actClearRefinedBox->setEnabled(held);
        this->m_actClearRefinedBox->setToolTip(tip);
        this->m_actClearRefinedBox->setStatusTip(tip);
    }
    // A refined box shown (or on its way) that the view can no longer show:
    // back to the plain preview, saying why.
    const bool shown = (this->m_lod && this->m_lod->active()) || this->m_lodPending;
    if (!gate.active && shown) {
        this->clearLod(u"Refined box removed: %1"_s.arg(gate.reason));
        if (auto *rw = ui->vtkCube->renderWindow())
            rw->Render();
    }
}

bool vtkWindowCube::volumeLayerShown(vtkRenderer *renderer) const
{
    if (this->m_lod && this->m_lod->active())
        return this->m_lod->visible();
    return renderer && renderer->HasViewProp(this->volume) && this->volume->GetVisibility() != 0;
}

void vtkWindowCube::setVolumeLayerVisible(bool visible)
{
    if (this->m_lod && this->m_lod->active()) {
        this->volume->SetVisibility(0);   // the blocks stand in for it
        this->m_lod->setVisible(visible);
        return;
    }
    this->volume->SetVisibility(visible ? 1 : 0);
}

void vtkWindowCube::applyLodUserCrop()
{
    if (!this->m_lod)
        return;
    if (!(this->m_headsetCropping && this->m_headsetClip.set)) {
        this->m_lod->setUserCrop(std::nullopt);
        return;
    }
    // The headset's clip box in cube voxels, faces at the outer edges of its
    // end voxels (as applyHeadsetClip draws it on the plain volume), in the
    // view's world.
    CubeLodAssembly::Crop crop;
    for (int i = 0; i < 3; ++i) {
        crop[2 * i] = this->cubeToDisplay(this->m_headsetClip.min[i] - 0.5);
        crop[2 * i + 1] = this->cubeToDisplay(this->m_headsetClip.max[i] + 0.5);
    }
    this->m_lod->setUserCrop(crop);
}

bool vtkWindowCube::lodPlanInputs(CubeLodPlan::Inputs &in)
{
    auto *previewImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!previewImage || previewImage->GetScalarType() != VTK_FLOAT
        || previewImage->GetNumberOfScalarComponents() != 1)
        return false;
    in.cubeShape = { this->remoteDatasetWidth, this->remoteDatasetHeight, this->remoteDatasetDepth };
    in.levelCount = std::max(1, this->m_lodPyramid.levels);
    in.previewStride = int(this->m_displaySampling.scale);
    in.budgetBytes = lodBudgetBytes();
    in.maxBlocks = kMaxBlocks;
    in.nodeSize = kNodeSize;
    // The GPU's own limit, asked once the window has a context.
    if (this->m_lodMaxTextureSide <= 0) {
        auto *gl = vtkOpenGLRenderWindow::SafeDownCast(ui->vtkCube->renderWindow());
        if (gl && ui->vtkCube->isVisible()) {
            gl->MakeCurrent();
            const int side = gl->IsCurrent() ? vtkTextureObject::GetMaximumTextureSize3D(gl) : -1;
            if (side > 0) {
                this->m_lodMaxTextureSide = side;
                qDebug().noquote() << u"[lod] GL_MAX_3D_TEXTURE_SIZE=%1"_s.arg(side);
            }
        }
    }
    in.maxTextureSide = this->m_lodMaxTextureSide > 0 ? this->m_lodMaxTextureSide : kMaxTextureSide;
    // The cache follows the budget, which the settings may have changed.
    if (this->m_lodCache)
        this->m_lodCache->setCapacity(2 * in.budgetBytes);
    return true;
}

bool vtkWindowCube::lodViewInputs(CubeLodPlan::Inputs &in)
{
    auto *rw = ui->vtkCube->renderWindow();
    auto *renderer = rw ? rw->GetRenderers()->GetFirstRenderer() : nullptr;
    vtkCamera *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    const int *size = renderer ? renderer->GetSize() : nullptr;
    if (!camera || !size || size[0] < 2 || size[1] < 2)
        return false;
    const CubeLodGeometry::Sampling &preview = this->m_displaySampling;
    const std::array<double, 6> cubeWorld = CubeLodAuto::cubeWorldBounds(in.cubeShape, preview);

    // Frustum ∩ cube: where rays through the viewport's corners, edges and
    // centre cross the cube, plus every corner of the cube in view (a small
    // cube between those rays would otherwise not count).
    const double inf = std::numeric_limits<double>::infinity();
    std::array<double, 6> seen{ inf, -inf, inf, -inf, inf, -inf };
    double rays[6];
    if (CubeGeom::computeViewportIntersectionBounds(renderer, rw, cubeWorld.data(), rays))
        std::copy(rays, rays + 6, seen.begin());
    const int *origin = renderer->GetOrigin();
    for (int corner = 0; corner < 8; ++corner) {
        const double p[3] = { cubeWorld[(corner & 1) ? 1 : 0], cubeWorld[(corner & 2) ? 3 : 2],
                              cubeWorld[(corner & 4) ? 5 : 4] };
        renderer->SetWorldPoint(p[0], p[1], p[2], 1.0);
        renderer->WorldToDisplay();
        double d[3];
        renderer->GetDisplayPoint(d);
        if (d[2] < 0.0 || d[2] > 1.0 || d[0] < origin[0] || d[0] > origin[0] + size[0]
            || d[1] < origin[1] || d[1] > origin[1] + size[1])
            continue;
        for (int a = 0; a < 3; ++a) {
            seen[2 * a] = std::min(seen[2 * a], p[a]);
            seen[2 * a + 1] = std::max(seen[2 * a + 1], p[a]);
        }
    }
    for (int a = 0; a < 3; ++a) {
        seen[2 * a] = std::max(seen[2 * a], cubeWorld[2 * a]);
        seen[2 * a + 1] = std::min(seen[2 * a + 1], cubeWorld[2 * a + 1]);
    }
    in.visibleBox = CubeLodAuto::visibleCubeBox(seen, preview, in.cubeShape);

    double focal[3];
    camera->GetFocalPoint(focal);
    for (int a = 0; a < 3; ++a)
        in.focalPoint[size_t(a)] = this->displayToCube(focal[a]);
    CubeLodAuto::Camera c;
    c.parallel = camera->GetParallelProjection() != 0;
    c.parallelScale = camera->GetParallelScale();
    c.viewAngleDeg = camera->GetViewAngle();
    c.focalDistance = camera->GetDistance();
    c.viewportHeightPx = size[1];
    in.voxelsPerPixel = CubeLodAuto::voxelsPerPixel(c, preview);
    return true;
}

void vtkWindowCube::maybeRefineLod()
{
    if (this->m_lodMotion.moving())
        return;   // planned when it settles
    if (!Settings(settingsDir()).getLodAutoRefine()) {
        // Switched off while the camera's set was shown or on its way: it
        // goes (a chosen box stays).
        const bool autoPending = this->m_lodPending && this->m_lodPending->automatic;
        if (autoPending)
            this->cancelLodFetch();
        if (!this->m_lodShownSignature.empty()) {
            this->clearLod();
            if (auto *rw = ui->vtkCube->renderWindow())
                rw->Render();
        }
        return;
    }
    const LodGateResult gate = this->lodGate();
    if (!gate.active || !this->m_show3dRendering) {
        qDebug().noquote() << u"[lod] settled, not refined: %1"_s.arg(
                gate.active ? u"the 3-D rendering layer is off"_s : gate.reason);
        return;
    }
    CubeLodPlan::Inputs in;
    if (!this->lodPlanInputs(in) || !this->lodViewInputs(in)) {
        qDebug().noquote() << u"[lod] settled, not refined: no preview or camera to plan with"_s;
        return;
    }
    const CubeLodPlan::Plan plan = CubeLodPlan::planFocus(in);

    CubeLodAuto::State state;
    state.explicitHeld = this->m_lodShownExplicit || (this->m_lodPending && !this->m_lodPending->automatic);
    state.shown = this->m_lodShownSignature;
    if (this->m_lodPending && this->m_lodPending->automatic)
        state.pending = this->m_lodPending->signature;
    const CubeLodAuto::Action action = CubeLodAuto::decide(plan, state);
    static const char *const names[] = { "keep", "fetch", "cancel", "clear" };
    qDebug().noquote() << u"[lod] settled: visible=%1 focal=(%2, %3, %4) voxels/px=%5 -> level %6, "
                          u"%7 nodes (%8); %9"_s
                                  .arg(CubeLodStatus::boxText(in.visibleBox))
                                  .arg(in.focalPoint[0], 0, 'f', 0)
                                  .arg(in.focalPoint[1], 0, 'f', 0)
                                  .arg(in.focalPoint[2], 0, 'f', 0)
                                  .arg(in.voxelsPerPixel, 0, 'g', 3)
                                  .arg(plan.level)
                                  .arg(plan.nodes.size())
                                  .arg(QString::fromUtf8(CubeLodPlan::reasonText(plan.reason)))
                                  .arg(QString::fromLatin1(names[int(action)]));
    switch (action) {
    case CubeLodAuto::Action::Keep:
        break;
    case CubeLodAuto::Action::Cancel:
        this->cancelLodFetch();
        break;
    case CubeLodAuto::Action::Clear:
        // The view needs no finer level: the blocks would only cost frames.
        this->clearLod();
        if (auto *rw = ui->vtkCube->renderWindow())
            rw->Render();
        break;
    case CubeLodAuto::Action::Fetch:
        this->startLodFetch(plan, in, true);
        break;
    }
}

void vtkWindowCube::refineBox(const std::array<int, 6> &cubeBox)
{
    const LodGateResult gate = this->lodGate();
    if (!gate.active) {
        qDebug().noquote() << u"[lod] refine refused: %1"_s.arg(gate.reason);
        this->statusBar()->showMessage(u"Refine Selected Box: %1"_s.arg(gate.reason), 6000);
        return;
    }
    CubeLodPlan::Inputs in;
    if (!this->lodPlanInputs(in)) {
        this->statusBar()->showMessage(u"Refine Selected Box: the preview is not loaded yet."_s, 6000);
        return;
    }
    // Inclusive bounds to the plan's half-open box.
    const CubeLodGeometry::Box box{ { cubeBox[0], cubeBox[2], cubeBox[4] },
                                    { cubeBox[1] + 1, cubeBox[3] + 1, cubeBox[5] + 1 } };
    const CubeLodPlan::Plan plan = CubeLodPlan::planBox(box, in);
    if (!plan.refine) {
        qDebug().noquote() << u"[lod] nothing to refine in %1: %2"_s.arg(
                CubeLodStatus::boxText(box), QString::fromUtf8(CubeLodPlan::reasonText(plan.reason)));
        this->statusBar()->showMessage(
                u"Refine Selected Box: %1."_s.arg(QString::fromUtf8(CubeLodPlan::reasonText(plan.reason))),
                8000);
        return;
    }
    qDebug().noquote() << u"[lod] refine asked=%1 -> box=%2 level=%3"_s.arg(CubeLodStatus::boxText(box),
                                                                          CubeLodStatus::boxText(plan.cubeBox))
                                  .arg(plan.level);
    this->startLodFetch(plan, in, false);
}

void vtkWindowCube::startLodFetch(const CubeLodPlan::Plan &plan, const CubeLodPlan::Inputs &in,
                                  bool automatic)
{
    auto *previewImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!previewImage)
        return;
    // A new request: whatever was in flight is moot. What is on screen stays
    // until the new set is complete.
    const quint64 generation = ++this->m_lodGeneration;
    this->m_lodLiveGeneration->store(generation);
    const CubeLodGeometry::Dims cube = in.cubeShape;
    auto pending = std::make_shared<LodPending>();
    pending->generation = generation;
    pending->automatic = automatic;
    pending->signature = CubeLodAuto::signatureOf(plan);
    pending->plan = plan;
    pending->cubeShape = cube;
    pending->preview = this->m_displaySampling;
    pending->previewImage = previewImage;
    pending->timer.start();
    for (const CubeLodGeometry::Node &n : plan.nodes)
        pending->fine.push_back(CubeLodAssembly::fineBlock(cube, plan.nodeSize, n, CubeLodPlan::kGhost,
                                                             plan.levelBox));
    pending->results.resize(pending->fine.size());
    pending->arrived.assign(pending->fine.size(), false);
    pending->remaining = int(pending->fine.size());

    // The blocks read before (by this plan's predecessors) come from the
    // cache, as long as their blanks took the same invisible value.
    const float sentinel = static_cast<float>(this->currentCubeInvisibleSentinel);
    const std::string dataset = this->remoteDatasetId.toStdString();
    std::vector<int> toRead;
    for (int i = 0; i < int(plan.nodes.size()); ++i) {
        const CubeLodGeometry::Node &node = plan.nodes[size_t(i)];
        const LodBlock *cached = this->m_lodCache
                ? this->m_lodCache->find(CubeLodAuto::blockKey(dataset, plan.nodeSize, node,
                                                               CubeLodPlan::blockLevelBox(plan, cube, node)))
                : nullptr;
        if (cached && cached->sentinel == sentinel) {
            pending->results[size_t(i)] = *cached;
            pending->arrived[size_t(i)] = true;
            --pending->remaining;
            ++pending->fromCache;
        } else {
            toRead.push_back(i);
        }
    }
    this->m_lodPending = pending;
    this->updateRefineBoxAction();

    const qint64 bytes = CubeLodPlan::planBytes(in, plan);
    QString what = u"level %1, %2 blocks, %3 MB"_s.arg(plan.level)
                           .arg(plan.nodes.size())
                           .arg(double(bytes) / (1 << 20), 0, 'f', 0);
    if (plan.reason == CubeLodPlan::Reason::Shrunk)
        what += u"; around the focal point"_s;
    else if (plan.reason == CubeLodPlan::Reason::BoxShrunk)
        what += u"; the budget holds only part of the box, around its centre"_s;
    qDebug().noquote() << u"[lod] %1 gen=%2 box=%3 level=%4 nodes=%5 cached=%6 bytes=%7 reason=%8"_s
                                  .arg(automatic ? u"auto"_s : u"refine"_s)
                                  .arg(generation)
                                  .arg(CubeLodStatus::boxText(plan.cubeBox))
                                  .arg(plan.level)
                                  .arg(plan.nodes.size())
                                  .arg(pending->fromCache)
                                  .arg(bytes)
                                  .arg(QString::fromUtf8(CubeLodPlan::reasonText(plan.reason)));
    if (pending->remaining == 0) {
        this->applyLodSet();
        return;
    }
    this->showPersistentStatusMessage(automatic ? u"Refining the view (%1)…"_s.arg(what)
                                                : u"Refining the box (%1)…"_s.arg(what));

    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString session = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;
    const std::shared_ptr<std::atomic<quint64>> live = this->m_lodLiveGeneration;
    for (int i : toRead) {
        const CubeLodGeometry::Node node = plan.nodes[size_t(i)];
        const CubeLodGeometry::Box level = CubeLodPlan::blockLevelBox(plan, cube, node);
        const int nodeSize = plan.nodeSize;
        const CubeLodGeometry::Dims expectedDims = CubeLodAssembly::dims(pending->fine[size_t(i)]);
        const CubeLodGeometry::Dims expectedOrigin = pending->fine[size_t(i)].samples.lo;
        auto *watcher = new QFutureWatcher<LodBlock>(this);
        QObject::connect(watcher, &QFutureWatcher<LodBlock>::finished, this,
                         [this, watcher, generation, i, node, nodeSize, level, datasetId, expectedDims,
                          expectedOrigin]() {
                             const LodBlock block = watcher->result();
                             watcher->deleteLater();
                             // Kept even when a newer request superseded this
                             // one: the camera may well come back to it.
                             // Only a block of the shape asked for: a wrong one
                             // would be replayed from the cache by every plan.
                             const bool asked = block.meta.valid && block.array
                                     && CubeLodGeometry::Dims{ block.meta.width, block.meta.height,
                                                               block.meta.depth } == expectedDims
                                     && block.meta.levelOrigin == expectedOrigin
                                     && block.array->GetNumberOfTuples()
                                             == vtkIdType(expectedDims[0]) * expectedDims[1] * expectedDims[2];
                             if (asked && this->m_lodCache && datasetId == this->remoteDatasetId) {
                                 const qint64 bytes = qint64(block.array->GetNumberOfTuples()) * qint64(sizeof(float));
                                 this->m_lodCache->insert(
                                         CubeLodAuto::blockKey(datasetId.toStdString(), nodeSize, node, level), block,
                                         bytes);
                             }
                             this->onLodBlock(generation, i, block);
                         });
        watcher->setFuture(QtConcurrent::run(lodPool(), [=]() {
            LodBlock out;
            out.sentinel = sentinel;
            if (live->load() != generation) {
                out.meta.error = u"superseded"_s;   // a newer refine or a clear came first
                return out;
            }
            BackendClient client(url, token);
            client.setSessionId(session);
            out.meta = client.requestRegionLevel(datasetId, node.level, level.lo[0], level.hi[0] - 1,
                                                 level.lo[1], level.hi[1] - 1, level.lo[2],
                                                 level.hi[2] - 1, CubeLodPlan::kGhost, u"auto"_s);
            if (!out.meta.valid)
                return out;
            // The samples as VTK will draw them, here rather than on the GUI
            // thread: blanks as the preview has them (the same invisible
            // value, never a NaN the ray caster would spread through its
            // interpolation), and the visible range stays the preview's, so
            // colours match across the box's faces.
            const qsizetype count = out.meta.data.size() / qsizetype(sizeof(float));
            out.array = vtkSmartPointer<vtkFloatArray>::New();
            out.array->SetName("intensity");
            out.array->SetNumberOfComponents(1);
            out.array->SetNumberOfTuples(vtkIdType(count));
            float *dst = out.array->GetPointer(0);
            std::memcpy(dst, out.meta.data.constData(), size_t(count) * sizeof(float));
            QByteArray().swap(out.meta.data);   // one copy of the block, not two
            for (qsizetype k = 0; k < count; ++k)
                if (!std::isfinite(dst[k]))
                    dst[k] = sentinel;
            return out;
        }));
    }
}

void vtkWindowCube::onLodBlock(quint64 generation, int index, const LodBlock &block)
{
    const std::shared_ptr<LodPending> pending = this->m_lodPending;
    if (!pending || pending->generation != generation || index < 0
        || index >= int(pending->results.size()) || pending->arrived[size_t(index)])
        return;
    if (!block.meta.valid || !block.array) {
        // One block missing is a box with a hole: give up on this one.
        const bool automatic = pending->automatic;
        this->cancelLodFetch();
        const QString error = block.meta.error.isEmpty() ? u"no answer"_s : block.meta.error;
        qDebug().noquote() << u"[lod] gen=%1 block %2 failed: %3"_s.arg(generation).arg(index).arg(error);
        this->statusBar()->showMessage(
                (automatic ? u"Refining the view failed: %1"_s : u"Refine Selected Box failed: %1"_s).arg(error),
                10000);
        return;
    }
    pending->results[size_t(index)] = block;
    pending->arrived[size_t(index)] = true;
    if (--pending->remaining == 0)
        this->applyLodSet();
}

void vtkWindowCube::cancelLodFetch()
{
    if (!this->m_lodPending)
        return;
    this->m_lodPending.reset();
    this->m_lodLiveGeneration->store(++this->m_lodGeneration);
    this->clearPersistentStatusMessage();
    this->updateRefineBoxAction();
}

void vtkWindowCube::applyLodSet()
{
    const std::shared_ptr<LodPending> pending = std::move(this->m_lodPending);
    this->m_lodPending.reset();
    this->clearPersistentStatusMessage();
    this->updateRefineBoxAction();
    if (!pending)
        return;
    const QString failed = pending->automatic ? u"Refining the view failed"_s
                                              : u"Refine Selected Box failed"_s;
    const qint64 fetchedMs = pending->timer.elapsed();
    auto *renderer = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    auto *previewImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    const LodGateResult gate = this->lodGate();
    const CubeLodGeometry::Sampling &preview = pending->preview;
    if (!renderer || !gate.active || previewImage != pending->previewImage
        || preview.scale != this->m_displaySampling.scale || preview.offset != this->m_displaySampling.offset) {
        // The view changed while the blocks were read: they belong to a
        // preview (or a drawing mode) that is no longer on screen.
        if (!pending->automatic)
            this->statusBar()->showMessage(u"Refine Selected Box: the view changed while the box was "
                                           u"read; refine it again."_s,
                                           8000);
        return;
    }

    std::vector<CubeLodAssembly::Block> assembly;
    std::vector<CubeLodVolumes::Block> blocks;
    QString fineSource;
    int keptOnGpu = 0;

    // The fine blocks, as the backend sent them.
    for (size_t i = 0; i < pending->fine.size(); ++i) {
        CubeLodAssembly::Block b = pending->fine[i];
        const LodBlock &r = pending->results[i];
        const CubeLodGeometry::Dims got{ r.meta.width, r.meta.height, r.meta.depth };
        if (got != CubeLodAssembly::dims(b) || r.meta.levelOrigin != b.samples.lo
            || !r.array || r.array->GetNumberOfTuples() != vtkIdType(b.samples.count())) {
            this->statusBar()->showMessage(
                    u"%1: block %2 came back as %3 x %4 x %5 samples from (%6, %7, %8)."_s.arg(failed)
                            .arg(i)
                            .arg(r.meta.width)
                            .arg(r.meta.height)
                            .arg(r.meta.depth)
                            .arg(r.meta.levelOrigin[0])
                            .arg(r.meta.levelOrigin[1])
                            .arg(r.meta.levelOrigin[2]),
                    10000);
            return;
        }
        // Where its samples stand, from the answer: block means of the
        // pyramid, the cube itself, or a strided read without a pyramid.
        b.sampling = { double(CubeLodGeometry::levelScale(r.meta.level)), r.meta.voxelOffset };
        if (!CubeLodAssembly::covers(b)) {
            this->statusBar()->showMessage(u"%1: block %2 does not reach its faces."_s.arg(failed).arg(i),
                                           10000);
            return;
        }
        fineSource = r.meta.source;
        CubeLodVolumes::Block vb;
        vb.geometry = CubeLodAssembly::geometryOf(b, pending->cubeShape, preview);
        vb.key = fineKey(b.node, pending->plan.nodeSize);
        if (this->m_lod && this->m_lod->holds(vb.key, vb.geometry))
            ++keptOnGpu;
        vb.array = r.array;   // shared with the cache, not copied
        assembly.push_back(b);
        blocks.push_back(std::move(vb));
    }

    // The coarse pieces around the box, cut from the preview on screen; a
    // piece the view already draws (the same box around the same preview)
    // is kept as it is.
    int extent[6];
    previewImage->GetExtent(extent);
    const CubeLodGeometry::Dims previewShape{ extent[1] - extent[0] + 1, extent[3] - extent[2] + 1,
                                              extent[5] - extent[4] + 1 };
    const auto *previewValues = static_cast<const float *>(previewImage->GetScalarPointer());
    const vtkMTimeType previewTime = previewImage->GetMTime();
    for (const CubeLodAssembly::Block &b :
         CubeLodAssembly::coarsePieces(pending->cubeShape, preview, pending->plan.cubeBox)) {
        CubeLodVolumes::Block vb;
        vb.geometry = CubeLodAssembly::geometryOf(b, pending->cubeShape, preview);
        vb.key = coarseKey(b, previewImage, previewTime);
        if (this->m_lod && this->m_lod->holds(vb.key, vb.geometry))
            ++keptOnGpu;
        else
            vb.samples = CubeLodAssembly::cutSamples(previewValues, previewShape, b.samples);
        assembly.push_back(b);
        blocks.push_back(std::move(vb));
    }

    // Back to front for the camera of every frame. The eye goes from the
    // view's world to cube voxels, where the plan's boxes are.
    const CubeLodGeometry::Box fineBox = pending->plan.cubeBox;
    const int nodeSize = pending->plan.nodeSize;
    CubeLodVolumes::OrderFunction order = [assembly, fineBox, nodeSize,
                                           preview](const std::array<double, 3> &eyeWorld) {
        const std::array<double, 3> eye{ CubeLodGeometry::toCube(eyeWorld[0], preview),
                                         CubeLodGeometry::toCube(eyeWorld[1], preview),
                                         CubeLodGeometry::toCube(eyeWorld[2], preview) };
        return CubeLodAssembly::drawOrder(assembly, fineBox, eye, nodeSize);
    };

    QElapsedTimer swapTimer;
    swapTimer.start();
    if (!this->m_lod)
        this->m_lod = std::make_unique<CubeLodVolumes>(this->volume->GetProperty());
    this->m_lod->copySettingsFrom(this->volumeMapper);
    this->m_lod->setVisible(this->m_show3dRendering);
    if (!this->m_lod->replace(renderer, std::move(blocks), std::move(order))) {
        // Nothing was swapped in: what was shown stays, if anything was.
        this->statusBar()->showMessage(u"%1: a block could not be built."_s.arg(failed), 10000);
        return;
    }
    this->applyLodUserCrop();
    this->volume->SetVisibility(0);   // the pieces and blocks draw the whole cube now
    this->m_lodShownExplicit = !pending->automatic;
    this->m_lodShownSignature = pending->automatic ? pending->signature : CubeLodAuto::Signature{};

    this->m_lodStatus = CubeLodStatus::detailText(pending->plan.level, fineSource, fineBox,
                                                  this->m_previewSource, this->m_previewLevel,
                                                  int(preview.scale));
    ui->vtkCube->renderWindow()->Render();
    const qint64 swapMs = swapTimer.elapsed();
    this->statusBar()->showMessage(this->m_lodStatus, pending->automatic ? 6000 : 15000);
    this->updateDataStatePanel();
    this->updateRefineBoxAction();
    qDebug().noquote() << u"[lod] applied gen=%1 %2 blocks=%3 (%4 fine, %5 from cache, %6 kept on the GPU) "
                          u"gpuMB=%7 cacheMB=%8 fetch=%9 ms swap+render=%10 ms total=%11 ms; %12"_s
                                  .arg(pending->generation)
                                  .arg(pending->automatic ? u"auto"_s : u"explicit"_s)
                                  .arg(this->m_lod->blockCount())
                                  .arg(pending->fine.size())
                                  .arg(pending->fromCache)
                                  .arg(keptOnGpu)
                                  .arg(double(this->m_lod->bytes()) / (1 << 20), 0, 'f', 1)
                                  .arg(this->m_lodCache ? double(this->m_lodCache->bytes()) / (1 << 20) : 0.0, 0,
                                       'f', 1)
                                  .arg(fetchedMs)
                                  .arg(swapMs)
                                  .arg(pending->timer.elapsed())
                                  .arg(this->m_lodStatus);
}

void vtkWindowCube::clearLod(const QString &why, bool dropCache)
{
    // A request in flight is dropped: its blocks are ignored as they come,
    // and those not sent yet are never sent.
    if (this->m_lodLiveGeneration)
        this->m_lodLiveGeneration->store(++this->m_lodGeneration);
    const bool wasPending = static_cast<bool>(this->m_lodPending);
    this->m_lodPending.reset();
    if (wasPending)
        this->clearPersistentStatusMessage();
    const bool wasShown = this->m_lod && this->m_lod->active();
    if (this->m_lod)
        this->m_lod->clear();
    this->m_lodShownExplicit = false;
    this->m_lodShownSignature = CubeLodAuto::Signature{};
    if (dropCache && this->m_lodCache)
        this->m_lodCache->clear();
    if (wasShown) {
        this->volume->SetVisibility(this->m_show3dRendering ? 1 : 0);
        this->m_lodStatus.clear();
        this->updateDataStatePanel();
        if (!why.isEmpty())
            this->statusBar()->showMessage(why, 8000);
    }
    if (this->m_actClearRefinedBox)
        this->m_actClearRefinedBox->setEnabled(false);
}
