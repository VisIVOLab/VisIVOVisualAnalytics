#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeRegionGeom.h"
#include "CubeViewportGeometry.h"

#include "app/BackendClient.h"
#include "app/DatasetBeam.h"
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
#include "ProductOrigin.h"
#include "RegionFlux.h"
#include "RegionMeasure.h"
#include "RegionFluxUi.h"
#include "RegionFullRes.h"
#include "RegionImport.h"
#include "RegionListExport.h"
#include "RegionReadout.h"
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
#include <QScopeGuard>
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

using namespace Qt::StringLiterals;
using namespace CubeRegionGeom;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

// ── Region (ROI) interaction + statistics + CRTF/DS9 export ──────────────────
// Extracted verbatim from vtkWindowImage.cpp. ROI drawing (box/circle/polygon/
// annulus), overlay, pixel statistics, and region export.

namespace {
struct RegionStatistics
{
    long long totalCount{ 0 };
    long long validCount{ 0 };
    long long blankedCount{ 0 };
    long long zeroCount{ 0 };   ///< valid pixels exactly 0 (displayed only; a clipped moment map)
    double minValue{ 0. };
    double maxValue{ 0. };
    double mean{ 0. };
    double median{ 0. };
    double stddev{ 0. };
    double sum{ 0. };       ///< Sum of valid pixels (for Jy = sum / Ω_beam_px)
    double madSigma{ 0. };  ///< 1.4826 × MAD — robust σ estimate
    double rmsUnweighted{ 0. };   ///< √⟨x²⟩, not the standard deviation (issue #503)
    bool hasRmsUnweighted{ true };   ///< false from a backend that predates it
    bool hasPeak{ false };        ///< the first pixel holding the maximum, 0-based source pixels
    double peakX{ 0. }, peakY{ 0. };
    /// Full-resolution pixels each counted pixel stands for: 1, or the block
    /// area for the displayed pixels below full resolution (issue #401).
    double pixelsPerSample{ 1. };
    bool valid{ false };
};

QString regionModeLabel(vtkWindowImage::RegionMode mode)
{
    switch (mode) {
    case vtkWindowImage::RegionMode::Box:
        return u"Box"_s;
    case vtkWindowImage::RegionMode::Circle:
        return u"Circle"_s;
    case vtkWindowImage::RegionMode::Polygon:
        return u"Polygon"_s;
    case vtkWindowImage::RegionMode::Annulus:
        return u"Annulus"_s;
    case vtkWindowImage::RegionMode::Point:
        return u"Point"_s;
    case vtkWindowImage::RegionMode::Line:
        return u"Line"_s;
    case vtkWindowImage::RegionMode::None:
    default:
        return u"Region"_s;
    }
}

/// The displayed voxels to visit for `region`, clipped to what is loaded.
/// False when none are.
bool visitBounds(vtkImageData *imageData, const visivo::regions::Region &region,
                 const visivo::DisplayGrid &grid, int out[4])
{
    int extent[6];
    imageData->GetExtent(extent);
    const auto b = visivo::displayedVoxelBounds(region, grid);
    out[0] = std::max(extent[0], b[0]);
    out[1] = std::min(extent[1], b[1]);
    out[2] = std::max(extent[2], b[2]);
    out[3] = std::min(extent[3], b[3]);
    return out[1] >= out[0] && out[3] >= out[2];
}

/// Sum and mean only, in one pass, over at most a fixed budget of pixels.
///
/// The full statistics sort the values (median, MAD); doing that on every mouse
/// move costs a tenth of a second on a megapixel region. This is what the live
/// readout can afford — and when it has to sample, it says so, because a sampled
/// sum scaled back up is an estimate and must not be read as the flux.
struct RegionQuickStats
{
    double sum{ 0. };
    double mean{ 0. };
    long long validCount{ 0 };
    bool sampled{ false };
};

RegionQuickStats computeRegionQuickStats(vtkImageData *imageData,
                                         const visivo::regions::Region &region,
                                         const visivo::DisplayGrid &grid)
{
    RegionQuickStats quick;
    int b[4];
    if (!imageData || !visitBounds(imageData, region, grid, b))
        return quick;
    int extent[6];
    imageData->GetExtent(extent);

    const long long box = static_cast<long long>(b[1] - b[0] + 1) * (b[3] - b[2] + 1);
    const int stride = visivo::regionSampleStride(box);
    quick.sampled = stride > 1;

    double sum = 0.0;
    long long counted = 0;
    for (int y = b[2]; y <= b[3]; y += stride) {
        for (int x = b[0]; x <= b[1]; x += stride) {
            if (!visivo::displayedVoxelIn(region, grid, x, y))
                continue;
            const double value = imageData->GetScalarComponentAsDouble(x, y, extent[4], 0);
            if (!std::isfinite(value))
                continue;
            sum += value;
            ++counted;
        }
    }
    if (counted == 0)
        return quick;

    quick.mean = sum / static_cast<double>(counted);
    // Scale the sum back to the whole region: the mean is unbiased under
    // sampling, the sum is not.
    const double scale = static_cast<double>(stride) * static_cast<double>(stride);
    quick.sum = quick.sampled ? sum * scale : sum;
    quick.validCount = quick.sampled ? static_cast<long long>(counted * scale) : counted;
    return quick;
}

RegionStatistics computeRegionStatistics2D(vtkImageData *imageData,
                                           const visivo::regions::Region &region,
                                           const visivo::DisplayGrid &grid)
{
    RegionStatistics stats;
    int b[4];
    if (!imageData || !visitBounds(imageData, region, grid, b)) {
        return stats;
    }

    std::vector<double> values;
    double sum = 0.0;
    double sumSq = 0.0;
    stats.minValue = std::numeric_limits<double>::infinity();
    stats.maxValue = -std::numeric_limits<double>::infinity();

    for (int y = b[2]; y <= b[3]; ++y) {
        for (int x = b[0]; x <= b[1]; ++x) {
            if (!visivo::displayedVoxelIn(region, grid, x, y)) {
                continue;
            }

            ++stats.totalCount;
            const double value = imageData->GetScalarComponentAsDouble(x, y, 0, 0);
            if (!std::isfinite(value)) {
                ++stats.blankedCount;
                continue;
            }

            ++stats.validCount;
            if (value == 0.0)
                ++stats.zeroCount;
            if (!stats.hasPeak || value > stats.maxValue) {
                stats.hasPeak = true;
                stats.peakX = grid.sourceX(x);
                stats.peakY = grid.sourceY(y);
            }
            stats.minValue = std::min(stats.minValue, value);
            stats.maxValue = std::max(stats.maxValue, value);
            sum += value;
            sumSq += value * value;
            values.push_back(value);
        }
    }

    if (stats.validCount <= 0) {
        return stats;
    }

    stats.sum = sum;
    stats.rmsUnweighted = visivo::measure::rmsFromSumSq(sumSq, stats.validCount);
    stats.mean = sum / static_cast<double>(stats.validCount);
    if (grid.reduced())
        stats.pixelsPerSample = std::max(1.0, grid.spacingX) * std::max(1.0, grid.spacingY);
    stats.stddev = std::sqrt(std::max(0.0, sumSq / static_cast<double>(stats.validCount)
                                               - stats.mean * stats.mean));
    std::sort(values.begin(), values.end());
    const int mid = stats.validCount / 2;
    stats.median = (stats.validCount % 2 == 0) ? 0.5 * (values[mid - 1] + values[mid]) : values[mid];

    // MAD-based robust σ — much more reliable than std-dev for radio fields
    // where bright sources skew the variance.
    std::vector<double> absDev;
    absDev.reserve(values.size());
    for (double v : values) absDev.push_back(std::abs(v - stats.median));
    std::sort(absDev.begin(), absDev.end());
    const int amid = static_cast<int>(absDev.size()) / 2;
    const double mad = (absDev.size() % 2 == 0)
            ? 0.5 * (absDev[amid - 1] + absDev[amid])
            : absDev[amid];
    stats.madSigma = 1.4826 * mad;

    stats.valid = true;
    return stats;
}

/// What a free function needs of the window to give the flux rows.
struct FluxContext
{
    double bmajDeg{ 0.0 }, bminDeg{ 0.0 }, cdelt1Deg{ 0.0 }, cdelt2Deg{ 0.0 };
    QString bunit;
    QString unitNote;   ///< how the unit was inferred without BUNIT, or empty
    vtkImageData *image{ nullptr };   ///< the master layer, for the map noise
    visivo::DisplayGrid grid;
    std::optional<RegionStatistics> background;
    QString backgroundName;
};

/// The rows of the Region Analysis "Flux" section (issues #401, #416):
/// beam and beam area when there is a beam, the region's flux (or Σ W ΔΩ for
/// a map in K) with its error, where the error comes from, the HI mass at the
/// distance set, and the peak SNR; or the reason there is no flux.
QList<QPair<QString, QString>> regionFluxRows(const RegionStatistics &stats, const FluxContext &c)
{
    QList<QPair<QString, QString>> rows;
    const double omega = visivo::flux::beamAreaPixels(
            c.bmajDeg, c.bminDeg, c.cdelt1Deg, c.cdelt2Deg);
    const double pixelAreaDeg2 = std::abs(c.cdelt1Deg * c.cdelt2Deg);
    if (omega > 0.0) {
        rows.append({ u"Beam (BMAJ × BMIN)"_s,
                      visivo::beam::formatBeamSize(c.bmajDeg, c.bminDeg) });
        rows.append({ u"Beam area (pixels)"_s, QStringLiteral("%1").arg(omega, 0, 'f', 2) });
    }
    const auto appendSnr = [&rows, &stats]() {
        const double snrPeak = (stats.madSigma > 0.0) ? (stats.maxValue / stats.madSigma) : 0.0;
        rows.append({ u"Peak SNR (peak/σ_MAD)"_s, QStringLiteral("%1").arg(snrPeak, 0, 'f', 1) });
    };

    // What the sum of a region is follows the image's unit (issues #401,
    // #416): a flux in Jy for Jy/beam, Jy/pixel or MJy/sr, Σ W ΔΩ for a map in
    // K, nothing for a unit that is unknown. A frequency-integrated map stays
    // in Jy Hz: an exported map has no rest frequency.
    const QString bunit = c.bunit.trimmed();
    const visivo::flux::FluxUnit unit = visivo::flux::parseFluxUnit(bunit);
    const QString refusal = visivo::flux::fluxRefusal(unit, bunit, omega, pixelAreaDeg2);
    if (!refusal.isEmpty()) {
        rows.append({ u"Integrated flux"_s, refusal });
        appendSnr();
        return rows;
    }
    const bool temperature = unit.kind == visivo::flux::UnitKind::Temperature;
    const QString quantity = temperature ? (unit.velocity ? u"Σ W ΔΩ"_s : u"Σ T ΔΩ"_s)
                                         : u"Integrated flux"_s;
    const auto fmt = [](double v) { return QString::number(v, 'g', 5); };
    const QString shownUnit = bunit.isEmpty() ? QString() : u" "_s + bunit;
    visivo::flux::Noise noise;
    QString noiseFrom;
    bool clipped = false;
    bool approximate = stats.pixelsPerSample > 1.0;
    if (c.background && c.background->valid) {
        const RegionStatistics &b = *c.background;
        clipped = visivo::flux::looksClipped(long(b.validCount), long(b.zeroCount),
                                             long(b.blankedCount));
        approximate = approximate || b.pixelsPerSample > 1.0;
        noise.mean = b.mean;
        noise.sigma = b.validCount >= 2 ? b.stddev : std::numeric_limits<double>::quiet_NaN();
        noise.count = long(b.validCount);
        noise.subtract = true;
        noiseFrom = u"background %1 (%2 px, mean %3, σ %4%5), mean subtracted"_s
                            .arg(c.backgroundName)
                            .arg(b.validCount)
                            .arg(fmt(b.mean), fmt(b.stddev), shownUnit);
    } else if (!c.grid.reduced()) {
        // The image's own noise, from the full-resolution pixels loaded.
        std::vector<double> values;
        long zeros = 0, blanks = 0;
        if (vtkImageData *img = c.image) {
            int e[6];
            img->GetExtent(e);
            values.reserve(std::size_t(e[1] - e[0] + 1) * std::size_t(e[3] - e[2] + 1));
            for (int y = e[2]; y <= e[3]; ++y)
                for (int x = e[0]; x <= e[1]; ++x) {
                    const double v = img->GetScalarComponentAsDouble(x, y, e[4], 0);
                    if (!std::isfinite(v))
                        ++blanks;
                    else if (v == 0.0)
                        ++zeros;
                    values.push_back(v);
                }
        }
        clipped = visivo::flux::looksClipped(long(values.size()) - blanks, zeros, blanks);
        noise.sigma = visivo::flux::madSigma(values);
        noiseFrom = u"map noise (σ_MAD %1%2 over the pixels loaded), no background subtracted"_s
                            .arg(fmt(noise.sigma), shownUnit);
    } else {
        noiseFrom = u"none: mark a background region for an error (the map is shown "
                    u"below full resolution)"_s;
    }
    if (!c.backgroundName.isEmpty() && !noise.subtract)
        noiseFrom = u"background %1 not measured; "_s.arg(c.backgroundName) + noiseFrom;
    // Statistics of displayed pixels below full resolution: each stands
    // for a block of the file's, and their σ is a block's, not a pixel's.
    const visivo::flux::RegionFlux f =
            visivo::flux::regionFlux(stats.sum, long(stats.validCount), omega, unit, noise,
                                     stats.pixelsPerSample, pixelAreaDeg2);
    if (f.valid) {
        if (!std::isfinite(f.error) && (noise.subtract || !c.grid.reduced()))
            noiseFrom += u"; σ cannot be measured (zero, or fewer than two pixels): no error"_s;
        if (temperature && stats.validCount > 0) {
            const double meanW = (stats.sum / double(stats.validCount)
                                  - (noise.subtract ? noise.mean : 0.0))
                    * unit.scale;
            rows.append({ unit.velocity ? u"Mean W"_s : u"Mean T"_s,
                          u"%1 %2"_s.arg(meanW, 0, 'g', 5).arg(unit.label) });
        }
        rows.append({ quantity, visivo::flux::formatFlux(f) });
        rows.append({ u"Flux error from"_s, noiseFrom });
        QStringList notes;
        const QString caveat = visivo::flux::fluxCaveat(unit, omega > 0.0);
        if (!caveat.isEmpty())
            notes << caveat;
        if (!c.unitNote.isEmpty())
            notes << c.unitNote;
        if (approximate)
            notes << u"from the displayed pixels below full resolution, each a block of the "
                     u"file's: approximate"_s;
        if (unit.frequencyToHz > 0.0)
            notes << u"integrated over frequency: %1 (the image has no rest frequency)"_s.arg(
                    unit.label);
        if (!notes.isEmpty())
            rows.append({ u"Flux note"_s, notes.join(u"; "_s) });
        if (clipped)
            rows.append({ u"Flux warning"_s,
                          u"more than a fifth of the noise pixels are 0 or blank: the map "
                          u"looks masked or thresholded, the result is biased (faint emission "
                          u"below the threshold is lost) and the error too small"_s });
        QString lineNote;
        if (visivo::flux::hiMassApplies(unit, 0.0, &lineNote)) {
            const visivo::flux::TypedDistance d = visivo::flux::storedHiDistance();
            if (d.ok) {
                const double massError = std::isfinite(f.error)
                        ? d.hiMass(f.error)
                        : std::numeric_limits<double>::quiet_NaN();
                rows.append({ u"HI mass"_s,
                              u"%1 at %2; %3"_s.arg(visivo::flux::formatHiMass(d.hiMass(f.flux),
                                                                                massError),
                                                     d.describe(), lineNote) });
            } else {
                rows.append({ u"HI mass"_s,
                              u"set a distance in Tools ▸ Regions ▸ HI Mass Distance…"_s });
            }
        }
    }
    appendSnr();
    return rows;
}

}  // namespace

