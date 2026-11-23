#include "vtkWindowCube.h"

#include "vtk/LutScale.h"
#include "ui_vtkWindowCube.h"
#include "CubeViewportGeometry.h"
#include "CubeStatsUtil.h"
#include "SliceDetailPlan.h"

#include <QByteArray>  // decodeRemoteSlice / fetchRemoteSlice operate on raw bytes
#include <QScopedValueRollback>

#include "qcustomplot.h"
#include "ColorMaps.h"
#include "ChannelOverlay.h"
#include "CubeUiAssembler.h"
#include "CubeVRController.h"
#include "DiagnosticsWindow.h"
#include "theme/SegmentedToggle.h"
#include "CatalogueOverlayUtils.h"
#include "CatalogueTableModel.h"
#include "LUTCustomizerDialog.h"
#include "BaselineDialog.h"
#include "LinewidthDialog.h"
#include "MomentMapController.h"
#include "NoiseController.h"
#include "StackDialog.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"
#include "ToolResultDialog.h"
#include "ProfileWidget.h"
#include "PvController.h"
#include "PvDiagramWidget.h"
#include "RemoteRenderWidget.h"
#include "SidebarPanel.h"
#include "Settings.h"
#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "vtkInteractorStyleProfile.h"
#include "vtkLegendScaleActorWCS.h"
#include "wcs.h"

#include <QVTKInteractor.h>
#include <vtkActor.h>
#include <vtkAxisActor2D.h>
#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkColorTransferFunction.h>
#include <vtkContourTriangulator.h>
#include <vtkCoordinate.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges2D.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageMapToColors.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleUser.h>
#include <vtkCubeSource.h>
#include <vtkLineSource.h>
#include <vtkLookupTable.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkCubeAxesActor.h>
#include <vtkOutlineFilter.h>
#include <vtkPropPicker.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkPlaneSource.h>
#include <vtkPiecewiseFunction.h>
#include <vtkTexture.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkRenderWindow.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkDataArray.h>
#include <vtkIdTypeArray.h>
#include <vtkPointData.h>
#include <vtkTextProperty.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <QFileInfo>
#include <QActionGroup>
#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolBar>
#include <QToolButton>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include "ChannelMapsDialog.h"
#include "ChannelMapsWindow.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QList>
#include <QMetaObject>
#include <QMenu>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QGroupBox>
#include <QSlider>
#include <QSpinBox>
#include <QTableView>
#include <QThread>
#include <QtConcurrentRun>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

using namespace Qt::StringLiterals;
using namespace CubeGeom;
using namespace CubeStats;

// ── Remote (server-side) slice fetch + cache ─────────────────────────────────
// Extracted verbatim from vtkWindowCube.cpp. On-demand fetch of 2-D slices from
// the backend, LRU cache, neighbour prefetch, and drag feedback.

namespace {
void refitParallelSliceCamera(vtkRenderer *renderer, vtkImageData *sliceImage, vtkRenderWindow *win);
RemoteCubeSliceResult fetchRemoteSlice(const QString &backendUrl, const QString &datasetId,
                                       int index, const QString &sessionId,
                                       const QString &backendToken, std::optional<QRect> region);

void refitParallelSliceCamera(vtkRenderer *renderer, vtkImageData *sliceImage, vtkRenderWindow *win)
{
    if (!renderer || !sliceImage || !win) {
        return;
    }

    double bounds[6];
    sliceImage->GetBounds(bounds);
    if (!validBounds(bounds)) {
        return;
    }

    auto *camera = renderer->GetActiveCamera();
    double position[3];
    double focalPoint[3];
    camera->GetPosition(position);
    camera->GetFocalPoint(focalPoint);

    const double center[3] = { 0.5 * (bounds[0] + bounds[1]), 0.5 * (bounds[2] + bounds[3]),
                               0.5 * (bounds[4] + bounds[5]) };
    const double offset[3] = { position[0] - focalPoint[0], position[1] - focalPoint[1],
                               position[2] - focalPoint[2] };

    camera->SetFocalPoint(center);
    camera->SetPosition(center[0] + offset[0], center[1] + offset[1], center[2] + offset[2]);

    const int *size = win->GetSize();
    const double aspect = (size && size[1] > 0) ? static_cast<double>(size[0]) / size[1] : 1.0;
    const double width = std::max(1e-6, bounds[1] - bounds[0]);
    const double height = std::max(1e-6, bounds[3] - bounds[2]);
    constexpr double margin = 1.1;
    camera->SetParallelScale(
            margin * std::max(height * 0.5, width * 0.5 / std::max(1.0, aspect)));
}

RemoteCubeSliceResult fetchRemoteSlice(const QString &backendUrl, const QString &datasetId,
                                       int index, const QString &sessionId,
                                       const QString &backendToken, std::optional<QRect> region)
{
    RemoteCubeSliceResult result;
    result.index = index;

    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    const auto response = client.requestSlice(datasetId, QStringLiteral("z"), index,
                                              visivo::kRemoteSliceLongestSide, region);
    if (!response.valid) {
        result.errorMessage = response.error.isEmpty() ? u"Remote slice request failed."_s
                                                       : response.error;
        return result;
    }

    if (response.scalarType != u"float32"_s) {
        result.errorMessage = u"Unsupported remote slice scalar type."_s;
        return result;
    }

    result.imageData = decodeRemoteSlice(response);
    if (!result.imageData) {
        result.errorMessage = u"Invalid remote slice payload."_s;
        return result;
    }

    result.valid = true;
    result.imageRange = { response.rangeMin, response.rangeMax };
    result.fullWidth = response.fullWidth;
    result.fullHeight = response.fullHeight;
    result.stride = response.stride;
    if (region)
        result.region = QRect(response.x0, response.y0,
                              (response.width - 1) * response.stride + 1,
                              (response.height - 1) * response.stride + 1);
    return result;
}

}  // namespace

