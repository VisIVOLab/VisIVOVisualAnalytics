#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"
#include "CubeRegionGeom.h"
#include "CubeViewportGeometry.h"
#include "CubeStatsUtil.h"

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
using namespace CubeRegionGeom;
using namespace CubeGeom;
using namespace CubeStats;

// ── Region (ROI) interaction + statistics ────────────────────────────────────
// Extracted verbatim from vtkWindowCube.cpp. ROI drawing (box/circle/polygon/
// annulus), overlay rendering, and pixel statistics over the drawn region.
// distance2d / polygonClosureTolerance come from CubeGeom (shared util).
// regionBounds2D is still duplicated here — the main TU keeps its own copy for
// exportSubCubeAsFits (it depends on the vtkWindowCube::RegionMode enum).

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
    bool valid{ false };
};







std::array<int, 4> regionBounds2D(vtkWindowCube::RegionMode mode, const std::array<int, 2> &anchor,
                                  const std::array<int, 2> &current,
                                  const std::vector<std::array<int, 2>> &polygonVertices)
{
    if (mode == vtkWindowCube::RegionMode::Polygon && !polygonVertices.empty()) {
        int xmin = polygonVertices.front()[0];
        int xmax = polygonVertices.front()[0];
        int ymin = polygonVertices.front()[1];
        int ymax = polygonVertices.front()[1];
        for (const auto &vertex : polygonVertices) {
            xmin = std::min(xmin, vertex[0]);
            xmax = std::max(xmax, vertex[0]);
            ymin = std::min(ymin, vertex[1]);
            ymax = std::max(ymax, vertex[1]);
        }
        xmin = std::min(xmin, current[0]);
        xmax = std::max(xmax, current[0]);
        ymin = std::min(ymin, current[1]);
        ymax = std::max(ymax, current[1]);
        return { xmin, xmax, ymin, ymax };
    }

    const int xmin = std::min(anchor[0], current[0]);
    const int xmax = std::max(anchor[0], current[0]);
    const int ymin = std::min(anchor[1], current[1]);
    const int ymax = std::max(anchor[1], current[1]);
    return { xmin, xmax, ymin, ymax };
}

bool pointInRegion(vtkWindowCube::RegionMode mode, const std::array<int, 2> &anchor,
                   const std::array<int, 2> &current,
                   const std::vector<std::array<int, 2>> &polygonVertices, double annulusInnerRadius,
                   int x, int y)
{
    switch (mode) {
    case vtkWindowCube::RegionMode::Box:
        return pointInBox(anchor, current, x, y);
    case vtkWindowCube::RegionMode::Circle:
        return pointInCircle(anchor, current, x, y);
    case vtkWindowCube::RegionMode::Polygon:
        return pointInPolygon(polygonVertices, x, y);
    case vtkWindowCube::RegionMode::Annulus:
        return pointInAnnulus(anchor, current, annulusInnerRadius, x, y);
    case vtkWindowCube::RegionMode::None:
    default:
        return false;
    }
}

QString regionModeLabel(vtkWindowCube::RegionMode mode)
{
    switch (mode) {
    case vtkWindowCube::RegionMode::Box:
        return u"Box"_s;
    case vtkWindowCube::RegionMode::Circle:
        return u"Circle"_s;
    case vtkWindowCube::RegionMode::Polygon:
        return u"Polygon"_s;
    case vtkWindowCube::RegionMode::Annulus:
        return u"Annulus"_s;
    case vtkWindowCube::RegionMode::None:
    default:
        return u"Region"_s;
    }
}

RegionStatistics computeRegionStatistics2D(vtkImageData *imageData, vtkWindowCube::RegionMode mode,
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

    stats.mean = sum / static_cast<double>(stats.validCount);
    stats.stddev = std::sqrt(std::max(0.0, sumSq / static_cast<double>(stats.validCount)
                                               - stats.mean * stats.mean));
    std::sort(values.begin(), values.end());
    const int mid = stats.validCount / 2;
    stats.median = (stats.validCount % 2 == 0) ? 0.5 * (values[mid - 1] + values[mid]) : values[mid];
    stats.valid = true;
    return stats;
}
}  // namespace

