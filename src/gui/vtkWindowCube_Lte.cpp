// vtkWindowCube_Lte.cpp
// The cube window's side of the LTE analysis of CO isotopologues (issue #416):
// what it hands the dialog, and where the results go.

#include "vtkWindowCube.h"

#include "LteCoDialog.h"
#include "MomentMapController.h"
#include "ProfileWidget.h"
#include "RegionModel.h"
#include "app/LteCoRequest.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

void vtkWindowCube::openLteCoDialog()
{
    if (!this->hasBackendVisibleDataset())
        return;

    LteCoDialog::Context ctx;
    ctx.backendUrl = this->remoteBackendUrl;
    ctx.backendToken = this->remoteBackendToken;
    ctx.sessionId = this->remoteSessionId;
    ctx.hostDatasetId = this->remoteDatasetId;
    ctx.hostRestFreqHz = this->m_restFreqHz;

    // The window prefilled is the one the user is looking at: the spectrum's
    // zoom or typed fit range first, then the channels of the moment map on
    // screen. Only a velocity axis can say it in km/s.
    const QString unit = this->spectralAxisDescriptor().unit;
    const auto setWindow = [&ctx, &unit](double a, double b, const QString &source) {
        const auto lo = visivo::lte::toKms(std::min(a, b), unit);
        const auto hi = visivo::lte::toKms(std::max(a, b), unit);
        if (!lo || !hi || !(*lo < *hi))
            return false;
        ctx.vMinKms = lo;
        ctx.vMaxKms = hi;
        ctx.windowSource = source;
        return true;
    };
    bool haveWindow = false;
    // A spectrum pane can hold a product on another axis (a stack is in
    // channels): its range is in this cube's unit only when its axis says so.
    if (this->m_specInspTarget && this->m_specInspTarget->hasSpectrum()
        && this->m_specInspTarget->spectrumXAxisLabel() == this->spectralAxisTitle()) {
        const auto w = this->m_specInspTarget->currentFitWindow();
        if (w.active)
            haveWindow = setWindow(w.xMin, w.xMax, u"the spectrum's fit range"_s);
    }
    if (!haveWindow && this->momentProvenanceState.valid && this->momentController) {
        const auto c = this->momentController->lastAcceptedConfig();
        bool ok0 = false, ok1 = false, okStep = false;
        const double v0 = this->spectralAxisValue(c.channelStart, &ok0);
        const double v1 = this->spectralAxisValue(c.channelEnd, &ok1);
        const double vNext = this->spectralAxisValue(c.channelStart + 1, &okStep);
        // Channel centres as the edges would put the end channels on the
        // boundary, where rounding can drop them: half a channel either side.
        const double half = okStep ? 0.5 * std::abs(vNext - v0) : 0.0;
        if (ok0 && ok1 && c.channelEnd > c.channelStart)
            setWindow(std::min(v0, v1) - half, std::max(v0, v1) + half,
                      u"the moment map's channels %1–%2"_s.arg(c.channelStart + 1)
                              .arg(c.channelEnd + 1));
    }

    for (const visivo::regions::Region &r : this->m_regions.regions()) {
        QJsonObject canonical = visivo::regions::toCanonical(r, 0.0);
        if (!visivo::lte::isAreaRegion(canonical))
            continue;
        canonical.remove(u"props"_s);
        ctx.regions.append({ r.name.isEmpty() ? u"Region %1"_s.arg(r.id) : r.name, canonical });
    }

    if (auto *open = this->findChild<LteCoDialog *>(u"lteCoDialog"_s)) {
        open->refreshHostContext(ctx);
        open->show();
        open->raise();
        open->activateWindow();
        return;
    }

    auto *dlg = new LteCoDialog(ctx, this);
    dlg->setObjectName(u"lteCoDialog"_s);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(dlg, &LteCoDialog::openMapRequested, this,
                     [self](const QString &path, const QString &label) {
                         if (self)
                             emit self->openBackendFitsRequested(path, label);
                     });
    // Each run is its own row with no window: the dialog shows only its latest
    // run, so an older row must reopen its own record instead.
    QObject::connect(dlg, &LteCoDialog::runRecorded, this,
                     [self](const QString &label, const QVariantMap &params) {
                         if (self)
                             self->registerAnalysisProduct(
                                     u"lte"_s, label, params, nullptr,
                                     self->scopedProductId(u"lte%1"_s.arg(++self->m_lteRuns)));
                     });
    dlg->show();
}
