#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"
#include "CubeViewportGeometry.h"

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

// ── Catalogue overlay ────────────────────────────────────────────────────────
// Extracted verbatim from vtkWindowCube.cpp. Loads/renders DS9/CSV source
// catalogues as an interactive overlay (table, labels, hover, selection).

namespace {
constexpr double pi = 3.14159265358979323846;  // local copy for buildEllipsePolyline
constexpr double catalogueMarkerHalfSize = 3.0;
constexpr int maxCatalogueLabelCount = 200;
constexpr int smartCatalogueLabelMaxVisible = 24;
constexpr double smartCatalogueLabelMinPixelsPerUnit = 2.5;
constexpr double catalogueHoverDisplayThresholdPx = 10.0;

QString catalogueFrameName(CatalogueOverlayEntry::Frame frame)
{
    return frame == CatalogueOverlayEntry::Frame::Image ? QStringLiteral("image")
                                                        : QStringLiteral("sky");
}

QString catalogueShapeName(CatalogueOverlayEntry::Shape shape)
{
    return shape == CatalogueOverlayEntry::Shape::Ellipse ? QStringLiteral("ellipse")
                                                          : QStringLiteral("marker");
}

std::vector<std::array<double, 2>> buildEllipsePolyline(double centerX, double centerY, double radiusX,
                                                        double radiusY, double angleDeg, int samples = 96)
{
    std::vector<std::array<double, 2>> polyline;
    if (radiusX <= 0.0 || radiusY <= 0.0 || samples < 8) {
        return polyline;
    }
    polyline.reserve(static_cast<std::size_t>(samples + 1));
    const double angleRad = angleDeg * pi / 180.0;
    const double cosA = std::cos(angleRad);
    const double sinA = std::sin(angleRad);
    for (int i = 0; i <= samples; ++i) {
        const double t = (2.0 * pi * static_cast<double>(i)) / static_cast<double>(samples);
        const double ex = radiusX * std::cos(t);
        const double ey = radiusY * std::sin(t);
        polyline.push_back(
                { centerX + ex * cosA - ey * sinA, centerY + ex * sinA + ey * cosA });
    }
    return polyline;
}
}  // namespace