// The samples sit on the plane's own pixel frame (origin x0/y0, spacing the
// stride), so a reduced channel lies where the full one would and everything
// placed in plane pixels (regions, probe, WCS, linked cameras) still lands.
vtkSmartPointer<vtkImageData> decodeRemoteSlice(const BackendCubeSliceResult &response)
{
    const qsizetype expectedBytes = static_cast<qsizetype>(response.width) * response.height
            * static_cast<qsizetype>(sizeof(float));
    if (response.width <= 0 || response.height <= 0 || response.data.size() != expectedBytes) {
        return nullptr;
    }

    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, response.width - 1, 0, response.height - 1, 0, 0);
    image->SetOrigin(response.x0, response.y0, 0.);
    const double stride = std::max(1, response.stride);
    image->SetSpacing(stride, stride, 1.);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(image->GetScalarPointer(), response.data.constData(),
                static_cast<std::size_t>(expectedBytes));
    return image;
}

void vtkWindowCube::requestRemoteSlice(int sliceIndex)
{
    sliceIndex = this->clampRemoteSliceIndex(sliceIndex);
    this->currentRequestedRemoteSliceIndex = sliceIndex;
    // Mark this as a USER-initiated request so its resulting
    // applyRemoteSliceResult is allowed to spawn ±1 prefetches.
    // Prefetched results themselves leave this flag false → no cascade.
    // Linked-driven requests skip prefetch so synced scrubbing across N windows
    // doesn't multiply neighbour fetches.
    this->prefetchAllowedForNextResult = !this->m_applyingLinkedUpdate;
    // Cancel any pending idle prefetch — the user just acted, so a
    // previously-scheduled prefetch is no longer interesting. The
    // applyRemoteSliceResult for THIS request will re-arm the timer.
    this->idlePrefetchTimer.stop();
    if (this->tryApplyCachedRemoteSlice(sliceIndex)) {
        return;
    }

    // Asking for a slice again — whether that starts a fetch or adopts one that
    // is already out — takes a previous failure off the pane. Both paths, or
    // adopting an in-flight fetch would leave the old error sitting there while
    // the status bar says it is loading.
    if (this->m_slice2dLoadingActor && this->m_slice2dLoadingActor->GetVisibility()) {
        this->setSlice2dPlaceholder(u"Loading 2D view…"_s);
    }

    const QString key = this->remoteSliceCacheKey(sliceIndex);
    if (const auto inFlight = this->remoteSliceFetchesInFlight.constFind(key);
        inFlight != this->remoteSliceFetchesInFlight.cend()) {
        // Adopt that fetch as the current request. Returning without doing so
        // left BOTH fetches unappliable: this one's requestId was no longer
        // current, and the other one's index no longer matched, so asking again
        // for a channel already being fetched displayed neither.
        if (QObject *watcher = inFlight.value()) {
            // PROMOTE it to this request: a fresh id (so nothing else can be
            // confused for it), this caller's origin for the linked-broadcast
            // decision, and - the reason the watcher is kept rather than its id -
            // no longer a prefetch, since a prefetch's result is cached but never
            // displayed and never reports its errors.
            const int oldId = watcher->property("requestId").toInt();
            const bool wasPrefetch = watcher->property("isPrefetch").toBool();
            this->m_linkedRemoteRequestIds.remove(oldId);
            const int adopted = ++this->m_remoteSliceRequestIdSeq;
            watcher->setProperty("requestId", adopted);
            watcher->setProperty("isPrefetch", false);
            if (wasPrefetch)
                ++this->activeRemoteSliceRequests;   // the handler will decrement it now
            if (this->m_applyingLinkedUpdate)
                this->m_linkedRemoteRequestIds.insert(adopted);
            this->currentRemoteSliceRequestId = adopted;
        }
        this->statusBar()->showMessage(u"Loading remote slice..."_s);
        return;
    }

    // Allocate from a counter that only ever goes forward. currentRemoteSliceRequestId
    // says which result is wanted and can move BACKWARDS when a fetch is adopted
    // above; deriving new ids from it would then hand a live watcher's id to a
    // second request, and the two would consume each other's linked tokens.
    const int requestId = ++this->m_remoteSliceRequestIdSeq;
    this->currentRemoteSliceRequestId = requestId;
    // E-07/M6: remember linked-originated fetches by requestId so the (async)
    // result can be recognised as linked-driven and not echo-re-broadcast.
    if (this->m_applyingLinkedUpdate)
        this->m_linkedRemoteRequestIds.insert(requestId);
    ++this->activeRemoteSliceRequests;
    this->statusBar()->showMessage(u"Loading remote slice..."_s);
    this->startRemoteSliceFetch(sliceIndex, false, requestId);
}

