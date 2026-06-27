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

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr int overlayTickCount = 5;
constexpr double polygonClosureTolerance = 3.0;
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

double distance2d(const std::array<int, 2> &a, const std::array<int, 2> &b)
{
    const double dx = static_cast<double>(a[0] - b[0]);
    const double dy = static_cast<double>(a[1] - b[1]);
    return std::sqrt(dx * dx + dy * dy);
}

void buildAnnulusFill(vtkPoints *points, vtkCellArray *cells, vtkPolyData *polyData,
                      const std::array<int, 2> &center, double innerRadius, double outerRadius,
                      int sides = 96)
{
    points->Reset();
    cells->Reset();
    if (!polyData || !points || !cells || outerRadius <= 0.0 || innerRadius < 0.0
        || innerRadius >= outerRadius) {
        if (polyData) {
            polyData->Modified();
        }
        return;
    }

    const double cx = static_cast<double>(center[0]);
    const double cy = static_cast<double>(center[1]);
    const double twoPi = 2.0 * std::acos(-1.0);
    for (int i = 0; i < sides; ++i) {
        const double theta = twoPi * static_cast<double>(i) / static_cast<double>(sides);
        const double cosTheta = std::cos(theta);
        const double sinTheta = std::sin(theta);
        points->InsertNextPoint(cx + outerRadius * cosTheta, cy + outerRadius * sinTheta, 0.0);
        points->InsertNextPoint(cx + innerRadius * cosTheta, cy + innerRadius * sinTheta, 0.0);
    }

    cells->InsertNextCell(static_cast<vtkIdType>(2 * (sides + 1)));
    for (int i = 0; i <= sides; ++i) {
        const int wrapped = i % sides;
        const vtkIdType outer = static_cast<vtkIdType>(2 * wrapped);
        const vtkIdType inner = outer + 1;
        cells->InsertCellPoint(outer);
        cells->InsertCellPoint(inner);
    }

    polyData->SetPoints(points);
    polyData->SetPolys(nullptr);
    polyData->SetStrips(cells);
    polyData->Modified();
}

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

bool axisHasAnyMetadata(const QString &ctype, const QString &cunit, double crval, double crpix, double cdelt)
{
    return !ctype.trimmed().isEmpty() || !cunit.trimmed().isEmpty() || std::isfinite(crval)
            || std::isfinite(crpix) || std::isfinite(cdelt);
}

bool axisHasLinearWcs(const QString &ctype, double crval, double crpix, double cdelt)
{
    return !ctype.trimmed().isEmpty() && std::isfinite(crval) && std::isfinite(crpix)
            && std::isfinite(cdelt) && std::abs(cdelt) > 1e-12;
}

bool isCelestialLikeAxis(const QString &ctypeRaw)
{
    const QString ctype = ctypeRaw.trimmed().toUpper();
    return ctype.startsWith(u"RA"_s) || ctype.startsWith(u"DEC"_s) || ctype.startsWith(u"GLON"_s)
            || ctype.startsWith(u"GLAT"_s) || ctype.startsWith(u"ELON"_s)
            || ctype.startsWith(u"ELAT"_s);
}

bool pointInBox(const std::array<int, 2> &anchor, const std::array<int, 2> &current, int x, int y)
{
    const int xmin = std::min(anchor[0], current[0]);
    const int xmax = std::max(anchor[0], current[0]);
    const int ymin = std::min(anchor[1], current[1]);
    const int ymax = std::max(anchor[1], current[1]);
    return x >= xmin && x <= xmax && y >= ymin && y <= ymax;
}

bool pointInCircle(const std::array<int, 2> &anchor, const std::array<int, 2> &current, int x, int y)
{
    const double dx = static_cast<double>(current[0] - anchor[0]);
    const double dy = static_cast<double>(current[1] - anchor[1]);
    const double radius = std::sqrt(dx * dx + dy * dy);
    if (radius <= 0.0) {
        return x == anchor[0] && y == anchor[1];
    }
    const double px = static_cast<double>(x - anchor[0]);
    const double py = static_cast<double>(y - anchor[1]);
    return (px * px + py * py) <= radius * radius;
}

bool pointInPolygon(const std::vector<std::array<int, 2>> &vertices, int x, int y)
{
    if (vertices.size() < 3) {
        return false;
    }

    bool inside = false;
    const double px = static_cast<double>(x) + 0.5;
    const double py = static_cast<double>(y) + 0.5;
    for (std::size_t i = 0, j = vertices.size() - 1; i < vertices.size(); j = i++) {
        const double xi = static_cast<double>(vertices[i][0]) + 0.5;
        const double yi = static_cast<double>(vertices[i][1]) + 0.5;
        const double xj = static_cast<double>(vertices[j][0]) + 0.5;
        const double yj = static_cast<double>(vertices[j][1]) + 0.5;
        const bool intersects = ((yi > py) != (yj > py))
                && (px < (xj - xi) * (py - yi) / ((yj - yi) == 0.0 ? 1e-12 : (yj - yi)) + xi);
        if (intersects) {
            inside = !inside;
        }
    }
    return inside;
}

