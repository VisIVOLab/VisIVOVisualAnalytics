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
    int totalCount{ 0 };
    int validCount{ 0 };
    int blankedCount{ 0 };
    double minValue{ 0. };
    double maxValue{ 0. };
    double mean{ 0. };
    double median{ 0. };
    double stddev{ 0. };
    double sum{ 0. };       ///< Sum of valid pixels (for Jy = sum / Ω_beam_px)
    double madSigma{ 0. };  ///< 1.4826 × MAD — robust σ estimate
    bool valid{ false };
};

std::array<int, 4> regionBounds2D(vtkWindowImage::RegionMode mode, const std::array<int, 2> &anchor,
                                  const std::array<int, 2> &current,
                                  const std::vector<std::array<int, 2>> &polygonVertices);
bool pointInRegion(vtkWindowImage::RegionMode mode, const std::array<int, 2> &anchor,
                   const std::array<int, 2> &current,
                   const std::vector<std::array<int, 2>> &polygonVertices, double annulusInnerRadius,
                   int x, int y);
QString regionModeLabel(vtkWindowImage::RegionMode mode);
RegionStatistics computeRegionStatistics2D(vtkImageData *imageData, vtkWindowImage::RegionMode mode,
                                           const std::array<int, 2> &anchor,
                                           const std::array<int, 2> &current,
                                           const std::vector<std::array<int, 2>> &polygonVertices,
                                           double annulusInnerRadius);






std::array<int, 4> regionBounds2D(vtkWindowImage::RegionMode mode, const std::array<int, 2> &anchor,
                                  const std::array<int, 2> &current,
                                  const std::vector<std::array<int, 2>> &polygonVertices)
{
    // Each shape's own box (CubeRegionGeom, unit-tested). A circle and an annulus
    // are a CENTRE plus a point on the rim: the box between those two corners is
    // one quadrant, and the stats loop only ever asked the hit-tester about
    // pixels inside it - so their statistics covered a quarter of the shape, in
    // whichever direction the user dragged.
    if (mode == vtkWindowImage::RegionMode::Polygon && !polygonVertices.empty()) {
        auto b = CubeRegionGeom::polygonBounds(polygonVertices);
        // While the polygon is still being drawn, `current` is the cursor vertex
        // and is not in the list yet.
        b[0] = std::min(b[0], current[0]);
        b[1] = std::max(b[1], current[0]);
        b[2] = std::min(b[2], current[1]);
        b[3] = std::max(b[3], current[1]);
        return b;
    }
    if (mode == vtkWindowImage::RegionMode::Circle || mode == vtkWindowImage::RegionMode::Annulus)
        return CubeRegionGeom::circleBounds(anchor, current);
    return CubeRegionGeom::boxBounds(anchor, current);
}

