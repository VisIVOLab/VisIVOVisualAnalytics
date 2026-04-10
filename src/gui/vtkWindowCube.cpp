#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "ColorMaps.h"
#include "CatalogueOverlayUtils.h"
#include "CatalogueTableModel.h"
#include "LUTCustomizerDialog.h"
#include "MomentMapController.h"
#include "NoiseController.h"
#include "ProfileWidget.h"
#include "PvController.h"
#include "PvDiagramWidget.h"
#include "SidebarPanel.h"
#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
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
#include <vtkLineSource.h>
#include <vtkLookupTable.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkOutlineFilter.h>
#include <vtkPlaneSource.h>
#include <vtkPiecewiseFunction.h>
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
#include <vtkIdTypeArray.h>
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
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QList>
#include <QMetaObject>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
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

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr int overlayTickCount = 5;
constexpr double polygonClosureTolerance = 3.0;
constexpr double catalogueMarkerHalfSize = 3.0;
constexpr int maxCatalogueLabelCount = 200;
constexpr int smartCatalogueLabelMaxVisible = 24;
constexpr double smartCatalogueLabelMinPixelsPerUnit = 2.5;
constexpr double catalogueHoverDisplayThresholdPx = 10.0;
const QString kMomentBackendTooltip =
        QStringLiteral("Moment computation requires a backend-visible dataset. Use File → Open Remote or copy the dataset to the backend host.");
const QString kPvBackendTooltip =
        QStringLiteral("PV computation requires a backend-visible dataset. Use File → Open Remote or copy the dataset to the backend host.");
const QString kIsosurfaceBackendTooltip =
        QStringLiteral("Isosurface extraction requires a backend-visible dataset.");
const QString kNoiseBackendTooltip =
        QStringLiteral("Noise estimation requires a backend-visible dataset. Use File → Open Remote or copy the dataset to the backend host.");

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

struct VisibleImageBounds2D
{
    bool valid{ false };
    double xmin{ 0. };
    double xmax{ 0. };
    double ymin{ 0. };
    double ymax{ 0. };
};

bool pointInVisibleBounds(const std::array<double, 2> &point, const VisibleImageBounds2D &visible)
{
    return visible.valid && point[0] >= visible.xmin && point[0] <= visible.xmax && point[1] >= visible.ymin
            && point[1] <= visible.ymax;
}

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

VisibleImageBounds2D computeVisibleImageBounds2D(vtkRenderer *renderer, vtkImageData *imageData)
{
    VisibleImageBounds2D result;
    if (!renderer || !imageData || !renderer->GetActiveCamera()) {
        return result;
    }

    const int *size = renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0) {
        return result;
    }

    double bounds[6];
    imageData->GetBounds(bounds);
    auto *camera = renderer->GetActiveCamera();
    const double *focalPoint = camera->GetFocalPoint();
    const double halfHeight = camera->GetParallelScale();
    const double halfWidth = halfHeight * static_cast<double>(size[0]) / static_cast<double>(size[1]);

    result.xmin = std::max(bounds[0], focalPoint[0] - halfWidth);
    result.xmax = std::min(bounds[1], focalPoint[0] + halfWidth);
    result.ymin = std::max(bounds[2], focalPoint[1] - halfHeight);
    result.ymax = std::min(bounds[3], focalPoint[1] + halfHeight);
    result.valid = result.xmin <= result.xmax && result.ymin <= result.ymax;
    return result;
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
                                   vtkImageData *cubeImage)
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

    const double blankFraction = computeBlankFraction(cubeImage);
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

bool validBounds(const double bounds[6])
{
    return std::isfinite(bounds[0]) && std::isfinite(bounds[1]) && std::isfinite(bounds[2])
            && std::isfinite(bounds[3]) && std::isfinite(bounds[4]) && std::isfinite(bounds[5])
            && bounds[0] <= bounds[1] && bounds[2] <= bounds[3] && bounds[4] <= bounds[5];
}

bool displayPointToWorld(vtkRenderer *renderer, double x, double y, double z, double world[3])
{
    if (!renderer) {
        return false;
    }

    renderer->SetDisplayPoint(x, y, z);
    renderer->DisplayToWorld();
    const double *worldPoint = renderer->GetWorldPoint();
    if (!worldPoint || std::fabs(worldPoint[3]) <= 1e-9) {
        return false;
    }

    world[0] = worldPoint[0] / worldPoint[3];
    world[1] = worldPoint[1] / worldPoint[3];
    world[2] = worldPoint[2] / worldPoint[3];
    return std::isfinite(world[0]) && std::isfinite(world[1]) && std::isfinite(world[2]);
}

