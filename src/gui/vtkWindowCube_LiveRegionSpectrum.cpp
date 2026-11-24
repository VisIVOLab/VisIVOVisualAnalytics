// The cube viewer's live region spectrum (issue #521) — partial class of vtkWindowCube.
//
// Until now a region's spectrum was made on release or by Analyse Region, each
// time as a new product in Session Data. Here the spectrum of the SELECTED
// region lives in one transitory pane, as the pixel probe's does:
//
//   * while a region is moved or resized it follows it, from the 3-D view's
//     samples (instant), recomputed at most every 150 ms;
//   * on release, or when a region is selected, the backend's mean over every
//     pixel of the file replaces it (#499) - the answer of the latest region
//     only, and kept by geometry so a region seen before asks nothing;
//   * Keep (Analyse Region) turns it into a permanent SPEC product, with the
//     statistics and the region in its Provenance, as before;
//   * the live spectra of all the other regions can be laid over it, each in
//     its region's colour and under its name.
//
// The pure parts (throttle, latest-wins tickets, cache, overlay plan) are in
// LiveRegionSpectra.h and unit-tested.

#include "vtkWindowCube.h"
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QtConcurrent>
#include "ui_vtkWindowCube.h"

#include "ProfileWidget.h"
#include "RegionCubeSampling.h"
#include "app/BackendClient.h"
#include "app/ProductRegistry.h"

#include <vtkImageData.h>
#include <vtkTrivialProducer.h>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;
using visivo::regions::Region;

namespace {
constexpr int kLiveIntervalMs = 150;
}

// ── The numbers ──────────────────────────────────────────────────────────────

bool vtkWindowCube::regionPreviewSpectrum(const Region &region, QVector<double> *spectral,
                                          QVector<double> *meanValues, int *samples) const
{
    // The 3-D view's cube is the full cube, a region of it, or a preview that
    // samples it (RegionCubeSampling.h). Each sample whose cube position is in
    // the region counts once: with a preview the spectrum is the mean of the
    // samples the preview has inside the region, not of every pixel of it.
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!cubeImage)
        return false;
    int extent[6];
    double o[3], sp[3];
    cubeImage->GetExtent(extent);
    cubeImage->GetOrigin(o);
    cubeImage->GetSpacing(sp);
    const int zCount = extent[5] - extent[4] + 1;
    spectral->resize(zCount);
    meanValues->fill(0.0, zCount);
    QVector<int> counts(zCount, 0);
    // Every channel keeps its spectral coordinate, blank or not: a fully
    // NaN plane must plot as a gap (NaN value), and QCustomPlot only gaps
    // on NaN VALUES - a NaN key broke the line into a spike instead.
    for (int i = 0; i < zCount; ++i) {
        const double datasetZ = this->displayToCube(o[2] + i * sp[2]);
        bool ok = false;
        (*spectral)[i] = this->spectralAxisValue(datasetZ, &ok);
        if (!ok)
            (*spectral)[i] = datasetZ;
    }
    const visivo::regions::AxisGrid gx{ extent[0], extent[1], o[0], sp[0] };
    const visivo::regions::AxisGrid gy{ extent[2], extent[3], o[1], sp[1] };
    const auto inside = visivo::regions::samplesInRegion(region, gx, gy, this->m_displaySampling);
    if (samples)
        *samples = int(inside.size());
    // Blanks were replaced by a finite sentinel below the visible minimum
    // when the cube was sanitised; averaging it turned a fully blank
    // channel into a deep negative spike.
    const double blankBelow = this->currentCubeVisibleRange[0];
    for (const auto &k : inside) {
        for (int z = extent[4]; z <= extent[5]; ++z) {
            const double v = cubeImage->GetScalarComponentAsDouble(k[0], k[1], z, 0);
            if (!std::isfinite(v) || v < blankBelow)
                continue;
            (*meanValues)[z - extent[4]] += v;
            counts[z - extent[4]] += 1;
        }
    }
    bool haveSpectrum = false;
    for (int i = 0; i < zCount; ++i) {
        if (counts[i] > 0) {
            (*meanValues)[i] /= counts[i];
            haveSpectrum = true;
        } else {
            (*meanValues)[i] = std::numeric_limits<double>::quiet_NaN();
        }
    }
    return haveSpectrum;
}

