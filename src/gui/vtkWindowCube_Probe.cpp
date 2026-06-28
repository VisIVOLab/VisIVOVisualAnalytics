#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"
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
using namespace CubeGeom;
using namespace CubeStats;

// ── Spectral probe / cursor profile ──────────────────────────────────────────
// Extracted verbatim from vtkWindowCube.cpp. Cursor-driven spectral probe:
// pick a voxel, read out value/coords, and plot the spectrum at that pixel.
// distance2d / polygonClosureTolerance come from CubeGeom (shared util).

void vtkWindowCube::mouseCallback()
{
    if (this->isBusy()) {
        return;
    }

    if (this->probeModeActive && this->probeFrozen) {
        return;
    }

    const int *position = ui->vtkImage->renderWindow()->GetInteractor()->GetEventPosition();
    if (!position) {
        return;
    }
    this->refreshCatalogueOverlayInteraction(position[0], position[1], false);
    this->updateProbeFromDisplayPosition(position[0], position[1]);
    if (this->pvModeActive) {
        if (this->updatePvFromDisplayPosition(position[0], position[1]) && this->pvDragging) {
            ui->vtkImage->renderWindow()->Render();
        }
        return;
    }
    if (this->regionMode != RegionMode::None && this->regionDragging) {
        if (this->updateRegionFromDisplayPosition(position[0], position[1])) {
            ui->vtkImage->renderWindow()->Render();
        }
    }
}

void vtkWindowCube::toggleProbeFreeze()
{
    if (this->isBusy()) {
        return;
    }

    const int *position = ui->vtkImage->renderWindow()->GetInteractor()->GetEventPosition();
    if (!position) {
        return;
    }

    if (this->catalogueOverlayLoaded && !this->pvModeActive && this->regionMode == RegionMode::None) {
        this->refreshCatalogueOverlayInteraction(position[0], position[1], true);
        if (this->selectedCatalogueSourceIndex >= 0) {
            this->sliceWin->Render();
            this->momentWin->Render();
            return;
        }
    }

    if (this->pvModeActive) {
        if (this->updatePvFromDisplayPosition(position[0], position[1])) {
            if (!this->pvDragging) {
                this->pvPolylineVertices.clear();
                this->pvPolylineVertices.push_back(this->pvCurrentVoxel);
                this->pvDragging = true;
            } else if (this->pvPolylineVertices.empty()
                       || this->pvPolylineVertices.back() != this->pvCurrentVoxel) {
                this->pvPolylineVertices.push_back(this->pvCurrentVoxel);
            } else {
                this->statusBar()->showMessage(u"PV vertex unchanged; move and click to add a new point."_s,
                                               1500);
            }
            this->pvValid = this->pvPolylineVertices.size() >= 1;
            this->refreshPvOverlay();
            ui->vtkImage->renderWindow()->Render();
        }
        return;
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
                        ui->vtkImage->renderWindow()->Render();
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
            ui->vtkImage->renderWindow()->Render();
        }
        return;
    }

    if (!this->probeModeActive) {
        return;
    }

    // First click pins at the clicked pixel, second click unpins and resumes
    // live updates.
    if (!this->probeFrozen) {
        if (this->updateProbeFromDisplayPosition(position[0], position[1])) {
            this->probeFrozen = true;
        }
    } else {
        this->probeFrozen = false;
        this->updateProbeFromDisplayPosition(position[0], position[1]);
    }
    // The spectrum draw inside updateProbeFromDisplayPosition() ran with the
    // OLD probeFrozen value (we hadn't toggled it yet), so the header badge
    // would show the stale "Live"/"Pinned" until the next hover/click. Push
    // the now-current state so the badge flips on this very click instead.
    if (this->probePlotWidget) {
        this->probePlotWidget->setSpectrumFrozen(this->probeFrozen);
    }
}