bool intersectLineWithBounds(const double p0[3], const double p1[3], const double bounds[6],
                             double entry[3], double exit[3])
{
    double direction[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    double tMin = 0.0;
    double tMax = 1.0;

    for (int axis = 0; axis < 3; ++axis) {
        const int i0 = axis * 2;
        const int i1 = i0 + 1;
        if (std::fabs(direction[axis]) <= 1e-12) {
            if (p0[axis] < bounds[i0] || p0[axis] > bounds[i1]) {
                return false;
            }
            continue;
        }

        const double invDir = 1.0 / direction[axis];
        double t0 = (bounds[i0] - p0[axis]) * invDir;
        double t1 = (bounds[i1] - p0[axis]) * invDir;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }

    for (int axis = 0; axis < 3; ++axis) {
        entry[axis] = p0[axis] + tMin * direction[axis];
        exit[axis] = p0[axis] + tMax * direction[axis];
    }
    return true;
}

bool computeViewportIntersectionBounds(vtkRenderer *renderer, vtkRenderWindow *window,
                                       const double cubeBounds[6], double bounds[6])
{
    if (!renderer || !window || !validBounds(cubeBounds)) {
        return false;
    }

    const int *size = window->GetSize();
    if (!size || size[0] <= 1 || size[1] <= 1) {
        return false;
    }

    bounds[0] = bounds[2] = bounds[4] = std::numeric_limits<double>::infinity();
    bounds[1] = bounds[3] = bounds[5] = -std::numeric_limits<double>::infinity();
    const std::array<std::pair<double, double>, 9> samples = {{
            { 0.0, 0.0 },
            { static_cast<double>(size[0] - 1), 0.0 },
            { 0.0, static_cast<double>(size[1] - 1) },
            { static_cast<double>(size[0] - 1), static_cast<double>(size[1] - 1) },
            { 0.5 * (size[0] - 1), 0.0 },
            { 0.5 * (size[0] - 1), static_cast<double>(size[1] - 1) },
            { 0.0, 0.5 * (size[1] - 1) },
            { static_cast<double>(size[0] - 1), 0.5 * (size[1] - 1) },
            { 0.5 * (size[0] - 1), 0.5 * (size[1] - 1) },
    }};

    bool hasIntersection = false;
    for (const auto &[x, y] : samples) {
        double nearWorld[3];
        double farWorld[3];
        if (!displayPointToWorld(renderer, x, y, 0.0, nearWorld)
            || !displayPointToWorld(renderer, x, y, 1.0, farWorld)) {
            continue;
        }

        double entry[3];
        double exit[3];
        if (!intersectLineWithBounds(nearWorld, farWorld, cubeBounds, entry, exit)) {
            continue;
        }

        hasIntersection = true;
        for (int axis = 0; axis < 3; ++axis) {
            const int i0 = axis * 2;
            const int i1 = i0 + 1;
            bounds[i0] = std::min(bounds[i0], std::min(entry[axis], exit[axis]));
            bounds[i1] = std::max(bounds[i1], std::max(entry[axis], exit[axis]));
        }
    }

    return hasIntersection && validBounds(bounds);
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

void computeVolumeStats(vtkImageData *image, std::array<double, 2> &range, double &mean, double &rms)
{
    range = { 0., 0. };
    mean = 0.;
    rms = 0.;
    if (!image) {
        return;
    }

    double scalarRange[2];
    image->GetScalarRange(scalarRange);
    range = { scalarRange[0], scalarRange[1] };

    int extent[6];
    image->GetExtent(extent);
    const auto voxelCount = static_cast<qsizetype>(extent[1] - extent[0] + 1)
            * static_cast<qsizetype>(extent[3] - extent[2] + 1)
            * static_cast<qsizetype>(extent[5] - extent[4] + 1);
    if (voxelCount <= 0) {
        return;
    }

    const auto *values = static_cast<const float *>(image->GetScalarPointer());
    double sum = 0.;
    double sumSq = 0.;
    qsizetype finiteCount = 0;
    for (qsizetype i = 0; i < voxelCount; ++i) {
        const double value = values[i];
        if (!std::isfinite(value)) {
            continue;
        }
        sum += value;
        sumSq += value * value;
        ++finiteCount;
    }

    if (finiteCount <= 0) {
        return;
    }

    mean = sum / static_cast<double>(finiteCount);
    rms = std::sqrt(sumSq / static_cast<double>(finiteCount));
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

RemoteCubePreviewResult fetchRemotePreview(const QString &backendUrl, const QString &datasetId,
                                           int downsample, const QString &sessionId,
                                           const QString &backendToken)
{
    RemoteCubePreviewResult result;
    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    const auto response = client.requestPreview(datasetId, downsample);
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
    computeVolumeStats(result.cubeImageData, result.cubeRange, result.cubeMean, result.cubeRms);
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

// Generate a horizontal gradient QPixmap for a named color map.
// Safe to call from the UI thread; w/h are in logical pixels.
QPixmap buildLutPreview(const QString &name, int w, int h)
{
    vtkNew<vtkLookupTable> lut;
    lut->SetTableRange(0., 1.);
    lut->SetNumberOfTableValues(w);
    ColorMaps::SetColorMap(lut, name.toStdString());
    lut->Build();

    QImage img(w, h, QImage::Format_RGB888);
    for (int x = 0; x < w; ++x) {
        const double t = (w > 1) ? static_cast<double>(x) / static_cast<double>(w - 1) : 0.0;
        double rgb[3];
        lut->GetColor(t, rgb);
        const QColor c(std::clamp(static_cast<int>(rgb[0] * 255.0 + 0.5), 0, 255),
                       std::clamp(static_cast<int>(rgb[1] * 255.0 + 0.5), 0, 255),
                       std::clamp(static_cast<int>(rgb[2] * 255.0 + 0.5), 0, 255));
        for (int y = 0; y < h; ++y) {
            img.setPixel(x, y, c.rgb());
        }
    }
    return QPixmap::fromImage(img);
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
                             const QString &remoteBackendToken, QWidget *parent)
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
      lutCustomizer(nullptr),
      profileWidget(nullptr),
      level(15)
{
    ui->setupUi(this);
    this->setupSidebar();
    this->setupWorkspaceDocks();
    this->setWindowTitle(this->filepath);
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
    this->dataStateLabel = new QLabel(this);
    this->dataStateLabel->setStyleSheet(u"QLabel { padding-left: 8px; color: palette(window-text); }"_s);
    this->dataStateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addPermanentWidget(this->dataStateLabel);
    this->sanityLabel = new QLabel(this);
    this->sanityLabel->setStyleSheet(u"QLabel { padding-left: 8px; }"_s);
    this->sanityLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addPermanentWidget(this->sanityLabel);
    this->wcsStatusLabel = new QLabel(this);
    this->wcsStatusLabel->setStyleSheet(u"QLabel { padding-left: 8px; font-weight: 600; }"_s);
    this->wcsStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addPermanentWidget(this->wcsStatusLabel);
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
        this->syncVolumeModeSidebar(idx);
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

    this->updateDataStatePanel();
    this->updateSanityPanel();
    this->updateWcsStatusIndicator();
    this->remoteRoiRefinementCheck = new QCheckBox(u"Use Camera ROI"_s, this);
    this->remoteRoiRefinementCheck->setChecked(this->useCameraRoiRefinement);
    this->remoteRoiRefinementCheck->setToolTip(
            u"Unchecked: refine using the full dataset. Checked: refine using the current camera view."_s);
    this->remoteRoiRefinementCheck->setVisible(true);
    this->remoteRoiRefinementCheck->setEnabled(true);
    this->statusBar()->addPermanentWidget(this->remoteRoiRefinementCheck);
    QObject::connect(this->remoteRoiRefinementCheck, &QCheckBox::toggled, this,
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
                         const int requestId =
                                 this->remotePreviewWatcher.property("requestId").toInt();
                         if (requestId != this->currentRemotePreviewRequestId) {
                             return;
                         }

                         this->setCubeOpenActionsEnabled(true);

                         const auto result = this->remotePreviewWatcher.result();
                         if (!result.valid || !result.cubeImageData) {
                             this->persistentStatusActive = false;
                             this->statusMessageClearTimer.stop();
                             this->statusBar()->showMessage(result.errorMessage.isEmpty()
                                                                    ? u"Could not load preview."_s
                                                                    : result.errorMessage);
                             return;
                         }

                         const double span = result.cubeRange[1] - result.cubeRange[0];
                         this->lowerBound = static_cast<float>(result.cubeRange[0] + 0.1 * span);
                         this->upperBound = static_cast<float>(result.cubeRange[1]);
                         this->applyCubeOpenResult({ true,
                                                     { },
                                                     result.cubeImageData,
                                                     nullptr,
                                                     result.cubeRange,
                                                     { 0., 0. },
                                                     result.dataExtent,
                                                     0.,
                                                     0. });
                         this->setRemoteCubeDisplayState(RemoteCubeDisplayState::Preview);
                         qDebug().noquote() << QStringLiteral("[remote-plane] preview valid");
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
                                                     result.cubeRms });
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
                                    DiagnosticsManager::instance()->publish(
                                            DiagnosticsLevel::Warning,
                                            DiagnosticsCategory::Scientific,
                                            QStringLiteral("MomentMap"),
                                            QStringLiteral("Moment computation failed."),
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

                // Provenance tooltip using the vertices captured at dispatch time.
                const auto &verts = result.capturedVertices;
                const int usedWidth = result.widthPixels > 0 ? result.widthPixels
                                                              : result.capturedWidthPixels;
                const QString details =
                        u"Start: %1\nEnd: %2\nVertices: %3\nPath width: %4 px\n"
                        "Path length: %5 px\n"
                        "Sampling: nearest-neighbor perpendicular averaging\n"
                        "Computation: Computed on backend-visible dataset\n"
                        "Spectral axis: %6\nSpatial unit: %7\n"
                        "Pixel scale: %8 arcsec/pixel"_s
                                .arg(verts.size() >= 1
                                             ? this->formatSpatialPointSummary(verts.front())
                                             : QString{})
                                .arg(verts.size() >= 2
                                             ? this->formatSpatialPointSummary(verts.back())
                                             : QString{})
                                .arg(result.vertexCount > 0
                                             ? result.vertexCount
                                             : static_cast<int>(verts.size()))
                                .arg(usedWidth)
                                .arg(result.totalLength, 0, 'f', 2)
                                .arg(yLabel)
                                .arg(result.spatialUnit.isEmpty() ? u"pixels"_s
                                                                  : result.spatialUnit)
                                .arg(result.pixelScaleArcsecPerPixel, 0, 'g', 8);

                // Create PvDiagramWidget lazily on first successful extraction.
                if (!this->pvDiagramWidget) {
                    qDebug().noquote() << QStringLiteral("[pv] creating PvDiagramWidget");
                    this->pvDiagramWidget = new PvDiagramWidget(this);
                    QObject::connect(
                            this->pvDiagramWidget, &PvDiagramWidget::destroyed, this,
                            [this]() {
                                this->pvDiagramWidget = nullptr;
                                if (this->actionExtractPvDiagram
                                    && this->actionExtractPvDiagram->isChecked()) {
                                    this->actionExtractPvDiagram->setChecked(false);
                                }
                            });
                }

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
                                                 u"PV Diagram (Polyline)"_s, details);
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

    // Setup menu Moment – wired through the thin setMomentOrder() wrapper so the
    // cross-system isBusy() guard still applies before delegating to the controller.
    QObject::connect(ui->actionMoment0, &QAction::triggered, this,
                     [this]() { this->setMomentOrder(0); });
    QObject::connect(ui->actionMoment1, &QAction::triggered, this,
                     [this]() { this->setMomentOrder(1); });
    QObject::connect(ui->actionMoment2, &QAction::triggered, this,
                     [this]() { this->setMomentOrder(2); });
    QObject::connect(ui->actionMoment6, &QAction::triggered, this,
                     [this]() { this->setMomentOrder(6); });
    QObject::connect(ui->actionMoment8, &QAction::triggered, this,
                     [this]() { this->setMomentOrder(8); });
    QObject::connect(ui->actionMoment10, &QAction::triggered, this,
                     [this]() { this->setMomentOrder(10); });
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
    ui->actionExtractSpectrum->setCheckable(true);
    QObject::connect(ui->actionExtractSpectrum, &QAction::toggled, this,
                     &vtkWindowCube::setProbeModeActive);
    this->actionExtractPvDiagram = ui->menuTools->addAction(u"Extract PV Diagram"_s);
    this->actionExtractPvDiagram->setCheckable(true);
    QObject::connect(this->actionExtractPvDiagram, &QAction::toggled, this,
                     &vtkWindowCube::setPvModeActive);
    this->actionEstimateNoise = ui->menuTools->addAction(u"Estimate Noise..."_s);
    QObject::connect(this->actionEstimateNoise, &QAction::triggered,
                     this->noiseController, &NoiseController::startEstimation);

    // Wire the sidebar Tools card buttons to their actions now that both exist.
    // setDefaultAction() links button text, tooltip, enabled state, and checkability.
    if (this->m_btnEstimateNoise) {
        this->m_btnEstimateNoise->setDefaultAction(this->actionEstimateNoise);
    }
    if (this->m_btnDataProbe) {
        this->m_btnDataProbe->setDefaultAction(ui->actionExtractSpectrum);
        this->m_btnDataProbe->setToolTip(u"Probe data values at a point in the 2D view."_s);
    }
    if (this->m_btnPvDiagram) {
        this->m_btnPvDiagram->setDefaultAction(this->actionExtractPvDiagram);
        this->m_btnPvDiagram->setToolTip(u"Draw a path in the 2D view to extract a position-velocity diagram."_s);
    }

    this->updateBackendDerivedActionsAvailability();
    this->actionBoxRegion = ui->menuTools->addAction(u"Box Region Analysis"_s);
    this->actionCircleRegion = ui->menuTools->addAction(u"Circle Region Analysis"_s);
    this->actionPolygonRegion = ui->menuTools->addAction(u"Polygon Region Analysis"_s);
    this->actionAnnulusRegion = ui->menuTools->addAction(u"Annulus Region Analysis"_s);
    this->actionBoxRegion->setCheckable(true);
    this->actionCircleRegion->setCheckable(true);
    this->actionPolygonRegion->setCheckable(true);
    this->actionAnnulusRegion->setCheckable(true);
    auto *regionGroup = new QActionGroup(this);
    regionGroup->setExclusive(false);
    regionGroup->addAction(this->actionBoxRegion);
    regionGroup->addAction(this->actionCircleRegion);
    regionGroup->addAction(this->actionPolygonRegion);
    regionGroup->addAction(this->actionAnnulusRegion);
    QObject::connect(this->actionBoxRegion, &QAction::toggled, this,
                     [this](bool checked) { this->setRegionMode(RegionMode::Box, checked); });
    QObject::connect(this->actionCircleRegion, &QAction::toggled, this,
                     [this](bool checked) { this->setRegionMode(RegionMode::Circle, checked); });
    QObject::connect(this->actionPolygonRegion, &QAction::toggled, this,
                     [this](bool checked) { this->setRegionMode(RegionMode::Polygon, checked); });
    QObject::connect(this->actionAnnulusRegion, &QAction::toggled, this,
                     [this](bool checked) { this->setRegionMode(RegionMode::Annulus, checked); });
    ui->menuTools->addSeparator();
    this->actionLoadCatalogueOverlay = ui->menuTools->addAction(u"Load Catalogue Overlay"_s);
    this->actionShowCatalogueOverlay = ui->menuTools->addAction(u"Show Catalogue Overlay"_s);
    this->actionShowCatalogueLabels = ui->menuTools->addAction(u"Show Catalogue Labels"_s);
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

    // Setup Contours UI
    ui->lineLevel->setText(QString::number(this->level));
    ui->lineLevel->setValidator(new QIntValidator(ui->lineLevel));
    ui->lineLowerBound->setText(QString::number(this->lowerBound));
    ui->lineLowerBound->setValidator(new QDoubleValidator(ui->lineLowerBound));
    ui->lineUpperBound->setText(QString::number(this->upperBound));
    ui->lineUpperBound->setValidator(new QDoubleValidator(ui->lineUpperBound));
    QObject::connect(ui->checkContours, &QCheckBox::checkStateChanged, this,
                     &vtkWindowCube::updateContoursVisibility);
    QObject::connect(ui->lineLevel, &QLineEdit::editingFinished, this,
                     &vtkWindowCube::updateContours);
    QObject::connect(ui->lineLowerBound, &QLineEdit::editingFinished, this,
                     &vtkWindowCube::updateContours);
    QObject::connect(ui->lineUpperBound, &QLineEdit::editingFinished, this,
                     &vtkWindowCube::updateContours);

    // Backend-authoritative: statistics fields are blank until the backend preview
    // populates them; actions disabled until first content arrives.
    ui->lineCubeMin->clear();
    ui->lineCubeMax->clear();
    ui->lineCubeMean->clear();
    ui->lineCubeRms->clear();
    this->setCubeOpenActionsEnabled(false);
    ui->actionIsosurface->setChecked(false);
    ui->actionVolume->setChecked(true);
    this->setCubeRenderModeLocally(false);
    this->setRemoteCubeDisplayState(RemoteCubeDisplayState::Preview);
    this->showPersistentStatusMessage(u"Loading preview..."_s);
    this->remotePreviewWatcher.setProperty("requestId", ++this->currentRemotePreviewRequestId);
    this->remotePreviewWatcher.setFuture(
            QtConcurrent::run(&fetchRemotePreview, this->remoteBackendUrl, this->remoteDatasetId,
                              4, this->remoteSessionId, this->remoteBackendToken));
}

vtkWindowCube::~vtkWindowCube()
{
    delete ui;
}

// ── Sidebar ───────────────────────────────────────────────────────────────────

void vtkWindowCube::setupSidebar()
{
    // The UI file creates a hand-rolled sidebarContainer in mainHLayout.
    // Remove it from the layout and replace it with SidebarPanel, reusing the
    // four page content widgets (they are reparented into SidebarPanel's scroll areas).
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->sidebarContainer);
    ui->sidebarContainer->hide();

    // Build the redesigned "3D View Settings" page (creates m_comboRenderMode,
    // m_comboLut, m_colorMapCard, m_surfaceColorCard; reparents sliderThreshold /
    // lineThreshold / btnCubeColor into the new widget).
    QWidget *page3dSettings = this->setupThresholdPage();

    // Build the "2D View Settings" page (creates m_combo2dMode, m_comboSliceLut,
    // m_sliceControlsCard, m_momentInfoCard; reparents sliderSlice / spinSlice /
    // lineSpectral into the new widget).
    QWidget *page2dSettings = this->setup2dViewPage();

    // Build the "Info / Statistics" page (reparents frameImgStats / frameCubeStats
    // and adds new dynamic labels for dataset, WCS and system state).
    QWidget *pageInfoStats = this->setupInfoStatsPage();

    // Build the "Tools" page (creates tool buttons; wired to QActions after this
    // function returns, when the menu Tools are constructed in the constructor).
    QWidget *pageTools = this->setupToolsPage();

    this->m_sidebar = new SidebarPanel(220, this);
    this->m_sidebar->addPage(u":/icons/rail_3d.svg"_s,    u"3D View Settings"_s, page3dSettings);
    this->m_sidebar->addPage(u":/icons/rail_slice.svg"_s, u"2D View Settings"_s, page2dSettings);
    this->m_sidebar->addPage(u":/icons/rail_filter.svg"_s, u"Tools"_s,           pageTools);
    this->m_sidebar->addPage(u":/icons/rail_stats.svg"_s, u"Info / Stats"_s,     pageInfoStats);
    this->m_sidebar->finalize(0);
    mainLayout->insertWidget(0, this->m_sidebar);

    // ui->frameContours is reparented into setup2dViewPage().
    // ui->labelImg, ui->frameImgStats, and ui->frameCubeStats are reparented and
    // styled inside setupInfoStatsPage(); no additional styling needed here.

    // ── Bidirectional sync: rendering mode combo ↔ menu actions ──────────
    QObject::connect(this->m_comboRenderMode, &QComboBox::currentIndexChanged,
                     this, [this](int idx) {
        const bool wantIsosurface = (idx == 0);
        if (wantIsosurface == ui->actionIsosurface->isChecked()) {
            return; // already in sync
        }
        const QSignalBlocker b1(ui->actionIsosurface);
        const QSignalBlocker b2(ui->actionVolume);
        ui->actionIsosurface->setChecked(wantIsosurface);
        ui->actionVolume->setChecked(!wantIsosurface);
        this->updateVolumeModeVisibility(!wantIsosurface);
        this->changeCubeRender();
    });
    QObject::connect(ui->actionIsosurface, &QAction::toggled, this, [this](bool checked) {
        if (checked && this->m_comboRenderMode
                && this->m_comboRenderMode->currentIndex() != 0) {
            const QSignalBlocker b(this->m_comboRenderMode);
            this->m_comboRenderMode->setCurrentIndex(0);
        }
        if (checked) {
            this->updateVolumeModeVisibility(false);
        }
    });
    QObject::connect(ui->actionVolume, &QAction::toggled, this, [this](bool checked) {
        if (checked && this->m_comboRenderMode
                && this->m_comboRenderMode->currentIndex() != 1) {
            const QSignalBlocker b(this->m_comboRenderMode);
            this->m_comboRenderMode->setCurrentIndex(1);
        }
        if (checked) {
            this->updateVolumeModeVisibility(true);
        }
    });

    // ── Volume mode combo → apply blend mode ──────────────────────────────
    QObject::connect(this->m_comboVolumeMode, &QComboBox::currentIndexChanged,
                     this, [this](int idx) {
        this->applyVolumeBlendMode(idx);
        // Sync menu actions.
        const QSignalBlocker b1(this->m_actionComposite);
        const QSignalBlocker b2(this->m_actionMIP);
        const QSignalBlocker b3(this->m_actionMinIP);
        if (this->m_actionComposite) this->m_actionComposite->setChecked(idx == 0);
        if (this->m_actionMIP)       this->m_actionMIP->setChecked(idx == 1);
        if (this->m_actionMinIP)     this->m_actionMinIP->setChecked(idx == 2);
    });

    // ── LUT combo → apply color map ───────────────────────────────────────
    QObject::connect(this->m_comboLut, &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applyColorMapByName(name);
    });

    // ── Bidirectional sync: 2D mode combo ↔ actionSlice / actionMomentMap ─
    QObject::connect(this->m_combo2dMode, &QComboBox::currentIndexChanged,
                     this, [this](int idx) {
        const bool wantSlice = (idx == 0);
        if (wantSlice == ui->actionSlice->isChecked()) {
            return; // already in sync
        }
        if (wantSlice) {
            // Switching to Slice — straightforward
            const QSignalBlocker b1(ui->actionSlice);
            const QSignalBlocker b2(ui->actionMomentMap);
            ui->actionSlice->setChecked(true);
            ui->actionMomentMap->setChecked(false);
            this->update2dModeVisibility(true);
            this->changeImageRenderer();
        } else {
            // Switching to Moment — route through the availability guard
            // (may revert the combo back to Slice if no moment is available
            //  and the user cancels the dialog)
            this->requestMomentMode();
        }
    });
    QObject::connect(ui->actionSlice, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            this->sync2dModeSidebar(true);
            this->update2dModeVisibility(true);
        }
    });
    QObject::connect(ui->actionMomentMap, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            this->sync2dModeSidebar(false);
            this->update2dModeVisibility(false);
        }
    });

    // ── Slice LUT combo → apply slice color map ───────────────────────────
    QObject::connect(this->m_comboSliceLut, &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applySliceLutByName(name);
    });

    // ── Moment LUT combo → apply moment color map ─────────────────────────
    QObject::connect(this->m_comboMomentLut, &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applyMomentLutByName(name);
    });

    // View menu toggle action — wired bidirectionally by SidebarPanel.
    this->actionShowControlsSidebar = this->m_sidebar->createToggleAction(
            u"Controls Sidebar"_s, this);
}

