#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeWcsUtil.h"
#include "CubeViewportGeometry.h"
#include "WcsAxisTicks.h"

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
using namespace CubeWcs;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

// ── WCS / celestial-axis overlay + coordinate formatting ─────────────────────
// Extracted verbatim from vtkWindowImage.cpp. Geometry helpers come from
// CubeGeom; shared axis/coordinate-format helpers from CubeWcs (CubeWcsUtil).
// hideCustomWcsOverlay / isImageExtentUsable stay duplicated here (also used by
// the layer code in main; not yet identical enough to share).

namespace {
constexpr int overlayTickCount = 5;

bool isImageExtentUsable(vtkImageData *imageData);
void hideCustomWcsOverlay(vtkAxisActor2D *xAxis, vtkAxisActor2D *yAxis, vtkTextActor *xTitle,
                          vtkTextActor *yTitle,
                          const std::vector<vtkSmartPointer<vtkTextActor>> &xTicks,
                          const std::vector<vtkSmartPointer<vtkTextActor>> &yTicks);

bool isImageExtentUsable(vtkImageData *imageData)
{
    if (!imageData) {
        return false;
    }
    int extent[6];
    imageData->GetExtent(extent);
    return extent[1] > extent[0] && extent[3] > extent[2];
}

void hideCustomWcsOverlay(vtkAxisActor2D *xAxis, vtkAxisActor2D *yAxis, vtkTextActor *xTitle,
                          vtkTextActor *yTitle,
                          const std::vector<vtkSmartPointer<vtkTextActor>> &xTicks,
                          const std::vector<vtkSmartPointer<vtkTextActor>> &yTicks)
{
    if (xAxis) {
        xAxis->VisibilityOff();
    }
    if (yAxis) {
        yAxis->VisibilityOff();
    }
    if (xTitle) {
        xTitle->VisibilityOff();
    }
    if (yTitle) {
        yTitle->VisibilityOff();
    }
    for (const auto &actor : xTicks) {
        if (actor) {
            actor->VisibilityOff();
        }
    }
    for (const auto &actor : yTicks) {
        if (actor) {
            actor->VisibilityOff();
        }
    }
}





}  // namespace

