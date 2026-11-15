// vtkWindowCube_Lod.cpp
// Level of detail of the 3-D view of a large cube (issue #321): Tools ▸ Views ▸
// Refine Selected Box reads a box of the cube at the finest level the detail
// budget allows and draws it over the preview, which keeps covering the rest
// of the cube as at most six coarse pieces around the box.
//
// The view's world frame stays the preview's, so every overlay, region, the
// lasso, the WCS and the camera stay where they are; only the volume layer
// changes. Its blocks share the window's volume property (colour map,
// threshold, scale and opacity apply to all of them at once) and are drawn by
// CubeLodVolumes. The geometry is CubeLodGeometry / CubeLodAssembly and the
// choice of level CubeLodPlan, all pure and unit-tested; when it may be shown
// at all is CubeLodGate.
//
// The blocks are read in the background, a few at a time, and the view
// changes only once every block of the box has arrived, so it never shows a
// hole. Anything that replaces the displayed cube or changes how it is drawn
// (full resolution, a region, a new preview, the backend rendering, MIP or
// MinIP, the isosurface, a headset) ends the refined box.

#include "vtkWindowCube.h"

#include "ui_vtkWindowCube.h"
#include "CubeLodAssembly.h"
#include "CubeLodGate.h"
#include "CubeLodPlan.h"
#include "CubeLodStatus.h"
#include "CubeLodVolumes.h"
#include "CubeVRController.h"
#include "HostMemory.h"
#include "Settings.h"

#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QStatusBar>
#include <QThreadPool>
#include <QtConcurrent>

#include <cmath>

using namespace Qt::StringLiterals;

namespace {

/// Node side, in level voxels, of the blocks a box is read in: a multiple of
/// the pyramid's 64-voxel bricks, so every read is whole chunks, and 258^3
/// samples with their ghosts, under any GPU's 3-D texture limit.
constexpr int kNodeSize = 256;
/// GL_MAX_3D_TEXTURE_SIZE every desktop OpenGL 4.1 driver offers. The plan
/// halves the node side below it; at 256 it never has to.
constexpr int kMaxTextureSide = 2048;
/// At most this many fine blocks: each is one ray-casting pass per frame.
constexpr int kMaxBlocks = 16;

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

}   // namespace

/// A refine in flight: what was planned and what has arrived.
struct vtkWindowCube::LodPending
{
    quint64 generation{ 0 };
    CubeLodPlan::Plan plan;
    CubeLodGeometry::Dims cubeShape{ 0, 0, 0 };
    CubeLodGeometry::Sampling preview;
    /// The preview image the coarse pieces will be cut from: if the window
    /// shows another one by the time the blocks arrive, the set is dropped.
    vtkImageData *previewImage{ nullptr };
    std::vector<CubeLodAssembly::Block> fine;   ///< one per node, before reading
    std::vector<BackendRegionLevelResult> results;
    std::vector<bool> arrived;
    int remaining{ 0 };
    QElapsedTimer timer;
};