bool pointInRegion(vtkWindowImage::RegionMode mode, const std::array<int, 2> &anchor,
                   const std::array<int, 2> &current,
                   const std::vector<std::array<int, 2>> &polygonVertices, double annulusInnerRadius,
                   int x, int y)
{
    switch (mode) {
    case vtkWindowImage::RegionMode::Box:
        return pointInBox(anchor, current, x, y);
    case vtkWindowImage::RegionMode::Circle:
        return pointInCircle(anchor, current, x, y);
    case vtkWindowImage::RegionMode::Polygon:
        return pointInPolygon(polygonVertices, x, y);
    case vtkWindowImage::RegionMode::Annulus:
        return pointInAnnulus(anchor, current, annulusInnerRadius, x, y);
    case vtkWindowImage::RegionMode::None:
    default:
        return false;
    }
}

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
                                         vtkWindowImage::RegionMode mode,
                                         const std::array<int, 2> &anchor,
                                         const std::array<int, 2> &current,
                                         const std::vector<std::array<int, 2>> &polygonVertices,
                                         double annulusInnerRadius)
{
    RegionQuickStats quick;
    if (!imageData)
        return quick;

    int extent[6];
    imageData->GetExtent(extent);
    const auto bounds = regionBounds2D(mode, anchor, current, polygonVertices);
    const int x0 = std::max(extent[0], bounds[0]);
    const int x1 = std::min(extent[1], bounds[1]);
    const int y0 = std::max(extent[2], bounds[2]);
    const int y1 = std::min(extent[3], bounds[3]);
    if (x1 < x0 || y1 < y0)
        return quick;

    const long long box = static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1);
    const int stride = visivo::regionSampleStride(box);
    quick.sampled = stride > 1;

    double sum = 0.0;
    long long counted = 0;
    for (int y = y0; y <= y1; y += stride) {
        for (int x = x0; x <= x1; x += stride) {
            if (!pointInRegion(mode, anchor, current, polygonVertices, annulusInnerRadius, x, y))
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

RegionStatistics computeRegionStatistics2D(vtkImageData *imageData, vtkWindowImage::RegionMode mode,
                                           const std::array<int, 2> &anchor,
                                           const std::array<int, 2> &current,
                                           const std::vector<std::array<int, 2>> &polygonVertices,
                                           double annulusInnerRadius)
{
    RegionStatistics stats;
    if (!imageData) {
        return stats;
    }

    int extent[6];
    imageData->GetExtent(extent);
    const auto bounds = regionBounds2D(mode, anchor, current, polygonVertices);
    std::vector<double> values;
    double sum = 0.0;
    double sumSq = 0.0;
    stats.minValue = std::numeric_limits<double>::infinity();
    stats.maxValue = -std::numeric_limits<double>::infinity();

    for (int y = std::max(extent[2], bounds[2]); y <= std::min(extent[3], bounds[3]); ++y) {
        for (int x = std::max(extent[0], bounds[0]); x <= std::min(extent[1], bounds[1]); ++x) {
            const bool inside =
                    pointInRegion(mode, anchor, current, polygonVertices, annulusInnerRadius, x, y);
            if (!inside) {
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
                    u"Inner radius must be >= 0 and smaller than the outer radius."_s, 3000);
            this->regionAnnulusInnerRadius = 0.0;
            this->regionValid = true;
            this->refreshRegionOverlay();
            ui->vtk->renderWindow()->Render();
            return;
        }
        this->regionAnnulusInnerRadius = innerRadius;
        this->regionDragging = false;
        this->regionValid = true;
        this->refreshRegionOverlay();
        ui->vtk->renderWindow()->Render();
    }
    this->regionDragging = false;
    this->regionValid = true;
    this->refreshRegionOverlay();
    this->analyzeCurrentRegion();
    ui->vtk->renderWindow()->Render();
    // Note: the region action is intentionally NOT unchecked here.
    // The overlay stays visible while the result dialog is open, and is cleared
    // (along with the action check state) when the dialog is closed.
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
    if (!imageData || this->regionMode == RegionMode::None || !this->regionValid) {
        this->hideRegionReadout();
        return;
    }

    if (!m_regionReadout) {
        // A child of the render widget, so it floats over the image and moves
        // with it; the VTK surface has no layout to put it in.
        m_regionReadout = new QLabel(ui->vtk);
        m_regionReadout->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_regionReadout->setStyleSheet(
                QStringLiteral("QLabel { background: rgba(0,0,0,0.55); color:%1;"
                               " border:1px solid %2; border-radius:6px;"
                               " padding:5px 10px;"
                               " font-family:'JetBrains Mono','SF Mono',monospace;"
                               " font-size:10px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface()),
                             QLatin1StringView(VisivoTheme::kOutline())));
    }

    const auto quick = computeRegionQuickStats(imageData, this->regionMode,
                                               this->regionAnchorVoxel, this->regionCurrentVoxel,
                                               this->regionPolygonVertices,
                                               this->regionAnnulusInnerRadius);
    m_regionReadout->setText(visivo::regionReadoutText(regionModeLabel(this->regionMode),
                                                       quick.sum, quick.mean, quick.validCount,
                                                       quick.sampled));
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
        this->clearRegion();
        this->hideRegionReadout();
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
    // The readout describes a region; with none there is nothing for it to say.
    this->hideRegionReadout();
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
}

QJsonObject vtkWindowImage::currentRegionToJson() const
{
    // Canonical image-frame region in 1-based pixel coordinates (the FITS /
    // ds9 / CRTF convention). The viewer stores 0-based *display* voxels; when
    // the displayed image is decimated (spacing>1 for a preview or a future LOD
    // level) those must be mapped back onto the full-resolution pixel grid
    // before the +1. This is a no-op for a full-resolution image (origin 0,
    // spacing 1, extent0 0 → pxX(v) == v+1), so exports are byte-identical.
    double ox = 0.0, oy = 0.0, sx = 1.0, sy = 1.0;
    int ex0 = 0, ey0 = 0;
    if (this->layers) {
        if (auto *img = this->layers->getImageData(this->layers->getMasterIndex())) {
            double origin[3], spacing[3];
            int extent[6];
            img->GetOrigin(origin);
            img->GetSpacing(spacing);
            img->GetExtent(extent);
            ox = origin[0]; oy = origin[1];
            sx = spacing[0]; sy = spacing[1];
            ex0 = extent[0]; ey0 = extent[2];
        }
    }
    QJsonObject r;
    r[u"frame"_s] = u"image"_s;
    r[u"unit"_s]  = u"pix"_s;
    // Display voxel → full-res 1-based FITS pixel (per axis).
    const auto pxX = [=](int v) { return (ox + (v - ex0) * sx) + 1.0; };
    const auto pxY = [=](int v) { return (oy + (v - ey0) * sy) + 1.0; };
    switch (this->regionMode) {
    case RegionMode::Box: {
        const double ax = pxX(regionAnchorVoxel[0]), ay = pxY(regionAnchorVoxel[1]);
        const double bx = pxX(regionCurrentVoxel[0]), by = pxY(regionCurrentVoxel[1]);
        r[u"shape"_s] = u"box"_s;
        r[u"center"_s] = QJsonArray{ (ax + bx) / 2.0, (ay + by) / 2.0 };
        r[u"size"_s]   = QJsonArray{ std::abs(bx - ax), std::abs(by - ay) };
        r[u"angle_deg"_s] = 0.0;
        break;
    }
    case RegionMode::Circle: {
        const double dx = (regionCurrentVoxel[0] - regionAnchorVoxel[0]) * sx;
        const double dy = (regionCurrentVoxel[1] - regionAnchorVoxel[1]) * sy;
        r[u"shape"_s] = u"circle"_s;
        r[u"center"_s] = QJsonArray{ pxX(regionAnchorVoxel[0]), pxY(regionAnchorVoxel[1]) };
        r[u"radius"_s] = std::sqrt(dx * dx + dy * dy);
        break;
    }
    case RegionMode::Annulus: {
        const double dx = (regionCurrentVoxel[0] - regionAnchorVoxel[0]) * sx;
        const double dy = (regionCurrentVoxel[1] - regionAnchorVoxel[1]) * sy;
        r[u"shape"_s] = u"annulus"_s;
        r[u"center"_s] = QJsonArray{ pxX(regionAnchorVoxel[0]), pxY(regionAnchorVoxel[1]) };
        r[u"radii"_s]  = QJsonArray{ regionAnnulusInnerRadius * sx, std::sqrt(dx * dx + dy * dy) };
        break;
    }
    case RegionMode::Polygon: {
        r[u"shape"_s] = u"polygon"_s;
        QJsonArray pts;
        for (const auto &v : regionPolygonVertices)
            pts.append(QJsonArray{ pxX(v[0]), pxY(v[1]) });
        r[u"points"_s] = pts;
        break;
    }
    default:
        break;
    }
    return r;
}

void vtkWindowImage::exportRegionsToFile()
{
    if (this->regionMode == RegionMode::None || !this->regionValid) {
        QMessageBox::information(this, u"Export Region"_s,
                u"Draw a region first (Tools ▸ Box/Circle/Polygon/Annulus)."_s);
        return;
    }
    const QStringList choices{
        u"DS9, sky (FK5)"_s, u"DS9, pixel"_s,
        u"CASA CRTF, sky (FK5)"_s, u"CASA CRTF, pixel"_s };
    bool ok = false;
    const QString choice = QInputDialog::getItem(
            this, u"Export Region"_s, u"Format and coordinate frame:"_s, choices, 0, false, &ok);
    if (!ok) return;
    const bool isCrtf = choice.startsWith(u"CASA"_s);
    const QString format  = isCrtf ? u"crtf"_s : u"ds9"_s;
    const QString toFrame = choice.contains(u"sky"_s) ? u"fk5"_s : QString();
    const QString filter  = isCrtf ? u"CRTF region (*.crtf)"_s : u"DS9 region (*.reg)"_s;
    const QString suffix  = isCrtf ? u".crtf"_s : u".reg"_s;
    QString path = QFileDialog::getSaveFileName(this, u"Export Region File"_s,
            QFileInfo(this->filepath).baseName() + suffix, filter);
    if (path.isEmpty()) return;
    if (!path.endsWith(suffix)) path += suffix;

    QJsonArray arr{ this->currentRegionToJson() };
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken, dsId = this->remoteDatasetId;
    this->statusBar()->showMessage(u"Serializing region…"_s, 0);
    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendRegionExportResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendRegionExportResult>::finished, this, [self, watcher, path]() {
        const BackendRegionExportResult r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Export Region"_s, u"Export failed:\n%1"_s.arg(r.error));
            return;
        }
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(self, u"Export Region"_s, u"Cannot write file:\n%1"_s.arg(path));
            return;
        }
        out.write(r.text.toUtf8());
        out.close();
        QMessageBox::information(self, u"Export Region"_s, u"Saved region to:\n%1"_s.arg(path));
    });
    watcher->setFuture(QtConcurrent::run([url, token, arr, format, dsId, toFrame]() -> BackendRegionExportResult {
        BackendClient c(url, token);
        return c.exportRegions(arr, format, toFrame.isEmpty() ? QString() : dsId, toFrame);
    }));
}