void vtkWindowImage::updateWcsOverlay()
{
    if (this->suspendHeavyOverlayUpdates) {
        return;
    }
    if (!this->wcsOverlayInitialized) {
        qDebug().noquote() << QStringLiteral("[wcs-overlay] vtkWindowImage not initialized yet");
        return;
    }

    if (!ui || !ui->vtk) {
        qDebug().noquote() << QStringLiteral("[wcs-overlay] vtkWindowImage missing ui/ui->vtk ui=%1 vtk=%2")
                                      .arg(reinterpret_cast<quintptr>(ui), 0, 16)
                                      .arg(reinterpret_cast<quintptr>(ui ? ui->vtk : nullptr), 0, 16);
        return;
    }

    auto *renderWindow = ui->vtk->renderWindow();
    auto *rendererCollection = renderWindow ? renderWindow->GetRenderers() : nullptr;
    auto *renderer = rendererCollection ? rendererCollection->GetFirstRenderer() : nullptr;
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!renderWindow || !renderer || !this->overlayXAxis.GetPointer() || !this->overlayYAxis.GetPointer()
        || !this->overlayXTitleActor.GetPointer() || !this->overlayYTitleActor.GetPointer()) {
        qDebug().noquote()
                << QStringLiteral("[wcs-overlay] vtkWindowImage missing objects renderer=%1 renderWindow=%2 xAxis=%3 yAxis=%4 xTitle=%5 yTitle=%6 image=%7")
                           .arg(reinterpret_cast<quintptr>(renderer), 0, 16)
                           .arg(reinterpret_cast<quintptr>(renderWindow), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->overlayXAxis.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->overlayYAxis.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->overlayXTitleActor.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->overlayYTitleActor.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(imageData), 0, 16);
        return;
    }
    if (!isImageExtentUsable(imageData)) {
        qDebug().noquote()
                << QStringLiteral("[wcs-overlay] remote image extent not ready x=%1..%2 y=%3..%4")
                           .arg(imageData ? imageData->GetExtent()[0] : -1)
                           .arg(imageData ? imageData->GetExtent()[1] : -1)
                           .arg(imageData ? imageData->GetExtent()[2] : -1)
                           .arg(imageData ? imageData->GetExtent()[3] : -1);
        this->invalidateWcsOverlayCache();
        hideCustomWcsOverlay(this->overlayXAxis, this->overlayYAxis, this->overlayXTitleActor,
                             this->overlayYTitleActor, this->overlayXTickActors,
                             this->overlayYTickActors);
        return;
    }
    this->setWcsOverlayVisible(this->showWcsAxes);
    if (!this->showWcsAxes || !renderer || !imageData) {
        return;
    }
    this->ensureOverlayTickActors(renderer);
    if (this->overlayXTickActors.size() < overlayTickCount
        || this->overlayYTickActors.size() < overlayTickCount) {
        qDebug().noquote()
                << QStringLiteral("[wcs-overlay] vtkWindowImage tick actors incomplete x=%1 y=%2")
                           .arg(this->overlayXTickActors.size())
                           .arg(this->overlayYTickActors.size());
        return;
    }

    const auto visible = computeVisibleImageBounds2D(renderer, imageData);
    if (!visible.valid || std::abs(visible.xmax - visible.xmin) < 1.0
        || std::abs(visible.ymax - visible.ymin) < 1.0) {
        qDebug().noquote()
                << QStringLiteral("[wcs-overlay] visible bounds not ready x=%1..%2 y=%3..%4")
                           .arg(visible.xmin, 0, 'g', 12)
                           .arg(visible.xmax, 0, 'g', 12)
                           .arg(visible.ymin, 0, 'g', 12)
                           .arg(visible.ymax, 0, 'g', 12);
        this->invalidateWcsOverlayCache();
        hideCustomWcsOverlay(this->overlayXAxis, this->overlayYAxis, this->overlayXTitleActor,
                             this->overlayYTitleActor, this->overlayXTickActors,
                             this->overlayYTickActors);
        return;
    }

    const int *size = renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0) {
        return;
    }

    const std::array<double, 4> visibleBounds = { visible.xmin, visible.xmax, visible.ymin, visible.ymax };
    const std::array<int, 2> viewportSize = { size[0], size[1] };
    const int currentFrame = this->selectedWcsFrame();
    const int currentFormat = this->useSexagesimalWcsFormat ? 1 : 0;
    // The camera's time stamp is part of the key: the axes follow the image on
    // screen, so a pan that leaves the whole image visible still moves them.
    const unsigned long camMTime = renderer->GetActiveCamera()->GetMTime();
    if (visibleBounds == this->lastOverlayVisibleBounds && viewportSize == this->lastOverlayViewportSize
        && camMTime == this->lastOverlayCameraMTime
        && currentFrame == this->lastRenderedWcsFrame && currentFormat == this->lastRenderedWcsFormat) {
        return;
    }
    this->lastOverlayVisibleBounds = visibleBounds;
    this->lastOverlayViewportSize = viewportSize;
    this->lastOverlayCameraMTime = camMTime;
    this->lastRenderedWcsFrame = currentFrame;
    this->lastRenderedWcsFormat = currentFormat;

    constexpr double leftMargin = 168.;
    // The axes run along the image as it sits on screen, not along the whole
    // viewport: the image rarely fills it, and the ticks must read the
    // coordinate under them. The same frame the cube's slice overlay uses
    // (CubeGeom::axisFrameFor); this viewer has no colour-bar column, so its
    // gutter is the room the labels need.
    CubeGeom::ImageGutter gutter;
    gutter.left = 136.;
    gutter.right = 34.;
    gutter.bottom = 58.;
    gutter.top = 28.;
    const CubeGeom::AxisFrame frame =
            CubeGeom::axisFrameFor(size, renderer->GetActiveCamera(), imageData, gutter,
                                    16.);
    if (!frame.valid) {
        // Too little of the image on screen to label. Axes laid to the
        // viewport's edges would read the wrong coordinates, so show none.
        this->invalidateWcsOverlayCache();
        hideCustomWcsOverlay(this->overlayXAxis, this->overlayYAxis, this->overlayXTitleActor,
                             this->overlayYTitleActor, this->overlayXTickActors,
                             this->overlayYTickActors);
        return;
    }
    const double axisX = frame.left;
    const double xRight = frame.right;
    const double bottomMargin = frame.bottom;
    const double yTop = frame.top;
    // The voxel range the two ends of each axis stand for.
    const double axXmin = frame.xmin;
    const double axXmax = frame.xmax;
    const double axYmin = frame.ymin;
    const double axYmax = frame.ymax;
    configureAxisActor(this->overlayXAxis, axisX, bottomMargin, xRight, bottomMargin);
    configureAxisActor(this->overlayYAxis, axisX, yTop, axisX, bottomMargin);
    this->overlayXAxis->SetTitle("");
    this->overlayXTitleActor->SetInput(this->remoteOverlayAxisTitle(0).toStdString().c_str());
    this->overlayXTitleActor->SetDisplayPosition((axisX + xRight) / 2,
                                                 static_cast<int>(bottomMargin / 2.0) - 2);
    this->overlayYAxis->SetTitle("");
    this->overlayYTitleActor->SetInput(this->remoteOverlayAxisTitle(1).toStdString().c_str());
    this->overlayYTitleActor->SetDisplayPosition(static_cast<int>(leftMargin / 3.0),
                                                 (bottomMargin + yTop) / 2);

    bool xOk = false;
    const double xMin = this->remoteVoxelToWcs(0, axXmin, &xOk);
    const double xMax = this->remoteVoxelToWcs(0, axXmax, &xOk);
    bool yOk = false;
    const double yMin = this->remoteVoxelToWcs(1, axYmin, &yOk);
    const double yMax = this->remoteVoxelToWcs(1, axYmax, &yOk);
    this->overlayXAxis->SetRange(xOk ? xMin : axXmin, xOk ? xMax : axXmax);
    this->overlayYAxis->SetRange(yOk ? yMax : axYmax, yOk ? yMin : axYmin);
    this->overlayXAxis->LabelVisibilityOff();
    this->overlayYAxis->LabelVisibilityOff();
    this->overlayXAxis->VisibilityOn();
    this->overlayYAxis->VisibilityOn();
    this->overlayXTitleActor->VisibilityOn();
    this->overlayYTitleActor->VisibilityOn();
    // Each axis is read along its own edge, not on the diagonal: in another
    // frame than the native one both pixel axes move both coordinates
    // (WcsAxisTicks.h).
    const auto frameAt = [this](const visivo::AxisVoxel &at, int axis) {
        bool okX = false;
        bool okY = false;
        const double nativeX = this->remoteVoxelToWcs(0, at.x, &okX);
        const double nativeY = this->remoteVoxelToWcs(1, at.y, &okY);
        double fx = nativeX;
        double fy = nativeY;
        if (okX && okY && this->remoteHasCelestialAxes())
            this->convertRemoteCelestialCoordinates(nativeX, nativeY, fx, fy);
        return axis == 0 ? fx : fy;
    };
    // How far each axis runs in the displayed frame, so the labels carry enough
    // decimals for neighbouring ticks to differ.
    const double xStart = frameAt(visivo::xAxisTickVoxel(0.0, axXmin, axXmax, axYmin), 0);
    const double xEnd = frameAt(visivo::xAxisTickVoxel(1.0, axXmin, axXmax, axYmin), 0);
    // Only a longitude wraps at 360; a length of 300 AU is not 60.
    const double spanX = this->remoteHasCelestialAxes() ? visivo::angularAxisSpan(xStart, xEnd)
                                                        : std::fabs(xStart - xEnd);
    const double spanY = std::fabs(
            frameAt(visivo::yAxisTickVoxel(0.0, axYmin, axYmax, axXmin), 1)
            - frameAt(visivo::yAxisTickVoxel(1.0, axYmin, axYmax, axXmin), 1));
    int yLabelChars = 0;
    for (int i = 0; i < overlayTickCount; ++i) {
        const double t = overlayTickCount == 1 ? 0.0
                                               : static_cast<double>(i)
                        / static_cast<double>(overlayTickCount - 1);
        const double frameX =
                frameAt(visivo::xAxisTickVoxel(t, axXmin, axXmax, axYmin), 0);
        const double frameY =
                frameAt(visivo::yAxisTickVoxel(t, axYmin, axYmax, axXmin), 1);

        this->overlayXTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(0, frameX, spanX).toStdString().c_str());
        // The last x label ends at its tick: centred there, 34 px from the
        // window's edge, it was cut off (#516).
        auto *xProp = this->overlayXTickActors[static_cast<std::size_t>(i)]->GetTextProperty();
        if (i == overlayTickCount - 1)
            xProp->SetJustificationToRight();
        else
            xProp->SetJustificationToCentered();
        this->overlayXTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX + t * (xRight - axisX)),
                static_cast<int>(bottomMargin - 18));

        const QString yLabel = this->formatRemoteOverlayCoordinate(1, frameY, spanY);
        yLabelChars = std::max(yLabelChars, int(yLabel.size()));
        this->overlayYTickActors[static_cast<std::size_t>(i)]->SetInput(
                yLabel.toStdString().c_str());
        // The lowest y label sits on the x axis, not across it: centred there,
        // its lower half met the first x label in the corner (issue #516).
        auto *yProp = this->overlayYTickActors[static_cast<std::size_t>(i)]->GetTextProperty();
        if (i == 0)
            yProp->SetVerticalJustificationToBottom();
        else
            yProp->SetVerticalJustificationToCentered();
        this->overlayYTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX - 10),
                static_cast<int>(bottomMargin + t * (yTop - bottomMargin))
                        + (i == 0 ? 2 : 0));
        this->overlayYTickActors[static_cast<std::size_t>(i)]->VisibilityOn();
    }
    // Only as many x labels as fit side by side; this viewer keeps the leftmost
    // one, which sits above the corner (#516) (#544).
    CubeWcs::thinXTickLabels(renderer, this->overlayXTickActors, xRight - axisX, 1.0,
                             /*skipFirst=*/false);
    // The y title clears the tick labels, whatever their precision: their
    // width measured as drawn (font 16, and in device pixels on a HiDPI
    // screen), not estimated from a character count, which left it over them.
    double labelWidth = 0.0;
    for (auto &tick : this->overlayYTickActors) {
        if (!tick || !tick->GetVisibility())
            continue;
        double bbox[4] = { 0.0, 0.0, 0.0, 0.0 };
        tick->GetBoundingBox(renderer, bbox);
        labelWidth = std::max(labelWidth, bbox[1] - bbox[0]);
    }
    double titleBox[4] = { 0.0, 0.0, 0.0, 0.0 };
    this->overlayYTitleActor->GetBoundingBox(renderer, titleBox);
    const double titleHalf = std::max(9.0, 0.5 * (titleBox[1] - titleBox[0]));
    const double clearX = labelWidth > 0.0
            ? axisX - 10.0 - labelWidth - 6.0 - titleHalf
            : visivo::yAxisTitleX(axisX, axisX - 74.0, 8.0, yLabelChars, 7.5, 10.0);
    // Just left of the labels, which follow the image: a title kept at the
    // window's edge floated far from an axis that now starts at the image.
    this->overlayYTitleActor->SetDisplayPosition(static_cast<int>(std::max(8.0, clearX)),
                                                 (bottomMargin + yTop) / 2);

    // The x title goes below the tick labels as they are drawn (issue #516):
    // at a fixed height it sat over them once the labels grew (font 16, and in
    // device pixels on a HiDPI screen).
    // A text actor's bounding box is relative to its own position, so the
    // labels' lowest edge is where they are drawn (18 px under the axis) plus
    // the box's own offset.
    double labelsBottom = bottomMargin - 18.0;
    for (auto &tick : this->overlayXTickActors) {
        if (!tick || !tick->GetVisibility())
            continue;
        double bbox[4] = { 0.0, 0.0, 0.0, 0.0 };
        tick->GetBoundingBox(renderer, bbox);
        if (bbox[3] > bbox[2])
            labelsBottom = std::min(labelsBottom, bottomMargin - 18.0 + bbox[2]);
    }
    double xTitleBox[4] = { 0.0, 0.0, 0.0, 0.0 };
    this->overlayXTitleActor->GetBoundingBox(renderer, xTitleBox);
    const double xTitleHalf = std::max(6.0, 0.5 * (xTitleBox[3] - xTitleBox[2]));
    // Right under the labels, wherever the axis sits: the axis now follows the
    // image, so a fixed fraction of its height left the title far below it.
    const double xTitleY = std::max(xTitleHalf + 1.0, labelsBottom - 8.0 - xTitleHalf);
    this->overlayXTitleActor->SetDisplayPosition((axisX + xRight) / 2, static_cast<int>(xTitleY));

    qDebug().noquote()
            << QStringLiteral("[wcs-overlay] updated ticks x=%1..%2 y=%3..%4 size=%5x%6 renderer=%7 renderWindow=%8 xAxis=%9 yAxis=%10 xTitle=%11 yTitle=%12 endpoints=(%13,%14)->(%15,%16) outer=%17")
                       .arg(visible.xmin, 0, 'g', 12)
                       .arg(visible.xmax, 0, 'g', 12)
                       .arg(visible.ymin, 0, 'g', 12)
                       .arg(visible.ymax, 0, 'g', 12)
                       .arg(size[0])
                       .arg(size[1])
                       .arg(reinterpret_cast<quintptr>(renderer), 0, 16)
                       .arg(reinterpret_cast<quintptr>(renderWindow), 0, 16)
                       .arg(reinterpret_cast<quintptr>(this->overlayXAxis.GetPointer()), 0, 16)
                       .arg(reinterpret_cast<quintptr>(this->overlayYAxis.GetPointer()), 0, 16)
                       .arg(reinterpret_cast<quintptr>(this->overlayXTitleActor.GetPointer()), 0, 16)
                       .arg(reinterpret_cast<quintptr>(this->overlayYTitleActor.GetPointer()), 0, 16)
                       .arg(axisX, 0, 'g', 12)
                       .arg(yTop, 0, 'g', 12)
                       .arg(axisX, 0, 'g', 12)
                       .arg(bottomMargin, 0, 'g', 12)
                       .arg(leftMargin, 0, 'g', 12);
}