QWidget *vtkWindowCube::setupThresholdPage()
{
    // Style constants — same palette as the rest of the sidebar.
    const QString hdrStyle =
            u"QLabel {"
            "  color: #8AB8CC;"
            "  font-size: 10px;"
            "  font-weight: 600;"
            "  padding-bottom: 4px;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
            "}"_s;
    const QString cardStyle =
            u"QFrame {"
            "  background-color: rgba(255, 255, 255, 0.03);"
            "  border: 1px solid rgba(4, 138, 191, 0.14);"
            "  border-radius: 6px;"
            "}"_s;

    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(12, 14, 12, 12);
    root->setSpacing(12);

    // ── Section 1: Rendering Mode ─────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"RENDERING MODE"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(0);

        this->m_comboRenderMode = new QComboBox();
        this->m_comboRenderMode->addItem(u"Isosurface"_s);
        this->m_comboRenderMode->addItem(u"Volume Rendering"_s);
        this->m_comboRenderMode->setItemData(
                0, u"Render an isosurface extracted at the rendering threshold."_s,
                Qt::ToolTipRole);
        this->m_comboRenderMode->setItemData(
                1, u"Render the cube volume directly (supports Composite, MIP, MinIP)."_s,
                Qt::ToolTipRole);
        this->m_comboRenderMode->setToolTip(u"Select how the 3D cube is rendered."_s);
        // Reflect current state (Volume is the default on startup).
        this->m_comboRenderMode->setCurrentIndex(ui->actionVolume->isChecked() ? 1 : 0);

        cl->addWidget(this->m_comboRenderMode);
        root->addWidget(card);
    }

    // ── Section 1b: Volume Mode (hidden when Isosurface is active) ────────
    {
        auto *modeCard = new QFrame();
        modeCard->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(modeCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *lbl = new QLabel(u"VOLUME RENDERING"_s);
        lbl->setStyleSheet(hdrStyle);
        cl->addWidget(lbl);

        this->m_comboVolumeMode = new QComboBox();
        this->m_comboVolumeMode->addItem(u"Composite"_s);
        this->m_comboVolumeMode->addItem(u"MIP (Max Intensity)"_s);
        this->m_comboVolumeMode->addItem(u"MinIP (Min Intensity)"_s);
        this->m_comboVolumeMode->setToolTip(
                u"Choose the volume blending method.\n"
                "Composite: full ray-casting with opacity transfer function.\n"
                "MIP: maximum intensity projection along each ray.\n"
                "MinIP: minimum intensity projection along each ray."_s);
        this->m_comboVolumeMode->setCurrentIndex(0);
        cl->addWidget(this->m_comboVolumeMode);

        // Visible only when Volume rendering is active.
        modeCard->setVisible(ui->actionVolume->isChecked());
        this->m_volumeModeCard = modeCard;
        root->addWidget(modeCard);
    }

    // ── Section 2: Threshold ──────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"RENDERING THRESHOLD"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        // Reparent the existing slider into the new card.
        ui->sliderThreshold->setParent(card);
        ui->sliderThreshold->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        ui->sliderThreshold->setToolTip(u"Set the rendering threshold for isosurface extraction and volume opacity."_s);
        cl->addWidget(ui->sliderThreshold);

        // Value row: line edit only (no color button here).
        auto *row = new QHBoxLayout();
        row->setSpacing(6);
        ui->lineThreshold->setParent(card);
        ui->lineThreshold->setToolTip(u"Current rendering threshold value."_s);
        row->addWidget(ui->lineThreshold, 1);
        cl->addLayout(row);

        root->addWidget(card);
    }

    // ── Section 3: Surface Color (Isosurface mode only) ───────────────────
    {
        auto *surfCard = new QFrame();
        surfCard->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(surfCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"SURFACE COLOR"_s);
        hdr->setStyleSheet(hdrStyle);
        cl->addWidget(hdr);

        auto *row = new QHBoxLayout();
        row->setSpacing(6);
        ui->btnCubeColor->setParent(surfCard);
        ui->btnCubeColor->setFixedSize(28, 28);
        ui->btnCubeColor->setToolTip(u"Select the surface color for isosurface rendering."_s);
        row->addWidget(ui->btnCubeColor);
        row->addStretch();
        cl->addLayout(row);

        // Visible only when Isosurface rendering is active.
        surfCard->setVisible(!ui->actionVolume->isChecked());
        this->m_surfaceColorCard = surfCard;
        root->addWidget(surfCard);
    }

    // ── Section 4: Color Map (Volume mode only) ───────────────────────────
    {
        auto *cmCard = new QFrame();
        cmCard->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(cmCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(hdrStyle);
        cl->addWidget(hdr);

        // Combo with per-item gradient icons.
        this->m_comboLut = new QComboBox();
        this->m_comboLut->setIconSize(QSize(48, 12));
        const auto names = ColorMaps::GetColorMapNames();
        for (const auto &stdName : names) {
            const QString qName = QString::fromStdString(stdName);
            this->m_comboLut->addItem(QIcon(buildLutPreview(qName, 48, 12)), qName);
        }
        const QString defaultName = QString::fromStdString(ColorMaps::DefaultColorMap);
        this->m_comboLut->setCurrentText(defaultName);
        this->m_comboLut->setToolTip(u"Select the color map for volume rendering."_s);
        this->m_activeColorMapName = defaultName;
        cl->addWidget(this->m_comboLut);

        // Visible only when Volume rendering is active.
        cmCard->setVisible(ui->actionVolume->isChecked());
        this->m_colorMapCard = cmCard;
        root->addWidget(cmCard);
    }

    root->addStretch();
    return page;
}