bool vtkWindowCube::updateProbeFromDisplayPosition(int displayX, int displayY)
{
    auto *renderer = ui->vtkImage->renderWindow() ? ui->vtkImage->renderWindow()->GetRenderers()->GetFirstRenderer()
                                                  : nullptr;
    auto *imageData = this->viewingSlice()
            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
            : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
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
    const int voxelZ = this->clampRemoteSliceIndex(ui->spinSlice->value() - 1);
    if (voxelX < extent[0] || voxelX > extent[1] || voxelY < extent[2] || voxelY > extent[3]) {
        if (!this->probeFrozen) {
            this->clearProbe();
        }
        return false;
    }

    if (this->probeValid && this->probeVoxel[0] == voxelX && this->probeVoxel[1] == voxelY
        && this->probeVoxel[2] == voxelZ) {
        return true;
    }

    this->probeValid = true;
    this->probeVoxel = { voxelX, voxelY, voxelZ };
    this->updateProbeReadout(imageData);
    if (this->probeModeActive) {
        this->refreshProbeOverlay();
        this->updateProbePlot();
        ui->vtkImage->renderWindow()->Render();
    }
    return true;
}

void vtkWindowCube::refreshProbeOverlay()
{
    const auto updateActors = [this](vtkImageData *imageData, vtkLineSource *horizontal,
                                     vtkLineSource *vertical, vtkActor *horizontalActor,
                                     vtkActor *verticalActor) {
        if (!imageData || !this->probeValid || !this->probeModeActive) {
            horizontalActor->VisibilityOff();
            verticalActor->VisibilityOff();
            return;
        }
        double bounds[6];
        imageData->GetBounds(bounds);
        horizontal->SetPoint1(bounds[0], static_cast<double>(this->probeVoxel[1]), 0.0);
        horizontal->SetPoint2(bounds[1], static_cast<double>(this->probeVoxel[1]), 0.0);
        vertical->SetPoint1(static_cast<double>(this->probeVoxel[0]), bounds[2], 0.0);
        vertical->SetPoint2(static_cast<double>(this->probeVoxel[0]), bounds[3], 0.0);
        horizontalActor->VisibilityOn();
        verticalActor->VisibilityOn();
    };

    updateActors(vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0)),
                 this->sliceProbeHorizontalLine, this->sliceProbeVerticalLine,
                 this->sliceProbeHorizontalActor, this->sliceProbeVerticalActor);
    updateActors(vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0)),
                 this->momentProbeHorizontalLine, this->momentProbeVerticalLine,
                 this->momentProbeHorizontalActor, this->momentProbeVerticalActor);
}

void vtkWindowCube::clearProbe()
{
    this->probeValid = false;
    if (this->hoverReadoutLabel) {
        this->hoverReadoutLabel->clear();
    }
    this->sliceProbeHorizontalActor->VisibilityOff();
    this->sliceProbeVerticalActor->VisibilityOff();
    this->momentProbeHorizontalActor->VisibilityOff();
    this->momentProbeVerticalActor->VisibilityOff();
}

void vtkWindowCube::updateProbeReadout(vtkImageData *imageData)
{
    if (!imageData || !this->probeValid) {
        return;
    }

    const float value = imageData->GetScalarComponentAsFloat(this->probeVoxel[0], this->probeVoxel[1], 0, 0);
    QString valueText = std::isfinite(value) ? QString::number(value, 'g', 8) : u"NaN"_s;
    QString message = u"X=%1  Y=%2  Z=%3  Value=%4"_s.arg(this->probeVoxel[0])
                              .arg(this->probeVoxel[1])
                              .arg(this->probeVoxel[2])
                              .arg(valueText);
    {
        QString axis0 = this->remoteFormatAxisCoordinate(0, this->probeVoxel[0]);
        QString axis1 = this->remoteFormatAxisCoordinate(1, this->probeVoxel[1]);
        if (this->remoteHasCelestialAxes()) {
            bool ok0 = false;
            bool ok1 = false;
            const double nativeX = this->remoteVoxelToWcs(0, this->probeVoxel[0], &ok0);
            const double nativeY = this->remoteVoxelToWcs(1, this->probeVoxel[1], &ok1);
            double frameX = nativeX;
            double frameY = nativeY;
            if (ok0 && ok1 && this->convertRemoteCelestialCoordinates(nativeX, nativeY, frameX, frameY)) {
                axis0 = this->formatRemoteOverlayCoordinate(0, frameX);
                axis1 = this->formatRemoteOverlayCoordinate(1, frameY);
            }
        }
        message += u"  %1=%2  %3=%4  %5=%6"_s.arg(this->selectedFrameAxisTitle(0),
                                                  axis0,
                                                  this->selectedFrameAxisTitle(1),
                                                  axis1,
                                                  this->spectralAxisTitle(),
                                                  this->formatSpectralAxisValue(this->probeVoxel[2]));
    }
    if (this->probeFrozen) {
        message += u"  [Frozen]"_s;
    }
    if (this->hoverReadoutLabel) {
        this->hoverReadoutLabel->setText(message);
        this->hoverReadoutLabel->setToolTip(message);
    }
}