void vtkWindowImage::setWcsOverlayVisible(bool visible)
{
    if (!this->wcsOverlayInitialized) {
        return;
    }
    if (this->legendWCS) {
        this->legendWCS->SetVisibility(false);
    }
    this->overlayXAxis->SetVisibility(visible);
    this->overlayYAxis->SetVisibility(visible);
    this->overlayXTitleActor->SetVisibility(visible);
    this->overlayYTitleActor->SetVisibility(visible);
    for (const auto &actor : this->overlayXTickActors) {
        if (actor) {
            actor->SetVisibility(visible);
        }
    }
    for (const auto &actor : this->overlayYTickActors) {
        if (actor) {
            actor->SetVisibility(visible);
        }
    }
}

void vtkWindowImage::ensureOverlayTickActors(vtkRenderer *renderer)
{
    if (!renderer) {
        return;
    }
    if (this->overlayXTickActors.empty()) {
        for (int i = 0; i < overlayTickCount; ++i) {
            auto actor = vtkSmartPointer<vtkTextActor>::New();
            configureTickLabelActor(actor, false);
            renderer->AddViewProp(actor);
            this->overlayXTickActors.push_back(actor);
        }
    }
    if (this->overlayYTickActors.empty()) {
        for (int i = 0; i < overlayTickCount; ++i) {
            auto actor = vtkSmartPointer<vtkTextActor>::New();
            configureTickLabelActor(actor, true);
            renderer->AddViewProp(actor);
            this->overlayYTickActors.push_back(actor);
        }
    }
}

