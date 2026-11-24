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
#include "Pane2DView.h"
#include "ProductOrigin.h"
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

// ── Region drawing (the draft) ───────────────────────────────────────────────
// ROI drawing (box/circle/polygon/annulus) and the draft's overlay. A finished
// draft joins the region list (vtkWindowCube_RegionList.cpp), which keeps,
// edits and analyses it. distance2d / polygonClosureTolerance come from
// CubeGeom (shared util).


void vtkWindowCube::finishRegionInteraction()
{
    if (this->m_pvEdit != PvEdit::None) {
        this->endPvEdit();   // a PV path drag ends here, busy or not
        return;
    }
    if (this->isBusy()) {
        return;
    }
    if (this->m_lassoEditActive) {
        return;   // lasso 2-D edit consumes the release
    }

    // A draft on a Product pane ends with that pane's release (#355), not
    // with one on the slice.
    if (this->regionDraftWin() != this->active2dWin()) {
        return;
    }
    this->finishRegionDraft();
}

void vtkWindowCube::finishRegionDraft()
{
    if (this->regionMode == RegionMode::None || !this->regionDragging) {
        return;
    }
    vtkRenderWindow *win = this->regionDraftWin();
    if (!win) {
        return;
    }

    if (this->regionMode == RegionMode::Polygon) {
        if (this->ignoreNextPolygonRelease) {
            this->ignoreNextPolygonRelease = false;
            return;
        }
        if (this->finalizePolygonRegion()) {
            win->Render();
        }
        return;
    }

    const int *position = win->GetInteractor()->GetEventPosition();
    if (position) {
        this->updateRegionFromDisplayPosition(position[0], position[1]);
    }
    if (this->regionMode == RegionMode::Annulus) {
        const double dx = static_cast<double>(this->regionCurrentVoxel[0] - this->regionAnchorVoxel[0]);
        const double dy = static_cast<double>(this->regionCurrentVoxel[1] - this->regionAnchorVoxel[1]);
        const double outerRadius = std::sqrt(dx * dx + dy * dy);
        // Live preview while the radius is typed — the same amber the crop and
        // mask box previews use, so it reads as "this is what you are setting",
        // and back to the region's own colour once accepted. A modal
        // QInputDialog::getDouble gave no feedback at all: you typed a radius
        // blind and found out afterwards (user request). Both panes carry their
        // own ring, so both are recoloured.
        const auto innerActors = { this->sliceRegionAnnulusInnerActor.Get(),
                                   this->momentRegionAnnulusInnerActor.Get() };
        for (vtkActor *a : innerActors)
            if (a)
                a->GetProperty()->SetColor(0.95, 0.65, 0.15);

        QInputDialog prompt(this);
        prompt.setWindowTitle(u"Annulus Inner Radius"_s);
        prompt.setLabelText(u"Inner radius (pixels), 0 to %1:"_s
                                    .arg(std::max(0.0, outerRadius), 0, 'f', 1));
        prompt.setInputMode(QInputDialog::DoubleInput);
        prompt.setDoubleRange(0.0, std::max(0.0, outerRadius));
        prompt.setDoubleDecimals(1);
        prompt.setDoubleValue(0.0);
        const auto showInner = [this, win](double r) {
            this->regionAnnulusInnerRadius = r;
            this->regionDragging = false;   // the overlay hides the ring while dragging
            this->regionValid = true;
            this->refreshRegionOverlay();
            win->Render();
        };
        showInner(0.0);
        QObject::connect(&prompt, &QInputDialog::doubleValueChanged, this, showInner);

        const bool accepted = prompt.exec() == QDialog::Accepted;
        const double innerRadius = prompt.doubleValue();
        for (vtkActor *a : innerActors)
            if (a)
                a->GetProperty()->SetColor(0.2, 0.9, 1.0);
        if (!accepted) {
            this->regionAnnulusInnerRadius = 0.0;
            this->refreshRegionOverlay();
            win->Render();
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
            win->Render();
            return;
        }
        this->regionAnnulusInnerRadius = innerRadius;
        this->regionDragging = false;
        this->regionValid = true;
        this->refreshRegionOverlay();
        win->Render();
    }
    // The finished draft joins the list, selected, and is measured on the
    // map it was drawn on.
    const int added = this->commitDraftRegion();
    if (added >= 0)
        this->measureNewRegion(added, win);
    win->Render();
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
            // Only an unfinished draft goes; finished regions are in the list.
            this->clearRegion();
            this->setInteractorStyleImage();
            if (auto *w2d = this->active2dWidget()) w2d->setCursor(this->probeModeActive ? Qt::CrossCursor : Qt::ArrowCursor);
            if (this->active2dWin()) {
                this->active2dWin()->Render();
            }
            this->renderRegionPanes();   // a draft on a map pane goes too
            this->refreshPaneActiveStyles();
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
    // Regions are drawn on the slice and on every sky map in a Product pane
    // (#355): only with neither on screen does a map pane become the slice.
    if (!this->slice2dVisible() && !this->anyRegionMapPaneVisible())
        this->ensureSliceForTools();
    this->regionMode = mode;
    this->regionDragging = false;
    this->regionValid = false;
    this->ignoreNextPolygonRelease = false;
    this->regionPolygonVertices.clear();
    this->regionAnnulusInnerRadius = 0.0;
    this->clearRegion();
    this->setInteractorStyleRegion();
    if (auto *w2d = this->active2dWidget()) w2d->setCursor(Qt::CrossCursor);
    // The panes the tool listens on wear the armed border and the crosshair.
    this->refreshPaneActiveStyles();
}

