// vtkWindowCube_Cosmology.cpp
// The cube viewer's moment-map texture tools (issue #29): the angular power
// spectrum and the skeleton / void detector, on the moment map on screen.
//
// Both backend tools read a DATASET (POST /v1/cosmology/angular_power/{id},
// POST /v1/analysis/skeleton/{id}), and a moment map is not one: it lives in
// this viewer. So a run first writes the moment to the Workspace Exports with
// the same task "Export Moment Map as FITS" uses (at full resolution, whatever
// the pane shows), opens that file, and analyses it. The dataset is kept per
// moment product and reused until that moment is recomputed.
//
// Results are products like every other tool's: the P(ℓ) plot is a window
// (reopened from its row), the skeleton or void mask an image product that
// mounts in a pane. Both carry the replay record — moment export, open,
// analysis — so the recipe export reproduces them.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "CosmologyToolWidgets.h"
#include "app/DatasetName.h"
#include "ToolProgressSheet.h"
#include "ToolResultDialog.h"
#include "WorkspaceChrome.h"
#include "app/BackendClient.h"

#include <QAction>
#include <QDockWidget>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QtConcurrentRun>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include <cstring>
#include <memory>

using namespace Qt::StringLiterals;
namespace cosmo = visivo::cosmology;

namespace {
constexpr auto kMomentPowerTool = "Angular power spectrum";
constexpr auto kMomentSkeletonTool = "Skeleton / voids";

/// What the pool thread hands back: the moment's dataset (fresh or reused) and
/// the analysis run on it.
template <typename Result>
struct MomentRun
{
    QString error;
    bool cancelled{ false };
    QString datasetId;
    QString path;
    Result result;
};
}   // namespace

void vtkWindowCube::setupMomentMapTextureTools()
{
    m_actMomentPower = ui->menuTools->addAction(u"Angular Power Spectrum of Moment Map"_s);
    m_actMomentPower->setCheckable(true);
    m_actMomentPower->setToolTip(
            u"Power spectrum P(ℓ) of the moment map on screen, with its slope. The "
            u"map is written to the Workspace Exports at full resolution and analysed "
            u"there. Settings in Inspector ▸ Parameters."_s);
    m_actMomentSkeleton = ui->menuTools->addAction(u"Skeleton / Voids of Moment Map"_s);
    m_actMomentSkeleton->setCheckable(true);
    m_actMomentSkeleton->setToolTip(
            u"Trace the filaments (skeleton of the bright regions) or voids (skeleton "
            u"of the faint ones) of the moment map on screen; the mask opens as a new "
            u"map. Settings in Inspector ▸ Parameters."_s);

    m_momentPowerSettings = new AngularPowerSettings(this);
    m_momentPowerSettings->hide();
    m_momentSkeletonSettings = new SkeletonSettings(this);
    m_momentSkeletonSettings->hide();
    // Here the mask is a map of its own, in a pane, not an overlay on the
    // moment: there is nothing for a Clear button to take off the view.
    m_momentSkeletonSettings->setClearVisible(false);

    const auto toggle = [this](QAction *action, const char *name, QWidget *settings) {
        QObject::connect(action, &QAction::toggled, this, [this, name, settings](bool on) {
            if (on) {
                this->refreshMomentMapToolSources();
                this->claimToolSettings(QString::fromLatin1(name), settings);
            } else {
                this->releaseToolSettings(QString::fromLatin1(name));
                this->refreshToolSettingsPanel(/*reveal=*/false);
            }
        });
    };
    toggle(m_actMomentPower, kMomentPowerTool, m_momentPowerSettings);
    toggle(m_actMomentSkeleton, kMomentSkeletonTool, m_momentSkeletonSettings);
    QObject::connect(m_momentPowerSettings, &AngularPowerSettings::runRequested, this,
                     &vtkWindowCube::runMomentAngularPower);
    QObject::connect(m_momentSkeletonSettings, &SkeletonSettings::runRequested, this,
                     &vtkWindowCube::runMomentSkeleton);
}

QString vtkWindowCube::momentProductForMapTools() const
{
    // The same choice "Export Moment Map as FITS" makes: a VISIBLE moment pane
    // first, else the moment computed last.
    for (int i = 0; i < 4; ++i) {
        const PaneView &pv = m_slots[i].view;
        if (pv.kind == ViewId::Product && pv.productId.startsWith(u"moment"_s)
            && m_slots[i].frame && m_slots[i].frame->isVisible()
            && m_products2d.contains(pv.productId))
            return pv.productId;
    }
    if (m_products2d.contains(m_lastComputedMomentProductId))
        return m_lastComputedMomentProductId;
    return {};
}

