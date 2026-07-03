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

using namespace Qt::StringLiterals;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

// ── Probe ──
// Extracted verbatim from vtkWindowImage.cpp. Cursor probe: value/coord readout + spectrum-at-pixel.

void vtkWindowImage::mouseCallback()
{
    if (this->isBusy()) {
        return;
    }

    if (this->suspendHeavyOverlayUpdates) {
        return;
    }

    if (this->probeModeActive && this->probeFrozen) {
        return;
    }

    const int *position = ui->vtk->renderWindow()->GetInteractor()->GetEventPosition();
    if (!position) {
        return;
    }
    this->refreshCatalogueOverlayInteraction(position[0], position[1], false);
    this->updateProbeFromDisplayPosition(position[0], position[1]);
    if (this->regionMode != RegionMode::None && this->regionDragging) {
        if (this->updateRegionFromDisplayPosition(position[0], position[1])) {
            ui->vtk->renderWindow()->Render();
        }
    }

    // Live arrow preview while placing an annotation
    if (this->m_annotPlaceMode == AnnotPlaceMode::ArrowLabel) {
        auto *ren = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer();
        if (ren) {
            this->coordinate->SetValue(position[0], position[1]);
            const double *wc = this->coordinate->GetComputedWorldValue(ren);
            if (wc) {
                this->updateAnnotationPreview(wc[0], wc[1]);
                ui->vtk->renderWindow()->Render();
            }
        }
    }
}

void vtkWindowImage::beginImageInteraction()
{
    if (!this->largeImageModeActive) {
        return;
    }
    if (this->probeModeActive || this->regionMode != RegionMode::None) {
        return;
    }
    this->suspendHeavyOverlayUpdates = true;
    qDebug().noquote() << QStringLiteral("[large-image] begin interactive drag; suspending hover/overlay updates");
}

void vtkWindowImage::endImageInteraction()
{
    if (!this->suspendHeavyOverlayUpdates) {
        return;
    }
    this->suspendHeavyOverlayUpdates = false;
    qDebug().noquote() << QStringLiteral("[large-image] end interactive drag; refreshing overlays");
    this->invalidateWcsOverlayCache();
    this->requestWcsOverlayRender();
}

void vtkWindowImage::toggleProbeFreeze()
{
    if (this->isBusy()) {
        return;
    }

    const int *position = ui->vtk->renderWindow()->GetInteractor()->GetEventPosition();
    if (!position) {
        return;
    }

    // ── Annotation placement click ────────────────────────────────────────
    if (this->m_annotPlaceMode != AnnotPlaceMode::None) {
        auto *annotRen = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer();
        if (annotRen) {
            this->coordinate->SetValue(position[0], position[1]);
            const double *wc = this->coordinate->GetComputedWorldValue(annotRen);
            if (wc) {
                this->placeAnnotationAt(wc[0], wc[1]);
                ui->vtk->renderWindow()->Render();
            }
        }
        return;
    }

    // ── Measurement mode click ──────────────────────────────────────────
    if (this->m_measMode != MeasurementMode::None) {
        auto *measRenderer = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer();
        auto *measImage = this->layers
                ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
        if (measRenderer && measImage) {
            this->coordinate->SetValue(position[0], position[1]);
            const double *wc = this->coordinate->GetComputedWorldValue(measRenderer);
            if (wc) {
                int ext[6]; double ori[3]; double sp[3];
                measImage->GetExtent(ext);
                measImage->GetOrigin(ori);
                measImage->GetSpacing(sp);
                const int vx = std::lround(ext[0] + (wc[0] - ori[0]) / sp[0]);
                const int vy = std::lround(ext[2] + (wc[1] - ori[1]) / sp[1]);
                if (vx >= ext[0] && vx <= ext[1] && vy >= ext[2] && vy <= ext[3])
                    this->addMeasurementPoint(vx, vy);
            }
        }
        return;
    }

    if (this->catalogueOverlayLoaded && this->regionMode == RegionMode::None) {
        this->refreshCatalogueOverlayInteraction(position[0], position[1], true);
        if (this->selectedCatalogueSourceIndex >= 0) {
            ui->vtk->renderWindow()->Render();
            return;
        }
    }

    if (this->regionMode != RegionMode::None) {
        if (this->updateRegionFromDisplayPosition(position[0], position[1])) {
            if (this->regionMode == RegionMode::Polygon) {
                if (!this->regionDragging) {
                    this->regionPolygonVertices.clear();
                    this->regionPolygonVertices.push_back(this->regionCurrentVoxel);
                    this->regionDragging = true;
                    this->ignoreNextPolygonRelease = true;
                } else if (this->regionPolygonVertices.size() >= 3
                           && distance2d(this->regionCurrentVoxel, this->regionPolygonVertices.front())
                                   <= polygonClosureTolerance) {
                    this->regionCurrentVoxel = this->regionPolygonVertices.front();
                    this->ignoreNextPolygonRelease = false;
                    if (this->finalizePolygonRegion()) {
                        ui->vtk->renderWindow()->Render();
                    }
                    return;
                } else if (this->regionPolygonVertices.empty()
                           || this->regionPolygonVertices.back() != this->regionCurrentVoxel) {
                    this->regionPolygonVertices.push_back(this->regionCurrentVoxel);
                    this->ignoreNextPolygonRelease = true;
                }
                this->regionValid = !this->regionPolygonVertices.empty();
            } else {
                this->regionAnchorVoxel = this->regionCurrentVoxel;
                this->regionDragging = true;
                this->regionValid = true;
            }
            this->refreshRegionOverlay();
            ui->vtk->renderWindow()->Render();
        }
        return;
    }

    if (!this->probeModeActive) {
        return;
    }

    if (!this->probeFrozen) {
        if (this->updateProbeFromDisplayPosition(position[0], position[1])) {
            this->probeFrozen = true;
        }
    } else {
        this->probeFrozen = false;
        this->updateProbeFromDisplayPosition(position[0], position[1]);
    }
}