void vtkWindowCube::setSlice2dPlaceholder(const QString &text)
{
    // One owner for the text in the middle of an empty 2-D pane: "Loading…"
    // while a fetch is out, the reason when one fails. An empty string hides it.
    if (!this->m_slice2dLoadingActor) {
        return;
    }
    if (text.isEmpty()) {
        this->m_slice2dLoadingActor->VisibilityOff();
    } else {
        this->m_slice2dLoadingActor->SetInput(text.toUtf8().constData());
        this->m_slice2dLoadingActor->VisibilityOn();
    }
    // sliceWin, not active2dWin(): the placeholder was added to the slice
    // renderer, and the "active" 2-D window may be the moment map.
    if (this->sliceWin) {
        this->sliceWin->Render();
    }
}

void vtkWindowCube::applyRemoteSliceResult(const RemoteCubeSliceResult &result)
{
    // Hide 2D loading placeholder on first real slice tile.
    this->m_slice2dLoadingActor->VisibilityOff();

    this->cacheRemoteSliceResult(result);
    // A map is the slice's BACKGROUND (setSliceBackdrop): the channel still
    // becomes the current one — slider, linked windows, 3-D cutting plane — but
    // its pixels wait in the cache until the background goes back to "Channel".
    const bool backdrop = !this->m_sliceBackdropProductId.isEmpty();
    vtkImageData *img = nullptr;
    if (!backdrop) {
        const int stride = std::max(1, result.stride);
        int dims[3] = { 0, 0, 0 };
        result.imageData->GetDimensions(dims);
        this->m_displayedSliceGrid = { result.fullWidth > 0 ? result.fullWidth : dims[0] * stride,
                                       result.fullHeight > 0 ? result.fullHeight : dims[1] * stride,
                                       stride };
        if (this->m_sliceDetailShown
            && (this->m_sliceDetailShown->index != result.index
                || this->m_sliceDetailShown->datasetId != this->remoteDatasetId))
            this->hideSliceDetail();
        this->remoteSliceDisplaySource->SetOutput(result.imageData);
        this->remoteSliceDisplaySource->Modified();
        img = vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
        if (img) {
            this->sliceColors->SetInputData(img);
        } else {
            qWarning() << "[vtk] Expected vtkImageData for remote slice colors but got null";
            return;
        }
    }
    // Through LutScale, not SetTableRange: on a log stretch the raw range of a
    // radio slice is negative at the low end, VTK rejects it and quietly swaps
    // in [1, 10].
    //
    // Under log the low end is recomputed from the new pixels rather than left
    // to LutScale's blind fallback. Handing it result.imageRange[0], which is
    // negative on any slice with noise in it, would floor at max x 1e-6 and undo
    // the sampled floor on the very next channel — the linear path already
    // follows the data slice by slice, so the log path does too. A slice with no
    // positive values keeps the floor already in force.
    if (backdrop || this->m_sliceRangeLocked) {
        // One colour scale for every channel (see m_sliceRangeLocked): the range
        // in force stays, so channel maps compare by colour. (Under a backdrop
        // the range is the map's.)
    } else if (this->m_sliceScaleMode == LutScale::Mode::Log) {
        const double floorValue = LutScale::sampledPositiveFloor(img);
        const double lo = (floorValue > 0.0) ? floorValue
                                             : this->lutSlice->GetTableRange()[0];
        LutScale::setRange(this->lutSlice, this->m_sliceScaleMode, lo,
                           std::max(result.imageRange[1], lo * (1.0 + 1e-6)));
    } else {
        LutScale::setRange(this->lutSlice, this->m_sliceScaleMode,
                           result.imageRange[0], result.imageRange[1]);
    }

    {
        const QSignalBlocker blockSlider(ui->sliderSlice);
        const QSignalBlocker blockSpin(ui->spinSlice);
        ui->sliderSlice->setValue(result.index + 1);
        ui->spinSlice->setValue(result.index + 1);
    }
    // The channel actually ON SCREEN. The spin box is written from pending
    // requests too, so anything that has to name the data it measured (region
    // provenance) must read this, not the control.
    this->m_displayedSliceIndex = result.index;
    this->m_sliceErrorOnStatusBar = false;   // a slice arrived; the error is stale now
    ui->lineSpectral->setText(this->formatSpectralAxisValue(result.index));
    // The floating channel scrubber is normally driven by spinSlice::valueChanged,
    // which we just blocked — update it explicitly so it tracks every displayed
    // slice (cached / fetched / linked).
    if (this->channelOverlay) {
        for (ChannelOverlay *co : this->channelOverlays()) {
            co->setChannel(result.index + 1);
            co->setSpectralLabel(ui->lineSpectral->text());
        }
    }

    // Linked views: this is the single convergence where a slice is actually
    // displayed (covers cached scrubbing AND fetched slices). Broadcast the
    // committed channel to linked peers, de-duped so an echoed/own channel is
    // not re-broadcast. E-07/M6: suppress the re-broadcast when this result came
    // from a linked-originated request — via the RAII guard (cached/sync path)
    // OR the captured token (async fetched path, where the guard has died) — so
    // a clamped/late index can't ping-pong across ≥3 linked windows.
    const bool linkedOriginated = this->m_applyingLinkedUpdate || this->m_applyingLinkedResult;
    if (this->m_linkViews && !linkedOriginated
        && result.index != this->m_lastLinkedChannel) {
        this->m_lastLinkedChannel = result.index;
        this->broadcastLinkedChannel(result.index);
    }
    this->refreshSpectralAxisUi();
    this->updateRemoteCuttingPlane(result.index);
    ui->vtkCube->renderWindow()->Render();
    qDebug().noquote() << QStringLiteral("[remote-plane] render triggered");
    if (backdrop) {
        // The "Channels" pane shows this channel, at the range the slice had (kept
        // when the user had locked one, else this channel's own).
        const bool locked = this->m_sliceRangeLockedBeforeBackdrop
                && this->m_sliceRangeBeforeBackdrop[1] > this->m_sliceRangeBeforeBackdrop[0];
        this->updateChannelMirror(result.imageData,
                                  locked ? this->m_sliceRangeBeforeBackdrop[0] : result.imageRange[0],
                                  locked ? this->m_sliceRangeBeforeBackdrop[1] : result.imageRange[1]);
        // The channel overlays (kinematic contour, lasso outline) follow the
        // channel even over a background map; the pixel work below does not apply.
        this->updateKinematic2dContour(/*render=*/false);
        this->updateLassoMaskOverlay(/*render=*/false);
        this->sliceWin->Render();
        return;
    }

    if (this->viewingSlice()) {
        ui->lineImgMin->setText(fmtStat(result.imageRange[0]));
        ui->lineImgMax->setText(fmtStat(result.imageRange[1]));
        double mean, rms;
        computeImageStats(result.imageData, mean, rms);
        ui->lineImgMean->setText(fmtStat(mean));
        this->updateSliceStatsCaption();
        ui->lineImgRms->setText(fmtStat(rms));
        this->updateLUTCustomizer();
    }
    // (a) the Inspector's 2-D block follows the active pane — a channel scrub lands here
    // (not updateInfoStatsPage), so refresh it so the active SLICE's stats stay current.
    this->updateActiveMapStats();

    auto *sliceRenderer = this->sliceWin->GetRenderers()->GetFirstRenderer();
    refitParallelSliceCamera(sliceRenderer, result.imageData, this->sliceWin);
    sliceRenderer->ResetCameraClippingRange();
    // A new channel refits the slice in code: not a move to send to the linked views. Recorded
    // before the letterbox below draws, since that draws the plain refit as it is when the
    // letterbox cannot run (issue #358).
    this->refitSliceCamera();
    this->layout2dPaneFor(/*slice=*/true, true);   // gutter-aware letterbox supersedes the plain refit
    if (this->probeModeActive && this->probeValid) {
        this->refreshProbeOverlay();
        this->updateProbeReadout(result.imageData);
    }
    // Refresh the kinematic-model contour + the Kinematic-Lasso selection outline
    // for the newly displayed channel (the following sliceWin->Render() paints them
    // — no extra render needed here).
    this->updateKinematic2dContour(/*render=*/false);
    this->updateLassoMaskOverlay(/*render=*/false);
    this->sliceWin->Render();
    this->scheduleSliceDetail();
    // Smart prefetch: schedule ±1 neighbour fetch ``idlePrefetchDelayMs``
    // from now, only when this slice was the user's explicit request
    // (prefetchAllowedForNextResult) — not a prefetched neighbour itself
    // (no cascade). A new requestRemoteSlice() will CANCEL this timer
    // (see requestRemoteSlice), so rapid scroll produces zero prefetches
    // and the SSH tunnel stays clear for the user-visible slice.
    if (this->prefetchAllowedForNextResult) {
        this->prefetchAllowedForNextResult = false;
        this->idlePrefetchTimer.start(idlePrefetchDelayMs);
    }
}