bool pointInAnnulus(const std::array<int, 2> &center, const std::array<int, 2> &outerPoint,
                    double innerRadius, int x, int y)
{
    const double outerDx = static_cast<double>(outerPoint[0] - center[0]);
    const double outerDy = static_cast<double>(outerPoint[1] - center[1]);
    const double outerRadius = std::sqrt(outerDx * outerDx + outerDy * outerDy);
    if (outerRadius <= 0.0) {
        return false;
    }

    const double px = static_cast<double>(x - center[0]);
    const double py = static_cast<double>(y - center[1]);
    const double radius = std::sqrt(px * px + py * py);
    return radius >= std::max(0.0, innerRadius) && radius <= outerRadius;
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


void configureAxisActor(vtkAxisActor2D *axis, double x1, double y1, double x2, double y2)
{
    axis->GetPoint1Coordinate()->SetCoordinateSystemToDisplay();
    axis->GetPoint2Coordinate()->SetCoordinateSystemToDisplay();
    axis->GetPoint1Coordinate()->SetValue(x1, y1);
    axis->GetPoint2Coordinate()->SetValue(x2, y2);
    axis->SetNumberOfLabels(5);
    axis->AdjustLabelsOn();
    axis->SetLabelFormat("%-#6.4g");
    axis->SetFontFactor(0.6);
    axis->SetTickLength(6);
    axis->AxisVisibilityOn();
    axis->TickVisibilityOn();
    axis->LabelVisibilityOn();
    axis->TitleVisibilityOn();
    axis->GetProperty()->SetColor(1., 1., 1.);
    axis->GetLabelTextProperty()->SetColor(1., 1., 1.);
    axis->GetTitleTextProperty()->SetColor(1., 1., 1.);
    axis->GetLabelTextProperty()->SetFontSize(14);
    axis->GetTitleTextProperty()->SetFontSize(16);
}

void configureVerticalAxisTitle(vtkAxisActor2D *axis)
{
    auto *titleProp = axis->GetTitleTextProperty();
    titleProp->SetOrientation(90.);
    titleProp->SetJustificationToCentered();
    titleProp->SetVerticalJustificationToCentered();
    titleProp->SetColor(1., 1., 1.);
    titleProp->SetFontSize(14);
    axis->SetTitlePosition(0.5);
}

QString upperCtype(const QString &value)
{
    return value.trimmed().toUpper();
}

QString cleanAxisUnit(const QString &value)
{
    return value.trimmed();
}

QString spectralAxisKindLabel(vtkWindowCube::SpectralAxisKind kind)
{
    switch (kind) {
    case vtkWindowCube::SpectralAxisKind::Frequency:
        return u"Frequency"_s;
    case vtkWindowCube::SpectralAxisKind::RadioVelocity:
        return u"Radio Velocity"_s;
    case vtkWindowCube::SpectralAxisKind::OpticalVelocity:
        return u"Optical Velocity"_s;
    case vtkWindowCube::SpectralAxisKind::GenericVelocity:
        return u"Velocity"_s;
    case vtkWindowCube::SpectralAxisKind::GenericSpectral:
        return u"Spectral Axis"_s;
    case vtkWindowCube::SpectralAxisKind::Channel:
    default:
        return u"Channel"_s;
    }
}

vtkWindowCube::SpectralAxisDescriptor inferSpectralAxisDescriptor(const QString &ctypeRaw,
                                                                  const QString &unitRaw,
                                                                  bool hasPhysicalCoordinates)
{
    vtkWindowCube::SpectralAxisDescriptor descriptor;
    const QString ctype = upperCtype(ctypeRaw);
    descriptor.unit = cleanAxisUnit(unitRaw);
    descriptor.sourceLabel = ctypeRaw.trimmed();

    if (!hasPhysicalCoordinates) {
        descriptor.kind = vtkWindowCube::SpectralAxisKind::Channel;
        descriptor.label = u"Channel"_s;
        descriptor.sourceLabel = u"Channel"_s;
        return descriptor;
    }

    descriptor.physical = true;
    if (ctype.startsWith(u"FREQ"_s)) {
        descriptor.kind = vtkWindowCube::SpectralAxisKind::Frequency;
        descriptor.label = u"Frequency"_s;
        descriptor.trusted = true;
    } else if (ctype.startsWith(u"VRAD"_s)) {
        descriptor.kind = vtkWindowCube::SpectralAxisKind::RadioVelocity;
        descriptor.label = u"Radio Velocity"_s;
        descriptor.trusted = true;
    } else if (ctype.startsWith(u"VOPT"_s) || ctype.startsWith(u"FELO"_s)) {
        descriptor.kind = vtkWindowCube::SpectralAxisKind::OpticalVelocity;
        descriptor.label = u"Optical Velocity"_s;
        descriptor.trusted = true;
    } else if (ctype.startsWith(u"VELO"_s) || ctype.contains(u"VELO"_s)) {
        descriptor.kind = vtkWindowCube::SpectralAxisKind::GenericVelocity;
        descriptor.label = u"Velocity"_s;
        descriptor.inferred = true;
    } else if (ctype.contains(u"CHAN"_s) || ctype == u"CHANNEL"_s) {
        descriptor.kind = vtkWindowCube::SpectralAxisKind::Channel;
        descriptor.label = u"Channel"_s;
        descriptor.sourceLabel = u"Channel"_s;
        descriptor.physical = false;
    } else {
        descriptor.kind = vtkWindowCube::SpectralAxisKind::GenericSpectral;
        descriptor.label = ctypeRaw.trimmed().isEmpty() ? u"Spectral Axis"_s : ctypeRaw.trimmed();
        descriptor.inferred = !ctypeRaw.trimmed().isEmpty();
    }

    if (descriptor.sourceLabel.isEmpty()) {
        descriptor.sourceLabel = spectralAxisKindLabel(descriptor.kind);
    }
    return descriptor;
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

int inferCelestialFrameFromCtypePair(const std::array<QString, 3> &ctype)
{
    const QString c1 = upperCtype(ctype[0]);
    const QString c2 = upperCtype(ctype[1]);
    if (c1.startsWith(u"GLON"_s) && c2.startsWith(u"GLAT"_s)) {
        return WCS_GALACTIC;
    }
    if (c1.startsWith(u"ELON"_s) && c2.startsWith(u"ELAT"_s)) {
        return WCS_ECLIPTIC;
    }
    if (c1.startsWith(u"RA"_s) && c2.startsWith(u"DEC"_s)) {
        return WCS_J2000;
    }
    return -1;
}

QString formatCelestialCoordinate(int frame, int axis, double value)
{
    char buffer[64] = { 0 };
    if (frame == WCS_J2000 && axis == 0) {
        ra2str(buffer, sizeof(buffer), value, 1);
    } else {
        dec2str(buffer, sizeof(buffer), value, axis == 0 ? 1 : 0);
    }
    return QString::fromLatin1(buffer).trimmed();
}

void configureTickLabelActor(vtkTextActor *actor, bool rightAligned)
{
    actor->GetTextProperty()->SetColor(1., 1., 1.);
    actor->GetTextProperty()->SetFontSize(12);
    actor->GetTextProperty()->SetVerticalJustificationToCentered();
    if (rightAligned) {
        actor->GetTextProperty()->SetJustificationToRight();
    } else {
        actor->GetTextProperty()->SetJustificationToCentered();
    }
}

vtkSmartPointer<vtkImageData> createPlaceholderImageData()
{
    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, 0, 0, 0, 0, 0);
    image->AllocateScalars(VTK_FLOAT, 1);
    image->SetScalarComponentFromFloat(0, 0, 0, 0, 0.f);
    return image;
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





void refitParallelSliceCamera(vtkRenderer *renderer, vtkImageData *sliceImage, vtkRenderWindow *win)
{
    if (!renderer || !sliceImage || !win) {
        return;
    }

    double bounds[6];
    sliceImage->GetBounds(bounds);
    if (!validBounds(bounds)) {
        return;
    }

    auto *camera = renderer->GetActiveCamera();
    double position[3];
    double focalPoint[3];
    camera->GetPosition(position);
    camera->GetFocalPoint(focalPoint);

    const double center[3] = { 0.5 * (bounds[0] + bounds[1]), 0.5 * (bounds[2] + bounds[3]),
                               0.5 * (bounds[4] + bounds[5]) };
    const double offset[3] = { position[0] - focalPoint[0], position[1] - focalPoint[1],
                               position[2] - focalPoint[2] };

    camera->SetFocalPoint(center);
    camera->SetPosition(center[0] + offset[0], center[1] + offset[1], center[2] + offset[2]);

    const int *size = win->GetSize();
    const double aspect = (size && size[1] > 0) ? static_cast<double>(size[0]) / size[1] : 1.0;
    const double width = std::max(1e-6, bounds[1] - bounds[0]);
    const double height = std::max(1e-6, bounds[3] - bounds[2]);
    constexpr double margin = 1.1;
    camera->SetParallelScale(
            margin * std::max(height * 0.5, width * 0.5 / std::max(1.0, aspect)));
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

vtkSmartPointer<vtkImageData> decodeRemoteSlice(const QByteArray &data, int width, int height)
{
    const qsizetype expectedBytes =
            static_cast<qsizetype>(width) * height * static_cast<qsizetype>(sizeof(float));
    if (width <= 0 || height <= 0 || data.size() != expectedBytes) {
        return nullptr;
    }

    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, width - 1, 0, height - 1, 0, 0);
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

// ─────────────────────────────────────────────────────────────────────────────
// Image statistics helpers
// ─────────────────────────────────────────────────────────────────────────────

// Compute finite-pixel mean and RMS from any vtkImageData scalar array.
// Outputs are set to quiet_NaN when the image is null or has no finite pixels.
static void computeImageStats(vtkImageData *img, double &meanOut, double &rmsOut)
{
    constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
    meanOut = qnan;
    rmsOut  = qnan;
    if (!img) return;
    vtkDataArray *scalars = img->GetPointData()->GetScalars();
    if (!scalars) return;
    const vtkIdType n = scalars->GetNumberOfTuples();
    double sum = 0.0, sumSq = 0.0;
    vtkIdType count = 0;
    for (vtkIdType i = 0; i < n; ++i) {
        const double v = scalars->GetComponent(i, 0);
        if (!std::isfinite(v)) continue;
        sum   += v;
        sumSq += v * v;
        ++count;
    }
    if (count == 0) return;
    meanOut = sum / static_cast<double>(count);
    rmsOut  = std::sqrt(sumSq / static_cast<double>(count));
}

// Format a scalar statistic value for display.
// Uses 6 significant digits in compact notation; returns "—" for non-finite values.
static QString fmtStat(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 6) : u"\u2014"_s;
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

RemoteCubeSliceResult fetchRemoteSlice(const QString &backendUrl, const QString &datasetId,
                                       int index, const QString &sessionId,
                                       const QString &backendToken)
{
    RemoteCubeSliceResult result;
    result.index = index;

    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    const auto response = client.requestSlice(datasetId, QStringLiteral("z"), index);
    if (!response.valid) {
        result.errorMessage = response.error.isEmpty() ? u"Remote slice request failed."_s
                                                       : response.error;
        return result;
    }

    if (response.scalarType != u"float32"_s) {
        result.errorMessage = u"Unsupported remote slice scalar type."_s;
        return result;
    }

    result.imageData = decodeRemoteSlice(response.data, response.width, response.height);
    if (!result.imageData) {
        result.errorMessage = u"Invalid remote slice payload."_s;
        return result;
    }

    result.valid = true;
    result.imageRange = { response.rangeMin, response.rangeMax };
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
    this->hoverReadoutLabel->setStyleSheet(u"QLabel { padding-left: 8px; }"_s);
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
    ui->menuView->addAction(this->dockCubeView->toggleViewAction());
    ui->menuView->addAction(this->dockSliceView->toggleViewAction());
    ui->menuView->addSeparator();
    this->actionResetLayout = new QAction(u"Reset Layout"_s, this);
    ui->menuView->addAction(this->actionResetLayout);
    QObject::connect(this->actionResetLayout, &QAction::triggered,
                     this, &vtkWindowCube::resetWorkspaceLayout);

    // ── View → Rendering submenu (SSR mode toggle) ────────────────────────────
    this->setupRenderModeUi();

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
        m_uiAssembler->syncVolumeModeSidebar(idx);
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
                   const MomentMapController::Config & /*config*/,
                   const QString &summary,
                   const QString &details) {
                QElapsedTimer applyTimer;
                applyTimer.start();
                this->applyMomentMapResult(
                        { result.imageData, { result.imageRange[0], result.imageRange[1] } });
                qDebug().noquote()
                        << QStringLiteral("[perf][moment] UI apply: %1 ms")
                                   .arg(applyTimer.elapsed());

                this->momentProvenanceState.valid = true;
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
                                if (ui->vtkImage->renderWindow())
                                    ui->vtkImage->renderWindow()->Render();
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
                if (ui->vtkImage->renderWindow())
                    ui->vtkImage->renderWindow()->Render();

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
}

vtkWindowCube::~vtkWindowCube()
{
    // Stop the slice animation timer and detach any vtk observer we installed,
    // so we don't fire callbacks after the widget hierarchy starts going away.
    this->sliceAnimationTimer.stop();
    this->remove3dPickObserver();
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

void vtkWindowCube::setupViewerToolbar()
{
    // Layout B principle: a thin horizontal "screen header" above the central
    // area showing the file context (path-pill), data tags, and quick actions
    // (Find / Command / Export). Hosted via QMainWindow::addToolBar so we
    // don't have to restructure the .ui file's central widget — the toolbar
    // sits between the menu bar and centralwidget by default.
    VisivoTheme::ViewerToolbarSpec spec;
    spec.filePath = this->filepath;
    spec.primaryTag = QStringLiteral("CUBE · %1×%2×%3")
                              .arg(this->remoteDatasetWidth)
                              .arg(this->remoteDatasetHeight)
                              .arg(this->remoteDatasetDepth);
    spec.secondaryTag = QStringLiteral("remote · full-res");

    QToolButton *findBtn = nullptr;
    QToolButton *cmdBtn  = nullptr;
    QToolButton *expBtn  = nullptr;
    QToolButton *tagBtn  = nullptr;
    QWidget *bar = VisivoTheme::makeViewerToolbar(
            spec, &findBtn, &cmdBtn, &expBtn, &tagBtn, this);

    // Click on the "CUBE · …" tag opens the FITS header viewer.
    if (tagBtn) {
        QObject::connect(tagBtn, &QToolButton::clicked, this, [this]() {
            this->showFitsHeaderDialog();
        });
    }

    auto *qtBar = new QToolBar(this);
    qtBar->setObjectName(u"viewerToolbarHost"_s);
    qtBar->setMovable(false);
    qtBar->setFloatable(false);
    qtBar->setContextMenuPolicy(Qt::PreventContextMenu);
    // Strip QToolBar's default chrome — let the inner widget supply its own.
    qtBar->setStyleSheet(
            QStringLiteral(
                    "QToolBar#viewerToolbarHost {"
                    "  background-color: transparent;"
                    "  border: none;"
                    "  padding: 6px 10px 4px 10px;"
                    "  spacing: 0px;"
                    "}"));
    qtBar->addWidget(bar);
    this->addToolBar(Qt::TopToolBarArea, qtBar);

    // The Find/Command/Export buttons are deliberately left unwired for now —
    // they read as visual affordances. Find triggers Edit→Find, Command opens
    // the future ⌘K palette, Export hooks into the existing export menu.
    Q_UNUSED(findBtn);
    Q_UNUSED(cmdBtn);
    Q_UNUSED(expBtn);
}

void vtkWindowCube::setupSidebar()
{
    // The UI file creates a hand-rolled sidebarContainer in mainHLayout.
    // Remove it from the layout and replace it with SidebarPanel, reusing the
    // four page content widgets (they are reparented into SidebarPanel's scroll areas).
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->sidebarContainer);
    ui->sidebarContainer->hide();

    // Construct the assembler (owns all sidebar widget pointers).
    m_uiAssembler = std::make_unique<CubeUiAssembler>(ui);

    // Build pages — each method reparents ui->* widgets and stores QPointers.
    QWidget *page3dSettings = m_uiAssembler->buildThresholdPage();
    QWidget *page2dSettings = m_uiAssembler->build2dViewPage();
    QWidget *pageTools      = m_uiAssembler->buildToolsPage();

    // ── Append "Export & Inspection" card to the Tools page so every menu
    // action that has no representation in the assembler-built cards is
    // also accessible from the sidebar (keeps menu ↔ sidebar coherent).
    if (auto *toolsLayout = qobject_cast<QVBoxLayout *>(pageTools->layout())) {
        const QString cardStyle   = VisivoTheme::sidebarCardStyleSheet();
        const QString subHdrStyle = VisivoTheme::sidebarSubHeaderStyleSheet();
        const QString toolBtnStyle = VisivoTheme::sidebarToolButtonStyleSheet();
        const auto makeBtn = [&toolBtnStyle](QAction *act, QWidget *parent) {
            auto *b = new QToolButton(parent);
            b->setDefaultAction(act);
            b->setStyleSheet(toolBtnStyle);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setToolButtonStyle(Qt::ToolButtonTextOnly);
            return b;
        };

        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *hdr = new QLabel(u"Export & Inspection"_s, card);
        hdr->setStyleSheet(subHdrStyle);
        cl->addWidget(hdr);

        if (m_actExportSubCube)     cl->addWidget(makeBtn(m_actExportSubCube,     card));
        if (m_actExportChannel2d)   cl->addWidget(makeBtn(m_actExportChannel2d,   card));
        if (m_actExportMomentFits)  cl->addWidget(makeBtn(m_actExportMomentFits,  card));
        if (m_actSliceHistogram)    cl->addWidget(makeBtn(m_actSliceHistogram,    card));
        if (m_actSendSliceToImage)  cl->addWidget(makeBtn(m_actSendSliceToImage,  card));

        // Insert before any trailing stretch the assembler may have added.
        int insertIdx = toolsLayout->count();
        for (int i = toolsLayout->count() - 1; i >= 0; --i) {
            if (toolsLayout->itemAt(i)->spacerItem()) { insertIdx = i; break; }
        }
        toolsLayout->insertWidget(insertIdx, card);
    }

    CubeUiAssembler::InfoStatsInput infoIn;
    infoIn.filepath         = this->filepath;
    infoIn.datasetWidth     = this->remoteDatasetWidth;
    infoIn.datasetHeight    = this->remoteDatasetHeight;
    infoIn.datasetDepth     = this->remoteDatasetDepth;
    infoIn.wcsFrameLabel    = this->currentWcsFrameLabel();
    infoIn.spectralAxisTitle = this->spectralAxisTitle();
    infoIn.wcsStatus        = this->remoteWcsStatus;
    infoIn.wcsWarningMessage = this->remoteWcsWarningMessage;
    QWidget *pageInfoStats  = m_uiAssembler->buildInfoStatsPage(infoIn);

    // Capture the default LUT names chosen by the assembler.
    this->m_activeColorMapName  = m_uiAssembler->lutCombo()
                                          ? m_uiAssembler->lutCombo()->currentText() : QString();
    this->m_activeSliceLutName  = m_uiAssembler->sliceLutCombo()
                                          ? m_uiAssembler->sliceLutCombo()->currentText() : QString();
    this->m_activeMomentLutName = m_uiAssembler->momentLutCombo()
                                          ? m_uiAssembler->momentLutCombo()->currentText() : QString();

    // ── Extras → distributed across 3D View / 2D View settings ───────────────
    // What used to live in a separate "Cube Extras" tab is now redistributed
    // where it semantically belongs: 3D-scoped affordances (cutting-plane
    // visibility, 3D WCS axes, click-to-pick spectrum on the plane) go into
    // the 3D View Settings tab; the slice animator goes into 2D View Settings.
    // Visual style matches the rest of the sidebar via CubeUiAssembler's
    // shared header / card QSS. All widgets remain *mirrors* of the existing
    // QAction objects (menu + sidebar share one source of truth).
    const auto buildSection = [](const QString &title, QWidget **outCardOpt)
            -> std::pair<QLabel*, QVBoxLayout*> {
        auto *hdr = new QLabel(title);
        hdr->setStyleSheet(CubeUiAssembler::sidebarHdrStyle());
        auto *card = new QFrame();
        card->setStyleSheet(CubeUiAssembler::sidebarCardStyle());
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);
        if (outCardOpt) *outCardOpt = card;
        return { hdr, cl };
    };
    // Append a section (header label + card frame) to a page, keeping the
    // trailing addStretch() at the very bottom so layouts don't collapse.
    const auto appendToPage = [](QWidget *page, QLabel *hdr, QWidget *card) {
        auto *root = qobject_cast<QVBoxLayout *>(page->layout());
        if (!root) return;
        QLayoutItem *trailingStretch = root->takeAt(root->count() - 1);
        root->addWidget(hdr);
        root->addWidget(card);
        if (trailingStretch) root->addItem(trailingStretch);
    };

    // ─── Section: Cutting Plane (→ 3D View Settings) ────────────────────────
    // The "Show cutting plane" toggle is a full-width QToolButton bound to
    // its QAction via setDefaultAction() — same visual treatment as Show
    // WCS Axes / Show Contours (selectable button instead of a checkbox).
    // The opacity row mirrors RENDERING THRESHOLD: full-width slider with
    // a read-only QLineEdit beneath showing the current percentage.
    QSlider   *opacitySlider = nullptr;
    QLineEdit *opacityVal = nullptr;
    {
        QWidget *card = nullptr;
        auto [hdr, cl] = buildSection(u"CUTTING PLANE"_s, &card);
        auto *planeShowBtn = CubeUiAssembler::makeSidebarButton(u"Show cutting plane"_s);
        planeShowBtn->setEnabled(true);
        planeShowBtn->setDefaultAction(this->actionShowCuttingPlane);
        cl->addWidget(planeShowBtn);

        opacitySlider = new QSlider(Qt::Horizontal, card);
        opacitySlider->setRange(0, 100);
        opacitySlider->setValue(50);
        opacitySlider->setSingleStep(5);
        opacitySlider->setPageStep(25);
        opacitySlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        opacitySlider->setToolTip(u"Cutting plane opacity (0–100%)."_s);
        cl->addWidget(opacitySlider);

        opacityVal = new QLineEdit(u"50 %"_s, card);
        opacityVal->setReadOnly(true);
        opacityVal->setFocusPolicy(Qt::NoFocus);
        opacityVal->setAlignment(Qt::AlignCenter);
        opacityVal->setToolTip(u"Current cutting plane opacity."_s);
        cl->addWidget(opacityVal);
        appendToPage(page3dSettings, hdr, card);
    }

    // ─── Section: 3D Reference (→ 3D View Settings) ─────────────────────────
    {
        QWidget *card = nullptr;
        auto [hdr, cl] = buildSection(u"3D REFERENCE"_s, &card);
        auto *axesBtn = CubeUiAssembler::makeSidebarButton(u"Show 3D WCS axes"_s);
        axesBtn->setEnabled(true);
        axesBtn->setDefaultAction(this->actionShow3dWcsAxes);
        cl->addWidget(axesBtn);
        appendToPage(page3dSettings, hdr, card);
    }

    // ─── Section: 3D Interaction (→ 3D View Settings) ───────────────────────
    {
        QWidget *card = nullptr;
        auto [hdr, cl] = buildSection(u"3D INTERACTION"_s, &card);
        auto *pickBtn = CubeUiAssembler::makeSidebarButton(u"Pick spectrum on plane click"_s);
        pickBtn->setEnabled(true);
        pickBtn->setDefaultAction(this->actionPickSpectrum3d);
        pickBtn->setToolTip(
                u"Click the cutting plane in the 3D view to extract the spectral profile at "
                u"that (RA, DEC) position into the probe plot."_s);
        cl->addWidget(pickBtn);
        appendToPage(page3dSettings, hdr, card);
    }

    // ─── Section: Slice Animation (→ 2D View Settings) ──────────────────────
    QPushButton *playBtn = nullptr;
    QComboBox   *fpsCombo = nullptr;
    QComboBox   *modeCombo = nullptr;
    {
        QWidget *card = nullptr;
        auto [hdr, cl] = buildSection(u"SLICE ANIMATION"_s, &card);
        playBtn = new QPushButton(u"▶  Play"_s, card);
        playBtn->setCheckable(true);
        playBtn->setChecked(this->actionPlaySlices->isChecked());
        cl->addWidget(playBtn);
        auto *fpsRow = new QHBoxLayout;
        fpsRow->setSpacing(6);
        fpsRow->addWidget(new QLabel(u"Speed"_s, card));
        // Predefined fps values only — matches the View › Animation Speed
        // menu radios and prevents the user typing a weird value the menu
        // can't reflect. Editable=false ensures no free-form entry.
        fpsCombo = new QComboBox(card);
        for (int fps : { 2, 5, 10, 15, 30 }) {
            fpsCombo->addItem(u"%1 fps"_s.arg(fps), fps);
        }
        const int initFpsIdx = fpsCombo->findData(this->animationFps);
        fpsCombo->setCurrentIndex(initFpsIdx >= 0 ? initFpsIdx : fpsCombo->findData(5));
        fpsRow->addWidget(fpsCombo, /*stretch=*/1);
        cl->addLayout(fpsRow);
        auto *modeRow = new QHBoxLayout;
        modeRow->setSpacing(6);
        modeRow->addWidget(new QLabel(u"Mode"_s, card));
        modeCombo = new QComboBox(card);
        modeCombo->addItem(u"Loop"_s,        static_cast<int>(AnimationMode::Loop));
        modeCombo->addItem(u"Bounce"_s,      static_cast<int>(AnimationMode::Bounce));
        modeCombo->addItem(u"Stop at End"_s, static_cast<int>(AnimationMode::Once));
        modeCombo->setCurrentIndex(modeCombo->findData(static_cast<int>(this->animationMode)));
        modeRow->addWidget(modeCombo, /*stretch=*/1);
        cl->addLayout(modeRow);
        appendToPage(page2dSettings, hdr, card);
    }

    // The three checkable toggles (Show cutting plane, Show 3D WCS axes,
    // Pick spectrum on plane click) are bound to their QAction via
    // QToolButton::setDefaultAction() in the section blocks above — Qt
    // takes care of bidirectional state sync, no manual wiring needed.

    // Play button ↔ actionPlaySlices, and label flips ▶/⏸
    const auto refreshPlayLabel = [playBtn](bool playing) {
        playBtn->setText(playing ? u"⏸  Pause"_s : u"▶  Play"_s);
    };
    refreshPlayLabel(playBtn->isChecked());
    QObject::connect(this->actionPlaySlices, &QAction::toggled, playBtn,
                     [playBtn, refreshPlayLabel](bool v) {
                         if (playBtn->isChecked() != v) {
                             const QSignalBlocker block(playBtn);
                             playBtn->setChecked(v);
                         }
                         refreshPlayLabel(v);
                     });
    QObject::connect(playBtn, &QPushButton::toggled, this->actionPlaySlices,
                     [this](bool v) {
                         if (this->actionPlaySlices->isChecked() != v) {
                             this->actionPlaySlices->setChecked(v);
                         }
                     });

    // Opacity slider ↔ cuttingPlaneOpacityGroup.
    QObject::connect(opacitySlider, &QSlider::valueChanged, this,
                     [this, opacityVal](int pct) {
                         opacityVal->setText(u"%1 %"_s.arg(pct));
                         this->setCuttingPlaneOpacity(pct / 100.0);
                         if (this->cuttingPlaneOpacityGroup) {
                             for (auto *a : this->cuttingPlaneOpacityGroup->actions()) {
                                 if (a->data().toInt() == pct) {
                                     const QSignalBlocker block(a);
                                     a->setChecked(true);
                                     break;
                                 }
                             }
                         }
                     });
    QObject::connect(this->cuttingPlaneOpacityGroup, &QActionGroup::triggered, opacitySlider,
                     [opacitySlider](QAction *a) {
                         const int pct = a->data().toInt();
                         if (opacitySlider->value() != pct) {
                             const QSignalBlocker block(opacitySlider);
                             opacitySlider->setValue(pct);
                         }
                     });

    // FPS combo ↔ animationFpsGroup. Only preset values are exposed; the
    // combo's currentData carries the fps int — no free-form entry possible.
    QObject::connect(fpsCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this, fpsCombo](int) {
                         const int v = fpsCombo->currentData().toInt();
                         this->animationFps = std::max(1, v);
                         if (this->sliceAnimationTimer.isActive()) {
                             this->sliceAnimationTimer.start(
                                     std::max(1, 1000 / this->animationFps));
                         }
                         if (this->animationFpsGroup) {
                             for (auto *a : this->animationFpsGroup->actions()) {
                                 if (a->data().toInt() == v) {
                                     const QSignalBlocker block(a);
                                     a->setChecked(true);
                                     return;
                                 }
                             }
                         }
                     });
    QObject::connect(this->animationFpsGroup, &QActionGroup::triggered, fpsCombo,
                     [fpsCombo](QAction *a) {
                         const int v = a->data().toInt();
                         const int idx = fpsCombo->findData(v);
                         if (idx >= 0 && fpsCombo->currentIndex() != idx) {
                             const QSignalBlocker block(fpsCombo);
                             fpsCombo->setCurrentIndex(idx);
                         }
                     });

    // Mode combo ↔ animationModeGroup
    QObject::connect(modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this, modeCombo](int) {
                         const auto m = static_cast<AnimationMode>(
                                 modeCombo->currentData().toInt());
                         this->animationMode = m;
                         this->animationDirection = +1;
                         if (this->animationModeGroup) {
                             for (auto *a : this->animationModeGroup->actions()) {
                                 if (a->data().toInt() == static_cast<int>(m)) {
                                     const QSignalBlocker block(a);
                                     a->setChecked(true);
                                     break;
                                 }
                             }
                         }
                     });
    QObject::connect(this->animationModeGroup, &QActionGroup::triggered, modeCombo,
                     [modeCombo](QAction *a) {
                         const int v = a->data().toInt();
                         const int idx = modeCombo->findData(v);
                         if (idx >= 0 && modeCombo->currentIndex() != idx) {
                             const QSignalBlocker block(modeCombo);
                             modeCombo->setCurrentIndex(idx);
                         }
                     });

    this->m_sidebar = new SidebarPanel(220, this);
    this->m_sidebar->addPage(u":/icons/rail_3d.svg"_s,    u"3D View Settings"_s, page3dSettings);
    this->m_sidebar->addPage(u":/icons/rail_slice.svg"_s, u"2D View Settings"_s, page2dSettings);
    this->m_sidebar->addPage(u":/icons/rail_filter.svg"_s, u"Tools"_s,           pageTools);
    this->m_sidebar->addPage(u":/icons/rail_stats.svg"_s, u"Inspector"_s,        pageInfoStats);
    this->m_sidebar->finalize(0);
    mainLayout->insertWidget(0, this->m_sidebar);

    // ── Bidirectional sync: rendering mode combo ↔ menu actions ──────────────
    QObject::connect(m_uiAssembler->renderModeCombo(), &SegmentedToggle::currentIndexChanged,
                     this, [this](int idx) {
        const bool wantIsosurface = (idx == 0);
        if (wantIsosurface == ui->actionIsosurface->isChecked())
            return;
        const QSignalBlocker b1(ui->actionIsosurface);
        const QSignalBlocker b2(ui->actionVolume);
        ui->actionIsosurface->setChecked(wantIsosurface);
        ui->actionVolume->setChecked(!wantIsosurface);
        this->updateVolumeModeVisibility(!wantIsosurface);
        this->changeCubeRender();
    });
    QObject::connect(ui->actionIsosurface, &QAction::toggled, this, [this](bool checked) {
        if (auto *c = m_uiAssembler->renderModeCombo(); checked && c && c->currentIndex() != 0) {
            const QSignalBlocker b(c);
            c->setCurrentIndex(0);
        }
        if (checked)
            this->updateVolumeModeVisibility(false);
    });
    QObject::connect(ui->actionVolume, &QAction::toggled, this, [this](bool checked) {
        if (auto *c = m_uiAssembler->renderModeCombo(); checked && c && c->currentIndex() != 1) {
            const QSignalBlocker b(c);
            c->setCurrentIndex(1);
        }
        if (checked)
            this->updateVolumeModeVisibility(true);
    });

    // ── Volume mode combo → apply blend mode ──────────────────────────────────
    QObject::connect(m_uiAssembler->volumeModeCombo(), &SegmentedToggle::currentIndexChanged,
                     this, [this](int idx) {
        this->applyVolumeBlendMode(idx);
        const QSignalBlocker b1(this->m_actionComposite);
        const QSignalBlocker b2(this->m_actionMIP);
        const QSignalBlocker b3(this->m_actionMinIP);
        if (this->m_actionComposite) this->m_actionComposite->setChecked(idx == 0);
        if (this->m_actionMIP)       this->m_actionMIP->setChecked(idx == 1);
        if (this->m_actionMinIP)     this->m_actionMinIP->setChecked(idx == 2);
    });

    // ── LUT combo → apply 3D volume color map ────────────────────────────────
    QObject::connect(m_uiAssembler->lutCombo(), &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applyColorMapByName(name);
    });

    // ── Bidirectional sync: 2D mode toggle ↔ actionSlice / actionMomentMap ───
    QObject::connect(m_uiAssembler->combo2dMode(), &SegmentedToggle::currentIndexChanged,
                     this, [this](int idx) {
        const bool wantSlice = (idx == 0);
        if (wantSlice == ui->actionSlice->isChecked())
            return;
        if (wantSlice) {
            const QSignalBlocker b1(ui->actionSlice);
            const QSignalBlocker b2(ui->actionMomentMap);
            ui->actionSlice->setChecked(true);
            ui->actionMomentMap->setChecked(false);
            this->update2dModeVisibility(true);
            this->changeImageRenderer();
        } else {
            this->requestMomentMode();
        }
    });
    QObject::connect(ui->actionSlice, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            m_uiAssembler->sync2dModeSidebar(true);
            this->update2dModeVisibility(true);
        }
    });
    QObject::connect(ui->actionMomentMap, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            m_uiAssembler->sync2dModeSidebar(false);
            this->update2dModeVisibility(false);
        }
    });

    // ── Slice LUT combo → apply slice color map ───────────────────────────────
    QObject::connect(m_uiAssembler->sliceLutCombo(), &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applySliceLutByName(name);
    });

    // ── Moment LUT combo → apply moment color map ─────────────────────────────
    QObject::connect(m_uiAssembler->momentLutCombo(), &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applyMomentLutByName(name);
    });

    // ── Advanced LUT editor buttons ───────────────────────────────────────────
    // 3D volume (inside m_colorMapCard — hidden automatically in Isosurface mode)
    QObject::connect(m_uiAssembler->lutEdit3dButton(), &QToolButton::clicked,
                     this, &vtkWindowCube::showLUTCustomizer3D);
    // 2D slice (inside m_sliceLutCard — hidden automatically in Moment mode)
    QObject::connect(m_uiAssembler->lutEditSliceButton(), &QToolButton::clicked,
                     this, &vtkWindowCube::showLUTCustomizer);
    // 2D moment (inside m_momentLutCard — hidden automatically in Slice mode)
    QObject::connect(m_uiAssembler->lutEditMomentButton(), &QToolButton::clicked,
                     this, &vtkWindowCube::showLUTCustomizer);

    // View menu toggle action — wired bidirectionally by SidebarPanel.
    this->actionShowControlsSidebar = this->m_sidebar->createToggleAction(
            u"Controls Sidebar"_s, this);
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
    this->lutVolume->SetTableRange(cubeRange[0], cubeRange[1]);
    ColorMaps::SetColorMap(this->lutVolume, stdName);
    this->lutVolume->Build();
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
}