bool vtkWindowCube::previewIsFullResolution() const
{
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!cubeImage || this->m_displaySampling.scale > 1.0)
        return false;
    int e[6];
    cubeImage->GetExtent(e);
    return e[1] - e[0] + 1 == this->remoteDatasetWidth
            && e[3] - e[2] + 1 == this->remoteDatasetHeight
            && e[5] - e[4] + 1 == this->remoteDatasetDepth;
}

QVector<double> vtkWindowCube::fullResolutionSpectralAxis(qsizetype channels) const
{
    // Every channel of the file: a preview may show fewer (its sampling is
    // along z too), so the axis is rebuilt, in the viewer's own spectral
    // convention and unit, from the file's channel indices.
    QVector<double> key(channels);
    for (qsizetype i = 0; i < channels; ++i) {
        bool okAxis = false;
        const double v = this->spectralAxisValue(double(i), &okAxis);
        key[i] = okAxis ? v : double(i);
    }
    return key;
}

QString vtkWindowCube::liveRegionCubeKey() const
{
    // What a cached preview spectrum was measured on: the displayed cube, its
    // sampling, the blank sentinel and the spectral axis. Any change makes it stale.
    auto *img = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!img)
        return QString();
    int e[6];
    img->GetExtent(e);
    return u"%1/%2/%3,%4,%5,%6,%7,%8/%9/%10/%11"_s.arg(quintptr(img))
            .arg(qulonglong(img->GetMTime()))
            .arg(e[0]).arg(e[1]).arg(e[2]).arg(e[3]).arg(e[4]).arg(e[5])
            .arg(this->m_displaySampling.scale)
            .arg(this->currentCubeVisibleRange[0])
            // The spectral axis the keys are written in: a new choice makes
            // every cached preview curve stale.
            .arg(this->spectralAxisTitle() + this->m_spectralAxis.frame
                 + QString::number(int(this->m_spectralAxis.tableActive)));
}

// ── What a new region does ───────────────────────────────────────────────────

void vtkWindowCube::measureNewRegion(int id, vtkRenderWindow *win)
{
    const Region *r = this->m_regions.find(id);
    if (!r)
        return;
    if (!m_liveRegionSpectra || !visivo::regions::hasArea(r->shape)) {
        this->analyzeRegion(id, win);
        return;
    }
    // The list change that added it already put its spectrum in the live pane:
    // a product per region drawn is what made several regions tedious.
    this->statusBar()->showMessage(
            u"%1: live spectrum in its pane. Analyse Region keeps it, with the statistics, "
            u"in Session Data."_s.arg(r->name),
            6000);
}

// ── Following the selection ──────────────────────────────────────────────────

void vtkWindowCube::updateLiveRegionSpectrum(bool dragging)
{
    if (!m_liveRegionSpectra)
        return;
    if (!m_regionLiveClock.isValid())
        m_regionLiveClock.start();
    if (!m_regionLiveTimer) {
        m_regionLiveTimer = new QTimer(this);
        m_regionLiveTimer->setSingleShot(true);
        QObject::connect(m_regionLiveTimer, &QTimer::timeout, this, [this]() {
            m_regionLiveThrottle.fired(m_regionLiveClock.elapsed());
            if (m_regionDrag.active) {
                this->runLiveRegionSpectrum(/*allowFullRes=*/false);
                // The interval counts from the END of the run: on a big cube
                // one preview can take longer than it.
                m_regionLiveThrottle.ranNow(m_regionLiveClock.elapsed());
            }
        });
    }
    const Region *sel = this->m_regions.selected();
    if (!sel || !visivo::regions::hasArea(sel->shape)) {
        // Clicking empty slice to pan lets go of the selection: the spectrum
        // stays. Only the region going away ends it.
        if (!m_regionLiveId.isEmpty() && !this->m_regions.find(m_regionLiveRegionId)) {
            // The region the pane shows is gone. While other visible regions
            // remain the pane follows the latest drawn one (not selecting it:
            // a second Delete must not take that one too) instead of leaving
            // an empty slot; with the last one gone the pane is given back.
            int next = -1;
            const auto &all = this->m_regions.regions();
            for (auto it = all.rbegin(); it != all.rend(); ++it)
                if (it->visible && visivo::regions::hasArea(it->shape)) {
                    next = it->id;
                    break;
                }
            if (next >= 0 && this->cubeDisplaySource) {
                m_regionLiveRegionId = next;
                m_regionLiveSignature.clear();
                this->runLiveRegionSpectrum(/*allowFullRes=*/true, /*force=*/true);
                if (!m_regionLiveSignature.isEmpty())   // it drew
                    return;
            }
            this->stopLiveRegionSpectrum();
        }
        return;
    }
    if (dragging) {
        const auto d = m_regionLiveThrottle.request(m_regionLiveClock.elapsed());
        if (d.action == visivo::liveregion::Throttle::Action::Run) {
            this->runLiveRegionSpectrum(/*allowFullRes=*/false);
            m_regionLiveThrottle.ranNow(m_regionLiveClock.elapsed());
        } else if (d.action == visivo::liveregion::Throttle::Action::Arm)
            m_regionLiveTimer->start(d.delayMs);
        return;
    }
    // A selection, a release, a typed value: now, and the clock restarts. A
    // timer still armed from the drag is not owed any more.
    m_regionLiveTimer->stop();
    m_regionLiveThrottle.ranNow(m_regionLiveClock.elapsed());
    this->runLiveRegionSpectrum(/*allowFullRes=*/true);
}