void vtkWindowCube::updateRemoteSliceDragFeedback(int sliceIndex)
{
    const int clampedSlice = this->clampRemoteSliceIndex(sliceIndex);
    this->currentRequestedRemoteSliceIndex = clampedSlice;
    ui->lineSpectral->setText(this->formatSpectralAxisValue(clampedSlice));
    this->updateRemoteCuttingPlane(clampedSlice);
    ui->vtkCube->renderWindow()->Render();
}

int vtkWindowCube::remoteSliceCount() const
{
    return std::max(1, this->remoteDatasetDepth);
}

int vtkWindowCube::clampRemoteSliceIndex(int sliceIndex) const
{
    return std::clamp(sliceIndex, 0, this->remoteSliceCount() - 1);
}

double vtkWindowCube::remoteSliceCoordinate(int sliceIndex) const
{
    const int clampedSlice = this->clampRemoteSliceIndex(sliceIndex);
    bool ok = false;
    const double axisValue = this->spectralAxisValue(static_cast<double>(clampedSlice), &ok);
    return ok ? axisValue : static_cast<double>(clampedSlice);
}

QString vtkWindowCube::remoteSliceCacheKey(int sliceIndex) const
{
    return QStringLiteral("%1|z|%2")
            .arg(this->remoteDatasetId)
            .arg(this->clampRemoteSliceIndex(sliceIndex));
}