void vtkWindowImage::finishRegionInteraction()
{
    if (this->m_vlkbSelectActive) return;
    if (this->m_annotPlaceMode != AnnotPlaceMode::None) {
        this->cancelAnnotationPlacement();
        return;
    }

    if (this->isBusy() || this->regionMode == RegionMode::None || !this->regionDragging) {
        return;
    }

    if (this->regionMode == RegionMode::Polygon) {
        if (this->ignoreNextPolygonRelease) {
            this->ignoreNextPolygonRelease = false;
            return;
        }
        if (this->finalizePolygonRegion()) {
            ui->vtk->renderWindow()->Render();
        }
        return;
    }

    const int *position = ui->vtk->renderWindow()->GetInteractor()->GetEventPosition();
    if (position) {
        this->updateRegionFromDisplayPosition(position[0], position[1]);
    }
    if (this->regionMode == RegionMode::Annulus) {
        const double dx = static_cast<double>(this->regionCurrentVoxel[0] - this->regionAnchorVoxel[0]);
        const double dy = static_cast<double>(this->regionCurrentVoxel[1] - this->regionAnchorVoxel[1]);
        const double outerRadius = std::sqrt(dx * dx + dy * dy);
        // Live preview while the radius is typed. The inner ring is drawn as the
        // number changes, in the amber of the crop/mask preview so it reads as
        // "this is what you are setting", and takes the region's own colour once
        // accepted. A modal QInputDialog::getDouble gave no feedback at all: you
        // typed a radius blind and found out afterwards (user request).
        QInputDialog prompt(this);
        prompt.setWindowTitle(u"Annulus Inner Radius"_s);
        prompt.setLabelText(u"Inner radius (pixels), 0 to %1:"_s
                                    .arg(std::max(0.0, outerRadius), 0, 'f', 1));
        prompt.setInputMode(QInputDialog::DoubleInput);
        prompt.setDoubleRange(0.0, std::max(0.0, outerRadius));
        prompt.setDoubleDecimals(1);
        prompt.setDoubleValue(0.0);

        this->regionAnnulusInnerActor->GetProperty()->SetColor(0.95, 0.65, 0.15);
        const auto showInner = [this](double r) {
            this->regionAnnulusInnerRadius = r;
            this->regionDragging = false;   // the overlay hides the ring while dragging
            this->regionValid = true;
            this->refreshRegionOverlay();
            ui->vtk->renderWindow()->Render();
        };
        showInner(0.0);
        QObject::connect(&prompt, &QInputDialog::doubleValueChanged, this, showInner);

        const bool accepted = prompt.exec() == QDialog::Accepted;
        const double innerRadius = prompt.doubleValue();
        // Back to the region's own colour: amber meant "being chosen".
        this->regionAnnulusInnerActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
        if (!accepted) {
            this->regionAnnulusInnerRadius = 0.0;
            this->refreshRegionOverlay();
            ui->vtk->renderWindow()->Render();
            if (this->actionAnnulusRegion && this->actionAnnulusRegion->isChecked()) {
                this->actionAnnulusRegion->setChecked(false);
            }
            return;
        }
        if (innerRadius < 0.0 || innerRadius >= outerRadius) {
            this->statusBar()->showMessage(
                    u"Inner radius must be >= 0 and smaller than the outer radius: the annulus "
                    u"keeps no hole (Region Properties sets it)."_s, 5000);
            this->regionAnnulusInnerRadius = 0.0;
        } else {
            this->regionAnnulusInnerRadius = innerRadius;
        }
    }
    this->regionDragging = false;
    this->regionValid = true;
    // The finished shape joins the window's region list and is measured. The
    // tool stays armed: the next drag on empty image draws another region, a
    // press on a region selects it.
    const int id = this->commitDraftRegion();
    if (id > 0)
        this->analyzeRegion(id);
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::setRegionMode(RegionMode mode, bool active)
{
    // However this function returns, the strip ends up showing the state it
    // left behind — including the early returns.
    const QScopeGuard syncStrip([this]() { this->syncModeStrip(); });
    if (!active) {
        if (this->regionMode == mode) {
            this->regionMode = RegionMode::None;
            this->regionDragging = false;
            this->clearRegion();
            this->setInteractorStyleImage();
            ui->vtk->setCursor(this->probeModeActive ? Qt::CrossCursor : Qt::ArrowCursor);
            if (ui->vtk->renderWindow()) {
                ui->vtk->renderWindow()->Render();
            }
        }
        return;
    }

    if (this->probeModeActive && ui->actionProfile->isChecked()) {
        ui->actionProfile->setChecked(false);
    }
    // A measurement outranks a region when a click is dispatched (see
    // ViewerModeState.h), and setMeasurementMode() disarms any region for
    // exactly that reason — but the reverse was missing. Arming a region from
    // the Tools ▸ Regions menu while the ruler was on left the ruler consuming
    // every drag: the menu entry showed as checked, the strip still said Ruler,
    // and dragging drew a distance instead of measuring a region. Unchecking
    // happens here, BEFORE regionMode is assigned, so that the measurement
    // setter's own region cleanup has nothing to undo.
    // An annotation waiting to be placed outranks both (ViewerModeState.h again)
    // — the next click would put the text down instead of starting the region.
    // The mode strip cancels it; the menu path did not.
    if (this->m_annotPlaceMode != AnnotPlaceMode::None)
        this->cancelAnnotationPlacement();
    if (this->m_measMode != MeasurementMode::None) {
        QAction *measurements[] = { this->m_actRuler, this->m_actAngle };
        for (auto *action : measurements) {
            if (action && action->isChecked())
                action->setChecked(false);
        }
        if (this->m_measMode != MeasurementMode::None)
            this->setMeasurementMode(MeasurementMode::None);
    }
    if (mode == RegionMode::Box && this->actionCircleRegion && this->actionCircleRegion->isChecked()) {
        this->actionCircleRegion->setChecked(false);
    }
    if (mode == RegionMode::Circle && this->actionBoxRegion && this->actionBoxRegion->isChecked()) {
        this->actionBoxRegion->setChecked(false);
    }
    if (mode != RegionMode::Polygon && this->actionPolygonRegion && this->actionPolygonRegion->isChecked()) {
        this->actionPolygonRegion->setChecked(false);
    }
    if (mode != RegionMode::Annulus && this->actionAnnulusRegion && this->actionAnnulusRegion->isChecked()) {
        this->actionAnnulusRegion->setChecked(false);
    }
    if (mode != RegionMode::Point && this->actionPointRegion && this->actionPointRegion->isChecked()) {
        this->actionPointRegion->setChecked(false);
    }
    if (mode != RegionMode::Line && this->actionLineRegion && this->actionLineRegion->isChecked()) {
        this->actionLineRegion->setChecked(false);
    }
    this->regionMode = mode;
    this->regionDragging = false;
    this->regionValid = false;
    this->ignoreNextPolygonRelease = false;
    this->regionPolygonVertices.clear();
    this->regionAnnulusInnerRadius = 0.0;
    this->clearRegion();
    this->setInteractorStyleRegion();
    ui->vtk->setCursor(Qt::CrossCursor);
}

bool vtkWindowImage::updateRegionFromDisplayPosition(int displayX, int displayY)
{
    auto *renderer = ui->vtk->renderWindow() ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                                             : nullptr;
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!renderer || !imageData) {
        return false;
    }

    this->coordinate->SetValue(displayX, displayY);
    const double *worldCoord = this->coordinate->GetComputedWorldValue(renderer);
    if (!worldCoord) {
        return false;
    }

    int extent[6];
    imageData->GetExtent(extent);
    double origin[3];
    double spacing[3];
    imageData->GetOrigin(origin);
    imageData->GetSpacing(spacing);
    const int voxelX = std::lround(extent[0] + (worldCoord[0] - origin[0]) / spacing[0]);
    const int voxelY = std::lround(extent[2] + (worldCoord[1] - origin[1]) / spacing[1]);
    if (voxelX < extent[0] || voxelX > extent[1] || voxelY < extent[2] || voxelY > extent[3]) {
        return false;
    }

    this->regionCurrentVoxel = { voxelX, voxelY };
    // The renderer's world is source pixels: kept unrounded for a point or a line.
    this->regionCurrentWorld = { worldCoord[0], worldCoord[1] };
    this->refreshRegionOverlay();
    return true;
}