void vtkWindowCube::runLiveRegionSpectrum(bool allowFullRes, bool force)
{
    const Region *sel = this->m_regions.selected();
    // A forced redraw (the spectral axis changed) needs no selection: it
    // redraws the region the pane shows.
    if (force && (!sel || !visivo::regions::hasArea(sel->shape)))
        sel = this->m_regions.find(m_regionLiveRegionId);
    if (!sel || !visivo::regions::hasArea(sel->shape) || !this->cubeDisplaySource)
        return;
    const Region region = *sel;   // a copy: the list may change under the callbacks below
    const QString signature = visivo::liveregion::geometrySignature(region);
    const QString cubeKey = this->liveRegionCubeKey();
    const bool unchanged = !force && !m_regionLiveId.isEmpty() && region.id == m_regionLiveRegionId
            && signature == m_regionLiveSignature && cubeKey == m_regionLiveCubeKey;
    if (!unchanged) {
        const int previousId = m_regionLiveRegionId;
        QVector<double> key, values;
        int samples = 0;
        if (!this->regionPreviewSpectrum(region, &key, &values, &samples)) {
            // Nothing of the cube under it (off the field, or all blank): say so
            // where the user looks, and keep the last spectrum out of it.
            this->statusBar()->showMessage(
                    u"%1: no valid pixel of the 3-D view's cube is inside the region."_s.arg(region.name),
                    4000);
            // Whatever the backend still computes is for a geometry no longer there.
            m_regionLiveTicket.invalidate();
            m_regionLiveSignature.clear();
            m_regionLiveRequestedSignature.clear();
            return;
        }
        // A new geometry: whatever the backend is still computing is for the old one.
        m_regionLiveTicket.invalidate();
        m_regionLiveRequestedSignature.clear();
        m_regionLiveRegionId = region.id;
        m_regionLiveSignature = signature;
        m_regionLiveCubeKey = cubeKey;
        const bool full = this->previewIsFullResolution();
        const double scale = this->m_displaySampling.scale;
        this->applyLiveRegionSpectrum(
                region, key, values, full,
                full ? u"full resolution (%1 pixels)"_s.arg(samples)
                     : u"3-D preview, 1 pixel in %1 per axis (%2 samples)"_s.arg(scale, 0, 'g', 3)
                               .arg(samples),
                /*reshow=*/region.id != previousId);
    } else {
        this->updateLiveRegionHeader();   // a rename
        if (m_liveRegionOverlayAll)
            this->pushLiveRegionOverlay();   // another region changed
    }
    if (!allowFullRes || m_regionLiveFullRes || this->previewIsFullResolution()
        || this->remoteDatasetId.isEmpty())
        return;
    if (const QVector<double> *cached = m_regionFullResCache.find(this->liveRegionCacheKey(signature))) {
        this->applyLiveRegionSpectrum(region, this->fullResolutionSpectralAxis(cached->size()),
                                      *cached, /*fullResolution=*/true,
                                      u"full resolution (from the file)"_s);
        return;
    }
    // Asked once per geometry: a rename or a second press on the region must not
    // take a new ticket (it would make the answer on its way stale), and a
    // failure is not retried on every list change.
    if (m_regionLiveRequestedSignature == signature)
        return;
    m_regionLiveRequestedSignature = signature;
    this->requestLiveFullResolution(region, signature);
}