void vtkWindowCube::cacheRemoteSliceResult(const RemoteCubeSliceResult &result)
{
    // Delegate to RemoteSliceCache (E-04 step 1): it applies the valid/imageData
    // guard, refreshes recency, and evicts past capacity — verbatim behaviour.
    this->remoteSliceCache.insert(this->remoteSliceCacheKey(result.index), result);
}

bool vtkWindowCube::tryApplyCachedRemoteSlice(int sliceIndex)
{
    const QString key = this->remoteSliceCacheKey(sliceIndex);
    const RemoteCubeSliceResult *cached = this->remoteSliceCache.find(key);
    if (!cached) {
        return false;
    }
    this->applyRemoteSliceResult(*cached);
    this->clearPersistentStatusMessage();
    return true;
}

void vtkWindowCube::startRemoteSliceFetch(int sliceIndex, bool isPrefetch, int requestId)
{
    sliceIndex = this->clampRemoteSliceIndex(sliceIndex);
    const QString key = this->remoteSliceCacheKey(sliceIndex);
    if (this->remoteSliceFetchesInFlight.contains(key)) {
        // Deduped against an in-flight fetch → no watcher will be created for
        // this requestId, so consume its linked token now (E-07/M6) to avoid a
        // slow leak of the QSet; the in-flight fetch's own result handles the
        // display + any linked broadcast.
        this->m_linkedRemoteRequestIds.remove(requestId);
        return;
    }

    auto *watcher = new QFutureWatcher<RemoteCubeSliceResult>(this);
    this->remoteSliceFetchesInFlight.insert(key, watcher);
    // Prefetches get a real id too: id 0 on every one of them meant an adopted
    // prefetch shared its token with all the others.
    if (requestId == 0)
        requestId = ++this->m_remoteSliceRequestIdSeq;
    watcher->setProperty("requestId", requestId);
    watcher->setProperty("sliceKey", key);
    watcher->setProperty("isPrefetch", isPrefetch);
    QObject::connect(watcher, &QFutureWatcher<RemoteCubeSliceResult>::finished, this,
                     [this, watcher]() {
                         const auto result = watcher->result();
                         const int requestId = watcher->property("requestId").toInt();
                         // Consume the per-request linked token here (any branch),
                         // so the set never leaks a dropped/stale request (E-07/M6).
                         const bool linkedResult = this->m_linkedRemoteRequestIds.remove(requestId);
                         const QString key = watcher->property("sliceKey").toString();
                         const bool isPrefetch = watcher->property("isPrefetch").toBool();
                         this->remoteSliceFetchesInFlight.remove(key);
                         if (!isPrefetch) {
                             --this->activeRemoteSliceRequests;
                         }
                         watcher->deleteLater();

                         if (!result.valid || !result.imageData) {
                             if (!isPrefetch && requestId == this->currentRemoteSliceRequestId) {
                                 this->persistentStatusActive = false;
                                 this->statusMessageClearTimer.stop();
                                 this->statusBar()->showMessage(result.errorMessage.isEmpty()
                                                                        ? u"Could not load remote slice."_s
                                                                        : result.errorMessage);
                                 // Hold it: a stale success finishing after this
                                 // one would otherwise clear the only thing that
                                 // told the user the slice failed.
                                 this->m_sliceErrorOnStatusBar = true;
                                 // …and say it in the pane itself. A status-bar
                                 // line is easy to miss next to a 2-D view that
                                 // goes on claiming it is still loading, which is
                                 // how a wedged backend read as a slow one.
                                 this->setSlice2dPlaceholder(
                                         u"2-D view could not be loaded.\n%1"_s.arg(
                                                 result.errorMessage.isEmpty()
                                                         ? u"The backend did not return the slice."_s
                                                         : result.errorMessage));
                             }
                             return;
                         }

                         this->cacheRemoteSliceResult(result);

                         const bool requestedSliceMatches = this->clampRemoteSliceIndex(result.index)
                                 == this->currentRequestedRemoteSliceIndex;
                         if (!isPrefetch && requestId != this->currentRemoteSliceRequestId) {
                             if (this->activeRemoteSliceRequests == 0
                                 && !this->m_sliceErrorOnStatusBar) {
                                 this->clearPersistentStatusMessage();
                             }
                             return;
                         }

                         if (requestedSliceMatches) {
                             // RAII: mark this synchronous apply as linked-driven
                             // (only for its own scope) so the echo re-broadcast
                             // is suppressed for an async linked result too.
                             QScopedValueRollback<bool> linkedGuard(
                                     this->m_applyingLinkedResult, linkedResult);
                             this->applyRemoteSliceResult(result);
                             if (!isPrefetch && this->activeRemoteSliceRequests == 0) {
                                 this->clearPersistentStatusMessage();
                             }
                         } else if (!isPrefetch && this->activeRemoteSliceRequests == 0) {
                             this->clearPersistentStatusMessage();
                         }
                     });
    watcher->setFuture(QtConcurrent::run(&fetchRemoteSlice, this->remoteBackendUrl,
                                         this->remoteDatasetId, sliceIndex, this->remoteSessionId,
                                         this->remoteBackendToken, std::optional<QRect>()));
}