void vtkWindowCube::applyVolumeBlendMode(int idx)
{
    if (!this->volumeMapper) {
        return;
    }
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
    ColorMaps::SetColorMap(this->lutSlice, stdName);
    this->lutSlice->Modified();

    if (ui->vtkImage->renderWindow() && this->viewingSlice()) {
        ui->vtkImage->renderWindow()->Render();
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
    ColorMaps::SetColorMap(this->lutMoment, stdName);
    this->lutMoment->Modified();

    if (ui->vtkImage->renderWindow() && !this->viewingSlice()) {
        ui->vtkImage->renderWindow()->Render();
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
            detail += u" · mask \u2265 %1"_s.arg(cfg.thresholdValue, 0, 'g', 4);
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

void vtkWindowCube::setupWorkspaceDocks()
{
    // Allow docks to be nested and tabbed for maximum layout flexibility.
    this->setDockNestingEnabled(true);
    this->setDockOptions(QMainWindow::AllowNestedDocks
                         | QMainWindow::AllowTabbedDocks
                         | QMainWindow::AnimatedDocks);

    // Remove the viewer container (vtkCube + vtkImage) from the central layout.
    // The VTK widgets will be reparented into dock widgets below; the pointer
    // values (ui->vtkCube, ui->vtkImage) remain valid and all rendering code
    // continues to work without any changes.
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->viewerContainer);
    ui->viewerContainer->hide();

    // The central widget now only holds m_sidebar.  Prevent it from expanding
    // horizontally so the dock area gets the rest of the window width.
    ui->centralwidget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);

    // Shared dock stylesheet. The title-bar text color tracks the theme so the
    // section labels stay legible whether the app shell is dark or light.
    const QString dockStyle =
            QStringLiteral(
                    "QDockWidget {"
                    "  border: 1px solid rgba(4, 138, 191, 0.22);"
                    "}"
                    "QDockWidget::title {"
                    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "      stop:0 rgba(4,138,191,0.18), stop:1 rgba(4,138,191,0.06));"
                    "  color: %1;"
                    "  font-size: 10px;"
                    "  font-weight: 600;"
                    "  text-transform: uppercase;"
                    "  padding: 5px 10px;"
                    "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
                    "}"
                    "QDockWidget::close-button, QDockWidget::float-button {"
                    "  border: none;"
                    "  background: transparent;"
                    "  subcontrol-position: top right;"
                    "  subcontrol-origin: margin;"
                    "  top: 3px;"
                    "}"
                    "QDockWidget::close-button:hover, QDockWidget::float-button:hover {"
                    "  background: rgba(4, 138, 191, 0.25);"
                    "  border-radius: 3px;"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

    // ── 3D View dock ─────────────────────────────────────────────────────────
    auto *dockCube = new QDockWidget(u"3D View"_s, this);
    dockCube->setObjectName(u"dockCubeView"_s);

    // The dock wraps a QStackedWidget so the user can flip between the
    // local QVTKOpenGLNativeWidget pipeline and a server-rendered JPEG
    // stream. ui->vtkCube becomes page 0 (Local); page 1 (Remote) is
    // a container with a dismissible banner above the RemoteRenderWidget.
    this->m_renderStack = new QStackedWidget(dockCube);

    // Local page (0): wrap ui->vtkCube in a container so we can stack a
    // soft "Remote rendering recommended" banner on top when the cube
    // is big enough that the local preview is just a context placeholder
    // and full-quality should come from the V100 stream. The banner
    // stays hidden for cubes that fit locally.
    auto *localContainer = new QWidget(this->m_renderStack);
    auto *localLayout = new QVBoxLayout(localContainer);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(0);

    this->m_localRecommendRemoteBanner = new QFrame(localContainer);
    this->m_localRecommendRemoteBanner->setObjectName(
            u"localRecommendRemoteBanner"_s);
    // Same amber palette as the existing remote banner — visual
    // consistency cue that both relate to the Remote rendering mode.
    this->m_localRecommendRemoteBanner->setStyleSheet(
            u"QFrame#localRecommendRemoteBanner {"
            u"  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
            u"      stop:0 rgba(245, 158, 11, 0.22),"
            u"      stop:1 rgba(217, 119, 6, 0.16));"
            u"  border-bottom: 1px solid rgba(217, 119, 6, 0.40);"
            u"}"
            u"QFrame#localRecommendRemoteBanner QLabel {"
            u"  color: #f3f4f6; padding: 4px 8px;"
            u"}"
            u"QFrame#localRecommendRemoteBanner QPushButton {"
            u"  background: rgba(255, 255, 255, 0.14);"
            u"  border: 1px solid rgba(255, 255, 255, 0.25);"
            u"  border-radius: 3px; color: #f3f4f6;"
            u"  padding: 2px 10px; font-weight: 600;"
            u"}"
            u"QFrame#localRecommendRemoteBanner QPushButton:hover {"
            u"  background: rgba(255, 255, 255, 0.22);"
            u"}"
            u"QFrame#localRecommendRemoteBanner QToolButton {"
            u"  border: none; color: #f3f4f6; padding: 0 8px; font-weight: 600;"
            u"}"
            u"QFrame#localRecommendRemoteBanner QToolButton:hover {"
            u"  background: rgba(255, 255, 255, 0.10); border-radius: 3px;"
            u"}"_s);
    auto *recLayout = new QHBoxLayout(this->m_localRecommendRemoteBanner);
    recLayout->setContentsMargins(8, 2, 4, 2);
    recLayout->setSpacing(8);
    this->m_localRecommendRemoteLabel =
            new QLabel(u""_s, this->m_localRecommendRemoteBanner);
    recLayout->addWidget(this->m_localRecommendRemoteLabel, 1);
    this->m_localRecommendRemoteSwitchBtn = new QPushButton(
            u"Switch to Remote"_s, this->m_localRecommendRemoteBanner);
    this->m_localRecommendRemoteSwitchBtn->setToolTip(
            u"Render the cube on the backend GPU and stream it here — "
            u"full quality without downloading the full data."_s);
    recLayout->addWidget(this->m_localRecommendRemoteSwitchBtn);
    auto *dismissBtn =
            new QToolButton(this->m_localRecommendRemoteBanner);
    dismissBtn->setText(u"✕"_s);
    dismissBtn->setToolTip(u"Dismiss this suggestion"_s);
    recLayout->addWidget(dismissBtn);
    this->m_localRecommendRemoteBanner->setVisible(false);
    localLayout->addWidget(this->m_localRecommendRemoteBanner);
    localLayout->addWidget(ui->vtkCube, 1);

    QObject::connect(this->m_localRecommendRemoteSwitchBtn,
                     &QPushButton::clicked, this, [this]() {
        this->applyRenderMode(RenderMode::Remote, true);
    });
    QObject::connect(dismissBtn, &QToolButton::clicked, this, [this]() {
        this->m_localRecommendRemoteBanner->setVisible(false);
    });

    this->m_renderStack->addWidget(localContainer);   // page 0 — Local

    this->m_remoteRenderContainer = new QWidget(this->m_renderStack);
    auto *remoteLayout = new QVBoxLayout(this->m_remoteRenderContainer);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(0);

    // Banner: amber strip with a label + close button. The close ("×")
    // sends the user back to Local rendering; we do NOT delete the
    // session — switching back to Remote later reuses it without paying
    // the EGL warm-up cost again.
    auto *banner = new QFrame(this->m_remoteRenderContainer);
    banner->setObjectName(u"remoteRenderBanner"_s);
    banner->setStyleSheet(
            u"QFrame#remoteRenderBanner {"
            u"  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
            u"      stop:0 rgba(245, 158, 11, 0.22),"
            u"      stop:1 rgba(217, 119, 6, 0.16));"
            u"  border-bottom: 1px solid rgba(217, 119, 6, 0.40);"
            u"}"
            u"QFrame#remoteRenderBanner QLabel { color: #f3f4f6; padding: 4px 8px; }"
            u"QFrame#remoteRenderBanner QToolButton {"
            u"  border: none; color: #f3f4f6; padding: 0 8px; font-weight: 600;"
            u"}"
            u"QFrame#remoteRenderBanner QToolButton:hover {"
            u"  background: rgba(255, 255, 255, 0.10); border-radius: 3px;"
            u"}"_s);
    auto *bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(8, 2, 4, 2);
    bannerLayout->setSpacing(8);
    this->m_remoteRenderBannerLabel = new QLabel(
            u"Remote rendering (GPU on backend) — click ✕ to switch to Local"_s,
            banner);
    bannerLayout->addWidget(this->m_remoteRenderBannerLabel, 1);
    this->m_remoteRenderBannerClose = new QToolButton(banner);
    this->m_remoteRenderBannerClose->setText(u"✕"_s);
    this->m_remoteRenderBannerClose->setToolTip(u"Switch to Local rendering"_s);
    bannerLayout->addWidget(this->m_remoteRenderBannerClose);
    remoteLayout->addWidget(banner);

    this->m_remoteRenderWidget = new RemoteRenderWidget(this->m_remoteRenderContainer);
    remoteLayout->addWidget(this->m_remoteRenderWidget, 1);

    this->m_renderStack->addWidget(this->m_remoteRenderContainer);  // page 1 — Remote
    this->m_renderStack->setCurrentIndex(0);

    dockCube->setWidget(this->m_renderStack);  // dock owns the stack now
    dockCube->setAllowedAreas(Qt::AllDockWidgetAreas);
    dockCube->setFeatures(QDockWidget::DockWidgetMovable
                          | QDockWidget::DockWidgetFloatable
                          | QDockWidget::DockWidgetClosable);
    dockCube->setStyleSheet(dockStyle);

    // ── 2D View dock ─────────────────────────────────────────────────────────
    auto *dockSlice = new QDockWidget(u"2D View"_s, this);
    dockSlice->setObjectName(u"dockSliceView"_s);
    dockSlice->setWidget(ui->vtkImage);        // reparents vtkImage into the dock
    dockSlice->setAllowedAreas(Qt::AllDockWidgetAreas);
    dockSlice->setFeatures(QDockWidget::DockWidgetMovable
                           | QDockWidget::DockWidgetFloatable
                           | QDockWidget::DockWidgetClosable);
    dockSlice->setStyleSheet(dockStyle);

    // Place both docks in the right dock area, then split them side-by-side.
    this->addDockWidget(Qt::RightDockWidgetArea, dockCube);
    this->addDockWidget(Qt::RightDockWidgetArea, dockSlice);
    this->splitDockWidget(dockCube, dockSlice, Qt::Horizontal);

    this->dockCubeView  = dockCube;
    this->dockSliceView = dockSlice;

    // Before the QStackedWidget swap, splitDockWidget gave 50/50 by default
    // because both dock contents (ui->vtkCube, ui->vtkImage) reported the
    // same sizeHint. The stacked widget now wraps ui->vtkCube and contributes
    // a wider preferred size that biases the split heavily towards the 3D
    // view (~95/5 in the wild). Force the split with resizeDocks deferred to
    // the event loop so the dock area has already been laid out — calling it
    // immediately is a no-op because the dock widths are still zero at this
    // point in the constructor.
    QTimer::singleShot(0, this, [this]() {
        if (!this->dockCubeView || !this->dockSliceView) return;
        const int sidebarW = this->m_sidebar ? this->m_sidebar->width() : 48;
        const int avail = std::max(400, this->width() - sidebarW);
        this->resizeDocks({ this->dockCubeView, this->dockSliceView },
                          { avail / 2, avail / 2 }, Qt::Horizontal);
    });
}

void vtkWindowCube::resetWorkspaceLayout()
{
    if (!this->dockCubeView || !this->dockSliceView) {
        return;
    }

    // Unfloat and show both docks.
    this->dockCubeView->setFloating(false);
    this->dockSliceView->setFloating(false);
    this->dockCubeView->show();
    this->dockSliceView->show();

    // Re-establish the side-by-side layout in the right dock area.
    this->addDockWidget(Qt::RightDockWidgetArea, this->dockCubeView);
    this->addDockWidget(Qt::RightDockWidgetArea, this->dockSliceView);
    this->splitDockWidget(this->dockCubeView, this->dockSliceView, Qt::Horizontal);

    // Resize to equal halves of the available horizontal space.
    const int sidebarW = this->m_sidebar ? this->m_sidebar->width() : 48;
    const int avail = std::max(400, this->width() - sidebarW);
    this->resizeDocks({ this->dockCubeView, this->dockSliceView },
                      { avail / 2, avail / 2 }, Qt::Horizontal);
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

void vtkWindowCube::setupRenderModeUi()
{
    // Status-bar pill — created here so updateRenderModeBadges() can rely
    // on it existing immediately even before the first mode switch.
    this->m_renderModeBadge = new QLabel(u"Render: LOCAL"_s, this);
    this->m_renderModeBadge->setStyleSheet(
            u"QLabel {"
            u"  padding: 1px 8px; margin-right: 6px;"
            u"  border-radius: 9px;"
            u"  background: rgba(120, 120, 120, 0.18);"
            u"  color: palette(window-text);"
            u"  font-size: 10px; font-weight: 600;"
            u"  text-transform: uppercase; letter-spacing: 0.04em;"
            u"}"_s);
    this->statusBar()->addPermanentWidget(this->m_renderModeBadge);

    this->m_renderFpsBadge = new QLabel(QString(), this);
    this->m_renderFpsBadge->setStyleSheet(
            u"QLabel { padding: 1px 6px; color: palette(window-text); }"_s);
    this->m_renderFpsBadge->hide();   // only visible while in Remote mode
    this->statusBar()->addPermanentWidget(this->m_renderFpsBadge);

    // View → Rendering submenu.
    auto *renderMenu = ui->menuView->addMenu(u"Rendering"_s);
    auto *renderGroup = new QActionGroup(this);
    renderGroup->setExclusive(true);

    this->m_actionRenderAuto = renderMenu->addAction(u"Auto"_s);
    this->m_actionRenderAuto->setCheckable(true);
    this->m_actionRenderAuto->setChecked(true);
    this->m_actionRenderAuto->setToolTip(
            u"Pick Local or Remote based on dataset size and backend support"_s);
    renderGroup->addAction(this->m_actionRenderAuto);

    this->m_actionRenderLocal = renderMenu->addAction(u"Local (CPU/GPU on this machine)"_s);
    this->m_actionRenderLocal->setCheckable(true);
    renderGroup->addAction(this->m_actionRenderLocal);

    this->m_actionRenderRemote = renderMenu->addAction(u"Remote (GPU on backend)"_s);
    this->m_actionRenderRemote->setCheckable(true);
    this->m_actionRenderRemote->setToolTip(
            u"Stream JPEG frames from an EGL-equipped backend over WebSocket"_s);
    renderGroup->addAction(this->m_actionRenderRemote);

    QObject::connect(this->m_actionRenderAuto, &QAction::triggered, this,
                     [this]() { this->applyRenderMode(RenderMode::Auto, true); });
    QObject::connect(this->m_actionRenderLocal, &QAction::triggered, this,
                     [this]() { this->applyRenderMode(RenderMode::Local, true); });
    QObject::connect(this->m_actionRenderRemote, &QAction::triggered, this,
                     [this]() { this->applyRenderMode(RenderMode::Remote, true); });

    // Banner close ("✕") → drop back to Local without tearing the session.
    if (this->m_remoteRenderBannerClose) {
        QObject::connect(this->m_remoteRenderBannerClose, &QToolButton::clicked, this, [this]() {
            this->applyRenderMode(RenderMode::Local, true);
        });
    }

    // RemoteRenderWidget → status-bar wiring. Safe to connect now even
    // before any session exists; the widget just stays silent.
    if (this->m_remoteRenderWidget) {
        QObject::connect(this->m_remoteRenderWidget,
                         &RemoteRenderWidget::framesPerSecondChanged,
                         this, &vtkWindowCube::onRemoteRenderFpsChanged);
        QObject::connect(this->m_remoteRenderWidget,
                         &RemoteRenderWidget::connectionStateChanged,
                         this, &vtkWindowCube::onRemoteRenderConnectionStateChanged);
    }

    // Soft recommendation — DO NOT auto-switch. The local preview is
    // already useful and the user might be happy with it; we just
    // surface the option to switch to remote via a non-intrusive
    // banner on the Local page. The banner exposes a button that does
    // the actual mode change when the user decides. This trades a
    // 150-ms auto-switch (used to feel pushy on medium cubes) for a
    // calmer, opt-in flow that respects whatever the user is already
    // looking at.
    const QString autoWorkDir = QDir::home().absoluteFilePath(qApp->applicationName());
    Settings autoSettings(autoWorkDir);
    const double cubeFullMb = (static_cast<double>(this->remoteDatasetWidth)
                               * static_cast<double>(this->remoteDatasetHeight)
                               * static_cast<double>(this->remoteDatasetDepth)
                               * 4.0) / (1024.0 * 1024.0);
    const int    thresholdMb = autoSettings.getLocalFullThresholdMb();
    const bool   sizeWantsRemote = cubeFullMb > static_cast<double>(thresholdMb);
    const bool   backendWantsRemote =
            (this->recommendedLoadMode == QLatin1String("preview_only"));
    qDebug().noquote() << QStringLiteral(
            "[remote-render] auto-decision cube=%1MB threshold=%2MB "
            "backend_hint=%3 -> recommendRemote=%4")
                                  .arg(cubeFullMb, 0, 'f', 1)
                                  .arg(thresholdMb)
                                  .arg(this->recommendedLoadMode)
                                  .arg((sizeWantsRemote || backendWantsRemote) ? "yes" : "no");
    if ((sizeWantsRemote || backendWantsRemote)
        && !this->remoteBackendUrl.isEmpty()
        && this->m_localRecommendRemoteBanner
        && this->m_localRecommendRemoteLabel) {
        const double sizeGb = cubeFullMb / 1024.0;
        QString reason;
        if (backendWantsRemote) {
            reason = u"backend tagged this cube as too large for a local "
                     "full-resolution load."_s;
        } else {
            reason = QStringLiteral(
                    "cube is %1 GB at full resolution (threshold %2 MB).")
                     .arg(sizeGb, 0, 'f', 2)
                     .arg(thresholdMb);
        }
        this->m_localRecommendRemoteLabel->setText(
                u"Remote rendering recommended — " + reason
                + u" Click ⮕ for full-quality interactive 3D without the transfer."_s);
        this->m_localRecommendRemoteBanner->setVisible(true);
    }

    this->updateRenderModeBadges();
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

void vtkWindowCube::setupCubeRenderer()
{
    vtkNew<vtkRenderer> ren;
    ren->SetBackground(0.21, 0.23, 0.25);

    vtkNew<vtkGenericOpenGLRenderWindow> win;
    win->AddRenderer(ren);
    ui->vtkCube->setRenderWindow(win);
    ui->vtkCube->setEnableTouchEventProcessing(false);

    // Isosurface
    this->isosurfaceFilter->SetInputConnection(this->cubeDisplaySource->GetOutputPort());
    this->isosurfaceFilter->SetValue(0, this->lowerBound);
    vtkNew<vtkPolyDataMapper> isosurfaceMapper;
    isosurfaceMapper->SetInputConnection(this->isosurfaceFilter->GetOutputPort());
    isosurfaceMapper->ScalarVisibilityOff();
    this->isosurface->SetMapper(isosurfaceMapper);
    this->isosurface->GetProperty()->SetColor(1., 0.5, 1.);
    // Isosurface visibility managed by scheduleIsosurfaceRecompute / setCubeRenderModeLocally

    // Volume
    const auto cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    const double cubeRange[2] = { this->currentCubeVisibleRange[0], this->currentCubeVisibleRange[1] };
    vtkNew<vtkLookupTable> lutVolume;
    lutVolume->SetTableRange(cubeRange);
    ColorMaps::SetColorMap(lutVolume);
    ColorMaps::SetColorTransferFunction(lutVolume, this->volumeColorTransferFunction);
    this->volumeColorTransferFunction->SetObjectName(lutVolume->GetObjectName());
    // Opacity transfer function. Previous defaults (5% at threshold, 30% at
    // max) made the volume rendering near-invisible — even the brightest
    // peaks topped out at 30% opacity, indistinguishable from background.
    // The new ramp goes:
    //     sentinel / min  → 0.0    (fully transparent: blanks + below-noise)
    //     lowerBound      → 0.12   (faint emission becomes visible)
    //     midpoint        → 0.45   (mid-range structures stand out)
    //     max             → 0.85   (peaks read clearly without saturating)
    // This produces a much more legible default render for HI / molecular
    // cubes whose emission is concentrated in the lower half of the range.
    // Users can still customise via the LUT editor (3D View → Advanced…).
    const double midpoint = 0.5 * (this->lowerBound + cubeRange[1]);
    this->volumeOpacity->RemoveAllPoints();
    this->volumeOpacity->AddPoint(this->currentCubeInvisibleSentinel, 0.0);
    this->volumeOpacity->AddPoint(cubeRange[0],    0.0);
    this->volumeOpacity->AddPoint(this->lowerBound, 0.12);
    this->volumeOpacity->AddPoint(midpoint,         0.45);
    this->volumeOpacity->AddPoint(cubeRange[1],     0.85);
    vtkNew<vtkVolumeProperty> volumeProperty;
    volumeProperty->SetColor(this->volumeColorTransferFunction);
    volumeProperty->SetScalarOpacity(this->volumeOpacity);
    volumeProperty->SetInterpolationTypeToLinear();
    vtkNew<vtkGPUVolumeRayCastMapper> volumeMapper;
    volumeMapper->SetInputConnection(this->cubeDisplaySource->GetOutputPort());
    volumeMapper->SetBlendModeToComposite(); // default
    // Lower sample density during interaction so frames stay responsive
    // immediately after a full-resolution swap (when GPU caches are cold).
    volumeMapper->SetAutoAdjustSampleDistances(1);
    volumeMapper->SetUseJittering(1);
    this->volumeMapper = volumeMapper;       // keep reference for blend-mode changes
    this->volume->SetMapper(volumeMapper);
    this->volume->SetProperty(volumeProperty);
    // By default, we show the isosurface

    // Outline
    vtkNew<vtkOutlineFilter> outline;
    outline->SetInputConnection(this->cubeDisplaySource->GetOutputPort());
    vtkNew<vtkPolyDataMapper> outlineMapper;
    outlineMapper->SetInputConnection(outline->GetOutputPort());
    vtkNew<vtkActor> outlineActor;
    outlineActor->SetMapper(outlineMapper);
    ren->AddViewProp(outlineActor);

    // ── Noise-region preview cube (3D) ────────────────────────────────────
    // Semi-transparent amber box inside the volume that mirrors the slice's
    // (x0..x1, y0..y1) rectangle and extrudes it along Z over the user's
    // chosen channel range. Two actors share the same cube source: a
    // filled-face actor at low opacity (translucent fill) and a wireframe
    // actor at higher opacity (crisp edges). Both default to hidden — they
    // get bounds and visibility from setNoiseRegionPreview() while the
    // NoiseRegionDialog is open and through the result dialog's lifetime.
    {
        vtkNew<vtkPolyDataMapper> faceMapper;
        faceMapper->SetInputConnection(this->noisePreviewCubeSource->GetOutputPort());
        faceMapper->ScalarVisibilityOff();
        this->noisePreviewCubeFaceActor->SetMapper(faceMapper);
        this->noisePreviewCubeFaceActor->GetProperty()->SetColor(0.95, 0.65, 0.15);
        this->noisePreviewCubeFaceActor->GetProperty()->SetOpacity(0.18);
        this->noisePreviewCubeFaceActor->GetProperty()->SetRepresentationToSurface();
        this->noisePreviewCubeFaceActor->GetProperty()->LightingOff();
        this->noisePreviewCubeFaceActor->PickableOff();
        this->noisePreviewCubeFaceActor->VisibilityOff();
        ren->AddViewProp(this->noisePreviewCubeFaceActor);

        vtkNew<vtkPolyDataMapper> edgeMapper;
        edgeMapper->SetInputConnection(this->noisePreviewCubeSource->GetOutputPort());
        edgeMapper->ScalarVisibilityOff();
        this->noisePreviewCubeEdgeActor->SetMapper(edgeMapper);
        this->noisePreviewCubeEdgeActor->GetProperty()->SetColor(0.95, 0.65, 0.15);
        this->noisePreviewCubeEdgeActor->GetProperty()->SetOpacity(0.95);
        this->noisePreviewCubeEdgeActor->GetProperty()->SetRepresentationToWireframe();
        this->noisePreviewCubeEdgeActor->GetProperty()->SetLineWidth(2.0);
        this->noisePreviewCubeEdgeActor->GetProperty()->LightingOff();
        this->noisePreviewCubeEdgeActor->PickableOff();
        this->noisePreviewCubeEdgeActor->VisibilityOff();
        ren->AddViewProp(this->noisePreviewCubeEdgeActor);
    }

    {
        // The cutting plane shows the actual 2D slice content textured onto
        // the plane in the 3D view (instead of a plain green fill). The
        // texture pipeline reuses sliceColors → it updates automatically
        // whenever a new slice tile arrives via remoteSliceDisplaySource.
        // vtkPlaneSource emits texture coordinates (origin→(0,0), point1→
        // (1,0), point2→(0,1)) that match the slice image orientation.
        vtkNew<vtkPolyDataMapper> planeMapper;
        planeMapper->SetInputConnection(this->remoteCuttingPlaneSource->GetOutputPort());
        planeMapper->ScalarVisibilityOff();
        this->remoteCuttingPlaneActor->SetMapper(planeMapper);

        // The slice texture is attached AFTER setupSliceRenderer() finishes
        // (see the ctor, right after this->setupMomentRenderer()). Here we
        // only configure the plane's solid-colour fallback: a translucent
        // white surface with a red outline so the plane is still identifiable
        // before the first slice tile arrives.
        this->remoteCuttingPlaneActor->GetProperty()->SetColor(1.0, 1.0, 1.0);
        this->remoteCuttingPlaneActor->GetProperty()->SetOpacity(0.5);
        this->remoteCuttingPlaneActor->GetProperty()->EdgeVisibilityOn();
        this->remoteCuttingPlaneActor->GetProperty()->SetEdgeColor(1.0, 0.0, 0.0);
        this->remoteCuttingPlaneActor->GetProperty()->SetLineWidth(3.0);
        this->remoteCuttingPlaneActor->GetProperty()->LightingOff();
        this->remoteCuttingPlaneActor->GetProperty()->BackfaceCullingOff();
        this->remoteCuttingPlaneActor->GetProperty()->FrontfaceCullingOff();
        this->remoteCuttingPlaneActor->VisibilityOn();
        ren->AddViewProp(this->remoteCuttingPlaneActor);
        qDebug().noquote()
                << QStringLiteral("[remote-plane] actor added renderer=%1 actor=%2 visible=%3")
                           .arg(reinterpret_cast<quintptr>(ren.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->remoteCuttingPlaneActor.GetPointer()),
                                0, 16)
                           .arg(this->remoteCuttingPlaneActor->GetVisibility());
    }

    // Slice overlay in cube renderer not used in backend mode; cutting plane actor serves this role.

    // Axes
    vtkNew<vtkAxesActor> axes;
    this->axesWidget->SetOrientationMarker(axes);
    this->axesWidget->SetInteractor(win->GetInteractor());
    this->axesWidget->SetEnabled(1);
    this->axesWidget->InteractiveOff();

    // ── Loading placeholder text actor ────────────────────────────────────────
    // Shown until the first real volume preview arrives from the backend.
    // Hidden (VisibilityOff) in remotePreviewWatcher::finished.
    {
        auto *prop = this->m_cube3dLoadingActor->GetTextProperty();
        prop->SetFontSize(16);
        double r, g, b;
        ThemeManager::instance().vtkTextColor(r, g, b);
        prop->SetColor(r, g, b);
        prop->SetJustificationToCentered();
        prop->SetVerticalJustificationToCentered();
        prop->BoldOff();
        prop->ShadowOff();
        this->m_cube3dLoadingActor->SetInput("Loading 3D view\xe2\x80\xa6");  // UTF-8 ellipsis
        this->m_cube3dLoadingActor->GetPositionCoordinate()
                ->SetCoordinateSystemToNormalizedViewport();
        this->m_cube3dLoadingActor->SetPosition(0.5, 0.5);
        this->m_cube3dLoadingActor->VisibilityOn();
        ren->AddViewProp(this->m_cube3dLoadingActor);

        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                         this, [this](EffectiveTheme) {
                             double tr, tg, tb;
                             ThemeManager::instance().vtkTextColor(tr, tg, tb);
                             this->m_cube3dLoadingActor->GetTextProperty()->SetColor(tr, tg, tb);
                         });
    }

    ren->ResetCamera();
    win->Render();

    ren->GetActiveCamera()->GetPosition(this->initialCameraPosition);
    ren->GetActiveCamera()->GetFocalPoint(this->initialCameraFocalPoint);
}

void vtkWindowCube::setupSliceRenderer()
{
    this->sliceWcsOverlayInitialized = false;
    vtkNew<vtkRenderer> ren;
    ren->SetBackground(0.21, 0.23, 0.25);
    ren->GetActiveCamera()->ParallelProjectionOn();

    this->sliceWin->AddRenderer(ren);
    ui->vtkImage->setRenderWindow(this->sliceWin);
    ui->vtkImage->setEnableTouchEventProcessing(false);

    vtkNew<vtkInteractorStyleImage> style;
    this->sliceWin->GetInteractor()->SetInteractorStyle(style);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::MouseMoveEvent, this,
                                                 &vtkWindowCube::mouseCallback);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::LeftButtonPressEvent, this,
                                                 &vtkWindowCube::toggleProbeFreeze);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                                                 &vtkWindowCube::finishRegionInteraction);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::RightButtonPressEvent, this,
                                                 &vtkWindowCube::finalizePvInteraction);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, this,
                                                 &vtkWindowCube::finalizePvInteraction);
    this->coordinate->SetCoordinateSystemToDisplay();
    this->coordinate->SetViewport(ren);

    // Slice — remote backend always provides slice tiles; initialize with a placeholder.
    this->lutSlice->SetTableRange(0., 0.);
    this->lutSlice->SetNanColor(1., 1., 1., 1.);
    ColorMaps::SetColorMap(this->lutSlice);
    {
        auto *img = vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
        if (img) {
            this->sliceColors->SetInputData(img);
        } else {
            this->sliceColors->SetInputData(createPlaceholderImageData());
        }
    }
    this->sliceColors->SetLookupTable(this->lutSlice);
    vtkNew<vtkImageSliceMapper> sliceMapper;
    sliceMapper->SetInputConnection(this->sliceColors->GetOutputPort());
    sliceMapper->BorderOn();
    vtkNew<vtkImageSlice> sliceActor;
    sliceActor->SetMapper(sliceMapper);
    sliceActor->GetProperty()->SetInterpolationTypeToNearest();
    ren->AddViewProp(sliceActor);

    // Color bar
    vtkNew<vtkScalarBarActor> colorbar;
    colorbar->SetLookupTable(this->lutSlice);
    colorbar->SetMaximumWidthInPixels(100);
    colorbar->SetPosition(0.9, 0.1);
    ren->AddViewProp(colorbar);

    // Contours
    this->contours->SetInputConnection(this->remoteSliceDisplaySource->GetOutputPort());
    this->contours->GenerateValues(this->level, this->lowerBound, this->upperBound);
    vtkNew<vtkPolyDataMapper> contoursMapper;
    contoursMapper->SetInputConnection(this->contours->GetOutputPort());
    contoursMapper->SetScalarRange(this->lowerBound, this->upperBound);
    this->contoursActor->SetMapper(contoursMapper);
    this->contoursActor->VisibilityOff();
    ren->AddViewProp(this->contoursActor);

    // Legend: local AstroUtils-based legend not used in backend mode; remote WCS overlay actors are used instead.
    ren->AddViewProp(this->sliceOverlayXAxis);
    ren->AddViewProp(this->sliceOverlayYAxis);
    ren->AddViewProp(this->sliceOverlayXTitleActor);
    ren->AddViewProp(this->sliceOverlayYTitleActor);
    vtkNew<vtkPolyDataMapper> sliceProbeHorizontalMapper;
    sliceProbeHorizontalMapper->SetInputConnection(this->sliceProbeHorizontalLine->GetOutputPort());
    this->sliceProbeHorizontalActor->SetMapper(sliceProbeHorizontalMapper);
    this->sliceProbeHorizontalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->sliceProbeHorizontalActor->GetProperty()->SetLineWidth(1.5);
    this->sliceProbeHorizontalActor->VisibilityOff();
    ren->AddActor(this->sliceProbeHorizontalActor);
    vtkNew<vtkPolyDataMapper> sliceProbeVerticalMapper;
    sliceProbeVerticalMapper->SetInputConnection(this->sliceProbeVerticalLine->GetOutputPort());
    this->sliceProbeVerticalActor->SetMapper(sliceProbeVerticalMapper);
    this->sliceProbeVerticalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->sliceProbeVerticalActor->GetProperty()->SetLineWidth(1.5);
    this->sliceProbeVerticalActor->VisibilityOff();
    ren->AddActor(this->sliceProbeVerticalActor);
    this->catalogueOverlayData->SetPoints(this->catalogueOverlayPoints);
    this->catalogueOverlayData->SetLines(this->catalogueOverlayCells);
    vtkNew<vtkPolyDataMapper> sliceCatalogueOverlayMapper;
    sliceCatalogueOverlayMapper->SetInputData(this->catalogueOverlayData);
    this->sliceCatalogueOverlayActor->SetMapper(sliceCatalogueOverlayMapper);
    this->sliceCatalogueOverlayActor->GetProperty()->SetColor(1.0, 0.45, 0.15);
    this->sliceCatalogueOverlayActor->GetProperty()->SetLineWidth(1.5);
    this->sliceCatalogueOverlayActor->VisibilityOff();
    ren->AddActor(this->sliceCatalogueOverlayActor);
    vtkNew<vtkPolyDataMapper> sliceCatalogueHoverMapper;
    sliceCatalogueHoverMapper->SetInputData(this->catalogueHoverOverlayData);
    this->sliceCatalogueHoverOverlayActor->SetMapper(sliceCatalogueHoverMapper);
    this->sliceCatalogueHoverOverlayActor->GetProperty()->SetColor(1.0, 0.95, 0.45);
    this->sliceCatalogueHoverOverlayActor->GetProperty()->SetLineWidth(2.5);
    this->sliceCatalogueHoverOverlayActor->VisibilityOff();
    ren->AddActor(this->sliceCatalogueHoverOverlayActor);
    vtkNew<vtkPolyDataMapper> sliceCatalogueSelectionMapper;
    sliceCatalogueSelectionMapper->SetInputData(this->catalogueSelectionOverlayData);
    this->sliceCatalogueSelectionOverlayActor->SetMapper(sliceCatalogueSelectionMapper);
    this->sliceCatalogueSelectionOverlayActor->GetProperty()->SetColor(0.15, 0.95, 1.0);
    this->sliceCatalogueSelectionOverlayActor->GetProperty()->SetLineWidth(3.2);
    this->sliceCatalogueSelectionOverlayActor->VisibilityOff();
    ren->AddActor(this->sliceCatalogueSelectionOverlayActor);
    this->slicePvData->SetPoints(this->slicePvPoints);
    this->slicePvData->SetLines(this->slicePvCells);
    vtkNew<vtkPolyDataMapper> slicePvMapper;
    slicePvMapper->SetInputData(this->slicePvData);
    this->slicePvActor->SetMapper(slicePvMapper);
    this->slicePvActor->GetProperty()->SetColor(1.0, 0.4, 0.1);
    this->slicePvActor->GetProperty()->SetLineWidth(2.5);
    this->slicePvActor->VisibilityOff();
    ren->AddActor(this->slicePvActor);
    vtkRenderer *sliceRenderer = ren;
    const auto configurePvBoundaryActor = [sliceRenderer](vtkPoints *points, vtkCellArray *cells,
                                                          vtkPolyData *polyData, vtkActor *actor) {
        polyData->SetPoints(points);
        polyData->SetLines(cells);
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(polyData);
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(1.0, 0.72, 0.2);
        actor->GetProperty()->SetLineWidth(1.5);
        actor->GetProperty()->SetOpacity(0.8);
        actor->VisibilityOff();
        sliceRenderer->AddActor(actor);
    };
    configurePvBoundaryActor(this->slicePvUpperPoints, this->slicePvUpperCells,
                             this->slicePvUpperData, this->slicePvUpperActor);
    configurePvBoundaryActor(this->slicePvLowerPoints, this->slicePvLowerCells,
                             this->slicePvLowerData, this->slicePvLowerActor);
    const auto configureRegionLineActor = [sliceRenderer](vtkLineSource *line, vtkActor *actor) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.2, 0.9, 1.0);
        actor->GetProperty()->SetLineWidth(2.0);
        actor->VisibilityOff();
        sliceRenderer->AddActor(actor);
    };
    configureRegionLineActor(this->sliceRegionTopLine, this->sliceRegionTopActor);
    configureRegionLineActor(this->sliceRegionBottomLine, this->sliceRegionBottomActor);
    configureRegionLineActor(this->sliceRegionLeftLine, this->sliceRegionLeftActor);
    configureRegionLineActor(this->sliceRegionRightLine, this->sliceRegionRightActor);
    // Noise-region preview rectangle: amber, slightly thicker, drawn over
    // the slice while the NoiseRegionDialog is open. Distinct colour from
    // the active Box region (cyan above) so the two never get confused.
    const auto configureNoisePreviewActor = [sliceRenderer](vtkLineSource *line, vtkActor *actor) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.95, 0.65, 0.15);  // amber
        actor->GetProperty()->SetLineWidth(2.5);
        actor->GetProperty()->SetOpacity(0.9);
        actor->VisibilityOff();
        sliceRenderer->AddActor(actor);
    };
    configureNoisePreviewActor(this->noisePreviewTopLine,    this->noisePreviewTopActor);
    configureNoisePreviewActor(this->noisePreviewBottomLine, this->noisePreviewBottomActor);
    configureNoisePreviewActor(this->noisePreviewLeftLine,   this->noisePreviewLeftActor);
    configureNoisePreviewActor(this->noisePreviewRightLine,  this->noisePreviewRightActor);
    this->sliceRegionCircleSource->SetNumberOfSides(96);
    this->sliceRegionCircleSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> sliceRegionCircleMapper;
    sliceRegionCircleMapper->SetInputConnection(this->sliceRegionCircleSource->GetOutputPort());
    this->sliceRegionCircleActor->SetMapper(sliceRegionCircleMapper);
    this->sliceRegionCircleActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionCircleActor->GetProperty()->SetLineWidth(2.0);
    this->sliceRegionCircleActor->VisibilityOff();
    ren->AddActor(this->sliceRegionCircleActor);
    this->sliceRegionAnnulusOuterSource->SetNumberOfSides(96);
    this->sliceRegionAnnulusOuterSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> sliceRegionAnnulusOuterMapper;
    sliceRegionAnnulusOuterMapper->SetInputConnection(this->sliceRegionAnnulusOuterSource->GetOutputPort());
    this->sliceRegionAnnulusOuterActor->SetMapper(sliceRegionAnnulusOuterMapper);
    this->sliceRegionAnnulusOuterActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionAnnulusOuterActor->GetProperty()->SetLineWidth(2.0);
    this->sliceRegionAnnulusOuterActor->GetProperty()->SetRepresentationToWireframe();
    this->sliceRegionAnnulusOuterActor->VisibilityOff();
    ren->AddActor(this->sliceRegionAnnulusOuterActor);
    this->sliceRegionAnnulusInnerSource->SetNumberOfSides(96);
    this->sliceRegionAnnulusInnerSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> sliceRegionAnnulusInnerMapper;
    sliceRegionAnnulusInnerMapper->SetInputConnection(this->sliceRegionAnnulusInnerSource->GetOutputPort());
    this->sliceRegionAnnulusInnerActor->SetMapper(sliceRegionAnnulusInnerMapper);
    this->sliceRegionAnnulusInnerActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionAnnulusInnerActor->GetProperty()->SetLineWidth(1.5);
    this->sliceRegionAnnulusInnerActor->GetProperty()->SetRepresentationToWireframe();
    this->sliceRegionAnnulusInnerActor->VisibilityOff();
    ren->AddActor(this->sliceRegionAnnulusInnerActor);
    vtkNew<vtkPolyDataMapper> sliceRegionAnnulusFillMapper;
    sliceRegionAnnulusFillMapper->SetInputData(this->sliceRegionAnnulusFillData);
    this->sliceRegionAnnulusFillActor->SetMapper(sliceRegionAnnulusFillMapper);
    this->sliceRegionAnnulusFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionAnnulusFillActor->GetProperty()->SetOpacity(0.18);
    this->sliceRegionAnnulusFillActor->GetProperty()->SetRepresentationToSurface();
    this->sliceRegionAnnulusFillActor->VisibilityOff();
    ren->AddActor(this->sliceRegionAnnulusFillActor);
    this->sliceRegionPolygonTriangulator->SetInputData(this->sliceRegionPolygonFillData);
    vtkNew<vtkPolyDataMapper> sliceRegionPolygonMapper;
    sliceRegionPolygonMapper->SetInputData(this->sliceRegionPolygonData);
    this->sliceRegionPolygonActor->SetMapper(sliceRegionPolygonMapper);
    this->sliceRegionPolygonActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionPolygonActor->GetProperty()->SetLineWidth(2.0);
    this->sliceRegionPolygonActor->VisibilityOff();
    ren->AddActor(this->sliceRegionPolygonActor);
    vtkNew<vtkPolyDataMapper> sliceRegionPolygonFillMapper;
    sliceRegionPolygonFillMapper->SetInputConnection(this->sliceRegionPolygonTriangulator->GetOutputPort());
    this->sliceRegionPolygonFillActor->SetMapper(sliceRegionPolygonFillMapper);
    this->sliceRegionPolygonFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionPolygonFillActor->GetProperty()->SetOpacity(0.18);
    this->sliceRegionPolygonFillActor->VisibilityOff();
    ren->AddActor(this->sliceRegionPolygonFillActor);
    this->sliceOverlayXAxis->GetTitleTextProperty()->SetFontSize(16);
    this->sliceOverlayXAxis->GetTitleTextProperty()->SetBold(false);
    this->sliceOverlayYAxis->GetTitleTextProperty()->SetFontSize(16);
    this->sliceOverlayYAxis->GetTitleTextProperty()->SetOrientation(90.);
    this->sliceOverlayYAxis->GetTitleTextProperty()->SetBold(false);
    this->sliceOverlayXTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->sliceOverlayXTitleActor->GetTextProperty()->SetFontSize(14);
    this->sliceOverlayXTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->sliceOverlayXTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->sliceOverlayYTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->sliceOverlayYTitleActor->GetTextProperty()->SetFontSize(14);
    this->sliceOverlayYTitleActor->GetTextProperty()->SetOrientation(90.);
    this->sliceOverlayYTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->sliceOverlayYTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->ensureOverlayTickActors(ren, this->sliceOverlayXTickActors, this->sliceOverlayYTickActors);
    this->sliceWcsOverlayInitialized = true;
    this->set2dWcsOverlayVisible(this->showWcsAxes);
    this->sliceWin->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::updateSliceWcsOverlay);
    this->sliceWin->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::updateCatalogueOverlayLabels);

    // ── Loading placeholder text actor ────────────────────────────────────────
    // Shown until the first remote slice tile arrives from the backend.
    // Hidden (VisibilityOff) in applyRemoteSliceResult().
    {
        auto *prop = this->m_slice2dLoadingActor->GetTextProperty();
        prop->SetFontSize(16);
        prop->SetColor(0.72, 0.84, 0.91);   // #B8D6E8 — matches app blue-gray palette
        prop->SetJustificationToCentered();
        prop->SetVerticalJustificationToCentered();
        prop->BoldOff();
        prop->ShadowOff();
        this->m_slice2dLoadingActor->SetInput("Loading 2D view\xe2\x80\xa6");  // UTF-8 ellipsis
        this->m_slice2dLoadingActor->GetPositionCoordinate()
                ->SetCoordinateSystemToNormalizedViewport();
        this->m_slice2dLoadingActor->SetPosition(0.5, 0.5);
        this->m_slice2dLoadingActor->VisibilityOn();
        ren->AddViewProp(this->m_slice2dLoadingActor);
    }

    ren->ResetCamera();
    this->sliceWin->Render();
}