QString vtkWindowCube::liveRegionCacheKey(const QString &signature) const
{
    // A spectrum is the file's: the same geometry on another dataset is another one.
    return this->remoteDatasetId + u'|' + signature;
}

void vtkWindowCube::requestLiveFullResolution(const Region &region, const QString &signature)
{
    const QJsonObject canonical = visivo::regions::toCanonical(region, 0.0);
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
    const std::uint64_t ticket = m_regionLiveTicket.next();
    auto *watcher = new QFutureWatcher<BackendSpectrumResult>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, ticket, signature, region]() {
        watcher->deleteLater();
        const BackendSpectrumResult r = watcher->result();
        // Latest wins: the region moved, another was selected, the spectrum
        // was switched off. A stale answer is dropped, not drawn over the new one.
        if (!m_regionLiveTicket.isCurrent(ticket) || m_regionLiveId.isEmpty()
            || signature != m_regionLiveSignature)
            return;
        if (!r.valid || r.values.isEmpty()) {
            this->statusBar()->showMessage(
                    u"%1: the full-resolution spectrum failed (%2); the spectrum shown is the "
                    u"3-D preview's."_s.arg(region.name, r.valid ? u"no channels"_s : r.error),
                    8000);
            return;
        }
        m_regionFullResCache.insert(this->liveRegionCacheKey(signature), r.values);
        this->applyLiveRegionSpectrum(
                region, this->fullResolutionSpectralAxis(r.values.size()), r.values,
                /*fullResolution=*/true,
                u"full resolution, %1 pixels (from the file)"_s.arg(r.nPix));
    });
    watcher->setFuture(QtConcurrent::run([url, token, session, dataset, canonical]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        return client.spectralRegion(dataset, canonical);
    }));
}

// ── The pane ─────────────────────────────────────────────────────────────────

void vtkWindowCube::keepSourcePaneActive()
{
    // borrowPaneSlot() never takes the active pane. A press on a map pane may
    // not have made it active yet, so say so before a pane is asked for.
    const int src = m_regionFocusWin ? this->regionPaneSlot(m_regionFocusWin) : -1;
    if (src >= 0 && src != m_activePane)
        setActivePane(src);
}

void vtkWindowCube::ensureLiveRegionProduct(bool reshow)
{
    if (!m_regionLiveId.isEmpty() && m_products1d.contains(m_regionLiveId)) {
        // Selecting another region brings the pane back if it was closed;
        // a drag does not (the user may have closed it on purpose).
        if (reshow) {
            this->keepSourcePaneActive();
            this->acquirePaneForProduct(m_regionLiveId, /*mayBorrow=*/true);
            refreshPaneChrome();
        }
        return;
    }
    m_regionLiveId = this->scopedProductId(u"regionspectrumlive"_s);
    Spectrum1D s;
    s.xLabel = this->spectralAxisTitle().isEmpty() ? u"Spectral axis"_s : this->spectralAxisTitle();
    s.yLabel = u"Mean value"_s;
    s.label = u"Region spectrum (live)"_s;
    m_products1d.insert(m_regionLiveId, s);
    // No Provenance reveal: nothing was asked for, and the row is transitory.
    this->registerAnalysisProduct(u"spectrum"_s, s.label, QVariantMap{}, this, m_regionLiveId,
                                  u"spectrum1d"_s, /*revealProvenance=*/false,
                                  QVariantMap{}, /*selectInSession=*/false);
    ProductRegistry::instance().setStatus(m_regionLiveId, u"live"_s);   // italic/muted in the tree
    // The pane the region is being edited in must not be the one lent out.
    this->keepSourcePaneActive();
    // It may borrow a pane, like the probe's live spectrum, and gives it back
    // when it ends (the setting switched off, the region deleted): the live
    // spectrum has to be visible, and a full layout must not hide it.
    const int target = acquirePaneForProduct(m_regionLiveId, /*mayBorrow=*/true);
    if (target < 0)
        this->statusBar()->showMessage(
                u"Live region spectrum: no pane could be freed - close a pane's view, or pick "
                u"“Region spectrum (live)” from a pane's ▾ menu."_s,
                6000);
    refreshPaneChrome();
}