void vtkWindowCube::finishRegionInteraction()
{
    if (this->isBusy()) {
        return;
    }
    if (this->m_lassoEditActive) {
        return;   // lasso 2-D edit consumes the release
    }

    if (this->regionMode == RegionMode::None || !this->regionDragging) {
        return;
    }

    if (this->regionMode == RegionMode::Polygon) {
        if (this->ignoreNextPolygonRelease) {
            this->ignoreNextPolygonRelease = false;
            return;
        }
        if (this->finalizePolygonRegion()) {
            this->active2dWin()->Render();
        }
        return;
    }

    const int *position = this->active2dWin()->GetInteractor()->GetEventPosition();
    if (position) {
        this->updateRegionFromDisplayPosition(position[0], position[1]);
    }
    if (this->regionMode == RegionMode::Annulus) {
        const double dx = static_cast<double>(this->regionCurrentVoxel[0] - this->regionAnchorVoxel[0]);
        const double dy = static_cast<double>(this->regionCurrentVoxel[1] - this->regionAnchorVoxel[1]);
        const double outerRadius = std::sqrt(dx * dx + dy * dy);
        bool accepted = false;
        const double innerRadius = QInputDialog::getDouble(
                this, u"Annulus Inner Radius"_s, u"Inner radius (pixels):"_s, 0.0, 0.0,
                std::max(0.0, outerRadius), 1, &accepted);
        if (!accepted) {
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
            this->active2dWin()->Render();
            return;
        }
        this->regionAnnulusInnerRadius = innerRadius;
        this->regionDragging = false;
        this->regionValid = true;
        this->refreshRegionOverlay();
        this->active2dWin()->Render();
    }
    this->regionDragging = false;
    this->regionValid = true;
    this->refreshRegionOverlay();
    this->analyzeCurrentRegion();
    this->active2dWin()->Render();
    if (this->regionMode == RegionMode::Box && this->actionBoxRegion && this->actionBoxRegion->isChecked()) {
        this->actionBoxRegion->setChecked(false);
    } else if (this->regionMode == RegionMode::Circle && this->actionCircleRegion
               && this->actionCircleRegion->isChecked()) {
        this->actionCircleRegion->setChecked(false);
    } else if (this->regionMode == RegionMode::Annulus && this->actionAnnulusRegion
               && this->actionAnnulusRegion->isChecked()) {
        this->actionAnnulusRegion->setChecked(false);
    } else if (this->regionMode == RegionMode::Polygon && this->actionPolygonRegion
               && this->actionPolygonRegion->isChecked()) {
        this->actionPolygonRegion->setChecked(false);
    }
}

