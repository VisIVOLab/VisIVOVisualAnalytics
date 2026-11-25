// vtkWindowCube_Amr.cpp
// The cube window's side of a cube made from an AMR output (issue #550): its
// AMR* header cards say where it came from, and then the window can draw the
// mesh's cells that the slice cuts, coloured by refinement level, and refine a
// box of itself (the same output resampled over that box at the finest level it
// holds, opened in a new window beside this one). The arithmetic is in
// app/AmrView.h; the backend serves the cells (/v1/simulations/amr/level-map)
// and the resampling (/amr/plan, /amr/uniform).

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "InspectorPanel.h"
#include "RegionPanePlacement.h"
#include "ToolProgressSheet.h"
#include "app/BackendClient.h"

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

#include <QAction>
#include <QCheckBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

using namespace Qt::StringLiterals;
namespace av = visivo::amrview;

namespace {
/// The cells of one slice that are drawn: more than this and the coarsest levels
/// are kept (the backend says so), which the label reports.
constexpr int kMaxBlocks = 200000;
/// The limit the refinement is planned against, as the dialog's default.
constexpr qint64 kRefineMaxVoxels = 64LL * 1000000;
}   // namespace

std::optional<std::array<av::AxisScale, 3>> vtkWindowCube::amrAxes() const
{
    std::array<av::AxisScale, 3> axes;
    for (int i = 0; i < 3; ++i) {
        bool okOrigin = false, okNext = false;
        const double origin = this->remoteVoxelToNative(i, 0.0, &okOrigin);
        const double next = this->remoteVoxelToNative(i, 1.0, &okNext);
        if (!okOrigin || !okNext)
            return std::nullopt;
        axes[std::size_t(i)] = { origin, next - origin };
    }
    return axes;
}

void vtkWindowCube::detectAmrCube()
{
    if (!m_simulationVolume || this->filepath.isEmpty())
        return;
    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString path = this->filepath;
    auto *watcher = new QFutureWatcher<BackendFileHeaderResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendFileHeaderResult>::finished, this,
                     [self, watcher]() {
                         const BackendFileHeaderResult header = watcher->result();
                         watcher->deleteLater();
                         if (!self || !header.valid)
                             return;
                         const auto provenance = av::provenanceFromCards(header.cards);
                         if (!provenance)
                             return;
                         self->m_amr = provenance;
                         self->showAmrTools();
                         // One more Tools entry, for this kind of cube only.
                         if (!self->m_actRefineAmr) {
                             self->m_actRefineAmr =
                                     new QAction(u"Refine Box of the AMR Mesh…"_s, self->ui->menuTools);
                             self->m_actRefineAmr->setToolTip(
                                     u"Resample the box you choose from the AMR output at the finest "
                                     u"level it holds, and open the result beside this window."_s);
                             QObject::connect(self->m_actRefineAmr, &QAction::triggered, self.data(),
                                              &vtkWindowCube::refineAmrBox);
                             self->ui->menuTools->addAction(self->m_actRefineAmr);
                         }
                         auto *renderer = self->sliceWin->GetRenderers()->GetFirstRenderer();
                         self->m_amrBlocks.attach(renderer);
                         self->m_amrBlocksTimer.setSingleShot(true);
                         self->m_amrBlocksTimer.setInterval(150);
                         QObject::connect(&self->m_amrBlocksTimer, &QTimer::timeout, self.data(),
                                          &vtkWindowCube::fetchAmrBlocks);
                     });
    watcher->setFuture(QtConcurrent::run([url, token, path]() {
        BackendClient client(url, token);
        return client.fileHeader(path);
    }));
}

void vtkWindowCube::addAmrInspectorRows(InspectorPanel *panel)
{
    if (!panel || !m_simulationVolume)
        return;
    // Built hidden and shown once the header says this is an AMR cube: it is
    // read after the Inspector is up.
    m_amrCaption = panel->addPropertyCaption(u"AMR MESH"_s);
    auto *host = new QWidget();
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    m_amrStatus = new QLabel();
    m_amrStatus->setWordWrap(true);
    layout->addWidget(m_amrStatus);
    m_amrBlocksCheck = new QCheckBox(u"Show AMR blocks"_s);
    m_amrBlocksCheck->setToolTip(
            u"Outline the cells of the mesh that this slice cuts, coloured by refinement level "
            u"(blue the coarsest, red the finest). It follows the slice."_s);
    // An Inspector rebuilt while the blocks are shown keeps the box ticked.
    m_amrBlocksCheck->setChecked(m_amrBlocksOn);
    QObject::connect(m_amrBlocksCheck, &QCheckBox::toggled, this,
                     &vtkWindowCube::setAmrBlocksShown);
    layout->addWidget(m_amrBlocksCheck);
    auto *refine = new QPushButton(u"Refine this box…"_s);
    refine->setToolTip(
            u"Pick a box of this cube and resample it from the AMR output at the finest level it "
            u"holds; it opens in a new window."_s);
    QObject::connect(refine, &QPushButton::clicked, this, &vtkWindowCube::refineAmrBox);
    layout->addWidget(refine);
    m_amrHost = host;
    panel->addPropertiesSection(host);
    host->hide();
    m_amrCaption->hide();
    if (m_amr)   // the Inspector was rebuilt after the header was read
        this->showAmrTools();
}