void vtkWindowCube::applyLiveRegionSpectrum(const Region &region, const QVector<double> &key,
                                            const QVector<double> &values, bool fullResolution,
                                            const QString &meta, bool reshow)
{
    this->ensureLiveRegionProduct(reshow);
    m_regionLiveFullRes = fullResolution;
    m_regionLiveMeta = meta;
    Spectrum1D &s = m_products1d[m_regionLiveId];
    s.key = key;
    s.values = values;
    s.xLabel = this->spectralAxisTitle().isEmpty() ? u"Spectral axis"_s : this->spectralAxisTitle();
    s.yLabel = u"Mean value"_s;
    s.label = u"%1 (live)"_s.arg(region.name);
    for (int i = 0; i < 4; ++i) {
        if (m_slots[i].view.kind != ViewId::Product || m_slots[i].view.productId != m_regionLiveId
            || !m_paneSpectra[i])
            continue;
        m_paneSpectra[i]->setSpectrumAxisLabels(s.xLabel, s.yLabel);
        m_paneSpectra[i]->updateSpectrumPlot(key, values, s.label, /*live=*/true);
        m_paneSpectra[i]->setSpectrumContext(this->filepath, -1, -1, QString());
    }
    this->updateLiveRegionHeader();
    this->pushLiveRegionOverlay();
    this->updateSpectrumInspectorControls();
}

void vtkWindowCube::updateLiveRegionHeader()
{
    if (m_regionLiveId.isEmpty())
        return;
    const Region *r = this->m_regions.find(m_regionLiveRegionId);
    const QString name = r ? r->name : u"Region"_s;
    const QString state = m_regionLiveFullRes ? u"● full resolution"_s : u"○ preview"_s;
    const QString hint = u"%1  ·  Analyse Region (or Keep in the Inspector) saves it in "
                         u"Session Data"_s.arg(m_regionLiveMeta);
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product && m_slots[i].view.productId == m_regionLiveId
            && m_paneSpectra[i]) {
            // The embedded pane hides the widget's own header: the pane's
            // qualifier carries the state, the meta line the description.
            m_paneSpectra[i]->setSpectrumMeta(u"%1 (live)  ·  %2"_s.arg(name, m_regionLiveMeta),
                                              hint);
            if (m_slots[i].qualifier)
                m_slots[i].qualifier->setText(u"%1  ·  %2"_s.arg(name, state));
        }
}

void vtkWindowCube::stopLiveRegionSpectrum()
{
    m_regionLiveTicket.invalidate();
    if (m_regionLiveTimer)
        m_regionLiveTimer->stop();
    // A timer stopped here would leave the throttle waiting for it for good.
    if (m_regionLiveClock.isValid())
        m_regionLiveThrottle.ranNow(m_regionLiveClock.elapsed());
    m_regionLiveRegionId = -1;
    m_regionLiveRequestedSignature.clear();
    m_regionLiveSignature.clear();
    m_regionLiveCubeKey.clear();
    m_regionLiveFullRes = false;
    if (m_regionLiveId.isEmpty())
        return;
    const QString id = m_regionLiveId;
    m_regionLiveId.clear();
    m_products1d.remove(id);
    ProductRegistry::instance().removeProduct(id);
    // A borrowed pane goes back to what it was showing; any other pane that
    // still shows the product returns to "Pick a view".
    releaseBorrowedPane(id);
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product && m_slots[i].view.productId == id) {
            m_slots[i].view = {};
            mountView(i);
        }
    refreshPaneChrome();
    this->updateSpectrumInspectorControls();
}

// ── Overlay of all regions ───────────────────────────────────────────────────