void vtkWindowCube::setupMomentRenderer()
{
    this->momentWcsOverlayInitialized = false;
    vtkNew<vtkRenderer> ren;
    ren->SetBackground(0.21, 0.23, 0.25);
    ren->GetActiveCamera()->ParallelProjectionOn();
    this->momentWin->AddRenderer(ren);

    vtkNew<QVTKInteractor> iren;
    this->momentWin->SetInteractor(iren);
    iren->Initialize();
    iren->AddObserver(vtkCommand::MouseMoveEvent, this, &vtkWindowCube::mouseCallback);
    iren->AddObserver(vtkCommand::LeftButtonPressEvent, this, &vtkWindowCube::toggleProbeFreeze);
    iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, this, &vtkWindowCube::finishRegionInteraction);
    iren->AddObserver(vtkCommand::RightButtonPressEvent, this, &vtkWindowCube::finalizePvInteraction);
    iren->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, this, &vtkWindowCube::finalizePvInteraction);
    iren->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, this, &vtkWindowCube::finalizePvInteraction);

    vtkNew<vtkInteractorStyleImage> style;
    iren->SetInteractorStyle(style);

    if (!vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0))) {
        this->momentDisplaySource->SetOutput(createPlaceholderImageData());
    }

    const auto momentImage =
            vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    if (momentImage) {
        this->lutMoment->SetTableRange(momentImage->GetScalarRange());
    } else {
        this->lutMoment->SetTableRange(0., 0.);
    }
    this->lutMoment->SetNanColor(1., 1., 1., 1.);
    ColorMaps::SetColorMap(this->lutMoment);
    if (momentImage) {
        this->momentColors->SetInputData(momentImage);
    } else {
        this->momentColors->SetInputData(createPlaceholderImageData());
    }
    this->momentColors->SetLookupTable(this->lutMoment);
    vtkNew<vtkImageSliceMapper> momentMapper;
    momentMapper->SetInputConnection(this->momentColors->GetOutputPort());
    momentMapper->BorderOn();
    vtkNew<vtkImageSlice> momentActor;
    momentActor->SetMapper(momentMapper);
    momentActor->GetProperty()->SetInterpolationTypeToNearest();
    ren->AddViewProp(momentActor);

    // Color bar
    vtkNew<vtkScalarBarActor> colorbar;
    colorbar->SetLookupTable(this->lutMoment);
    colorbar->SetMaximumWidthInPixels(100);
    colorbar->SetPosition(0.9, 0.1);
    ren->AddViewProp(colorbar);

    // Legend: local AstroUtils-based legend not used in backend mode; remote WCS overlay actors are used instead.
    ren->AddViewProp(this->momentOverlayXAxis);
    ren->AddViewProp(this->momentOverlayYAxis);
    ren->AddViewProp(this->momentOverlayXTitleActor);
    ren->AddViewProp(this->momentOverlayYTitleActor);
    vtkNew<vtkPolyDataMapper> momentProbeHorizontalMapper;
    momentProbeHorizontalMapper->SetInputConnection(this->momentProbeHorizontalLine->GetOutputPort());
    this->momentProbeHorizontalActor->SetMapper(momentProbeHorizontalMapper);
    this->momentProbeHorizontalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->momentProbeHorizontalActor->GetProperty()->SetLineWidth(1.5);
    this->momentProbeHorizontalActor->VisibilityOff();
    ren->AddActor(this->momentProbeHorizontalActor);
    vtkNew<vtkPolyDataMapper> momentProbeVerticalMapper;
    momentProbeVerticalMapper->SetInputConnection(this->momentProbeVerticalLine->GetOutputPort());
    this->momentProbeVerticalActor->SetMapper(momentProbeVerticalMapper);
    this->momentProbeVerticalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->momentProbeVerticalActor->GetProperty()->SetLineWidth(1.5);
    this->momentProbeVerticalActor->VisibilityOff();
    ren->AddActor(this->momentProbeVerticalActor);
    vtkNew<vtkPolyDataMapper> momentCatalogueOverlayMapper;
    momentCatalogueOverlayMapper->SetInputData(this->catalogueOverlayData);
    this->momentCatalogueOverlayActor->SetMapper(momentCatalogueOverlayMapper);
    this->momentCatalogueOverlayActor->GetProperty()->SetColor(1.0, 0.45, 0.15);
    this->momentCatalogueOverlayActor->GetProperty()->SetLineWidth(1.5);
    this->momentCatalogueOverlayActor->VisibilityOff();
    ren->AddActor(this->momentCatalogueOverlayActor);
    vtkNew<vtkPolyDataMapper> momentCatalogueHoverMapper;
    momentCatalogueHoverMapper->SetInputData(this->catalogueHoverOverlayData);
    this->momentCatalogueHoverOverlayActor->SetMapper(momentCatalogueHoverMapper);
    this->momentCatalogueHoverOverlayActor->GetProperty()->SetColor(1.0, 0.95, 0.45);
    this->momentCatalogueHoverOverlayActor->GetProperty()->SetLineWidth(2.5);
    this->momentCatalogueHoverOverlayActor->VisibilityOff();
    ren->AddActor(this->momentCatalogueHoverOverlayActor);
    vtkNew<vtkPolyDataMapper> momentCatalogueSelectionMapper;
    momentCatalogueSelectionMapper->SetInputData(this->catalogueSelectionOverlayData);
    this->momentCatalogueSelectionOverlayActor->SetMapper(momentCatalogueSelectionMapper);
    this->momentCatalogueSelectionOverlayActor->GetProperty()->SetColor(0.15, 0.95, 1.0);
    this->momentCatalogueSelectionOverlayActor->GetProperty()->SetLineWidth(3.2);
    this->momentCatalogueSelectionOverlayActor->VisibilityOff();
    ren->AddActor(this->momentCatalogueSelectionOverlayActor);
    this->momentPvData->SetPoints(this->momentPvPoints);
    this->momentPvData->SetLines(this->momentPvCells);
    vtkNew<vtkPolyDataMapper> momentPvMapper;
    momentPvMapper->SetInputData(this->momentPvData);
    this->momentPvActor->SetMapper(momentPvMapper);
    this->momentPvActor->GetProperty()->SetColor(1.0, 0.4, 0.1);
    this->momentPvActor->GetProperty()->SetLineWidth(2.5);
    this->momentPvActor->VisibilityOff();
    ren->AddActor(this->momentPvActor);
    vtkRenderer *momentRenderer = ren;
    const auto configureMomentPvBoundaryActor =
            [momentRenderer](vtkPoints *points, vtkCellArray *cells, vtkPolyData *polyData,
                             vtkActor *actor) {
        polyData->SetPoints(points);
        polyData->SetLines(cells);
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(polyData);
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(1.0, 0.72, 0.2);
        actor->GetProperty()->SetLineWidth(1.5);
        actor->GetProperty()->SetOpacity(0.8);
        actor->VisibilityOff();
        momentRenderer->AddActor(actor);
    };
    configureMomentPvBoundaryActor(this->momentPvUpperPoints, this->momentPvUpperCells,
                                   this->momentPvUpperData, this->momentPvUpperActor);
    configureMomentPvBoundaryActor(this->momentPvLowerPoints, this->momentPvLowerCells,
                                   this->momentPvLowerData, this->momentPvLowerActor);
    const auto configureRegionLineActor = [momentRenderer](vtkLineSource *line, vtkActor *actor) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.2, 0.9, 1.0);
        actor->GetProperty()->SetLineWidth(2.0);
        actor->VisibilityOff();
        momentRenderer->AddActor(actor);
    };
    configureRegionLineActor(this->momentRegionTopLine, this->momentRegionTopActor);
    configureRegionLineActor(this->momentRegionBottomLine, this->momentRegionBottomActor);
    configureRegionLineActor(this->momentRegionLeftLine, this->momentRegionLeftActor);
    configureRegionLineActor(this->momentRegionRightLine, this->momentRegionRightActor);
    this->momentRegionCircleSource->SetNumberOfSides(96);
    this->momentRegionCircleSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> momentRegionCircleMapper;
    momentRegionCircleMapper->SetInputConnection(this->momentRegionCircleSource->GetOutputPort());
    this->momentRegionCircleActor->SetMapper(momentRegionCircleMapper);
    this->momentRegionCircleActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionCircleActor->GetProperty()->SetLineWidth(2.0);
    this->momentRegionCircleActor->VisibilityOff();
    ren->AddActor(this->momentRegionCircleActor);
    this->momentRegionAnnulusOuterSource->SetNumberOfSides(96);
    this->momentRegionAnnulusOuterSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> momentRegionAnnulusOuterMapper;
    momentRegionAnnulusOuterMapper->SetInputConnection(this->momentRegionAnnulusOuterSource->GetOutputPort());
    this->momentRegionAnnulusOuterActor->SetMapper(momentRegionAnnulusOuterMapper);
    this->momentRegionAnnulusOuterActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionAnnulusOuterActor->GetProperty()->SetLineWidth(2.0);
    this->momentRegionAnnulusOuterActor->GetProperty()->SetRepresentationToWireframe();
    this->momentRegionAnnulusOuterActor->VisibilityOff();
    ren->AddActor(this->momentRegionAnnulusOuterActor);
    this->momentRegionAnnulusInnerSource->SetNumberOfSides(96);
    this->momentRegionAnnulusInnerSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> momentRegionAnnulusInnerMapper;
    momentRegionAnnulusInnerMapper->SetInputConnection(this->momentRegionAnnulusInnerSource->GetOutputPort());
    this->momentRegionAnnulusInnerActor->SetMapper(momentRegionAnnulusInnerMapper);
    this->momentRegionAnnulusInnerActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionAnnulusInnerActor->GetProperty()->SetLineWidth(1.5);
    this->momentRegionAnnulusInnerActor->GetProperty()->SetRepresentationToWireframe();
    this->momentRegionAnnulusInnerActor->VisibilityOff();
    ren->AddActor(this->momentRegionAnnulusInnerActor);
    vtkNew<vtkPolyDataMapper> momentRegionAnnulusFillMapper;
    momentRegionAnnulusFillMapper->SetInputData(this->momentRegionAnnulusFillData);
    this->momentRegionAnnulusFillActor->SetMapper(momentRegionAnnulusFillMapper);
    this->momentRegionAnnulusFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionAnnulusFillActor->GetProperty()->SetOpacity(0.18);
    this->momentRegionAnnulusFillActor->GetProperty()->SetRepresentationToSurface();
    this->momentRegionAnnulusFillActor->VisibilityOff();
    ren->AddActor(this->momentRegionAnnulusFillActor);
    this->momentRegionPolygonTriangulator->SetInputData(this->momentRegionPolygonFillData);
    vtkNew<vtkPolyDataMapper> momentRegionPolygonMapper;
    momentRegionPolygonMapper->SetInputData(this->momentRegionPolygonData);
    this->momentRegionPolygonActor->SetMapper(momentRegionPolygonMapper);
    this->momentRegionPolygonActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionPolygonActor->GetProperty()->SetLineWidth(2.0);
    this->momentRegionPolygonActor->VisibilityOff();
    ren->AddActor(this->momentRegionPolygonActor);
    vtkNew<vtkPolyDataMapper> momentRegionPolygonFillMapper;
    momentRegionPolygonFillMapper->SetInputConnection(this->momentRegionPolygonTriangulator->GetOutputPort());
    this->momentRegionPolygonFillActor->SetMapper(momentRegionPolygonFillMapper);
    this->momentRegionPolygonFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionPolygonFillActor->GetProperty()->SetOpacity(0.18);
    this->momentRegionPolygonFillActor->VisibilityOff();
    ren->AddActor(this->momentRegionPolygonFillActor);
    this->momentOverlayXAxis->GetTitleTextProperty()->SetFontSize(16);
    this->momentOverlayXAxis->GetTitleTextProperty()->SetBold(false);
    this->momentOverlayYAxis->GetTitleTextProperty()->SetFontSize(16);
    this->momentOverlayYAxis->GetTitleTextProperty()->SetOrientation(90.);
    this->momentOverlayYAxis->GetTitleTextProperty()->SetBold(false);
    this->momentOverlayXTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->momentOverlayXTitleActor->GetTextProperty()->SetFontSize(14);
    this->momentOverlayXTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->momentOverlayXTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->momentOverlayYTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->momentOverlayYTitleActor->GetTextProperty()->SetFontSize(14);
    this->momentOverlayYTitleActor->GetTextProperty()->SetOrientation(90.);
    this->momentOverlayYTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->momentOverlayYTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->ensureOverlayTickActors(ren, this->momentOverlayXTickActors, this->momentOverlayYTickActors);
    this->momentWcsOverlayInitialized = true;
    this->set2dWcsOverlayVisible(this->showWcsAxes);
    this->momentWin->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::updateMomentWcsOverlay);
    this->momentWin->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::updateCatalogueOverlayLabels);

    ren->ResetCamera();
}