void vtkWindowImage::updateRegionReadout()
{
    if (!ui->vtk)
        return;
    auto *imageData =
            this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    // The region being drawn, else the selected one of the list: the numbers
    // follow whichever shape the user is working on, while it is dragged.
    std::optional<visivo::regions::Region> shown;
    QString shapeLabel;
    if (imageData && this->regionMode != RegionMode::None && this->regionValid) {
        shown = this->draftRegion();
        shapeLabel = regionModeLabel(this->regionMode);
    } else if (imageData && this->m_regions.selected()) {
        shown = *this->m_regions.selected();
        shapeLabel = shown->name;
    }
    // A line has no pixels to sum: its numbers are the profile, on release.
    if (!shown || !visivo::regions::hasArea(shown->shape)) {
        this->hideRegionReadout();
        return;
    }

    if (!m_regionReadout) {
        // A child of the render widget, so it floats over the image and moves
        // with it; the VTK surface has no layout to put it in.
        m_regionReadout = new QLabel(ui->vtk);
        m_regionReadout->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_regionReadout->setStyleSheet(
                QStringLiteral("QLabel { background:%3; color:%1;"
                               " border:1px solid %2; border-radius:6px;"
                               " padding:5px 10px;"
                               " font-family:'JetBrains Mono','SF Mono',monospace;"
                               " font-size:10px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface()),
                             QLatin1StringView(VisivoTheme::kOutline()),
                             QLatin1StringView(VisivoTheme::kSurfaceContainer())));
    }

    auto quick = computeRegionQuickStats(imageData, *shown, this->displayGrid());
    // Below full resolution a displayed pixel stands for a block: the sum is
    // scaled by its area and marked as an estimate, and the readout says so;
    // the statistics on release come from the file (issue #95).
    double spacing[3];
    imageData->GetSpacing(spacing);
    if (spacing[0] > 1.001) {
        const int block = static_cast<int>(std::lround(spacing[0]));
        quick.sum *= spacing[0] * spacing[1];
        quick.validCount = static_cast<long long>(quick.validCount * spacing[0] * spacing[1]);
        quick.sampled = true;
        shapeLabel += this->m_displayAveraged
                ? u" (overview, %1\u00d7%1 means)"_s.arg(block)
                : u" (overview, 1 in %1\u00d7%1)"_s.arg(block);
    }
    QString text = visivo::regionReadoutText(shapeLabel, quick.sum, quick.mean,
                                             quick.validCount, quick.sampled);
    if (m_regionStatsPending)
        text += u"\nComputing the statistics on the full-resolution data…"_s;
    m_regionReadout->setText(text);
    // Never wider than the viewport it floats in: a long shape name plus two
    // figures on a narrow window would run out past the right edge, where there
    // is nothing to clip it.
    m_regionReadout->setMaximumWidth(std::max(120, ui->vtk->width() - 24));
    m_regionReadout->adjustSize();
    // Bottom-left of the viewport, clear of the colorbar on the right — and
    // above the WCS axis strip VTK draws along the bottom edge, which a 12 px
    // margin lands squarely on top of (seen by running it: the box covered the
    // "-0.50" tick label).
    constexpr int kAxisStripHeight = 44;
    m_regionReadout->move(12,
                          std::max(0, ui->vtk->height() - m_regionReadout->height()
                                              - kAxisStripHeight));
    m_regionReadout->show();
    m_regionReadout->raise();
}