QWidget *vtkWindowCube::setupToolsPage()
{
    const QString hdrStyle =
            u"QLabel {"
            "  color: #8AB8CC;"
            "  font-size: 10px;"
            "  font-weight: 600;"
            "  padding-bottom: 4px;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
            "}"_s;
    const QString cardStyle =
            u"QFrame {"
            "  background-color: rgba(255, 255, 255, 0.03);"
            "  border: 1px solid rgba(4, 138, 191, 0.14);"
            "  border-radius: 6px;"
            "}"_s;

    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(12, 14, 12, 12);
    root->setSpacing(12);

    // Helper: create a placeholder QToolButton (text + enabled state are
    // overwritten by setDefaultAction() once the QActions exist).
    auto makeToolBtn = [](const QString &label) -> QToolButton * {
        auto *btn = new QToolButton();
        btn->setText(label);
        btn->setEnabled(false);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return btn;
    };

    // ── Section 1: Analysis ───────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"ANALYSIS"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(4);

        this->m_btnEstimateNoise = makeToolBtn(u"Estimate Noise\u2026"_s);
        cl->addWidget(this->m_btnEstimateNoise);

        root->addWidget(card);
    }

    // ── Section 2: Exploration ─────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"EXPLORATION"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(4);

        this->m_btnDataProbe = makeToolBtn(u"Data Probe"_s);
        this->m_btnPvDiagram = makeToolBtn(u"PV Diagram"_s);

        cl->addWidget(this->m_btnDataProbe);
        cl->addWidget(this->m_btnPvDiagram);

        root->addWidget(card);
    }

    root->addStretch();
    return page;
}

void vtkWindowCube::applyColorMapByName(const QString &name)
{
    if (name.isEmpty()) {
        return;
    }
    this->m_activeColorMapName = name;
    const std::string stdName = name.toStdString();

    // Volume color transfer function only — slice and moment LUTs are NOT
    // touched here; they are managed independently by their own rendering paths.
    {
        auto *cubeImage =
                vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
        double cubeRange[2] = { 0., 0. };
        if (cubeImage) {
            cubeImage->GetScalarRange(cubeRange);
        }
        vtkNew<vtkLookupTable> lut;
        lut->SetTableRange(cubeRange[0], cubeRange[1]);
        ColorMaps::SetColorMap(lut, stdName);
        ColorMaps::SetColorTransferFunction(lut, this->volumeColorTransferFunction);
        this->volumeColorTransferFunction->SetObjectName(stdName);
    }

    // Re-render the 3D cube view.
    if (ui->vtkCube->renderWindow()) {
        ui->vtkCube->renderWindow()->Render();
    }
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
}

void vtkWindowCube::syncVolumeModeSidebar(int idx)
{
    if (!this->m_comboVolumeMode) {
        return;
    }
    const QSignalBlocker b(this->m_comboVolumeMode);
    this->m_comboVolumeMode->setCurrentIndex(idx);
}