void vtkWindowImage::invalidateWcsOverlayCache()
{
    this->lastOverlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
                                       std::numeric_limits<double>::quiet_NaN(),
                                       std::numeric_limits<double>::quiet_NaN(),
                                       std::numeric_limits<double>::quiet_NaN() };
    this->lastOverlayViewportSize = { -1, -1 };
    this->lastRenderedWcsFrame = std::numeric_limits<int>::min();
    this->lastRenderedWcsFormat = -1;
}

void vtkWindowImage::applyDefaultWcsFormatForSelectedFrame()
{
    if (this->wcsFormatExplicitlyChosen) {
        return;
    }

    this->useSexagesimalWcsFormat = this->selectedWcsFrame() == WCS_J2000;
    if (this->actionWcsSexagesimal) {
        this->actionWcsSexagesimal->setChecked(this->useSexagesimalWcsFormat);
    }
    if (this->actionWcsDecimal) {
        this->actionWcsDecimal->setChecked(!this->useSexagesimalWcsFormat);
    }
}

void vtkWindowImage::requestWcsOverlayRender()
{
    QMetaObject::invokeMethod(
            this,
            [this]() {
                if (!ui || !ui->vtk || !ui->vtk->renderWindow()) {
                    return;
                }
                this->updateWcsOverlay();
                if (this->probeValid) {
                    this->refreshProbeOverlay();
                }
                ui->vtk->renderWindow()->Render();
                ui->vtk->update();
            },
            Qt::QueuedConnection);
}