bool vtkWindowCube::updateRegionFromDisplayPosition(int displayX, int displayY)
{
    // In the voxels of the view the region is drawn on: the slice, or a
    // Product pane's map (#355).
    vtkRenderWindow *win = this->regionDraftWin();
    auto *renderer = win ? win->GetRenderers()->GetFirstRenderer() : nullptr;
    auto *imageData = this->regionPaneImage(win);
    if (!renderer || !imageData) {
        this->pvCursorValid = false;
        this->refreshPvOverlay();
        return false;
    }

    // A coordinate of its own: the shared one has its viewport pinned to the
    // slice's renderer (changeImageRenderer), and a vtkCoordinate with a
    // viewport set ignores the renderer it is handed, so a Product pane's
    // click was read with the slice's camera.
    vtkNew<vtkCoordinate> coordinate;
    coordinate->SetCoordinateSystemToDisplay();
    coordinate->SetValue(displayX, displayY);
    double worldCoord[3];
    {
        const double *w = coordinate->GetComputedWorldValue(renderer);
        if (!w) {
            this->pvCursorValid = false;
            this->refreshPvOverlay();
            return false;
        }
        std::copy(w, w + 3, worldCoord);
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

        // Region geometry is in DISPLAYED-voxel indices; the primitives below are
        // built straight in those numbers. That is only the same thing as world
        // coordinates while every 2-D image the cube shows has spacing 1 and
        // origin 0 - which is true today (slices and moment maps are both served
        // that way), and is exactly the assumption that bit the image viewer once
        // its images started arriving decimated. Carry the mapping on the actors
        // instead of relying on it: identity in the current case, correct if a
        // decimated slice or moment map is ever displayed.
        double ro[3], rs[3];
        int rext[6];
        imageData->GetOrigin(ro);
        imageData->GetSpacing(rs);
        imageData->GetExtent(rext);
        const double regionPosX = ro[0] - rext[0] * rs[0];
        const double regionPosY = ro[1] - rext[2] * rs[1];   // ext[2]: the Y index, not ext[1]
        for (vtkActor *a : { topActor, bottomActor, leftActor, rightActor, circleActor,
                             annulusOuterActor, annulusInnerActor, annulusFillActor,
                             polygonActor, polygonFillActor }) {
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

    // Each view draws the draft only when it is drawn there.
    vtkRenderWindow *draftWin = this->regionDraftWin();
    updateOverlay(draftWin == this->sliceWin.GetPointer()
                          ? vtkImageData::SafeDownCast(
                                    this->remoteSliceDisplaySource->GetOutputDataObject(0))
                          : nullptr,
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
    updateOverlay(draftWin == this->momentWin.GetPointer()
                          ? vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0))
                          : nullptr,
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

    // On a Product pane the draft is drawn as a region of a one-region list,
    // placed like the list there (RegionPanePlacement.h).
    const auto draft = this->draftDisplayRegion();
    for (int i = 0; i < 4; ++i) {
        Pane2DView *pv = m_paneViews[i];
        if (!pv || m_paneRegionHooked[i] != pv)
            continue;
        vtkRenderWindow *paneWin = pv->renderer()->GetRenderWindow();
        const auto placement = paneWin == draftWin ? this->regionPlacement(paneWin)
                                                   : std::optional<visivo::regions::MapPlacement>();
        if (!draft || !placement) {
            m_paneDraftOverlays[i].setVisible(false);
            continue;
        }
        visivo::regions::RegionList one;
        one.add(*draft);
        one.select(-1);   // drawn plain while it is drawn: no fill, no handles
        const visivo::regions::MapPlacement place = *placement;
        m_paneDraftOverlays[i].setVisible(true);
        m_paneDraftOverlays[i].update(
                one, 0.0, [place](const visivo::regions::Vec2 &p) { return place.toWorld(p); },
                /*withHandles=*/false);
    }
}

std::optional<visivo::regions::Region> vtkWindowCube::draftDisplayRegion() const
{
    if (this->regionMode == RegionMode::None || !this->regionValid)
        return std::nullopt;
    const visivo::DisplayGrid grid = this->regionDisplayGrid(this->regionDraftWin());
    if (this->regionMode == RegionMode::Polygon) {
        std::vector<std::array<int, 2>> vertices = this->regionPolygonVertices;
        if (this->regionDragging && (vertices.empty() || vertices.back() != this->regionCurrentVoxel))
            vertices.push_back(this->regionCurrentVoxel);
        if (!this->regionDragging && vertices.size() >= 3)
            return this->draftRegion();
        if (vertices.size() < 2)
            return std::nullopt;
        // Still open: the path so far, as a line through the vertices.
        visivo::regions::Region line;
        line.shape = visivo::regions::Shape::Line;
        for (const auto &v : vertices)
            line.points.push_back({ grid.sourceX(v[0]), grid.sourceY(v[1]) });
        return line;
    }
    if (this->regionMode == RegionMode::Annulus && this->regionDragging)
        // The outer ring while it is dragged; the inner one is asked for after.
        return visivo::regionFromDisplayed(u"circle"_s, grid, this->regionAnchorVoxel,
                                           this->regionCurrentVoxel, {}, 0.0);
    return this->draftRegion();
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
    // SetBounds takes (xmin,xmax, ymin,ymax, zmin,zmax) in the volume's local
    // coordinate frame — which is NOT the same as the full-res voxel indices
    // these bounds are expressed in. The 2-D slice above is served at full
    // resolution, but the 3-D volume on screen is very often a DECIMATED
    // preview (349x318x133 for a 1046x952x398 cube), so a box drawn at raw
    // voxel indices comes out ~3x too large and sits well outside the cube
    // outline. Map full-res index -> display world with the display's own
    // geometry, the same convention the Kinematic Lasso mask uses.
    // Remember what is being shown, so a preview -> full-resolution swap can
    // re-place the box instead of leaving it scaled for the display it was
    // drawn against.
    this->m_regionPreviewBounds = { int(xLo), int(xHi), int(yLo), int(yHi), int(zLo), int(zHi) };
    this->m_regionPreviewActive = true;
    // A high-resolution ROI load is already in full-res world coordinates (its
    // origin is the ROI corner, spacing 1) and cubeToDisplay is the identity
    // there; a preview places cube voxel c at (c - offset) / stride.
    this->noisePreviewCubeSource->SetBounds(this->cubeToDisplay(xLo), this->cubeToDisplay(xHi),
                                            this->cubeToDisplay(yLo), this->cubeToDisplay(yHi),
                                            this->cubeToDisplay(zLo), this->cubeToDisplay(zHi));
    this->noisePreviewCubeFaceActor->VisibilityOn();
    this->noisePreviewCubeEdgeActor->VisibilityOn();

    if (ui->vtkImage && this->active2dWin()) {
        this->active2dWin()->Render();
    }
    if (ui->vtkCube && ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }
}

void vtkWindowCube::refreshRegionPreviewAfterDisplaySwap()
{
    // The box was placed against the display that has just been replaced: its
    // bounds are full-res voxel indices, but the scaling from them is not.
    if (!this->m_regionPreviewActive) {
        return;
    }
    const auto b = this->m_regionPreviewBounds;
    this->setNoiseRegionPreview(b[0], b[1], b[2], b[3], b[4], b[5]);
}

void vtkWindowCube::clearNoiseRegionPreview()
{
    this->m_regionPreviewActive = false;
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
    // Clears the region being DRAWN (and its actors). The regions of the list
    // are not touched: they stay until the user deletes them.
    this->regionValid = false;
    this->regionDragging = false;
    this->ignoreNextPolygonRelease = false;
    this->regionPolygonVertices.clear();
    this->regionAnnulusInnerRadius = 0.0;
    this->m_regionDraftWin = nullptr;
    for (auto &overlay : m_paneDraftOverlays)
        overlay.setVisible(false);
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
}

bool vtkWindowCube::finalizePolygonRegion()
{
    vtkRenderWindow *win = this->regionDraftWin();
    if (!win || !win->GetInteractor())
        return false;
    const int *position = win->GetInteractor()->GetEventPosition();
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
    const int added = this->commitDraftRegion();
    if (added >= 0)
        this->measureNewRegion(added, win);
    if (this->actionPolygonRegion && this->actionPolygonRegion->isChecked()) {
        this->actionPolygonRegion->setChecked(false);
    }
    return true;
}