void vtkWindowCube::setRegionMode(RegionMode mode, bool active)
{
    if (!active) {
        if (this->regionMode == mode) {
            this->regionMode = RegionMode::None;
            this->regionDragging = false;
            // Keep the overlay visible while the region spectrum plot is still open — analogous
            // to how the PV overlay survives until pvDiagramWidget closes.
            // maybeClearRegionAfterDisplay() wipes it once that closes.
            if (!this->probePlotWidget) {
                this->clearRegion();
            }
            this->setInteractorStyleImage();
            if (auto *w2d = this->active2dWidget()) w2d->setCursor(this->probeModeActive ? Qt::CrossCursor : Qt::ArrowCursor);
            if (this->active2dWin()) {
                this->active2dWin()->Render();
            }
        }
        return;
    }

    if (this->probeModeActive && ui->actionExtractSpectrum->isChecked()) {
        ui->actionExtractSpectrum->setChecked(false);
    }
    if (this->actionExtractPvDiagram && this->actionExtractPvDiagram->isChecked()) {
        this->actionExtractPvDiagram->setChecked(false);
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
    if (auto *w2d = this->active2dWidget()) w2d->setCursor(Qt::CrossCursor);
}

bool vtkWindowCube::updateRegionFromDisplayPosition(int displayX, int displayY)
{
    auto *renderer = this->active2dWin() ? this->active2dWin()->GetRenderers()->GetFirstRenderer()
                                                  : nullptr;
    auto *imageData = this->viewingSlice()
            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
            : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    if (!renderer || !imageData) {
        this->pvCursorValid = false;
        this->refreshPvOverlay();
        return false;
    }

    this->coordinate->SetValue(displayX, displayY);
    const double *worldCoord = this->coordinate->GetComputedWorldValue(renderer);
    if (!worldCoord) {
        this->pvCursorValid = false;
        this->refreshPvOverlay();
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
        this->pvCursorValid = false;
        this->refreshPvOverlay();
        return false;
    }

    this->regionCurrentVoxel = { voxelX, voxelY };
    this->refreshRegionOverlay();
    return true;
}

void vtkWindowCube::refreshRegionOverlay()
{
    const auto updateOverlay = [this](vtkImageData *imageData, vtkLineSource *top, vtkLineSource *bottom,
                                      vtkLineSource *left, vtkLineSource *right, vtkActor *topActor,
                                      vtkActor *bottomActor, vtkActor *leftActor, vtkActor *rightActor,
                                      vtkRegularPolygonSource *circleSource, vtkActor *circleActor,
                                      vtkRegularPolygonSource *annulusOuterSource,
                                      vtkRegularPolygonSource *annulusInnerSource,
                                      vtkActor *annulusOuterActor, vtkActor *annulusInnerActor,
                                      vtkPoints *annulusFillPoints, vtkCellArray *annulusFillCells,
                                      vtkPolyData *annulusFillData, vtkActor *annulusFillActor,
                                      vtkPoints *polygonPoints, vtkCellArray *polygonCells,
                                      vtkPolyData *polygonData, vtkActor *polygonActor,
                                      vtkPolyData *polygonFillData, vtkContourTriangulator *polygonTriangulator,
                                      vtkActor *polygonFillActor) {
        if (!imageData || this->regionMode == RegionMode::None || !this->regionValid) {
            topActor->VisibilityOff();
            bottomActor->VisibilityOff();
            leftActor->VisibilityOff();
            rightActor->VisibilityOff();
            circleActor->VisibilityOff();
            annulusOuterActor->VisibilityOff();
            annulusInnerActor->VisibilityOff();
            annulusFillActor->VisibilityOff();
            polygonActor->VisibilityOff();
            polygonFillActor->VisibilityOff();
            return;
        }

        const int xmin = std::min(this->regionAnchorVoxel[0], this->regionCurrentVoxel[0]);
        const int xmax = std::max(this->regionAnchorVoxel[0], this->regionCurrentVoxel[0]);
        const int ymin = std::min(this->regionAnchorVoxel[1], this->regionCurrentVoxel[1]);
        const int ymax = std::max(this->regionAnchorVoxel[1], this->regionCurrentVoxel[1]);
        const bool showBox = this->regionMode == RegionMode::Box;
        const bool showCircle = this->regionMode == RegionMode::Circle;
        const bool showAnnulus = this->regionMode == RegionMode::Annulus;
        const bool showPolygon = this->regionMode == RegionMode::Polygon;
        topActor->SetVisibility(showBox);
        bottomActor->SetVisibility(showBox);
        leftActor->SetVisibility(showBox);
        rightActor->SetVisibility(showBox);
        circleActor->SetVisibility(showCircle);
        annulusOuterActor->SetVisibility(showAnnulus);
        annulusInnerActor->SetVisibility(showAnnulus && !this->regionDragging);
        annulusFillActor->SetVisibility(showAnnulus && !this->regionDragging);
        polygonActor->SetVisibility(showPolygon);
        polygonFillActor->SetVisibility(showPolygon);

        if (showBox) {
            top->SetPoint1(xmin, ymax, 0.0);
            top->SetPoint2(xmax, ymax, 0.0);
            bottom->SetPoint1(xmin, ymin, 0.0);
            bottom->SetPoint2(xmax, ymin, 0.0);
            left->SetPoint1(xmin, ymin, 0.0);
            left->SetPoint2(xmin, ymax, 0.0);
            right->SetPoint1(xmax, ymin, 0.0);
            right->SetPoint2(xmax, ymax, 0.0);
        } else if (showCircle || showAnnulus) {
            const double dx = static_cast<double>(this->regionCurrentVoxel[0] - this->regionAnchorVoxel[0]);
            const double dy = static_cast<double>(this->regionCurrentVoxel[1] - this->regionAnchorVoxel[1]);
            const double outerRadius = std::sqrt(dx * dx + dy * dy);
            if (showCircle) {
                circleSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
                circleSource->SetRadius(outerRadius);
            } else {
                annulusOuterSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
                annulusOuterSource->SetRadius(outerRadius);
                if (!this->regionDragging) {
                    annulusInnerSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
                    const double innerRadius = std::max(0.0, std::min(this->regionAnnulusInnerRadius, outerRadius));
                    annulusInnerSource->SetRadius(innerRadius);
                    buildAnnulusFill(annulusFillPoints, annulusFillCells, annulusFillData,
                                     this->regionAnchorVoxel, innerRadius, outerRadius, 96);
                    annulusFillActor->SetVisibility(innerRadius < outerRadius);
                } else {
                    annulusFillData->Initialize();
                    annulusInnerActor->SetVisibility(false);
                    annulusFillActor->SetVisibility(false);
                }
            }
        } else if (showPolygon) {
            std::vector<std::array<int, 2>> drawVertices = this->regionPolygonVertices;
            if (this->regionDragging && (drawVertices.empty() || drawVertices.back() != this->regionCurrentVoxel)) {
                drawVertices.push_back(this->regionCurrentVoxel);
            }
            const bool previewClosure = this->regionDragging && drawVertices.size() >= 3
                    && distance2d(this->regionCurrentVoxel, drawVertices.front()) <= polygonClosureTolerance;
            polygonPoints->Reset();
            polygonCells->Reset();
            polygonFillData->Initialize();
            if (drawVertices.size() >= 2) {
                const bool closed = !this->regionDragging || previewClosure;
                const vtkIdType count =
                        static_cast<vtkIdType>(drawVertices.size() + (closed ? 1 : 0));
                polygonCells->InsertNextCell(count);
                for (vtkIdType i = 0; i < static_cast<vtkIdType>(drawVertices.size()); ++i) {
                    polygonPoints->InsertNextPoint(drawVertices[static_cast<std::size_t>(i)][0],
                                                   drawVertices[static_cast<std::size_t>(i)][1], 0.0);
                    polygonCells->InsertCellPoint(i);
                }
                if (closed) {
                    polygonPoints->InsertNextPoint(drawVertices.front()[0], drawVertices.front()[1], 0.0);
                    polygonCells->InsertCellPoint(static_cast<vtkIdType>(drawVertices.size()));
                }
                polygonData->SetPoints(polygonPoints);
                polygonData->SetLines(polygonCells);
                polygonData->Modified();
                if (drawVertices.size() >= 3) {
                    polygonFillData->SetPoints(polygonPoints);
                    polygonFillData->SetLines(polygonCells);
                    polygonFillData->Modified();
                    if (closed) {
                        polygonTriangulator->Update();
                        polygonFillActor->SetVisibility(true);
                    } else {
                        polygonFillActor->SetVisibility(false);
                    }
                } else {
                    polygonFillActor->SetVisibility(false);
                }
            } else {
                polygonFillActor->SetVisibility(false);
            }
        } else {
            annulusFillActor->SetVisibility(false);
            polygonFillActor->SetVisibility(false);
        }
    };

    updateOverlay(vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0)),
                  this->sliceRegionTopLine, this->sliceRegionBottomLine, this->sliceRegionLeftLine,
                  this->sliceRegionRightLine, this->sliceRegionTopActor, this->sliceRegionBottomActor,
                  this->sliceRegionLeftActor, this->sliceRegionRightActor, this->sliceRegionCircleSource,
                  this->sliceRegionCircleActor, this->sliceRegionAnnulusOuterSource,
                  this->sliceRegionAnnulusInnerSource, this->sliceRegionAnnulusOuterActor,
                  this->sliceRegionAnnulusInnerActor, this->sliceRegionAnnulusFillPoints,
                  this->sliceRegionAnnulusFillCells, this->sliceRegionAnnulusFillData,
                  this->sliceRegionAnnulusFillActor, this->sliceRegionPolygonPoints,
                  this->sliceRegionPolygonCells, this->sliceRegionPolygonData, this->sliceRegionPolygonActor,
                  this->sliceRegionPolygonFillData, this->sliceRegionPolygonTriangulator,
                  this->sliceRegionPolygonFillActor);
    updateOverlay(vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0)),
                  this->momentRegionTopLine, this->momentRegionBottomLine, this->momentRegionLeftLine,
                  this->momentRegionRightLine, this->momentRegionTopActor, this->momentRegionBottomActor,
                  this->momentRegionLeftActor, this->momentRegionRightActor, this->momentRegionCircleSource,
                  this->momentRegionCircleActor, this->momentRegionAnnulusOuterSource,
                  this->momentRegionAnnulusInnerSource, this->momentRegionAnnulusOuterActor,
                  this->momentRegionAnnulusInnerActor, this->momentRegionAnnulusFillPoints,
                  this->momentRegionAnnulusFillCells, this->momentRegionAnnulusFillData,
                  this->momentRegionAnnulusFillActor, this->momentRegionPolygonPoints,
                  this->momentRegionPolygonCells, this->momentRegionPolygonData,
                  this->momentRegionPolygonActor, this->momentRegionPolygonFillData,
                  this->momentRegionPolygonTriangulator, this->momentRegionPolygonFillActor);
}

