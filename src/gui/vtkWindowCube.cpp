#include "vtkWindowCube.h"

#include "FilterFitsDialog.h"
#include "FitsHeaderEditorDialog.h"
#include "ui_vtkWindowCube.h"
#include "CubeWcsUtil.h"
#include "vtk/RayVoxelPick.h"
#include "CubeViewportGeometry.h"
#include "Pane2DView.h"
#include "CubeStatsUtil.h"
#include "CubeToolGate.h"

#include "qcustomplot.h"
#include "ColorMaps.h"
#include "ChannelOverlay.h"
#include "AssistantPanel.h"
#include "CopilotActionRouter.h"
#include "CubeUiAssembler.h"
#include "WorkspaceChrome.h"
#include "InspectorPanel.h"
#include "theme/SegmentedToggle.h"
#include "app/ProductRegistry.h"
#include "app/HostMemory.h"

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
#include "theme/ToolDialogStyle.h"
#include "ToolProgressSheet.h"
#include "ProductOrigin.h"
#include "ToolResultDialog.h"
#include "ProfileWidget.h"
#include "PvController.h"
#include "PvPathGeometry.h"
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
#include <vtkObjectFactory.h>
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
#include <vtkSphereSource.h>
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
#include <QScopeGuard>
#include <QRegularExpression>
#include <QSet>
#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
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
#include <QToolTip>
#include <QListWidget>
#include <QWidgetAction>
#include <QMouseEvent>
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
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QAbstractButton>
#include <QCoreApplication>
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
#include <memory>
#include <cstring>
#include <limits>
#include <sstream>
#include "CubeRegionGeom.h"

using namespace Qt::StringLiterals;
using namespace CubeWcs;
using namespace CubeGeom;
using namespace CubeStats;

namespace {
constexpr double pi = 3.14159265358979323846;

// §5.4b: a lightweight click filter so a custom QWidgetAction row in the LUT popover acts
// like a menu item (a QWidgetAction's default widget doesn't trigger on click by itself).
// No Q_OBJECT needed — eventFilter is virtual, and we carry a plain std::function callback.
class RowClickFilter : public QObject
{
public:
    using QObject::QObject;
    std::function<void()> onClick;
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (ev->type() == QEvent::MouseButtonRelease && onClick) {
            auto *me = static_cast<QMouseEvent *>(ev);
            if (me->button() != Qt::LeftButton)
                return false;   // only a left click activates
            // Ignore a release that drifted outside the row (drag-out cancel).
            if (auto *w = qobject_cast<QWidget *>(obj))
                if (!w->rect().contains(me->position().toPoint()))
                    return false;
            onClick();
            return true;
        }
        return QObject::eventFilter(obj, ev);
    }
};

// (c) A spin box that DISPLAYS a compact 6-significant-figure value (45035.2, not
// 45035.246094) while keeping the full value editable + in the tooltip. No Q_OBJECT —
// textFromValue/valueFromText are virtual. Applies to the Clip fields for every kind.
class SigFigSpinBox : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    QString textFromValue(double v) const override
    {
        // Keep the tooltip on the FULL value (const_cast so the display + tooltip stay in
        // sync even for programmatic setValue under a signal blocker).
        const_cast<SigFigSpinBox *>(this)->setToolTip(QString::number(v, 'f', 6));
        return QString::number(v, 'g', 6);
    }
    double valueFromText(const QString &text) const override
    {
        bool ok = false;
        const double v = text.toDouble(&ok);
        return ok ? v : QDoubleSpinBox::valueFromText(text);
    }
};

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
    // The third copy of this box (Export Sub-Cube and Mask 3-D Region default
    // their extents from it), and it had the same quadrant bug: a circle or an
    // annulus is a centre plus a rim point, not two opposite corners.
    if (mode == vtkWindowCube::RegionMode::Polygon && !polygonVertices.empty()) {
        auto b = CubeRegionGeom::polygonBounds(polygonVertices);
        b[0] = std::min(b[0], current[0]);
        b[1] = std::max(b[1], current[0]);
        b[2] = std::min(b[2], current[1]);
        b[3] = std::max(b[3], current[1]);
        return b;
    }
    if (mode == vtkWindowCube::RegionMode::Circle || mode == vtkWindowCube::RegionMode::Annulus)
        return CubeRegionGeom::circleBounds(anchor, current);
    return CubeRegionGeom::boxBounds(anchor, current);
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
    float invisibleSentinel =
            static_cast<float>(range > 0.0 ? (rangeMin - 0.01 * range) : (rangeMin - 1.0));
    // Strictly below the minimum once narrowed to float, guaranteed. On a cube
    // whose values are large — a range of [1e10, 1e10 + 1024], say — one per
    // cent of the span is smaller than the gap between neighbouring floats up
    // there, so the subtraction rounds straight back to rangeMin and the
    // sentinel stops being distinguishable from real data. Anything that later
    // asks "is this a blank?" by comparing against the minimum then answers no
    // — the 3-D picker's MinIP would return a blank.
    {
        const float rangeMinF = static_cast<float>(rangeMin);
        if (!(invisibleSentinel < rangeMinF))
            invisibleSentinel = std::nextafter(rangeMinF, -std::numeric_limits<float>::infinity());
    }

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
    float invisibleSentinel =
            static_cast<float>(range > 0.0 ? (rangeMin - 0.01 * range) : (rangeMin - 1.0));
    // Strictly below the minimum once narrowed to float, guaranteed. On a cube
    // whose values are large — a range of [1e10, 1e10 + 1024], say — one per
    // cent of the span is smaller than the gap between neighbouring floats up
    // there, so the subtraction rounds straight back to rangeMin and the
    // sentinel stops being distinguishable from real data. Anything that later
    // asks "is this a blank?" by comparing against the minimum then answers no
    // — the 3-D picker's MinIP would return a blank.
    {
        const float rangeMinF = static_cast<float>(rangeMin);
        if (!(invisibleSentinel < rangeMinF))
            invisibleSentinel = std::nextafter(rangeMinF, -std::numeric_limits<float>::infinity());
    }

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

/// Assemble the volume from its Z-slabs straight into the image's buffer,
/// releasing each slab once copied, so a multi-GB load holds at most the image
/// plus the slabs still to copy, never a stitched intermediate as well.
vtkSmartPointer<vtkImageData> decodeRemoteVolume(QList<QByteArray> &slabs, int width, int height,
                                                 int depth)
{
    const qsizetype expectedBytes = static_cast<qsizetype>(width) * height
            * static_cast<qsizetype>(depth) * static_cast<qsizetype>(sizeof(float));
    qsizetype total = 0;
    for (const auto &s : slabs)
        total += s.size();
    if (width <= 0 || height <= 0 || depth <= 0 || total != expectedBytes) {
        return nullptr;
    }
    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, width - 1, 0, height - 1, 0, depth - 1);
    image->SetOrigin(0., 0., 0.);
    image->SetSpacing(1., 1., 1.);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto *dst = static_cast<char *>(image->GetScalarPointer());
    for (auto &s : slabs) {
        std::memcpy(dst, s.constData(), static_cast<std::size_t>(s.size()));
        dst += s.size();
        s = QByteArray();   // give the slab back before the next one is copied
    }
    return image;
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
    auto response = client.requestSubvolume(datasetId, roi[0], roi[1], roi[2], roi[3], roi[4],
                                            roi[5], /*keepSlabs=*/true);
    if (!response.valid) {
        result.errorMessage =
                response.error.isEmpty() ? u"Remote subvolume request failed."_s : response.error;
        return result;
    }

    if (response.scalarType != u"float32"_s) {
        result.errorMessage = u"Unsupported remote subvolume scalar type."_s;
        return result;
    }

    // Slabs from the binary route are copied into the image one at a time and
    // released; the JSON fallback (an old backend) still arrives stitched.
    result.cubeImageData = response.slabs.isEmpty()
            ? decodeRemoteVolume(response.data, response.width, response.height, response.depth)
            : decodeRemoteVolume(response.slabs, response.width, response.height, response.depth);
    response.data = QByteArray();
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
    // #5: mint this window's product scope BEFORE any product can be registered — it salts
    // every product id and tags each product's ownerId (see scopedProductId).
    this->m_windowScope = ProductRegistry::instance().newOwnerScope();
    ui->setupUi(this);
    this->setupViewerToolbar();

    // Pre-create the QActions and QActionGroups the sidebar's "Cube Extras"
    // page binds to. Menu placement, tooltips, presets and signal handlers
    // are configured further down in the constructor — but the pointers must
    // exist before setupSidebar() so the sidebar can wire its widgets.
    this->actionShowCuttingPlane = new QAction(u"Show Cutting Plane"_s, this);
    this->actionShowCuttingPlane->setCheckable(true);
    this->actionShowCuttingPlane->setChecked(true);
    this->actionShow3dWcsAxes = new QAction(u"WCS Box in the 3-D View"_s, this);
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
    // "Show WCS Axes" and "Show 3D WCS Axes" are different overlays in different
    // views, and sitting next to each other in the Display group they read as
    // one entry duplicated. Each says which view it belongs to.
    this->actionShowWcsAxes = new QAction(u"Sky Grid on the 2-D Panes"_s, this);
    this->actionShowWcsAxes->setCheckable(true);
    this->actionShowWcsAxes->setChecked(this->showWcsAxes);
    this->actionShowWcsAxes->setToolTip(
            u"Draw the RA/Dec (or Galactic, or ecliptic) grid and tick labels over "
            u"the slice and moment panes. The frame and the format come from the "
            u"WCS menu."_s);
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
    m_actFocusMode = focusAct;
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
                this->filepath, QFileInfo(this->filepath).fileName(), u"cube"_s,
                this->m_windowScope);
        // Tree-selection = pane-selection: activate (or mount) the matching pane.
        connect(this->m_workspaceChrome, &WorkspaceChrome::sessionRowActivated, this,
                &vtkWindowCube::activateSessionView);
        // …and selecting a row focuses the pane that already shows it, so the dock's
        // DISPLAY section describes the row you just highlighted.
        connect(this->m_workspaceChrome, &WorkspaceChrome::sessionRowSelected, this,
                &vtkWindowCube::focusSessionViewIfVisible);
        // (#4) A pane-number badge click activates that pane (badge is 1-based).
        connect(this->m_workspaceChrome, &WorkspaceChrome::sessionPaneBadgeActivated, this,
                [this](int paneNumber) { setActivePane(paneNumber - 1); });
        // "Show in ▸": right-click a product → mount it in a chosen pane.
        connect(this->m_workspaceChrome, &WorkspaceChrome::sessionRowContextMenu, this,
                &vtkWindowCube::showSessionRowContextMenu);
        connect(this->m_workspaceChrome, &WorkspaceChrome::sessionProductReopen, this,
                &vtkWindowCube::reopenProductFromFile);
        // Reverse sync: activating a pane by any route highlights its Session-tree row.
        connect(this, &vtkWindowCube::activePaneChanged, this, [this](int) {
            syncSessionTreeToActivePane();
            applyMomentToolGate();   // §5.1b: probe/region/PV disabled while a moment is active
            updateActiveMapStats();   // 2D stats block follows the active pane
            updateSpectrumInspectorControls();   // Spectrum panel follows the active pane
        });
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
        this->setRemoteCuttingPlaneVisible(checked);
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
        m_menuCuttingPlaneOpacity = opacityMenu;
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
        m_actLoadFullRes = loadFullAction;
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
                // Two different situations, and this dialog used to describe
                // only the remote one — "without the transfer" and "downloading
                // to this desktop" are both meaningless when the backend is a
                // process on this machine, where the honest cost is memory and
                // the honest default is to load it (user).
                const bool localBackend = this->backendIsLocal();
                QMessageBox box(this);
                box.setIcon(QMessageBox::Warning);
                box.setWindowTitle(
                        (localBackend || !this->m_serverSideRenderingAvailable)
                                ? u"Large cube - this needs the whole volume in memory"_s
                                : u"Large cube, remote rendering recommended"_s);
                box.setText(localBackend
                        ? QStringLiteral(
                                  "This cube is %1 GB at full resolution, above the %2 MB "
                                  "local-load threshold.\n\nThe backend is on this machine, so "
                                  "nothing is downloaded over a network - but the whole volume "
                                  "has to be resident here, in RAM and as a 3-D GPU texture.\n\n"
                                  "%3")
                                  .arg(warnCubeMb / 1024.0, 0, 'f', 2)
                                  .arg(warnThreshold)
                                  .arg(this->m_serverSideRenderingAvailable
                                               ? u"Rendering it in the backend process instead "
                                                 u"uses the same GPU for the same volume and "
                                                 u"adds a per-frame encode; what it avoids is "
                                                 u"holding the cube in THIS process."_s
                                               : u"This backend cannot render server-side (no "
                                                 u"EGL context), so loading it here is the only "
                                                 u"way to the full grid."_s)
                        : QStringLiteral(
                                  "This cube is %1 GB at full resolution, well above the "
                                  "%2 MB local-load threshold.\n\nDownloading the full "
                                  "cube to this desktop will use the full size in RAM "
                                  "and may take a long time.\n\n%3")
                                  .arg(warnCubeMb / 1024.0, 0, 'f', 2)
                                  .arg(warnThreshold)
                                  .arg(this->m_serverSideRenderingAvailable
                                               ? u"Remote rendering (GPU on the backend) is "
                                                 u"already configured and will give you "
                                                 u"full-quality, fully interactive 3D without "
                                                 u"the transfer."_s
                                               : u"This backend cannot render server-side (no "
                                                 u"EGL context), so there is no alternative to "
                                                 u"the download, consider Crop Full Resolution "
                                                 u"for a region instead."_s));
                // Only when the backend can actually do it: with no EGL there,
                // this button led straight into a refused request (verified —
                // the probe returns false on macOS).
                QPushButton *btnRemote =
                        this->m_serverSideRenderingAvailable
                                ? box.addButton(u"Use Remote Rendering"_s,
                                                QMessageBox::AcceptRole)
                                : nullptr;
                auto *btnLocal = box.addButton(
                        localBackend ? u"Load It Here"_s : u"Download Locally Anyway"_s,
                        localBackend ? QMessageBox::AcceptRole
                                     : QMessageBox::DestructiveRole);
                box.addButton(QMessageBox::Cancel);
                box.setDefaultButton((localBackend || !btnRemote) ? btnLocal : btnRemote);
                box.exec();
                if (btnRemote && box.clickedButton() == btnRemote) {
                    this->applyRenderMode(RenderMode::Remote, true);
                    return;
                }
                if (box.clickedButton() != btnLocal) {
                    return;  // Cancel or [X]
                }
                // Falls through to the local fetch path below.
            }
            this->startFullResolutionLoad();
        });

        auto *cropFullAction =
                ui->menuView->addAction(u"Crop Full Resolution…"_s);
        m_actCropFullRes = cropFullAction;
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
        m_menuAnimationSpeed = fpsMenu;
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
        m_menuAnimationMode = modeMenu;
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
    // NOT in View: this is a tool, and it is listed in Tools ▸ Spectral and in
    // Inspector ▸ Analysis, where the gate can say whether the 3-D view it needs
    // is even on screen.
    QObject::connect(this->actionPickSpectrum3d, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            // Give the 3-D pane the focus first. Otherwise the pane container
            // swallows the activating press and the user has to click twice —
            // which reads as the tool ignoring the first click.
            for (int i = 0; i < 4; ++i)
                if (m_slots[i].view.kind == ViewId::View3D && m_slots[i].frame
                    && m_slots[i].frame->isVisible()) {
                    this->setActivePane(i);
                    break;
                }
            this->install3dPickObserver();
            ui->vtkCube->setCursor(Qt::CrossCursor);
            this->statusBar()->showMessage(
                    u"3D pick armed: click the volume, the isosurface or the cutting "
                    u"plane. The blend mode decides which voxel along the ray you "
                    u"get - MIP the brightest, Composite the front surface."_s,
                    6000);
        } else {
            this->remove3dPickObserver();
            ui->vtkCube->unsetCursor();
            this->hidePick3dMarker();
            this->statusBar()->clearMessage();
            // The picker STARTS the live spectrum when nothing else has (see
            // handle3dPickEvent), so it must end it too — otherwise the live
            // product outlives the tool and, now that placement may borrow a
            // pane, keeps that pane lent for good.
            //
            // The owner test is the ACTION, not probeModeActive: a successful
            // pick sets that flag itself (handle3dPickEvent, deliberately, so
            // updateProbePlot does not early-return), so it is true here even
            // when Extract Spectrum was never armed.
            if (!ui->actionExtractSpectrum->isChecked()) {
                this->probeModeActive = false;   // the pick's own flag, undone with it
                this->stopLiveSpectrum();
                // …and the crosshairs it drew: the overlay is drawn on the probe
                // flag, so clearing the flag without a redraw leaves a marker on
                // the slice for a probe that is no longer running.
                this->probeFrozen = false;
                this->refreshProbeOverlay();
                if (auto *win = this->active2dWin())
                    win->Render();
            }
        }
        this->refreshPaneActiveStyles();   // paint / clear the armed border
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
    m_actDiagnostics = actionDiagnostics;
    QObject::connect(actionDiagnostics, &QAction::triggered, this,
                     []() { DiagnosticsWindow::showSingleton(); });

    auto *actionCommandPalette = new QAction(u"Command Palette…"_s, this);
    actionCommandPalette->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    actionCommandPalette->setShortcutContext(Qt::ApplicationShortcut);
    actionCommandPalette->setToolTip(
            u"Quick action search, same palette as the main window (⌘K)."_s);
    ui->menuView->addAction(actionCommandPalette);
    m_actCommandPalette = actionCommandPalette;
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
    ui->menuCamera->addSeparator();
    ui->menuCamera->addAction(this->actionCameraRoi);
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
                         const bool hadHighRes = this->usingHighResCube;
                         this->usingHighResCube = false;
                         qDebug().noquote()
                                 << QStringLiteral("[remote-roi] starting refinement reload");
                         if (!this->requestHighResCube()) {
                             // Refused (not enough memory for the new region): the
                             // image on screen is still the one loaded before.
                             this->usingHighResCube = hadHighRes;
                         }
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
    // Attached here, not on the first slice. Deferring it looked tidier — the
    // texture is transparent until a slice arrives — but the deferred path is
    // not reached in every flow, and the plane then showed its border and
    // never the data. The border actor covers the empty-texture case instead,
    // without touching when the texture is bound.

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
                             qDebug().noquote() << QStringLiteral("[preview-debug] stale requestId=%1 vs current=%2, return")
                                                           .arg(requestId).arg(this->currentRemotePreviewRequestId);
                             return;
                         }

                         this->setCubeOpenActionsEnabled(true);

                         const auto result = this->remotePreviewWatcher.result();
                         // The preview is fetched on open and can take tens of seconds
                         // through a tunnel. If the view switched to rendering on the
                         // backend meanwhile, applying it now would push the proxy's
                         // threshold, render mode and slice into that session and
                         // start work (slice, isosurface, full-resolution fetches)
                         // that keeps doing so afterwards. Keep it for a switch back
                         // to Local, which is the only place it is shown.
                         if (result.valid && result.cubeImageData
                             && (this->m_effectiveRenderMode == RenderMode::Remote
                                 || this->m_remoteSwitchInProgress)) {
                             this->m_deferredPreview = result;
                             // What the remote session needs from it, and nothing
                             // the session would be told: the data range the
                             // threshold controls work in (still the 0..1
                             // placeholder until now) and the 2-D slice at the
                             // channel on screen.
                             const double span = result.cubeRange[1] - result.cubeRange[0];
                             this->lowerBound = static_cast<float>(result.cubeRange[0] + 0.03 * span);
                             this->upperBound = static_cast<float>(result.cubeRange[1]);
                             this->refreshContourBoundsFromData();
                             this->requestRemoteSlice(this->clampRemoteSliceIndex(ui->spinSlice->value() - 1));
                             qDebug().noquote() << QStringLiteral(
                                     "[remote-plane] preview valid, deferred: the view renders on the backend");
                             return;
                         }
                         this->applyRemotePreviewResult(result);
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
                         this->m_deferredPreview.reset();   // superseded by the full grid
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
                        u"Mask"_s, MomentMapController::describeMask(config));
                this->m_momentProvenanceParams.insert(u"Scope"_s,
                                                      MomentMapController::describeMomentScope());
                this->m_momentProvenanceParams.insert(
                        u"Unit"_s, result.momentUnit.isEmpty() ? u"-"_s : result.momentUnit);

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
    // One place that reacts to a moment computation actually STARTING, for both
    // entry points (the 2-D mode toggle and the moment-order actions).
    QObject::connect(this->momentController, &MomentMapController::computationStarted, this,
                     [this](const MomentMapController::Config &cfg) {
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
                     });

    // The estimate is a product like any other: a statistics row in Session Data
    // whose Provenance holds σ/MAD. No renderer — there is nothing to draw in a
    // pane — and no Provenance reveal, because the tool's own result dialog is
    // already opening on top.
    QObject::connect(this->noiseController, &NoiseController::noiseProvenanceReady, this,
                     [this](const QString &stableKey, const QString &label,
                            const QVariantMap &params) {
                         m_lastStatisticsProductId = this->registerAnalysisProduct(
                                 u"noise"_s, label, params,
                                 /*window=*/nullptr,
                                 this->scopedProductId(stableKey),
                                 /*renderer=*/QString(),
                                 /*revealProvenance=*/false);
                     });
    // σ, measured once, is the currency of the tools that need a noise level:
    // the moment mask, the line-width mask and the Kinematic Lasso use it from
    // now on (each says so where its threshold is set).
    QObject::connect(this->noiseController, &NoiseController::noiseEstimated, this,
                     [this](double sigma, const QString &where) {
                         this->m_noiseSigma = sigma;
                         this->m_noiseWhere = where;
                         if (this->momentController)
                             this->momentController->setNoiseEstimate(sigma, where);
                         this->m_lassoLevels = {};   // σ scales the lasso's cost
                         this->refreshLassoNoiseLabel();
                         // A selection grown (or growing) with the old σ is grown again,
                         // or Isolate would record a σ its mask was not made with; a grow
                         // in flight is marked pending and Isolate waits for it.
                         if (this->m_lassoHasSeed)
                             this->requestLassoSelection();
                     });
    // …and the recipe of the window that reported it, emitted right after, so
    // reopening the row shows that window again rather than a reconstruction.
    QObject::connect(this->noiseController, &NoiseController::noiseResultViewReady, this,
                     [this](const QVariantMap &record) {
                         if (!m_lastStatisticsProductId.isEmpty())
                             ProductRegistry::instance().setResultView(
                                     m_lastStatisticsProductId, record);
                     });

    // ── PV diagram backend fetch – delegated to PvController ─────────────────
    this->pvController = new PvController(
            this->remoteBackendUrl, this->remoteDatasetId,
            this->remoteSessionId, this->remoteBackendToken,
            this,
            [this]() { this->updateBackendDerivedActionsAvailability(); },
            [this](const RemotePvFetchResult &result) {
                // The Y axis: the backend's own channel coordinates (the same axis
                // its CTYPE label names, channel 0 first, possibly decreasing). The
                // local FITS-axis-3 reconstruction is only the fallback for an older
                // backend — it can disagree with the label on a cube whose spectral
                // axis is not NAXIS3.
                QVector<double> spectral = result.spectralCoordinates;
                if (spectral.size() != result.depth) {
                    spectral.resize(result.depth);
                    for (int z = 0; z < result.depth; ++z) {
                        bool ok = false;
                        spectral[z] = this->spectralAxisValue(static_cast<double>(z), &ok);
                        if (!ok) {
                            spectral[z] = static_cast<double>(z);
                        }
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
                pvInfo.bunit = result.bunit;
                if (result.skyMatrixArcsec.size() == 4)
                    this->m_pvSkyMatrix = { result.skyMatrixArcsec[0], result.skyMatrixArcsec[1],
                                            result.skyMatrixArcsec[2], result.skyMatrixArcsec[3] };
                // What Save as FITS exports: the cut this diagram shows.
                this->m_pvShownVertices = result.capturedVertices;
                this->m_pvShownWidth = result.capturedWidthPixels;

                // Create PvDiagramWidget lazily on first successful extraction.
                if (!this->pvDiagramWidget) {
                    qDebug().noquote() << QStringLiteral("[pv] creating PvDiagramWidget");
                    this->pvDiagramWidget = new PvDiagramWidget(this);
                    this->pvDiagramWidget->applyPlotTheme(this->m_darkPlots);
                    // #5: host the widget IN a pane, not as a floating window — clear the
                    // Qt::Window flag + WA_DeleteOnClose the ctor set (mirrors ensureSlotSpectrumView
                    // for ProfileWidget), and click-to-activate the hosting pane (§5.1).
                    this->pvDiagramWidget->setWindowFlags(Qt::Widget);
                    this->pvDiagramWidget->setAttribute(Qt::WA_DeleteOnClose, false);
                    this->pvDiagramWidget->installEventFilter(this);
                    // Qt event filters are NOT recursive: a click lands on the child QCustomPlot
                    // (the PV body), not pvDiagramWidget itself, so filter the descendants too or
                    // a non-active PV pane's body would not click-to-activate. The
                    // eventFilter's isAncestorOf() check then confirms which slot owns the click.
                    const auto pvKids = this->pvDiagramWidget->findChildren<QWidget *>();
                    for (QWidget *kid : pvKids)
                        kid->installEventFilter(this);
                    // Path controls inline in the diagram: every change recomputes.
                    QObject::connect(this->pvDiagramWidget, &PvDiagramWidget::widthChanged, this,
                                     [this](int width) {
                                         this->pvWidthPixels = std::max(1, width);
                                         this->refreshPvOverlay();
                                         if (auto *win = this->active2dWin())
                                             win->Render();
                                         this->schedulePvLiveUpdate(0);
                                     });
                    QObject::connect(this->pvDiagramWidget, &PvDiagramWidget::smoothChanged, this,
                                     [this](bool smooth) {
                                         this->m_pvSmooth = smooth;
                                         this->refreshPvOverlay();
                                         if (auto *win = this->active2dWin())
                                             win->Render();
                                         this->schedulePvLiveUpdate(0);
                                     });
                    QObject::connect(this->pvDiagramWidget, &PvDiagramWidget::newPathRequested,
                                     this, &vtkWindowCube::startNewPvPath);
                    QObject::connect(this->pvDiagramWidget, &PvDiagramWidget::saveFitsRequested,
                                     this, &vtkWindowCube::exportPvAsFits);
                    QObject::connect(
                            this->pvDiagramWidget, &PvDiagramWidget::destroyed, this,
                            [this]() {
                                this->pvDiagramWidget = nullptr;
                                this->m_pvProductId.clear();
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

                // PV mode stays ON: the finished path remains editable and the
                // diagram follows it. Turning PV mode off keeps the path visible
                // (pvDiagramWidget is non-null now) but frozen; on again resumes.
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

                // #5: register the PV as a first-class pane product (renderer "pvdiagram") and
                // host the widget in a pane. Determine the target pane BEFORE setPvData so its
                // internal show() lands inside a visible pane (not as a stray top-level window):
                //   • recompute (a visible pane already shows it) → refresh that pane in place,
                //   • else first FREE visible pane (no-steal),
                //   • else park it hidden and let the tree row activate it later.
                this->m_pvProductId = this->scopedProductId(u"pv"_s);
                // The cut it was taken along. This product registered NO
                // parameters at all, so its row said "PV diagram" and nothing
                // else: with two cuts taken, neither row could be told from the
                // other, and neither said where on the sky it ran.
                // From the CAPTURED request, never the live drawing state: the
                // user can start a new path while the extraction runs, and the
                // row would then describe a cut this diagram was not taken along.
                const auto &pvVerts = result.capturedVertices.empty() ? this->pvPolylineVertices
                                                                      : result.capturedVertices;
                const int pvWidth = result.capturedVertices.empty() ? this->pvWidthPixels
                                                                    : result.capturedWidthPixels;
                const QVariantMap pvOrigin = visivo::encodePvPathOrigin(pvVerts, pvWidth);
                QVariantMap pvParams;
                pvParams.insert(u"Path"_s, u"%1 points"_s.arg(pvVerts.size()));
                pvParams.insert(u"Width"_s, u"%1 px"_s.arg(pvWidth));
                if (!pvVerts.empty()) {
                    const auto &a = pvVerts.front();
                    const auto &b = pvVerts.back();
                    pvParams.insert(u"From"_s, u"(%1, %2)"_s.arg(a[0], 0, 'f', 1).arg(a[1], 0, 'f', 1));
                    pvParams.insert(u"To"_s, u"(%1, %2)"_s.arg(b[0], 0, 'f', 1).arg(b[1], 0, 'f', 1));
                }
                pvParams.insert(u"Measured on"_s, visivo::describeOrigin(pvOrigin));
                auto pvVisible = [this](int i) {
                    return m_slots[i].frame && m_slots[i].frame->isVisible();
                };
                int pvTarget = -1;
                for (int i = 0; i < 4; ++i)
                    if (pvVisible(i)
                        && m_slots[i].view == PaneView{ ViewId::Product, this->m_pvProductId }) {
                        pvTarget = i;   // already mounted somewhere → refresh in place
                        break;
                    }
                // A diagram already on screen is being REFRESHED (a live edit every
                // fraction of a second): update it in place. Selecting the product,
                // opening its provenance and moving the focus to its pane belong to the
                // first result only — mid-drag they would pull the pane, the dock and
                // the Inspector away from the path being edited.
                const bool refreshInPlace = pvTarget >= 0;
                this->registerAnalysisProduct(u"pv"_s, u"PV diagram"_s, pvParams,
                                              this->pvDiagramWidget, this->m_pvProductId,
                                              u"pvdiagram"_s, /*revealProvenance=*/!refreshInPlace,
                                              pvOrigin, /*selectInSession=*/!refreshInPlace);
                // A first result goes where it can be SEEN, like a region spectrum: a
                // free pane, else the layout grows (1→2→4); never borrowed. With every
                // pane taken it used to be parked out of sight.
                if (pvTarget < 0)
                    pvTarget = acquirePaneForProduct(this->m_pvProductId, /*mayBorrow=*/false);
                if (pvTarget >= 0) {
                    // A hidden slot may still claim PV from a prior layout; drop those (else a
                    // later layout expansion could re-surface the stale claim and move PV away
                    // from where recompute placed it).
                    clearViewFromOtherSlots(pvTarget);
                    // The focus stays on the pane the path is drawn in: the next thing
                    // the user does is edit it there.
                    if (!refreshInPlace)
                        mountView(pvTarget);        // embeds the widget into the pane host
                } else {
                    // No free pane: park the widget hidden (its parent becomes the hidden
                    // holder) so setPvData's show() can't pop a floating window; the tree row
                    // waits for manual activation.
                    this->pvDiagramWidget->setParent(m_viewParkingLot);
                    if (auto *l = m_viewParkingLot->layout())
                        l->addWidget(this->pvDiagramWidget);
                    this->pvDiagramWidget->hide();
                }

                qDebug().noquote()
                        << QStringLiteral("[pv] updating widget (hosted in pane)");
                this->pvDiagramWidget->setPathControls(this->pvWidthPixels, this->m_pvSmooth);
                this->pvDiagramWidget->setPvData(pvPositions, spectral, result.values,
                                                 result.numSamples, result.depth,
                                                 xLabel, yLabel,
                                                 this->m_pvSmooth ? u"PV Diagram (curve)"_s
                                                                  : u"PV Diagram (polyline)"_s,
                                                 pvInfo);
                this->clearPersistentStatusMessage();
                refreshPaneChrome();
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

    // ── Camera ───────────────────────────────────────────────────────────────
    // Six standard viewpoints, each with its icon and a number shortcut, and
    // below them what the camera FRAMES (the ROI refinement toggle, which used
    // to sit at the bottom of an already overloaded View menu). Nothing else
    // belongs here: this menu answers "where am I looking from".
    ui->actionCameraFront->setIcon(QIcon(u":/icons/PIC_FRONT.png"_s));
    ui->actionCameraBack->setIcon(QIcon(u":/icons/PIC_BACK.png"_s));
    ui->actionCameraTop->setIcon(QIcon(u":/icons/PIC_TOP.png"_s));
    ui->actionCameraRight->setIcon(QIcon(u":/icons/PIC_RIGHT.png"_s));
    ui->actionCameraBottom->setIcon(QIcon(u":/icons/PIC_BOTTOM.png"_s));
    ui->actionCameraLeft->setIcon(QIcon(u":/icons/PIC_LEFT.png"_s));
    // Ctrl+1..6, not bare 1..6: the window is full of spin boxes and a channel
    // slider that legitimately take digits when focused.
    ui->actionCameraFront->setShortcut(QKeySequence(u"Ctrl+1"_s));
    ui->actionCameraBack->setShortcut(QKeySequence(u"Ctrl+2"_s));
    ui->actionCameraLeft->setShortcut(QKeySequence(u"Ctrl+3"_s));
    ui->actionCameraRight->setShortcut(QKeySequence(u"Ctrl+4"_s));
    ui->actionCameraTop->setShortcut(QKeySequence(u"Ctrl+5"_s));
    ui->actionCameraBottom->setShortcut(QKeySequence(u"Ctrl+6"_s));
    for (QAction *a : { ui->actionCameraFront, ui->actionCameraBack, ui->actionCameraLeft,
                        ui->actionCameraRight, ui->actionCameraTop, ui->actionCameraBottom })
        a->setToolTip(u"Look at the cube from %1 (%2)."_s
                              .arg(a->text().remove(u'&').toLower(),
                                   a->shortcut().toString(QKeySequence::NativeText)));
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

    // ── Edit ─────────────────────────────────────────────────────────────────
    // It held one entry, "Edit LUT", which is a menu that exists to hold a menu
    // item. It now holds what the window can actually undo — the copilot's
    // display changes, which were reversible all along but reachable only from
    // the assistant panel — plus the colour-map editor, renamed to match the
    // "Color map" control it opens in the left dock.
    ui->actionEditLUT->setText(u"Edit Color Map…"_s);
    ui->actionEditLUT->setIcon(QIcon(u":/icons/COLORIZE.png"_s));
    ui->actionEditLUT->setToolTip(
            u"Open the colour-map editor for the 3-D volume: stops, opacity curve "
            u"and range."_s);
    QObject::connect(ui->actionEditLUT, &QAction::triggered, this,
                     &vtkWindowCube::showLUTCustomizer);

    m_actUndoCopilot = new QAction(u"Undo Copilot Display Changes"_s, this);
    m_actUndoCopilot->setShortcut(QKeySequence::Undo);
    m_actUndoCopilot->setToolTip(
            u"Put the display back as it was before the assistant's last batch of "
            u"changes (colour map, threshold, render and blend mode, channel). It "
            u"does not undo computed products."_s);
    m_actUndoCopilot->setEnabled(false);   // nothing to undo until the copilot acts
    QObject::connect(m_actUndoCopilot, &QAction::triggered, this,
                     &vtkWindowCube::undoCopilotActions);
    ui->menuEdit->insertAction(ui->actionEditLUT, m_actUndoCopilot);
    ui->menuEdit->insertSeparator(ui->actionEditLUT);

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
        if (this->m_noiseSigma > 0.0)
            dlg->setNoiseEstimate(this->m_noiseSigma, this->m_noiseWhere);
        dlg->setRestFrequencyHz(this->m_restFreqHz);
        // A later re-estimate reaches every open line-width window (each one is
        // the connection's context, so a closed window simply drops out).
        if (this->noiseController)
            QObject::connect(this->noiseController, &NoiseController::noiseEstimated, dlg,
                             &LinewidthDialog::setNoiseEstimate);
        // #5: the Line-width map is a first-class PANE product now — the dialog emits the raw
        // float map on completion; build a vtkImageData (same layout as a moment) + register.
        QObject::connect(dlg, &LinewidthDialog::mapReady, this,
                         [this](const QByteArray &data, int w, int h, float /*rmin*/,
                                float /*rmax*/, const QString &label, const QString &unit,
                                const QVariantMap &recipe) {
                             if (w <= 0 || h <= 0
                                 || data.size()
                                         != static_cast<qsizetype>(w) * h
                                                 * static_cast<qsizetype>(sizeof(float)))
                                 return;
                             auto img = vtkSmartPointer<vtkImageData>::New();
                             img->SetExtent(0, w - 1, 0, h - 1, 0, 0);
                             img->AllocateScalars(VTK_FLOAT, 1);
                             std::memcpy(img->GetScalarPointer(), data.constData(),
                                         static_cast<std::size_t>(w) * h * sizeof(float));
                             // FWHM and EW are distinct maps → distinct stable ids (a re-run of
                             // the same method updates its product, like a moment recompute).
                             const QString key = label.startsWith(u"FWHM"_s) ? u"linewidth_fwhm"_s
                                                                             : u"linewidth_ew"_s;
                             QVariantMap params = recipe;   // method, channels, mask
                             params.insert(u"Type"_s, label);
                             if (!unit.isEmpty())
                                 params.insert(u"Unit"_s, unit);
                             // A line-width map is ≥ 0 (a width) → sequential Viridis, like mom2.
                             addImageProduct(u"linewidth"_s, key, label, img, unit, params,
                                             u"Viridis"_s);
                         });
        dlg->show();
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
        // The row is registered when the tool OPENS (so it is visible while it
        // runs), and completed when the result lands. Both matter:
        //
        //  * `window` is a QPointer to the dialog, so once the user closes it the
        //    row's only action was gone — right-click offered nothing, double
        //    click did nothing, and the result was unreachable for ever;
        //  * the params were EMPTY, so the new dataset id and the output path —
        //    which the dialog showed once in a popup and then forgot — were not
        //    written down anywhere. The cube itself was on disk the whole time,
        //    in the backend's temp directory (user report).
        //
        // The dialog's own baselineResultReady signal existed for this and was
        // connected to nothing at all, its comment notwithstanding.
        const QString baselineId =
                this->registerAnalysisProduct(u"baseline"_s, u"Baseline-subtracted cube"_s,
                                              { { u"Status"_s, u"running…"_s } }, dlg);
        QObject::connect(dlg, &BaselineDialog::baselineResultReady, this,
                         [this, baselineId](const QString &newDatasetId, const QString &outputPath,
                                            const QString &summary) {
                             this->completeBaselineProduct(baselineId, newDatasetId, outputPath,
                                                           summary);
                         });
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
                                 // the backend did not report a path. The path travels too,
                                 // so StackDialog can collapse the rows that are the same
                                 // FILE registered twice.
                                 const QString fileName = QFileInfo(d.path).fileName();
                                 const QString label = fileName.isEmpty() ? d.datasetId : fileName;
                                 entries.push_back({ d.datasetId, d.kind, label,
                                                     d.width, d.height, d.depth, d.path });
                             }
                             // Shared ownership: the async stack run captures a copy
                             // of the shared_ptr, so the client survives an early
                             // dialog close (no use-after-free).
                             auto client = std::make_shared<BackendClient>(url, token);
                             client->setSessionId(sessionId);
                             auto *dlg = new StackDialog(client, sessionId, entries,
                                                         currentId, self);
                             dlg->setAttribute(Qt::WA_DeleteOnClose);
                             // #5: the stacked spectrum becomes a first-class SPEC pane product
                             // (not a standalone plot window) — same pattern as Line-width for
                             // images. The stacked channel axis has no shared spectral WCS (the
                             // inputs are different cubes), so x is channel index.
                             QObject::connect(
                                     dlg, &StackDialog::spectrumReady, self,
                                     [self](const QVector<double> &values, const QString &label,
                                            const QVariantMap &meta) {
                                         QVector<double> chan(values.size());
                                         for (int i = 0; i < values.size(); ++i)
                                             chan[i] = static_cast<double>(i);
                                         // Each stack is a DISTINCT product (compare two stacks),
                                         // so the stableKey carries an incrementing suffix.
                                         const QString key =
                                                 u"stack%1"_s.arg(++self->m_stackCount);
                                         self->addSpectrumProduct(u"stack"_s, key, label, chan,
                                                                  values, u"Channel"_s, u"Flux"_s,
                                                                  meta);
                                     });
                             // Non-modal to stay consistent with the other tool
                             // dialogs (Linewidth, Baseline, LUT customizer); the
                             // long-running compute already shows its own modal
                             // QProgressDialog so the wait is still indicated.
                             dlg->show();
                         });
        watcher->setFuture(QtConcurrent::run([url, token, sessionId]() {
            BackendClient c(url, token);
            c.setSessionId(sessionId);
            return c.listSessionDatasets(sessionId);
        }));
    });

    this->updateBackendDerivedActionsAvailability();

    // ── WCS ──────────────────────────────────────────────────────────────────
    // Two exclusive choices about the same thing, the sky coordinates drawn on
    // the overlay: which FRAME they are in, and how they are FORMATTED. Both are
    // meaningless without celestial axes, so the whole menu switches off for a
    // cube that has none — and, as everywhere else, it says why rather than just
    // greying out (a disabled menu shows its tooltip; a disabled menu BAR entry
    // does not, so the reason goes in the title).
    auto groupWCS = new QActionGroup(this);
    groupWCS->addAction(ui->actionGalactic);
    groupWCS->addAction(ui->actionFK5);
    groupWCS->addAction(ui->actionEcliptic);
    const bool hasCelestial = this->remoteHasCelestialAxes();
    ui->menuWCS->setEnabled(hasCelestial);
    // The two overlays the WCS menu configures go with it: a sky grid with no
    // sky is an entry that can only disappoint.
    for (QAction *a : { this->actionShowWcsAxes.data(), this->actionShow3dWcsAxes.data() }) {
        if (!a || hasCelestial)
            continue;
        a->setEnabled(false);
        a->setToolTip(u"This cube's spatial axes are not celestial, so there are no "
                      u"sky coordinates to draw."_s);
    }
    if (!hasCelestial) {
        ui->menuWCS->setTitle(u"WCS (no celestial axes)"_s);
        ui->menuWCS->setToolTip(
                u"This cube's spatial axes are not celestial (no RA/Dec, Galactic or "
                u"ecliptic coordinates to draw), so there is no frame or format to "
                u"choose."_s);
    }
    QObject::connect(groupWCS, &QActionGroup::triggered, this, &vtkWindowCube::changeLegendWCS);
    for (QAction *a : { ui->actionGalactic, ui->actionFK5, ui->actionEcliptic })
        a->setToolTip(u"Draw the sky overlay in %1 coordinates."_s
                              .arg(a->text().remove(u'&')));
    ui->menuWCS->addSeparator();
    auto *formatGroup = new QActionGroup(this);
    this->actionWcsSexagesimal = ui->menuWCS->addAction(u"Sexagesimal"_s);
    this->actionWcsDecimal = ui->menuWCS->addAction(u"Decimal"_s);
    this->actionWcsSexagesimal->setCheckable(true);
    this->actionWcsDecimal->setCheckable(true);
    this->actionWcsSexagesimal->setToolTip(
            u"Label the overlay as hours/degrees, minutes and seconds (12h34m56s)."_s);
    this->actionWcsDecimal->setToolTip(u"Label the overlay in decimal degrees (188.7354°)."_s);
    ui->menuWCS->setToolTipsVisible(true);
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
                                     u"The cube is not ready yet, wait for the "
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
        // Outside the branch: the .ui supplies a separator, so construction takes
        // the insertAction path and recording it only in the else left the member
        // null — cubeTools() then skipped it and the regroup dropped it from the
        // menu too, so "Open in VR" vanished from all three lists at once.
        this->m_actOpenInVR = actOpenInVR;
    }

    // ANALYSIS: Estimate Noise goes at the head of the group (before Compute
    // Moment), to match the sidebar's "Estimate Noise → Moment Map" order.
    this->actionEstimateNoise = new QAction(u"Estimate Noise..."_s, ui->menuTools);
    QObject::connect(this->actionEstimateNoise, &QAction::triggered,
                     this->noiseController, &NoiseController::startEstimation);
    ui->menuTools->insertAction(ui->actionComputeMoment, this->actionEstimateNoise);
    // §5.1b: show action tooltips on hover in the Tools menu so a disabled tool states its
    // reason ("a moment map is collapsed in velocity …"), not just a status-bar tip.
    ui->menuTools->setToolTipsVisible(true);

    // ANALYSIS: Extract PV Diagram goes at the tail of the group (after Stack
    // Spectral Cubes, which is currently the last action defined in .ui).
    this->actionExtractPvDiagram = ui->menuTools->addAction(u"Extract PV Diagram…"_s);
    this->actionExtractPvDiagram->setCheckable(true);
    QObject::connect(this->actionExtractPvDiagram, &QAction::toggled, this,
                     &vtkWindowCube::setPvModeActive);

    // #5: Pin Spectrum — promote the current probe spectrum to a first-class product (SPEC)
    // that lives in Session Data + any pane's dropdown, not just inside the probe plot.
    {
        QAction *pinSpectrum = ui->menuTools->addAction(u"Pin Spectrum"_s);
        this->m_actPinSpectrum = pinSpectrum;   // listed in cubeTools()
        pinSpectrum->setToolTip(
                u"Promote the current probe spectrum to a product in Session Data"_s);
        QObject::connect(pinSpectrum, &QAction::triggered, this,
                         &vtkWindowCube::pinCurrentSpectrum);
    }

    // ANALYSIS: Channel Maps mosaic (N × M grid of consecutive slice panels).
    this->m_actChannelMaps = ui->menuTools->addAction(u"Channel Maps…"_s);
    QAction *actChannelMaps = this->m_actChannelMaps;
    actChannelMaps->setToolTip(
            u"Display a mosaic of N consecutive channel slices with the same "
            u"color scale, useful for identifying coherent structures across "
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
        // The recipe, not just the name: this row recorded nothing, so two runs
        // over different channel ranges were indistinguishable in the tree.
        QVariantMap cmParams;
        cmParams.insert(u"Channels"_s, u"%1..%2"_s.arg(cfg.channelStart + 1).arg(cfg.channelEnd + 1));
        cmParams.insert(u"Stride"_s, QString::number(cfg.stride));
        cmParams.insert(u"Grid"_s, u"%1 columns"_s.arg(cfg.columns));
        cmParams.insert(u"Colour map"_s, cfg.lutName);
        cmParams.insert(u"Scale"_s, cfg.sharedRange ? u"shared across panels"_s
                                                    : u"per panel"_s);
        this->registerAnalysisProduct(u"channelmaps"_s,
                                      u"Channel maps %1..%2"_s.arg(cfg.channelStart + 1)
                                              .arg(cfg.channelEnd + 1),
                                      cmParams, win);
    });

    // ANALYSIS: persist-to-FITS exports (sub-cube crop + moment map). These
    // round-trip the heavy work through the backend exactly like the rest
    // of the ANALYSIS group, so the UI thread stays responsive while the
    // FITS is being written.
    // Smooth / regrid — the same backend operation the image viewer offers, on
    // every plane of the cube. Spatial only: smoothing along the spectral axis
    // would be a different instrument, not a filter.
    m_actFilterFits = ui->menuTools->addAction(u"Smooth / Regrid…"_s);
    m_actFilterFits->setToolTip(
            u"Gaussian-smooth and/or shrink every plane of this cube on the backend, "
            u"into a new FITS file in the Workspace."_s);
    QObject::connect(m_actFilterFits, &QAction::triggered, this, [this]() {
        const double cdeltX = std::abs(this->remoteDatasetCdelt[0]);
        const double cdeltY = std::abs(this->remoteDatasetCdelt[1]);
        const double scaleDeg = (std::isfinite(cdeltX) && cdeltX > 0
                                 && std::isfinite(cdeltY) && cdeltY > 0)
                ? 0.5 * (cdeltX + cdeltY)
                : 0.0;
        auto *dialog = new FilterFitsDialog(this->filepath, scaleDeg * 3600.0,
                                            this->remoteDatasetWidth, this->remoteDatasetHeight,
                                            this->remoteBackendUrl, this->remoteBackendToken,
                                            this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<vtkWindowCube> self(this);
        QObject::connect(dialog, &FilterFitsDialog::filtered, this,
                         [this, self](const QString &path) {
                             if (!self)
                                 return;
                             // A cube cannot layer another cube: the result is
                             // a dataset of its own, and only the host opens one.
                             emit this->openBackendFitsRequested(
                                     path, QFileInfo(path).fileName());
                         });
        dialog->open();
    });

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

    // SoFiA-2 source finding. It existed only in the Data Hub's Science menu,
    // where it ran against "the last dataset opened" — a cube tool reachable
    // only from the one place that does not know which cube you are looking at.
    // The host already had the request signal (the copilot's show_sources uses
    // it) and captures this window's own dataset, so the tool just needed to be
    // in the list.
    m_actSourceFind = ui->menuTools->addAction(u"Source Finding (SoFiA-2)…"_s);
    m_actSourceFind->setToolTip(
            u"Run the SoFiA-2 source finder over this cube and overlay the "
            u"detections; needs sofia2 on the backend's PATH."_s);
    QObject::connect(m_actSourceFind, &QAction::triggered, this,
                     [this]() { emit this->sourceFinderRequested(); });

    m_actKinematicLasso = ui->menuTools->addAction(u"Kinematic Lasso (click to select)"_s);
    m_actKinematicLasso->setCheckable(true);
    m_actKinematicLasso->setToolTip(
            u"Click a source in the 3-D view to select the whole kinematically-"
            u"coherent object (geodesic, velocity-aware), no coordinates, no box. "
            u"The green isosurface is the selection."_s);
    QObject::connect(m_actKinematicLasso, &QAction::toggled,
                     this, &vtkWindowCube::setKinematicLassoActive);
    QObject::connect(&m_lassoSelectWatcher,
                     &QFutureWatcher<RemoteKinematicSelectResult>::finished,
                     this, &vtkWindowCube::onLassoSelectReady);
    QObject::connect(&this->m_lassoLevelsWatcher, &QFutureWatcher<BackendKinematicLevelsResult>::finished,
                     this, &vtkWindowCube::onLassoLevelsReady);

    m_actExportChannel2d = ui->menuTools->addAction(
            u"Export Current Channel as 2-D FITS…"_s);
    m_actExportChannel2d->setToolTip(
            u"Save the currently displayed channel as a true 2-D FITS image "
            u"(no degenerate spectral axis). Use this to extract Q/U/I/V at a "
            u"single frequency for the Stokes / spectral-index workflows."_s);
    QObject::connect(m_actExportChannel2d, &QAction::triggered,
                     this, &vtkWindowCube::exportCurrentChannelAs2dFits);

    // Stokes / polarimetry (vtkWindowCube_Polarisation.cpp): companion
    // discovery, PI/PA and RM synthesis, all computed backend-side.
    this->setupPolarisationTools();

    m_actSliceHistogram = ui->menuTools->addAction(u"Pixel Histogram (current slice)"_s);
    m_actSliceHistogram->setToolTip(
            u"Open a floating histogram of the current 2-D slice with two "
            u"draggable cursors (red = low, green = high) that live-clip the "
            u"LUT range. Useful for identifying multi-plane products such as "
            u"the planes of MFImage RM cubes."_s);
    QObject::connect(m_actSliceHistogram, &QAction::triggered,
                     this, &vtkWindowCube::showSliceHistogramPanel);

    m_actExportSelected = ui->menuTools->addAction(u"Export Selected View…"_s);
    QObject::connect(m_actExportSelected, &QAction::triggered,
                     this, &vtkWindowCube::exportSelectedView);

    m_actExportDisplayedMap = ui->menuTools->addAction(u"Export Displayed Map as FITS…"_s);
    m_actExportDisplayedMap->setToolTip(
            u"Save the 2-D map currently shown in a pane — a moment, a line-width "
            u"map, polarised intensity, a Faraday-depth map — as a standalone 2-D "
            u"FITS in Workspace Exports, with the celestial WCS and the parameters "
            u"it was computed with recorded as HISTORY."_s);
    QObject::connect(m_actExportDisplayedMap, &QAction::triggered, this,
                     [this]() { this->exportDisplayedMapAsFits(); });

    m_actExportMomentFits = ui->menuTools->addAction(u"Export Moment Map as FITS…"_s);
    m_actExportMomentFits->setToolTip(
            u"Persist the moment map currently displayed in the 2-D view "
            u"as a standalone 2-D FITS file (celestial WCS + BUNIT)."_s);
    // Disabled until a moment has actually been computed (otherwise we'd
    // have nothing to export). updateMomentExportEnabled() flips this on
    // when momentController publishes its first result.
    m_actExportMomentFits->setEnabled(false);
    // Through a lambda, not a member-pointer: triggered(bool) cannot bind to a
    // slot taking a QString, and the action's own path wants the default.
    QObject::connect(m_actExportMomentFits, &QAction::triggered, this,
                     [this]() { this->exportMomentMapAsFits(); });

    // Send current slice / moment to an open image viewer as contour overlay
    // "Send Slice to Image Viewer" read as "open this slice in an image viewer",
    // so its needing a viewer ALREADY open looked like a bug rather than the
    // point of the tool (user report). The name says what it does now.
    m_actSendSliceToImage = ui->menuTools->addAction(u"Overlay Slice on an Open Image"_s);
    m_actSendSliceToImage->setToolTip(
            u"Draw the current 2-D slice as contours over an image that is already "
            u"open in an image viewer, to compare the two. It does not open a "
            u"viewer of its own: open the image first, then come back here."_s);
    QObject::connect(m_actSendSliceToImage, &QAction::triggered, this, [this]() {
        auto *output = this->remoteSliceDisplaySource
                ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
                : nullptr;
        if (!output || output->GetNumberOfPoints() == 0) {
            statusBar()->showMessage(u"No 2-D slice data available."_s, 2000);
            return;
        }
        // The signal is relayed by MainWindow to the FIRST open image viewer; with
        // none open it went nowhere and the click did nothing at all. The tool gate
        // disables the action in that case — this covers the window between a
        // viewer closing and the next gate pass.
        bool haveViewer = false;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w && w->inherits("vtkWindowImage")) {
                haveViewer = true;
                break;
            }
        if (!haveViewer) {
            statusBar()->showMessage(
                    u"No image viewer open - open a 2-D FITS image first; the slice "
                    u"overlays there as contours."_s,
                    5000);
            this->applyMomentToolGate();
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
            u"Export an animation (a velocity channel scan, or a 3-D camera "
            u"orbit) as an MP4 video, or as a PNG frame sequence."_s);
    QObject::connect(m_actExportMovie, &QAction::triggered,
                     this, &vtkWindowCube::showExportMovieDialog);

    // Linked views (F3): sync camera / channel / colour-scale with other open
    // cube viewers that also have linking enabled.
    // "Link Views", with the rest in the tooltip. The full sentence measured
    // 258 px against the ~236 px a tool button in the Inspector's Analysis list
    // has — the only one of the thirty that did not fit — and shortening the
    // ACTION rather than overriding the button is what makes it stick:
    // QToolButton re-reads its text from the action on every change, so a
    // per-button override would be undone by the first click.
    m_actLinkViews = ui->menuTools->addAction(u"Link Views"_s);
    m_actLinkViews->setCheckable(true);
    m_actLinkViews->setToolTip(
            u"Synchronise camera, velocity channel and colour map across all open "
            u"cube viewers that have Link Views enabled - for side-by-side comparison."_s);
    QObject::connect(m_actLinkViews, &QAction::toggled,
                     this, &vtkWindowCube::setLinkViews);
    vtkWindowCube::s_registry.add(this);
    // A new window gives existing linked windows a cross-window peer, so refresh
    // their facet-chip enabled state (this window's own chips are refreshed at the
    // end of its toolbar setup). See refreshLinkedFacetChips's `master` gate.
    for (const auto &w : vtkWindowCube::s_registry.snapshot())
        if (w && w != this)
            w->refreshLinkedFacetChips();

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
    this->actionLoadCatalogueOverlay  = ui->menuTools->addAction(u"Load Catalogue Overlay…"_s);
    this->actionShowCatalogueOverlay  = ui->menuTools->addAction(u"Show Catalogue Overlay"_s);
    this->actionShowCatalogueLabels   = ui->menuTools->addAction(u"Show Catalogue Labels"_s);
    this->actionClearCatalogueOverlay = ui->menuTools->addAction(u"Clear Catalogue Overlay"_s);
    this->actionShowCatalogueOverlay->setCheckable(true);
    this->actionShowCatalogueLabels->setCheckable(true);
    this->actionShowCatalogueOverlay->setEnabled(false);
    this->actionShowCatalogueLabels->setEnabled(false);
    // NOT checked here. It used to start checked-but-disabled, meaning "labels
    // on by default once a catalogue arrives" — a reasonable intent expressed in
    // a way that reads as "labels are on" while there is no catalogue at all.
    // Invisible until the sidebars began mirroring checked state; now it is a
    // highlighted toggle for something that does not exist. The default is
    // applied where it belongs, in loadCatalogueOverlay(), which already sets it.
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
        ui->vtkCube->installEventFilter(this->channelOverlay);
        // The same scrubber on the 2-D Slice (see channelOverlay2d).
        this->channelOverlay2d = new ChannelOverlay(ui->vtkImage);
        ui->vtkImage->installEventFilter(this->channelOverlay2d);
        for (ChannelOverlay *co : this->channelOverlays()) {
            co->setRange(ui->spinSlice->minimum(), ui->spinSlice->maximum());
            co->setChannel(ui->spinSlice->value());
            co->setSpectralLabel(ui->lineSpectral->text());
            co->show();
            // Overlay → sidebar spin: drives the existing slice-fetch pipeline.
            QObject::connect(co, &ChannelOverlay::channelChanged, this, [this](int v) {
                if (ui->spinSlice->value() == v) return;
                ui->spinSlice->setValue(v);
            });
        }
        // Sidebar spin → overlay (programmatic updates: range change, animation,
        // user drag of the sidebar slider). SignalBlocker inside ChannelOverlay
        // prevents the bidirectional loop.
        QObject::connect(ui->spinSlice, &QSpinBox::valueChanged, this, [this](int v) {
            for (ChannelOverlay *co : this->channelOverlays()) {
                co->setChannel(v);
                co->setSpectralLabel(ui->lineSpectral->text());
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
                             for (ChannelOverlay *co : this->channelOverlays())
                                 co->setRange(lo, hi);
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
        auto *btn = m_uiAssembler->contoursButton();
        QObject::connect(btn, &QToolButton::toggled, ui->checkContours, &QCheckBox::setChecked);
        // And back. The link used to be one-way, which was invisible while the
        // button was the only way in — but the LAYERS row now writes the same
        // checkbox, and without this the button kept showing the old state:
        // turn contours on from LAYERS and "Show Contours" stayed unchecked, so
        // the first click on it did nothing visible and the second turned them
        // off. Blocked, or the two would drive each other.
        QObject::connect(ui->checkContours, &QCheckBox::toggled, btn, [btn](bool on) {
            QSignalBlocker b(btn);
            btn->setChecked(on);
        });
    }
    QObject::connect(ui->lineLevel, &QLineEdit::editingFinished, this,
                     &vtkWindowCube::updateContours);
    // Typing a bound claims it: the data-driven initialisation must not
    // overwrite the user's own numbers if the preview lands afterwards.
    for (QLineEdit *edit : { ui->lineLowerBound, ui->lineUpperBound }) {
        QObject::connect(edit, &QLineEdit::editingFinished, this, [this]() {
            m_contourBoundsInitialised = true;
            this->updateContours();
        });
    }

    // Backend-authoritative: statistics fields show "-" until populated.
    ui->lineCubeMin->clear();
    ui->lineCubeMax->clear();
    ui->lineCubeMean->clear();
    ui->lineCubeRms->clear();
    ui->lineImgMin->setText(u"-"_s);
    ui->lineImgMax->setText(u"-"_s);
    ui->lineImgMean->setText(u"-"_s);
    ui->lineImgRms->setText(u"-"_s);
    this->setCubeOpenActionsEnabled(false);
    ui->actionIsosurface->setChecked(false);
    ui->actionVolume->setChecked(true);
    this->setCubeRenderModeLocally(false);
    this->setRemoteCubeDisplayState(RemoteCubeDisplayState::Preview);
    this->showPersistentStatusMessage(u"Loading preview..."_s);
    this->m_deferredPreview.reset();   // superseded by this request
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
    // Child widgets (PV diagram, probe/profile plots, …) are deleted later by ~QWidget, when
    // this object is already half destroyed — but their destroyed() lambdas use `this` as the
    // context and would still run (Qt drops receiver connections only in ~QObject, after the
    // children are gone), calling clearPv()/Render() on torn-down members → segfault on quit.
    // Cut every child → this connection up front.
    for (QObject *child : this->findChildren<QObject *>())
        QObject::disconnect(child, nullptr, this, nullptr);
    // #5b: drop the transitory live-spectrum entry from the SESSION-WIDE registry (panes are
    // being torn down anyway — just clear the registry so a closed window leaves nothing).
    if (!this->m_spectrumLiveId.isEmpty()) {
        ProductRegistry::instance().removeProduct(this->m_spectrumLiveId);
        this->m_spectrumLiveId.clear();
    }
    // Pop-out: detach the event filter from any detached-pane window BEFORE teardown (the
    // window is a child → Qt deletes it, but a stray event must not reach a half-dead cube).
    for (auto &s : m_slots)
        if (s.detachedWindow)
            s.detachedWindow->removeEventFilter(this);
    // Stop the slice animation timer and detach any vtk observer we installed,
    // so we don't fire callbacks after the widget hierarchy starts going away.
    this->sliceAnimationTimer.stop();
    this->remove3dPickObserver();
    // Leave the linked-views group and drop the camera observer from the exact
    // interactor it was added to (weak ptr → only if still alive).
    vtkWindowCube::s_registry.remove(this);
    // Closing a window removes a cross-window peer, so refresh the remaining
    // windows' facet chips (self is already out of the registry).
    for (const auto &w : vtkWindowCube::s_registry.snapshot())
        if (w)
            w->refreshLinkedFacetChips();
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
    dlg->setWindowTitle(u"FITS Header - %1"_s.arg(QFileInfo(this->filepath).fileName()));
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

    // Editing belongs where the header already is — the legacy put it in a
    // separate window that only appeared when a file failed to open.
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(12, 8, 12, 12);
    actions->addStretch(1);
    auto *edit = new QPushButton(u"Edit…"_s, dlg);
    edit->setEnabled(result.valid && !result.cards.isEmpty());
    edit->setToolTip(u"Correct keywords and save the result as a new FITS file "
                     u"in the Workspace. This file is not modified."_s);
    const QStringList cards = result.cards;
    QPointer<vtkWindowCube> self(this);
    QObject::connect(edit, &QPushButton::clicked, dlg, [this, self, dlg, cards]() {
        if (!self)
            return;
        auto *editor = new FitsHeaderEditorDialog(this->filepath, cards,
                                                  this->remoteBackendUrl,
                                                  this->remoteBackendToken, self);
        editor->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(editor, &FitsHeaderEditorDialog::saved, self,
                         [this, self](const QString &path) {
                             if (!self)
                                 return;
                             emit this->openBackendFitsRequested(
                                     path, QFileInfo(path).fileName());
                         });
        editor->open();
        dlg->close();
    });
    actions->addWidget(edit);
    vbox->addLayout(actions);

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
        d.loadStateStyleSheet  = u"QLabel { font-size: 9px; color: %1; }"_s.arg(QLatin1StringView(VisivoTheme::kWarning()));
        break;
    case RemoteCubeDisplayState::LoadingFullResolution:
        d.loadStateText        = u"Loading full resolution\u2026"_s;
        d.loadStateStyleSheet  = u"QLabel { font-size: 9px; color: %1; font-style: italic; }"_s.arg(QLatin1StringView(VisivoTheme::kWarning()));
        break;
    case RemoteCubeDisplayState::FullResolution:
        d.loadStateText        = u"Full resolution"_s;
        d.loadStateStyleSheet  = u"QLabel { font-size: 9px; color: %1; }"_s.arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));
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
        d.momentStateText        = u"-"_s;
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
    // (a) the Inspector's 2-D block shows the ACTIVE slice's stats — refresh it whenever the
    // slice data changes (channel scrub / recompute). Cheap for a moment (cached); a fresh
    // pass for the slice, which only runs when the slice is the active source.
    updateActiveMapStats();
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
    if (this->m_sliceInvert) {
        // §5.4 Invert: reverse the colour ramp in place (range/stretch unchanged).
        const int n = this->lutSlice->GetNumberOfTableValues();
        std::vector<std::array<double, 4>> tmp(n);
        for (int i = 0; i < n; ++i)
            this->lutSlice->GetTableValue(i, tmp[i].data());
        for (int i = 0; i < n; ++i)
            this->lutSlice->SetTableValue(i, tmp[n - 1 - i].data());
    }
    this->lutSlice->Modified();

    if (this->active2dWin() && this->viewingSlice()) {
        this->active2dWin()->Render();
    }
    // STEP6-P4: push the slice colour to linked peer windows (no-op during a receiver apply — the
    // m_applyingLinkedUpdate guard inside broadcastLinked2DSliceLut prevents the echo).
    this->broadcastLinked2DSliceLut();
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
    // The window is non-modal and stays open across the run, so nothing here can
    // know whether a computation started. It does not need to: the view stays on
    // Slice either way (below) until a result arrives, and the Computing card
    // and start diagnostics hang off MomentMapController::computationStarted.
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info,
            DiagnosticsCategory::Scientific,
            QStringLiteral("MomentMap"),
            QStringLiteral("No moment map available, opening configuration dialog."),
            this->remoteDatasetId,
            this->remoteSessionId,
            QStringLiteral("moment"));

    this->momentController->startComputation(0);

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


void vtkWindowCube::applyDeferredPreviewWhenLocal()
{
    // A preview that arrived while the backend was rendering is shown once the
    // switch back to Local has run to its end (a cancelled negotiation, the
    // channel slider moved back), and only if nothing newer replaced it: a
    // full-resolution cube loaded meanwhile is better data than the proxy.
    if (!this->m_deferredPreview)
        return;
    QTimer::singleShot(0, this, [this]() {
        if (!this->m_deferredPreview || this->m_effectiveRenderMode != RenderMode::Local
            || this->m_remoteSwitchInProgress)
            return;
        const RemoteCubePreviewResult deferred = *this->m_deferredPreview;
        this->m_deferredPreview.reset();
        if (this->usingHighResCube)
            return;
        this->applyRemotePreviewResult(deferred);
    });
}

void vtkWindowCube::applyRemotePreviewResult(const RemoteCubePreviewResult &result)
{
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
    // The contour levels were built from the 0..1 placeholder
    // in the constructor; move them onto the real data now.
    this->refreshContourBoundsFromData();

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
}

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
                "[remote-render] applyRenderMode(%1) ignored, already switching")
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
    // Raw QSettings, not Settings: that constructor runs python3 with blocking
    // waits, and this now runs on every cube opened on a remote backend
    // (applyAutoRenderModeOnOpen). Default and clamp mirror
    // Settings::getLocalFullThresholdMb.
    const int rmThresholdMb = [] {
        QSettings raw(vtkWindowCube::settingsIniPath(), QSettings::IniFormat);
        return std::clamp(raw.value(QStringLiteral("Cube/local_full_threshold_mb"), 256).toInt(), 16,
                          16384);
    }();
    const double rmCubeFullMb = (static_cast<double>(this->remoteDatasetWidth)
                                  * static_cast<double>(this->remoteDatasetHeight)
                                  * static_cast<double>(this->remoteDatasetDepth)
                                  * 4.0) / (1024.0 * 1024.0);
    const bool rmSizeWantsRemote =
            rmCubeFullMb > static_cast<double>(rmThresholdMb);
    // A backend that cannot render server-side is not a candidate, for Auto or
    // for an explicit request: /v1/render/* refuses it, and Auto would have gone
    // looking for it on every large cube.
    const bool wantRemote = this->m_serverSideRenderingAvailable
            && ((requested == RenderMode::Remote)
                || (requested == RenderMode::Auto
                    && (this->recommendedLoadMode == QLatin1String("preview_only")
                        || rmSizeWantsRemote)
                    && !this->remoteBackendUrl.isEmpty()));

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
        // …and the local alternative with it. The local page stays visible while
        // the session negotiates, so this button was still clickable — and a
        // full-volume fetch started there would have both processes allocating
        // the same cube on the same machine.
        if (this->m_localRecommendLoadFullBtn)
            this->m_localRecommendLoadFullBtn->setEnabled(false);
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
                // Watchdog. The ticker used to narrate an unbounded wait: if the
                // negotiation never reaches Live and never reports a failure —
                // a dropped connection, a wedged backend — the banner counted
                // seconds for ever with both actions disabled. Generous,
                // because a multi-GB cube legitimately takes minutes to land in
                // the backend's GPU.
                constexpr qint64 kSwitchTimeoutS = 300;
                if (secs >= kSwitchTimeoutS) {
                    this->abortRemoteSwitch(
                            QStringLiteral("Remote rendering did not start within %1 s - "
                                           "staying local. Check the backend's GPU / EGL "
                                           "support in Diagnostics, then retry.")
                                    .arg(kSwitchTimeoutS));
                    return;
                }
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
                            "Loading cube into backend GPU memory, large "
                            "datasets take a minute or two… (%1 s elapsed)")
                                  .arg(secs);
                } else {
                    const qint64 mins = secs / 60;
                    const qint64 rem  = secs % 60;
                    msg = QStringLiteral(
                            "Still loading, very large cubes (20+ GB) can "
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
            } else if (this->m_remoteSwitchInProgress) {
                // Already streaming: no connection is attempted, so no Live
                // signal will arrive to clear the guard — and the watchdog would
                // have aborted a perfectly good stream 300 s later.
                // Nothing to wait for, so end the switch here.
                this->m_remoteSwitchInProgress = false;
                if (this->m_remoteSwitchProgressTimer)
                    this->m_remoteSwitchProgressTimer->stop();
                if (this->m_localRecommendRemoteBanner)
                    this->m_localRecommendRemoteBanner->setVisible(false);
                if (this->m_localRecommendRemoteSwitchBtn) {
                    this->m_localRecommendRemoteSwitchBtn->setEnabled(true);
                    this->m_localRecommendRemoteSwitchBtn->setText(u"Switch to Remote"_s);
                }
                if (this->m_localRecommendLoadFullBtn)
                    this->m_localRecommendLoadFullBtn->setEnabled(true);
            }
        }
    } else {
        if (this->m_renderStack) {
            this->m_renderStack->setCurrentIndex(0);
        }
        this->m_effectiveRenderMode = RenderMode::Local;
        this->applyDeferredPreviewWhenLocal();
        // Going back to Local CANCELS a switch that was still negotiating —
        // BEFORE the disconnect below, which emits Disconnected synchronously
        // and whose handler clears this very flag, so anything gated on it
        // afterwards never ran. Says so itself rather than relying on a
        // signal: otherwise the guard stayed set and both banner buttons stayed
        // disabled, with no way to retry.
        if (this->m_remoteSwitchInProgress) {
            this->m_remoteSwitchInProgress = false;
            // Invalidate any create_session reply still in flight: it used to
            // reconnect and flip to Remote regardless, undoing the very
            // cancellation the user just asked for.
            ++this->m_remoteSwitchGeneration;
            if (this->m_remoteSwitchProgressTimer)
                this->m_remoteSwitchProgressTimer->stop();
            if (this->m_localRecommendRemoteSwitchBtn) {
                this->m_localRecommendRemoteSwitchBtn->setEnabled(true);
                this->m_localRecommendRemoteSwitchBtn->setText(u"Switch to Remote"_s);
            }
            if (this->m_localRecommendLoadFullBtn)
                this->m_localRecommendLoadFullBtn->setEnabled(true);
        }
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
    if (this->isBusyForPvInteraction()) {
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
    if (this->pvEditing()) {
        // A finished path: double-click adds a vertex on it, right-click removes one.
        // (QVTK reports a double-click with repeat count 1.)
        if (position) {
            const bool doubleClick = this->active2dWin()->GetInteractor()->GetRepeatCount() > 0;
            this->editPvVertexAt(position[0], position[1], /*insert=*/doubleClick);
        }
        return;
    }
    if (position) {
        this->updatePvFromDisplayPosition(position[0], position[1]);
    }
    const auto samePoint = [](const std::array<double, 2> &a, const std::array<double, 2> &b) {
        return std::hypot(a[0] - b[0], a[1] - b[1]) < 0.25;
    };
    if (this->pvDragging && this->pvCursorValid
        && (this->pvPolylineVertices.empty()
            || !samePoint(this->pvPolylineVertices.back(), this->pvCurrentVoxel))) {
        this->pvPolylineVertices.push_back(this->pvCurrentVoxel);
    }

    std::vector<std::array<double, 2>> cleaned;
    cleaned.reserve(this->pvPolylineVertices.size());
    for (const auto &vertex : this->pvPolylineVertices) {
        if (cleaned.empty() || !samePoint(cleaned.back(), vertex)) {
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
    this->refreshPvOverlay();   // the finished path shows its handles: it is editable now
    this->statusBar()->showMessage(
            u"Path ready. Drag a vertex or the path (Shift: rotate), double-click it to add a "
            u"vertex, right-click a vertex to remove it — the diagram follows."_s, 8000);
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
        this->m_pvEdit = PvEdit::None;
        // A finished path whose diagram is still being computed is kept too: the
        // result is about to open a diagram for it, and that diagram must find
        // its path (to show and to resume editing).
        const bool pathAwaitingDiagram = this->pvValid && this->pvPolylineVertices.size() >= 2
                && this->pvController && this->pvController->isBusy();
        if (pathAwaitingDiagram) {
            this->setInteractorStyleImage();
            this->refreshPvOverlay();
            if (auto *w2d = this->active2dWidget()) w2d->setCursor(this->probeModeActive ? Qt::CrossCursor : Qt::ArrowCursor);
            if (this->active2dWin())
                this->active2dWin()->Render();
            return;
        }
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

    // No width prompt: the width is a control of the diagram itself, editable at
    // any time, and the last one used is kept.
    this->ensureSliceForTools();   // a moment pane becomes the slice to draw on
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
    const bool pvComputing = this->pvController && this->pvController->isBusy();
    if ((this->pvDiagramWidget || pvComputing) && this->pvPolylineVertices.size() >= 2) {
        // A diagram is open (or its first result is on the way): PV mode resumes
        // EDITING its path instead of wiping it.
        this->pvValid = true;
        this->statusBar()->showMessage(
                u"Editing the PV path: drag a vertex or the path (Shift: rotate). "
                u"New path… in the diagram starts over."_s, 6000);
    } else {
        this->pvValid = false;
        this->clearPv();
    }
    this->setInteractorStyleRegion();
    this->refreshPvOverlay();
    if (auto *w2d = this->active2dWidget()) w2d->setCursor(Qt::CrossCursor);
    if (auto *win = this->active2dWin())
        win->Render();
}


bool vtkWindowCube::updatePvFromDisplayPosition(int displayX, int displayY)
{
    std::array<double, 2> pixel{};
    if (!this->pvPixelFromDisplay(displayX, displayY, pixel))
        return false;
    this->pvCurrentVoxel = pixel;
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
    // Drawn in world coordinates where cube pixel (x, y) sits at (x, y).
    // Kept on screen while a diagram shows it, or while its first diagram computes.
    const bool keepingForPlotWindow = (this->pvDiagramWidget != nullptr)
            || (this->pvValid && this->pvController && this->pvController->isBusy());
    std::vector<std::array<double, 2>> control = this->pvPolylineVertices;
    // The hovering cursor extends the path only while it is being drawn.
    if (this->pvModeActive && this->pvDragging && this->pvCursorValid
        && (control.empty() || control.back() != this->pvCurrentVoxel)) {
        control.push_back(this->pvCurrentVoxel);
    }
    const auto path = this->m_pvSmooth ? pvpath::smoothed(control, 8) : control;
    const bool editing = this->pvEditing();

    const auto updateLine = [&](vtkImageData *imageData, vtkPoints *points, vtkCellArray *cells,
                                vtkPolyData *polyData, vtkActor *actor, vtkPoints *upperPoints,
                                vtkCellArray *upperCells, vtkPolyData *upperData,
                                vtkActor *upperActor, vtkPoints *lowerPoints,
                                vtkCellArray *lowerCells, vtkPolyData *lowerData,
                                vtkActor *lowerActor) {
        if (!imageData || (!this->pvModeActive && !keepingForPlotWindow) || path.size() < 2) {
            actor->VisibilityOff();
            upperActor->VisibilityOff();
            lowerActor->VisibilityOff();
            return;
        }
        points->Reset();
        cells->Reset();
        cells->InsertNextCell(static_cast<vtkIdType>(path.size()));
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(path.size()); ++i) {
            points->InsertNextPoint(path[std::size_t(i)][0], path[std::size_t(i)][1], 0.0);
            cells->InsertCellPoint(i);
        }
        points->Modified();
        polyData->Modified();
        actor->VisibilityOn();

        if (this->pvWidthPixels <= 1) {
            upperActor->VisibilityOff();
            lowerActor->VisibilityOff();
            return;
        }
        // The two edges of the band the cut averages over, in the backend's
        // convention: normals and spacing on the sky (pixels need not be square),
        // one sample per FINER pixel; each path vertex offset along the mean of
        // its adjacent segment normals. Square pixels: plain pixel offsets.
        // Pixel → sky matrix M (row-major a b / c d): the backend's after the first
        // result, else the header's CDELTs. Normals and offsets live on the sky.
        std::array<double, 4> M = this->m_pvSkyMatrix;
        double det = M[0] * M[3] - M[1] * M[2];
        if (!(std::abs(det) > 0.0) || !std::isfinite(det)) {
            double ax = std::abs(this->remoteDatasetCdelt[0]);
            double ay = std::abs(this->remoteDatasetCdelt[1]);
            if (!(ax > 0.0) || !(ay > 0.0) || !std::isfinite(ax) || !std::isfinite(ay))
                ax = ay = 1.0;
            M = { ax, 0.0, 0.0, ay };
            det = ax * ay;
        }
        const std::array<double, 4> Minv{ M[3] / det, -M[1] / det, -M[2] / det, M[0] / det };
        const double step = std::min(std::hypot(M[0], M[2]), std::hypot(M[1], M[3]));
        const double halfWidth = 0.5 * static_cast<double>(this->pvWidthPixels - 1);
        upperPoints->Reset();
        upperCells->Reset();
        lowerPoints->Reset();
        lowerCells->Reset();
        upperCells->InsertNextCell(static_cast<vtkIdType>(path.size()));
        lowerCells->InsertNextCell(static_cast<vtkIdType>(path.size()));
        const auto segNormal = [&](std::size_t i) -> std::array<double, 2> {   // on the sky
            const double dx = path[i + 1][0] - path[i][0];
            const double dy = path[i + 1][1] - path[i][1];
            const double tx = M[0] * dx + M[1] * dy;
            const double ty = M[2] * dx + M[3] * dy;
            const double len = std::hypot(tx, ty);
            return len > 0.0 ? std::array<double, 2>{ -ty / len, tx / len }
                             : std::array<double, 2>{ 0.0, 0.0 };
        };
        for (std::size_t i = 0; i < path.size(); ++i) {
            std::array<double, 2> n{ 0.0, 0.0 };
            if (i > 0) { const auto a = segNormal(i - 1); n[0] += a[0]; n[1] += a[1]; }
            if (i + 1 < path.size()) { const auto b = segNormal(i); n[0] += b[0]; n[1] += b[1]; }
            const double len = std::hypot(n[0], n[1]);
            if (len > 0.0) { n[0] /= len; n[1] /= len; }
            // Sky offset (halfWidth steps along n) back to pixels.
            const double su = halfWidth * step * n[0];
            const double sv = halfWidth * step * n[1];
            const double ox = Minv[0] * su + Minv[1] * sv;
            const double oy = Minv[2] * su + Minv[3] * sv;
            upperPoints->InsertNextPoint(path[i][0] + ox, path[i][1] + oy, 0.0);
            lowerPoints->InsertNextPoint(path[i][0] - ox, path[i][1] - oy, 0.0);
            upperCells->InsertCellPoint(static_cast<vtkIdType>(i));
            lowerCells->InsertCellPoint(static_cast<vtkIdType>(i));
        }
        upperPoints->Modified();
        lowerPoints->Modified();
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

    // Handles on the control vertices while the path can be edited.
    this->slicePvHandlePoints->Reset();
    this->slicePvHandleCells->Reset();
    if (editing) {
        for (const auto &v : this->pvPolylineVertices) {
            const vtkIdType id = this->slicePvHandlePoints->InsertNextPoint(v[0], v[1], 0.0);
            this->slicePvHandleCells->InsertNextCell(1, &id);
        }
    }
    this->slicePvHandlePoints->Modified();
    this->slicePvHandleData->Modified();
    this->slicePvHandleActor->SetVisibility(editing);

    // The same path on the map panes of this field (channels, moments): the cut
    // is seen against every view of the data, as in kpvslice.
    // The COMMITTED path (no hover segment): the panes follow edits, not every
    // mouse move, which would re-render each of them for nothing.
    const auto committed = this->pvEffectivePath();
    const bool pathShown = (this->pvModeActive || keepingForPlotWindow) && committed.size() >= 2;
    this->refreshPaneOverlayPaths(pathShown ? committed : std::vector<std::array<double, 2>>{});
}

void vtkWindowCube::clearPv()
{
    this->pvValid = false;
    this->pvCursorValid = false;
    this->pvPolylineVertices.clear();
    this->slicePvActor->VisibilityOff();
    this->slicePvUpperActor->VisibilityOff();
    this->slicePvLowerActor->VisibilityOff();
    this->slicePvHandleActor->VisibilityOff();
    this->refreshPaneOverlayPaths({});
    this->m_pvEdit = PvEdit::None;
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
/// The parameter window shared by the two tools that act on a 3-D box:
/// Export Sub-Cube as FITS and Mask 3-D Region.
///
/// It behaves like every other tool dialog and for the same reasons: the window
/// STAYS OPEN across the run, its action button starts the work instead of
/// closing it, and the caller puts a window-modal progress sheet on THIS window
/// so the work is reported where it was asked for. It also carries the tool's
/// one extra parameter (the export basename, the mask mode), because the two
/// tools used to ask for it in a SECOND modal prompt after this one closed, and
/// a tool that interrogates you twice before starting is not a settings window.
///
/// The two tools also used to share this dialog verbatim, so Mask 3-D Region
/// opened a window titled "Export Sub-Cube as FITS" that described exporting.
/// Title, hint and action verb are per-tool now.
///
/// No Q_OBJECT: everything is wired with lambdas, so the class can live here
/// next to its only two users instead of in a header of its own.
class CubeBoxToolDialog : public QDialog
{
public:
    CubeBoxToolDialog(QWidget *parent, const QString &title, const QString &hint,
                      const QString &actionText,
                      int fullWidth, int fullHeight, int fullDepth,
                      int defaultX0, int defaultX1, int defaultY0, int defaultY1,
                      int defaultZ0, int defaultZ1,
                      const QString &extraLabel, QWidget *extraWidget)
        : QDialog(parent)
    {
        using namespace Qt::StringLiterals;
        setWindowTitle(title);
        setAttribute(Qt::WA_DeleteOnClose);
        setStyleSheet(ToolDialogStyle::inputCss());

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(16, 14, 16, 14);
        root->setSpacing(10);
        root->addWidget(ToolDialogStyle::makeBreadcrumb(this, title));

        auto *hintLabel = new QLabel(hint, this);
        hintLabel->setStyleSheet(ToolDialogStyle::helperCss());
        hintLabel->setWordWrap(true);
        root->addWidget(hintLabel);

        auto *form = new QFormLayout();
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(6);

        auto makeSpin = [this](int lo, int hi, int val) {
            auto *sb = new QSpinBox(this);
            sb->setRange(lo, hi);
            sb->setValue(std::clamp(val, lo, hi));
            sb->setAlignment(Qt::AlignRight);
            return sb;
        };
        const int xMax = std::max(0, fullWidth  - 1);
        const int yMax = std::max(0, fullHeight - 1);
        const int zMax = std::max(0, fullDepth  - 1);
        m_spX0 = makeSpin(0, xMax, defaultX0);
        m_spX1 = makeSpin(0, xMax, defaultX1);
        m_spY0 = makeSpin(0, yMax, defaultY0);
        m_spY1 = makeSpin(0, yMax, defaultY1);
        // Channels from 1, as on the channel bar; bounds() stays 0-based.
        m_spZ0 = makeSpin(1, zMax + 1, defaultZ0 + 1);
        m_spZ1 = makeSpin(1, zMax + 1, defaultZ1 + 1);
        auto makeRow = [this](QSpinBox *lo, QSpinBox *hi) {
            auto *w = new QWidget(this);
            auto *h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(8);
            h->addWidget(lo, 1);
            h->addWidget(new QLabel(u"→"_s, w));
            h->addWidget(hi, 1);
            return w;
        };
        form->addRow(u"X (columns)"_s,  makeRow(m_spX0, m_spX1));
        form->addRow(u"Y (rows)"_s,     makeRow(m_spY0, m_spY1));
        form->addRow(u"Z (channels)"_s, makeRow(m_spZ0, m_spZ1));
        if (extraWidget) {
            extraWidget->setParent(this);
            form->addRow(extraLabel, extraWidget);
        }
        root->addLayout(form);

        auto *btnRow = new QHBoxLayout();
        btnRow->addStretch();
        auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, this);
        m_actionBtn = VisivoTheme::makePrimaryButton(actionText, this);
        m_actionBtn->setDefault(true);
        btnRow->addWidget(closeBtn);
        btnRow->addWidget(m_actionBtn);
        root->addLayout(btnRow);
        QObject::connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    }

    /// Inclusive 0-indexed bounds, normalised so the user may type them in any
    /// order: {x0, x1, y0, y1, z0, z1}. (The channel boxes show z + 1.)
    std::array<int, 6> bounds() const
    {
        return { std::min(m_spX0->value(), m_spX1->value()),
                 std::max(m_spX0->value(), m_spX1->value()),
                 std::min(m_spY0->value(), m_spY1->value()),
                 std::max(m_spY0->value(), m_spY1->value()),
                 std::min(m_spZ0->value(), m_spZ1->value()) - 1,
                 std::max(m_spZ0->value(), m_spZ1->value()) - 1 };
    }

    /// Runs @p onRun with this dialog as the owner of the run.
    void onAction(const std::function<void()> &handler)
    {
        QObject::connect(m_actionBtn, &QPushButton::clicked, this, handler);
    }

    /// Call @p handler whenever any of the six bounds changes, and once now.
    ///
    /// Typing six numbers and hoping is not a way to choose a sub-cube: the
    /// caller uses this to draw the box being described, live, inside the
    /// volume (user request). Every keystroke, not on editingFinished — the
    /// point is to see the box move while deciding.
    void onBoundsChanged(const std::function<void(const std::array<int, 6> &)> &handler)
    {
        for (QSpinBox *sb : { m_spX0, m_spX1, m_spY0, m_spY1, m_spZ0, m_spZ1 }) {
            QObject::connect(sb, &QSpinBox::valueChanged, this,
                             [this, handler]() { handler(bounds()); });
        }
        handler(bounds());
    }

    /// The progress sheet is window-modal on this dialog and already blocks it;
    /// greying the action button makes the state visible as well as felt.
    void setBusy(bool busy) { if (m_actionBtn) m_actionBtn->setEnabled(!busy); }

private:
    QSpinBox *m_spX0{ nullptr };
    QSpinBox *m_spX1{ nullptr };
    QSpinBox *m_spY0{ nullptr };
    QSpinBox *m_spY1{ nullptr };
    QSpinBox *m_spZ0{ nullptr };
    QSpinBox *m_spZ1{ nullptr };
    QPushButton *m_actionBtn{ nullptr };
};
void previewBoxWhileEditing(vtkWindowCube *window, CubeBoxToolDialog *dialog)
{
    using namespace Qt::StringLiterals;
    if (!window || !dialog)
        return;
    // The same amber overlay Estimate Noise uses to show its sampling region: a
    // rectangle on the 2-D slice and a translucent box inside the volume. It was
    // already there, tied to one tool; the two tools that crop or mask a 3-D box
    // are exactly the ones that need it, because their parameters are six
    // numbers with no other picture of what they select (user request).
    QPointer<vtkWindowCube> self(window);
    QPointer<CubeBoxToolDialog> owner(dialog);
    dialog->onBoundsChanged([self, owner](const std::array<int, 6> &b) {
        if (!self)
            return;
        self->claimRegionPreview(owner);   // the box on screen is this window's
        self->setNoiseRegionPreview(b[0], b[1], b[2], b[3], b[4], b[5]);
    });
    // Cleared when the window goes, not when the run ends: the box describes
    // what the window is set to, and the window outlives the run now.
    QObject::connect(dialog, &QObject::destroyed, window, [self, dialog]() {
        if (self)
            self->clearRegionPreviewIfOwner(dialog);
    });
}
} // namespace

std::array<int, 4> vtkWindowCube::currentRegionBounds2D() const
{
    const int maxX = std::max(0, this->remoteDatasetWidth - 1);
    const int maxY = std::max(0, this->remoteDatasetHeight - 1);
    if (!this->regionValid || this->regionMode == RegionMode::None)
        return { 0, maxX, 0, maxY };
    const auto bounds = regionBounds2D(this->regionMode, this->regionAnchorVoxel,
                                       this->regionCurrentVoxel, this->regionPolygonVertices);
    const int x0 = std::clamp(bounds[0], 0, maxX);
    const int y0 = std::clamp(bounds[2], 0, maxY);
    return { x0, std::clamp(bounds[1], x0, maxX), y0, std::clamp(bounds[3], y0, maxY) };
}

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

    // One window with every parameter in it, kept open across the run. The
    // basename used to be a SECOND modal prompt after the bounds dialog closed.
    const QFileInfo srcInfo(this->filepath);
    auto *nameEdit = new QLineEdit();
    nameEdit->setPlaceholderText(u"leave empty to let the backend name it"_s);
    nameEdit->setText(u"%1_subcube_x%2-%3_y%4-%5_z%6-%7.fits"_s
                              .arg(srcInfo.completeBaseName())
                              .arg(defX0).arg(defX1).arg(defY0).arg(defY1)
                              .arg(defZ0 + 1).arg(defZ1 + 1));   // channels as shown in the Z boxes
    // One window per tool, re-raised rather than duplicated: a second window
    // could start a second run while the first was still going, and disabling
    // one window's button does nothing for the other.
    if (m_subCubeDialog) {
        m_subCubeDialog->show();
        m_subCubeDialog->raise();
        m_subCubeDialog->activateWindow();
        return;
    }
    auto *dlg = new CubeBoxToolDialog(
            this, u"Export Sub-Cube as FITS"_s,
            u"Crop the current cube to a spatial + spectral ROI and save it as a "
            u"standalone FITS in the Workspace Exports. WCS is preserved (CRPIX "
            u"shifted) so the coordinates of every pixel match the source. An "
            u"existing name gets a _1, _2 suffix."_s,
            u"Export"_s,
            this->remoteDatasetWidth, this->remoteDatasetHeight, this->remoteDatasetDepth,
            defX0, defX1, defY0, defY1, defZ0, defZ1,
            u"Save as"_s, nameEdit);
    m_subCubeDialog = dlg;
    previewBoxWhileEditing(this, dlg);

    QPointer<vtkWindowCube> self(this);
    dlg->onAction([this, self, dlg, nameEdit]() {
        // The flag lives on the WINDOW, not in the dialog, so closing and
        // reopening the dialog mid-run cannot start a second export.
        if (!self || m_subCubeBusy) return;
        m_subCubeBusy = true;
        const auto b = dlg->bounds();
        const QString basename  = nameEdit->text();
        const QString url       = this->remoteBackendUrl;
        const QString token     = this->remoteBackendToken;
        const QString sessionId = this->remoteSessionId;
        const QString datasetId = this->remoteDatasetId;

        this->statusBar()->showMessage(u"Exporting sub-cube to workspace…"_s, 0);
        dlg->setBusy(true);
        auto *sheet = visivo::showToolProgressSheet(dlg, u"Exporting the sub-cube"_s);
        QPointer<QProgressDialog> sheetPtr(sheet);
        QPointer<CubeBoxToolDialog> dlgPtr(dlg);

        auto *watcher = new QFutureWatcher<BackendSaveSubregionResult>(this);
        QObject::connect(watcher, &QFutureWatcher<BackendSaveSubregionResult>::finished, this,
                         [self, watcher, sheetPtr, dlgPtr]() mutable {
                             const auto r = watcher->result();
                             watcher->deleteLater();
                             visivo::closeToolProgressSheet(sheetPtr);
                             if (dlgPtr) dlgPtr->setBusy(false);
                             if (self) self->m_subCubeBusy = false;
                             if (!self) return;
                             self->statusBar()->clearMessage();
                             if (!r.valid) {
                                 QMessageBox::warning(dlgPtr ? static_cast<QWidget *>(dlgPtr)
                                                             : static_cast<QWidget *>(self),
                                         u"Export Sub-Cube"_s,
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
                             QMessageBox::information(dlgPtr ? static_cast<QWidget *>(dlgPtr)
                                                             : static_cast<QWidget *>(self),
                                                      u"Export Sub-Cube"_s, msg);
                         });
        watcher->setFuture(QtConcurrent::run(
                [url, token, sessionId, datasetId, b, basename]() {
                    BackendClient c(url, token);
                    c.setSessionId(sessionId);
                    return c.saveCubeSubregion(datasetId, b[0], b[1], b[2], b[3], b[4], b[5],
                                               basename);
                }));
    });
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
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

    // One window, with the mask mode in it. This tool used to borrow the
    // Export Sub-Cube dialog verbatim — so it opened a window titled "Export
    // Sub-Cube as FITS" that described exporting — and then asked for the mode
    // in a second modal prompt once that had closed.
    auto *modeCombo = new QComboBox();
    modeCombo->addItem(u"Isolate box (blank everything OUTSIDE)"_s, u"blank_outside"_s);
    modeCombo->addItem(u"Remove box (blank everything INSIDE)"_s,   u"blank_inside"_s);
    if (m_maskRegionDialog) {
        m_maskRegionDialog->show();
        m_maskRegionDialog->raise();
        m_maskRegionDialog->activateWindow();
        return;
    }
    auto *dlg = new CubeBoxToolDialog(
            this, u"Mask 3-D Region"_s,
            u"Blank the voxels inside or outside a 3-D box, keeping the cube's full "
            u"dimensions and WCS. The masked cube is saved to the Workspace Exports "
            u"and registered in this session, so you can moment-map or profile just "
            u"the selection."_s,
            u"Mask"_s,
            this->remoteDatasetWidth, this->remoteDatasetHeight, this->remoteDatasetDepth,
            defX0, defX1, defY0, defY1, defZ0, defZ1,
            u"Mode"_s, modeCombo);
    m_maskRegionDialog = dlg;
    previewBoxWhileEditing(this, dlg);

    QPointer<vtkWindowCube> self(this);
    dlg->onAction([this, self, dlg, modeCombo]() {
        // As above: no concurrent masks, whatever happens to the window. The
        // shared 3-D preview and the status line belong to one run at a time.
        if (!self || m_maskRegionBusy) return;
        m_maskRegionBusy = true;
        if (m_actMaskRegion) m_actMaskRegion->setEnabled(false);
        const auto b = dlg->bounds();
        const QString mode = modeCombo->currentData().toString();

        // The box is already on screen from the live preview below; it stays up
        // through the compute and is cleared when the window closes.

        const QString url       = this->remoteBackendUrl;
        const QString token     = this->remoteBackendToken;
        const QString sessionId = this->remoteSessionId;
        const QString datasetId = this->remoteDatasetId;
        this->statusBar()->showMessage(u"Masking cube region…"_s, 0);
        dlg->setBusy(true);
        auto *sheet = visivo::showToolProgressSheet(dlg, u"Masking the region"_s);
        QPointer<QProgressDialog> sheetPtr(sheet);
        QPointer<CubeBoxToolDialog> dlgPtr(dlg);

        auto *watcher = new QFutureWatcher<BackendSaveSubregionResult>(this);
        QObject::connect(watcher, &QFutureWatcher<BackendSaveSubregionResult>::finished, this,
                         [self, watcher, sheetPtr, dlgPtr]() mutable {
                             const auto r = watcher->result();
                             watcher->deleteLater();
                             visivo::closeToolProgressSheet(sheetPtr);
                             if (dlgPtr) dlgPtr->setBusy(false);
                             if (self) {
                                 self->m_maskRegionBusy = false;
                                 if (self->m_actMaskRegion)
                                     self->m_actMaskRegion->setEnabled(true);
                             }
                             if (!self) return;
                             self->statusBar()->clearMessage();
                             self->clearNoiseRegionPreview();
                             QWidget *owner = dlgPtr ? static_cast<QWidget *>(dlgPtr)
                                                     : static_cast<QWidget *>(self);
                             if (!r.valid) {
                                 QMessageBox::warning(owner, u"Mask 3-D Region"_s,
                                         u"Masking failed:\n\n%1"_s.arg(r.error));
                                 return;
                             }
                             emit self->workspaceExportsChanged();
                             QMessageBox::information(owner, u"Mask 3-D Region"_s,
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
                [url, token, sessionId, datasetId, b, mode]() {
                    BackendClient c(url, token);
                    c.setSessionId(sessionId);
                    return c.maskCubeRegion(datasetId, b[0], b[1], b[2], b[3], b[4], b[5], mode);
                }));
    });
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
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
                              .arg(channel + 1, 4, 10, QChar(u'0'));   // as on the channel bar

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
            QStringLiteral("Exporting channel %1 to workspace…").arg(channel + 1), 0);

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
                                 .arg(r.width).arg(r.height).arg(r.channel + 1);   // backend: 0-based
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
    win->setWindowTitle(QStringLiteral("Pixel Histogram, CH %1").arg(
            ui->sliderSlice->value()));
    win->resize(620, 380);
    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *info = new QLabel(QStringLiteral(
            "Slice %1 of %2  ·  range %3, %4  ·  %5 valid pixels  ·  drag cursors to clip LUT")
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
                    LutScale::setRange(self->lutSlice, self->m_sliceScaleMode, lo, hi);
                    self->lutSlice->Build();
                    self->setSliceRangeLocked(true);   // survives the next channel
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

QString vtkWindowCube::displayedImageProductId() const
{
    // The focused pane first, so with two maps side by side the export follows
    // the one the user is looking at rather than pane order.
    for (const bool focusedOnly : { true, false }) {
        for (int i = 0; i < 4; ++i) {
            const PaneView &pv = m_slots[i].view;
            if (pv.kind != ViewId::Product || !m_products2d.contains(pv.productId))
                continue;
            if (!m_slots[i].frame || !m_slots[i].frame->isVisible())
                continue;
            if (focusedOnly && i != m_activePane)
                continue;
            return pv.productId;
        }
    }
    return {};
}

void vtkWindowCube::updateExportSelectedAction()
{
    if (!m_actExportSelected)
        return;
    const PaneView pv = (m_activePane >= 0 && m_activePane < 4) ? m_slots[m_activePane].view
                                                                : PaneView{};
    QString label, why;
    switch (pv.kind) {
    case ViewId::View3D:
        label = u"Export Sub-Cube as FITS…"_s;
        why = u"Crop the cube to a spatial and spectral box and save it as a standalone "
              u"FITS in Workspace Exports. The bounds start from the drawn region, if any."_s;
        break;
    case ViewId::Slice2D:
        label = u"Export Current Channel as 2-D FITS…"_s;
        why = u"Save the channel on screen as a true 2-D FITS image — the spectral axis "
              u"is dropped, not kept as a degenerate dimension."_s;
        break;
    case ViewId::Spectrum:
        label = u"Export Spectrum as CSV…"_s;
        why = u"Save the spectrum in this pane as CSV."_s;
        break;
    case ViewId::Product: {
        const QString name = m_products2d.contains(pv.productId)
                ? m_products2d.value(pv.productId).label
                : (m_products1d.contains(pv.productId) ? m_products1d.value(pv.productId).label
                                                       : QString());
        if (m_products1d.contains(pv.productId)) {
            label = u"Export %1 as CSV…"_s.arg(name);
            why = u"Save this spectrum as CSV."_s;
        } else if (m_products2d.contains(pv.productId)) {
            label = u"Export %1 as FITS…"_s.arg(name);
            why = u"Save this map as a 2-D FITS in Workspace Exports, with the celestial "
                  u"WCS, its own BUNIT and the parameters it was computed with recorded "
                  u"as HISTORY."_s;
        }
        break;
    }
    case ViewId::None:
    default:
        break;
    }

    // A name, not a verdict: the label says what will be written, so the one
    // entry is as explicit as the four it replaced.
    const bool ready = !label.isEmpty();
    m_actExportSelected->setText(ready ? label : u"Export Selected View…"_s);
    m_actExportSelected->setEnabled(ready);
    m_actExportSelected->setToolTip(
            ready ? why
                  : u"Select a pane first: this exports what the focused pane is showing — "
                    u"the cube, the current channel, a map, or a spectrum."_s);
}

void vtkWindowCube::exportSelectedView()
{
    const PaneView pv = (m_activePane >= 0 && m_activePane < 4) ? m_slots[m_activePane].view
                                                                : PaneView{};
    switch (pv.kind) {
    case ViewId::View3D:
        exportSubCubeAsFits();
        return;
    case ViewId::Slice2D:
        exportCurrentChannelAs2dFits();
        return;
    case ViewId::Spectrum:
        if (auto *w = m_paneSpectra[m_activePane])
            w->saveSpectrumCsvNow();
        return;
    case ViewId::Product:
        if (m_products1d.contains(pv.productId)) {
            if (auto *w = m_paneSpectra[m_activePane])
                w->saveSpectrumCsvNow();
            return;
        }
        // A moment keeps its own exporter: that one sends a RECIPE and lets the
        // service recompute at full precision, which beats uploading the pixels
        // that happen to be on screen.
        if (pv.productId.startsWith(u"moment"_s))
            exportMomentMapAsFits(pv.productId);
        else
            exportDisplayedMapAsFits(pv.productId);
        return;
    case ViewId::None:
    default:
        QMessageBox::information(this, u"Export"_s,
                u"Select the pane holding what you want to save first."_s);
        return;
    }
}

void vtkWindowCube::exportDisplayedMapAsFits(const QString &requestedProductId)
{
    const QString productId = requestedProductId.isEmpty() ? displayedImageProductId()
                                                           : requestedProductId;
    if (productId.isEmpty()) {
        QMessageBox::warning(this, u"Export Map"_s,
                u"No 2-D map is on screen. Compute one — a moment, a line-width map, "
                u"a polarimetry map — and show it in a pane first."_s);
        return;
    }
    // A moment has a recipe, and exporting the recipe is better than exporting
    // the pixels: the service recomputes at full precision and records the
    // channels and mask it used. Only what cannot be recomputed goes up as
    // pixels.
    if (productId.startsWith(u"moment"_s)) {
        // Named explicitly: the moment exporter picks the first VISIBLE moment
        // in pane order, which with Moment 0 in an earlier pane and Moment 1
        // focused would export the one the user is not looking at.
        exportMomentMapAsFits(productId);
        return;
    }

    const auto it = m_products2d.constFind(productId);
    if (it == m_products2d.constEnd() || !it->imageData)
        return;
    // Everything needed is COPIED out now, before the dialog below spins a
    // nested event loop. A computation finishing during that loop calls
    // addImageProduct(), which inserts into m_products2d and invalidates this
    // iterator — and would otherwise pair pixels already read with another
    // product's label and unit.
    vtkImageData *img = it->imageData;
    const QString label = it->label;
    const QString unit = it->unit;
    const QPoint origin = it->regionOrigin;
    const QVariantMap params = ProductRegistry::instance().product(productId).params;
    int ext[6];
    img->GetExtent(ext);
    const int w = ext[1] - ext[0] + 1;
    const int h = ext[3] - ext[2] + 1;
    if (w <= 0 || h <= 0)
        return;

    // Copied out as float32 whatever the product stores, because that is what
    // the wire format and the FITS both are.
    QByteArray pixels(static_cast<qsizetype>(w) * h * static_cast<qsizetype>(sizeof(float)),
                      Qt::Uninitialized);
    auto *out = reinterpret_cast<float *>(pixels.data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            out[static_cast<qsizetype>(y) * w + x] =
                    img->GetScalarComponentAsFloat(ext[0] + x, ext[2] + y, 0, 0);
        }
    }

    QString base = label.simplified().toLower();
    base.replace(QRegularExpression(u"[^a-z0-9]+"_s), u"_"_s);
    while (base.endsWith(u'_'))
        base.chop(1);
    if (base.isEmpty())
        base = u"map"_s;
    const QFileInfo srcInfo(this->filepath);
    bool ok = false;
    const QString basename = QInputDialog::getText(
            this, u"Export Map as FITS"_s,
            u"Save in Workspace Exports as (.fits is appended if missing;\n"
            u"existing names get a _1, _2 suffix):"_s,
            QLineEdit::Normal, u"%1_%2.fits"_s.arg(srcInfo.completeBaseName(), base), &ok);
    if (!ok)
        return;

    // Provenance as HISTORY cards. These are the same rows the Inspector shows
    // for the product, so what the file records and what the window says are
    // one thing — a map whose parameters live only in a screenshot is not a
    // result anyone can check.
    // Provenance from the registry — the same rows the Inspector shows, so the
    // file records exactly what the window says.
    QStringList history;
    history << u"%1 computed by VisIVO from %2"_s.arg(label, srcInfo.fileName());
    for (auto p = params.constBegin(); p != params.constEnd(); ++p)
        history << u"%1: %2"_s.arg(p.key(), p.value().toString());

    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString sessionId = this->remoteSessionId, datasetId = this->remoteDatasetId;

    this->statusBar()->showMessage(u"Exporting %1 to Workspace Exports…"_s.arg(label), 0);
    auto *watcher = new QFutureWatcher<BackendSaveChannel2dResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSaveChannel2dResult>::finished, this,
                     [self, watcher, label]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self)
                             return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Export Map"_s,
                                     r.error.isEmpty() ? u"The export failed."_s : r.error);
                             return;
                         }
                         QMessageBox::information(self, u"Export Map"_s,
                                 u"%1 saved to the Workspace Exports as 2-D FITS.\n\n"
                                 u"Filename: %2\n"
                                 u"Path: %3\n"
                                 u"Shape: %4 × %5 (W × H)\n\n"
                                 u"Browse / download / delete from the\n"
                                 u"Workspace Exports panel in the Data Hub, or open it\n"
                                 u"in the image viewer."_s
                                         .arg(label, r.workspaceFilename, r.outputPath)
                                         .arg(r.width).arg(r.height));
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, sessionId, datasetId, pixels, w, h, basename, unit, history, origin]() {
                BackendClient c(url, token);
                c.setSessionId(sessionId);
                return c.saveMap2dToWorkspace(datasetId, pixels, w, h, basename, unit,
                                              history, origin);
            }));
}

void vtkWindowCube::exportMomentMapAsFits(const QString &requestedProductId)
{
    if (!this->momentController) return;
    // §3.2: export the recipe of the moment currently DISPLAYED — with several moments
    // cached they differ. Prefer a VISIBLE Product pane's moment; else the most recently
    // computed one; else the controller's last accepted config (single-moment case).
    struct { int order, channelStart, channelEnd; bool maskEnabled;
             double thresholdValue; bool thresholdAuto; } cfg{};
    QString exportId = requestedProductId;
    for (int i = 0; exportId.isEmpty() && i < 4; ++i) {
        const PaneView &pv = m_slots[i].view;
        // Only a MOMENT product carries the export recipe (order/channels/mask). m_products2d
        // now also holds other image products (line-width, …) with default order/channels —
        // skip those or the export reads a bogus recipe.
        if (pv.kind == ViewId::Product && pv.productId.startsWith(u"moment"_s)
            && m_slots[i].frame && m_slots[i].frame->isVisible()
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
    // zero-extent config here would be a bug. Defensive guard anyway (a valid
    // single-channel moment has channelEnd == channelStart, so reject only an
    // inverted range).
    if (cfg.channelEnd < cfg.channelStart) {
        QMessageBox::warning(this, u"Export Moment as FITS"_s,
                u"No moment has been computed yet - open Tools → Compute "
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

QString vtkWindowCube::formatSpatialPointSummary(const std::array<double, 2> &pixel) const
{
    QString axis0 = this->remoteFormatAxisCoordinate(0, pixel[0]);
    QString axis1 = this->remoteFormatAxisCoordinate(1, pixel[1]);
    if (this->remoteHasCelestialAxes()) {
        bool ok0 = false;
        bool ok1 = false;
        const double nativeX = this->remoteVoxelToWcs(0, pixel[0], &ok0);
        const double nativeY = this->remoteVoxelToWcs(1, pixel[1], &ok1);
        double frameX = nativeX;
        double frameY = nativeY;
        if (ok0 && ok1 && this->convertRemoteCelestialCoordinates(nativeX, nativeY, frameX, frameY)) {
            axis0 = this->formatRemoteOverlayCoordinate(0, frameX);
            axis1 = this->formatRemoteOverlayCoordinate(1, frameY);
        }
    }
    return u"(%1, %2)  %3=%4  %5=%6"_s.arg(pixel[0], 0, 'f', 1)
            .arg(pixel[1], 0, 'f', 1)
            .arg(this->selectedFrameAxisTitle(0))
            .arg(axis0)
            .arg(this->selectedFrameAxisTitle(1))
            .arg(axis1);
}

std::vector<std::array<double, 2>> vtkWindowCube::pvEffectivePath() const
{
    return this->m_pvSmooth ? pvpath::smoothed(this->pvPolylineVertices, 8)
                            : this->pvPolylineVertices;
}

bool vtkWindowCube::pvPixelFromDisplay(int displayX, int displayY,
                                       std::array<double, 2> &pixel) const
{
    auto *renderer = this->active2dWin() ? this->active2dWin()->GetRenderers()->GetFirstRenderer()
                                         : nullptr;
    auto *imageData = this->viewingSlice()
            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
            : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    if (!renderer || !imageData)
        return false;
    this->coordinate->SetValue(displayX, displayY);
    const double *world = this->coordinate->GetComputedWorldValue(renderer);
    if (!world)
        return false;
    int extent[6];
    double origin[3];
    double spacing[3];
    imageData->GetExtent(extent);
    imageData->GetOrigin(origin);
    imageData->GetSpacing(spacing);
    if (spacing[0] == 0.0 || spacing[1] == 0.0)
        return false;
    const double x = extent[0] + (world[0] - origin[0]) / spacing[0];
    const double y = extent[2] + (world[1] - origin[1]) / spacing[1];
    // Half a pixel of slack: the edge pixels are clickable all the way to their border.
    if (x < extent[0] - 0.5 || x > extent[1] + 0.5 || y < extent[2] - 0.5 || y > extent[3] + 0.5)
        return false;
    pixel = { x, y };
    return true;
}

bool vtkWindowCube::pvDisplayFromPixel(const std::array<double, 2> &pixel,
                                       std::array<double, 2> &display) const
{
    auto *renderer = this->active2dWin() ? this->active2dWin()->GetRenderers()->GetFirstRenderer()
                                         : nullptr;
    if (!renderer)
        return false;
    // The overlay draws pixel (x, y) at world (x, y) (see refreshPvOverlay).
    renderer->SetWorldPoint(pixel[0], pixel[1], 0.0, 1.0);
    renderer->WorldToDisplay();
    const double *d = renderer->GetDisplayPoint();
    display = { d[0], d[1] };
    return true;
}

std::vector<std::array<double, 2>> vtkWindowCube::pvDisplayPath(
        const std::vector<std::array<double, 2>> &pixels) const
{
    std::vector<std::array<double, 2>> out;
    out.reserve(pixels.size());
    for (const auto &p : pixels) {
        std::array<double, 2> d{};
        if (!this->pvDisplayFromPixel(p, d))
            return {};
        out.push_back(d);
    }
    return out;
}

bool vtkWindowCube::pvEditing() const
{
    return this->pvModeActive && this->pvValid && !this->pvDragging
            && this->pvPolylineVertices.size() >= 2;
}

namespace {
// Screen-pixel reach of a handle / of the path for the editing gestures.
constexpr double kPvHandleTolerance = 9.0;
constexpr double kPvPathTolerance = 7.0;
}  // namespace

bool vtkWindowCube::beginPvEdit(int displayX, int displayY)
{
    const auto display = this->pvDisplayPath(this->pvPolylineVertices);
    if (display.size() != this->pvPolylineVertices.size())
        return false;
    const std::array<double, 2> at{ double(displayX), double(displayY) };
    std::array<double, 2> pixel{};
    if (!this->pvPixelFromDisplay(displayX, displayY, pixel)) {
        // Off the image: the hit test still works on the path, but the anchor
        // needs a pixel — extrapolate through the renderer instead.
        auto *renderer = this->active2dWin()->GetRenderers()->GetFirstRenderer();
        renderer->SetDisplayPoint(displayX, displayY, 0.0);
        renderer->DisplayToWorld();
        const double *w = renderer->GetWorldPoint();
        pixel = { w[0] / (w[3] != 0.0 ? w[3] : 1.0), w[1] / (w[3] != 0.0 ? w[3] : 1.0) };
    }
    const int vertex = pvpath::hitVertex(display, at, kPvHandleTolerance);
    auto *iren = this->active2dWin()->GetInteractor();
    if (vertex >= 0) {
        this->m_pvEdit = PvEdit::MoveVertex;
        this->m_pvEditVertex = vertex;
    } else if (pvpath::hitSegment(pvDisplayPath(this->pvEffectivePath()), at, kPvPathTolerance).segment >= 0) {
        this->m_pvEdit = iren && iren->GetShiftKey() ? PvEdit::Rotate : PvEdit::Translate;
    } else {
        return false;
    }
    this->m_pvEditAnchor = pixel;
    this->m_pvEditStart = this->pvPolylineVertices;
    // The reference angle is taken on the first move clear of the centre (see
    // updatePvEdit): pressed on the middle of a straight path — the natural grab
    // point, and its centroid — atan2 of a near-zero vector is arbitrary.
    this->m_pvEditStartAngle = std::numeric_limits<double>::quiet_NaN();
    return true;
}

void vtkWindowCube::updatePvEdit(int displayX, int displayY)
{
    if (this->m_pvEdit == PvEdit::None)
        return;
    auto *renderer = this->active2dWin()->GetRenderers()->GetFirstRenderer();
    renderer->SetDisplayPoint(displayX, displayY, 0.0);
    renderer->DisplayToWorld();
    const double *w = renderer->GetWorldPoint();
    const double h = w[3] != 0.0 ? w[3] : 1.0;
    const std::array<double, 2> pixel{ w[0] / h, w[1] / h };
    switch (this->m_pvEdit) {
    case PvEdit::MoveVertex:
        if (this->m_pvEditVertex >= 0
            && this->m_pvEditVertex < static_cast<int>(this->pvPolylineVertices.size()))
            this->pvPolylineVertices[std::size_t(this->m_pvEditVertex)] = pixel;
        break;
    case PvEdit::Translate:
        this->pvPolylineVertices = pvpath::translated(this->m_pvEditStart,
                                                      pixel[0] - this->m_pvEditAnchor[0],
                                                      pixel[1] - this->m_pvEditAnchor[1]);
        break;
    case PvEdit::Rotate: {
        const auto center = pvpath::centroid(this->m_pvEditStart);
        if (std::hypot(pixel[0] - center[0], pixel[1] - center[1]) < 2.0)
            return;   // too close to the centre to define an angle
        if (std::isnan(this->m_pvEditStartAngle)) {
            this->m_pvEditStartAngle = pvpath::angleAround(center, pixel);
            return;   // reference set; rotation starts with the next move
        }
        this->pvPolylineVertices = pvpath::rotated(
                this->m_pvEditStart, center,
                pvpath::angleAround(center, pixel) - this->m_pvEditStartAngle);
        break;
    }
    case PvEdit::None:
        break;
    }
    this->refreshPvOverlay();
    this->active2dWin()->Render();
    this->schedulePvLiveUpdate(150);   // the diagram follows the drag
}

void vtkWindowCube::endPvEdit()
{
    const bool changed = this->m_pvEdit != PvEdit::None
            && this->pvPolylineVertices != this->m_pvEditStart;
    this->m_pvEdit = PvEdit::None;
    this->m_pvEditVertex = -1;
    this->m_pvEditStart.clear();
    if (changed)
        this->schedulePvLiveUpdate(0);
}

void vtkWindowCube::editPvVertexAt(int displayX, int displayY, bool insert)
{
    // A double-click begins with a press that may have started a drag: drop it
    // first, or the next mouse move would restore the path it captured.
    this->m_pvEdit = PvEdit::None;
    this->m_pvEditStart.clear();
    const std::array<double, 2> at{ double(displayX), double(displayY) };
    std::vector<std::array<double, 2>> next = this->pvPolylineVertices;
    if (insert) {
        // Hit-test what is DRAWN (the smooth curve when on) and map the span back
        // to its control interval: the curve has 8 samples per control segment.
        const auto display = this->pvDisplayPath(this->pvEffectivePath());
        const auto hit = pvpath::hitSegment(display, at, kPvPathTolerance);
        std::array<double, 2> pixel{};
        if (hit.segment < 0 || !this->pvPixelFromDisplay(displayX, displayY, pixel))
            return;
        const bool curved = this->m_pvSmooth && this->pvPolylineVertices.size() >= 3;
        const int controlSegment = curved ? hit.segment / 8 : hit.segment;
        next = pvpath::withVertexInserted(next, controlSegment, pixel);
    } else {
        const int vertex = pvpath::hitVertex(this->pvDisplayPath(this->pvPolylineVertices), at,
                                             kPvHandleTolerance);
        if (vertex < 0)
            return;
        if (next.size() <= 2) {
            this->statusBar()->showMessage(u"A PV path needs at least two vertices."_s, 2500);
            return;
        }
        next = pvpath::withVertexRemoved(next, vertex);
    }
    this->pvPolylineVertices = std::move(next);
    this->refreshPvOverlay();
    this->active2dWin()->Render();
    this->schedulePvLiveUpdate(0);
}

void vtkWindowCube::schedulePvLiveUpdate(int delayMs)
{
    if (!this->m_pvLiveTimer) {
        this->m_pvLiveTimer = new QTimer(this);
        this->m_pvLiveTimer->setSingleShot(true);
        QObject::connect(this->m_pvLiveTimer, &QTimer::timeout, this, [this]() {
            if (!this->pvValid || this->pvPolylineVertices.size() < 2 || !this->pvController
                || this->remoteDatasetId.isEmpty())
                return;
            this->pvController->startExtraction(this->pvEffectivePath(), this->pvWidthPixels,
                                                /*interactive=*/true);
        });
    }
    this->m_pvLiveTimer->start(std::max(0, delayMs));
}

void vtkWindowCube::startNewPvPath()
{
    this->m_pvEdit = PvEdit::None;
    this->pvPolylineVertices.clear();
    this->pvValid = false;
    this->pvDragging = false;
    this->pvCursorValid = false;
    this->refreshPvOverlay();
    if (this->actionExtractPvDiagram && !this->actionExtractPvDiagram->isChecked())
        this->actionExtractPvDiagram->setChecked(true);   // → setPvModeActive(true)
    this->statusBar()->showMessage(
            u"Click on the image to place the vertices of a new path; double-click or "
            u"right-click to finish it."_s, 5000);
    if (auto *win = this->active2dWin())
        win->Render();
}

void vtkWindowCube::exportPvAsFits()
{
    if (this->m_pvShownVertices.size() < 2 || this->remoteDatasetId.isEmpty()) {
        QMessageBox::warning(this, u"Save PV as FITS"_s,
                             u"There is no PV diagram to save yet."_s);
        return;
    }
    const QFileInfo srcInfo(this->filepath);
    bool ok = false;
    const QString basename = QInputDialog::getText(
            this, u"Save PV as FITS"_s,
            u"Save in Workspace Exports as (.fits will be appended if missing;\n"
            u"existing names get a _1, _2 suffix):"_s,
            QLineEdit::Normal, u"%1_pv.fits"_s.arg(srcInfo.completeBaseName()), &ok);
    if (!ok)
        return;
    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sessionId = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;
    const auto vertices = this->m_pvShownVertices;
    const int width = this->m_pvShownWidth;
    this->statusBar()->showMessage(u"Saving the PV diagram as FITS…"_s, 0);
    auto *watcher = new QFutureWatcher<BackendSavePvFitsResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSavePvFitsResult>::finished, this,
                     [self, watcher]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self)
                             return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Save PV as FITS"_s,
                                                  u"PV FITS export failed:\n\n%1"_s.arg(r.error));
                             return;
                         }
                         emit self->workspaceExportsChanged();
                         QString msg = u"PV diagram saved to the Workspace Exports.\n\n"
                                       u"Filename: %1\nPath: %2\n"
                                       u"Shape: %3 offsets × %4 channels"_s
                                               .arg(r.workspaceFilename, r.outputPath)
                                               .arg(r.width).arg(r.height);
                         if (!r.bunit.isEmpty())
                             msg += u"\nBUNIT: %1"_s.arg(r.bunit);
                         msg += u"\n\nAxis 1 = offset along the path (arcsec), axis 2 = the "
                                u"cube's spectral axis."_s;
                         QMessageBox::information(self, u"Save PV as FITS"_s, msg);
                     });
    watcher->setFuture(QtConcurrent::run([url, token, sessionId, datasetId, vertices, width,
                                          basename]() {
        BackendClient client(url, token);
        client.setSessionId(sessionId);
        return client.savePvFits(datasetId, vertices, width, basename.trimmed());
    }));
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
    this->pvController->startExtraction(this->pvEffectivePath(), this->pvWidthPixels);
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
    // The slice view mounted anywhere visible settles it: tools bind to that view,
    // not to the focused pane (the shared moment window below is never shown).
    if (slice2dVisible())
        return true;
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
    // What the load will hold at its peak: the volume as it arrives (the slabs),
    // the volume assembled, and its copy on the GPU — three times the region,
    // beside whatever is already resident. A machine without that much RAM
    // was left to find out by swapping until the system killed the client
    // (a 1.9 GB cube on a laptop reached a 45 GB footprint); it is told first.
    const double roiBytes = double(roi[1] - roi[0] + 1) * double(roi[3] - roi[2] + 1)
            * double(roi[5] - roi[4] + 1) * 4.0;
    const double physical = double(hostPhysicalMemoryBytes());
    if (physical > 0.0 && 3.0 * roiBytes > physical) {
        const auto gib = [](double b) { return QString::number(b / (1024.0 * 1024.0 * 1024.0), 'f', 1); };
        qWarning().noquote() << QStringLiteral("[remote-roi] full-resolution load refused: "
                                               "needs ~%1 GiB, machine has %2 GiB")
                                        .arg(gib(3.0 * roiBytes), gib(physical));
        QMessageBox::warning(
                this, u"Full resolution"_s,
                u"This region is %1 GiB at full resolution. Loading it holds about three "
                u"times that, %2 GiB, while it arrives and is handed to the GPU, and this "
                u"machine has %3 GiB of memory.\n\nThe decimated proxy stays on screen. "
                u"Crop a region (Tools \u25b8 Crop Full Resolution), or render on a "
                u"remote backend."_s
                        .arg(gib(roiBytes), gib(3.0 * roiBytes), gib(physical)));
        // Nothing was requested, so nothing changes: whatever image is installed
        // (the proxy, or a smaller ROI loaded earlier) keeps its state, and the
        // caller that cleared a flag ahead of this call puts it back.
        return false;
    }
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

void vtkWindowCube::setRemoteCuttingPlaneVisible(bool visible)
{
    if (this->remoteCuttingPlaneActor)
        this->remoteCuttingPlaneActor->SetVisibility(visible ? 1 : 0);
    if (this->remoteCuttingPlaneEdgeActor)
        this->remoteCuttingPlaneEdgeActor->SetVisibility(visible ? 1 : 0);
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
        this->setRemoteCuttingPlaneVisible(false);
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
        this->setRemoteCuttingPlaneVisible(false);
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
    // From the checkbox, not unconditionally on: switching the plane off and
    // then changing channel used to bring it straight back.
    this->setRemoteCuttingPlaneVisible(!this->actionShowCuttingPlane
                                       || this->actionShowCuttingPlane->isChecked());
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
    // A lasso selection on screen was placed against the display we just replaced.
    this->refreshLassoAfterDisplaySwap();
    // …and so was any amber ROI preview box (Export Sub-Cube / Mask 3-D / noise).
    this->refreshRegionPreviewAfterDisplaySwap();
    this->remoteIsosurfaceReady = false;
    this->setCubeRenderModeLocally(false);
    if (result.momentImageData) {
        this->momentDisplaySource->SetOutput(result.momentImageData);
        this->momentDisplaySource->Modified();
        LutScale::setRange(this->lutMoment, this->m_sliceScaleMode,
                           result.momentRange[0], result.momentRange[1]);
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
    // Append the brightness unit (BUNIT, e.g. "Jy/beam") so the 3D-cube stats read with units
    // like the 2D-image block, instead of bare numbers.
    const QString bu = this->m_brightnessUnit.isEmpty() ? QString()
                                                        : (u" "_s + this->m_brightnessUnit);
    ui->lineCubeMin->setText(QString::number(this->currentCubeVisibleRange[0]) + bu);
    ui->lineCubeMax->setText(QString::number(this->currentCubeVisibleRange[1]) + bu);
    ui->lineCubeMean->setText(QString::number(result.cubeMean) + bu);
    ui->lineCubeRms->setText(QString::number(result.cubeRms) + bu);
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
    // §3.0b (d): the moment provenance now lives in Inspector › Parameters (fed via
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
    const bool on = ui->checkContours->isChecked();
    this->contoursActor->SetVisibility(on);
    // The tool's settings live in the Inspector's Parameters tab while the tool
    // is on. They used to live in a .ui page (pageContours) inside the hidden
    // sidebar container - built, wired, and unreachable: Level / Lower / Upper
    // could not be typed at all, which is also why the bounds bug below stayed
    // invisible for so long.
    if (on && ui->frameContours) {
        this->claimToolSettings(u"Contours"_s, ui->frameContours);
    } else {
        this->releaseToolSettings(u"Contours"_s);
        this->refreshToolSettingsPanel(/*reveal=*/false);
    }
    this->sliceWin->Render();
}

void vtkWindowCube::setMomentOrder(int order)
{
    // Cross-system busy guard: block moment computation while the cube itself
    // is still loading (slice/isosurface requests in flight, etc.).
    if (this->isBusy()) {
        return;
    }
    // The card transition and the start diagnostics are driven by
    // MomentMapController::computationStarted: the settings window is non-modal,
    // so isBusy() right here is false (Compute has not been pressed yet) and
    // both used to be skipped entirely.
    this->momentController->startComputation(order);
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
    // Whatever reason the bar is being cleared for, a slice error that was on it
    // is gone with the message - the flag must not outlive what it protects.
    this->m_sliceErrorOnStatusBar = false;
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
    const int levels = ui->lineLevel->text().toInt();
    const double lower = ui->lineLowerBound->text().toDouble();
    const double upper = ui->lineUpperBound->text().toDouble();
    // An inverted or empty range is a user-input mistake, not something to draw
    // a guess from, so say why instead of leaving a toggle that does nothing.
    if (levels < 1 || !(lower < upper)) {
        this->statusBar()->showMessage(
                u"Contour levels need Lower < Upper and at least one level."_s, 4000);
        return;
    }
    this->contours->GenerateValues(levels, lower, upper);
    // The mapper's colour range was built from the placeholder bounds too;
    // without this every line comes out the same end-of-table colour.
    if (this->contoursActor && this->contoursActor->GetMapper())
        this->contoursActor->GetMapper()->SetScalarRange(lower, upper);
    this->sliceWin->Render();
}

void vtkWindowCube::refreshContourBoundsFromData()
{
    // Only the first time: after that the boxes hold the user's own numbers and
    // a later data update must not overwrite them.
    if (m_contourBoundsInitialised)
        return;
    if (!(this->lowerBound < this->upperBound))
        return;
    // The fields are the source of truth for updateContours(), so the bounds
    // have to survive the round trip through text: QString::number's default
    // six significant digits can collapse two close bounds onto one number,
    // which would then fail validation with the flag already spent.
    const QString lowerText = QString::number(this->lowerBound, 'g', 9);
    const QString upperText = QString::number(this->upperBound, 'g', 9);
    if (!(lowerText.toDouble() < upperText.toDouble()))
        return;
    {
        const QSignalBlocker b(ui->lineLowerBound);
        ui->lineLowerBound->setText(lowerText);
    }
    {
        const QSignalBlocker b(ui->lineUpperBound);
        ui->lineUpperBound->setText(upperText);
    }
    m_contourBoundsInitialised = true;
    this->updateContours();
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
                             // rebuild lutSlice with a stale name. Key on the dialog's
                             // captured target, not the (non-modal, Product-inclusive)
                             // viewingSlice() — else a slice-bound Apply after switching panes
                             // would leave m_activeSliceLutName stale and broadcast the old
                             // colormap.
                             if (this->m_lutCustomizerTargetsSlice && this->lutSlice) {
                                 const std::string nm = this->lutSlice->GetObjectName();
                                 if (!nm.empty())
                                     this->m_activeSliceLutName = QString::fromStdString(nm);
                                 // STEP6-P4: the customizer rebuilt lutSlice via LutScale::build,
                                 // WITHOUT re-applying m_sliceInvert — re-run applySliceLutByName so
                                 // the sender's slice LUT matches (scale+gamma+invert) what it
                                 // broadcasts, and broadcast the slice colour once through it. Only
                                 // when the dialog actually targets the SLICE (it also edits the
                                 // moment LUT); the captured target is reliable, viewingSlice() is
                                 // not (non-modal; true for a Product pane).
                                 this->applySliceLutByName(this->m_activeSliceLutName);
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

    // STEP6-P4: capture which LUT the dialog targets NOW (its callback fires later, when
    // viewingSlice() may differ) so only a genuine slice edit broadcasts to linked peers.
    this->m_lutCustomizerTargetsSlice = this->viewingSlice();
    if (this->m_lutCustomizerTargetsSlice) {
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

QString vtkWindowCube::scopedProductId(const QString &stem) const
{
    return QStringLiteral("%1@%2#%3").arg(stem, this->datasetPersistKey(), this->m_windowScope);
}

QString vtkWindowCube::registerAnalysisProduct(const QString &kind, const QString &label,
                                               const QVariantMap &params, QWidget *window,
                                               const QString &stableId, const QString &renderer,
                                               bool revealProvenance,
                                               const QVariantMap &origin,
                                               bool selectInSession,
                                               const QList<ReplayStep> &replay,
                                               const QString &replayCaveat)
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
    p.ownerId = this->m_windowScope;   // this window owns + renders the product
    p.window = window;
    p.origin = origin;
    p.replay = replay;
    p.replayCaveat = replayCaveat;
    const QString id = ProductRegistry::instance().registerProduct(std::move(p));
    if (this->m_workspaceChrome) {
        // Select the new product first (so Provenance is populated), then auto-expand
        // the Inspector onto Provenance (§4.1.4) — otherwise the tab reveals empty
        // (§3.0b (c)). Also refresh the left rail's product count.
        if (selectInSession)
            this->m_workspaceChrome->selectSessionProduct(id);
        if (revealProvenance)
            this->m_workspaceChrome->revealParametersTab();
        // Count only THIS window's products (the registry is session-wide; a sibling window on
        // the same dataset shares parentKey but has a different ownerId).
        const auto dsProducts =
                ProductRegistry::instance().productsFor(this->datasetPersistKey());
        int mine = 0;
        for (const AnalysisProduct &ap : dsProducts)
            if (ap.ownerId == this->m_windowScope)
                ++mine;
        this->m_workspaceChrome->setSessionRailCount(mine);
    }
    return id;
}

QVector<vtkWindowCube::CubeTool> vtkWindowCube::cubeTools() const
{
    // Grouped by what the astronomer is trying to DO, not by which part of the
    // code owns the action. The Tools menu used to list all of these in one run
    // of thirty-one entries broken by two separators, which is a list you read
    // top to bottom every time instead of going where you meant to.
    const auto *self = this;
    QVector<CubeTool> t;
    // No label: the QAction carries its own text, and every surface now reads it
    // from there. A second copy here would be one more thing to keep in step.
    const auto add = [&t](const QString &g, QAction *a) {
        if (a)
            t.append({ g, a });
    };

    add(u"Spectral"_s, ui->actionExtractSpectrum);
    // The 3-D counterpart of Extract Spectrum: click the cutting plane in the
    // 3-D view instead of a pixel on the slice. The gate already knew about it
    // (view3dTools) but it was listed nowhere a user browses tools — only in
    // the View menu and the INTERACTION dock.
    add(u"Spectral"_s, self->actionPickSpectrum3d);
    add(u"Spectral"_s, self->m_actPinSpectrum);
    add(u"Spectral"_s, self->actionExtractPvDiagram);
    add(u"Spectral"_s, ui->actionLinewidthMap);
    add(u"Spectral"_s, ui->actionBaselineSubtract);
    add(u"Spectral"_s, ui->actionStackCubes);

    add(u"Maps"_s, ui->actionComputeMoment);
    add(u"Maps"_s, self->m_actChannelMaps);

    add(u"Statistics"_s, self->actionEstimateNoise);
    add(u"Statistics"_s, self->m_actSliceHistogram);

    // Polarimetry (vtkWindowCube_Polarisation.cpp). This table is the ONLY way
    // into the Tools menu: regroupToolsMenu() rebuilds the menu from it and
    // detaches everything it does not list, so an action added straight to
    // menuTools is created, wired, and then silently removed before the user
    // ever sees it. The Inspector's Analysis tab is built from the same table,
    // so listing here is also what puts them in the sidebar.
    add(u"Polarimetry"_s, self->m_actLoadStokesCompanions);
    add(u"Polarimetry"_s, self->m_actPolarisedIntensity);
    add(u"Polarimetry"_s, self->m_actRmSynthesis);

    add(u"Regions"_s, self->actionBoxRegion);
    add(u"Regions"_s, self->actionCircleRegion);
    add(u"Regions"_s, self->actionPolygonRegion);
    add(u"Regions"_s, self->actionAnnulusRegion);
    add(u"Regions"_s, self->m_actMaskRegion);
    add(u"Regions"_s, self->m_actImportCubeRegion);
    add(u"Regions"_s, self->m_actExportCubeRegion);

    add(u"Sources & Kinematics"_s, self->m_actSourceFind);
    add(u"Sources & Kinematics"_s, self->m_actKinematicLasso);
    add(u"Sources & Kinematics"_s, self->m_actKinematicModel);

    add(u"Catalogue"_s, self->actionLoadCatalogueOverlay);
    add(u"Catalogue"_s, self->actionShowCatalogueOverlay);
    add(u"Catalogue"_s, self->actionShowCatalogueLabels);
    add(u"Catalogue"_s, self->actionClearCatalogueOverlay);

    // ONE export of the data, which follows the focused pane and re-labels
    // itself. The four it replaced — sub-cube, current channel, displayed map,
    // moment map — are still the code underneath, and are still reachable
    // programmatically; they are simply not four things the user has to choose
    // between while looking at one pane. Smooth / Regrid writes a new cube
    // rather than exporting this one, and a movie is not the data, so those
    // two keep their own entries.
    add(u"Export"_s, self->m_actExportSelected);
    add(u"Export"_s, self->m_actFilterFits);
    add(u"Export"_s, self->m_actExportMovie);

    add(u"Views"_s, self->m_actSendSliceToImage);
    add(u"Views"_s, self->m_actLinkViews);
    add(u"Views"_s, self->m_actOpenInVR);
    return t;
}

void vtkWindowCube::regroupToolsMenu()
{
    if (!ui->menuTools)
        return;
    // Rebuild rather than reorder: the actions were added to the menu from a
    // dozen places as they were created, so their order is construction order.
    //
    // NOT via QMenu::clear(). Most of these were created by
    // menu->addAction("text"), which parents them to the MENU — and clear()
    // deletes the actions it owns. It spares one that is currently shown in
    // another widget, which would have made this look fine as long as the
    // Inspector happened to be built first, and turn into dangling QActions the
    // day that order changed. Reparent to the window
    // first, then detach one by one.
    const auto tools = cubeTools();
    const auto existing = ui->menuTools->actions();
    for (QAction *a : existing) {
        if (!a)
            continue;
        if (a->parent() == ui->menuTools)
            a->setParent(this);
        ui->menuTools->removeAction(a);
    }

    QString current;
    QMenu *sub = nullptr;
    for (const CubeTool &t : tools) {
        if (!t.action)
            continue;
        if (t.group != current) {
            current = t.group;
            sub = ui->menuTools->addMenu(current);
            // Not inherited from the parent menu: without this the "why is this
            // greyed out" tooltips that menuTools shows would stop appearing the
            // moment the entries moved into submenus.
            if (sub)
                sub->setToolTipsVisible(ui->menuTools->toolTipsVisible());
        }
        if (sub)
            sub->addAction(t.action);
    }
}


void vtkWindowCube::regroupViewMenu()
{
    if (!ui->menuView)
        return;
    // Same treatment, and the same reason, as regroupToolsMenu(): the entries
    // are added from a dozen places as they are created, so the menu's order was
    // construction order. It had grown to twenty-seven entries with submenus
    // interleaved — display state, docks, animation, data resolution,
    // diagnostics and one actual TOOL, in the order the code happened to run.
    //
    // Four groups, each answering a different question:
    //   Workspace  — where do the panels go
    //   Display    — how is the data drawn
    //   Animation  — how does it move
    //   Data       — how much of it is loaded
    // with the developer-facing entries last, after a separator.
    //
    // Removed rather than moved: "Pick Spectrum on Plane Click" is a TOOL and
    // lives in Tools ▸ Spectral and Inspector ▸ Analysis, and "Use Camera ROI"
    // is about what the camera frames, so it is in Camera now.
    //
    // NOT via QMenu::clear() — see regroupToolsMenu() for why that deletes
    // actions the menu owns. Reparent, then detach.
    // Every menu in this window shows its action tooltips, so a greyed entry can
    // state its reason. It was set on Tools alone, which is why the Camera and
    // Edit tooltips written next to their actions never appeared.
    for (QMenu *m : { ui->menuView, ui->menuCamera, ui->menuEdit, ui->menuWCS,
                      ui->menu3DRendering, ui->menu2DPanel,
                      this->m_menuVolumeRendering.data(), m_menuCuttingPlaneOpacity.data(),
                      m_menuRenderMode.data(),
                      m_menuAnimationSpeed.data(), m_menuAnimationMode.data() }) {
        if (m)
            m->setToolTipsVisible(true);
    }

    const auto existing = ui->menuView->actions();
    for (QAction *a : existing) {
        if (!a)
            continue;
        if (a->parent() == ui->menuView)
            a->setParent(this);
        ui->menuView->removeAction(a);
    }

    const auto addAct = [this](QAction *a) {
        if (a)
            ui->menuView->addAction(a);
    };
    const auto addSub = [this](QMenu *m) {
        if (m)
            ui->menuView->addMenu(m);
    };
    bool needSeparator = false;
    const auto group = [this, &needSeparator]() {
        if (needSeparator)
            ui->menuView->addSeparator();
        needSeparator = true;
    };

    // Workspace: what is on screen around the data.
    group();
    addAct(this->actionShowSessionPanel);
    addAct(this->actionShowControlsSidebar);
    addAct(m_actFocusMode);
    addAct(this->actionResetLayout);

    // Display: how the data is drawn. The 3-D representation and its blend mode
    // belong together, as do the cutting plane and its opacity.
    group();
    addSub(ui->menu3DRendering);
    addSub(this->m_menuVolumeRendering);
    // Where the 3-D view is drawn: on this machine or on the backend's GPU.
    addSub(this->m_menuRenderMode);
    addAct(this->actionShowCuttingPlane);
    addSub(m_menuCuttingPlaneOpacity);
    addSub(ui->menu2DPanel);
    addAct(this->actionShowWcsAxes);
    addAct(this->actionShow3dWcsAxes);

    // Animation along the spectral axis.
    group();
    addAct(this->actionPlaySlices);
    addSub(m_menuAnimationSpeed);
    addSub(m_menuAnimationMode);

    // How much of the cube is actually loaded (absent on a small cube).
    if (m_actLoadFullRes || m_actCropFullRes) {
        group();
        addAct(m_actLoadFullRes);
        addAct(m_actCropFullRes);
    }

    // Developer-facing, last.
    group();
    addAct(m_actDiagnostics);
    addAct(m_actCommandPalette);

    // Anything added after this point (a dock's toggleViewAction, for one) lands
    // at the end, which is where a per-dataset entry belongs.
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
            "QPushButton{background:transparent;border:none;color:%1;%2"
            "padding:8px 14px;text-align:left;}")
            .arg(fgMuted, VisivoTheme::sectionHeaderCss());

    // Collapsible flat section: header (▼/▶) + content. A full-width rule is drawn
    // only when @p divider — a section reads from the weight of its header, so the
    // collapsible ▶ sections (INTERACTION / SLICE ANIMATION) carry no line (§3.0c).
    // QPushButton, not QToolButton: Qt's stylesheet `text-align` is honoured by
    // QPushButton and silently ignored by QToolButton, which always centres its
    // label. The headers had carried `text-align:left` all along and still came
    // out centred, out of line with every control underneath them.
    auto addSection = [&](const QString &title, bool expanded, bool divider = true,
                          QPushButton **outHdr = nullptr) -> QVBoxLayout * {
        auto *hdr = new QPushButton(root);
        if (outHdr)
            *outHdr = hdr;
        hdr->setCheckable(true);
        hdr->setChecked(expanded);
        hdr->setCursor(Qt::PointingHandCursor);
        hdr->setFlat(true);
        // TabFocus is what QToolButton set for itself, so the headers stay
        // reachable by keyboard exactly as before; QPushButton's own default
        // (StrongFocus) would additionally take focus on a click.
        hdr->setFocusPolicy(Qt::TabFocus);
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
        QObject::connect(hdr, &QPushButton::toggled, content,
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
    QPushButton *dispHdr = nullptr;
    QVBoxLayout *disp = addSection(u"DISPLAY, 3D"_s, true, true, &dispHdr);
    m_displayHeaderBtn = dispHdr;
    auto *dispStack = new QStackedWidget(root);
    m_displayStack = dispStack;
    disp->addWidget(dispStack);
    // A QStackedWidget sizes itself to its LARGEST page, hidden ones included:
    // the 2D page (266 px) pushed the dock content to 286 px against a 272 px
    // dock, and the 3D page was clipped on the right. Only the current page
    // counts; the others are Ignored until they are shown.
    QObject::connect(dispStack, &QStackedWidget::currentChanged, dispStack, [dispStack](int cur) {
        for (int i = 0; i < dispStack->count(); ++i) {
            QWidget *pg = dispStack->widget(i);
            pg->setSizePolicy(i == cur ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                              i == cur ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        }
        dispStack->updateGeometry();
    });
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
        h->setSpacing(8);
        // Labelled, like the "Surface color" row it swaps with. Bare, it was a
        // second full-width segmented bar sitting directly under the
        // Isosurface/Volume one and looking exactly like it — so switching mode,
        // which replaces this row with the sparser surface-colour row, read as
        // the mode selector itself changing size. (It does not: measured at
        // 252 px in both modes.) A word in front makes them two controls
        // instead of twins.
        auto *l = new QLabel(u"Blend"_s, volRow);
        l->setStyleSheet(labelCss);
        h->addWidget(l, 0);
        rehost(v);
        v->setMinimumWidth(0);
        v->setParent(volRow);
        h->addWidget(v, 1);
        // Same fixed step as surfRow, so the block below the selector keeps its
        // height across the swap.
        volRow->setFixedHeight(28);
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
    {
        // Slack goes to the bottom, not into the controls.
        //
        // The stack is as tall as its TALLEST page, so the 3-D page usually has
        // spare height — and a QVBoxLayout with nowhere to put it grows whatever
        // children can grow. Every row here is height-pinned except the
        // Isosurface/Volume selector, so the selector absorbed the lot: measured
        // at 164 px in a 300 px page and 364 px in a 500 px one, against its
        // natural 28. That is the "sometimes it changes size" this started from
        // — the amount of slack varies with which page is taller and what is
        // visible, so the selector was a different height on different days.
        disp3d->addStretch(1);
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
    // Page 2: the active view has no colour map driven from here (the PV diagram
    // carries its own controls, a spectrum has none). Saying so beats a page of
    // colour controls that do nothing.
    {
        auto *page = new QWidget();
        auto *v = new QVBoxLayout(page);
        v->setContentsMargins(12, 8, 12, 8);
        m_displayNoControls = new QLabel(page);
        m_displayNoControls->setWordWrap(true);
        m_displayNoControls->setStyleSheet(
                QStringLiteral("QLabel { color: %1; font-size: 11px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        v->addWidget(m_displayNoControls);
        v->addStretch(1);
        dispStack->addWidget(page);   // index 2
    }
    // Pages added after the first never emitted currentChanged: size them now.
    emit dispStack->currentChanged(dispStack->currentIndex());
    // The DISPLAY section header + which page follow the active pane; keep the header
    // text correct through collapse/expand too (its built-in setTxt would revert it).
    QObject::connect(this, &vtkWindowCube::activePaneChanged, root,
                     [this](int) { updateDisplaySectionForActivePane(); });
    if (m_displayHeaderBtn)
        QObject::connect(m_displayHeaderBtn, &QPushButton::toggled, root,
                         [this](bool) { updateDisplaySectionForActivePane(); });
    updateDisplaySectionForActivePane();

    // ── LAYERS — visibility rows, filtered to the active pane ─────────────────
    QVBoxLayout *lay = addSection(u"LAYERS"_s, true, /*divider=*/false);
    // Returns the row so the caller can keep it for pane-dependent visibility.
    auto layerCheck = [&](const QString &label, QAction *act, const QString &disabledReason,
                          QWidget *trailing) -> QWidget * {
        auto *row = new QWidget();
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *cb = new QCheckBox(label, row);
        cb->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fg));
        if (act) {
            cb->setChecked(act->isChecked());
            cb->setEnabled(act->isEnabled());
            // The reason applies to an action-backed box too. An action can be
            // disabled for a good reason — no catalogue loaded yet, say — and a
            // grey box with no tooltip is indistinguishable from a broken one.
            const auto syncReason = [cb, act, disabledReason]() {
                // Only while it IS disabled: left in place, "Load a catalogue
                // first" would still be sitting there after one was loaded.
                cb->setToolTip(act->isEnabled() ? QString() : disabledReason);
            };
            if (!disabledReason.isEmpty())
                syncReason();
            QObject::connect(cb, &QCheckBox::clicked, act, [act]() { act->trigger(); });
            QObject::connect(act, &QAction::toggled, cb,
                             [cb](bool on) { QSignalBlocker b(cb); cb->setChecked(on); });
            QObject::connect(act, &QAction::changed, cb,
                             [cb, act, disabledReason, syncReason]() {
                                 cb->setEnabled(act->isEnabled());
                                 if (!disabledReason.isEmpty())
                                     syncReason();
                             });
        } else {
            cb->setEnabled(false);
            cb->setToolTip(disabledReason);
        }
        h->addWidget(cb, 1);
        if (trailing) { rehost(trailing); trailing->setParent(row); trailing->setMaximumWidth(96); h->addWidget(trailing, 0); }
        lay->addWidget(row);
        return row;
    };
    // A visibility layer, not a second render-mode control.
    //
    // It used to mirror the Isosurface/Volume segmented control, so clearing it
    // just switched to Isosurface — which the selector two rows above already
    // does, and which left the user with no way to say "show me the cube box and
    // the slice, without the rendering on top". That is what a LAYERS entry is
    // for, so this now hides whichever of the two is mounted and leaves the mode
    // alone. Hence the label: "Volume" named one of the two things it governs.
    {
        auto *row = new QWidget();
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *cb = new QCheckBox(u"3D rendering"_s, row);
        cb->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fg));
        cb->setToolTip(u"Show the volume or isosurface. Clear it to leave just the "
                       u"cube outline, the cutting plane and any overlays - the "
                       u"Isosurface / Volume selector above chooses which of the "
                       u"two is drawn when this is on."_s);
        cb->setChecked(m_show3dRendering);
        QObject::connect(cb, &QCheckBox::clicked, this, [this](bool on) {
            m_show3dRendering = on;
            apply3dRenderingVisibility();
        });
        h->addWidget(cb, 1);
        lay->addWidget(row);
        m_layerRowVolume = row;
    }
    m_layerRowCuttingPlane = layerCheck(u"Cutting plane"_s, actionShowCuttingPlane, QString(),
                                        nullptr);

    // The opacity slider gets its own labelled row, indented under the checkbox
    // it belongs to. Trailing the checkbox it was a bare slider with nothing to
    // say what it did — of the several things one might expect a slider next to
    // "Cutting plane" to move, position is the obvious guess and opacity is the
    // answer. It also follows the checkbox: a control for a layer that is off is
    // not worth the row.
    if (m_cuttingPlaneOpacitySlider) {
        auto *row = new QWidget();
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(20, 0, 0, 0);   // indent: it belongs to the row above
        h->setSpacing(8);
        auto *l = new QLabel(u"Opacity"_s, row);
        l->setStyleSheet(labelCss);
        h->addWidget(l, 0);
        rehost(m_cuttingPlaneOpacitySlider);
        m_cuttingPlaneOpacitySlider->setParent(row);
        m_cuttingPlaneOpacitySlider->setToolTip(u"Opacity of the cutting plane."_s);
        h->addWidget(m_cuttingPlaneOpacitySlider, 1);
        lay->addWidget(row);
        m_layerRowCuttingPlaneOpacity = row;

        // One predicate, shared with the pane filter. An earlier version asked
        // m_layerRowCuttingPlane->isVisible(), which is false while LAYERS is
        // merely collapsed — so toggling the plane from the View menu with the
        // section shut hid this row, and expanding the section did not bring it
        // back, because expanding shows the parent without re-running the filter.
        if (actionShowCuttingPlane)
            QObject::connect(actionShowCuttingPlane, &QAction::toggled, row,
                             [this](bool) { updateCuttingPlaneOpacityRowVisibility(); });
        updateCuttingPlaneOpacityRowVisibility();
    }

    // Contours mirror ui->checkContours, the canonical state the CONTOURS card's
    // "Show Contours" button already drives. This row used to be built with a
    // null action, which left it disabled for the life of the window under a
    // tooltip claiming the contours had not been computed — they are not
    // computed, they are switched on, and the switch was three sections away.
    {
        auto *row = new QWidget();
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *cb = new QCheckBox(u"Contours"_s, row);
        cb->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fg));
        if (ui->checkContours) {
            cb->setChecked(ui->checkContours->isChecked());
            QObject::connect(cb, &QCheckBox::clicked, ui->checkContours,
                             [this](bool on) { ui->checkContours->setChecked(on); });
            QObject::connect(ui->checkContours, &QCheckBox::toggled, cb,
                             [cb](bool on) { QSignalBlocker b(cb); cb->setChecked(on); });
        } else {
            cb->setEnabled(false);
        }
        h->addWidget(cb, 1);
        lay->addWidget(row);
        m_layerRowContours = row;
    }

    layerCheck(u"Catalogue sources"_s, actionShowCatalogueOverlay,
               u"Load a catalogue first: Tools \u25b8 Load Catalogue Overlay."_s, nullptr);
    m_layerRow3dWcsAxes = layerCheck(u"3D WCS axes"_s, actionShow3dWcsAxes, QString(), nullptr);

    // Again, now that the rows exist. The call above runs while building the
    // DISPLAY section, before any of these are constructed, so it set nothing —
    // and every row would have stayed visible until the first pane change.
    updateDisplaySectionForActivePane();

    // NO tools section here. The left dock answers "how is this view drawn" —
    // colour map, scale, clip, layers, slice animation — and the tool list
    // answers "what can I compute", which is Inspector ▸ Analysis's question. A
    // second copy of the same thirty actions in this panel had no authority
    // (which one is canonical?) and sat away from the state that belongs to it:
    // the gating tooltips and the SPECTRUM controls of the product a tool just
    // produced both live in the Inspector. Reach is covered by the Tools menu
    // and ⌘K, and the Inspector's rail expands in one click (user request).

    // ── INTERACTION ▶ (collapsed) ─────────────────────────────────────────────
    QVBoxLayout *inter = addSection(u"INTERACTION"_s, false, /*divider=*/false);
    {
        auto *cb = new QCheckBox(u"Pick spectrum on cutting plane"_s);
        cb->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(fg));
        if (actionPickSpectrum3d) {
            cb->setChecked(actionPickSpectrum3d->isChecked());
            QObject::connect(cb, &QCheckBox::clicked, actionPickSpectrum3d,
                             [this]() { actionPickSpectrum3d->trigger(); });
            QObject::connect(actionPickSpectrum3d, &QAction::toggled, cb,
                             [cb](bool on) { QSignalBlocker b(cb); cb->setChecked(on); });

            // No longer gated on the cutting plane. It once picked only that
            // plane, so the plane was a prerequisite; now it reads the volume and
            // the isosurface too, and requiring the plane would block the case
            // the tool exists for — a feature you can see in the rendering and
            // in no single channel.
            cb->setToolTip(
                    u"Click the 3D view to plot the spectrum at what you clicked. "
                    u"Which voxel that is follows the blend mode: MIP gives the "
                    u"brightest sample along the ray, Composite the front surface, "
                    u"the isosurface and the cutting plane the point you hit. The "
                    u"status bar names the channel and its velocity."_s);
        } else {
            cb->setEnabled(false);
        }
        inter->addWidget(cb);
    }

    // ── SLICE ANIMATION ▶ (collapsed) ─────────────────────────────────────────
    QVBoxLayout *anim = addSection(u"SLICE ANIMATION"_s, false, /*divider=*/false);
    {
        // The real controls, re-hosted. This section used to hold one line of
        // text sending the reader to the channel scrubber — which steps channels
        // by hand and is not what animation means. Play, Speed and Mode existed
        // all along, built and wired, on a page that is added to a hidden holder
        // and never shown; only the View menu could reach them.
        // Compact padding to go with the 28 px row step. The global theme gives
        // these controls a 13 px font and 8 px of vertical padding, which needs
        // more than 28 px — pinning the height alone would clip the ▶/⏸ glyph.
        const QString compactCss =
                QStringLiteral("font-family:'Lato'; font-size:11px; padding:2px 8px;");
        if (m_animPlayBtn) {
            rehost(m_animPlayBtn);
            m_animPlayBtn->setParent(nullptr);
            m_animPlayBtn->setStyleSheet(compactCss);
            m_animPlayBtn->setFixedHeight(28);
            anim->addWidget(m_animPlayBtn);
        }
        const auto labelled = [&](const QString &text, QWidget *w) {
            if (!w)
                return;
            auto *row = new QWidget();
            auto *h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(8);
            auto *l = new QLabel(text, row);
            l->setStyleSheet(labelCss);
            h->addWidget(l, 0);
            rehost(w);
            w->setParent(row);
            w->setMinimumWidth(0);
            w->setStyleSheet(compactCss);
            h->addWidget(w, 1);
            row->setFixedHeight(28);   // the same row step as the rest of the dock
            anim->addWidget(row);
        };
        labelled(u"Speed"_s, m_animFpsCombo);
        labelled(u"Mode"_s, m_animModeCombo);
        if (!m_animPlayBtn && !m_animFpsCombo && !m_animModeCombo) {
            auto *ph = new QLabel(u"Animation controls are unavailable for this cube."_s);
            ph->setWordWrap(true);
            ph->setStyleSheet(labelCss);
            anim->addWidget(ph);
        }
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
// §5.4b: the six curated 2-D colormaps offered directly in the LUT strip popover and the
// dock's Color-map combo (registry casing). Everything else — the volume-rendering transfer
// functions and single-colour ramps — lives behind "More maps…". MinMax is NOT a colormap
// (it's a range mode) and is excluded from every colormap list.
static const QStringList &curatedColorMaps()
{
    static const QStringList k{ u"Inferno"_s,  u"Magma"_s,    u"Plasma"_s,
                               u"Viridis"_s, u"CoolWarm"_s, u"Gray"_s };
    return k;
}

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
    // #5: only an IMAGE product (m_products2d) has a Pane2DView. A spectrum product
    // (m_products1d) renders a ProfileWidget — return null so the DISPLAY dock / LUT strip
    // never operate on a stale/empty 2-D view for it.
    if (!m_products2d.contains(m_slots[m_activePane].view.productId))
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
    // §5.4b: a non-curated pick becomes this pane's persistent custom entry (top of the
    // popover + combo), replacing any prior one. Curated picks never clear it; MinMax is a
    // range mode, never a colormap, so it never becomes a custom entry.
    if (m_activePane >= 0 && m_activePane < 4 && !name.isEmpty() && name != u"MinMax"_s
        && !curatedColorMaps().contains(name))
        m_paneCustomColorMap[m_activePane] = name;
    if (Pane2DView *pv = activePane2DView()) {
        pv->setColorMapName(name);   // emits lutChanged → strip refresh + Linked propagate
        return;
    }
    if (activePaneIsSlice()) {
        applySliceLutByName(name);   // rebuilds lutSlice; renders if the slice is on screen
        if (this->sliceWin)
            this->sliceWin->Render();
        refreshPaneLutStrip(m_activePane);   // §5.4: slice strip mirrors the new colormap
    }
}

// Anchor the slice LUT's log floor at the data's own faint end, rather than
// letting LutScale fall back to max x 1e-6 — six fixed decades on a map with two
// or three of real dynamic range leaves the whole bright end crammed into the
// top of the ramp.
//
// Returns false when log was asked for and the image has no positive values; the
// caller must then leave m_sliceScaleMode alone and tell the user. Only on the
// transition into log: re-anchoring while already logarithmic would discard a
// lower bound the user had set by hand.
bool vtkWindowCube::anchorSliceLogFloor(LutScale::Mode mode)
{
    if (mode != LutScale::Mode::Log || this->m_sliceScaleMode == mode || !this->lutSlice)
        return true;
    // The SLICE, not activeDisplayImageData(): this is called for a linked update
    // too, which has no active-pane restriction, and that accessor answers with
    // whatever pane has focus — a moment map, a product, or nothing at all when
    // the 3-D view is active. Anchoring lutSlice against another pane's pixels
    // would accept log for a slice with nothing positive in it, or refuse a good
    // slice because the focus happened to be elsewhere.
    auto *sliceImg = vtkImageData::SafeDownCast(
            this->remoteSliceDisplaySource->GetOutputDataObject(0));
    // No slice yet is not the same as a slice with nothing positive. A linked
    // window can be told to go logarithmic before its first channel has arrived,
    // and refusing there would drop the request for good — the peer would stay
    // in log and this window in linear until some later, unrelated LUT edit.
    // Accept it; applyRemoteSliceResult() samples the floor when the pixels land.
    if (!sliceImg || sliceImg->GetNumberOfPoints() <= 1)
        return true;
    const double floorValue = LutScale::sampledPositiveFloor(sliceImg);
    if (!(floorValue > 0.0))
        return false;
    const double *cur = this->lutSlice->GetTableRange();
    this->lutSlice->SetTableRange(floorValue, std::max(cur[1], floorValue * (1.0 + 1e-6)));
    return true;
}

void vtkWindowCube::setActiveDisplayScale(LutScale::Mode mode)
{
    if (Pane2DView *pv = activePane2DView()) {
        pv->setScale(mode);   // emits lutChanged → strip refresh + Linked propagate
        return;
    }
    if (activePaneIsSlice()) {
        if (!anchorSliceLogFloor(mode)) {
            this->statusBar()->showMessage(
                    u"Log scale needs positive values, and this image has none."_s, 4000);
            updateDisplaySectionForActivePane();   // control back on the real scale
            return;
        }
        this->m_sliceScaleMode = mode;
        applySliceLutByName(this->m_activeSliceLutName);   // re-apply the stretch at range
        if (this->sliceWin)
            this->sliceWin->Render();
    }
}

void vtkWindowCube::setActiveDisplayRange(double lo, double hi)
{
    if (Pane2DView *pv = activePane2DView()) {
        pv->setRange(lo, hi);   // emits rangeChanged → dock sync + Linked range propagate
        return;
    }
    if (activePaneIsSlice() && this->lutSlice) {
        if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo)
            return;
        LutScale::setRange(this->lutSlice, this->m_sliceScaleMode, lo, hi);
        this->lutSlice->Build();
        this->setSliceRangeLocked(true);   // a chosen range must survive the next channel
        this->layout2dPaneFor(/*slice=*/true, /*force=*/false);
        if (this->sliceWin)
            this->sliceWin->Render();
        // The clip fields must show the range now in force (Cube range, presets):
        // stale values would be re-applied the next time a field loses focus.
        if (!m_applyingPaneDisplay)
            updateDisplaySectionForActivePane();
    }
}

void vtkWindowCube::setSliceRangeLocked(bool locked)
{
    this->m_sliceRangeLocked = locked;
    if (m_sliceRangeLock && m_sliceRangeLock->isChecked() != locked) {
        const QSignalBlocker block(m_sliceRangeLock);
        m_sliceRangeLock->setChecked(locked);
    }
}

QList<QPair<QString, QString>> vtkWindowCube::sliceBackdropCandidates() const
{
    QList<QPair<QString, QString>> out;
    int dims[3] = { 0, 0, 0 };
    if (auto *slice = vtkImageData::SafeDownCast(
                this->remoteSliceDisplaySource->GetOutputDataObject(0)))
        slice->GetDimensions(dims);
    const QString mirror = channelMirrorId();
    for (auto it = m_products2d.constBegin(); it != m_products2d.constEnd(); ++it) {
        const Product2D &p = it.value();
        if (!p.imageData || !p.regionOrigin.isNull() || it.key() == mirror)
            continue;   // a cropped map is not on the slice's pixel grid; channels are channels
        int pd[3] = { 0, 0, 0 };
        p.imageData->GetDimensions(pd);
        if (dims[0] > 1 && (pd[0] != dims[0] || pd[1] != dims[1]))
            continue;
        out.append({ it.key(), p.label.isEmpty() ? it.key() : p.label });
    }
    std::sort(out.begin(), out.end(),
              [](const auto &a, const auto &b) { return a.second < b.second; });
    return out;
}

void vtkWindowCube::refreshSliceBackdropCombo()
{
    if (!m_sliceBackdropProductId.isEmpty() && !m_products2d.contains(m_sliceBackdropProductId))
        setSliceBackdrop(QString());   // its product is gone
}

int vtkWindowCube::channelMirrorSlot() const
{
    const QString mirror = channelMirrorId();
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product && m_slots[i].view.productId == mirror)
            return i;
    return -1;
}

void vtkWindowCube::updateChannelMirror(vtkImageData *img, double lo, double hi)
{
    auto it = m_products2d.find(channelMirrorId());
    if (it == m_products2d.end() || !img)
        return;
    it->imageData = img;
    it->statsComputed = false;
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product && m_slots[i].view.productId == it.key()
            && m_paneViews[i]) {
            Pane2DView *pv = m_paneViews[i];
            // A range the user set on the Channels pane (clip fields, presets,
            // window/level) differs from the one we installed: keep it, as the
            // slice keeps a chosen range across channels.
            const double tol = 1e-9 * std::max({ 1.0, std::abs(pv->rangeMin()), std::abs(pv->rangeMax()) });
            if (!m_channelMirrorUserRange && pv->lutInitialized()
                && (std::abs(pv->rangeMin() - m_channelMirrorInstalledRange[0]) > tol
                    || std::abs(pv->rangeMax() - m_channelMirrorInstalledRange[1]) > tol))
                m_channelMirrorUserRange = true;   // sticky until the mirror goes
            const double useLo = m_channelMirrorUserRange ? pv->rangeMin() : lo;
            const double useHi = m_channelMirrorUserRange ? pv->rangeMax() : hi;
            pv->setImageData(img, useLo, useHi);
            pv->render();
            it->imageRange = { useLo, useHi };
            // What the pane ACTUALLY holds (a log stretch floors a non-positive
            // minimum): comparing against the request would read that as a user edit.
            if (!m_channelMirrorUserRange)
                m_channelMirrorInstalledRange = { pv->rangeMin(), pv->rangeMax() };
        }
}

void vtkWindowCube::refreshPaneOverlayPaths(const std::vector<std::array<double, 2>> &path)
{
    // Only maps on the slice's own pixel grid: the path is in those pixels.
    QSet<QString> sameGrid;
    for (const auto &c : sliceBackdropCandidates())
        sameGrid.insert(c.first);
    sameGrid.insert(channelMirrorId());
    for (int i = 0; i < 4; ++i) {
        if (!m_paneViews[i])
            continue;
        const PaneView &v = m_slots[i].view;
        const bool onGrid = v.kind == ViewId::Product && sameGrid.contains(v.productId);
        m_paneViews[i]->setOverlayPath(onGrid ? path : std::vector<std::array<double, 2>>{});
    }
}

QList<ChannelOverlay *> vtkWindowCube::channelOverlays() const
{
    QList<ChannelOverlay *> out;
    if (this->channelOverlay)
        out << this->channelOverlay.data();
    if (this->channelOverlay2d)
        out << this->channelOverlay2d.data();
    return out;
}

void vtkWindowCube::attachChannelBar2d(QWidget *host)
{
    if (!this->channelOverlay2d || !host)
        return;
    // The view the bar sits on keeps room for it below its axes (and the one it
    // leaves gives that room back), so the bar never covers the x labels.
    const auto paneFor = [this](QWidget *w) -> Pane2DView * {
        for (Pane2DView *pv : m_paneViews)
            if (pv && pv->widget() == w)
                return pv;
        return nullptr;
    };
    QWidget *cur = this->channelOverlay2d->parentWidget();
    if (cur != host) {
        if (cur) {
            cur->removeEventFilter(this->channelOverlay2d);
            if (Pane2DView *pv = paneFor(cur))
                pv->setBottomReserve(0);
        }
        this->channelOverlay2d->setParent(host);
        host->installEventFilter(this->channelOverlay2d);
    }
    if (Pane2DView *pv = paneFor(host))
        pv->setBottomReserve(ChannelOverlay::reservedHeight());
}

int vtkWindowCube::momentPaneForSliceTools() const
{
    const auto candidates = sliceBackdropCandidates();
    const auto usable = [&](int i) {
        if (i < 0 || i > 3 || !m_slots[i].frame || !m_slots[i].frame->isVisible()
            || m_slots[i].view.kind != ViewId::Product)
            return false;
        for (const auto &c : candidates)
            if (c.first == m_slots[i].view.productId)
                return true;
        return false;
    };
    if (usable(m_activePane))
        return m_activePane;
    for (int i = 0; i < 4; ++i)
        if (usable(i))
            return i;
    return -1;
}

bool vtkWindowCube::ensureSliceForTools()
{
    // The pane the user is working in decides: a tool armed while a moment pane is
    // ACTIVE draws on that moment, even with the 2D Slice on screen elsewhere.
    int candidate = momentPaneForSliceTools();
    bool activeIsMoment = candidate >= 0 && candidate == m_activePane;
    // The channel to seed a new Channels pane with: when retargeting, the current
    // mirror's — restoring the slice may still be fetching its channel, and the
    // slice's pixels would then be the old map's.
    vtkSmartPointer<vtkImageData> channelSeed;
    std::array<double, 2> channelSeedRange{ 0.0, 0.0 };
    if (activeIsMoment && !m_sliceBackdropProductId.isEmpty()) {
        if (auto mit = m_products2d.constFind(channelMirrorId()); mit != m_products2d.constEnd()) {
            channelSeed = mit->imageData;
            channelSeedRange = mit->imageRange;
        }
        // Already drawing on another map: put that one back first (its panes as
        // they were), then take this one from a clean layout.
        const int keepActive = m_activePane;
        setSliceBackdrop(QString());
        if (keepActive != m_activePane)
            setActivePane(keepActive);
        candidate = momentPaneForSliceTools();
        activeIsMoment = candidate >= 0 && candidate == m_activePane;
    }
    if (slice2dVisible() && !activeIsMoment)
        return true;
    const int slot = candidate;
    if (slot < 0)
        return false;
    const QString productId = m_slots[slot].view.productId;
    // The map's look AS SHOWN in its pane (the user may have changed it there).
    m_pendingBackdropLook = {};
    if (Pane2DView *pv = m_paneViews[slot]; pv && pv->lutInitialized()) {
        m_pendingBackdropLook = { true, pv->colorMapName(), pv->scale(), pv->gamma(),
                                  pv->invert(), pv->clampOutliers(), pv->rangeMin(),
                                  pv->rangeMax() };
    }
    // The slice is one view: if another visible pane shows it, the two panes swap
    // (as the pane menu does), so that pane gets the map and none goes blank.
    int sliceSlot = -1;
    for (int i = 0; i < 4; ++i)
        if (i != slot && m_slots[i].view.kind == ViewId::Slice2D && m_slots[i].frame
            && m_slots[i].frame->isVisible()) {
            sliceSlot = i;
            break;
        }
    if (sliceSlot >= 0) {
        // That pane keeps the CHANNELS (display-only, following the channel bar):
        // seeded with the channel on screen now, with the slice's own colours.
        Product2D channels;
        const double *r = this->lutSlice ? this->lutSlice->GetTableRange() : nullptr;
        if (channelSeed) {
            channels.imageData = channelSeed;
            channels.imageRange = channelSeedRange;
        } else {
            channels.imageData = vtkImageData::SafeDownCast(
                    this->remoteSliceDisplaySource->GetOutputDataObject(0));
            channels.imageRange = r ? std::array<double, 2>{ r[0], r[1] }
                                    : std::array<double, 2>{ 0.0, 1.0 };
        }
        channels.lutName = this->m_activeSliceLutName;
        channels.scaleMode = this->m_sliceScaleMode;
        channels.gamma = this->m_sliceScaleGamma;
        channels.label = u"Channels"_s;
        channels.unit = this->m_brightnessUnit;
        m_products2d.insert(channelMirrorId(), channels);
        m_channelMirrorInstalledRange = channels.imageRange;
        m_channelMirrorUserRange = false;
    }
    m_slots[slot].view = { ViewId::Slice2D, {} };
    if (sliceSlot >= 0)
        m_slots[sliceSlot].view = { ViewId::Product, channelMirrorId() };
    clearViewFromOtherSlots(slot);
    mountView(slot);
    if (sliceSlot >= 0) {
        mountView(sliceSlot);
        // The baseline is what the pane installed (a log stretch floors the minimum).
        if (Pane2DView *mpv = m_paneViews[sliceSlot])
            m_channelMirrorInstalledRange = { mpv->rangeMin(), mpv->rangeMax() };
    }
    syncPick3d2d();
    refreshPaneChrome();
    layout2dPane(true);
    setActivePane(slot);
    m_sliceBackdropSlot = slot;
    setSliceBackdrop(productId);
    m_pendingBackdropLook = {};
    // The channel bar goes with the channels.
    if (sliceSlot >= 0 && m_paneViews[sliceSlot] && m_paneViews[sliceSlot]->widget()
        && this->channelOverlay2d) {
        QWidget *host = m_paneViews[sliceSlot]->widget();
        attachChannelBar2d(host);
        this->channelOverlay2d->setSpectralLabel(ui->lineSpectral->text());
        this->channelOverlay2d->show();
        QResizeEvent ev(host->size(), host->size());
        QCoreApplication::sendEvent(host, &ev);   // place it at the host's bottom edge
    }
    this->refreshPvOverlay();   // the path on the channels too
    this->statusBar()->showMessage(
            u"The tool draws on %1. Choose 2D Slice in the pane's menu to see the channels."_s
                    .arg(m_products2d.value(productId).label),
            7000);
    return true;
}

void vtkWindowCube::setSliceBackdrop(const QString &productId)
{
    if (productId == m_sliceBackdropProductId)
        return;
    if (productId.isEmpty()) {
        const QString wornId = m_sliceBackdropProductId;
        const int wornSlot = m_sliceBackdropSlot;
        m_sliceBackdropProductId.clear();
        // The channel bar back on the slice.
        attachChannelBar2d(ui->vtkImage);
        // The layout as it was: the "Channels" pane becomes the slice again and the
        // map pane the map (unless something else was chosen there).
        const int mirrorSlot = channelMirrorSlot();
        if (mirrorSlot >= 0) {
            bool sliceElsewhere = false;
            for (int i = 0; i < 4; ++i)
                if (i != mirrorSlot && i != wornSlot && m_slots[i].view.kind == ViewId::Slice2D)
                    sliceElsewhere = true;
            if (wornSlot >= 0 && wornSlot < 4 && m_slots[wornSlot].view.kind == ViewId::Slice2D) {
                m_slots[wornSlot].view = { ViewId::Product, wornId };
            }
            m_slots[mirrorSlot].view = sliceElsewhere ? PaneView{ ViewId::Product, wornId }
                                                      : PaneView{ ViewId::Slice2D, {} };
            mountView(mirrorSlot);   // takes the slice widget first…
            if (wornSlot >= 0 && wornSlot < 4)
                mountView(wornSlot);   // …then the map's pane gets its own view
            m_products2d.remove(channelMirrorId());
        } else {
            m_products2d.remove(channelMirrorId());
        }
        // The channel's range comes back with it (and the lock as it was).
        // …and the slice's own colour map, stretch and range (taken off on entry).
        this->m_sliceScaleMode = this->m_sliceScaleBeforeBackdrop;
        this->m_sliceScaleGamma = this->m_sliceGammaBeforeBackdrop;
        this->m_sliceInvert = this->m_sliceInvertBeforeBackdrop;
        this->m_sliceBackdropSlot = -1;
        if (this->lutSlice) {
            this->lutSlice->SetUseBelowRangeColor(0);
            this->lutSlice->SetUseAboveRangeColor(0);
            if (this->m_sliceRangeBeforeBackdrop[1] > this->m_sliceRangeBeforeBackdrop[0])
                LutScale::setRange(this->lutSlice, this->m_sliceScaleMode,
                                   this->m_sliceRangeBeforeBackdrop[0],
                                   this->m_sliceRangeBeforeBackdrop[1]);
        }
        if (!this->m_sliceLutNameBeforeBackdrop.isEmpty())
            applySliceLutByName(this->m_sliceLutNameBeforeBackdrop);
        this->setSliceRangeLocked(this->m_sliceRangeLockedBeforeBackdrop);
        if (this->channelOverlay)
            for (ChannelOverlay *co : this->channelOverlays())
                co->setSpectralLabel(ui->lineSpectral->text());
        if (this->channelOverlay2d) {
            this->channelOverlay2d->show();   // channels on the slice again
            QResizeEvent ev(ui->vtkImage->size(), ui->vtkImage->size());
            QCoreApplication::sendEvent(ui->vtkImage, &ev);
        }
        const int channel = ui->spinSlice->value() - 1;
        this->m_displayedSliceIndex = -1;   // force the channel back onto the pane
        this->requestRemoteSlice(this->clampRemoteSliceIndex(channel));
    } else {
        auto it = m_products2d.constFind(productId);
        if (it == m_products2d.constEnd() || !it->imageData)
            return;
        if (m_sliceBackdropProductId.isEmpty() && this->lutSlice) {
            const double *r = this->lutSlice->GetTableRange();
            this->m_sliceRangeBeforeBackdrop = { r[0], r[1] };
            this->m_sliceRangeLockedBeforeBackdrop = this->m_sliceRangeLocked;
            this->m_sliceLutNameBeforeBackdrop = this->m_activeSliceLutName;
            this->m_sliceScaleBeforeBackdrop = this->m_sliceScaleMode;
            this->m_sliceGammaBeforeBackdrop = this->m_sliceScaleGamma;
            this->m_sliceInvertBeforeBackdrop = this->m_sliceInvert;
        }
        m_sliceBackdropProductId = productId;
        this->m_slice2dLoadingActor->VisibilityOff();
        this->remoteSliceDisplaySource->SetOutput(it->imageData);
        this->remoteSliceDisplaySource->Modified();
        this->sliceColors->SetInputData(it->imageData);
        // The map's OWN appearance — colour map, stretch, range, hidden out-of-range
        // pixels for a velocity field — so it looks as in its own pane.
        const BackdropLook &look = m_pendingBackdropLook;
        this->m_sliceScaleMode = look.valid ? look.scale : it->scaleMode;
        this->m_sliceScaleGamma = look.valid ? look.gamma : it->gamma;
        this->m_sliceInvert = look.valid && look.invert;
        double lo = look.valid ? look.lo : it->imageRange[0];
        double hi = look.valid ? look.hi : it->imageRange[1];
        const QString lutName = look.valid && !look.lutName.isEmpty() ? look.lutName : it->lutName;
        const bool clamp = look.valid ? look.clamp : it->clampOutliers;
        if (this->m_sliceScaleMode == LutScale::Mode::Log && !(lo > 0.0)) {
            const double floorValue = LutScale::sampledPositiveFloor(it->imageData);
            lo = floorValue > 0.0 ? floorValue : hi * 1e-6;
        }
        if (this->lutSlice && std::isfinite(lo) && std::isfinite(hi) && hi > lo)
            LutScale::setRange(this->lutSlice, this->m_sliceScaleMode, lo, hi);
        if (!lutName.isEmpty())
            applySliceLutByName(lutName);
        if (this->lutSlice) {
            this->lutSlice->SetUseBelowRangeColor(clamp ? 1 : 0);
            this->lutSlice->SetUseAboveRangeColor(clamp ? 1 : 0);
            this->lutSlice->SetBelowRangeColor(0., 0., 0., 0.);
            this->lutSlice->SetAboveRangeColor(0., 0., 0., 0.);
        }
        // The map's own range; it does not leak into the channels (restored above).
        this->m_sliceRangeLocked = true;
        // The pane shows the map, not a channel: its channel bar would only confuse.
        // (The 3-D view keeps its own.)
        if (this->channelOverlay2d)
            this->channelOverlay2d->hide();
    }
    // With or without the channel bar the slice keeps a different bottom margin.
    this->layout2dPaneFor(/*slice=*/true, /*force=*/false);
    if (this->lutSlice)
        this->lutSlice->Build();
    this->refreshPvOverlay();
    if (this->regionValid)
        this->refreshRegionOverlay();
    this->layout2dPaneFor(/*slice=*/true, /*force=*/false);
    if (this->sliceWin)
        this->sliceWin->Render();
    refreshSliceBackdropCombo();
    // Pane title and menu show the map (or the slice again); the dock's range
    // fields follow the new image — unless this runs from inside the dock's own
    // repopulation (refreshSliceBackdropCombo dropping a vanished map).
    if (!m_applyingPaneDisplay) {
        refreshPaneChrome();
        updateDisplaySectionForActivePane();
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

bool vtkWindowCube::activeDisplayInvert() const
{
    if (Pane2DView *pv = activePane2DView())
        return pv->invert();
    if (activePaneIsSlice())
        return this->m_sliceInvert;
    return false;
}

void vtkWindowCube::setActiveDisplayInvert(bool on)
{
    if (Pane2DView *pv = activePane2DView()) {
        pv->setInvert(on);   // emits lutChanged → strip refresh + Linked propagate
        return;
    }
    if (activePaneIsSlice()) {
        this->m_sliceInvert = on;
        applySliceLutByName(this->m_activeSliceLutName);   // rebuild + flip
        if (this->sliceWin)
            this->sliceWin->Render();
    }
}

void vtkWindowCube::resetActiveDisplayLut()
{
    // §5.4 Reset → the default appearance BY KIND. A Product pane re-seeds from the shared
    // product cache (the original colormap/scale/gamma/range the moment was registered
    // with); the Slice drops any stretch/invert and refits its full data range.
    if (Pane2DView *pv = activePane2DView()) {
        const QString pid = m_slots[m_activePane].view.productId;
        auto it = m_products2d.constFind(pid);
        if (it == m_products2d.constEnd())
            return;
        const Product2D &p = *it;
        {
            // Range FIRST, then setColorMap — setColorMap rebuilds via LutScale::build at the
            // pane's current TableRange, so seeding the range first keeps the Log positive-
            // floor handling (a lo≤0 default would otherwise be mishandled). Guarded so the
            // intermediate rangeChanged doesn't double-propagate; one explicit full propagate
            // after (setColorMap is the SILENT seed setter, emits nothing).
            QScopedValueRollback<bool> guard(m_applyingPaneDisplay, true);
            pv->setRange(p.imageRange[0], p.imageRange[1]);
            pv->setColorMap(p.lutName, p.scaleMode, p.gamma, p.clampOutliers);
        }
        propagatePaneLutWithinWindow(m_activePane);
        updateDisplaySectionForActivePane();
        refreshPaneLutStrip(m_activePane);
        return;
    }
    if (activePaneIsSlice() && this->lutSlice) {
        this->m_sliceInvert = false;
        this->m_sliceScaleMode = LutScale::Mode::Linear;
        this->setSliceRangeLocked(false);   // back to each channel's own range
        vtkImageData *img = activeDisplayImageData();
        if (img) {
            double range[2] = { 0., 0. };
            img->GetScalarRange(range);
            if (std::isfinite(range[0]) && std::isfinite(range[1]) && range[1] > range[0])
                LutScale::setRange(this->lutSlice, this->m_sliceScaleMode, range[0], range[1]);
        }
        applySliceLutByName(this->m_activeSliceLutName);
        this->layout2dPaneFor(/*slice=*/true, /*force=*/false);
        if (this->sliceWin)
            this->sliceWin->Render();
        updateDisplaySectionForActivePane();
        refreshPaneLutStrip(m_activePane);
    }
}

void vtkWindowCube::refreshPaneLutStrip(int slot)
{
    // §5.4: the header strip mirrors THIS pane's colormap. Visible only for a 2-D LUT pane
    // (a Product with cached pixels, or the 2D Slice); hidden for 3D / Spectrum / None.
    if (slot < 0 || slot > 3 || !m_slots[slot].lutBtn)
        return;
    QToolButton *btn = m_slots[slot].lutBtn;
    const PaneView &v = m_slots[slot].view;
    QString cmap;
    if (v.kind == ViewId::Product && m_paneViews[slot] && m_products2d.contains(v.productId))
        cmap = m_paneViews[slot]->colorMapName();   // image product only (a spectrum has no LUT)
    else if (v.kind == ViewId::Slice2D)
        cmap = m_activeSliceLutName;
    const bool show = !cmap.isEmpty();
    btn->setVisible(show);
    if (show)
        btn->setIcon(QIcon(CubeUiAssembler::buildLutPreview(cmap, 30, 9)));
}

void vtkWindowCube::showPaneLutStripPopover(int slot)
{
    using namespace Qt::StringLiterals;
    if (slot < 0 || slot > 3)
        return;
    // Clicking the strip activates the pane so the activeDisplay* dispatch targets it, then
    // offers the one-click colormap list + Edit/Invert/Reset — the same per-pane state the
    // left dock writes (§5.4).
    setActivePane(slot);
    const PaneView &v = m_slots[slot].view;
    const bool is2dLut = (v.kind == ViewId::Product && m_products2d.contains(v.productId))
            || v.kind == ViewId::Slice2D;   // spectrum products have no colormap popover (#5)
    if (!is2dLut)
        return;

    auto *menu = new QMenu(m_slots[slot].lutBtn ? m_slots[slot].lutBtn.data() : nullptr);
    menu->setToolTipsVisible(true);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QString current = activeDisplayColorMapName();
    // Sync the pane's custom entry to the current map right here — covers the same-active-pane
    // open where a non-curated current arrived via a path that didn't go through
    // setActiveDisplayColorMap (e.g. a legacy combo), so the current row is present + marked
    //. Never MinMax (a range mode).
    if (!current.isEmpty() && current != u"MinMax"_s && !curatedColorMaps().contains(current))
        m_paneCustomColorMap[slot] = current;
    const QLatin1StringView fg(VisivoTheme::kOnSurface());
    const QLatin1StringView fg2(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView line(VisivoTheme::kOutline());

    // §5.4b: custom rows (not plain checkable QActions) so the CURRENT map is visibly marked
    // — check on the right edge, semibold kOnSurface label, an rgba(4,138,191,0.14) fill with
    // a 1 px kOutline border — the app's selected-list-row treatment. Exactly one is marked.
    auto addMapRow = [&](const QString &qn) {
        const bool cur = (qn == current);
        auto *row = new QWidget(menu);
        row->setCursor(Qt::PointingHandCursor);
        row->setStyleSheet(cur ? QStringLiteral("background:rgba(4,138,191,0.14);"
                                                "border:1px solid %1;border-radius:3px;")
                                         .arg(line)
                               : u"background:transparent;border:1px solid transparent;"_s);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(8, 3, 8, 3);
        h->setSpacing(8);
        auto *sw = new QLabel(row);
        sw->setPixmap(CubeUiAssembler::buildLutPreview(qn, 40, 12));
        sw->setAttribute(Qt::WA_TransparentForMouseEvents);
        h->addWidget(sw, 0);
        auto *name = new QLabel(qn, row);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        name->setStyleSheet(QStringLiteral("color:%1;font-family:'Lato';font-size:11px;%2")
                                    .arg(cur ? fg : fg2, cur ? u"font-weight:600;"_s : QString()));
        h->addWidget(name, 1);
        auto *check = new QLabel(cur ? u"✓"_s : QString(), row);
        check->setAttribute(Qt::WA_TransparentForMouseEvents);
        check->setStyleSheet(QStringLiteral("color:%1;font-size:11px;font-weight:700;")
                                     .arg(QLatin1StringView(VisivoTheme::kPrimary())));
        h->addWidget(check, 0);
        auto *wa = new QWidgetAction(menu);
        wa->setDefaultWidget(row);
        auto *filter = new RowClickFilter(menu);
        filter->onClick = [this, slot, qn, menu]() {
            setActivePane(slot);
            setActiveDisplayColorMap(qn);
            updateDisplaySectionForActivePane();
            refreshPaneLutStrip(slot);
            menu->close();
        };
        row->installEventFilter(filter);
        menu->addAction(wa);
    };
    // A non-standard pick (from More maps…) persists as the 7th row at the TOP, above the six.
    if (!m_paneCustomColorMap[slot].isEmpty()
        && !curatedColorMaps().contains(m_paneCustomColorMap[slot]))
        addMapRow(m_paneCustomColorMap[slot]);
    for (const QString &qn : curatedColorMaps())
        addMapRow(qn);
    menu->addSeparator();
    QObject::connect(menu->addAction(u"More maps…"_s), &QAction::triggered, this,
                     [this, slot]() { setActivePane(slot); showMoreColorMapsDialog(); });
    menu->addSeparator();
    QObject::connect(menu->addAction(u"Edit LUT…"_s), &QAction::triggered, this,
                     [this, slot]() { setActivePane(slot); showActiveDisplayLUTEditor(); });
    QAction *inv = menu->addAction(u"Invert"_s);
    inv->setCheckable(true);
    inv->setChecked(activeDisplayInvert());
    QObject::connect(inv, &QAction::triggered, this, [this, slot](bool on) {
        setActivePane(slot);
        setActiveDisplayInvert(on);
        updateDisplaySectionForActivePane();
        refreshPaneLutStrip(slot);
    });
    QObject::connect(menu->addAction(u"Reset"_s), &QAction::triggered, this,
                     [this, slot]() { setActivePane(slot); resetActiveDisplayLut(); });

    QToolButton *btn = m_slots[slot].lutBtn;
    menu->popup(btn ? btn->mapToGlobal(QPoint(0, btn->height())) : QCursor::pos());
}

void vtkWindowCube::showMoreColorMapsDialog()
{
    // §5.4b: the rare-case full picker — every registry colormap EXCEPT MinMax (a range
    // mode, not a colormap). Scrollable, swatches, current pre-selected. Applies to the
    // active display target (the pane the popover / combo was opened for).
    QDialog dlg(this);
    dlg.setWindowTitle(u"Color Maps"_s);
    auto *lay = new QVBoxLayout(&dlg);
    auto *list = new QListWidget(&dlg);
    list->setIconSize(QSize(64, 12));
    const QString current = activeDisplayColorMapName();
    for (const auto &nm : ColorMaps::GetColorMapNames()) {
        const QString qn = QString::fromStdString(nm);
        if (qn == u"MinMax"_s)
            continue;   // a range mode, never a colormap (§5.4b)
        auto *it =
                new QListWidgetItem(QIcon(CubeUiAssembler::buildLutPreview(qn, 64, 12)), qn, list);
        if (qn == current)
            list->setCurrentItem(it);
    }
    lay->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg,
                     [&dlg](QListWidgetItem *) { dlg.accept(); });
    dlg.resize(280, 440);
    // exec() spins a nested loop — async work could change the active pane. Re-assert the
    // pane this picker was opened for before applying.
    const int slot = m_activePane;
    if (dlg.exec() == QDialog::Accepted && list->currentItem()) {
        if (slot >= 0 && slot < 4)
            setActivePane(slot);
        setActiveDisplayColorMap(list->currentItem()->text());
        updateDisplaySectionForActivePane();
        if (m_activePane >= 0 && m_activePane < 4)
            refreshPaneLutStrip(m_activePane);
    }
}

void vtkWindowCube::flashPaneLutStrip(int slot)
{
    // §5.5: a ~400ms confirmation flash on a pane that just RECEIVED a linked LUT, so the
    // propagation is visible ("correct implementation = no effect = looks broken").
    if (slot < 0 || slot > 3 || !m_slots[slot].lutBtn)
        return;
    QPointer<QToolButton> btn = m_slots[slot].lutBtn;
    const QLatin1StringView pri(VisivoTheme::kPrimary());
    const QString base =
            u"QToolButton{background:transparent;border:1px solid #2A3238;border-radius:2px;"
            u"padding:1px;}QToolButton:hover{border-color:%1;}"_s.arg(pri);
    btn->setStyleSheet(u"QToolButton{background:rgba(4,138,191,0.28);border:2px solid %1;"
                       u"border-radius:2px;padding:0px;}"_s.arg(pri));
    QTimer::singleShot(400, btn, [btn, base]() {
        if (btn)
            btn->setStyleSheet(base);
    });
}

void vtkWindowCube::refreshLinkedFacetChips()
{
    // Linking needs something to link to: >1 pane in this window OR >1 open cube
    // window (cross-window sync). The master used to stay clickable regardless,
    // as a preference armable before any peer existed — but then it reads as an
    // available tool whose only effect is a "enable it on another cube" hint, so
    // it is gated on real targets now (the Tools ▸ Link Views action is gated
    // with it, in applyMomentToolGate, and states the reason).
    // Disabled chips keep their checked state, so re-enabling restores the choice.
    const bool hasLinkTargets = (m_paneCount > 1 || s_registry.size() > 1);
    if (m_paneLinkedBtn)
        m_paneLinkedBtn->setEnabled(hasLinkTargets);
    const bool master = m_linkViews && hasLinkTargets;
    const auto setChip = [master](QToolButton *chip, bool checked) {
        if (!chip)
            return;
        chip->setEnabled(master);
        QSignalBlocker b(chip);
        chip->setChecked(checked);
    };
    setChip(m_linkCameraChip, m_linkCamera);
    setChip(m_linkChannelChip, m_linkChannel);
    setChip(m_linkLutChip, m_linkLut);
    setChip(m_linkRenderChip, m_linkRender);
    setChip(m_linkBlendChip, m_linkBlend);
    // Every call site of this function is exactly a moment when the link-target
    // count can have changed (pane layout, a window opening or closing), so it
    // is also where the Link Views ACTION — menu + dock + Inspector — is re-gated.
    applyMomentToolGate();
}

void vtkWindowCube::showLinkNoReceiverHint(int srcSlot)
{
    // §5.5: Linked·LUT is on but nothing else shows this product, so the change had no
    // visible peer. Say it once (per layout) near the chip rather than leaving the user to
    // wonder why "Linked" did nothing.
    if (m_linkNoReceiverHintShown || srcSlot < 0 || srcSlot > 3)
        return;
    m_linkNoReceiverHintShown = true;
    const QString label = viewTitle(m_slots[srcSlot].view);
    const QString msg = QStringLiteral("Only pane showing %1 - nothing to link").arg(label);
    QWidget *anchor = m_linkLutChip ? m_linkLutChip.data()
                    : m_paneLinkedBtn ? m_paneLinkedBtn.data()
                                      : nullptr;
    if (anchor)
        QToolTip::showText(anchor->mapToGlobal(QPoint(0, anchor->height())), msg, anchor);
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
        // §5.4b: the six curated maps, then "More maps…" as the last entry (opens the full
        // registry picker). NOT the whole VTK registry, and never MinMax (a range mode).
        for (const QString &qn : curatedColorMaps())
            m_paneLutCombo->addItem(QIcon(CubeUiAssembler::buildLutPreview(qn, 48, 12)), qn);
        m_paneLutCombo->insertSeparator(m_paneLutCombo->count());
        m_paneLutCombo->addItem(u"More maps…"_s);
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
                             if (name == u"More maps…"_s) {
                                 // Not a real choice — open the full picker, then restore the
                                 // combo to whatever is actually active (§5.4b).
                                 showMoreColorMapsDialog();
                                 updateDisplaySectionForActivePane();
                                 return;
                             }
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
    }

    // Same range on every channel (2D Slice only).
    {
        m_sliceRangeLockRow = new QWidget();
        auto *h = new QHBoxLayout(m_sliceRangeLockRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        m_sliceRangeLock = new QCheckBox(u"Same range on every channel"_s, m_sliceRangeLockRow);
        m_sliceRangeLock->setStyleSheet(labelCss);
        m_sliceRangeLock->setToolTip(
                u"Keep this colour range while you move through the channels, so emission "
                u"compares by colour from one channel to the next. Off: each channel is "
                u"stretched to its own minimum and maximum.\n\n"
                u"Choosing a range yourself (percentile, clip values, histogram) turns this on."_s);
        h->addWidget(m_sliceRangeLock, 1);
        auto *cubeBtn = new QToolButton(m_sliceRangeLockRow);
        cubeBtn->setText(u"Cube range"_s);
        cubeBtn->setCursor(Qt::PointingHandCursor);
        cubeBtn->setToolTip(u"Use the minimum and maximum of the whole cube for every channel "
                            u"(of the loaded cube: the preview's until the full resolution is "
                            u"loaded)."_s);
        h->addWidget(cubeBtn, 0);
        m_sliceRangeLockRow->setFixedHeight(28);
        lay->addWidget(m_sliceRangeLockRow);
        QObject::connect(m_sliceRangeLock, &QCheckBox::toggled, this, [this](bool on) {
            if (m_applyingPaneDisplay)
                return;
            setSliceRangeLocked(on);
        });
        QObject::connect(cubeBtn, &QToolButton::clicked, this, [this]() {
            const double lo = this->currentCubeVisibleRange[0];
            const double hi = this->currentCubeVisibleRange[1];
            if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo) {
                this->statusBar()->showMessage(u"The cube's range is not known yet."_s, 3000);
                return;
            }
            setActiveDisplayRange(lo, hi);   // explicit range → locked
        });
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
                pv->setRange(-m, m);   // emits rangeChanged → Linked range propagate
            }
        });
    }

    // Range: CARTA percentile presets + manual clip min/max (§5.2, user-chosen).
    {
        // Bare numbers in a grid are a guess unless you already know CARTA, where
        // this control comes from. Name the row, and give each button the
        // percentiles it actually clips at — "99.5" meaning 0.25 to 99.75 is not
        // something to work out from the caption.
        auto *presetHdr = new QLabel(u"Percentile clip"_s);
        presetHdr->setStyleSheet(labelCss);
        presetHdr->setToolTip(
                u"Set the display range from the distribution of pixel values, keeping "
                u"the central N% and clipping the tails at each end.\n\n"
                u"Lower percentages bring out faint structure by saturating the "
                u"brightest pixels; 100 uses the full min–max of the data."_s);
        lay->addWidget(presetHdr);

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
            if (pr.pct >= 100.) {
                b->setToolTip(u"Full range: the minimum and maximum of the finite pixels, "
                              u"nothing clipped."_s);
            } else {
                const double tail = (100. - pr.pct) / 2.;
                b->setToolTip(u"Keep the central %1% of pixel values: clip below the "
                              u"%2 percentile and above the %3 percentile."_s
                                      .arg(QString::number(pr.pct))
                                      .arg(QString::number(tail, 'g', 4))
                                      .arg(QString::number(100. - tail, 'g', 6)));
            }
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
        l->setToolTip(u"The display range in data units: pixels at or below the first "
                      u"value take the bottom colour, at or above the second the top. "
                      u"The percentile buttons above fill these in; type to override."_s);
        h->addWidget(l, 0);
        auto mkSpin = [&]() {
            auto *s = new SigFigSpinBox(r);   // (c) compact 6-sig-fig display, full value in tooltip
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
    lay->addStretch(1);   // same reason as the 3-D page: slack at the bottom
    return page;
}

void vtkWindowCube::applyPaneDisplayRange(bool /*fromMin*/)
{
    if (m_applyingPaneDisplay)
        return;
    if (!m_paneRangeMin || !m_paneRangeMax)
        return;
    double lo = m_paneRangeMin->value(), hi = m_paneRangeMax->value();
    // editingFinished also fires when focus merely leaves the field: an unchanged
    // value is not a choice (and on the slice, a choice locks the range).
    const auto same = [](double a, double b) {
        // The field rounds to its 6 decimals; anything within that is "unchanged".
        return std::abs(a - b) <= 1e-6 + 1e-6 * std::max(std::abs(a), std::abs(b));
    };
    if (same(lo, activeDisplayRangeLo()) && same(hi, activeDisplayRangeHi()))
        return;
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
    // not whatever is active later (§5.2).
    if (!m_paneLutCustomizer) {
        m_paneLutCustomizer = new LUTCustomizerDialog(this);
        QObject::connect(m_paneLutCustomizer, &LUTCustomizerDialog::lutUpdated, this, [this]() {
            Pane2DView *p = m_paneLutTarget;
            if (!p || !p->lookupTable())
                return;
            // The dialog mutated the pane's OWN lut in place (name via ObjectName, range,
            // stretch). Adopt name+scale+gamma so getters/Linked aren't stale, then re-fit.
            // adoptLutState emits lutChanged → the pane's own slot refreshes its strip and
            // runs the Linked propagation (no explicit propagate here — would double-flash).
            p->adoptLutState(QString::fromStdString(p->lookupTable()->GetObjectName()),
                             m_paneLutCustomizer->scaleMode(), m_paneLutCustomizer->scaleGamma());
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
    // Page 1 (the per-pane controls) drives EITHER an IMAGE Product pane (its own Pane2DView)
    // or the 2D Slice (shared lutSlice) via the activeDisplay* dispatch. A spectrum Product
    // (#5) has no colormap/range/scale → the page stays disabled for it.
    const bool activeIsImageProduct =
            (kind == ViewId::Product && m_activePane >= 0 && m_activePane < 4
             && m_products2d.contains(m_slots[m_activePane].view.productId));
    const bool isPane = (activeIsImageProduct || kind == ViewId::Slice2D);

    m_displayStack->setCurrentIndex(is3d ? 0 : (isPane ? 1 : 2));
    if (auto *p1 = m_displayStack->widget(1))
        p1->setEnabled(isPane);
    if (m_displayNoControls && !is3d && !isPane) {
        const bool pv = kind == ViewId::Product && m_activePane >= 0 && m_activePane < 4
                && !m_pvProductId.isEmpty() && m_slots[m_activePane].view.productId == m_pvProductId;
        m_displayNoControls->setText(
                pv ? u"The PV diagram's width, curve and exports are in the diagram itself; "
                     u"its colour bar follows the data."_s
                : kind == ViewId::Product
                        ? u"A spectrum has no colour map: its tools (compare, fit, line list, "
                          u"export) are in Inspector › Analysis."_s
                        : u"Pick a view for this pane from its ▾ menu."_s);
    }

    // Header text `DISPLAY — <active pane title>` (pane name in primary is handled by the
    // section style; keep it simple text here). The built-in setTxt would revert this on
    // toggle, so this runs on toggle too.
    const bool hasActive = (m_activePane >= 0 && m_activePane < 4);
    const bool wearingMap = (kind == ViewId::Slice2D) && !m_sliceBackdropProductId.isEmpty();
    QString title = is3d ? u"3D VIEW"_s
                  : wearingMap ? viewTitle(PaneView{ ViewId::Product, m_sliceBackdropProductId }).toUpper()
                  : (kind == ViewId::Slice2D)  ? u"2D SLICE"_s
                  : (kind == ViewId::Product)  ? viewTitle(m_slots[m_activePane].view).toUpper()
                  : (kind == ViewId::Spectrum) ? u"SPECTRUM"_s
                  : hasActive                  ? u"NO PRODUCT"_s   // a None "empty" pane is active
                                               : u"NO PANE SELECTED"_s;
    const bool open = m_displayHeaderBtn->isChecked();
    m_displayHeaderBtn->setText((open ? u"▼  "_s : u"▶  "_s) + u"DISPLAY - "_s + title);

    // LAYERS follows the active pane. Volume, the cutting plane and the WCS axes
    // are 3-D actors; contours are drawn into the slice renderer. Showing all of
    // them at once meant half the section was inert whichever pane you were on,
    // and a checkbox you cannot press because you are looking elsewhere is noise
    // rather than information. Catalogue sources stays: it applies to both, and
    // its own tooltip explains the one reason it greys out.
    //
    // ABOVE the isPane guard, deliberately. isPane means "has the DISPLAY page's
    // colormap/range controls", which is true for the slice and image products
    // and false for the 3-D view — the very pane three of these four rows exist
    // for. Below the return, opening in 3-D left every row showing and switching
    // slice → 3-D left the 3-D rows hidden.
    const bool isSlice = (kind == ViewId::Slice2D);
    if (m_layerRowVolume)        m_layerRowVolume->setVisible(is3d);
    if (m_layerRowCuttingPlane)  m_layerRowCuttingPlane->setVisible(is3d);
    updateCuttingPlaneOpacityRowVisibility();
    if (m_layerRow3dWcsAxes)     m_layerRow3dWcsAxes->setVisible(is3d);
    if (m_layerRowContours)      m_layerRowContours->setVisible(isSlice);

    if (!isPane)
        return;

    // Repopulate the pane page from the active display target (Product pane or 2D Slice)
    // via the activeDisplay* dispatch; guard the two-way binding.
    QSignalBlocker bMin(m_paneRangeMin), bMax(m_paneRangeMax), bSym(m_paneSymmetric),
            bScale(m_paneScaleSeg), bLut(m_paneLutCombo);
    // E-07/M7: RAII guard — a throw mid-repopulate must not leave the flag stuck
    // true, which would permanently disable propagatePaneLut/RangeWithinWindow.
    QScopedValueRollback<bool> paneDisplayGuard(m_applyingPaneDisplay, true);
    if (m_paneScaleRow)
        m_paneScaleRow->setVisible(kind == ViewId::Slice2D);   // Scale: 2D Slice only
    // "Same range on every channel" means nothing while the slice shows a map.
    if (m_sliceRangeLockRow)
        m_sliceRangeLockRow->setVisible(kind == ViewId::Slice2D
                                        && m_sliceBackdropProductId.isEmpty());
    if (kind == ViewId::Slice2D)
        refreshSliceBackdropCombo();
    if (m_sliceRangeLock)
        m_sliceRangeLock->setChecked(this->m_sliceRangeLocked);

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
    // Keep the pane's persistent custom entry in sync with ANY non-curated current — whether
    // it arrived via More maps…, Edit LUT…, or a legacy combo — so the popover marks it and
    // the dock combo can show it. Never MinMax (a range mode, not a colormap) (§5.4b).
    if (!cmap.isEmpty() && cmap != u"MinMax"_s && !curatedColorMaps().contains(cmap)
        && m_activePane >= 0 && m_activePane < 4)
        m_paneCustomColorMap[m_activePane] = cmap;
    if (m_paneLutCombo && !cmap.isEmpty()) {
        // The combo lists the six curated maps + "More maps…". The active pane's persistent
        // non-standard pick (m_paneCustomColorMap) shows as a REAL top entry that stays put
        // for re-selection — not a transient label; a newer non-standard replaces it (§5.4b).
        const QString custom =
                (m_activePane >= 0 && m_activePane < 4) ? m_paneCustomColorMap[m_activePane]
                                                        : QString();
        // Drop any previously-inserted custom extra at index 0 (bounded to one).
        if (m_paneLutCombo->count() && m_paneLutCombo->itemText(0) != u"More maps…"_s
            && !m_paneLutCombo->itemText(0).isEmpty()
            && !curatedColorMaps().contains(m_paneLutCombo->itemText(0)))
            m_paneLutCombo->removeItem(0);
        if (!custom.isEmpty() && !curatedColorMaps().contains(custom))
            m_paneLutCombo->insertItem(
                    0, QIcon(CubeUiAssembler::buildLutPreview(custom, 48, 12)), custom);
        m_paneLutCombo->setCurrentText(cmap);
    }
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
    // paneDisplayGuard restores m_applyingPaneDisplay on scope exit (RAII).
}

void vtkWindowCube::propagatePaneLutWithinWindow(int srcSlot)
{
    // §5.5: with Linked on, a pane's LUT change follows only panes showing the SAME
    // product (same kind AND productId) — never across kinds, never moment→slice. This
    // is within-window and separate from the cross-window broadcastLinkedColour path.
    if (!m_linkViews || !m_linkLut || m_applyingPaneDisplay || srcSlot < 0 || srcSlot > 3)
        return;
    Pane2DView *src = m_paneViews[srcSlot];
    if (!src || m_slots[srcSlot].view.kind != ViewId::Product)
        return;
    const QString pid = m_slots[srcSlot].view.productId;
    int receivers = 0;
    {
        QScopedValueRollback<bool> guard(m_applyingPaneDisplay, true);
        for (int j = 0; j < 4; ++j) {
            if (j == srcSlot || !m_paneViews[j])
                continue;
            if (m_slots[j].view.kind != ViewId::Product || m_slots[j].view.productId != pid)
                continue;
            m_paneViews[j]->applyLut(src->colorMapName(), src->scale(), src->gamma(),
                                     src->invert());
            m_paneViews[j]->setRange(src->rangeMin(), src->rangeMax());
            refreshPaneLutStrip(j);
            flashPaneLutStrip(j);   // §5.5: the receiver flashes so propagation is VISIBLE
            ++receivers;
        }
    }
    // §5.5: with Linked·LUT on but nothing showing the same product, say so once — otherwise
    // "correct = no effect" reads as broken. (A cross-window peer may still receive it below.)
    if (receivers == 0)
        showLinkNoReceiverHint(srcSlot);
    // STEP6-P4: also push to linked peer WINDOWS — moment / image products matched by the
    // window-independent stem (m_products2d stems are deterministic: moment<order>, linewidth_*).
    this->broadcastLinked2DImageLut(productStem(pid), src->colorMapName(),
                                    static_cast<int>(src->scale()), src->gamma(), src->invert());
}

void vtkWindowCube::propagatePaneRangeWithinWindow(int srcSlot)
{
    // §5.5: cheap path used on a LIVE window/level drag — only the RANGE follows the
    // same-product peers (no colormap rebuild), so a linked drag doesn't re-Build LUTs
    // on every mouse move. Colormap/scale/invert use the full path (lutChanged). Range is
    // part of the "LUT" facet (colour map / range), so it obeys m_linkLut too.
    if (!m_linkViews || !m_linkLut || m_applyingPaneDisplay || srcSlot < 0 || srcSlot > 3)
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

void vtkWindowCube::propagate2DCamera(int srcSlot)
{
    // Linked 2D: a pan/zoom on one 2-D view follows every other visible 2-D view — image-product
    // panes (Pane2DView) AND the 2-D slice (STEP6-P1). They share the same cube spatial grid, and
    // the view is copied GUTTER-INDEPENDENTLY (data-area centre + world height, STEP6-P3), so panes
    // with different colorbar gutters frame the same feature at the same zoom. Gated on the Linked
    // master + camera facet. Receivers apply echo-guarded (or programmatic Render → no
    // InteractionEvent), so there is no loop.
    if (!m_linkViews || !m_linkCamera || srcSlot < 0 || srcSlot > 3)
        return;
    double center[2] = { 0., 0. };
    double worldHeight = 0.;
    bool ok = false;
    const ViewId sk = m_slots[srcSlot].view.kind;
    if (sk == ViewId::Slice2D)
        ok = this->sliceCameraNormalized(center, worldHeight);
    else if (sk == ViewId::Product && m_paneViews[srcSlot]
             && m_products2d.contains(m_slots[srcSlot].view.productId))
        ok = m_paneViews[srcSlot]->cameraStateNormalized(center, worldHeight);
    if (!ok || !(worldHeight > 0.))
        return;   // spectra / PV / no geometry → nothing to sync
    // STEP6-P2: remember the shared view so a later fit() on any linked pane re-adopts it.
    m_linked2DCenter[0] = center[0];
    m_linked2DCenter[1] = center[1];
    m_linked2DWorldHeight = worldHeight;
    m_linked2DValid = true;
    this->applyNormalizedTo2DPanes(center, worldHeight, srcSlot);
    // STEP6-P4: also push to linked peer WINDOWS on the same spatial grid.
    this->broadcastLinked2DCamera(center, worldHeight);
}

void vtkWindowCube::applyNormalizedTo2DPanes(const double dataCenterXY[2], double worldHeight,
                                             int skipSlot)
{
    for (int j = 0; j < 4; ++j) {
        if (j == skipSlot)
            continue;
        if (!m_slots[j].frame || !m_slots[j].frame->isVisible())
            continue;   // don't render parked/hidden panes (they re-sync when shown)
        const ViewId jk = m_slots[j].view.kind;
        if (jk == ViewId::Slice2D)
            this->applySliceCameraNormalized(dataCenterXY, worldHeight);
        else if (jk == ViewId::Product && m_paneViews[j]
                 && m_products2d.contains(m_slots[j].view.productId))
            m_paneViews[j]->setCameraStateNormalized(dataCenterXY, worldHeight);
    }
}

void vtkWindowCube::reassertLinked2DCamera(int slot)
{
    // STEP6-P2: a pane just re-fitted (range/LUT/resize letterbox), discarding the shared zoom.
    // If Linked·camera is on and we HAVE a shared 2D camera, re-apply it so the pane rejoins the
    // group's view instead of snapping back to full-frame. Applies echo-guarded and does not call
    // fit(), so this can't recurse. First user pan/zoom seeds m_linked2DValid.
    if (!m_linkViews || !m_linkCamera || !m_linked2DValid || slot < 0 || slot > 3)
        return;
    if (!m_slots[slot].frame || !m_slots[slot].frame->isVisible())
        return;
    const ViewId k = m_slots[slot].view.kind;
    if (k == ViewId::Slice2D)
        this->applySliceCameraNormalized(m_linked2DCenter, m_linked2DWorldHeight);
    else if (k == ViewId::Product && m_paneViews[slot]
             && m_products2d.contains(m_slots[slot].view.productId))
        m_paneViews[slot]->setCameraStateNormalized(m_linked2DCenter, m_linked2DWorldHeight);
}

int vtkWindowCube::sliceSlot() const
{
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Slice2D && m_slots[i].frame
            && m_slots[i].frame->isVisible())
            return i;
    return -1;
}

bool vtkWindowCube::sliceCameraNormalized(double dataCenterXY[2], double &worldHeight) const
{
    // STEP6-P1: the slice viewport's gutter-independent camera — same math as Pane2DView, on the
    // singular slice renderer + its own gutter (gutter2dFor(slice)).
    if (!this->sliceWin)
        return false;
    auto *rc = this->sliceWin->GetRenderers();
    auto *renderer = rc ? rc->GetFirstRenderer() : nullptr;
    if (!renderer)
        return false;
    const int *size = renderer->GetSize();
    if (!size)
        return false;
    auto *image = vtkImageData::SafeDownCast(
            this->remoteSliceDisplaySource->GetOutputDataObject(0));
    const auto data = CubeGeom::dataAreaFor(size, image, this->gutter2dFor(/*slice=*/true));
    return CubeGeom::readNormalizedCamera(renderer, data, dataCenterXY, worldHeight);
}

void vtkWindowCube::applySliceCameraNormalized(const double dataCenterXY[2], double worldHeight,
                                               bool render)
{
    if (!this->sliceWin)
        return;
    auto *rc = this->sliceWin->GetRenderers();
    auto *renderer = rc ? rc->GetFirstRenderer() : nullptr;
    if (!renderer)
        return;
    const int *size = renderer->GetSize();
    if (!size)
        return;
    auto *image = vtkImageData::SafeDownCast(
            this->remoteSliceDisplaySource->GetOutputDataObject(0));
    const auto data = CubeGeom::dataAreaFor(size, image, this->gutter2dFor(/*slice=*/true));
    double focalXY[2] = { 0., 0. }, parScale = 0.;
    if (!CubeGeom::normalizedToFocalScale(size, data, dataCenterXY, worldHeight, focalXY, parScale))
        return;
    auto *cam = renderer->GetActiveCamera();
    double f[3] = { 0., 0., 0. }, p[3] = { 0., 0., 0. };
    cam->GetFocalPoint(f);
    cam->GetPosition(p);
    f[0] = focalXY[0]; f[1] = focalXY[1];
    p[0] = focalXY[0]; p[1] = focalXY[1];   // parallel down-Z: keep the z components
    cam->SetFocalPoint(f);
    cam->SetPosition(p);
    cam->SetParallelScale(parScale);
    // A programmatic camera set + Render fires no InteractionEvent, so onSliceCameraInteraction
    // won't echo. Record the new pan/zoom pose so a following window/level drag (which does NOT
    // move the camera) is correctly ignored by onSliceCameraInteraction. Remember this
    // as the slice's fit baseline so layout2dPaneFor's guard treats the shared zoom as already
    // fitted and doesn't snap it back.
    this->captureSliceCamPose();
    this->m_slice2dFitSize = { size[0], size[1] };
    const auto fitGutter = this->gutter2dFor(/*slice=*/true);
    this->m_slice2dFitRight = fitGutter.right;
    this->m_slice2dFitBottom = fitGutter.bottom;
    if (render)
        this->sliceWin->Render();
}

void vtkWindowCube::captureSliceCamPose()
{
    // Record the slice camera's pan/zoom (focal x/y + parallel scale) as the last-seen pose, so
    // onSliceCameraInteraction can tell a real pan/zoom from a window/level drag or a bare
    // clipping-range bump. Called after the initial fit + every programmatic slice-camera set.
    if (!this->sliceWin)
        return;
    auto *rc = this->sliceWin->GetRenderers();
    auto *renderer = rc ? rc->GetFirstRenderer() : nullptr;
    auto *cam = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!cam)
        return;
    double f[3] = { 0., 0., 0. };
    cam->GetFocalPoint(f);
    m_sliceCamFocalX = f[0];
    m_sliceCamFocalY = f[1];
    m_sliceCamScale = cam->GetParallelScale();
}

void vtkWindowCube::onSliceCameraInteraction()
{
    // STEP6-P1: the user panned/zoomed the 2-D slice → propagate to the linked image panes. But
    // vtkInteractorStyleImage also fires InteractionEvent on a window/level LEFT-drag (no pan/zoom),
    // and overlays bump the camera via ResetCameraClippingRange; compare the actual PAN/ZOOM POSE
    // (not the MTime) so only a real move propagates. The pose is tracked UNCONDITIONALLY (even
    // unlinked) so it never goes stale.
    if (!this->sliceWin)
        return;
    auto *rc = this->sliceWin->GetRenderers();
    auto *renderer = rc ? rc->GetFirstRenderer() : nullptr;
    auto *cam = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!cam)
        return;
    double f[3] = { 0., 0., 0. };
    cam->GetFocalPoint(f);
    const double scale = cam->GetParallelScale();
    const bool moved = (f[0] != m_sliceCamFocalX || f[1] != m_sliceCamFocalY
                        || scale != m_sliceCamScale);
    m_sliceCamFocalX = f[0];
    m_sliceCamFocalY = f[1];
    m_sliceCamScale = scale;
    if (!moved || !m_linkViews || !m_linkCamera)
        return;   // window/level / clipping-only / not linked → nothing to propagate
    const int s = this->sliceSlot();
    if (s >= 0)
        this->propagate2DCamera(s);
}

void vtkWindowCube::setActivePane(int pane)
{
    if (pane < 0 || pane > 3)
        return;
    const bool changed = (m_activePane != pane);
    // Moving the focus does NOT disarm the 3-D picker any more. It listens to the
    // 3-D view and that view keeps its input while armed (paneIsArmedTarget), so
    // selecting another pane — the picker's own live spectrum, typically, to reach
    // its controls — used to stop the tool and delete the preview the user had
    // just made. The amber border says where it is listening; Esc ends it.
    m_activePane = pane;
    refreshPaneActiveStyles();   // also re-gates interactors (§5.1)
    if (changed)
        emit activePaneChanged(pane);   // §5.2: DISPLAY dock follows the active pane
}

void vtkWindowCube::focusSessionViewIfVisible(const QString &viewTag, const QString &productId)
{
    using namespace Qt::StringLiterals;
    // The gentle half of activateSessionView: selecting a row says "this is what I
    // am looking at", so the pane showing it becomes the active one — and the left
    // dock's DISPLAY section, which follows the ACTIVE PANE, is then about that row.
    // Without this, highlighting "2D Slice" while the 3-D pane stayed active left
    // the dock headed "DISPLAY - 3D VIEW", and the colour map changed there went to
    // the volume (user report).
    //
    // What it deliberately does NOT do is mount anything or move panes around: a
    // row that is not on screen is left alone, and double-click still mounts it.
    PaneView target;
    if (viewTag == u"view3d"_s)
        target = { ViewId::View3D, {} };
    else if (viewTag == u"slice2d"_s)
        target = { ViewId::Slice2D, {} };
    else if (!productId.isEmpty())
        target = { ViewId::Product, productId };
    else
        return;

    // A product duplicated into two panes matches both. If one of them is already
    // active, stay there: switching to the lowest-numbered match would move the
    // focus off the pane the user is working in, for a row that describes it too.
    if (m_activePane >= 0 && m_activePane < 4 && m_slots[m_activePane].frame
        && m_slots[m_activePane].frame->isVisible() && m_slots[m_activePane].view == target) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (m_slots[i].frame && m_slots[i].frame->isVisible() && m_slots[i].view == target) {
            setActivePane(i);
            return;
        }
    }
}

void vtkWindowCube::activateSessionView(const QString &viewTag, const QString &productId)
{
    // Resolve the Session-tree row to a pane view. A dataset root (both empty) has no
    // pane — leave the selection alone.
    using namespace Qt::StringLiterals;
    // Whatever the row turns out to be, show WHERE it came from. The numbers or
    // the curve say what was measured; the overlay says on which pixels, which
    // is the half a product could not answer before (user report). Harmless for
    // rows with no geometry: restoreProductOrigin returns false and draws
    // nothing.
    if (!productId.isEmpty()) {
        const AnalysisProduct product = ProductRegistry::instance().product(productId);
        if (this->restoreProductOrigin(product.origin)) {
            const QString where = visivo::describeOrigin(product.origin);
            if (!where.isEmpty())
                this->statusBar()->showMessage(
                        u"Showing %1%2"_s.arg(where, this->m_originRestoreNote), 6000);
        }
        // Reopening a product must put back everything producing it put up, not
        // just the pane. A region analysis shows a spectrum AND its statistics —
        // the statistics live in Provenance here rather than in a floating
        // window, and registration reveals that tab (§4.1.4), so reopening has
        // to as well or the numbers are simply missing the second time (user
        // report). Only on explicit activation: passive selection must not yank
        // the Inspector off the Analysis list.
        if (this->m_workspaceChrome && !product.params.isEmpty())
            this->m_workspaceChrome->revealParametersTab();
    }

    PaneView target;
    if (viewTag == u"view3d"_s)
        target = { ViewId::View3D, {} };
    else if (viewTag == u"slice2d"_s)
        target = { ViewId::Slice2D, {} };
    else if (!productId.isEmpty() && isPaneRenderableProduct(productId))
        // Only a pane-RENDERABLE product — an image in m_products2d (moment/…), a spectrum in
        // m_products1d (#5), or the PV diagram — can be mounted. Baseline / noise rows open
        // their OWN windows: a single-click drives Provenance, double-click raises the window.
        target = { ViewId::Product, productId };
    else
        return;

    // Asking for the 2D Slice means the channels: a map worn as background goes.
    if (target.kind == ViewId::Slice2D && !m_sliceBackdropProductId.isEmpty())
        setSliceBackdrop(QString());

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

void vtkWindowCube::showSessionRowContextMenu(const QString &viewTag, const QString &productId,
                                              const QPoint &globalPos)
{
    using namespace Qt::StringLiterals;
    // What the row stands for. An intrinsic view (3D Volume / 2D Slice) is as
    // mountable as a product and used to be the one row with no menu at all.
    PaneView target;
    if (viewTag == u"view3d"_s)
        target = PaneView{ ViewId::View3D, {} };
    else if (viewTag == u"slice2d"_s)
        target = PaneView{ ViewId::Slice2D, {} };
    else if (!productId.isEmpty() && isPaneRenderableProduct(productId))
        target = PaneView{ ViewId::Product, productId };

    QMenu menu;
    if (target.isNone()) {
        // A product that has no pane renderer (baseline, noise, channel maps, …)
        // lives in its own window. Say so, and offer the one thing that applies,
        // rather than showing nothing at all on right-click.
        const AnalysisProduct p = ProductRegistry::instance().product(productId);
        // A product stored as a FITS on the backend (a baseline-subtracted cube,
        // a mask, a sub-cube) is reopenable for as long as that file exists —
        // which outlives the tool window by far. Without this the row died with
        // its dialog: nothing to click, double-click inert, and a 1.5 GB result
        // sitting in the backend's temp directory that the UI could not reach
        // (user report).
        const QString fitsPath = p.params.value(u"Output path"_s).toString();
        if (!fitsPath.isEmpty()) {
            QObject::connect(menu.addAction(u"Open as a New Cube"_s), &QAction::triggered, this,
                             [this, fitsPath, label = p.label]() {
                                 emit this->openBackendFitsRequested(fitsPath, label);
                             });
            // Keeping it. The file is in the backend's TEMP directory, so the
            // honest answer to "how do I hold on to this?" used to be "run
            // Export Sub-Cube as FITS" — a crop dialog, to copy a whole cube
            // (user report). One click, straight into Workspace Exports.
            const QString backendId = p.params.value(u"New dataset ID"_s).toString();
            const bool temporary = p.params.value(u"Storage"_s).toString().contains(
                    u"temporary"_s, Qt::CaseInsensitive);
            if (!backendId.isEmpty() && temporary) {
                QAction *keep = menu.addAction(u"Save a Permanent Copy (Workspace Exports)"_s);
                keep->setToolTip(u"Copy this cube out of the backend's temporary "
                                 u"directory into Workspace Exports, where it survives "
                                 u"the session and the system will not delete it."_s);
                QObject::connect(keep, &QAction::triggered, this,
                                 [this, backendId, label = p.label, fitsPath]() {
                                     this->saveProductCopyToWorkspace(backendId, label, fitsPath);
                                 });
            }
            QAction *where = menu.addAction(u"Copy File Path"_s);
            QObject::connect(where, &QAction::triggered, this, [fitsPath]() {
                QGuiApplication::clipboard()->setText(fitsPath);
            });
            menu.addSeparator();
        }
        // A MEASUREMENT (no renderer, no file, no window) still has something to
        // open: the numbers it recorded. Double-click does it too, but a
        // double-click nobody guesses is the same as no action at all, which is
        // how this row felt (user report).
        const bool isMeasurement = fitsPath.isEmpty() && !p.window && !p.params.isEmpty();
        if (isMeasurement) {
            QObject::connect(menu.addAction(u"Show the Numbers Again"_s), &QAction::triggered,
                             this, [this, pid = p.productId]() {
                                 const AnalysisProduct prod =
                                         ProductRegistry::instance().product(pid);
                                 if (this->restoreProductOrigin(prod.origin)) {
                                     if (auto *win = this->active2dWin())
                                         win->Render();
                                     if (!this->m_originRestoreNote.isEmpty())
                                         this->statusBar()->showMessage(
                                                 u"Showing %1%2"_s.arg(
                                                         visivo::describeOrigin(prod.origin),
                                                         this->m_originRestoreNote),
                                                 6000);
                                 }
                                 this->showRecordedResult(prod);
                             });
            menu.addSeparator();
        }
        QAction *note = menu.addAction(
                !fitsPath.isEmpty()  ? u"Stored on the backend"_s
                : isMeasurement      ? u"A measurement: its numbers are in Provenance"_s
                                     : u"Opens in its own window"_s);
        note->setEnabled(false);
        if (p.window) {
            QObject::connect(menu.addAction(u"Bring window to front"_s), &QAction::triggered,
                             this, [win = QPointer<QWidget>(p.window)]() {
                                 if (win) {
                                     win->show();
                                     win->raise();
                                     win->activateWindow();
                                 }
                             });
        }
        menu.exec(globalPos);
        return;
    }

    QMenu *showIn = menu.addMenu(u"Show in"_s);
    // One entry per VISIBLE pane (numbered), marking where it already shows.
    for (int i = 0; i < 4; ++i) {
        if (!m_slots[i].frame || !m_slots[i].frame->isVisible())
            continue;
        const bool here = (m_slots[i].view == target);
        QString label = QStringLiteral("Pane %1").arg(i + 1);
        if (here)
            label += u"  ✓"_s;
        else if (!m_slots[i].view.isNone())
            label += u"  (replaces %1)"_s.arg(viewTitle(m_slots[i].view));
        QAction *a = showIn->addAction(label);
        a->setEnabled(!here);
        const int slot = i;
        QObject::connect(a, &QAction::triggered, this,
                         [this, target, slot]() { showViewInPane(target, slot); });
    }
    if (m_paneCount < 4) {
        showIn->addSeparator();
        QObject::connect(showIn->addAction(u"New pane"_s), &QAction::triggered, this,
                         [this, target]() { showViewInPane(target, -1); });
    }
    menu.exec(globalPos);
}

void vtkWindowCube::showProductInPane(const QString &productId, int slot)
{
    if (!isPaneRenderableProduct(productId))
        return;
    showViewInPane(PaneView{ ViewId::Product, productId }, slot);
}

void vtkWindowCube::showViewInPane(const PaneView &target, int slot)
{
    if (target.isNone())
        return;
    if (slot < 0) {
        // New pane: grow the grid, then take a slot that TRANSITIONED hidden→visible (assignViews
        // seeds the grown slot with a default like Slice2D, so a plain "None" search would find
        // nothing and must NOT fall back to the active pane — that would replace the user's
        // content).
        std::array<bool, 4> wasVisible{};
        for (int i = 0; i < 4; ++i)
            wasVisible[i] = m_slots[i].frame && m_slots[i].frame->isVisible();
        if (m_paneCount == 1)
            applyPaneLayout(2);
        else if (m_paneCount == 2)
            applyPaneLayout(4);
        slot = -1;
        for (int i = 0; i < 4; ++i)
            if (!wasVisible[i] && m_slots[i].frame && m_slots[i].frame->isVisible()) {
                slot = i;   // the newly-revealed cell
                break;
            }
        if (slot < 0)   // already full / nothing newly visible → first free None, else give up
            for (int i = 0; i < 4; ++i)
                if (m_slots[i].frame && m_slots[i].frame->isVisible()
                    && m_slots[i].view.isNone()) {
                    slot = i;
                    break;
                }
        if (slot < 0)
            return;   // don't steal an occupied pane
    }
    // A stale explicit target (menu left open across a layout change) must not mount into a
    // now-hidden slot.
    if (slot < 0 || slot > 3 || !m_slots[slot].frame || !m_slots[slot].frame->isVisible())
        return;
    m_slots[slot].view = target;
    clearViewFromOtherSlots(slot);   // no-op for a Product (viewWidget==null) → duplication OK
    mountView(slot);
    syncPick3d2d();
    refreshPaneChrome();
    layout2dPane(true);
    setActivePane(slot);
}

void vtkWindowCube::pushProductPaneBadges()
{
    // (#4) Tell the Session tree which VISIBLE pane(s) show each product → pane-number
    // badges. A product duplicated in two panes lists both; one not on screen lists none.
    if (!m_workspaceChrome)
        return;
    QHash<QString, QList<int>> assignments;
    for (int i = 0; i < 4; ++i) {
        if (!m_slots[i].frame || !m_slots[i].frame->isVisible())
            continue;
        if (m_slots[i].view.kind == ViewId::Product && !m_slots[i].view.productId.isEmpty())
            assignments[m_slots[i].view.productId] << (i + 1);   // pane numbers are 1-based
    }
    m_workspaceChrome->setSessionProductPaneAssignments(assignments);
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
    else if (v.kind == ViewId::Slice2D && !m_sliceBackdropProductId.isEmpty())
        pid = m_sliceBackdropProductId;   // the slice wearing a map IS that map to the user
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
            m_paneViews[i]->setInteractive(i == m_activePane || paneIsArmedTarget(i));
    // §5.1b: the remote 3D widget renders a server stream through its OWN QWidget mouse
    // handlers, so the VTK/Qt-level swallow can't reach it — gate it explicitly. Interactive
    // only when the pane showing the 3D view is the active pane (there is one 3D view).
    // isHidden(), not isVisible(): before the window is first shown every widget
    // reports isVisible() false, and a refresh run then left the 3D view
    // non-interactive until the next focus or layout change.
    if (m_remoteRenderWidget) {
        bool active3d = false;
        for (int i = 0; i < 4; ++i)
            if (m_slots[i].view.kind == ViewId::View3D && m_slots[i].frame
                && !m_slots[i].frame->isHidden()) {
                active3d = (i == m_activePane) || paneIsArmedTarget(i);
                break;
            }
        m_remoteRenderWidget->setInteractive(active3d);
    }
}

void vtkWindowCube::refreshPaneActiveStyles()
{
    // Called on every focus and layout change, which is exactly when the one
    // export entry has to say something else.
    updateExportSelectedAction();
    const QLatin1StringView pri(VisivoTheme::kPrimary());
    // §3.2: the active-pane distinction must be VISIBLE, not documented. The active
    // pane carries the brand-blue border + a filled brand-blue dot; every other pane
    // gets a near-flush #1E2427 border and a #3A4247 dot, so "tools act here" reads
    // straight off the pane rather than the changelog.
    static constexpr QLatin1StringView kInactiveBorder("#1E2427");
    static constexpr QLatin1StringView kInactiveDot("#3A4247");
    // While an interaction tool is armed, the pane it LISTENS TO wears a thicker
    // amber border. Every one of these tools installs its observer on a single
    // view and silently ignores clicks in any other, which is unreadable without
    // a mark saying where it is listening — it was previously marked for the 3-D
    // picker only, so the six 2-D tools armed invisibly.
    // Set here rather than alongside each toggle because this function rewrites
    // every pane frame and would otherwise paint over it.
    const bool armed3d = anyTool3dArmed();
    const bool armed2d = anyTool2dArmed();
    // Esc is the way out of an armed tool ("how do I deselect this?"). The
    // shortcut is otherwise the maximized-pane restore, and stays disabled while
    // neither applies so an idle Esc never steals the key from a focused child.
    if (m_escRestoreShortcut)
        m_escRestoreShortcut->setEnabled(armed2d || armed3d || m_maximizedPane >= 0);
    const QLatin1StringView armed(VisivoTheme::kWarning());
    for (int i = 0; i < 4; ++i) {
        const bool active = (m_activePane == i);
        const bool armedHere = (armed3d && m_slots[i].view.kind == ViewId::View3D)
                || (armed2d && m_slots[i].view.kind == ViewId::Slice2D);
        if (m_slots[i].frame)
            m_slots[i].frame->setStyleSheet(
                    armedHere
                            ? QStringLiteral("QFrame#paneFrame { border: 2px solid %1; }")
                                      .arg(armed)
                            : QStringLiteral("QFrame#paneFrame { border: 1px solid %1; }")
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
            // …and an ARMED slice pane keeps the crosshair even when the focus is
            // elsewhere, because it still takes the tool's input.
            if (QWidget *w = viewWidget(m_slots[i].view))
                w->setCursor((m_activePane == i || paneIsArmedTarget(i)) ? Qt::CrossCursor
                                                                        : Qt::ArrowCursor);
        } else if (k == ViewId::Product && m_paneViews[i] && m_paneViews[i]->widget()) {
            // Display-only: always the plain arrow, never a crosshair — a moment pane
            // takes no probe/region/PV (those act on the 2-D Slice). §3.2 B3.
            m_paneViews[i]->widget()->setCursor(Qt::ArrowCursor);
        }
    }
}

void vtkWindowCube::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    // Whether an image viewer is open gates "Send Slice to Image Viewer", and this
    // window is not told when another window appears or closes. Activation is the
    // moment before the user can reach for the tool, so it is a sufficient — and
    // cheap — place to recheck. (Ditto for the cross-window link peers, which the
    // registry already refreshes, but a re-run costs nothing.)
    if (event && event->type() == QEvent::ActivationChange && this->isActiveWindow())
        this->applyMomentToolGate();
}

bool vtkWindowCube::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type et = event->type();
    // Closing a detached-pane window re-docks the pane into its grid slot (never destroys it).
    if (et == QEvent::Close)
        for (int i = 0; i < 4; ++i)
            if (m_slots[i].detachedWindow && watched == m_slots[i].detachedWindow) {
                bringBackPane(i);
                return true;   // we handled the close (reparented the frame out first)
            }
    if (et == QEvent::MouseButtonPress) {
        for (int i = 0; i < 4; ++i)
            if (m_slots[i].header && watched == m_slots[i].header) {
                setActivePane(i);
                break;
            }
    }
    // Double-click a pane header toggles pop-out (detach / re-attach).
    if (et == QEvent::MouseButtonDblClick)
        for (int i = 0; i < 4; ++i)
            if (m_slots[i].header && watched == m_slots[i].header) {
                if (paneDetached(i))
                    bringBackPane(i);
                else
                    detachPane(i);
                return true;
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
            QWidget *paneW = nullptr;
            if (k == ViewId::Slice2D)
                paneW = ui->vtkImage;
            else if (k == ViewId::View3D)
                paneW = ui->vtkCube;
            else if (k == ViewId::Product) {
                // A Product slot renders through its Pane2DView (image), its ProfileWidget
                // (spectrum, #5), or the shared PvDiagramWidget (PV) — match whichever it uses.
                if (m_products1d.contains(m_slots[i].view.productId) && m_paneSpectra[i] && rw
                    && (rw == m_paneSpectra[i] || m_paneSpectra[i]->isAncestorOf(rw)))
                    paneW = rw;   // click landed on the spectrum widget or its child QCustomPlot
                else if (!m_pvProductId.isEmpty() && m_slots[i].view.productId == m_pvProductId
                         && pvDiagramWidget && rw
                         && (rw == pvDiagramWidget || pvDiagramWidget->isAncestorOf(rw)))
                    paneW = rw;   // click landed on the PV widget or one of its children
                else if (m_paneViews[i])
                    paneW = m_paneViews[i]->widget();
            }
            // §5.1b: the 3D view's render surface is EITHER the local VTK widget (ui->vtkCube)
            // or the remote widget (server stream) depending on render mode — activation +
            // swallow must recognise both.
            const bool is3dRemote = (k == ViewId::View3D && m_remoteRenderWidget
                                     && rw == m_remoteRenderWidget);
            if (((paneW && paneW == rw) || is3dRemote) && m_slots[i].frame
                && m_slots[i].frame->isVisible()) {
                renderSlot = i;
                break;
            }
        }
    if (renderSlot >= 0) {
        // A pane showing the view an armed tool listens to keeps its input even
        // when the focus is elsewhere. Without this, selecting another pane — to
        // reach its controls in the Inspector, say — made the armed tool inert
        // with no sign of it: the amber border still said "listening here" while
        // every hover and click was being swallowed.
        bool nonActive = (m_activePane != renderSlot) && !paneIsArmedTarget(renderSlot);
        // A CONTROL inside a pane (the PV diagram's Save as FITS…, its width box, a
        // spectrum's buttons) works on the first click: it activates the pane and the
        // click goes through. Swallowing it made every button need two clicks.
        const bool onControl = qobject_cast<QAbstractButton *>(rw) || qobject_cast<QAbstractSpinBox *>(rw)
                || qobject_cast<QComboBox *>(rw) || qobject_cast<QLineEdit *>(rw)
                || (rw && rw->parentWidget()
                    && (qobject_cast<QAbstractSpinBox *>(rw->parentWidget())
                        || qobject_cast<QComboBox *>(rw->parentWidget())));
        if (onControl) {
            // Activate AFTER the click (on the release, deferred): done during the
            // press, the pane change re-lays the chrome out and the button loses
            // the mouse before its release — it then never emits clicked().
            if (et == QEvent::MouseButtonPress && nonActive)
                m_pendingControlActivation = renderSlot;
            if (et == QEvent::MouseButtonRelease && m_pendingControlActivation == renderSlot) {
                const int slot = renderSlot;
                m_pendingControlActivation = -1;
                QTimer::singleShot(0, this, [this, slot]() { setActivePane(slot); });
            }
            m_paneActivationGesture = nullptr;
            nonActive = false;
        }
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

ProfileWidget *vtkWindowCube::ensureSlotSpectrumView(int slot, const QString &xLabel,
                                                     const QString &yLabel)
{
    // #5: a per-slot, display-only ProfileWidget for a 1-D spectrum product (independent of
    // the shared probePlotWidget, which the live probe / region owns). Instantiated lazily,
    // one per slot; setupSpectrumPlot runs ONCE here (it insertRow(0)s a title element, so
    // re-running it per mount would accumulate rows).
    if (slot < 0 || slot > 3)
        return nullptr;
    if (!m_paneSpectra[slot]) {
        auto *sp = new ProfileWidget(m_viewParkingLot);
        // The ProfileWidget ctor sets Qt::Window + WA_DeleteOnClose (standalone dialog). For
        // an EMBEDDED pane widget clear both, so it never pops a transient top-level window
        // and a stray close can't delete the cached pointer.
        sp->setWindowFlags(Qt::Widget);
        sp->setAttribute(Qt::WA_DeleteOnClose, false);
        sp->setUsageMode(ProfileWidget::UsageMode::RegionStatic);   // static, non-live display
        sp->setupSpectrumPlot(xLabel, yLabel);
        sp->installEventFilter(this);   // click-to-activate (§5.1)
        // Qt event filters are NOT recursive: a click lands on the child QCustomPlot, so filter
        // the descendants too or a non-active spectrum pane's body won't click-to-activate — then
        // the Inspector's Spectrum controls would target the wrong pane. Built by
        // setupSpectrumPlot above, so the children exist now.
        const auto spKids = sp->findChildren<QWidget *>();
        for (QWidget *kid : spKids)
            kid->installEventFilter(this);
        // The spectrum controls now live in Inspector › Analysis (they drove this widget's
        // clutter); hide the inline chrome and give it the backend so the Inspector's Fit works.
        sp->setPaneChromeHidden(true);
        sp->setBackend(this->remoteBackendUrl, this->remoteBackendToken);
        // NOT the axis context here: this slot's FIRST product may be on another
        // axis (a channel-axis stack), and pushing the cube's CTYPE / unit /
        // RESTFRQ unconditionally left "Load Lines…" converting to velocity and
        // drawing the numbers as channel indices. mountView decides,
        // both ways, on every mount.
        // Keep the Inspector's Spectrum panel live when THIS pane is the active target.
        QObject::connect(sp, &ProfileWidget::spectrumChanged, this, [this, sp]() {
            if (m_specInspTarget == sp)
                updateSpectrumInspectorControls();
        });
        m_paneSpectra[slot] = sp;
    }
    return m_paneSpectra[slot];
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
        ctx.formatCoord = [this](int axis, double value, double axisSpan) {
            return this->formatRemoteOverlayCoordinate(axis, value, axisSpan);
        };
        // The same paired frame conversion as the 2D Slice's ticks: without it the
        // pane printed native RA/Dec under a "Galactic" title.
        ctx.voxelToFrame = [this](double vx, double vy, double &fx, double &fy) {
            bool okX = false, okY = false;
            const double nx = this->remoteVoxelToWcs(0, vx, &okX);
            const double ny = this->remoteVoxelToWcs(1, vy, &okY);
            return okX && okY && this->convertRemoteCelestialCoordinates(nx, ny, fx, fy);
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
            refreshPaneLutStrip(slot);            // §5.4: keep this pane's header strip current
            if (m_applyingPaneDisplay)
                return;
            propagatePaneLutWithinWindow(slot);   // colormap/scale/invert → same-product panes
        });
        // A refused log stretch has to say so: the pane keeps its current scale,
        // and without this the control would simply snap back with no explanation.
        QObject::connect(pv, &Pane2DView::logScaleUnavailable, this, [this, slot]() {
            this->statusBar()->showMessage(
                    u"Log scale needs positive values, and this image has none."_s, 4000);
            refreshPaneLutStrip(slot);
            // And put the DISPLAY section back on the scale still in force.
            // refreshPaneLutStrip() only touches the header strip's colormap
            // button; without this the segmented control sits on Log while the
            // LUT is still linear, which is the one state the control must never
            // show.
            if (slot == m_activePane)
                updateDisplaySectionForActivePane();
        });
        // Linked 2D: a pan/zoom on this pane syncs the same-WCS image panes.
        QObject::connect(pv, &Pane2DView::cameraChanged, this,
                         [this, slot]() { propagate2DCamera(slot); });
        // STEP6-P2: after a fit() re-letterboxes this pane, re-adopt the shared Linked-2D camera
        // (if any) so range/LUT/resize doesn't break the shared zoom.
        QObject::connect(pv, &Pane2DView::fitted, this,
                         [this, slot]() { reassertLinked2DCamera(slot); });
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
        auto it1 = m_products1d.constFind(v.productId);
        if (it1 != m_products1d.constEnd())
            return it1->label;
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
        // Renderable AND actually cached in THIS window (image in m_products2d, spectrum in
        // m_products1d, or the PV diagram). The registry is session-wide, so a same-dataset
        // sibling window could register a product this window never cached — offering it would
        // mount a blank pane. isPaneRenderableProduct() is the same predicate
        // mountView/activateSessionView use.
        if (!p.renderer.isEmpty() && isPaneRenderableProduct(p.productId))
            out << PaneView{ ViewId::Product, p.productId };
    // NO fixed "Spectrum" entry. It was a placeholder for a RESULT, not a view of
    // the dataset: selectable at any time, empty until some tool happened to fill
    // it, and duplicating the SPEC products that already appear above the moment a
    // spectrum exists. A spectrum now shows up the way every other result does —
    // as a product, placed in a pane by the tool that computed it (user request).
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
    case ViewId::Product:  return m_products2d.contains(v.productId)
                                || m_products1d.contains(v.productId)   // image OR spectrum
                                || (!m_pvProductId.isEmpty() && v.productId == m_pvProductId
                                    && pvDiagramWidget);                  // OR PV diagram
    default:               return false;
    }
}

bool vtkWindowCube::isPaneRenderableProduct(const QString &productId) const
{
    return m_products2d.contains(productId) || m_products1d.contains(productId)
            || (!m_pvProductId.isEmpty() && productId == m_pvProductId && pvDiagramWidget);
}

QString vtkWindowCube::productRenderer(const QString &productId) const
{
    if (m_products1d.contains(productId))
        return u"spectrum1d"_s;
    if (m_products2d.contains(productId))
        return u"image2d"_s;
    if (!m_pvProductId.isEmpty() && productId == m_pvProductId)
        return u"pvdiagram"_s;
    return QString();
}

QString vtkWindowCube::addImageProduct(const QString &kind, const QString &stableKey,
                                       const QString &label, vtkImageData *img,
                                       const QString &unit, const QVariantMap &params,
                                       const QString &lutName, bool symmetricRange)
{
    if (!img)
        return QString();
    const QString productId = this->scopedProductId(stableKey);
    const bool recompute = m_products2d.contains(productId);

    Product2D cached;
    cached.imageData = img;
    // NaN-safe display range (generator maps have masked pixels) — reuse the stats helper.
    const MapStats st = computeMapStats(img);
    if (st.valid)
        cached.imageRange = { st.min, st.max };
    else {
        double range[2] = { 0., 1. };
        img->GetScalarRange(range);
        cached.imageRange = { range[0], range[1] };
    }
    if (symmetricRange) {
        double a = std::max(std::abs(cached.imageRange[0]), std::abs(cached.imageRange[1]));
        if (!(a > 0.0))
            a = 1.0;   // an all-zero signed map: zero still sits on the neutral colour
        cached.imageRange = { -a, a };
    }
    // A constant / degenerate map gives lo==hi → a broken LUT range; widen it.
    if (!(cached.imageRange[1] > cached.imageRange[0]))
        cached.imageRange[1] = cached.imageRange[0] + 1.0;
    cached.stats = st;
    cached.statsComputed = true;
    cached.lutName = lutName.isEmpty() ? u"Inferno"_s : lutName;
    cached.scaleMode = LutScale::Mode::Linear;
    cached.gamma = this->m_sliceScaleGamma;
    cached.label = label;
    cached.unit = unit;
    m_products2d.insert(productId, cached);   // replaces on recompute (statsComputed resets)
    if (productId == this->m_sliceBackdropProductId && cached.imageData) {
        // The slice is showing this map as its background: show the new pixels.
        this->remoteSliceDisplaySource->SetOutput(cached.imageData);
        this->remoteSliceDisplaySource->Modified();
        this->sliceColors->SetInputData(cached.imageData);
        if (this->sliceWin)
            this->sliceWin->Render();
    }
    this->registerAnalysisProduct(kind, label, params, this, productId, u"image2d"_s);

    auto visible = [&](int i) { return m_slots[i].frame && m_slots[i].frame->isVisible(); };
    int target = -1;
    if (recompute) {
        // Refresh EVERY visible pane already showing it (duplicated copies), keeping each
        // pane's own LUT (mountView's same-product path).
        for (int i = 0; i < 4; ++i)
            if (visible(i) && m_slots[i].view == PaneView{ ViewId::Product, productId }) {
                mountView(i);
                if (target < 0)
                    target = i;
            }
    }
    if (target < 0)   // first run (or no visible copy): drop into the first free pane, no-steal
        for (int i = 0; i < 4; ++i)
            if (visible(i) && m_slots[i].view.isNone()) {
                m_slots[i].view = PaneView{ ViewId::Product, productId };
                mountView(i);
                target = i;
                break;
            }
    if (target >= 0)
        setActivePane(target);
    refreshPaneChrome();
    this->statusBar()->showMessage(QStringLiteral("%1 ready.").arg(label), 4000);
    return productId;
}

void vtkWindowCube::pinCurrentSpectrum()
{
    // #5: promote the current probe spectrum to a first-class 1-D product — it appears in the
    // Session tree (SPEC), is offered in every pane dropdown, is duplicable, and lands in a
    // free pane if one is open (else waits in the tree for manual activation).
    if (m_lastProbeValues.isEmpty()) {
        this->statusBar()->showMessage(
                u"Pin Spectrum: probe a pixel in the 2D view first."_s, 3000);
        return;
    }
    const int n = ++m_pinnedSpectrumCount;
    const QString productId = this->scopedProductId(QStringLiteral("spectrum%1").arg(n));
    const QString label = QStringLiteral("Spectrum %1").arg(n);
    m_lastProbeSpectrumPinned = true;   // this curve is a product now; do not promote it twice
    m_lastProbeSpectrumProduct = label;   // …and the pane says so, by name
    Spectrum1D s;
    s.key = m_lastProbeKey;
    s.values = m_lastProbeValues;
    s.xLabel = m_lastProbeXLabel;
    s.yLabel = m_lastProbeYLabel;
    s.label = label;
    s.pixelX = m_lastProbeVoxel[0];
    s.pixelY = m_lastProbeVoxel[1];
    s.position = m_lastProbeContext;
    m_products1d.insert(productId, s);

    QVariantMap params;
    if (!m_lastProbeContext.isEmpty())
        params.insert(u"Position"_s, m_lastProbeContext);
    params.insert(u"Channels"_s, QString::number(m_lastProbeKey.size()));
    // The pixel it was probed at travels with it: "which line of sight is this?"
    // is the first thing asked of a pinned spectrum, and the Position string
    // alone cannot put the crosshair back on the slice.
    const QVariantMap origin = visivo::encodeProbeOrigin(m_lastProbeVoxel);
    params.insert(u"Measured on"_s, visivo::describeOrigin(origin));
    this->registerAnalysisProduct(u"spectrum"_s, label, params, this, productId,
                                  u"spectrum1d"_s, /*revealProvenance=*/true, origin);

    // A pin is PERMANENT, so it may grow the layout but never borrow: a borrowed
    // pane is given back when a tool ends, and this product has no end.
    const int target = acquirePaneForProduct(productId, /*mayBorrow=*/false,
                                             /*avoidActivePane=*/false);
    if (target >= 0)
        setActivePane(target);
    refreshPaneChrome();
    // Where it went, always — a pinned spectrum that lands in no pane used to be
    // announced as "Pinned Spectrum 1." with nothing visibly happening.
    this->statusBar()->showMessage(
            target >= 0
                    ? QStringLiteral("Pinned %1 into pane %2.").arg(label).arg(target + 1)
                    : QStringLiteral("Pinned %1 - no free pane: pick it from a pane's "
                                     "\u25BE menu or from Session Data.").arg(label),
            6000);
}

QString vtkWindowCube::addSpectrumProduct(const QString &kind, const QString &stableKey,
                                          const QString &label, const QVector<double> &key,
                                          const QVector<double> &values, const QString &xLabel,
                                          const QString &yLabel, const QVariantMap &params)
{
    // #5: the spectrum1d twin of addImageProduct — promote a 1-D generator result (Stack, …)
    // to a first-class SPEC product: cache it in m_products1d, register it (renderer
    // "spectrum1d"), and mount it in a free pane (no-steal), or refresh every visible copy on
    // a recompute of the same stableKey. Reuse this for any tool that yields a 1-D spectrum.
    if (values.isEmpty())
        return QString();
    const QString productId = this->scopedProductId(stableKey);
    const bool recompute = m_products1d.contains(productId);

    Spectrum1D s;
    s.key = key;
    s.values = values;
    s.xLabel = xLabel;
    s.yLabel = yLabel;
    s.label = label;
    m_products1d.insert(productId, s);   // replaces on recompute
    this->registerAnalysisProduct(kind, label, params, this, productId, u"spectrum1d"_s);

    auto visible = [&](int i) { return m_slots[i].frame && m_slots[i].frame->isVisible(); };
    int target = -1;
    if (recompute) {
        // Refresh EVERY visible pane already showing it (spectra re-load on every mount).
        for (int i = 0; i < 4; ++i)
            if (visible(i) && m_slots[i].view == PaneView{ ViewId::Product, productId }) {
                mountView(i);
                if (target < 0)
                    target = i;
            }
    }
    if (target < 0)   // first run (or no visible copy): drop into the first free pane, no-steal
        for (int i = 0; i < 4; ++i)
            if (visible(i) && m_slots[i].view.isNone()) {
                m_slots[i].view = PaneView{ ViewId::Product, productId };
                mountView(i);
                target = i;
                break;
            }
    if (target >= 0)
        setActivePane(target);
    refreshPaneChrome();
    this->statusBar()->showMessage(QStringLiteral("%1 ready.").arg(label), 4000);
    return productId;
}

bool vtkWindowCube::viewAssigned(ViewId kind) const
{
    // Assigned to a slot, visible or not. Only used to tell "hidden by a maximize"
    // apart from "not in the layout at all": a parked view (a 4→1 collapse keeps
    // the other slots' views) must NOT count as showable, or maximizing any pane
    // would let a tool arm against a view nobody can see.
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == kind)
            return true;
    return false;
}

bool vtkWindowCube::slice2dVisible() const
{
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Slice2D && m_slots[i].frame
            && m_slots[i].frame->isVisible())
            return true;
    return false;
}

bool vtkWindowCube::view3dVisible() const
{
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::View3D && m_slots[i].frame
            && m_slots[i].frame->isVisible())
            return true;
    return false;
}

bool vtkWindowCube::anyTool2dArmed() const
{
    const auto armed = [](const QAction *a) { return a && a->isChecked(); };
    return armed(ui->actionExtractSpectrum) || armed(actionExtractPvDiagram.data())
            || armed(actionBoxRegion.data()) || armed(actionCircleRegion.data())
            || armed(actionPolygonRegion.data()) || armed(actionAnnulusRegion.data());
}

bool vtkWindowCube::anyTool3dArmed() const
{
    const auto armed = [](const QAction *a) { return a && a->isChecked(); };
    return armed(m_actKinematicLasso.data()) || armed(actionPickSpectrum3d.data());
}

bool vtkWindowCube::paneIsArmedTarget(int slot) const
{
    if (slot < 0 || slot > 3)
        return false;
    const ViewId k = m_slots[slot].view.kind;
    if (k == ViewId::Slice2D)
        return anyTool2dArmed();
    if (k == ViewId::View3D)
        return anyTool3dArmed();
    return false;
}

bool vtkWindowCube::disarmArmedTools()
{
    bool any = false;
    for (QAction *a : { ui->actionExtractSpectrum, actionExtractPvDiagram.data(),
                        actionBoxRegion.data(), actionCircleRegion.data(),
                        actionPolygonRegion.data(), actionAnnulusRegion.data(),
                        m_actKinematicLasso.data(), actionPickSpectrum3d.data() }) {
        if (a && a->isChecked()) {
            a->setChecked(false);   // its own toggled() handler tears the tool down
            any = true;
        }
    }
    if (any)
        this->statusBar()->showMessage(u"Tool disarmed."_s, 2000);
    return any;
}

int vtkWindowCube::acquirePaneForProduct(const QString &productId, bool mayBorrow,
                                         bool avoidActivePane)
{
    // A result the user asked for has to become VISIBLE. Placing it only "if a pane
    // happens to be free" meant that in a full layout the tool answered with a row
    // in Session Data and a status line, which reads as the tool doing nothing.
    if (productId.isEmpty())
        return -1;
    auto visible = [this](int i) { return m_slots[i].frame && m_slots[i].frame->isVisible(); };
    for (int i = 0; i < 4; ++i)
        if (visible(i) && m_slots[i].view.kind == ViewId::Product
            && m_slots[i].view.productId == productId)
            return i;   // already on screen
    // For a live preview, not the active pane: that is the slice being hovered.
    // For a finished product the exclusion is wrong — the pane the tool just
    // vacated is usually the active one, and skipping it sent the kept spectrum
    // into Session Data with an empty pane sitting right there.
    auto firstFree = [&]() {
        for (int i = 0; i < 4; ++i)
            if ((!avoidActivePane || i != m_activePane) && visible(i)
                && m_slots[i].view.isNone())
                return i;
        return -1;
    };
    int slot = firstFree();
    // Grow before taking anything: 1 → 2 → 4. assignViews() auto-fills only the
    // cube's intrinsic views (3-D, 2-D slice) and only when they are not already on
    // screen, so a grown pane normally comes up free; when it does not (the slice
    // lands in it), grow once more.
    while (slot < 0 && m_paneCount < 4) {
        applyPaneLayout(m_paneCount == 1 ? 2 : 4);
        slot = firstFree();
    }
    if (slot < 0 && mayBorrow)
        slot = borrowPaneSlot(productId);
    if (slot < 0)
        return -1;
    m_slots[slot].view = PaneView{ ViewId::Product, productId };
    mountView(slot);
    return slot;
}

int vtkWindowCube::borrowPaneSlot(const QString &productId)
{
    auto visible = [this](int i) { return m_slots[i].frame && m_slots[i].frame->isVisible(); };
    auto duplicateOnScreen = [&](int i) {
        for (int j = 0; j < 4; ++j)
            if (j != i && visible(j) && m_slots[j].view == m_slots[i].view)
                return true;
        return false;
    };
    int best = -1, bestScore = 0;
    for (int i = 0; i < 4; ++i) {
        if (!visible(i) || i == m_activePane)
            continue;
        if (m_slots[i].view.kind == ViewId::Slice2D)
            continue;   // the source of every 2-D tool — taking it breaks the tool
        if (paneIsLent(i))
            continue;   // already lent out to something else
        // Cheapest to displace first: a duplicate of a view that stays on screen
        // anyway, then a derived product (recomputable, still in Session Data),
        // then a primary view like the 3-D render.
        const int score = duplicateOnScreen(i)                    ? 3
                        : (m_slots[i].view.kind == ViewId::Product) ? 2
                                                                    : 1;
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best < 0)
        return -1;
    m_borrowedPanes.insert(productId, m_slots[best].view);
    this->statusBar()->showMessage(
            u"Borrowed pane %1 (was %2) (it comes back when the tool ends)."_s
                    .arg(best + 1)
                    .arg(viewTitle(m_slots[best].view)),
            6000);
    return best;
}

bool vtkWindowCube::paneIsLent(int slot) const
{
    return slot >= 0 && slot <= 3 && m_slots[slot].view.kind == ViewId::Product
            && m_borrowedPanes.contains(m_slots[slot].view.productId);
}

void vtkWindowCube::releaseBorrowedPane(const QString &productId)
{
    const auto it = m_borrowedPanes.constFind(productId);
    if (it == m_borrowedPanes.constEnd())
        return;
    const PaneView displaced = it.value();
    m_borrowedPanes.remove(productId);
    // Find the pane by PRODUCT, not by the slot it was lent from: a 4→1 collapse
    // swaps views between slots.
    for (int slot = 0; slot < 4; ++slot) {
        if (m_slots[slot].view.kind != ViewId::Product
            || m_slots[slot].view.productId != productId)
            continue;
        // Never hand back a view that has meanwhile been mounted elsewhere — the
        // fixed views are singleton widgets, so two slots claiming one leaves the
        // other blank.
        bool elsewhere = false;
        for (int j = 0; j < 4; ++j)
            if (j != slot && !displaced.isNone() && m_slots[j].view == displaced)
                elsewhere = true;
        m_slots[slot].view = elsewhere ? PaneView{} : displaced;
        mountView(slot);
        return;
    }
}

void vtkWindowCube::startLiveSpectrum()
{
    // #5b: the live probe preview lives in a PANE, not a floating window. Create a transitory
    // "Spectrum (live)" product and place it in the FIRST free pane; if none, the active pane
    // UNLESS it's the source 2D Slice (never steal what the user is hovering) — then hint.
    if (!m_spectrumLiveId.isEmpty())
        return;
    m_spectrumLiveId = this->scopedProductId(u"spectrumlive"_s);
    Spectrum1D s;
    s.xLabel = this->spectralAxisTitle().isEmpty() ? u"Spectral axis"_s : this->spectralAxisTitle();
    s.yLabel = u"Intensity"_s;
    s.label = u"Spectrum (live)"_s;
    m_products1d.insert(m_spectrumLiveId, s);
    // No Provenance reveal: this product exists because the user just ARMED a tool
    // in Inspector ▸ Analysis, and jumping the panel to an empty Provenance page
    // ("No recorded parameters") took the tool list out from under them.
    this->registerAnalysisProduct(u"spectrum"_s, s.label, QVariantMap{}, this, m_spectrumLiveId,
                                  u"spectrum1d"_s, /*revealProvenance=*/false);
    ProductRegistry::instance().setStatus(m_spectrumLiveId, u"live"_s);   // italic/muted in tree

    // The live preview is transient, so it MAY borrow a pane and give it back when
    // the tool ends — the one placement that is allowed to displace something.
    const int target = acquirePaneForProduct(m_spectrumLiveId, /*mayBorrow=*/true);
    if (target >= 0) {
        // NOT setActivePane — the source pane keeps focus for hover.
        this->updateLiveSpectrumHeader();   // mountView writes the product label; this the state
    } else {
        this->statusBar()->showMessage(
                u"Extract Spectrum: no pane could be freed for the live spectrum - "
                u"close a pane's view, or pick \u201CSpectrum (live)\u201D from a pane's "
                u"\u25BE menu."_s,
                6000);
    }
    refreshPaneChrome();
}

void vtkWindowCube::stopLiveSpectrum()
{
    if (m_spectrumLiveId.isEmpty())
        return;
    const QString id = m_spectrumLiveId;
    m_spectrumLiveId.clear();
    // A spectrum the user PICKED (clicked a pixel, so the probe is frozen) is a
    // result and survives the tool: it is promoted to a permanent SPEC product,
    // there in Session Data to come back to. A spectrum that was still tracking
    // the cursor is a preview and goes with the tool — otherwise every arm/disarm
    // would leave a row nobody asked for.
    const bool promote = this->probeFrozen && !m_lastProbeValues.isEmpty()
            && !m_lastProbeSpectrumPinned;
    m_products1d.remove(id);
    ProductRegistry::instance().removeProduct(id);   // drops the transitory tree entry
    // A borrowed pane goes back to what it was showing (this also un-mounts the
    // live product from it).
    releaseBorrowedPane(id);
    // Any pane still showing the live spectrum (i.e. not a borrowed one) returns
    // to "Pick a view" — the product is gone.
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product && m_slots[i].view.productId == id) {
            m_slots[i].view = {};
            mountView(i);
        }
    // AFTER the live product is gone, so the pane it just vacated is the free one
    // the promoted product lands in (pinCurrentSpectrum registers it from
    // m_lastProbe*, with the pixel in its Provenance, and places it).
    if (promote)
        this->pinCurrentSpectrum();
    refreshPaneChrome();
}

void vtkWindowCube::removeImageProduct(const QString &stableKey)
{
    const QString id = this->scopedProductId(stableKey);
    if (!m_products2d.contains(id) && !m_products1d.contains(id))
        return;
    // The slice may be wearing this map as its background: give the slice its
    // channels back first, or it keeps showing a product that no longer exists.
    if (id == this->m_sliceBackdropProductId)
        this->setSliceBackdrop(QString());
    m_products2d.remove(id);
    m_products1d.remove(id);
    ProductRegistry::instance().removeProduct(id);
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product && m_slots[i].view.productId == id) {
            m_slots[i].view = {};
            mountView(i);
        }
    refreshPaneChrome();
}

void vtkWindowCube::updateLiveSpectrumPane(const QVector<double> &key,
                                           const QVector<double> &values,
                                           const QString &pixelLabel)
{
    // #5b: push the current probe spectrum into the live product + its pane (real-time on
    // hover), unless frozen. The pane header's qualifier shows the current pixel.
    if (m_spectrumLiveId.isEmpty() || this->probeFrozen)
        return;
    Spectrum1D &s = m_products1d[m_spectrumLiveId];
    s.key = key;
    s.values = values;
    const bool labelsChanged = s.xLabel != m_lastProbeXLabel || s.yLabel != m_lastProbeYLabel;
    s.xLabel = m_lastProbeXLabel;
    s.yLabel = m_lastProbeYLabel;
    s.pixelX = m_lastProbeVoxel[0];
    s.pixelY = m_lastProbeVoxel[1];
    s.position = m_lastProbeContext;
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product && m_slots[i].view.productId == m_spectrumLiveId
            && m_paneSpectra[i]) {
            if (labelsChanged)
                m_paneSpectra[i]->setSpectrumAxisLabels(s.xLabel, s.yLabel);
            m_paneSpectra[i]->updateSpectrumPlot(key, values, u"Spectrum (live)"_s, /*live=*/true);
            m_paneSpectra[i]->setSpectrumContext(this->filepath, s.pixelX, s.pixelY, s.position);
        }
    Q_UNUSED(pixelLabel);   // the pane header owns the pixel line (updateLiveSpectrumHeader)
    this->updateLiveSpectrumHeader();
}

void vtkWindowCube::updateLiveSpectrumHeader()
{
    if (m_spectrumLiveId.isEmpty())
        return;
    const QString px = u"pixel (%1, %2)"_s.arg(this->probeVoxel[0]).arg(this->probeVoxel[1]);
    QStringList parts{ u"Spectrum (live)"_s, px };
    if (!m_lastProbeContext.isEmpty())
        parts << m_lastProbeContext;
    // A pane's ProfileWidget runs in RegionStatic mode (its Live/Pinned pill and
    // click-to-pin hint are suppressed by design), so the tool's state and the
    // ways OUT of it go in the meta line — otherwise the whole interaction is
    // undiscoverable: hover tracks, one click pins, a second click resumes, and
    // nothing on screen said any of that, so the tool read as "never finishes".
    // Three states, each with its own next step — the one that was missing is
    // "already saved": ending the tool then removes the live row and adds
    // nothing, which read as losing the spectrum rather than as it being safe.
    const QString hint = !this->probeFrozen
            ? u"\U0001F7E2 Live - hover the slice to track, click a pixel to pin its spectrum"_s
            : m_lastProbeSpectrumPinned
            ? u"\U0001F4BE Saved as \u201C%1\u201D in Session Data  ·  click the slice "
              u"again to resume tracking"_s.arg(m_lastProbeSpectrumProduct)
            : u"\U0001F512 Pinned - click the slice again to resume tracking  ·  "
              u"ending the tool (Extract Spectrum / Esc) keeps this spectrum in "
              u"Session Data  ·  \u201CPin Spectrum\u201D keeps it now"_s;
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].view.kind == ViewId::Product
            && m_slots[i].view.productId == m_spectrumLiveId && m_paneSpectra[i]) {
            // The meta line is set for a standalone ProfileWidget only — an EMBEDDED
            // pane one runs with setPaneChromeHidden(true), which hides m_specHeader
            // and with it that label. The visible surface is the pane's own header
            // row, so the state chip goes in its qualifier (the instructions are in
            // the status bar, where there is room for them).
            m_paneSpectra[i]->setSpectrumMeta(parts.join(u"  \u00B7  "_s), hint);
            if (m_slots[i].qualifier)
                m_slots[i].qualifier->setText(
                        !this->probeFrozen
                                ? px + u"  \u00B7  \U0001F7E2 live"_s
                                : m_lastProbeSpectrumPinned
                                ? px + u"  \u00B7  \U0001F4BE saved as %1"_s
                                          .arg(m_lastProbeSpectrumProduct)
                                : px + u"  \u00B7  \U0001F512 pinned, click again to resume"_s);
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
    const PaneView keep = m_slots[keepSlot].view;
    // The PV diagram is a SINGLE reused widget (unlike per-slot images/spectra), so it cannot
    // live in two panes — dedup it by product id, treating it like a singular view.
    if (keep.kind == ViewId::Product && !m_pvProductId.isEmpty()
        && keep.productId == m_pvProductId) {
        for (int j = 0; j < 4; ++j)
            if (j != keepSlot && m_slots[j].view == keep) {
                m_slots[j].view = {};
                // Unlike the per-slot image/spectrum dedup (widget stays put), the SINGLE PV
                // widget will be reparented to keepSlot, so this slot's host would be left
                // blank — remount it now to show its None placeholder.
                mountView(j);
            }
        return;
    }
    QWidget *w = viewWidget(keep);
    if (!w)
        return;   // None / not-yet-created Spectrum: nothing to deduplicate
    for (int j = 0; j < 4; ++j)
        if (j != keepSlot && viewWidget(m_slots[j].view) == w) {
            m_slots[j].view = {};
            // The singular widget is about to be reparented into keepSlot, so
            // this slot's host is left empty — remount it to show its "Pick a
            // view" placeholder. Harmless before "Show in ▸" learned to move a
            // fixed view (only products reached here, and they return above),
            // and a blank pane the moment it did.
            mountView(j);
        }
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

        // §5.4: the LUT strip is a real button — a 30×9 swatch of THIS pane's colormap.
        // Click activates the pane and opens the colormap popover (list + Edit/Invert/Reset).
        // Shown only on 2-D LUT panes (Product / Slice); hidden for 3D / Spectrum / None.
        auto *lutBtn = new QToolButton(hdr);
        lutBtn->setCursor(Qt::PointingHandCursor);
        lutBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        lutBtn->setIconSize(QSize(30, 9));
        lutBtn->setToolTip(u"Colour map - click to change"_s);
        lutBtn->setStyleSheet(u"QToolButton{background:transparent;border:1px solid #2A3238;"
                              u"border-radius:2px;padding:1px;}QToolButton:hover{border-color:%1;}"_s
                                      .arg(pri));
        lutBtn->setVisible(false);
        QObject::connect(lutBtn, &QToolButton::clicked, this,
                         [this, i]() { this->showPaneLutStripPopover(i); });
        h->addWidget(lutBtn);

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

        // Pop-out: detach this pane into its own window (for a second screen). Re-attach with
        // the same button (⤴ / ⤵), the window close, or the placeholder's Bring back.
        auto *detachBtn = new QToolButton(hdr);
        detachBtn->setText(u"⤴"_s);
        detachBtn->setCursor(Qt::PointingHandCursor);
        detachBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        detachBtn->setToolTip(u"Pop out to a window"_s);
        detachBtn->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;"
                                                "color:%1;font-size:13px;}QToolButton:hover{color:%2;}")
                                         .arg(fgMuted, pri));
        QObject::connect(detachBtn, &QToolButton::clicked, this, [this, i]() {
            if (paneDetached(i))
                bringBackPane(i);
            else
                detachPane(i);
        });
        h->addWidget(detachBtn);
        m_slots[i].detachBtn = detachBtn;

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
        m_slots[i].lutBtn = lutBtn;
        m_slots[i].view = {};
    };
    for (int i = 0; i < 4; ++i)
        makeSlot(i);

    // Esc restores the layout when a pane is maximized (§3.2). Enabled ONLY while
    // maximized, so an idle Esc never steals the key from a focused child / dialog.
    m_escRestoreShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_escRestoreShortcut->setEnabled(false);
    QObject::connect(m_escRestoreShortcut, &QShortcut::activated, this, [this]() {
        // An armed tool first: it is the more recent, more modal state, and the
        // amber border it paints is the thing the user is trying to get rid of.
        if (disarmArmedTools())
            return;
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
    if (v.kind == ViewId::Product && viewHasData(v) && m_products1d.contains(v.productId)) {
        // #5: a 1-D spectrum product renders into the slot's OWN ProfileWidget (like a moment
        // uses its own Pane2DView) — so two panes can show two different spectra, or the same
        // one, independently. Spectra are small → just re-load on every mount.
        const Spectrum1D &s = m_products1d[v.productId];
        ProfileWidget *sp = ensureSlotSpectrumView(slot, s.xLabel, s.yLabel);
        // A slot's ProfileWidget is created ONCE (with the first product's axes/header). When it
        // is reused for a DIFFERENT product, refresh the axis titles + header so a channel-axis
        // stack after a velocity-axis pin doesn't keep stale labels / "Region spectrum".
        sp->setSpectrumAxisLabels(s.xLabel, s.yLabel);
        // The spectral-axis context follows the PRODUCT's axis, and is set both
        // ways on every mount: supplied for a product on the cube's own axis
        // (probe / region / live spectrum), explicitly cleared for one on another
        // axis (a channel-axis stack), so a line list is refused there instead of
        // being placed with the cube's velocity numbers. Setting it only one way
        // left a re-mount of the same channel product carrying stale context
        // (setSpectrumAxisLabels sees no label change, so it clears nothing).
        if (s.xLabel.isEmpty() || s.xLabel == spectralAxisTitle())
            pushSpectralAxisContext(sp);
        else
            sp->setSpectralAxisContext(QString(), QString(), 0.0);
        sp->setSpectrumMeta(s.label.isEmpty() ? QString() : s.label);
        // What the CSV export writes as provenance: the dataset, and the line of
        // sight when there is one (a region or a stack has none).
        sp->setSpectrumContext(this->filepath, s.pixelX, s.pixelY, s.position);
        // …except for the LIVE product, whose header carries the tool's state and
        // the ways out of it. Every remount (a pane swap, a layout change, a
        // 4→1 collapse) would otherwise reduce it back to "Spectrum (live)" —
        // and while the probe is frozen no hover comes to write it again.
        const bool isLiveProduct = !m_spectrumLiveId.isEmpty() && v.productId == m_spectrumLiveId;
        // Call unconditionally: an EMPTY live product clears the plot (so a slot reused from a
        // previous spectrum doesn't show stale data until the first hover).
        sp->updateSpectrumPlot(s.key, s.values, s.label, /*live=*/false);
        m_paneProductId[slot] = v.productId;
        w = sp;
        if (isLiveProduct) {
            // After the assignment below, so the header helper finds this slot
            // mounted; deferred because mountView() is mid-flight.
            QMetaObject::invokeMethod(this, [this]() { this->updateLiveSpectrumHeader(); },
                                      Qt::QueuedConnection);
        }
    } else if (v.kind == ViewId::Product && !m_pvProductId.isEmpty()
               && v.productId == m_pvProductId && pvDiagramWidget) {
        // #5: the PV diagram is hosted IN the pane (the SINGLE reused widget — PV is not
        // duplicable, matching its former single-window model). It carries its own
        // position/velocity axes + header, so we embed it wholesale (like spectrum1d embeds
        // ProfileWidget). Embed flags are cleared in the fetch handler; nothing to re-load here
        // — setPvData already filled it.
        m_paneProductId[slot] = v.productId;
        w = pvDiagramWidget;
    } else if (v.kind == ViewId::Product && viewHasData(v)) {
        pv = ensureSlotProductView(slot);
        const Product2D &p = m_products2d[v.productId];
        // Seed the by-kind default on a FIRST mount OR when this slot is reused for a
        // DIFFERENT product; keep the pane's own LUT/range only on a same-product recompute
        // (lutInitialized alone can't tell those apart — it's per-slot, not per-product).
        const bool sameProduct = pv->lutInitialized() && (m_paneProductId[slot] == v.productId);
        if (!sameProduct) {
            pv->setImageData(p.imageData, p.imageRange[0], p.imageRange[1], p.scaleMode);
            pv->setColorMap(p.lutName, p.scaleMode, p.gamma, p.clampOutliers);
            m_paneProductId[slot] = v.productId;
        } else {
            // Recompute of the SAME product: keep THIS pane's own LUT + range, swap only the
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
    // A map pane mounted while a PV path is up shows it too (and a reused pane
    // drops a path that does not belong to its product).
    if (pv)
        this->refreshPvOverlay();
    // …and refit once the move has settled: the pane's own resize refit runs a tick
    // later, when VTK may still report the OLD size, and the view then keeps a camera
    // for the old pane (an image shrunk into a corner after a swap or a relayout).
    this->scheduleSettledRefit(slot);
}

unsigned long vtkWindowCube::paneCameraMTime(int slot) const
{
    if (slot < 0 || slot > 3)
        return 0;
    vtkRenderer *ren = nullptr;
    if (m_slots[slot].view.kind == ViewId::Product && m_paneViews[slot])
        ren = m_paneViews[slot]->renderer();
    else if (m_slots[slot].view.kind == ViewId::Slice2D && this->sliceWin)
        ren = this->sliceWin->GetRenderers()->GetFirstRenderer();
    return (ren && ren->GetActiveCamera()) ? ren->GetActiveCamera()->GetMTime() : 0;
}

void vtkWindowCube::scheduleSettledRefit(int slot)
{
    if (slot < 0 || slot > 3)
        return;
    const int generation = ++m_refitGeneration[slot];
    const PaneView view = m_slots[slot].view;
    const unsigned long camera = paneCameraMTime(slot);
    QTimer::singleShot(120, this, [this, slot, generation, view, camera]() {
        if (generation != m_refitGeneration[slot] || m_slots[slot].view != view)
            return;   // a newer mount or relayout owns this pane now
        if (!m_slots[slot].frame || !m_slots[slot].frame->isVisible())
            return;
        if (paneCameraMTime(slot) != camera)
            return;   // panned / zoomed (or already refit) since: leave that view alone
        if (view.kind == ViewId::Product && m_paneViews[slot])
            m_paneViews[slot]->fit(true);
        else if (view.kind == ViewId::Slice2D)
            this->layout2dPaneFor(/*slice=*/true, /*force=*/true);
    });
}

void vtkWindowCube::parkView(int slot)
{
    if (slot < 0 || slot > 3 || !m_viewParkingLot)
        return;
    // Product panes have no shared singleton (viewWidget → null); their widget is the slot's
    // own Pane2DView (image) OR ProfileWidget (spectrum, #5). Park whichever is in the host.
    QWidget *w = viewWidget(m_slots[slot].view);
    if (!w && m_slots[slot].view.kind == ViewId::Product) {
        QWidget *spectrumW = m_paneSpectra[slot];   // ProfileWidget IS-A QWidget (upcast)
        if (spectrumW && spectrumW->parentWidget() == m_slots[slot].viewHost)
            w = spectrumW;
        else if (m_paneViews[slot])
            w = m_paneViews[slot]->widget();
    }
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
    // The 2-D channel bar lives on the slice, or on the Channels pane while the
    // slice wears a map. A Channels pane switched to something else gives it back.
    if (this->channelOverlay2d && this->channelOverlay2d->parentWidget() != ui->vtkImage
        && channelMirrorSlot() < 0) {
        attachChannelBar2d(ui->vtkImage);
        this->channelOverlay2d->setVisible(m_sliceBackdropProductId.isEmpty());
        this->layout2dPaneFor(/*slice=*/true, /*force=*/false);   // room for the bar again
    }
    // The slice wears a map only in the pane it was put there for: any view change
    // that took the slice out of that pane (menu, toggle, swap) means channels.
    // A pane merely hidden by the layout keeps its view, and the background.
    if (!m_sliceBackdropProductId.isEmpty()
        && (m_sliceBackdropSlot < 0 || m_sliceBackdropSlot > 3
            || m_slots[m_sliceBackdropSlot].view.kind != ViewId::Slice2D)) {
        setSliceBackdrop(QString());   // then this refresh carries on with the slice
    }
    // Re-gate the slice tools. setActivePane() emits activePaneChanged only when
    // the pane INDEX changes, so replacing the content of the pane already in
    // focus — 3-D swapped for the slice, say — left the tools at whatever the
    // previous view had set: disabled with a stale reason, or enabled on a view
    // that cannot take them.
    applyMomentToolGate();

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
            // A slice wearing a map as background (setSliceBackdrop) IS that map to the
            // user: same title, and the map's entry is the checked one below.
            const bool backdropHere = s.view.kind == ViewId::Slice2D
                    && !m_sliceBackdropProductId.isEmpty();
            const PaneView shownAs = backdropHere
                    ? PaneView{ ViewId::Product, m_sliceBackdropProductId }
                    : s.view;
            s.titleBtn->setText(viewTitle(shownAs) + u"  ▾"_s);
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
                // The slice worn as a map by THIS pane is shown here as the map; its
                // "2D Slice" entry is the way back to the channels, not "already here".
                if (backdropHere && cand.kind == ViewId::Slice2D && atPane == i)
                    atPane = -1;
                if (backdropHere && cand.kind == ViewId::Product
                    && cand.productId == m_sliceBackdropProductId)
                    atPane = i;
                if (atPane >= 0)
                    label += QStringLiteral("   ·  P%1").arg(atPane + 1);
                else if (collideJ >= 0)
                    label += QStringLiteral("   ·  P%1").arg(collideJ + 1);  // the blocking pane
                // Duplication hint: a product shown in ANOTHER pane is NOT checked here
                // (this pane doesn't show it) and reads "· P1" — clicking it DUPLICATES it
                // into this pane (each Product gets its own Pane2DView, §3.2 B3). Only the
                // view THIS pane currently shows is checked.
                // Image and spectrum products duplicate (each pane has its own view);
                // the PV diagram is a single widget that MOVES.
                if (atPane >= 0 && atPane != i && cand.kind == ViewId::Product
                    && cand.productId != m_pvProductId)
                    label += u"  (duplicate here)"_s;
                QAction *a = menu->addAction(label);
                a->setCheckable(true);
                a->setChecked(cand == shownAs);
                if (collideJ >= 0) {
                    // Disabled + the pane number reads as "can't - a moment is in P3"
                    // without needing the tooltip (which stays as the full explanation).
                    a->setEnabled(false);
                    a->setToolTip(QStringLiteral("%1 is already shown in P%2, one moment map "
                                                 "at a time")
                                          .arg(viewTitle(m_slots[collideJ].view))
                                          .arg(collideJ + 1));
                    continue;
                }
                QObject::connect(a, &QAction::triggered, this, [this, i, cand, backdropHere]() {
                    // Already this map (drawn on through the slice): nothing to change.
                    if (backdropHere && cand.kind == ViewId::Product
                        && cand.productId == m_sliceBackdropProductId) {
                        refreshPaneChrome();
                        return;
                    }
                    // Asking for the 2D Slice while the slice wears a map means "stop
                    // drawing on the map": the panes go back as they were.
                    if (!m_sliceBackdropProductId.isEmpty() && cand.kind == ViewId::Slice2D) {
                        setSliceBackdrop(QString());
                        return;
                    }
                    // Any other choice on the map's pane: the background goes first.
                    if (backdropHere)
                        setSliceBackdrop(QString());
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
        // Qualifier: the 3D view carries one ("volume"), and the live spectrum its
        // probe state — written back below, since this loop clears every other one.
        if (s.qualifier)
            s.qualifier->setText(s.view.kind == ViewId::View3D ? u"volume"_s : QString());
        // Per-view controls: only the 3D view has one now (Auto/Local/Remote).
        // Placed by layout, not by isVisible(): a refresh before the window is
        // on screen saw no pane as visible, and a single-pane window never got
        // the control until the layout changed.
        const bool laidOut = s.frame && !s.frame->isHidden();
        if (laidOut && s.controls && s.view.kind == ViewId::View3D && m_renderModeSeg) {
            m_renderModeSeg->setParent(s.controls->parentWidget());
            s.controls->addWidget(m_renderModeSeg);
            m_renderModeSeg->show();
        }
    }
    refreshPaneActiveStyles();
    updateDisplaySectionForActivePane();   // §5.2: view of the active pane may have changed
    syncSessionTreeToActivePane();         // reverse sync when the active pane's view changed
    for (int i = 0; i < 4; ++i)
        refreshPaneLutStrip(i);            // §5.4: keep each header strip on its pane's LUT
    m_linkNoReceiverHintShown = false;     // §5.5: pane composition changed → re-arm the hint
    updateActiveMapStats();          // active pane view may have changed → refresh 2D stats
    updateSpectrumInspectorControls();   // Spectrum panel follows the active pane's product
    pushProductPaneBadges();         // (#4) tree pane-number badges follow the pane layout
    // The live spectrum's "pixel (x, y) · pinned" state, cleared by the qualifier
    // reset above. Nothing else rewrites it while the probe is frozen — no hover
    // comes — so pinning a spectrum used to blank the live pane's status.
    updateLiveSpectrumHeader();
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
    // §5.5: "channel · LUT" is REAL state now — two per-facet toggles. Each gates one
    // propagation kind; both grey out (disabled) when the Linked master is off, so the
    // control reads its own state instead of being a decorative caption.
    auto makeFacetChip = [&](const QString &txt) {
        auto *c = new QToolButton(toolbar);
        c->setText(txt);
        c->setCheckable(true);
        c->setChecked(true);
        c->setCursor(Qt::PointingHandCursor);
        c->setToolButtonStyle(Qt::ToolButtonTextOnly);
        // Pill affordance: a bordered chip that clearly reads as a toggle. Off =
        // muted text + faint outline; on = accent text/border + a faint tint; hover
        // brightens the outline; disabled (Linked off / nothing to link) greys out.
        c->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:1px solid %1;border-radius:4px;"
                "font-family:'JetBrains Mono','SF Mono',monospace;font-size:10px;"
                "padding:1px 6px;color:%2;}"
                "QToolButton:checked{color:%3;border-color:%3;background:rgba(120,170,187,0.14);}"
                "QToolButton:hover:enabled{border-color:%3;}"
                "QToolButton:disabled{color:#3A4247;border-color:#2A3237;background:transparent;}")
                                 .arg(QLatin1StringView(VisivoTheme::kOutline()), fgMuted, pri));
        return c;
    };
    // STEP6-P5: a "camera" facet — camera sync used to be always-on when Linked; now it's a
    // toggle (gates BOTH 2D-pane and 3D camera, intra- and cross-window).
    m_linkCameraChip = makeFacetChip(u"camera"_s);
    m_linkCameraChip->setToolTip(u"Linked: sync pan / zoom / rotation across panes and windows"_s);
    QObject::connect(m_linkCameraChip, &QToolButton::toggled, this, [this](bool on) {
        m_linkCamera = on;
        if (!on)
            m_linked2DValid = false;   // STEP6-P2: drop the shared 2-D camera so re-enabling
                                        // camera sync doesn't snap panes back to a stale zoom
    });
    tb->addWidget(m_linkCameraChip);
    tb->addSpacing(4);
    m_linkChannelChip = makeFacetChip(u"channel"_s);
    m_linkChannelChip->setToolTip(u"Linked: sync the channel / slice index across panes"_s);
    QObject::connect(m_linkChannelChip, &QToolButton::toggled, this,
                     [this](bool on) { m_linkChannel = on; });
    tb->addWidget(m_linkChannelChip);
    tb->addSpacing(4);
    m_linkLutChip = makeFacetChip(u"LUT"_s);
    m_linkLutChip->setToolTip(u"Linked: propagate colour map / range to panes on the same product"_s);
    QObject::connect(m_linkLutChip, &QToolButton::toggled, this,
                     [this](bool on) { m_linkLut = on; });
    tb->addWidget(m_linkLutChip);
    tb->addSpacing(4);
    m_linkRenderChip = makeFacetChip(u"render"_s);
    m_linkRenderChip->setToolTip(u"Linked: sync the 3-D representation (Volume ↔ Isosurface) across windows"_s);
    QObject::connect(m_linkRenderChip, &QToolButton::toggled, this,
                     [this](bool on) { m_linkRender = on; });
    tb->addWidget(m_linkRenderChip);
    tb->addSpacing(4);
    m_linkBlendChip = makeFacetChip(u"blend"_s);
    m_linkBlendChip->setToolTip(u"Linked: sync the volume blend mode (Composite / MIP / MinIP)"_s);
    QObject::connect(m_linkBlendChip, &QToolButton::toggled, this,
                     [this](bool on) { m_linkBlend = on; });
    tb->addWidget(m_linkBlendChip);
    refreshLinkedFacetChips();
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

vtkWindowCube::MapStats vtkWindowCube::computeMapStats(vtkImageData *img)
{
    MapStats s;
    vtkDataArray *sc = (img && img->GetPointData()) ? img->GetPointData()->GetScalars() : nullptr;
    if (!sc)
        return s;   // valid = false
    const vtkIdType n = sc->GetNumberOfTuples();
    std::vector<double> vals;
    vals.reserve(static_cast<size_t>(n));
    double vmin = std::numeric_limits<double>::infinity();
    double vmax = -std::numeric_limits<double>::infinity();
    double sum = 0., sumsq = 0.;
    for (vtkIdType i = 0; i < n; ++i) {
        const double v = sc->GetComponent(i, 0);
        if (!std::isfinite(v))
            continue;   // NaN / masked pixels are ABSENT, not zero
        vals.push_back(v);
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
        sum += v;
        sumsq += v * v;
    }
    s.count = static_cast<long>(vals.size());
    if (vals.empty()) {
        // No finite pixels → summary stats UNDEFINED (rendered "-"), not a misleading 0.
        const double nan = std::numeric_limits<double>::quiet_NaN();
        s.min = s.max = s.mean = s.median = s.std = s.rms = nan;
        return s;   // valid = false, count = 0, sum = 0
    }
    s.valid = true;
    const double mean = sum / static_cast<double>(vals.size());
    s.min = vmin;
    s.max = vmax;
    s.mean = mean;
    s.sum = sum;
    s.rms = std::sqrt(sumsq / static_cast<double>(vals.size()));   // sqrt(E[x²]) — no cancellation
    // Population std from a SECOND pass over the deviations (stable for large-offset values).
    double ss = 0.;
    for (double v : vals) {
        const double d = v - mean;
        ss += d * d;
    }
    s.std = std::sqrt(ss / static_cast<double>(vals.size()));
    const size_t mid = vals.size() / 2;
    std::nth_element(vals.begin(), vals.begin() + mid, vals.end());
    double median = vals[mid];
    if ((vals.size() & 1u) == 0)   // even count → mean of the two central values
        median = 0.5 * (median + *std::max_element(vals.begin(), vals.begin() + mid));
    s.median = median;
    return s;
}

void vtkWindowCube::ensureMomentStats(Product2D &p)
{
    if (p.statsComputed || !p.imageData)
        return;
    p.statsComputed = true;   // compute once; imageData is immutable per product version
    p.stats = computeMapStats(p.imageData);
}

void vtkWindowCube::updateActiveMapStats()
{
    if (!m_activeMapStatsSection)
        return;
    // Rebuild the block each time the active pane's 2-D data changes.
    if (QLayout *lay = m_activeMapStatsSection->layout())
        while (QLayoutItem *it = lay->takeAt(0)) {
            if (QWidget *w = it->widget()) {
                w->hide();
                w->deleteLater();
            }
            delete it;
        }

    // The block follows the ACTIVE pane and shows ONLY when that pane HAS a 2-D map: a Moment
    // shows its cached whole-map stats; the 2D Slice shows the live slice's stats. A 3D /
    // Spectrum / empty pane has no 2-D map → the block hides (truly pane-aware, per (a)/Q3).
    QString label, unit;
    MapStats stats;
    bool have = false;
    const ViewId kind =
            (m_activePane >= 0 && m_activePane < 4) ? m_slots[m_activePane].view.kind : ViewId::None;
    if (kind == ViewId::Product) {
        auto found = m_products2d.find(m_slots[m_activePane].view.productId);
        if (found != m_products2d.end() && found->imageData) {
            ensureMomentStats(*found);
            stats = found->stats;
            unit = found->unit;
            label = found->label.toUpper();
            have = true;
        }
    } else if (kind == ViewId::Slice2D) {
        vtkImageData *sliceImg =
                this->remoteSliceDisplaySource
                        ? vtkImageData::SafeDownCast(
                                  this->remoteSliceDisplaySource->GetOutputDataObject(0))
                        : nullptr;
        if (sliceImg) {
            stats = computeMapStats(sliceImg);   // recomputed per scrub only while Slice is active
            label = u"2D SLICE"_s;
            have = true;   // slice brightness unit not tracked here → left blank
        }
    }
    if (!have) {
        m_activeMapStatsSection->setVisible(false);
        return;
    }

    auto *lay = qobject_cast<QVBoxLayout *>(m_activeMapStatsSection->layout());
    const QString unitSuffix = unit.isEmpty() ? QString() : (u" "_s + unit);
    // Gate on stats.valid so a map with no finite pixels (or no scalar array) renders "-",
    // never a misleading 0 <unit>.
    const bool ok = stats.valid;
    auto num = [&](double v) {
        return (ok && std::isfinite(v)) ? (QString::number(v, 'g', 6) + unitSuffix) : u"-"_s;
    };

    // Sub-caption = the pane name (this IS the "2D image" block, now pane-aware).
    auto *cap = new QLabel(label, m_activeMapStatsSection);
    cap->setStyleSheet(QStringLiteral("QLabel{color:%1; font-family:'Lato'; font-size:10px;"
                                      "font-weight:600; padding:6px 0 1px 0;}")
                               .arg(QLatin1StringView(VisivoTheme::kSecondary())));
    lay->addWidget(cap);

    auto addRow = [&](const QString &key, const QString &value) {
        auto *row = new QWidget(m_activeMapStatsSection);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 1, 0, 1);
        h->setSpacing(8);
        auto *k = new QLabel(key, row);
        k->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;")
                                 .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        h->addWidget(k, 1);
        auto *val = new QLabel(value, row);
        val->setStyleSheet(QStringLiteral("color:%1; font-family:'JetBrains Mono','SF Mono',"
                                          "monospace; font-size:11px;")
                                   .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        h->addWidget(val, 0, Qt::AlignRight);
        lay->addWidget(row);
    };
    addRow(u"Pixels"_s, QString::number(stats.count));
    addRow(u"Min"_s, num(stats.min));
    addRow(u"Max"_s, num(stats.max));
    addRow(u"Mean"_s, num(stats.mean));
    addRow(u"Median"_s, num(stats.median));
    addRow(u"Std"_s, num(stats.std));
    addRow(u"RMS"_s, num(stats.rms));
    addRow(u"Sum"_s, num(stats.sum));
    m_activeMapStatsSection->setVisible(true);
}

void vtkWindowCube::setupInspector()
{
    if (!this->m_workspaceChrome)
        return;
    InspectorPanel *panel = this->m_workspaceChrome->installInspectorDock();
    if (!panel)
        return;

    // R-22: add the scientific-copilot tab, wired to this window's backend
    // endpoint + dataset. Only meaningful for a remote (server-backed) dataset.
    if (!this->remoteBackendUrl.isEmpty()) {
        AssistantPanel *copilot = this->m_workspaceChrome->installCopilot(
                this->remoteBackendUrl, this->remoteBackendToken, this->remoteDatasetId);
        if (copilot) {
            // Phase 2: register this cube with the session-level router so the
            // copilot can target it by dataset id even from another view's panel.
            CopilotActionRouter::instance()->registerTarget(
                    this->remoteDatasetId, this,
                    [this](const QList<QJsonObject> &acts) { this->applyCopilotActions(acts); },
                    [this]() { this->undoCopilotActions(); });
            copilot->setSuggestions({
                    u"What are the statistics of this cube?"_s,
                    u"What's the noise level and a good threshold?"_s,
                    u"Compute and show the moment-0 map"_s,
                    u"Find the sources"_s });
        }
    }

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
    panel->addPropertyRow(u"Sanity"_s, this->remoteWcsStatus.isEmpty() ? u"-"_s
                                                                       : this->remoteWcsStatus);

    // Flatten a re-hosted stat widget so it reads as a value, not an input box.
    const QString statCss =
            QStringLiteral("border:none; background:transparent; color:%1;"
                           " font-family:'JetBrains Mono','SF Mono',monospace; font-size:11px;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface()));
    auto statVal = [&](QWidget *w) { if (w) w->setStyleSheet(statCss); return w; };

    panel->addPropertyCaption(u"STATISTICS"_s);
    // (a) ONE authoritative 2-D block that FOLLOWS the active pane (labelled by the pane:
    // "2D SLICE" / "MOMENT 0") with the full stat set — replaces the old static "2D image"
    // rows (which showed the last-computed image, not the active pane) and the separate
    // per-moment block (which duplicated the same numbers). Computed from the active pane's
    // own pixels; the 3-D cube stats below stay live.
    m_activeMapStatsSection = new QWidget();
    auto *statsLay = new QVBoxLayout(m_activeMapStatsSection);
    statsLay->setContentsMargins(0, 0, 0, 0);
    statsLay->setSpacing(0);
    panel->addPropertiesSection(m_activeMapStatsSection);
    updateActiveMapStats();

    panel->addPropertyCaption(u"3D cube"_s, /*sub=*/true);
    panel->addPropertyRow(u"Min"_s,  statVal(ui->lineCubeMin));
    panel->addPropertyRow(u"Max"_s,  statVal(ui->lineCubeMax));
    panel->addPropertyRow(u"Mean"_s, statVal(ui->lineCubeMean));
    // The full 8-row set (Median/Std/Sum added below) is filled from the backend's out-of-core
    // full-res scan — the ui->lineCube* fields show the fast local preview until it lands.
    auto *cubeMedian = new QLabel(u"-"_s);
    auto *cubeStd    = new QLabel(u"-"_s);
    auto *cubeSum    = new QLabel(u"-"_s);
    m_cubeStatMedian = cubeMedian;
    m_cubeStatStd    = cubeStd;
    m_cubeStatSum    = cubeSum;
    panel->addPropertyRow(u"Median"_s, statVal(cubeMedian));
    panel->addPropertyRow(u"Std"_s,    statVal(cubeStd));
    panel->addPropertyRow(u"RMS"_s,    statVal(ui->lineCubeRms));
    panel->addPropertyRow(u"Sum"_s,    statVal(cubeSum));
    this->fetchCubeStatsAsync();   // full-res stats overwrite the 4 preview rows + fill the rest

    panel->addPropertyCaption(u"WCS"_s);
    panel->addPropertyRow(u"Status"_s, this->remoteWcsStatus.isEmpty() ? u"-"_s
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
    // Same for the contour settings, which the Parameters tab mounts while the
    // contour tool is on (updateContoursVisibility).
    if (ui->frameContours) {
        ui->frameContours->setParent(panel);
        ui->frameContours->hide();
    }

    // View → "Inspector Panel" expands/collapses the dock (never setVisible, which
    // would hide the rail to 0 px and bypass the collapse state — §4.1).
    if (this->actionShowSessionPanel) {
        QObject::connect(this->actionShowSessionPanel, &QAction::toggled, this,
                         [this](bool checked) {
                             if (this->m_workspaceChrome)
                                 this->m_workspaceChrome->setSessionCollapsed(!checked);
                         });
    }
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
                             if (this->actionShowSessionPanel) {
                                 QSignalBlocker bs(this->actionShowSessionPanel);
                                 this->actionShowSessionPanel->setChecked(
                                         !this->m_workspaceChrome->sessionCollapsed());
                             }
                             QSignalBlocker b(this->actionShowControlsSidebar);
                             this->actionShowControlsSidebar->setChecked(
                                     !this->m_workspaceChrome->inspectorCollapsed());
                         });
        // Seed the checkstate from the (already-applied) collapsed default.
        if (this->actionShowSessionPanel) {
            QSignalBlocker bs(this->actionShowSessionPanel);
            this->actionShowSessionPanel->setChecked(
                    !this->m_workspaceChrome->sessionCollapsed());
        }
        QSignalBlocker b(this->actionShowControlsSidebar);
        this->actionShowControlsSidebar->setChecked(
                !this->m_workspaceChrome->inspectorCollapsed());
    }

    // ── Analysis (tools trigger the existing dialog QActions) ─────────────────
    const auto add = [panel](const QString &grp, const QString &label, QAction *a) {
        if (a) panel->addTool(grp, label, a);
    };
    // Every tool, from the one table. This used to be eleven entries chosen by
    // hand out of thirty-one, so two thirds of the window's tools existed only
    // in the menu.
    for (const CubeTool &t : cubeTools())
        add(t.group, t.action->text().remove(u'&'), t.action);

    // And the menu itself, regrouped from the same table.
    regroupToolsMenu();
    regroupViewMenu();
    // Once, now that the panes exist and the action does too. Without this the
    // unified export keeps its placeholder label — and its default ENABLED
    // state — until the first pane change, so a window opened and left alone
    // offers "Export Selected View…" that does not say what it would write.
    updateExportSelectedAction();

    // Armed-pane border: every interaction tool listens to ONE view, and the
    // amber frame says which. Wired here, once, for all of them — by this point
    // every action exists — rather than in each toggle handler (three of which
    // have early-return branches that would silently skip it).
    for (QAction *a : { ui->actionExtractSpectrum, actionExtractPvDiagram.data(),
                        actionBoxRegion.data(), actionCircleRegion.data(),
                        actionPolygonRegion.data(), actionAnnulusRegion.data(),
                        m_actKinematicLasso.data(), actionPickSpectrum3d.data() }) {
        if (a)
            QObject::connect(a, &QAction::toggled, this, [this]() {
                this->refreshPaneActiveStyles();   // amber armed border + Esc availability
                // …and re-gate: disarming a tool can change what may arm next. A
                // tool armed before a pane was maximized keeps its enabled state
                // while it runs (see keepArmedSlice / keepArmed3d), so once it is
                // switched off the gate has to run again or the action stays
                // enabled and can re-arm against the still-hidden view.
                // Re-entrant calls from the gate's own un-arming are suppressed by
                // m_inToolGate, and the outer pass finishes the job.
                this->applyMomentToolGate();
            });
    }

    // Migrated spectrum controls at the TOP of the Analysis tab (shown only while the active
    // pane is a spectrum product). The pane itself is left chrome-less.
    this->buildSpectrumInspectorControls(panel);

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

void vtkWindowCube::buildSpectrumInspectorControls(InspectorPanel *panel)
{
    using namespace Qt::StringLiterals;
    if (!panel)
        return;

    auto *section = new QWidget();
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(4, 4, 4, 8);
    col->setSpacing(6);

    const QLatin1StringView fgMuted(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView fg(VisivoTheme::kOnSurface());

    auto *caption = new QLabel(u"SPECTRUM"_s);
    caption->setStyleSheet(QStringLiteral("color:%1; %2 padding:2px 2px;")
                                   .arg(fgMuted, VisivoTheme::sectionHeaderCss()));
    col->addWidget(caption);

    m_specInspTitle = new QLabel(u"-"_s);
    m_specInspTitle->setStyleSheet(
            QStringLiteral("color:%1; font-family:'Lato'; font-size:12px; font-weight:600;").arg(fg));
    m_specInspTitle->setWordWrap(true);
    col->addWidget(m_specInspTitle);

    // Smoothing row (same kernel codes as ProfileWidget's inline combo).
    auto *smoothRow = new QHBoxLayout();
    smoothRow->setSpacing(6);
    auto *smoothLabel = new QLabel(u"Smooth:"_s);
    smoothLabel->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(fgMuted));
    m_specInspSmooth = new QComboBox();
    m_specInspSmooth->addItem(u"None"_s, 0);
    m_specInspSmooth->addItem(u"Hanning"_s, 1);
    m_specInspSmooth->addItem(u"Boxcar 3"_s, 3);
    m_specInspSmooth->addItem(u"Boxcar 5"_s, 5);
    m_specInspSmooth->addItem(u"Boxcar 7"_s, 7);
    m_specInspSmooth->addItem(u"Gaussian σ=1"_s, 10);
    m_specInspSmooth->addItem(u"Gaussian σ=2"_s, 20);
    m_specInspSmooth->setToolTip(u"Spectral smoothing kernel (display only; CSV export stays raw)."_s);
    smoothRow->addWidget(smoothLabel);
    smoothRow->addWidget(m_specInspSmooth, 1);
    col->addLayout(smoothRow);

    // Action buttons in a 2-column grid.
    const QString btnCss = VisivoTheme::sidebarToolButtonStyleSheet();
    auto mkBtn = [&](const QString &text, const QString &tip) {
        auto *b = new QPushButton(text);
        b->setStyleSheet(btnCss);
        b->setCursor(Qt::PointingHandCursor);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (!tip.isEmpty())
            b->setToolTip(tip);
        return b;
    };
    m_specInspFit        = mkBtn(u"Fit Gaussian"_s,
                                 u"Fit a single Gaussian (+ linear baseline) via the backend."_s);
    m_specInspClearFit   = mkBtn(u"Clear Fit"_s,
                                 u"Remove the fitted Gaussian overlay and its summary."_s);
    m_specInspLoadLines  = mkBtn(u"Load Lines…"_s,
                                 u"Overlay spectral-line markers from a CSV (frequency,label)."_s);
    m_specInspClearLines = mkBtn(u"Clear Lines"_s, u"Remove the line-ID overlay."_s);
    // "Compare", not "Pin Spectrum" — SPECTRAL ▸ Pin Spectrum in the tool list
    // above saves the curve as a product in Session Data, and this one only lays
    // it on the plot for comparison. One label for both was the reason a user
    // pinned a spectrum and then could not find it in Session Data.
    m_specInspPin        = mkBtn(u"Compare"_s,
                                 u"Keep the current curve on this plot as a comparison "
                                 u"overlay - display only, NOT saved as a product "
                                 u"(that is Tools ▸ Pin Spectrum)."_s);
    m_specInspClearPins  = mkBtn(u"Clear Overlays"_s,
                                 u"Remove all comparison curves from this plot."_s);
    m_specInspSaveCsv    = mkBtn(u"Save CSV…"_s, u"Export the raw spectrum as CSV."_s);
    m_specInspSavePng    = mkBtn(u"Save PNG…"_s, u"Export the plot as a PNG image."_s);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(4);
    // One row per action, with its own "clear" beside it — the pairing is the
    // point: every overlay this panel can add, it can also take away.
    grid->addWidget(m_specInspFit,        0, 0);
    grid->addWidget(m_specInspClearFit,   0, 1);
    grid->addWidget(m_specInspLoadLines,  1, 0);
    grid->addWidget(m_specInspClearLines, 1, 1);
    grid->addWidget(m_specInspPin,        2, 0);
    grid->addWidget(m_specInspClearPins,  2, 1);
    grid->addWidget(m_specInspSaveCsv,    3, 0);
    grid->addWidget(m_specInspSavePng,    3, 1);
    col->addLayout(grid);

    m_specInspStats = new QLabel(u"-"_s);
    m_specInspStats->setStyleSheet(
            QStringLiteral("color:%1; font-size:10px; font-family:'JetBrains Mono','SF Mono',monospace;")
                    .arg(fg));
    m_specInspStats->setWordWrap(true);
    m_specInspStats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(m_specInspStats);

    // Wire the controls ONCE to the CURRENT target (m_specInspTarget), so no per-pane rewiring is
    // needed — retargeting just repoints the pointer + reflects state.
    QObject::connect(m_specInspSmooth, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this](int) {
                         if (m_specInspTarget && m_specInspSmooth)
                             m_specInspTarget->applySmoothingCode(
                                     m_specInspSmooth->currentData().toInt());
                     });
    auto onTarget = [this](void (ProfileWidget::*fn)()) {
        return [this, fn]() { if (m_specInspTarget) (m_specInspTarget->*fn)(); };
    };
    QObject::connect(m_specInspFit,        &QPushButton::clicked, this, onTarget(&ProfileWidget::fitGaussianNow));
    QObject::connect(m_specInspClearFit,   &QPushButton::clicked, this, onTarget(&ProfileWidget::clearFitNow));
    QObject::connect(m_specInspLoadLines,  &QPushButton::clicked, this, onTarget(&ProfileWidget::loadLinesFromFile));
    QObject::connect(m_specInspClearLines, &QPushButton::clicked, this, onTarget(&ProfileWidget::clearLines));
    QObject::connect(m_specInspPin,        &QPushButton::clicked, this,
                     [this]() { if (m_specInspTarget) m_specInspTarget->pinCurrentSpectrum(); });
    QObject::connect(m_specInspClearPins,  &QPushButton::clicked, this,
                     [this]() { if (m_specInspTarget) m_specInspTarget->clearPinnedSpectra(); });
    QObject::connect(m_specInspSaveCsv,    &QPushButton::clicked, this, onTarget(&ProfileWidget::saveSpectrumCsvNow));
    QObject::connect(m_specInspSavePng,    &QPushButton::clicked, this, onTarget(&ProfileWidget::saveSpectrumPngNow));

    m_spectrumInspector = section;
    panel->addAnalysisSection(section);
    section->setVisible(false);   // shown only when the active pane is a spectrum
}

void vtkWindowCube::updateSpectrumInspectorControls()
{
    using namespace Qt::StringLiterals;
    if (!m_spectrumInspector)
        return;
    // Target: the spectrum the user is working on — the SELECTED pane, or, while a
    // probe tool is running, the live preview that tool is driving.
    //
    // Those are the only two answers that mean anything. Selection alone made the
    // controls appear only after clicking the plot and vanish again on a click in
    // the 3-D view (the tool was running the whole time). But "any spectrum pane
    // on screen" is equally wrong the other way: with the tool closed there is no
    // current spectrum, only products, so a panel of fit / pin / export controls
    // aimed at whichever one happens to be mounted is aimed at nothing the user
    // pointed at. No spectrum selected and no tool running → no panel.
    auto spectrumSlot = [this]() {
        auto isSpectrum = [this](int i) {
            return m_slots[i].view.kind == ViewId::Product
                    && m_products1d.contains(m_slots[i].view.productId) && m_paneSpectra[i]
                    && m_slots[i].frame && m_slots[i].frame->isVisible();
        };
        if (m_activePane >= 0 && m_activePane < 4 && isSpectrum(m_activePane))
            return m_activePane;
        if (!m_spectrumLiveId.isEmpty())   // a probe tool is armed and drawing
            for (int i = 0; i < 4; ++i)
                if (isSpectrum(i) && m_slots[i].view.productId == m_spectrumLiveId)
                    return i;
        return -1;
    };
    const int slot = spectrumSlot();
    ProfileWidget *sp = (slot >= 0) ? m_paneSpectra[slot] : nullptr;
    m_specInspTarget = sp;
    if (!sp) {
        m_spectrumInspector->setVisible(false);
        return;
    }
    m_spectrumInspector->setVisible(true);
    m_specInspTitle->setText(m_products1d[m_slots[slot].view.productId].label);
    {   // reflect the pane's current smoothing without echoing back into it
        QSignalBlocker b(m_specInspSmooth);
        const int idx = m_specInspSmooth->findData(sp->smoothingCode());
        if (idx >= 0)
            m_specInspSmooth->setCurrentIndex(idx);
    }
    const bool haveSpec = sp->hasSpectrum();
    m_specInspFit->setEnabled(haveSpec && sp->canFit());
    m_specInspClearFit->setEnabled(sp->hasFit());
    m_specInspPin->setEnabled(haveSpec);
    m_specInspClearPins->setEnabled(sp->pinnedSpectrumCount() > 0);
    m_specInspClearPins->setText(sp->pinnedSpectrumCount() > 0
                                         ? u"Clear Overlays (%1)"_s.arg(sp->pinnedSpectrumCount())
                                         : u"Clear Overlays"_s);
    m_specInspSaveCsv->setEnabled(haveSpec);
    m_specInspSavePng->setEnabled(haveSpec);
    const QString stats = sp->statsText();
    m_specInspStats->setText(stats.isEmpty() ? u"-"_s : stats);
}

void vtkWindowCube::fetchCubeStatsAsync()
{
    // Ask the backend for FULL-resolution whole-cube stats (out-of-core) and overwrite the
    // local decimated-preview rows when they land + fill Median/Std/Sum. Once per window; a
    // local-only cube (no backend dataset) keeps the client-computed preview stats.
    if (m_cubeStatsRequested || this->remoteDatasetId.isEmpty()
        || this->remoteBackendUrl.isEmpty())
        return;
    m_cubeStatsRequested = true;
    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString session = this->remoteSessionId;
    const QString datasetId = this->remoteDatasetId;
    auto *watcher = new QFutureWatcher<BackendCubeStatsResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendCubeStatsResult>::finished, this,
                     [this, watcher]() {
                         const BackendCubeStatsResult s = watcher->result();
                         watcher->deleteLater();
                         if (!s.valid)
                             return;   // keep the preview stats on failure
                         const QString bu = this->m_brightnessUnit.isEmpty()
                                 ? QString() : (u" "_s + this->m_brightnessUnit);
                         auto fmt = [&](double v) { return CubeStats::fmtStat(v) + bu; };
                         if (ui->lineCubeMin)  ui->lineCubeMin->setText(fmt(s.min));
                         if (ui->lineCubeMax)  ui->lineCubeMax->setText(fmt(s.max));
                         if (ui->lineCubeMean) ui->lineCubeMean->setText(fmt(s.mean));
                         if (ui->lineCubeRms)  ui->lineCubeRms->setText(fmt(s.rms));
                         if (m_cubeStatMedian)
                             m_cubeStatMedian->setText(
                                     fmt(s.median)
                                     + (s.medianExact ? QString() : u"  (approx)"_s));
                         if (m_cubeStatStd) m_cubeStatStd->setText(fmt(s.std));
                         if (m_cubeStatSum) m_cubeStatSum->setText(fmt(s.sum));
                     });
    watcher->setFuture(QtConcurrent::run([url, token, session, datasetId]() {
        BackendClient c(url, token);
        c.setSessionId(session);
        return c.fetchCubeStats(datasetId);
    }));
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

bool vtkWindowCube::paneDetached(int slot) const
{
    return slot >= 0 && slot <= 3 && m_slots[slot].detachedWindow;
}

QWidget *vtkWindowCube::makeDetachPlaceholder(int slot)
{
    // Occupies the grid slot while the pane lives in its own window — SAME chrome as a pane
    // (paneFrame border + #000004 body + a header bar with the title) so it reads as the
    // slot's own cell, not naked text. The frame's size behaviour is copied by the caller so
    // the splitter never re-flows.
    const QLatin1StringView ink(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView fg(VisivoTheme::kOnSurface());
    auto *ph = new QFrame();
    ph->setObjectName(u"paneFrame"_s);
    ph->setStyleSheet(u"QFrame#paneFrame{border:1px solid #1E2427;}"_s);
    auto *v = new QVBoxLayout(ph);
    v->setContentsMargins(1, 1, 1, 1);
    v->setSpacing(0);

    // Header bar (matches a pane header's height + dot + title).
    auto *hdr = new QWidget(ph);
    hdr->setObjectName(u"paneHeader"_s);
    hdr->setFixedHeight(34);
    auto *h = new QHBoxLayout(hdr);
    h->setContentsMargins(10, 0, 6, 0);
    h->setSpacing(8);
    auto *dot = new QLabel(u"○"_s, hdr);
    dot->setStyleSheet(u"font-size:9px; color:#3A4247;"_s);
    h->addWidget(dot);
    auto *title = new QLabel(QStringLiteral("Pane %1 · detached").arg(slot + 1), hdr);
    title->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;"
                                        "font-weight:700;").arg(fg));
    h->addWidget(title);
    h->addStretch(1);
    v->addWidget(hdr);

    // Body: #000004 canvas + centred Bring back.
    auto *body = new QWidget(ph);
    body->setStyleSheet(u"background:#000004;"_s);
    auto *bv = new QVBoxLayout(body);
    bv->addStretch(1);
    auto *sub = new QLabel(u"Open in its own window"_s, body);
    sub->setAlignment(Qt::AlignCenter);
    sub->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(ink));
    bv->addWidget(sub);
    auto *row = new QHBoxLayout();
    row->addStretch(1);
    auto *btn = new QPushButton(u"Bring back"_s, body);
    btn->setCursor(Qt::PointingHandCursor);
    QObject::connect(btn, &QPushButton::clicked, this, [this, slot]() { bringBackPane(slot); });
    row->addWidget(btn);
    row->addStretch(1);
    bv->addSpacing(8);
    bv->addLayout(row);
    bv->addStretch(1);
    v->addWidget(body, 1);
    return ph;
}

void vtkWindowCube::refitDetachablePane(int slot)
{
    // Post-reparent (detach/bring-back) render + refit, per view kind — a top-level reparent
    // is an OpenGL-context-sensitive path, so force the paint for every kind, not just the
    // per-slot Pane2DView.
    if (slot < 0 || slot > 3)
        return;
    const ViewId k = m_slots[slot].view.kind;
    if (k == ViewId::Product && m_paneViews[slot])
        m_paneViews[slot]->fit(true);
    else if (k == ViewId::Slice2D) {
        layout2dPaneFor(/*slice=*/true, /*force=*/true);
        if (this->sliceWin)
            this->sliceWin->Render();
    } else if (k == ViewId::View3D && ui->vtkCube && ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::detachPane(int slot)
{
    if (slot < 0 || slot > 3 || !m_slots[slot].frame || m_slots[slot].detachedWindow)
        return;
    if (m_maximizedPane >= 0)
        return;   // maximize + detach don't compose — ignore while maximized
    QSplitter *col = (slot == 0 || slot == 2) ? m_leftColumn : m_rightColumn;
    if (!col)
        return;
    const int idx = col->indexOf(m_slots[slot].frame);
    if (idx < 0)
        return;

    // Snapshot BOTH splitters' pixel sizes — QSplitter re-negotiates when a widget is
    // swapped (that is what collapsed the 3D pane), so re-apply them right after the swap.
    const QList<int> colSizes = col->sizes();
    const QList<int> mainSizes = m_paneSplitter ? m_paneSplitter->sizes() : QList<int>{};

    // The placeholder must occupy EXACTLY the frame's cell: same sizePolicy + minimumSize +
    // stretch so the splitter gives it the frame's extent, nothing more.
    QWidget *ph = makeDetachPlaceholder(slot);
    ph->setSizePolicy(m_slots[slot].frame->sizePolicy());
    ph->setMinimumSize(m_slots[slot].frame->minimumSize());
    ph->setMaximumSize(m_slots[slot].frame->maximumSize());
    m_slots[slot].placeholder = ph;
    QWidget *taken = col->replaceWidget(idx, ph);   // == m_slots[slot].frame
    col->setStretchFactor(idx, 1);
    if (col->count() == colSizes.size())
        col->setSizes(colSizes);
    if (m_paneSplitter && !mainSizes.isEmpty() && m_paneSplitter->count() == mainSizes.size())
        m_paneSplitter->setSizes(mainSizes);

    // The frame (header + view + colorbar, WITH its active border) moves to a bare top-level
    // window — no toolbar, no menu, no duplicate controls; the docks stay in the main window.
    // Child of THIS (Qt::Window) so it's destroyed with the cube (no orphan on close).
    auto *win = new QWidget(this, Qt::Window);
    win->setObjectName(u"detachedPane"_s);
    win->setWindowTitle(viewTitle(m_slots[slot].view));
    win->setAttribute(Qt::WA_DeleteOnClose, false);
    auto *wl = new QVBoxLayout(win);
    wl->setContentsMargins(0, 0, 0, 0);
    if (taken)
        wl->addWidget(taken);
    if (m_slots[slot].frame)
        m_slots[slot].frame->show();
    m_slots[slot].detachedWindow = win;
    win->installEventFilter(this);   // Close → bring back
    win->resize(640, 560);
    // Reopen where the user last left this slot's detached window (session memory).
    if (!m_detachedGeom[slot].isEmpty())
        win->restoreGeometry(m_detachedGeom[slot]);
    win->show();
    win->raise();

    if (m_slots[slot].detachBtn) {
        m_slots[slot].detachBtn->setText(u"⤵"_s);
        m_slots[slot].detachBtn->setToolTip(u"Dock back into the grid"_s);
    }
    setActivePane(slot);   // the detached pane becomes the single app-wide active pane
    refreshPaneChrome();
    QTimer::singleShot(0, this, [this, slot]() { refitDetachablePane(slot); });
}

void vtkWindowCube::bringBackPane(int slot)
{
    if (slot < 0 || slot > 3 || !m_slots[slot].detachedWindow)
        return;
    QSplitter *col = (slot == 0 || slot == 2) ? m_leftColumn : m_rightColumn;
    if (!col)
        return;

    // Snapshot both splitters' sizes so bring-back is pixel-identical to before the detach.
    const QList<int> colSizes = col->sizes();
    const QList<int> mainSizes = m_paneSplitter ? m_paneSplitter->sizes() : QList<int>{};
    const int idx = m_slots[slot].placeholder ? col->indexOf(m_slots[slot].placeholder) : -1;
    if (m_slots[slot].frame && idx >= 0) {
        QWidget *oldPh = col->replaceWidget(idx, m_slots[slot].frame);
        col->setStretchFactor(idx, 1);
        if (oldPh)
            oldPh->deleteLater();
        m_slots[slot].placeholder = nullptr;
    } else if (m_slots[slot].frame) {
        col->addWidget(m_slots[slot].frame);   // fallback: append
        if (m_slots[slot].placeholder) {
            m_slots[slot].placeholder->setParent(nullptr);
            m_slots[slot].placeholder->deleteLater();
            m_slots[slot].placeholder = nullptr;
        }
    }
    if (col->count() == colSizes.size())
        col->setSizes(colSizes);
    if (m_paneSplitter && !mainSizes.isEmpty() && m_paneSplitter->count() == mainSizes.size())
        m_paneSplitter->setSizes(mainSizes);
    QWidget *win = m_slots[slot].detachedWindow;
    m_slots[slot].detachedWindow = nullptr;   // null BEFORE deleting → bring-back is idempotent
    if (win) {
        m_detachedGeom[slot] = win->saveGeometry();   // remember where it was for a re-detach
        win->removeEventFilter(this);
        win->hide();
        win->deleteLater();
    }
    if (m_slots[slot].detachBtn) {
        m_slots[slot].detachBtn->setText(u"⤴"_s);
        m_slots[slot].detachBtn->setToolTip(u"Pop out to a window"_s);
    }
    refreshPaneChrome();
    QTimer::singleShot(0, this, [this, slot]() { refitDetachablePane(slot); });
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
        m_escRestoreShortcut->setEnabled(anyTool2dArmed() || anyTool3dArmed());
    applyPaneLayout(m_preMaxPaneCount);     // re-applies frame/column visibility + fits
}

void vtkWindowCube::applyPaneLayout(int panes)
{
    if (!m_leftColumn || !m_rightColumn)
        return;
    // A layout change re-negotiates the grid; a detached pane's placeholder can't survive
    // that, so re-dock any detached panes first (they return to their slots, then the new
    // layout applies).
    for (int i = 0; i < 4; ++i)
        if (m_slots[i].detachedWindow)
            bringBackPane(i);
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
    // The master "Linked" button + facet chips' enabled state (pane- AND
    // cross-window-aware) is set centrally in refreshLinkedFacetChips().
    refreshLinkedFacetChips();
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
    // The product panes refit on their own resize, one event-loop tick later — when
    // VTK may not have adopted the new size yet, so the fit sees the old one and
    // keeps a camera scaled for the old pane (an image shrunk into a corner after
    // the layout grows). Fit them again once the layout has settled.
    for (int i = 0; i < 4; ++i)
        if (vis[i] && (m_slots[i].view.kind == ViewId::Product
                       || m_slots[i].view.kind == ViewId::Slice2D))
            scheduleSettledRefit(i);
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
            this->scopedProductId(QStringLiteral("moment%1").arg(result.order));
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
    // §5.4b: mom0 (intensity) → Inferno, mom1 (velocity) → CoolWarm, mom2 (dispersion/width)
    // → Viridis; any higher order falls back to Inferno.
    const QString momentLut = (result.order == 1)   ? u"CoolWarm"_s
                            : (result.order == 2)   ? u"Viridis"_s
                                                    : u"Inferno"_s;
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
    // Physical unit for the Inspector stats block — taken from the provenance params just
    // populated for this moment (MomentMapApplyResult itself carries no unit).
    {
        const QString u = this->m_momentProvenanceParams.value(u"Unit"_s).toString();
        cached.unit = (u == u"-"_s) ? QString() : u;
    }
    this->m_products2d.insert(productId, cached);
    if (productId == this->m_sliceBackdropProductId && cached.imageData) {
        // The slice is showing this map as its background: show the new pixels.
        this->remoteSliceDisplaySource->SetOutput(cached.imageData);
        this->remoteSliceDisplaySource->Modified();
        this->sliceColors->SetInputData(cached.imageData);
        if (this->sliceWin)
            this->sliceWin->Render();
    }
    // New pixels under a product id that survives a recompute of the same order:
    // measurements stamped with the previous generation are no longer about this
    // map, and say so when restored. Per product - switching between two cached
    // moments must not mark either of them as recomputed.
    ++this->m_momentProductGeneration[productId];
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
    refreshPaneChrome();   // also refreshes the active moment's Inspector stats

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

bool vtkWindowCube::isBusyForPvInteraction() const
{
    const bool pvBusy = this->pvController && this->pvController->isBusy();
    if (!pvBusy || !this->pvModeActive)
        return this->isBusy();
    return this->cubeOpenWatcher.isRunning() || this->remotePreviewWatcher.isRunning()
            || this->remoteHighResCubeWatcher.isRunning()
            || (this->momentController && this->momentController->isBusy())
            || this->activeRemoteSliceRequests > 0
            || this->activeRemoteIsosurfaceRequests > 0 || this->isosurfaceWatcher.isRunning();
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
    // Source finding runs entirely on the backend over this window's cube, so
    // it follows the same rule. (In the Data Hub it was gated on "a dataset was
    // opened at some point", which is not the same question.)
    if (this->m_actSourceFind) {
        this->m_actSourceFind->setEnabled(spectralEnabled);
        this->m_actSourceFind->setToolTip(
                available ? u"Run the SoFiA-2 source finder over this cube and overlay "
                            u"the detections; needs sofia2 on the backend's PATH."_s
                          : kMomentBackendTooltip);
    }
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
        // Not disabled while a PV computes: the controller queues the latest
        // request, and a live-edited path recomputes continuously.
        const bool pvEnabled = available && !this->cubeOpenWatcher.isRunning()
                && !this->remotePreviewWatcher.isRunning();
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

    // §5.1b: remember PV's backend baseline, then re-assert the moment gate on top.
    this->m_pvBaselineEnabled = this->actionExtractPvDiagram
            ? this->actionExtractPvDiagram->isEnabled() : false;
    this->applyMomentToolGate();
}

void vtkWindowCube::reopenProductFromFile(const QString &productId)
{
    // Double-click on a product whose tool window is gone. If it left a FITS on
    // the backend, that is what "open" means; otherwise say so rather than
    // doing nothing at all, which is what the row did before (user report).
    const AnalysisProduct p = ProductRegistry::instance().product(productId);
    const QString fitsPath = p.params.value(u"Output path"_s).toString();
    if (!fitsPath.isEmpty()) {
        emit this->openBackendFitsRequested(fitsPath, p.label);
        return;
    }
    // A MEASUREMENT has no file and no pane to mount: what it has is the numbers
    // it recorded, and "open" means putting them back on screen in the result
    // window they came from. Without this the row was inert — selecting it filled
    // Inspector ▸ Provenance, but double-clicking did nothing at all and the
    // popup the user had closed was unrecoverable (user report).
    if (!p.params.isEmpty()) {
        // The overlay too: a stats-only region clears its own overlay when the
        // analysis ends, so without this the numbers come back with nothing on
        // screen saying which pixels they came from.
        if (this->restoreProductOrigin(p.origin)) {
            if (auto *win = this->active2dWin())
                win->Render();
            if (!this->m_originRestoreNote.isEmpty())
                this->statusBar()->showMessage(
                        u"Showing %1%2"_s.arg(visivo::describeOrigin(p.origin),
                                              this->m_originRestoreNote),
                        6000);
        }
        this->showRecordedResult(p);
        return;
    }
    this->statusBar()->showMessage(
            p.label.isEmpty()
                    ? u"This product recorded nothing to reopen."_s
                    : u"“%1” recorded nothing to reopen."_s.arg(p.label),
            5000);
}

QVariantMap vtkWindowCube::stampRegionSurface(QVariantMap origin) const
{
    using namespace Qt::StringLiterals;
    const bool onSlice = this->viewingSlice();
    // m_displayedSliceIndex, not the spin box: the control is written from a
    // pending request too, so measuring during a fetch would stamp the channel
    // the user asked for rather than the one the statistics came from.
    const int channel = onSlice ? (this->m_displayedSliceIndex >= 0
                                           ? this->m_displayedSliceIndex
                                           : this->clampRemoteSliceIndex(ui->spinSlice->value() - 1))
                                : -1;
    if (onSlice && !this->m_sliceBackdropProductId.isEmpty()) {
        // The slice is showing a map (setSliceBackdrop): that map was measured,
        // not a channel — name it and its version, as for a moment pane.
        origin = visivo::withMeasuredSurface(std::move(origin), u"moment"_s, -1);
        origin.insert(u"momentProduct"_s, this->m_sliceBackdropProductId);
        origin.insert(u"momentGeneration"_s,
                      this->m_momentProductGeneration.value(this->m_sliceBackdropProductId, 0));
        return origin;
    }
    origin = visivo::withMeasuredSurface(std::move(origin), onSlice ? u"slice"_s : u"moment"_s,
                                         channel);
    // "moment" alone does not say WHICH moment map: the pipeline holds one at a
    // time and a recompute replaces it, so name the product too.
    if (!onSlice) {
        if (!this->m_momentPipelineProductId.isEmpty())
            origin.insert(u"momentProduct"_s, this->m_momentPipelineProductId);
        // …and WHICH version of it: the product id survives a recompute of the
        // same order, the pixels do not.
        if (!this->m_momentPipelineProductId.isEmpty())
            origin.insert(u"momentGeneration"_s,
                          this->m_momentProductGeneration.value(this->m_momentPipelineProductId, 0));
    }
    return origin;
}

bool vtkWindowCube::restoreProductOrigin(const QVariantMap &origin)
{
    using namespace Qt::StringLiterals;
    // At entry: the probe and PV branches return early, and a note left from a
    // previous region would be appended to their status line instead.
    m_originRestoreNote.clear();
    const QString kind = visivo::originKind(origin);
    if (kind == u"probe"_s) {
        // A pinned spectrum: put the crosshair back where it was probed, and go
        // to that channel, so the plot and the slice agree about the pixel.
        int v[3] = { 0, 0, 0 };
        if (!visivo::originPoint(origin, u"voxel"_s, v, 3))
            return false;
        this->requestRemoteSlice(this->clampRemoteSliceIndex(v[2]));
        this->probeVoxel = { v[0], v[1], v[2] };
        this->probeValid = true;
        this->refreshProbeOverlay();
        if (auto *win = this->active2dWin())
            win->Render();
        return true;
    }
    if (kind == u"pvpath"_s) {
        const auto verts = visivo::originPathF(origin);
        if (verts.size() < 2)
            return false;
        this->pvPolylineVertices = verts;
        this->m_pvSmooth = false;   // the recorded points ARE the cut that was taken
        this->pvWidthPixels = std::max(1, visivo::originWidth(origin));
        this->pvCurrentVoxel = verts.back();
        this->pvValid = true;
        this->refreshPvOverlay();
        if (auto *win = this->active2dWin())
            win->Render();
        return true;
    }
    if (kind != u"region"_s)
        return false;
    const QString shape = visivo::originShape(origin);
    if (shape == u"polygon"_s) {
        const auto verts = visivo::originPolygon(origin);
        if (verts.size() < 3)
            return false;
        this->regionPolygonVertices = verts;
        this->regionMode = RegionMode::Polygon;
    } else {
        int a[2] = { 0, 0 }, c[2] = { 0, 0 };
        if (!visivo::originPoint(origin, u"anchor"_s, a, 2)
            || !visivo::originPoint(origin, u"current"_s, c, 2))
            return false;   // half a geometry is worse than none
        this->regionAnchorVoxel = { a[0], a[1] };
        this->regionCurrentVoxel = { c[0], c[1] };
        this->regionAnnulusInnerRadius = visivo::originInnerRadius(origin);
        this->regionMode = shape == u"box"_s     ? RegionMode::Box
                         : shape == u"circle"_s  ? RegionMode::Circle
                         : shape == u"annulus"_s ? RegionMode::Annulus
                                                 : RegionMode::None;
        if (this->regionMode == RegionMode::None)
            return false;
    }
    // Back to the surface the numbers came from. Redrawing a channel-10 region
    // over channel 50 puts the overlay in the right pixels of the wrong data.
    const QString surface = visivo::originSurface(origin);
    const int channel = visivo::originChannel(origin);
    if (surface == u"slice"_s && channel >= 0) {
        const int target = this->clampRemoteSliceIndex(channel);
        if (!this->viewingSlice()) {
            m_originRestoreNote = u" - measured on channel %1 of the cube, not on this "
                                  u"moment map"_s.arg(target + 1);
        } else {
            // Restoring supersedes any navigation in progress, even when the
            // right channel happens to be on screen already: a scrub waiting out
            // its debounce, or a fetch in flight for another channel, would
            // otherwise land after us and move the data out from under the
            // overlay we just put back.
            this->remoteSliceDebounceTimer.stop();
            if (this->m_displayedSliceIndex != target
                || this->currentRequestedRemoteSliceIndex != target)
                this->requestRemoteSlice(target);
        }
    } else if (surface == u"moment"_s) {
        const QString measuredOn = origin.value(u"momentProduct"_s).toString();
        if (this->viewingSlice())
            m_originRestoreNote = u" - measured on a moment map, not on this slice"_s;
        else if (!measuredOn.isEmpty() && measuredOn != this->m_momentPipelineProductId)
            m_originRestoreNote = u" - measured on a different moment map than the one shown"_s;
        else if (!measuredOn.isEmpty() && !this->m_products2d.contains(measuredOn))
            m_originRestoreNote = u" - the moment map it was measured on is no longer loaded"_s;
        else if (origin.contains(u"momentGeneration"_s) && !measuredOn.isEmpty()
                 && origin.value(u"momentGeneration"_s).toInt()
                         != this->m_momentProductGeneration.value(measuredOn, 0))
            // Same product id, different image: the id is stable across a
            // recompute of the same order, so it cannot tell the two apart.
            m_originRestoreNote =
                    u" - the moment map has been recomputed since this was measured"_s;
    }
    this->regionDragging = false;
    this->regionValid = true;
    this->refreshRegionOverlay();
    if (auto *win = this->active2dWin())
        win->Render();
    return true;
}

void vtkWindowCube::showRecordedResult(const AnalysisProduct &product)
{
    using namespace Qt::StringLiterals;
    // Rebuilt from the product's own recorded parameters, so it survives the
    // tool, the dialog and anything else: the registry IS the record.
    // One window per product: double-clicking the row three times used to stack
    // three identical result windows.
    const QString slot = u"recorded-result:"_s + product.productId;
    if (auto *existing = this->findChild<ToolResultDialog *>(slot)) {
        // Replace rather than raise: a second run of the same tool overwrites the
        // registry entry under this id, so an open window can be showing the
        // previous numbers next to the newly restored overlay.
        existing->setObjectName(QString());
        existing->close();
        existing->deleteLater();
    }
    auto *dlg = new ToolResultDialog(this);
    dlg->setObjectName(slot);

    // The recorded window, replayed. Same record the tool showed, same code
    // path, so the two cannot drift apart.
    if (!product.resultView.isEmpty()) {
        dlg->applyRecord(product.resultView);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
        return;
    }

    // No recipe (an older product, or a tool that never showed a result window):
    // fall back to laying out whatever it recorded in Provenance.
    dlg->setTitle(product.label.isEmpty() ? u"Result"_s : product.label);

    // Keys that describe WHERE/OVER WHAT the measurement was taken read as a
    // subtitle; the rest are the measurement itself. Insertion order is not
    // preserved by QVariantMap, so the context keys are named explicitly and the
    // remainder follows in the map's own (alphabetical) order.
    static const QStringList kContextKeys{ u"Region"_s, u"Channels"_s, u"Position"_s,
                                           u"2D stats from"_s, u"Valid pixels"_s,
                                           u"Storage"_s, u"Status"_s };
    QStringList context;
    for (const QString &key : kContextKeys) {
        const QString v = product.params.value(key).toString();
        if (!v.isEmpty())
            context << u"%1: %2"_s.arg(key, v);
    }
    if (!context.isEmpty())
        dlg->setSubtitle(context.join(u"  ·  "_s));

    QList<QPair<QString, QString>> rows;
    for (auto it = product.params.cbegin(); it != product.params.cend(); ++it) {
        if (kContextKeys.contains(it.key()))
            continue;
        rows.append({ it.key(), it.value().toString() });
    }
    // QVariantMap iterates in KEY order, which lists a statistic as max, mean,
    // median, min — alphabetical, and not how anyone reads it. Group by the
    // quantity (the key up to its last word) and order the summary within a
    // group the way the tools present it.
    static const QStringList kStatOrder{ u"mean"_s, u"median"_s, u"min"_s, u"max"_s };
    std::stable_sort(rows.begin(), rows.end(),
                     [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
                         const QString qa = a.first.section(u' ', 0, -2);
                         const QString qb = b.first.section(u' ', 0, -2);
                         if (qa != qb)
                             return qa < qb;
                         const int ia = kStatOrder.indexOf(a.first.section(u' ', -1).toLower());
                         const int ib = kStatOrder.indexOf(b.first.section(u' ', -1).toLower());
                         if (ia < 0 || ib < 0)
                             return a.first < b.first;
                         return ia < ib;
                     });
    if (rows.isEmpty()) {
        // Everything it recorded was context; show that rather than an empty box.
        for (const QString &line : context)
            rows.append({ line.section(u": "_s, 0, 0), line.section(u": "_s, 1) });
    }
    dlg->addSection(u"Recorded"_s, rows);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void vtkWindowCube::completeBaselineProduct(const QString &productId,
                                            const QString &newDatasetId,
                                            const QString &outputPath, const QString &summary)
{
    // Write down what the popup used to show once and then lose. "Output path"
    // is the key one: it is what makes the row reopenable after the dialog is
    // gone (see showSessionRowContextMenu), and it tells the user where the cube
    // actually is — the backend's temp directory, which is why it was not in
    // Workspace Exports (user report).
    QVariantMap params;
    if (!newDatasetId.isEmpty())
        params.insert(u"New dataset ID"_s, newDatasetId);
    if (!outputPath.isEmpty()) {
        params.insert(u"Output path"_s, outputPath);
        params.insert(u"Storage"_s, u"temporary file on the backend, export to keep it"_s);
    }
    // The summary carries method / RMS before-after / fit channels; keep it as
    // one Provenance row rather than re-parsing it into fields.
    if (!summary.isEmpty())
        params.insert(u"Summary"_s, summary);
    ProductRegistry::instance().setParams(productId, params);
    ProductRegistry::instance().setStatus(productId, u"ready"_s);
    // Hand the row back its FILE. The product's window pointer is the Baseline
    // dialog, and activation prefers a window over a path, so while that dialog
    // was still open a double-click on the finished row raised the tool again
    // instead of opening the cube it had just produced. The dialog is
    // the tool, not the product; the cube is the product.
    if (!outputPath.isEmpty())
        ProductRegistry::instance().attachWindow(productId, nullptr);
}

void vtkWindowCube::saveProductCopyToWorkspace(const QString &backendDatasetId,
                                               const QString &label,
                                               const QString &sourcePath)
{
    using namespace Qt::StringLiterals;
    // Basename from the product label, so the file is recognisable in the
    // Workspace Exports list rather than being another hex id.
    QString base = label.simplified().toLower();
    base.replace(QRegularExpression(u"[^a-z0-9]+"_s), u"_"_s);
    base = base.mid(0, 48);
    while (base.endsWith(u'_'))
        base.chop(1);
    if (base.isEmpty())
        base = u"product"_s;

    this->statusBar()->showMessage(u"Copying %1 into Workspace Exports…"_s.arg(label), 0);
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString sessionId = this->remoteSessionId;
    QPointer<vtkWindowCube> self(this);
    auto *watcher = new QFutureWatcher<BackendWorkspaceDeleteResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendWorkspaceDeleteResult>::finished, this,
                     [self, watcher, label, sourcePath]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Save a Permanent Copy"_s,
                                     u"Could not copy %1 into Workspace Exports:\n\n%2"_s
                                             .arg(label, r.error));
                             return;
                         }
                         emit self->workspaceExportsChanged();
                         self->statusBar()->showMessage(
                                 u"Saved to Workspace Exports as %1"_s.arg(r.deletedFilename),
                                 8000);
                         QMessageBox::information(self, u"Save a Permanent Copy"_s,
                                 u"%1 is now in Workspace Exports as:\n\n%2\n\n"
                                 u"The temporary original stays where it was (%3) until "
                                 u"the system clears it; open the permanent copy from the "
                                 u"Workspace Exports panel in the Data Hub."_s
                                         .arg(label, r.deletedFilename, sourcePath));
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, sessionId, backendDatasetId, base]() {
                BackendClient c(url, token);
                c.setSessionId(sessionId);
                return c.copyDatasetToWorkspace(backendDatasetId, base);
            }));
}

void vtkWindowCube::startFullResolutionLoad()
{
    // The tail of View ▸ Load Full Resolution, without its warning dialog: the
    // recommendation banner offers this on a LOCAL backend, where it has already
    // said what the cost is (GPU memory) and there is no download to warn about.
    this->userForcedFullResolution = true;
    this->statusBar()->showMessage(u"Loading full-resolution cube from backend…"_s, 4000);
    if (!this->requestHighResCube()) {
        this->setRemoteCubeDisplayState(this->usingHighResCube
                                                ? RemoteCubeDisplayState::FullResolution
                                                : RemoteCubeDisplayState::Preview);
        this->clearPersistentStatusMessage();
    }
}

void vtkWindowCube::applyMomentToolGate()
{
    // Availability of every view- or state-dependent tool, from ONE table
    // (CubeToolGate, unit-tested) applied to the QActions the Tools menu and
    // Inspector ▸ Analysis are both built from. A tool that cannot
    // act says why in its tooltip; it never sits enabled and does nothing.
    //
    // Re-entrancy: disabling a checkable tool un-arms it, and those toggled()
    // handlers (setProbeModeActive, setRegionMode, …) drop region / probe state
    // and call back here. One pass is enough — the second would run against
    // half-updated state.
    if (this->m_inToolGate)
        return;
    this->m_inToolGate = true;
    const auto guard = qScopeGuard([this] { this->m_inToolGate = false; });

    static const QString reasonNoSlice =
            u"acts on the 2D Slice, show it in a pane (the pane's ▾ menu)"_s;
    static const QString reasonNo3d =
            u"acts on the 3D view, show it in a pane (the pane's ▾ menu)"_s;
    static const QString reasonNoProbe =
            u"probe a pixel first (Extract Spectrum, or Pick Spectrum on the 3D plane)"_s;
    static const QString reasonNoRegion =
            u"draw or import a region first (Box / Circle / Polygon / Annulus)"_s;
    static const QString reasonNoImageView =
            u"open a 2D image viewer first - the slice overlays there as contours"_s;
    static const QString reasonNoLinkTargets =
            u"nothing to link yet, split into 2 panes or open a second cube viewer"_s;
    // Every reason this function writes, so a tooltip of ours is never mistaken
    // for the tool's own sentence and cached over it.
    static const QSet<QString> ourReasons{ reasonNoSlice,      reasonNo3d,
                                           reasonNoProbe,      reasonNoRegion,
                                           reasonNoImageView,  reasonNoLinkTargets };

    // enabled=false with a reason disables + explains; otherwise the action goes
    // back to its own baseline and its own tooltip.
    auto applyGate = [this](QAction *a, bool enabled, const QString &reason) {
        if (!a)
            return;
        // Adopt the real tooltip on EVERY pass where it is not one of ours,
        // rather than caching once: this gate first runs during construction,
        // from setupPaneChrome() -> applyPaneLayout() -> refreshPaneChrome(),
        // which is BEFORE some of these tooltips are assigned — a cache-once
        // would have stored QAction's fallback text and then restored that over
        // the real sentence. Re-adopting also keeps up with anything that
        // legitimately rewrites a tooltip later (PV's backend tip).
        const bool showingOurReason = ourReasons.contains(a->toolTip());
        if (!showingOurReason)
            m_toolBaseTooltips.insert(a, a->toolTip());
        if (!enabled && !reason.isEmpty()) {
            if (a->isCheckable() && a->isChecked()) {
                // This gate un-arms because the tool's VIEW left the layout, not
                // because the user closed the tool — so the lasso must suspend
                // (stop listening for clicks) rather than close, which would throw
                // away a selection the user refined merely for switching panes.
                const bool lasso = (a == m_actKinematicLasso.data());
                if (lasso) m_lassoDisarmIsSuspend = true;
                a->setChecked(false);   // un-arm a tool whose view left the layout
                if (lasso) m_lassoDisarmIsSuspend = false;
            }
            a->setEnabled(false);
            a->setToolTip(reason);
            a->setStatusTip(reason);
            return;
        }
        a->setEnabled(enabled);
        if (showingOurReason) {
            const QString base = m_toolBaseTooltips.value(a);
            a->setToolTip(base);
            a->setStatusTip(base);
        }
    };

    CubeToolGate::Inputs in;
    in.slice2dVisible = slice2dVisible();
    in.view3dVisible = view3dVisible();
    in.slicePaneAvailable = momentPaneForSliceTools() >= 0;
    const CubeToolGate::Decision d = CubeToolGate::evaluate(in);
    // Preserving a RUNNING tool is a different question from letting a new one
    // start. While a pane is maximized the other frames are hidden but
    // their views are still assigned, so a tool already armed on one of them
    // survives the zoom untouched — its pane comes back on Esc. Nothing new may
    // arm against a view that cannot be seen.
    const bool zoomed = (m_maximizedPane >= 0);
    const bool keepArmedSlice = !d.slice2dTools && zoomed && viewAssigned(ViewId::Slice2D);
    const bool keepArmed3d = !d.view3dTools && zoomed && viewAssigned(ViewId::View3D);

    // The slice family. Import Region belongs here too: it draws the parsed
    // region onto the 2-D view and runs the region analysis on it, so without
    // that view on screen it is the same no-op as drawing one by hand.
    const std::array<QAction *, 7> sliceTools{ ui->actionExtractSpectrum,
                                               actionExtractPvDiagram.data(),
                                               actionBoxRegion.data(),
                                               actionCircleRegion.data(),
                                               actionPolygonRegion.data(),
                                               actionAnnulusRegion.data(),
                                               m_actImportCubeRegion.data() };
    for (QAction *a : sliceTools) {
        if (!a)
            continue;
        // PV additionally carries a backend baseline (dataset visible, no run in
        // flight); the rest share the cube-open baseline.
        const bool baseline = (a == actionExtractPvDiagram.data()) ? m_pvBaselineEnabled
                                                                   : m_cubeActionsEnabled;
        // A REASON un-arms; a baseline denial only greys out. PV drops its
        // baseline the moment an extraction starts, and un-arming there runs
        // clearPv() while the controller is still reading the path it was handed
        // — the first extraction then submits an empty polyline.
        if (!d.slice2dTools) {
            if (keepArmedSlice && a->isChecked())
                continue;   // running, and its pane is only hidden by the zoom
            applyGate(a, false, reasonNoSlice);
        } else {
            applyGate(a, baseline, QString());
            // Enabled because a moment pane could become the slice — but a tool
            // ALREADY armed listens on the slice, which has just left the screen:
            // un-arm it (arming again draws on the moment pane).
            const bool sliceHiddenByZoom = zoomed && viewAssigned(ViewId::Slice2D);
            if (!in.slice2dVisible && !sliceHiddenByZoom && a->isCheckable() && a->isChecked())
                a->setChecked(false);
        }
    }
    m_momentToolGated = !d.slice2dTools;

    // The mirror image: the tools that act on the 3-D VIEW. The lasso installs
    // its double-click observer on the 3-D interactor and the spectral picker
    // its own, so with no 3-D pane on screen they wait for a click that cannot
    // come.
    for (QAction *a : { m_actKinematicLasso.data(), actionPickSpectrum3d.data() }) {
        if (!a)
            continue;
        if (!d.view3dTools) {
            if (keepArmed3d && a->isChecked())
                continue;                        // running; only hidden by the zoom
            applyGate(a, false, reasonNo3d);     // un-arms: its view is not on screen
        } else {
            applyGate(a, m_cubeActionsEnabled, QString());
        }
    }

    // State gates: the tool is fine on this layout, but there is nothing yet for
    // it to act on. Each of these used to be enabled always and answer a click
    // with a modal "draw a region first" / a transient status line — a dialog
    // doing the job of a disabled button.
    //
    // Read the state HERE, not with the view inputs above: the un-arming just
    // done runs the tools' own toggled() handlers, and those change exactly this
    // state — dropping an unfinished polygon through setRegionMode() ->
    // clearRegion(), whose own gate call this pass suppresses. Deciding from an
    // earlier snapshot left Export Region enabled with no region behind it.
    CubeToolGate::Inputs stateIn = in;
    // Pin Spectrum promotes the LAST probed spectrum, which the 2-D probe and the
    // 3-D plane pick both feed (updateProbePlot is the single writer).
    stateIn.haveProbeSpectrum = !m_lastProbeValues.isEmpty();
    // Region export serialises the live shape, or the last finalised one after
    // analysis reset regionMode (same fallback exportRegionsToFile() uses).
    stateIn.haveRegion = this->regionValid
            && (this->regionMode != RegionMode::None
                || this->m_exportableRegionShape != RegionMode::None);
    // "Send Slice to Image Viewer" emits contourDataReady, which MainWindow
    // relays to the first open image viewer — with none open the click did
    // nothing at all, no message, no window. Checked by class name so this file
    // need not pull in the image viewer's header.
    stateIn.imageViewerOpen = false;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w && w->inherits("vtkWindowImage")) {
            stateIn.imageViewerOpen = true;
            break;
        }
    }
    // Same test refreshLinkedFacetChips() uses for the facet chips: >1 pane here
    // (intra-window sync) or >1 open cube window (cross-window sync).
    stateIn.haveLinkTargets = (m_paneCount > 1 || s_registry.size() > 1);
    const CubeToolGate::Decision sd = CubeToolGate::evaluate(stateIn);

    applyGate(m_actPinSpectrum.data(), m_cubeActionsEnabled && sd.pinSpectrum, reasonNoProbe);
    applyGate(m_actExportCubeRegion.data(), m_cubeActionsEnabled && sd.exportRegion,
              reasonNoRegion);
    applyGate(m_actSendSliceToImage.data(), m_cubeActionsEnabled && sd.sendSliceToImageViewer,
              reasonNoImageView);
    applyGate(m_actLinkViews.data(), sd.linkViews, reasonNoLinkTargets);
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
    // §5.1b: probe / region baseline = "cube open + usable"; re-assert the moment gate.
    this->m_cubeActionsEnabled = enabled;
    this->applyMomentToolGate();
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

namespace {
// While a drawing tool (PV path, region) is armed, the left and right buttons
// belong to the tool — the observers on the interactor read them — so the style
// must not window/level or zoom with them. The wheel still zooms and the middle
// button still pans: drawing a path along a small source needs both.
class DrawingToolInteractorStyle : public vtkInteractorStyleImage
{
public:
    static DrawingToolInteractorStyle *New();
    vtkTypeMacro(DrawingToolInteractorStyle, vtkInteractorStyleImage);
    void OnLeftButtonDown() override {}
    void OnLeftButtonUp() override {}
    void OnRightButtonDown() override {}
    void OnRightButtonUp() override {}
    void OnChar() override {}   // no 'r'/'w'/… shortcuts while drawing
};
vtkStandardNewMacro(DrawingToolInteractorStyle);
}  // namespace

void vtkWindowCube::setInteractorStyleRegion()
{
    for (auto *win : { this->sliceWin.GetPointer(), this->momentWin.GetPointer() }) {
        if (win && win->GetInteractor()) {
            vtkNew<DrawingToolInteractorStyle> style;
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
                momentId = this->scopedProductId(u"moment0"_s);
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
    // Linked views: mirror the Volume↔Isosurface choice to linked peers (gated by
    // the master; suppressed when this switch is itself a linked-driven update).
    if (this->m_linkViews && !this->m_applyingLinkedUpdate)
        this->broadcastLinkedRenderMode();
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

// ── Copilot actions ──────────────────────────────────────────────────────────

// Set the Volume/Isosurface actions to `iso` without firing their handlers and
// run the representation switch (mirrors applyLinkedRenderMode's local part).
void vtkWindowCube::setCopilotRenderMode(bool iso)
{
    if (!ui->actionIsosurface || ui->actionIsosurface->isChecked() == iso)
        return;
    {
        QSignalBlocker bIso(ui->actionIsosurface);
        QSignalBlocker bVol(ui->actionVolume);
        ui->actionIsosurface->setChecked(iso);
        if (ui->actionVolume) ui->actionVolume->setChecked(!iso);
    }
    // The blocked actionIso/Volume::toggled handlers normally keep the sidebar in
    // sync (the Volume/Iso segment + the volume-mode card visibility) — replicate.
    if (this->m_uiAssembler) {
        if (auto *c = this->m_uiAssembler->renderModeCombo()) {
            const QSignalBlocker b(c);
            c->setCurrentIndex(iso ? 0 : 1);
        }
    }
    this->updateVolumeModeVisibility(!iso);
    this->changeCubeRender();
}

void vtkWindowCube::applyCopilotBlendMode(int idx)
{
    this->applyVolumeBlendMode(idx);
    if (this->m_uiAssembler)
        this->m_uiAssembler->syncVolumeModeSidebar(idx);
    const QSignalBlocker b1(this->m_actionComposite);
    const QSignalBlocker b2(this->m_actionMIP);
    const QSignalBlocker b3(this->m_actionMinIP);
    if (this->m_actionComposite) this->m_actionComposite->setChecked(idx == 0);
    if (this->m_actionMIP) this->m_actionMIP->setChecked(idx == 1);
    if (this->m_actionMinIP) this->m_actionMinIP->setChecked(idx == 2);
}

void vtkWindowCube::applyCopilotActions(const QList<QJsonObject> &actions)
{
    if (actions.isEmpty())
        return;
    // Distinguish REVERSIBLE display directives (colormap/threshold/render/blend/
    // channel/camera) from products/hand-offs (compute_moment, show_sources,
    // pin_spectrum) that create their own removable output. Only snapshot + offer
    // Undo when the batch actually changes reversible display state, so pinning a
    // spectrum or opening source-finding can't later revert unrelated manual edits.
    bool hasReversible = false;
    for (const QJsonObject &a : actions) {
        const QString t = a.value(u"type"_s).toString();
        if (t == u"set_colormap"_s || t == u"set_threshold"_s || t == u"set_render_mode"_s
            || t == u"set_blend_mode"_s || t == u"goto_channel"_s || t == u"reset_camera"_s) {
            hasReversible = true;
            break;
        }
    }
    if (hasReversible) {
        // Snapshot the affected display state ONCE before the batch, so Undo can
        // restore it (display-only, reversible).
        this->m_copilotUndo = CopilotSnapshot{};
        this->m_copilotUndo.valid = true;
        // Edit ▸ Undo becomes available exactly when there is something to go
        // back to, so the menu never offers an action that would do nothing.
        if (m_actUndoCopilot)
            m_actUndoCopilot->setEnabled(true);
        this->m_copilotUndo.colormap = this->m_activeColorMapName;
        this->m_copilotUndo.threshold = ui->lineThreshold ? ui->lineThreshold->text() : QString();
        this->m_copilotUndo.isosurface = ui->actionIsosurface && ui->actionIsosurface->isChecked();
        this->m_copilotUndo.blendMode = this->m_volumeBlendMode;
        this->m_copilotUndo.channel0 = ui->spinSlice ? ui->spinSlice->value() - 1 : 0;
    } else {
        // A products/hand-off-only batch (pin_spectrum / show_sources / moment):
        // invalidate any earlier snapshot so the panel's "Undo latest change"
        // can't revert an OLDER reversible edit — the latest change isn't one
        // this mechanism reverts.
        this->m_copilotUndo = CopilotSnapshot{};
    }
    // Capture the camera only if this batch resets it, so Undo of a non-camera
    // batch won't clobber the user's own camera moves.
    bool batchResetsCamera = false;
    for (const QJsonObject &a : actions)
        if (a.value(u"type"_s).toString() == u"reset_camera"_s) { batchResetsCamera = true; break; }
    if (batchResetsCamera && ui->vtkCube && ui->vtkCube->renderWindow()) {
        if (auto *ren = ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()) {
            if (auto *cam = ren->GetActiveCamera()) {
                cam->GetPosition(this->m_copilotUndo.camPos);
                cam->GetFocalPoint(this->m_copilotUndo.camFoc);
                cam->GetViewUp(this->m_copilotUndo.camUp);
                this->m_copilotUndo.camScale = cam->GetParallelScale();
                this->m_copilotUndo.cameraCaptured = true;
            }
        }
    }

    for (const QJsonObject &a : actions) {
        const QString type = a.value(u"type"_s).toString();
        if (type == u"set_colormap"_s) {
            const QString want = a.value(u"name"_s).toString().trimmed();
            // The backend emits free-form / lowercase names; match the app's
            // colormap registry case-insensitively.
            QString matched;
            for (const std::string &n : ColorMaps::GetColorMapNames()) {
                const QString qn = QString::fromStdString(n);
                if (qn.compare(want, Qt::CaseInsensitive) == 0) { matched = qn; break; }
            }
            if (!matched.isEmpty())
                this->applyColorMapByName(matched);
        } else if (type == u"set_threshold"_s) {
            const double v = std::clamp(a.value(u"value"_s).toDouble(),
                                        double(this->lowerBound), double(this->upperBound));
            if (ui->lineThreshold) {
                ui->lineThreshold->setText(QString::number(v));
                this->thresholdLineChanged();
            }
        } else if (type == u"set_render_mode"_s) {
            this->setCopilotRenderMode(a.value(u"mode"_s).toString().compare(
                    u"isosurface"_s, Qt::CaseInsensitive) == 0);
        } else if (type == u"set_blend_mode"_s) {
            const QString m = a.value(u"mode"_s).toString().toLower();
            this->applyCopilotBlendMode(m == u"mip"_s ? 1 : m == u"minip"_s ? 2 : 0);
        } else if (type == u"goto_channel"_s) {
            this->requestRemoteSlice(this->clampRemoteSliceIndex(a.value(u"index"_s).toInt()));
        } else if (type == u"reset_camera"_s) {
            this->resetCameraFront();
        } else if (type == u"show_sources"_s) {
            // Not a reversible display change: hand off to the host to open the
            // SoFiA-2 source-find dialog for this cube and overlay the mask in 3D.
            emit this->sourceFinderRequested();
        } else if (type == u"pin_spectrum"_s) {
            // The backend already extracted the spectrum at the position (no
            // viewer coordinate math): pin the axis+values as a 1-D product.
            const QJsonArray axis = a.value(u"axis"_s).toArray();
            const QJsonArray vals = a.value(u"values"_s).toArray();
            // Require a matching axis (or none); preserve blank channels sent as
            // JSON null as NaN so the profile shows gaps, not spurious zeros.
            const bool axisOk = axis.isEmpty() || axis.size() == vals.size();
            if (!vals.isEmpty() && axisOk) {
                const double kNaN = std::numeric_limits<double>::quiet_NaN();
                QVector<double> key, values;
                key.reserve(axis.size());
                values.reserve(vals.size());
                for (const QJsonValue &v : axis) key.append(v.isDouble() ? v.toDouble() : kNaN);
                for (const QJsonValue &v : vals) values.append(v.isDouble() ? v.toDouble() : kNaN);
                const int px = a.value(u"pixel_x"_s).toInt();
                const int py = a.value(u"pixel_y"_s).toInt();
                const QString xLabel = this->spectralAxisTitle().isEmpty()
                        ? u"Spectral axis"_s : this->spectralAxisTitle();
                const QString pid = this->addSpectrumProduct(
                        u"Copilot Spectrum"_s,
                        QStringLiteral("copilotspec_%1_%2").arg(px).arg(py),
                        QStringLiteral("Spectrum @ (%1, %2)").arg(px).arg(py),
                        key, values, xLabel, u"Intensity"_s, {});
                // fit=true → overlay a Gaussian fit on the pinned profile, reusing
                // the ProfileWidget's own backend fit machinery (which draws the
                // model curve). Fit the VISIBLE pane now showing this product —
                // prefer the just-activated target, and never a hidden/parked
                // duplicate (which could hold stale cached data).
                if (a.value(u"fit"_s).toBool() && !pid.isEmpty()) {
                    auto paneShowsPid = [&](int i) {
                        return i >= 0 && i < 4 && m_slots[i].frame
                                && m_slots[i].frame->isVisible()
                                && m_slots[i].view.kind == ViewId::Product
                                && m_slots[i].view.productId == pid && m_paneSpectra[i];
                    };
                    int fitSlot = paneShowsPid(m_activePane) ? m_activePane : -1;
                    if (fitSlot < 0)
                        for (int i = 0; i < 4; ++i)
                            if (paneShowsPid(i)) { fitSlot = i; break; }
                    if (fitSlot >= 0)
                        m_paneSpectra[fitSlot]->fitGaussianNow();
                }
            }
        } else if (type == u"compute_moment"_s && this->momentController) {
            // Compute AND display a moment product (added to Session Data) via the
            // same worker/registration as the dialog — headless. Not part of the
            // display Undo (it creates a product the user can remove separately).
            // Only override a Config default when the directive carries the key,
            // so an omitted mask/threshold keeps the dialog's defaults.
            MomentMapController::Config cfg;   // defaults: mask on, auto threshold
            cfg.order = a.value(u"order"_s).toInt(0);
            const int depth = std::max(1, this->remoteSliceCount());
            // Omitted channel bounds → the full cube; explicit bounds are used as
            // given (a single-channel start==end is respected).
            cfg.channelStart = a.contains(u"channel_start"_s) ? a.value(u"channel_start"_s).toInt() : 0;
            cfg.channelEnd = a.contains(u"channel_end"_s) ? a.value(u"channel_end"_s).toInt() : depth - 1;
            if (a.contains(u"mask_enabled"_s)) cfg.maskEnabled = a.value(u"mask_enabled"_s).toBool();
            if (a.contains(u"threshold_value"_s)) cfg.thresholdValue = a.value(u"threshold_value"_s).toDouble();
            if (a.contains(u"threshold_auto"_s)) cfg.thresholdAuto = a.value(u"threshold_auto"_s).toBool();
            // No threshold named: the same default as the dialog — 3σ of the
            // session's noise estimate when there is one, else Auto.
            if (cfg.maskEnabled && this->m_noiseSigma > 0.0 && !a.contains(u"threshold_value"_s)
                && !a.contains(u"threshold_auto"_s)) {
                cfg.thresholdAuto = false;
                cfg.thresholdValue = 3.0 * this->m_noiseSigma;
                cfg.thresholdSource = u"3σ of the noise estimate, %1"_s.arg(this->m_noiseWhere);
            }
            this->momentController->startComputationHeadless(cfg);
        }
    }
    if (ui->vtkCube && ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
    this->statusBar()->showMessage(
            hasReversible
                    ? tr("Copilot applied %n visualisation change(s), Undo available.",
                         "", actions.size())
                    : tr("Copilot applied %n change(s).", "", actions.size()),
            6000);
}

void vtkWindowCube::undoCopilotActions()
{
    if (!this->m_copilotUndo.valid)
        return;
    const CopilotSnapshot s = this->m_copilotUndo;
    this->m_copilotUndo.valid = false;   // one-shot; a new batch re-snapshots
    if (m_actUndoCopilot)
        m_actUndoCopilot->setEnabled(false);

    this->setCopilotRenderMode(s.isosurface);   // representation first
    if (!s.colormap.isEmpty())
        this->applyColorMapByName(s.colormap);
    if (ui->lineThreshold && !s.threshold.isEmpty()) {
        ui->lineThreshold->setText(s.threshold);
        this->thresholdLineChanged();
    }
    this->applyCopilotBlendMode(s.blendMode);
    this->requestRemoteSlice(this->clampRemoteSliceIndex(s.channel0));
    if (s.cameraCaptured && ui->vtkCube && ui->vtkCube->renderWindow()) {
        if (auto *ren = ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()) {
            if (auto *cam = ren->GetActiveCamera()) {
                cam->SetPosition(s.camPos[0], s.camPos[1], s.camPos[2]);
                cam->SetFocalPoint(s.camFoc[0], s.camFoc[1], s.camFoc[2]);
                cam->SetViewUp(s.camUp[0], s.camUp[1], s.camUp[2]);
                cam->SetParallelScale(s.camScale);
                ren->ResetCameraClippingRange();
            }
        }
    }
    if (ui->vtkCube && ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
    this->statusBar()->showMessage(tr("Reverted the copilot's visualisation changes."), 4000);
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
    const int target = std::clamp(next, minIndex, maxIndex);
    ui->spinSlice->setValue(target);

    // Fetch NOW, past the debounce. setValue() lands in updateSlice(), which
    // moves the cutting plane straight away but defers the fetch by 300 ms — and
    // restarts that timer on every change. At anything above ~3 fps the next
    // tick re-arms it before it fires, so the timer never fires at all: the
    // plane slid through the cube in the 3-D view while the 2-D slice and the
    // texture on the plane stayed on the frame where playback started.
    // updateSlice()'s own comment says the animation tick is one of the paths
    // that should bypass the debounce; it just was not doing it.
    //
    // Safe to call every tick: requestRemoteSlice() applies a cached slice
    // immediately and drops a request for one already in flight, so a fast
    // animation over a cached range costs nothing and over an uncached one
    // issues each fetch once.
    this->remoteSliceDebounceTimer.stop();
    this->requestRemoteSlice(target - 1);
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
    // Both observers are PASSIVE: the trackball style grabs the interactor's focus
    // on the press, after which VTK delivers the release only to it and to passive
    // observers — an active observer never saw a single click. Passive observers
    // must not change anything, so they only note positions; the pick runs queued,
    // after VTK has finished with the event, and only for a click: the release
    // that ends a rotation is not a pick.
    auto press = vtkSmartPointer<vtkCallbackCommand>::New();
    press->SetClientData(this);
    press->SetCallback([](vtkObject *caller, unsigned long, void *clientData, void *) {
        auto *self = static_cast<vtkWindowCube *>(clientData);
        int p[2];
        static_cast<vtkRenderWindowInteractor *>(caller)->GetEventPosition(p);
        self->m_pick3dPressPos = { p[0], p[1] };
        self->m_pick3dDragged = false;
    });
    press->SetPassiveObserver(1);
    this->cube3dPickPressTag = iren->AddObserver(vtkCommand::LeftButtonPressEvent, press, 1.0);

    constexpr int kClickSlop = 4;   // device px; beyond it, at any point, is a drag
    auto move = vtkSmartPointer<vtkCallbackCommand>::New();
    move->SetClientData(this);
    move->SetCallback([](vtkObject *caller, unsigned long, void *clientData, void *) {
        auto *self = static_cast<vtkWindowCube *>(clientData);
        const std::array<int, 2> from = self->m_pick3dPressPos;
        if (from[0] < 0 || self->m_pick3dDragged)
            return;
        int p[2];
        static_cast<vtkRenderWindowInteractor *>(caller)->GetEventPosition(p);
        if (std::abs(p[0] - from[0]) > kClickSlop || std::abs(p[1] - from[1]) > kClickSlop)
            self->m_pick3dDragged = true;   // latched: coming back does not make it a click
    });
    move->SetPassiveObserver(1);
    this->cube3dPickMoveTag = iren->AddObserver(vtkCommand::MouseMoveEvent, move, 1.0);

    auto release = vtkSmartPointer<vtkCallbackCommand>::New();
    release->SetClientData(this);
    release->SetCallback([](vtkObject *caller, unsigned long, void *clientData, void *) {
        auto *self = static_cast<vtkWindowCube *>(clientData);
        int p[2];
        static_cast<vtkRenderWindowInteractor *>(caller)->GetEventPosition(p);
        const std::array<int, 2> pos{ p[0], p[1] };
        const std::array<int, 2> from = self->m_pick3dPressPos;
        const bool dragged = self->m_pick3dDragged;
        self->m_pick3dPressPos = { -1, -1 };
        self->m_pick3dDragged = false;
        if (from[0] < 0 || dragged || std::abs(pos[0] - from[0]) > kClickSlop
            || std::abs(pos[1] - from[1]) > kClickSlop)
            return;
        QPointer<vtkWindowCube> guard(self);
        QTimer::singleShot(0, self, [guard, pos]() {
            if (guard && guard->actionPickSpectrum3d && guard->actionPickSpectrum3d->isChecked())
                guard->handle3dPickEvent(pos);
        });
    });
    release->SetPassiveObserver(1);
    this->cube3dPickObserverTag = iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, release, 1.0);
}

void vtkWindowCube::remove3dPickObserver()
{
    auto *iren = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetInteractor()
            : nullptr;
    if (iren && this->cube3dPickObserverTag != 0) {
        iren->RemoveObserver(this->cube3dPickObserverTag);
    }
    if (iren && this->cube3dPickPressTag != 0) {
        iren->RemoveObserver(this->cube3dPickPressTag);
    }
    if (iren && this->cube3dPickMoveTag != 0) {
        iren->RemoveObserver(this->cube3dPickMoveTag);
    }
    this->cube3dPickObserverTag = 0;
    this->cube3dPickPressTag = 0;
    this->cube3dPickMoveTag = 0;
    this->m_pick3dPressPos = { -1, -1 };
    this->m_pick3dDragged = false;
}

void vtkWindowCube::showPick3dMarker(const double world[3])
{
    auto *renderer = ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    if (!renderer)
        return;

    if (!m_pick3dMarkerActor) {
        // A sphere rather than a crosshair: the pick can land anywhere in the
        // volume, at any orientation, and a flat cross drawn in world space
        // disappears the moment you rotate away from it.
        auto sphere = vtkSmartPointer<vtkSphereSource>::New();
        sphere->SetThetaResolution(16);
        sphere->SetPhiResolution(16);
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputConnection(sphere->GetOutputPort());
        m_pick3dMarkerSource = sphere;
        m_pick3dMarkerActor = vtkSmartPointer<vtkActor>::New();
        m_pick3dMarkerActor->SetMapper(mapper);
        // Not pickable: it sits exactly where the user just clicked, so the very
        // next click would hit the marker instead of the data under it.
        m_pick3dMarkerActor->PickableOff();
        auto *prop = m_pick3dMarkerActor->GetProperty();
        prop->SetColor(0.02, 0.54, 0.75);      // brand blue, as the rest of the chrome
        prop->SetAmbient(0.9);                 // readable against a dark volume
        prop->SetDiffuse(0.2);
        renderer->AddActor(m_pick3dMarkerActor);
    }

    // Size it from the cube, not in absolute units: a marker fixed at N world
    // units is invisible on a big cube and swallows a small one.
    double radius = 1.0;
    if (auto *img = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0))) {
        double b[6];
        img->GetBounds(b);
        const double diag = std::sqrt((b[1] - b[0]) * (b[1] - b[0])
                                      + (b[3] - b[2]) * (b[3] - b[2])
                                      + (b[5] - b[4]) * (b[5] - b[4]));
        if (diag > 0.)
            radius = 0.012 * diag;
    }
    m_pick3dMarkerSource->SetRadius(radius);
    m_pick3dMarkerSource->SetCenter(world[0], world[1], world[2]);
    m_pick3dMarkerSource->Modified();
    m_pick3dMarkerActor->VisibilityOn();
    ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::hidePick3dMarker()
{
    if (m_pick3dMarkerActor) {
        m_pick3dMarkerActor->VisibilityOff();
        if (ui->vtkCube && ui->vtkCube->renderWindow())
            ui->vtkCube->renderWindow()->Render();
    }
}

int vtkWindowCube::channelAtWorldZ(double worldZ) const
{
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!cubeImage)
        return -1;
    int extent[6];
    double origin[3], spacing[3];
    cubeImage->GetExtent(extent);
    cubeImage->GetOrigin(origin);
    cubeImage->GetSpacing(spacing);
    if (!(std::fabs(spacing[2]) > 0.))
        return -1;
    const int z = static_cast<int>(std::lround(extent[4] + (worldZ - origin[2]) / spacing[2]));
    return (z < extent[4] || z > extent[5]) ? -1 : z;
}

bool vtkWindowCube::pickVolumeAlongRay(vtkRenderer *renderer, const int displayPos[2],
                                       double worldOut[3], int &channelOut) const
{
    // Resolve a click on the rendering to a voxel along the view ray.
    //
    // What makes a 3-D pick worth having is not sensitivity — MIP takes a
    // maximum and Composite blends, neither integrates, so neither beats the
    // channel maps on signal-to-noise. It is that a position-position-velocity
    // rendering makes COHERENT STRUCTURE ACROSS CHANNELS recognisable: a tail or
    // a warp is a connected tilted feature in (x, y, v) and a scatter of
    // unrelated blobs in any single channel. Having spotted one, the user should
    // be able to go straight from it to a spectrum.
    //
    // The criterion is read off the blend mode rather than invented. For MIP and
    // MinIP that IS the sample the pixel shows; Composite has no single
    // producing voxel, so there it is a stated convention — the front surface.
    // The walk lives in RayVoxelPick, free of VTK so it can be tested.
    //
    // The third axis is velocity, not depth, so what a ray crosses depends on
    // the camera: down the spectral axis it crosses channels and the maximum is
    // the line peak at that sky position; side-on it crosses sky positions at
    // roughly fixed velocity. Hence channelOut — the spectrum is always the full
    // spectrum at the hit (x, y), but which channel was hit is what lets the
    // user read it.
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!renderer || !cubeImage)
        return false;

    RayVoxelPick::Grid grid;
    cubeImage->GetExtent(grid.extent);
    cubeImage->GetOrigin(grid.origin);
    cubeImage->GetSpacing(grid.spacing);
    if (!grid.valid())
        return false;

    // The ray: near-plane to far-plane through the clicked pixel.
    double near4[4], far4[4];
    renderer->SetDisplayPoint(displayPos[0], displayPos[1], 0.);
    renderer->DisplayToWorld();
    renderer->GetWorldPoint(near4);
    renderer->SetDisplayPoint(displayPos[0], displayPos[1], 1.);
    renderer->DisplayToWorld();
    renderer->GetWorldPoint(far4);
    if (!(std::fabs(near4[3]) > 0.) || !(std::fabs(far4[3]) > 0.))
        return false;
    double p0[3], p1[3];
    for (int a = 0; a < 3; ++a) {
        p0[a] = near4[a] / near4[3];
        p1[a] = far4[a] / far4[3];
    }

    using C = RayVoxelPick::Criterion;
    const C criterion = (this->m_volumeBlendMode == 1)   ? C::Maximum
                      : (this->m_volumeBlendMode == 2)   ? C::Minimum
                                                         : C::FirstAboveThreshold;
    // The threshold the OPACITY RAMP uses, not lowerBound — lowerBound is the
    // bottom of the data range, so it would stop the walk on the first voxel of
    // anything, including material the user has thresholded into invisibility.
    const double threshold = ui->lineThreshold->text().toDouble();

    // Blanks arrive here as a finite sentinel below the visible minimum, put
    // there when the cube was sanitised. isfinite() lets them through, and for
    // MinIP they are the smallest thing on any ray that crosses one.
    const double visibleMin = this->currentCubeVisibleRange[0];

    const auto res = RayVoxelPick::along(
            grid, p0, p1, criterion, threshold,
            [cubeImage](int x, int y, int z) {
                return cubeImage->GetScalarComponentAsDouble(x, y, z, 0);
            },
            [visibleMin](double v) { return v < visibleMin; });
    if (!res.hit)
        return false;
    std::copy(res.world, res.world + 3, worldOut);
    channelOut = res.voxel[2];
    return true;
}

void vtkWindowCube::handle3dPickEvent(const std::array<int, 2> &eventPos)
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

    int pos[2] = { eventPos[0], eventPos[1] };   // captured at the release (queued call)
    auto picker = vtkSmartPointer<vtkPropPicker>::New();
    const bool hitProp = (picker->Pick(pos[0], pos[1], 0., renderer) != 0);

    double world[3] = { 0., 0., 0. };
    bool haveWorld = false;
    int rayChannel = -1;   // the channel the ray was picked AT, when it came from the volume

    if (hitProp && picker->GetViewProp() == this->remoteCuttingPlaneActor.GetPointer()) {
        // Clicked the cutting plane itself: the voxel is on the displayed slice.
        picker->GetPickPosition(world);
        haveWorld = true;
    } else if (hitProp && this->currentIsosurfaceActor
               && picker->GetViewProp() == this->currentIsosurfaceActor.GetPointer()) {
        // Clicked the isosurface: the surface point IS the feature, exactly.
        picker->GetPickPosition(world);
        haveWorld = true;
        rayChannel = channelAtWorldZ(world[2]);
    } else if (m_show3dRendering && this->volume && this->volume->GetVisibility()
               && renderer->HasViewProp(this->volume)
               && pickVolumeAlongRay(renderer, pos, world, rayChannel)) {
        // Clicked the volume — and only when the volume is actually the thing on
        // screen. Marching unconditionally meant that in isosurface mode, or with
        // the 3D rendering layer switched off, a click on empty space inside the
        // cube still returned a spectrum out of data nobody was being shown.
        haveWorld = true;
    }

    if (!haveWorld) {
        return;   // outline, axes, or empty space — nothing to pick
    }

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
    if (!(std::fabs(spacing[0]) > 0.) || !(std::fabs(spacing[1]) > 0.)
        || !(std::fabs(spacing[2]) > 0.)) {
        return;
    }
    // DATASET voxels, not image-local ones. A ROI subvolume is built with its
    // extent starting at zero and its ORIGIN set to the ROI's first voxel, so
    // the world coordinate already IS the dataset index — while
    // extent[0] + (world - origin)/spacing gives the index within the ROI. On a
    // ROI starting at channel 100, a hit seven planes in was being reported as
    // channel 8, with channel 8's velocity.
    const int voxelX = static_cast<int>(std::lround(world[0] / spacing[0]));
    const int voxelY = static_cast<int>(std::lround(world[1] / spacing[1]));
    // The channel the ray actually hit, when it hit the volume or the isosurface.
    // Falling back to the displayed slice would be a lie for a volume pick: the
    // feature you clicked may live nowhere near the channel currently on the
    // cutting plane, which is the whole reason for picking in 3-D.
    const int voxelZ = (rayChannel >= 0)
            ? this->clampRemoteSliceIndex(static_cast<int>(std::lround(world[2] / spacing[2])))
            : this->clampRemoteSliceIndex(ui->spinSlice->value() - 1);
    // Bounds are checked against the DATASET, since the indices are now dataset
    // ones: origin + extent gives the ROI's span in dataset voxels.
    const int dsX0 = static_cast<int>(std::lround(origin[0] / spacing[0])) + extent[0];
    const int dsX1 = static_cast<int>(std::lround(origin[0] / spacing[0])) + extent[1];
    const int dsY0 = static_cast<int>(std::lround(origin[1] / spacing[1])) + extent[2];
    const int dsY1 = static_cast<int>(std::lround(origin[1] / spacing[1])) + extent[3];
    if (voxelX < dsX0 || voxelX > dsX1 || voxelY < dsY0 || voxelY > dsY1) {
        this->statusBar()->showMessage(u"Pick outside cube bounds."_s, 1500);
        return;
    }

    this->probeValid = true;
    // updateProbePlot() early-returns if probeModeActive==false; the 3D pick is an explicit
    // request to see the spectrum, so flip the flag here (without setProbeModeActive(), which
    // would also flip the 2D cursor and disable region/PV — out of scope for 3D pick).
    this->probeModeActive = true;
    this->probeVoxel = { voxelX, voxelY, voxelZ };
    // #5b: render into the live spectrum PANE (create it if the tool wasn't already active),
    // update it UNFROZEN, THEN freeze so a later 2D hover doesn't overwrite the picked pixel.
    this->probeFrozen = false;
    if (m_spectrumLiveId.isEmpty())
        this->startLiveSpectrum();
    auto *sliceImage =
            vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
    if (sliceImage) {
        this->updateProbeReadout(sliceImage);
    }
    this->updateProbePlot();     // updates the live pane with the picked voxel's spectrum
    this->probeFrozen = true;    // hold the pick
    // The header was written by updateProbePlot() while still unfrozen, so it read
    // "Live, hover the slice to track" over a pick that is in fact held.
    this->updateLiveSpectrumHeader();
    this->refreshProbeOverlay();
    if (this->sliceWin) {
        this->sliceWin->Render();
    }
    // Name the channel and its velocity. Which channel the ray hit is not
    // incidental: with the camera down the spectral axis it is the line peak at
    // that sky position, side-on it is wherever the ray crossed. The user has to
    // be told which, or the spectrum cannot be read.
    const QString where = (rayChannel >= 0)
            ? u"Spectrum at voxel (%1, %2); the ray hit channel %3 (%4)"_s
                      .arg(voxelX).arg(voxelY).arg(voxelZ + 1)
                      .arg(this->formatSpectralAxisValue(voxelZ))
            : u"Spectrum extracted at voxel (%1, %2), slice %3"_s
                      .arg(voxelX).arg(voxelY).arg(voxelZ + 1);
    this->statusBar()->showMessage(where, 6000);
    this->showPick3dMarker(world);   // mark WHERE the click landed, in the 3-D view
}

void vtkWindowCube::updateCuttingPlaneOpacityRowVisibility()
{
    if (!m_layerRowCuttingPlaneOpacity)
        return;
    const ViewId kind =
            (m_activePane >= 0 && m_activePane < 4) ? m_slots[m_activePane].view.kind : ViewId::None;
    m_layerRowCuttingPlaneOpacity->setVisible(kind == ViewId::View3D && actionShowCuttingPlane
                                              && actionShowCuttingPlane->isChecked());
}

void vtkWindowCube::apply3dRenderingVisibility()
{
    // Applied to BOTH props, not just the mounted one: the mode can change while
    // the layer is off, and the newly mounted prop has to come up hidden too.
    this->volume->SetVisibility(m_show3dRendering ? 1 : 0);
    if (this->currentIsosurfaceActor)
        this->currentIsosurfaceActor->SetVisibility(m_show3dRendering ? 1 : 0);
    if (ui->vtkCube && ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();

    // And the server, when it is the one drawing. Hiding only the local props
    // would leave the remote stream showing the rendering the user just turned
    // off, with no clue that the toggle had gone nowhere.
    QVariantMap m;
    m[u"type"_s] = u"set_show_rendering"_s;
    m[u"visible"_s] = m_show3dRendering;
    this->sendRemoteRenderStateCommand(m);
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
    // The isosurface fetch is async; if the representation was switched back to
    // Volume (by the user OR a linked-view update) while it was in flight, discard
    // the stale result instead of forcing this window back into isosurface mode.
    if (ui->actionIsosurface && !ui->actionIsosurface->isChecked()) {
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
    apply3dRenderingVisibility();   // a fresh isosurface must honour the layer toggle
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