void vtkWindowImage::refreshWcsOverlayImmediately()
{
    if (!ui || !ui->vtk || !ui->vtk->renderWindow()) {
        return;
    }
    this->updateWcsOverlay();
    if (this->probeValid) {
        auto *interactor = ui->vtk->renderWindow()->GetInteractor();
        const int *position = interactor ? interactor->GetEventPosition() : nullptr;
        if (position) {
            this->updateProbeFromDisplayPosition(position[0], position[1]);
        } else {
            this->refreshProbeOverlay();
        }
    }
    ui->vtk->renderWindow()->Render();
    ui->vtk->update();
    this->requestWcsOverlayRender();
}

QString vtkWindowImage::currentWcsFrameLabel() const
{
    // X, Y, Z of a simulation are not on the sky: say so instead of naming
    // the frame the menu happens to have selected.
    if (!this->remoteHasCelestialAxes())
        return visivo::linearFrameLabel(this->remoteDatasetCtype[0], this->remoteDatasetCtype[1]);
    const int frame = this->selectedWcsFrame();
    return frame == WCS_GALACTIC ? u"Galactic"_s
            : (frame == WCS_J2000 ? u"FK5"_s : u"Ecliptic"_s);
}

bool vtkWindowImage::remoteHasWcsAxis(int axis) const
{
    return axis >= 0 && axis < 3 && std::isfinite(this->remoteDatasetCrval[axis])
            && std::isfinite(this->remoteDatasetCrpix[axis])
            && std::isfinite(this->remoteDatasetCdelt[axis])
            && std::abs(this->remoteDatasetCdelt[axis]) > 1e-12;
}