void vtkWindowCube::loadCatalogueOverlay()
{
    const QString path = QFileDialog::getOpenFileName(
            this, u"Load Catalogue Overlay"_s, QString(),
            u"Catalogue files (*.reg *.csv);;DS9 region (*.reg);;CSV (*.csv)"_s);
    if (path.isEmpty()) {
        return;
    }

    const CatalogueOverlayParseResult parsed = CatalogueOverlayUtils::parseFile(path);
    if (!parsed.valid) {
        QMessageBox::warning(this, u"Catalogue Overlay"_s, parsed.errorMessage);
        return;
    }

    if (!this->remoteHasCelestialAxes()) {
        const bool hasImageEntries = std::any_of(
                parsed.entries.cbegin(), parsed.entries.cend(), [](const CatalogueOverlayEntry &entry) {
                    return entry.frame == CatalogueOverlayEntry::Frame::Image;
                });
        if (hasImageEntries) {
            // Image-frame DS9 entries do not require WCS.
        } else {
        QMessageBox::warning(this, u"Catalogue Overlay"_s,
                             u"Catalogue overlay requires celestial WCS information."_s);
        return;
        }
    }

    this->catalogueOverlayEntries.clear();
    this->catalogueOverlayPixels.clear();
    this->catalogueOverlayPolylines.clear();
    this->catalogueOverlaySourceFirstPolyline.clear();
    this->catalogueOverlaySourcePolylineCount.clear();
    this->catalogueOverlayLabels.clear();
    this->catalogueOverlayLabelIndices.clear();
    this->hoveredCatalogueSourceIndex = -1;
    this->selectedCatalogueSourceIndex = -1;
    int skippedProjection = 0;
    for (const CatalogueOverlayEntry &entry : parsed.entries) {
        CatalogueOverlayEntry storedEntry = entry;
        std::array<double, 2> anchor{ 0.0, 0.0 };
        const int firstPolyline = static_cast<int>(this->catalogueOverlayPolylines.size());
        if (entry.frame == CatalogueOverlayEntry::Frame::Image) {
            anchor = { entry.pixelX, entry.pixelY };
            if (entry.shape == CatalogueOverlayEntry::Shape::Ellipse) {
                auto ellipse = buildEllipsePolyline(entry.pixelX, entry.pixelY, entry.radiusX, entry.radiusY,
                                                    entry.angleDeg);
                if (ellipse.empty()) {
                    ++skippedProjection;
                    continue;
                }
                this->catalogueOverlayPolylines.push_back(std::move(ellipse));
            }
        } else {
            if (!this->catalogueWorldToPixel(entry.raDeg, entry.decDeg, anchor)) {
                ++skippedProjection;
                continue;
            }
            this->catalogueOverlayPolylines.push_back(
                    { { anchor[0] - catalogueMarkerHalfSize, anchor[1] },
                      { anchor[0] + catalogueMarkerHalfSize, anchor[1] } });
            this->catalogueOverlayPolylines.push_back(
                    { { anchor[0], anchor[1] - catalogueMarkerHalfSize },
                      { anchor[0], anchor[1] + catalogueMarkerHalfSize } });
        }
        storedEntry.pixelX = anchor[0];
        storedEntry.pixelY = anchor[1];
        this->catalogueOverlayEntries.push_back(storedEntry);
        this->catalogueOverlayPixels.push_back(anchor);
        this->catalogueOverlaySourceFirstPolyline.push_back(firstPolyline);
        this->catalogueOverlaySourcePolylineCount.push_back(
                static_cast<int>(this->catalogueOverlayPolylines.size()) - firstPolyline);
        this->catalogueOverlayLabels.push_back(entry.label);
    }

    if (this->catalogueOverlayPixels.empty()) {
        QMessageBox::warning(this, u"Catalogue Overlay"_s,
                             u"No sources could be projected onto the current cube WCS."_s);
        return;
    }

    this->catalogueOverlayLoaded = true;
    this->catalogueOverlaySummary =
            u"%1 sources from %2 (%3)"_s.arg(this->catalogueOverlayPixels.size())
                    .arg(parsed.sourceLabel, parsed.frameLabel);
    this->rebuildCatalogueOverlay();
    this->refreshCatalogueTable();
    this->updateCatalogueInfoPanel();
    if (this->actionShowCatalogueOverlay) {
        this->actionShowCatalogueOverlay->setEnabled(true);
        this->actionShowCatalogueOverlay->setChecked(true);
    }
    if (this->actionShowCatalogueLabels) {
        this->actionShowCatalogueLabels->setEnabled(true);
        this->actionShowCatalogueLabels->setChecked(true);   // labels on by default
    }
    if (this->actionClearCatalogueOverlay) {
        this->actionClearCatalogueOverlay->setEnabled(true);
    }
    this->setCatalogueOverlayVisible(true);
    this->showPersistentStatusMessage(
            u"Catalogue overlay loaded: %1 sources (%2)."_s.arg(this->catalogueOverlayPixels.size())
                    .arg(parsed.frameLabel),
            3000);
    if (skippedProjection > 0 || parsed.skippedEntries > 0) {
        qDebug().noquote() << QStringLiteral("[catalogue] loaded=%1 skipped_parse=%2 skipped_wcs=%3")
                                          .arg(this->catalogueOverlayPixels.size())
                                          .arg(parsed.skippedEntries)
                                          .arg(skippedProjection);
    }
    this->sliceWin->Render();
    this->momentWin->Render();
}