void vtkWindowCube::updateProbePlot()
{
    if (!this->probeValid || !this->probeModeActive) {
        return;
    }

    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!cubeImage) {
        return;
    }

    int extent[6];
    cubeImage->GetExtent(extent);
    double origin[3];
    double spacing[3];
    cubeImage->GetOrigin(origin);
    cubeImage->GetSpacing(spacing);

    const int localX = std::lround(extent[0] + (this->probeVoxel[0] - origin[0]) / spacing[0]);
    const int localY = std::lround(extent[2] + (this->probeVoxel[1] - origin[1]) / spacing[1]);
    if (localX < extent[0] || localX > extent[1] || localY < extent[2] || localY > extent[3]) {
        return;
    }

    const int zCount = extent[5] - extent[4] + 1;
    QVector<double> spectral(zCount);
    QVector<double> values(zCount);
    for (int localZ = extent[4]; localZ <= extent[5]; ++localZ) {
        const int idx = localZ - extent[4];
        const double datasetZ = origin[2] + (localZ - extent[4]) * spacing[2];
        bool ok = false;
        spectral[idx] = this->spectralAxisValue(datasetZ, &ok);
        if (!ok) {
            spectral[idx] = datasetZ;
        }
        values[idx] = cubeImage->GetScalarComponentAsFloat(localX, localY, localZ, 0);
    }

    if (!this->probePlotWidget) {
        this->probePlotWidget = new ProfileWidget(this);
        this->probePlotWidget->setBackend(this->remoteBackendUrl, this->remoteBackendToken);
        const QString xLabel = this->spectralAxisTitle();
        const QString yLabel = u"Intensity"_s;
        this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::ProbeLive,
                                            u"Spectral Profile"_s);
        this->probePlotWidget->setupSpectrumPlot(xLabel.isEmpty() ? u"Spectral axis"_s : xLabel,
                                                 yLabel);
        this->probePlotWidget->applyPlotTheme(this->m_darkPlots);
        // The Pin button in the header owns the freeze state from the user's
        // side — propagate it back to the cube's probeFrozen flag so the
        // slice click no longer overwrites the pinned spectrum.
        QObject::connect(this->probePlotWidget, &ProfileWidget::freezeToggled,
                         this, [this](bool frozen) { this->probeFrozen = frozen; });
        // Capture the action as a QPointer so the destroyed handler is
        // safe at app-shutdown time: the cube viewer's ui-> members are
        // wiped in ~vtkWindowCube before QObject::deleteChildren()
        // destroys this ProfileWidget child, so dereferencing
        // ui->actionExtractSpectrum from the lambda crashes (use-after-
        // free on a null'd ui slot). QPointer goes null when the action
        // is destroyed; we simply skip the click-undo step in that case.
        QPointer<QAction> actExtract(ui->actionExtractSpectrum);
        QObject::connect(this->probePlotWidget, &ProfileWidget::destroyed, this,
                         [this, actExtract]() {
                             this->probePlotWidget = nullptr;
                             if (actExtract && actExtract->isChecked()) {
                                 actExtract->setChecked(false);
                             }
                             // The same probePlotWidget is reused for Region
                             // Analysis. If it was originally created by the
                             // probe path and later switched to RegionStatic
                             // mode, this is the only destroyed handler that
                             // gets installed — so it also has to release the
                             // 2D region overlay when both viewers close.
                             this->maybeClearRegionAfterDisplay();
                         });
    } else {
        this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::ProbeLive,
                                            u"Spectral Profile"_s);
    }

    // Push current context + freeze status into the header so meta row /
    // status badge stay in sync with the data this call is about to draw.
    // Build the WCS label using the same pipeline as the bottom status bar
    // (remoteVoxelToWcs → convertRemoteCelestialCoordinates →
    //  formatRemoteOverlayCoordinate) so the format honours the user's
    //  active sky frame (J2000 / Galactic / Ecliptic) and sexagesimal vs
    //  decimal choice. Falls back to pixel-axis labels if WCS is missing.
    QString wcsLabel;
    {
        QString axis0 = this->remoteFormatAxisCoordinate(0, this->probeVoxel[0]);
        QString axis1 = this->remoteFormatAxisCoordinate(1, this->probeVoxel[1]);
        if (this->remoteHasCelestialAxes()) {
            bool ok0 = false, ok1 = false;
            const double nativeX = this->remoteVoxelToWcs(0, this->probeVoxel[0], &ok0);
            const double nativeY = this->remoteVoxelToWcs(1, this->probeVoxel[1], &ok1);
            double frameX = nativeX, frameY = nativeY;
            if (ok0 && ok1
                && this->convertRemoteCelestialCoordinates(nativeX, nativeY, frameX, frameY)) {
                axis0 = this->formatRemoteOverlayCoordinate(0, frameX);
                axis1 = this->formatRemoteOverlayCoordinate(1, frameY);
            }
        }
        wcsLabel = u"%1=%2  %3=%4"_s.arg(this->selectedFrameAxisTitle(0), axis0,
                                         this->selectedFrameAxisTitle(1), axis1);
    }
    this->probePlotWidget->setSpectrumContext(
            QFileInfo(this->filepath).fileName(),
            this->probeVoxel[0], this->probeVoxel[1],
            wcsLabel);
    this->probePlotWidget->setSpectrumFrozen(this->probeFrozen);

    // Title element inside the plot: keep it short — the header band now
    // carries all the context (filename, pixel, WCS, status).
    this->probePlotWidget->updateSpectrumPlot(
            spectral, values,
            u"Spectral Profile"_s,
            !this->probeFrozen);

    // Draw / refresh the dashed vertical line marking the slice channel
    // currently shown in the 2D view, so the user can see where their
    // navigation lands on the spectrum without having to mentally
    // re-compute the channel→velocity mapping.
    {
        bool ok = false;
        const double sp = this->spectralAxisValue(ui->spinSlice->value() - 1, &ok);
        this->probePlotWidget->setSpectrumCurrentChannel(
                ok ? sp : std::numeric_limits<double>::quiet_NaN());
    }
}