QString vtkWindowCube::momentMapToolSourceText() const
{
    const QString id = momentProductForMapTools();
    if (id.isEmpty())
        return u"No moment map yet: compute one (Tools ▸ Maps ▸ Compute Moment)."_s;
    const Product2D &p = m_products2d[id];
    // Channels as the channel bar numbers them: 1-based.
    return u"%1, channels %2–%3%4, written out at full resolution."_s
            .arg(p.label)
            .arg(p.channelStart + 1)
            .arg(p.channelEnd + 1)
            .arg(p.maskEnabled ? u", masked"_s : QString());
}

void vtkWindowCube::refreshMomentMapToolSources()
{
    const QString text = momentMapToolSourceText();
    const bool have = !momentProductForMapTools().isEmpty();
    const QString reason = u"Compute a moment map first."_s;
    if (m_momentPowerSettings) {
        m_momentPowerSettings->setSourceText(text);
        m_momentPowerSettings->setAvailable(have, reason);
    }
    if (m_momentSkeletonSettings) {
        m_momentSkeletonSettings->setSourceText(text);
        m_momentSkeletonSettings->setAvailable(have, reason);
    }
}

template <typename Result>
void vtkWindowCube::withMomentDataset(
        const QString &title, const QString &momentProductId,
        std::function<Result(BackendClient &, const QString &)> analysis,
        std::function<void(const Result &, const MomentDataset &, const QString &)> done)
{
    const auto pit = m_products2d.constFind(momentProductId);
    if (pit == m_products2d.cend())
        return;
    const int generation = m_momentProductGeneration.value(momentProductId, 0);

    MomentDataset dataset;
    dataset.generation = generation;
    dataset.source.datasetId = this->remoteDatasetId;
    dataset.source.order = pit->order;
    dataset.source.channelStart = pit->channelStart;
    dataset.source.channelEnd = pit->channelEnd;
    dataset.source.maskEnabled = pit->maskEnabled;
    dataset.source.thresholdValue = pit->thresholdValue;
    dataset.source.thresholdAuto = pit->thresholdAuto;
    // The export's name needs the cube's revision, read on the backend: it is
    // set by the worker (below), before anything is written or reused.
    const QString cubePath = this->filepath;
    const QString exportStem =
            QFileInfo(visivo::datasetExportStem(this->filepath)).completeBaseName();
    // Reuse the dataset written for these very pixels: same product, same
    // generation. A recompute of the moment bumps the generation.
    const auto cached = m_momentDatasets.constFind(momentProductId);
    const bool reuse = cached != m_momentDatasets.cend() && cached->generation == generation
            && !cached->datasetId.isEmpty();
    if (reuse)
        dataset = cached.value();

    m_momentMapToolBusy = true;
    if (m_momentPowerSettings)
        m_momentPowerSettings->setBusy(true);
    if (m_momentSkeletonSettings)
        m_momentSkeletonSettings->setBusy(true);

    auto control = std::make_shared<visivo::BackendTaskControl>();
    QPointer<QProgressDialog> sheet = visivo::showTaskProgressSheet(
            this, reuse ? title : u"%1 (writing the moment map first)"_s.arg(title), control);
    const QMetaObject::Connection abortOnClose =
            QObject::connect(this, &QObject::destroyed, [control]() { control->requestAbort(); });

    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sessionId = this->remoteSessionId;
    using Run = MomentRun<Result>;
    auto *watcher = new QFutureWatcher<Run>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<Run>::finished, this,
                     [self, watcher, sheet, abortOnClose, dataset, momentProductId,
                      done, control]() mutable {
                         const Run run = watcher->result();
                         watcher->deleteLater();
                         QObject::disconnect(abortOnClose);
                         // Read BEFORE closing the sheet: QProgressDialog::close()
                         // emits canceled(), which sets the flag for a run that
                         // finished normally. Cancel can still land after the
                         // worker's last check and before this queued handler.
                         const bool cancelled = run.cancelled || control->aborted();
                         visivo::closeToolProgressSheet(sheet);
                         if (!self)
                             return;
                         self->m_momentMapToolBusy = false;
                         if (self->m_momentPowerSettings)
                             self->m_momentPowerSettings->setBusy(false);
                         if (self->m_momentSkeletonSettings)
                             self->m_momentSkeletonSettings->setBusy(false);
                         if (cancelled) {
                             self->statusBar()->showMessage(u"Cancelled."_s, 5000);
                             return;
                         }
                         if (!run.datasetId.isEmpty()) {
                             dataset.datasetId = run.datasetId;
                             dataset.path = run.path;
                             // The recipe names the file Provenance shows.
                             if (!run.path.isEmpty())
                                 dataset.source.outputBasename = QFileInfo(run.path).fileName();
                             self->m_momentDatasets.insert(momentProductId, dataset);
                         }
                         done(run.result, dataset, run.error);
                     });
    const cosmo::MomentSource src = dataset.source;
    const QString knownDataset = reuse ? dataset.datasetId : QString();
    const QString knownPath = reuse ? dataset.path : QString();
    watcher->setFuture(QtConcurrent::run([url, token, sessionId, src = src, knownDataset,
                                          knownPath, analysis, control, cubePath,
                                          exportStem]() mutable {
        Run run;
        BackendClient client(url, token);
        client.setSessionId(sessionId);
        run.datasetId = knownDataset;
        run.path = knownPath;
        if (run.datasetId.isEmpty()) {
            // The cube's revision as the backend sees it (size and mtime from
            // its directory listing). Empty when the cube is not a plain file
            // there (a VBT field reference, a listing refused): then nothing
            // is reused, since an export could not be told from a stale one.
            QString fingerprint;
            const QFileInfo cube(cubePath);
            const BackendListFilesResult listing = client.listFiles(cube.path());
            for (const BackendFileEntry &e : listing.entries)
                if (e.path == cubePath || e.name == cube.fileName()) {
                    fingerprint = u"%1|%2"_s.arg(e.size).arg(e.modifiedTime);
                    break;
                }
            src.outputBasename = cosmo::momentExportBasename(cubePath, exportStem, src, fingerprint);
            // Written before, by this window or an earlier session: open it.
            // The backend publishes a moment export by renaming the complete
            // file onto its reserved name, so a zero-size entry is one still
            // being written (or never finished), never a moment to read. An
            // export that does not open is left alone: it may be another
            // viewer's, and this run simply writes its own under a new name.
            if (!fingerprint.isEmpty()) {
                const BackendWorkspaceExportsList exports = client.listExports();
                for (const BackendWorkspaceExportEntry &e : exports.entries) {
                    if (e.filename != src.outputBasename || e.size <= 0)
                        continue;
                    const BackendOpenDatasetResult opened = client.openDataset(e.absolutePath);
                    if (opened.valid) {
                        run.datasetId = opened.datasetId;
                        run.path = e.absolutePath;
                    }
                    break;
                }
            }
        }
        if (run.datasetId.isEmpty()) {
            const BackendSaveMomentFitsResult written = client.exportMomentFitsFollowingTask(
                    src.datasetId, src.order, src.channelStart, src.channelEnd, src.maskEnabled,
                    src.thresholdValue, src.outputBasename, src.thresholdAuto, control.get());
            if (written.cancelled) {
                run.cancelled = true;
                return run;
            }
            if (!written.valid) {
                run.error = u"Writing the moment map failed: %1"_s.arg(written.error);
                return run;
            }
            // Opened in THIS session (the client carries its id), so the new
            // dataset sits beside the cube rather than in a session of its own.
            const BackendOpenDatasetResult opened = client.openDataset(written.outputPath);
            if (!opened.valid) {
                run.error = u"Opening the moment map failed: %1"_s.arg(opened.error);
                return run;
            }
            run.datasetId = opened.datasetId;
            run.path = written.outputPath;
        }
        if (control->aborted()) {
            run.cancelled = true;
            return run;
        }
        run.result = analysis(client, run.datasetId);
        // Cancel pressed while the analysis ran: the request cannot be taken
        // back, but its answer is not wanted.
        if (control->aborted())
            run.cancelled = true;
        return run;
    }));
}