void vtkWindowCube::prefetchNeighborRemoteSlices(int sliceIndex)
{
    for (const int neighbor : { sliceIndex - 1, sliceIndex + 1 }) {
        if (neighbor < 0 || neighbor >= this->remoteSliceCount()) {
            continue;
        }

        const QString key = this->remoteSliceCacheKey(neighbor);
        if (this->remoteSliceCache.contains(key) || this->remoteSliceFetchesInFlight.contains(key)) {
            continue;
        }

        this->startRemoteSliceFetch(neighbor, true);
    }
}

vtkWindowCube::ExactProbe vtkWindowCube::exactSliceProbeValue(vtkImageData *image)
{
    if (!image || image != this->remoteSliceDisplaySource->GetOutputDataObject(0)
        || !this->m_sliceBackdropProductId.isEmpty() || this->m_displayedSliceGrid[2] <= 1
        || !this->probeValid)
        return {};
    const int x = this->probeVoxel[0], y = this->probeVoxel[1], z = this->probeVoxel[2];
    if (z == this->m_displayedSliceIndex) {
        double origin[3];
        image->GetOrigin(origin);
        std::optional<visivo::SliceDetailWindow> detail;
        if (this->m_sliceDetailShown && this->m_sliceDetailShown->index == z
            && this->m_sliceDetailShown->datasetId == this->remoteDatasetId)
            detail = this->m_sliceDetailShown->window;
        const QPoint first(int(std::lround(origin[0])), int(std::lround(origin[1])));
        switch (visivo::probeValueSource(x, y, first, this->m_displayedSliceGrid[2], detail)) {
        case visivo::ProbeValueSource::Channel:
            return {};
        case visivo::ProbeValueSource::Detail:
            if (auto *fine = vtkImageData::SafeDownCast(this->m_sliceDetailColors->GetInput())) {
                int ext[6];
                fine->GetExtent(ext);
                const int i = ext[0] + (x - detail->region.left()) / detail->stride;
                const int j = ext[2] + (y - detail->region.top()) / detail->stride;
                if (i >= ext[0] && i <= ext[1] && j >= ext[2] && j <= ext[3])
                    return { ExactProbe::Ready, fine->GetScalarComponentAsFloat(i, j, 0, 0) };
            }
            break;
        case visivo::ProbeValueSource::Backend:
            break;
        }
    }
    const std::array<int, 3> voxel{ x, y, z };
    if (this->m_probeExact && this->m_probeExact->voxel == voxel
        && this->m_probeExact->datasetId == this->remoteDatasetId)
        return { this->m_probeExact->valid ? ExactProbe::Ready : ExactProbe::Failed,
                 this->m_probeExact->value };
    if (!(this->m_probeExactPending && this->m_probeExactPending->voxel == voxel
          && this->m_probeExactPending->datasetId == this->remoteDatasetId))
        this->m_probeExactTimer.start();
    return { ExactProbe::Loading, 0.f };
}