void vtkWindowCube::setNoiseRegionPreview(int x0, int x1, int y0, int y1,
                                          int chStart, int chEnd)
{
    // Normalise so a user dragging max<min (allowed by the spinbox while
    // they're editing) doesn't produce an inverted rectangle. Pixel-coord
    // convention matches the Box region tool — the slight half-pixel
    // misalignment between line centres and image cells is invisible under
    // the line width.
    const double xLo = static_cast<double>(std::min(x0, x1));
    const double xHi = static_cast<double>(std::max(x0, x1));
    const double yLo = static_cast<double>(std::min(y0, y1));
    const double yHi = static_cast<double>(std::max(y0, y1));
    const double zLo = static_cast<double>(std::min(chStart, chEnd));
    const double zHi = static_cast<double>(std::max(chStart, chEnd));

    // ── 2D slice rectangle ──────────────────────────────────────────────
    this->noisePreviewTopLine   ->SetPoint1(xLo, yHi, 0.0);
    this->noisePreviewTopLine   ->SetPoint2(xHi, yHi, 0.0);
    this->noisePreviewBottomLine->SetPoint1(xLo, yLo, 0.0);
    this->noisePreviewBottomLine->SetPoint2(xHi, yLo, 0.0);
    this->noisePreviewLeftLine  ->SetPoint1(xLo, yLo, 0.0);
    this->noisePreviewLeftLine  ->SetPoint2(xLo, yHi, 0.0);
    this->noisePreviewRightLine ->SetPoint1(xHi, yLo, 0.0);
    this->noisePreviewRightLine ->SetPoint2(xHi, yHi, 0.0);

    this->noisePreviewTopActor   ->VisibilityOn();
    this->noisePreviewBottomActor->VisibilityOn();
    this->noisePreviewLeftActor  ->VisibilityOn();
    this->noisePreviewRightActor ->VisibilityOn();

    // ── 3D volume box ───────────────────────────────────────────────────
    // SetBounds takes (xmin,xmax, ymin,ymax, zmin,zmax) in the volume's
    // local coordinate frame. The default cube load uses spacing (1,1,1)
    // and origin (0,0,0), so user pixel coordinates map identically. For
    // ROI-cropped cubes the volume's origin sits at the ROI corner — we
    // could adjust here, but the spinboxes in NoiseRegionDialog are
    // already full-cube pixel indices (0..width-1, 0..depth-1) and the
    // backend interprets them the same way, so any offset is consistent.
    this->noisePreviewCubeSource->SetBounds(xLo, xHi, yLo, yHi, zLo, zHi);
    this->noisePreviewCubeFaceActor->VisibilityOn();
    this->noisePreviewCubeEdgeActor->VisibilityOn();

    if (ui->vtkImage && this->active2dWin()) {
        this->active2dWin()->Render();
    }
    if (ui->vtkCube && ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }
}