void vtkWindowCube::resetCubeCamera()
{
    auto ren = ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer();
    auto camera = ren->GetActiveCamera();
    camera->SetPosition(this->initialCameraPosition);
    camera->SetFocalPoint(this->initialCameraFocalPoint);
    camera->SetViewUp(0., 1., 0.);
    ren->ResetCamera();
}

void vtkWindowCube::setCameraAzimuth(double az)
{
    this->resetCubeCamera();
    ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActiveCamera()->Azimuth(az);
    ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::setCameraElevation(double el)
{
    this->resetCubeCamera();
    ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActiveCamera()->Elevation(
            el);
    ui->vtkCube->renderWindow()->Render();
}

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

void vtkWindowCube::finishRegionInteraction()
{
    if (this->isBusy()) {
        return;
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
            ui->vtkImage->renderWindow()->Render();
        }
        return;
    }

    const int *position = ui->vtkImage->renderWindow()->GetInteractor()->GetEventPosition();
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
            ui->vtkImage->renderWindow()->Render();
            return;
        }
        this->regionAnnulusInnerRadius = innerRadius;
        this->regionDragging = false;
        this->regionValid = true;
        this->refreshRegionOverlay();
        ui->vtkImage->renderWindow()->Render();
    }
    this->regionDragging = false;
    this->regionValid = true;
    this->refreshRegionOverlay();
    this->analyzeCurrentRegion();
    ui->vtkImage->renderWindow()->Render();
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