void vtkWindowCube::fetchExactProbeValue()
{
    if (!this->probeValid || this->remoteDatasetId.isEmpty())
        return;
    const std::array<int, 3> voxel{ this->probeVoxel[0], this->probeVoxel[1],
                                    this->probeVoxel[2] };
    if (this->m_probeExactPending && this->m_probeExactPending->voxel == voxel
        && this->m_probeExactPending->datasetId == this->remoteDatasetId)
        return;
    const int seq = ++this->m_probeExactSeq;
    this->m_probeExactPending = ExactProbeValue{ this->remoteDatasetId, voxel };
    auto *watcher = new QFutureWatcher<RemoteCubeSliceResult>(this);
    QObject::connect(watcher, &QFutureWatcher<RemoteCubeSliceResult>::finished, this,
                     [this, watcher, seq]() {
                         watcher->deleteLater();
                         if (seq != this->m_probeExactSeq || !this->m_probeExactPending)
                             return;
                         ExactProbeValue got = *this->m_probeExactPending;
                         this->m_probeExactPending.reset();
                         const RemoteCubeSliceResult result = watcher->result();
                         got.valid = result.valid && result.imageData && result.stride == 1;
                         if (got.valid) {
                             got.value = result.imageData->GetScalarComponentAsFloat(0, 0, 0, 0);
                             this->m_probeExact = got;
                         } else {
                             // Shown once as "n/a", not kept: a revisit asks again.
                             this->m_probeExact = got;
                             QTimer::singleShot(0, this, [this, voxel = got.voxel]() {
                                 if (this->m_probeExact && this->m_probeExact->voxel == voxel
                                     && !this->m_probeExact->valid)
                                     this->m_probeExact.reset();
                             });
                         }
                         if (this->viewingSlice() && this->m_sliceBackdropProductId.isEmpty()
                             && this->probeValid && got.datasetId == this->remoteDatasetId
                             && got.voxel[0] == this->probeVoxel[0]
                             && got.voxel[1] == this->probeVoxel[1]
                             && got.voxel[2] == this->probeVoxel[2])
                             this->updateProbeReadout(vtkImageData::SafeDownCast(
                                     this->remoteSliceDisplaySource->GetOutputDataObject(0)));
                     });
    watcher->setFuture(QtConcurrent::run(&fetchRemoteSlice, this->remoteBackendUrl,
                                         this->remoteDatasetId, voxel[2], this->remoteSessionId,
                                         this->remoteBackendToken,
                                         std::optional<QRect>(QRect(voxel[0], voxel[1], 1, 1))));
}