void vtkWindowCube::clearNoiseRegionPreview()
{
    this->noisePreviewTopActor    ->VisibilityOff();
    this->noisePreviewBottomActor ->VisibilityOff();
    this->noisePreviewLeftActor   ->VisibilityOff();
    this->noisePreviewRightActor  ->VisibilityOff();
    this->noisePreviewCubeFaceActor->VisibilityOff();
    this->noisePreviewCubeEdgeActor->VisibilityOff();

    if (ui->vtkImage && this->active2dWin()) {
        this->active2dWin()->Render();
    }
    if (ui->vtkCube && ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }
}

void vtkWindowCube::clearRegion()
{
    this->regionValid = false;
    this->m_exportableRegionShape = RegionMode::None;
    this->regionDragging = false;
    this->ignoreNextPolygonRelease = false;
    this->regionPolygonVertices.clear();
    this->regionAnnulusInnerRadius = 0.0;
    this->sliceRegionTopActor->VisibilityOff();
    this->sliceRegionBottomActor->VisibilityOff();
    this->sliceRegionLeftActor->VisibilityOff();
    this->sliceRegionRightActor->VisibilityOff();
    this->sliceRegionCircleActor->VisibilityOff();
    this->sliceRegionAnnulusOuterActor->VisibilityOff();
    this->sliceRegionAnnulusInnerActor->VisibilityOff();
    this->sliceRegionAnnulusFillActor->VisibilityOff();
    this->sliceRegionPolygonActor->VisibilityOff();
    this->sliceRegionPolygonFillActor->VisibilityOff();
    this->momentRegionTopActor->VisibilityOff();
    this->momentRegionBottomActor->VisibilityOff();
    this->momentRegionLeftActor->VisibilityOff();
    this->momentRegionRightActor->VisibilityOff();
    this->momentRegionCircleActor->VisibilityOff();
    this->momentRegionAnnulusOuterActor->VisibilityOff();
    this->momentRegionAnnulusInnerActor->VisibilityOff();
    this->momentRegionAnnulusFillActor->VisibilityOff();
    this->momentRegionPolygonActor->VisibilityOff();
    this->momentRegionPolygonFillActor->VisibilityOff();
    // There is no region any more — "Export Region (CRTF/DS9)" has nothing to
    // serialise and says so instead of answering the click with a dialog.
    this->applyMomentToolGate();
}