void vtkWindowCube::updateVolumeModeVisibility(bool volumeActive)
{
    if (this->m_volumeModeCard) {
        this->m_volumeModeCard->setVisible(volumeActive);
    }
    if (this->m_colorMapCard) {
        this->m_colorMapCard->setVisible(volumeActive);
    }
    if (this->m_surfaceColorCard) {
        this->m_surfaceColorCard->setVisible(!volumeActive);
    }
    if (this->m_menuVolumeRendering) {
        this->m_menuVolumeRendering->setEnabled(volumeActive);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2D View Settings
// ─────────────────────────────────────────────────────────────────────────────

QWidget *vtkWindowCube::setup2dViewPage()
{
    const QString hdrStyle =
            u"QLabel {"
            "  color: #8AB8CC;"
            "  font-size: 10px;"
            "  font-weight: 600;"
            "  padding-bottom: 4px;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
            "}"_s;
    const QString cardStyle =
            u"QFrame {"
            "  background-color: rgba(255, 255, 255, 0.03);"
            "  border: 1px solid rgba(4, 138, 191, 0.14);"
            "  border-radius: 6px;"
            "}"_s;

    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(12, 14, 12, 12);
    root->setSpacing(12);

    const bool initiallySlice = ui->actionSlice->isChecked();

    // ── Section 1: 2D Mode ────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"2D MODE"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(0);

        this->m_combo2dMode = new QComboBox();
        this->m_combo2dMode->addItem(u"Slice"_s);
        this->m_combo2dMode->addItem(u"Moment Map"_s);
        this->m_combo2dMode->setItemData(0, u"Display a 2D slice through the cube."_s,
                                          Qt::ToolTipRole);
        this->m_combo2dMode->setItemData(1, u"Display the computed moment map of the cube."_s,
                                          Qt::ToolTipRole);
        this->m_combo2dMode->setToolTip(u"Select the 2D display mode."_s);
        this->m_combo2dMode->setCurrentIndex(initiallySlice ? 0 : 1);
        cl->addWidget(this->m_combo2dMode);

        root->addWidget(card);
    }

    // ── Section 2: Slice Controls (Slice mode only) ────────────────────────
    {
        auto *sliceCard = new QFrame();
        sliceCard->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(sliceCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"CUTTING PLANE"_s);
        hdr->setStyleSheet(hdrStyle);
        cl->addWidget(hdr);

        // Reparent the existing slice controls into this card.
        ui->sliderSlice->setParent(sliceCard);
        ui->sliderSlice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        cl->addWidget(ui->sliderSlice);

        auto *row = new QHBoxLayout();
        row->setSpacing(6);
        ui->spinSlice->setParent(sliceCard);
        row->addWidget(ui->spinSlice);
        ui->lineSpectral->setParent(sliceCard);
        row->addWidget(ui->lineSpectral, 1);
        cl->addLayout(row);

        sliceCard->setVisible(initiallySlice);
        this->m_sliceControlsCard = sliceCard;
        root->addWidget(sliceCard);
    }

    // ── Section 3: Slice Color Map (Slice mode only) ─────────────────────
    {
        auto *lutCard = new QFrame();
        lutCard->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(lutCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(hdrStyle);
        cl->addWidget(hdr);

        this->m_comboSliceLut = new QComboBox();
        this->m_comboSliceLut->setIconSize(QSize(48, 12));
        const auto names = ColorMaps::GetColorMapNames();
        for (const auto &stdName : names) {
            const QString qName = QString::fromStdString(stdName);
            this->m_comboSliceLut->addItem(QIcon(buildLutPreview(qName, 48, 12)), qName);
        }
        const QString defaultName = QString::fromStdString(ColorMaps::DefaultColorMap);
        this->m_comboSliceLut->setCurrentText(defaultName);
        this->m_comboSliceLut->setToolTip(u"Select the color map for slice views."_s);
        this->m_activeSliceLutName = defaultName;
        cl->addWidget(this->m_comboSliceLut);

        lutCard->setVisible(initiallySlice);
        this->m_sliceLutCard = lutCard;
        root->addWidget(lutCard);
    }

    // ── Section 4: Contours (Slice mode only) ────────────────────────────
    {
        // Reparent the existing contours frame from the .ui file.
        // All signal connections wired in the constructor (checkContours,
        // lineLevel, lineLowerBound, lineUpperBound) remain valid after reparenting.
        auto *hdr = new QLabel(u"CONTOURS"_s);
        hdr->setStyleSheet(hdrStyle);

        ui->frameContours->setStyleSheet(cardStyle);
        ui->frameContours->setParent(page);

        // Wrap in a container so visibility toggling keeps the header + frame together.
        auto *contoursCard = new QWidget();
        auto *ccl = new QVBoxLayout(contoursCard);
        ccl->setContentsMargins(0, 0, 0, 0);
        ccl->setSpacing(4);
        ccl->addWidget(hdr);
        ccl->addWidget(ui->frameContours);

        contoursCard->setVisible(initiallySlice);
        this->m_sliceContoursCard = contoursCard;
        root->addWidget(contoursCard);
    }

    // ── Section 5: Moment Map Info (Moment mode only) ────────────────────
    {
        auto *momentCard = new QFrame();
        momentCard->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(momentCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *hdr = new QLabel(u"MOMENT MAP"_s);
        hdr->setStyleSheet(hdrStyle);
        cl->addWidget(hdr);

        // Dynamic label — updated by updateMomentInfoCard() when a moment is computed.
        auto *info = new QLabel(u"No moment map available.\nOpen Compute \u2192 Moment Map\nto generate one."_s);
        info->setWordWrap(true);
        info->setStyleSheet(u"QLabel { color: #7AAABB; font-size: 9px; }"_s);
        cl->addWidget(info);
        this->m_momentInfoLabel = info;

        momentCard->setVisible(!initiallySlice);
        this->m_momentInfoCard = momentCard;
        root->addWidget(momentCard);
    }

    // ── Section 6: Moment Color Map (Moment mode only) ────────────────────
    {
        auto *lutCard = new QFrame();
        lutCard->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(lutCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(hdrStyle);
        cl->addWidget(hdr);

        this->m_comboMomentLut = new QComboBox();
        this->m_comboMomentLut->setIconSize(QSize(48, 12));
        const auto names2 = ColorMaps::GetColorMapNames();
        for (const auto &stdName : names2) {
            const QString qName = QString::fromStdString(stdName);
            this->m_comboMomentLut->addItem(QIcon(buildLutPreview(qName, 48, 12)), qName);
        }
        const QString defaultName2 = QString::fromStdString(ColorMaps::DefaultColorMap);
        this->m_comboMomentLut->setCurrentText(defaultName2);
        this->m_comboMomentLut->setToolTip(u"Select the color map for moment map views."_s);
        this->m_activeMomentLutName = defaultName2;
        cl->addWidget(this->m_comboMomentLut);

        lutCard->setVisible(!initiallySlice);
        this->m_momentLutCard = lutCard;
        root->addWidget(lutCard);
    }

    root->addStretch();
    return page;
}

QWidget *vtkWindowCube::setupInfoStatsPage()
{
    const QString hdrStyle =
            u"QLabel { font-size: 8px; font-weight: 700; letter-spacing: 1px;"
            "  color: rgba(4,138,191,0.75); margin-top: 6px; margin-bottom: 2px; }"_s;

    const QString cardStyle =
            u"QFrame {"
            "  background: rgba(255,255,255,0.04);"
            "  border: 1px solid rgba(4,138,191,0.18);"
            "  border-radius: 4px;"
            "}"_s;

    // Read-only line-edit tint applied to the existing stats frames.
    const QString roStyle = u"QLineEdit[readOnly='true'] { color: #7AAABB; }"_s;

    // Helper: create a small read-only value label.
    auto makeVal = [](const QString &text = {}) -> QLabel * {
        auto *l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet(u"QLabel { font-size: 9px; color: #AABBCC; }"_s);
        return l;
    };

    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(4);

    // ── 1. DATASET SUMMARY ────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"DATASET SUMMARY"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(cardStyle);
        auto *fl = new QFormLayout(card);
        fl->setContentsMargins(8, 6, 8, 6);
        fl->setSpacing(3);
        fl->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        this->m_infoDsName = makeVal(QFileInfo(this->filepath).fileName());
        this->m_infoDsDimensions = makeVal(
                u"%1 × %2 × %3"_s
                        .arg(this->remoteDatasetWidth)
                        .arg(this->remoteDatasetHeight)
                        .arg(this->remoteDatasetDepth));
        this->m_infoDsLoadState = makeVal(u"Preview"_s);

        fl->addRow(u"File:"_s, this->m_infoDsName);
        fl->addRow(u"Dimensions:"_s, this->m_infoDsDimensions);
        fl->addRow(u"Load state:"_s, this->m_infoDsLoadState);

        root->addWidget(card);
    }

    // ── 2. DATA STATISTICS ────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"DATA STATISTICS"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        // Reparent existing stats frames from the .ui file into this section.
        // Pointer values remain valid; all existing data-population code still works.
        auto *subHdr2d = new QLabel(u"2D IMAGE"_s);
        subHdr2d->setStyleSheet(
                u"QLabel { font-size: 8px; font-weight: 600; letter-spacing: 1px; color: rgba(4,138,191,0.55); }"_s);
        root->addWidget(subHdr2d);

        ui->labelImg->setStyleSheet(
                u"QLabel { font-size: 8px; font-weight: 600; letter-spacing: 1px; color: rgba(4,138,191,0.55); }"_s);
        ui->frameImgStats->setStyleSheet(cardStyle + roStyle);
        ui->frameImgStats->setParent(page);
        root->addWidget(ui->labelImg);
        root->addWidget(ui->frameImgStats);

        auto *subHdr3d = new QLabel(u"3D CUBE"_s);
        subHdr3d->setStyleSheet(
                u"QLabel { font-size: 8px; font-weight: 600; letter-spacing: 1px; color: rgba(4,138,191,0.55); }"_s);
        root->addWidget(subHdr3d);

        ui->frameCubeStats->setStyleSheet(cardStyle + roStyle);
        ui->frameCubeStats->setParent(page);
        root->addWidget(ui->frameCubeStats);
    }

    // ── 3. COORDINATE / WCS ───────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"COORDINATE / WCS"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(cardStyle);
        auto *fl = new QFormLayout(card);
        fl->setContentsMargins(8, 6, 8, 6);
        fl->setSpacing(3);
        fl->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        this->m_infoWcsBadge = makeVal();
        this->m_infoWcsFrame = makeVal(this->currentWcsFrameLabel());
        this->m_infoSpectral = makeVal(this->spectralAxisTitle());

        this->m_infoWcsBadge->setToolTip(u"World Coordinate System status for this dataset."_s);
        this->m_infoWcsFrame->setToolTip(u"The celestial reference frame currently in use."_s);
        this->m_infoSpectral->setToolTip(u"The spectral axis type and units for the third dimension."_s);

        fl->addRow(u"WCS:"_s, this->m_infoWcsBadge);
        fl->addRow(u"Frame:"_s, this->m_infoWcsFrame);
        fl->addRow(u"Spectral axis:"_s, this->m_infoSpectral);

        root->addWidget(card);
    }

    // ── 4. SYSTEM / STATUS ────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"SYSTEM / STATUS"_s);
        hdr->setStyleSheet(hdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(cardStyle);
        auto *fl = new QFormLayout(card);
        fl->setContentsMargins(8, 6, 8, 6);
        fl->setSpacing(3);
        fl->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        this->m_infoRenderMode = makeVal(u"Isosurface"_s);
        this->m_info2dMode = makeVal(u"Slice"_s);
        this->m_infoMomentState = makeVal(u"—"_s);

        this->m_infoRenderMode->setToolTip(u"Current 3D rendering mode (Isosurface or Volume Rendering)."_s);
        this->m_info2dMode->setToolTip(u"Current 2D view mode (Slice or Moment Map)."_s);
        this->m_infoMomentState->setToolTip(u"State of the last moment map computation."_s);

        fl->addRow(u"Rendering mode:"_s, this->m_infoRenderMode);
        fl->addRow(u"2D view:"_s, this->m_info2dMode);
        fl->addRow(u"Moment map:"_s, this->m_infoMomentState);

        root->addWidget(card);
    }

    root->addStretch();

    // Populate dynamic fields that depend on state already known at setup time.
    this->updateInfoStatsPage();

    return page;
}

void vtkWindowCube::updateInfoStatsPage()
{
    // Guard: labels are created inside setupInfoStatsPage(); this is a no-op
    // until the sidebar is fully set up.
    if (!this->m_infoDsLoadState) {
        return;
    }

    // ── Dataset Summary ───────────────────────────────────────────────────────
    // m_infoDsName and m_infoDsDimensions are set once at construction (static).

    switch (this->remoteCubeDisplayState) {
    case RemoteCubeDisplayState::Preview:
        this->m_infoDsLoadState->setText(u"Preview proxy"_s);
        this->m_infoDsLoadState->setStyleSheet(u"QLabel { font-size: 9px; color: #E8A840; }"_s);
        break;
    case RemoteCubeDisplayState::LoadingFullResolution:
        this->m_infoDsLoadState->setText(u"Loading full resolution\u2026"_s);
        this->m_infoDsLoadState->setStyleSheet(
                u"QLabel { font-size: 9px; color: #E8A840; font-style: italic; }"_s);
        break;
    case RemoteCubeDisplayState::FullResolution:
        this->m_infoDsLoadState->setText(u"Full resolution"_s);
        this->m_infoDsLoadState->setStyleSheet(u"QLabel { font-size: 9px; color: #7AAABB; }"_s);
        break;
    }

    // ── Coordinate / WCS ─────────────────────────────────────────────────────
    if (this->m_infoWcsFrame) {
        this->m_infoWcsFrame->setText(this->currentWcsFrameLabel());
    }
    if (this->m_infoSpectral) {
        this->m_infoSpectral->setText(this->spectralAxisTitle());
    }
    if (this->m_infoWcsBadge) {
        if (this->remoteWcsStatus.compare(u"ok"_s, Qt::CaseInsensitive) == 0) {
            this->m_infoWcsBadge->setText(u"OK"_s);
            this->m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 9px; color: #5DA87A; }"_s);
        } else if (this->remoteWcsStatus.compare(u"degraded"_s, Qt::CaseInsensitive) == 0) {
            this->m_infoWcsBadge->setText(u"Degraded"_s);
            this->m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 9px; color: #E8A840; }"_s);
        } else {
            this->m_infoWcsBadge->setText(u"Repaired"_s);
            this->m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 9px; color: #9a6700; }"_s);
        }
        if (!this->remoteWcsWarningMessage.isEmpty()) {
            this->m_infoWcsBadge->setToolTip(this->remoteWcsWarningMessage);
        }
    }

    // ── System / Status ───────────────────────────────────────────────────────
    if (this->m_infoRenderMode) {
        const bool iso = ui->actionIsosurface->isChecked();
        this->m_infoRenderMode->setText(iso ? u"Isosurface"_s : u"Volume Rendering"_s);
    }
    if (this->m_info2dMode) {
        const bool slc = this->viewingSlice();
        this->m_info2dMode->setText(slc ? u"Slice"_s : u"Moment Map"_s);
    }
    if (this->m_infoMomentState) {
        switch (m_momentCardState) {
        case MomentCardState::NoMoment:
            this->m_infoMomentState->setText(u"\u2014"_s); // em-dash
            this->m_infoMomentState->setStyleSheet(u"QLabel { font-size: 9px; color: #AABBCC; }"_s);
            break;
        case MomentCardState::Computing:
            this->m_infoMomentState->setText(u"Computing\u2026"_s);
            this->m_infoMomentState->setStyleSheet(
                    u"QLabel { font-size: 9px; color: #E8A840; font-style: italic; }"_s);
            break;
        case MomentCardState::Ready:
            this->m_infoMomentState->setText(this->momentProvenanceState.summary);
            this->m_infoMomentState->setStyleSheet(u"QLabel { font-size: 9px; color: #7AAABB; }"_s);
            break;
        case MomentCardState::Error:
            this->m_infoMomentState->setText(u"Error"_s);
            this->m_infoMomentState->setStyleSheet(u"QLabel { font-size: 9px; color: #CC6060; }"_s);
            break;
        }
    }
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
        this->sync2dModeSidebar(false);
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
        this->sync2dModeSidebar(true);
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
        this->sync2dModeSidebar(true);
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
    this->sync2dModeSidebar(true);
    this->update2dModeVisibility(true);
}

void vtkWindowCube::setMomentCardState(MomentCardState state, const QString &detail)
{
    m_momentCardState = state;
    if (!detail.isEmpty()) {
        m_momentCardDetail = detail;
    }
    this->updateMomentInfoCard();
    this->updateInfoStatsPage();
}

void vtkWindowCube::updateMomentInfoCard()
{
    if (!this->m_momentInfoLabel) {
        return;
    }

    // Common stylesheet fragments.
    const QString baseStyle = u"QLabel { font-size: 9px; word-wrap: true; }"_s;
    const QString infoColor = u"color: #7AAABB;"_s;
    const QString busyColor = u"color: #E8A840;"_s; // warm amber — visually distinct
    const QString errorColor = u"color: #CC6060;"_s;

    switch (m_momentCardState) {
    case MomentCardState::NoMoment:
        this->m_momentInfoLabel->setStyleSheet(
                QStringLiteral("QLabel { font-size: 9px; %1 }").arg(infoColor));
        this->m_momentInfoLabel->setText(
                u"No moment map available.\nOpen Compute \u2192 Moment Map\nto generate one."_s);
        this->m_momentInfoLabel->setToolTip(QString());
        break;

    case MomentCardState::Computing:
        this->m_momentInfoLabel->setStyleSheet(
                QStringLiteral("QLabel { font-size: 9px; font-style: italic; %1 }").arg(busyColor));
        this->m_momentInfoLabel->setText(
                u"Computing moment map\u2026\n%1"_s.arg(m_momentCardDetail));
        this->m_momentInfoLabel->setToolTip(
                u"Moment computation is running on the backend.\nThe panel will update automatically when done."_s);
        break;

    case MomentCardState::Ready:
        this->m_momentInfoLabel->setStyleSheet(
                QStringLiteral("QLabel { font-size: 9px; %1 }").arg(infoColor));
        this->m_momentInfoLabel->setText(this->momentProvenanceState.summary);
        this->m_momentInfoLabel->setToolTip(this->momentProvenanceState.details);
        break;

    case MomentCardState::Error:
        this->m_momentInfoLabel->setStyleSheet(
                QStringLiteral("QLabel { font-size: 9px; %1 }").arg(errorColor));
        this->m_momentInfoLabel->setText(
                u"Moment computation failed.\n%1"_s.arg(
                        m_momentCardDetail.isEmpty()
                                ? u"Check the Diagnostics panel for details."_s
                                : m_momentCardDetail));
        this->m_momentInfoLabel->setToolTip(
                u"The last moment computation did not produce a result.\nSee the Diagnostics panel for the error."_s);
        break;
    }
}

void vtkWindowCube::update2dModeVisibility(bool sliceActive)
{
    if (this->m_sliceControlsCard) {
        this->m_sliceControlsCard->setVisible(sliceActive);
    }
    if (this->m_sliceLutCard) {
        this->m_sliceLutCard->setVisible(sliceActive);
    }
    if (this->m_sliceContoursCard) {
        this->m_sliceContoursCard->setVisible(sliceActive);
    }
    if (this->m_momentInfoCard) {
        this->m_momentInfoCard->setVisible(!sliceActive);
    }
    if (this->m_momentLutCard) {
        this->m_momentLutCard->setVisible(!sliceActive);
    }
}

void vtkWindowCube::sync2dModeSidebar(bool sliceActive)
{
    if (!this->m_combo2dMode) {
        return;
    }
    const QSignalBlocker b(this->m_combo2dMode);
    this->m_combo2dMode->setCurrentIndex(sliceActive ? 0 : 1);
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

    // Shared dock stylesheet — matches the dark/blue-accent theme used by the sidebar.
    const QString dockStyle =
            u"QDockWidget {"
            "  border: 1px solid rgba(4, 138, 191, 0.22);"
            "}"
            "QDockWidget::title {"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
            "      stop:0 rgba(4,138,191,0.18), stop:1 rgba(4,138,191,0.06));"
            "  color: #8AB8CC;"
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
            "}"_s;

    // ── 3D View dock ─────────────────────────────────────────────────────────
    auto *dockCube = new QDockWidget(u"3D View"_s, this);
    dockCube->setObjectName(u"dockCubeView"_s);
    dockCube->setWidget(ui->vtkCube);          // reparents vtkCube into the dock
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

void vtkWindowCube::closeEvent(QCloseEvent *event)
{
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
    this->volumeOpacity->RemoveAllPoints();
    this->volumeOpacity->AddPoint(this->currentCubeInvisibleSentinel, 0.0);
    this->volumeOpacity->AddPoint(cubeRange[0], 0.0);
    this->volumeOpacity->AddPoint(this->lowerBound, 0.05);
    this->volumeOpacity->AddPoint(cubeRange[1], 0.3);
    vtkNew<vtkVolumeProperty> volumeProperty;
    volumeProperty->SetColor(this->volumeColorTransferFunction);
    volumeProperty->SetScalarOpacity(this->volumeOpacity);
    volumeProperty->SetInterpolationTypeToLinear();
    vtkNew<vtkGPUVolumeRayCastMapper> volumeMapper;
    volumeMapper->SetInputConnection(this->cubeDisplaySource->GetOutputPort());
    volumeMapper->SetBlendModeToComposite(); // default
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

    {
        vtkNew<vtkPolyDataMapper> planeMapper;
        planeMapper->SetInputConnection(this->remoteCuttingPlaneSource->GetOutputPort());
        planeMapper->ScalarVisibilityOff();
        this->remoteCuttingPlaneActor->SetMapper(planeMapper);
        this->remoteCuttingPlaneActor->GetProperty()->SetColor(0.1, 1.0, 0.1);
        this->remoteCuttingPlaneActor->GetProperty()->SetOpacity(0.55);
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

    if (!this->probeFrozen) {
        if (this->updateProbeFromDisplayPosition(position[0], position[1])) {
            this->probeFrozen = true;
        }
    } else {
        this->probeFrozen = false;
        this->updateProbeFromDisplayPosition(position[0], position[1]);
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
    if (this->actionExtractPvDiagram && this->actionExtractPvDiagram->isChecked()) {
        this->actionExtractPvDiagram->setChecked(false);
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

void vtkWindowCube::setRegionMode(RegionMode mode, bool active)
{
    if (!active) {
        if (this->regionMode == mode) {
            this->regionMode = RegionMode::None;
            this->regionDragging = false;
            this->clearRegion();
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
        this->clearPv();
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
        if (!imageData || !this->pvModeActive) {
            actor->VisibilityOff();
            upperActor->VisibilityOff();
            lowerActor->VisibilityOff();
            return;
        }
        std::vector<std::array<int, 2>> drawVertices = this->pvPolylineVertices;
        if (this->pvDragging && this->pvCursorValid
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

    QString text;
    if (!stats.valid) {
        text = u"No valid pixels in the selected region.\nBlanked/NaN voxels are ignored."_s;
    } else {
        text = u"Shape: %1\nValid pixels: %2 / %3\nBlanked/NaN: %4\nMin: %5\nMax: %6\nMean: %7\nMedian: %8\nStddev: %9\n\nComputed on valid pixels only."_s
                       .arg(regionModeLabel(this->regionMode))
                       .arg(stats.validCount)
                       .arg(stats.totalCount)
                       .arg(stats.blankedCount)
                       .arg(stats.minValue, 0, 'g', 8)
                       .arg(stats.maxValue, 0, 'g', 8)
                       .arg(stats.mean, 0, 'g', 8)
                       .arg(stats.median, 0, 'g', 8)
                       .arg(stats.stddev, 0, 'g', 8);
    }

    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (cubeImage && stats.valid) {
        int extent[6];
        cubeImage->GetExtent(extent);
        double origin[3];
        double spacing[3];
        cubeImage->GetOrigin(origin);
        cubeImage->GetSpacing(spacing);
        const int zCount = extent[5] - extent[4] + 1;
        const auto regionBounds =
                regionBounds2D(this->regionMode, this->regionAnchorVoxel, this->regionCurrentVoxel,
                               this->regionPolygonVertices);
        QVector<double> spectral(zCount);
        QVector<double> meanValues(zCount, 0.0);
        QVector<double> sumValues(zCount, 0.0);
        QVector<int> counts(zCount, 0);
        for (int y = regionBounds[2]; y <= regionBounds[3]; ++y) {
            for (int x = regionBounds[0]; x <= regionBounds[1]; ++x) {
                const bool inside = pointInRegion(this->regionMode, this->regionAnchorVoxel,
                                                  this->regionCurrentVoxel, this->regionPolygonVertices,
                                                  this->regionAnnulusInnerRadius, x, y);
                if (!inside) {
                    continue;
                }
                const int localX = std::lround(extent[0] + (x - origin[0]) / spacing[0]);
                const int localY = std::lround(extent[2] + (y - origin[1]) / spacing[1]);
                if (localX < extent[0] || localX > extent[1] || localY < extent[2] || localY > extent[3]) {
                    continue;
                }
                for (int localZ = extent[4]; localZ <= extent[5]; ++localZ) {
                    const int idx = localZ - extent[4];
                    const double value = cubeImage->GetScalarComponentAsDouble(localX, localY, localZ, 0);
                    if (!std::isfinite(value)) {
                        continue;
                    }
                    const double datasetZ = origin[2] + (localZ - extent[4]) * spacing[2];
                    bool ok = false;
                    spectral[idx] = this->spectralAxisValue(datasetZ, &ok);
                    if (!ok) {
                        spectral[idx] = datasetZ;
                    }
                    sumValues[idx] += value;
                    counts[idx] += 1;
                }
            }
        }

        bool haveSpectrum = false;
        for (int i = 0; i < zCount; ++i) {
            if (counts[i] > 0) {
                meanValues[i] = sumValues[i] / static_cast<double>(counts[i]);
                haveSpectrum = true;
            } else {
                meanValues[i] = std::numeric_limits<double>::quiet_NaN();
                sumValues[i] = std::numeric_limits<double>::quiet_NaN();
            }
        }

        if (haveSpectrum) {
            if (!this->probePlotWidget) {
                this->probePlotWidget = new ProfileWidget(this);
                const QString yLabel = u"Value"_s;
                this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::RegionStatic,
                                                    u"Region Spectrum"_s);
                this->probePlotWidget->setupSpectrumPlot(this->spectralAxisTitle(),
                                                         yLabel.isEmpty() ? u"Value"_s : yLabel);
            } else {
                this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::RegionStatic,
                                                    u"Region Spectrum"_s);
            }
            this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::RegionStatic,
                                                u"Region Spectrum (%1)"_s.arg(regionModeLabel(this->regionMode)));
            this->probePlotWidget->updateSpectrumPlotSeries(
                    spectral, meanValues, u"Mean"_s, sumValues, u"Sum"_s,
                    u"Region Spectrum (%1)"_s.arg(regionModeLabel(this->regionMode)), false);
            text += u"\n\nMean and sum spectra updated from the selected region."_s;
        }
    }

    text += u"\n\nComputed on the currently loaded data block."_s;
    QMessageBox::information(this, u"Region Analysis"_s, text);
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
        this->actionShowCatalogueLabels->setChecked(true);
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
        this->actionShowCatalogueLabels->setChecked(true);
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

    vtkRenderer *renderer = nullptr;
    if (ui->actionMomentMap->isChecked()) {
        renderer = this->momentWin->GetRenderers()->GetFirstRenderer();
    } else {
        renderer = this->sliceWin->GetRenderers()->GetFirstRenderer();
    }
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
        this->catalogueInfoLabel->setText(u"Source: none"_s);
        this->catalogueInfoLabel->setToolTip(u"No source selected"_s);
        return;
    }

    const QString summary = this->catalogueSourceSummary(this->selectedCatalogueSourceIndex);
    this->catalogueInfoLabel->setText(summary);
    this->catalogueInfoLabel->setToolTip(summary);
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

    vtkRenderer *renderer = nullptr;
    vtkGenericOpenGLRenderWindow *window = nullptr;
    if (ui->actionMomentMap->isChecked()) {
        renderer = this->momentWin->GetRenderers()->GetFirstRenderer();
        window = this->momentWin;
    } else {
        renderer = this->sliceWin->GetRenderers()->GetFirstRenderer();
        window = this->sliceWin;
    }
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
                "Stage or open it on the backend first."_s;
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
        const QString xLabel = this->spectralAxisTitle();
        const QString yLabel = u"Value"_s;
        this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::ProbeLive, u"Profile"_s);
        this->probePlotWidget->setupSpectrumPlot(xLabel.isEmpty() ? u"Z"_s : xLabel,
                                                 yLabel.isEmpty() ? u"Value"_s : yLabel);
        QObject::connect(this->probePlotWidget, &ProfileWidget::destroyed, this,
                         [this]() {
                             this->probePlotWidget = nullptr;
                             if (ui->actionExtractSpectrum->isChecked()) {
                                 ui->actionExtractSpectrum->setChecked(false);
                             }
                         });
    } else {
        this->probePlotWidget->setUsageMode(ProfileWidget::UsageMode::ProbeLive, u"Profile"_s);
    }

    this->probePlotWidget->updateSpectrumPlot(
            spectral, values,
            u"Z Profile (%1, %2)"_s.arg(this->probeVoxel[0]).arg(this->probeVoxel[1]),
            !this->probeFrozen);
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

void vtkWindowCube::updateCube()
{
    const double threshold = ui->lineThreshold->text().toDouble();
    this->isosurfaceFilter->SetValue(0, threshold);
    this->volumeOpacity->RemoveAllPoints();
    this->volumeOpacity->AddPoint(this->currentCubeInvisibleSentinel, 0.0);
    this->volumeOpacity->AddPoint(this->currentCubeVisibleRange[0], 0.0);
    this->volumeOpacity->AddPoint(threshold, 0.05);
    this->volumeOpacity->AddPoint(this->currentCubeVisibleRange[1], 0.3);
    if (!this->cubeOpenWatcher.isRunning() && ui->actionIsosurface->isChecked()) {
        this->scheduleIsosurfaceRecompute();
    }
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
    ui->vtkCube->renderWindow()->Render();
    qDebug().noquote() << QStringLiteral("[remote-plane] render triggered");
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
        const auto sanitized = sanitizeCubeScalarsInPlace(result.cubeImageData);
        this->currentCubeVisibleRange = sanitized.visibleRange;
        this->currentCubeInvisibleSentinel = sanitized.invisibleSentinel;
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
        ui->lineImgMin->setText(QString::number(result.momentRange[0]));
        ui->lineImgMax->setText(QString::number(result.momentRange[1]));
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
        this->updateRemoteCuttingPlane(clampedSlice);
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
                this->scheduleIsosurfaceRecompute();
            }
        }
    }
    cubeRenderer->Modified();
    ui->vtkCube->renderWindow()->Modified();

    QElapsedTimer renderTimer;
    renderTimer.start();
    ui->vtkCube->renderWindow()->Render();
    ui->vtkImage->renderWindow()->Render();
    QTimer::singleShot(0, this, [this]() { ui->vtkCube->renderWindow()->Render(); });
    this->requestRemoteSlice(clampedSlice);
    qDebug().noquote()
            << QStringLiteral("[perf][cube] apply render: %1 ms").arg(renderTimer.elapsed());
    qDebug().noquote() << QStringLiteral("[perf][cube] apply total: %1 ms").arg(
            totalTimer.elapsed());
}

void vtkWindowCube::updateSlice()
{
    const int slice = ui->spinSlice->value() - 1;
    this->remoteSliceDebounceTimer.stop();
    this->requestRemoteSlice(slice);
}

void vtkWindowCube::requestRemoteSlice(int sliceIndex)
{
    sliceIndex = this->clampRemoteSliceIndex(sliceIndex);
    this->currentRequestedRemoteSliceIndex = sliceIndex;
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
        ui->lineImgMin->setText(QString::number(result.imageRange[0]));
        ui->lineImgMax->setText(QString::number(result.imageRange[1]));
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
    this->prefetchNeighborRemoteSlices(result.index);
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
        this->momentProvenanceLabel->setText(u"Moment map: none"_s);
        this->momentProvenanceLabel->setToolTip(
                u"No moment map has been computed for this dataset yet."_s);
        return;
    }

    this->momentProvenanceLabel->setText(this->momentProvenanceState.summary);
    this->momentProvenanceLabel->setToolTip(this->momentProvenanceState.details);
}