// ── Angular power spectrum ───────────────────────────────────────────────────

void vtkWindowCube::runMomentAngularPower()
{
    if (m_momentMapToolBusy)
        return;
    const QString momentId = momentProductForMapTools();
    if (momentId.isEmpty())
        return;
    const int nBins = m_momentPowerSettings ? m_momentPowerSettings->nBins() : 64;
    const QString momentLabel = m_products2d.value(momentId).label;
    QPointer<vtkWindowCube> self(this);
    withMomentDataset<BackendAngularPowerResult>(
            u"Angular power spectrum of %1"_s.arg(momentLabel), momentId,
            [nBins](BackendClient &client, const QString &datasetId) {
                return client.requestAngularPower(datasetId, 0, nBins);
            },
            [self, nBins, momentLabel](const BackendAngularPowerResult &r,
                                       const MomentDataset &dataset, const QString &error) {
                if (!self)
                    return;
                if (!error.isEmpty() || !r.valid) {
                    QMessageBox::warning(self, u"Angular Power Spectrum"_s,
                                         error.isEmpty() ? r.error : error);
                    return;
                }
                const visivo::PowerSlopeOptions options;
                const visivo::PowerSlopeFit fit =
                        visivo::fitPowerSlope(r.ell, r.power, r.nModes, options, r.clErr);
                const QString productId = self->scopedProductId(
                        u"powerspectrum-%1"_s.arg(++self->m_momentPowerRuns));
                QVariantMap params;
                params.insert(u"Map"_s, u"%1, channels %2–%3"_s.arg(momentLabel)
                                                .arg(dataset.source.channelStart + 1)
                                                .arg(dataset.source.channelEnd + 1));
                params.insert(u"Moment FITS"_s, dataset.path);
                params.insert(u"Bins requested"_s, nBins);
                params.insert(u"Bins returned"_s, r.nBins);
                params.insert(u"Axis"_s, r.ellIsAngular
                                                 ? u"ℓ = 2π k / θ_pix"_s
                                                 : u"k (pixel⁻¹): no celestial scale"_s);
                if (std::isfinite(r.pixelScaleArcsec))
                    params.insert(u"Pixel scale (arcsec)"_s, r.pixelScaleArcsec);
                params.insert(u"Slope"_s, PowerSpectrumWindow::slopeSummary(fit, r.ellIsAngular));
                params.insert(u"Fit: minimum modes per bin"_s, options.minModes);
                params.insert(u"Calibrated"_s,
                              r.calibrated ? u"yes"_s : u"no (relative spectrum)"_s);
                const QString finishedAt = cosmo::runFinishedAt();
                params.insert(u"Finished"_s, finishedAt);
                QList<ReplayStep> replay = cosmo::momentDatasetSteps(dataset.source);
                replay.append(cosmo::angularPowerStep(
                        0, nBins, u"dataset_id: the one the open step returned."_s));

                self->m_momentPowerSpectra.insert(productId,
                                                  std::make_shared<BackendAngularPowerResult>(r));
                self->m_momentPowerFits.insert(productId, options);
                self->m_momentPowerLabels.insert(productId, momentLabel);
                self->registerAnalysisProduct(
                        u"powerspectrum"_s,
                        cosmo::powerSpectrumRunLabel(fit.valid, fit.slope, finishedAt,
                                                     momentLabel),
                        params, nullptr, productId, QString(), true, {}, true, replay);
                self->showMomentPowerSpectrum(productId);
            });
}