void vtkWindowCube::clearCatalogueOverlay()
{
    this->catalogueOverlayLoaded = false;
    this->catalogueOverlayEntries.clear();
    this->catalogueOverlayPixels.clear();
    this->catalogueOverlayPolylines.clear();
    this->catalogueOverlaySourceFirstPolyline.clear();
    this->catalogueOverlaySourcePolylineCount.clear();
    this->catalogueOverlayLabels.clear();
    this->catalogueOverlayLabelIndices.clear();
    this->catalogueOverlaySummary.clear();
    this->hoveredCatalogueSourceIndex = -1;
    this->selectedCatalogueSourceIndex = -1;
    this->catalogueOverlayPoints->Reset();
    this->catalogueOverlayCells->Reset();
    this->catalogueOverlayData->Modified();
    this->catalogueHoverOverlayPoints->Reset();
    this->catalogueHoverOverlayCells->Reset();
    this->catalogueHoverOverlayData->Modified();
    this->catalogueSelectionOverlayPoints->Reset();
    this->catalogueSelectionOverlayCells->Reset();
    this->catalogueSelectionOverlayData->Modified();
    if (auto *sliceRenderer = this->sliceWin->GetRenderers()->GetFirstRenderer()) {
        for (const auto &actor : this->sliceCatalogueOverlayLabelActors) {
            sliceRenderer->RemoveViewProp(actor);
        }
    }
    if (auto *momentRenderer = this->momentWin->GetRenderers()->GetFirstRenderer()) {
        for (const auto &actor : this->momentCatalogueOverlayLabelActors) {
            momentRenderer->RemoveViewProp(actor);
        }
    }
    this->sliceCatalogueOverlayLabelActors.clear();
    this->momentCatalogueOverlayLabelActors.clear();
    this->sliceCatalogueOverlayActor->VisibilityOff();
    this->momentCatalogueOverlayActor->VisibilityOff();
    this->sliceCatalogueHoverOverlayActor->VisibilityOff();
    this->momentCatalogueHoverOverlayActor->VisibilityOff();
    this->sliceCatalogueSelectionOverlayActor->VisibilityOff();
    this->momentCatalogueSelectionOverlayActor->VisibilityOff();
    if (this->actionShowCatalogueOverlay) {
        const QSignalBlocker blocker(this->actionShowCatalogueOverlay);
        this->actionShowCatalogueOverlay->setChecked(false);
        this->actionShowCatalogueOverlay->setEnabled(false);
    }
    if (this->actionShowCatalogueLabels) {
        const QSignalBlocker blocker(this->actionShowCatalogueLabels);
        // Unchecked, not checked. With the catalogue gone there are no labels to
        // show, and leaving it on gave a disabled toggle that reads as active —
        // the same state the constructor was fixed for, reintroduced on every
        // clear. loadCatalogueOverlay() re-checks it when one arrives.
        this->actionShowCatalogueLabels->setChecked(false);
        this->actionShowCatalogueLabels->setEnabled(false);
    }
    if (this->actionClearCatalogueOverlay) {
        this->actionClearCatalogueOverlay->setEnabled(false);
    }
    this->refreshCatalogueTable();
    this->updateCatalogueInfoPanel();
    this->sliceWin->Render();
    this->momentWin->Render();
}

void vtkWindowCube::setCatalogueOverlayVisible(bool visible)
{
    const bool effectiveVisible = visible && this->catalogueOverlayLoaded;
    this->sliceCatalogueOverlayActor->SetVisibility(effectiveVisible ? 1 : 0);
    this->momentCatalogueOverlayActor->SetVisibility(effectiveVisible ? 1 : 0);
    if (effectiveVisible) {
        this->updateCatalogueOverlayLabels();
    } else {
        for (const auto &actor : this->sliceCatalogueOverlayLabelActors) {
            actor->SetVisibility(0);
        }
        for (const auto &actor : this->momentCatalogueOverlayLabelActors) {
            actor->SetVisibility(0);
        }
        this->sliceCatalogueHoverOverlayActor->VisibilityOff();
        this->momentCatalogueHoverOverlayActor->VisibilityOff();
        this->sliceCatalogueSelectionOverlayActor->VisibilityOff();
        this->momentCatalogueSelectionOverlayActor->VisibilityOff();
    }
    this->sliceWin->Render();
    this->momentWin->Render();
}

