// vtkWindowImage_Cosmology.cpp
// The image viewer's two map-texture tools (issue #29), which until now the
// Copilot alone could reach:
//
//   Tools ▸ Structure ▸ Angular Power Spectrum — P(ℓ) of this map on the
//       backend (POST /v1/cosmology/angular_power), plotted on log axes with
//       its fitted slope;
//   Tools ▸ Structure ▸ Skeleton / Voids — the medial axis of the regions above
//       (filaments) or below (voids) a threshold (POST /v1/analysis/skeleton),
//       drawn over the map.
//
// Each is a checkable tool: on, its settings take Inspector ▸ Parameters
// (claimToolSettings); a run registers a product with its replay record, and
// opening that product's row puts the plot or the overlay back.

#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

#include "app/BackendClient.h"
#include "CosmologyReplay.h"
#include "CosmologyToolWidgets.h"
#include "ToolResultDialog.h"
#include "WorkspaceChrome.h"

#include <QAction>
#include <QDockWidget>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrent>

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

#include <memory>

using namespace Qt::StringLiterals;
namespace cosmo = visivo::cosmology;

namespace {
constexpr auto kPowerToolName = "Angular power spectrum";
constexpr auto kSkeletonToolName = "Skeleton / voids";
}   // namespace

void vtkWindowImage::setupMapTextureTools()
{
    const bool haveDataset = !this->remoteDatasetId.isEmpty();
    const QString noDataset =
            u"Needs the image open on a backend: the tool runs there, on the full map."_s;

    m_actAngularPower = ui->menuTools->addAction(u"Angular Power Spectrum"_s);
    m_actAngularPower->setCheckable(true);
    m_actAngularPower->setToolTip(
            haveDataset ? u"Azimuthally averaged power spectrum P(ℓ) of this map, with "
                          u"its slope. Settings in Inspector ▸ Parameters."_s
                        : noDataset);
    m_actAngularPower->setEnabled(haveDataset);

    m_actSkeleton = ui->menuTools->addAction(u"Skeleton / Voids"_s);
    m_actSkeleton->setCheckable(true);
    m_actSkeleton->setToolTip(
            haveDataset ? u"Trace filaments (the skeleton of the bright regions) or voids "
                          u"(the skeleton of the faint ones) and draw them over the map. "
                          u"Settings in Inspector ▸ Parameters."_s
                        : noDataset);
    m_actSkeleton->setEnabled(haveDataset);

    // The settings are built once and kept: the Inspector re-homes them, it
    // never deletes them (InspectorPanel::setToolSettings).
    m_angularPowerSettings = new AngularPowerSettings(this);
    m_angularPowerSettings->hide();
    m_angularPowerSettings->setSourceText(
            u"This image: %1"_s.arg(QFileInfo(this->filepath).fileName()));
    m_skeletonSettings = new SkeletonSettings(this);
    m_skeletonSettings->hide();
    m_skeletonSettings->setSourceText(
            u"This image: %1"_s.arg(QFileInfo(this->filepath).fileName()));

    QObject::connect(m_actAngularPower, &QAction::toggled, this, [this](bool on) {
        this->setMapToolOn(m_actAngularPower, QString::fromLatin1(kPowerToolName),
                           m_angularPowerSettings, on);
    });
    QObject::connect(m_actSkeleton, &QAction::toggled, this, [this](bool on) {
        this->setMapToolOn(m_actSkeleton, QString::fromLatin1(kSkeletonToolName),
                           m_skeletonSettings, on);
    });
    QObject::connect(m_angularPowerSettings, &AngularPowerSettings::runRequested, this,
                     &vtkWindowImage::runAngularPower);
    QObject::connect(m_skeletonSettings, &SkeletonSettings::runRequested, this,
                     &vtkWindowImage::runSkeleton);
    QObject::connect(m_skeletonSettings, &SkeletonSettings::clearRequested, this,
                     &vtkWindowImage::clearSkeletonOverlay);
}