void vtkWindowCube::showAmrTools()
{
    if (!m_amr)
        return;
    if (m_amrHost)
        m_amrHost->show();
    if (m_amrCaption)
        m_amrCaption->show();
    if (m_amrStatus)
        m_amrStatus->setText(u"%1 at level %2 of %3, from %4."_s
                                     .arg(m_amr->field)
                                     .arg(m_amr->level)
                                     .arg(m_amr->maxLevel)
                                     .arg(m_amr->code));
}

void vtkWindowCube::setAmrBlocksShown(bool shown)
{
    m_amrBlocksOn = shown && m_amr.has_value();
    if (!m_amrBlocksOn) {
        ++m_amrBlocksSeq;   // an answer still on its way is not wanted
        m_amrBlocks.hide();
        this->sliceWin->Render();
        return;
    }
    this->fetchAmrBlocks();
}

void vtkWindowCube::scheduleAmrBlocks()
{
    if (!m_amrBlocksOn)
        return;
    // A scrub through the slices is one request, for the one it stops on.
    ++m_amrBlocksSeq;
    m_amrBlocks.hide();
    m_amrBlocksTimer.start();
}

void vtkWindowCube::fetchAmrBlocks()
{
    if (!m_amrBlocksOn || !m_amr || m_displayedSliceIndex < 0)
        return;
    if (!this->m_sliceBackdropProductId.isEmpty()) {
        // The slice pane shows a product, not the cube: the blocks would not
        // belong to the picture. Say so rather than leave the box ticked silently.
        m_amrBlocks.hide();
        this->statusBar()->showMessage(
                u"AMR blocks are drawn over the cube's own slice, not over a product shown "
                u"in its place."_s,
                6000);
        return;
    }
    const int normal = static_cast<int>(m_sliceAxis);
    const int index = m_displayedSliceIndex;
    const int seq = ++m_amrBlocksSeq;
    const QString axis = QString(QChar::fromLatin1(char('x' + normal)));
    const double coordinate = this->remoteVoxelToNative(normal, double(index), nullptr);
    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString source = m_amr->source;
    auto *watcher = new QFutureWatcher<av::LevelMap>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(
            watcher, &QFutureWatcher<av::LevelMap>::finished, this,
            [self, watcher, seq, index, normal]() {
                const av::LevelMap map = watcher->result();
                watcher->deleteLater();
                // The slice moved on (or the blocks went off) while this was asked.
                if (!self || seq != self->m_amrBlocksSeq || !self->m_amrBlocksOn
                    || index != self->m_displayedSliceIndex
                    || normal != static_cast<int>(self->m_sliceAxis))
                    return;
                if (!map.valid) {
                    self->m_amrBlocks.hide();
                    self->statusBar()->showMessage(
                            u"AMR blocks: %1"_s.arg(map.error), 6000);
                    self->sliceWin->Render();
                    return;
                }
                const auto placement = self->regionPlacement(self->sliceWin.GetPointer());
                if (!placement) {
                    self->m_amrBlocks.hide();
                    return;
                }
                const auto scales = self->amrAxes();
                if (!scales) {
                    self->m_amrBlocks.hide();
                    self->statusBar()->showMessage(
                            u"AMR blocks: this cube's axes have no world coordinates."_s, 6000);
                    return;
                }
                const auto &axes = *scales;
                const auto plane = av::planeAxes(normal);
                const int lowest = map.levels.firstKey(), highest = map.levels.lastKey();
                QVector<AmrBlockOverlay::Group> groups;
                for (auto it = map.levels.constBegin(); it != map.levels.constEnd(); ++it) {
                    AmrBlockOverlay::Group group;
                    group.rgb = av::levelColour(it.key(), lowest, highest);
                    group.rects.reserve(it.value().size());
                    for (const av::Rect &r : it.value()) {
                        const av::Rect v = av::rectToVoxels(r, axes[std::size_t(plane[0])],
                                                            axes[std::size_t(plane[1])]);
                        const auto a = placement->toWorld({ v[0], v[1] });
                        const auto b = placement->toWorld({ v[2], v[3] });
                        group.rects.append({ a[0], a[1], b[0], b[1] });
                    }
                    groups.append(group);
                }
                self->m_amrBlocks.show(groups);
                QString text = u"%1 cells cut by this slice, levels %2 to %3."_s
                                       .arg(map.cells).arg(lowest).arg(highest);
                if (map.truncated)
                    text += u" Too many to draw: the finest levels are left out."_s;
                if (self->m_amrBlocksCheck)
                    self->m_amrBlocksCheck->setToolTip(text);
                self->statusBar()->showMessage(text, 6000);
                self->sliceWin->Render();
            });
    watcher->setFuture(QtConcurrent::run([url, token, source, axis, coordinate]() {
        BackendClient client(url, token);
        return client.amrLevelMap(source, axis, coordinate, kMaxBlocks);
    }));
}