void vtkWindowCube::rebuildCatalogueOverlay()
{
    this->catalogueOverlayPoints->Reset();
    this->catalogueOverlayCells->Reset();
    this->catalogueOverlayLabelIndices.clear();
    for (const auto &polyline : this->catalogueOverlayPolylines) {
        if (polyline.size() < 2) {
            continue;
        }
        this->catalogueOverlayCells->InsertNextCell(static_cast<vtkIdType>(polyline.size()));
        for (const auto &point : polyline) {
            const vtkIdType id = this->catalogueOverlayPoints->InsertNextPoint(point[0], point[1], 0.0);
            this->catalogueOverlayCells->InsertCellPoint(id);
        }
    }
    for (std::size_t i = 0; i < this->catalogueOverlayPixels.size(); ++i) {
        if (!this->catalogueOverlayLabels.value(static_cast<qsizetype>(i)).trimmed().isEmpty()
            && static_cast<int>(this->catalogueOverlayLabelIndices.size()) < maxCatalogueLabelCount) {
            this->catalogueOverlayLabelIndices.push_back(static_cast<int>(i));
        }
    }
    this->catalogueOverlayData->SetPoints(this->catalogueOverlayPoints);
    this->catalogueOverlayData->SetLines(this->catalogueOverlayCells);
    this->catalogueOverlayData->Modified();

    if (auto *sliceRenderer = this->sliceWin->GetRenderers()->GetFirstRenderer()) {
        for (const auto &actor : this->sliceCatalogueOverlayLabelActors) {
            sliceRenderer->RemoveViewProp(actor);
        }
        this->sliceCatalogueOverlayLabelActors.clear();
        for (int index : this->catalogueOverlayLabelIndices) {
            auto actor = vtkSmartPointer<vtkTextActor>::New();
            actor->SetInput(this->catalogueOverlayLabels.value(index).toUtf8().constData());
            actor->GetTextProperty()->SetColor(1.0, 0.65, 0.25);
            actor->GetTextProperty()->SetFontSize(12);
            actor->GetTextProperty()->SetShadow(true);
            actor->SetVisibility(this->actionShowCatalogueOverlay && this->actionShowCatalogueOverlay->isChecked());
            sliceRenderer->AddViewProp(actor);
            this->sliceCatalogueOverlayLabelActors.push_back(actor);
        }
    }
    if (auto *momentRenderer = this->momentWin->GetRenderers()->GetFirstRenderer()) {
        for (const auto &actor : this->momentCatalogueOverlayLabelActors) {
            momentRenderer->RemoveViewProp(actor);
        }
        this->momentCatalogueOverlayLabelActors.clear();
        for (int index : this->catalogueOverlayLabelIndices) {
            auto actor = vtkSmartPointer<vtkTextActor>::New();
            actor->SetInput(this->catalogueOverlayLabels.value(index).toUtf8().constData());
            actor->GetTextProperty()->SetColor(1.0, 0.65, 0.25);
            actor->GetTextProperty()->SetFontSize(12);
            actor->GetTextProperty()->SetShadow(true);
            actor->SetVisibility(this->actionShowCatalogueOverlay && this->actionShowCatalogueOverlay->isChecked());
            momentRenderer->AddViewProp(actor);
            this->momentCatalogueOverlayLabelActors.push_back(actor);
        }
    }
    this->rebuildCatalogueHighlightOverlay(this->catalogueHoverOverlayPoints,
                                           this->catalogueHoverOverlayCells,
                                           this->catalogueHoverOverlayData,
                                           this->hoveredCatalogueSourceIndex);
    this->rebuildCatalogueHighlightOverlay(this->catalogueSelectionOverlayPoints,
                                           this->catalogueSelectionOverlayCells,
                                           this->catalogueSelectionOverlayData,
                                           this->selectedCatalogueSourceIndex);
    this->sliceCatalogueHoverOverlayActor->SetVisibility(this->hoveredCatalogueSourceIndex >= 0 ? 1 : 0);
    this->momentCatalogueHoverOverlayActor->SetVisibility(this->hoveredCatalogueSourceIndex >= 0 ? 1 : 0);
    this->sliceCatalogueSelectionOverlayActor->SetVisibility(this->selectedCatalogueSourceIndex >= 0 ? 1 : 0);
    this->momentCatalogueSelectionOverlayActor->SetVisibility(this->selectedCatalogueSourceIndex >= 0 ? 1 : 0);
    this->updateCatalogueOverlayLabels();
}

