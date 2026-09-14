#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeViewportGeometry.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "CatalogueTableModel.h"
#include "ColorMaps.h"
#include "CatalogueOverlayUtils.h"
#include "DiagnosticsWindow.h"
#include "ImageLayerController.h"
#include "ImageLayerImportService.h"
#include "ImageLayerLoadTask.h"
#include "LUTCustomizerDialog.h"
#include "LayerListModel.h"
#include "ProfileWidget.h"
#include "RemoteFileBrowserDialog.h"
#include "qcustomplot.h"
#include "SidebarPanel.h"
#include "theme/SegmentedToggle.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"
#include "ToolResultDialog.h"
#include "vtkInteractorStyleProfile.h"
#include "vtkLegendScaleActorWCS.h"
#include "wcs.h"

#include <vtkCamera.h>
#include <vtkAxisActor2D.h>
#include <vtkContourTriangulator.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageSlice.h>
#include <vtkImageStack.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleUser.h>
#include <vtkLineSource.h>
#include <vtkLookupTable.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkPolyData.h>
#include "vtkFITSReader.h"
#include <vtkContourFilter.h>
#include <vtkFlyingEdges2D.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDebug>
#include <QKeySequence>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QPlainTextEdit>
#include <QDialog>
#include <QSlider>
#include <QStringList>
#include <QToolBar>
#include <QToolButton>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include "ProductOrigin.h"
#include "CubeRegionGeom.h"

using namespace Qt::StringLiterals;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

// ── Dynspec ──
// Extracted verbatim from vtkWindowImage.cpp. Dynamic-spectrum tools: dedispersion / pulse profile / RFI mask.

void vtkWindowImage::setDynamicSpectrumMode(bool enabled)
{
    if (m_dynamicSpectrumMode == enabled) return;
    m_dynamicSpectrumMode = enabled;

    // Hide celestial-WCS specific tools and overlays. They make no sense for
    // a time × frequency waterfall: no beam, no Galactic / FK5 frame, no
    // polarimetry workflow, no catalogue overlay against sky coords.
    const auto setActionEnabled = [enabled](QAction *act) {
        if (!act) return;
        act->setEnabled(!enabled);
        act->setVisible(!enabled);
    };
    setActionEnabled(actionWcsSexagesimal);
    setActionEnabled(actionWcsDecimal);
    setActionEnabled(actionLoadCatalogueOverlay);
    setActionEnabled(actionCrossmatch);
    setActionEnabled(actionShowCatalogueOverlay);
    setActionEnabled(actionShowCatalogueLabels);
    setActionEnabled(actionClearCatalogueOverlay);
    setActionEnabled(m_actLoadStokesCompanions);
    setActionEnabled(m_actComputeStokesP);
    setActionEnabled(m_actComputeStokesPA);
    setActionEnabled(m_actComputeStokesFrac);
    setActionEnabled(m_actComputeStokesPdebiased);
    setActionEnabled(m_actComputeSpectralIndex);
    setActionEnabled(m_actComputeFaradayRM);
    setActionEnabled(m_actTogglePolVecs);
    // …and the one tool that only makes sense the OTHER way round: on a time x
    // frequency plane, collapsing time IS a spectrum.
    if (this->actionExtractSpectrum) {
        this->actionExtractSpectrum->setEnabled(enabled);
        this->actionExtractSpectrum->setToolTip(
                enabled ? u"Collapse the time axis into a spectrum, over the drawn region's "
                          u"time span if there is one."_s
                        : u"Spectrum extraction needs a spectral axis: a cube, or a dynamic "
                          u"spectrum."_s);
    }
    if (ui->actionGalactic)  ui->actionGalactic->setEnabled(!enabled);
    if (ui->actionFK5)       ui->actionFK5->setEnabled(!enabled);
    if (ui->actionEcliptic)  ui->actionEcliptic->setEnabled(!enabled);

    // Sidebar cards: hide celestial-only ones and reveal the Time-Series
    // Tools placeholder. Done outside the if/else so the inverse direction
    // (re-enabling celestial mode after a dynspec was loaded) also works.
    for (const auto &card : std::as_const(m_dynspecHiddenCards)) {
        if (card) card->setVisible(!enabled);
    }
    if (m_dynspecToolsCard) {
        m_dynspecToolsCard->setVisible(enabled);
    }

    if (enabled) {
        // Beam indicator is meaningless for beamformed data.
        m_beamMajorDeg = std::numeric_limits<double>::quiet_NaN();
        m_beamMinorDeg = std::numeric_limits<double>::quiet_NaN();
        m_beamPaDeg    = std::numeric_limits<double>::quiet_NaN();
        if (m_beamActor) m_beamActor->VisibilityOff();
        // Status-bar panels: replace sanity messaging with a stable note.
        if (this->sanityLabel) {
            this->sanityLabel->setText(u"Dynamic Spectrum"_s);
            this->sanityLabel->setToolTip(
                    u"This dataset is a beamformed / dynamic-spectrum product. "
                    u"Time × frequency axes; celestial-WCS tools are disabled."_s);
        }
    }

    if (ui->vtk && ui->vtk->renderWindow()) {
        this->invalidateWcsOverlayCache();
        this->requestWcsOverlayRender();
    }
}