void vtkWindowCube::runAmrRefine(const std::array<int, 6> &box, QWidget *owner,
                                 const std::function<void(bool)> &setBusy)
{
    if (!m_amr || m_refineAmrBusy)
        return;
    const auto axes = this->amrAxes();
    const av::Region region = axes ? av::regionFromVoxelBox(box, *axes) : av::Region{};
    if (!region.set) {
        QMessageBox::warning(owner, u"Refine a Box"_s,
                             u"This cube's axes have no usable coordinates, so the box cannot be "
                             u"named to the mesh."_s);
        return;
    }
    m_refineAmrBusy = true;
    setBusy(true);
    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString source = m_amr->source;
    const QString field = m_amr->field;

    // First what it would cost: the user sees the level and the memory before
    // the job is taken.
    auto *planWatcher = new QFutureWatcher<av::Plan>(this);
    QPointer<vtkWindowCube> self(this);
    QPointer<QWidget> ownerPtr(owner);
    QObject::connect(
            planWatcher, &QFutureWatcher<av::Plan>::finished, this,
            [self, planWatcher, ownerPtr, setBusy, url, token, source, field, region]() {
                const av::Plan plan = planWatcher->result();
                planWatcher->deleteLater();
                if (!self)
                    return;
                QWidget *parent = ownerPtr ? ownerPtr.data() : static_cast<QWidget *>(self.data());
                const auto done = [self, setBusy]() {
                    if (self)
                        self->m_refineAmrBusy = false;
                    setBusy(false);
                };
                if (!plan.valid) {
                    done();
                    QMessageBox::warning(parent, u"Refine a Box"_s, plan.error);
                    return;
                }
                if (!plan.fits) {
                    done();
                    QMessageBox::warning(parent, u"Refine a Box"_s,
                                         u"This box is too large to resample: %1"_s.arg(plan.reason));
                    return;
                }
                QMessageBox ask(QMessageBox::Question, u"Refine a Box"_s,
                                av::summary(plan) + u"<br><br>Resample this box at that level?"_s,
                                QMessageBox::Yes | QMessageBox::No, parent);
                ask.setTextFormat(Qt::RichText);
                const bool yes = ask.exec() == QMessageBox::Yes;
                // The window or the dialog may have gone while the question was open.
                if (!yes || !self) {
                    done();
                    return;
                }
                parent = ownerPtr ? ownerPtr.data() : static_cast<QWidget *>(self.data());
                // The level is left to the backend: the finest the box holds.
                auto *runWatcher = new QFutureWatcher<av::Result>(self.data());
                auto *sheet = visivo::showToolProgressSheet(parent, u"Refining the box"_s);
                QPointer<QProgressDialog> sheetPtr(sheet);
                QObject::connect(
                        runWatcher, &QFutureWatcher<av::Result>::finished, self.data(),
                        [self, runWatcher, sheetPtr, parent = QPointer<QWidget>(parent), done]() mutable {
                            const av::Result result = runWatcher->result();
                            runWatcher->deleteLater();
                            visivo::closeToolProgressSheet(sheetPtr);
                            done();
                            if (!self)
                                return;
                            QWidget *where = parent ? parent.data() : static_cast<QWidget *>(self.data());
                            if (!result.valid) {
                                QMessageBox::warning(where, u"Refine a Box"_s, result.error);
                                return;
                            }
                            emit self->openBackendFitsRequested(
                                    result.outputs.first().path,
                                    u"%1, box at level %2"_s.arg(result.outputs.first().field)
                                            .arg(result.plan.level));
                            self->statusBar()->showMessage(
                                    u"Refined box at level %1 opened in a new window."_s
                                            .arg(result.plan.level),
                                    6000);
                        });
                runWatcher->setFuture(QtConcurrent::run([url, token, source, field, region]() {
                    BackendClient client(url, token);
                    return client.extractAmrUniform(source, field, -1, region, kRefineMaxVoxels);
                }));
            });
    planWatcher->setFuture(QtConcurrent::run([url, token, source, field, region]() {
        BackendClient client(url, token);
        return client.planAmrUniform(source, field, -1, region, kRefineMaxVoxels);
    }));
}
