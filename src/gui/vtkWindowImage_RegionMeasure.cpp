#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

#include "app/BackendClient.h"
#include "ProductOrigin.h"
#include "ProfileWidget.h"
#include "RegionMeasure.h"
#include "RegionModel.h"
#include "ToolResultDialog.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QtConcurrentRun>

#include <cmath>

using namespace Qt::StringLiterals;

// ── Line profile, image noise and sky positions (issue #503) ─────────────────
// What a line region and the noise of a continuum image need beyond the area
// statistics of vtkWindowImage_Region.cpp. The numbers come from the backend,
// read from the file at full resolution (POST /v1/regions/profile and
// /v1/regions/noise); the rows and the CSV are plain functions in
// RegionMeasure.h.

QString vtkWindowImage::worldLabelAtPixel(double x, double y) const
{
    if (!this->remoteHasCelestialAxes())
        return {};
    bool ok0 = false, ok1 = false;
    const double nativeX = this->remoteVoxelToWcs(0, x, &ok0);
    const double nativeY = this->remoteVoxelToWcs(1, y, &ok1);
    double frameX = nativeX, frameY = nativeY;
    if (!ok0 || !ok1 || !this->convertRemoteCelestialCoordinates(nativeX, nativeY, frameX, frameY))
        return {};
    // The same text as the probe readout, in the frame and format chosen.
    return u"%1=%2  %3=%4"_s.arg(this->selectedFrameAxisTitle(0),
                                 this->formatRemoteOverlayCoordinate(0, frameX),
                                 this->selectedFrameAxisTitle(1),
                                 this->formatRemoteOverlayCoordinate(1, frameY));
}

void vtkWindowImage::analyzeLineRegion(const visivo::regions::Region &line)
{
    if (this->remoteDatasetId.isEmpty()) {
        this->statusBar()->showMessage(
                u"The profile along a line is read from the file by the backend: this image "
                u"is not open on one."_s, 6000);
        return;
    }
    QJsonObject canonical = visivo::regions::toCanonical(line, 0.0);
    canonical.remove(u"props"_s);
    const int generation = ++this->m_regionDialogGeneration;
    this->statusBar()->showMessage(u"%1: reading the profile along the line…"_s.arg(line.name), 4000);

    using Result = BackendRegionProfileResult;
    auto *watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this,
                     [this, watcher, line, generation]() {
        watcher->deleteLater();
        const Result p = watcher->result();
        if (generation != this->m_regionDialogGeneration)
            return;   // another region measured meanwhile
        const visivo::regions::Region *now = this->m_regions.find(line.id);
        if (!now || !visivo::regions::sameGeometry(*now, line))
            return;   // deleted or moved meanwhile: these numbers are not its
        if (!p.valid) {
            QMessageBox::warning(this, u"Line profile"_s,
                                 p.error.isEmpty() ? u"The backend could not read the profile."_s
                                                   : p.error);
            return;
        }
        const bool arcsec = p.offsetUnit == QLatin1String("arcsec");
        const QString title = u"%1  ·  %2 px wide"_s.arg(line.name).arg(p.widthPix);
        auto *plot = new ProfileWidget(this);
        plot->setAttribute(Qt::WA_DeleteOnClose);
        plot->setWindowTitle(u"Line Profile"_s);
        plot->setUsageMode(ProfileWidget::UsageMode::RegionStatic, title);
        const QString unit = p.bunit.trimmed();
        plot->setupSpectrumPlot(arcsec ? u"Offset (arcsec)"_s : u"Offset (pixels)"_s,
                                unit.isEmpty() ? u"Value"_s : u"Value (%1)"_s.arg(unit));
        plot->updateSpectrumPlot(p.offsets, p.values, title, /*live=*/false);
        plot->setSpectrumMeta(
                u"Line profile  ·  %1 valid samples of %2"_s.arg(p.validSamples).arg(p.values.size()),
                u"from %1 to %2"_s.arg(this->worldLabelAtPixel(p.x.first(), p.y.first()),
                                       this->worldLabelAtPixel(p.x.last(), p.y.last())));
        plot->show();

        // Kept in Session Data with the line it was measured on.
        const QVariantMap record = ToolResultDialog::makeRecord(
                u"Line Profile"_s, title,
                { { u"Profile"_s, visivo::measure::profileRows(p) } });
        QVariantMap params;
        params.insert(u"Region"_s, line.name);
        params.insert(u"Samples"_s, QString::number(p.values.size()));
        params.insert(u"Width"_s, u"%1 px"_s.arg(p.widthPix));
        const QVariantMap origin = visivo::encodeListRegionOrigin(
                visivo::regions::toCanonical(line, 0.0).toVariantMap());
        params.insert(u"Measured on"_s, visivo::describeOrigin(origin));
        this->registerAnalysisProduct(u"region"_s, u"%1 profile"_s.arg(line.name), params,
                                      this->scopedProductId(u"region-%1"_s.arg(line.id)), record,
                                      /*layerIndex=*/-1, origin);
    });
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
    watcher->setFuture(QtConcurrent::run([url, token, session, dataset, canonical]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        return client.regionProfile(dataset, canonical);
    }));
}