void vtkWindowImage::runDynspecDedispersion()
{
    if (!m_dynamicSpectrumMode) return;
    auto *edit = this->findChild<QLineEdit *>(u"dynspecDmEdit"_s);
    if (!edit) return;
    const double dm = edit->text().toDouble();
    if (dm <= 0.0) {
        QMessageBox::information(this, u"Dedispersion"_s,
                u"Set a positive Dispersion Measure (pc cm⁻³) before computing."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Dedispersing at DM=%1 pc cm⁻³…").arg(dm), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecDedisperseResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecDedisperseResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Dedispersion"_s,
                    u"Dedispersion failed:\n\n%1"_s.arg(r.error));
            return;
        }
        QMessageBox::information(self, u"Dedispersion"_s,
                u"Dedispersed dynspec saved.\n\n"
                u"Filename: %1\nDM: %2 pc cm⁻³\nRef. freq.: %3 MHz\n"
                u"Max channel shift: %4 samples\n\n"
                u"Open the file in a new viewer or as a layer to inspect "
                u"the result."_s
                .arg(r.workspaceFilename)
                .arg(r.dmPcCm3, 0, 'f', 4)
                .arg(r.refFreqMhz, 0, 'f', 3)
                .arg(r.maxShiftSamples));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, dm]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.dedisperseDynspec(dsId, dm, /*refFreqMhz=*/0.0, QString());
    }));
}