void vtkWindowCube::updateCatalogueOverlayLabels()
{
    if (!this->catalogueOverlayLoaded || !this->actionShowCatalogueOverlay
        || !this->actionShowCatalogueOverlay->isChecked()) {
        for (const auto &actor : this->sliceCatalogueOverlayLabelActors) {
            actor->SetVisibility(0);
        }
        for (const auto &actor : this->momentCatalogueOverlayLabelActors) {
            actor->SetVisibility(0);
        }
        this->sliceCatalogueHoverOverlayActor->VisibilityOff();
        this->momentCatalogueHoverOverlayActor->VisibilityOff();
        this->sliceCatalogueSelectionOverlayActor->VisibilityOff();
        this->momentCatalogueSelectionOverlayActor->VisibilityOff();
        return;
    }

    const auto updateRendererLabels =
            [this](vtkRenderer *renderer, std::vector<vtkSmartPointer<vtkTextActor>> &actors) {
                if (!renderer) {
                    return;
                }
                auto *imageData =
                        vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
                const auto visible = computeVisibleImageBounds2D(
                        renderer, imageData);
                const int *size = renderer->GetSize();
                const double pixelsPerUnitX =
                        (visible.valid && size && (visible.xmax - visible.xmin) > 0.0)
                        ? static_cast<double>(size[0]) / (visible.xmax - visible.xmin)
                        : 0.0;
                const double pixelsPerUnitY =
                        (visible.valid && size && (visible.ymax - visible.ymin) > 0.0)
                        ? static_cast<double>(size[1]) / (visible.ymax - visible.ymin)
                        : 0.0;
                int visibleSources = 0;
                for (const auto &point : this->catalogueOverlayPixels) {
                    if (pointInVisibleBounds(point, visible)) {
                        ++visibleSources;
                    }
                }
                const bool labelsEnabled =
                        this->actionShowCatalogueLabels && this->actionShowCatalogueLabels->isChecked();
                const bool smartVisible = labelsEnabled && visibleSources <= smartCatalogueLabelMaxVisible
                        && std::max(pixelsPerUnitX, pixelsPerUnitY) >= smartCatalogueLabelMinPixelsPerUnit;

                vtkNew<vtkCoordinate> coordinate;
                coordinate->SetCoordinateSystemToWorld();
                auto *interactor = renderer->GetRenderWindow() ? renderer->GetRenderWindow()->GetInteractor() : nullptr;
                const int *eventPos = interactor ? interactor->GetEventPosition() : nullptr;
                if (eventPos && this->hoveredCatalogueSourceIndex < 0) {
                    double distancePx = 0.0;
                    this->hoveredCatalogueSourceIndex = this->catalogueSourceIndexNearDisplayPosition(
                            eventPos[0], eventPos[1], renderer, &distancePx);
                }
                for (std::size_t i = 0; i < actors.size()
                                         && i < this->catalogueOverlayLabelIndices.size();
                     ++i) {
                    const int pointIndex = this->catalogueOverlayLabelIndices[i];
                    if (pointIndex < 0
                        || static_cast<std::size_t>(pointIndex) >= this->catalogueOverlayPixels.size()) {
                        actors[i]->SetVisibility(0);
                        continue;
                    }
                    coordinate->SetValue(this->catalogueOverlayPixels[pointIndex][0],
                                         this->catalogueOverlayPixels[pointIndex][1], 0.0);
                    int *display = coordinate->GetComputedDisplayValue(renderer);
                    if (!display) {
                        actors[i]->SetVisibility(0);
                        continue;
                    }
                    actors[i]->SetDisplayPosition(display[0] + 4, display[1] + 4);
                    const bool showThis = smartVisible || pointIndex == this->hoveredCatalogueSourceIndex
                            || pointIndex == this->selectedCatalogueSourceIndex;
                    actors[i]->SetVisibility(showThis ? 1 : 0);
                }
            };

    updateRendererLabels(this->sliceWin->GetRenderers()->GetFirstRenderer(),
                         this->sliceCatalogueOverlayLabelActors);
    updateRendererLabels(this->momentWin->GetRenderers()->GetFirstRenderer(),
                         this->momentCatalogueOverlayLabelActors);
    const bool showHover = this->hoveredCatalogueSourceIndex >= 0
            && this->hoveredCatalogueSourceIndex != this->selectedCatalogueSourceIndex;
    this->sliceCatalogueHoverOverlayActor->SetVisibility(showHover ? 1 : 0);
    this->momentCatalogueHoverOverlayActor->SetVisibility(showHover ? 1 : 0);
    this->sliceCatalogueSelectionOverlayActor->SetVisibility(this->selectedCatalogueSourceIndex >= 0 ? 1 : 0);
    this->momentCatalogueSelectionOverlayActor->SetVisibility(this->selectedCatalogueSourceIndex >= 0 ? 1 : 0);
}