void vtkWindowCube::finalizePvInteraction()
{
    if (this->isBusy()) {
        return;
    }

    if (this->regionMode == RegionMode::Polygon && this->regionDragging) {
        if (this->finalizePolygonRegion()) {
            ui->vtkImage->renderWindow()->Render();
        }
        return;
    }

    if (!this->pvModeActive) {
        return;
    }

    const int *position = ui->vtkImage->renderWindow()->GetInteractor()->GetEventPosition();
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
        ui->vtkImage->renderWindow()->Render();
        return;
    }

    this->extractCurrentPvDiagram();
    ui->vtkImage->renderWindow()->Render();
    // DO NOT toggle off actionExtractPvDiagram here. It triggers
    // setPvModeActive(false) → clearPv() (guarded by `if (!pvDiagramWidget)`
    // which is still null until the async backend response arrives), wiping
    // the polyline before the plot window can take over the overlay's
    // lifetime. The uncheck happens later, inside the pvWatcher 'finished'
    // handler, right after pvDiagramWidget is created.
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

void vtkWindowCube::setRegionMode(RegionMode mode, bool active)
{
    if (!active) {
        if (this->regionMode == mode) {
            this->regionMode = RegionMode::None;
            this->regionDragging = false;
            // Keep the overlay visible while the result dialog and / or
            // spectrum plot for this region are still open — analogous to
            // how the PV overlay survives until pvDiagramWidget closes.
            // maybeClearRegionAfterDisplay() will wipe it once both close.
            const bool keepingForDisplay =
                    this->m_regionResultDialog || this->probePlotWidget;
            if (!keepingForDisplay) {
                this->clearRegion();
            }
            this->setInteractorStyleImage();
            ui->vtkImage->setCursor(this->probeModeActive ? Qt::CrossCursor : Qt::ArrowCursor);
            if (ui->vtkImage->renderWindow()) {
                ui->vtkImage->renderWindow()->Render();
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
    ui->vtkImage->setCursor(Qt::CrossCursor);
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
        ui->vtkImage->setCursor(this->probeModeActive ? Qt::CrossCursor : Qt::ArrowCursor);
        if (ui->vtkImage->renderWindow()) {
            ui->vtkImage->renderWindow()->Render();
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
    ui->vtkImage->setCursor(Qt::CrossCursor);
}

bool vtkWindowCube::updateRegionFromDisplayPosition(int displayX, int displayY)
{
    auto *renderer = ui->vtkImage->renderWindow() ? ui->vtkImage->renderWindow()->GetRenderers()->GetFirstRenderer()
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

bool vtkWindowCube::updatePvFromDisplayPosition(int displayX, int displayY)
{
    auto *renderer = ui->vtkImage->renderWindow() ? ui->vtkImage->renderWindow()->GetRenderers()->GetFirstRenderer()
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

    if (ui->vtkImage && ui->vtkImage->renderWindow()) {
        ui->vtkImage->renderWindow()->Render();
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

    if (ui->vtkImage && ui->vtkImage->renderWindow()) {
        ui->vtkImage->renderWindow()->Render();
    }
    if (ui->vtkCube && ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }
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
    auto *ren = ui->vtkImage->renderWindow()
                        ? ui->vtkImage->renderWindow()->GetRenderers()->GetFirstRenderer()
                        : nullptr;
    if (ren && !ren->HasViewProp(m_beamActor)) {
        ren->AddActor(m_beamActor);
    }
    if (ui->vtkImage->renderWindow()) {
        ui->vtkImage->renderWindow()->Render();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCube::clearRegion()
{
    this->regionValid = false;
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
}

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

void vtkWindowCube::analyzeCurrentRegion()
{
    if (this->regionMode == RegionMode::None || !this->regionValid) {
        return;
    }

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

    // ── Show results in ToolResultDialog ─────────────────────────────────────
    // The dialog shows 2D global statistics (computed on the current slice or
    // moment map).  The spectral plot (ProfileWidget) shows the mean per channel
    // across all channels of the 3D cube — these are complementary views.
    const QString dataSource = this->viewingSlice()
            ? u"current 2D slice"_s : u"current moment map"_s;

    auto *dlg = new ToolResultDialog(this);
    dlg->setTitle(u"Region Analysis"_s);
    dlg->setSubtitle(
            u"%1  ·  global 2D statistics from the %2"_s.arg(shapeLabel, dataSource));

    if (!stats.valid) {
        dlg->addSection(u"Result"_s, {
            { u"Status"_s, u"No valid pixels in the selected region (all blanked / NaN)"_s },
        });
    } else {
        dlg->addSection(u"2D Statistics"_s, {
            { u"Region"_s,       shapeLabel },
            { u"Valid pixels"_s, u"%1 / %2"_s.arg(stats.validCount).arg(stats.totalCount) },
            { u"Blanked / NaN"_s, QString::number(stats.blankedCount) },
            { u"Min"_s,          fmt(stats.minValue) },
            { u"Max"_s,          fmt(stats.maxValue) },
            { u"Mean"_s,         fmt(stats.mean) },
            { u"Median"_s,       fmt(stats.median) },
            { u"Std Dev"_s,      fmt(stats.stddev) },
        });
        dlg->addSection(u"Note"_s, {
            { u"Spectral profile"_s, u"Mean per channel across all cube channels (see plot window)"_s },
        });
    }
    // Track the dialog so the region overlay stays visible on the 2D image
    // for as long as either the result dialog or the spectrum plot is open.
    // Closing the LAST one triggers maybeClearRegionAfterDisplay() which
    // wipes the overlay (and resets regionMode if still pending).
    // Manual null-before-call: see the matching block on probePlotWidget.
    this->m_regionResultDialog = dlg;
    QObject::connect(dlg, &QObject::destroyed, this,
                     [this]() {
                         this->m_regionResultDialog = nullptr;
                         this->maybeClearRegionAfterDisplay();
                     });
    dlg->show();
}

void vtkWindowCube::maybeClearRegionAfterDisplay()
{
    // Called from the destroyed() signals of the region result dialog and
    // the region spectrum plot. Both can be open simultaneously; only when
    // the last one is gone do we wipe the 2D overlay. QPointer auto-nulls
    // on destruction so the checks below are race-free.
    if (this->m_regionResultDialog) return;
    if (this->probePlotWidget) return;
    if (this->regionMode != RegionMode::None) return;  // user re-armed a tool
    this->clearRegion();
    if (ui->vtkImage && ui->vtkImage->renderWindow()) {
        ui->vtkImage->renderWindow()->Render();
    }
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
    const auto cfg = this->momentController->lastAcceptedConfig();
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
             maskEnabled, thresholdValue, basename]() {
                BackendClient c(url, token);
                c.setSessionId(sessionId);
                return c.saveMomentFits(datasetId, order, chStart, chEnd,
                                        maskEnabled, thresholdValue, basename);
            }));
}

bool vtkWindowCube::finalizePolygonRegion()
{
    const int *position = ui->vtkImage->renderWindow()->GetInteractor()->GetEventPosition();
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
    return ui->vtkImage->renderWindow() == this->sliceWin;
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

        ui->vtkImage->renderWindow()->Render();
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

void vtkWindowCube::requestRemoteSlice(int sliceIndex)
{
    sliceIndex = this->clampRemoteSliceIndex(sliceIndex);
    this->currentRequestedRemoteSliceIndex = sliceIndex;
    // Mark this as a USER-initiated request so its resulting
    // applyRemoteSliceResult is allowed to spawn ±1 prefetches.
    // Prefetched results themselves leave this flag false → no cascade.
    this->prefetchAllowedForNextResult = true;
    // Cancel any pending idle prefetch — the user just acted, so a
    // previously-scheduled prefetch is no longer interesting. The
    // applyRemoteSliceResult for THIS request will re-arm the timer.
    this->idlePrefetchTimer.stop();
    if (this->tryApplyCachedRemoteSlice(sliceIndex)) {
        return;
    }

    const QString key = this->remoteSliceCacheKey(sliceIndex);
    if (this->remoteSliceFetchesInFlight.contains(key)) {
        this->statusBar()->showMessage(u"Loading remote slice..."_s);
        return;
    }

    const int requestId = ++this->currentRemoteSliceRequestId;
    ++this->activeRemoteSliceRequests;
    this->statusBar()->showMessage(u"Loading remote slice..."_s);
    this->startRemoteSliceFetch(sliceIndex, false, requestId);
}

void vtkWindowCube::applyRemoteSliceResult(const RemoteCubeSliceResult &result)
{
    // Hide 2D loading placeholder on first real slice tile.
    this->m_slice2dLoadingActor->VisibilityOff();

    this->cacheRemoteSliceResult(result);
    this->remoteSliceDisplaySource->SetOutput(result.imageData);
    this->remoteSliceDisplaySource->Modified();
    auto *img = vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
    if (img) {
        this->sliceColors->SetInputData(img);
    } else {
        qWarning() << "[vtk] Expected vtkImageData for remote slice colors but got null";
        return;
    }
    this->lutSlice->SetTableRange(result.imageRange[0], result.imageRange[1]);

    const QSignalBlocker blockSlider(ui->sliderSlice);
    const QSignalBlocker blockSpin(ui->spinSlice);
    ui->sliderSlice->setValue(result.index + 1);
    ui->spinSlice->setValue(result.index + 1);
    ui->lineSpectral->setText(this->formatSpectralAxisValue(result.index));
    this->refreshSpectralAxisUi();
    this->updateRemoteCuttingPlane(result.index);
    ui->vtkCube->renderWindow()->Render();
    qDebug().noquote() << QStringLiteral("[remote-plane] render triggered");

    if (this->viewingSlice()) {
        ui->lineImgMin->setText(fmtStat(result.imageRange[0]));
        ui->lineImgMax->setText(fmtStat(result.imageRange[1]));
        double mean, rms;
        computeImageStats(result.imageData, mean, rms);
        ui->lineImgMean->setText(fmtStat(mean));
        ui->lineImgRms->setText(fmtStat(rms));
        this->updateLUTCustomizer();
    }

    auto *sliceRenderer = this->sliceWin->GetRenderers()->GetFirstRenderer();
    refitParallelSliceCamera(sliceRenderer, result.imageData, this->sliceWin);
    sliceRenderer->ResetCameraClippingRange();
    if (this->probeModeActive && this->probeValid) {
        this->refreshProbeOverlay();
        this->updateProbeReadout(result.imageData);
    }
    this->sliceWin->Render();
    // Smart prefetch: schedule ±1 neighbour fetch ``idlePrefetchDelayMs``
    // from now, only when this slice was the user's explicit request
    // (prefetchAllowedForNextResult) — not a prefetched neighbour itself
    // (no cascade). A new requestRemoteSlice() will CANCEL this timer
    // (see requestRemoteSlice), so rapid scroll produces zero prefetches
    // and the SSH tunnel stays clear for the user-visible slice.
    if (this->prefetchAllowedForNextResult) {
        this->prefetchAllowedForNextResult = false;
        this->idlePrefetchTimer.start(idlePrefetchDelayMs);
    }
}

void vtkWindowCube::updateRemoteSliceDragFeedback(int sliceIndex)
{
    const int clampedSlice = this->clampRemoteSliceIndex(sliceIndex);
    this->currentRequestedRemoteSliceIndex = clampedSlice;
    ui->lineSpectral->setText(this->formatSpectralAxisValue(clampedSlice));
    this->updateRemoteCuttingPlane(clampedSlice);
    ui->vtkCube->renderWindow()->Render();
}

int vtkWindowCube::remoteSliceCount() const
{
    return std::max(1, this->remoteDatasetDepth);
}

int vtkWindowCube::clampRemoteSliceIndex(int sliceIndex) const
{
    return std::clamp(sliceIndex, 0, this->remoteSliceCount() - 1);
}

double vtkWindowCube::remoteSliceCoordinate(int sliceIndex) const
{
    const int clampedSlice = this->clampRemoteSliceIndex(sliceIndex);
    bool ok = false;
    const double axisValue = this->spectralAxisValue(static_cast<double>(clampedSlice), &ok);
    return ok ? axisValue : static_cast<double>(clampedSlice);
}

vtkWindowCube::SpectralAxisDescriptor vtkWindowCube::spectralAxisDescriptor() const
{
    const bool hasBackendSpectralMetadata = !this->remoteSpectralAxisType.trimmed().isEmpty()
            || !this->remoteSpectralAxisUnit.trimmed().isEmpty();
    if (hasBackendSpectralMetadata) {
        auto descriptor =
                inferSpectralAxisDescriptor(this->remoteSpectralAxisType,
                                           !this->remoteSpectralAxisUnit.trimmed().isEmpty()
                                                   ? this->remoteSpectralAxisUnit
                                                   : this->remoteDatasetCunit[2],
                                           true);
        descriptor.trusted = true;
        descriptor.inferred = false;
        descriptor.physical = true;
        descriptor.sourceLabel = this->remoteDatasetCtype[2].trimmed().isEmpty()
                ? this->remoteSpectralAxisType.trimmed()
                : this->remoteDatasetCtype[2].trimmed();
        if (descriptor.label.trimmed().isEmpty()) {
            descriptor.label = u"Spectral"_s;
        }
        return descriptor;
    }
    return inferSpectralAxisDescriptor(this->remoteDatasetCtype[2], this->remoteDatasetCunit[2],
                                       this->remoteHasWcsAxis(2));
}

double vtkWindowCube::spectralAxisValue(double datasetVoxelIndex, bool *ok) const
{
    const auto descriptor = this->spectralAxisDescriptor();
    if (descriptor.kind == SpectralAxisKind::Channel || !descriptor.physical) {
        if (ok) {
            *ok = false;
        }
        return datasetVoxelIndex;
    }

    return this->remoteVoxelToWcs(2, datasetVoxelIndex, ok);
}

QString vtkWindowCube::formatSpectralAxisValue(double datasetVoxelIndex) const
{
    const auto descriptor = this->spectralAxisDescriptor();
    bool ok = false;
    const double value = this->spectralAxisValue(datasetVoxelIndex, &ok);
    if (descriptor.kind == SpectralAxisKind::Channel || !descriptor.physical || !ok) {
        return QString::number(std::lround(datasetVoxelIndex));
    }

    if (descriptor.unit.isEmpty()) {
        return QString::number(value, 'g', 12);
    }
    return u"%1 %2"_s.arg(QString::number(value, 'g', 12), descriptor.unit);
}

QString vtkWindowCube::spectralAxisTitle() const
{
    const auto descriptor = this->spectralAxisDescriptor();
    if (descriptor.unit.isEmpty() || descriptor.kind == SpectralAxisKind::Channel || !descriptor.physical) {
        return descriptor.label;
    }
    return u"%1 (%2)"_s.arg(descriptor.label, descriptor.unit);
}

QString vtkWindowCube::spectralAxisTooltip() const
{
    const auto descriptor = this->spectralAxisDescriptor();
    QString trust;
    if (descriptor.kind == SpectralAxisKind::Channel || !descriptor.physical) {
        trust = u"Fallback to channel index."_s;
    } else if (!this->remoteSpectralAxisType.trimmed().isEmpty()) {
        trust = u"Inferred from backend WCS."_s;
    } else if (descriptor.trusted) {
        trust = u"Trusted from FITS spectral metadata."_s;
    } else if (descriptor.inferred) {
        trust = u"Inferred from FITS CTYPE3."_s;
    } else {
        trust = u"Generic spectral-axis interpretation."_s;
    }

    const QString source = descriptor.sourceLabel.isEmpty() ? descriptor.label : descriptor.sourceLabel;
    const QString unit = descriptor.unit.isEmpty() ? u"unknown units"_s : descriptor.unit;
    return u"%1 Native axis: %2. Units: %3"_s.arg(trust, source, unit);
}

void vtkWindowCube::refreshSpectralAxisUi()
{
    const auto descriptor = this->spectralAxisDescriptor();
    const QString title = this->spectralAxisTitle();
    const QString tooltip = this->spectralAxisTooltip();
    ui->labelSliceHdr->setText(u"CUTTING PLANE (%1)"_s.arg(title).toUpper());
    ui->labelSliceHdr->setToolTip(tooltip);
    ui->lineSpectral->setToolTip(tooltip);
    ui->lineSpectral->setStatusTip(tooltip);
    qDebug().noquote()
            << QStringLiteral("[spectral] axis3 title=%1 unit=%2 source=%3")
                       .arg(descriptor.label,
                            descriptor.unit.isEmpty() ? QStringLiteral("unknown units")
                                                      : descriptor.unit,
                            !this->remoteSpectralAxisType.trimmed().isEmpty()
                                    ? QStringLiteral("backend_wcs")
                                    : descriptor.sourceLabel);
    this->updateDataStatePanel();
    this->updateSanityPanel();
}

bool vtkWindowCube::remoteHasWcsAxis(int axis) const
{
    return axis >= 0 && axis < 3 && std::isfinite(this->remoteDatasetCrval[axis])
            && std::isfinite(this->remoteDatasetCrpix[axis])
            && std::isfinite(this->remoteDatasetCdelt[axis])
            && std::abs(this->remoteDatasetCdelt[axis]) > 1e-12;
}

double vtkWindowCube::remoteVoxelToWcs(int axis, double voxelIndex, bool *ok) const
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

QString vtkWindowCube::remoteFormatAxisCoordinate(int axis, double voxelIndex) const
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

QString vtkWindowCube::remoteAxisTitle(int axis) const
{
    const QString base = axis == 0 ? u"X"_s : (axis == 1 ? u"Y"_s : u"Z"_s);
    const QString ctype = (axis >= 0 && axis < 3) ? this->remoteDatasetCtype[axis].trimmed() : QString();
    const QString cunit = (axis >= 0 && axis < 3) ? this->remoteDatasetCunit[axis].trimmed() : QString();
    const QString label = ctype.isEmpty() ? base : ctype;
    return cunit.isEmpty() ? label : u"%1 (%2)"_s.arg(label, cunit);
}

int vtkWindowCube::selectedWcsFrame() const
{
    return ui->actionGalactic->isChecked() ? WCS_GALACTIC
            : (ui->actionFK5->isChecked() ? WCS_J2000 : WCS_ECLIPTIC);
}

int vtkWindowCube::remoteNativeCelestialFrame() const
{
    return inferCelestialFrameFromCtypePair(this->remoteDatasetCtype);
}

bool vtkWindowCube::remoteHasCelestialAxes() const
{
    return this->remoteNativeCelestialFrame() >= 0 && this->remoteHasWcsAxis(0)
            && this->remoteHasWcsAxis(1);
}

bool vtkWindowCube::convertRemoteCelestialCoordinates(double nativeX, double nativeY, double &frameX,
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

QString vtkWindowCube::formatRemoteOverlayCoordinate(int axis, double value) const
{
    if (!this->remoteHasCelestialAxes()) {
        return QString::number(value, 'g', 8);
    }
    if (!this->useSexagesimalWcsFormat) {
        return this->formatDegreeCoordinate(value);
    }
    return formatCelestialCoordinate(this->selectedWcsFrame(), axis, value);
}

QString vtkWindowCube::remoteOverlayAxisTitle(int axis) const
{
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

QString vtkWindowCube::formatDegreeCoordinate(double value) const
{
    return QString::number(value, 'f', 2);
}

QString vtkWindowCube::currentWcsFrameLabel() const
{
    const int frame = this->selectedWcsFrame();
    return frame == WCS_GALACTIC ? u"Galactic"_s
            : (frame == WCS_J2000 ? u"FK5"_s : u"Ecliptic"_s);
}

void vtkWindowCube::updateMomentProvenancePanel()
{
    if (!this->momentProvenanceLabel) {
        return;
    }

    if (!this->momentProvenanceState.valid) {
        this->momentProvenanceLabel->setVisible(false);
        return;
    }

    this->momentProvenanceLabel->setText(this->momentProvenanceState.summary);
    this->momentProvenanceLabel->setToolTip(this->momentProvenanceState.details);
    this->momentProvenanceLabel->setVisible(true);
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

QString vtkWindowCube::selectedFrameAxisTitle(int axis) const
{
    return this->remoteOverlayAxisTitle(axis);
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
}

void vtkWindowCube::ensureOverlayTickActors(
        vtkRenderer *renderer, std::vector<vtkSmartPointer<vtkTextActor>> &xActors,
        std::vector<vtkSmartPointer<vtkTextActor>> &yActors)
{
    if (!renderer) {
        return;
    }
    if (xActors.empty()) {
        for (int i = 0; i < overlayTickCount; ++i) {
            auto actor = vtkSmartPointer<vtkTextActor>::New();
            configureTickLabelActor(actor, false);
            renderer->AddViewProp(actor);
            xActors.push_back(actor);
        }
    }
    if (yActors.empty()) {
        for (int i = 0; i < overlayTickCount; ++i) {
            auto actor = vtkSmartPointer<vtkTextActor>::New();
            configureTickLabelActor(actor, true);
            renderer->AddViewProp(actor);
            yActors.push_back(actor);
        }
    }
}

void vtkWindowCube::invalidateWcsOverlayCache()
{
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
}

void vtkWindowCube::applyDefaultWcsFormatForSelectedFrame()
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

void vtkWindowCube::requestWcsOverlayRender()
{
    QMetaObject::invokeMethod(
            this,
            [this]() {
                this->updateSliceWcsOverlay();
                this->updateMomentWcsOverlay();
                if (this->probeValid) {
                    this->refreshProbeOverlay();
                    auto *currentImage = this->viewingSlice()
                            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
                            : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
                    this->updateProbeReadout(currentImage);
                }
                if (!ui || !ui->vtkImage) {
                    return;
                }
                if (this->viewingSlice() && this->sliceWin) {
                    this->sliceWin->Render();
                } else if (!this->viewingSlice() && this->momentWin) {
                    this->momentWin->Render();
                } else if (ui->vtkImage->renderWindow()) {
                    ui->vtkImage->renderWindow()->Render();
                }
                ui->vtkImage->update();
            },
            Qt::QueuedConnection);
}

void vtkWindowCube::updateSliceWcsOverlay()
{
    if (!this->sliceWcsOverlayInitialized || !this->sliceWin) {
        qDebug().noquote() << QStringLiteral("[wcs-overlay] slice overlay not initialized win=%1")
                                      .arg(reinterpret_cast<quintptr>(this->sliceWin.GetPointer()), 0, 16);
        return;
    }

    auto *rendererCollection = this->sliceWin->GetRenderers();
    auto *renderer = rendererCollection ? rendererCollection->GetFirstRenderer() : nullptr;
    auto *imageData = this->viewingSlice()
            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
            : nullptr;
    if (!renderer || !this->sliceOverlayXAxis.GetPointer() || !this->sliceOverlayYAxis.GetPointer()
        || !this->sliceOverlayXTitleActor.GetPointer() || !this->sliceOverlayYTitleActor.GetPointer()) {
        qDebug().noquote()
                << QStringLiteral("[wcs-overlay] slice missing objects renderer=%1 win=%2 xAxis=%3 yAxis=%4 xTitle=%5 yTitle=%6 image=%7")
                           .arg(reinterpret_cast<quintptr>(renderer), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->sliceWin.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->sliceOverlayXAxis.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->sliceOverlayYAxis.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->sliceOverlayXTitleActor.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->sliceOverlayYTitleActor.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(imageData), 0, 16);
        return;
    }
    this->set2dWcsOverlayVisible(this->showWcsAxes);
    if (!this->showWcsAxes || !renderer || !imageData) {
        return;
    }
    this->ensureOverlayTickActors(renderer, this->sliceOverlayXTickActors, this->sliceOverlayYTickActors);
    if (this->sliceOverlayXTickActors.size() < overlayTickCount
        || this->sliceOverlayYTickActors.size() < overlayTickCount) {
        qDebug().noquote()
                << QStringLiteral("[wcs-overlay] slice tick actors incomplete x=%1 y=%2")
                           .arg(this->sliceOverlayXTickActors.size())
                           .arg(this->sliceOverlayYTickActors.size());
        return;
    }

    const auto visible = computeVisibleImageBounds2D(renderer, imageData);
    if (!visible.valid) {
        this->sliceOverlayXAxis->VisibilityOff();
        this->sliceOverlayYAxis->VisibilityOff();
        this->sliceOverlayXTitleActor->VisibilityOff();
        this->sliceOverlayYTitleActor->VisibilityOff();
        return;
    }

    const int *size = renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0) {
        return;
    }

    const std::array<double, 4> visibleBounds = { visible.xmin, visible.xmax, visible.ymin, visible.ymax };
    const std::array<int, 2> viewportSize = { size[0], size[1] };
    if (visibleBounds == this->lastSliceOverlayVisibleBounds
        && viewportSize == this->lastSliceOverlayViewportSize) {
        return;
    }
    this->lastSliceOverlayVisibleBounds = visibleBounds;
    this->lastSliceOverlayViewportSize = viewportSize;

    constexpr double leftMargin = 168.;
    constexpr double axisX = 136.;
    constexpr double bottomMargin = 58.;
    constexpr double rightMargin = 34.;
    constexpr double topMargin = 28.;
    configureAxisActor(this->sliceOverlayXAxis, axisX, bottomMargin, size[0] - rightMargin,
                       bottomMargin);
    configureAxisActor(this->sliceOverlayYAxis, axisX, size[1] - topMargin, axisX, bottomMargin);
    this->sliceOverlayXAxis->SetTitle("");
    this->sliceOverlayXTitleActor->SetInput(this->remoteOverlayAxisTitle(0).toStdString().c_str());
    this->sliceOverlayXTitleActor->SetDisplayPosition((axisX + (size[0] - rightMargin)) / 2,
                                                      static_cast<int>(bottomMargin / 2.0) - 2);
    this->sliceOverlayYAxis->SetTitle("");
    this->sliceOverlayYTitleActor->SetInput(this->remoteOverlayAxisTitle(1).toStdString().c_str());
    this->sliceOverlayYTitleActor->SetDisplayPosition(static_cast<int>(leftMargin / 3.0), size[1] / 2);

    bool xOk = false;
    bool yOk = false;
    const double xMin = this->remoteVoxelToWcs(0, visible.xmin, &xOk);
    const double xMax = this->remoteVoxelToWcs(0, visible.xmax, &xOk);
    const double yMin = this->remoteVoxelToWcs(1, visible.ymin, &yOk);
    const double yMax = this->remoteVoxelToWcs(1, visible.ymax, &yOk);
    this->sliceOverlayXAxis->SetRange(xOk ? xMin : visible.xmin, xOk ? xMax : visible.xmax);
    this->sliceOverlayYAxis->SetRange(yOk ? yMax : visible.ymax, yOk ? yMin : visible.ymin);
    this->sliceOverlayXAxis->LabelVisibilityOff();
    this->sliceOverlayYAxis->LabelVisibilityOff();
    this->sliceOverlayXAxis->VisibilityOn();
    this->sliceOverlayYAxis->VisibilityOn();
    this->sliceOverlayXTitleActor->VisibilityOn();
    this->sliceOverlayYTitleActor->VisibilityOn();
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
        this->sliceOverlayXTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(0, frameX).toStdString().c_str());
        this->sliceOverlayXTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX + t * ((size[0] - rightMargin) - axisX)),
                static_cast<int>(bottomMargin - 18));
        this->sliceOverlayXTickActors[static_cast<std::size_t>(i)]->VisibilityOn();
        this->sliceOverlayYTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(1, frameY).toStdString().c_str());
        this->sliceOverlayYTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX - 10),
                static_cast<int>(bottomMargin + t * ((size[1] - topMargin) - bottomMargin)));
        this->sliceOverlayYTickActors[static_cast<std::size_t>(i)]->VisibilityOn();
    }
    qDebug().noquote()
            << QStringLiteral("[wcs-overlay] updated ticks for slice x=%1..%2 y=%3..%4 size=%5x%6 actor=%7 endpoints=(%8,%9)->(%10,%11) outer=%12")
                       .arg(visible.xmin, 0, 'g', 12)
                       .arg(visible.xmax, 0, 'g', 12)
                       .arg(visible.ymin, 0, 'g', 12)
                       .arg(visible.ymax, 0, 'g', 12)
                       .arg(size[0])
                       .arg(size[1])
                       .arg(this->sliceOverlayXAxis->GetVisibility())
                       .arg(axisX, 0, 'g', 12)
                       .arg(size[1] - topMargin, 0, 'g', 12)
                       .arg(axisX, 0, 'g', 12)
                       .arg(bottomMargin, 0, 'g', 12)
                       .arg(leftMargin, 0, 'g', 12);
}

void vtkWindowCube::updateMomentWcsOverlay()
{
    if (!this->momentWcsOverlayInitialized || !this->momentWin) {
        qDebug().noquote() << QStringLiteral("[wcs-overlay] moment overlay not initialized win=%1")
                                      .arg(reinterpret_cast<quintptr>(this->momentWin.GetPointer()), 0, 16);
        return;
    }

    auto *rendererCollection = this->momentWin->GetRenderers();
    auto *renderer = rendererCollection ? rendererCollection->GetFirstRenderer() : nullptr;
    auto *imageData = vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    if (!renderer || !this->momentOverlayXAxis.GetPointer() || !this->momentOverlayYAxis.GetPointer()
        || !this->momentOverlayXTitleActor.GetPointer() || !this->momentOverlayYTitleActor.GetPointer()) {
        qDebug().noquote()
                << QStringLiteral("[wcs-overlay] moment missing objects renderer=%1 win=%2 xAxis=%3 yAxis=%4 xTitle=%5 yTitle=%6 image=%7")
                           .arg(reinterpret_cast<quintptr>(renderer), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->momentWin.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->momentOverlayXAxis.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->momentOverlayYAxis.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->momentOverlayXTitleActor.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->momentOverlayYTitleActor.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(imageData), 0, 16);
        return;
    }
    this->set2dWcsOverlayVisible(this->showWcsAxes);
    if (!this->showWcsAxes || !renderer || !imageData) {
        return;
    }
    this->ensureOverlayTickActors(renderer, this->momentOverlayXTickActors, this->momentOverlayYTickActors);
    if (this->momentOverlayXTickActors.size() < overlayTickCount
        || this->momentOverlayYTickActors.size() < overlayTickCount) {
        qDebug().noquote()
                << QStringLiteral("[wcs-overlay] moment tick actors incomplete x=%1 y=%2")
                           .arg(this->momentOverlayXTickActors.size())
                           .arg(this->momentOverlayYTickActors.size());
        return;
    }

    const auto visible = computeVisibleImageBounds2D(renderer, imageData);
    if (!visible.valid) {
        this->momentOverlayXAxis->VisibilityOff();
        this->momentOverlayYAxis->VisibilityOff();
        this->momentOverlayXTitleActor->VisibilityOff();
        this->momentOverlayYTitleActor->VisibilityOff();
        return;
    }

    const int *size = renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0) {
        return;
    }

    const std::array<double, 4> visibleBounds = { visible.xmin, visible.xmax, visible.ymin, visible.ymax };
    const std::array<int, 2> viewportSize = { size[0], size[1] };
    if (visibleBounds == this->lastMomentOverlayVisibleBounds
        && viewportSize == this->lastMomentOverlayViewportSize) {
        return;
    }
    this->lastMomentOverlayVisibleBounds = visibleBounds;
    this->lastMomentOverlayViewportSize = viewportSize;

    constexpr double leftMargin = 168.;
    constexpr double axisX = 136.;
    constexpr double bottomMargin = 58.;
    constexpr double rightMargin = 34.;
    constexpr double topMargin = 28.;
    configureAxisActor(this->momentOverlayXAxis, axisX, bottomMargin, size[0] - rightMargin,
                       bottomMargin);
    configureAxisActor(this->momentOverlayYAxis, axisX, size[1] - topMargin, axisX, bottomMargin);
    this->momentOverlayXAxis->SetTitle("");
    this->momentOverlayXTitleActor->SetInput(this->remoteOverlayAxisTitle(0).toStdString().c_str());
    this->momentOverlayXTitleActor->SetDisplayPosition((axisX + (size[0] - rightMargin)) / 2,
                                                       static_cast<int>(bottomMargin / 2.0) - 2);
    this->momentOverlayYAxis->SetTitle("");
    this->momentOverlayYTitleActor->SetInput(this->remoteOverlayAxisTitle(1).toStdString().c_str());
    this->momentOverlayYTitleActor->SetDisplayPosition(static_cast<int>(leftMargin / 3.0), size[1] / 2);

    bool xOk = false;
    bool yOk = false;
    const double xMin = this->remoteVoxelToWcs(0, visible.xmin, &xOk);
    const double xMax = this->remoteVoxelToWcs(0, visible.xmax, &xOk);
    const double yMin = this->remoteVoxelToWcs(1, visible.ymin, &yOk);
    const double yMax = this->remoteVoxelToWcs(1, visible.ymax, &yOk);
    this->momentOverlayXAxis->SetRange(xOk ? xMin : visible.xmin, xOk ? xMax : visible.xmax);
    this->momentOverlayYAxis->SetRange(yOk ? yMax : visible.ymax, yOk ? yMin : visible.ymin);
    this->momentOverlayXAxis->LabelVisibilityOff();
    this->momentOverlayYAxis->LabelVisibilityOff();
    this->momentOverlayXAxis->VisibilityOn();
    this->momentOverlayYAxis->VisibilityOn();
    this->momentOverlayXTitleActor->VisibilityOn();
    this->momentOverlayYTitleActor->VisibilityOn();
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
        this->momentOverlayXTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(0, frameX).toStdString().c_str());
        this->momentOverlayXTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX + t * ((size[0] - rightMargin) - axisX)),
                static_cast<int>(bottomMargin - 18));
        this->momentOverlayXTickActors[static_cast<std::size_t>(i)]->VisibilityOn();
        this->momentOverlayYTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(1, frameY).toStdString().c_str());
        this->momentOverlayYTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX - 10),
                static_cast<int>(bottomMargin + t * ((size[1] - topMargin) - bottomMargin)));
        this->momentOverlayYTickActors[static_cast<std::size_t>(i)]->VisibilityOn();
    }
    qDebug().noquote()
            << QStringLiteral("[wcs-overlay] updated ticks for moment x=%1..%2 y=%3..%4 size=%5x%6 actor=%7 endpoints=(%8,%9)->(%10,%11) outer=%12")
                       .arg(visible.xmin, 0, 'g', 12)
                       .arg(visible.xmax, 0, 'g', 12)
                       .arg(visible.ymin, 0, 'g', 12)
                       .arg(visible.ymax, 0, 'g', 12)
                       .arg(size[0])
                       .arg(size[1])
                       .arg(this->momentOverlayXAxis->GetVisibility())
                       .arg(axisX, 0, 'g', 12)
                       .arg(size[1] - topMargin, 0, 'g', 12)
                       .arg(axisX, 0, 'g', 12)
                       .arg(bottomMargin, 0, 'g', 12)
                       .arg(leftMargin, 0, 'g', 12);
}