void vtkWindowCube::updateSanityPanel()
{
    if (!this->sanityLabel) {
        return;
    }

    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    const auto report = buildCubeSanityReport(this->remoteDatasetCtype, this->remoteDatasetCunit,
                                              this->remoteDatasetCrval, this->remoteDatasetCrpix,
                                              this->remoteDatasetCdelt, cubeImage);
    this->sanityLabel->setText(report.summary);
    this->sanityLabel->setToolTip(report.details);
}

void vtkWindowCube::updateWcsStatusIndicator()
{
    if (!this->wcsStatusLabel) {
        return;
    }
    if (this->remoteWcsStatus.compare(u"ok"_s, Qt::CaseInsensitive) == 0) {
        this->wcsStatusLabel->clear();
        this->wcsStatusLabel->setToolTip({});
        this->wcsStatusLabel->hide();
        this->wcsDiagnosticPublished = false;
        return;
    }

    const bool degraded = this->remoteWcsStatus.compare(u"degraded"_s, Qt::CaseInsensitive) == 0;
    const QString message =
            this->remoteWcsWarningMessage.isEmpty()
            ? (degraded ? u"Remote cube opened with degraded WCS metadata."_s
                        : u"Remote cube opened with sanitized WCS metadata."_s)
            : this->remoteWcsWarningMessage;
    this->wcsStatusLabel->setText(degraded ? u"WCS degraded"_s : u"WCS repaired"_s);
    this->wcsStatusLabel->setStyleSheet(
            degraded
                    ? u"QLabel { padding-left: 8px; font-weight: 700; color: #b54708; }"_s
                    : u"QLabel { padding-left: 8px; font-weight: 600; color: #9a6700; }"_s);
    this->wcsStatusLabel->setToolTip(message);
    this->wcsStatusLabel->show();
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
    if (!this->dataStateLabel) {
        return;
    }

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
    const QString note = u"Probe/profile: loaded block only"_s;
    const QString degenerateSummary = this->remoteDegenerateAxesSummary;
    QString text = u"%1 | %2 | Loaded: %3 | Dataset: %4 | WCS: %5 | Axis3: %6"_s.arg(origin,
                                                                                         representation,
                                                                                         loadedBounds,
                                                                                         datasetBounds,
                                                                                         this->currentWcsFrameLabel(),
                                                                                         this->spectralAxisTitle());
    text += u" | Refine: %1"_s.arg(refinement);
    if (!degenerateSummary.isEmpty()) {
        text += u" | Collapsed axes"_s;
    }
    text += u" | %1"_s.arg(note);
    this->dataStateLabel->setText(text);
    this->dataStateLabel->setToolTip(
            degenerateSummary.isEmpty()
                    ? u"Persistent data provenance: origin, current representation, loaded bounds, full dataset bounds, WCS frame, spectral-axis semantics, remote refinement strategy, and probe/profile scope."_s
                    : u"Persistent data provenance: origin, current representation, loaded bounds, full dataset bounds, WCS frame, spectral-axis semantics, remote refinement strategy, and probe/profile scope.\n%1"_s
                              .arg(degenerateSummary));
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
    this->sync2dModeSidebar(false);
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
        ui->lineImgMin->setText(QString::number(result.imageRange[0]));
        ui->lineImgMax->setText(QString::number(result.imageRange[1]));
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
    ui->actionMoment0->setEnabled(finalEnabled);
    ui->actionMoment1->setEnabled(finalEnabled);
    ui->actionMoment2->setEnabled(finalEnabled);
    ui->actionMoment6->setEnabled(finalEnabled);
    ui->actionMoment8->setEnabled(finalEnabled);
    ui->actionMoment10->setEnabled(finalEnabled);

    const QString tooltip = available ? QString {} : kMomentBackendTooltip;
    ui->actionMoment0->setToolTip(tooltip);
    ui->actionMoment1->setToolTip(tooltip);
    ui->actionMoment2->setToolTip(tooltip);
    ui->actionMoment6->setToolTip(tooltip);
    ui->actionMoment8->setToolTip(tooltip);
    ui->actionMoment10->setToolTip(tooltip);
    ui->actionMoment0->setStatusTip(tooltip);
    ui->actionMoment1->setStatusTip(tooltip);
    ui->actionMoment2->setStatusTip(tooltip);
    ui->actionMoment6->setStatusTip(tooltip);
    ui->actionMoment8->setStatusTip(tooltip);
    ui->actionMoment10->setStatusTip(tooltip);
}