void vtkWindowImage::setMapToolOn(QAction *action, const QString &toolName, QWidget *settings,
                                  bool on)
{
    Q_UNUSED(action);
    if (!on) {
        releaseToolSettings(toolName);
        refreshToolSettingsPanel(/*reveal=*/false);
        return;
    }
    if (this->m_workspaceChrome) {
        claimToolSettings(toolName, settings);
        return;
    }
    // No Inspector to show the settings in (a window built without the
    // workspace chrome): run once with the defaults rather than do nothing.
    if (settings == m_angularPowerSettings)
        runAngularPower();
    else
        runSkeleton();
}

// ── Angular power spectrum ───────────────────────────────────────────────────

void vtkWindowImage::runAngularPower()
{
    if (m_angularPowerBusy || this->remoteDatasetId.isEmpty())
        return;
    const int nBins = m_angularPowerSettings ? m_angularPowerSettings->nBins() : 64;
    m_angularPowerBusy = true;
    if (m_angularPowerSettings)
        m_angularPowerSettings->setBusy(true);
    statusBar()->showMessage(u"Computing the angular power spectrum on the backend…"_s);

    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString session = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;
    auto *watcher = new QFutureWatcher<BackendAngularPowerResult>(this);
    QPointer<vtkWindowImage> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendAngularPowerResult>::finished, this,
                     [self, watcher, nBins]() {
        const BackendAngularPowerResult r = watcher->result();
        watcher->deleteLater();
        if (!self)
            return;
        self->m_angularPowerBusy = false;
        if (self->m_angularPowerSettings)
            self->m_angularPowerSettings->setBusy(false);
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Angular Power Spectrum"_s,
                                 u"The backend could not compute it:\n\n%1"_s.arg(r.error));
            return;
        }
        const visivo::PowerSlopeOptions options;
        const visivo::PowerSlopeFit fit =
                visivo::fitPowerSlope(r.ell, r.power, r.nModes, options, r.clErr);

        const QString productId =
                self->scopedProductId(u"powerspectrum-%1"_s.arg(++self->m_powerSpectrumRuns));
        QVariantMap params;
        params.insert(u"Bins requested"_s, nBins);
        params.insert(u"Bins returned"_s, r.nBins);
        params.insert(u"Axis"_s, r.ellIsAngular ? u"ℓ = 2π k / θ_pix"_s
                                                : u"k (pixel⁻¹): no celestial scale"_s);
        if (std::isfinite(r.pixelScaleArcsec))
            params.insert(u"Pixel scale (arcsec)"_s, r.pixelScaleArcsec);
        params.insert(u"Slope"_s, PowerSpectrumWindow::slopeSummary(fit, r.ellIsAngular));
        params.insert(u"Fit: minimum modes per bin"_s, options.minModes);
        params.insert(u"Calibrated"_s, r.calibrated ? u"yes"_s : u"no (relative spectrum)"_s);
        const QString finishedAt = cosmo::runFinishedAt();
        params.insert(u"Finished"_s, finishedAt);

        const QVariantMap record = ToolResultDialog::makeRecord(
                u"Angular Power Spectrum"_s, QFileInfo(self->filepath).fileName(),
                { { u"Result"_s,
                    { { u"Slope"_s, PowerSpectrumWindow::slopeSummary(fit, r.ellIsAngular) },
                      { u"Bins"_s, QString::number(r.nBins) },
                      { u"Map"_s, u"%1 × %2 px"_s.arg(r.width).arg(r.height) } } } });
        self->m_powerSpectra.insert(productId,
                                    std::make_shared<BackendAngularPowerResult>(r));
        self->m_powerSpectrumFits.insert(productId, options);
        self->registerAnalysisProduct(
                u"powerspectrum"_s,
                cosmo::powerSpectrumRunLabel(fit.valid, fit.slope, finishedAt),
                params, productId, record, /*layerIndex=*/-1, /*origin=*/{},
                { cosmo::angularPowerStep(0, nBins) });
        self->showPowerSpectrum(productId);
    });
    watcher->setFuture(QtConcurrent::run([url, token, session, datasetId, nBins]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        return client.requestAngularPower(datasetId, 0, nBins);
    }));
}