QString vtkWindowCube::remoteSliceCacheKey(int sliceIndex) const
{
    return QStringLiteral("%1|z|%2")
            .arg(this->remoteDatasetId)
            .arg(this->clampRemoteSliceIndex(sliceIndex));
}

void vtkWindowCube::touchRemoteSliceCacheKey(const QString &key)
{
    this->remoteSliceCacheLru.removeAll(key);
    this->remoteSliceCacheLru.append(key);
}

void vtkWindowCube::cacheRemoteSliceResult(const RemoteCubeSliceResult &result)
{
    if (!result.valid || !result.imageData) {
        return;
    }

    const QString key = this->remoteSliceCacheKey(result.index);
    this->remoteSliceCache.insert(key, result);
    this->touchRemoteSliceCacheKey(key);
    while (this->remoteSliceCacheLru.size() > remoteSliceCacheCapacity) {
        const QString evictedKey = this->remoteSliceCacheLru.takeFirst();
        this->remoteSliceCache.remove(evictedKey);
    }
}

bool vtkWindowCube::tryApplyCachedRemoteSlice(int sliceIndex)
{
    const QString key = this->remoteSliceCacheKey(sliceIndex);
    const auto it = this->remoteSliceCache.constFind(key);
    if (it == this->remoteSliceCache.cend()) {
        return false;
    }

    this->touchRemoteSliceCacheKey(key);
    this->applyRemoteSliceResult(it.value());
    this->clearPersistentStatusMessage();
    return true;
}