void vtkWindowCube::scheduleSliceDetail()
{
    if (this->m_sliceDetailSuspended)
        return;
    if (this->m_displayedSliceGrid[2] <= 1 && !this->m_sliceDetailShown)
        return;
    this->m_sliceDetailTimer.start();
}

void vtkWindowCube::hideSliceDetail()
{
    ++this->m_sliceDetailSeq;
    this->m_sliceDetailTimer.stop();
    this->m_sliceDetailPending.reset();
    this->m_sliceDetailShown.reset();
    this->m_sliceDetailActor->VisibilityOff();
}

void vtkWindowCube::refreshSliceDetail()
{
    auto *renderer = this->sliceWin ? this->sliceWin->GetRenderers()->GetFirstRenderer() : nullptr;
    const int stride = this->m_displayedSliceGrid[2];
    if (!renderer || this->m_sliceDetailSuspended || !this->m_sliceBackdropProductId.isEmpty()
        || this->m_displayedSliceIndex < 0 || this->remoteDatasetId.isEmpty() || stride <= 1) {
        if (this->m_sliceDetailShown || this->m_sliceDetailPending) {
            this->hideSliceDetail();
            if (this->sliceWin)
                this->sliceWin->Render();
        }
        return;
    }
    vtkNew<vtkCoordinate> corner;
    corner->SetCoordinateSystemToNormalizedViewport();
    corner->SetValue(0., 0., 0.);
    const double *lo = corner->GetComputedWorldValue(renderer);
    const double lx = lo[0], ly = lo[1];
    corner->SetValue(1., 1., 0.);
    const double *hi = corner->GetComputedWorldValue(renderer);
    const QRect visible = visivo::visiblePlanePixels(lx, hi[0], ly, hi[1],
                                                     this->m_displayedSliceGrid[0],
                                                     this->m_displayedSliceGrid[1]);
    const auto want = visivo::sliceDetailFor(visible, this->m_displayedSliceGrid[0],
                                             this->m_displayedSliceGrid[1], stride);
    if (!want) {
        if (this->m_sliceDetailShown || this->m_sliceDetailPending) {
            this->hideSliceDetail();
            this->sliceWin->Render();
        }
        return;
    }
    const auto sameChannel = [this](const std::optional<SliceDetailState> &s) {
        return s && s->index == this->m_displayedSliceIndex
                && s->datasetId == this->remoteDatasetId;
    };
    if (sameChannel(this->m_sliceDetailShown)
        && visivo::sliceDetailCovers(this->m_sliceDetailShown->window, visible, *want))
        return;
    if (sameChannel(this->m_sliceDetailPending)
        && visivo::sliceDetailCovers(this->m_sliceDetailPending->window, visible, *want))
        return;

    const int seq = ++this->m_sliceDetailSeq;
    this->m_sliceDetailPending = SliceDetailState{ this->remoteDatasetId,
                                                   this->m_displayedSliceIndex, *want };
    auto *watcher = new QFutureWatcher<RemoteCubeSliceResult>(this);
    QObject::connect(watcher, &QFutureWatcher<RemoteCubeSliceResult>::finished, this,
                     [this, watcher, seq]() {
                         watcher->deleteLater();
                         if (seq != this->m_sliceDetailSeq || !this->m_sliceDetailPending)
                             return;
                         const SliceDetailState asked = *this->m_sliceDetailPending;
                         this->m_sliceDetailPending.reset();
                         const RemoteCubeSliceResult result = watcher->result();
                         if (!result.valid || !result.imageData || !result.region.isValid()
                             || asked.index != this->m_displayedSliceIndex
                             || asked.datasetId != this->remoteDatasetId
                             || !this->m_sliceBackdropProductId.isEmpty()
                             || this->m_sliceDetailSuspended)
                             return;
                         this->m_sliceDetailColors->SetInputData(result.imageData);
                         this->m_sliceDetailActor->VisibilityOn();
                         this->m_sliceDetailShown = SliceDetailState{
                             asked.datasetId, asked.index,
                             visivo::SliceDetailWindow{
                                     QRect(result.region.topLeft(),
                                           asked.window.region.bottomRight()),
                                     result.stride } };
                         this->sliceWin->Render();
                     });
    watcher->setFuture(QtConcurrent::run(&fetchRemoteSlice, this->remoteBackendUrl,
                                         this->remoteDatasetId, this->m_displayedSliceIndex,
                                         this->remoteSessionId, this->remoteBackendToken,
                                         std::optional<QRect>(want->region)));
}