void vtkWindowCube::applyLiveRegionOverlay(ProfileWidget *plot)
{
    if (!plot)
        return;
    const Region *sel = this->m_regions.find(m_regionLiveRegionId);
    if (!m_liveRegionOverlayAll || !sel) {
        if (plot->overlaySpectrumCount() > 0)
            plot->setOverlaySpectra({});
        return;
    }
    const QString cubeKey = this->liveRegionCubeKey();
    QVector<ProfileWidget::OverlayCurve> curves;
    QHash<int, LiveOverlayMemo> memo;
    const auto colours = visivo::liveregion::overlayColours(this->m_regions.regions(), sel->id);
    for (const auto &entry :
         visivo::liveregion::overlayPlan(this->m_regions.regions(), sel->id)) {
        const Region *r = this->m_regions.find(entry.regionId);
        if (!r)
            continue;
        const QString sig = visivo::liveregion::geometrySignature(*r);
        ProfileWidget::OverlayCurve c;
        c.name = entry.name;
        c.colour = QColor(entry.colour);
        if (const QVector<double> *full = m_regionFullResCache.find(this->liveRegionCacheKey(sig))) {
            // Seen at full resolution already: that one, on its own axis.
            c.key = this->fullResolutionSpectralAxis(full->size());
            c.values = *full;
        } else {
            LiveOverlayMemo m = m_regionOverlayMemo.value(entry.regionId);
            if (m.signature != sig || m.cubeKey != cubeKey) {
                m.signature = sig;
                m.cubeKey = cubeKey;
                int samples = 0;
                if (!this->regionPreviewSpectrum(*r, &m.key, &m.values, &samples)) {
                    m.key.clear();
                    m.values.clear();
                }
            }
            memo.insert(entry.regionId, m);
            c.key = m.key;
            c.values = m.values;
        }
        if (!c.values.isEmpty())
            curves.push_back(std::move(c));
    }
    m_regionOverlayMemo = std::move(memo);   // regions that went away drop out
    plot->setOverlaySpectra(curves, sel->name, QColor(colours.at(sel->id)));
}

void vtkWindowCube::pushLiveRegionOverlay()
{
    if (m_regionLiveId.isEmpty())
        return;
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product && m_slots[i].view.productId == m_regionLiveId
            && m_paneSpectra[i])
            this->applyLiveRegionOverlay(m_paneSpectra[i]);
}

// ── Settings and Keep ────────────────────────────────────────────────────────

void vtkWindowCube::setLiveRegionSpectra(bool on)
{
    if (m_liveRegionSpectra == on)
        return;
    m_liveRegionSpectra = on;
    {
        QSettings s;
        s.setValue(u"regions/liveSpectrum"_s, on);
    }
    if (m_actLiveRegionSpectra && m_actLiveRegionSpectra->isChecked() != on) {
        const QSignalBlocker b(m_actLiveRegionSpectra);
        m_actLiveRegionSpectra->setChecked(on);
    }
    if (on)
        this->updateLiveRegionSpectrum(/*dragging=*/false);
    else
        this->stopLiveRegionSpectrum();
}

void vtkWindowCube::setLiveRegionOverlayAll(bool on)
{
    m_liveRegionOverlayAll = on;
    for (QAction *a : { m_actOverlayRegionSpectra.data() })
        if (a && a->isChecked() != on) {
            const QSignalBlocker b(a);
            a->setChecked(on);
        }
    if (m_specInspOverlayAll && m_specInspOverlayAll->isChecked() != on) {
        const QSignalBlocker b(m_specInspOverlayAll);
        m_specInspOverlayAll->setChecked(on);
    }
    if (!m_liveRegionSpectra)
        return;
    // Turning it on from nothing selected has nothing to lay over yet.
    if (m_regionLiveId.isEmpty())
        this->updateLiveRegionSpectrum(/*dragging=*/false);
    else
        this->pushLiveRegionOverlay();
}