void vtkWindowImage::hideRegionReadout()
{
    if (m_regionReadout)
        m_regionReadout->hide();
}

void vtkWindowImage::refreshRegionOverlay()
{
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!imageData || this->regionMode == RegionMode::None || !this->regionValid) {
        this->clearRegion();   // the draft; the readout falls back to the selected region
        return;
    }
    // The numbers follow the shape: same call site, so they cannot show a
    // region that is no longer the one on screen.
    this->updateRegionReadout();

    // Region geometry (anchor/current voxels, polygon vertices, annulus radii) is expressed in
    // the *displayed* image's voxel index space. On a decimated LOD image that voxel space is
    // coarser than full-res world coordinates (spacing ~= 2), so drawing the overlay directly in
    // voxel coordinates would place it at half position / half size. Map the whole region overlay
    // from voxel space to world space with a per-actor transform instead of converting every
    // primitive (box lines, circle/annulus sources, triangulated fills, polygon). At spacing=1,
    // origin=0, extent0=0 this is the identity, so full-res images are unaffected.
    double ro[3], rs[3];
    int rext[6];
    imageData->GetOrigin(ro);
    imageData->GetSpacing(rs);
    imageData->GetExtent(rext);
    const double regionPosX = ro[0] - rext[0] * rs[0];
    const double regionPosY = ro[1] - rext[2] * rs[1];
    for (vtkActor *a : { this->regionTopActor.Get(), this->regionBottomActor.Get(),
                         this->regionLeftActor.Get(), this->regionRightActor.Get(),
                         this->regionCircleActor.Get(), this->regionAnnulusOuterActor.Get(),
                         this->regionAnnulusInnerActor.Get(), this->regionAnnulusFillActor.Get(),
                         this->regionPolygonActor.Get(), this->regionPolygonFillActor.Get() }) {
        a->SetScale(rs[0], rs[1], 1.0);
        a->SetPosition(regionPosX, regionPosY, 0.0);
    }

    const int xmin = std::min(this->regionAnchorVoxel[0], this->regionCurrentVoxel[0]);
    const int xmax = std::max(this->regionAnchorVoxel[0], this->regionCurrentVoxel[0]);
    const int ymin = std::min(this->regionAnchorVoxel[1], this->regionCurrentVoxel[1]);
    const int ymax = std::max(this->regionAnchorVoxel[1], this->regionCurrentVoxel[1]);

    const bool showBox = this->regionMode == RegionMode::Box;
    const bool showCircle = this->regionMode == RegionMode::Circle;
    const bool showAnnulus = this->regionMode == RegionMode::Annulus;
    const bool showPolygon = this->regionMode == RegionMode::Polygon;
    const bool showLine = this->regionMode == RegionMode::Line;
    // The line's preview is one edge of the box's actors: no new actor for a
    // segment that lives only while it is dragged.
    this->regionTopActor->SetVisibility(showBox || showLine);
    this->regionBottomActor->SetVisibility(showBox);
    this->regionLeftActor->SetVisibility(showBox);
    this->regionRightActor->SetVisibility(showBox);
    this->regionCircleActor->SetVisibility(showCircle);
    this->regionAnnulusOuterActor->SetVisibility(showAnnulus);
    this->regionAnnulusInnerActor->SetVisibility(showAnnulus && !this->regionDragging);
    this->regionAnnulusFillActor->SetVisibility(showAnnulus && !this->regionDragging);
    this->regionPolygonActor->SetVisibility(showPolygon);
    this->regionPolygonFillActor->SetVisibility(showPolygon);

    if (showLine) {
        this->regionTopLine->SetPoint1(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
        this->regionTopLine->SetPoint2(this->regionCurrentVoxel[0], this->regionCurrentVoxel[1], 0.0);
    } else if (showBox) {
        this->regionTopLine->SetPoint1(xmin, ymax, 0.0);
        this->regionTopLine->SetPoint2(xmax, ymax, 0.0);
        this->regionBottomLine->SetPoint1(xmin, ymin, 0.0);
        this->regionBottomLine->SetPoint2(xmax, ymin, 0.0);
        this->regionLeftLine->SetPoint1(xmin, ymin, 0.0);
        this->regionLeftLine->SetPoint2(xmin, ymax, 0.0);
        this->regionRightLine->SetPoint1(xmax, ymin, 0.0);
        this->regionRightLine->SetPoint2(xmax, ymax, 0.0);
    } else if (showCircle || showAnnulus) {
        const double dx = static_cast<double>(this->regionCurrentVoxel[0] - this->regionAnchorVoxel[0]);
        const double dy = static_cast<double>(this->regionCurrentVoxel[1] - this->regionAnchorVoxel[1]);
        const double outerRadius = std::sqrt(dx * dx + dy * dy);
        if (showCircle) {
            this->regionCircleSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
            this->regionCircleSource->SetRadius(outerRadius);
        } else {
            this->regionAnnulusOuterSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
            this->regionAnnulusOuterSource->SetRadius(outerRadius);
            if (!this->regionDragging) {
                this->regionAnnulusInnerSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
                const double innerRadius = std::max(0.0, std::min(this->regionAnnulusInnerRadius, outerRadius));
                this->regionAnnulusInnerSource->SetRadius(innerRadius);
                buildAnnulusFill(this->regionAnnulusFillPoints, this->regionAnnulusFillCells,
                                 this->regionAnnulusFillData, this->regionAnchorVoxel, innerRadius,
                                 outerRadius, 96);
                this->regionAnnulusFillActor->SetVisibility(innerRadius < outerRadius);
            } else {
                this->regionAnnulusFillData->Initialize();
                this->regionAnnulusInnerActor->SetVisibility(false);
                this->regionAnnulusFillActor->SetVisibility(false);
            }
        }
    } else if (showPolygon) {
        std::vector<std::array<int, 2>> drawVertices = this->regionPolygonVertices;
        if (this->regionDragging && (drawVertices.empty() || drawVertices.back() != this->regionCurrentVoxel)) {
            drawVertices.push_back(this->regionCurrentVoxel);
        }
        const bool previewClosure = this->regionDragging && drawVertices.size() >= 3
                && distance2d(this->regionCurrentVoxel, drawVertices.front()) <= polygonClosureTolerance;
        this->regionPolygonPoints->Reset();
        this->regionPolygonCells->Reset();
        this->regionPolygonFillData->Initialize();
        if (drawVertices.size() >= 2) {
            const bool closed = !this->regionDragging || previewClosure;
            const vtkIdType count = static_cast<vtkIdType>(drawVertices.size() + (closed ? 1 : 0));
            this->regionPolygonCells->InsertNextCell(count);
            for (vtkIdType i = 0; i < static_cast<vtkIdType>(drawVertices.size()); ++i) {
                this->regionPolygonPoints->InsertNextPoint(drawVertices[static_cast<std::size_t>(i)][0],
                                                           drawVertices[static_cast<std::size_t>(i)][1], 0.0);
                this->regionPolygonCells->InsertCellPoint(i);
            }
            if (closed) {
                this->regionPolygonPoints->InsertNextPoint(drawVertices.front()[0], drawVertices.front()[1], 0.0);
                this->regionPolygonCells->InsertCellPoint(static_cast<vtkIdType>(drawVertices.size()));
            }
            this->regionPolygonData->SetPoints(this->regionPolygonPoints);
            this->regionPolygonData->SetLines(this->regionPolygonCells);
            this->regionPolygonData->Modified();
            if (drawVertices.size() >= 3 && closed) {
                this->regionPolygonFillData->SetPoints(this->regionPolygonPoints);
                this->regionPolygonFillData->SetLines(this->regionPolygonCells);
                this->regionPolygonFillData->Modified();
                this->regionPolygonTriangulator->Update();
                this->regionPolygonFillActor->SetVisibility(true);
            } else {
                this->regionPolygonFillActor->SetVisibility(false);
            }
        } else {
            this->regionPolygonFillActor->SetVisibility(false);
        }
    } else {
        this->regionAnnulusFillActor->SetVisibility(false);
        this->regionPolygonFillActor->SetVisibility(false);
    }
}