bool vtkWindowImage::updateProbeFromDisplayPosition(int displayX, int displayY)
{
    auto *renderer = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer();
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!renderer || !imageData) {
        this->clearProbe();
        return false;
    }

    this->coordinate->SetValue(displayX, displayY);
    const double *worldCoord = this->coordinate->GetComputedWorldValue(renderer);
    if (!worldCoord) {
        this->clearProbe();
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
        if (!this->probeFrozen) {
            this->clearProbe();
        }
        return false;
    }

    // Full-resolution pixel coordinate from the CONTINUOUS world position (world
    // coords == full-res pixel coords via the image spacing). On a decimated
    // display (spacing>1) the displayed voxel grid is coarser than full-res, so
    // track the readout + change-detection at full-res granularity — otherwise
    // the coordinates stall within one decimated pixel when zoomed in.
    // remoteVoxelToWcs expects a full-res pixel index. No-op for a full-res image
    // (spacing 1, origin 0 → worldCoord == voxel).
    const double fullResX = worldCoord[0] - origin[0];
    const double fullResY = worldCoord[1] - origin[1];
    const int fullXi = std::lround(fullResX);
    const int fullYi = std::lround(fullResY);
    if (this->probeValid && this->m_probeFullPixel[0] == fullXi
        && this->m_probeFullPixel[1] == fullYi) {
        return true;
    }

    this->probeValid = true;
    this->m_probeFullPixel = { fullXi, fullYi };
    this->probeVoxel = { voxelX, voxelY };  // display voxel — value lookup + crosshair/profile

    const float value = this->layers->getPixelValue(this->layers->getMasterIndex(), voxelX, voxelY);
    QString valueText = std::isfinite(value) ? QString::number(value, 'g', 8) : u"NaN"_s;
    QString message = u"X=%1  Y=%2  Value=%3"_s.arg(fullXi).arg(fullYi).arg(valueText);
    {
        QString axis0 = this->remoteFormatAxisCoordinate(0, fullResX);
        QString axis1 = this->remoteFormatAxisCoordinate(1, fullResY);
        if (this->remoteHasCelestialAxes()) {
            bool ok0 = false;
            bool ok1 = false;
            const double nativeX = this->remoteVoxelToWcs(0, fullResX, &ok0);
            const double nativeY = this->remoteVoxelToWcs(1, fullResY, &ok1);
            double frameX = nativeX;
            double frameY = nativeY;
            if (ok0 && ok1 && this->convertRemoteCelestialCoordinates(nativeX, nativeY, frameX, frameY)) {
                axis0 = this->formatRemoteOverlayCoordinate(0, frameX);
                axis1 = this->formatRemoteOverlayCoordinate(1, frameY);
            }
        }
        message += u"  %1=%2  %3=%4"_s.arg(this->selectedFrameAxisTitle(0),
                                           axis0,
                                           this->selectedFrameAxisTitle(1),
                                           axis1);
    }
    if (this->probeFrozen) {
        message += u"  [Frozen]"_s;
    }
    if (this->hoverReadoutLabel) {
        this->hoverReadoutLabel->setText(message);
        this->hoverReadoutLabel->setToolTip(message);
    }
    if (this->probeModeActive) {
        this->refreshProbeOverlay();
        this->updateProbeProfile();
        ui->vtk->renderWindow()->Render();
    }
    return true;
}

void vtkWindowImage::refreshProbeOverlay()
{
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!imageData || !this->probeValid || !this->probeModeActive) {
        this->probeHorizontalActor->VisibilityOff();
        this->probeVerticalActor->VisibilityOff();
        return;
    }

    double bounds[6];
    imageData->GetBounds(bounds);
    // Crosshair position is in WORLD coordinates (== full-res pixel via spacing),
    // so use the full-res probe coordinate, not the decimated display voxel.
    // No-op for a full-res image (m_probeFullPixel == probeVoxel).
    const double cx = static_cast<double>(this->m_probeFullPixel[0]);
    const double cy = static_cast<double>(this->m_probeFullPixel[1]);
    this->probeHorizontalLine->SetPoint1(bounds[0], cy, 0.0);
    this->probeHorizontalLine->SetPoint2(bounds[1], cy, 0.0);
    this->probeVerticalLine->SetPoint1(cx, bounds[2], 0.0);
    this->probeVerticalLine->SetPoint2(cx, bounds[3], 0.0);
    this->probeHorizontalActor->VisibilityOn();
    this->probeVerticalActor->VisibilityOn();
}