double vtkWindowImage::remoteVoxelToWcs(int axis, double voxelIndex, bool *ok) const
{
    const bool valid = this->remoteHasWcsAxis(axis);
    if (ok) {
        *ok = valid;
    }
    if (!valid) {
        return voxelIndex;
    }

    return this->remoteDatasetCrval[axis]
            + ((voxelIndex + 1.0) - this->remoteDatasetCrpix[axis]) * this->remoteDatasetCdelt[axis];
}

QString vtkWindowImage::remoteFormatAxisCoordinate(int axis, double voxelIndex) const
{
    bool ok = false;
    const double world = this->remoteVoxelToWcs(axis, voxelIndex, &ok);
    if (!ok) {
        return QString::number(voxelIndex, 'g', 12);
    }

    const QString unit = (axis >= 0 && axis < 3) ? this->remoteDatasetCunit[axis].trimmed() : QString();
    if (unit.isEmpty()) {
        return QString::number(world, 'g', 12);
    }
    return u"%1 %2"_s.arg(QString::number(world, 'g', 12), unit);
}

QString vtkWindowImage::remoteAxisTitle(int axis) const
{
    const QString base = axis == 0 ? u"X"_s : (axis == 1 ? u"Y"_s : u"Z"_s);
    const QString ctype = (axis >= 0 && axis < 3) ? this->remoteDatasetCtype[axis].trimmed() : QString();
    const QString cunit = (axis >= 0 && axis < 3) ? this->remoteDatasetCunit[axis].trimmed() : QString();
    const QString label = ctype.isEmpty() ? base : ctype;
    return cunit.isEmpty() ? label : u"%1 (%2)"_s.arg(label, cunit);
}