void vtkWindowImage::showPowerSpectrum(const QString &productId)
{
    const auto it = m_powerSpectra.constFind(productId);
    if (it == m_powerSpectra.cend() || !it.value())
        return;
    auto *window = new PowerSpectrumWindow(*it.value(), QFileInfo(this->filepath).fileName(),
                                           m_powerSpectrumFits.value(productId), this);
    QPointer<vtkWindowImage> self(this);
    // A fit range the user settles on becomes part of the product's record:
    // the slope in Provenance is the one on screen, over the range on screen.
    window->setFitCallback([self, productId](const visivo::PowerSlopeFit &fit,
                                             const visivo::PowerSlopeOptions &o) {
        if (!self)
            return;
        self->m_powerSpectrumFits.insert(productId, o);
        const auto spectrum = self->m_powerSpectra.value(productId);
        AnalysisProduct p = ProductRegistry::instance().product(productId);
        QVariantMap params = p.params;
        params.insert(u"Slope"_s,
                      PowerSpectrumWindow::slopeSummary(fit, spectrum && spectrum->ellIsAngular));
        params.insert(u"Fit range"_s, u"%1 – %2"_s
                                              .arg(o.ellMin > 0.0 ? QString::number(o.ellMin)
                                                                  : u"first bin"_s)
                                              .arg(o.ellMax > 0.0 ? QString::number(o.ellMax)
                                                                  : u"last bin"_s));
        params.insert(u"Fit: minimum modes per bin"_s, o.minModes);
        ProductRegistry::instance().setParams(productId, params);
    });
    window->show();
    window->raise();
}

// ── Skeleton / voids ─────────────────────────────────────────────────────────

void vtkWindowImage::runSkeleton()
{
    if (m_skeletonBusy || this->remoteDatasetId.isEmpty())
        return;
    const BackendSkeletonRequest request =
            m_skeletonSettings ? m_skeletonSettings->request() : BackendSkeletonRequest{};
    m_skeletonBusy = true;
    if (m_skeletonSettings)
        m_skeletonSettings->setBusy(true);
    statusBar()->showMessage(u"Tracing the skeleton on the backend…"_s);

    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString session = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;
    auto *watcher = new QFutureWatcher<BackendSkeletonResult>(this);
    QPointer<vtkWindowImage> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSkeletonResult>::finished, this,
                     [self, watcher, request]() {
        const BackendSkeletonResult r = watcher->result();
        watcher->deleteLater();
        if (!self)
            return;
        self->m_skeletonBusy = false;
        if (self->m_skeletonSettings)
            self->m_skeletonSettings->setBusy(false);
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Skeleton / Voids"_s,
                                 u"The backend could not trace it:\n\n%1"_s.arg(r.error));
            return;
        }
        const bool voids = r.mode == u"void"_s;
        const QString productId =
                self->scopedProductId(u"skeleton-%1"_s.arg(++self->m_skeletonRuns));
        self->m_skeletonMasks.insert(productId, { r.data, r.width, r.height, r.mode });

        QVariantMap params = cosmo::skeletonDisplayParams(request, r.threshold);
        params.insert(u"Skeleton pixels"_s, r.nSkeletonPixels);
        params.insert(u"End points"_s, r.nEndpoints);
        const QString finishedAt = cosmo::runFinishedAt();
        params.insert(u"Finished"_s, finishedAt);
        const QString summary = u"%1 skeleton pixels, %2 end points, threshold %3"_s
                                        .arg(r.nSkeletonPixels)
                                        .arg(r.nEndpoints)
                                        .arg(r.threshold, 0, 'g', 6);
        const QVariantMap record = ToolResultDialog::makeRecord(
                voids ? u"Void Skeleton"_s : u"Filament Skeleton"_s,
                QFileInfo(self->filepath).fileName(),
                { { u"Result"_s, { { u"Summary"_s, summary } } } });
        self->registerAnalysisProduct(u"skeleton"_s,
                                      cosmo::skeletonRunLabel(voids, r.nSkeletonPixels,
                                                              finishedAt),
                                      params, productId, record, /*layerIndex=*/-1,
                                      /*origin=*/{}, { cosmo::skeletonStep(request) });
        if (self->m_skeletonSettings)
            self->m_skeletonSettings->setResultText(summary);
        self->showSkeletonOverlay(productId);
    });
    watcher->setFuture(QtConcurrent::run([url, token, session, datasetId, request]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        return client.requestSkeleton(datasetId, request);
    }));
}