void vtkWindowImage::clearProbe()
{
    this->probeValid = false;
    if (this->hoverReadoutLabel) {
        this->hoverReadoutLabel->clear();
    }
    this->probeHorizontalActor->VisibilityOff();
    this->probeVerticalActor->VisibilityOff();
}

void vtkWindowImage::setProbeModeActive(bool active)
{
    this->probeModeActive = active;
    if (active) {
        if (this->actionBoxRegion && this->actionBoxRegion->isChecked()) {
            this->actionBoxRegion->setChecked(false);
        }
        if (this->actionCircleRegion && this->actionCircleRegion->isChecked()) {
            this->actionCircleRegion->setChecked(false);
        }
        if (this->actionPolygonRegion && this->actionPolygonRegion->isChecked()) {
            this->actionPolygonRegion->setChecked(false);
        }
        if (this->actionAnnulusRegion && this->actionAnnulusRegion->isChecked()) {
            this->actionAnnulusRegion->setChecked(false);
        }
    }
    ui->vtk->setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    this->probeFrozen = false;
    if (!active) {
        this->probeHorizontalActor->VisibilityOff();
        this->probeVerticalActor->VisibilityOff();
        if (this->profileWidget) {
            this->profileWidget->close();
        }
        this->vtkRender();
        return;
    }

    if (!this->profileWidget) {
        this->profileWidget = new ProfileWidget(this);
        this->profileWidget->setUsageMode(ProfileWidget::UsageMode::ProbeLive, u"Profile"_s);
        // Label the profile value axis with the physical unit (BUNIT) when known.
        this->profileWidget->setupImagePlots(
                u"Pixel"_s,
                this->remoteDatasetBunit.isEmpty() ? u"Value"_s : this->remoteDatasetBunit);
        QObject::connect(this->profileWidget, &ProfileWidget::destroyed, this, [this]() {
            this->profileWidget = nullptr;
            if (ui->actionProfile->isChecked()) {
                ui->actionProfile->setChecked(false);
            }
        });
    } else {
        this->profileWidget->setUsageMode(ProfileWidget::UsageMode::ProbeLive, u"Profile"_s);
    }

    if (!this->probeValid) {
        auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
        if (imageData) {
            int extent[6];
            imageData->GetExtent(extent);
            this->probeValid = true;
            this->probeVoxel = { (extent[0] + extent[1]) / 2, (extent[2] + extent[3]) / 2 };
        }
    }
    this->refreshProbeOverlay();
    this->updateProbeProfile();
    this->profileWidget->show();
    this->profileWidget->raise();
    this->vtkRender();
}

void vtkWindowImage::updateProbeProfile()
{
    if (!this->probeModeActive || !this->probeValid || !this->profileWidget || !this->layers) {
        return;
    }

    auto *imageData = this->layers->getImageData(this->layers->getMasterIndex());
    if (!imageData) {
        return;
    }
    int extent[6];
    double origin[3];
    double spacing[3];
    imageData->GetExtent(extent);
    imageData->GetOrigin(origin);
    imageData->GetSpacing(spacing);
    if (this->probeVoxel[0] < extent[0] || this->probeVoxel[0] > extent[1] || this->probeVoxel[1] < extent[2]
        || this->probeVoxel[1] > extent[3]) {
        return;
    }

    // Profile is sampled over the DISPLAYED (possibly decimated) image, but the
    // X/Y axes and marker are labelled in full-resolution pixels (world coords)
    // so they match the readout. No-op for a full-res image (spacing 1).
    const int width = extent[1] - extent[0] + 1;
    const int height = extent[3] - extent[2] + 1;
    QVector<double> keyX(width), valuesX(width), keyY(height), valuesY(height);
    for (int x = extent[0]; x <= extent[1]; ++x) {
        const int idx = x - extent[0];
        keyX[idx] = origin[0] + (x - extent[0]) * spacing[0];
        valuesX[idx] = this->layers->getPixelValue(this->layers->getMasterIndex(), x, this->probeVoxel[1]);
    }
    for (int y = extent[2]; y <= extent[3]; ++y) {
        const int idx = y - extent[2];
        keyY[idx] = origin[1] + (y - extent[2]) * spacing[1];
        valuesY[idx] = this->layers->getPixelValue(this->layers->getMasterIndex(), this->probeVoxel[0], y);
    }
    this->profileWidget->updateImageProfiles(keyX, valuesX, keyY, valuesY,
                                             this->m_probeFullPixel[0], this->m_probeFullPixel[1],
                                             !this->probeFrozen);
}