void vtkWindowCube::setupLevelOfDetail()
{
    this->m_lodLiveGeneration = std::make_shared<std::atomic<quint64>>(0);
    this->m_actRefineBox = new QAction(u"Refine Selected Box…"_s, ui->menuTools);
    this->m_actRefineBox->setToolTip(
            u"Show a box of the cube at a finer level over the 3-D preview: the box is read from "
            u"the backend at the finest level the detail budget allows (Settings ▸ General ▸ Large "
            u"Cubes) and drawn in place of the preview there."_s);
    QObject::connect(this->m_actRefineBox, &QAction::triggered, this,
                     &vtkWindowCube::showRefineBoxDialog);
    ui->menuTools->addAction(this->m_actRefineBox);
    this->updateRefineBoxAction();
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
    const LodGateResult gate = this->lodGate();
    if (this->m_actRefineBox) {
        const QString base =
                u"Show a box of the cube at a finer level over the 3-D preview: the box is read "
                u"from the backend at the finest level the detail budget allows (Settings ▸ "
                u"General ▸ Large Cubes) and drawn in place of the preview there."_s;
        this->m_actRefineBox->setEnabled(gate.active);
        this->m_actRefineBox->setToolTip(gate.active ? base : gate.reason);
        this->m_actRefineBox->setStatusTip(gate.active ? base : gate.reason);
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

void vtkWindowCube::refineBox(const std::array<int, 6> &cubeBox)
{
    const LodGateResult gate = this->lodGate();
    if (!gate.active) {
        this->statusBar()->showMessage(u"Refine Selected Box: %1"_s.arg(gate.reason), 6000);
        return;
    }
    auto *previewImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!previewImage || previewImage->GetScalarType() != VTK_FLOAT
        || previewImage->GetNumberOfScalarComponents() != 1) {
        this->statusBar()->showMessage(u"Refine Selected Box: the preview is not loaded yet."_s, 6000);
        return;
    }

    const CubeLodGeometry::Dims cube{ this->remoteDatasetWidth, this->remoteDatasetHeight,
                                      this->remoteDatasetDepth };
    CubeLodPlan::Inputs in;
    in.cubeShape = cube;
    in.levelCount = std::max(1, this->m_lodPyramid.levels);
    in.previewStride = int(this->m_displaySampling.scale);
    in.budgetBytes = lodBudgetBytes();
    in.maxTextureSide = kMaxTextureSide;
    in.maxBlocks = kMaxBlocks;
    in.nodeSize = kNodeSize;
    // Inclusive bounds to the plan's half-open box.
    const CubeLodGeometry::Box box{ { cubeBox[0], cubeBox[2], cubeBox[4] },
                                    { cubeBox[1] + 1, cubeBox[3] + 1, cubeBox[5] + 1 } };
    const CubeLodPlan::Plan plan = CubeLodPlan::planBox(box, in);
    if (!plan.refine) {
        this->statusBar()->showMessage(
                u"Refine Selected Box: %1."_s.arg(QString::fromUtf8(CubeLodPlan::reasonText(plan.reason))),
                8000);
        return;
    }

    // A new request: whatever was in flight is moot. What is on screen stays
    // until the new set is complete.
    const quint64 generation = ++this->m_lodGeneration;
    this->m_lodLiveGeneration->store(generation);
    auto pending = std::make_shared<LodPending>();
    pending->generation = generation;
    pending->plan = plan;
    pending->cubeShape = cube;
    pending->preview = this->m_displaySampling;
    pending->previewImage = previewImage;
    pending->timer.start();
    for (const CubeLodGeometry::Node &n : plan.nodes)
        pending->fine.push_back(CubeLodAssembly::fineBlock(cube, plan.nodeSize, n, CubeLodPlan::kGhost));
    pending->results.resize(pending->fine.size());
    pending->arrived.assign(pending->fine.size(), false);
    pending->remaining = int(pending->fine.size());
    this->m_lodPending = pending;

    const qint64 bytes = CubeLodPlan::nodeBytes(in, plan.nodes);
    QString what = u"level %1, %2 blocks, %3 MB"_s.arg(plan.level)
                           .arg(plan.nodes.size())
                           .arg(double(bytes) / (1 << 20), 0, 'f', 0);
    if (plan.reason == CubeLodPlan::Reason::Shrunk)
        what += u"; the budget holds only part of the box, around its centre"_s;
    this->showPersistentStatusMessage(u"Refining the box (%1)…"_s.arg(what));
    qDebug().noquote() << u"[lod] refine gen=%1 box=%2 level=%3 nodes=%4 bytes=%5 reason=%6"_s
                                  .arg(generation)
                                  .arg(CubeLodStatus::boxText(plan.cubeBox))
                                  .arg(plan.level)
                                  .arg(plan.nodes.size())
                                  .arg(bytes)
                                  .arg(QString::fromUtf8(CubeLodPlan::reasonText(plan.reason)));

    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString session = this->remoteSessionId;
    const QString dataset = this->remoteDatasetId;
    const std::shared_ptr<std::atomic<quint64>> live = this->m_lodLiveGeneration;
    for (int i = 0; i < int(plan.nodes.size()); ++i) {
        const CubeLodGeometry::Box level =
                CubeLodGeometry::nodeLevelBox(cube, plan.nodeSize, plan.nodes[size_t(i)]);
        const int lvl = plan.level;
        auto *watcher = new QFutureWatcher<BackendRegionLevelResult>(this);
        QObject::connect(watcher, &QFutureWatcher<BackendRegionLevelResult>::finished, this,
                         [this, watcher, generation, i]() {
                             const BackendRegionLevelResult r = watcher->result();
                             watcher->deleteLater();
                             this->onLodBlock(generation, i, r);
                         });
        watcher->setFuture(QtConcurrent::run(lodPool(), [=]() {
            BackendRegionLevelResult r;
            if (live->load() != generation) {
                r.error = u"superseded"_s;   // a newer refine or a clear came first
                return r;
            }
            BackendClient client(url, token);
            client.setSessionId(session);
            return client.requestRegionLevel(dataset, lvl, level.lo[0], level.hi[0] - 1, level.lo[1],
                                             level.hi[1] - 1, level.lo[2], level.hi[2] - 1,
                                             CubeLodPlan::kGhost, u"auto"_s);
        }));
    }
}

void vtkWindowCube::onLodBlock(quint64 generation, int index, const BackendRegionLevelResult &block)
{
    const std::shared_ptr<LodPending> pending = this->m_lodPending;
    if (!pending || pending->generation != generation || index < 0
        || index >= int(pending->results.size()) || pending->arrived[size_t(index)])
        return;
    if (!block.valid) {
        // One block missing is a box with a hole: give up on this one.
        this->m_lodPending.reset();
        this->m_lodLiveGeneration->store(++this->m_lodGeneration);
        this->clearPersistentStatusMessage();
        this->statusBar()->showMessage(
                u"Refine Selected Box failed: %1"_s.arg(block.error.isEmpty() ? u"no answer"_s : block.error),
                10000);
        return;
    }
    pending->results[size_t(index)] = block;
    pending->arrived[size_t(index)] = true;
    if (--pending->remaining == 0)
        this->applyLodSet();
}

void vtkWindowCube::applyLodSet()
{
    const std::shared_ptr<LodPending> pending = std::move(this->m_lodPending);
    this->m_lodPending.reset();
    this->clearPersistentStatusMessage();
    if (!pending)
        return;
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
        this->statusBar()->showMessage(u"Refine Selected Box: the view changed while the box was read; "
                                       u"refine it again."_s,
                                       8000);
        return;
    }

    const float sentinel = static_cast<float>(this->currentCubeInvisibleSentinel);
    std::vector<CubeLodAssembly::Block> assembly;
    std::vector<CubeLodVolumes::Block> blocks;
    QString fineSource;

    // The fine blocks, as the backend sent them.
    for (size_t i = 0; i < pending->fine.size(); ++i) {
        CubeLodAssembly::Block b = pending->fine[i];
        BackendRegionLevelResult &r = pending->results[i];
        const CubeLodGeometry::Dims got{ r.width, r.height, r.depth };
        if (got != CubeLodAssembly::dims(b) || r.levelOrigin != b.samples.lo) {
            this->statusBar()->showMessage(
                    u"Refine Selected Box failed: block %1 came back as %2 x %3 x %4 samples from "
                    u"(%5, %6, %7)."_s.arg(i)
                            .arg(r.width)
                            .arg(r.height)
                            .arg(r.depth)
                            .arg(r.levelOrigin[0])
                            .arg(r.levelOrigin[1])
                            .arg(r.levelOrigin[2]),
                    10000);
            return;
        }
        // Where its samples stand, from the answer: block means of the
        // pyramid, the cube itself, or a strided read without a pyramid.
        b.sampling = { double(CubeLodGeometry::levelScale(r.level)), r.voxelOffset };
        if (!CubeLodAssembly::covers(b)) {
            this->statusBar()->showMessage(
                    u"Refine Selected Box failed: block %1 does not reach its faces."_s.arg(i), 10000);
            return;
        }
        fineSource = r.source;
        CubeLodVolumes::Block vb;
        vb.geometry = CubeLodAssembly::geometryOf(b, pending->cubeShape, preview);
        const auto *src = reinterpret_cast<const float *>(r.data.constData());
        vb.samples.assign(src, src + r.data.size() / qsizetype(sizeof(float)));
        QByteArray().swap(r.data);   // one copy of the block at a time, not two
        // Blanks as the preview has them: the same invisible value, never a
        // NaN the ray caster would spread through its interpolation. The
        // visible range stays the preview's, so colours match across the face.
        for (float &v : vb.samples)
            if (!std::isfinite(v))
                v = sentinel;
        assembly.push_back(b);
        blocks.push_back(std::move(vb));
    }

    // The coarse pieces around the box, cut from the preview on screen.
    int extent[6];
    previewImage->GetExtent(extent);
    const CubeLodGeometry::Dims previewShape{ extent[1] - extent[0] + 1, extent[3] - extent[2] + 1,
                                              extent[5] - extent[4] + 1 };
    const auto *previewValues = static_cast<const float *>(previewImage->GetScalarPointer());
    for (const CubeLodAssembly::Block &b :
         CubeLodAssembly::coarsePieces(pending->cubeShape, preview, pending->plan.cubeBox)) {
        CubeLodVolumes::Block vb;
        vb.geometry = CubeLodAssembly::geometryOf(b, pending->cubeShape, preview);
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
        // Nothing was swapped in: the preview volume stays as it was.
        this->clearLod();
        this->statusBar()->showMessage(u"Refine Selected Box failed: a block could not be built."_s,
                                       10000);
        return;
    }
    this->applyLodUserCrop();
    this->volume->SetVisibility(0);   // the pieces and blocks draw the whole cube now

    this->m_lodStatus = CubeLodStatus::detailText(pending->plan.level, fineSource, fineBox,
                                                  this->m_previewSource, this->m_previewLevel,
                                                  int(preview.scale));
    ui->vtkCube->renderWindow()->Render();
    const qint64 swapMs = swapTimer.elapsed();
    this->statusBar()->showMessage(this->m_lodStatus, 15000);
    this->updateDataStatePanel();
    qDebug().noquote() << u"[lod] applied gen=%1 blocks=%2 (%3 fine) gpuMB=%4 fetch=%5 ms swap+render=%6 ms "
                          u"total=%7 ms; %8"_s
                                  .arg(pending->generation)
                                  .arg(this->m_lod->blockCount())
                                  .arg(pending->fine.size())
                                  .arg(double(this->m_lod->bytes()) / (1 << 20), 0, 'f', 1)
                                  .arg(fetchedMs)
                                  .arg(swapMs)
                                  .arg(pending->timer.elapsed())
                                  .arg(this->m_lodStatus);
}

void vtkWindowCube::clearLod(const QString &why)
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
    if (wasShown) {
        this->volume->SetVisibility(this->m_show3dRendering ? 1 : 0);
        this->m_lodStatus.clear();
        this->updateDataStatePanel();
        if (!why.isEmpty())
            this->statusBar()->showMessage(why, 8000);
    }
}