void vtkWindowCube::startRemoteSliceFetch(int sliceIndex, bool isPrefetch, int requestId)
{
    sliceIndex = this->clampRemoteSliceIndex(sliceIndex);
    const QString key = this->remoteSliceCacheKey(sliceIndex);
    if (this->remoteSliceFetchesInFlight.contains(key)) {
        return;
    }

    this->remoteSliceFetchesInFlight.insert(key);
    auto *watcher = new QFutureWatcher<RemoteCubeSliceResult>(this);
    watcher->setProperty("requestId", requestId);
    watcher->setProperty("sliceKey", key);
    watcher->setProperty("isPrefetch", isPrefetch);
    QObject::connect(watcher, &QFutureWatcher<RemoteCubeSliceResult>::finished, this,
                     [this, watcher]() {
                         const auto result = watcher->result();
                         const int requestId = watcher->property("requestId").toInt();
                         const QString key = watcher->property("sliceKey").toString();
                         const bool isPrefetch = watcher->property("isPrefetch").toBool();
                         this->remoteSliceFetchesInFlight.remove(key);
                         if (!isPrefetch) {
                             --this->activeRemoteSliceRequests;
                         }
                         watcher->deleteLater();

                         if (!result.valid || !result.imageData) {
                             if (!isPrefetch && requestId == this->currentRemoteSliceRequestId) {
                                 this->persistentStatusActive = false;
                                 this->statusMessageClearTimer.stop();
                                 this->statusBar()->showMessage(result.errorMessage.isEmpty()
                                                                        ? u"Could not load remote slice."_s
                                                                        : result.errorMessage);
                             }
                             return;
                         }

                         this->cacheRemoteSliceResult(result);

                         const bool requestedSliceMatches = this->clampRemoteSliceIndex(result.index)
                                 == this->currentRequestedRemoteSliceIndex;
                         if (!isPrefetch && requestId != this->currentRemoteSliceRequestId) {
                             if (this->activeRemoteSliceRequests == 0) {
                                 this->clearPersistentStatusMessage();
                             }
                             return;
                         }

                         if (requestedSliceMatches) {
                             this->applyRemoteSliceResult(result);
                             if (!isPrefetch && this->activeRemoteSliceRequests == 0) {
                                 this->clearPersistentStatusMessage();
                             }
                         } else if (!isPrefetch && this->activeRemoteSliceRequests == 0) {
                             this->clearPersistentStatusMessage();
                         }
                     });
    watcher->setFuture(QtConcurrent::run(&fetchRemoteSlice, this->remoteBackendUrl,
                                         this->remoteDatasetId, sliceIndex, this->remoteSessionId,
                                         this->remoteBackendToken));
}

void vtkWindowCube::prefetchNeighborRemoteSlices(int sliceIndex)
{
    for (const int neighbor : { sliceIndex - 1, sliceIndex + 1 }) {
        if (neighbor < 0 || neighbor >= this->remoteSliceCount()) {
            continue;
        }

        const QString key = this->remoteSliceCacheKey(neighbor);
        if (this->remoteSliceCache.contains(key) || this->remoteSliceFetchesInFlight.contains(key)) {
            continue;
        }

        this->startRemoteSliceFetch(neighbor, true);
    }
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
            detail += u" · mask \u2265 %1"_s.arg(cfg.thresholdValue, 0, 'g', 4);
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
                         &vtkWindowCube::renderImage);
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
                this->lutSlice);
    } else {
        this->lutCustomizer->init(
                vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0)),
                this->lutMoment);
    }
}

void vtkWindowCube::showLUTCustomizer3D()
{
    if (!this->lutCustomizer3D) {
        this->lutCustomizer3D = new LUTCustomizerDialog(this);
        QObject::connect(this->lutCustomizer3D, &LUTCustomizerDialog::lutUpdated, this,
                         [this]() {
                             // lutVolume was already updated and rebuilt by the dialog.
                             // Re-derive the CTF from the member LUT and re-render.
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
                         });
    }

    // Always reinitialise with the current cube data and member LUT.
    auto *cubeImage =
            vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (cubeImage && cubeImage->GetNumberOfPoints() > 0) {
        this->lutCustomizer3D->init(cubeImage, this->lutVolume);
    }
    this->lutCustomizer3D->show();
    this->lutCustomizer3D->raise();
    this->lutCustomizer3D->activateWindow();
}

void vtkWindowCube::applyMomentMapResult(const MomentMapApplyResult &result)
{
    if (!result.imageData) {
        return;
    }

    QElapsedTimer totalTimer;
    totalTimer.start();

    this->momentDisplaySource->SetOutput(result.imageData);
    auto *img = vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    if (img) {
        this->momentColors->SetInputData(img);
    } else {
        qWarning() << "[vtk] Expected vtkImageData for moment colors but got null";
        return;
    }
    double currentRange[2];
    this->lutMoment->GetTableRange(currentRange);
    if (std::fabs(currentRange[0] - result.imageRange[0]) >= 1e-6
        || std::fabs(currentRange[1] - result.imageRange[1]) >= 1e-6) {
        this->lutMoment->SetTableRange(result.imageRange[0], result.imageRange[1]);
    }

    {
        const QSignalBlocker blockSliceAction(ui->actionSlice);
        const QSignalBlocker blockMomentAction(ui->actionMomentMap);
        ui->actionSlice->setChecked(false);
        ui->actionMomentMap->setChecked(true);
    }
    // Sync the 2D sidebar (actions were blocked above so the toggled handler
    // did not fire; update manually here).
    m_uiAssembler->sync2dModeSidebar(false);
    this->update2dModeVisibility(false);

    ui->vtkImage->setRenderWindow(this->momentWin);
    ui->labelImg->setText(u"Moment Map:"_s);
    this->coordinate->SetViewport(ui->vtkImage->renderWindow()->GetRenderers()->GetFirstRenderer());
    if (auto *renderer = this->momentWin->GetRenderers()->GetFirstRenderer()) {
        renderer->ResetCamera();
        renderer->ResetCameraClippingRange();
    }
    this->lastMomentOverlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
                                             std::numeric_limits<double>::quiet_NaN(),
                                             std::numeric_limits<double>::quiet_NaN(),
                                             std::numeric_limits<double>::quiet_NaN() };
    this->lastMomentOverlayViewportSize = { -1, -1 };
    this->updateMomentWcsOverlay();

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

    this->updateLUTCustomizer();
    if (this->probeModeActive && this->probeValid) {
        this->refreshProbeOverlay();
        this->updateProbeReadout(result.imageData);
    }
    qDebug().noquote()
            << QStringLiteral("[perf][moment] apply data+ui sync: %1 ms").arg(totalTimer.elapsed());
    QElapsedTimer renderTimer;
    renderTimer.start();
    ui->vtkImage->renderWindow()->Render();
    QMetaObject::invokeMethod(
            this,
            [this]() {
                this->lastMomentOverlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
                                                         std::numeric_limits<double>::quiet_NaN(),
                                                         std::numeric_limits<double>::quiet_NaN(),
                                                         std::numeric_limits<double>::quiet_NaN() };
                this->lastMomentOverlayViewportSize = { -1, -1 };
                this->updateMomentWcsOverlay();
                if (this->momentWin) {
                    this->momentWin->Render();
                }
                if (ui && ui->vtkImage) {
                    ui->vtkImage->update();
                }
            },
            Qt::QueuedConnection);
    qDebug().noquote() << QStringLiteral("[perf][moment] render after apply: %1 ms").arg(
            renderTimer.elapsed());
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

void vtkWindowCube::setCubeOpenStateLabel(const QString &text)
{
    if (!this->cubeOpenStateLabel) {
        return;
    }

    if (text.isEmpty()) {
        this->cubeOpenStateLabel->hide();
        this->cubeOpenStateLabel->clear();
        return;
    }

    this->cubeOpenStateLabel->setText(text);
    this->cubeOpenStateLabel->show();
}

void vtkWindowCube::setInteractorStyleImage()
{
    vtkNew<vtkInteractorStyleImage> style;
    ui->vtkImage->renderWindow()->GetInteractor()->SetInteractorStyle(style);
    ui->vtkImage->renderWindow()->Render();
}

void vtkWindowCube::setInteractorStyleRegion()
{
    vtkNew<vtkInteractorStyleUser> style;
    ui->vtkImage->renderWindow()->GetInteractor()->SetInteractorStyle(style);
    ui->vtkImage->renderWindow()->Render();
}

void vtkWindowCube::setInteractorStyleProfile()
{
    if (!this->profileWidget) {
        vtkNew<vtkInteractorStyleProfile> style;
        ui->vtkImage->renderWindow()->GetInteractor()->SetInteractorStyle(style);
        ui->vtkImage->renderWindow()->Render();

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

void vtkWindowCube::changeImageRenderer()
{
    double imgRange[2];
    vtkImageData *imgForStats = nullptr;

    if (ui->actionSlice->isChecked()) {
        ui->vtkImage->setRenderWindow(this->sliceWin);
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
        ui->vtkImage->setRenderWindow(this->momentWin);
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
    this->coordinate->SetViewport(ui->vtkImage->renderWindow()->GetRenderers()->GetFirstRenderer());
    if (this->probeModeActive && this->probeValid) {
        this->refreshProbeOverlay();
        auto *currentImage = this->viewingSlice()
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
    m_uiAssembler->sync2dModeSidebar(ui->actionSlice->isChecked());
    this->update2dModeVisibility(ui->actionSlice->isChecked());
    this->updateInfoStatsPage();
    ui->vtkImage->renderWindow()->Render();
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

void vtkWindowCube::resetCameraFront()
{
    this->setCameraAzimuth(0.);
}

void vtkWindowCube::resetCameraBack()
{
    this->setCameraAzimuth(-180.);
}

void vtkWindowCube::resetCameraTop()
{
    this->setCameraElevation(90.);
}

void vtkWindowCube::resetCameraRight()
{
    this->setCameraAzimuth(90.);
}

void vtkWindowCube::resetCameraBottom()
{
    this->setCameraElevation(-90.);
}

void vtkWindowCube::resetCameraLeft()
{
    this->setCameraAzimuth(-90.);
}

void vtkWindowCube::renderImage()
{
    ui->vtkImage->renderWindow()->Render();
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
    if (this->probePlotWidget) {
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