void vtkWindowCube::keepRegionSpectrumFull(int id, bool show,
                                           std::function<void(const QString &, bool)> done)
{
    const Region *found = this->m_regions.find(id);
    if (!found || !visivo::regions::hasArea(found->shape) || !this->cubeDisplaySource) {
        done(QString(), false);
        return;
    }
    const Region region = *found;
    const QString signature = visivo::liveregion::geometrySignature(region);
    const QString ledgerKey =
            visivo::liveregion::KeptLedger::key(signature, this->liveRegionCubeKey());
    // Whatever full-resolution spectrum is at hand when it is there: made into
    // the product unless this very spectrum has one already.
    auto finish = [this, id, ledgerKey, signature, show, done](const QVector<double> &full,
                                                               bool haveFull) {
        // The region may have been dragged while the backend was computing:
        // a spectrum of the old geometry must not become the product of the
        // new one.
        const Region *now = this->m_regions.find(id);
        if (haveFull
            && (!now || visivo::liveregion::geometrySignature(*now) != signature
                || visivo::liveregion::KeptLedger::key(signature, this->liveRegionCubeKey())
                        != ledgerKey)) {
            this->statusBar()->showMessage(
                    u"The region changed while its spectrum was being computed; nothing was "
                    u"kept. Try again."_s,
                    6000);
            done(QString(), false);
            return;
        }
        const QString already = m_keptSpectra.find(id, ledgerKey, [this](const QString &p) {
            return m_products1d.contains(p);
        });
        if (!already.isEmpty()) {
            done(already, true);
            return;
        }
        vtkRenderWindow *win = this->regionAnalysisWin();
        if (!win) {
            this->statusBar()->showMessage(
                    u"Show the 2D Slice or a moment map in a pane to keep the region's spectrum "
                    u"with its statistics."_s,
                    5000);
            done(QString(), false);
            return;
        }
        const QString pid = this->analyzeRegion(id, win, haveFull ? &full : nullptr, show);
        if (!pid.isEmpty())
            m_keptSpectra.record(id, ledgerKey, pid);
        done(pid, !pid.isEmpty());
    };
    // The 3-D view already holds the whole cube, or there is no backend to
    // ask: the spectrum at hand is the best there is.
    if (this->previewIsFullResolution() || this->remoteDatasetId.isEmpty()) {
        finish(QVector<double>(), false);
        return;
    }
    const QString cacheKey = this->liveRegionCacheKey(signature);
    if (const QVector<double> *cached = m_regionFullResCache.find(cacheKey)) {
        const QVector<double> copy = *cached;   // the cache may move under finish()
        finish(copy, true);
        return;
    }
    const QJsonObject canonical = visivo::regions::toCanonical(region, 0.0);
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
    auto *watcher = new QFutureWatcher<BackendSpectrumResult>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this,
                     [this, watcher, cacheKey, region, finish, done]() {
        watcher->deleteLater();
        const BackendSpectrumResult r = watcher->result();
        if (!r.valid || r.values.isEmpty()) {
            this->statusBar()->showMessage(
                    u"%1: the full-resolution spectrum failed (%2)."_s.arg(
                            region.name, r.valid ? u"no channels"_s : r.error),
                    8000);
            done(QString(), false);
            return;
        }
        m_regionFullResCache.insert(cacheKey, r.values);
        finish(r.values, true);
    });
    watcher->setFuture(QtConcurrent::run([url, token, session, dataset, canonical]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        return client.spectralRegion(dataset, canonical);
    }));
}

void vtkWindowCube::keepLiveRegionForExport(std::function<void()> proceed)
{
    const Region *r = this->m_regions.find(m_regionLiveRegionId);
    if (!r || !visivo::regions::hasArea(r->shape)) {
        proceed();   // nothing of a region behind the pane
        return;
    }
    if (m_liveExportBusy)
        return;   // an export is already waiting for its full-resolution spectrum
    m_liveExportBusy = true;
    const int id = r->id;
    const QString signature = visivo::liveregion::geometrySignature(*r);
    this->statusBar()->showMessage(
            u"%1: keeping its full-resolution spectrum in Session Data before exporting..."_s
                    .arg(r->name),
            0);
    this->keepRegionSpectrumFull(id, /*show=*/true,
                                 [this, id, signature, proceed](const QString &, bool ok) {
        m_liveExportBusy = false;
        if (!ok) {
            this->statusBar()->showMessage(
                    u"Nothing exported: the region's full-resolution spectrum could not be kept."_s,
                    8000);
            return;
        }
        // The file written is the product's: the pane shows the full-resolution
        // spectrum too before it is read.
        const Region *now = this->m_regions.find(id);
        if (now && signature == m_regionLiveSignature && !m_regionLiveFullRes) {
            if (const QVector<double> *full =
                        m_regionFullResCache.find(this->liveRegionCacheKey(signature))) {
                const QVector<double> copy = *full;
                this->applyLiveRegionSpectrum(
                        *now, this->fullResolutionSpectralAxis(copy.size()), copy,
                        /*fullResolution=*/true, u"full resolution (from the file)"_s);
            }
        }
        if (m_regionLiveRegionId != id || m_regionLiveSignature != signature) {
            this->statusBar()->showMessage(
                    u"The live spectrum changed while the region was being kept; nothing was "
                    u"exported. Export again."_s,
                    6000);
            return;
        }
        this->statusBar()->clearMessage();
        proceed();
    });
}

