#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeWcsUtil.h"
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
    if (visibleBounds == this->lastOverlayVisibleBounds && viewportSize == this->lastOverlayViewportSize
        && currentFrame == this->lastRenderedWcsFrame && currentFormat == this->lastRenderedWcsFormat) {
        return;
    }
    this->lastOverlayVisibleBounds = visibleBounds;
    this->lastOverlayViewportSize = viewportSize;
    this->lastRenderedWcsFrame = currentFrame;
    this->lastRenderedWcsFormat = currentFormat;

    constexpr double leftMargin = 168.;
    constexpr double axisX = 136.;
    constexpr double bottomMargin = 58.;
    constexpr double rightMargin = 34.;
    constexpr double topMargin = 28.;
    configureAxisActor(this->overlayXAxis, axisX, bottomMargin, size[0] - rightMargin, bottomMargin);
    configureAxisActor(this->overlayYAxis, axisX, size[1] - topMargin, axisX, bottomMargin);
    this->overlayXAxis->SetTitle("");
    this->overlayXTitleActor->SetInput(this->remoteOverlayAxisTitle(0).toStdString().c_str());
    this->overlayXTitleActor->SetDisplayPosition((axisX + (size[0] - rightMargin)) / 2,
                                                 static_cast<int>(bottomMargin / 2.0) - 2);
    this->overlayYAxis->SetTitle("");
    this->overlayYTitleActor->SetInput(this->remoteOverlayAxisTitle(1).toStdString().c_str());
    this->overlayYTitleActor->SetDisplayPosition(static_cast<int>(leftMargin / 3.0), size[1] / 2);

    bool xOk = false;
    const double xMin = this->remoteVoxelToWcs(0, visible.xmin, &xOk);
    const double xMax = this->remoteVoxelToWcs(0, visible.xmax, &xOk);
    bool yOk = false;
    const double yMin = this->remoteVoxelToWcs(1, visible.ymin, &yOk);
    const double yMax = this->remoteVoxelToWcs(1, visible.ymax, &yOk);
    this->overlayXAxis->SetRange(xOk ? xMin : visible.xmin, xOk ? xMax : visible.xmax);
    this->overlayYAxis->SetRange(yOk ? yMax : visible.ymax, yOk ? yMin : visible.ymin);
    this->overlayXAxis->LabelVisibilityOff();
    this->overlayYAxis->LabelVisibilityOff();
    this->overlayXAxis->VisibilityOn();
    this->overlayYAxis->VisibilityOn();
    this->overlayXTitleActor->VisibilityOn();
    this->overlayYTitleActor->VisibilityOn();
    for (int i = 0; i < overlayTickCount; ++i) {
        const double t = overlayTickCount == 1 ? 0.0
                                               : static_cast<double>(i)
                        / static_cast<double>(overlayTickCount - 1);
        const double voxelX = visible.xmin + t * (visible.xmax - visible.xmin);
        const double voxelY = visible.ymin + t * (visible.ymax - visible.ymin);
        bool tickXOk = false;
        bool tickYOk = false;
        double tickX = this->remoteVoxelToWcs(0, voxelX, &tickXOk);
        double tickY = this->remoteVoxelToWcs(1, voxelY, &tickYOk);
        double frameX = tickX;
        double frameY = tickY;
        if (tickXOk && tickYOk && this->remoteHasCelestialAxes()) {
            this->convertRemoteCelestialCoordinates(tickX, tickY, frameX, frameY);
        }

        this->overlayXTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(0, frameX).toStdString().c_str());
        this->overlayXTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX + t * ((size[0] - rightMargin) - axisX)),
                static_cast<int>(bottomMargin - 18));
        this->overlayXTickActors[static_cast<std::size_t>(i)]->VisibilityOn();

        this->overlayYTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(1, frameY).toStdString().c_str());
        this->overlayYTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX - 10),
                static_cast<int>(bottomMargin + t * ((size[1] - topMargin) - bottomMargin)));
        this->overlayYTickActors[static_cast<std::size_t>(i)]->VisibilityOn();
    }
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
                       .arg(size[1] - topMargin, 0, 'g', 12)
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

QString vtkWindowImage::formatRemoteOverlayCoordinate(int axis, double value) const
{
    if (!this->remoteHasCelestialAxes()) {
        return QString::number(value, 'g', 8);
    }
    if (!this->useSexagesimalWcsFormat) {
        return this->formatDegreeCoordinate(value);
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

QString vtkWindowImage::formatDegreeCoordinate(double value) const
{
    QString text = QString::number(value, 'f', 2);
    if (text.startsWith(u'-') && text.mid(1).toDouble() == 0.0)
        text.remove(0, 1);   // "-0.00": a tiny negative value at the origin
    return text;
}

QString vtkWindowImage::selectedFrameAxisTitle(int axis) const
{
    return this->remoteOverlayAxisTitle(axis);
}