void vtkWindowImage::estimateImageNoise()
{
    if (this->remoteDatasetId.isEmpty() || this->isBusy()) {
        this->statusBar()->showMessage(
                u"The noise is read from the file by the backend: this image is not open on one."_s,
                6000);
        return;
    }
    // The regions marked as background in their properties are the source-free
    // pixels the user chose; with none, the whole image is clipped.
    QJsonArray background;
    for (const visivo::regions::Region &r : this->m_regions.regions()) {
        if (!visivo::regions::isBackground(r) || !visivo::regions::hasArea(r.shape))
            continue;
        QJsonObject c = visivo::regions::toCanonical(r, 0.0);
        c.remove(u"props"_s);
        c[u"name"_s] = r.name;
        background.append(c);
    }
    bool useRegions = !background.isEmpty();
    if (useRegions) {
        QMessageBox ask(QMessageBox::Question, u"Estimate Noise"_s,
                        u"%1 region%2 marked as background. Measure the noise on %3, or "
                        u"sigma-clip the whole image?"_s
                                .arg(background.size()).arg(background.size() == 1 ? u" is"_s : u"s are"_s)
                                .arg(background.size() == 1 ? u"it"_s : u"them"_s),
                        QMessageBox::NoButton, this);
        auto *regionsButton = ask.addButton(u"Background regions"_s, QMessageBox::AcceptRole);
        auto *autoButton = ask.addButton(u"Whole image (automatic)"_s, QMessageBox::ActionRole);
        ask.addButton(QMessageBox::Cancel);
        ask.exec();
        if (ask.clickedButton() == autoButton)
            useRegions = false;
        else if (ask.clickedButton() != regionsButton)
            return;
    }
    this->statusBar()->showMessage(u"Estimating the noise of the image…"_s, 0);
    const int generation = ++this->m_noiseGeneration;

    using Result = BackendImageNoiseResult;
    auto *watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this,
                     [this, watcher, generation, useRegions]() {
        watcher->deleteLater();
        const Result r = watcher->result();
        if (generation != this->m_noiseGeneration)
            return;
        this->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(this, u"Estimate Noise"_s,
                                 r.error.isEmpty() ? u"The backend could not estimate the noise."_s
                                                   : r.error);
            return;
        }
        const QString subtitle = useRegions ? u"background regions"_s
                                            : u"whole image, sigma-clipped"_s;
        const QVariantMap record = ToolResultDialog::makeRecord(
                u"Image Noise"_s, subtitle, { { u"Noise"_s, visivo::measure::noiseRows(r) } });
        auto *dlg = new ToolResultDialog(this);
        dlg->applyRecord(record);
        dlg->show();

        QVariantMap params;
        params.insert(u"From"_s, subtitle);
        params.insert(visivo::measure::rmsLabel(), visivo::measure::withUnit(r.rms, r.bunit));
        params.insert(visivo::measure::madLabel(), visivo::measure::withUnit(r.madSigma, r.bunit));
        if (r.beamAreaPix > 0.0)
            params.insert(u"RMS per pixel"_s,
                          u"%1 Jy/pixel"_s.arg(visivo::measure::num(r.rmsPerPixel, 6)));
        this->registerAnalysisProduct(u"noise"_s, u"Image noise"_s, params,
                                      this->scopedProductId(u"noise"_s), record);
    });
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
    watcher->setFuture(QtConcurrent::run([url, token, session, dataset, background]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        return client.imageNoise(dataset, background);
    }));
}