bool vtkWindowCube::hasBackendVisibleDataset() const
{
    return !this->remoteDatasetId.isEmpty();
}

void vtkWindowCube::updateBackendDerivedActionsAvailability()
{
    const bool available = this->hasBackendVisibleDataset();
    if (ui->menuMoment) {
        ui->menuMoment->setEnabled(available);
        ui->menuMoment->setToolTipsVisible(true);
        ui->menuMoment->menuAction()->setToolTip(available ? QString {} : kMomentBackendTooltip);
        ui->menuMoment->menuAction()->setStatusTip(available ? QString {} : kMomentBackendTooltip);
    }
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
        if (ui->menuMoment) {
            ui->menuMoment->setEnabled(false);
        }
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

    if (ui->actionSlice->isChecked()) {
        ui->vtkImage->setRenderWindow(this->sliceWin);
        ui->labelImg->setText(u"Slice:"_s);
        auto *sliceImage = vtkImageData::SafeDownCast(
                this->remoteSliceDisplaySource->GetOutputDataObject(0));
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
        if (momentImage) {
            momentImage->GetScalarRange(imgRange);
        } else {
            imgRange[0] = 0.;
            imgRange[1] = 0.;
        }
    }

    ui->lineImgMin->setText(QString::number(imgRange[0]));
    ui->lineImgMax->setText(QString::number(imgRange[1]));
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
    this->sync2dModeSidebar(ui->actionSlice->isChecked());
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
            if (this->m_comboLut) {
                const QSignalBlocker b(this->m_comboLut);
                this->m_comboLut->setCurrentText(palette);
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