void vtkWindowImage::runDynspecPulseProfile()
{
    if (!m_dynamicSpectrumMode) return;
    auto *pEdit = this->findChild<QLineEdit *>(u"dynspecPeriodEdit"_s);
    auto *dmEdit = this->findChild<QLineEdit *>(u"dynspecDmEdit"_s);
    if (!pEdit) return;
    const double period = pEdit->text().toDouble();
    const double dm = dmEdit ? dmEdit->text().toDouble() : 0.0;
    if (period <= 0.0) {
        QMessageBox::information(this, u"Pulse Profile"_s,
                u"Set a positive period (seconds) before folding."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Folding at P=%1 s (DM=%2)…")
                    .arg(period, 0, 'g', 6).arg(dm, 0, 'f', 3), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecPulseProfileResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecPulseProfileResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Pulse Profile"_s,
                    u"Pulse profile computation failed:\n\n%1"_s.arg(r.error));
            return;
        }

        // Floating QCustomPlot window with the folded profile
        auto *win = new QWidget(nullptr, Qt::Window);
        win->setAttribute(Qt::WA_DeleteOnClose);
        win->setWindowTitle(
                QStringLiteral("Pulse Profile, P=%1 s · DM=%2 · SNR=%3")
                        .arg(r.periodS, 0, 'g', 6)
                        .arg(r.dmPcCm3, 0, 'f', 3)
                        .arg(r.snrPeak, 0, 'f', 1));
        win->resize(640, 360);
        auto *layout = new QVBoxLayout(win);
        layout->setContentsMargins(4, 4, 4, 4);

        auto *info = new QLabel(QStringLiteral(
                "Period: %1 s    DM: %2 pc cm⁻³    Bins: %3    "
                "Peak: %4    σ_MAD: %5    SNR (peak/σ): %6")
                        .arg(r.periodS, 0, 'g', 6).arg(r.dmPcCm3, 0, 'f', 3)
                        .arg(r.nBins)
                        .arg(r.peak, 0, 'g', 5).arg(r.sigmaMad, 0, 'g', 5)
                        .arg(r.snrPeak, 0, 'f', 2), win);
        layout->addWidget(info);

        auto *plot = new QCustomPlot(win);
        layout->addWidget(plot);
        plot->addGraph();
        plot->graph(0)->setData(r.phase, r.profile);
        plot->graph(0)->setPen(QPen(QColor(70, 130, 180), 1.5));
        plot->graph(0)->setBrush(QBrush(QColor(70, 130, 180, 40)));
        plot->xAxis->setLabel(u"Pulse phase"_s);
        plot->yAxis->setLabel(u"Mean intensity"_s);
        if (!r.phase.isEmpty()) {
            plot->xAxis->setRange(0.0, 1.0);
            double minY = std::numeric_limits<double>::infinity();
            double maxY = -std::numeric_limits<double>::infinity();
            for (double v : r.profile) {
                if (std::isfinite(v)) {
                    minY = std::min(minY, v);
                    maxY = std::max(maxY, v);
                }
            }
            if (std::isfinite(minY) && std::isfinite(maxY) && minY < maxY) {
                const double pad = (maxY - minY) * 0.10;
                plot->yAxis->setRange(minY - pad, maxY + pad);
            }
        }
        plot->replot();
        win->show();
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, period, dm]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.pulseProfile(dsId, period, /*nBins=*/64, dm,
                              /*fLo*/ 0.0, /*fHi*/ 0.0);
    }));
}

void vtkWindowImage::runDynspecRfiMask()
{
    if (!m_dynamicSpectrumMode) return;
    auto *edit = this->findChild<QLineEdit *>(u"dynspecSigmaEdit"_s);
    if (!edit) return;
    const double sigma = edit->text().toDouble();
    if (sigma <= 0.0) {
        QMessageBox::information(this, u"RFI Mask"_s,
                u"Set a positive σ threshold before flagging."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Flagging RFI at %1 σ…").arg(sigma), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecRfiMaskResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecRfiMaskResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"RFI Mask"_s,
                    u"RFI mask computation failed:\n\n%1"_s.arg(r.error));
            return;
        }
        QMessageBox::information(self, u"RFI Mask"_s,
                u"RFI mask saved to Workspace Exports.\n\n"
                u"Filename: %1\n"
                u"Flagged fraction: %2 %\n"
                u"σ threshold: %3 (across %4 axis)\n\n"
                u"Open it as a layer over the source dynspec to highlight "
                u"the flagged pixels."_s
                .arg(r.workspaceFilename)
                .arg(r.flaggedFraction * 100.0, 0, 'f', 2)
                .arg(r.nSigma, 0, 'f', 1)
                .arg(r.axis));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, sigma]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.rfiMask(dsId, sigma, QStringLiteral("channel"), QString());
    }));
}