int vtkWindowImage::selectedWcsFrame() const
{
    return ui->actionGalactic->isChecked() ? WCS_GALACTIC
            : (ui->actionFK5->isChecked() ? WCS_J2000 : WCS_ECLIPTIC);
}

int vtkWindowImage::remoteNativeCelestialFrame() const
{
    return inferCelestialFrameFromCtypePair(this->remoteDatasetCtype);
}

bool vtkWindowImage::remoteHasCelestialAxes() const
{
    return this->remoteNativeCelestialFrame() >= 0 && this->remoteHasWcsAxis(0)
            && this->remoteHasWcsAxis(1);
}

bool vtkWindowImage::convertRemoteCelestialCoordinates(double nativeX, double nativeY, double &frameX,
                                                       double &frameY) const
{
    frameX = nativeX;
    frameY = nativeY;
    const int nativeFrame = this->remoteNativeCelestialFrame();
    const int targetFrame = this->selectedWcsFrame();
    if (nativeFrame < 0) {
        return false;
    }
    if (nativeFrame != targetFrame) {
        wcscon(nativeFrame, targetFrame, 2000.0, 2000.0, &frameX, &frameY, 2000.0);
    }
    return true;
}

QString vtkWindowImage::formatRemoteOverlayCoordinate(int axis, double value, double axisSpan) const
{
    if (!this->remoteHasCelestialAxes()) {
        return visivo::formatLinearTick(value, axisSpan);
    }
    if (!this->useSexagesimalWcsFormat) {
        return this->formatDegreeCoordinate(value, axisSpan);
    }
    return formatCelestialCoordinate(this->selectedWcsFrame(), axis, value);
}

QString vtkWindowImage::remoteOverlayAxisTitle(int axis) const
{
    if (m_dynamicSpectrumMode) {
        // Beamformed convention: NAXIS1 = TIME, NAXIS2 = FREQUENCY (see
        // hdf5_dynspec.convert_hdf5_to_sidecar_fits and the FITS CTYPE check
        // in ScientificFitsDataset.is_dynamic_spectrum). Inherit the unit
        // from the FITS CUNIT so users with non-default units (e.g.
        // CUNIT2='MHz') see the right label.
        const QString cunit = (axis >= 0 && axis < 3)
                ? this->remoteDatasetCunit[axis].trimmed() : QString();
        const QString base = axis == 0 ? u"Time"_s : u"Frequency"_s;
        return cunit.isEmpty() ? base : u"%1 (%2)"_s.arg(base, cunit);
    }
    if (!this->remoteHasCelestialAxes()) {
        return this->remoteAxisTitle(axis);
    }
    const int frame = this->selectedWcsFrame();
    if (frame == WCS_J2000) {
        return axis == 0 ? u"Right Ascension"_s : u"Declination"_s;
    }
    if (frame == WCS_GALACTIC) {
        return axis == 0 ? u"Galactic Longitude"_s : u"Galactic Latitude"_s;
    }
    return axis == 0 ? u"Ecliptic Longitude"_s : u"Ecliptic Latitude"_s;
}

QString vtkWindowImage::formatDegreeCoordinate(double value, double axisSpan) const
{
    return visivo::formatDegreeTick(value, axisSpan);
}

QString vtkWindowImage::selectedFrameAxisTitle(int axis) const
{
    return this->remoteOverlayAxisTitle(axis);
}
