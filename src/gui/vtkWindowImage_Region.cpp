#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeRegionGeom.h"
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
#include "ProductOrigin.h"
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
    double minValue{ 0. };
    double maxValue{ 0. };
    double mean{ 0. };
    double median{ 0. };
    double stddev{ 0. };
    double sum{ 0. };       ///< Sum of valid pixels (for Jy = sum / Ω_beam_px)
    double madSigma{ 0. };  ///< 1.4826 × MAD — robust σ estimate
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
    stats.mean = sum / static_cast<double>(stats.validCount);
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
    if (!shown) {
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
    this->regionTopActor->SetVisibility(showBox);
    this->regionBottomActor->SetVisibility(showBox);
    this->regionLeftActor->SetVisibility(showBox);
    this->regionRightActor->SetVisibility(showBox);
    this->regionCircleActor->SetVisibility(showCircle);
    this->regionAnnulusOuterActor->SetVisibility(showAnnulus);
    this->regionAnnulusInnerActor->SetVisibility(showAnnulus && !this->regionDragging);
    this->regionAnnulusFillActor->SetVisibility(showAnnulus && !this->regionDragging);
    this->regionPolygonActor->SetVisibility(showPolygon);
    this->regionPolygonFillActor->SetVisibility(showPolygon);

    if (showBox) {
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
                if (!self)
                    return;
                // A drag still running ends here: the import is its own
                // change, after it, not inside its undo step.
                if (self->m_regionDrag.active) {
                    self->m_regionDrag.active = false;
                    self->m_regions.endGesture();
                }
                // One change for the whole file (one Undo takes it back); the
                // first region is selected.
                if (!self->m_regions.addAll(result.regions).empty())
                    self->onRegionListChanged();
            },
            path, note);
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

    auto present = [this, region, grid](const RegionStatistics &stats, const QString &scope) {
        auto *imageData = this->layers->getImageData(this->layers->getMasterIndex());

        // Built as a RECORD, then shown and registered from that single description
        // (ToolResultDialog::makeRecord). Registering it is what makes the result
        // outlive this window: the statistics used to exist only inside a dialog
        // with WA_DeleteOnClose, so closing it meant drawing the region again.
        QList<QPair<QString, QList<QPair<QString, QString>>>> sections;
        const QString subtitle =
                QStringLiteral("%1  ·  computed on %2").arg(region.name, scope);

        if (!stats.valid) {
            sections.append({ u"Statistics"_s,
                            { { u"Result"_s, u"No valid pixels in the selected region."_s },
                              { u"Note"_s,   u"Blanked / NaN pixels are ignored."_s } } });
        } else {
            sections.append({ u"Statistics"_s,
                            { { u"Valid pixels"_s,
                                QStringLiteral("%1 / %2").arg(QLocale(QLocale::English).toString(stats.validCount),
                                                              QLocale(QLocale::English).toString(stats.totalCount)) },
                              { u"Blanked / NaN"_s, QLocale(QLocale::English).toString(stats.blankedCount) },
                              { u"Min"_s,    QStringLiteral("%1").arg(stats.minValue, 0, 'g', 8) },
                              { u"Max"_s,    QStringLiteral("%1").arg(stats.maxValue, 0, 'g', 8) },
                              { u"Mean"_s,   QStringLiteral("%1").arg(stats.mean,     0, 'g', 8) },
                              { u"Median"_s, QStringLiteral("%1").arg(stats.median,   0, 'g', 8) },
                              { u"Std Dev"_s,QStringLiteral("%1").arg(stats.stddev,   0, 'g', 8) },
                              { u"σ (MAD)"_s,QStringLiteral("%1").arg(stats.madSigma, 0, 'g', 8) } } });

            // ── Radio-specific: integrated flux if beam known ────────────────
            if (this->hasBeamInfo()) {
                const double Omega_beam_px = this->beamAreaPixels();
                if (Omega_beam_px > 0.0) {
                    const double fluxJy = stats.sum / Omega_beam_px;
                    const double snrPeak = (stats.madSigma > 0.0)
                            ? (stats.maxValue / stats.madSigma) : 0.0;
                    sections.append({ u"Radio (beam-aware)"_s,
                            { { u"Beam (BMAJ × BMIN)"_s,
                                QStringLiteral("%1\" × %2\"")
                                        .arg(this->m_beamMajorDeg * 3600.0, 0, 'f', 2)
                                        .arg(this->m_beamMinorDeg * 3600.0, 0, 'f', 2) },
                              { u"Beam area (pixels)"_s,
                                QStringLiteral("%1").arg(Omega_beam_px, 0, 'f', 2) },
                              { u"Integrated flux"_s,
                                QStringLiteral("%1 Jy").arg(fluxJy, 0, 'g', 6) },
                              { u"Peak SNR (peak/σ_MAD)"_s,
                                QStringLiteral("%1").arg(snrPeak, 0, 'f', 1) } } });
                }
            }

            // ── Per-Stokes flux when a Stokes set is loaded ──────────────────
            if (!m_stokesRoles.isEmpty()) {
                QList<QPair<QString, QString>> rows;
                const StokesRole order[] = {
                    StokesRole::I, StokesRole::Q, StokesRole::U, StokesRole::V,
                    StokesRole::DerivedP, StokesRole::DerivedPdb,
                    StokesRole::DerivedPA, StokesRole::DerivedFrac,
                    StokesRole::DerivedAlpha, StokesRole::DerivedRM };
                const double Omega = this->hasBeamInfo() ? this->beamAreaPixels() : 0.0;
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
            params.insert(u"Mean"_s,   QStringLiteral("%1").arg(stats.mean,     0, 'g', 8));
            params.insert(u"Median"_s, QStringLiteral("%1").arg(stats.median,   0, 'g', 8));
            params.insert(u"Std Dev"_s,QStringLiteral("%1").arg(stats.stddev,   0, 'g', 8));
            params.insert(u"σ (MAD)"_s,QStringLiteral("%1").arg(stats.madSigma, 0, 'g', 8));
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
    if (!grid.reduced() && !clipped) {
        present(displayed, u"the full-resolution pixels"_s);
        return;
    }
    const QString loadedPixels = grid.reduced()
            ? displayedPixels
            : u"the full-resolution pixels loaded (the region reaches beyond them)"_s;
    if (this->remoteDatasetId.isEmpty()) {
        present(displayed, loadedPixels);
        return;
    }

    // The list region is already in source pixels: the backend gets it as is.
    QJsonObject canonical = visivo::regions::toCanonical(region, 0.0);
    canonical.remove(u"props"_s);
    this->m_regionStatsPending = true;
    this->updateRegionReadout();
    auto *watcher = new QFutureWatcher<BackendRegionStatsResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendRegionStatsResult>::finished, this,
                     [this, watcher, present, displayed, loadedPixels, requestGeneration,
                      region]() {
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
        const BackendRegionStatsResult r = watcher->result();
        if (!r.valid) {
            present(displayed, u"%1 (the full-resolution statistics failed: %2)"_s
                                       .arg(loadedPixels, r.error));
            return;
        }
        RegionStatistics stats;
        stats.totalCount = r.totalCount;
        stats.validCount = r.validCount;
        stats.blankedCount = r.blankedCount;
        stats.minValue = r.minValue;
        stats.maxValue = r.maxValue;
        stats.sum = r.sum;
        stats.mean = r.mean;
        stats.median = r.median;
        stats.stddev = r.rms;
        stats.madSigma = r.madSigma;
        stats.valid = r.validCount > 0;
        present(stats, u"the full-resolution data (from the file)"_s);
    });
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
    watcher->setFuture(QtConcurrent::run([url, token, session, dataset, canonical]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        return client.regionStats(dataset, canonical);
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