struct vtkWindowCube::KeepAllState
{
    std::vector<int> ids;
    std::size_t next{ 0 };
    int kept{ 0 };
    int failed{ 0 };
    bool cancelled{ false };
    QPointer<QProgressDialog> dialog;
};

void vtkWindowCube::keepAllRegionSpectra()
{
    if (m_keepAllRunning)
        return;
    auto st = std::make_shared<KeepAllState>();
    st->ids = visivo::liveregion::keepAllTargets(this->m_regions.regions());
    if (st->ids.empty()) {
        this->statusBar()->showMessage(u"No region with pixels to keep a spectrum of."_s, 5000);
        return;
    }
    if (!this->regionAnalysisWin()) {
        this->statusBar()->showMessage(
                u"Show the 2D Slice or a moment map in a pane to keep the regions' spectra with "
                u"their statistics."_s,
                5000);
        return;
    }
    m_keepAllRunning = true;
    auto *dlg = new QProgressDialog(u"Keeping the spectra of the regions..."_s, u"Cancel"_s, 0,
                                    int(st->ids.size()), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(u"Keep Spectra of All Regions"_s);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoReset(false);
    dlg->setAutoClose(false);
    dlg->setValue(0);
    st->dialog = dlg;
    QObject::connect(dlg, &QProgressDialog::canceled, this, [st]() { st->cancelled = true; });
    dlg->show();
    this->keepAllStep(st);
}

void vtkWindowCube::keepAllStep(std::shared_ptr<KeepAllState> st)
{
    if (st->cancelled || st->next >= st->ids.size()) {
        m_keepAllRunning = false;
        const std::size_t left = st->ids.size() - st->next;
        if (st->dialog)
            st->dialog->close();
        this->statusBar()->showMessage(
                u"Kept the full-resolution spectra of %1 region(s) in Session Data%2%3."_s
                        .arg(st->kept)
                        .arg(st->failed > 0 ? u"; %1 failed"_s.arg(st->failed) : QString())
                        .arg(st->cancelled && left > 0 ? u"; cancelled, %1 left"_s.arg(left)
                                                       : QString()),
                10000);
        this->refreshPaneChrome();
        return;
    }
    const int id = st->ids[st->next];
    if (st->dialog) {
        const Region *r = this->m_regions.find(id);
        st->dialog->setLabelText(u"Region %1 of %2: %3"_s.arg(st->next + 1)
                                         .arg(st->ids.size())
                                         .arg(r ? r->name : QString()));
        st->dialog->setValue(int(st->next));
    }
    this->keepRegionSpectrumFull(id, /*show=*/false,
                                 [this, st](const QString &, bool ok) {
        ++(ok ? st->kept : st->failed);
        ++st->next;
        // Out of this call: a synchronous product would otherwise recurse
        // once per region.
        QTimer::singleShot(0, this, [this, st]() { this->keepAllStep(st); });
    });
}

void vtkWindowCube::keepLiveRegionSpectrum()
{
    // The same as Analyse Region: the statistics, the region in the product's
    // Provenance, the full-resolution spectrum asked for. The product is the
    // result of that, not a copy of the live curve, so it carries the numbers
    // it is read with.
    if (m_regionLiveRegionId < 0 || !this->m_regions.find(m_regionLiveRegionId))
        return;
    vtkRenderWindow *win = this->regionAnalysisWin();
    if (!win) {
        this->statusBar()->showMessage(
                u"Show the 2D Slice or a moment map in a pane to keep the region's spectrum "
                u"with its statistics."_s,
                5000);
        return;
    }
    const QString pid = this->analyzeRegion(m_regionLiveRegionId, win);
    // Recorded only when the product holds the full spectrum now: with a
    // preview it is replaced later (or not, if the backend fails), and an
    // export must not take it for the full-resolution one.
    if (!pid.isEmpty() && (this->previewIsFullResolution() || this->remoteDatasetId.isEmpty()))
        m_keptSpectra.record(m_regionLiveRegionId,
                             visivo::liveregion::KeptLedger::key(
                                     visivo::liveregion::geometrySignature(
                                             *this->m_regions.find(m_regionLiveRegionId)),
                                     this->liveRegionCubeKey()),
                             pid);
}