void vtkWindowCube::showMomentPowerSpectrum(const QString &productId)
{
    const auto spectrum = m_momentPowerSpectra.value(productId);
    if (!spectrum)
        return;
    auto *window = new PowerSpectrumWindow(*spectrum,
                                           m_momentPowerLabels.value(productId, u"moment map"_s),
                                           m_momentPowerFits.value(productId), this);
    QPointer<vtkWindowCube> self(this);
    window->setFitCallback([self, productId](const visivo::PowerSlopeFit &fit,
                                             const visivo::PowerSlopeOptions &o) {
        if (!self)
            return;
        self->m_momentPowerFits.insert(productId, o);
        const auto s = self->m_momentPowerSpectra.value(productId);
        QVariantMap params = ProductRegistry::instance().product(productId).params;
        params.insert(u"Slope"_s, PowerSpectrumWindow::slopeSummary(fit, s && s->ellIsAngular));
        params.insert(u"Fit range"_s, u"%1 – %2"_s
                                              .arg(o.ellMin > 0.0 ? QString::number(o.ellMin)
                                                                  : u"first bin"_s)
                                              .arg(o.ellMax > 0.0 ? QString::number(o.ellMax)
                                                                  : u"last bin"_s));
        params.insert(u"Fit: minimum modes per bin"_s, o.minModes);
        ProductRegistry::instance().setParams(productId, params);
    });
    ProductRegistry::instance().attachWindow(productId, window);
    window->show();
    window->raise();
}