void vtkWindowCube::refreshCatalogueOverlayInteraction(int displayX, int displayY, bool fromClick)
{
    if (!this->catalogueOverlayLoaded || !this->actionShowCatalogueOverlay
        || !this->actionShowCatalogueOverlay->isChecked()) {
        return;
    }

    // §3.2 B3: route to the active interactive 2-D pipeline. A moment product pane is a
    // display-only Pane2DView (no catalogue actors), so viewingSlice() folds it to the
    // slice — never the now-dead moment window.
    vtkRenderer *renderer = this->active2dWin()
            ? this->active2dWin()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    if (!renderer) {
        return;
    }

    double hoverDistance = 0.0;
    const int hoveredIndex =
            this->catalogueSourceIndexNearDisplayPosition(displayX, displayY, renderer, &hoverDistance);
    if (hoveredIndex != this->hoveredCatalogueSourceIndex) {
        qDebug().noquote() << QStringLiteral("[catalogue:cube] hover index=%1 distance_px=%2")
                                      .arg(hoveredIndex)
                                      .arg(hoverDistance, 0, 'f', 2);
        this->hoveredCatalogueSourceIndex = hoveredIndex;
        this->rebuildCatalogueHighlightOverlay(this->catalogueHoverOverlayPoints,
                                               this->catalogueHoverOverlayCells,
                                               this->catalogueHoverOverlayData,
                                               this->hoveredCatalogueSourceIndex);
    }

    if (fromClick) {
        qDebug().noquote() << QStringLiteral("[catalogue:cube] select index=%1").arg(hoveredIndex);
        this->setSelectedCatalogueSourceIndex(hoveredIndex);
    }

    this->updateCatalogueOverlayLabels();
}

int vtkWindowCube::catalogueSourceIndexNearDisplayPosition(int displayX, int displayY,
                                                           vtkRenderer *renderer,
                                                           double *distancePx) const
{
    if (distancePx) {
        *distancePx = std::numeric_limits<double>::quiet_NaN();
    }
    if (!renderer) {
        return -1;
    }

    vtkNew<vtkCoordinate> coordinate;
    coordinate->SetCoordinateSystemToWorld();
    int nearestIndex = -1;
    double nearestDistance2 = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < this->catalogueOverlayPixels.size(); ++i) {
        coordinate->SetValue(this->catalogueOverlayPixels[i][0], this->catalogueOverlayPixels[i][1], 0.0);
        int *display = coordinate->GetComputedDisplayValue(renderer);
        if (!display) {
            continue;
        }
        const double dx = static_cast<double>(display[0] - displayX);
        const double dy = static_cast<double>(display[1] - displayY);
        const double dist2 = dx * dx + dy * dy;
        if (dist2 < nearestDistance2) {
            nearestDistance2 = dist2;
            nearestIndex = static_cast<int>(i);
        }
    }

    if (nearestDistance2 > catalogueHoverDisplayThresholdPx * catalogueHoverDisplayThresholdPx) {
        nearestIndex = -1;
    }
    if (distancePx) {
        *distancePx = nearestIndex >= 0 ? std::sqrt(nearestDistance2) : std::numeric_limits<double>::quiet_NaN();
    }
    return nearestIndex;
}

void vtkWindowCube::rebuildCatalogueHighlightOverlay(vtkPoints *points, vtkCellArray *cells, vtkPolyData *data,
                                                     int sourceIndex)
{
    if (!points || !cells || !data) {
        return;
    }
    points->Reset();
    cells->Reset();

    if (sourceIndex >= 0 && static_cast<std::size_t>(sourceIndex) < this->catalogueOverlayEntries.size()
        && static_cast<std::size_t>(sourceIndex) < this->catalogueOverlaySourceFirstPolyline.size()
        && static_cast<std::size_t>(sourceIndex) < this->catalogueOverlaySourcePolylineCount.size()) {
        const int first = this->catalogueOverlaySourceFirstPolyline[sourceIndex];
        const int count = this->catalogueOverlaySourcePolylineCount[sourceIndex];
        for (int polylineIndex = first; polylineIndex < first + count; ++polylineIndex) {
            if (polylineIndex < 0
                || static_cast<std::size_t>(polylineIndex) >= this->catalogueOverlayPolylines.size()) {
                continue;
            }
            const auto &polyline = this->catalogueOverlayPolylines[polylineIndex];
            if (polyline.size() < 2) {
                continue;
            }
            cells->InsertNextCell(static_cast<vtkIdType>(polyline.size()));
            for (const auto &point : polyline) {
                const vtkIdType id = points->InsertNextPoint(point[0], point[1], 0.0);
                cells->InsertCellPoint(id);
            }
        }
    }

    data->SetPoints(points);
    data->SetLines(cells);
    data->Modified();
}

void vtkWindowCube::updateCatalogueInfoPanel()
{
    if (!this->catalogueInfoLabel) {
        return;
    }
    if (this->selectedCatalogueSourceIndex < 0
        || static_cast<std::size_t>(this->selectedCatalogueSourceIndex) >= this->catalogueOverlayEntries.size()) {
        this->catalogueInfoLabel->setVisible(false);
        return;
    }

    const QString summary = this->catalogueSourceSummary(this->selectedCatalogueSourceIndex);
    this->catalogueInfoLabel->setText(summary);
    this->catalogueInfoLabel->setToolTip(summary);
    this->catalogueInfoLabel->setVisible(true);
}

