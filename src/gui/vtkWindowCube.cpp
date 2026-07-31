#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"
#include "CubeWcsUtil.h"
#include "CubeViewportGeometry.h"
#include "Pane2DView.h"
#include "CubeStatsUtil.h"

#include "qcustomplot.h"
#include "ColorMaps.h"
#include "ChannelOverlay.h"
#include "CubeUiAssembler.h"
#include "WorkspaceChrome.h"
#include "InspectorPanel.h"
#include "theme/SegmentedToggle.h"
#include "app/ProductRegistry.h"

#include <QEvent>
#include <QIcon>
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
#include <QAbstractSpinBox>
#include <QGridLayout>
#include <QMenu>
#include <QShortcut>
#include <QSettings>
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
#include <QVTKOpenGLNativeWidget.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

using namespace Qt::StringLiterals;
using namespace CubeWcs;
using namespace CubeGeom;
using namespace CubeStats;

namespace {
constexpr double pi = 3.14159265358979323846;
const QString kMomentBackendTooltip =
        QStringLiteral("Moment computation requires a backend-visible dataset. Use File → Open Remote or copy the dataset to the backend host.");
const QString kPvBackendTooltip =
        QStringLiteral("PV computation requires a backend-visible dataset. Use File → Open Remote or copy the dataset to the backend host.");
const QString kIsosurfaceBackendTooltip =
        QStringLiteral("Isosurface extraction requires a backend-visible dataset.");
const QString kNoiseBackendTooltip =
        QStringLiteral("Noise estimation requires a backend-visible dataset. Use File → Open Remote or copy the dataset to the backend host.");




// RemotePvFetchResult moved to PvTypes.h (included via PvController.h)

enum class SanityLevel
{
    Ok,
    Warning,
    Unknown,
};

struct SanityReport
{
    SanityLevel level{ SanityLevel::Ok };
    QString summary;
    QString details;
};




double computeBlankFraction(vtkImageData *imageData)
{
    if (!imageData) {
        return 0.0;
    }
    int extent[6];
    imageData->GetExtent(extent);
    const qsizetype total = static_cast<qsizetype>(extent[1] - extent[0] + 1)
            * static_cast<qsizetype>(extent[3] - extent[2] + 1)
            * static_cast<qsizetype>(std::max(1, extent[5] - extent[4] + 1));
    if (total <= 0) {
        return 0.0;
    }

    // Fast path: float / single-component scalars (the backend always returns
    // float32 volumes). Iterating with a raw pointer is ~100× faster than the
    // virtual GetScalarComponentAsDouble loop — the slow path used to take
    // ~3 s on a 512×512×200 cube and was being called 3× per full-res swap,
    // accounting for the entire observed UI freeze.
    if (imageData->GetScalarType() == VTK_FLOAT && imageData->GetNumberOfScalarComponents() == 1) {
        const auto *values = static_cast<const float *>(imageData->GetScalarPointer());
        qsizetype blanked = 0;
        for (qsizetype i = 0; i < total; ++i) {
            if (!std::isfinite(values[i])) {
                ++blanked;
            }
        }
        return static_cast<double>(blanked) / static_cast<double>(total);
    }

    // Generic fallback for non-float / multi-component scalars.
    qsizetype blanked = 0;
    for (int z = extent[4]; z <= extent[5]; ++z) {
        for (int y = extent[2]; y <= extent[3]; ++y) {
            for (int x = extent[0]; x <= extent[1]; ++x) {
                if (!std::isfinite(imageData->GetScalarComponentAsDouble(x, y, z, 0))) {
                    ++blanked;
                }
            }
        }
    }
    return static_cast<double>(blanked) / static_cast<double>(total);
}








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











SanityReport buildCubeSanityReport(const std::array<QString, 3> &ctype,
                                   const std::array<QString, 3> &cunit,
                                   const std::array<double, 3> &crval,
                                   const std::array<double, 3> &crpix,
                                   const std::array<double, 3> &cdelt,
                                   double blankFraction)
{
    const auto pairRecognized = [&ctype]() {
        const QString c1 = ctype[0].trimmed().toUpper();
        const QString c2 = ctype[1].trimmed().toUpper();
        return (c1.startsWith(u"GLON"_s) && c2.startsWith(u"GLAT"_s))
                || (c1.startsWith(u"ELON"_s) && c2.startsWith(u"ELAT"_s))
                || (c1.startsWith(u"RA"_s) && c2.startsWith(u"DEC"_s));
    };

    QStringList warnings;
    QStringList unknowns;

    for (int axis = 0; axis < 3; ++axis) {
        if (axisHasAnyMetadata(ctype[axis], cunit[axis], crval[axis], crpix[axis], cdelt[axis])
            && !axisHasLinearWcs(ctype[axis], crval[axis], crpix[axis], cdelt[axis])) {
            warnings << u"Axis %1 has incomplete WCS metadata."_s.arg(axis + 1);
        }
    }

    const bool axis0Celestial = isCelestialLikeAxis(ctype[0]);
    const bool axis1Celestial = isCelestialLikeAxis(ctype[1]);
    if (axis0Celestial != axis1Celestial) {
        warnings << u"Only one spatial axis looks celestial; the spatial WCS pairing is incomplete."_s;
    } else if (axis0Celestial && !pairRecognized()) {
        warnings << u"Celestial axis pairing is not recognized as FK5, Galactic, or Ecliptic."_s;
    } else if (!axis0Celestial && !axis1Celestial) {
        unknowns << u"Celestial WCS metadata unavailable; slice/moment overlays may fall back to voxel coordinates."_s;
    }

    const QString spectralType = upperCtype(ctype[2]);
    if (spectralType.isEmpty()) {
        unknowns << u"Spectral-axis metadata incomplete; axis 3 falls back to generic/channel semantics."_s;
    } else if ((spectralType.startsWith(u"FREQ"_s) || spectralType.contains(u"VRAD"_s)
                || spectralType.contains(u"VOPT"_s) || spectralType.contains(u"VELO"_s))
               && cunit[2].trimmed().isEmpty()) {
        warnings << u"Spectral axis has physical semantics but no unit is declared."_s;
    } else if (!spectralType.contains(u"CHAN"_s) && !spectralType.startsWith(u"FREQ"_s)
               && !spectralType.contains(u"VRAD"_s) && !spectralType.contains(u"VOPT"_s)
               && !spectralType.contains(u"VELO"_s)) {
        unknowns << u"Spectral-axis semantics are ambiguous (CTYPE3=%1)."_s.arg(ctype[2]);
    }

    if (blankFraction >= 0.2) {
        warnings << u"Current loaded cube block is heavily blanked/NaN (%1%)."_s.arg(blankFraction * 100.0, 0, 'f', 1);
    }

    if (!warnings.isEmpty() || !unknowns.isEmpty()) {
        unknowns << u"Backend preview/full/ROI preserve dataset coordinates, but incomplete metadata lowers WCS confidence."_s;
    }

    SanityReport report;
    if (!warnings.isEmpty()) {
        report.level = SanityLevel::Warning;
        report.summary = u"Sanity: Warning (%1)"_s.arg(warnings.size());
    } else if (!unknowns.isEmpty()) {
        report.level = SanityLevel::Unknown;
        report.summary = u"Sanity: Unknown / incomplete"_s;
    } else {
        report.level = SanityLevel::Ok;
        report.summary = u"Sanity: OK"_s;
    }

    QString details;
    if (!warnings.isEmpty()) {
        details += u"Warnings:\n- %1"_s.arg(warnings.join(u"\n- "_s));
    }
    if (!unknowns.isEmpty()) {
        if (!details.isEmpty()) {
            details += u"\n\n"_s;
        }
        details += u"Unknown / incomplete:\n- %1"_s.arg(unknowns.join(u"\n- "_s));
    }
    if (details.isEmpty()) {
        details = u"No common FITS/WCS issues detected in the current cube metadata."_s;
    }
    report.details = details;
    return report;
}

QString formatCubeBoundsSummary(vtkImageData *imageData)
{
    if (!imageData) {
        return u"unavailable"_s;
    }

    double bounds[6];
    imageData->GetBounds(bounds);
    return u"x=%1..%2 y=%3..%4 z=%5..%6"_s.arg(std::lround(bounds[0]))
            .arg(std::lround(bounds[1]))
            .arg(std::lround(bounds[2]))
            .arg(std::lround(bounds[3]))
            .arg(std::lround(bounds[4]))
            .arg(std::lround(bounds[5]));
}





struct SanitizedCubeStats
{
    std::array<double, 2> visibleRange{ 0., 0. };
    double invisibleSentinel{ -1.0 };
};

SanitizedCubeStats sanitizeCubeScalarsInPlace(vtkImageData *image)
{
    if (!image) {
        return { { 0., 0. }, -1.0 };
    }

    if (image->GetScalarType() != VTK_FLOAT || image->GetNumberOfScalarComponents() != 1) {
        double scalarRange[2] = { 0., 0. };
        image->GetScalarRange(scalarRange);
        const double range = scalarRange[1] - scalarRange[0];
        const double sentinel = range > 0.0 ? (scalarRange[0] - 0.01 * range) : (scalarRange[0] - 1.0);
        return { { scalarRange[0], scalarRange[1] }, sentinel };
    }

    int extent[6];
    image->GetExtent(extent);
    const auto voxelCount = static_cast<qsizetype>(extent[1] - extent[0] + 1)
            * static_cast<qsizetype>(extent[3] - extent[2] + 1)
            * static_cast<qsizetype>(extent[5] - extent[4] + 1);
    if (voxelCount <= 0) {
        return { { 0., 0. }, -1.0 };
    }

    auto *values = static_cast<float *>(image->GetScalarPointer());
    double rangeMin = std::numeric_limits<double>::infinity();
    double rangeMax = -std::numeric_limits<double>::infinity();
    for (qsizetype i = 0; i < voxelCount; ++i) {
        const float value = values[i];
        if (!std::isfinite(value)) {
            continue;
        }

        rangeMin = std::min(rangeMin, static_cast<double>(value));
        rangeMax = std::max(rangeMax, static_cast<double>(value));
    }

    if (!std::isfinite(rangeMin) || !std::isfinite(rangeMax)) {
        rangeMin = 0.;
        rangeMax = 0.;
    }

    const double range = rangeMax - rangeMin;
    const float invisibleSentinel =
            static_cast<float>(range > 0.0 ? (rangeMin - 0.01 * range) : (rangeMin - 1.0));

    qsizetype replaced = 0;
    for (qsizetype i = 0; i < voxelCount; ++i) {
        if (!std::isfinite(values[i])) {
            values[i] = invisibleSentinel;
            ++replaced;
        }
    }

    if (replaced > 0) {
        image->Modified();
        qDebug().noquote()
                << QStringLiteral("[nan] replaced %1 NaN voxels with sentinel %2")
                           .arg(replaced)
                           .arg(invisibleSentinel, 0, 'g', 12);
    }
    qDebug().noquote()
            << QStringLiteral("[nan] cube visible range after sanitization min=%1 max=%2")
                       .arg(rangeMin, 0, 'g', 12)
                       .arg(rangeMax, 0, 'g', 12);
    return { { rangeMin, rangeMax }, invisibleSentinel };
}

struct SanitizedCubeFullStats
{
    std::array<double, 2> visibleRange{ 0., 0. };
    double invisibleSentinel{ -1.0 };
    double mean{ 0. };
    double rms{ 0. };
    // Fraction of NaN/inf voxels that were replaced by the sentinel during
    // sanitization. Carried in the result so updateSanityPanel does not need
    // to rescan the (possibly multi-GB) volume on the GUI thread.
    double blankFraction{ 0. };
};

// Worker-thread variant: combines sanitize + range + mean/rms in two passes
// over the voxel buffer, instead of three (sanitize=2 passes, stats=1 pass).
// Designed to run off the GUI thread so the swap to full-resolution data is
// cheap (no scan needed in applyCubeOpenResult when result.preSanitized=true).
SanitizedCubeFullStats sanitizeAndStatsCubeInPlace(vtkImageData *image)
{
    SanitizedCubeFullStats stats;
    if (!image) {
        return stats;
    }

    if (image->GetScalarType() != VTK_FLOAT || image->GetNumberOfScalarComponents() != 1) {
        double scalarRange[2] = { 0., 0. };
        image->GetScalarRange(scalarRange);
        const double range = scalarRange[1] - scalarRange[0];
        stats.visibleRange = { scalarRange[0], scalarRange[1] };
        stats.invisibleSentinel =
                range > 0.0 ? (scalarRange[0] - 0.01 * range) : (scalarRange[0] - 1.0);
        return stats;
    }

    int extent[6];
    image->GetExtent(extent);
    const auto voxelCount = static_cast<qsizetype>(extent[1] - extent[0] + 1)
            * static_cast<qsizetype>(extent[3] - extent[2] + 1)
            * static_cast<qsizetype>(extent[5] - extent[4] + 1);
    if (voxelCount <= 0) {
        return stats;
    }

    auto *values = static_cast<float *>(image->GetScalarPointer());
    double rangeMin = std::numeric_limits<double>::infinity();
    double rangeMax = -std::numeric_limits<double>::infinity();
    double sum = 0.;
    double sumSq = 0.;
    qsizetype finiteCount = 0;
    for (qsizetype i = 0; i < voxelCount; ++i) {
        const float value = values[i];
        if (!std::isfinite(value)) {
            continue;
        }
        const double d = static_cast<double>(value);
        if (d < rangeMin) rangeMin = d;
        if (d > rangeMax) rangeMax = d;
        sum += d;
        sumSq += d * d;
        ++finiteCount;
    }

    if (!std::isfinite(rangeMin) || !std::isfinite(rangeMax)) {
        rangeMin = 0.;
        rangeMax = 0.;
    }

    const double range = rangeMax - rangeMin;
    const float invisibleSentinel =
            static_cast<float>(range > 0.0 ? (rangeMin - 0.01 * range) : (rangeMin - 1.0));

    qsizetype replaced = 0;
    for (qsizetype i = 0; i < voxelCount; ++i) {
        if (!std::isfinite(values[i])) {
            values[i] = invisibleSentinel;
            ++replaced;
        }
    }

    if (replaced > 0) {
        image->Modified();
    }

    stats.visibleRange = { rangeMin, rangeMax };
    stats.invisibleSentinel = invisibleSentinel;
    if (finiteCount > 0) {
        stats.mean = sum / static_cast<double>(finiteCount);
        stats.rms = std::sqrt(sumSq / static_cast<double>(finiteCount));
    }
    stats.blankFraction = static_cast<double>(replaced) / static_cast<double>(voxelCount);
    return stats;
}






void refitCubeCamera(vtkRenderer *renderer, vtkImageData *cubeImage)
{
    if (!renderer || !cubeImage) {
        return;
    }

    double bounds[6];
    cubeImage->GetBounds(bounds);
    if (!validBounds(bounds)) {
        return;
    }

    auto *camera = renderer->GetActiveCamera();
    double position[3];
    double focalPoint[3];
    camera->GetPosition(position);
    camera->GetFocalPoint(focalPoint);

    double direction[3] = { position[0] - focalPoint[0], position[1] - focalPoint[1],
                            position[2] - focalPoint[2] };
    double distance = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1]
                                + direction[2] * direction[2]);
    if (distance <= 1e-6) {
        direction[0] = 0.;
        direction[1] = 0.;
        direction[2] = 1.;
        distance = 1.;
    } else {
        direction[0] /= distance;
        direction[1] /= distance;
        direction[2] /= distance;
    }

    const double center[3] = { 0.5 * (bounds[0] + bounds[1]), 0.5 * (bounds[2] + bounds[3]),
                               0.5 * (bounds[4] + bounds[5]) };
    const double dx = bounds[1] - bounds[0];
    const double dy = bounds[3] - bounds[2];
    const double dz = bounds[5] - bounds[4];
    const double radius = std::max(1e-6, 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz));
    const double viewAngleRad = std::max(1e-3, camera->GetViewAngle() * pi / 180.);
    const double fitDistance = radius / std::sin(viewAngleRad * 0.5);
    const double finalDistance = std::max(distance, fitDistance);

    camera->SetFocalPoint(center);
    camera->SetPosition(center[0] + direction[0] * finalDistance,
                        center[1] + direction[1] * finalDistance,
                        center[2] + direction[2] * finalDistance);
}

AsyncIsosurfaceResult computeIsosurface(vtkSmartPointer<vtkImageData> image, double isoValue,
                                        int requestId)
{
    QElapsedTimer timer;
    timer.start();
    AsyncIsosurfaceResult result;
    result.requestId = requestId;

    if (!image) {
        return result;
    }

    vtkNew<vtkFlyingEdges3D> filter;
    filter->SetInputData(image);
    filter->SetValue(0, isoValue);
    filter->ComputeNormalsOff();
    filter->ComputeGradientsOff();
    filter->Update();

    vtkSmartPointer<vtkPolyData> mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->ShallowCopy(filter->GetOutput());
    result.mesh = mesh;
    result.meshInDisplayCoordinates = true;
    qDebug().noquote()
            << QStringLiteral("[perf][isosurface] compute: %1 ms").arg(timer.elapsed());
    return result;
}

vtkSmartPointer<vtkImageData> decodeRemoteVolume(const QByteArray &data, int width, int height, int depth)
{
    const qsizetype expectedBytes = static_cast<qsizetype>(width) * height
            * static_cast<qsizetype>(depth) * static_cast<qsizetype>(sizeof(float));
    if (width <= 0 || height <= 0 || depth <= 0 || data.size() != expectedBytes) {
        return nullptr;
    }

    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, width - 1, 0, height - 1, 0, depth - 1);
    image->SetOrigin(0., 0., 0.);
    image->SetSpacing(1., 1., 1.);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(image->GetScalarPointer(), data.constData(), static_cast<std::size_t>(expectedBytes));
    return image;
}


vtkSmartPointer<vtkPolyData> decodeRemoteIsosurface(const QByteArray &pointsData,
                                                    const QByteArray &polysData, int numPoints,
                                                    int numPolys)
{
    if (numPoints <= 0 || numPolys <= 0) {
        return vtkSmartPointer<vtkPolyData>::New();
    }

    const qsizetype expectedPointsBytes =
            static_cast<qsizetype>(numPoints) * 3 * static_cast<qsizetype>(sizeof(float));
    const qsizetype expectedPolysBytes =
            static_cast<qsizetype>(numPolys) * 4 * static_cast<qsizetype>(sizeof(qint32));
    if (pointsData.size() != expectedPointsBytes || polysData.size() != expectedPolysBytes) {
        return nullptr;
    }

    vtkNew<vtkFloatArray> pointArray;
    pointArray->SetNumberOfComponents(3);
    pointArray->SetNumberOfTuples(numPoints);
    std::memcpy(pointArray->GetVoidPointer(0), pointsData.constData(),
                static_cast<std::size_t>(expectedPointsBytes));

    vtkNew<vtkPoints> points;
    points->SetData(pointArray);

    const auto *rawPolys = reinterpret_cast<const qint32 *>(polysData.constData());
    vtkNew<vtkIdTypeArray> legacyCells;
    legacyCells->SetNumberOfValues(static_cast<vtkIdType>(numPolys) * 4);
    for (vtkIdType i = 0; i < legacyCells->GetNumberOfValues(); ++i) {
        legacyCells->SetValue(i, static_cast<vtkIdType>(rawPolys[i]));
    }

    vtkNew<vtkCellArray> cells;
    cells->ImportLegacyFormat(legacyCells);

    vtkSmartPointer<vtkPolyData> mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(cells);
    mesh->BuildCells();
    mesh->BuildLinks();
    return mesh;
}

// Pick a downsample factor that brings the preview voxel count into the
// caller's MB target. All voxels are float32 (4 bytes) so the math is
// just ``(nx/f) * (ny/f) * (nz/f) * 4 <= target_bytes``.
//
// Why uniform-per-axis instead of axis-asymmetric: the backend's
// /v1/cube/preview takes a single integer parameter that's applied
// equally to all three axes. Asymmetric downsampling would require a
// new backend endpoint. Uniform is "wrong" for very flat cubes but
// preserves the spatial structure, which is what the preview is for.
//
// Floor at 1 (don't UPsample a small cube), ceiling at 64 (beyond
// that we lose too much spatial detail even on huge cubes, better to
// pay slightly more bandwidth).
//
// Returns 1 for cubes that already fit (preview == full).
static int computePreviewDownsample(int nx, int ny, int nz, double targetMb)
{
    if (nx <= 0 || ny <= 0 || nz <= 0) return 4;   // fall back to legacy
    const double targetVoxels = (targetMb * 1024.0 * 1024.0) / 4.0;
    const double totalVoxels  = static_cast<double>(nx)
                                * static_cast<double>(ny)
                                * static_cast<double>(nz);
    if (totalVoxels <= targetVoxels) return 1;
    const double ratio  = totalVoxels / targetVoxels;
    const double factor = std::cbrt(ratio);
    int f = static_cast<int>(std::ceil(factor));
    return std::clamp(f, 1, 64);
}

RemoteCubePreviewResult fetchRemotePreview(const QString &backendUrl, const QString &datasetId,
                                           int downsample, const QString &sessionId,
                                           const QString &backendToken)
{
    qDebug().noquote() << QStringLiteral("[preview-debug] fetcher start ds=%1 down=%2").arg(datasetId).arg(downsample);
    RemoteCubePreviewResult result;
    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    const auto response = client.requestPreview(datasetId, downsample);
    qDebug().noquote() << QStringLiteral("[preview-debug] fetcher response valid=%1 error=%2 w=%3 h=%4 d=%5")
                                  .arg(response.valid).arg(response.error)
                                  .arg(response.width).arg(response.height).arg(response.depth);
    if (!response.valid) {
        result.errorMessage = response.error.isEmpty() ? u"Remote preview request failed."_s
                                                       : response.error;
        return result;
    }

    if (response.scalarType != u"float32"_s) {
        result.errorMessage = u"Unsupported remote preview scalar type."_s;
        return result;
    }

    result.cubeImageData = decodeRemoteVolume(response.data, response.width, response.height,
                                              response.depth);
    if (!result.cubeImageData) {
        result.errorMessage = u"Invalid remote preview payload."_s;
        return result;
    }

    result.valid = true;
    result.cubeRange = { response.rangeMin, response.rangeMax };
    result.dataExtent = { 0, response.width - 1, 0, response.height - 1, 0, response.depth - 1 };
    return result;
}


RemoteCubeSubvolumeResult fetchRemoteSubvolume(const QString &backendUrl, const QString &datasetId,
                                               const std::array<int, 6> &roi, const QString &sessionId,
                                               const QString &backendToken)
{
    RemoteCubeSubvolumeResult result;

    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    const auto response = client.requestSubvolume(datasetId, roi[0], roi[1], roi[2], roi[3], roi[4],
                                                  roi[5]);
    if (!response.valid) {
        result.errorMessage =
                response.error.isEmpty() ? u"Remote subvolume request failed."_s : response.error;
        return result;
    }

    if (response.scalarType != u"float32"_s) {
        result.errorMessage = u"Unsupported remote subvolume scalar type."_s;
        return result;
    }

    result.cubeImageData = decodeRemoteVolume(response.data, response.width, response.height,
                                              response.depth);
    if (!result.cubeImageData) {
        result.errorMessage = u"Invalid remote subvolume payload."_s;
        return result;
    }

    result.cubeImageData->SetOrigin(static_cast<double>(roi[0]), static_cast<double>(roi[2]),
                                    static_cast<double>(roi[4]));
    result.cubeImageData->SetSpacing(1., 1., 1.);
    result.cubeImageData->Modified();

    result.valid = true;
    result.dataExtent = roi;
    // Sanitize + compute range/mean/rms here on the worker thread so the GUI
    // does not stall scanning the full volume when swapping to full-resolution.
    const auto stats = sanitizeAndStatsCubeInPlace(result.cubeImageData);
    result.cubeRange = stats.visibleRange;
    result.invisibleSentinel = stats.invisibleSentinel;
    result.cubeMean = stats.mean;
    result.cubeRms = stats.rms;
    result.blankFraction = stats.blankFraction;
    return result;
}

// fetchRemotePv() moved to PvController.cpp

AsyncIsosurfaceResult fetchRemoteIsosurface(const QString &backendUrl, const QString &datasetId,
                                            double isoValue, int requestId, const QString &sessionId,
                                            const QString &backendToken)
{
    AsyncIsosurfaceResult result;
    result.requestId = requestId;

    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    const auto response = client.requestIsosurface(datasetId, isoValue);
    qDebug().noquote()
            << QStringLiteral("[remote-iso] response valid=%1 error=%2 num_points=%3 num_polys=%4")
                       .arg(response.valid)
                       .arg(response.error)
                       .arg(response.numPoints)
                       .arg(response.numPolys);
    qDebug().noquote()
            << QStringLiteral("[remote-iso] decode points bytes=%1 polys bytes=%2")
                       .arg(response.pointsData.size())
                       .arg(response.polysData.size());
    if (!response.valid) {
        result.errorMessage =
                response.error.isEmpty() ? u"Remote isocontour request failed."_s : response.error;
        return result;
    }

    result.mesh = decodeRemoteIsosurface(response.pointsData, response.polysData, response.numPoints,
                                         response.numPolys);
    if (!result.mesh) {
        result.errorMessage = u"Invalid remote isocontour payload."_s;
        return result;
    }
    result.meshInDisplayCoordinates = false;
    qDebug().noquote()
            << QStringLiteral("[remote-iso] mesh points=%1 polys=%2")
                       .arg(result.mesh->GetNumberOfPoints())
                       .arg(result.mesh->GetNumberOfPolys());
    return result;
}

} // anonymous namespace

vtkWindowCube::vtkWindowCube(const QString &filepath, const QString &backendUrl,
                             const QString &datasetId, int remoteWidth, int remoteHeight,
                             int remoteDepth, const std::array<double, 3> &remoteSpacing,
                             const std::array<double, 3> &remoteOrigin,
                             const std::array<QString, 3> &remoteCtype,
                             const std::array<QString, 3> &remoteCunit,
                             const std::array<double, 3> &remoteCrval,
                             const std::array<double, 3> &remoteCrpix,
                             const std::array<double, 3> &remoteCdelt,
                             const QString &remoteDegenerateAxesSummary,
                             const QString &remoteSpectralAxisType,
                             const QString &remoteSpectralAxisUnit,
                             const QString &remoteWcsStatus,
                             const QString &remoteWcsWarningMessage,
                             const QString &remoteSessionId,
                             const QString &remoteBackendToken,
                             const QString &recommendedLoadMode,
                             qint64 fileSizeBytes,
                             qint64 fullResThresholdBytes,
                             QWidget *parent)
    : QMainWindow(parent),
      ui(new Ui::vtkWindowCube),
      filepath(filepath),
      remoteBackendUrl(backendUrl),
      remoteDatasetId(datasetId),
      remoteSessionId(remoteSessionId),
      remoteBackendToken(remoteBackendToken),
      remoteDatasetWidth(remoteWidth),
      remoteDatasetHeight(remoteHeight),
      remoteDatasetDepth(remoteDepth),
      remoteDatasetSpacing(remoteSpacing),
      remoteDatasetOrigin(remoteOrigin),
      remoteDatasetCtype(remoteCtype),
      remoteDatasetCunit(remoteCunit),
      remoteDatasetCrval(remoteCrval),
      remoteDatasetCrpix(remoteCrpix),
      remoteDatasetCdelt(remoteCdelt),
      remoteDegenerateAxesSummary(remoteDegenerateAxesSummary),
      remoteSpectralAxisType(remoteSpectralAxisType),
      remoteSpectralAxisUnit(remoteSpectralAxisUnit),
      remoteWcsStatus(remoteWcsStatus),
      remoteWcsWarningMessage(remoteWcsWarningMessage),
      recommendedLoadMode(recommendedLoadMode),
      datasetFileSizeBytes(fileSizeBytes),
      datasetFullResThresholdBytes(fullResThresholdBytes),
      lutCustomizer(nullptr),
      profileWidget(nullptr),
      level(15)
{
    ui->setupUi(this);
    this->setupViewerToolbar();

    // Pre-create the QActions and QActionGroups the sidebar's "Cube Extras"
    // page binds to. Menu placement, tooltips, presets and signal handlers
    // are configured further down in the constructor — but the pointers must
    // exist before setupSidebar() so the sidebar can wire its widgets.
    this->actionShowCuttingPlane = new QAction(u"Show Cutting Plane"_s, this);
    this->actionShowCuttingPlane->setCheckable(true);
    this->actionShowCuttingPlane->setChecked(true);
    this->actionShow3dWcsAxes = new QAction(u"Show 3D WCS Axes"_s, this);
    this->actionShow3dWcsAxes->setCheckable(true);
    this->actionShow3dWcsAxes->setChecked(false);
    this->actionPlaySlices = new QAction(u"Play Slices"_s, this);
    this->actionPlaySlices->setCheckable(true);
    this->actionPlaySlices->setChecked(false);
    this->actionPickSpectrum3d = new QAction(u"Pick Spectrum on Plane Click"_s, this);
    this->actionPickSpectrum3d->setCheckable(true);
    this->actionPickSpectrum3d->setChecked(false);
    this->cuttingPlaneOpacityGroup = new QActionGroup(this);
    this->cuttingPlaneOpacityGroup->setExclusive(true);
    this->animationFpsGroup = new QActionGroup(this);
    this->animationFpsGroup->setExclusive(true);
    this->animationModeGroup = new QActionGroup(this);
    this->animationModeGroup->setExclusive(true);

    this->setupSidebar();
    this->setupWorkspaceDocks();
    // The file path is shown in the Layout B path-pill toolbar; the OS title
    // bar carries the brand name to avoid duplicate context.
    this->setWindowTitle(u"VisIVO Visual Analytics"_s);
    this->setAttribute(Qt::WA_DeleteOnClose);
    qDebug().noquote()
            << QStringLiteral("[wcs] remote metadata loaded ctype=%1,%2,%3 cdelt=%4,%5,%6")
                       .arg(this->remoteDatasetCtype[0], this->remoteDatasetCtype[1],
                            this->remoteDatasetCtype[2])
                       .arg(this->remoteDatasetCdelt[0], 0, 'g', 12)
                       .arg(this->remoteDatasetCdelt[1], 0, 'g', 12)
                       .arg(this->remoteDatasetCdelt[2], 0, 'g', 12);
    this->cubeOpenStateLabel = new QLabel(this);
    this->cubeOpenStateLabel->setStyleSheet(u"QLabel { font-weight: 600; padding-left: 8px; }"_s);
    this->cubeOpenStateLabel->hide();
    this->statusBar()->addPermanentWidget(this->cubeOpenStateLabel);
    this->hoverReadoutLabel = new QLabel(this);
    // Compact cursor readout (spec §1.1): JetBrains Mono 10 px. A fixed minimum width
    // sized to a representative full string stops the toolbar reflowing as the cursor
    // moves; overflow elides at the tail rather than the head.
    this->hoverReadoutLabel->setStyleSheet(
            u"QLabel { padding-left: 8px; font-family: 'JetBrains Mono','SF Mono',monospace;"
            u" font-size: 10px; }"_s);
    this->hoverReadoutLabel->setMinimumWidth(
            this->hoverReadoutLabel->fontMetrics().horizontalAdvance(
                    u"l 104.89  b +68.53  ·  1.37e-02 Jy/beam  ·  ch 1"_s)
            + 12);
    this->hoverReadoutLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addPermanentWidget(this->hoverReadoutLabel, 1);
    m_uiAssembler->initStatusBarLabels(this->statusBar());
    this->momentProvenanceLabel = new QLabel(this);
    this->momentProvenanceLabel->setStyleSheet(u"QLabel { padding-left: 8px; color: palette(window-text); }"_s);
    this->momentProvenanceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addPermanentWidget(this->momentProvenanceLabel);
    this->catalogueInfoLabel = new QLabel(this);
    this->catalogueInfoLabel->setStyleSheet(u"QLabel { padding-left: 8px; color: palette(window-text); }"_s);
    this->catalogueInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addPermanentWidget(this->catalogueInfoLabel);
    this->updateCatalogueInfoPanel();
    this->updateMomentProvenancePanel();
    this->actionShowWcsAxes = new QAction(u"Show WCS Axes"_s, this);
    this->actionShowWcsAxes->setCheckable(true);
    this->actionShowWcsAxes->setChecked(this->showWcsAxes);
    ui->menuView->addSeparator();
    ui->menuView->addAction(this->actionShowControlsSidebar);
    ui->menuView->addAction(this->actionShowWcsAxes);
    ui->menuView->addSeparator();
    // (The 3D/2D views are now central panes, not dockable views — their
    // show/hide is driven by the Inspector's 1/2/4 pane control.)
    this->actionResetLayout = new QAction(u"Reset Layout"_s, this);
    ui->menuView->addAction(this->actionResetLayout);
    QObject::connect(this->actionResetLayout, &QAction::triggered,
                     this, &vtkWindowCube::resetWorkspaceLayout);

    // Focus mode (§4.2): collapse both docks to rails and thin the status rail; a
    // toggle, not a mode — leaving it restores the exact pair it collapsed.
    auto *focusAct = new QAction(u"Focus mode"_s, this);
    focusAct->setCheckable(true);
#ifdef Q_OS_MACOS
    focusAct->setShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_F));
#else
    focusAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
#endif
    ui->menuView->addAction(focusAct);
    QObject::connect(focusAct, &QAction::toggled, this, [this](bool on) {
        if (!this->m_workspaceChrome)
            return;
        if (on) {
            this->m_focusPrevSessionCollapsed = this->m_workspaceChrome->sessionCollapsed();
            this->m_focusPrevInspectorCollapsed = this->m_workspaceChrome->inspectorCollapsed();
            this->m_workspaceChrome->setSessionCollapsed(true);
            this->m_workspaceChrome->setInspectorCollapsed(true);
        } else {
            this->m_workspaceChrome->setSessionCollapsed(this->m_focusPrevSessionCollapsed);
            this->m_workspaceChrome->setInspectorCollapsed(this->m_focusPrevInspectorCollapsed);
        }
        this->m_workspaceChrome->setStatusRailCompact(on);
    });

    // ── View → Rendering submenu (SSR mode toggle) ────────────────────────────
    this->setupRenderModeUi();

    // Centre toolbar + per-pane headers (Step 1). Runs after setupRenderModeUi()
    // so the render-mode badge exists to re-host into the 3D pane header, and
    // after hoverReadoutLabel exists to re-host as the toolbar cursor readout.
    this->setupPaneChrome();

    // Install the additive status rail AFTER setupRenderModeUi() (which appends
    // the render-mode/FPS badges as permanent widgets) so the rail's four slots
    // land to the right of every window-owned status widget, then start polling.
    if (this->m_workspaceChrome) {
        this->m_workspaceChrome->installStatusRail();
        this->m_workspaceChrome->installSessionDock(
                this->filepath, QFileInfo(this->filepath).fileName(), u"cube"_s);
        // Tree-selection = pane-selection: activate (or mount) the matching pane.
        connect(this->m_workspaceChrome, &WorkspaceChrome::sessionRowActivated, this,
                &vtkWindowCube::activateSessionView);
        // Reverse sync: activating a pane by any route highlights its Session-tree row.
        connect(this, &vtkWindowCube::activePaneChanged, this,
                [this](int) { syncSessionTreeToActivePane(); });
        // setupInspector()/start() run at the END of the constructor — the
        // Analysis-tab tools reference QActions created later below.
    }

    // ── Volume Rendering submenu ──────────────────────────────────────────
    ui->menuView->addSeparator();
    this->m_menuVolumeRendering = ui->menuView->addMenu(u"Volume Rendering"_s);
    // Enabled only when Volume mode is active; starts disabled (Volume is off by default).
    this->m_menuVolumeRendering->setEnabled(ui->actionVolume->isChecked());

    auto *blendGroup = new QActionGroup(this);
    blendGroup->setExclusive(true);

    this->m_actionComposite = this->m_menuVolumeRendering->addAction(u"Composite"_s);
    this->m_actionComposite->setCheckable(true);
    this->m_actionComposite->setChecked(true);
    this->m_actionComposite->setToolTip(
            u"Full ray-casting with opacity transfer function."_s);
    blendGroup->addAction(this->m_actionComposite);

    this->m_actionMIP = this->m_menuVolumeRendering->addAction(u"MIP (Max Intensity)"_s);
    this->m_actionMIP->setCheckable(true);
    this->m_actionMIP->setToolTip(u"Maximum intensity projection along each ray."_s);
    blendGroup->addAction(this->m_actionMIP);

    this->m_actionMinIP = this->m_menuVolumeRendering->addAction(u"MinIP (Min Intensity)"_s);
    this->m_actionMinIP->setCheckable(true);
    this->m_actionMinIP->setToolTip(u"Minimum intensity projection along each ray."_s);
    blendGroup->addAction(this->m_actionMinIP);

    QObject::connect(blendGroup, &QActionGroup::triggered, this, [this](QAction *action) {
        int idx = 0;
        if (action == this->m_actionMIP)   idx = 1;
        if (action == this->m_actionMinIP) idx = 2;
        this->applyVolumeBlendMode(idx);
        if (this->m_uiAssembler)
            this->m_uiAssembler->syncVolumeModeSidebar(idx);
        if (this->m_linkViews && !this->m_applyingLinkedUpdate)
            this->broadcastLinkedBlendMode();
    });

    QObject::connect(this->actionShowWcsAxes, &QAction::toggled, this, [this](bool checked) {
    this->showWcsAxes = checked;
    this->lastSliceOverlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN() };
    this->lastMomentOverlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN() };
    this->lastSliceOverlayViewportSize = { -1, -1 };
    this->lastMomentOverlayViewportSize = { -1, -1 };
    this->set2dWcsOverlayVisible(checked);
    // The gutter (and thus the image fit) depends on showWcsAxes: with the axes
    // off the image grows into the freed left/bottom margin. Reset both fit caches
    // so the hidden pane re-fits when next shown, then re-letterbox the visible one.
    this->m_slice2dFitSize = { -1, -1 };
    this->m_moment2dFitSize = { -1, -1 };
    this->layout2dPane(/*force=*/true);
    this->requestWcsOverlayRender();
    });

    // ── Cutting plane visibility + opacity submenu (View menu) ────────────
    // QAction itself was pre-created above setupSidebar(); here we configure
    // its tooltip and place it in the menu.
    ui->menuView->addSeparator();
    this->actionShowCuttingPlane->setToolTip(
            u"Show the 2D slice mapped as a textured plane inside the 3D cube view."_s);
    ui->menuView->addAction(this->actionShowCuttingPlane);
    QObject::connect(this->actionShowCuttingPlane, &QAction::toggled, this, [this](bool checked) {
        if (this->remoteCuttingPlaneActor) {
            this->remoteCuttingPlaneActor->SetVisibility(checked ? 1 : 0);
        }
        if (ui->vtkCube->renderWindow()) {
            ui->vtkCube->renderWindow()->Render();
        }
        // Mirror to remote renderer.
        QVariantMap cmd;
        cmd[u"type"_s]    = u"set_cutting_plane"_s;
        cmd[u"visible"_s] = checked;
        this->sendRemoteRenderStateCommand(cmd);
    });
    {
        auto *opacityMenu = ui->menuView->addMenu(u"Cutting Plane Opacity"_s);
        // cuttingPlaneOpacityGroup was pre-created above setupSidebar().
        const std::array<int, 5> presets { 25, 50, 75, 100, 0 };
        const int defaultPercent = 50;   // match setupCubeRenderer's plane opacity
        for (int pct : presets) {
            const QString label =
                    pct == 0 ? u"Transparent (0%)"_s : u"%1%"_s.arg(pct);
            auto *action = opacityMenu->addAction(label);
            action->setCheckable(true);
            action->setChecked(pct == defaultPercent);
            action->setData(pct);
            this->cuttingPlaneOpacityGroup->addAction(action);
        }
        QObject::connect(this->cuttingPlaneOpacityGroup, &QActionGroup::triggered, this,
                         [this](QAction *action) {
                             this->setCuttingPlaneOpacity(action->data().toInt() / 100.0);
                         });
    }

    // ── 3D WCS axes (vtkCubeAxesActor) toggle ────────────────────────────
    this->actionShow3dWcsAxes->setToolTip(
            u"Show a labeled bounding box in the 3D cube view with RA / DEC / "
            u"Velocity ticks derived from the WCS metadata."_s);
    ui->menuView->addAction(this->actionShow3dWcsAxes);
    QObject::connect(this->actionShow3dWcsAxes, &QAction::toggled, this,
                     [this](bool checked) { this->apply3dWcsAxesVisibility(checked); });

    // ── Pattern B: size-aware loading policy actions ────────────────────
    // Shown for every cube, but only meaningful when the backend
    // signalled ``preview_only`` (large cubes). For small cubes both
    // actions stay available — operator can re-trigger a full-res reload
    // after parameter changes or call up the crop dialog manually.
    {
        ui->menuView->addSeparator();
        auto *loadFullAction =
                ui->menuView->addAction(u"Load Full Resolution"_s);
        loadFullAction->setToolTip(
                u"Fetch the entire cube at full resolution from the backend. "
                u"For very large cubes this can be slow and memory-intensive; "
                u"prefer Crop Full Resolution when only a region is needed."_s);
        QObject::connect(loadFullAction, &QAction::triggered, this, [this]() {
            // Size guard — when the user picks "Load Full Resolution"
            // on a cube above the LocalFullThresholdMb setting (default
            // 256 MB, but tunable), pop a warning suggesting Remote.
            // For a 22 GB cube the local fetch downloads tens of GB
            // over the link, then spikes the desktop's RAM/VRAM; in
            // most cases the user has already seen the auto-picked
            // remote stream and just doesn't realise it's full quality.
            const QString warnWorkDir =
                    QDir::home().absoluteFilePath(qApp->applicationName());
            Settings warnSettings(warnWorkDir);
            const double warnCubeMb =
                    (static_cast<double>(this->remoteDatasetWidth)
                     * static_cast<double>(this->remoteDatasetHeight)
                     * static_cast<double>(this->remoteDatasetDepth)
                     * 4.0) / (1024.0 * 1024.0);
            const int warnThreshold = warnSettings.getLocalFullThresholdMb();
            if (warnCubeMb > static_cast<double>(warnThreshold)
                && !this->remoteBackendUrl.isEmpty()) {
                QMessageBox box(this);
                box.setIcon(QMessageBox::Warning);
                box.setWindowTitle(u"Large cube — remote rendering recommended"_s);
                box.setText(QStringLiteral(
                        "This cube is %1 GB at full resolution, well above the "
                        "%2 MB local-load threshold.\n\nDownloading the full "
                        "cube to this desktop will use the full size in RAM "
                        "and may take a long time.\n\nRemote rendering "
                        "(GPU on the backend) is already configured and will "
                        "give you full-quality, fully interactive 3D without "
                        "the transfer.")
                                       .arg(warnCubeMb / 1024.0, 0, 'f', 2)
                                       .arg(warnThreshold));
                auto *btnRemote = box.addButton(
                        u"Use Remote Rendering"_s, QMessageBox::AcceptRole);
                auto *btnLocal = box.addButton(
                        u"Download Locally Anyway"_s, QMessageBox::DestructiveRole);
                box.addButton(QMessageBox::Cancel);
                box.setDefaultButton(btnRemote);
                box.exec();
                if (box.clickedButton() == btnRemote) {
                    this->applyRenderMode(RenderMode::Remote, true);
                    return;
                }
                if (box.clickedButton() != btnLocal) {
                    return;  // Cancel or [X]
                }
                // Falls through to the local fetch path below.
            }
            this->userForcedFullResolution = true;
            this->statusBar()->showMessage(
                    u"Loading full-resolution cube from backend…"_s, 4000);
            if (!this->requestHighResCube()) {
                this->setRemoteCubeDisplayState(
                        this->usingHighResCube
                                ? RemoteCubeDisplayState::FullResolution
                                : RemoteCubeDisplayState::Preview);
                this->clearPersistentStatusMessage();
            }
        });

        auto *cropFullAction =
                ui->menuView->addAction(u"Crop Full Resolution…"_s);
        cropFullAction->setToolTip(
                u"Open the sub-cube extraction dialog to fetch only an X/Y/Z "
                u"region at full resolution. Recommended over Load Full "
                u"Resolution for cubes above the backend's size threshold."_s);
        QObject::connect(cropFullAction, &QAction::triggered, this,
                         [this]() { this->exportSubCubeAsFits(); });
    }

    // ── Slice animation (play/pause + FPS + mode) ────────────────────────
    ui->menuView->addSeparator();
    this->actionPlaySlices->setToolTip(
            u"Auto-advance the slice index, animating the cutting plane through "
            u"the cube along the spectral axis."_s);
    ui->menuView->addAction(this->actionPlaySlices);
    QObject::connect(this->actionPlaySlices, &QAction::toggled, this,
                     [this](bool checked) { this->setSliceAnimationActive(checked); });
    {
        auto *fpsMenu = ui->menuView->addMenu(u"Animation Speed"_s);
        // animationFpsGroup was pre-created above setupSidebar().
        const std::array<int, 5> fpsPresets { 2, 5, 10, 15, 30 };
        for (int fps : fpsPresets) {
            auto *action = fpsMenu->addAction(u"%1 fps"_s.arg(fps));
            action->setCheckable(true);
            action->setChecked(fps == this->animationFps);
            action->setData(fps);
            this->animationFpsGroup->addAction(action);
        }
        QObject::connect(this->animationFpsGroup, &QActionGroup::triggered, this,
                         [this](QAction *action) {
                             this->animationFps = std::max(1, action->data().toInt());
                             if (this->sliceAnimationTimer.isActive()) {
                                 this->sliceAnimationTimer.start(
                                         std::max(1, 1000 / this->animationFps));
                             }
                         });
    }
    {
        auto *modeMenu = ui->menuView->addMenu(u"Animation Mode"_s);
        // animationModeGroup was pre-created above setupSidebar().
        struct Entry { AnimationMode mode; QString label; };
        const std::array<Entry, 3> entries {
            Entry{ AnimationMode::Loop,   u"Loop"_s },
            Entry{ AnimationMode::Bounce, u"Bounce"_s },
            Entry{ AnimationMode::Once,   u"Stop at End"_s },
        };
        for (const auto &e : entries) {
            auto *action = modeMenu->addAction(e.label);
            action->setCheckable(true);
            action->setChecked(e.mode == this->animationMode);
            action->setData(static_cast<int>(e.mode));
            this->animationModeGroup->addAction(action);
        }
        QObject::connect(this->animationModeGroup, &QActionGroup::triggered, this,
                         [this](QAction *action) {
                             this->animationMode =
                                     static_cast<AnimationMode>(action->data().toInt());
                             this->animationDirection = +1;
                         });
    }
    this->sliceAnimationTimer.setSingleShot(false);
    QObject::connect(&this->sliceAnimationTimer, &QTimer::timeout, this,
                     &vtkWindowCube::advanceSliceAnimation);

    // ── 3D spectral picker toggle ────────────────────────────────────────
    this->actionPickSpectrum3d->setToolTip(
            u"When enabled, clicking on the cutting plane inside the 3D view "
            u"extracts the spectral profile at that voxel into the probe plot."_s);
    ui->menuView->addAction(this->actionPickSpectrum3d);
    QObject::connect(this->actionPickSpectrum3d, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            this->install3dPickObserver();
            // Make the picking mode obvious: cross cursor on the 3D widget and
            // a status hint. Without this the toggle does nothing visible until
            // the user happens to click on the plane.
            ui->vtkCube->setCursor(Qt::CrossCursor);
            this->statusBar()->showMessage(
                    u"3D pick: click the cutting plane to extract its spectral profile. "
                    u"Click off-plane to rotate as usual."_s,
                    5000);
        } else {
            this->remove3dPickObserver();
            ui->vtkCube->unsetCursor();
            this->statusBar()->clearMessage();
        }
    });

    this->updateDataStatePanel();
    this->updateSanityPanel();
    this->updateWcsStatusIndicator();

    // ── Diagnostics + Command palette (replicated across windows) ────────
    // These two entries make the app-wide "View → Diagnostics" and "⌘K"
    // affordances reachable from this child window too, so the user does
    // not have to switch back to the main window.
    ui->menuView->addSeparator();
    auto *actionDiagnostics = new QAction(u"Diagnostics"_s, this);
    actionDiagnostics->setToolTip(u"Open the application-wide diagnostics log window."_s);
    ui->menuView->addAction(actionDiagnostics);
    QObject::connect(actionDiagnostics, &QAction::triggered, this,
                     []() { DiagnosticsWindow::showSingleton(); });

    auto *actionCommandPalette = new QAction(u"Command Palette…"_s, this);
    actionCommandPalette->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    actionCommandPalette->setShortcutContext(Qt::ApplicationShortcut);
    actionCommandPalette->setToolTip(
            u"Quick action search — same palette as the main window (⌘K)."_s);
    ui->menuView->addAction(actionCommandPalette);
    QObject::connect(actionCommandPalette, &QAction::triggered, this, [this]() {
        // Find the MainWindow instance and ask it to open the palette. We
        // don't link against MainWindow here to avoid a circular include;
        // instead we walk top-level widgets and use a metacall.
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w->inherits("MainWindow")) {
                QMetaObject::invokeMethod(w, "openCommandPalette", Qt::QueuedConnection);
                return;
            }
        }
    });

    // ── Camera ROI action – View menu + Tools sidebar ─────────────────────
    this->actionCameraRoi = new QAction(u"Use Camera ROI"_s, this);
    this->actionCameraRoi->setCheckable(true);
    this->actionCameraRoi->setChecked(this->useCameraRoiRefinement);
    this->actionCameraRoi->setToolTip(
            u"When enabled, the high-resolution cube is loaded only for the current camera viewport.\n"
            u"Unchecked: full dataset is used for refinement."_s);
    ui->menuView->addSeparator();
    ui->menuView->addAction(this->actionCameraRoi);
    QObject::connect(this->actionCameraRoi, &QAction::toggled, this,
                     [this](bool checked) {
                         this->useCameraRoiRefinement = checked;
                         this->currentRemoteRefinementModeLabel =
                                 checked ? u"Viewport ROI"_s : u"Full"_s;
                         this->updateDataStatePanel();
                         qDebug().noquote()
                                 << QStringLiteral("[remote-roi] mode toggled to %1")
                                            .arg(checked ? u"Camera ROI"_s : u"Full"_s);
                         if (this->remoteHighResCubeWatcher.isRunning()) {
                             this->pendingRemoteRefinementReload = true;
                             qDebug().noquote()
                                     << QStringLiteral("[remote-roi] scheduling refinement reload");
                             return;
                         }
                         this->usingHighResCube = false;
                         qDebug().noquote()
                                 << QStringLiteral("[remote-roi] starting refinement reload");
                         this->requestHighResCube();
                     });
    this->statusMessageClearTimer.setSingleShot(true);
    QObject::connect(&this->statusMessageClearTimer, &QTimer::timeout, this, [this]() {
        this->persistentStatusActive = false;
        this->statusBar()->clearMessage();
    });
    this->remoteSliceDebounceTimer.setSingleShot(true);
    QObject::connect(&this->remoteSliceDebounceTimer, &QTimer::timeout, this, [this]() {
        this->requestRemoteSlice(this->pendingRemoteSliceIndex);
    });
    // Idle-triggered neighbour prefetch — fires `idlePrefetchDelayMs`
    // after the LAST applyRemoteSliceResult that was allowed to schedule
    // a prefetch (user-initiated). Re-armed on every user request so
    // a scroll burst suppresses it cleanly.
    this->idlePrefetchTimer.setSingleShot(true);
    QObject::connect(&this->idlePrefetchTimer, &QTimer::timeout, this, [this]() {
        this->prefetchNeighborRemoteSlices(this->currentRequestedRemoteSliceIndex);
    });
    this->remoteFullResolutionStateTimer.setSingleShot(true);
    QObject::connect(&this->remoteFullResolutionStateTimer, &QTimer::timeout, this, [this]() {
        if (this->remoteCubeDisplayState != RemoteCubeDisplayState::LoadingFullResolution
            || !this->remoteHighResCubeWatcher.isRunning()) {
            return;
        }

        this->setCubeOpenStateLabel(u"Loading full resolution..."_s);
    });
    this->isosurfaceDebounceTimer.setSingleShot(true);
    QObject::connect(&this->isosurfaceDebounceTimer, &QTimer::timeout, this, [this]() {
        qDebug().noquote()
                << QStringLiteral("[remote-iso] debounce fired checked=%1")
                           .arg(ui->actionIsosurface->isChecked());
        if (this->cubeOpenWatcher.isRunning() || !ui->actionIsosurface->isChecked()) {
            return;
        }

        qDebug().noquote()
                << QStringLiteral("[remote-iso] launching remote compute threshold=%1")
                           .arg(ui->lineThreshold->text());
        this->startAsyncIsosurface(ui->lineThreshold->text().toDouble());
    });

    // Backend-authoritative mode: initialize with a placeholder until the first
    // remote preview or full-resolution subvolume arrives from the backend.
    {
        vtkNew<vtkImageData> placeholder;
        placeholder->SetExtent(0, 0, 0, 0, 0, 0);
        placeholder->AllocateScalars(VTK_FLOAT, 1);
        static_cast<float *>(placeholder->GetScalarPointer())[0] = 0.f;
        this->cubeDisplaySource->SetOutput(placeholder);
        this->currentCubeVisibleRange = { 0., 0. };
        this->currentCubeInvisibleSentinel = -1.0;
        this->lowerBound = 0.f;
        this->upperBound = 1.f;
    }

    // Setup Renderers
    this->setupCubeRenderer();
    this->setupSliceRenderer();
    this->setupMomentRenderer();
    // §3.0: bind the moment host widget now that momentWin has its renderer +
    // (manual) QVTKInteractor with the probe/region/PV observers — same order as the
    // pre-§3.0 changeImageRenderer swap, which is known to preserve the interactor.
    if (m_vtkMoment)
        m_vtkMoment->setRenderWindow(this->momentWin);

    // Attach the cutting-plane texture now that setupSliceRenderer() has
    // wired sliceColors with a real input and LUT — connecting it earlier
    // (e.g. inside setupCubeRenderer) feeds the first 3D Render a broken
    // texture and the entire scene fails to draw.
    this->remoteCuttingPlaneTexture = vtkSmartPointer<vtkTexture>::New();
    this->remoteCuttingPlaneTexture->SetInputConnection(this->sliceColors->GetOutputPort());
    this->remoteCuttingPlaneTexture->InterpolateOn();
    this->remoteCuttingPlaneTexture->RepeatOff();
    this->remoteCuttingPlaneActor->SetTexture(this->remoteCuttingPlaneTexture);

    this->updateDataStatePanel();
    this->updateSanityPanel();
    QTimer::singleShot(0, this, [this]() {
        if (!this->isVisible()) {
            return;
        }

        QElapsedTimer timer;
        timer.start();
        ui->vtkCube->renderWindow()->Render();
        qDebug().noquote()
                << QStringLiteral("[perf][cube] warm-up render: %1 ms").arg(timer.elapsed());
    });
    // Backend-authoritative: no local applyCubeOpenResult here.
    // The first content arrives via requestRemotePreview() below.
    QObject::connect(&this->cubeOpenWatcher, &QFutureWatcher<CubeOpenStageResult>::finished, this,
                     [this]() {
                         this->setCubeOpenActionsEnabled(true);

                         const auto result = this->cubeOpenWatcher.result();
                         if (!result.valid || !result.cubeImageData) {
                             this->setCubeOpenStateLabel(u"Preview"_s);
                             if (!result.errorMessage.isEmpty()) {
                                 this->persistentStatusActive = false;
                                 this->statusMessageClearTimer.stop();
                                 this->statusBar()->showMessage(result.errorMessage);
                             } else {
                                 this->clearPersistentStatusMessage();
                             }
                             return;
                         }

                         this->setCubeOpenStateLabel(u"Loading full resolution..."_s);
                         this->showPersistentStatusMessage(u"Loading full resolution..."_s);
                         QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                         QElapsedTimer applyTimer;
                         applyTimer.start();
                         this->applyCubeOpenResult(result);
                         qDebug().noquote()
                                 << QStringLiteral("[perf][cube] apply full result: %1 ms")
                                            .arg(applyTimer.elapsed());
                         ++this->currentFullCubeGeneration;
                         this->setCubeOpenStateLabel({ });
                         this->clearPersistentStatusMessage();
                     });
    QObject::connect(&this->remotePreviewWatcher, &QFutureWatcher<RemoteCubePreviewResult>::finished, this,
                     [this]() {
                         qDebug().noquote() << QStringLiteral("[preview-debug] watcher::finished fired");
                         const int requestId =
                                 this->remotePreviewWatcher.property("requestId").toInt();
                         if (requestId != this->currentRemotePreviewRequestId) {
                             qDebug().noquote() << QStringLiteral("[preview-debug] stale requestId=%1 vs current=%2 — return")
                                                           .arg(requestId).arg(this->currentRemotePreviewRequestId);
                             return;
                         }

                         this->setCubeOpenActionsEnabled(true);

                         const auto result = this->remotePreviewWatcher.result();
                         if (!result.valid || !result.cubeImageData) {
                             this->m_cube3dLoadingActor->VisibilityOff();
                             this->m_slice2dLoadingActor->VisibilityOff();
                             ui->vtkCube->renderWindow()->Render();
                             this->sliceWin->Render();
                             this->persistentStatusActive = false;
                             this->statusMessageClearTimer.stop();
                             this->statusBar()->showMessage(result.errorMessage.isEmpty()
                                                                    ? u"Could not load preview."_s
                                                                    : result.errorMessage);
                             return;
                         }

                         // The cube's min/max is dominated by background blanks and a
                         // few bright peaks, so taking literal min as the volume
                         // floor and lowerBound = min + 10% leaves most of the
                         // interesting emission below the visibility threshold.
                         // 3% gives a tighter noise-floor cut that still hides
                         // the constant-zero background but keeps faint emission
                         // (the bulk of the science signal) visible.
                         const double span = result.cubeRange[1] - result.cubeRange[0];
                         this->lowerBound = static_cast<float>(result.cubeRange[0] + 0.03 * span);
                         this->upperBound = static_cast<float>(result.cubeRange[1]);

                         // applyCubeOpenResult() sets currentCubeVisibleRange from the real data.
                         // The initial threshold must be applied AFTER this, so that
                         // applyThresholdToPipeline() sees correct range control points.
                         this->applyCubeOpenResult({ true,
                                                     { },
                                                     result.cubeImageData,
                                                     nullptr,
                                                     result.cubeRange,
                                                     { 0., 0. },
                                                     result.dataExtent,
                                                     0.,
                                                     0. });

                         // ── Data-driven initial threshold (first load only) ──
                         // Picks the isosurface level on first load. Used to be
                         // 65% of (upper-lower) — too aggressive for HI / molecular
                         // cubes whose interesting emission tail sits in the lower
                         // half of the visible range, so users saw only a couple of
                         // bright peaks and assumed the cube was empty. 20% catches
                         // the bulk of the emission contour while still hiding the
                         // background noise floor, and the user can always raise it
                         // via the 3D View Settings slider.
                         if (!this->m_initialThresholdApplied) {
                             const float initial = this->lowerBound
                                     + 0.20f * (this->upperBound - this->lowerBound);
                             this->applyThreshold(initial);   // updates UI + pipeline, no Render
                             this->m_initialThresholdApplied = true;
                         }

                         // ── Synchronise the volume LUT to the pipeline ────────
                         // setupCubeRenderer() built the color transfer function with
                         // cubeRange = {0,0} (placeholder), so Inferno ends up mapped
                         // over a degenerate range and VTK clamps all real data to the
                         // endpoint color (yellow). Re-apply the selected LUT now that
                         // the cube data and its scalar range are known.
                         this->setVolumeLut(this->m_activeColorMapName);   // no Render

                         // Hide the 3D loading placeholder and do a single final render
                         // that reflects real data, correct threshold, and correct LUT.
                         this->m_cube3dLoadingActor->VisibilityOff();
                         ui->vtkCube->renderWindow()->Render();

                         this->setRemoteCubeDisplayState(RemoteCubeDisplayState::Preview);
                         qDebug().noquote() << QStringLiteral("[remote-plane] preview valid");
                         // Pattern B: skip the automatic full-resolution
                         // local fetch when EITHER:
                         //   (a) backend recommended ``preview_only`` (it
                         //       knows the cube is too big to fit locally), OR
                         //   (b) the cube's full-res MB exceeds the user's
                         //       LocalFullThresholdMb (default 256 MB).
                         // Without (b) a 1 GB cube would auto-download all
                         // 1 GB locally even though the auto-render-mode
                         // logic was already moving us to remote streaming
                         // — wasteful, confusing UX (status bar shows
                         // "loading full resolution" while the screen is
                         // already showing the remote stream). The user
                         // can still force a local full-res fetch via View
                         // → Load Full Resolution, which sets
                         // userForcedFullResolution=true and bypasses both
                         // guards.
                         const QString fetchWorkDir =
                                 QDir::home().absoluteFilePath(qApp->applicationName());
                         Settings fetchSettings(fetchWorkDir);
                         const double fetchCubeMb =
                                 (static_cast<double>(this->remoteDatasetWidth)
                                  * static_cast<double>(this->remoteDatasetHeight)
                                  * static_cast<double>(this->remoteDatasetDepth)
                                  * 4.0) / (1024.0 * 1024.0);
                         const bool tooLargeForLocal =
                                 fetchCubeMb > static_cast<double>(
                                         fetchSettings.getLocalFullThresholdMb());
                         const bool shouldAutoLoad =
                                 this->userForcedFullResolution
                                 || (this->recommendedLoadMode
                                            != QLatin1String("preview_only")
                                     && !tooLargeForLocal);
                         qDebug().noquote() << QStringLiteral(
                                 "[remote-plane] auto-full-fetch decision "
                                 "cube=%1MB threshold=%2MB tooLarge=%3 forced=%4 "
                                 "backend_hint=%5 -> shouldAutoLoad=%6")
                                 .arg(fetchCubeMb, 0, 'f', 1)
                                 .arg(fetchSettings.getLocalFullThresholdMb())
                                 .arg(tooLargeForLocal)
                                 .arg(this->userForcedFullResolution)
                                 .arg(this->recommendedLoadMode)
                                 .arg(shouldAutoLoad);
                         if (!shouldAutoLoad) {
                             this->clearPersistentStatusMessage();
                             const double sizeGiB =
                                     double(this->datasetFileSizeBytes)
                                     / double(qint64(1) << 30);
                             const double thresholdGiB =
                                     double(this->datasetFullResThresholdBytes)
                                     / double(qint64(1) << 30);
                             this->statusBar()->showMessage(
                                     u"Preview-only mode: cube is %1 GB (threshold %2 GB). "
                                     u"Use View → Load Full Resolution or Crop Region for full-res data."_s
                                             .arg(sizeGiB, 0, 'f', 2)
                                             .arg(thresholdGiB, 0, 'f', 1),
                                     10000);
                             return;
                         }
                         if (!this->requestHighResCube()) {
                             this->setRemoteCubeDisplayState(this->usingHighResCube
                                                                     ? RemoteCubeDisplayState::FullResolution
                                                                     : RemoteCubeDisplayState::Preview);
                             this->clearPersistentStatusMessage();
                         }
                     });
    QObject::connect(&this->remoteHighResCubeWatcher,
                     &QFutureWatcher<RemoteCubeSubvolumeResult>::finished, this, [this]() {
                         const int requestId =
                                 this->remoteHighResCubeWatcher.property("requestId").toInt();
                         if (requestId != this->currentRemoteHighResRequestId) {
                             return;
                         }

                         const auto result = this->remoteHighResCubeWatcher.result();
                         if (!result.valid || !result.cubeImageData) {
                             this->persistentStatusActive = false;
                             this->statusMessageClearTimer.stop();
                             this->setRemoteCubeDisplayState(RemoteCubeDisplayState::Preview);
                             this->statusBar()->showMessage(result.errorMessage.isEmpty()
                                                                    ? u"Could not load full-resolution cube."_s
                                                                    : result.errorMessage);
                             if (this->pendingRemoteRefinementReload) {
                                 this->pendingRemoteRefinementReload = false;
                                 this->usingHighResCube = false;
                                 qDebug().noquote()
                                         << QStringLiteral("[remote-roi] starting refinement reload");
                                 this->requestHighResCube();
                             }
                             return;
                         }

                         this->usingHighResCube = true;
                         this->applyCubeOpenResult({ true,
                                                     { },
                                                     result.cubeImageData,
                                                     nullptr,
                                                     result.cubeRange,
                                                     { 0., 0. },
                                                     result.dataExtent,
                                                     result.cubeMean,
                                                     result.cubeRms,
                                                     /*preSanitized=*/true,
                                                     result.invisibleSentinel,
                                                     result.blankFraction });
                         if (auto *cubeImage = vtkImageData::SafeDownCast(
                                     this->cubeDisplaySource->GetOutputDataObject(0))) {
                             double bounds[6];
                             cubeImage->GetBounds(bounds);
                             qDebug().noquote()
                                     << QStringLiteral("[remote-roi] applied roi=%1..%2,%3..%4,%5..%6 bounds=%7,%8,%9,%10,%11,%12")
                                                .arg(this->currentRemoteRoi[0])
                                                .arg(this->currentRemoteRoi[1])
                                                .arg(this->currentRemoteRoi[2])
                                                .arg(this->currentRemoteRoi[3])
                                                .arg(this->currentRemoteRoi[4])
                                                .arg(this->currentRemoteRoi[5])
                                                .arg(bounds[0], 0, 'g', 12)
                                                .arg(bounds[1], 0, 'g', 12)
                                                .arg(bounds[2], 0, 'g', 12)
                                                .arg(bounds[3], 0, 'g', 12)
                                                .arg(bounds[4], 0, 'g', 12)
                                                .arg(bounds[5], 0, 'g', 12);
                         }
                         this->setRemoteCubeDisplayState(RemoteCubeDisplayState::FullResolution);
                         this->clearPersistentStatusMessage();
                         if (this->pendingRemoteRefinementReload) {
                             this->pendingRemoteRefinementReload = false;
                             this->usingHighResCube = false;
                             qDebug().noquote()
                                     << QStringLiteral("[remote-roi] starting refinement reload");
                             this->requestHighResCube();
                         }
                     });
    // ── Moment computation – delegated to MomentMapController ────────────────
    this->momentController = new MomentMapController(
            this->filepath,
            this->remoteBackendUrl,
            this->remoteDatasetId,
            this->remoteSessionId,
            this->remoteBackendToken,
            this->remoteDatasetDepth,
            this->lowerBound,
            this,
            [this]() {
                this->updateBackendDerivedActionsAvailability();
                // When the controller transitions from busy→idle we don't yet know
                // whether onCompleted will be called (it fires right after this in
                // the same watcher lambda).  Schedule a deferred check for the next
                // event-loop tick: if we're still in Computing state by then,
                // onCompleted was not called → the computation failed.
                if (!this->momentController->isBusy()
                    && this->m_momentCardState == MomentCardState::Computing) {
                    QMetaObject::invokeMethod(
                            this,
                            [this]() {
                                if (this->m_momentCardState == MomentCardState::Computing) {
                                    this->setMomentCardState(MomentCardState::Error);
                                    const QString errMsg =
                                            this->momentController && !this->momentController->lastError().isEmpty()
                                            ? this->momentController->lastError()
                                            : QStringLiteral("Moment computation failed.");
                                    DiagnosticsManager::instance()->publish(
                                            DiagnosticsLevel::Warning,
                                            DiagnosticsCategory::Scientific,
                                            QStringLiteral("MomentMap"),
                                            errMsg,
                                            this->remoteDatasetId,
                                            this->remoteSessionId,
                                            QStringLiteral("moment"));
                                }
                            },
                            Qt::QueuedConnection);
                }
            },
            [this](const MomentMapComputeResult &result,
                   const MomentMapController::Config &config,
                   const QString &summary,
                   const QString &details) {
                // Build the Provenance recipe BEFORE applyMomentMapResult — it ends
                // by registering the product, and registerAnalysisProduct reads these
                // params (§3.0b (c)). Same fields as the status summary.
                this->m_momentProvenanceParams.clear();
                this->m_momentProvenanceParams.insert(
                        u"Order"_s, MomentMapController::describeMomentOrder(config.order));
                this->m_momentProvenanceParams.insert(
                        u"Channels"_s,
                        u"%1–%2"_s.arg(config.channelStart + 1).arg(config.channelEnd + 1));
                this->m_momentProvenanceParams.insert(
                        u"Mask"_s,
                        config.maskEnabled
                                ? (config.thresholdAuto
                                           ? u"auto (≈ 3σ)"_s
                                           : u">= %1"_s.arg(config.thresholdValue, 0, 'g', 8))
                                : u"none"_s);
                this->m_momentProvenanceParams.insert(u"Scope"_s,
                                                      MomentMapController::describeMomentScope());
                this->m_momentProvenanceParams.insert(
                        u"Unit"_s, result.momentUnit.isEmpty() ? u"—"_s : result.momentUnit);

                // Mark the moment available BEFORE applying — mountView() reads
                // momentProvenanceState.valid to decide render vs empty state (§3.1).
                this->momentProvenanceState.valid = true;
                QElapsedTimer applyTimer;
                applyTimer.start();
                this->applyMomentMapResult(
                        { result.imageData,
                          { result.imageRange[0], result.imageRange[1] },
                          config.order, config.channelStart, config.channelEnd,
                          config.maskEnabled, config.thresholdValue, config.thresholdAuto });
                qDebug().noquote()
                        << QStringLiteral("[perf][moment] UI apply: %1 ms")
                                   .arg(applyTimer.elapsed());

                this->momentProvenanceState.summary = summary;
                // Append WCS-specific fields that require window state.
                this->momentProvenanceState.details =
                        details
                        + u"\nAxis 3 semantics: %1\nWCS frame: %2"_s
                                  .arg(this->spectralAxisTitle())
                                  .arg(this->currentWcsFrameLabel());
                this->updateMomentProvenancePanel();

                // Transition card to Ready and update info label with provenance.
                this->setMomentCardState(MomentCardState::Ready);
                // A moment is now visible — make "Export Moment Map as FITS…"
                // selectable from the Tools menu. The action stays enabled
                // for the rest of the window's life (the user can re-export
                // the same moment, or recompute and re-export).
                if (this->m_actExportMomentFits) {
                    this->m_actExportMomentFits->setEnabled(true);
                }

                // Diagnostics — success.
                DiagnosticsManager::instance()->publish(
                        DiagnosticsLevel::Info,
                        DiagnosticsCategory::Scientific,
                        QStringLiteral("MomentMap"),
                        QStringLiteral("Moment computation completed: ") + summary,
                        this->remoteDatasetId,
                        this->remoteSessionId,
                        QStringLiteral("moment"));

                this->clearPersistentStatusMessage();

                // Show a transient WCS warning for sanitized/degraded WCS.
                if (!result.wcsStatus.isEmpty() && result.wcsStatus != u"ok"_s) {
                    const QString warnText = result.wcsWarningMessage.isEmpty()
                            ? u"Moment map WCS: %1"_s.arg(result.wcsStatus)
                            : u"Moment map WCS (%1): %2"_s.arg(result.wcsStatus,
                                                                result.wcsWarningMessage);
                    this->statusBar()->showMessage(warnText, 8000);
                }
            },
            this);
    // ── Noise estimation – delegated to NoiseController ───────────────────────
    this->noiseController = new NoiseController(
            this->remoteBackendUrl, this->remoteDatasetId,
            this->remoteSessionId, this->remoteBackendToken,
            this->remoteDatasetWidth, this->remoteDatasetHeight, this->remoteDatasetDepth,
            this,
            [this]() { this->updateBackendDerivedActionsAvailability(); },
            this);

    // ── PV diagram backend fetch – delegated to PvController ─────────────────
    this->pvController = new PvController(
            this->remoteBackendUrl, this->remoteDatasetId,
            this->remoteSessionId, this->remoteBackendToken,
            this,
            [this]() { this->updateBackendDerivedActionsAvailability(); },
            [this](const RemotePvFetchResult &result) {
                // Build spectral axis for the Y axis of the diagram.
                QVector<double> spectral(result.depth);
                for (int z = 0; z < result.depth; ++z) {
                    bool ok = false;
                    spectral[z] = this->spectralAxisValue(static_cast<double>(z), &ok);
                    if (!ok) {
                        spectral[z] = static_cast<double>(z);
                    }
                }

                // Y-axis label: prefer backend-reported type, fall back to local descriptor.
                const QString yLabel = !result.spectralAxisType.isEmpty()
                        ? (!result.spectralAxisUnit.isEmpty()
                                   ? u"%1 (%2)"_s.arg(result.spectralAxisType,
                                                       result.spectralAxisUnit)
                                   : result.spectralAxisType)
                        : this->spectralAxisTitle();

                // Build the path-info struct shown in the PV diagram header.
                const auto &verts = result.capturedVertices;
                const int usedWidth = result.widthPixels > 0 ? result.widthPixels
                                                              : result.capturedWidthPixels;
                PvDiagramWidget::PvPathInfo pvInfo;
                pvInfo.startPoint    = verts.size() >= 1
                        ? this->formatSpatialPointSummary(verts.front()) : QString{};
                pvInfo.endPoint      = verts.size() >= 2
                        ? this->formatSpatialPointSummary(verts.back())  : QString{};
                pvInfo.vertexCount   = result.vertexCount > 0
                        ? result.vertexCount : static_cast<int>(verts.size());
                pvInfo.widthPixels   = usedWidth;
                pvInfo.lengthPixels  = result.totalLength;
                pvInfo.spectralAxisLabel = yLabel;
                pvInfo.spatialUnit   = result.spatialUnit.isEmpty()
                        ? u"pixels"_s : result.spatialUnit;
                pvInfo.pixelScaleArcsecPerPixel = result.pixelScaleArcsecPerPixel;

                // Create PvDiagramWidget lazily on first successful extraction.
                if (!this->pvDiagramWidget) {
                    qDebug().noquote() << QStringLiteral("[pv] creating PvDiagramWidget");
                    this->pvDiagramWidget = new PvDiagramWidget(this);
                    this->pvDiagramWidget->applyPlotTheme(this->m_darkPlots);
                    QObject::connect(
                            this->pvDiagramWidget, &PvDiagramWidget::destroyed, this,
                            [this]() {
                                this->pvDiagramWidget = nullptr;
                                // Window closed → remove the path overlay from
                                // both the slice and moment 2D views.
                                this->clearPv();
                                if (this->active2dWin())
                                    this->active2dWin()->Render();
                                if (this->actionExtractPvDiagram
                                    && this->actionExtractPvDiagram->isChecked()) {
                                    this->actionExtractPvDiagram->setChecked(false);
                                }
                            });
                }

                // Now that the widget exists, it's safe to flip PV mode off.
                // Doing this earlier (right after the user finished drawing)
                // would race with the async extraction: setPvModeActive(false)
                // calls clearPv() unless pvDiagramWidget is non-null, so the
                // polyline would vanish before the plot opened.
                if (this->actionExtractPvDiagram
                    && this->actionExtractPvDiagram->isChecked()) {
                    this->actionExtractPvDiagram->setChecked(false);
                }
                // refreshPvOverlay() keeps the polyline visible because
                // pvDiagramWidget is now non-null (see the guard in
                // refreshPvOverlay()), so the overlay survives the
                // PV-mode-off transition.
                this->refreshPvOverlay();
                if (this->active2dWin())
                    this->active2dWin()->Render();

                // Choose spatial axis (prefer physical arcsec when available).
                const bool hasArcsec = !result.positionsArcsec.isEmpty()
                        && result.positionsArcsec.size() == result.positions.size();
                const QVector<double> &pvPositions =
                        hasArcsec ? result.positionsArcsec : result.positions;
                const QString xLabel = hasArcsec
                        ? u"Offset Along Path (%1)"_s.arg(
                                  result.spatialUnit.isEmpty() ? u"arcsec"_s
                                                               : result.spatialUnit)
                        : u"Offset Along Path (pixels)"_s;

                qDebug().noquote()
                        << QStringLiteral("[pv] updating widget and requesting show/raise");
                this->pvDiagramWidget->setPvData(pvPositions, spectral, result.values,
                                                 result.numSamples, result.depth,
                                                 xLabel, yLabel,
                                                 u"PV Diagram (Polyline)"_s, pvInfo);
                this->clearPersistentStatusMessage();
                // The PV widget is created once and reused → stable id updates
                // the same product row; the widget is what we raise on activate.
                this->registerAnalysisProduct(
                        u"pv"_s, u"PV diagram"_s, {}, this->pvDiagramWidget,
                        QStringLiteral("pv@%1").arg(this->datasetPersistKey()));
            },
            this);

    QObject::connect(&this->isosurfaceWatcher, &QFutureWatcher<AsyncIsosurfaceResult>::finished,
                     this, [this]() {
                         const auto result = this->isosurfaceWatcher.result();
                         if (result.requestId != this->currentIsosurfaceRequestId) {
                             return;
                         }

                         if (!result.mesh || result.mesh->GetNumberOfPoints() == 0) {
                             if (!result.errorMessage.isEmpty()) {
                                 this->persistentStatusActive = false;
                                 this->statusMessageClearTimer.stop();
                                 this->statusBar()->showMessage(result.errorMessage);
                             }
                             this->remoteIsosurfaceReady = false;
                             this->setCubeRenderModeLocally(false);
                             ui->actionVolume->setChecked(true);
                             ui->vtkCube->renderWindow()->Render();
                             return;
                         }
                         this->applyIsosurfaceResult(result);
                         this->clearPersistentStatusMessage();
                     });

    // Setup menu Camera
    ui->actionCameraFront->setIcon(QIcon(u":/icons/PIC_FRONT.png"_s));
    ui->actionCameraBack->setIcon(QIcon(u":/icons/PIC_BACK.png"_s));
    ui->actionCameraTop->setIcon(QIcon(u":/icons/PIC_TOP.png"_s));
    ui->actionCameraRight->setIcon(QIcon(u":/icons/PIC_RIGHT.png"_s));
    ui->actionCameraBottom->setIcon(QIcon(u":/icons/PIC_BOTTOM.png"_s));
    ui->actionCameraLeft->setIcon(QIcon(u":/icons/PIC_LEFT.png"_s));
    QObject::connect(ui->actionCameraFront, &QAction::triggered, this,
                     &vtkWindowCube::resetCameraFront);
    QObject::connect(ui->actionCameraBack, &QAction::triggered, this,
                     &vtkWindowCube::resetCameraBack);
    QObject::connect(ui->actionCameraTop, &QAction::triggered, this,
                     &vtkWindowCube::resetCameraTop);
    QObject::connect(ui->actionCameraRight, &QAction::triggered, this,
                     &vtkWindowCube::resetCameraRight);
    QObject::connect(ui->actionCameraBottom, &QAction::triggered, this,
                     &vtkWindowCube::resetCameraBottom);
    QObject::connect(ui->actionCameraLeft, &QAction::triggered, this,
                     &vtkWindowCube::resetCameraLeft);

    // Setup menu Edit
    QObject::connect(ui->actionEditLUT, &QAction::triggered, this,
                     &vtkWindowCube::showLUTCustomizer);

    // Setup menu View
    auto imageGroup = new QActionGroup(this);
    imageGroup->addAction(ui->actionSlice);
    imageGroup->addAction(ui->actionMomentMap);
    QObject::connect(imageGroup, &QActionGroup::triggered, this, [this](QAction *action) {
        if (action == ui->actionMomentMap) {
            // Route through the availability guard — may show the computation dialog,
            // revert to Slice on cancel/failure, or show an existing moment map.
            this->requestMomentMode();
        } else {
            this->changeImageRenderer(); // Slice — also syncs sidebar
        }
        this->updateLUTCustomizer();
    });
    auto cubeGroup = new QActionGroup(this);
    cubeGroup->addAction(ui->actionIsosurface);
    cubeGroup->addAction(ui->actionVolume);
    QObject::connect(cubeGroup, &QActionGroup::triggered, this, &vtkWindowCube::changeCubeRender);

    // Compute Moment – single action always opens the dialog (FIX 1+2).
    QObject::connect(ui->actionComputeMoment, &QAction::triggered, this,
                     [this]() { this->setMomentOrder(0); });

    // S-02 Line-width map
    QObject::connect(ui->actionLinewidthMap, &QAction::triggered, this, [this]() {
        if (this->isBusy() || !this->hasBackendVisibleDataset()) {
            return;
        }
        auto *dlg = new LinewidthDialog(this->remoteBackendUrl,
                                        this->remoteBackendToken,
                                        this->remoteSessionId,
                                        this->remoteDatasetId,
                                        this->remoteDatasetDepth,
                                        static_cast<double>(this->lowerBound),
                                        this->remoteSpectralAxisType,
                                        this->remoteSpectralAxisUnit,
                                        this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        // NOTE: LinewidthDialog spawns its actual result window (ScienceMapWindow)
        // only after the user runs it, so registering the product belongs with
        // that result window — deferred to tappa 4 (dialog→product migration).
    });

    // S-03 Baseline subtraction
    QObject::connect(ui->actionBaselineSubtract, &QAction::triggered, this, [this]() {
        if (this->isBusy() || !this->hasBackendVisibleDataset()) {
            return;
        }
        auto *dlg = new BaselineDialog(this->remoteBackendUrl,
                                       this->remoteBackendToken,
                                       this->remoteSessionId,
                                       this->remoteDatasetId,
                                       this->remoteDatasetDepth,
                                       this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        this->registerAnalysisProduct(u"baseline"_s, u"Baseline-subtracted cube"_s, {}, dlg);
    });

    // S-04 Stack spectral cubes
    QObject::connect(ui->actionStackCubes, &QAction::triggered, this, [this]() {
        if (!this->hasBackendVisibleDataset()) {
            return;
        }
        // Enumerate ALL cube datasets open in this backend session, not just
        // this window's own dataset, so the user can actually pick ≥2 cubes
        // to stack. Done in a worker because BackendClient is synchronous.
        const QString url       = this->remoteBackendUrl;
        const QString token     = this->remoteBackendToken;
        const QString sessionId = this->remoteSessionId;
        const QString currentId = this->remoteDatasetId;
        auto *watcher = new QFutureWatcher<BackendSessionDatasetsResult>(this);
        QPointer<vtkWindowCube> self(this);
        QObject::connect(watcher, &QFutureWatcher<BackendSessionDatasetsResult>::finished,
                         this, [self, watcher, url, token, sessionId, currentId]() {
                             const auto listing = watcher->result();
                             watcher->deleteLater();
                             if (!self) return;
                             if (!listing.valid) {
                                 QMessageBox::warning(self,
                                                      u"Stack spectral cubes"_s,
                                                      u"Could not list session datasets: %1"_s
                                                              .arg(listing.error));
                                 return;
                             }
                             std::vector<StackDialog::DatasetEntry> entries;
                             entries.reserve(listing.datasets.size());
                             for (const auto &d : listing.datasets) {
                                 // Use the file basename as the human label so the user can
                                 // tell the cubes apart; fall back to the dataset id when
                                 // the backend did not report a path.
                                 const QString fileName = QFileInfo(d.path).fileName();
                                 const QString label = fileName.isEmpty() ? d.datasetId : fileName;
                                 entries.push_back({ d.datasetId, d.kind, label,
                                                     d.width, d.height, d.depth });
                             }
                             auto *client = new BackendClient(url, token);
                             client->setSessionId(sessionId);
                             auto *dlg = new StackDialog(client, sessionId, entries,
                                                         currentId, self);
                             QObject::connect(dlg, &QDialog::finished,
                                              [client](int) { delete client; });
                             dlg->setAttribute(Qt::WA_DeleteOnClose);
                             // Non-modal to stay consistent with the other tool
                             // dialogs (Linewidth, Baseline, LUT customizer); the
                             // long-running compute already shows its own modal
                             // QProgressDialog so the wait is still indicated.
                             dlg->show();
                             // NOTE: StackDialog creates its result plot window on
                             // accept and closes itself, so product registration
                             // belongs with that result window — deferred to tappa 4.
                         });
        watcher->setFuture(QtConcurrent::run([url, token, sessionId]() {
            BackendClient c(url, token);
            c.setSessionId(sessionId);
            return c.listSessionDatasets(sessionId);
        }));
    });

    this->updateBackendDerivedActionsAvailability();

    // Setup menu WCS
    auto groupWCS = new QActionGroup(this);
    groupWCS->addAction(ui->actionGalactic);
    groupWCS->addAction(ui->actionFK5);
    groupWCS->addAction(ui->actionEcliptic);
    ui->menuWCS->setEnabled(this->remoteHasCelestialAxes());
    QObject::connect(groupWCS, &QActionGroup::triggered, this, &vtkWindowCube::changeLegendWCS);
    ui->menuWCS->addSeparator();
    auto *formatGroup = new QActionGroup(this);
    this->actionWcsSexagesimal = ui->menuWCS->addAction(u"Sexagesimal"_s);
    this->actionWcsDecimal = ui->menuWCS->addAction(u"Decimal"_s);
    this->actionWcsSexagesimal->setCheckable(true);
    this->actionWcsDecimal->setCheckable(true);
    formatGroup->addAction(this->actionWcsSexagesimal);
    formatGroup->addAction(this->actionWcsDecimal);
    QObject::connect(formatGroup, &QActionGroup::triggered, this, [this](QAction *action) {
        this->wcsFormatExplicitlyChosen = true;
        this->useSexagesimalWcsFormat = action == this->actionWcsSexagesimal;
        this->invalidateWcsOverlayCache();
        this->requestWcsOverlayRender();
    });
    this->applyDefaultWcsFormatForSelectedFrame();

    // Setup menu Tools
    //
    // Final structure (consistent with the sidebar Tools page):
    //   EXPLORATION : Extract Spectrum, Open in VR
    //   ───────────
    //   ANALYSIS    : Estimate Noise, Compute Moment, Line-width Map,
    //                 Baseline Subtraction, Stack Spectral Cubes,
    //                 Extract PV Diagram
    //   ───────────
    //   REGIONS     : Box / Circle / Polygon / Annulus
    //   ───────────
    //   CATALOGUE   : Load / Show / Show Labels / Clear
    //
    // The .ui file pre-populates the EXPLORATION head and the four ANALYSIS
    // items from the .ui (Compute Moment, Line-width, Baseline, Stack); the
    // other actions are created in code and slotted into the right group
    // via QMenu::insertAction(beforeAction, ...).

    ui->actionExtractSpectrum->setCheckable(true);
    QObject::connect(ui->actionExtractSpectrum, &QAction::toggled, this,
                     &vtkWindowCube::setProbeModeActive);

    // EXPLORATION: Open in VR, inserted right after Extract Spectrum (i.e.
    // before the first separator the .ui placed between EXPLORATION and
    // ANALYSIS). We *don't* runtime-probe up front: most OpenXR loaders cap
    // the process at one xrInstance and the probe leaks enough state that
    // the real open() then fails with LIMIT_REACHED. Instead, click → real
    // attempt → friendly error dialog on failure.
    QAction *actOpenInVR = nullptr;
    {
        actOpenInVR = new QAction(u"Open in VR"_s, ui->menuTools);
        const bool compiled = CubeVRController::isCompiledIn();
        actOpenInVR->setEnabled(compiled);
        // Checkable so the linked QToolButton (sidebar) visually latches when
        // a VR session is active. Toggling acts as open/close.
        actOpenInVR->setCheckable(true);
        if (!compiled) {
            actOpenInVR->setToolTip(u"This build was compiled without VR support. "
                                    u"Rebuild with -DVISIVO_ENABLE_VR=ON (requires VTK "
                                    u"with the RenderingOpenXR module)."_s);
        } else {
            actOpenInVR->setToolTip(u"Open the current cube in your VR headset. "
                                    u"Requires a running OpenXR runtime (SteamVR / "
                                    u"Oculus / WMR) and a connected HMD. The desktop "
                                    u"volume, LUT and threshold stay synchronised. "
                                    u"Click again to close the VR session."_s);
        }
        QObject::connect(actOpenInVR, &QAction::triggered, this, [this, actOpenInVR]() {
            // Toggle. If we already have a live VR session, the user clicking
            // the (now-checked) button means "stop". Otherwise it's "start".
            if (m_vrController && m_vrController->isActive()) {
                m_vrController->close();
                // sessionEnded() will fire and the lambda below resets the
                // action's checked state — nothing to do here.
                return;
            }
            auto *cubeRen = ui->vtkCube->renderWindow()
                    ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()
                    : nullptr;
            if (!cubeRen || !this->volume) {
                actOpenInVR->setChecked(false);
                QMessageBox::warning(this, u"Open in VR"_s,
                                     u"The cube is not ready yet — wait for the "
                                     u"preview to load and try again."_s);
                return;
            }
            if (!m_vrController) {
                m_vrController = new CubeVRController(this);
                // Reflect session-end (whether from desktop close or from
                // inside the headset) back onto the action's checked state,
                // so the sidebar button un-latches in both cases.
                QObject::connect(m_vrController.data(), &CubeVRController::sessionEnded,
                                 actOpenInVR, [actOpenInVR]() {
                                     actOpenInVR->setChecked(false);
                                 });
            }
            if (!m_vrController->open(cubeRen, this->volume)) {
                actOpenInVR->setChecked(false);
                QMessageBox::warning(this, u"Open in VR"_s, m_vrController->lastError());
            } else {
                actOpenInVR->setChecked(true);
            }
        });
        // Insert into EXPLORATION group — i.e. right after Extract Spectrum,
        // before the first separator that the .ui placed after it.
        const auto toolsActions = ui->menuTools->actions();
        QAction *firstSep = nullptr;
        for (QAction *a : toolsActions) {
            if (a->isSeparator()) { firstSep = a; break; }
        }
        if (firstSep) {
            ui->menuTools->insertAction(firstSep, actOpenInVR);
        } else {
            ui->menuTools->addAction(actOpenInVR);
        }
    }

    // ANALYSIS: Estimate Noise goes at the head of the group (before Compute
    // Moment), to match the sidebar's "Estimate Noise → Moment Map" order.
    this->actionEstimateNoise = new QAction(u"Estimate Noise..."_s, ui->menuTools);
    QObject::connect(this->actionEstimateNoise, &QAction::triggered,
                     this->noiseController, &NoiseController::startEstimation);
    ui->menuTools->insertAction(ui->actionComputeMoment, this->actionEstimateNoise);

    // ANALYSIS: Extract PV Diagram goes at the tail of the group (after Stack
    // Spectral Cubes, which is currently the last action defined in .ui).
    this->actionExtractPvDiagram = ui->menuTools->addAction(u"Extract PV Diagram"_s);
    this->actionExtractPvDiagram->setCheckable(true);
    QObject::connect(this->actionExtractPvDiagram, &QAction::toggled, this,
                     &vtkWindowCube::setPvModeActive);

    // ANALYSIS: Channel Maps mosaic (N × M grid of consecutive slice panels).
    this->m_actChannelMaps = ui->menuTools->addAction(u"Channel Maps…"_s);
    QAction *actChannelMaps = this->m_actChannelMaps;
    actChannelMaps->setToolTip(
            u"Display a mosaic of N consecutive channel slices with the same "
            u"color scale — useful for identifying coherent structures across "
            u"the spectral axis."_s);
    QObject::connect(actChannelMaps, &QAction::triggered, this, [this]() {
        ChannelMapsDialog dlg(this->remoteDatasetDepth,
                              ui->spinSlice->value() - 1,
                              this->m_activeSliceLutName, this);
        if (dlg.exec() != QDialog::Accepted) return;
        const auto cfg = dlg.config();
        if (cfg.channelStart > cfg.channelEnd) return;
        auto *win = new ChannelMapsWindow(
                cfg,
                this->remoteBackendUrl, this->remoteBackendToken,
                this->remoteSessionId, this->remoteDatasetId,
                this->remoteDatasetWidth, this->remoteDatasetHeight,
                this->remoteDatasetDepth,
                this->filepath, this);
        win->show();
        win->raise();
        this->registerAnalysisProduct(u"channelmaps"_s, u"Channel maps"_s, {}, win);
    });

    // ANALYSIS: persist-to-FITS exports (sub-cube crop + moment map). These
    // round-trip the heavy work through the backend exactly like the rest
    // of the ANALYSIS group, so the UI thread stays responsive while the
    // FITS is being written.
    m_actExportSubCube = ui->menuTools->addAction(u"Export Sub-Cube as FITS…"_s);
    m_actExportSubCube->setToolTip(
            u"Crop the current cube to a spatial+spectral ROI and persist "
            u"it as a standalone FITS file (WCS preserved)."_s);
    QObject::connect(m_actExportSubCube, &QAction::triggered,
                     this, &vtkWindowCube::exportSubCubeAsFits);

    m_actMaskRegion = ui->menuTools->addAction(u"Mask 3-D Region…"_s);
    m_actMaskRegion->setToolTip(
            u"Blank the voxels inside or outside a 3-D box (full dimensions + WCS "
            u"kept) → a new masked cube you can moment-map / profile on just the "
            u"selection. Isolate anomalous gas, or remove an artefact."_s);
    QObject::connect(m_actMaskRegion, &QAction::triggered,
                     this, &vtkWindowCube::maskCubeRegion3d);

    m_actKinematicLasso = ui->menuTools->addAction(u"Kinematic Lasso (click to select)"_s);
    m_actKinematicLasso->setCheckable(true);
    m_actKinematicLasso->setToolTip(
            u"Click a source in the 3-D view to select the whole kinematically-"
            u"coherent object (geodesic, velocity-aware) — no coordinates, no box. "
            u"The green isosurface is the selection."_s);
    QObject::connect(m_actKinematicLasso, &QAction::toggled,
                     this, &vtkWindowCube::setKinematicLassoActive);
    QObject::connect(&m_lassoSelectWatcher,
                     &QFutureWatcher<RemoteKinematicSelectResult>::finished,
                     this, &vtkWindowCube::onLassoSelectReady);

    m_actExportChannel2d = ui->menuTools->addAction(
            u"Export Current Channel as 2-D FITS…"_s);
    m_actExportChannel2d->setToolTip(
            u"Save the currently displayed channel as a true 2-D FITS image "
            u"(no degenerate spectral axis). Use this to extract Q/U/I/V at a "
            u"single frequency for the Stokes / spectral-index workflows."_s);
    QObject::connect(m_actExportChannel2d, &QAction::triggered,
                     this, &vtkWindowCube::exportCurrentChannelAs2dFits);

    m_actSliceHistogram = ui->menuTools->addAction(u"Pixel Histogram (current slice)…"_s);
    m_actSliceHistogram->setToolTip(
            u"Open a floating histogram of the current 2-D slice with two "
            u"draggable cursors (red = low, green = high) that live-clip the "
            u"LUT range. Useful for identifying multi-plane products such as "
            u"the planes of MFImage RM cubes."_s);
    QObject::connect(m_actSliceHistogram, &QAction::triggered,
                     this, &vtkWindowCube::showSliceHistogramPanel);

    m_actExportMomentFits = ui->menuTools->addAction(u"Export Moment Map as FITS…"_s);
    m_actExportMomentFits->setToolTip(
            u"Persist the moment map currently displayed in the 2-D view "
            u"as a standalone 2-D FITS file (celestial WCS + BUNIT)."_s);
    // Disabled until a moment has actually been computed (otherwise we'd
    // have nothing to export). updateMomentExportEnabled() flips this on
    // when momentController publishes its first result.
    m_actExportMomentFits->setEnabled(false);
    QObject::connect(m_actExportMomentFits, &QAction::triggered,
                     this, &vtkWindowCube::exportMomentMapAsFits);

    // Send current slice / moment to an open image viewer as contour overlay
    m_actSendSliceToImage = ui->menuTools->addAction(u"Send Slice to Image Viewer…"_s);
    m_actSendSliceToImage->setToolTip(
            u"Overlay the current 2-D slice as contours on any open image viewer."_s);
    QObject::connect(m_actSendSliceToImage, &QAction::triggered, this, [this]() {
        auto *output = this->remoteSliceDisplaySource
                ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
                : nullptr;
        if (!output || output->GetNumberOfPoints() == 0) {
            statusBar()->showMessage(u"No 2-D slice data available."_s, 2000);
            return;
        }
        const int ch = ui->sliderSlice->value();
        const QString label = QStringLiteral("Slice CH %1 from %2")
                .arg(ch).arg(QFileInfo(this->filepath).fileName());
        emit contourDataReady(label, output);
    });

    // Region IO (CRTF/DS9) on the 2-D view — interop parity with the image viewer.
    m_actImportCubeRegion = ui->menuTools->addAction(u"Import Region (CRTF/DS9)…"_s);
    m_actImportCubeRegion->setToolTip(
            u"Load a CASA CRTF or DS9 (.reg) region file onto the 2-D view and analyse it."_s);
    QObject::connect(m_actImportCubeRegion, &QAction::triggered,
                     this, &vtkWindowCube::importRegionsFromFile);
    m_actExportCubeRegion = ui->menuTools->addAction(u"Export Region (CRTF/DS9)…"_s);
    m_actExportCubeRegion->setToolTip(
            u"Save the drawn region as a CASA CRTF or DS9 (.reg) file."_s);
    QObject::connect(m_actExportCubeRegion, &QAction::triggered,
                     this, &vtkWindowCube::exportRegionsToFile);

    // Kinematic-model overlay (F2): contour a tilted-ring rotating-disk model
    // over the cube to highlight gas departing from regular rotation.
    m_actKinematicModel = ui->menuTools->addAction(u"Kinematic Model Overlay…"_s);
    m_actKinematicModel->setToolTip(
            u"Overlay a tilted-ring rotating-disk model on the cube to highlight "
            u"gas that departs from regular rotation (anomalous / extra-planar)."_s);
    QObject::connect(m_actKinematicModel, &QAction::triggered,
                     this, &vtkWindowCube::showKinematicModelDialog);
    QObject::connect(&this->m_kinematicWatcher,
                     &QFutureWatcher<RemoteKinematicModelResult>::finished,
                     this, &vtkWindowCube::onKinematicModelReady);

    // Movie export (F3): channel scan / camera orbit → MP4 (or PNG frames).
    m_actExportMovie = ui->menuTools->addAction(u"Export Movie…"_s);
    m_actExportMovie->setToolTip(
            u"Export an animation — a velocity channel scan or a 3-D camera "
            u"orbit — as an MP4 video (or a PNG frame sequence)."_s);
    QObject::connect(m_actExportMovie, &QAction::triggered,
                     this, &vtkWindowCube::showExportMovieDialog);

    // Linked views (F3): sync camera / channel / colour-scale with other open
    // cube viewers that also have linking enabled.
    m_actLinkViews = ui->menuTools->addAction(u"Link Views (sync camera / channel / colour)"_s);
    m_actLinkViews->setCheckable(true);
    m_actLinkViews->setToolTip(
            u"Synchronise camera, velocity channel and colour map across all open "
            u"cube viewers that have Link Views enabled — for side-by-side comparison."_s);
    QObject::connect(m_actLinkViews, &QAction::toggled,
                     this, &vtkWindowCube::setLinkViews);
    vtkWindowCube::s_cubeWindows.append(this);

    // REGIONS group
    ui->menuTools->addSeparator();
    this->actionBoxRegion     = ui->menuTools->addAction(u"Box Region Analysis"_s);
    this->actionCircleRegion  = ui->menuTools->addAction(u"Circle Region Analysis"_s);
    this->actionPolygonRegion = ui->menuTools->addAction(u"Polygon Region Analysis"_s);
    this->actionAnnulusRegion = ui->menuTools->addAction(u"Annulus Region Analysis"_s);
    this->actionBoxRegion->setCheckable(true);
    this->actionCircleRegion->setCheckable(true);
    this->actionPolygonRegion->setCheckable(true);
    this->actionAnnulusRegion->setCheckable(true);
    {
        // ExclusiveOptional = at most one of the four region-shape actions
        // can be checked at any moment, but the user can also have NONE
        // active (clicking the currently-active shape un-checks it, which
        // turns off region drawing entirely). The old setExclusive(false)
        // let the user check Circle + Polygon together, which then made
        // the cube viewer enter an undefined region-mode state.
        auto *regionGroup = new QActionGroup(this);
        regionGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
        regionGroup->addAction(this->actionBoxRegion);
        regionGroup->addAction(this->actionCircleRegion);
        regionGroup->addAction(this->actionPolygonRegion);
        regionGroup->addAction(this->actionAnnulusRegion);
    }
    QObject::connect(this->actionBoxRegion, &QAction::toggled, this,
                     [this](bool checked) { this->setRegionMode(RegionMode::Box, checked); });
    QObject::connect(this->actionCircleRegion, &QAction::toggled, this,
                     [this](bool checked) { this->setRegionMode(RegionMode::Circle, checked); });
    QObject::connect(this->actionPolygonRegion, &QAction::toggled, this,
                     [this](bool checked) { this->setRegionMode(RegionMode::Polygon, checked); });
    QObject::connect(this->actionAnnulusRegion, &QAction::toggled, this,
                     [this](bool checked) { this->setRegionMode(RegionMode::Annulus, checked); });

    // Wire the sidebar Tools page buttons to their actions via CubeUiAssembler.
    if (m_uiAssembler) {
        m_uiAssembler->wireToolActions(this->actionEstimateNoise,
                                       ui->actionExtractSpectrum,
                                       this->actionExtractPvDiagram,
                                       this->actionCameraRoi,
                                       ui->actionComputeMoment,
                                       ui->actionLinewidthMap,
                                       ui->actionBaselineSubtract,
                                       ui->actionStackCubes,
                                       actOpenInVR,
                                       this->actionShowWcsAxes,
                                       this->actionBoxRegion,
                                       this->actionCircleRegion,
                                       this->actionPolygonRegion,
                                       this->actionAnnulusRegion,
                                       this->m_actChannelMaps);
    }

    this->updateBackendDerivedActionsAvailability();

    // CATALOGUE group (menu-only — overlay state toggles don't fit the
    // sidebar's "do this action" tool-button paradigm).
    ui->menuTools->addSeparator();
    this->actionLoadCatalogueOverlay  = ui->menuTools->addAction(u"Load Catalogue Overlay"_s);
    this->actionShowCatalogueOverlay  = ui->menuTools->addAction(u"Show Catalogue Overlay"_s);
    this->actionShowCatalogueLabels   = ui->menuTools->addAction(u"Show Catalogue Labels"_s);
    this->actionClearCatalogueOverlay = ui->menuTools->addAction(u"Clear Catalogue Overlay"_s);
    this->actionShowCatalogueOverlay->setCheckable(true);
    this->actionShowCatalogueLabels->setCheckable(true);
    this->actionShowCatalogueOverlay->setEnabled(false);
    this->actionShowCatalogueLabels->setEnabled(false);
    this->actionShowCatalogueLabels->setChecked(true);
    this->actionClearCatalogueOverlay->setEnabled(false);
    QObject::connect(this->actionLoadCatalogueOverlay, &QAction::triggered, this,
                     &vtkWindowCube::loadCatalogueOverlay);
    QObject::connect(this->actionShowCatalogueOverlay, &QAction::toggled, this,
                     &vtkWindowCube::setCatalogueOverlayVisible);
    QObject::connect(this->actionShowCatalogueLabels, &QAction::toggled, this,
                     [this](bool) { this->updateCatalogueOverlayLabels(); this->sliceWin->Render();
                                    this->momentWin->Render(); });
    QObject::connect(this->actionClearCatalogueOverlay, &QAction::triggered, this,
                     &vtkWindowCube::clearCatalogueOverlay);
    this->ensureCatalogueDock();

    // Setup Threshold UI
    const std::string bunit{}; // BUNIT comes from backend metadata, not AstroUtils
    if (!bunit.empty()) {
        ui->labelThresholdHdr->setText(
                u"THRESHOLD (%1)"_s.arg(QString::fromStdString(bunit)).toUpper());
    }
    ui->lineThreshold->setText(QString::number(this->lowerBound));
    ui->lineThreshold->setValidator(new QDoubleValidator(ui->lineThreshold));
    ui->btnCubeColor->setIcon(QIcon(u":/icons/COLORIZE.png"_s));
    QObject::connect(ui->lineThreshold, &QLineEdit::editingFinished, this,
                     &vtkWindowCube::thresholdLineChanged);
    QObject::connect(ui->sliderThreshold, &QSlider::actionTriggered, this,
                     &vtkWindowCube::thresholdSliderChanged);
    QObject::connect(ui->btnCubeColor, &QPushButton::clicked, this,
                     &vtkWindowCube::changeCubeColor);

    // Setup Slice UI
    this->refreshSpectralAxisUi();
    ui->lineSpectral->setText(this->formatSpectralAxisValue(0.0));
    QObject::connect(ui->sliderSlice, &QSlider::actionTriggered, this,
                     &vtkWindowCube::sliceSliderChanged);
    QObject::connect(ui->spinSlice, &QSpinBox::valueChanged, this,
                     &vtkWindowCube::sliceSpinChanged);
    {
        const int maxSliceValue = std::max(1, this->remoteSliceCount());
        ui->sliderSlice->setMinimum(1);
        ui->spinSlice->setMinimum(1);
        ui->sliderSlice->setMaximum(maxSliceValue);
        ui->spinSlice->setMaximum(maxSliceValue);
        ui->sliderSlice->setValue(1);
        ui->spinSlice->setValue(1);
    }

    // ── Floating channel scrubber anchored at the bottom of the 3D view ────
    // (mirrors ui->spinSlice; design ref: cubeviewer.jsx "Channel scrubber")
    {
        this->channelOverlay = new ChannelOverlay(ui->vtkCube);
        this->channelOverlay->setRange(ui->spinSlice->minimum(),
                                       ui->spinSlice->maximum());
        this->channelOverlay->setChannel(ui->spinSlice->value());
        this->channelOverlay->setSpectralLabel(ui->lineSpectral->text());
        ui->vtkCube->installEventFilter(this->channelOverlay);
        this->channelOverlay->show();

        // Overlay → sidebar spin: drives the existing slice-fetch pipeline.
        QObject::connect(this->channelOverlay, &ChannelOverlay::channelChanged,
                         this, [this](int v) {
                             if (ui->spinSlice->value() == v) return;
                             ui->spinSlice->setValue(v);
                         });
        // Sidebar spin → overlay (programmatic updates: range change, animation,
        // user drag of the sidebar slider). SignalBlocker inside ChannelOverlay
        // prevents the bidirectional loop.
        QObject::connect(ui->spinSlice, &QSpinBox::valueChanged, this, [this](int v) {
            if (this->channelOverlay) {
                this->channelOverlay->setChannel(v);
                this->channelOverlay->setSpectralLabel(ui->lineSpectral->text());
            }
            // Keep the spectrum window's channel marker in sync with the
            // slice the user is looking at: a dashed vertical line at the
            // spectral coordinate of the current channel makes it obvious
            // which sample of the profile corresponds to the slice on screen.
            if (this->probePlotWidget) {
                bool ok = false;
                // v is 1-based in the spin box; spectralAxisValue expects a
                // dataset voxel index (0-based).
                const double sp = this->spectralAxisValue(v - 1, &ok);
                this->probePlotWidget->setSpectrumCurrentChannel(
                        ok ? sp : std::numeric_limits<double>::quiet_NaN());
            }
        });
        QObject::connect(ui->sliderSlice, &QSlider::rangeChanged, this,
                         [this](int lo, int hi) {
                             if (this->channelOverlay) {
                                 this->channelOverlay->setRange(lo, hi);
                             }
                         });
    }

    // Setup Contours UI
    ui->lineLevel->setText(QString::number(this->level));
    ui->lineLevel->setValidator(new QIntValidator(ui->lineLevel));
    ui->lineLowerBound->setText(QString::number(this->lowerBound));
    ui->lineLowerBound->setValidator(new QDoubleValidator(ui->lineLowerBound));
    ui->lineUpperBound->setText(QString::number(this->upperBound));
    ui->lineUpperBound->setValidator(new QDoubleValidator(ui->lineUpperBound));
    QObject::connect(ui->checkContours, &QCheckBox::checkStateChanged, this,
                     &vtkWindowCube::updateContoursVisibility);
    // The sidebar's "Show Contours" toggle button drives the canonical
    // checkbox; the existing checkStateChanged handler above continues to
    // own the visibility update logic.
    if (m_uiAssembler && m_uiAssembler->contoursButton()) {
        QObject::connect(m_uiAssembler->contoursButton(), &QToolButton::toggled,
                         ui->checkContours, &QCheckBox::setChecked);
    }
    QObject::connect(ui->lineLevel, &QLineEdit::editingFinished, this,
                     &vtkWindowCube::updateContours);
    QObject::connect(ui->lineLowerBound, &QLineEdit::editingFinished, this,
                     &vtkWindowCube::updateContours);
    QObject::connect(ui->lineUpperBound, &QLineEdit::editingFinished, this,
                     &vtkWindowCube::updateContours);

    // Backend-authoritative: statistics fields show "—" until populated.
    ui->lineCubeMin->clear();
    ui->lineCubeMax->clear();
    ui->lineCubeMean->clear();
    ui->lineCubeRms->clear();
    ui->lineImgMin->setText(u"\u2014"_s);
    ui->lineImgMax->setText(u"\u2014"_s);
    ui->lineImgMean->setText(u"\u2014"_s);
    ui->lineImgRms->setText(u"\u2014"_s);
    this->setCubeOpenActionsEnabled(false);
    ui->actionIsosurface->setChecked(false);
    ui->actionVolume->setChecked(true);
    this->setCubeRenderModeLocally(false);
    this->setRemoteCubeDisplayState(RemoteCubeDisplayState::Preview);
    this->showPersistentStatusMessage(u"Loading preview..."_s);
    this->remotePreviewWatcher.setProperty("requestId", ++this->currentRemotePreviewRequestId);
    // Adaptive preview downsample — target the user's PreviewTargetMb
    // setting (default 100 MB). For a small cube this comes out to
    // factor 1 (the preview IS the full data); for an SKA-scale cube
    // it can reach 30-60 per axis and we still hit the target. See
    // computePreviewDownsample() docstring for the math.
    // Use a short-lived Settings instance pointed at the canonical
    // workDir (matches MainWindow's construction at startup); this
    // reads from the same Cube/preview_target_mb key the dialog
    // would write to once we wire the UI.
    const QString workDir = QDir::home().absoluteFilePath(qApp->applicationName());
    Settings settings(workDir);
    const int targetMb = settings.getPreviewTargetMb();
    const int downsample = computePreviewDownsample(
            this->remoteDatasetWidth,
            this->remoteDatasetHeight,
            this->remoteDatasetDepth,
            static_cast<double>(targetMb));
    qDebug().noquote() << QStringLiteral(
            "[preview-debug] launching fetcher requestId=%1 url=%2 dataset=%3 "
            "cube=%4x%5x%6 target=%7MB downsample=%8")
                                  .arg(this->currentRemotePreviewRequestId)
                                  .arg(this->remoteBackendUrl)
                                  .arg(this->remoteDatasetId)
                                  .arg(this->remoteDatasetWidth)
                                  .arg(this->remoteDatasetHeight)
                                  .arg(this->remoteDatasetDepth)
                                  .arg(targetMb)
                                  .arg(downsample);
    this->remotePreviewWatcher.setFuture(
            QtConcurrent::run(&fetchRemotePreview, this->remoteBackendUrl, this->remoteDatasetId,
                              downsample, this->remoteSessionId, this->remoteBackendToken));
    qDebug().noquote() << QStringLiteral("[preview-debug] setFuture returned, isRunning=%1")
                                  .arg(this->remotePreviewWatcher.isRunning());

    // Inspector shell + first health poll: done last so every Analysis-tab tool
    // action (created above) is available and Linked is wired.
    if (this->m_workspaceChrome) {
        this->setupInspector();
        this->m_workspaceChrome->start();
    }
}

vtkWindowCube::~vtkWindowCube()
{
    // Stop the slice animation timer and detach any vtk observer we installed,
    // so we don't fire callbacks after the widget hierarchy starts going away.
    this->sliceAnimationTimer.stop();
    this->remove3dPickObserver();
    // Leave the linked-views group and drop the camera observer from the exact
    // interactor it was added to (weak ptr → only if still alive).
    vtkWindowCube::s_cubeWindows.removeAll(this);
    if (this->m_cubeCameraInteractor) {
        if (this->m_cubeCameraObserverTag != 0)
            this->m_cubeCameraInteractor->RemoveObserver(this->m_cubeCameraObserverTag);
        if (this->m_cubeCameraEndObserverTag != 0)
            this->m_cubeCameraInteractor->RemoveObserver(this->m_cubeCameraEndObserverTag);
    }
    this->m_cubeCameraObserverTag = 0;
    this->m_cubeCameraEndObserverTag = 0;
    // §3.2 B3: destroy the per-slot Pane2DViews FIRST — their destructors remove the
    // render-window EndEvent observer, so it can't fire against a half-torn-down widget
    // once `delete ui` starts tearing the pane hierarchy down.
    for (auto *&pv : m_paneViews) {
        delete pv;
        pv = nullptr;
    }
    delete ui;
}

// ── Sidebar ───────────────────────────────────────────────────────────────────

void vtkWindowCube::showFitsHeaderDialog()
{
    // Reuse the same backend endpoint the Remote File Browser already uses.
    BackendClient client(this->remoteBackendUrl, this->remoteBackendToken);
    const auto result = client.fileHeader(this->filepath);

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setWindowTitle(u"FITS Header — %1"_s.arg(QFileInfo(this->filepath).fileName()));
    dlg->resize(620, 480);

    auto *vbox = new QVBoxLayout(dlg);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    auto *header = new QFrame(dlg);
    header->setStyleSheet(
            QStringLiteral(
                    "QFrame {"
                    "  background-color: %1;"
                    "  border-bottom: 1px solid %2;"
                    "}"
                    "QFrame QLabel { background: transparent; }")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                         QLatin1StringView(VisivoTheme::kOutline())));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(16, 12, 16, 12);
    headerLayout->setSpacing(2);
    auto *title = new QLabel(u"FITS Header"_s, header);
    title->setStyleSheet(
            QStringLiteral("color: %1; font-size: 14px; font-weight: 700;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
    headerLayout->addWidget(title);
    auto *sub = new QLabel(this->filepath, header);
    sub->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; "
                            "font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    sub->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerLayout->addWidget(sub);
    vbox->addWidget(header);

    auto *body = new QPlainTextEdit(dlg);
    body->setReadOnly(true);
    body->setLineWrapMode(QPlainTextEdit::NoWrap);
    body->setStyleSheet(
            QStringLiteral(
                    "QPlainTextEdit {"
                    "  background-color: %1; color: %2;"
                    "  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "  font-size: 11px;"
                    "  border: none;"
                    "  padding: 12px 14px;"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kSurface()),
                         QLatin1StringView(VisivoTheme::kOnSurface())));
    if (result.valid && !result.cards.isEmpty()) {
        body->setPlainText(result.cards.join(u"\n"_s));
    } else if (!result.valid) {
        body->setPlainText(
                QStringLiteral("Could not load FITS header.\n\n%1")
                        .arg(result.error.isEmpty() ? u"Unknown error."_s : result.error));
    } else {
        body->setPlainText(u"(empty header)"_s);
    }
    vbox->addWidget(body, 1);

    dlg->show();
}



// ── setupThresholdPage / setup2dViewPage / setupToolsPage / setupInfoStatsPage
//    have been moved to CubeUiAssembler.  setupSidebar() calls them via
//    m_uiAssembler->build*Page().
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCube::applyVolumeLutToPipeline(const QString &name)
{
    if (name.isEmpty())
        return;

    const std::string stdName = name.toStdString();

    // Volume color transfer function only — slice/moment LUTs are managed separately.
    // Use the scalar range of the current cube data so the CTF is mapped over the
    // real data range (not the degenerate [0,0] placeholder range from setup time).
    auto *cubeImage =
            vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    double cubeRange[2] = { this->currentCubeVisibleRange[0],
                            this->currentCubeVisibleRange[1] };
    if (cubeImage) {
        // GetScalarRange reflects the actual data values including any sanitisation.
        cubeImage->GetScalarRange(cubeRange);
    }

    // Keep lutVolume member in sync — it is the authoritative LUT for the 3D
    // LUT customizer dialog and must always reflect the current volume state.
    // build() fills the colormap AND re-applies the tracked stretch so swapping
    // the volume colormap keeps a Sqrt/Square/Power selection.
    LutScale::build(this->lutVolume, stdName, this->m_volumeScaleMode, cubeRange[0], cubeRange[1],
                    this->m_volumeScaleGamma);
    ColorMaps::SetColorTransferFunction(this->lutVolume, this->volumeColorTransferFunction);
    this->volumeColorTransferFunction->SetObjectName(stdName);
    // NOTE: does NOT call Render() — caller decides when to render.
}

void vtkWindowCube::setVolumeLut(const QString &name)
{
    if (name.isEmpty())
        return;

    this->m_activeColorMapName = name;

    // Update the combo without triggering the currentTextChanged signal (which
    // would call applyColorMapByName and render prematurely).
    if (auto *combo = m_uiAssembler ? m_uiAssembler->lutCombo() : nullptr) {
        const QSignalBlocker block(combo);
        combo->setCurrentText(name);
    }

    this->applyVolumeLutToPipeline(name);
    // NOTE: does NOT call Render() — caller is responsible for the next render.
}

void vtkWindowCube::applyColorMapByName(const QString &name)
{
    if (name.isEmpty())
        return;

    this->m_activeColorMapName = name;

    // Apply LUT to the VTK pipeline using the real data range.
    this->applyVolumeLutToPipeline(name);

    // Re-render the 3D cube view.
    if (ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }

    // Mirror to the remote renderer when SSR is active so the volume
    // colours stay in sync with the user's pick. Re-push threshold
    // first so the rebuilt opacity TF uses the latest slider value
    // (same defensive pattern as applyVolumeBlendMode).
    this->resendThresholdToRemote();
    QVariantMap cmd;
    cmd[u"type"_s] = u"set_lut"_s;
    cmd[u"name"_s] = name;
    this->sendRemoteRenderStateCommand(cmd);

    // Linked views: mirror the colour (map + volume stretch) to linked cubes.
    if (this->m_linkViews && !this->m_applyingLinkedUpdate)
        this->broadcastLinkedColour();
}

void vtkWindowCube::applyVolumeBlendMode(int idx)
{
    if (!this->volumeMapper) {
        return;
    }
    this->m_volumeBlendMode = idx;   // tracked for linked-views sync
    switch (idx) {
    case 1:
        this->volumeMapper->SetBlendModeToMaximumIntensity();
        break;
    case 2:
        this->volumeMapper->SetBlendModeToMinimumIntensity();
        break;
    default:
        this->volumeMapper->SetBlendModeToComposite();
        break;
    }
    if (ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }
    // Mirror blend mode to the remote renderer — push threshold first
    // so MIP/MinIP renders against the user's current slider value,
    // not whatever the backend was last told (which can lag behind
    // when the user hasn't touched the threshold since session open).
    this->resendThresholdToRemote();
    QVariantMap cmd;
    cmd[u"type"_s] = u"set_blend_mode"_s;
    cmd[u"mode"_s] = (idx == 1 || idx == 2) ? idx : 0;
    this->sendRemoteRenderStateCommand(cmd);
}

void vtkWindowCube::updateVolumeModeVisibility(bool volumeActive)
{
    // Sidebar cards delegated to assembler.
    if (m_uiAssembler) {
        if (auto *c = m_uiAssembler->volumeModeCard())   c->setVisible(volumeActive);
        if (auto *c = m_uiAssembler->colorMapCard())     c->setVisible(volumeActive);
        if (auto *c = m_uiAssembler->surfaceColorCard()) c->setVisible(!volumeActive);
    }
    // Volume Rendering menu is owned by vtkWindowCube.
    if (this->m_menuVolumeRendering)
        this->m_menuVolumeRendering->setEnabled(volumeActive);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2D View Settings
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCube::updateInfoStatsPage()
{
    if (!m_uiAssembler)
        return;

    // ── Dataset load state ────────────────────────────────────────────────────
    CubeUiAssembler::InfoStatsData d;
    switch (this->remoteCubeDisplayState) {
    case RemoteCubeDisplayState::Preview:
        d.loadStateText        = u"Preview proxy"_s;
        d.loadStateStyleSheet  = u"QLabel { font-size: 9px; color: #E8A840; }"_s;
        break;
    case RemoteCubeDisplayState::LoadingFullResolution:
        d.loadStateText        = u"Loading full resolution\u2026"_s;
        d.loadStateStyleSheet  = u"QLabel { font-size: 9px; color: #E8A840; font-style: italic; }"_s;
        break;
    case RemoteCubeDisplayState::FullResolution:
        d.loadStateText        = u"Full resolution"_s;
        d.loadStateStyleSheet  = u"QLabel { font-size: 9px; color: #7AAABB; }"_s;
        break;
    }

    // ── Coordinate / WCS ─────────────────────────────────────────────────────
    d.wcsFrame    = this->currentWcsFrameLabel();
    d.spectralAxis = this->spectralAxisTitle();
    if (this->remoteWcsStatus.compare(u"ok"_s, Qt::CaseInsensitive) == 0) {
        d.wcsBadgeText        = u"OK"_s;
        d.wcsBadgeStyleSheet  = u"QLabel { font-size: 9px; color: #5DA87A; }"_s;
    } else if (this->remoteWcsStatus.compare(u"degraded"_s, Qt::CaseInsensitive) == 0) {
        d.wcsBadgeText        = u"Degraded"_s;
        d.wcsBadgeStyleSheet  = u"QLabel { font-size: 9px; color: #E8A840; }"_s;
    } else {
        d.wcsBadgeText        = u"Repaired"_s;
        d.wcsBadgeStyleSheet  = u"QLabel { font-size: 9px; color: #9a6700; }"_s;
    }
    d.wcsBadgeTooltip = this->remoteWcsWarningMessage;

    // ── System / Status ───────────────────────────────────────────────────────
    d.renderModeText = ui->actionIsosurface->isChecked() ? u"Isosurface"_s : u"Volume Rendering"_s;
    d.imageModeText  = this->viewingSlice() ? u"Slice"_s : u"Moment Map"_s;
    switch (m_momentCardState) {
    case MomentCardState::NoMoment:
        d.momentStateText        = u"\u2014"_s;
        d.momentStateStyleSheet  = u"QLabel { font-size: 9px; color: #AABBCC; }"_s;
        break;
    case MomentCardState::Computing:
        d.momentStateText        = u"Computing\u2026"_s;
        d.momentStateStyleSheet  = u"QLabel { font-size: 9px; color: #E8A840; font-style: italic; }"_s;
        break;
    case MomentCardState::Ready:
        d.momentStateText        = this->momentProvenanceState.summary;
        d.momentStateStyleSheet  = u"QLabel { font-size: 9px; color: #7AAABB; }"_s;
        break;
    case MomentCardState::Error:
        d.momentStateText        = u"Error"_s;
        d.momentStateStyleSheet  = u"QLabel { font-size: 9px; color: #CC6060; }"_s;
        break;
    }

    m_uiAssembler->updateInfoStats(d);
}

void vtkWindowCube::applySliceLutByName(const QString &name)
{
    if (name.isEmpty()) {
        return;
    }
    this->m_activeSliceLutName = name;
    const std::string stdName = name.toStdString();

    // Update only the slice LUT — does not touch the 3D volume or moment LUT.
    // build() preserves a Sqrt/Square/Power stretch across the colormap swap.
    const double *r = this->lutSlice->GetTableRange();
    LutScale::build(this->lutSlice, stdName, this->m_sliceScaleMode, r[0], r[1],
                    this->m_sliceScaleGamma);
    this->lutSlice->Modified();

    if (this->active2dWin() && this->viewingSlice()) {
        this->active2dWin()->Render();
    }
}

void vtkWindowCube::applyMomentLutByName(const QString &name)
{
    if (name.isEmpty()) {
        return;
    }
    this->m_activeMomentLutName = name;
    const std::string stdName = name.toStdString();

    // Update only the moment LUT — does not touch the slice LUT or 3D volume.
    // build() preserves a Sqrt/Square/Power stretch across the colormap swap.
    const double *r = this->lutMoment->GetTableRange();
    LutScale::build(this->lutMoment, stdName, this->m_sliceScaleMode, r[0], r[1],
                    this->m_sliceScaleGamma);
    this->lutMoment->Modified();

    if (this->active2dWin() && !this->viewingSlice()) {
        this->active2dWin()->Render();
    }
}

void vtkWindowCube::requestMomentMode()
{
    // ── Case A: a moment map is already available ─────────────────────────
    if (this->momentProvenanceState.valid) {
        // Sync actions (they may not be in sync yet if called from the sidebar).
        const QSignalBlocker b1(ui->actionSlice);
        const QSignalBlocker b2(ui->actionMomentMap);
        ui->actionSlice->setChecked(false);
        ui->actionMomentMap->setChecked(true);
        m_uiAssembler->sync2dModeSidebar(false);
        this->update2dModeVisibility(false);
        this->changeImageRenderer();
        return;
    }

    // ── Case B: system is busy (cube loading, previous moment in flight) ──
    if (this->isBusy()) {
        // Revert the combo/action to Slice — can't start computation now.
        const QSignalBlocker b1(ui->actionSlice);
        const QSignalBlocker b2(ui->actionMomentMap);
        ui->actionSlice->setChecked(true);
        ui->actionMomentMap->setChecked(false);
        m_uiAssembler->sync2dModeSidebar(true);
        this->update2dModeVisibility(true);
        return;
    }

    // ── Case C: no moment yet — show the configuration dialog ────────────
    // startComputation() shows the dialog synchronously, then either returns
    // immediately (cancelled) or starts async computation and sets m_busy = true.
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info,
            DiagnosticsCategory::Scientific,
            QStringLiteral("MomentMap"),
            QStringLiteral("No moment map available — opening configuration dialog."),
            this->remoteDatasetId,
            this->remoteSessionId,
            QStringLiteral("moment"));

    this->momentController->startComputation(0);

    if (!this->momentController->isBusy()) {
        // Dialog was cancelled or failed before async start — revert to Slice.
        const QSignalBlocker b1(ui->actionSlice);
        const QSignalBlocker b2(ui->actionMomentMap);
        ui->actionSlice->setChecked(true);
        ui->actionMomentMap->setChecked(false);
        m_uiAssembler->sync2dModeSidebar(true);
        this->update2dModeVisibility(true);
        return;
    }

    // Computation is now in flight. Build a compact summary of what was requested.
    {
        const auto &cfg = this->momentController->lastAcceptedConfig();
        QString detail = u"Moment %1 · channels %2–%3"_s
                                 .arg(cfg.order)
                                 .arg(cfg.channelStart + 1)
                                 .arg(cfg.channelEnd + 1);
        if (cfg.maskEnabled) {
            detail += cfg.thresholdAuto
                    ? u" · mask auto (\u2248 3\u03c3)"_s
                    : u" · mask \u2265 %1"_s.arg(cfg.thresholdValue, 0, 'g', 4);
        }
        this->setMomentCardState(MomentCardState::Computing, detail);

        // Diagnostics — start.
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info,
                DiagnosticsCategory::Scientific,
                QStringLiteral("MomentMap"),
                QStringLiteral("Moment computation started: ") + detail,
                this->remoteDatasetId,
                this->remoteSessionId,
                QStringLiteral("moment"));
    }

    // Keep the UI in Slice view while computing — applyMomentMapResult() will
    // switch to Moment automatically when the result arrives.
    const QSignalBlocker b1(ui->actionSlice);
    const QSignalBlocker b2(ui->actionMomentMap);
    ui->actionSlice->setChecked(true);
    ui->actionMomentMap->setChecked(false);
    m_uiAssembler->sync2dModeSidebar(true);
    this->update2dModeVisibility(true);
}

void vtkWindowCube::setMomentCardState(MomentCardState state, const QString &detail)
{
    // Ensure MomentCardState stays in sync with the assembler enum.
    // If you add an enumerator to CubeUiAssembler::MomentCardData::State you must
    // add it here too, and vice-versa.
    static_assert(static_cast<int>(MomentCardState::NoMoment)
                  == static_cast<int>(CubeUiAssembler::MomentCardData::State::NoMoment));
    static_assert(static_cast<int>(MomentCardState::Computing)
                  == static_cast<int>(CubeUiAssembler::MomentCardData::State::Computing));
    static_assert(static_cast<int>(MomentCardState::Ready)
                  == static_cast<int>(CubeUiAssembler::MomentCardData::State::Ready));
    static_assert(static_cast<int>(MomentCardState::Error)
                  == static_cast<int>(CubeUiAssembler::MomentCardData::State::Error));

    m_momentCardState = state;
    if (!detail.isEmpty())
        m_momentCardDetail = detail;

    if (m_uiAssembler) {
        CubeUiAssembler::MomentCardData data;
        data.state              = static_cast<CubeUiAssembler::MomentCardData::State>(state);
        data.detail             = m_momentCardDetail;
        data.provenanceSummary  = this->momentProvenanceState.summary;
        data.provenanceDetails  = this->momentProvenanceState.details;
        m_uiAssembler->updateMomentCard(data);
    }
    this->updateInfoStatsPage();
}

void vtkWindowCube::update2dModeVisibility(bool sliceActive)
{
    if (!m_uiAssembler)
        return;
    if (auto *c = m_uiAssembler->sliceControlsCard())  c->setVisible(sliceActive);
    if (auto *c = m_uiAssembler->sliceLutCard())       c->setVisible(sliceActive);
    if (auto *c = m_uiAssembler->sliceContoursCard())  c->setVisible(sliceActive);
    if (auto *c = m_uiAssembler->momentInfoCard())     c->setVisible(!sliceActive);
    if (auto *c = m_uiAssembler->momentLutCard())      c->setVisible(!sliceActive);
}



// ── Server-side rendering: mode wiring ──────────────────────────────────────
//
// Three render modes are exposed:
//
//   Auto    — try Remote when the backend looks SSR-capable AND the dataset
//             was opened in ``preview_only`` mode (which signals it is too
//             big for a comfortable local full-resolution load). Falls back
//             to Local silently on any failure.
//   Local   — never use the remote renderer (current behaviour, what the
//             user gets before this change rolled out).
//   Remote  — force the remote renderer; surfaces an error banner if the
//             backend can't provide it.
//
// All three live under View → Rendering. A status-bar pill shows the
// effective mode + the live FPS reported by RemoteRenderWidget. Slice
// viewers, moment maps, region tools etc. continue to use the existing
// REST endpoints in either mode.


void vtkWindowCube::applyRenderMode(RenderMode requested, bool fromUserAction)
{
    // Reentrancy guard. The user can rage-click the banner's "Switch
    // to Remote" button during the 3-5 s WebRTC negotiation —
    // pile-ups used to trigger duplicate session-create requests AND
    // a duplicate ``connectToSession`` on the WebRTC client, which
    // then crashed in libdatachannel on teardown of the orphans.
    // The flag is cleared when the connection reaches Live OR when
    // the user explicitly drops back to Local.
    if ((requested == RenderMode::Remote || requested == RenderMode::Auto)
        && this->m_remoteSwitchInProgress) {
        qDebug().noquote() << QStringLiteral(
                "[remote-render] applyRenderMode(%1) ignored — already switching")
                .arg(requested == RenderMode::Remote ? "Remote" : "Auto");
        return;
    }

    this->m_renderMode = requested;
    // Sync the menu's check state in case this is a programmatic switch
    // (banner close, Auto fallback) rather than a user click.
    if (this->m_actionRenderAuto)   this->m_actionRenderAuto->setChecked(requested == RenderMode::Auto);
    if (this->m_actionRenderLocal)  this->m_actionRenderLocal->setChecked(requested == RenderMode::Local);
    if (this->m_actionRenderRemote) this->m_actionRenderRemote->setChecked(requested == RenderMode::Remote);

    // Auto mode falls back to Remote when either the backend told us
    // ``preview_only`` (it knows the cube is huge) OR the full-res
    // cube size exceeds the user's LocalFullThresholdMb setting. The
    // size-based path is what lets the client decide independently
    // when the backend just says ``auto_full``.
    const QString rmWorkDir = QDir::home().absoluteFilePath(qApp->applicationName());
    Settings rmSettings(rmWorkDir);
    const double rmCubeFullMb = (static_cast<double>(this->remoteDatasetWidth)
                                  * static_cast<double>(this->remoteDatasetHeight)
                                  * static_cast<double>(this->remoteDatasetDepth)
                                  * 4.0) / (1024.0 * 1024.0);
    const bool rmSizeWantsRemote =
            rmCubeFullMb > static_cast<double>(rmSettings.getLocalFullThresholdMb());
    const bool wantRemote = (requested == RenderMode::Remote)
            || (requested == RenderMode::Auto
                && (this->recommendedLoadMode == QLatin1String("preview_only")
                    || rmSizeWantsRemote)
                && !this->remoteBackendUrl.isEmpty());

    if (wantRemote) {
        // Mark switch in progress so further clicks are ignored until
        // the WebRTC stream goes Live (cleared in
        // ``onRemoteRenderConnectionStateChanged``). Also give the
        // user immediate visual feedback: hide the recommendation
        // banner and show a "Connecting…" message on the Local page
        // so they know the click registered while the 3-5 s WebRTC
        // negotiation happens.
        this->m_remoteSwitchInProgress = true;
        // Start the elapsed-time counter that drives the progressive
        // status messages. A 1 Hz ticker re-paints the banner text
        // based on how long we've been waiting — the user sees
        // movement instead of a frozen line, which on a 22 GB cube
        // can sit there for 2-3 minutes while the backend reads the
        // FITS file off /mnt/beegfs and uploads the volume texture
        // to the V100.
        this->m_remoteSwitchElapsed.start();
        if (this->m_localRecommendRemoteBanner
            && this->m_localRecommendRemoteLabel) {
            this->m_localRecommendRemoteLabel->setText(
                    u"Connecting to remote rendering…"_s);
            this->m_localRecommendRemoteBanner->setVisible(true);
        }
        // Disable the Switch button so the user physically can't
        // rage-click. The button comes back enabled when the stream
        // either goes Live (banner hides too) or fails (Disconnected
        // / Error path re-arms it for retry).
        if (this->m_localRecommendRemoteSwitchBtn) {
            this->m_localRecommendRemoteSwitchBtn->setEnabled(false);
            this->m_localRecommendRemoteSwitchBtn->setText(u"Connecting…"_s);
        }
        if (this->m_remoteRenderBannerLabel) {
            this->m_remoteRenderBannerLabel->setText(
                    u"Connecting to remote rendering…"_s);
        }
        if (!this->m_remoteSwitchProgressTimer) {
            this->m_remoteSwitchProgressTimer = new QTimer(this);
            this->m_remoteSwitchProgressTimer->setInterval(1000);
            QObject::connect(this->m_remoteSwitchProgressTimer,
                             &QTimer::timeout, this, [this]() {
                if (!this->m_remoteSwitchInProgress) {
                    this->m_remoteSwitchProgressTimer->stop();
                    return;
                }
                const qint64 secs = this->m_remoteSwitchElapsed.elapsed() / 1000;
                QString msg;
                if (secs < 5) {
                    msg = QStringLiteral(
                            "Connecting to remote rendering… (%1 s)").arg(secs);
                } else if (secs < 30) {
                    msg = QStringLiteral(
                            "Negotiating WebRTC + allocating cube on backend "
                            "GPU… (%1 s)").arg(secs);
                } else if (secs < 90) {
                    msg = QStringLiteral(
                            "Loading cube into backend GPU memory — large "
                            "datasets take a minute or two… (%1 s elapsed)")
                                  .arg(secs);
                } else {
                    const qint64 mins = secs / 60;
                    const qint64 rem  = secs % 60;
                    msg = QStringLiteral(
                            "Still loading — very large cubes (20+ GB) can "
                            "take several minutes. Elapsed %1m%2s. The 3D "
                            "will appear automatically when ready.")
                                  .arg(mins).arg(rem, 2, 10, QLatin1Char('0'));
                }
                if (this->m_localRecommendRemoteLabel) {
                    this->m_localRecommendRemoteLabel->setText(msg);
                }
                if (this->m_remoteRenderBannerLabel) {
                    this->m_remoteRenderBannerLabel->setText(msg);
                }
            });
        }
        this->m_remoteSwitchProgressTimer->start();
        if (this->m_renderSessionId.isEmpty()) {
            // Will flip to page 1 in the reply handler once the session
            // is created; until then we keep showing Local so the user
            // sees the preview the existing pipeline already rendered.
            this->requestRemoteRenderSession();
        } else if (this->m_renderStack) {
            this->m_renderStack->setCurrentIndex(1);
            this->m_effectiveRenderMode = RenderMode::Remote;
            // The session is still warm on the backend (held by
            // RenderManager's TTL); if the WebSocket was torn down on
            // the previous Local switch, re-open it. Hands the user a
            // live image again without paying for EGL warm-up.
            if (this->m_remoteRenderWidget && !this->m_remoteRenderWidget->isLive()) {
                this->connectRemoteRenderWidgetWithTransport(this->m_renderSessionId);
            }
        }
    } else {
        if (this->m_renderStack) {
            this->m_renderStack->setCurrentIndex(0);
        }
        this->m_effectiveRenderMode = RenderMode::Local;
        // For explicit Local clicks, disconnect the WebSocket so we stop
        // burning frames behind a hidden page. We KEEP the session alive
        // on the backend so a flip back to Remote is instant — the TTL
        // evicter in RenderManager (300 s idle) will reap it eventually.
        if (this->m_remoteRenderWidget && fromUserAction
            && requested == RenderMode::Local) {
            this->m_remoteRenderWidget->disconnectFromSession();
        }
    }

    // Move the floating channel scrubber to whichever 3-D widget is now
    // visible. Without this it stays parented to ui->vtkCube (the
    // Local-mode QVTKOpenGLNativeWidget), which is hidden inside the
    // stack while we're in Remote mode — and the user loses access to
    // the cutting-plane channel slider.
    if (this->channelOverlay) {
        QWidget *target = (this->m_effectiveRenderMode == RenderMode::Remote)
                ? static_cast<QWidget *>(this->m_remoteRenderWidget)
                : static_cast<QWidget *>(ui->vtkCube);
        QWidget *current = this->channelOverlay->parentWidget();
        if (target && target != current) {
            if (current) {
                current->removeEventFilter(this->channelOverlay);
            }
            this->channelOverlay->setParent(target);
            target->installEventFilter(this->channelOverlay);
            this->channelOverlay->show();
            // Force a synthetic resize on the new parent so the overlay
            // re-positions to the bottom-edge of the visible widget. We
            // can't call repositionOverParent() directly — it's private
            // — but a resize event triggers exactly that codepath.
            QResizeEvent ev(target->size(), target->size());
            QApplication::sendEvent(target, &ev);
        }
    }

    this->updateRenderModeBadges();
}


void vtkWindowCube::closeEvent(QCloseEvent *event)
{
    // Stop any background timers / observers we own before tearing down the
    // VTK widgets, so we don't try to update destroyed objects on the way out.
    this->sliceAnimationTimer.stop();
    this->remove3dPickObserver();

    // Tear down the remote render session BEFORE the rest of the cleanup;
    // fire-and-forget so the close isn't blocked on a slow backend. We
    // pass through Local first so the widget disconnects cleanly even if
    // the DELETE never makes it.
    this->releaseRemoteRenderSession();

    // Capture a thumbnail of the last view BEFORE the cube renderer is torn
    // down — the Data Hub's recents list shows it next to this dataset.
    if (!this->isBusy() && ui && ui->vtkCube) {
        ThumbnailCache::captureFromWidget(ui->vtkCube, this->filepath);
    }

    if (this->isBusy()) {
        if (this->cubeOpenWatcher.isRunning() || this->remotePreviewWatcher.isRunning()
            || this->remoteHighResCubeWatcher.isRunning()) {
            this->showPersistentStatusMessage(u"Loading cube data..."_s);
        } else if (this->activeRemoteSliceRequests > 0) {
            this->showPersistentStatusMessage(u"Loading slice..."_s);
        } else if (this->activeRemoteIsosurfaceRequests > 0 || this->isosurfaceWatcher.isRunning()) {
            this->showPersistentStatusMessage(u"Computing isosurface..."_s);
        } else if (this->pvController && this->pvController->isBusy()) {
            this->showPersistentStatusMessage(u"Computing PV diagram..."_s);
        } else {
            this->showPersistentStatusMessage(u"Computing moment map..."_s);
        }
        event->ignore();
        return;
    }

    QMainWindow::closeEvent(event);
}










void vtkWindowCube::finalizePvInteraction()
{
    if (this->isBusy()) {
        return;
    }
    if (this->m_lassoEditActive) {
        return;   // lasso 2-D edit consumes the right-click / double-click
    }

    if (this->regionMode == RegionMode::Polygon && this->regionDragging) {
        if (this->finalizePolygonRegion()) {
            this->active2dWin()->Render();
        }
        return;
    }

    if (!this->pvModeActive) {
        return;
    }

    const int *position = this->active2dWin()->GetInteractor()->GetEventPosition();
    if (position) {
        this->updatePvFromDisplayPosition(position[0], position[1]);
    }
    if (this->pvDragging && this->pvCursorValid
        && (this->pvPolylineVertices.empty() || this->pvPolylineVertices.back() != this->pvCurrentVoxel)) {
        this->pvPolylineVertices.push_back(this->pvCurrentVoxel);
    }

    std::vector<std::array<int, 2>> cleaned;
    cleaned.reserve(this->pvPolylineVertices.size());
    for (const auto &vertex : this->pvPolylineVertices) {
        if (cleaned.empty() || cleaned.back() != vertex) {
            cleaned.push_back(vertex);
        }
    }
    this->pvPolylineVertices = std::move(cleaned);
    this->pvDragging = false;
    this->pvValid = this->pvPolylineVertices.size() >= 2;
    this->refreshPvOverlay();

    qDebug().noquote() << QStringLiteral("[pv] finalize vertices=%1 valid=%2")
                                  .arg(this->pvPolylineVertices.size())
                                  .arg(this->pvValid);

    if (!this->pvValid) {
        this->statusBar()->showMessage(u"Define at least two points for PV path."_s, 2500);
        this->active2dWin()->Render();
        return;
    }

    this->extractCurrentPvDiagram();
    this->active2dWin()->Render();
    // DO NOT toggle off actionExtractPvDiagram here. It triggers
    // setPvModeActive(false) → clearPv() (guarded by `if (!pvDiagramWidget)`
    // which is still null until the async backend response arrives), wiping
    // the polyline before the plot window can take over the overlay's
    // lifetime. The uncheck happens later, inside the pvWatcher 'finished'
    // handler, right after pvDiagramWidget is created.
}





void vtkWindowCube::setPvModeActive(bool active)
{
    this->pvModeActive = active;
    if (!active) {
        this->pvDragging = false;
        // Only clear the overlay if no PV Diagram window is currently open.
        // When a window IS open it continues to show the path; clearPv() is
        // called from the pvDiagramWidget::destroyed handler when the user
        // closes that window.
        if (!this->pvDiagramWidget) {
            this->clearPv();
        }
        this->setInteractorStyleImage();
        if (auto *w2d = this->active2dWidget()) w2d->setCursor(this->probeModeActive ? Qt::CrossCursor : Qt::ArrowCursor);
        if (this->active2dWin()) {
            this->active2dWin()->Render();
        }
        return;
    }

    bool accepted = false;
    const int width = QInputDialog::getInt(this, u"PV Width"_s, u"Path width (pixels):"_s,
                                           this->pvWidthPixels, 1, 99, 1, &accepted);
    if (!accepted) {
        this->pvModeActive = false;
        if (this->actionExtractPvDiagram && this->actionExtractPvDiagram->isChecked()) {
            const QSignalBlocker blocker(this->actionExtractPvDiagram);
            this->actionExtractPvDiagram->setChecked(false);
        }
        return;
    }
    this->pvWidthPixels = width;

    if (this->probeModeActive && ui->actionExtractSpectrum->isChecked()) {
        ui->actionExtractSpectrum->setChecked(false);
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
    this->pvDragging = false;
    this->pvValid = false;
    this->clearPv();
    this->setInteractorStyleRegion();
    if (auto *w2d = this->active2dWidget()) w2d->setCursor(Qt::CrossCursor);
}


bool vtkWindowCube::updatePvFromDisplayPosition(int displayX, int displayY)
{
    auto *renderer = this->active2dWin() ? this->active2dWin()->GetRenderers()->GetFirstRenderer()
                                                  : nullptr;
    auto *imageData = this->viewingSlice()
            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
            : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
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

    this->pvCurrentVoxel = { voxelX, voxelY };
    this->pvCursorValid = true;
    this->refreshPvOverlay();
    return true;
}




// ── Beam ellipse indicator ───────────────────────────────────────────────────

void vtkWindowCube::setBeamInfo(double majorDeg, double minorDeg, double paDeg)
{
    m_beamMajorDeg = majorDeg;
    m_beamMinorDeg = minorDeg;
    m_beamPaDeg    = paDeg;

    if (!std::isfinite(majorDeg) || !std::isfinite(minorDeg)
        || majorDeg <= 0.0 || minorDeg <= 0.0) {
        if (m_beamActor) m_beamActor->VisibilityOff();
        return;
    }

    // Convert beam size from degrees to pixels using CDELT1 (deg/pixel).
    // BMAJ/BMIN are the FULL width at half max; the ellipse is drawn at that
    // diameter (not semi-axis).
    const double cdelt = std::abs(this->remoteDatasetCdelt[0]);
    if (cdelt <= 0.0) return;
    const double majPx = majorDeg / cdelt;
    const double minPx = minorDeg / cdelt;

    // Build an ellipse from parametric points. 64 sides is smooth enough
    // at any zoom level.
    const int nSides = 64;
    const double paRad = paDeg * M_PI / 180.0;

    vtkNew<vtkPoints> points;
    points->SetNumberOfPoints(nSides);
    vtkNew<vtkCellArray> poly;
    poly->InsertNextCell(nSides);
    for (int i = 0; i < nSides; ++i) {
        const double theta = 2.0 * M_PI * i / nSides;
        // Ellipse in local frame (semi-axes = half FWHM)
        const double lx = (minPx / 2.0) * std::cos(theta);
        const double ly = (majPx / 2.0) * std::sin(theta);
        // Rotate by PA (north through east = CCW in image coords)
        const double rx = lx * std::cos(paRad) - ly * std::sin(paRad);
        const double ry = lx * std::sin(paRad) + ly * std::cos(paRad);
        points->SetPoint(i, rx, ry, 0.0);
        poly->InsertCellPoint(i);
    }

    vtkNew<vtkPolyData> ellipseData;
    ellipseData->SetPoints(points);
    ellipseData->SetPolys(poly);

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(ellipseData);

    if (!m_beamActor) {
        m_beamActor = vtkSmartPointer<vtkActor>::New();
    }
    m_beamActor->SetMapper(mapper);
    m_beamActor->GetProperty()->SetColor(1.0, 1.0, 1.0);
    m_beamActor->GetProperty()->SetOpacity(0.6);
    m_beamActor->GetProperty()->SetRepresentationToSurface();
    m_beamActor->GetProperty()->EdgeVisibilityOn();
    m_beamActor->GetProperty()->SetEdgeColor(1.0, 1.0, 1.0);

    // Position: bottom-left corner of the slice image, offset by the beam
    // radius so the ellipse sits just inside the data area.
    const double offsetX = majPx / 2.0 + 5.0;
    const double offsetY = majPx / 2.0 + 5.0;
    m_beamActor->SetPosition(offsetX, offsetY, 0.0);
    m_beamActor->VisibilityOn();

    // Add to the 2D slice renderer if not already added.
    auto *ren = this->active2dWin()
                        ? this->active2dWin()->GetRenderers()->GetFirstRenderer()
                        : nullptr;
    if (ren && !ren->HasViewProp(m_beamActor)) {
        ren->AddActor(m_beamActor);
    }
    if (this->active2dWin()) {
        this->active2dWin()->Render();
    }
}

// ─────────────────────────────────────────────────────────────────────────────


void vtkWindowCube::refreshPvOverlay()
{
    const auto updateLine = [this](vtkImageData *imageData, vtkPoints *points, vtkCellArray *cells,
                                   vtkPolyData *polyData, vtkActor *actor, vtkPoints *upperPoints,
                                   vtkCellArray *upperCells, vtkPolyData *upperData,
                                   vtkActor *upperActor, vtkPoints *lowerPoints,
                                   vtkCellArray *lowerCells, vtkPolyData *lowerData,
                                   vtkActor *lowerActor) {
        // Hide if there's nothing to draw on. The polyline stays visible
        // through the PvDiagramWidget's lifetime even after PV mode is
        // toggled off — that way the user can see the path the plot was
        // computed from. clearPv() (called from the widget's destroyed
        // handler) is what actually removes it.
        const bool keepingForPlotWindow = (this->pvDiagramWidget != nullptr);
        if (!imageData || (!this->pvModeActive && !keepingForPlotWindow)) {
            actor->VisibilityOff();
            upperActor->VisibilityOff();
            lowerActor->VisibilityOff();
            return;
        }
        std::vector<std::array<int, 2>> drawVertices = this->pvPolylineVertices;
        // The hovering cursor preview only makes sense while the user is
        // actively drawing — don't extend the saved polyline with the
        // current mouse position when we're just keeping it on screen
        // because the plot window is open.
        if (this->pvModeActive
            && this->pvDragging && this->pvCursorValid
            && (drawVertices.empty() || drawVertices.back() != this->pvCurrentVoxel)) {
            drawVertices.push_back(this->pvCurrentVoxel);
        }
        if (drawVertices.size() < 2) {
            actor->VisibilityOff();
            upperActor->VisibilityOff();
            lowerActor->VisibilityOff();
            return;
        }

        points->Reset();
        cells->Reset();
        cells->InsertNextCell(static_cast<vtkIdType>(drawVertices.size()));
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(drawVertices.size()); ++i) {
            points->InsertNextPoint(drawVertices[static_cast<std::size_t>(i)][0],
                                    drawVertices[static_cast<std::size_t>(i)][1], 0.0);
            cells->InsertCellPoint(i);
        }
        polyData->Modified();
        actor->VisibilityOn();

        if (this->pvWidthPixels <= 1) {
            upperActor->VisibilityOff();
            lowerActor->VisibilityOff();
            return;
        }

        const auto sampledPath = this->pvSampledPath(drawVertices);
        if (sampledPath.size() < 2) {
            upperActor->VisibilityOff();
            lowerActor->VisibilityOff();
            return;
        }

        const double halfWidth = 0.5 * static_cast<double>(this->pvWidthPixels - 1);
        upperPoints->Reset();
        upperCells->Reset();
        upperCells->InsertNextCell(static_cast<vtkIdType>(sampledPath.size()));
        lowerPoints->Reset();
        lowerCells->Reset();
        lowerCells->InsertNextCell(static_cast<vtkIdType>(sampledPath.size()));

        for (vtkIdType i = 0; i < static_cast<vtkIdType>(sampledPath.size()); ++i) {
            const auto &point = sampledPath[static_cast<std::size_t>(i)];
            const auto normal = this->pvLocalNormalForSample(sampledPath,
                                                             static_cast<std::size_t>(i));
            upperPoints->InsertNextPoint(static_cast<double>(point[0]) + halfWidth * normal[0],
                                         static_cast<double>(point[1]) + halfWidth * normal[1],
                                         0.0);
            upperCells->InsertCellPoint(i);
            lowerPoints->InsertNextPoint(static_cast<double>(point[0]) - halfWidth * normal[0],
                                         static_cast<double>(point[1]) - halfWidth * normal[1],
                                         0.0);
            lowerCells->InsertCellPoint(i);
        }

        upperData->Modified();
        lowerData->Modified();
        upperActor->VisibilityOn();
        lowerActor->VisibilityOn();
    };

    updateLine(vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0)),
               this->slicePvPoints, this->slicePvCells, this->slicePvData, this->slicePvActor,
               this->slicePvUpperPoints, this->slicePvUpperCells, this->slicePvUpperData,
               this->slicePvUpperActor, this->slicePvLowerPoints, this->slicePvLowerCells,
               this->slicePvLowerData, this->slicePvLowerActor);
    updateLine(vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0)),
               this->momentPvPoints, this->momentPvCells, this->momentPvData, this->momentPvActor,
               this->momentPvUpperPoints, this->momentPvUpperCells, this->momentPvUpperData,
               this->momentPvUpperActor, this->momentPvLowerPoints, this->momentPvLowerCells,
               this->momentPvLowerData, this->momentPvLowerActor);
}

void vtkWindowCube::clearPv()
{
    this->pvValid = false;
    this->pvCursorValid = false;
    this->pvPolylineVertices.clear();
    this->slicePvActor->VisibilityOff();
    this->slicePvUpperActor->VisibilityOff();
    this->slicePvLowerActor->VisibilityOff();
    this->momentPvActor->VisibilityOff();
    this->momentPvUpperActor->VisibilityOff();
    this->momentPvLowerActor->VisibilityOff();
}



// ─────────────────────────────────────────────────────────────────────────────
// FITS exports (sub-cube crop + moment map)
// ─────────────────────────────────────────────────────────────────────────────
//
// Both flows are non-blocking: the synchronous BackendClient call runs in a
// QtConcurrent::run task, with a QFutureWatcher posting back to the UI
// thread. A QPointer<vtkWindowCube> guards the result handler so the user
// can close the cube window while the FITS is being written without the
// callback dereferencing a dead this.

namespace {
// Build a small modal dialog asking for X/Y/Z bounds for sub-cube export.
// Defaults to the full extent of the cube; pre-fills with the region
// bounds if one is currently drawn on the slice. Returns true if the
// user accepted; bounds are written into the inclusive 0-indexed *out
// arguments.
bool runSubCubeBoundsDialog(QWidget *parent,
                            int fullWidth, int fullHeight, int fullDepth,
                            int defaultX0, int defaultX1,
                            int defaultY0, int defaultY1,
                            int defaultZ0, int defaultZ1,
                            int &outX0, int &outX1,
                            int &outY0, int &outY1,
                            int &outZ0, int &outZ1)
{
    using namespace Qt::StringLiterals;
    QDialog dlg(parent);
    dlg.setWindowTitle(u"Export Sub-Cube as FITS"_s);
    dlg.setModal(true);

    auto *root = new QVBoxLayout(&dlg);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto *hint = new QLabel(
            u"Crop the current cube to a spatial + spectral ROI and save it "
            u"as a standalone FITS file. WCS is preserved (CRPIX shifted) "
            u"so coordinates of every pixel match the source."_s,
            &dlg);
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto *form = new QFormLayout();
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);

    auto makeSpin = [&dlg](int lo, int hi, int val) {
        auto *sb = new QSpinBox(&dlg);
        sb->setRange(lo, hi);
        sb->setValue(std::clamp(val, lo, hi));
        sb->setAlignment(Qt::AlignRight);
        return sb;
    };
    const int xMax = std::max(0, fullWidth  - 1);
    const int yMax = std::max(0, fullHeight - 1);
    const int zMax = std::max(0, fullDepth  - 1);
    auto *spX0 = makeSpin(0, xMax, defaultX0);
    auto *spX1 = makeSpin(0, xMax, defaultX1);
    auto *spY0 = makeSpin(0, yMax, defaultY0);
    auto *spY1 = makeSpin(0, yMax, defaultY1);
    auto *spZ0 = makeSpin(0, zMax, defaultZ0);
    auto *spZ1 = makeSpin(0, zMax, defaultZ1);
    auto makeRow = [&dlg](QSpinBox *lo, QSpinBox *hi) {
        auto *w = new QWidget(&dlg);
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        h->addWidget(lo, 1);
        h->addWidget(new QLabel(u"→"_s, w));
        h->addWidget(hi, 1);
        return w;
    };
    form->addRow(u"X (columns)"_s,  makeRow(spX0, spX1));
    form->addRow(u"Y (rows)"_s,     makeRow(spY0, spY1));
    form->addRow(u"Z (channels)"_s, makeRow(spZ0, spZ1));
    root->addLayout(form);

    auto *btnBox = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    btnBox->button(QDialogButtonBox::Ok)->setText(u"Export"_s);
    root->addWidget(btnBox);
    QObject::connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return false;
    // Normalise so the user can type the values in any order.
    outX0 = std::min(spX0->value(), spX1->value());
    outX1 = std::max(spX0->value(), spX1->value());
    outY0 = std::min(spY0->value(), spY1->value());
    outY1 = std::max(spY0->value(), spY1->value());
    outZ0 = std::min(spZ0->value(), spZ1->value());
    outZ1 = std::max(spZ0->value(), spZ1->value());
    return true;
}
} // namespace

void vtkWindowCube::exportSubCubeAsFits()
{
    // Pre-fill bounds from a drawn region if available. We can only do this
    // for axis-aligned shapes (Box) trivially; for Circle / Annulus /
    // Polygon use the AABB of the geometry (regionBounds2D — same helper
    // used by analyzeCurrentRegion). Always fall back to the full extent.
    int defX0 = 0, defX1 = std::max(0, this->remoteDatasetWidth  - 1);
    int defY0 = 0, defY1 = std::max(0, this->remoteDatasetHeight - 1);
    int defZ0 = 0, defZ1 = std::max(0, this->remoteDatasetDepth  - 1);
    if (this->regionValid && this->regionMode != RegionMode::None) {
        const auto bounds = regionBounds2D(
                this->regionMode,
                this->regionAnchorVoxel, this->regionCurrentVoxel,
                this->regionPolygonVertices);
        defX0 = std::clamp(bounds[0], 0, defX1);
        defX1 = std::clamp(bounds[1], defX0, std::max(0, this->remoteDatasetWidth  - 1));
        defY0 = std::clamp(bounds[2], 0, defY1);
        defY1 = std::clamp(bounds[3], defY0, std::max(0, this->remoteDatasetHeight - 1));
    }

    int x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;
    if (!runSubCubeBoundsDialog(this,
                                this->remoteDatasetWidth,
                                this->remoteDatasetHeight,
                                this->remoteDatasetDepth,
                                defX0, defX1, defY0, defY1, defZ0, defZ1,
                                x0, x1, y0, y1, z0, z1)) {
        return;
    }

    // Ask for the basename to save under in the **Workspace Exports**.
    // Empty input → backend synthesises a name from source + crop range.
    // The artefact stays on the backend; the user can browse / download /
    // delete it later from the DataHub "Workspace Exports" panel.
    const QFileInfo srcInfo(this->filepath);
    const QString defaultName =
            u"%1_subcube_x%2-%3_y%4-%5_z%6-%7.fits"_s
            .arg(srcInfo.completeBaseName())
            .arg(x0).arg(x1).arg(y0).arg(y1).arg(z0).arg(z1);
    bool ok = false;
    const QString basename = QInputDialog::getText(
            this, u"Export Sub-Cube as FITS"_s,
            u"Save in Workspace Exports as (.fits will be appended if missing;\n"
            u"existing names get a _1, _2 suffix):"_s,
            QLineEdit::Normal, defaultName, &ok);
    if (!ok) return;   // user cancelled

    const QString url       = this->remoteBackendUrl;
    const QString token     = this->remoteBackendToken;
    const QString sessionId = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;

    this->statusBar()->showMessage(u"Exporting sub-cube to workspace…"_s, 0);

    auto *watcher = new QFutureWatcher<BackendSaveSubregionResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSaveSubregionResult>::finished, this,
                     [self, watcher]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Export Sub-Cube"_s,
                                     u"Sub-cube export failed:\n\n%1"_s.arg(r.error));
                             return;
                         }
                         emit self->workspaceExportsChanged();
                         const QString msg =
                                 u"Sub-cube saved to the Workspace Exports.\n\n"
                                 u"Filename: %1\n"
                                 u"Path: %2\n"
                                 u"Shape: %3 × %4 × %5 (W × H × channels)\n\n"
                                 u"Browse / download / delete from the\n"
                                 u"Workspace Exports panel in the Data Hub.\n"
                                 u"This sub-cube is also available in the current\n"
                                 u"session as dataset '%6'."_s
                                 .arg(r.workspaceFilename, r.outputPath)
                                 .arg(r.width).arg(r.height).arg(r.depth)
                                 .arg(r.newDatasetId);
                         QMessageBox::information(self, u"Export Sub-Cube"_s, msg);
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, sessionId, datasetId, x0, x1, y0, y1, z0, z1, basename]() {
                BackendClient c(url, token);
                c.setSessionId(sessionId);
                return c.saveCubeSubregion(datasetId, x0, x1, y0, y1, z0, z1, basename);
            }));
}

void vtkWindowCube::maskCubeRegion3d()
{
    // Default box from a drawn region (AABB) or the full extent — same seeding as
    // the sub-cube export.
    int defX0 = 0, defX1 = std::max(0, this->remoteDatasetWidth  - 1);
    int defY0 = 0, defY1 = std::max(0, this->remoteDatasetHeight - 1);
    int defZ0 = 0, defZ1 = std::max(0, this->remoteDatasetDepth  - 1);
    if (this->regionValid && this->regionMode != RegionMode::None) {
        const auto bounds = regionBounds2D(this->regionMode, this->regionAnchorVoxel,
                                           this->regionCurrentVoxel, this->regionPolygonVertices);
        defX0 = std::clamp(bounds[0], 0, defX1);
        defX1 = std::clamp(bounds[1], defX0, std::max(0, this->remoteDatasetWidth  - 1));
        defY0 = std::clamp(bounds[2], 0, defY1);
        defY1 = std::clamp(bounds[3], defY0, std::max(0, this->remoteDatasetHeight - 1));
    }

    int x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;
    if (!runSubCubeBoundsDialog(this, this->remoteDatasetWidth, this->remoteDatasetHeight,
                                this->remoteDatasetDepth, defX0, defX1, defY0, defY1, defZ0, defZ1,
                                x0, x1, y0, y1, z0, z1)) {
        return;
    }

    // Choose what to blank.
    const QStringList modes{ u"Isolate box (blank everything OUTSIDE)"_s,
                             u"Remove box (blank everything INSIDE)"_s };
    bool ok = false;
    const QString choice = QInputDialog::getItem(
            this, u"Mask 3-D Region"_s,
            u"How should the selected box be masked?"_s, modes, 0, false, &ok);
    if (!ok) return;
    const QString mode = (choice == modes.at(1)) ? u"blank_inside"_s : u"blank_outside"_s;

    // Show the selected box in the 3-D volume while the mask computes (reuses the
    // noise-region 3-D box preview — cleared when the result arrives).
    this->setNoiseRegionPreview(x0, x1, y0, y1, z0, z1);
    if (m_actMaskRegion) m_actMaskRegion->setEnabled(false);   // no concurrent masks

    const QString url       = this->remoteBackendUrl;
    const QString token     = this->remoteBackendToken;
    const QString sessionId = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;
    this->statusBar()->showMessage(u"Masking cube region…"_s, 0);

    auto *watcher = new QFutureWatcher<BackendSaveSubregionResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSaveSubregionResult>::finished, this,
                     [self, watcher]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;
                         self->statusBar()->clearMessage();
                         self->clearNoiseRegionPreview();
                         if (self->m_actMaskRegion) self->m_actMaskRegion->setEnabled(true);
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Mask 3-D Region"_s,
                                     u"Masking failed:\n\n%1"_s.arg(r.error));
                             return;
                         }
                         emit self->workspaceExportsChanged();
                         QMessageBox::information(self, u"Mask 3-D Region"_s,
                             u"Masked cube saved to the Workspace Exports and registered "
                             u"in this session as '%1'.\n\n"
                             u"Filename: %2\n"
                             u"Shape: %3 × %4 × %5 (W × H × channels)\n\n"
                             u"Open it from the Workspace Exports panel in the Data Hub to "
                             u"volume-render, moment-map or profile just the selection."_s
                             .arg(r.newDatasetId, r.workspaceFilename)
                             .arg(r.width).arg(r.height).arg(r.depth));
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, sessionId, datasetId, x0, x1, y0, y1, z0, z1, mode]() {
                BackendClient c(url, token);
                c.setSessionId(sessionId);
                return c.maskCubeRegion(datasetId, x0, x1, y0, y1, z0, z1, mode);
            }));
}

void vtkWindowCube::exportCurrentChannelAs2dFits()
{
    if (this->remoteDatasetDepth <= 0) {
        QMessageBox::warning(this, u"Export Channel"_s,
                u"This dataset has no spectral axis to extract a channel from."_s);
        return;
    }

    const int channel = std::clamp(ui->sliderSlice->value() - 1,
                                   0, this->remoteDatasetDepth - 1);

    const QFileInfo srcInfo(this->filepath);
    const QString defaultName =
            u"%1_ch%2.fits"_s.arg(srcInfo.completeBaseName())
                              .arg(channel, 4, 10, QChar(u'0'));

    bool ok = false;
    const QString basename = QInputDialog::getText(
            this, u"Export Current Channel as 2-D FITS"_s,
            u"Save in Workspace Exports as (.fits is appended if missing;\n"
            u"existing names get a _1, _2 suffix):"_s,
            QLineEdit::Normal, defaultName, &ok);
    if (!ok) return;

    const QString url       = this->remoteBackendUrl;
    const QString token     = this->remoteBackendToken;
    const QString sessionId = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;

    this->statusBar()->showMessage(
            QStringLiteral("Exporting channel %1 to workspace…").arg(channel), 0);

    auto *watcher = new QFutureWatcher<BackendSaveChannel2dResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSaveChannel2dResult>::finished, this,
                     [self, watcher]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Export Channel"_s,
                                     u"Channel export failed:\n\n%1"_s.arg(r.error));
                             return;
                         }
                         emit self->workspaceExportsChanged();
                         const QString msg =
                                 u"Channel saved to the Workspace Exports as 2-D FITS.\n\n"
                                 u"Filename: %1\n"
                                 u"Path: %2\n"
                                 u"Shape: %3 × %4 (W × H)\n"
                                 u"Channel: %5\n\n"
                                 u"Browse / download / delete from the\n"
                                 u"Workspace Exports panel in the Data Hub.\n"
                                 u"Open the file in the image viewer to use it in the\n"
                                 u"Stokes / spectral-index / Faraday-RM workflows."_s
                                 .arg(r.workspaceFilename, r.outputPath)
                                 .arg(r.width).arg(r.height).arg(r.channel);
                         QMessageBox::information(self, u"Export Channel"_s, msg);
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, sessionId, datasetId, channel, basename]() {
                BackendClient c(url, token);
                c.setSessionId(sessionId);
                return c.saveCubeChannelAs2dFits(datasetId, channel, basename);
            }));
}

// ── Pixel histogram of the current 2-D slice ───────────────────────────────
//
// Floating window with a 256-bin QCustomPlot bar chart of the current slice
// values; two draggable cursors (red = low, green = high) live-clip the slice
// LUT range. Used to identify multi-plane products such as the planes of an
// MFImage RM cube where each "channel" is a different derived map.
void vtkWindowCube::showSliceHistogramPanel()
{
    if (m_histogramWindow) {
        m_histogramWindow->raise();
        m_histogramWindow->activateWindow();
        return;
    }

    auto *imageData = vtkImageData::SafeDownCast(
            this->remoteSliceDisplaySource
                    ? this->remoteSliceDisplaySource->GetOutputDataObject(0) : nullptr);
    if (!imageData || imageData->GetNumberOfPoints() == 0) {
        QMessageBox::information(this, u"Pixel Histogram"_s,
                u"No 2-D slice data is available yet."_s);
        return;
    }

    int extent[6];
    imageData->GetExtent(extent);
    const int width  = extent[1] - extent[0] + 1;
    const int height = extent[3] - extent[2] + 1;
    if (width <= 0 || height <= 0) return;

    std::vector<double> values;
    const qint64 pixels = static_cast<qint64>(width) * height;
    const int stride = std::max(
            1, static_cast<int>(std::sqrt(static_cast<double>(pixels) / 500000.0)));
    values.reserve(static_cast<std::size_t>(pixels / std::max(1, stride * stride)));
    for (int y = extent[2]; y <= extent[3]; y += stride) {
        for (int x = extent[0]; x <= extent[1]; x += stride) {
            const double v = imageData->GetScalarComponentAsDouble(x, y, 0, 0);
            if (std::isfinite(v)) values.push_back(v);
        }
    }
    if (values.empty()) {
        QMessageBox::information(this, u"Pixel Histogram"_s,
                u"This slice has no finite pixels."_s);
        return;
    }

    std::sort(values.begin(), values.end());
    const double vmin = values.front();
    const double vmax = values.back();
    if (vmax <= vmin) {
        QMessageBox::information(this, u"Pixel Histogram"_s,
                u"All pixels in this slice are the same value."_s);
        return;
    }

    constexpr int nBins = 256;
    const double binWidth = (vmax - vmin) / nBins;
    QVector<double> binCenters(nBins), counts(nBins, 0.0);
    for (int i = 0; i < nBins; ++i) binCenters[i] = vmin + (i + 0.5) * binWidth;
    for (const double v : values) {
        int bin = static_cast<int>((v - vmin) / binWidth);
        if (bin >= nBins) bin = nBins - 1;
        counts[bin] += 1.0;
    }

    auto *win = new QWidget(nullptr, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("Pixel Histogram — CH %1").arg(
            ui->sliderSlice->value()));
    win->resize(620, 380);
    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *info = new QLabel(QStringLiteral(
            "Slice %1 of %2  ·  range %3 — %4  ·  %5 valid pixels  ·  drag cursors to clip LUT")
                    .arg(ui->sliderSlice->value())
                    .arg(this->remoteDatasetDepth)
                    .arg(vmin, 0, 'g', 6)
                    .arg(vmax, 0, 'g', 6)
                    .arg(values.size()), win);
    layout->addWidget(info);

    auto *plot = new QCustomPlot(win);
    layout->addWidget(plot);

    auto *bars = new QCPBars(plot->xAxis, plot->yAxis);
    bars->setWidth(binWidth * 0.92);
    bars->setData(binCenters, counts);
    bars->setPen(QPen(QColor(70, 130, 180)));
    bars->setBrush(QColor(70, 130, 180, 120));

    plot->xAxis->setLabel(u"Pixel Value"_s);
    plot->yAxis->setLabel(u"Count"_s);
    plot->xAxis->setRange(vmin, vmax);
    const double maxCount = *std::max_element(counts.begin(), counts.end()) * 1.05;
    plot->yAxis->setRange(0, maxCount);

    const double *lutRange = this->lutSlice ? this->lutSlice->GetTableRange() : nullptr;
    const double clipLow  = lutRange ? lutRange[0] : vmin;
    const double clipHigh = lutRange ? lutRange[1] : vmax;

    auto *lowLine = new QCPItemLine(plot);
    lowLine->setPen(QPen(QColor(220, 50, 50), 2, Qt::DashLine));
    lowLine->start->setType(QCPItemPosition::ptPlotCoords);
    lowLine->end->setType(QCPItemPosition::ptPlotCoords);
    lowLine->start->setCoords(clipLow, 0);
    lowLine->end->setCoords(clipLow, maxCount);

    auto *highLine = new QCPItemLine(plot);
    highLine->setPen(QPen(QColor(50, 180, 50), 2, Qt::DashLine));
    highLine->start->setType(QCPItemPosition::ptPlotCoords);
    highLine->end->setType(QCPItemPosition::ptPlotCoords);
    highLine->start->setCoords(clipHigh, 0);
    highLine->end->setCoords(clipHigh, maxCount);

    auto *lowLabel = new QCPItemText(plot);
    lowLabel->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    lowLabel->position->setType(QCPItemPosition::ptPlotCoords);
    lowLabel->position->setCoords(clipLow, maxCount * 0.98);
    lowLabel->setText(QStringLiteral("Low: %1").arg(clipLow, 0, 'g', 5));
    lowLabel->setColor(QColor(220, 50, 50));

    auto *highLabel = new QCPItemText(plot);
    highLabel->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    highLabel->position->setType(QCPItemPosition::ptPlotCoords);
    highLabel->position->setCoords(clipHigh, maxCount * 0.98);
    highLabel->setText(QStringLiteral("High: %1").arg(clipHigh, 0, 'g', 5));
    highLabel->setColor(QColor(50, 180, 50));

    QPointer<vtkWindowCube> self(this);
    QPointer<QCPItemLine> lowLinePtr(lowLine);
    QPointer<QCPItemLine> highLinePtr(highLine);
    QPointer<QCPItemText> lowLabelPtr(lowLabel);
    QPointer<QCPItemText> highLabelPtr(highLabel);

    bool *draggingLow  = new bool(false);
    bool *draggingHigh = new bool(false);

    QObject::connect(plot, &QCustomPlot::mousePress, win,
                     [plot, lowLinePtr, highLinePtr, draggingLow, draggingHigh](QMouseEvent *event) {
        const double x = plot->xAxis->pixelToCoord(event->pos().x());
        if (lowLinePtr && std::abs(x - lowLinePtr->start->coords().x())
                < (plot->xAxis->range().size() * 0.015))
            *draggingLow = true;
        else if (highLinePtr && std::abs(x - highLinePtr->start->coords().x())
                < (plot->xAxis->range().size() * 0.015))
            *draggingHigh = true;
    });

    QObject::connect(plot, &QCustomPlot::mouseMove, win,
                     [plot, lowLinePtr, highLinePtr, lowLabelPtr, highLabelPtr,
                      draggingLow, draggingHigh, maxCount](QMouseEvent *event) {
        if (!*draggingLow && !*draggingHigh) return;
        const double x = plot->xAxis->pixelToCoord(event->pos().x());
        if (*draggingLow && lowLinePtr) {
            lowLinePtr->start->setCoords(x, 0);
            lowLinePtr->end->setCoords(x, maxCount);
            if (lowLabelPtr) {
                lowLabelPtr->position->setCoords(x, maxCount * 0.98);
                lowLabelPtr->setText(QStringLiteral("Low: %1").arg(x, 0, 'g', 5));
            }
            plot->replot();
        } else if (*draggingHigh && highLinePtr) {
            highLinePtr->start->setCoords(x, 0);
            highLinePtr->end->setCoords(x, maxCount);
            if (highLabelPtr) {
                highLabelPtr->position->setCoords(x, maxCount * 0.98);
                highLabelPtr->setText(QStringLiteral("High: %1").arg(x, 0, 'g', 5));
            }
            plot->replot();
        }
    });

    QObject::connect(plot, &QCustomPlot::mouseRelease, win,
                     [self, lowLinePtr, highLinePtr, draggingLow, draggingHigh](QMouseEvent *) {
        if (*draggingLow || *draggingHigh) {
            if (self && lowLinePtr && highLinePtr) {
                double lo = lowLinePtr->start->coords().x();
                double hi = highLinePtr->start->coords().x();
                if (lo > hi) std::swap(lo, hi);
                if (hi > lo && self->lutSlice) {
                    self->lutSlice->SetTableRange(lo, hi);
                    self->lutSlice->Build();
                    // A range change can widen the colorbar labels → re-fit only if
                    // the gutter width actually changed (else the zoom is preserved).
                    self->layout2dPaneFor(/*slice=*/true, /*force=*/false);
                    // Always repaint the new LUT — layout2dPaneFor() early-returns
                    // (no render) when the gutter did not change.
                    if (self->sliceWin) self->sliceWin->Render();
                }
            }
        }
        *draggingLow = false;
        *draggingHigh = false;
    });

    QObject::connect(win, &QObject::destroyed, win, [draggingLow, draggingHigh]() {
        delete draggingLow;
        delete draggingHigh;
    });

    plot->replot();
    m_histogramWindow = win;
    win->show();
}

void vtkWindowCube::exportMomentMapAsFits()
{
    if (!this->momentController) return;
    // §3.2: export the recipe of the moment currently DISPLAYED — with several moments
    // cached they differ. Prefer a VISIBLE Product pane's moment; else the most recently
    // computed one; else the controller's last accepted config (single-moment case).
    struct { int order, channelStart, channelEnd; bool maskEnabled;
             double thresholdValue; bool thresholdAuto; } cfg{};
    QString exportId;
    for (int i = 0; i < 4; ++i) {
        const PaneView &pv = m_slots[i].view;
        if (pv.kind == ViewId::Product && m_slots[i].frame && m_slots[i].frame->isVisible()
            && m_products2d.contains(pv.productId)) {
            exportId = pv.productId;
            break;
        }
    }
    if (exportId.isEmpty())
        exportId = m_lastComputedMomentProductId;
    auto pit = m_products2d.constFind(exportId);
    if (pit != m_products2d.constEnd()) {
        cfg = { pit->order, pit->channelStart, pit->channelEnd,
                pit->maskEnabled, pit->thresholdValue, pit->thresholdAuto };
    } else {
        const auto c = this->momentController->lastAcceptedConfig();
        cfg = { c.order, c.channelStart, c.channelEnd,
                c.maskEnabled, c.thresholdValue, c.thresholdAuto };
    }
    // The action is only enabled once a moment has been computed, so a
    // zero-extent config here would be a bug. Defensive guard anyway.
    if (cfg.channelEnd <= cfg.channelStart) {
        QMessageBox::warning(this, u"Export Moment as FITS"_s,
                u"No moment has been computed yet — open Tools → Compute "
                u"Moment first, then retry the export."_s);
        return;
    }

    // Ask for the basename to save under in the Workspace Exports. Empty
    // input → backend synthesises a name from source + order.
    const QFileInfo srcInfo(this->filepath);
    const QString defaultName =
            u"%1_m%2.fits"_s.arg(srcInfo.completeBaseName()).arg(cfg.order);
    bool ok = false;
    const QString basename = QInputDialog::getText(
            this, u"Export Moment as FITS"_s,
            u"Save in Workspace Exports as (.fits will be appended if missing;\n"
            u"existing names get a _1, _2 suffix):"_s,
            QLineEdit::Normal, defaultName, &ok);
    if (!ok) return;

    const QString url       = this->remoteBackendUrl;
    const QString token     = this->remoteBackendToken;
    const QString sessionId = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;
    const int order             = cfg.order;
    const int chStart           = cfg.channelStart;
    const int chEnd             = cfg.channelEnd;
    const bool maskEnabled      = cfg.maskEnabled;
    const double thresholdValue = cfg.thresholdValue;
    const bool thresholdAuto    = cfg.thresholdAuto;

    this->statusBar()->showMessage(
            u"Exporting moment-%1 map to workspace…"_s.arg(order), 0);

    auto *watcher = new QFutureWatcher<BackendSaveMomentFitsResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSaveMomentFitsResult>::finished, this,
                     [self, watcher, order]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self,
                                     u"Export Moment as FITS"_s,
                                     u"Moment FITS export failed:\n\n%1"_s.arg(r.error));
                             return;
                         }
                         emit self->workspaceExportsChanged();
                         QString msg =
                                 u"Moment-%1 map saved to the Workspace Exports.\n\n"
                                 u"Filename: %2\n"
                                 u"Path: %3\n"
                                 u"Shape: %4 × %5 (W × H)"_s
                                 .arg(order).arg(r.workspaceFilename, r.outputPath)
                                 .arg(r.width).arg(r.height);
                         if (!r.bunit.isEmpty()) {
                             msg += u"\nBUNIT: %1"_s.arg(r.bunit);
                         }
                         msg += u"\n\nBrowse / download / delete from the\n"
                                u"Workspace Exports panel in the Data Hub."_s;
                         QMessageBox::information(self,
                                 u"Export Moment as FITS"_s, msg);
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, sessionId, datasetId, order, chStart, chEnd,
             maskEnabled, thresholdValue, thresholdAuto, basename]() {
                BackendClient c(url, token);
                c.setSessionId(sessionId);
                return c.saveMomentFits(datasetId, order, chStart, chEnd,
                                        maskEnabled, thresholdValue, basename,
                                        thresholdAuto);
            }));
}



QString vtkWindowCube::formatSpatialPointSummary(const std::array<int, 2> &voxel) const
{
    QString axis0 = this->remoteFormatAxisCoordinate(0, voxel[0]);
    QString axis1 = this->remoteFormatAxisCoordinate(1, voxel[1]);
    if (this->remoteHasCelestialAxes()) {
        bool ok0 = false;
        bool ok1 = false;
        double nativeX = this->remoteVoxelToWcs(0, voxel[0], &ok0);
        double nativeY = this->remoteVoxelToWcs(1, voxel[1], &ok1);
        double frameX = nativeX;
        double frameY = nativeY;
        if (ok0 && ok1 && this->convertRemoteCelestialCoordinates(nativeX, nativeY, frameX, frameY)) {
            axis0 = this->formatRemoteOverlayCoordinate(0, frameX);
            axis1 = this->formatRemoteOverlayCoordinate(1, frameY);
        }
    }
    return u"(%1, %2)  %3=%4  %5=%6"_s.arg(voxel[0])
            .arg(voxel[1])
            .arg(this->selectedFrameAxisTitle(0))
            .arg(axis0)
            .arg(this->selectedFrameAxisTitle(1))
            .arg(axis1);
}

std::vector<std::array<int, 2>> vtkWindowCube::pvSampledPath(
        const std::vector<std::array<int, 2>> &vertices) const
{
    std::vector<std::array<int, 2>> sampledPoints;
    if (vertices.size() < 2) {
        return sampledPoints;
    }

    sampledPoints.reserve(vertices.size() * 4);
    for (std::size_t vertexIndex = 1; vertexIndex < vertices.size(); ++vertexIndex) {
        const auto &start = vertices[vertexIndex - 1];
        const auto &end = vertices[vertexIndex];
        const int dx = end[0] - start[0];
        const int dy = end[1] - start[1];
        const int steps = std::max(std::abs(dx), std::abs(dy));
        if (steps <= 0) {
            if (sampledPoints.empty() || sampledPoints.back() != start) {
                sampledPoints.push_back(start);
            }
            continue;
        }

        for (int step = 0; step <= steps; ++step) {
            const double t = static_cast<double>(step) / static_cast<double>(steps);
            const std::array<int, 2> point = { static_cast<int>(std::lround(
                                                       static_cast<double>(start[0]) + t * dx)),
                                               static_cast<int>(std::lround(
                                                       static_cast<double>(start[1]) + t * dy)) };
            if (sampledPoints.empty() || sampledPoints.back() != point) {
                sampledPoints.push_back(point);
            }
        }
    }

    return sampledPoints;
}

std::array<double, 2> vtkWindowCube::pvLocalNormalForSample(
        const std::vector<std::array<int, 2>> &sampledPoints, std::size_t index) const
{
    if (sampledPoints.empty()) {
        return { 0.0, 1.0 };
    }

    const auto &center = sampledPoints[index];
    const auto &prev = sampledPoints[index == 0 ? index : index - 1];
    const auto &next = sampledPoints[index + 1 < sampledPoints.size() ? index + 1 : index];
    double tx = static_cast<double>(next[0] - prev[0]);
    double ty = static_cast<double>(next[1] - prev[1]);
    if (tx == 0.0 && ty == 0.0) {
        tx = 1.0;
        ty = 0.0;
        if (index > 0) {
            tx = static_cast<double>(center[0] - sampledPoints[index - 1][0]);
            ty = static_cast<double>(center[1] - sampledPoints[index - 1][1]);
        } else if (index + 1 < sampledPoints.size()) {
            tx = static_cast<double>(sampledPoints[index + 1][0] - center[0]);
            ty = static_cast<double>(sampledPoints[index + 1][1] - center[1]);
        }
    }

    const double length = std::hypot(tx, ty);
    if (length <= 0.0) {
        return { 0.0, 1.0 };
    }

    const double nx = -ty / length;
    const double ny = tx / length;
    return { nx, ny };
}

void vtkWindowCube::extractCurrentPvDiagram()
{
    // Polyline must have been finalised before the action fires.
    if (!this->pvValid || this->pvPolylineVertices.size() < 2) {
        qDebug().noquote() << QStringLiteral("[pv] extraction skipped: line not valid");
        this->statusBar()->showMessage(u"Define at least two points for PV path."_s, 2000);
        return;
    }

    if (this->remoteDatasetId.isEmpty()) {
        const QString message =
                u"PV computation requires a backend-visible dataset. "
                u"Stage or open it on the backend first."_s;
        qWarning().noquote() << QStringLiteral("[pv] missing backend dataset_id");
        DiagnosticsManager::instance()->publish(DiagnosticsLevel::Warning,
                                                DiagnosticsCategory::Remote,
                                                QStringLiteral("PvController"), message,
                                                this->remoteDatasetId, this->remoteSessionId,
                                                QStringLiteral("pv"));
        QMessageBox::warning(this, u"PV Diagram"_s, message);
        this->statusBar()->showMessage(message, 4000);
        return;
    }

    qDebug().noquote()
            << QStringLiteral("[remote-pv] dispatching async pv vertices=%1 width=%2")
                       .arg(this->pvPolylineVertices.size())
                       .arg(this->pvWidthPixels);
    this->showPersistentStatusMessage(u"Computing PV diagram..."_s);
    this->pvController->startExtraction(this->pvPolylineVertices, this->pvWidthPixels);
}



bool vtkWindowCube::viewingIsosurface() const
{
    if (!this->currentIsosurfaceActor) {
        return false;
    }

    return ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()->HasViewProp(
            this->currentIsosurfaceActor);
}

bool vtkWindowCube::viewingSlice() const
{
    // §3.1: prefer the ACTIVE pane's 2-D view — at 4 panes slice and moment are both
    // on screen, so the global Slice/Moment action alone can't say which one probe /
    // resize / overlays should target. Fall back to the action when the active pane
    // is not a 2-D view (≤2 panes, or the active pane is 3D/Spectrum).
    if (m_activePane >= 0 && m_activePane < 4) {
        const ViewId k = m_slots[m_activePane].view.kind;
        if (k == ViewId::Slice2D)
            return true;
        // §3.2 B3: a Product pane is a display-only Pane2DView with no interactive
        // pipeline. Route probe/region/PV and any programmatic active2dWin() render to
        // the LIVE slice pipeline, NOT the now-dead shared moment window (which is never
        // shown) — tools act on the 2-D Slice, where they belong for a collapsed map.
        if (k == ViewId::Product)
            return true;
    }
    return ui->actionSlice->isChecked();
}

vtkRenderWindow *vtkWindowCube::active2dWin() const
{
    return this->viewingSlice() ? static_cast<vtkRenderWindow *>(this->sliceWin.GetPointer())
                                : static_cast<vtkRenderWindow *>(this->momentWin.GetPointer());
}

QVTKOpenGLNativeWidget *vtkWindowCube::active2dWidget() const
{
    return this->viewingSlice() ? ui->vtkImage : m_vtkMoment.data();
}

void vtkWindowCube::applyThresholdToPipeline(double threshold)
{
    // Mirror the new threshold to the remote renderer first so the
    // network call is on its way while the local VTK pipeline updates.
    {
        QVariantMap cmd;
        cmd[u"type"_s]  = u"set_threshold"_s;
        cmd[u"value"_s] = threshold;
        this->sendRemoteRenderStateCommand(cmd);
    }

    // Isosurface isovalue
    this->isosurfaceFilter->SetValue(0, threshold);

    // Volume opacity transfer function (Composite mode).
    // MIP and MinIP ignore the opacity TF — this curve only affects Composite.
    //
    // Shape: fully transparent below threshold, steep ramp just above it,
    // rapid saturation towards high values. Produces high contrast without
    // requiring the user to tune additional parameters.
    //
    //  opacity
    //  1.0 |                                  ___________
    //  0.9 |                          _______/
    //  0.6 |                  _______/
    //  0.25|          _______/
    //  0.0 |_________/
    //       lower   t-ε   t   t+5%  t+20%   upper
    {
        const double lower  = this->currentCubeVisibleRange[0];
        const double upper  = this->currentCubeVisibleRange[1];
        const double span   = upper - lower;

        // Guard: if range is degenerate or threshold is out of range, fall back to
        // a safe flat ramp so the volume is still visible.
        const bool valid = (span > 0.0)
                        && (threshold >= lower)
                        && (threshold <= upper);

        this->volumeOpacity->RemoveAllPoints();
        // Masked / NaN sentinel: always invisible.
        this->volumeOpacity->AddPoint(this->currentCubeInvisibleSentinel, 0.0);

        if (valid) {
            // Transparent sub-threshold zone — slight pull-back so the knee is sharp.
            this->volumeOpacity->AddPoint(lower,                         0.0);
            this->volumeOpacity->AddPoint(threshold - 0.01 * span,       0.0);
            // Steep ramp: threshold itself jumps to 0.25 (first visible voxels).
            this->volumeOpacity->AddPoint(threshold,                     0.25);
            // Rapid rise to 0.6 within 5 % of span above threshold.
            this->volumeOpacity->AddPoint(
                    std::min(threshold + 0.05 * span, upper),            0.6);
            // Saturates near 0.9 at 20 % of span above threshold.
            this->volumeOpacity->AddPoint(
                    std::min(threshold + 0.20 * span, upper),            0.9);
            // Full opacity at the top of the range.
            this->volumeOpacity->AddPoint(upper,                         1.0);
        } else {
            // Fallback: gentle linear ramp across the full range.
            this->volumeOpacity->AddPoint(lower,  0.0);
            this->volumeOpacity->AddPoint(upper,  0.4);
        }
    }

    // In isosurface mode, schedule an async remote recompute.
    if (!this->cubeOpenWatcher.isRunning() && ui->actionIsosurface->isChecked()) {
        this->scheduleIsosurfaceRecompute();
    }
    // NOTE: does NOT call Render() — caller decides when to render.
}

void vtkWindowCube::applyThreshold(float value)
{
    // Clamp to the current display range.
    const float clamped = std::clamp(value, this->lowerBound, this->upperBound);

    // Update UI widgets without triggering user-interaction signals.
    {
        const QSignalBlocker blockLine(ui->lineThreshold);
        ui->lineThreshold->setText(QString::number(clamped));

        const float span = this->upperBound - this->lowerBound;
        const int p = (span > 0.f)
                ? static_cast<int>(100.f * (clamped - this->lowerBound) / span)
                : 0;
        const QSignalBlocker blockSlider(ui->sliderThreshold);
        ui->sliderThreshold->setValue(p);
    }

    // Apply to the VTK pipeline. currentCubeVisibleRange must be valid at this point.
    // (Call only after applyCubeOpenResult() has set the range from real data.)
    this->applyThresholdToPipeline(static_cast<double>(clamped));
    // NOTE: does NOT call Render() — caller is responsible for the next render.
}

void vtkWindowCube::updateCube()
{
    // Read the current threshold from the UI and apply it to the pipeline + render.
    const double threshold = ui->lineThreshold->text().toDouble();
    this->applyThresholdToPipeline(threshold);
    ui->vtkCube->renderWindow()->Render();
}

std::array<int, 6> vtkWindowCube::computeVisibleROI()
{
    const auto fullRoi = std::array<int, 6> { 0,
                                              std::max(0, this->remoteDatasetWidth - 1),
                                              0,
                                              std::max(0, this->remoteDatasetHeight - 1),
                                              0,
                                              std::max(0, this->remoteDatasetDepth - 1) };
    this->currentRemoteRoiThicknessExpanded = false;
    if (!this->useCameraRoiRefinement) {
        this->currentRemoteRefinementModeLabel = u"Full"_s;
        return fullRoi;
    }

    auto *renderer = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!renderer || !cubeImage) {
        this->currentRemoteRefinementModeLabel = u"Full"_s;
        return fullRoi;
    }

    double displayedBounds[6];
    cubeImage->GetBounds(displayedBounds);
    if (!validBounds(displayedBounds)) {
        this->currentRemoteRefinementModeLabel = u"Full"_s;
        return fullRoi;
    }

    double viewportBounds[6];
    if (!computeViewportIntersectionBounds(renderer, ui->vtkCube->renderWindow(), displayedBounds,
                                           viewportBounds)) {
        this->currentRemoteRefinementModeLabel = u"Full"_s;
        return fullRoi;
    }

    double clippedBounds[6];
    for (int axis = 0; axis < 3; ++axis) {
        const int i0 = axis * 2;
        const int i1 = i0 + 1;
        clippedBounds[i0] = std::max(displayedBounds[i0], viewportBounds[i0]);
        clippedBounds[i1] = std::min(displayedBounds[i1], viewportBounds[i1]);
        if (!std::isfinite(clippedBounds[i0]) || !std::isfinite(clippedBounds[i1])
            || clippedBounds[i0] > clippedBounds[i1]) {
            this->currentRemoteRefinementModeLabel = u"Full"_s;
            return fullRoi;
        }
    }

    const auto convertAxis = [&](double worldMin, double worldMax, double displayMin, double displayMax,
                                 int fullSize) -> std::array<int, 2> {
        const int maxIndex = std::max(0, fullSize - 1);
        if (maxIndex <= 0 || !std::isfinite(worldMin) || !std::isfinite(worldMax)) {
            return { 0, maxIndex };
        }

        if (this->usingHighResCube) {
            const int start = std::clamp(static_cast<int>(std::floor(worldMin)), 0, maxIndex);
            const int end = std::clamp(static_cast<int>(std::ceil(worldMax)), 0, maxIndex);
            return { std::min(start, end), std::max(start, end) };
        }

        const double span = displayMax - displayMin;
        if (span <= 1e-9) {
            return { 0, maxIndex };
        }

        const double normalizedMin = std::clamp((worldMin - displayMin) / span, 0.0, 1.0);
        const double normalizedMax = std::clamp((worldMax - displayMin) / span, 0.0, 1.0);
        const int start =
                std::clamp(static_cast<int>(std::floor(normalizedMin * maxIndex)), 0, maxIndex);
        const int end =
                std::clamp(static_cast<int>(std::ceil(normalizedMax * maxIndex)), 0, maxIndex);
        return { std::min(start, end), std::max(start, end) };
    };

    auto x = convertAxis(clippedBounds[0], clippedBounds[1], displayedBounds[0], displayedBounds[1],
                         this->remoteDatasetWidth);
    auto y = convertAxis(clippedBounds[2], clippedBounds[3], displayedBounds[2], displayedBounds[3],
                         this->remoteDatasetHeight);
    auto z = convertAxis(clippedBounds[4], clippedBounds[5], displayedBounds[4], displayedBounds[5],
                         this->remoteDatasetDepth);

    const auto padAxis = [](std::array<int, 2> roi, int fullSize) -> std::array<int, 2> {
        const int maxIndex = std::max(0, fullSize - 1);
        const int extent = std::max(1, roi[1] - roi[0] + 1);
        const int padding = std::max(1, static_cast<int>(std::ceil(extent * 0.1)));
        roi[0] = std::max(0, roi[0] - padding);
        roi[1] = std::min(maxIndex, roi[1] + padding);
        return roi;
    };

    x = padAxis(x, this->remoteDatasetWidth);
    y = padAxis(y, this->remoteDatasetHeight);
    z = padAxis(z, this->remoteDatasetDepth);

    qDebug().noquote()
            << QStringLiteral("[remote-roi] viewport roi before thickness safeguard x=%1..%2 y=%3..%4 z=%5..%6")
                       .arg(x[0])
                       .arg(x[1])
                       .arg(y[0])
                       .arg(y[1])
                       .arg(z[0])
                       .arg(z[1]);

    const auto enforceMinThickness = [this](std::array<int, 2> roi, int fullSize,
                                            int minExtent) -> std::array<int, 2> {
        const int maxIndex = std::max(0, fullSize - 1);
        minExtent = std::max(1, std::min(minExtent, fullSize));
        const int currentExtent = roi[1] - roi[0] + 1;
        if (currentExtent >= minExtent) {
            return roi;
        }

        const int deficit = minExtent - currentExtent;
        const int expandBefore = deficit / 2;
        const int expandAfter = deficit - expandBefore;
        roi[0] = std::max(0, roi[0] - expandBefore);
        roi[1] = std::min(maxIndex, roi[1] + expandAfter);
        const int remainingDeficit = minExtent - (roi[1] - roi[0] + 1);
        if (remainingDeficit > 0) {
            if (roi[0] == 0) {
                roi[1] = std::min(maxIndex, roi[1] + remainingDeficit);
            } else if (roi[1] == maxIndex) {
                roi[0] = std::max(0, roi[0] - remainingDeficit);
            }
        }
        return roi;
    };

    const int minViewportExtentX =
            std::max(8, std::min(this->remoteDatasetWidth, std::max(12, this->remoteDatasetWidth / 24)));
    const int minViewportExtentY =
            std::max(8, std::min(this->remoteDatasetHeight, std::max(12, this->remoteDatasetHeight / 24)));
    const int minViewportExtentZ =
            std::max(6, std::min(this->remoteDatasetDepth, std::max(10, this->remoteDatasetDepth / 24)));
    const auto originalX = x;
    const auto originalY = y;
    const auto originalZ = z;
    x = enforceMinThickness(x, this->remoteDatasetWidth, minViewportExtentX);
    y = enforceMinThickness(y, this->remoteDatasetHeight, minViewportExtentY);
    z = enforceMinThickness(z, this->remoteDatasetDepth, minViewportExtentZ);
    this->currentRemoteRoiThicknessExpanded =
            (x != originalX) || (y != originalY) || (z != originalZ);

    qDebug().noquote()
            << QStringLiteral("[remote-roi] viewport roi final x=%1..%2 y=%3..%4 z=%5..%6 thicknessExpanded=%7")
                       .arg(x[0])
                       .arg(x[1])
                       .arg(y[0])
                       .arg(y[1])
                       .arg(z[0])
                       .arg(z[1])
                       .arg(this->currentRemoteRoiThicknessExpanded);

    if (x[0] > x[1] || y[0] > y[1] || z[0] > z[1]) {
        this->currentRemoteRefinementModeLabel = u"Full"_s;
        return fullRoi;
    }

    qDebug().noquote()
            << QStringLiteral("[remote-roi] viewport roi x=%1..%2 y=%3..%4 z=%5..%6")
                       .arg(x[0])
                       .arg(x[1])
                       .arg(y[0])
                       .arg(y[1])
                       .arg(z[0])
                       .arg(z[1]);
    const std::array<int, 6> viewportRoi = { x[0], x[1], y[0], y[1], z[0], z[1] };
    this->currentRemoteRefinementModeLabel = this->currentRemoteRoiThicknessExpanded
            ? u"Viewport ROI + Min Thickness"_s
            : u"Viewport ROI"_s;
    if (cubeImage->GetScalarType() != VTK_FLOAT || cubeImage->GetNumberOfScalarComponents() != 1) {
        qDebug().noquote() << QStringLiteral("[remote-roi] content roi fallback to viewport");
        return viewportRoi;
    }

    const double visibleMin = this->currentCubeVisibleRange[0];
    const double visibleMax = this->currentCubeVisibleRange[1];
    const double visibleRange = visibleMax - visibleMin;
    const double contentThreshold =
            visibleRange > 0.0 ? (visibleMin + 0.05 * visibleRange) : visibleMin;
    qDebug().noquote()
            << QStringLiteral("[remote-roi] content threshold visibleMin=%1 visibleMax=%2 threshold=%3")
                       .arg(visibleMin, 0, 'g', 8)
                       .arg(visibleMax, 0, 'g', 8)
                       .arg(contentThreshold, 0, 'g', 8);

    int extent[6];
    cubeImage->GetExtent(extent);
    const auto clampAxisToExtent = [&](std::array<int, 2> roi, int axis) -> std::array<int, 2> {
        const int minExtent = extent[axis * 2];
        const int maxExtent = extent[axis * 2 + 1];
        roi[0] = std::clamp(roi[0], minExtent, maxExtent);
        roi[1] = std::clamp(roi[1], minExtent, maxExtent);
        return roi;
    };

    auto contentX = clampAxisToExtent({ viewportRoi[0], viewportRoi[1] }, 0);
    auto contentY = clampAxisToExtent({ viewportRoi[2], viewportRoi[3] }, 1);
    auto contentZ = clampAxisToExtent({ viewportRoi[4], viewportRoi[5] }, 2);

    auto *values = static_cast<const float *>(cubeImage->GetScalarPointer());
    const int dimX = extent[1] - extent[0] + 1;
    const int dimY = extent[3] - extent[2] + 1;
    const int dimZ = extent[5] - extent[4] + 1;
    const auto linearIndex = [&](int xIdx, int yIdx, int zIdx) -> qsizetype {
        const qsizetype localX = xIdx - extent[0];
        const qsizetype localY = yIdx - extent[2];
        const qsizetype localZ = zIdx - extent[4];
        return localZ * static_cast<qsizetype>(dimX) * dimY + localY * dimX + localX;
    };

    int minContentX = contentX[1];
    int maxContentX = contentX[0];
    int minContentY = contentY[1];
    int maxContentY = contentY[0];
    int minContentZ = contentZ[1];
    int maxContentZ = contentZ[0];
    int meaningfulVoxelCount = 0;
    bool foundMeaningful = false;
    for (int zIdx = contentZ[0]; zIdx <= contentZ[1]; ++zIdx) {
        for (int yIdx = contentY[0]; yIdx <= contentY[1]; ++yIdx) {
            for (int xIdx = contentX[0]; xIdx <= contentX[1]; ++xIdx) {
                const float value = values[linearIndex(xIdx, yIdx, zIdx)];
                if (!std::isfinite(value)
                    || std::fabs(value - this->currentCubeInvisibleSentinel) <= 1e-6
                    || value < contentThreshold) {
                    continue;
                }

                foundMeaningful = true;
                ++meaningfulVoxelCount;
                minContentX = std::min(minContentX, xIdx);
                maxContentX = std::max(maxContentX, xIdx);
                minContentY = std::min(minContentY, yIdx);
                maxContentY = std::max(maxContentY, yIdx);
                minContentZ = std::min(minContentZ, zIdx);
                maxContentZ = std::max(maxContentZ, zIdx);
            }
        }
    }

    if (!foundMeaningful || minContentX > maxContentX || minContentY > maxContentY
        || minContentZ > maxContentZ) {
        qDebug().noquote() << QStringLiteral("[remote-roi] content roi fallback to viewport");
        return viewportRoi;
    }

    auto padContentAxis = [](int minValue, int maxValue, int fullSize, int minBound,
                             int maxBound) -> std::array<int, 2> {
        const int maxIndex = std::max(0, fullSize - 1);
        const int extentValue = std::max(1, maxValue - minValue + 1);
        const int padding = std::max(1, static_cast<int>(std::ceil(extentValue * 0.1)));
        return { std::max(minBound, std::max(0, minValue - padding)),
                 std::min(maxBound, std::min(maxIndex, maxValue + padding)) };
    };

    contentX = padContentAxis(minContentX, maxContentX, this->remoteDatasetWidth, viewportRoi[0],
                              viewportRoi[1]);
    contentY = padContentAxis(minContentY, maxContentY, this->remoteDatasetHeight, viewportRoi[2],
                              viewportRoi[3]);
    contentZ = padContentAxis(minContentZ, maxContentZ, this->remoteDatasetDepth, viewportRoi[4],
                              viewportRoi[5]);

    const int minContentExtentX =
            std::max(4, std::min(this->remoteDatasetWidth, std::max(8, (viewportRoi[1] - viewportRoi[0] + 1) / 20)));
    const int minContentExtentY =
            std::max(4, std::min(this->remoteDatasetHeight, std::max(8, (viewportRoi[3] - viewportRoi[2] + 1) / 20)));
    const int minContentExtentZ =
            std::max(4, std::min(this->remoteDatasetDepth, std::max(6, (viewportRoi[5] - viewportRoi[4] + 1) / 20)));
    const int minMeaningfulVoxelCount = std::max(
            16,
            std::min(512, ((viewportRoi[1] - viewportRoi[0] + 1) * (viewportRoi[3] - viewportRoi[2] + 1)
                           * std::max(1, viewportRoi[5] - viewportRoi[4] + 1))
                              / 2000));
    if (meaningfulVoxelCount < minMeaningfulVoxelCount) {
        qDebug().noquote()
                << QStringLiteral("[remote-roi] content roi fallback to viewport meaningful=%1 minRequired=%2")
                           .arg(meaningfulVoxelCount)
                           .arg(minMeaningfulVoxelCount);
        return viewportRoi;
    }

    if (contentX[1] - contentX[0] + 1 < minContentExtentX
        || contentY[1] - contentY[0] + 1 < minContentExtentY
        || contentZ[1] - contentZ[0] + 1 < minContentExtentZ) {
        qDebug().noquote()
                << QStringLiteral("[remote-roi] content roi fallback to viewport extent=%1x%2x%3 min=%4x%5x%6")
                           .arg(contentX[1] - contentX[0] + 1)
                           .arg(contentY[1] - contentY[0] + 1)
                           .arg(contentZ[1] - contentZ[0] + 1)
                           .arg(minContentExtentX)
                           .arg(minContentExtentY)
                           .arg(minContentExtentZ);
        return viewportRoi;
    }

    qDebug().noquote()
            << QStringLiteral("[remote-roi] content roi x=%1..%2 y=%3..%4 z=%5..%6 meaningful=%7")
                       .arg(contentX[0])
                       .arg(contentX[1])
                       .arg(contentY[0])
                       .arg(contentY[1])
                       .arg(contentZ[0])
                       .arg(contentZ[1])
                       .arg(meaningfulVoxelCount);
    this->currentRemoteRefinementModeLabel = this->currentRemoteRoiThicknessExpanded
            ? u"Viewport + Content ROI + Min Thickness"_s
            : u"Viewport + Content ROI"_s;
    return { contentX[0], contentX[1], contentY[0], contentY[1], contentZ[0], contentZ[1] };
}

bool vtkWindowCube::requestHighResCube()
{
    if (this->usingHighResCube || this->remoteHighResCubeWatcher.isRunning()) {
        return false;
    }

    const auto roi = this->computeVisibleROI();
    this->currentRemoteRoi = roi;
    qDebug().noquote()
            << QStringLiteral("[remote-roi] mode=%1 request x=%2..%3 y=%4..%5 z=%6..%7")
                       .arg(this->currentRemoteRefinementModeLabel)
                       .arg(roi[0])
                       .arg(roi[1])
                       .arg(roi[2])
                       .arg(roi[3])
                       .arg(roi[4])
                       .arg(roi[5]);
    this->setRemoteCubeDisplayState(RemoteCubeDisplayState::LoadingFullResolution);
    this->showPersistentStatusMessage(u"Loading full resolution..."_s);
    this->remoteHighResCubeWatcher.setProperty("requestId", ++this->currentRemoteHighResRequestId);
    this->remoteHighResCubeWatcher.setFuture(
            QtConcurrent::run(&fetchRemoteSubvolume, this->remoteBackendUrl, this->remoteDatasetId,
                              roi, this->remoteSessionId, this->remoteBackendToken));
    return true;
}

void vtkWindowCube::updateRemoteCuttingPlane(int sliceIndex)
{
    // Mirror the slice index to the SSR backend so the textured plane
    // in the remote scene shows the same channel. Cheap; throttled
    // automatically by RemoteRenderWidget's in-flight gate.
    {
        QVariantMap cmd;
        cmd[u"type"_s]        = u"set_cutting_plane"_s;
        cmd[u"slice_index"_s] = this->clampRemoteSliceIndex(sliceIndex);
        this->sendRemoteRenderStateCommand(cmd);
    }

    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!cubeImage) {
        qDebug().noquote() << QStringLiteral("[remote-plane] preview valid=0");
        this->remoteCuttingPlaneActor->VisibilityOff();
        return;
    }

    int extent[6];
    cubeImage->GetExtent(extent);
    if (extent[0] > extent[1] || extent[2] > extent[3] || extent[4] > extent[5]) {
        qDebug().noquote()
                << QStringLiteral("[remote-plane] invalid extent=%1,%2,%3,%4,%5,%6")
                           .arg(extent[0])
                           .arg(extent[1])
                           .arg(extent[2])
                           .arg(extent[3])
                           .arg(extent[4])
                           .arg(extent[5]);
        this->remoteCuttingPlaneActor->VisibilityOff();
        return;
    }

    const int clampedSlice = this->clampRemoteSliceIndex(sliceIndex);
    double bounds[6];
    cubeImage->GetBounds(bounds);

    double z = 0.5 * (bounds[4] + bounds[5]);
    const int realDepth = this->remoteSliceCount();
    if (realDepth > 1) {
        const double fraction =
                static_cast<double>(clampedSlice) / static_cast<double>(realDepth - 1);
        z = bounds[4] + fraction * (bounds[5] - bounds[4]);
    }

    this->remoteCuttingPlaneSource->SetOrigin(bounds[0], bounds[2], z);
    this->remoteCuttingPlaneSource->SetPoint1(bounds[1], bounds[2], z);
    this->remoteCuttingPlaneSource->SetPoint2(bounds[0], bounds[3], z);
    this->remoteCuttingPlaneSource->Modified();
    this->remoteCuttingPlaneSource->Update();
    if (auto *mapper = this->remoteCuttingPlaneActor->GetMapper()) {
        mapper->Modified();
    }
    this->remoteCuttingPlaneActor->Modified();
    this->remoteCuttingPlaneActor->VisibilityOn();
    qDebug().noquote()
            << QStringLiteral("[remote-plane] update z=%1 bounds=%2,%3,%4,%5,%6,%7 visible=%8")
                       .arg(z, 0, 'g', 12)
                       .arg(bounds[0], 0, 'g', 12)
                       .arg(bounds[1], 0, 'g', 12)
                       .arg(bounds[2], 0, 'g', 12)
                       .arg(bounds[3], 0, 'g', 12)
                       .arg(bounds[4], 0, 'g', 12)
                       .arg(bounds[5], 0, 'g', 12)
                       .arg(this->remoteCuttingPlaneActor->GetVisibility());
    // NOTE: caller is responsible for the next Render(). We used to call Render
    // here, but all 3 callers (applyCubeOpenResult, applyRemoteSliceResult,
    // updateRemoteSliceDragFeedback) already trigger a Render immediately
    // afterwards — and during the full-res swap that internal Render was
    // firing the synchronous GPU volume upload *before* the deferred render
    // in applyCubeOpenResult, causing the macOS beach-ball.
}

void vtkWindowCube::applyCubeOpenResult(const CubeOpenStageResult &result)
{
    if (!result.cubeImageData) {
        return;
    }

    QElapsedTimer totalTimer;
    totalTimer.start();

    const auto *currentCubeImage =
            vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (currentCubeImage != result.cubeImageData.GetPointer()) {
        if (result.preSanitized) {
            // Worker thread already scanned + sanitized the volume; just adopt
            // the visible range and sentinel without re-scanning here (which
            // would freeze the UI for multi-GB cubes).
            this->currentCubeVisibleRange = result.cubeRange;
            this->currentCubeInvisibleSentinel = result.invisibleSentinel;
            this->currentCubeBlankFraction = result.blankFraction;
        } else {
            const auto sanitized = sanitizeCubeScalarsInPlace(result.cubeImageData);
            this->currentCubeVisibleRange = sanitized.visibleRange;
            this->currentCubeInvisibleSentinel = sanitized.invisibleSentinel;
            // sanitizeCubeScalarsInPlace does not compute blankFraction; the
            // preview cube is small so a single quick rescan via the fast-path
            // computeBlankFraction is acceptable.
            this->currentCubeBlankFraction = computeBlankFraction(result.cubeImageData);
        }
    }

    this->cubeDisplaySource->SetOutput(result.cubeImageData);
    this->cubeDisplaySource->Modified();
    result.cubeImageData->Modified();
    this->remoteIsosurfaceReady = false;
    this->setCubeRenderModeLocally(false);
    if (result.momentImageData) {
        this->momentDisplaySource->SetOutput(result.momentImageData);
        this->momentDisplaySource->Modified();
        this->lutMoment->SetTableRange(result.momentRange[0], result.momentRange[1]);
        // Moment range changed → its colorbar labels may widen the gutter; re-fit
        // only if it actually changed (§3.0b).
        this->layout2dPaneFor(/*slice=*/false, /*force=*/false);
    }
    if (auto *mapper = this->volume->GetMapper()) {
        mapper->Modified();
    }
    if (this->currentIsosurfaceActor && this->currentIsosurfaceActor->GetMapper()) {
        this->currentIsosurfaceActor->GetMapper()->Modified();
    }

    const int clampedSlice = this->clampRemoteSliceIndex(ui->spinSlice->value() - 1);
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply clamp slice index: %1 ms").arg(
                       totalTimer.elapsed());

    QElapsedTimer sliderSyncTimer;
    sliderSyncTimer.start();
    {
        const QSignalBlocker blockSlider(ui->sliderSlice);
        const QSignalBlocker blockSpin(ui->spinSlice);
        const int maxSliceValue = std::max(1, this->remoteSliceCount());
        ui->sliderSlice->setMaximum(maxSliceValue);
        ui->spinSlice->setMaximum(maxSliceValue);
        ui->sliderSlice->setValue(clampedSlice + 1);
        ui->spinSlice->setValue(clampedSlice + 1);
    }
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply slider/spin sync: %1 ms").arg(
                       sliderSyncTimer.elapsed());

    QElapsedTimer cubeFieldsTimer;
    cubeFieldsTimer.start();
    ui->lineCubeMin->setText(QString::number(this->currentCubeVisibleRange[0]));
    ui->lineCubeMax->setText(QString::number(this->currentCubeVisibleRange[1]));
    ui->lineCubeMean->setText(QString::number(result.cubeMean));
    ui->lineCubeRms->setText(QString::number(result.cubeRms));
    ui->lineSpectral->setText(this->formatSpectralAxisValue(clampedSlice));
    this->refreshSpectralAxisUi();
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply cube UI fields: %1 ms").arg(
                       cubeFieldsTimer.elapsed());
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply data+ui sync: %1 ms").arg(totalTimer.elapsed());

    QElapsedTimer cameraTimer;
    cameraTimer.start();
    auto cubeRenderer = ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer();
    QElapsedTimer cubeFitTimer;
    cubeFitTimer.start();
    refitCubeCamera(cubeRenderer, result.cubeImageData);
    cubeRenderer->ResetCameraClippingRange();
    // Keep the 3D WCS axes (if the user enabled them) aligned with the new
    // cube bounds and WCS range. No-op when the actor hasn't been created yet.
    if (this->wcs3dAxesActor) {
        this->refresh3dWcsAxes();
    }
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply cube clipping sync: %1 ms").arg(
                       cubeFitTimer.elapsed());

    auto sliceRenderer = this->sliceWin->GetRenderers()->GetFirstRenderer();
    // Slice camera is fit when a remote slice tile is applied (applyRemoteSliceResult).

    QElapsedTimer sliceClipTimer;
    sliceClipTimer.start();
    sliceRenderer->ResetCameraClippingRange();
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply slice clipping sync: %1 ms").arg(
                       sliceClipTimer.elapsed());

    auto momentRenderer = this->momentWin->GetRenderers()->GetFirstRenderer();
    QElapsedTimer momentClipTimer;
    momentClipTimer.start();
    momentRenderer->ResetCameraClippingRange();
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply moment clipping sync: %1 ms").arg(
                       momentClipTimer.elapsed());
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply camera sync: %1 ms").arg(cameraTimer.elapsed());

    QElapsedTimer imgFieldsTimer;
    imgFieldsTimer.start();
    if (result.momentImageData) {
        ui->lineImgMin->setText(fmtStat(result.momentRange[0]));
        ui->lineImgMax->setText(fmtStat(result.momentRange[1]));
        double mean, rms;
        computeImageStats(result.momentImageData, mean, rms);
        ui->lineImgMean->setText(fmtStat(mean));
        ui->lineImgRms->setText(fmtStat(rms));
        this->updateLUTCustomizer();
    }
    if (this->probeModeActive && this->probeValid) {
        this->refreshProbeOverlay();
        auto *currentImage = this->viewingSlice()
                ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
                : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
        this->updateProbeReadout(currentImage);
    }
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply image UI fields: %1 ms").arg(
                       imgFieldsTimer.elapsed());

    this->setCubeRenderModeLocally(ui->actionIsosurface->isChecked());
    {
        // NOTE: do NOT call updateRemoteCuttingPlane() here — it triggers a
        // synchronous Render(), which would be the *first* render with the new
        // volume data and would block the UI thread on the GPU texture upload.
        // It is deferred into the singleShot below so the upload happens after
        // the event loop has had a chance to pump pending UI events.
        if (ui->actionIsosurface->isChecked()) {
            const double threshold = ui->lineThreshold->text().toDouble();
            qDebug().noquote()
                    << QStringLiteral("[remote-iso] block range=%1..%2 threshold=%3")
                               .arg(this->currentCubeVisibleRange[0], 0, 'g', 8)
                               .arg(this->currentCubeVisibleRange[1], 0, 'g', 8)
                               .arg(threshold, 0, 'g', 8);
            if (threshold < this->currentCubeVisibleRange[0]
                || threshold > this->currentCubeVisibleRange[1]) {
                qDebug().noquote()
                        << QStringLiteral("[remote-iso] recompute skipped threshold outside current ROI range");
                this->persistentStatusActive = false;
                this->statusMessageClearTimer.stop();
                this->statusBar()->showMessage(
                        u"Isosurface threshold is outside the current ROI data range (%1 .. %2)."_s
                                .arg(this->currentCubeVisibleRange[0], 0, 'g', 6)
                                .arg(this->currentCubeVisibleRange[1], 0, 'g', 6),
                        5000);
                {
                    const QSignalBlocker blockIso(ui->actionIsosurface);
                    const QSignalBlocker blockVolume(ui->actionVolume);
                    ui->actionIsosurface->setChecked(false);
                    ui->actionVolume->setChecked(true);
                }
                this->setCubeRenderModeLocally(false);
            } else {
                // Push the isosurface recompute well after the GPU has settled
                // from the volume upload below, so the iso compute does not
                // pile on top of the swap stall (C). 500 ms + 150 ms debounce.
                QTimer::singleShot(500, this, [this]() {
                    if (ui->actionIsosurface->isChecked()) {
                        this->scheduleIsosurfaceRecompute();
                    }
                });
            }
        }
    }
    cubeRenderer->Modified();
    ui->vtkCube->renderWindow()->Modified();

    // Keep the 3D WCS axes (if visible) in sync with the new cube bounds.
    if (this->wcs3dAxesActor && this->actionShow3dWcsAxes
        && this->actionShow3dWcsAxes->isChecked()) {
        this->refresh3dWcsAxes();
    }

    // Defer the final renders by one event-loop tick (B). The data swap above
    // is cheap; the cost is the GPU volume upload that happens on the next
    // Render(). Letting the event loop pump first means pending UI events
    // (paint, status updates, cursor) are processed before the GPU stall,
    // so the transition no longer feels like a freeze of the whole app.
    QTimer::singleShot(50, this, [this, clampedSlice]() {
        this->updateRemoteCuttingPlane(clampedSlice);
        // Pump pending paint / status updates BEFORE the cube Render(), so
        // "Loading full resolution..." and any other UI changes are visible
        // before the synchronous GPU volume upload below blocks the thread.
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        QElapsedTimer renderTimer;
        renderTimer.start();
        // First Render with the new volume — this is the actual GPU texture
        // upload. Inherently synchronous in vtkGPUVolumeRayCastMapper. Kept
        // as a canary timer so future regressions on volume-upload cost are
        // visible in logs.
        ui->vtkCube->renderWindow()->Render();
        qDebug().noquote()
                << QStringLiteral("[perf][cube] apply cube Render (GPU upload): %1 ms")
                           .arg(renderTimer.elapsed());

        this->active2dWin()->Render();
        this->requestRemoteSlice(clampedSlice);
    });
    qDebug().noquote() << QStringLiteral("[perf][cube] apply total (sync): %1 ms").arg(
            totalTimer.elapsed());
}

void vtkWindowCube::updateSlice()
{
    // Debounce ALL slice-change paths (slider drag, spinbox arrows,
    // mouse wheel on slider, ChannelOverlay scrubber) through the same
    // 300 ms timer. Previously this method called requestRemoteSlice
    // immediately on every spinSlice valueChanged, bypassing the debounce
    // entirely — a mouse-wheel scroll over 50 channels generated 50
    // separate POSTs, saturating the SSH tunnel for ~5-10 s of pure
    // wasted bandwidth before the user's intended slice arrived.
    //
    // For paths that should be IMMEDIATE (typed value, animation tick,
    // programmatic) callers can still bypass the debounce by stopping
    // the timer and calling requestRemoteSlice() directly — see the
    // existing animation handlers.
    const int slice = ui->spinSlice->value() - 1;
    // Apply the visual cutting-plane feedback synchronously so the
    // user sees the new slice position move during the debounce window
    // — only the network fetch is delayed.
    this->updateRemoteSliceDragFeedback(slice);
    if (this->tryApplyCachedRemoteSlice(slice)) {
        this->remoteSliceDebounceTimer.stop();
        return;
    }
    this->pendingRemoteSliceIndex = slice;
    this->remoteSliceDebounceTimer.start(remoteSliceDebounceDelayMs);
}

























void vtkWindowCube::updateMomentProvenancePanel()
{
    // §3.0b (d): the moment provenance now lives in Inspector › Provenance (fed via
    // the registered product's params). The status-bar label is retired — it sat in
    // the middle of the rail next to the duplicate "Full resolution" text. Keep the
    // widget (momentProvenanceState is still read elsewhere) but never show it.
    if (this->momentProvenanceLabel) {
        this->momentProvenanceLabel->hide();
    }
}

void vtkWindowCube::updateSanityPanel()
{
    if (!m_uiAssembler)
        return;

    // blankFraction is cached from the worker-thread sanitization, so this
    // method runs in O(1) — no per-call rescan of the (possibly multi-GB) cube.
    const auto report = buildCubeSanityReport(this->remoteDatasetCtype, this->remoteDatasetCunit,
                                              this->remoteDatasetCrval, this->remoteDatasetCrpix,
                                              this->remoteDatasetCdelt,
                                              this->currentCubeBlankFraction);
    CubeUiAssembler::SanityStatusData d;
    d.text    = report.summary;
    d.tooltip = report.details;
    m_uiAssembler->updateSanityStatus(d);
}

void vtkWindowCube::updateWcsStatusIndicator()
{
    if (!m_uiAssembler)
        return;

    if (this->remoteWcsStatus.compare(u"ok"_s, Qt::CaseInsensitive) == 0) {
        m_uiAssembler->updateWcsStatus({false, {}, {}, {}});
        this->wcsDiagnosticPublished = false;
        return;
    }

    const bool degraded = this->remoteWcsStatus.compare(u"degraded"_s, Qt::CaseInsensitive) == 0;
    const QString message =
            this->remoteWcsWarningMessage.isEmpty()
            ? (degraded ? u"Remote cube opened with degraded WCS metadata."_s
                        : u"Remote cube opened with sanitized WCS metadata."_s)
            : this->remoteWcsWarningMessage;

    CubeUiAssembler::WcsStatusData d;
    d.visible    = true;
    d.text       = degraded ? u"WCS degraded"_s : u"WCS repaired"_s;
    d.styleSheet = degraded
            ? u"QLabel { padding-left: 8px; font-weight: 700; color: #b54708; }"_s
            : u"QLabel { padding-left: 8px; font-weight: 600; color: #9a6700; }"_s;
    d.tooltip    = message;
    m_uiAssembler->updateWcsStatus(d);

    if (!this->wcsDiagnosticPublished) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning,
                DiagnosticsCategory::WCS, QStringLiteral("vtkWindowCube"), message,
                this->remoteDatasetId, this->remoteSessionId, QStringLiteral("dataset-open"));
        this->wcsDiagnosticPublished = true;
    }
    this->updateInfoStatsPage();
}

void vtkWindowCube::updateDataStatePanel()
{
    if (!m_uiAssembler)
        return;

    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    constexpr QLatin1StringView origin{ "Remote" };
    QString representation;
    {
        const std::array<int, 6> fullExtent = { 0,
                                                std::max(0, this->remoteDatasetWidth - 1),
                                                0,
                                                std::max(0, this->remoteDatasetHeight - 1),
                                                0,
                                                std::max(0, this->remoteDatasetDepth - 1) };
        switch (this->remoteCubeDisplayState) {
        case RemoteCubeDisplayState::Preview:
            representation = u"Preview proxy"_s;
            break;
        case RemoteCubeDisplayState::LoadingFullResolution:
            representation = u"Preview -> full loading"_s;
            break;
        case RemoteCubeDisplayState::FullResolution:
            representation = (this->usingHighResCube && this->currentRemoteRoi != fullExtent)
                    ? u"ROI / subvolume"_s
                    : u"Full resolution"_s;
            break;
        }
    }
    const QString refinement = this->currentRemoteRefinementModeLabel;

    const QString loadedBounds = formatCubeBoundsSummary(cubeImage);
    const QString datasetBounds =
            u"x=0..%1 y=0..%2 z=0..%3"_s.arg(std::max(0, this->remoteDatasetWidth - 1))
                    .arg(std::max(0, this->remoteDatasetHeight - 1))
                    .arg(std::max(0, this->remoteDatasetDepth - 1));
    const QString degenerateSummary = this->remoteDegenerateAxesSummary;

    // Compact badge: just origin + resolution state + refinement mode.
    QString badge = u"%1 \u00b7 %2"_s.arg(origin, representation);
    if (this->useCameraRoiRefinement) {
        badge += u" \u00b7 ROI"_s;
    }
    if (!degenerateSummary.isEmpty()) {
        badge += u" \u26a0"_s; // warning sign — collapsed axes
    }

    // Full provenance in tooltip.
    QString tip =
            u"Origin: %1\nResolution: %2\nRefinement: %3\nLoaded: %4\nDataset: %5\n"
            u"WCS frame: %6\nSpectral axis: %7\nProbe/profile: loaded block only"_s
                    .arg(origin, representation, refinement, loadedBounds, datasetBounds,
                         this->currentWcsFrameLabel(), this->spectralAxisTitle());
    if (!degenerateSummary.isEmpty()) {
        tip += u"\n\u26a0 Collapsed axes: %1"_s.arg(degenerateSummary);
    }

    CubeUiAssembler::DataStateUiData d;
    d.text    = badge;
    d.tooltip = tip;
    m_uiAssembler->updateDataState(d);
    this->updateInfoStatsPage();
}


void vtkWindowCube::set2dWcsOverlayVisible(bool visible)
{
    // Legend actors require local AstroUtils (not available in backend mode); always use overlay axes.
    this->legendSlice->SetVisibility(false);
    this->legendMoment->SetVisibility(false);
    this->sliceOverlayXAxis->SetVisibility(visible);
    this->sliceOverlayYAxis->SetVisibility(visible);
    this->sliceOverlayXTitleActor->SetVisibility(visible);
    this->sliceOverlayYTitleActor->SetVisibility(visible);
    this->momentOverlayXAxis->SetVisibility(visible);
    this->momentOverlayYAxis->SetVisibility(visible);
    this->momentOverlayXTitleActor->SetVisibility(visible);
    this->momentOverlayYTitleActor->SetVisibility(visible);
    for (const auto &actor : this->sliceOverlayXTickActors) {
        if (actor) {
            actor->SetVisibility(visible);
        }
    }
    for (const auto &actor : this->sliceOverlayYTickActors) {
        if (actor) {
            actor->SetVisibility(visible);
        }
    }
    for (const auto &actor : this->momentOverlayXTickActors) {
        if (actor) {
            actor->SetVisibility(visible);
        }
    }
    for (const auto &actor : this->momentOverlayYTickActors) {
        if (actor) {
            actor->SetVisibility(visible);
        }
    }
    // §3.2 B3: product panes render into their own Pane2DView — propagate the toggle.
    for (int i = 0; i < 4; ++i)
        if (m_paneViews[i])
            m_paneViews[i]->setWcsAxesVisible(visible);
}













void vtkWindowCube::updateContoursVisibility()
{
    this->contoursActor->SetVisibility(ui->checkContours->isChecked());
    this->sliceWin->Render();
}

void vtkWindowCube::setMomentOrder(int order)
{
    // Cross-system busy guard: block moment computation while the cube itself
    // is still loading (slice/isosurface requests in flight, etc.).
    if (this->isBusy()) {
        return;
    }
    this->momentController->startComputation(order);

    // If the dialog was accepted and computation started, transition the card
    // to Computing and publish start diagnostics (mirrors requestMomentMode()).
    if (this->momentController->isBusy()) {
        const auto &cfg = this->momentController->lastAcceptedConfig();
        QString detail = u"Moment %1 · channels %2–%3"_s
                                 .arg(cfg.order)
                                 .arg(cfg.channelStart + 1)
                                 .arg(cfg.channelEnd + 1);
        if (cfg.maskEnabled) {
            detail += cfg.thresholdAuto
                    ? u" · mask auto (\u2248 3\u03c3)"_s
                    : u" · mask \u2265 %1"_s.arg(cfg.thresholdValue, 0, 'g', 4);
        }
        this->setMomentCardState(MomentCardState::Computing, detail);
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info,
                DiagnosticsCategory::Scientific,
                QStringLiteral("MomentMap"),
                QStringLiteral("Moment computation started: ") + detail,
                this->remoteDatasetId,
                this->remoteSessionId,
                QStringLiteral("moment"));
    }
}

void vtkWindowCube::showPersistentStatusMessage(const QString &text, int minDurationMs)
{
    this->statusMessageClearTimer.stop();
    this->persistentStatusActive = true;
    this->statusMessageMinDurationMs = minDurationMs;
    this->statusMessageElapsed.restart();
    this->statusBar()->showMessage(text);
}

void vtkWindowCube::clearPersistentStatusMessage()
{
    if (!this->persistentStatusActive) {
        this->statusBar()->clearMessage();
        return;
    }

    const int remaining = this->statusMessageMinDurationMs - this->statusMessageElapsed.elapsed();
    if (remaining <= 0) {
        this->persistentStatusActive = false;
        this->statusBar()->clearMessage();
        return;
    }

    this->statusMessageClearTimer.start(remaining);
}

void vtkWindowCube::updateContours()
{
    this->contours->GenerateValues(ui->lineLevel->text().toInt(),
                                   ui->lineLowerBound->text().toDouble(),
                                   ui->lineUpperBound->text().toDouble());
    this->sliceWin->Render();
}

void vtkWindowCube::thresholdSliderChanged(int action)
{
    Q_UNUSED(action);
    const int p = ui->sliderThreshold->sliderPosition();
    const float threshold = 0.01 * p * (this->upperBound - this->lowerBound) + this->lowerBound;
    ui->lineThreshold->setText(QString::number(threshold));
    this->updateCube();
}

void vtkWindowCube::thresholdLineChanged()
{
    const float threshold =
            std::clamp(ui->lineThreshold->text().toFloat(), this->lowerBound, this->upperBound);
    ui->lineThreshold->setText(QString::number(threshold));
    const int p = 100 * (threshold - this->lowerBound) / (this->upperBound - this->lowerBound);
    ui->sliderThreshold->setValue(p);
    this->updateCube();
}

void vtkWindowCube::sliceSliderChanged(int action)
{
    Q_UNUSED(action);
    if (this->cubeOpenWatcher.isRunning() || this->remotePreviewWatcher.isRunning()) {
        const QSignalBlocker blockSpin(ui->spinSlice);
        ui->spinSlice->setValue(ui->sliderSlice->sliderPosition());
        return;
    }

    const int sliderValue = ui->sliderSlice->sliderPosition();
    if (ui->sliderSlice->isSliderDown()) {
        const QSignalBlocker blockSpin(ui->spinSlice);
        ui->spinSlice->setValue(sliderValue);
        const int sliceIndex = sliderValue - 1;
        this->updateRemoteSliceDragFeedback(sliceIndex);
        if (this->tryApplyCachedRemoteSlice(sliceIndex)) {
            this->remoteSliceDebounceTimer.stop();
            return;
        }

        this->pendingRemoteSliceIndex = sliceIndex;
        this->remoteSliceDebounceTimer.start(remoteSliceDebounceDelayMs);
        return;
    }

    ui->spinSlice->setValue(sliderValue);
    // updateSlice is called by spinSlice
}

void vtkWindowCube::sliceSpinChanged(int value)
{
    if (this->cubeOpenWatcher.isRunning() || this->remotePreviewWatcher.isRunning()) {
        const QSignalBlocker blockSlider(ui->sliderSlice);
        ui->sliderSlice->setValue(value);
        return;
    }

    ui->sliderSlice->setValue(value);
    this->remoteSliceDebounceTimer.stop();
    this->updateSlice();
}

void vtkWindowCube::changeLegendWCS()
{
    const int wcs = (ui->actionGalactic->isChecked()
                             ? WCS_GALACTIC
                             : (ui->actionFK5->isChecked() ? WCS_J2000 : WCS_ECLIPTIC));
    this->applyDefaultWcsFormatForSelectedFrame();
    this->invalidateWcsOverlayCache();
    qDebug().noquote()
            << QStringLiteral("[wcs] overlay using selected frame %1")
                       .arg(wcs == WCS_GALACTIC ? u"Galactic"_s
                                                : (wcs == WCS_J2000 ? u"FK5"_s : u"Ecliptic"_s));
    this->updateDataStatePanel();
    this->updateSanityPanel();
    this->requestWcsOverlayRender();
}

void vtkWindowCube::showLUTCustomizer()
{
    if (!this->lutCustomizer) {
        this->lutCustomizer = new LUTCustomizerDialog(this);
        QObject::connect(this->lutCustomizer, &LUTCustomizerDialog::lutUpdated, this,
                         [this]() {
                             // Remember the chosen stretch so reopening the dialog
                             // and colormap swaps keep it.
                             this->m_sliceScaleMode = this->lutCustomizer->scaleMode();
                             this->m_sliceScaleGamma = this->lutCustomizer->scaleGamma();
                             // Adopt the colormap the dialog just applied (it renamed the
                             // slice LUT) so the DISPLAY combo + a later scale change don't
                             // rebuild lutSlice with a stale name.
                             if (this->viewingSlice() && this->lutSlice) {
                                 const std::string nm = this->lutSlice->GetObjectName();
                                 if (!nm.empty())
                                     this->m_activeSliceLutName = QString::fromStdString(nm);
                             }
                             this->renderImage();
                             updateDisplaySectionForActivePane();   // resync the DISPLAY combo
                         });
    }

    this->updateLUTCustomizer();
    this->lutCustomizer->show();
    this->lutCustomizer->raise();
    this->lutCustomizer->activateWindow();
}

void vtkWindowCube::updateLUTCustomizer()
{
    if (!this->lutCustomizer) {
        return;
    }

    if (this->viewingSlice()) {
        this->lutCustomizer->init(
                vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0)),
                this->lutSlice, this->m_sliceScaleMode, this->m_sliceScaleGamma);
    } else {
        this->lutCustomizer->init(
                vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0)),
                this->lutMoment, this->m_sliceScaleMode, this->m_sliceScaleGamma);
    }
}

void vtkWindowCube::showLUTCustomizer3D()
{
    if (!this->lutCustomizer3D) {
        this->lutCustomizer3D = new LUTCustomizerDialog(this);
        QObject::connect(this->lutCustomizer3D, &LUTCustomizerDialog::lutUpdated, this,
                         [this]() {
                             // lutVolume was already updated and rebuilt by the dialog.
                             // Remember the stretch, then re-derive the CTF from the
                             // (already-stretched) member LUT and re-render.
                             this->m_volumeScaleMode = this->lutCustomizer3D->scaleMode();
                             this->m_volumeScaleGamma = this->lutCustomizer3D->scaleGamma();
                             const std::string name = this->lutVolume->GetObjectName();
                             ColorMaps::SetColorTransferFunction(this->lutVolume,
                                                                 this->volumeColorTransferFunction);
                             this->volumeColorTransferFunction->SetObjectName(name);
                             this->m_activeColorMapName = QString::fromStdString(name);
                             // Keep the sidebar combo in sync (without re-triggering applyColorMapByName)
                             if (auto *combo = m_uiAssembler ? m_uiAssembler->lutCombo() : nullptr) {
                                 const QSignalBlocker blocker(combo);
                                 combo->setCurrentText(this->m_activeColorMapName);
                             }
                             if (ui->vtkCube->renderWindow())
                                 ui->vtkCube->renderWindow()->Render();
                             // Linked views: a volume stretch change is part of
                             // the shared colour state.
                             if (this->m_linkViews && !this->m_applyingLinkedUpdate)
                                 this->broadcastLinkedColour();
                         });
    }

    // Always reinitialise with the current cube data and member LUT.
    auto *cubeImage =
            vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (cubeImage && cubeImage->GetNumberOfPoints() > 0) {
        this->lutCustomizer3D->init(cubeImage, this->lutVolume, this->m_volumeScaleMode,
                                    this->m_volumeScaleGamma);
    }
    this->lutCustomizer3D->show();
    this->lutCustomizer3D->raise();
    this->lutCustomizer3D->activateWindow();
}

QString vtkWindowCube::registerAnalysisProduct(const QString &kind, const QString &label,
                                               const QVariantMap &params, QWidget *window,
                                               const QString &stableId, const QString &renderer)
{
    AnalysisProduct p;
    p.productId = stableId;
    p.parentKey = this->datasetPersistKey();
    p.parentLabel = QFileInfo(this->filepath).fileName();
    p.kind = kind;
    p.label = label;
    p.renderer = renderer;
    p.params = params;
    p.sourceDatasetId = this->remoteDatasetId;
    p.sessionId = this->remoteSessionId;
    p.window = window;
    const QString id = ProductRegistry::instance().registerProduct(std::move(p));
    if (this->m_workspaceChrome) {
        // Select the new product first (so Provenance is populated), then auto-expand
        // the Inspector onto Provenance (§4.1.4) — otherwise the tab reveals empty
        // (§3.0b (c)). Also refresh the left rail's product count.
        this->m_workspaceChrome->selectSessionProduct(id);
        this->m_workspaceChrome->revealInspectorTab(2);
        this->m_workspaceChrome->setSessionRailCount(
                ProductRegistry::instance().productsFor(this->datasetPersistKey()).size());
    }
    return id;
}

QWidget *vtkWindowCube::buildFlatLeftDockControls()
{
    const QLatin1StringView fgMuted(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView fg(VisivoTheme::kOnSurface());
    const QLatin1StringView pri(VisivoTheme::kPrimary());
    const QString labelCss =
            QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fgMuted);

    auto *root = new QWidget();
    auto *rootLay = new QVBoxLayout(root);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    const QString hdrCss = QStringLiteral(
            "QToolButton{background:transparent;border:none;color:%1;%2"
            "padding:8px 14px;text-align:left;}")
            .arg(fgMuted, VisivoTheme::sectionHeaderCss());

    // Collapsible flat section: header (▼/▶) + content. A full-width rule is drawn
    // only when @p divider — a section reads from the weight of its header, so the
    // collapsible ▶ sections (INTERACTION / SLICE ANIMATION) carry no line (§3.0c).
    auto addSection = [&](const QString &title, bool expanded, bool divider = true,
                          QToolButton **outHdr = nullptr) -> QVBoxLayout * {
        auto *hdr = new QToolButton(root);
        if (outHdr)
            *outHdr = hdr;
        hdr->setCheckable(true);
        hdr->setChecked(expanded);
        hdr->setCursor(Qt::PointingHandCursor);
        hdr->setToolButtonStyle(Qt::ToolButtonTextOnly);
        hdr->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        hdr->setStyleSheet(hdrCss);
        auto setTxt = [hdr, title](bool on) { hdr->setText((on ? u"▼  "_s : u"▶  "_s) + title); };
        setTxt(expanded);
        rootLay->addWidget(hdr);
        auto *content = new QWidget(root);
        auto *cl = new QVBoxLayout(content);
        cl->setContentsMargins(10, 0, 10, 8);
        cl->setSpacing(6);   // rows are fixed 28 → step ≈ 34 (§3.0c density)
        content->setVisible(expanded);
        rootLay->addWidget(content);
        QObject::connect(hdr, &QToolButton::toggled, content,
                         [content, setTxt](bool on) { setTxt(on); content->setVisible(on); });
        if (divider) {
            auto *rule = new QFrame(root);
            rule->setFixedHeight(1);
            rule->setStyleSheet(u"background: rgba(4,138,191,0.20); border:none;"_s);
            rootLay->addWidget(rule);
        }
        return cl;
    };

    // Detach a widget from its current layout before re-hosting it, so no stale
    // layout item is left behind in the (hidden) assembler card.
    auto rehost = [](QWidget *w) {
        if (!w) return;
        if (auto *p = w->parentWidget())
            if (auto *l = p->layout())
                l->removeWidget(w);
    };

    // ── DISPLAY — <active pane> (§5.2): a stacked section. Page 0 = the 3D controls
    // (unchanged). Page 1 = the 2D/Moment pane page driving the active Pane2DView. The
    // header text + current page follow the active pane via activePaneChanged.
    QToolButton *dispHdr = nullptr;
    QVBoxLayout *disp = addSection(u"DISPLAY — 3D"_s, true, true, &dispHdr);
    m_displayHeaderBtn = dispHdr;
    auto *dispStack = new QStackedWidget(root);
    m_displayStack = dispStack;
    disp->addWidget(dispStack);
    auto *page3d = new QWidget(dispStack);
    auto *p3 = new QVBoxLayout(page3d);
    p3->setContentsMargins(0, 0, 0, 0);
    p3->setSpacing(6);
    dispStack->addWidget(page3d);   // index 0
    QVBoxLayout *disp3d = p3;       // the 3D widgets below go into the 3D PAGE, not `disp`
    SegmentedToggle *modeCombo = m_uiAssembler->renderModeCombo();
    if (modeCombo) {
        rehost(modeCombo);
        modeCombo->setMinimumWidth(0);   // let segments share the narrow dock, never overflow
        modeCombo->setParent(root);
        disp3d->addWidget(modeCombo);
    }
    QWidget *volRow = nullptr;
    if (auto *v = m_uiAssembler->volumeModeCombo()) {
        volRow = new QWidget();
        auto *h = new QHBoxLayout(volRow);
        h->setContentsMargins(0, 0, 0, 0);
        rehost(v);
        v->setMinimumWidth(0);
        v->setParent(volRow);
        h->addWidget(v, 1);
        disp3d->addWidget(volRow);
    }
    {   // Threshold: label + mono value, then the slider full-width.
        auto *r = new QWidget();
        auto *h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *l = new QLabel(u"Threshold"_s, r);
        l->setStyleSheet(labelCss);
        h->addWidget(l, 1);
        if (ui->lineThreshold) {
            rehost(ui->lineThreshold);
            ui->lineThreshold->setParent(r);
            // Room for the full value ("0.188071" = 8 chars, allow ~11).
            ui->lineThreshold->setMinimumWidth(96);
            ui->lineThreshold->setMaximumWidth(140);
            ui->lineThreshold->setAlignment(Qt::AlignRight);
            // Compact field (§3.0c): 26 px tall, mono 12, right-aligned value.
            ui->lineThreshold->setFixedHeight(26);
            ui->lineThreshold->setStyleSheet(
                    u"QLineEdit { font-family:'JetBrains Mono','SF Mono',monospace;"
                    u" font-size:12px; padding:2px 8px; }"_s);
            h->addWidget(ui->lineThreshold, 0);
        }
        r->setFixedHeight(28);   // deterministic row step (§3.0c density)
        disp3d->addWidget(r);
    }
    if (ui->sliderThreshold) {
        rehost(ui->sliderThreshold);
        ui->sliderThreshold->setParent(root);
        // Compact row step (§3.0c): setMaximumHeight alone loses to the QSlider's
        // style-imposed minimumHeight (groove + 14px handle + margin:-5px), so the
        // slider kept rendering ~39 px and dominated the Threshold→slider step.
        // setFixedHeight forces min==max==22.
        ui->sliderThreshold->setFixedHeight(22);
        disp3d->addWidget(ui->sliderThreshold);
    }
    {   // Color map: LUT combo + "Edit" link (no full-width "Advanced…").
        auto *r = new QWidget();
        auto *h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *l = new QLabel(u"Color map"_s, r);
        l->setStyleSheet(labelCss);
        h->addWidget(l, 0);
        if (auto *lut = m_uiAssembler->lutCombo()) {
            rehost(lut);
            // Let the combo shrink in the narrow dock instead of forcing the row
            // wider than the viewport (which clipped the whole content on the right).
            lut->setMinimumWidth(0);
            lut->setMinimumContentsLength(3);
            lut->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            lut->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            // Field height is a theme constant, not a per-widget property: match the
            // threshold line edit (26 px) so the two inputs in this column align (§3.0c).
            lut->setFixedHeight(26);
            lut->setParent(r);
            h->addWidget(lut, 1);
        }
        if (auto *edit = m_uiAssembler->lutEdit3dButton()) {
            rehost(edit);
            edit->setParent(r);
            edit->setText(u"Edit"_s);
            edit->setToolButtonStyle(Qt::ToolButtonTextOnly);
            edit->setStyleSheet(
                    QStringLiteral("QToolButton{background:transparent;border:none;color:%1;"
                                   "font-size:11px;font-weight:600;}")
                            .arg(pri));
            h->addWidget(edit, 0);
        }
        r->setFixedHeight(28);   // deterministic row step (§3.0c density)
        disp3d->addWidget(r);
    }
    QWidget *surfRow = nullptr;
    if (ui->btnCubeColor) {
        surfRow = new QWidget();
        auto *h = new QHBoxLayout(surfRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *l = new QLabel(u"Surface color"_s, surfRow);
        l->setStyleSheet(labelCss);
        h->addWidget(l, 1);
        rehost(ui->btnCubeColor);
        ui->btnCubeColor->setParent(surfRow);
        h->addWidget(ui->btnCubeColor, 0);
        surfRow->setFixedHeight(28);   // deterministic row step (§3.0c density)
        disp3d->addWidget(surfRow);
    }
    // Volume-mode / surface-color rows follow the render mode. Drive off the
    // segmented control's OWN signal — the render-mode combo updates actionVolume
    // under a QSignalBlocker, so actionVolume::toggled would miss these changes.
    if (modeCombo) {
        auto applyVis = [volRow, surfRow](int idx) {
            if (volRow) volRow->setVisible(idx == 1);
            if (surfRow) surfRow->setVisible(idx == 0);
        };
        applyVis(modeCombo->currentIndex());
        QObject::connect(modeCombo, &SegmentedToggle::currentIndexChanged, root,
                         [applyVis](int i) { applyVis(i); });
        // Also follow the View-menu path: the action handlers set the combo under
        // a QSignalBlocker, so the combo signal alone would miss menu toggles.
        // Each source blocks the other, so wiring both cannot loop.
        if (ui->actionVolume)
            QObject::connect(ui->actionVolume, &QAction::toggled, root,
                             [applyVis](bool on) { applyVis(on ? 1 : 0); });
    }
    // Page 1 (§5.2): the 2D/Moment pane DISPLAY controls, driving the active Pane2DView.
    dispStack->addWidget(buildPaneDisplayPage());   // index 1
    // The DISPLAY section header + which page follow the active pane; keep the header
    // text correct through collapse/expand too (its built-in setTxt would revert it).
    QObject::connect(this, &vtkWindowCube::activePaneChanged, root,
                     [this](int) { updateDisplaySectionForActivePane(); });
    if (m_displayHeaderBtn)
        QObject::connect(m_displayHeaderBtn, &QToolButton::toggled, root,
                         [this](bool) { updateDisplaySectionForActivePane(); });
    updateDisplaySectionForActivePane();

    // ── LAYERS — five fixed visibility rows (stable regardless of load) ───────
    QVBoxLayout *lay = addSection(u"LAYERS"_s, true, /*divider=*/false);
    auto layerCheck = [&](const QString &label, QAction *act, const QString &disabledReason,
                          QWidget *trailing) {
        auto *row = new QWidget();
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *cb = new QCheckBox(label, row);
        cb->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fg));
        if (act) {
            cb->setChecked(act->isChecked());
            cb->setEnabled(act->isEnabled());
            QObject::connect(cb, &QCheckBox::clicked, act, [act]() { act->trigger(); });
            QObject::connect(act, &QAction::toggled, cb,
                             [cb](bool on) { QSignalBlocker b(cb); cb->setChecked(on); });
            QObject::connect(act, &QAction::changed, cb,
                             [cb, act]() { cb->setEnabled(act->isEnabled()); });
        } else {
            cb->setEnabled(false);
            cb->setToolTip(disabledReason);
        }
        h->addWidget(cb, 1);
        if (trailing) { rehost(trailing); trailing->setParent(row); trailing->setMaximumWidth(96); h->addWidget(trailing, 0); }
        lay->addWidget(row);
    };
    // Volume row is custom: it mirrors the render mode (source = the segmented
    // control, which is not blocked), not the exclusive actionVolume directly.
    {
        auto *row = new QWidget();
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *cb = new QCheckBox(u"Volume"_s, row);
        cb->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fg));
        if (modeCombo) {
            cb->setChecked(modeCombo->currentIndex() == 1);
            QObject::connect(cb, &QCheckBox::clicked, modeCombo,
                             [modeCombo](bool on) { modeCombo->setCurrentIndex(on ? 1 : 0); });
            QObject::connect(modeCombo, &SegmentedToggle::currentIndexChanged, cb,
                             [cb](int i) { QSignalBlocker b(cb); cb->setChecked(i == 1); });
        }
        // Follow the View-menu path too (combo is set under a blocker there).
        if (ui->actionVolume)
            QObject::connect(ui->actionVolume, &QAction::toggled, cb,
                             [cb](bool on) { QSignalBlocker b(cb); cb->setChecked(on); });
        h->addWidget(cb, 1);
        lay->addWidget(row);
    }
    layerCheck(u"Cutting plane"_s, actionShowCuttingPlane, QString(),
               m_cuttingPlaneOpacitySlider);
    layerCheck(u"Contours"_s, nullptr, u"No contours computed for this cube yet."_s, nullptr);
    layerCheck(u"Catalogue sources"_s, actionShowCatalogueOverlay, QString(), nullptr);
    layerCheck(u"3D WCS axes"_s, actionShow3dWcsAxes, QString(), nullptr);

    // ── INTERACTION ▶ (collapsed) ─────────────────────────────────────────────
    QVBoxLayout *inter = addSection(u"INTERACTION"_s, false, /*divider=*/false);
    {
        auto *cb = new QCheckBox(u"Pick spectrum on plane click"_s);
        cb->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fg));
        if (actionPickSpectrum3d) {
            cb->setChecked(actionPickSpectrum3d->isChecked());
            QObject::connect(cb, &QCheckBox::clicked, actionPickSpectrum3d,
                             [this]() { actionPickSpectrum3d->trigger(); });
            QObject::connect(actionPickSpectrum3d, &QAction::toggled, cb,
                             [cb](bool on) { QSignalBlocker b(cb); cb->setChecked(on); });
        } else {
            cb->setEnabled(false);
        }
        inter->addWidget(cb);
    }

    // ── SLICE ANIMATION ▶ (collapsed) ─────────────────────────────────────────
    QVBoxLayout *anim = addSection(u"SLICE ANIMATION"_s, false, /*divider=*/false);
    {
        auto *ph = new QLabel(u"Use the channel scrubber below the 2D view."_s);
        ph->setWordWrap(true);
        ph->setStyleSheet(labelCss);
        anim->addWidget(ph);
    }

    rootLay->addStretch(1);   // stretch at the BOTTOM — sections stay flex:none

    // Instrumentation (measure, don't eyeball): if the content's minimum width
    // exceeds the dock viewport, the right side clips (h-scroll is off).
    QTimer::singleShot(0, root, [root]() {
        QWidget *vp = root->parentWidget();
        qDebug().noquote() << QStringLiteral(
                "[leftdock] content minWidth=%1  width=%2  viewport=%3  %4")
                .arg(root->minimumSizeHint().width())
                .arg(root->width())
                .arg(vp ? vp->width() : -1)
                .arg(vp && root->minimumSizeHint().width() <= vp->width()
                             ? u"OK (fits)"_s : u"OVERFLOW → clips"_s);
    });
    return root;
}

// Central-percentile clip range (CARTA-style): pct=99.9 keeps the central 99.9% of finite
// values, i.e. clip at the (100-pct)/2 and 100-(100-pct)/2 percentiles. pct=100 → min/max.
static bool percentileClipRange(vtkImageData *img, double pct, double &lo, double &hi)
{
    vtkDataArray *sc = img ? img->GetPointData()->GetScalars() : nullptr;
    if (!sc)
        return false;
    std::vector<double> v;
    const vtkIdType n = sc->GetNumberOfTuples();
    v.reserve(static_cast<size_t>(n));
    for (vtkIdType i = 0; i < n; ++i) {
        const double x = sc->GetComponent(i, 0);
        if (std::isfinite(x))
            v.push_back(x);
    }
    if (v.size() < 4)
        return false;
    std::sort(v.begin(), v.end());
    const double tail = std::clamp((100.0 - pct) / 2.0, 0.0, 50.0) / 100.0;
    auto at = [&](double f) {
        const double idx = f * static_cast<double>(v.size() - 1);
        const size_t i0 = static_cast<size_t>(idx);
        const size_t i1 = std::min(i0 + 1, v.size() - 1);
        const double frac = idx - static_cast<double>(i0);
        return v[i0] * (1.0 - frac) + v[i1] * frac;
    };
    lo = at(tail);
    hi = at(1.0 - tail);
    if (!(hi > lo)) {
        lo = v.front();
        hi = v.back();
    }
    return hi > lo;
}

Pane2DView *vtkWindowCube::activePane2DView() const
{
    if (m_activePane < 0 || m_activePane > 3)
        return nullptr;
    if (m_slots[m_activePane].view.kind != ViewId::Product)
        return nullptr;
    return m_paneViews[m_activePane];
}

// ── §5.4 follow-up: DISPLAY-page dispatch (Product pane vs 2D Slice) ─────────────
bool vtkWindowCube::activePaneIsSlice() const
{
    return m_activePane >= 0 && m_activePane < 4
            && m_slots[m_activePane].view.kind == ViewId::Slice2D;
}

vtkImageData *vtkWindowCube::activeDisplayImageData() const
{
    if (Pane2DView *pv = activePane2DView())
        return pv->imageData();
    if (activePaneIsSlice() && this->remoteSliceDisplaySource)
        return vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
    return nullptr;
}

QString vtkWindowCube::activeDisplayColorMapName() const
{
    if (Pane2DView *pv = activePane2DView())
        return pv->colorMapName();
    if (activePaneIsSlice())
        return this->m_activeSliceLutName;
    return QString();
}

LutScale::Mode vtkWindowCube::activeDisplayScale() const
{
    if (Pane2DView *pv = activePane2DView())
        return pv->scale();
    if (activePaneIsSlice())
        return this->m_sliceScaleMode;
    return LutScale::Mode::Linear;
}

double vtkWindowCube::activeDisplayRangeLo() const
{
    if (Pane2DView *pv = activePane2DView())
        return pv->rangeMin();
    if (activePaneIsSlice() && this->lutSlice)
        return this->lutSlice->GetTableRange()[0];
    return 0.;
}

double vtkWindowCube::activeDisplayRangeHi() const
{
    if (Pane2DView *pv = activePane2DView())
        return pv->rangeMax();
    if (activePaneIsSlice() && this->lutSlice)
        return this->lutSlice->GetTableRange()[1];
    return 0.;
}

void vtkWindowCube::setActiveDisplayColorMap(const QString &name)
{
    if (Pane2DView *pv = activePane2DView()) {
        pv->setColorMapName(name);
        propagatePaneLutWithinWindow(m_activePane);
        return;
    }
    if (activePaneIsSlice()) {
        applySliceLutByName(name);   // rebuilds lutSlice; renders if the slice is on screen
        if (this->sliceWin)
            this->sliceWin->Render();
    }
}

void vtkWindowCube::setActiveDisplayScale(LutScale::Mode mode)
{
    if (Pane2DView *pv = activePane2DView()) {
        pv->setScale(mode);
        propagatePaneLutWithinWindow(m_activePane);
        return;
    }
    if (activePaneIsSlice()) {
        this->m_sliceScaleMode = mode;
        applySliceLutByName(this->m_activeSliceLutName);   // re-apply the stretch at range
        if (this->sliceWin)
            this->sliceWin->Render();
    }
}

void vtkWindowCube::setActiveDisplayRange(double lo, double hi)
{
    if (Pane2DView *pv = activePane2DView()) {
        pv->setRange(lo, hi);
        propagatePaneLutWithinWindow(m_activePane);
        return;
    }
    if (activePaneIsSlice() && this->lutSlice) {
        if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo)
            return;
        this->lutSlice->SetTableRange(lo, hi);
        this->lutSlice->Build();
        this->layout2dPaneFor(/*slice=*/true, /*force=*/false);
        if (this->sliceWin)
            this->sliceWin->Render();
    }
}

void vtkWindowCube::showActiveDisplayLUTEditor()
{
    if (activePane2DView()) {
        showPaneLUTCustomizer();
        return;
    }
    if (activePaneIsSlice())
        showLUTCustomizer();   // the existing slice/3D customizer, already targets lutSlice
}

QWidget *vtkWindowCube::buildPaneDisplayPage()
{
    const QLatin1StringView fgMuted(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView pri(VisivoTheme::kPrimary());
    const QString labelCss =
            QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fgMuted);

    auto *page = new QWidget();
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    // Color map + Edit (Edit → the active pane's OWN lookup table, §5.2).
    {
        auto *r = new QWidget();
        auto *h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *l = new QLabel(u"Color map"_s, r);
        l->setStyleSheet(labelCss);
        h->addWidget(l, 0);
        m_paneLutCombo = new QComboBox(r);
        for (const auto &nm : ColorMaps::GetColorMapNames()) {
            const QString qn = QString::fromStdString(nm);
            m_paneLutCombo->addItem(QIcon(CubeUiAssembler::buildLutPreview(qn, 48, 12)), qn);
        }
        m_paneLutCombo->setMinimumWidth(0);
        m_paneLutCombo->setMinimumContentsLength(3);
        m_paneLutCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_paneLutCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_paneLutCombo->setFixedHeight(26);
        h->addWidget(m_paneLutCombo, 1);
        auto *edit = new QToolButton(r);
        edit->setText(u"Edit"_s);
        edit->setToolButtonStyle(Qt::ToolButtonTextOnly);
        edit->setCursor(Qt::PointingHandCursor);
        edit->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;"
                                           "color:%1;font-size:11px;font-weight:600;}")
                                    .arg(pri));
        h->addWidget(edit, 0);
        r->setFixedHeight(28);
        lay->addWidget(r);
        QObject::connect(m_paneLutCombo, &QComboBox::currentTextChanged, this,
                         [this](const QString &name) {
                             if (m_applyingPaneDisplay)
                                 return;
                             setActiveDisplayColorMap(name);
                         });
        QObject::connect(edit, &QToolButton::clicked, this,
                         [this]() { this->showActiveDisplayLUTEditor(); });
    }

    // Scale (2D Slice only): linear / log / asinh.
    {
        m_paneScaleRow = new QWidget();
        auto *h = new QHBoxLayout(m_paneScaleRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *l = new QLabel(u"Scale"_s, m_paneScaleRow);
        l->setStyleSheet(labelCss);
        h->addWidget(l, 0);
        m_paneScaleSeg = new SegmentedToggle(m_paneScaleRow);
        m_paneScaleSeg->addItem(u"Linear"_s);
        m_paneScaleSeg->addItem(u"Log"_s);
        m_paneScaleSeg->addItem(u"Sqrt"_s);   // LutScale has no Asinh; Sqrt is the DS9/CARTA stretch
        m_paneScaleSeg->setMinimumWidth(0);
        h->addWidget(m_paneScaleSeg, 1);
        m_paneScaleRow->setFixedHeight(28);
        lay->addWidget(m_paneScaleRow);
        QObject::connect(m_paneScaleSeg, &SegmentedToggle::currentIndexChanged, this,
                         [this](int i) {
                             if (m_applyingPaneDisplay)
                                 return;
                             const LutScale::Mode m = (i == 1) ? LutScale::Mode::Log
                                                     : (i == 2) ? LutScale::Mode::Sqrt
                                                                : LutScale::Mode::Linear;
                             setActiveDisplayScale(m);
                         });
    }

    // Symmetric about 0 (Moment only) — default on for velocity (mom1).
    {
        m_paneSymmetricRow = new QWidget();
        auto *h = new QHBoxLayout(m_paneSymmetricRow);
        h->setContentsMargins(0, 0, 0, 0);
        m_paneSymmetric = new QCheckBox(u"Symmetric about 0"_s, m_paneSymmetricRow);
        m_paneSymmetric->setStyleSheet(labelCss);
        // Only meaningful for a signed velocity field (Moment 1): force the colour range to
        // -v..+v so the diverging map puts systemic velocity (0) at the centre and shows
        // blueshift/redshift symmetrically. Meaningless for intensity (Moment 0) or width
        // (Moment 2), which are ≥ 0 — the row is hidden there.
        m_paneSymmetric->setToolTip(u"Centre the colour range on 0 (±max) so a velocity "
                                    u"field shows blueshift and redshift symmetrically."_s);
        h->addWidget(m_paneSymmetric, 1);
        m_paneSymmetricRow->setFixedHeight(28);
        lay->addWidget(m_paneSymmetricRow);
        QObject::connect(m_paneSymmetric, &QCheckBox::toggled, this, [this](bool on) {
            if (m_applyingPaneDisplay)
                return;
            Pane2DView *pv = activePane2DView();
            if (!pv)
                return;
            if (on) {
                const double m = std::max(std::fabs(pv->rangeMin()), std::fabs(pv->rangeMax()));
                pv->setRange(-m, m);
            }
            propagatePaneLutWithinWindow(m_activePane);
        });
    }

    // Range: CARTA percentile presets + manual clip min/max (§5.2, user-chosen).
    {
        auto *presets = new QWidget();
        auto *pg = new QGridLayout(presets);
        pg->setContentsMargins(0, 0, 0, 0);
        pg->setHorizontalSpacing(4);
        pg->setVerticalSpacing(4);
        const struct {
            const char16_t *txt;
            double pct;
        } kPresets[] = { { u"90", 90. },      { u"95", 95. },       { u"99", 99. },
                         { u"99.5", 99.5 },   { u"99.9", 99.9 },    { u"99.95", 99.95 },
                         { u"99.99", 99.99 }, { u"100", 100. } };
        int col = 0, rowN = 0;
        for (const auto &pr : kPresets) {
            auto *b = new QToolButton(presets);
            b->setText(QString::fromUtf16(pr.txt));
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral(
                    "QToolButton{background:%1;border:1px solid %2;border-radius:4px;"
                    "color:%3;font-size:10px;padding:3px 4px;}QToolButton:hover{border-color:%4;}")
                                     .arg(QLatin1StringView(VisivoTheme::kSurface()),
                                          QLatin1StringView(VisivoTheme::kOutline()),
                                          QLatin1StringView(VisivoTheme::kOnSurfaceVariant()), pri));
            pg->addWidget(b, rowN, col);
            if (++col == 4) {
                col = 0;
                ++rowN;
            }
            const double pct = pr.pct;
            QObject::connect(b, &QToolButton::clicked, this, [this, pct]() {
                vtkImageData *img = activeDisplayImageData();
                if (!img)
                    return;
                double lo = 0., hi = 0.;
                if (!percentileClipRange(img, pct, lo, hi))
                    return;
                if (m_paneSymmetricRow && m_paneSymmetricRow->isVisible() && m_paneSymmetric
                    && m_paneSymmetric->isChecked()) {
                    const double m = std::max(std::fabs(lo), std::fabs(hi));
                    lo = -m;
                    hi = m;
                }
                setActiveDisplayRange(lo, hi);
            });
        }
        lay->addWidget(presets);
    }
    {
        auto *r = new QWidget();
        auto *h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        auto *l = new QLabel(u"Clip"_s, r);
        l->setStyleSheet(labelCss);
        h->addWidget(l, 0);
        auto mkSpin = [&]() {
            auto *s = new QDoubleSpinBox(r);
            s->setRange(-1e30, 1e30);
            s->setDecimals(6);   // keep precision; the width fix below is the size policy
            s->setButtonSymbols(QAbstractSpinBox::NoButtons);
            s->setFixedHeight(26);
            // A ±1e30 range at N decimals gives a HUGE minimumSizeHint (space for a
            // 38-char value), so two side-by-side overflow the 272 px dock and clip the
            // whole page. Ignore the width hint (like the colormap combo) so the stretch
            // factor compresses both to fit; the value scrolls inside the field.
            s->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            s->setMinimumWidth(0);
            s->setStyleSheet(u"QDoubleSpinBox{font-family:'JetBrains Mono','SF Mono',monospace;"
                             u"font-size:11px;padding:2px 6px;}"_s);
            return s;
        };
        m_paneRangeMin = mkSpin();
        m_paneRangeMax = mkSpin();
        h->addWidget(m_paneRangeMin, 1);
        h->addWidget(m_paneRangeMax, 1);
        r->setFixedHeight(28);
        lay->addWidget(r);
        QObject::connect(m_paneRangeMin, &QDoubleSpinBox::editingFinished, this,
                         [this]() { applyPaneDisplayRange(true); });
        QObject::connect(m_paneRangeMax, &QDoubleSpinBox::editingFinished, this,
                         [this]() { applyPaneDisplayRange(false); });
    }
    return page;
}

void vtkWindowCube::applyPaneDisplayRange(bool /*fromMin*/)
{
    if (m_applyingPaneDisplay)
        return;
    if (!m_paneRangeMin || !m_paneRangeMax)
        return;
    double lo = m_paneRangeMin->value(), hi = m_paneRangeMax->value();
    if (m_paneSymmetricRow && m_paneSymmetricRow->isVisible() && m_paneSymmetric
        && m_paneSymmetric->isChecked()) {
        const double m = std::max(std::fabs(lo), std::fabs(hi));
        lo = -m;
        hi = m;
    }
    setActiveDisplayRange(lo, hi);
}

void vtkWindowCube::showPaneLUTCustomizer()
{
    Pane2DView *pv = activePane2DView();
    if (!pv)
        return;
    // Dedicated dialog instance (NOT the shared 3D/slice one) with a single lutUpdated
    // slot; the target pane is captured so Apply edits the pane that OPENED the dialog,
    // not whatever is active later (§5.2, review).
    if (!m_paneLutCustomizer) {
        m_paneLutCustomizer = new LUTCustomizerDialog(this);
        QObject::connect(m_paneLutCustomizer, &LUTCustomizerDialog::lutUpdated, this, [this]() {
            Pane2DView *p = m_paneLutTarget;
            if (!p || !p->lookupTable())
                return;
            // The dialog mutated the pane's OWN lut in place (name via ObjectName, range,
            // stretch). Adopt name+scale+gamma so getters/Linked aren't stale, then re-fit.
            p->adoptLutState(QString::fromStdString(p->lookupTable()->GetObjectName()),
                             m_paneLutCustomizer->scaleMode(), m_paneLutCustomizer->scaleGamma());
            // find the source slot for propagation
            for (int i = 0; i < 4; ++i)
                if (m_paneViews[i] == p) {
                    propagatePaneLutWithinWindow(i);
                    break;
                }
            updateDisplaySectionForActivePane();
        });
    }
    m_paneLutTarget = pv;
    m_paneLutCustomizer->init(pv->imageData(), pv->lookupTable(), pv->scale(), pv->gamma());
    m_paneLutCustomizer->show();
    m_paneLutCustomizer->raise();
    m_paneLutCustomizer->activateWindow();
}

void vtkWindowCube::updateDisplaySectionForActivePane()
{
    if (!m_displayStack || !m_displayHeaderBtn)
        return;
    const ViewId kind =
            (m_activePane >= 0 && m_activePane < 4) ? m_slots[m_activePane].view.kind : ViewId::None;
    const bool is3d = (kind == ViewId::View3D);
    // Page 1 (the per-pane controls) drives EITHER a Product pane (its own Pane2DView) or
    // the 2D Slice (the shared lutSlice/m_sliceScaleMode) via the activeDisplay* dispatch.
    const bool isPane = (kind == ViewId::Product || kind == ViewId::Slice2D);

    m_displayStack->setCurrentIndex(is3d ? 0 : 1);
    if (auto *p1 = m_displayStack->widget(1))
        p1->setEnabled(isPane);

    // Header text `DISPLAY — <active pane title>` (pane name in primary is handled by the
    // section style; keep it simple text here). The built-in setTxt would revert this on
    // toggle, so this runs on toggle too.
    const bool hasActive = (m_activePane >= 0 && m_activePane < 4);
    QString title = is3d ? u"3D VIEW"_s
                  : (kind == ViewId::Slice2D)  ? u"2D SLICE"_s
                  : (kind == ViewId::Product)  ? viewTitle(m_slots[m_activePane].view).toUpper()
                  : (kind == ViewId::Spectrum) ? u"SPECTRUM"_s
                  : hasActive                  ? u"NO PRODUCT"_s   // a None "empty" pane is active
                                               : u"NO PANE SELECTED"_s;
    const bool open = m_displayHeaderBtn->isChecked();
    m_displayHeaderBtn->setText((open ? u"▼  "_s : u"▶  "_s) + u"DISPLAY — "_s + title);

    if (!isPane)
        return;

    // Repopulate the pane page from the active display target (Product pane or 2D Slice)
    // via the activeDisplay* dispatch; guard the two-way binding.
    QSignalBlocker bMin(m_paneRangeMin), bMax(m_paneRangeMax), bSym(m_paneSymmetric),
            bScale(m_paneScaleSeg), bLut(m_paneLutCombo);
    m_applyingPaneDisplay = true;
    if (m_paneScaleRow)
        m_paneScaleRow->setVisible(kind == ViewId::Slice2D);   // Scale: 2D Slice only
    // Symmetric-about-0 only makes sense for a SIGNED velocity field (Moment 1). Intensity
    // (Moment 0) and width (Moment 2) are ≥ 0, so hide it there — showing it on Moment 0
    // reads as a no-op control.
    bool velocityMoment = false;
    if (kind == ViewId::Product) {
        const QString pid = m_slots[m_activePane].view.productId;
        auto it = m_products2d.constFind(pid);
        velocityMoment = (it != m_products2d.constEnd() && it->order == 1);
    }
    if (m_paneSymmetricRow)
        m_paneSymmetricRow->setVisible(velocityMoment);   // Symmetric: velocity Moment only
    const QString cmap = activeDisplayColorMapName();
    if (m_paneLutCombo && !cmap.isEmpty())
        m_paneLutCombo->setCurrentText(cmap);
    if (m_paneRangeMin)
        m_paneRangeMin->setValue(activeDisplayRangeLo());
    if (m_paneRangeMax)
        m_paneRangeMax->setValue(activeDisplayRangeHi());
    if (m_paneScaleSeg) {
        const LutScale::Mode sc = activeDisplayScale();
        m_paneScaleSeg->setCurrentIndex(sc == LutScale::Mode::Log    ? 1
                                        : sc == LutScale::Mode::Sqrt ? 2
                                                                     : 0);
    }
    m_applyingPaneDisplay = false;
}

void vtkWindowCube::propagatePaneLutWithinWindow(int srcSlot)
{
    // §5.5: with Linked on, a pane's LUT change follows only panes showing the SAME
    // product (same kind AND productId) — never across kinds, never moment→slice. This
    // is within-window and separate from the cross-window broadcastLinkedColour path.
    if (!m_linkViews || m_applyingPaneDisplay || srcSlot < 0 || srcSlot > 3)
        return;
    Pane2DView *src = m_paneViews[srcSlot];
    if (!src || m_slots[srcSlot].view.kind != ViewId::Product)
        return;
    const QString pid = m_slots[srcSlot].view.productId;
    QScopedValueRollback<bool> guard(m_applyingPaneDisplay, true);
    for (int j = 0; j < 4; ++j) {
        if (j == srcSlot || !m_paneViews[j])
            continue;
        if (m_slots[j].view.kind != ViewId::Product || m_slots[j].view.productId != pid)
            continue;
        m_paneViews[j]->applyLut(src->colorMapName(), src->scale(), src->gamma(), src->invert());
        m_paneViews[j]->setRange(src->rangeMin(), src->rangeMax());
    }
}

void vtkWindowCube::propagatePaneRangeWithinWindow(int srcSlot)
{
    // §5.5: cheap path used on a LIVE window/level drag — only the RANGE follows the
    // same-product peers (no colormap rebuild), so a linked drag doesn't re-Build LUTs
    // on every mouse move. Colormap/scale/invert use the full path (lutChanged).
    if (!m_linkViews || m_applyingPaneDisplay || srcSlot < 0 || srcSlot > 3)
        return;
    Pane2DView *src = m_paneViews[srcSlot];
    if (!src || m_slots[srcSlot].view.kind != ViewId::Product)
        return;
    const QString pid = m_slots[srcSlot].view.productId;
    QScopedValueRollback<bool> guard(m_applyingPaneDisplay, true);
    for (int j = 0; j < 4; ++j)
        if (j != srcSlot && m_paneViews[j] && m_slots[j].view.kind == ViewId::Product
            && m_slots[j].view.productId == pid)
            m_paneViews[j]->setRange(src->rangeMin(), src->rangeMax());
}

void vtkWindowCube::setActivePane(int pane)
{
    if (pane < 0 || pane > 3)
        return;
    const bool changed = (m_activePane != pane);
    m_activePane = pane;
    refreshPaneActiveStyles();   // also re-gates interactors (§5.1)
    if (changed)
        emit activePaneChanged(pane);   // §5.2: DISPLAY dock follows the active pane
}

void vtkWindowCube::activateSessionView(const QString &viewTag, const QString &productId)
{
    // Resolve the Session-tree row to a pane view. A dataset root (both empty) has no
    // pane — leave the selection alone.
    using namespace Qt::StringLiterals;
    PaneView target;
    if (viewTag == u"view3d"_s)
        target = { ViewId::View3D, {} };
    else if (viewTag == u"slice2d"_s)
        target = { ViewId::Slice2D, {} };
    else if (!productId.isEmpty() && m_products2d.contains(productId))
        // Only a pane-renderable 2-D product (a moment map, cached in m_products2d) can be
        // mounted. PV / baseline / channel-map / noise rows open their OWN windows — for
        // those, a single-click selection just drives Provenance; double-click raises the
        // window (onActivated). Mounting them would replace the pane with a bogus empty
        // state, since they have no image in m_products2d.
        target = { ViewId::Product, productId };
    else
        return;

    // Already on a visible pane → just make it active (tree-select = pane-select).
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].frame && m_slots[i].frame->isVisible() && m_slots[i].view == target) {
            setActivePane(i);
            return;
        }

    // Not shown → mount it in the active pane (fall back to pane 0 if that's hidden).
    int slot = (m_activePane >= 0 && m_activePane < 4 && m_slots[m_activePane].frame
                && m_slots[m_activePane].frame->isVisible())
                       ? m_activePane
                       : 0;
    m_slots[slot].view = target;
    clearViewFromOtherSlots(slot);
    mountView(slot);
    syncPick3d2d();
    refreshPaneChrome();
    layout2dPane(true);
    setActivePane(slot);
}

void vtkWindowCube::syncSessionTreeToActivePane()
{
    // Highlight the Session-tree row matching whatever the active pane now shows. Called
    // both on activePaneChanged (index change) AND from refreshPaneChrome, since a view
    // swap / 3D⇄2D toggle mutates m_slots[active].view WITHOUT changing the index — the
    // tree highlight would otherwise stay on the previous row.
    if (!m_workspaceChrome || m_activePane < 0 || m_activePane > 3)
        return;
    const PaneView &v = m_slots[m_activePane].view;
    QString tag, pid;
    if (v.kind == ViewId::View3D)
        tag = u"view3d"_s;
    else if (v.kind == ViewId::Slice2D)
        tag = u"slice2d"_s;
    else if (v.kind == ViewId::Product)
        pid = v.productId;
    else
        return;   // None / Spectrum: no tree row — leave the current highlight
    m_workspaceChrome->selectSessionRowForView(tag, pid);
}

void vtkWindowCube::refreshPaneInteractors()
{
    // §5.1: the "idle pane takes no input" rule is enforced by the event filter, which
    // swallows every mouse event on a NON-active pane (see eventFilter). We do NOT flip
    // the VTK interactor's Enabled flag — QVTKOpenGLNativeWidget drives its render loop
    // through the interactor, so disabling it paints the pane black. This hook stays so
    // the activation slot has one place to notify the (currently no-op) views.
    for (int i = 0; i < 4; ++i)
        if (m_paneViews[i])
            m_paneViews[i]->setInteractive(i == m_activePane);
}

void vtkWindowCube::refreshPaneActiveStyles()
{
    const QLatin1StringView pri(VisivoTheme::kPrimary());
    // §3.2: the active-pane distinction must be VISIBLE, not documented. The active
    // pane carries the brand-blue border + a filled brand-blue dot; every other pane
    // gets a near-flush #1E2427 border and a #3A4247 dot, so "tools act here" reads
    // straight off the pane rather than the changelog.
    static constexpr QLatin1StringView kInactiveBorder("#1E2427");
    static constexpr QLatin1StringView kInactiveDot("#3A4247");
    for (int i = 0; i < 4; ++i) {
        const bool active = (m_activePane == i);
        if (m_slots[i].frame)
            m_slots[i].frame->setStyleSheet(
                    QStringLiteral("QFrame#paneFrame { border: 1px solid %1; }")
                            .arg(active ? pri : kInactiveBorder));
        if (m_slots[i].dot)
            m_slots[i].dot->setStyleSheet(
                    QStringLiteral("font-size:9px; color:%1;").arg(active ? pri : kInactiveDot));
        // §5.1: the active pane's header row carries an 8% primary wash.
        if (m_slots[i].header)
            m_slots[i].header->setStyleSheet(
                    active ? QStringLiteral("QWidget#paneHeader { background: rgba(4,138,191,0.08); }")
                           : QStringLiteral("QWidget#paneHeader { background: transparent; }"));
    }
    refreshPaneCursors();
    refreshPaneInteractors();   // §5.1: keep the live interactor in step with the active pane
}

void vtkWindowCube::syncPick3d2d()
{
    // The toolbar 3D/2D toggle only exists at 1 pane; when the header dropdown moves
    // slot 0 to a 2D-family view keep the toggle in step (View3D → 3D, else → 2D).
    if (m_paneCount != 1 || !m_pick3d2d)
        return;
    QSignalBlocker b(m_pick3d2d);
    m_pick3d2d->setCurrentIndex(m_slots[0].view.kind == ViewId::View3D ? 0 : 1);
}

void vtkWindowCube::refreshPaneCursors()
{
    // A 2D pane that is not active still renders, but interactive tools (probe /
    // region / PV) act only on the active pane. Signal that with the cursor: the
    // active 2D data area gets a crosshair, every non-active 2D area keeps the plain
    // arrow — so hovering a secondary pane never *looks* like it will take a tool (§3.2).
    for (int i = 0; i < 4; ++i) {
        const ViewId k = m_slots[i].view.kind;
        if (k == ViewId::Slice2D) {
            if (QWidget *w = viewWidget(m_slots[i].view))
                w->setCursor((m_activePane == i) ? Qt::CrossCursor : Qt::ArrowCursor);
        } else if (k == ViewId::Product && m_paneViews[i] && m_paneViews[i]->widget()) {
            // Display-only: always the plain arrow, never a crosshair — a moment pane
            // takes no probe/region/PV (those act on the 2-D Slice). §3.2 B3.
            m_paneViews[i]->widget()->setCursor(Qt::ArrowCursor);
        }
    }
}

bool vtkWindowCube::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type et = event->type();
    if (et == QEvent::MouseButtonPress) {
        for (int i = 0; i < 4; ++i)
            if (m_slots[i].header && watched == m_slots[i].header) {
                setActivePane(i);
                break;
            }
    }
    // §5.1: ONE activation + idle-input rule for every render pane (2D Slice, local 3D,
    // and each Product Pane2DView). The VTK interactor stays enabled — QVTKOpenGLNativeWidget
    // drives its render through it, so we must NOT disable it. Instead the container
    // swallows the mouse events of an IDLE pane here. The first press on a non-active pane
    // activates it and is swallowed (so it doesn't also begin a drag / window-level); the
    // rest of that gesture is swallowed too; the next gesture on the now-active pane passes
    // through. Hover / wheel on an idle pane do nothing.
    QWidget *rw = qobject_cast<QWidget *>(watched);
    int renderSlot = -1;
    if (rw)
        for (int i = 0; i < 4; ++i) {
            const ViewId k = m_slots[i].view.kind;
            QWidget *paneW = (k == ViewId::Slice2D) ? static_cast<QWidget *>(ui->vtkImage)
                    : (k == ViewId::View3D)         ? static_cast<QWidget *>(ui->vtkCube)
                    : (k == ViewId::Product && m_paneViews[i]) ? m_paneViews[i]->widget()
                                                               : nullptr;
            if (paneW && paneW == rw && m_slots[i].frame && m_slots[i].frame->isVisible()) {
                renderSlot = i;
                break;
            }
        }
    if (renderSlot >= 0) {
        const bool nonActive = (m_activePane != renderSlot);
        if (et == QEvent::MouseButtonPress) {
            if (nonActive) {
                setActivePane(renderSlot);
                m_paneActivationGesture = rw;   // swallow the rest of this gesture
                return true;
            }
            m_paneActivationGesture = nullptr;  // fresh gesture on the active pane
        } else if (et == QEvent::MouseMove) {
            if (nonActive)
                return true;                    // no hover interaction on an idle pane
        } else if (et == QEvent::MouseButtonRelease || et == QEvent::MouseButtonDblClick) {
            if (nonActive || m_paneActivationGesture == rw)
                return true;                    // idle input, or tail of the activating gesture
        } else if (et == QEvent::Wheel) {
            if (nonActive)
                return true;                    // no wheel-zoom on an idle pane
        }
    }

    // Deferred re-fit on resize (the render window hasn't adopted the new size yet).
    if (et == QEvent::Resize) {
        if (watched == ui->vtkImage || watched == m_vtkMoment) {
            const bool slice = (watched == ui->vtkImage);
            QTimer::singleShot(0, this, [this, slice]() { this->layout2dPaneFor(slice, false); });
        } else {
            for (int i = 0; i < 4; ++i)
                if (m_paneViews[i] && watched == m_paneViews[i]->widget()) {
                    QTimer::singleShot(0, this, [this, i]() {
                        if (m_paneViews[i])
                            m_paneViews[i]->fit(false);
                    });
                    break;
                }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

QWidget *vtkWindowCube::viewWidget(const PaneView &v) const
{
    switch (v.kind) {
    case ViewId::View3D:   return m_renderStack;
    case ViewId::Slice2D:  return ui->vtkImage;
    case ViewId::Spectrum: return probePlotWidget;   // ProfileWidget (null until first probe)
    // §3.2 B3: a Product has NO shared singleton widget — each pane owns a Pane2DView
    // (see ensureSlotProductView / mountView). Returning null here means the dedup +
    // collision checks that key on "same render widget" never fire for products, so two
    // panes CAN show two moments (or the same moment) at once.
    case ViewId::Product:  return nullptr;
    default:               return nullptr;
    }
}

Pane2DView *vtkWindowCube::ensureSlotProductView(int slot)
{
    if (slot < 0 || slot > 3)
        return nullptr;
    if (!m_paneViews[slot]) {
        auto *pv = new Pane2DView(this);
        // Decoupled WCS: hand the view the dataset-level conversion closures (all const,
        // channel-independent). For the app's LINEAR/CAR cutouts the per-axis form is
        // exact; a gnomonic celestial cube would need the paired conversion (noted).
        Pane2DView::WcsContext ctx;
        ctx.voxelToWcs = [this](int axis, double v, bool *ok) {
            return this->remoteVoxelToWcs(axis, v, ok);
        };
        ctx.hasCelestial = [this]() { return this->remoteHasCelestialAxes(); };
        ctx.axisTitle = [this](int axis) { return this->remoteOverlayAxisTitle(axis); };
        ctx.formatCoord = [this](int axis, double value) {
            return this->formatRemoteOverlayCoordinate(axis, value);
        };
        pv->setWcsContext(std::move(ctx));
        pv->setWcsAxesVisible(this->showWcsAxes);
        if (pv->widget())
            pv->widget()->installEventFilter(this);   // click-to-activate + resize-fit
        // §5.2: a window/level DRAG on the pane writes its LUT range and emits this —
        // mirror it into the dock Range fields (drag + dock are one state), and propagate
        // to Linked same-product panes. Only for the ACTIVE pane, and never re-entrantly.
        QObject::connect(pv, &Pane2DView::rangeChanged, this, [this, slot](double lo, double hi) {
            if (m_applyingPaneDisplay)
                return;
            if (m_activePane == slot && m_paneRangeMin && m_paneRangeMax) {
                QSignalBlocker a(m_paneRangeMin), b(m_paneRangeMax);
                m_paneRangeMin->setValue(lo);
                m_paneRangeMax->setValue(hi);
            }
            propagatePaneRangeWithinWindow(slot);   // range-only: cheap on a live drag
        });
        QObject::connect(pv, &Pane2DView::lutChanged, this, [this, slot]() {
            if (m_applyingPaneDisplay)
                return;
            propagatePaneLutWithinWindow(slot);   // colormap/scale/invert → same-product panes
        });
        m_paneViews[slot] = pv;
    }
    return m_paneViews[slot];
}

QString vtkWindowCube::viewTitle(const PaneView &v) const
{
    switch (v.kind) {
    case ViewId::View3D:   return u"3D View"_s;
    case ViewId::Slice2D:  return u"2D Slice"_s;
    case ViewId::Spectrum: return u"Spectrum"_s;
    case ViewId::Product: {
        // The label is IDENTICAL to the Session Data tree row (§3.2 corollary 3):
        // registry → cache → derive "Moment N" from the id (an uncomputed seed slot
        // reads "Moment 0", never a meaningless "Product"). Only a truly unassigned
        // slot (kind None) says "Pick a view".
        const AnalysisProduct p = ProductRegistry::instance().product(v.productId);
        if (!p.label.isEmpty())
            return p.label;
        auto it = m_products2d.constFind(v.productId);
        if (it != m_products2d.constEnd())
            return it->label;
        const int at = v.productId.indexOf(u'@');
        if (v.productId.startsWith(u"moment"_s) && at > 6)
            return u"Moment "_s + v.productId.mid(6, at - 6);
        return u"Pick a view"_s;
    }
    default:               return u"Pick a view"_s;
    }
}

QList<vtkWindowCube::PaneView> vtkWindowCube::availableViews() const
{
    // The header dropdown offers the fixed views + every RENDERABLE product
    // (renderer != "") in GENERATION order (registry insertion order), so "the one I
    // just computed" is predictably last — same source + label as the Session Data
    // tree (§3.2: one list, three consumers).
    QList<PaneView> out;
    out << PaneView{ ViewId::View3D, {} } << PaneView{ ViewId::Slice2D, {} };
    const auto products = ProductRegistry::instance().productsFor(this->datasetPersistKey());
    for (const AnalysisProduct &p : products)
        // Renderable AND actually cached in THIS window (m_products2d holds the pixels).
        // The registry is session-wide, so a same-dataset sibling window could register a
        // product this window never cached — offering it would mount a blank Product pane
        //. viewHasData() is the same predicate mountView/activateSessionView use.
        if (!p.renderer.isEmpty() && m_products2d.contains(p.productId))
            out << PaneView{ ViewId::Product, p.productId };
    out << PaneView{ ViewId::Spectrum, {} };
    return out;
}

bool vtkWindowCube::paneStacked() const
{
    // Explicit user override wins over the width heuristic (§3.1).
    if (m_paneDirection == PaneDirection::Rows)
        return true;
    if (m_paneDirection == PaneDirection::Cols)
        return false;
    // Auto: measured on the pane CONTAINER (excludes the docks), in logical px.
    const int w = m_paneSplitter ? m_paneSplitter->width() : 0;
    return w > 0 ? (w < 900) : (this->width() < 1100);   // fallback before first layout
}

bool vtkWindowCube::viewHasData(const PaneView &v) const
{
    switch (v.kind) {
    case ViewId::View3D:   return true;   // the window exists only once a cube is open
    case ViewId::Slice2D:  return true;   // the slice pipeline is always present
    case ViewId::Spectrum: return this->probePlotWidget != nullptr;    // probed?
    case ViewId::Product:  return m_products2d.contains(v.productId);   // computed + cached?
    default:               return false;
    }
}

QWidget *vtkWindowCube::emptyStateWidget(const PaneView &v)
{
    // A pane with no data shows WHAT is missing and the action that fills it (§3.1) —
    // never a degenerate render (fake axes / repeated ticks) that reads as a bug.
    QPointer<QWidget> *slot = (v.kind == ViewId::Product)  ? &m_momentEmptyState
                             : (v.kind == ViewId::Spectrum) ? &m_spectrumEmptyState
                                                            : nullptr;
    if (!slot)
        return nullptr;   // View3D / Slice2D always have data
    if (*slot)
        return *slot;

    const QLatin1StringView ink(VisivoTheme::kOnSurfaceVariant());
    auto *w = new QWidget(m_viewParkingLot);
    w->setStyleSheet(u"background:#000004;"_s);   // matches the viewport
    auto *outer = new QVBoxLayout(w);
    outer->addStretch(1);
    auto *title = new QLabel(w);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:13px;").arg(ink));
    outer->addWidget(title);
    auto *sub = new QLabel(w);
    sub->setAlignment(Qt::AlignCenter);
    sub->setWordWrap(true);
    sub->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(ink));
    outer->addWidget(sub);
    if (v.kind == ViewId::Product) {
        title->setText(u"No moment map"_s);
        sub->setText(u"Compute one from Analysis ▸ Moment maps"_s);
        auto *btn = new QPushButton(u"Compute Moment 0"_s, w);
        btn->setCursor(Qt::PointingHandCursor);
        auto *row = new QHBoxLayout();
        row->addStretch(1);
        row->addWidget(btn);
        row->addStretch(1);
        outer->addSpacing(8);
        outer->addLayout(row);
        // setMomentOrder() is the same entry the Analysis ▸ Moment maps action uses:
        // it carries the busy guard, the Computing card state and the failure path
        // (startComputation() alone bypasses them).
        QObject::connect(btn, &QPushButton::clicked, this,
                         [this]() { this->setMomentOrder(0); });
    } else {   // Spectrum
        title->setText(u"No spectrum"_s);
        sub->setText(u"Click a pixel in the 2D view to trace its spectrum"_s);
        // Parity with the moment empty state (§3.2): a first-class action, not just a
        // hint. Arms the same Extract-Spectrum probe mode as Analysis ▸ Extract Spectrum.
        auto *btn = new QPushButton(u"Extract Spectrum…"_s, w);
        btn->setCursor(Qt::PointingHandCursor);
        auto *row = new QHBoxLayout();
        row->addStretch(1);
        row->addWidget(btn);
        row->addStretch(1);
        outer->addSpacing(8);
        outer->addLayout(row);
        QObject::connect(btn, &QPushButton::clicked, this, [this]() {
            if (ui->actionExtractSpectrum && !ui->actionExtractSpectrum->isChecked())
                ui->actionExtractSpectrum->trigger();   // enter probe mode → click a pixel
        });
    }
    outer->addStretch(1);
    *slot = w;
    return w;
}

QWidget *vtkWindowCube::noProductStateWidget(int slot)
{
    // Neutral placeholder for a None pane (e.g. the 3rd/4th pane at 4-up before any
    // product exists). Per-slot cached: several panes can be None simultaneously, and a
    // widget can only live in one host. No compute button — products are added from
    // Analysis and auto-placed here, or activated from Session Data (user request).
    if (slot < 0 || slot > 3)
        return nullptr;
    if (m_noProductState[slot])
        return m_noProductState[slot];
    const QLatin1StringView ink(VisivoTheme::kOnSurfaceVariant());
    auto *w = new QWidget(m_viewParkingLot);
    w->setStyleSheet(u"background:#000004;"_s);   // matches the viewport
    auto *outer = new QVBoxLayout(w);
    outer->addStretch(1);
    auto *title = new QLabel(u"No product available"_s, w);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:13px;").arg(ink));
    outer->addWidget(title);
    auto *sub = new QLabel(u"Compute a product, or pick a view from the pane header ▾"_s, w);
    sub->setAlignment(Qt::AlignCenter);
    sub->setWordWrap(true);
    sub->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(ink));
    outer->addWidget(sub);
    outer->addStretch(1);
    m_noProductState[slot] = w;
    return w;
}

void vtkWindowCube::clearViewFromOtherSlots(int keepSlot)
{
    // One slot per RENDER WIDGET. In B2 all products share the single moment pipeline
    // (m_vtkMoment), so two product panes would fight over one widget → deduplicate by
    // widget identity, which also keeps 3D (one GL context) and the 2D slice single.
    // In B3 each product gets its own Pane2DView widget, so distinct products no longer
    // collide here and true duplication (Moment 0 ∥ Moment 1) becomes possible.
    if (keepSlot < 0 || keepSlot > 3)
        return;
    QWidget *w = viewWidget(m_slots[keepSlot].view);
    if (!w)
        return;   // None / not-yet-created Spectrum: nothing to deduplicate
    for (int j = 0; j < 4; ++j)
        if (j != keepSlot && viewWidget(m_slots[j].view) == w)
            m_slots[j].view = {};
}

// A robust, symmetric display range for a signed field (a velocity moment): centre on
// the median, half-width = 4·(1.4826·MAD). MAD has a 50% breakdown point, so the sparse
// noise-spike pixels (±channel-velocity garbage) never inflate the scale the way min/max
// does. Returns false on degenerate data so the caller keeps the true min/max (§3.2).
static bool robustSymmetricMomentRange(vtkImageData *img, double &lo, double &hi)
{
    vtkDataArray *sc = img ? img->GetPointData()->GetScalars() : nullptr;
    if (!sc)
        return false;
    std::vector<double> v;
    const vtkIdType n = sc->GetNumberOfTuples();
    v.reserve(static_cast<size_t>(n));
    for (vtkIdType i = 0; i < n; ++i) {
        const double x = sc->GetComponent(i, 0);
        if (std::isfinite(x))
            v.push_back(x);
    }
    if (v.size() < 8)
        return false;
    auto medianOf = [](std::vector<double> &a) {
        std::sort(a.begin(), a.end());
        const size_t m = a.size();
        return (m % 2) ? a[m / 2] : 0.5 * (a[m / 2 - 1] + a[m / 2]);
    };
    const double med = medianOf(v);
    std::vector<double> dev;
    dev.reserve(v.size());
    for (double x : v)
        dev.push_back(std::fabs(x - med));
    const double mad = medianOf(dev);
    const double sigma = 1.4826 * mad;
    if (!(sigma > 0.0))
        return false;
    const double half = 4.0 * sigma;
    lo = med - half;
    hi = med + half;
    return true;
}

void vtkWindowCube::loadProductIntoMomentPipeline(const QString &productId, bool force)
{
    // B2: the single moment pipeline (m_vtkMoment) shows one product at a time; make it
    // hold THIS product's cached image before a Product pane mounts the shared widget.
    // Data + camera + overlay-cache only — the CALLER fits + renders (layout2dPane /
    // layout2dPaneFor), so this stays safe to call from inside mountView() before the
    // widget is on screen and never double-renders during a layout pass. `force` re-loads
    // even when the id is unchanged (a same-order recompute produced a NEW image).
    if (!force && m_momentPipelineProductId == productId)
        return;   // already showing it — avoid redundant reload in the mount loops
    auto it = m_products2d.constFind(productId);
    if (it == m_products2d.constEnd() || !it->imageData)
        return;
    m_momentPipelineProductId = productId;
    this->momentDisplaySource->SetOutput(it->imageData);
    this->momentColors->SetInputData(it->imageData);
    // Colormap + range come from the product (by kind): a velocity moment gets a
    // diverging map and a robust range, intensity keeps its sequential map + min/max.
    this->m_activeMomentLutName = it->lutName;
    LutScale::build(this->lutMoment, it->lutName.toStdString(), it->scaleMode,
                    it->imageRange[0], it->imageRange[1], it->gamma);
    // Velocity: sparse noise-spike pixels fall OUTSIDE the robust range → paint them
    // transparent (like NaN) so they don't dominate; intensity leaves this off so a
    // bright peak above the range is never hidden.
    if (it->clampOutliers) {
        this->lutMoment->SetUseAboveRangeColor(1);
        this->lutMoment->SetAboveRangeColor(0., 0., 0., 0.);
        this->lutMoment->SetUseBelowRangeColor(1);
        this->lutMoment->SetBelowRangeColor(0., 0., 0., 0.);
    } else {
        this->lutMoment->SetUseAboveRangeColor(0);
        this->lutMoment->SetUseBelowRangeColor(0);
    }
    this->lutMoment->Build();   // append the special (above/below/NaN) colors into the table
    this->lutMoment->Modified();
    if (auto *r = this->momentWin->GetRenderers()->GetFirstRenderer()) {
        r->ResetCamera();
        r->ResetCameraClippingRange();
    }
    // Force the overlay to recompute against the new image on the next render.
    this->lastMomentOverlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
                                             std::numeric_limits<double>::quiet_NaN(),
                                             std::numeric_limits<double>::quiet_NaN(),
                                             std::numeric_limits<double>::quiet_NaN() };
    this->lastMomentOverlayViewportSize = { -1, -1 };
}

int vtkWindowCube::firstVisibleSlot() const
{
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].frame && m_slots[i].frame->isVisible())
            return i;
    return 0;
}

void vtkWindowCube::buildPaneSlots()
{
    const QLatin1StringView fg(VisivoTheme::kOnSurface());
    const QLatin1StringView fgMuted(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView pri(VisivoTheme::kPrimary());

    m_viewParkingLot = new QWidget(this);
    m_viewParkingLot->setObjectName(u"viewParkingLot"_s);
    m_viewParkingLot->hide();
    new QVBoxLayout(m_viewParkingLot);   // a layout so parked views can be re-parented cleanly

    auto makeSlot = [&](int i) {
        auto *frame = new QFrame();
        frame->setObjectName(u"paneFrame"_s);
        auto *v = new QVBoxLayout(frame);
        v->setContentsMargins(1, 1, 1, 1);
        v->setSpacing(0);

        auto *hdr = new QWidget(frame);
        hdr->setObjectName(u"paneHeader"_s);
        hdr->setFixedHeight(34);
        auto *h = new QHBoxLayout(hdr);
        h->setContentsMargins(10, 0, 6, 0);
        h->setSpacing(8);

        auto *dot = new QLabel(u"●"_s, hdr);
        dot->setStyleSheet(QStringLiteral("font-size:9px; color:%1;").arg(pri));
        h->addWidget(dot);

        auto *title = new QToolButton(hdr);   // plain text @1 pane, dropdown @>1
        title->setToolButtonStyle(Qt::ToolButtonTextOnly);
        title->setPopupMode(QToolButton::InstantPopup);
        title->setCursor(Qt::PointingHandCursor);
        title->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:none;color:%1;font-family:'Lato';"
                "font-size:11px;font-weight:700;} QToolButton::menu-indicator{image:none;}")
                                     .arg(fg));
        h->addWidget(title);

        auto *qual = new QLabel(QString(), hdr);
        qual->setStyleSheet(QStringLiteral("color:%1; font-family:'JetBrains Mono','SF Mono',"
                                           "monospace; font-size:10px;").arg(fgMuted));
        h->addWidget(qual);

        // Per-view control slot (Auto/Local/Remote or Slice/Moment get re-hosted here).
        auto *ctrlHost = new QWidget(hdr);
        auto *ctrl = new QHBoxLayout(ctrlHost);
        ctrl->setContentsMargins(0, 0, 0, 0);
        ctrl->setSpacing(6);
        h->addWidget(ctrlHost);

        h->addStretch(1);

        auto *maxBtn = new QToolButton(hdr);
        maxBtn->setText(u"⤢"_s);
        maxBtn->setCursor(Qt::PointingHandCursor);
        maxBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        maxBtn->setToolTip(u"Maximize pane"_s);
        maxBtn->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;"
                                             "color:%1;font-size:13px;}QToolButton:hover{color:%2;}")
                                      .arg(fgMuted, pri));
        QObject::connect(maxBtn, &QToolButton::clicked, this, [this, i]() {
            // ⤢ toggles in-workspace maximize (§3.2): first click maximizes THIS pane,
            // clicking the ⤡ on the maximized pane (or Esc) restores the layout.
            if (m_maximizedPane < 0)
                this->maximizePane(i);
            else
                this->restorePaneMaximize();
        });
        h->addWidget(maxBtn);

        v->addWidget(hdr);

        auto *viewHost = new QWidget(frame);
        auto *vhl = new QHBoxLayout(viewHost);
        vhl->setContentsMargins(0, 0, 0, 0);
        vhl->setSpacing(0);
        v->addWidget(viewHost, 1);

        hdr->installEventFilter(this);

        m_slots[i].frame = frame;
        m_slots[i].header = hdr;
        m_slots[i].dot = dot;
        m_slots[i].titleBtn = title;
        m_slots[i].qualifier = qual;
        m_slots[i].viewHost = viewHost;
        m_slots[i].viewHostLayout = vhl;
        m_slots[i].controls = ctrl;
        m_slots[i].maxBtn = maxBtn;
        m_slots[i].view = {};
    };
    for (int i = 0; i < 4; ++i)
        makeSlot(i);

    // Esc restores the layout when a pane is maximized (§3.2). Enabled ONLY while
    // maximized, so an idle Esc never steals the key from a focused child / dialog.
    m_escRestoreShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_escRestoreShortcut->setEnabled(false);
    QObject::connect(m_escRestoreShortcut, &QShortcut::activated, this, [this]() {
        if (m_maximizedPane >= 0)
            restorePaneMaximize();
    });

    // Park the raw views FIRST — setupWorkspaceDocks() put m_renderStack + ui->vtkImage
    // straight into m_paneSplitter; parking (reparent) removes them so the splitter is
    // empty before the columns go in (otherwise the stretch factors below land on the
    // wrong indices). applyPaneLayout() mounts the visible ones back.
    if (!m_vtkMoment)
        m_vtkMoment = new QVTKOpenGLNativeWidget();
    for (QWidget *w : { static_cast<QWidget *>(ui->vtkImage), static_cast<QWidget *>(m_vtkMoment) })
        if (w)
            w->installEventFilter(this);   // either 2-D host can drive layout2dPane
    // §5.1: the local 3-D viewport body must also click-to-activate (its interactor is
    // gated in refreshPaneInteractors), so a body click re-activates 3D — not just the
    // header. ClickFocus so no hover focus.
    if (ui->vtkCube) {
        ui->vtkCube->installEventFilter(this);
        ui->vtkCube->setFocusPolicy(Qt::ClickFocus);
    }
    auto park = [this](QWidget *w) {
        if (!w)
            return;
        w->setParent(m_viewParkingLot);
        if (auto *l = m_viewParkingLot->layout())
            l->addWidget(w);
        w->hide();
    };
    park(m_renderStack);
    park(ui->vtkImage);
    park(m_vtkMoment);

    // Nested columns: left = slot0/slot2, right = slot1/slot3 (spec §3.2). m_paneSplitter
    // is now empty (raw views parked), so the columns land at indices 0 and 1.
    m_leftColumn = new QSplitter(Qt::Vertical);
    m_leftColumn->setChildrenCollapsible(false);
    m_leftColumn->addWidget(m_slots[0].frame);
    m_leftColumn->addWidget(m_slots[2].frame);
    m_leftColumn->setStretchFactor(0, 1);
    m_leftColumn->setStretchFactor(1, 1);
    m_rightColumn = new QSplitter(Qt::Vertical);
    m_rightColumn->setChildrenCollapsible(false);
    m_rightColumn->addWidget(m_slots[1].frame);
    m_rightColumn->addWidget(m_slots[3].frame);
    m_rightColumn->setStretchFactor(0, 1);
    m_rightColumn->setStretchFactor(1, 1);
    m_paneSplitter->addWidget(m_leftColumn);
    m_paneSplitter->addWidget(m_rightColumn);
    m_paneSplitter->setStretchFactor(0, 1);
    m_paneSplitter->setStretchFactor(1, 1);
}

void vtkWindowCube::mountView(int slot)
{
    if (slot < 0 || slot > 3 || !m_slots[slot].viewHostLayout || !m_viewParkingLot)
        return;
    // First empty this host — park whatever is currently in it (a previous view, or
    // a stale occupant after a swap) so a slot never holds two views.
    auto *vhl = m_slots[slot].viewHostLayout;
    while (vhl->count()) {
        QLayoutItem *it = vhl->takeAt(0);
        if (QWidget *old = it ? it->widget() : nullptr) {
            old->setParent(m_viewParkingLot);
            if (auto *pl = m_viewParkingLot->layout())
                pl->addWidget(old);
            old->hide();
        }
        delete it;
    }
    const PaneView v = m_slots[slot].view;
    // §3.2 B3: a Product pane renders into its OWN Pane2DView (display-only), configured
    // from the shared per-product cache — so two panes can show two different moments,
    // or the same moment, without a shared-pipeline collision.
    Pane2DView *pv = nullptr;
    QWidget *w = nullptr;
    if (v.kind == ViewId::Product && viewHasData(v)) {
        pv = ensureSlotProductView(slot);
        const Product2D &p = m_products2d[v.productId];
        if (!pv->lutInitialized()) {
            // First mount: seed the default LUT BY KIND (§5.3) from the product cache.
            pv->setImageData(p.imageData, p.imageRange[0], p.imageRange[1]);
            pv->setColorMap(p.lutName, p.scaleMode, p.gamma, p.clampOutliers);
        } else {
            // Remount / recompute: keep THIS pane's own LUT + range, swap only the
            // pixels (§5.3 / acceptance #7 — two panes on the same product may differ).
            pv->setImageData(p.imageData, pv->rangeMin(), pv->rangeMax());
        }
        w = pv->widget();
    } else {
        // No data yet → the empty state (what's missing + the action); fixed views →
        // their singleton widget (§3.1).
        w = viewHasData(v) ? viewWidget(v) : emptyStateWidget(v);
    }
    if (!w && v.isNone())
        w = noProductStateWidget(slot);   // neutral "No product available" placeholder
    if (!w)
        return;   // truly nothing to show — leave the host empty
    if (auto *p = w->parentWidget())
        if (auto *l = p->layout())
            l->removeWidget(w);
    w->setParent(m_slots[slot].viewHost);
    vhl->addWidget(w, 1);
    w->show();
    // Fit the Pane2DView against its now-visible host (letterbox + colorbar + overlay).
    // Only once the frame is actually visible, so a layout pass that mounts before
    // showing frames doesn't fit into a stale size (applyPaneLayout re-fits after).
    if (pv && m_slots[slot].frame && m_slots[slot].frame->isVisible())
        pv->fit(true);
}

void vtkWindowCube::parkView(int slot)
{
    if (slot < 0 || slot > 3 || !m_viewParkingLot)
        return;
    // Product panes have no shared singleton (viewWidget → null); their widget is the
    // slot's own Pane2DView. Park whichever it is.
    QWidget *w = viewWidget(m_slots[slot].view);
    if (!w && m_slots[slot].view.kind == ViewId::Product && m_paneViews[slot])
        w = m_paneViews[slot]->widget();
    if (!w || !m_slots[slot].viewHost)
        return;
    // Only park a view that actually lives in THIS slot's host — never steal one
    // that a visible slot currently owns (its parent is the source of truth).
    if (w->parentWidget() != m_slots[slot].viewHost)
        return;
    if (auto *l = m_slots[slot].viewHost->layout())
        l->removeWidget(w);
    w->setParent(m_viewParkingLot);
    if (auto *l = m_viewParkingLot->layout())
        l->addWidget(w);
    w->hide();
}

void vtkWindowCube::assignViews(const std::array<bool, 4> &visible)
{
    // Fills the empty VISIBLE slots (not slots 0..panes-1 — stacked 2-pane is {0,2}).
    auto onScreen = [&](const PaneView &id) {
        for (int j = 0; j < 4; ++j)
            if (visible[j] && m_slots[j].view == id)
                return true;
        return false;
    };
    if (visible[0] && m_slots[0].view.isNone())
        m_slots[0].view = { ViewId::View3D, {} };
    // Auto-fill only the cube's INTRINSIC views (3D on slot 0, the 2D Slice next). Moment /
    // Spectrum are PRODUCTS — never pre-seed them: an empty pane at 4-up shows the neutral
    // "No product available" state, and a computed product fills the first free pane (or
    // waits in Session Data for manual activation). This avoids a wall of "compute me"
    // placeholders the user didn't ask for (user request).
    const std::array<PaneView, 1> order{ PaneView{ ViewId::Slice2D, {} } };
    for (int i = 0; i < 4; ++i) {
        if (!visible[i] || !m_slots[i].view.isNone())
            continue;
        for (const PaneView &cand : order) {
            if (!onScreen(cand)) {
                m_slots[i].view = cand;
                break;
            }
        }
        // Nothing left → the pane stays None → "No product available" empty state.
    }
    // A view must live in exactly one slot: clear any duplicate a hidden slot still
    // holds before mounting (else parkView could later steal a visible slot's widget).
    for (int i = 0; i < 4; ++i)
        if (visible[i] && !m_slots[i].view.isNone())
            clearViewFromOtherSlots(i);
    for (int i = 0; i < 4; ++i)
        if (visible[i])
            mountView(i);
}

void vtkWindowCube::refreshPaneChrome()
{
    // Move the per-view controls into whichever slot currently shows that view.
    auto detach = [](QWidget *w) {
        if (!w)
            return;
        if (w->parentWidget() && w->parentWidget()->layout())
            w->parentWidget()->layout()->removeWidget(w);
        // Hide it too: with the dropdown available at 1 pane a view may have no header
        // control, and a merely-removed (not hidden) widget lingers at stale geometry.
        // The rehost branches below call show() on whichever control they re-add.
        w->hide();
    };
    // §3.2: the Slice/Moment segmented toggle is retired — the header dropdown is the
    // single 2D view selector (a computed moment is just another product in the list).
    // Keep it detached + hidden so its stale wiring never surfaces in a header.
    if (SegmentedToggle *combo2d = m_uiAssembler ? m_uiAssembler->combo2dMode() : nullptr)
        detach(combo2d);
    detach(m_renderModeSeg);
    const QList<PaneView> choices = availableViews();
    for (int i = 0; i < 4; ++i) {
        auto &s = m_slots[i];
        const bool shown = s.frame && s.frame->isVisible();
        // Title is a dropdown at EVERY pane count (§3.2): the most direct gesture is to
        // act on the pane you want to change. The " ▾" chevron makes it read as a menu;
        // it lists the fixed views + every renderable product; picking a view shown
        // elsewhere swaps the two slots, otherwise it moves the view here.
        if (s.titleBtn) {
            s.titleBtn->setPopupMode(QToolButton::InstantPopup);
            s.titleBtn->setText(viewTitle(s.view) + u"  ▾"_s);
            QMenu *menu = new QMenu(s.titleBtn);
            menu->setToolTipsVisible(true);
            for (const PaneView &cand : choices) {
                // A choice already on screen is checked and carries the pane number that
                // shows it (§3.2) — the dropdown answers "where is this?" at a glance.
                int atPane = -1;
                for (int p = 0; p < 4; ++p)
                    if (m_slots[p].frame && m_slots[p].frame->isVisible()
                        && m_slots[p].view == cand) {
                        atPane = p;
                        break;
                    }
                // Collision (§3.2): a DIFFERENT view already using cand's render widget on
                // a visible pane, while THIS pane doesn't own that widget. In B2 all moment
                // products share the single pipeline, so a second pane can't show another
                // moment — picking it would EVICT the on-screen one (a masked loss, not a
                // swap). Disable + explain, rather than silently steal. Fixed views have
                // distinct widgets so they never collide; relocating the SAME product
                // (exact match, handled by the swap below) stays allowed. B3 gives each
                // product its own Pane2DView widget → no collision → these re-enable.
                QWidget *wCand = viewWidget(cand);
                int collideJ = -1;
                if (wCand && viewWidget(s.view) != wCand)
                    for (int j = 0; j < 4; ++j)
                        if (j != i && m_slots[j].frame && m_slots[j].frame->isVisible()
                            && viewWidget(m_slots[j].view) == wCand
                            && m_slots[j].view != cand) {
                            collideJ = j;
                            break;
                        }
                QString label = viewTitle(cand);
                if (atPane >= 0)
                    label += QStringLiteral("   ·  P%1").arg(atPane + 1);
                else if (collideJ >= 0)
                    label += QStringLiteral("   ·  P%1").arg(collideJ + 1);  // the blocking pane
                // Duplication hint: a product shown in ANOTHER pane is NOT checked here
                // (this pane doesn't show it) and reads "· P1" — clicking it DUPLICATES it
                // into this pane (each Product gets its own Pane2DView, §3.2 B3). Only the
                // view THIS pane currently shows is checked.
                if (atPane >= 0 && atPane != i && cand.kind == ViewId::Product)
                    label += u"  (duplicate here)"_s;
                QAction *a = menu->addAction(label);
                a->setCheckable(true);
                a->setChecked(cand == s.view);
                if (collideJ >= 0) {
                    // Disabled + the pane number reads as "can't — a moment is in P3"
                    // without needing the tooltip (which stays as the full explanation).
                    a->setEnabled(false);
                    a->setToolTip(QStringLiteral("%1 is already shown in P%2 — one moment map "
                                                 "at a time")
                                          .arg(viewTitle(m_slots[collideJ].view))
                                          .arg(collideJ + 1));
                    continue;
                }
                QObject::connect(a, &QAction::triggered, this, [this, i, cand]() {
                    // A FIXED view (single shared widget) already shown elsewhere RELOCATES
                    // via a swap, so neither pane goes blank. A Product has a per-slot
                    // Pane2DView (viewWidget → null), so it can DUPLICATE — fall through to
                    // the set path and give this pane its own copy (§3.2 B3).
                    if (viewWidget(cand)) {
                        for (int j = 0; j < 4; ++j) {
                            if (j != i && m_slots[j].view == cand) {
                                std::swap(m_slots[i].view, m_slots[j].view);
                                mountView(i);
                                mountView(j);
                                syncPick3d2d();
                                refreshPaneChrome();
                                layout2dPane(true);
                                return;
                            }
                        }
                    }
                    m_slots[i].view = cand;
                    clearViewFromOtherSlots(i);
                    mountView(i);
                    syncPick3d2d();
                    refreshPaneChrome();
                    layout2dPane(true);
                });
            }
            QMenu *old = s.titleBtn->menu();
            s.titleBtn->setMenu(menu);
            if (old)
                old->deleteLater();
        }
        // Qualifier: only the 3D view carries one ("volume") for now.
        if (s.qualifier)
            s.qualifier->setText(s.view.kind == ViewId::View3D ? u"volume"_s : QString());
        // Per-view controls: only the 3D view has one now (Auto/Local/Remote).
        if (shown && s.controls && s.view.kind == ViewId::View3D && m_renderModeSeg) {
            m_renderModeSeg->setParent(s.controls->parentWidget());
            s.controls->addWidget(m_renderModeSeg);
            m_renderModeSeg->show();
        }
    }
    refreshPaneActiveStyles();
    updateDisplaySectionForActivePane();   // §5.2: view of the active pane may have changed
    syncSessionTreeToActivePane();         // reverse sync when the active pane's view changed
}

void vtkWindowCube::setupPaneChrome()
{
    if (!m_paneSplitter)
        return;
    const QLatin1StringView fgMuted(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView pri(VisivoTheme::kPrimary());

    buildPaneSlots();

    // Auto / Local / Remote segmented (spec #11a) — built once, re-hosted into the
    // View3D slot by refreshPaneChrome().
    m_renderModeSeg = new SegmentedToggle();
    m_renderModeSeg->addItem(u"Auto"_s, u"Choose local or remote rendering automatically."_s);
    m_renderModeSeg->addItem(u"Local"_s, u"Render on this machine."_s);
    m_renderModeSeg->addItem(u"Remote"_s, u"Render on the backend GPU and stream it."_s);
    const auto currentIdx = [this]() -> int {
        if (m_actionRenderLocal && m_actionRenderLocal->isChecked()) return 1;
        if (m_actionRenderRemote && m_actionRenderRemote->isChecked()) return 2;
        return 0;
    };
    m_renderModeSeg->setCurrentIndex(currentIdx());
    QObject::connect(m_renderModeSeg, &SegmentedToggle::currentIndexChanged, this, [this](int i) {
        QAction *a = (i == 1) ? m_actionRenderLocal : (i == 2) ? m_actionRenderRemote
                                                               : m_actionRenderAuto;
        if (a && !a->isChecked())
            a->trigger();
    });
    {
        SegmentedToggle *seg = m_renderModeSeg;
        const auto syncIdx = [seg, currentIdx]() {
            QSignalBlocker b(seg);
            seg->setCurrentIndex(currentIdx());
        };
        for (QAction *a : { m_actionRenderAuto, m_actionRenderLocal, m_actionRenderRemote })
            if (a) QObject::connect(a, &QAction::toggled, seg, [syncIdx](bool) { syncIdx(); });
    }
    if (m_renderModeBadge) {   // retire the status-bar badge (superseded)
        this->statusBar()->removeWidget(m_renderModeBadge);
        m_renderModeBadge->hide();
    }

    // ── Centre toolbar (h30): 1/2/4 · 3D/2D · Linked · cursor readout ─────────
    auto *toolbar = new QWidget();
    toolbar->setObjectName(u"paneToolbar"_s);
    toolbar->setFixedHeight(30);
    auto *tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(8, 0, 8, 0);
    tb->setSpacing(8);

    m_paneCountToggle = new SegmentedToggle(toolbar);
    QColor gl(QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()));
    m_paneCountToggle->addItem(VisivoTheme::renderThemeIcon(u":/icons/pane_1.svg"_s, 14, &gl),
                               u"One pane"_s);
    m_paneCountToggle->addItem(VisivoTheme::renderThemeIcon(u":/icons/pane_2.svg"_s, 14, &gl),
                               u"Two panes"_s);
    m_paneCountToggle->addItem(VisivoTheme::renderThemeIcon(u":/icons/pane_4.svg"_s, 14, &gl),
                               u"Four panes"_s);
    m_paneCountToggle->setCurrentIndex(0);   // spec §3.1 default: 1 pane
    m_paneCountToggle->setMaximumWidth(108);
    QObject::connect(m_paneCountToggle, &SegmentedToggle::currentIndexChanged, this,
                     [this](int i) { this->applyPaneLayout(i == 0 ? 1 : (i == 1 ? 2 : 4)); });
    // Second click on the "2" glyph flips the split direction (explicit override,
    // persisted). The heuristic is the default, not a cage (§3.1).
    QObject::connect(m_paneCountToggle, &SegmentedToggle::segmentClicked, this, [this](int idx) {
        if (idx != 1 || m_paneCount != 2)
            return;
        m_paneDirection = paneStacked() ? PaneDirection::Cols : PaneDirection::Rows;
        applyPaneLayout(2);
    });
    tb->addWidget(m_paneCountToggle);

    m_pick3d2d = new SegmentedToggle(toolbar);
    m_pick3d2d->addItem(u"3D"_s);
    m_pick3d2d->addItem(u"2D"_s);
    m_pick3d2d->setMaximumWidth(72);
    m_pick3d2d->setVisible(false);   // only at paneCount==1 (refreshed in applyPaneLayout)
    QObject::connect(m_pick3d2d, &SegmentedToggle::currentIndexChanged, this, [this](int i) {
        if (m_paneCount != 1)
            return;
        m_slots[0].view = (i == 0) ? PaneView{ ViewId::View3D, {} }
                                   : PaneView{ ViewId::Slice2D, {} };
        clearViewFromOtherSlots(0);
        mountView(0);
        refreshPaneChrome();
        layout2dPane(true);
    });
    tb->addWidget(m_pick3d2d);

    auto *linked = new QToolButton(toolbar);
    linked->setText(u" Linked"_s);
    linked->setCheckable(true);
    linked->setCursor(Qt::PointingHandCursor);
    linked->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    {
        QColor c(QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()));
        linked->setIcon(QIcon(VisivoTheme::renderThemeIcon(u":/icons/rail_viz.svg"_s, 13, &c)));
    }
    linked->setStyleSheet(
            QStringLiteral("QToolButton{background:transparent;border:1px solid %1;border-radius:5px;"
                           "color:%2;font-size:11px;padding:2px 8px;}"
                           "QToolButton:checked{color:%3;border-color:%3;}"
                           "QToolButton:disabled{color:%2;}")
                    .arg(QLatin1StringView(VisivoTheme::kOutline()), fgMuted, pri));
    QObject::connect(linked, &QToolButton::toggled, this,
                     [this](bool on) { this->setLinkViews(on); });
    m_paneLinkedBtn = linked;
    tb->addWidget(linked);
    m_linkedQualifier = new QLabel(u"camera · channel · LUT"_s, toolbar);
    m_linkedQualifier->setStyleSheet(QStringLiteral("color:%1; font-family:'JetBrains Mono','SF Mono',"
                                                    "monospace; font-size:10px;").arg(fgMuted));
    tb->addWidget(m_linkedQualifier);
    tb->addStretch(1);

    if (this->hoverReadoutLabel) {
        this->statusBar()->removeWidget(this->hoverReadoutLabel);
        this->hoverReadoutLabel->setParent(toolbar);
        this->hoverReadoutLabel->show();
        tb->addWidget(this->hoverReadoutLabel);
    }

    // ── Central column: [toolbar] above [pane splitter] ──────────────────────
    auto *centralLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    if (centralLayout)
        centralLayout->removeWidget(m_paneSplitter);
    auto *centreCol = new QWidget(ui->centralwidget);
    auto *cv = new QVBoxLayout(centreCol);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(8);
    cv->addWidget(toolbar);
    cv->addWidget(m_paneSplitter, 1);
    if (centralLayout)
        centralLayout->addWidget(centreCol);

    // Spec §3.1 opens at 1 pane (3D). restoreLayoutSettings() may override.
    m_slots[0].view = { ViewId::View3D, {} };
    m_activePane = 0;
    applyPaneLayout(1);
    restoreLayoutSettings();
}

void vtkWindowCube::setupInspector()
{
    if (!this->m_workspaceChrome)
        return;
    InspectorPanel *panel = this->m_workspaceChrome->installInspectorDock();
    if (!panel)
        return;

    // Collapsing/expanding a dock resizes the panes; the gutter fit is size-driven
    // and the QEvent::Resize filter is not reliable when both docks change in the
    // same event-loop turn (§4.1.2).
    connect(this->m_workspaceChrome, &WorkspaceChrome::dockCollapseChanged, this,
            [this]() { QTimer::singleShot(0, this, [this]() { this->layout2dPane(); }); });

    // ── Properties — captioned KV sections (spec #11a: never needs a scrollbar)
    // identity · DATASET · STATISTICS (2D image + 3D cube) · WCS. Rendering
    // controls live in the LEFT dock, not here. The statistics values are the
    // live stat widgets re-hosted from the (parked) info page, so they stay in
    // sync with updateInfoStats().
    panel->clearProperties();
    panel->addPropertyRow(u"Dataset"_s, QFileInfo(this->filepath).fileName());
    panel->addPropertyRow(u"Type"_s, QStringLiteral("CUBE · %1×%2×%3")
                                             .arg(this->remoteDatasetWidth)
                                             .arg(this->remoteDatasetHeight)
                                             .arg(this->remoteDatasetDepth));

    panel->addPropertyCaption(u"DATASET"_s);
    {   // Path elided on the LEFT so the filename + extension always show.
        const QString elided = QFontMetrics(QApplication::font())
                                       .elidedText(this->filepath, Qt::ElideLeft, 180);
        panel->addPropertyRow(u"Path"_s, elided);
    }
    panel->addPropertyRow(u"Session"_s, this->remoteSessionId);
    panel->addPropertyRow(u"Backend"_s, this->remoteBackendUrl);
    panel->addPropertyRow(u"Sanity"_s, this->remoteWcsStatus.isEmpty() ? u"—"_s
                                                                       : this->remoteWcsStatus);

    // Flatten a re-hosted stat widget so it reads as a value, not an input box.
    const QString statCss =
            QStringLiteral("border:none; background:transparent; color:%1;"
                           " font-family:'JetBrains Mono','SF Mono',monospace; font-size:11px;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface()));
    auto statVal = [&](QWidget *w) { if (w) w->setStyleSheet(statCss); return w; };

    panel->addPropertyCaption(u"STATISTICS"_s);
    panel->addPropertyCaption(u"2D image"_s, /*sub=*/true);
    panel->addPropertyRow(u"Min"_s,  statVal(ui->lineImgMin));
    panel->addPropertyRow(u"Max"_s,  statVal(ui->lineImgMax));
    panel->addPropertyRow(u"Mean"_s, statVal(ui->lineImgMean));
    panel->addPropertyRow(u"RMS"_s,  statVal(ui->lineImgRms));
    panel->addPropertyCaption(u"3D cube"_s, /*sub=*/true);
    panel->addPropertyRow(u"Min"_s,  statVal(ui->lineCubeMin));
    panel->addPropertyRow(u"Max"_s,  statVal(ui->lineCubeMax));
    panel->addPropertyRow(u"Mean"_s, statVal(ui->lineCubeMean));
    panel->addPropertyRow(u"RMS"_s,  statVal(ui->lineCubeRms));

    panel->addPropertyCaption(u"WCS"_s);
    panel->addPropertyRow(u"Status"_s, this->remoteWcsStatus.isEmpty() ? u"—"_s
                                                                      : this->remoteWcsStatus);
    panel->addPropertyRow(u"Frame"_s, this->currentWcsFrameLabel());
    panel->addPropertyRow(u"Spectral axis"_s, this->spectralAxisTitle());
    // Design #1d: the view/rendering controls (3D + 2D settings) live in the LEFT
    // column's LAYERS block, NOT in the Inspector — that keeps Properties compact
    // and fills the otherwise-empty Session Data column. Only info/stats stays in
    // Properties.
    // Left dock: the FLAT rebuilt controls (DISPLAY—3D / LAYERS / INTERACTION /
    // SLICE ANIMATION), re-hosting the wired widgets out of the assembler's cards.
    // The old card pages (m_inspectorPage3d/2d/Info) stay parked in the hidden
    // holder — their remaining widgets are used later (2D pane header, stats KV).
    this->m_workspaceChrome->addSessionLayers(this->buildFlatLeftDockControls());

    // Step-0 instrumentation: report (post-layout) whether Properties fits
    // without scrolling, naming the measured widget.
    panel->logPropertiesFit();
    // Re-home the kept-alive Tools page as a HIDDEN child of the inspector (not a
    // free child of the QMainWindow, where it painted at (0,0) over the left dock).
    if (this->m_orphanToolsPage) {
        this->m_orphanToolsPage->setParent(panel);
        this->m_orphanToolsPage->hide();
    }

    // View → "Inspector Panel" expands/collapses the dock (never setVisible, which
    // would hide the rail to 0 px and bypass the collapse state — §4.1).
    if (this->actionShowControlsSidebar) {
        QObject::connect(this->actionShowControlsSidebar, &QAction::toggled, this,
                         [this](bool checked) {
                             if (this->m_workspaceChrome)
                                 this->m_workspaceChrome->setInspectorCollapsed(!checked);
                         });
        QObject::connect(this->m_workspaceChrome, &WorkspaceChrome::dockCollapseChanged, this,
                         [this]() {
                             if (!this->m_workspaceChrome)
                                 return;
                             QSignalBlocker b(this->actionShowControlsSidebar);
                             this->actionShowControlsSidebar->setChecked(
                                     !this->m_workspaceChrome->inspectorCollapsed());
                         });
        // Seed the checkstate from the (already-applied) collapsed default.
        QSignalBlocker b(this->actionShowControlsSidebar);
        this->actionShowControlsSidebar->setChecked(
                !this->m_workspaceChrome->inspectorCollapsed());
    }

    // ── Analysis (tools trigger the existing dialog QActions) ─────────────────
    const auto add = [panel](const QString &grp, const QString &label, QAction *a) {
        if (a) panel->addTool(grp, label, a);
    };
    add(u"Statistics"_s, u"Estimate Noise…"_s, this->actionEstimateNoise);
    add(u"Statistics"_s, u"Slice Histogram…"_s, this->m_actSliceHistogram);
    add(u"Spectral"_s, u"Extract Spectrum"_s, ui->actionExtractSpectrum);
    add(u"Spectral"_s, u"Position–Velocity Diagram…"_s, this->actionExtractPvDiagram);
    add(u"Spectral"_s, u"Line-Width Map…"_s, ui->actionLinewidthMap);
    add(u"Spectral"_s, u"Baseline Subtraction…"_s, ui->actionBaselineSubtract);
    add(u"Spectral"_s, u"Stack Spectral Cubes…"_s, ui->actionStackCubes);
    add(u"Maps"_s, u"Compute Moment…"_s, ui->actionComputeMoment);
    add(u"Maps"_s, u"Channel Maps…"_s, this->m_actChannelMaps);
    add(u"Sources & Kinematics"_s, u"Kinematic Lasso"_s, this->m_actKinematicLasso);
    add(u"Sources & Kinematics"_s, u"Kinematic Model Overlay…"_s, this->m_actKinematicModel);

    // (Pane-layout 1/2/4 + Linked now live in the centre toolbar — setupPaneChrome.)
    // Wire the toolbar Linked chip ↔ m_actLinkViews now that the action exists.
    if (this->m_paneLinkedBtn && this->m_actLinkViews) {
        {
            QSignalBlocker b(this->m_paneLinkedBtn);
            this->m_paneLinkedBtn->setChecked(this->m_actLinkViews->isChecked());
        }
        QObject::connect(this->m_actLinkViews, &QAction::toggled, this->m_paneLinkedBtn,
                         [btn = this->m_paneLinkedBtn](bool on) {
                             if (!btn) return;
                             QSignalBlocker b(btn);
                             btn->setChecked(on);
                         });
    }

    // Belt-and-suspenders: park any stray direct-child widget of the window that
    // is not the central widget / a dock / a toolbar / the menu or status bar —
    // such a widget paints at (0,0) over the docks. Reparent it into the hidden
    // holder so it can never paint. (See vtkWindowCube_Setup.cpp bug-class note.)
    this->parkOrphanDirectChildren();
}

void vtkWindowCube::parkOrphanDirectChildren()
{
    if (!this->m_hiddenPageHolder)
        return;
    const auto children = this->findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
    for (QWidget *w : children) {
        if (w == this->centralWidget() || w == this->m_hiddenPageHolder
            || w == this->menuBar() || w == this->statusBar()
            || qobject_cast<QDockWidget *>(w) || qobject_cast<QToolBar *>(w)
            || w->isWindow())
            continue;
        qWarning().noquote() << QStringLiteral("[workspace] parking orphan direct child: %1 (%2) %3")
                                        .arg(w->objectName(),
                                             QString::fromLatin1(w->metaObject()->className()),
                                             QString::number(w->isVisible()));
        w->setParent(this->m_hiddenPageHolder);
        w->hide();
    }
}

void vtkWindowCube::maximizePane(int slot)
{
    if (slot < 0 || slot > 3 || !m_slots[slot].frame || !m_leftColumn || !m_rightColumn)
        return;
    if (m_maximizedPane >= 0)
        return;   // already maximized — the ⤡ restores instead
    m_preMaxPaneCount = m_paneCount;
    m_preMaxActive = m_activePane;
    m_maximizedPane = slot;
    m_activePane = slot;

    // Show ONLY this slot's frame + its column; the sibling column collapses so the
    // QSplitter gives the survivor the full width, and hiding the sibling frame gives
    // it the full height. No view is parked or re-rendered — pure visibility toggle.
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].frame)
            m_slots[i].frame->setVisible(i == slot);
    const bool inLeft = (slot == 0 || slot == 2);
    m_leftColumn->setVisible(inLeft);
    m_rightColumn->setVisible(!inLeft);

    if (m_slots[slot].maxBtn) {
        m_slots[slot].maxBtn->setText(u"⤡"_s);
        m_slots[slot].maxBtn->setToolTip(u"Restore layout"_s);
    }
    if (m_escRestoreShortcut)
        m_escRestoreShortcut->setEnabled(true);
    refreshPaneChrome();
    const ViewId k = m_slots[slot].view.kind;
    if (k == ViewId::Slice2D)
        layout2dPaneFor(/*slice=*/true, /*force=*/true);
    else if (k == ViewId::Product)
        layout2dPaneFor(/*slice=*/false, /*force=*/true);
}

void vtkWindowCube::restorePaneMaximize()
{
    if (m_maximizedPane < 0)
        return;
    const int wasActive = m_preMaxActive;
    for (auto &s : m_slots)                 // reset every ⤢ glyph (only one was ⤡)
        if (s.maxBtn) {
            s.maxBtn->setText(u"⤢"_s);
            s.maxBtn->setToolTip(u"Maximize pane"_s);
        }
    m_maximizedPane = -1;
    m_activePane = wasActive;
    if (m_escRestoreShortcut)
        m_escRestoreShortcut->setEnabled(false);
    applyPaneLayout(m_preMaxPaneCount);     // re-applies frame/column visibility + fits
}

void vtkWindowCube::applyPaneLayout(int panes)
{
    if (!m_leftColumn || !m_rightColumn)
        return;
    // Any explicit layout change (1/2/4 toggle, restore) leaves maximize mode; clear
    // the flag + glyphs so the ⤢ buttons are consistent with the arrangement below.
    if (m_maximizedPane >= 0) {
        m_maximizedPane = -1;
        for (auto &s : m_slots)
            if (s.maxBtn) {
                s.maxBtn->setText(u"⤢"_s);
                s.maxBtn->setToolTip(u"Maximize pane"_s);
            }
        if (m_escRestoreShortcut)
            m_escRestoreShortcut->setEnabled(false);
    }
    panes = (panes == 1 || panes == 2 || panes == 4) ? panes : 1;

    // 4 → 1 survivor rule (spec §3.4): the ACTIVE pane's view moves into slot 0
    // (swap, so slot 0's old view is kept for when panes grow again — no data lost).
    if (panes == 1 && m_paneCount > 1 && m_activePane != 0)
        std::swap(m_slots[0].view, m_slots[m_activePane].view);

    m_paneCount = panes;
    if (panes == 1)
        m_activePane = 0;

    // 2-pane direction: explicit override or width-derived (paneStacked()).
    const bool stacked = (panes == 2 && paneStacked());
    const std::array<bool, 4> vis =
            panes == 1 ? std::array<bool, 4>{ true, false, false, false }
          : panes == 2 ? (stacked ? std::array<bool, 4>{ true, false, true, false }
                                  : std::array<bool, 4>{ true, true, false, false })
                       : std::array<bool, 4>{ true, true, true, true };
    assignViews(vis);   // fill + mount the visible slots (handles stacked {0,2})

    for (int i = 0; i < 4; ++i) {
        if (!vis[i] && !m_slots[i].view.isNone())
            parkView(i);                       // re-parent off-screen views, keep their state
        if (m_slots[i].frame)
            m_slots[i].frame->setVisible(vis[i]);
    }
    m_leftColumn->setVisible(vis[0] || vis[2]);
    m_rightColumn->setVisible(vis[1] || vis[3]);
    if (!vis[m_activePane])
        setActivePane(firstVisibleSlot());

    // Start every multi-pane split at EQUAL sizes so the grid opens symmetric (the user
    // can drag the dividers from there). Skipped during a saved-layout restore — the
    // stored splitter state is re-applied right after applyPaneLayout() returns. Equal
    // ratio values suffice; QSplitter scales them to the real pixel extent (user request).
    if (!m_restoringLayout) {
        constexpr int kEq = 1 << 20;
        const QList<int> half{ kEq, kEq };
        if (panes == 4) {
            if (m_paneSplitter) m_paneSplitter->setSizes(half);
            if (m_leftColumn) m_leftColumn->setSizes(half);
            if (m_rightColumn) m_rightColumn->setSizes(half);
        } else if (panes == 2) {
            if (stacked) {
                if (m_leftColumn) m_leftColumn->setSizes(half);
            } else if (m_paneSplitter) {
                m_paneSplitter->setSizes(half);
            }
        }
    }

    if (m_pick3d2d) {
        m_pick3d2d->setVisible(panes == 1);
        if (panes == 1) {
            QSignalBlocker b(m_pick3d2d);
            // Any non-3D view (Slice2D / Moment0 / Spectrum) reads as "2D" on the
            // coarse toggle — matches syncPick3d2d() so the dropdown can't desync it.
            m_pick3d2d->setCurrentIndex(m_slots[0].view.kind == ViewId::View3D ? 0 : 1);
        }
    }
    if (m_paneLinkedBtn)
        m_paneLinkedBtn->setEnabled(panes > 1);   // nothing to link with a single pane
    if (m_linkedQualifier)
        m_linkedQualifier->setText(panes == 4 ? u"channel · LUT"_s : u"camera · channel · LUT"_s);
    if (m_paneCountToggle) {
        QSignalBlocker b(m_paneCountToggle);
        m_paneCountToggle->setCurrentIndex(panes == 1 ? 0 : (panes == 2 ? 1 : 2));
        // The "2" glyph shows the current split direction (two rows vs two columns).
        QColor gl(QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()));
        m_paneCountToggle->setItemIcon(
                1, VisivoTheme::renderThemeIcon(
                           stacked ? u":/icons/pane_2_rows.svg"_s : u":/icons/pane_2.svg"_s, 14, &gl));
    }

    refreshPaneChrome();
    // Fit every visible 2-D view — at 4 panes slice AND moment are both on screen,
    // so fitting only the active one (layout2dPane) would leave the other unfitted.
    bool sliceVis = false, momentVis = false;
    for (int i = 0; i < 4; ++i)
        if (vis[i]) {
            if (m_slots[i].view.kind == ViewId::Slice2D)
                sliceVis = true;
            else if (m_slots[i].view.kind == ViewId::Product)
                momentVis = true;
        }
    if (sliceVis)
        layout2dPaneFor(/*slice=*/true, /*force=*/true);
    if (momentVis)
        layout2dPaneFor(/*slice=*/false, /*force=*/true);
    saveLayoutSettings();
}

void vtkWindowCube::saveLayoutSettings()
{
    if (m_restoringLayout)
        return;   // don't overwrite the saved splitter states mid-restore
    // New prefix so no pre-§3.1 install can restore a stale arrangement (§4.3).
    QSettings s;
    s.beginGroup(u"layout_v2/cube"_s);
    s.setValue(u"paneCount"_s, m_paneCount);
    s.setValue(u"paneDirection"_s, static_cast<int>(m_paneDirection));
    // §3.2: a slot is now (kind, productId). Persist both, parallel per slot. Products
    // are session-scoped (ProductRegistry keeps nothing across restarts), so on restore
    // any Product whose id is gone falls back to the fill order.
    QList<int> viewKinds;
    QStringList viewProducts;
    for (const auto &sl : m_slots) {
        viewKinds << static_cast<int>(sl.view.kind);
        viewProducts << sl.view.productId;
    }
    s.setValue(u"viewKinds"_s, QVariant::fromValue(viewKinds));
    s.setValue(u"viewProducts"_s, viewProducts);
    s.setValue(u"activePane"_s, m_activePane);
    if (m_paneSplitter)
        s.setValue(u"splitterState"_s, m_paneSplitter->saveState());
    if (m_leftColumn)
        s.setValue(u"leftColumnState"_s, m_leftColumn->saveState());
    if (m_rightColumn)
        s.setValue(u"rightColumnState"_s, m_rightColumn->saveState());
    s.endGroup();
}

void vtkWindowCube::restoreLayoutSettings()
{
    QSettings s;
    s.beginGroup(u"layout_v2/cube"_s);
    if (!s.contains(u"paneCount"_s)) {
        s.endGroup();
        return;   // first run — keep the §3.1 default (1 pane, 3D)
    }
    const QList<int> kinds = s.value(u"viewKinds"_s).value<QList<int>>();
    const QStringList prods = s.value(u"viewProducts"_s).toStringList();
    for (int i = 0; i < 4 && i < kinds.size(); ++i) {
        const int k = kinds[i];
        const ViewId kind = (k >= 0 && k <= int(ViewId::Product)) ? static_cast<ViewId>(k)
                                                                  : ViewId::None;
        PaneView pv{ kind, (i < prods.size()) ? prods[i] : QString() };
        // A Product whose image is gone (session-scoped; empty right after open) or a
        // fixed view whose widget doesn't exist falls back to None → the fill order
        // re-seeds the pane rather than leaving it broken (§4.3).
        if (kind == ViewId::Product) {
            if (!m_products2d.contains(pv.productId))
                pv = {};
        } else if (kind != ViewId::None && !viewWidget(pv)) {
            pv = {};
        }
        m_slots[i].view = pv;
    }
    m_activePane = std::clamp(s.value(u"activePane"_s, 0).toInt(), 0, 3);
    const int dir = std::clamp(s.value(u"paneDirection"_s, 0).toInt(), 0, 2);
    m_paneDirection = static_cast<PaneDirection>(dir);
    const int panes = s.value(u"paneCount"_s, 1).toInt();
    const QByteArray splitterState = s.value(u"splitterState"_s).toByteArray();
    const QByteArray leftState = s.value(u"leftColumnState"_s).toByteArray();
    const QByteArray rightState = s.value(u"rightColumnState"_s).toByteArray();
    m_restoringLayout = true;   // applyPaneLayout must not re-save (would clobber states)
    applyPaneLayout(panes);     // re-mounts, re-fits, re-chromes
    m_restoringLayout = false;
    if (m_paneSplitter && !splitterState.isEmpty())
        m_paneSplitter->restoreState(splitterState);
    if (m_leftColumn && !leftState.isEmpty())
        m_leftColumn->restoreState(leftState);
    if (m_rightColumn && !rightState.isEmpty())
        m_rightColumn->restoreState(rightState);
    s.endGroup();
    layout2dPane(/*force=*/true);
}

void vtkWindowCube::applyMomentMapResult(const MomentMapApplyResult &result)
{
    if (!result.imageData) {
        return;
    }

    QElapsedTimer totalTimer;
    totalTimer.start();

    // §3.2: order-scoped product id — Moment 0 and Moment 1 are distinct rows that
    // coexist. Cache the image + register the product FIRST (before ANY refreshPaneChrome)
    // so the three consumers — Session Data tree, header dropdowns, cache — all see it at
    // once. renderer="moment2d" marks it showable in a pane. A same-order recompute
    // replaces the cache entry (QHash::insert) and re-registers the id (productUpdated).
    const QString productId =
            QStringLiteral("moment%1@%2").arg(result.order).arg(this->datasetPersistKey());
    const QString productLabel = QStringLiteral("Moment %1").arg(result.order);

    // Colormap + range default BY KIND, not inherited from the cube (§3.2). A velocity
    // field (Moment 1) reads as a DIVERGING map on a robust symmetric range centred on
    // systemic velocity, with sparse noise spikes clamped transparent; intensity
    // (Moment 0) and the rest keep the sequential map + true min/max.
    const bool velocity = (result.order == 1);
    std::array<double, 2> displayRange = result.imageRange;
    bool clampOutliers = false;
    if (velocity) {
        double lo = 0., hi = 0.;
        if (robustSymmetricMomentRange(result.imageData, lo, hi)) {
            displayRange = { lo, hi };
            clampOutliers = true;
        }
    }
    // Default strictly BY KIND, never inherited (else a prior velocity's CoolWarm would
    // leak onto a later intensity map). A signed velocity field is always shown Linear —
    // a Log/Sqrt stretch floors negatives and would push blueshift below-range (§3.2).
    const QString momentLut = velocity ? u"CoolWarm"_s : u"Inferno"_s;
    const LutScale::Mode momentScale = velocity ? LutScale::Mode::Linear : this->m_sliceScaleMode;

    Product2D cached;
    cached.imageData = result.imageData;
    cached.imageRange = displayRange;
    cached.lutName = momentLut;
    cached.scaleMode = momentScale;
    cached.gamma = this->m_sliceScaleGamma;
    cached.label = productLabel;
    cached.clampOutliers = clampOutliers;
    cached.order = result.order;
    cached.channelStart = result.channelStart;
    cached.channelEnd = result.channelEnd;
    cached.maskEnabled = result.maskEnabled;
    cached.thresholdValue = result.thresholdValue;
    cached.thresholdAuto = result.thresholdAuto;
    this->m_products2d.insert(productId, cached);
    this->m_lastComputedMomentProductId = productId;   // for the Slice/Moment toggle + export fallback
    this->registerAnalysisProduct(u"moment"_s, productLabel, this->m_momentProvenanceParams,
                                  this, productId, u"moment2d"_s);

    // §3.2 corollary 2 — a new product does NOT steal a pane showing other data. Pick a
    // target ONLY among VISIBLE panes: one already showing this exact product (recompute)
    // or one showing a moment EMPTY STATE ("No moment map"). A hidden/parked Product slot
    // (e.g. a 4-pane seed after collapsing to 1 pane) must NOT be targeted — otherwise the
    // compute would mutate the shared pipeline + activate an off-screen pane. When no
    // visible pane wants it, the product lives only in the tree + dropdown.
    auto visible = [&](int i) { return m_slots[i].frame && m_slots[i].frame->isVisible(); };
    int target = -1;
    // Recompute: refresh EVERY visible pane already showing this product (a pane may be
    // duplicated), so a re-run swaps the new image into all of them — not just the first.
    for (int i = 0; i < 4; ++i)
        if (visible(i) && m_slots[i].view == PaneView{ ViewId::Product, productId }) {
            mountView(i);
            if (target < 0)
                target = i;
        }
    // Else drop it into the FIRST free pane (no-steal): a None "No product available" pane,
    // or a legacy empty moment pane. A pane showing the slice / 3-D / a spectrum / a
    // different moment is never stolen — if nothing is free the product waits in Session
    // Data for manual activation (user request).
    if (target < 0)
        for (int i = 0; i < 4; ++i) {
            if (!visible(i))
                continue;
            PaneView &pv = m_slots[i].view;
            const bool freeNone = pv.isNone();
            const bool emptyMoment = (pv.kind == ViewId::Product && !viewHasData(pv));
            if (freeNone || emptyMoment) {
                pv = PaneView{ ViewId::Product, productId };
                mountView(i);
                target = i;
                break;
            }
        }

    if (target < 0) {
        // No-steal: nothing on screen wanted a moment — refresh chrome so existing
        // dropdowns list the new product, and leave everything else as it was.
        refreshPaneChrome();
        return;
    }

    // Activate the (first) pane that now shows it so its border/dot read as current.
    setActivePane(target);
    refreshPaneChrome();

    // Sidebar stats readout — the TRUE data min/max/mean/rms (informational).
    {
        const QSignalBlocker blockMin(ui->lineImgMin);
        const QSignalBlocker blockMax(ui->lineImgMax);
        ui->lineImgMin->setText(fmtStat(result.imageRange[0]));
        ui->lineImgMax->setText(fmtStat(result.imageRange[1]));
        double mean, rms;
        computeImageStats(result.imageData, mean, rms);
        ui->lineImgMean->setText(fmtStat(mean));
        ui->lineImgRms->setText(fmtStat(rms));
    }
    qDebug().noquote() << QStringLiteral("[perf][moment] apply total: %1 ms").arg(
            totalTimer.elapsed());
}

bool vtkWindowCube::isBusy() const
{
    return this->cubeOpenWatcher.isRunning() || this->remotePreviewWatcher.isRunning()
            || this->remoteHighResCubeWatcher.isRunning()
            || (this->momentController && this->momentController->isBusy())
            || (this->pvController && this->pvController->isBusy())
            || this->activeRemoteSliceRequests > 0
            || this->activeRemoteIsosurfaceRequests > 0 || this->isosurfaceWatcher.isRunning();
}

void vtkWindowCube::setRemoteCubeDisplayState(RemoteCubeDisplayState state)
{
    this->remoteCubeDisplayState = state;
    this->updateDataStatePanel();
    this->updateSanityPanel();

    switch (state) {
    case RemoteCubeDisplayState::Preview:
        this->remoteFullResolutionStateTimer.stop();
        this->setCubeOpenStateLabel(u"Preview"_s);
        break;
    case RemoteCubeDisplayState::LoadingFullResolution:
        this->remoteFullResolutionStateTimer.start(remoteLoadingStateDelayMs);
        break;
    case RemoteCubeDisplayState::FullResolution:
        this->remoteFullResolutionStateTimer.stop();
        this->setCubeOpenStateLabel(u"Full resolution"_s);
        break;
    }
}

void vtkWindowCube::setMomentActionsEnabled(bool enabled)
{
    const bool available = this->hasBackendVisibleDataset();
    const bool finalEnabled = enabled && available;
    const QString tooltip = available ? QString {} : kMomentBackendTooltip;

    ui->actionComputeMoment->setEnabled(finalEnabled);
    ui->actionComputeMoment->setToolTip(tooltip);
    ui->actionComputeMoment->setStatusTip(tooltip);

    // S-02/03/04 spectral tool actions follow the same availability rule
    const bool spectralEnabled = available && !this->isBusy();
    ui->actionLinewidthMap->setEnabled(spectralEnabled);
    ui->actionBaselineSubtract->setEnabled(spectralEnabled);
    ui->actionStackCubes->setEnabled(available);   // stacking is never "busy"
}

bool vtkWindowCube::hasBackendVisibleDataset() const
{
    return !this->remoteDatasetId.isEmpty();
}

void vtkWindowCube::updateBackendDerivedActionsAvailability()
{
    const bool available = this->hasBackendVisibleDataset();
    this->setMomentActionsEnabled(
            !(this->momentController && this->momentController->isBusy()));

    if (this->actionExtractPvDiagram) {
        const bool pvBusy = this->pvController && this->pvController->isBusy();
        const bool pvEnabled = available && !this->cubeOpenWatcher.isRunning()
                && !this->remotePreviewWatcher.isRunning() && !pvBusy;
        this->actionExtractPvDiagram->setEnabled(pvEnabled);
        this->actionExtractPvDiagram->setToolTip(available ? QString {} : kPvBackendTooltip);
        this->actionExtractPvDiagram->setStatusTip(available ? QString {} : kPvBackendTooltip);
    }

    if (this->actionEstimateNoise) {
        const bool noiseBusy = this->noiseController && this->noiseController->isBusy();
        const bool noiseEnabled = available && !this->cubeOpenWatcher.isRunning()
                && !this->remotePreviewWatcher.isRunning() && !noiseBusy;
        this->actionEstimateNoise->setEnabled(noiseEnabled);
        this->actionEstimateNoise->setToolTip(available ? QString {} : kNoiseBackendTooltip);
        this->actionEstimateNoise->setStatusTip(available ? QString {} : kNoiseBackendTooltip);
    }

    const bool isoEnabled = available && !this->cubeOpenWatcher.isRunning()
            && !this->remotePreviewWatcher.isRunning();
    ui->actionIsosurface->setEnabled(isoEnabled);
    ui->actionIsosurface->setToolTip(available ? QString {} : kIsosurfaceBackendTooltip);
    ui->actionIsosurface->setStatusTip(available ? QString {} : kIsosurfaceBackendTooltip);
}

void vtkWindowCube::setCubeOpenActionsEnabled(bool enabled)
{
    if (enabled) {
        this->updateBackendDerivedActionsAvailability();
    } else {
        this->setMomentActionsEnabled(false);
        ui->actionIsosurface->setEnabled(false);
        if (this->actionExtractPvDiagram) {
            this->actionExtractPvDiagram->setEnabled(false);
        }
        if (this->actionEstimateNoise) {
            this->actionEstimateNoise->setEnabled(false);
        }
    }
    ui->actionExtractSpectrum->setEnabled(enabled);
}

void vtkWindowCube::setCubeOpenStateLabel(const QString & /*text*/)
{
    // §3.0c: retired. This standalone label duplicated the load-state the status
    // panel already shows ("Remote · Full resolution"), leaving two "Full
    // resolution" strings in the rail. Kept as a no-op (callers unchanged); the
    // widget stays hidden.
    if (this->cubeOpenStateLabel)
        this->cubeOpenStateLabel->hide();
}

void vtkWindowCube::setInteractorStyleImage()
{
    // §3.0: slice and moment have separate interactors — set the style on BOTH so
    // the mode holds whichever 2-D widget is shown.
    for (auto *win : { this->sliceWin.GetPointer(), this->momentWin.GetPointer() }) {
        if (win && win->GetInteractor()) {
            vtkNew<vtkInteractorStyleImage> style;
            win->GetInteractor()->SetInteractorStyle(style);
        }
    }
    if (auto *win = this->active2dWin())
        win->Render();
}

void vtkWindowCube::setInteractorStyleRegion()
{
    for (auto *win : { this->sliceWin.GetPointer(), this->momentWin.GetPointer() }) {
        if (win && win->GetInteractor()) {
            vtkNew<vtkInteractorStyleUser> style;
            win->GetInteractor()->SetInteractorStyle(style);
        }
    }
    if (auto *win = this->active2dWin())
        win->Render();
}

void vtkWindowCube::setInteractorStyleProfile()
{
    if (!this->profileWidget) {
        // Profile (spectrum) extraction draws on the SLICE — a slice-mode operation.
        vtkNew<vtkInteractorStyleProfile> style;
        if (this->sliceWin && this->sliceWin->GetInteractor())
            this->sliceWin->GetInteractor()->SetInteractorStyle(style);
        if (this->sliceWin)
            this->sliceWin->Render();

        this->profileWidget = new ProfileWidget(
                style, vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0)),
                                                this->filepath.toStdString(), this);
        this->profileWidget->setUsageMode(ProfileWidget::UsageMode::ProbeLive, u"Profile"_s);
        this->profileWidget->setupSpectrumPlot(this->spectralAxisTitle(), u"Value"_s);
        this->profileWidget->applyPlotTheme(this->m_darkPlots);
        QObject::connect(this->profileWidget, &ProfileWidget::destroyed, this,
                         &vtkWindowCube::setInteractorStyleImage, Qt::QueuedConnection);
    }
}



void vtkWindowCube::changeImageRenderer()
{
    // Leaving the channel-map view invalidates 2-D mask editing (the moment map maps
    // differently) → tear it down cleanly while the slice interactor is still current.
    if (this->m_lassoEditActive && !ui->actionSlice->isChecked()) {
        this->setLassoEditActive(false);
        if (this->m_lassoEditToggle) {
            QSignalBlocker blk(this->m_lassoEditToggle);
            this->m_lassoEditToggle->setChecked(false);
        }
    }

    double imgRange[2];
    vtkImageData *imgForStats = nullptr;
    const bool slice = ui->actionSlice->isChecked();

    // §3.1/§3.2: at ≤2 panes the single 2-D slot swaps its VIEW between the slice and
    // the current moment product (mountView parks the other host). At 4 panes both are
    // separate slots, so this only fires for the ≤2 case. The moment target is the
    // product currently loaded in the pipeline, else the order-0 seed (empty state).
    if (m_paneCount <= 2) {
        int twoDSlot = -1;
        for (int i = 0; i < 4; ++i)
            if (m_slots[i].view.kind == ViewId::Slice2D
                || m_slots[i].view.kind == ViewId::Product) {
                twoDSlot = i;
                if (m_slots[i].frame && m_slots[i].frame->isVisible())
                    break;
            }
        if (twoDSlot >= 0) {
            // Toggling to Moment shows the most recently computed moment (else the loaded
            // pipeline product, else the order-0 seed → its empty state).
            QString momentId = m_lastComputedMomentProductId;
            if (momentId.isEmpty())
                momentId = m_momentPipelineProductId;
            if (momentId.isEmpty())
                momentId = QStringLiteral("moment0@%1").arg(this->datasetPersistKey());
            m_slots[twoDSlot].view = slice ? PaneView{ ViewId::Slice2D, {} }
                                           : PaneView{ ViewId::Product, momentId };
            clearViewFromOtherSlots(twoDSlot);
            mountView(twoDSlot);
            refreshPaneChrome();
        }
    }

    if (slice) {
        ui->labelImg->setText(u"Slice:"_s);
        auto *sliceImage = vtkImageData::SafeDownCast(
                this->remoteSliceDisplaySource->GetOutputDataObject(0));
        imgForStats = sliceImage;
        if (sliceImage) {
            sliceImage->GetScalarRange(imgRange);
        } else {
            imgRange[0] = 0.;
            imgRange[1] = 0.;
        }
    } else {
        ui->labelImg->setText(u"Moment Map:"_s);
        auto *momentImage = vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
        imgForStats = momentImage;
        if (momentImage) {
            momentImage->GetScalarRange(imgRange);
        } else {
            imgRange[0] = 0.;
            imgRange[1] = 0.;
        }
    }

    ui->lineImgMin->setText(fmtStat(imgRange[0]));
    ui->lineImgMax->setText(fmtStat(imgRange[1]));
    double mean, rms;
    computeImageStats(imgForStats, mean, rms);
    ui->lineImgMean->setText(fmtStat(mean));
    ui->lineImgRms->setText(fmtStat(rms));
    if (auto *win = this->active2dWin())
        this->coordinate->SetViewport(win->GetRenderers()->GetFirstRenderer());
    if (this->probeModeActive && this->probeValid) {
        this->refreshProbeOverlay();
        auto *currentImage = slice
                ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
                : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
        this->updateProbeReadout(currentImage);
    }
    if (this->regionMode != RegionMode::None && this->regionValid) {
        this->refreshRegionOverlay();
    }
    if (this->pvModeActive && this->pvValid) {
        this->refreshPvOverlay();
    }
    // Keep the 2D sidebar in sync with the active mode.
    m_uiAssembler->sync2dModeSidebar(slice);
    this->update2dModeVisibility(slice);
    this->updateInfoStatsPage();
    this->layout2dPaneFor(slice, /*force=*/true);   // re-letterbox the newly-shown 2-D window
    if (auto *win = this->active2dWin())
        win->Render();
}

void vtkWindowCube::changeCubeRender()
{
    qDebug().noquote()
            << QStringLiteral("[remote-iso] changeCubeRender triggered checked=%1")
                       .arg(ui->actionIsosurface->isChecked());
    if (ui->actionIsosurface->isChecked()) {
        this->scheduleIsosurfaceRecompute();
        this->setCubeRenderModeLocally(false);
        qDebug().noquote()
                << QStringLiteral("[remote-iso] keeping volume visible while waiting for mesh");
        ui->vtkCube->renderWindow()->Render();
        this->updateInfoStatsPage();
        return;
    }
    this->setCubeRenderModeLocally(false);
    ui->vtkCube->renderWindow()->Render();
    this->updateInfoStatsPage();
}

void vtkWindowCube::changeCubeColor()
{
    if (this->viewingIsosurface()) {
        double rgb[3];
        this->currentIsosurfaceActor->GetProperty()->GetColor(rgb);

        QColor color;
        color.setRgbF(rgb[0], rgb[1], rgb[2]);
        QColorDialog dialog(color, this);
        dialog.setOption(QColorDialog::ShowAlphaChannel, false);
        if (dialog.exec() == QDialog::Accepted) {
            const QColor selected = dialog.selectedColor();
            this->currentIsosurfaceActor->GetProperty()->SetColor(selected.redF(),
                                                                  selected.greenF(),
                                                                  selected.blueF());
            ui->vtkCube->renderWindow()->Render();
        }
    } else {
        const auto names = ColorMaps::GetColorMapNames();
        QStringList items;
        items.reserve(names.size());
        std::transform(names.cbegin(), names.cend(), std::back_inserter(items),
                       [](const std::string &name) { return QString::fromStdString(name); });

        const int idxCurrent = items.indexOf(this->volumeColorTransferFunction->GetObjectName());
        bool ok{ };
        const QString palette =
                QInputDialog::getItem(this, u"Select Color Map"_s, u"Color map:"_s, items,
                                      idxCurrent, false, &ok);
        if (ok && !palette.isEmpty()) {
            // Delegate full LUT application (updates slice, moment, and volume CTF).
            this->applyColorMapByName(palette);
            // Sync the sidebar combo so the two paths stay consistent.
            if (m_uiAssembler) {
                if (auto *c = m_uiAssembler->lutCombo()) {
                    const QSignalBlocker b(c);
                    c->setCurrentText(palette);
                }
            }
        }
    }
}







void vtkWindowCube::renderImage()
{
    this->active2dWin()->Render();
}

void vtkWindowCube::startAsyncIsosurface(double isoValue)
{
    if (this->remoteIsosurfaceRequestInFlight
        && std::fabs(this->inFlightRemoteIsosurfaceThreshold - isoValue) < 1e-9) {
        qDebug().noquote()
                << QStringLiteral("[remote-iso] skipping duplicate in-flight threshold=%1")
                           .arg(isoValue, 0, 'g', 12);
        return;
    }

    const int requestId = ++this->currentIsosurfaceRequestId;
    this->remoteIsosurfaceReady = false;
    qDebug().noquote()
            << QStringLiteral("[remote-iso] startAsyncIsosurface requestId=%1 threshold=%2")
                       .arg(requestId)
                       .arg(isoValue, 0, 'g', 12);
    this->showPersistentStatusMessage(u"Computing isosurface..."_s);

    {
        const std::array<int, 6> fullExtent = { 0,
                                                std::max(0, this->remoteDatasetWidth - 1),
                                                0,
                                                std::max(0, this->remoteDatasetHeight - 1),
                                                0,
                                                std::max(0, this->remoteDatasetDepth - 1) };
        const bool roiSubvolumeActive = this->usingHighResCube && this->currentRemoteRoi != fullExtent;
        if (roiSubvolumeActive) {
            if (this->isosurfaceWatcher.isRunning()) {
                return;
            }
            auto *source = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
            if (!source) {
                return;
            }

            vtkSmartPointer<vtkImageData> data = vtkSmartPointer<vtkImageData>::New();
            data->DeepCopy(source);
            qDebug().noquote()
                    << QStringLiteral("[remote-iso] using loaded ROI/subvolume block for isosurface roi=%1..%2,%3..%4,%5..%6")
                               .arg(this->currentRemoteRoi[0])
                               .arg(this->currentRemoteRoi[1])
                               .arg(this->currentRemoteRoi[2])
                               .arg(this->currentRemoteRoi[3])
                               .arg(this->currentRemoteRoi[4])
                               .arg(this->currentRemoteRoi[5]);
            this->isosurfaceWatcher.setFuture(
                    QtConcurrent::run([data, isoValue, requestId]() {
                        return computeIsosurface(data, isoValue, requestId);
                    }));
            return;
        }

        ++this->activeRemoteIsosurfaceRequests;
        this->remoteIsosurfaceRequestInFlight = true;
        this->inFlightRemoteIsosurfaceThreshold = isoValue;
        auto *watcher = new QFutureWatcher<AsyncIsosurfaceResult>(this);
        watcher->setProperty("requestId", requestId);
        watcher->setProperty("isoValue", isoValue);
        QObject::connect(watcher, &QFutureWatcher<AsyncIsosurfaceResult>::finished, this,
                         [this, watcher]() {
                             --this->activeRemoteIsosurfaceRequests;
                             const auto result = watcher->result();
                             const int requestId = watcher->property("requestId").toInt();
                             const double isoValue = watcher->property("isoValue").toDouble();
                             if (std::fabs(this->inFlightRemoteIsosurfaceThreshold - isoValue) < 1e-9) {
                                 this->remoteIsosurfaceRequestInFlight = false;
                                 this->inFlightRemoteIsosurfaceThreshold =
                                         std::numeric_limits<double>::quiet_NaN();
                             }
                             watcher->deleteLater();

                             if (requestId != this->currentIsosurfaceRequestId) {
                                 if (this->activeRemoteIsosurfaceRequests == 0) {
                                     this->clearPersistentStatusMessage();
                                 }
                                 return;
                             }

                             if (!result.mesh || result.mesh->GetNumberOfPoints() == 0) {
                                 this->persistentStatusActive = false;
                                 this->statusMessageClearTimer.stop();
                                 this->statusBar()->showMessage(result.errorMessage.isEmpty()
                                                                        ? u"Remote isocontour is empty."_s
                                                                        : result.errorMessage);
                                 this->remoteIsosurfaceReady = false;
                                 this->setCubeRenderModeLocally(false);
                                 ui->actionVolume->setChecked(true);
                                 ui->vtkCube->renderWindow()->Render();
                                 return;
                             }

                             this->applyIsosurfaceResult(result);
                             this->clearPersistentStatusMessage();
                         });
        watcher->setFuture(QtConcurrent::run(&fetchRemoteIsosurface, this->remoteBackendUrl,
                                             this->remoteDatasetId, isoValue, requestId,
                                             this->remoteSessionId, this->remoteBackendToken));
    }
}

void vtkWindowCube::scheduleIsosurfaceRecompute()
{
    this->isosurfaceDebounceTimer.start(150);
}

// ── Cutting plane visibility / opacity ──────────────────────────────────────
void vtkWindowCube::setCuttingPlaneOpacity(double opacity)
{
    if (!this->remoteCuttingPlaneActor) {
        return;
    }
    opacity = std::clamp(opacity, 0.0, 1.0);
    this->remoteCuttingPlaneActor->GetProperty()->SetOpacity(opacity);
    // Edge stays visible even at opacity 0 so the plane location is still
    // identifiable; toggle SetVisibility(0) via the Show Cutting Plane action
    // to hide it entirely.
    if (ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }
}

// ── Slice animation (play / pause through spectral axis) ────────────────────
void vtkWindowCube::setSliceAnimationActive(bool active)
{
    if (active) {
        this->animationDirection = +1;
        this->sliceAnimationTimer.start(std::max(1, 1000 / this->animationFps));
    } else {
        this->sliceAnimationTimer.stop();
    }
}

void vtkWindowCube::advanceSliceAnimation()
{
    if (!ui->spinSlice) {
        return;
    }
    const int minIndex = ui->spinSlice->minimum();
    const int maxIndex = ui->spinSlice->maximum();
    if (maxIndex <= minIndex) {
        return;
    }
    int next = ui->spinSlice->value() + this->animationDirection;
    if (next > maxIndex) {
        switch (this->animationMode) {
        case AnimationMode::Loop:   next = minIndex; break;
        case AnimationMode::Bounce: this->animationDirection = -1; next = maxIndex - 1; break;
        case AnimationMode::Once:
            next = maxIndex;
            this->sliceAnimationTimer.stop();
            if (this->actionPlaySlices) {
                this->actionPlaySlices->setChecked(false);
            }
            break;
        }
    } else if (next < minIndex) {
        // Only reachable in Bounce mode.
        this->animationDirection = +1;
        next = minIndex + 1;
    }
    ui->spinSlice->setValue(std::clamp(next, minIndex, maxIndex));
}

// ── 3D WCS axes (vtkCubeAxesActor) ──────────────────────────────────────────
void vtkWindowCube::apply3dWcsAxesVisibility(bool visible)
{
    auto *renderer = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    if (!renderer) {
        return;
    }
    if (visible) {
        if (!this->wcs3dAxesActor) {
            this->wcs3dAxesActor = vtkSmartPointer<vtkCubeAxesActor>::New();
            this->wcs3dAxesActor->SetCamera(renderer->GetActiveCamera());
            // Show only the X/Y/Z labels, ticks and minor ticks; no inner grid
            // lines to keep the 3D view readable.
            this->wcs3dAxesActor->XAxisMinorTickVisibilityOff();
            this->wcs3dAxesActor->YAxisMinorTickVisibilityOff();
            this->wcs3dAxesActor->ZAxisMinorTickVisibilityOff();
            this->wcs3dAxesActor->SetFlyModeToOuterEdges();
        }
        this->refresh3dWcsAxes();
        if (!renderer->HasViewProp(this->wcs3dAxesActor)) {
            renderer->AddViewProp(this->wcs3dAxesActor);
        }
    } else if (this->wcs3dAxesActor && renderer->HasViewProp(this->wcs3dAxesActor)) {
        renderer->RemoveViewProp(this->wcs3dAxesActor);
    }
    if (ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }
}

void vtkWindowCube::refresh3dWcsAxes()
{
    if (!this->wcs3dAxesActor) {
        return;
    }
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!cubeImage) {
        return;
    }
    double bounds[6];
    cubeImage->GetBounds(bounds);
    this->wcs3dAxesActor->SetBounds(bounds);

    // Each axis is labeled with its WCS coordinate range at the bounding-box
    // corners. remoteVoxelToWcs falls back to voxel indices if metadata is
    // missing, so we always have *some* label.
    const auto axisLabel = [this](int axis) {
        QString title = this->remoteAxisTitle(axis);
        if (title.isEmpty()) {
            title = u"Axis %1"_s.arg(axis + 1);
        }
        return title.toStdString();
    };
    this->wcs3dAxesActor->SetXTitle(axisLabel(0).c_str());
    this->wcs3dAxesActor->SetYTitle(axisLabel(1).c_str());
    this->wcs3dAxesActor->SetZTitle(axisLabel(2).c_str());

    const auto setRange = [this, bounds](int axis, vtkCubeAxesActor *a) {
        const int idxMin = axis * 2;
        const int idxMax = idxMin + 1;
        bool okMin = false, okMax = false;
        const double wMin = this->remoteVoxelToWcs(axis, bounds[idxMin], &okMin);
        const double wMax = this->remoteVoxelToWcs(axis, bounds[idxMax], &okMax);
        if (!okMin || !okMax || !std::isfinite(wMin) || !std::isfinite(wMax)) {
            return; // Leave default voxel-space range when WCS is unusable.
        }
        switch (axis) {
        case 0: a->SetXAxisRange(wMin, wMax); break;
        case 1: a->SetYAxisRange(wMin, wMax); break;
        case 2: a->SetZAxisRange(wMin, wMax); break;
        }
    };
    setRange(0, this->wcs3dAxesActor);
    setRange(1, this->wcs3dAxesActor);
    setRange(2, this->wcs3dAxesActor);
}

// ── 3D spectral picker ──────────────────────────────────────────────────────
void vtkWindowCube::install3dPickObserver()
{
    auto *iren = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetInteractor()
            : nullptr;
    if (!iren || this->cube3dPickObserverTag != 0) {
        return;
    }
    auto cb = vtkSmartPointer<vtkCallbackCommand>::New();
    cb->SetClientData(this);
    cb->SetCallback([](vtkObject *, unsigned long, void *clientData, void *) {
        auto *self = static_cast<vtkWindowCube *>(clientData);
        self->handle3dPickEvent();
    });
    this->cube3dPickObserverTag =
            iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, cb, 1.0);
}

void vtkWindowCube::remove3dPickObserver()
{
    auto *iren = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetInteractor()
            : nullptr;
    if (iren && this->cube3dPickObserverTag != 0) {
        iren->RemoveObserver(this->cube3dPickObserverTag);
    }
    this->cube3dPickObserverTag = 0;
}

void vtkWindowCube::handle3dPickEvent()
{
    auto *iren = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetInteractor()
            : nullptr;
    auto *renderer = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    if (!iren || !renderer || !this->remoteCuttingPlaneActor) {
        return;
    }

    int pos[2];
    iren->GetEventPosition(pos);
    auto picker = vtkSmartPointer<vtkPropPicker>::New();
    if (picker->Pick(pos[0], pos[1], 0., renderer) == 0) {
        return;
    }
    if (picker->GetViewProp() != this->remoteCuttingPlaneActor.GetPointer()) {
        return; // User clicked on volume / outline / axes — ignore.
    }

    double world[3];
    picker->GetPickPosition(world);

    // Map world XY (in cube-image coords) → voxel index on the current slice.
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!cubeImage) {
        return;
    }
    int extent[6];
    double origin[3], spacing[3];
    cubeImage->GetExtent(extent);
    cubeImage->GetOrigin(origin);
    cubeImage->GetSpacing(spacing);
    const int voxelX = std::lround(extent[0] + (world[0] - origin[0]) / spacing[0]);
    const int voxelY = std::lround(extent[2] + (world[1] - origin[1]) / spacing[1]);
    const int voxelZ = this->clampRemoteSliceIndex(ui->spinSlice->value() - 1);
    if (voxelX < extent[0] || voxelX > extent[1]
        || voxelY < extent[2] || voxelY > extent[3]) {
        this->statusBar()->showMessage(u"Pick outside cube bounds."_s, 1500);
        return;
    }

    this->probeValid = true;
    this->probeFrozen = true;  // hold the pick — 2D mouse moves should not overwrite it
    // updateProbePlot() early-returns if probeModeActive==false; the 3D pick
    // is an explicit user request to see the spectrum, so flip the flag here
    // (without calling setProbeModeActive() — that would also flip the 2D
    // cursor and disable region/PV actions, which is out of scope for 3D pick).
    this->probeModeActive = true;
    this->probeVoxel = { voxelX, voxelY, voxelZ };
    auto *sliceImage =
            vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
    if (sliceImage) {
        this->updateProbeReadout(sliceImage);
    }
    // updateProbePlot() lazily creates/raises the ProfileWidget; making the
    // pick visible is the whole point of this mode, so we ensure the plot
    // window is shown + raised even if the user had previously closed it.
    this->updateProbePlot();
    // §3.1: when the Spectrum plot is mounted in a pane, don't also pop the floating
    // window — the pane IS its home.
    bool spectrumInPane = false;
    for (const auto &s : m_slots)
        if (s.view.kind == ViewId::Spectrum && s.frame && s.frame->isVisible())
            spectrumInPane = true;
    if (this->probePlotWidget && !spectrumInPane) {
        this->probePlotWidget->show();
        this->probePlotWidget->raise();
    }
    this->refreshProbeOverlay();
    if (this->sliceWin) {
        this->sliceWin->Render();
    }
    this->statusBar()->showMessage(
            u"Spectrum extracted at voxel (%1, %2) — slice %3"_s
                    .arg(voxelX).arg(voxelY).arg(voxelZ + 1),
            3000);
}

void vtkWindowCube::setCubeRenderModeLocally(bool isosurfaceMode)
{
    auto *renderer = ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer();
    if (!renderer) {
        return;
    }

    if (isosurfaceMode && !this->remoteIsosurfaceReady) {
        isosurfaceMode = false;
    }

    if (isosurfaceMode) {
        if (this->currentIsosurfaceActor && !renderer->HasViewProp(this->currentIsosurfaceActor)) {
            renderer->AddActor(this->currentIsosurfaceActor);
        }
        if (this->currentIsosurfaceActor && renderer->HasViewProp(this->volume)) {
            renderer->RemoveViewProp(this->volume);
        }
    } else {
        if (this->currentIsosurfaceActor && renderer->HasViewProp(this->currentIsosurfaceActor)) {
            renderer->RemoveActor(this->currentIsosurfaceActor);
        }
        if (!renderer->HasViewProp(this->volume)) {
            renderer->AddViewProp(this->volume);
        }
    }

    // Mirror to the remote renderer.
    QVariantMap cmd;
    cmd[u"type"_s] = u"set_cube_render_mode"_s;
    cmd[u"mode"_s] = isosurfaceMode ? u"isosurface"_s : u"volume"_s;
    this->sendRemoteRenderStateCommand(cmd);
}

void vtkWindowCube::applyIsosurfaceResult(const AsyncIsosurfaceResult &result)
{
    if (!result.mesh || result.mesh->GetNumberOfPoints() == 0 || result.mesh->GetNumberOfPolys() == 0) {
        return;
    }

    vtkSmartPointer<vtkPolyData> displayMesh = result.mesh;

    double meshBounds[6];
    result.mesh->GetBounds(meshBounds);
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    double cubeBounds[6] = { 0., 0., 0., 0., 0., 0. };
    if (cubeImage) {
        cubeImage->GetBounds(cubeBounds);
    }
    qDebug().noquote()
            << QStringLiteral("[remote-iso] mesh bounds=%1,%2,%3,%4,%5,%6")
                       .arg(meshBounds[0], 0, 'g', 12)
                       .arg(meshBounds[1], 0, 'g', 12)
                       .arg(meshBounds[2], 0, 'g', 12)
                       .arg(meshBounds[3], 0, 'g', 12)
                       .arg(meshBounds[4], 0, 'g', 12)
                       .arg(meshBounds[5], 0, 'g', 12);
    qDebug().noquote()
            << QStringLiteral("[remote-iso] cube bounds=%1,%2,%3,%4,%5,%6")
                       .arg(cubeBounds[0], 0, 'g', 12)
                       .arg(cubeBounds[1], 0, 'g', 12)
                       .arg(cubeBounds[2], 0, 'g', 12)
                       .arg(cubeBounds[3], 0, 'g', 12)
                       .arg(cubeBounds[4], 0, 'g', 12)
                       .arg(cubeBounds[5], 0, 'g', 12);

    const bool degenerateBounds = !std::isfinite(meshBounds[0]) || !std::isfinite(meshBounds[1])
            || !std::isfinite(meshBounds[2]) || !std::isfinite(meshBounds[3])
            || !std::isfinite(meshBounds[4]) || !std::isfinite(meshBounds[5])
            || meshBounds[0] == meshBounds[1] || meshBounds[2] == meshBounds[3]
            || meshBounds[4] == meshBounds[5];
    if (degenerateBounds) {
        qDebug().noquote() << QStringLiteral("[remote-iso] degenerate mesh bounds");
        this->remoteIsosurfaceReady = false;
        this->persistentStatusActive = false;
        this->statusMessageClearTimer.stop();
        this->statusBar()->showMessage(u"Remote isocontour mesh has invalid bounds."_s);
        this->setCubeRenderModeLocally(false);
        ui->actionVolume->setChecked(true);
        ui->vtkCube->renderWindow()->Render();
        return;
    }

    if (cubeImage && validBounds(cubeBounds) && !result.meshInDisplayCoordinates) {
        const double fullBounds[6] = { 0.0,
                                       std::max(0, this->remoteDatasetWidth - 1) * 1.0,
                                       0.0,
                                       std::max(0, this->remoteDatasetHeight - 1) * 1.0,
                                       0.0,
                                       std::max(0, this->remoteDatasetDepth - 1) * 1.0 };
        const double fullSizeX = std::max(1e-9, fullBounds[1] - fullBounds[0]);
        const double fullSizeY = std::max(1e-9, fullBounds[3] - fullBounds[2]);
        const double fullSizeZ = std::max(1e-9, fullBounds[5] - fullBounds[4]);
        const double displaySizeX = cubeBounds[1] - cubeBounds[0];
        const double displaySizeY = cubeBounds[3] - cubeBounds[2];
        const double displaySizeZ = cubeBounds[5] - cubeBounds[4];

        vtkNew<vtkTransform> meshToDisplay;
        meshToDisplay->Scale(displaySizeX / fullSizeX, displaySizeY / fullSizeY,
                             displaySizeZ / fullSizeZ);
        meshToDisplay->Translate(cubeBounds[0], cubeBounds[2], cubeBounds[4]);

        vtkNew<vtkTransformPolyDataFilter> transformFilter;
        transformFilter->SetTransform(meshToDisplay);
        transformFilter->SetInputData(result.mesh);
        transformFilter->Update();

        displayMesh = vtkSmartPointer<vtkPolyData>::New();
        displayMesh->ShallowCopy(transformFilter->GetOutput());

        double transformedBounds[6];
        displayMesh->GetBounds(transformedBounds);
        qDebug().noquote()
                << QStringLiteral("[remote-iso] transformed mesh bounds=%1,%2,%3,%4,%5,%6")
                           .arg(transformedBounds[0], 0, 'g', 12)
                           .arg(transformedBounds[1], 0, 'g', 12)
                           .arg(transformedBounds[2], 0, 'g', 12)
                           .arg(transformedBounds[3], 0, 'g', 12)
                           .arg(transformedBounds[4], 0, 'g', 12)
                           .arg(transformedBounds[5], 0, 'g', 12);
    }

    QElapsedTimer actorTimer;
    actorTimer.start();
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(displayMesh);
    mapper->ScalarVisibilityOff();

    vtkNew<vtkActor> newActor;
    newActor->SetMapper(mapper);
    if (this->currentIsosurfaceActor) {
        newActor->GetProperty()->DeepCopy(this->currentIsosurfaceActor->GetProperty());
    } else {
        newActor->GetProperty()->SetColor(1., 0.5, 1.);
    }

    auto *renderer = ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer();
    if (this->currentIsosurfaceActor && renderer->HasViewProp(this->currentIsosurfaceActor)) {
        renderer->RemoveActor(this->currentIsosurfaceActor);
    }

    this->currentIsosurfaceActor = newActor;
    this->remoteIsosurfaceReady = true;
    qDebug().noquote()
            << QStringLiteral("[perf][isosurface] actor creation+swap: %1 ms")
                       .arg(actorTimer.elapsed());

    QElapsedTimer renderTimer;
    renderTimer.start();
    ui->actionIsosurface->setChecked(true);
    this->setCubeRenderModeLocally(true);
    ui->vtkCube->renderWindow()->Render();
    qDebug().noquote()
            << QStringLiteral("[perf][isosurface] render after apply: %1 ms")
                       .arg(renderTimer.elapsed());
}