void vtkWindowImage::showSkeletonOverlay(const QString &productId)
{
    const auto it = m_skeletonMasks.constFind(productId);
    if (it == m_skeletonMasks.cend())
        return;
    const SkeletonMask &mask = it.value();
    vtkRenderer *renderer = ui->vtk->renderWindow()
            ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    if (!renderer)
        return;

    // One filled quad per run of skeleton pixels along a row. The world
    // coordinate of this window's image is the FULL-RESOLUTION pixel index,
    // whatever level of the pyramid is on screen (ImageLod's mapping), and the
    // mask comes back at full resolution: pixel (x, y) covers x ± ½, y ± ½.
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto quads = vtkSmartPointer<vtkCellArray>::New();
    const auto *values = reinterpret_cast<const float *>(mask.data.constData());
    for (int y = 0; y < mask.height; ++y) {
        const float *row = values + static_cast<qsizetype>(y) * mask.width;
        int x = 0;
        while (x < mask.width) {
            if (!(row[x] > 0.f)) {
                ++x;
                continue;
            }
            const int start = x;
            while (x < mask.width && row[x] > 0.f)
                ++x;
            const vtkIdType a = points->InsertNextPoint(start - 0.5, y - 0.5, 0.0);
            const vtkIdType b = points->InsertNextPoint(x - 0.5, y - 0.5, 0.0);
            const vtkIdType c = points->InsertNextPoint(x - 0.5, y + 0.5, 0.0);
            const vtkIdType d = points->InsertNextPoint(start - 0.5, y + 0.5, 0.0);
            const vtkIdType quad[4] = { a, b, c, d };
            quads->InsertNextCell(4, quad);
        }
    }
    auto poly = vtkSmartPointer<vtkPolyData>::New();
    poly->SetPoints(points);
    poly->SetPolys(quads);

    if (!m_skeletonActor) {
        m_skeletonActor = vtkSmartPointer<vtkActor>::New();
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        m_skeletonActor->SetMapper(mapper);
        m_skeletonActor->GetProperty()->SetRepresentationToSurface();
        m_skeletonActor->GetProperty()->SetLighting(false);
        renderer->AddActor(m_skeletonActor);
    }
    static_cast<vtkPolyDataMapper *>(m_skeletonActor->GetMapper())->SetInputData(poly);
    // Filaments in cyan, voids in magenta: the two can be told apart when a
    // product of each is reopened in turn.
    if (mask.mode == u"void"_s)
        m_skeletonActor->GetProperty()->SetColor(1.0, 0.3, 0.85);
    else
        m_skeletonActor->GetProperty()->SetColor(0.2, 0.95, 1.0);
    m_skeletonActor->VisibilityOn();
    m_skeletonShownProduct = productId;
    this->vtkRender();
}

void vtkWindowImage::clearSkeletonOverlay()
{
    if (m_skeletonActor)
        m_skeletonActor->VisibilityOff();
    m_skeletonShownProduct.clear();
    if (m_skeletonSettings)
        m_skeletonSettings->setResultText(QString());
    this->vtkRender();
}

bool vtkWindowImage::openMapTextureProduct(const QString &productId)
{
    if (m_powerSpectra.contains(productId)) {
        showPowerSpectrum(productId);
        return true;
    }
    if (m_skeletonMasks.contains(productId)) {
        showSkeletonOverlay(productId);
        statusBar()->showMessage(u"Skeleton overlay restored."_s, 4000);
        return true;
    }
    return false;
}