void vtkWindowCube::extractSpectrumAtCurrentProbe()
{
    if (this->isBusy()) {
        return;
    }

    if (!this->probeValid) {
        this->statusBar()->showMessage(u"Probe a point in the 2D view first."_s, 2000);
        return;
    }

    this->updateProbePlot();
}

void vtkWindowCube::setProbeModeActive(bool active)
{
    this->probeModeActive = active;
    if (active) {
        if (this->actionExtractPvDiagram && this->actionExtractPvDiagram->isChecked()) {
            this->actionExtractPvDiagram->setChecked(false);
        }
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
    ui->vtkImage->setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    this->probeFrozen = false;

    if (!active) {
        this->sliceProbeHorizontalActor->VisibilityOff();
        this->sliceProbeVerticalActor->VisibilityOff();
        this->momentProbeHorizontalActor->VisibilityOff();
        this->momentProbeVerticalActor->VisibilityOff();
        if (this->probePlotWidget) {
            this->probePlotWidget->close();
        }
        if (ui->vtkImage->renderWindow()) {
            ui->vtkImage->renderWindow()->Render();
        }
        return;
    }

    if (!this->probeValid) {
        auto *imageData = this->viewingSlice()
                ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
                : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
        if (imageData) {
            int extent[6];
            imageData->GetExtent(extent);
            this->probeValid = true;
            this->probeVoxel = { (extent[0] + extent[1]) / 2, (extent[2] + extent[3]) / 2,
                                 this->clampRemoteSliceIndex(ui->spinSlice->value() - 1) };
        }
    }

    this->refreshProbeOverlay();
    auto *currentImage = this->viewingSlice()
            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
            : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    this->updateProbeReadout(currentImage);
    this->updateProbePlot();
}