void vtkWindowCube::analyzeCurrentRegion()
{
    if (this->regionMode == RegionMode::None || !this->regionValid) {
        return;
    }
    // Remember the shape: after analysis the region action is unchecked and
    // regionMode goes back to None (geometry stays for display), but Region IO
    // export still needs to know what shape to serialise.
    this->m_exportableRegionShape = this->regionMode;
    this->applyMomentToolGate();   // a region now exists → Export Region opens up

    auto *imageData = this->viewingSlice()
            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
            : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    const auto stats = computeRegionStatistics2D(imageData, this->regionMode, this->regionAnchorVoxel,
                                                 this->regionCurrentVoxel, this->regionPolygonVertices,
                                                 this->regionAnnulusInnerRadius);

    auto fmt = [](double v) { return QString::number(v, 'g', 8); };
    const QString shapeLabel = regionModeLabel(this->regionMode);

    // ── Per-channel spectral profile ──────────────────────────────────────────
    // Uses the 3D cube (cubeDisplaySource) to build mean-per-channel over the
    // selected region. Coordinate mapping: region voxels (in 2D imageData space)
    // are converted to physical world coords using imageData geometry, then to
    // cubeImage voxel indices using cubeImage geometry.  This is robust to
    // differing origins / spacings between the two datasets.
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    // True once THIS analysis registers a SPEC product (needs a cube spectrum). The 2D stats
    // live in that product's Provenance, and its spectrum plot governs the overlay's lifetime —
    // so the status message + overlay cleanup below both key off it.
    bool producedProduct = false;
    int regionSpectrumPane = -1;   // where the SPEC product landed (-1 = no room)
    if (cubeImage && stats.valid) {
        // ── Geometry of the 2D display data (where region voxels live) ─────────
        int extent2d[6];
        double origin2d[3];
        double spacing2d[3];
        imageData->GetExtent(extent2d);
        imageData->GetOrigin(origin2d);
        imageData->GetSpacing(spacing2d);

        // ── Geometry of the 3D cube data ──────────────────────────────────────
        int extent[6];
        double origin[3];
        double spacing[3];
        cubeImage->GetExtent(extent);
        cubeImage->GetOrigin(origin);
        cubeImage->GetSpacing(spacing);
        const int zCount = extent[5] - extent[4] + 1;

        const auto regionBounds =
                regionBounds2D(this->regionMode, this->regionAnchorVoxel, this->regionCurrentVoxel,
                               this->regionPolygonVertices);

        QVector<double> spectral(zCount);
        QVector<double> meanValues(zCount, 0.0);
        QVector<int>    counts(zCount, 0);

        // Clamp the 2D loop to the actually-available extent to avoid
        // unnecessary iterations.
        const int xMin = std::max(extent2d[0], regionBounds[0]);
        const int xMax = std::min(extent2d[1], regionBounds[1]);
        const int yMin = std::max(extent2d[2], regionBounds[2]);
        const int yMax = std::min(extent2d[3], regionBounds[3]);

        for (int y = yMin; y <= yMax; ++y) {
            for (int x = xMin; x <= xMax; ++x) {
                if (!pointInRegion(this->regionMode, this->regionAnchorVoxel,
                                   this->regionCurrentVoxel, this->regionPolygonVertices,
                                   this->regionAnnulusInnerRadius, x, y)) {
                    continue;
                }

                // Convert 2D voxel index → physical world → cubeImage voxel.
                // Using the two-step transform makes the mapping correct even if
                // the datasets have different origins or spacings.
                const double physX = origin2d[0] + static_cast<double>(x - extent2d[0]) * spacing2d[0];
                const double physY = origin2d[1] + static_cast<double>(y - extent2d[2]) * spacing2d[1];
                const int localX = std::lround(extent[0] + (physX - origin[0]) / spacing[0]);
                const int localY = std::lround(extent[2] + (physY - origin[1]) / spacing[1]);

                if (localX < extent[0] || localX > extent[1]
                    || localY < extent[2] || localY > extent[3]) {
                    continue;
                }

                for (int localZ = extent[4]; localZ <= extent[5]; ++localZ) {
                    const int idx = localZ - extent[4];
                    const double value =
                            cubeImage->GetScalarComponentAsDouble(localX, localY, localZ, 0);
                    if (!std::isfinite(value)) {
                        continue;
                    }
                    // Build spectral axis value once per channel (idempotent).
                    if (counts[idx] == 0) {
                        const double datasetZ = origin[2] + static_cast<double>(localZ - extent[4])
                                                * spacing[2];
                        bool ok = false;
                        spectral[idx] = this->spectralAxisValue(datasetZ, &ok);
                        if (!ok) {
                            spectral[idx] = datasetZ;
                        }
                    }
                    meanValues[idx] += value;
                    counts[idx] += 1;
                }
            }
        }

        // Normalise: convert accumulated sums to per-channel means.
        // Channels with no valid pixels keep NaN so QCustomPlot skips them.
        bool haveSpectrum = false;
        for (int i = 0; i < zCount; ++i) {
            if (counts[i] > 0) {
                meanValues[i] /= static_cast<double>(counts[i]);
                haveSpectrum = true;
            } else {
                meanValues[i] = std::numeric_limits<double>::quiet_NaN();
                spectral[i]   = std::numeric_limits<double>::quiet_NaN();
            }
        }

        if (haveSpectrum) {
            const QString plotTitle =
                    u"Mean per channel — %1 region"_s.arg(shapeLabel);
            const bool freshWidget = !this->probePlotWidget;
            if (freshWidget) {
                this->probePlotWidget = new ProfileWidget(this);
                this->probePlotWidget->setBackend(this->remoteBackendUrl, this->remoteBackendToken);
                this->probePlotWidget->setUsageMode(
                        ProfileWidget::UsageMode::RegionStatic, plotTitle);
                this->probePlotWidget->setupSpectrumPlot(
                        this->spectralAxisTitle(), u"Mean value"_s);
                this->probePlotWidget->applyPlotTheme(this->m_darkPlots);
                this->pushSpectralAxisContext(this->probePlotWidget);
            } else {
                this->probePlotWidget->setUsageMode(
                        ProfileWidget::UsageMode::RegionStatic, plotTitle);
            }
            // Header: describe the region (no Live badge, no pixel hint).
            // Keep the slice/cube scope wording in sync with the result
            // dialog so users don't get conflicting context from the two
            // views of the same computation.
            const QString scopeLine = this->viewingSlice()
                    ? u"2D stats: current slice  ·  spectrum: full cube"_s
                    : u"2D stats: current moment map  ·  spectrum: full cube"_s;
            this->probePlotWidget->setRegionContext(
                    shapeLabel, stats.validCount, stats.totalCount, scopeLine);
            // Show a single Mean-per-channel series.
            // Sum is intentionally omitted: its scale (N × pixel_value) is
            // incompatible with Mean on a shared Y axis and causes misleading spikes.
            this->probePlotWidget->updateSpectrumPlot(
                    spectral, meanValues, plotTitle, /*live=*/false);

            // #5: register the region spectrum as a first-class 1-D product (SPEC in Session
            // Data, openable in any pane), so it no longer lives ONLY inside the shared plot.
            {
                const int rn = ++this->m_pinnedSpectrumCount;
                const QString pid = this->scopedProductId(QStringLiteral("spectrum%1").arg(rn));
                const QString label = QStringLiteral("%1 spectrum").arg(shapeLabel);
                Spectrum1D sp;
                sp.key = spectral;
                sp.values = meanValues;
                sp.xLabel = this->spectralAxisTitle().isEmpty() ? u"Spectral axis"_s
                                                                : this->spectralAxisTitle();
                sp.yLabel = u"Mean value"_s;
                sp.label = label;
                this->m_products1d.insert(pid, sp);
                // Fold the 2D region statistics into the product's Provenance (they used to live
                // in a separate floating ToolResultDialog). registerAnalysisProduct reveals the
                // Inspector's Provenance tab, so the stats surface there the moment the region is
                // drawn — no floating window. The spatial stats are the region's mean/min/max/…
                // on the current slice or moment map; the spectrum (the pane) is the mean per
                // channel across the full cube.
                const QString statsScope = this->viewingSlice()
                        ? u"current 2D slice"_s : u"current moment map"_s;
                QVariantMap params;
                params.insert(u"Region"_s, shapeLabel);
                params.insert(u"2D stats from"_s, statsScope);
                params.insert(u"Valid pixels"_s,
                              u"%1 / %2"_s.arg(stats.validCount).arg(stats.totalCount));
                if (stats.valid) {
                    params.insert(u"Blanked / NaN"_s, QString::number(stats.blankedCount));
                    params.insert(u"Min"_s,    fmt(stats.minValue));
                    params.insert(u"Max"_s,    fmt(stats.maxValue));
                    params.insert(u"Mean"_s,   fmt(stats.mean));
                    params.insert(u"Median"_s, fmt(stats.median));
                    params.insert(u"Std Dev"_s, fmt(stats.stddev));
                }
                this->registerAnalysisProduct(u"spectrum"_s, label, params, this, pid,
                                              u"spectrum1d"_s);
                // SHOW it. The region spectrum used to be visible only by hand — via
                // the fixed "Spectrum" pane entry (which drew the shared plot above,
                // not this product) or by picking the SPEC row in a pane's ▾ menu. It
                // is a result the user just asked for, so it is placed like any other:
                // free pane → grow the layout → else it waits in Session Data. Never
                // borrowed: a region spectrum is a permanent product.
                regionSpectrumPane = this->acquirePaneForProduct(pid, /*mayBorrow=*/false);
                this->refreshPaneChrome();   // tree + pane dropdowns pick it up
                producedProduct = true;
            }
            // Region overlay must persist until the spectrum window closes,
            // not just until the next mouse-release. Hook a one-shot cleanup
            // that fires when the user closes the plot. See refreshRegionOverlay
            // for the "keep visible while widget exists" branch.
            // QPointer is NOT auto-nulled before destroyed() fires (the guard
            // clear runs later in ~QObject), so the lambda explicitly nulls
            // it BEFORE calling maybeClearRegionAfterDisplay — otherwise
            // the maybeClear check would still see probePlotWidget non-null
            // and never wipe the overlay.
            if (freshWidget) {
                QObject::connect(this->probePlotWidget, &QObject::destroyed, this,
                                 [this]() {
                                     this->probePlotWidget = nullptr;
                                     this->maybeClearRegionAfterDisplay();
                                 });
            }
        }
    }

    // The 2D region statistics no longer open a floating ToolResultDialog. When a SPEC product
    // was produced (the common cube path) the full stats live in its Provenance and its spectrum
    // plot keeps the overlay alive; the status bar just points there. Otherwise (empty region,
    // or no cube spectrum) there is NO product and NO plot to own the overlay — report the
    // outcome inline and wipe the just-drawn overlay so it can't linger under a stale plot.
    if (producedProduct) {
        this->statusBar()->showMessage(
                (regionSpectrumPane >= 0
                         ? u"%1 region: mean %2 · median %3 · %4/%5 px — spectrum in pane %6, "
                           u"full stats in Inspector › Provenance."_s
                         : u"%1 region: mean %2 · median %3 · %4/%5 px — no free pane for the "
                           u"spectrum (pick it from a pane's ▾ menu); full stats in "
                           u"Inspector › Provenance.%6"_s)
                        .arg(shapeLabel, fmt(stats.mean), fmt(stats.median))
                        .arg(stats.validCount)
                        .arg(stats.totalCount)
                        .arg(regionSpectrumPane >= 0 ? QString::number(regionSpectrumPane + 1)
                                                     : QString()),
                6000);
    } else {
        if (!stats.valid)
            this->statusBar()->showMessage(
                    u"Region Analysis: no valid pixels in the selected region "
                    u"(all blanked / NaN)."_s,
                    5000);
        else
            this->statusBar()->showMessage(
                    u"%1 region: mean %2 · median %3 · %4/%5 px (no cube spectrum "
                    u"to attach)."_s.arg(shapeLabel, fmt(stats.mean), fmt(stats.median))
                            .arg(stats.validCount)
                            .arg(stats.totalCount),
                    6000);
        // No result to inspect → don't leave an overlay a previous region's still-open plot
        // would otherwise keep visible; clear it now (a new analysis re-draws its own).
        this->clearRegion();
        if (this->active2dWin())
            this->active2dWin()->Render();
    }
}

void vtkWindowCube::maybeClearRegionAfterDisplay()
{
    // Called from the region spectrum plot's destroyed() signal (the 2D stats no longer open a
    // separate window). While the plot is open the overlay stays; once it closes we wipe the 2D
    // overlay. QPointer auto-nulls on destruction so the check below is race-free.
    if (this->probePlotWidget) return;
    if (this->regionMode != RegionMode::None) return;  // user re-armed a tool
    this->clearRegion();
    if (ui->vtkImage && this->active2dWin()) {
        this->active2dWin()->Render();
    }
}

bool vtkWindowCube::finalizePolygonRegion()
{
    const int *position = this->active2dWin()->GetInteractor()->GetEventPosition();
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
    if (this->actionPolygonRegion && this->actionPolygonRegion->isChecked()) {
        this->actionPolygonRegion->setChecked(false);
    }
    return true;
}