void vtkWindowImage::analyzeCurrentRegion()
{
    if (this->regionMode == RegionMode::None || !this->regionValid || !this->layers) {
        return;
    }

    // Close any previous result dialog (its stale generation prevents cleanup races)
    if (this->m_regionResultDialog) {
        this->m_regionResultDialog->close();
    }

    auto *imageData = this->layers->getImageData(this->layers->getMasterIndex());
    const auto stats = computeRegionStatistics2D(imageData, this->regionMode, this->regionAnchorVoxel,
                                                 this->regionCurrentVoxel, this->regionPolygonVertices,
                                                 this->regionAnnulusInnerRadius);

    const int myGeneration = ++this->m_regionDialogGeneration;
    const RegionMode capturedMode = this->regionMode;

    // Built as a RECORD, then shown and registered from that single description
    // (ToolResultDialog::makeRecord). Registering it is what makes the result
    // outlive this window: the statistics used to exist only inside a dialog
    // with WA_DeleteOnClose, so closing it meant drawing the region again.
    QList<QPair<QString, QList<QPair<QString, QString>>>> sections;
    const QString subtitle =
            QStringLiteral("%1  ·  computed on the currently loaded data block")
                    .arg(regionModeLabel(capturedMode));

    if (!stats.valid) {
        sections.append({ u"Statistics"_s,
                        { { u"Result"_s, u"No valid pixels in the selected region."_s },
                          { u"Note"_s,   u"Blanked / NaN pixels are ignored."_s } } });
    } else {
        sections.append({ u"Statistics"_s,
                        { { u"Valid pixels"_s,
                            QStringLiteral("%1 / %2").arg(stats.validCount).arg(stats.totalCount) },
                          { u"Blanked / NaN"_s, QString::number(stats.blankedCount) },
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
                const auto roleStats = computeRegionStatistics2D(
                        roleData, this->regionMode, this->regionAnchorVoxel,
                        this->regionCurrentVoxel, this->regionPolygonVertices,
                        this->regionAnnulusInnerRadius);
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

    // One row per REGION SHAPE: re-measuring the same kind of region replaces its
    // row, a different shape adds one, so two regions can be compared.
    QVariantMap params;
    params.insert(u"Region"_s, regionModeLabel(capturedMode));
    params.insert(u"Scope"_s, u"currently loaded data block"_s);
    if (stats.valid) {
        params.insert(u"Valid pixels"_s,
                      u"%1 / %2"_s.arg(stats.validCount).arg(stats.totalCount));
        params.insert(u"Mean"_s,   QStringLiteral("%1").arg(stats.mean,     0, 'g', 8));
        params.insert(u"Median"_s, QStringLiteral("%1").arg(stats.median,   0, 'g', 8));
        params.insert(u"Std Dev"_s,QStringLiteral("%1").arg(stats.stddev,   0, 'g', 8));
        params.insert(u"σ (MAD)"_s,QStringLiteral("%1").arg(stats.madSigma, 0, 'g', 8));
    }
    // …with the geometry it was measured on, so opening the row redraws the very
    // box/circle/polygon/annulus these numbers came from.
    QVariantMap origin = visivo::encodeRegionOrigin(
            static_cast<int>(capturedMode), this->regionAnchorVoxel, this->regionCurrentVoxel,
            this->regionPolygonVertices, this->regionAnnulusInnerRadius);
    // Stamp the voxel grid it was drawn on: this window shows a decimated LOD
    // image before the full-resolution swap, and replaying raw indices across
    // that swap would draw the overlay at half position and half size.
    double frameScale[2], framePos[2];
    if (this->currentVoxelFrame(frameScale, framePos))
        origin = visivo::withVoxelFrame(origin, frameScale[0], frameScale[1], framePos[0],
                                        framePos[1]);
    params.insert(u"Measured on"_s, visivo::describeOrigin(origin));
    this->registerAnalysisProduct(
            u"region"_s,
            u"%1 region stats"_s.arg(regionModeLabel(capturedMode)),
            params,
            this->scopedProductId(u"region-%1"_s.arg(static_cast<int>(capturedMode))),
            record, /*layerIndex=*/-1, origin);

    // When the dialog is closed, clear the overlay and deactivate the region tool.
    // The generation guard prevents the cleanup from running if a newer analysis has
    // already replaced this dialog (e.g., the user drew a second region while this
    // dialog was still open).
    QObject::connect(dlg, &QObject::destroyed, this, [this, myGeneration, capturedMode]() {
        if (this->m_regionDialogGeneration != myGeneration) {
            return; // stale — a newer analysis supersedes this one
        }
        this->clearRegion();
        // Deactivate the region action; setRegionMode() will restore the image interactor.
        QAction *regionAction = nullptr;
        switch (capturedMode) {
        case RegionMode::Box:     regionAction = this->actionBoxRegion;     break;
        case RegionMode::Circle:  regionAction = this->actionCircleRegion;  break;
        case RegionMode::Polygon: regionAction = this->actionPolygonRegion; break;
        case RegionMode::Annulus: regionAction = this->actionAnnulusRegion; break;
        default: break;
        }
        if (regionAction && regionAction->isChecked()) {
            regionAction->setChecked(false); // triggers setRegionMode(mode, false)
        } else {
            // Action was already unchecked; at minimum restore the interactor and render.
            this->setInteractorStyleImage();
            if (ui && ui->vtk && ui->vtk->renderWindow()) {
                ui->vtk->renderWindow()->Render();
            }
        }
    });

    dlg->show();
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
    this->refreshRegionOverlay();
    this->analyzeCurrentRegion();
    // Note: polygon action is NOT unchecked here — the overlay stays visible while
    // the result dialog is open and is cleared when the dialog closes.
    return true;
}