QString vtkWindowCube::catalogueSourceSummary(int sourceIndex) const
{
    if (sourceIndex < 0 || static_cast<std::size_t>(sourceIndex) >= this->catalogueOverlayEntries.size()
        || static_cast<std::size_t>(sourceIndex) >= this->catalogueOverlayPixels.size()) {
        return QStringLiteral("Source: none");
    }

    const CatalogueOverlayEntry &entry = this->catalogueOverlayEntries[sourceIndex];
    const auto &pixel = this->catalogueOverlayPixels[sourceIndex];
    QString summary = u"Source %1"_s.arg(sourceIndex + 1);
    if (!entry.label.trimmed().isEmpty()) {
        summary += u" [%1]"_s.arg(entry.label.trimmed());
    }
    summary += u" | %1 | %2 | x=%3 y=%4"_s.arg(catalogueShapeName(entry.shape),
                                               catalogueFrameName(entry.frame))
                       .arg(pixel[0], 0, 'f', 2)
                       .arg(pixel[1], 0, 'f', 2);
    if (entry.frame == CatalogueOverlayEntry::Frame::Sky) {
        summary += u" | RA=%1 deg DEC=%2 deg"_s.arg(entry.raDeg, 0, 'f', 6).arg(entry.decDeg, 0, 'f', 6);
    }
    if (entry.shape == CatalogueOverlayEntry::Shape::Ellipse) {
        summary += u" | rx=%1 ry=%2 ang=%3"_s.arg(entry.radiusX, 0, 'f', 2)
                           .arg(entry.radiusY, 0, 'f', 2)
                           .arg(entry.angleDeg, 0, 'f', 1);
    }
    return summary;
}

void vtkWindowCube::ensureCatalogueDock()
{
    if (this->catalogueDock) {
        return;
    }

    this->catalogueDock = new QDockWidget(u"Catalogue"_s, this);
    this->catalogueDock->setObjectName(u"CatalogueDockCube"_s);
    this->catalogueTableView = new QTableView(this->catalogueDock);
    this->catalogueTableModel = new CatalogueTableModel(this->catalogueDock);
    this->catalogueTableView->setModel(this->catalogueTableModel);
    this->catalogueTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->catalogueTableView->setSelectionMode(QAbstractItemView::SingleSelection);
    this->catalogueTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->catalogueTableView->setSortingEnabled(false);
    this->catalogueTableView->verticalHeader()->setVisible(false);
    this->catalogueTableView->horizontalHeader()->setStretchLastSection(true);
    this->catalogueDock->setWidget(this->catalogueTableView);
    this->addDockWidget(Qt::RightDockWidgetArea, this->catalogueDock);
    this->catalogueDock->hide();

    QObject::connect(this, &vtkWindowCube::catalogueSourceSelectionChanged, this,
                     &vtkWindowCube::syncCatalogueTableSelection);
    QObject::connect(this->catalogueTableView->selectionModel(), &QItemSelectionModel::currentRowChanged,
                     this, [this](const QModelIndex &current) {
        if (this->syncingCatalogueSelection) {
            return;
        }
        this->setSelectedCatalogueSourceIndex(current.isValid() ? current.row() : -1);
    });
    QObject::connect(this->catalogueTableView, &QTableView::doubleClicked, this,
                     [this](const QModelIndex &index) {
        if (index.isValid()) {
            this->setSelectedCatalogueSourceIndex(index.row());
            this->centerViewOnCatalogueSource(index.row());
        }
    });
    this->refreshCatalogueTable();
}

void vtkWindowCube::refreshCatalogueTable()
{
    if (!this->catalogueTableModel) {
        return;
    }
    this->catalogueTableModel->setEntries(&this->catalogueOverlayEntries);
    if (this->catalogueDock) {
        this->catalogueDock->setVisible(!this->catalogueOverlayEntries.empty());
    }
    if (this->catalogueTableView) {
        this->catalogueTableView->resizeColumnsToContents();
    }
    this->syncCatalogueTableSelection(this->selectedCatalogueSourceIndex);
}