// ── Skeleton / voids ─────────────────────────────────────────────────────────

void vtkWindowCube::runMomentSkeleton()
{
    if (m_momentMapToolBusy)
        return;
    const QString momentId = momentProductForMapTools();
    if (momentId.isEmpty())
        return;
    const BackendSkeletonRequest request =
            m_momentSkeletonSettings ? m_momentSkeletonSettings->request() : BackendSkeletonRequest{};
    const QString momentLabel = m_products2d.value(momentId).label;
    QPointer<vtkWindowCube> self(this);
    withMomentDataset<BackendSkeletonResult>(
            u"Skeleton of %1"_s.arg(momentLabel), momentId,
            [request](BackendClient &client, const QString &datasetId) {
                return client.requestSkeleton(datasetId, request);
            },
            [self, request, momentLabel](const BackendSkeletonResult &r,
                                         const MomentDataset &dataset, const QString &error) {
                if (!self)
                    return;
                if (!error.isEmpty() || !r.valid) {
                    QMessageBox::warning(self, u"Skeleton / Voids"_s,
                                         error.isEmpty() ? r.error : error);
                    return;
                }
                // The mask as a map of its own: 1 on the skeleton, 0 elsewhere,
                // on the moment's full-resolution pixel grid.
                auto img = vtkSmartPointer<vtkImageData>::New();
                img->SetExtent(0, r.width - 1, 0, r.height - 1, 0, 0);
                img->SetOrigin(0.0, 0.0, 0.0);
                img->SetSpacing(1.0, 1.0, 1.0);
                img->AllocateScalars(VTK_FLOAT, 1);
                auto *out = static_cast<float *>(img->GetScalarPointer());
                const auto *in = reinterpret_cast<const float *>(r.data.constData());
                const qsizetype n = static_cast<qsizetype>(r.width) * r.height;
                for (qsizetype i = 0; i < n; ++i)
                    out[i] = in[i] > 0.f ? 1.f : 0.f;

                const bool voids = r.mode == u"void"_s;
                QVariantMap params = cosmo::skeletonDisplayParams(request, r.threshold);
                params.insert(u"Map"_s, u"%1, channels %2–%3"_s.arg(momentLabel)
                                                .arg(dataset.source.channelStart + 1)
                                                .arg(dataset.source.channelEnd + 1));
                params.insert(u"Moment FITS"_s, dataset.path);
                params.insert(u"Skeleton pixels"_s, r.nSkeletonPixels);
                params.insert(u"End points"_s, r.nEndpoints);
                QList<ReplayStep> replay = cosmo::momentDatasetSteps(dataset.source);
                replay.append(cosmo::skeletonStep(
                        request, u"dataset_id: the one the open step returned."_s));
                const QString finishedAt = cosmo::runFinishedAt();
                params.insert(u"Finished"_s, finishedAt);
                const int run = ++self->m_momentSkeletonRuns;
                const QString productId = self->addImageProduct(
                        u"skeleton"_s, u"skeleton-%1"_s.arg(run),
                        cosmo::skeletonRunLabel(voids, r.nSkeletonPixels, finishedAt, momentLabel),
                        img, QString(), params, u"Gray"_s, /*symmetricRange=*/false, replay);
                // addImageProduct only fills a FREE pane (it never steals one), so
                // with every pane busy the mask waited unseen in Session Data. A
                // run the user just asked for is shown, as the power spectrum
                // shows its window: the same path as opening the row.
                bool shown = false;
                for (int i = 0; i < 4 && !productId.isEmpty(); ++i)
                    if (self->m_slots[i].frame && self->m_slots[i].frame->isVisible()
                        && self->m_slots[i].view == PaneView{ ViewId::Product, productId })
                        shown = true;
                if (!shown && !productId.isEmpty())
                    self->activateSessionView(QString(), productId);
                const QString summary = u"%1 skeleton pixels, %2 end points, threshold %3"_s
                                                .arg(r.nSkeletonPixels)
                                                .arg(r.nEndpoints)
                                                .arg(r.threshold, 0, 'g', 6);
                if (self->m_momentSkeletonSettings)
                    self->m_momentSkeletonSettings->setResultText(summary);
                self->statusBar()->showMessage(summary, 8000);
            });
}