void vtkWindowImage::clearRegion()
{
    // Clears the region being DRAWN (and its actors). The regions of the list
    // are not touched: they stay until the user deletes them.
    this->regionValid = false;
    this->regionDragging = false;
    this->ignoreNextPolygonRelease = false;
    this->regionPolygonVertices.clear();
    this->regionAnnulusInnerRadius = 0.0;
    this->regionTopActor->VisibilityOff();
    this->regionBottomActor->VisibilityOff();
    this->regionLeftActor->VisibilityOff();
    this->regionRightActor->VisibilityOff();
    this->regionCircleActor->VisibilityOff();
    this->regionAnnulusOuterActor->VisibilityOff();
    this->regionAnnulusInnerActor->VisibilityOff();
    this->regionAnnulusFillActor->VisibilityOff();
    this->regionPolygonActor->VisibilityOff();
    this->regionPolygonFillActor->VisibilityOff();
    // No draft any more: the readout shows the selected region, or nothing.
    this->updateRegionReadout();
}

void vtkWindowImage::importRegionsFromFile(const QString &path, const QString &note)
{
    QPointer<vtkWindowImage> self(this);
    visivo::regions::importRegionFile(
            this,
            { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
              this->remoteDatasetId, this->filepath },
            [self](const visivo::regions::ImportResult &result) {
                if (self)
                    self->applyImportedRegions(result);
            },
            path, note);
}