// ── Extract Spectrum (2-D) ───────────────────────────────────────────────────
// "Extract Spectrum" sat permanently disabled in this window with the tooltip
// "only available for cube views". That is true of a sky image - there is no
// third axis to run along - but NOT of a dynamic spectrum, which is exactly a
// time x frequency plane: collapsing the time axis IS a spectrum, and it is the
// first thing anyone looks at on beamformed data. So the action is enabled in
// dynspec mode and does that, over the drawn region's time span when there is
// one and the whole observation otherwise.
void vtkWindowImage::extractSpectrum2D()
{
    if (!m_dynamicSpectrumMode) {
        statusBar()->showMessage(
                u"Spectrum extraction needs a spectral axis: open a cube, or a dynamic "
                u"spectrum where one image axis is frequency."_s, 5000);
        return;
    }
    auto *imageData =
            this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!imageData)
        return;

    int ext[6];
    imageData->GetExtent(ext);
    // NAXIS1 = TIME, NAXIS2 = FREQUENCY (the beamformed convention this window
    // already assumes for its axis titles).
    int x0 = ext[0], x1 = ext[1], y0 = ext[2], y1 = ext[3];
    const bool fromRegion = this->regionValid && this->regionMode != RegionMode::None;
    if (fromRegion) {
        // The region's BOUNDING BOX, said so in the product: collapsing a circle
        // or a polygon per row would give each frequency a different number of
        // samples, which is a different measurement than the one the shape
        // suggests. The box comes from the shared helper, so a circle gives its
        // whole disc and not the quadrant its two corners span.
        auto b = (this->regionMode == RegionMode::Polygon
                  && !this->regionPolygonVertices.empty())
                ? CubeRegionGeom::polygonBounds(this->regionPolygonVertices)
                : (this->regionMode == RegionMode::Circle
                   || this->regionMode == RegionMode::Annulus)
                        ? CubeRegionGeom::circleBounds(this->regionAnchorVoxel,
                                                       this->regionCurrentVoxel)
                        : CubeRegionGeom::boxBounds(this->regionAnchorVoxel,
                                                    this->regionCurrentVoxel);
        if (this->regionMode == RegionMode::Polygon) {
            // Still being drawn: the cursor vertex is on screen but not yet in
            // the list, and the user can see it inside their selection.
            b[0] = std::min(b[0], this->regionCurrentVoxel[0]);
            b[1] = std::max(b[1], this->regionCurrentVoxel[0]);
            b[2] = std::min(b[2], this->regionCurrentVoxel[1]);
            b[3] = std::max(b[3], this->regionCurrentVoxel[1]);
        }
        x0 = std::clamp(b[0], ext[0], ext[1]);
        x1 = std::clamp(b[1], ext[0], ext[1]);
        y0 = std::clamp(b[2], ext[2], ext[3]);
        y1 = std::clamp(b[3], ext[2], ext[3]);
    }

    double frameScale[2] = { 1., 1. }, framePos[2] = { 0., 0. };
    const bool haveFrame = this->currentVoxelFrame(frameScale, framePos);

    QVector<double> freqAxis;
    QVector<double> meanValues;
    freqAxis.reserve(y1 - y0 + 1);
    meanValues.reserve(y1 - y0 + 1);
    int emptyRows = 0;
    for (int y = y0; y <= y1; ++y) {
        double sum = 0.0;
        int n = 0;
        for (int x = x0; x <= x1; ++x) {
            const double v = imageData->GetScalarComponentAsDouble(x, y, 0, 0);
            if (std::isfinite(v)) {
                sum += v;
                ++n;
            }
        }
        bool ok = false;
        // remoteVoxelToWcs speaks FULL-RESOLUTION indices. The displayed image
        // can be decimated or a viewport tile, so the row index has to go
        // through the display frame first or every channel is labelled with
        // another channel's frequency.
        const double sourceRow = haveFrame ? framePos[1] + y * frameScale[1] : y;
        const double freq = this->remoteVoxelToWcs(1, sourceRow, &ok);
        freqAxis.append(ok ? freq : sourceRow);
        if (n > 0) {
            meanValues.append(sum / n);
        } else {
            // A fully flagged channel is a gap, not a zero.
            meanValues.append(std::numeric_limits<double>::quiet_NaN());
            ++emptyRows;
        }
    }
    if (freqAxis.isEmpty() || emptyRows == freqAxis.size()) {
        statusBar()->showMessage(
                u"Extract Spectrum: every channel in that time range is blanked."_s, 5000);
        return;
    }

    // Say what was actually collapsed. Without a region this is the whole of the
    // image ON SCREEN, which is the whole observation only when the viewer holds
    // it all: a decimated or viewport-tiled display is a subset, and calling
    // that "the whole observation" would misdescribe the measurement.
    const auto sourceX = [&](int x) { return haveFrame ? framePos[0] + x * frameScale[0] : x; };
    const auto sourceY = [&](int y) { return haveFrame ? framePos[1] + y * frameScale[1] : y; };
    // "Whole observation" means the collapsed range really spans the dataset -
    // a level-0 tile union starting at sample 0 but stopping short of the end,
    // or a crop in frequency only, would pass an identity-frame check.
    const bool partialDisplay =
            m_lodFullWidth > 0 && m_lodFullHeight > 0
            ? (sourceX(x0) > 0.5 || sourceX(x1) < m_lodFullWidth - 1.5
               || sourceY(y0) > 0.5 || sourceY(y1) < m_lodFullHeight - 1.5
               || (haveFrame && frameScale[0] != 1.0))
            : (haveFrame && (frameScale[0] != 1.0 || framePos[0] != 0.0));
    const QString scope = fromRegion
            ? u"time samples %1..%2 (the region's bounding box)"_s
                      .arg(std::lround(sourceX(x0)) + 1)
                      .arg(std::lround(sourceX(x1)) + 1)
            : (partialDisplay
                       ? u"time samples %1..%2 of the data currently loaded"_s
                                 .arg(std::lround(sourceX(x0)) + 1)
                                 .arg(std::lround(sourceX(x1)) + 1)
                       : u"the whole observation"_s);
    const QString title = u"Spectrum, %1"_s.arg(scope);

    auto *plot = new ProfileWidget(this);
    plot->setAttribute(Qt::WA_DeleteOnClose);
    plot->setWindowTitle(u"Extracted Spectrum"_s);
    plot->setUsageMode(ProfileWidget::UsageMode::RegionStatic, title);
    plot->setupSpectrumPlot(this->remoteOverlayAxisTitle(1), u"Mean value"_s);
    plot->updateSpectrumPlot(freqAxis, meanValues, title, /*live=*/false);
    plot->show();

    QVariantMap params;
    params.insert(u"Collapsed over"_s, scope);
    params.insert(u"Channels"_s, QString::number(freqAxis.size()));
    if (emptyRows > 0)
        params.insert(u"Blanked channels"_s, QString::number(emptyRows));
    QVariantMap origin;
    if (fromRegion) {
        origin = visivo::encodeRegionOrigin(
                static_cast<int>(this->regionMode), this->regionAnchorVoxel,
                this->regionCurrentVoxel, this->regionPolygonVertices,
                this->regionAnnulusInnerRadius);
        double frameScale[2], framePos[2];
        if (this->currentVoxelFrame(frameScale, framePos))
            origin = visivo::withVoxelFrame(origin, frameScale[0], frameScale[1], framePos[0],
                                            framePos[1]);
        params.insert(u"Measured on"_s, visivo::describeOrigin(origin));
    }
    const QVariantMap record = ToolResultDialog::makeRecord(
            u"Extracted Spectrum"_s, title,
            { { u"Extraction"_s,
                { { u"Collapsed over"_s, scope },
                  { u"Channels"_s, QString::number(freqAxis.size()) },
                  { u"Blanked channels"_s, QString::number(emptyRows) } } } });

    const QString pid = this->scopedProductId(u"spectrum-%1"_s.arg(++m_extractedSpectrumCount));
    m_extractedSpectra.insert(pid, { freqAxis, meanValues });
    this->registerAnalysisProduct(u"spectrum"_s, u"Extracted spectrum"_s, params, pid, record,
                                  /*layerIndex=*/-1, origin);
}