void vtkWindowCube::setSelectedCatalogueSourceIndex(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= this->catalogueOverlayEntries.size()) {
        index = -1;
    }
    if (this->selectedCatalogueSourceIndex == index) {
        this->updateCatalogueInfoPanel();
        emit this->catalogueSourceSelectionChanged(index);
        return;
    }

    this->selectedCatalogueSourceIndex = index;
    this->rebuildCatalogueHighlightOverlay(this->catalogueSelectionOverlayPoints,
                                           this->catalogueSelectionOverlayCells,
                                           this->catalogueSelectionOverlayData,
                                           this->selectedCatalogueSourceIndex);
    this->updateCatalogueInfoPanel();
    this->updateCatalogueOverlayLabels();
    this->sliceWin->Render();
    this->momentWin->Render();
    emit this->catalogueSourceSelectionChanged(index);
}

void vtkWindowCube::centerViewOnCatalogueSource(int index, double zoomFactor)
{
    if (index < 0 || static_cast<std::size_t>(index) >= this->catalogueOverlayPixels.size()) {
        return;
    }

    // §3.2 B3: route to the active interactive 2-D window — a moment product pane is a
    // display-only Pane2DView, folded to the slice by viewingSlice(); never the now-dead
    // moment window.
    vtkGenericOpenGLRenderWindow *window = this->viewingSlice() ? this->sliceWin : this->momentWin;
    vtkRenderer *renderer = window->GetRenderers()->GetFirstRenderer();
    auto *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!renderer || !camera || !window) {
        return;
    }

    const auto &pixel = this->catalogueOverlayPixels[index];
    double focal[3];
    double position[3];
    camera->GetFocalPoint(focal);
    camera->GetPosition(position);
    const double dz = position[2] - focal[2];
    camera->SetFocalPoint(pixel[0], pixel[1], 0.0);
    camera->SetPosition(pixel[0], pixel[1], dz);
    if (camera->GetParallelProjection() && zoomFactor > 1.0) {
        camera->SetParallelScale(camera->GetParallelScale() / zoomFactor);
    }
    renderer->ResetCameraClippingRange();
    window->Render();
    // STEP6-P1: this directly moved the 2-D slice camera. If it's the slice, let the linked image
    // panes follow the centering AND record the new pan/zoom pose, so a later window/level drag
    // isn't mistaken for a fresh camera move. onSliceCameraInteraction handles both.
    if (window == this->sliceWin)
        this->onSliceCameraInteraction();
}

void vtkWindowCube::syncCatalogueTableSelection(int index)
{
    if (!this->catalogueTableView || !this->catalogueTableView->selectionModel()
        || !this->catalogueTableModel) {
        return;
    }

    this->syncingCatalogueSelection = true;
    if (index < 0 || index >= this->catalogueTableModel->rowCount()) {
        this->catalogueTableView->clearSelection();
    } else {
        const QModelIndex modelIndex = this->catalogueTableModel->index(index, 0);
        this->catalogueTableView->selectionModel()->setCurrentIndex(
                modelIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        this->catalogueTableView->scrollTo(modelIndex, QAbstractItemView::PositionAtCenter);
    }
    this->syncingCatalogueSelection = false;
}

bool vtkWindowCube::catalogueWorldToPixel(double raDeg, double decDeg, std::array<double, 2> &pixel) const
{
    if (!std::isfinite(raDeg) || !std::isfinite(decDeg)) {
        return false;
    }

    if (!this->remoteHasCelestialAxes()) {
        return false;
    }

    double nativeX = raDeg;
    double nativeY = decDeg;
    const int nativeFrame = this->remoteNativeCelestialFrame();
    if (nativeFrame < 0) {
        return false;
    }
    if (nativeFrame != WCS_J2000) {
        wcscon(WCS_J2000, nativeFrame, 2000.0, 2000.0, &nativeX, &nativeY, 2000.0);
    }
    if (std::abs(this->remoteDatasetCdelt[0]) <= std::numeric_limits<double>::epsilon()
        || std::abs(this->remoteDatasetCdelt[1]) <= std::numeric_limits<double>::epsilon()) {
        return false;
    }
    pixel[0] = ((nativeX - this->remoteDatasetCrval[0]) / this->remoteDatasetCdelt[0])
            + this->remoteDatasetCrpix[0] - 1.0;
    pixel[1] = ((nativeY - this->remoteDatasetCrval[1]) / this->remoteDatasetCdelt[1])
            + this->remoteDatasetCrpix[1] - 1.0;
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]);
}