void vtkWindowImage::applyImportedRegions(const visivo::regions::ImportResult &result)
{
    // A drag still running ends here: the import is its own change, after
    // it, not inside its undo step.
    if (this->m_regionDrag.active) {
        this->m_regionDrag.active = false;
        this->m_regions.endGesture();
    }
    // One change for the whole file (one Undo takes it back); the first region
    // is selected.
    if (!this->m_regions.addAll(result.regions, result.fileName).empty())
        this->onRegionListChanged();
}

void vtkWindowImage::exportRegionsToFile()
{
    visivo::regions::exportRegionList(
            this, this->m_regions,
            { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
              this->remoteDatasetId, this->filepath });
}

void vtkWindowImage::analyzeRegion(int id)
{
    if (id < 0)
        id = this->m_regions.selectedId();
    const visivo::regions::Region *found = this->m_regions.find(id);
    if (!found || !this->layers) {
        return;
    }
    // A copy: the list may change while the backend computes.
    const visivo::regions::Region region = *found;
    if (region.shape == visivo::regions::Shape::Line) {
        this->analyzeLineRegion(region);
        return;
    }
    if (!visivo::regions::hasArea(region.shape)) {
        this->statusBar()->showMessage(
                u"%1 is a %2: it has no pixels to measure."_s.arg(region.name,
                                                                    visivo::regions::shapeName(region.shape)),
                5000);
        return;
    }

    // Close any previous result dialog (its stale generation prevents cleanup races)
    if (this->m_regionResultDialog) {
        this->m_regionResultDialog->close();
    }

    auto *imageData = this->layers->getImageData(this->layers->getMasterIndex());
    // What a displayed pixel is (issue #95): at full resolution the displayed
    // pixels are the file's and their statistics are exact; below it they are
    // block means or one pixel per block, and the statistics come from the
    // file, computed by the backend.
    const visivo::DisplayGrid grid = this->displayGrid();
    const auto displayed = computeRegionStatistics2D(imageData, region, grid);
    const int requestGeneration = ++this->m_regionDialogGeneration;
    this->m_regionStatsPending = false;   // an earlier request is superseded

    const int block = static_cast<int>(std::lround(grid.spacingX));
    const QString displayedPixels = this->m_displayAveraged
            ? u"the displayed pixels, each the mean of %1\u00d7%1"_s.arg(block)
            : u"the displayed pixels, 1 in %1\u00d7%1 sampled"_s.arg(block);

    // The background region for the flux (issue #401): the nearest one marked
    // so, measured on the same pixels as the region.
    std::optional<visivo::regions::Region> backgroundRegion;
    if (const auto *bg = visivo::flux::nearestBackground(this->m_regions.regions(), region))
        backgroundRegion = *bg;
    const QString backgroundName = backgroundRegion ? backgroundRegion->name : QString();

    auto present = [this, region, grid, backgroundName](const RegionStatistics &stats,
                                                         const QString &scope,
                                                         const std::optional<RegionStatistics> &background) {
        auto *imageData = this->layers->getImageData(this->layers->getMasterIndex());

        // Built as a RECORD, then shown and registered from that single description
        // (ToolResultDialog::makeRecord). Registering it is what makes the result
        // outlive this window: the statistics used to exist only inside a dialog
        // with WA_DeleteOnClose, so closing it meant drawing the region again.
        QList<QPair<QString, QList<QPair<QString, QString>>>> sections;
        const QString subtitle =
                QStringLiteral("%1  ·  computed on %2").arg(region.name, scope);

        if (!stats.valid && region.shape == visivo::regions::Shape::Point) {
            sections.append({ u"Point"_s,
                              visivo::measure::pointRows(
                                      0.0, region.center[0], region.center[1],
                                      this->worldLabelAtPixel(region.center[0], region.center[1]),
                                      this->remoteDatasetBunit, true) });
        } else if (!stats.valid) {
            sections.append({ u"Statistics"_s,
                            { { u"Result"_s, u"No valid pixels in the selected region."_s },
                              { u"Note"_s,   u"Blanked / NaN pixels are ignored."_s } } });
        } else {
            // The numbers of the region, named so that the RMS (√⟨x²⟩) and the
            // standard deviation cannot be taken for each other (issue #503).
            visivo::measure::Numbers numbers;
            numbers.validCount = stats.validCount;
            numbers.totalCount = stats.totalCount;
            numbers.blankedCount = stats.blankedCount;
            numbers.minValue = stats.minValue;
            numbers.maxValue = stats.maxValue;
            numbers.sum = stats.sum;
            numbers.mean = stats.mean;
            numbers.median = stats.median;
            numbers.stddev = stats.stddev;
            numbers.rmsUnweighted = stats.rmsUnweighted;
            numbers.hasRmsUnweighted = stats.hasRmsUnweighted;
            numbers.madSigma = stats.madSigma;
            numbers.hasPeak = stats.hasPeak;
            numbers.peakX = stats.peakX;
            numbers.peakY = stats.peakY;
            const QString unit = this->remoteDatasetBunit;
            if (region.shape == visivo::regions::Shape::Point) {
                // One pixel: its value and where it is, not a table of statistics.
                sections.clear();
                sections.append({ u"Point"_s,
                                  visivo::measure::pointRows(
                                          stats.mean, stats.peakX, stats.peakY,
                                          this->worldLabelAtPixel(stats.peakX, stats.peakY), unit,
                                          false) });
            } else {
                sections.append({ u"Statistics"_s, visivo::measure::statisticsRows(numbers, unit) });
                // The peak of an unresolved source is its flux density.
                sections.append({ u"Peak"_s,
                                  visivo::measure::peakRows(
                                          numbers, unit,
                                          stats.hasPeak ? this->worldLabelAtPixel(stats.peakX, stats.peakY)
                                                        : QString()) });
            }

            // ── Flux, in whatever the image's unit allows ────────────────────
            // Not gated on a beam any more: a Jy/pixel, MJy/sr or K map has a
            // measure without one, and a Jy/beam map without one says why not.
            // A dynamic spectrum's axes are time and frequency: no sky area.
            if (!this->m_dynamicSpectrumMode && region.shape != visivo::regions::Shape::Point) {
                FluxContext flux;
                if (this->hasBeamInfo()) {
                    flux.bmajDeg = this->m_beamMajorDeg;
                    flux.bminDeg = this->m_beamMinorDeg;
                }
                flux.cdelt1Deg = this->remoteDatasetCdelt[0];
                flux.cdelt2Deg = this->remoteDatasetCdelt[1];
                flux.bunit = this->remoteDatasetBunit;
                flux.unitNote = this->m_intensityUnitNote;
                flux.image = imageData;
                flux.grid = grid;
                flux.background = background;
                flux.backgroundName = backgroundName;
                const auto radio = regionFluxRows(stats, flux);
                if (!radio.isEmpty())
                    sections.append({ u"Flux"_s, radio });
            }

            // ── Per-Stokes flux when a Stokes set is loaded ──────────────────
            if (!m_stokesRoles.isEmpty()) {
                QList<QPair<QString, QString>> rows;
                const StokesRole order[] = {
                    StokesRole::I, StokesRole::Q, StokesRole::U, StokesRole::V,
                    StokesRole::DerivedP, StokesRole::DerivedPdb,
                    StokesRole::DerivedPA, StokesRole::DerivedFrac,
                    StokesRole::DerivedAlpha, StokesRole::DerivedRM };
                // Stokes fluxes in Jy only for a Jy/beam set; otherwise the sum.
                const bool perBeam = visivo::flux::parseFluxUnit(this->remoteDatasetBunit).kind
                        == visivo::flux::UnitKind::PerBeam;
                const double Omega =
                        perBeam && this->hasBeamInfo() ? this->beamAreaPixels() : 0.0;
                for (StokesRole role : order) {
                    const int idx = this->findLayerByStokesRole(role);
                    if (idx < 0) continue;
                    auto *roleData = this->layers->getImageData(idx);
                    if (!roleData || roleData == imageData) continue;  // skip if same as master
                    const auto roleStats = computeRegionStatistics2D(roleData, region, grid);
                    if (!roleStats.valid) continue;
                    const QString roleName = this->stokesRoleLabel(role);
                    if (role == StokesRole::DerivedPA) {
                        rows.append({ roleName + u" (mean)"_s,
                                QStringLiteral("%1°").arg(roleStats.mean, 0, 'f', 2) });
                    } else if (role == StokesRole::DerivedFrac) {
                        rows.append({ roleName + u" (mean)"_s,
                                QStringLiteral("%1 %").arg(roleStats.mean * 100.0, 0, 'f', 2) });
                    } else if (role == StokesRole::DerivedAlpha) {
                        rows.append({ roleName + u" (mean)"_s,
                                QStringLiteral("%1").arg(roleStats.mean, 0, 'f', 2) });
                    } else if (role == StokesRole::DerivedRM) {
                        rows.append({ roleName + u" (mean)"_s,
                                QStringLiteral("%1 rad m⁻²").arg(roleStats.mean, 0, 'f', 1) });
                    } else if (Omega > 0.0) {
                        rows.append({ roleName,
                                QStringLiteral("%1 Jy").arg(roleStats.sum / Omega, 0, 'g', 6) });
                    } else {
                        rows.append({ roleName + u" (sum)"_s,
                                QStringLiteral("%1").arg(roleStats.sum, 0, 'g', 6) });
                    }
                }
                if (!rows.isEmpty()) {
                    if (grid.reduced())
                        rows.append({ u"Note"_s, u"From the displayed layers, not at full resolution."_s });
                    sections.append({ u"Stokes (per-layer)"_s, rows });
                }
            }

            sections.append({ u"Note"_s,
                            { { QString(), u"Computed on valid pixels only."_s } } });
        }

        const QVariantMap record =
                ToolResultDialog::makeRecord(u"Region Analysis"_s, subtitle, sections);
        auto *dlg = new ToolResultDialog(this);
        this->m_regionResultDialog = dlg;
        dlg->applyRecord(record);

        // One row per REGION of the list: re-measuring a region replaces its row,
        // another region adds one, so regions can be compared.
        QVariantMap params;
        params.insert(u"Region"_s, region.name);
        params.insert(u"Scope"_s, scope);
        if (stats.valid) {
            params.insert(u"Valid pixels"_s,
                          u"%1 / %2"_s.arg(stats.validCount).arg(stats.totalCount));
            params.insert(u"Sum"_s, visivo::measure::withUnit(stats.sum, this->remoteDatasetBunit));
            params.insert(u"Mean"_s,   QStringLiteral("%1").arg(stats.mean,     0, 'g', 8));
            params.insert(u"Median"_s, QStringLiteral("%1").arg(stats.median,   0, 'g', 8));
            params.insert(visivo::measure::stdLabel(),
                          QStringLiteral("%1").arg(stats.stddev, 0, 'g', 8));
            if (stats.hasRmsUnweighted)
                params.insert(visivo::measure::rmsLabel(),
                              QStringLiteral("%1").arg(stats.rmsUnweighted, 0, 'g', 8));
            params.insert(u"σ (MAD)"_s,QStringLiteral("%1").arg(stats.madSigma, 0, 'g', 8));
            params.insert(u"Peak"_s, visivo::measure::withUnit(stats.maxValue, this->remoteDatasetBunit));
            if (stats.hasPeak)
                params.insert(u"Peak pixel"_s, u"%1, %2"_s.arg(std::llround(stats.peakX))
                                                         .arg(std::llround(stats.peakY)));
        }
        // …with the geometry it was measured on, so opening the row puts that very
        // region back in the list. Full-resolution pixels: no voxel-grid stamp needed.
        const QVariantMap origin = visivo::encodeListRegionOrigin(
                visivo::regions::toCanonical(region, 0.0).toVariantMap());
        params.insert(u"Measured on"_s, visivo::describeOrigin(origin));
        this->registerAnalysisProduct(
                u"region"_s,
                u"%1 stats"_s.arg(region.name),
                params,
                this->scopedProductId(u"region-%1"_s.arg(region.id)),
                record, /*layerIndex=*/-1, origin);

        // The region stays in the list when the dialog closes, and the tool that
        // drew it stays armed: closing the numbers is not deleting the shape.
        dlg->show();
    };

    // The region can reach past the pixels loaded (a zoomed-in view holds the
    // part of the image on screen): their statistics would be the clipped
    // region's.
    bool clipped = false;
    if (imageData) {
        int extent[6];
        imageData->GetExtent(extent);
        const auto b = visivo::displayedVoxelBounds(region, grid);
        clipped = b[0] < extent[0] || b[1] > extent[1] || b[2] < extent[2] || b[3] > extent[3];
    }
    const auto reachesBeyond = [imageData, grid](const visivo::regions::Region &r) {
        if (!imageData)
            return false;
        int extent[6];
        imageData->GetExtent(extent);
        const auto b = visivo::displayedVoxelBounds(r, grid);
        return b[0] < extent[0] || b[1] > extent[1] || b[2] < extent[2] || b[3] > extent[3];
    };
    if (backgroundRegion && reachesBeyond(*backgroundRegion))
        clipped = true;   // its statistics come from the file too
    // The background measured on the pixels shown, for the paths that use them.
    std::optional<RegionStatistics> displayedBackground;
    if (backgroundRegion)
        displayedBackground = computeRegionStatistics2D(imageData, *backgroundRegion, grid);
    if (!grid.reduced() && !clipped) {
        present(displayed, u"the full-resolution pixels"_s, displayedBackground);
        return;
    }
    const QString loadedPixels = grid.reduced()
            ? displayedPixels
            : u"the full-resolution pixels loaded (the region reaches beyond them)"_s;
    if (this->remoteDatasetId.isEmpty()) {
        present(displayed, loadedPixels, displayedBackground);
        return;
    }

    // The list region is already in source pixels: the backend gets it as is.
    QJsonObject canonical = visivo::regions::toCanonical(region, 0.0);
    canonical.remove(u"props"_s);
    QJsonObject canonicalBackground;
    if (backgroundRegion) {
        canonicalBackground = visivo::regions::toCanonical(*backgroundRegion, 0.0);
        canonicalBackground.remove(u"props"_s);
    }
    this->m_regionStatsPending = true;
    this->updateRegionReadout();
    using Pair = std::pair<BackendRegionStatsResult, std::optional<BackendRegionStatsResult>>;
    auto *watcher = new QFutureWatcher<Pair>(this);
    QObject::connect(watcher, &QFutureWatcher<Pair>::finished, this,
                     [this, watcher, present, displayed, displayedBackground, loadedPixels,
                      requestGeneration, region, backgroundRegion]() {
        watcher->deleteLater();
        if (requestGeneration != this->m_regionDialogGeneration)
            return;   // another region meanwhile: the readout is its
        this->m_regionStatsPending = false;
        this->updateRegionReadout();
        const visivo::regions::Region *now = this->m_regions.find(region.id);
        if (!now)
            return;   // deleted meanwhile
        if (!visivo::regions::sameGeometry(*now, region))
            return;   // the region was edited meanwhile: these numbers are not its
        const auto toStats = [](const BackendRegionStatsResult &r) {
            RegionStatistics stats;
            stats.totalCount = r.totalCount;
            stats.validCount = r.validCount;
            stats.blankedCount = r.blankedCount;
            stats.minValue = r.minValue;
            stats.maxValue = r.maxValue;
            stats.sum = r.sum;
            stats.mean = r.mean;
            stats.median = r.median;
            stats.stddev = r.stddev;
            stats.rmsUnweighted = r.hasRmsUnweighted ? r.rmsUnweighted : 0.0;
            stats.hasRmsUnweighted = r.hasRmsUnweighted;
            stats.hasPeak = r.hasPeak;
            stats.peakX = r.peakX;
            stats.peakY = r.peakY;
            stats.madSigma = r.madSigma;
            stats.valid = r.validCount > 0;
            return stats;
        };
        const Pair result = watcher->result();
        if (!result.first.valid) {
            present(displayed, u"%1 (the full-resolution statistics failed: %2)"_s
                                       .arg(loadedPixels, result.first.error),
                    displayedBackground);
            return;
        }
        // The background from the file too, unless it was moved or failed:
        // then none, rather than one measured on other pixels than the region.
        std::optional<RegionStatistics> background;
        const visivo::regions::Region *bgNow =
                backgroundRegion ? this->m_regions.find(backgroundRegion->id) : nullptr;
        if (result.second && result.second->valid && bgNow
            && visivo::regions::sameGeometry(*bgNow, *backgroundRegion))
            background = toStats(*result.second);
        present(toStats(result.first), u"the full-resolution data (from the file)"_s, background);
    });
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
    watcher->setFuture(QtConcurrent::run([url, token, session, dataset, canonical,
                                          canonicalBackground]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        Pair out{ client.regionStats(dataset, canonical), std::nullopt };
        if (!canonicalBackground.isEmpty())
            out.second = client.regionStats(dataset, canonicalBackground);
        return out;
    }));
}

bool vtkWindowImage::finalizePolygonRegion()
{
    const int *position = ui->vtk->renderWindow()->GetInteractor()->GetEventPosition();
    if (position) {
        this->updateRegionFromDisplayPosition(position[0], position[1]);
        const bool closesToFirst = this->regionPolygonVertices.size() >= 3
                && this->regionCurrentVoxel == this->regionPolygonVertices.front();
        if (!closesToFirst && (this->regionPolygonVertices.empty()
                               || this->regionPolygonVertices.back() != this->regionCurrentVoxel)) {
            this->regionPolygonVertices.push_back(this->regionCurrentVoxel);
        }
    }

    std::vector<std::array<int, 2>> cleaned;
    cleaned.reserve(this->regionPolygonVertices.size());
    for (const auto &vertex : this->regionPolygonVertices) {
        if (cleaned.empty() || cleaned.back() != vertex) {
            cleaned.push_back(vertex);
        }
    }
    this->regionPolygonVertices = std::move(cleaned);
    if (this->regionPolygonVertices.size() < 3) {
        this->statusBar()->showMessage(u"Define at least three points for a polygon region."_s, 2500);
        return false;
    }

    this->regionDragging = false;
    this->regionValid = true;
    const int id = this->commitDraftRegion();
    if (id > 0)
        this->analyzeRegion(id);
    return id > 0;
}
