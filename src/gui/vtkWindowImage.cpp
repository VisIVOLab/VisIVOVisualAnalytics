#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

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

namespace {

static QIcon makeColormapIcon(const std::string &name, int iconW = 80, int iconH = 14)
{
    vtkNew<vtkLookupTable> lut;
    lut->SetNumberOfTableValues(256);
    lut->SetTableRange(0.0, 1.0);
    ColorMaps::SetColorMap(lut.Get(), name);
    lut->Build();
    QImage img(iconW, iconH, QImage::Format_RGB32);
    for (int x = 0; x < iconW; ++x) {
        const int tableIdx = static_cast<int>(x * 255.0 / std::max(1, iconW - 1));
        double rgba[4];
        lut->GetTableValue(tableIdx, rgba);
        const QRgb c = qRgb(static_cast<int>(rgba[0] * 255.0),
                            static_cast<int>(rgba[1] * 255.0),
                            static_cast<int>(rgba[2] * 255.0));
        for (int y = 0; y < iconH; ++y)
            img.setPixel(x, y, c);
    }
    return QIcon(QPixmap::fromImage(img));
}

/// Returns at most the last two path segments of @p fullPath, preceded by "…/".
/// E.g. "/home/user/data/fits" → "…/data/fits".  Paths with ≤ 2 segments are
/// returned unchanged.
static QString compactDirPath(const QString &fullPath)
{
    const QStringList parts = fullPath.split(u'/', Qt::SkipEmptyParts);
    if (parts.size() <= 2) {
        return fullPath;
    }
    return QStringLiteral("\u2026/%1/%2").arg(parts[parts.size() - 2], parts.last());
}

constexpr int overlayTickCount = 5;
constexpr double polygonClosureTolerance = 3.0;
constexpr double catalogueMarkerHalfSize = 3.0;
constexpr int maxCatalogueLabelCount = 200;
constexpr int smartCatalogueLabelMaxVisible = 24;
constexpr double smartCatalogueLabelMinPixelsPerUnit = 2.5;
constexpr double catalogueHoverDisplayThresholdPx = 10.0;
constexpr qint64 largeImagePixelThreshold = 25000000;
constexpr int initialAutoscaleTargetSamples = 250000;
constexpr double initialAutoscaleLowPercent = 1.0;
constexpr double initialAutoscaleHighPercent = 99.5;

struct VisibleImageBounds2D
{
    bool valid{ false };
    double xmin{ 0. };
    double xmax{ 0. };
    double ymin{ 0. };
    double ymax{ 0. };
};

bool isImageExtentUsable(vtkImageData *imageData)
{
    if (!imageData) {
        return false;
    }
    int extent[6];
    imageData->GetExtent(extent);
    return extent[1] > extent[0] && extent[3] > extent[2];
}

qint64 imagePixelCount(vtkImageData *imageData)
{
    if (!imageData) {
        return 0;
    }
    int extent[6];
    imageData->GetExtent(extent);
    const qint64 width = std::max(0, extent[1] - extent[0] + 1);
    const qint64 height = std::max(0, extent[3] - extent[2] + 1);
    return width * height;
}

double percentileFromSorted(const std::vector<double> &values, double percentile)
{
    if (values.empty()) {
        return 0.0;
    }
    const double clamped = std::clamp(percentile, 0.0, 100.0);
    const double rank = (clamped / 100.0) * static_cast<double>(values.size() - 1);
    const auto lowerIndex = static_cast<std::size_t>(std::floor(rank));
    const auto upperIndex = static_cast<std::size_t>(std::ceil(rank));
    if (lowerIndex == upperIndex) {
        return values[lowerIndex];
    }
    const double weight = rank - static_cast<double>(lowerIndex);
    return values[lowerIndex] * (1.0 - weight) + values[upperIndex] * weight;
}

double sampledPositiveFloor(vtkImageData *imageData, qint64 targetSamples = 250000)
{
    if (!imageData) {
        return 0.0;
    }

    int extent[6];
    imageData->GetExtent(extent);
    const int width = extent[1] - extent[0] + 1;
    const int height = extent[3] - extent[2] + 1;
    if (width <= 0 || height <= 0) {
        return 0.0;
    }

    const qint64 pixels = static_cast<qint64>(width) * static_cast<qint64>(height);
    const int stride =
            std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(pixels) / targetSamples))));
    std::vector<double> positives;
    positives.reserve(static_cast<std::size_t>(std::min<qint64>(pixels / stride, 32768)));
    for (int y = extent[2]; y <= extent[3]; y += stride) {
        for (int x = extent[0]; x <= extent[1]; x += stride) {
            const double value = imageData->GetScalarComponentAsDouble(x, y, 0, 0);
            if (std::isfinite(value) && value > 0.0) {
                positives.push_back(value);
            }
        }
    }

    if (positives.empty()) {
        return 0.0;
    }
    std::sort(positives.begin(), positives.end());
    return std::max(1e-12, percentileFromSorted(positives, 1.0));
}

std::vector<std::array<double, 2>> buildEllipsePolyline(double centerX, double centerY, double radiusX,
                                                        double radiusY, double angleDeg, int samples = 96)
{
    std::vector<std::array<double, 2>> polyline;
    if (radiusX <= 0.0 || radiusY <= 0.0 || samples < 8) {
        return polyline;
    }
    polyline.reserve(static_cast<std::size_t>(samples + 1));
    const double angleRad = angleDeg * vtkMath::Pi() / 180.0;
    const double cosA = std::cos(angleRad);
    const double sinA = std::sin(angleRad);
    for (int i = 0; i <= samples; ++i) {
        const double t = (2.0 * vtkMath::Pi() * static_cast<double>(i)) / static_cast<double>(samples);
        const double ex = radiusX * std::cos(t);
        const double ey = radiusY * std::sin(t);
        polyline.push_back(
                { centerX + ex * cosA - ey * sinA, centerY + ex * sinA + ey * cosA });
    }
    return polyline;
}

bool pointInVisibleBounds(const std::array<double, 2> &point, const VisibleImageBounds2D &visible)
{
    return visible.valid && point[0] >= visible.xmin && point[0] <= visible.xmax && point[1] >= visible.ymin
            && point[1] <= visible.ymax;
}

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
    double sum{ 0. };       ///< Sum of valid pixels (for Jy = sum / Ω_beam_px)
    double madSigma{ 0. };  ///< 1.4826 × MAD — robust σ estimate
    bool valid{ false };
};

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
    const qsizetype width  = extent[1] - extent[0] + 1;
    const qsizetype height = extent[3] - extent[2] + 1;
    const qsizetype depth  = std::max<qsizetype>(1, extent[5] - extent[4] + 1);
    const qsizetype total  = width * height * depth;
    if (total <= 0) {
        return 0.0;
    }

    // For huge images (e.g. 7500×7500 = 56 M pixels) a full scan with the
    // virtual GetScalarComponentAsDouble takes seconds and freezes the UI on
    // the preview→full swap. The blank fraction is only used to flag heavily
    // blanked images (≥ 20 %), so a stride-sampled estimate is more than
    // accurate enough — we target ~250 k samples regardless of image size.
    constexpr qsizetype targetSamples = 250'000;
    const qsizetype perPlane = std::max<qsizetype>(1, width * height);
    const int stride = std::max(1, static_cast<int>(
            std::ceil(std::sqrt(static_cast<double>(perPlane)
                                / std::max<double>(1.0, targetSamples)))));

    qsizetype blanked = 0;
    qsizetype sampled = 0;
    for (int z = extent[4]; z <= extent[5]; ++z) {
        for (int y = extent[2]; y <= extent[3]; y += stride) {
            for (int x = extent[0]; x <= extent[1]; x += stride) {
                ++sampled;
                if (!std::isfinite(imageData->GetScalarComponentAsDouble(x, y, z, 0))) {
                    ++blanked;
                }
            }
        }
    }
    if (sampled <= 0) {
        return 0.0;
    }
    return static_cast<double>(blanked) / static_cast<double>(sampled);
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

std::array<int, 4> regionBounds2D(vtkWindowImage::RegionMode mode, const std::array<int, 2> &anchor,
                                  const std::array<int, 2> &current,
                                  const std::vector<std::array<int, 2>> &polygonVertices)
{
    if (mode == vtkWindowImage::RegionMode::Polygon && !polygonVertices.empty()) {
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

bool pointInRegion(vtkWindowImage::RegionMode mode, const std::array<int, 2> &anchor,
                   const std::array<int, 2> &current,
                   const std::vector<std::array<int, 2>> &polygonVertices, double annulusInnerRadius,
                   int x, int y)
{
    switch (mode) {
    case vtkWindowImage::RegionMode::Box:
        return pointInBox(anchor, current, x, y);
    case vtkWindowImage::RegionMode::Circle:
        return pointInCircle(anchor, current, x, y);
    case vtkWindowImage::RegionMode::Polygon:
        return pointInPolygon(polygonVertices, x, y);
    case vtkWindowImage::RegionMode::Annulus:
        return pointInAnnulus(anchor, current, annulusInnerRadius, x, y);
    case vtkWindowImage::RegionMode::None:
    default:
        return false;
    }
}

QString regionModeLabel(vtkWindowImage::RegionMode mode)
{
    switch (mode) {
    case vtkWindowImage::RegionMode::Box:
        return u"Box"_s;
    case vtkWindowImage::RegionMode::Circle:
        return u"Circle"_s;
    case vtkWindowImage::RegionMode::Polygon:
        return u"Polygon"_s;
    case vtkWindowImage::RegionMode::Annulus:
        return u"Annulus"_s;
    case vtkWindowImage::RegionMode::None:
    default:
        return u"Region"_s;
    }
}

RegionStatistics computeRegionStatistics2D(vtkImageData *imageData, vtkWindowImage::RegionMode mode,
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

    stats.sum = sum;
    stats.mean = sum / static_cast<double>(stats.validCount);
    stats.stddev = std::sqrt(std::max(0.0, sumSq / static_cast<double>(stats.validCount)
                                               - stats.mean * stats.mean));
    std::sort(values.begin(), values.end());
    const int mid = stats.validCount / 2;
    stats.median = (stats.validCount % 2 == 0) ? 0.5 * (values[mid - 1] + values[mid]) : values[mid];

    // MAD-based robust σ — much more reliable than std-dev for radio fields
    // where bright sources skew the variance.
    std::vector<double> absDev;
    absDev.reserve(values.size());
    for (double v : values) absDev.push_back(std::abs(v - stats.median));
    std::sort(absDev.begin(), absDev.end());
    const int amid = static_cast<int>(absDev.size()) / 2;
    const double mad = (absDev.size() % 2 == 0)
            ? 0.5 * (absDev[amid - 1] + absDev[amid])
            : absDev[amid];
    stats.madSigma = 1.4826 * mad;

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

SanityReport buildImageSanityReport(const std::array<QString, 3> &ctype,
                                    const std::array<QString, 3> &cunit,
                                    const std::array<double, 3> &crval,
                                    const std::array<double, 3> &crpix,
                                    const std::array<double, 3> &cdelt,
                                    vtkImageData *imageData)
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

    for (int axis = 0; axis < 2; ++axis) {
        if (axisHasAnyMetadata(ctype[axis], cunit[axis], crval[axis], crpix[axis], cdelt[axis])
            && !axisHasLinearWcs(ctype[axis], crval[axis], crpix[axis], cdelt[axis])) {
            warnings << u"Axis %1 has incomplete WCS metadata."_s.arg(axis + 1);
        }
    }

    const bool axis0Celestial = isCelestialLikeAxis(ctype[0]);
    const bool axis1Celestial = isCelestialLikeAxis(ctype[1]);
    if (axis0Celestial != axis1Celestial) {
        warnings << u"Only one spatial axis looks celestial; the coordinate pairing is incomplete."_s;
    } else if (axis0Celestial && !pairRecognized()) {
        warnings << u"Celestial axis pairing is not recognized as FK5, Galactic, or Ecliptic."_s;
    } else if (!axis0Celestial && !axis1Celestial) {
        unknowns << u"Celestial WCS metadata unavailable; overlay may fall back to pixel coordinates."_s;
    }

    if (ctype[0].trimmed().isEmpty() || ctype[1].trimmed().isEmpty()) {
        unknowns << u"CTYPE metadata incomplete for one or more spatial axes."_s;
    }

    const double blankFraction = computeBlankFraction(imageData);
    if (blankFraction >= 0.2) {
        warnings << u"Loaded image is heavily blanked/NaN (%1%)."_s.arg(blankFraction * 100.0, 0, 'f', 1);
    }

    if (!warnings.isEmpty() || !unknowns.isEmpty()) {
        unknowns << u"Backend image still opens, but incomplete metadata lowers WCS confidence."_s;
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
        details = u"No common FITS/WCS issues detected in the current image metadata."_s;
    }
    report.details = details;
    return report;
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
    image->SetOrigin(0., 0., 0.);
    image->SetSpacing(1., 1., 1.);
    image->AllocateScalars(VTK_FLOAT, 1);
    image->SetScalarComponentFromFloat(0, 0, 0, 0, 0.f);
    return image;
}

ImageLayerLoadResult createPlaceholderRemoteLayerResult(const QString &filepath)
{
    ImageLayerLoadResult result;
    result.valid = true;
    result.filepath = filepath.toStdString();
    result.imageData = createPlaceholderImageData();
    result.scalarRange = { 0., 0. };
    return result;
}

ImageLayerLoadResult fetchRemoteImageLayer(const QString &backendUrl, const QString &datasetId,
                                           const QString &datasetPath, bool previewOnly = false,
                                           int previewMaxLongestSide = 1536, int requestGeneration = 0,
                                           const QString &sessionId = QString(),
                                           const QString &backendToken = QString())
{
    ImageLayerLoadResult result;
    result.filepath = datasetPath.toStdString();
    result.requestGeneration = requestGeneration;

    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    const auto response = previewOnly ? client.requestImagePreview(datasetId, previewMaxLongestSide)
                                      : client.requestImage(datasetId);
    if (!response.valid) {
        result.errorMessage = response.error.isEmpty() ? "Remote image request failed."
                                                       : response.error.toStdString();
        return result;
    }

    if (response.scalarType != u"float32"_s) {
        result.errorMessage = "Unsupported remote image scalar type.";
        return result;
    }

    const qsizetype expectedBytes =
            static_cast<qsizetype>(response.width) * response.height * static_cast<qsizetype>(sizeof(float));
    if (response.width <= 0 || response.height <= 0 || response.data.size() != expectedBytes) {
        result.errorMessage = "Invalid remote image payload.";
        return result;
    }

    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, response.width - 1, 0, response.height - 1, 0, 0);
    image->SetOrigin(0., 0., 0.);
    double spacingX = 1.0;
    double spacingY = 1.0;
    if (response.isPreview && response.width > 1 && response.height > 1 && response.fullWidth > 1
        && response.fullHeight > 1) {
        spacingX = static_cast<double>(response.fullWidth - 1) / static_cast<double>(response.width - 1);
        spacingY = static_cast<double>(response.fullHeight - 1) / static_cast<double>(response.height - 1);
    }
    image->SetSpacing(spacingX, spacingY, 1.);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(image->GetScalarPointer(), response.data.constData(),
                static_cast<std::size_t>(expectedBytes));

    result.imageData = image;
    result.scalarRange = { response.rangeMin, response.rangeMax };
    result.isPreview = response.isPreview;
    result.fullWidth = response.fullWidth;
    result.fullHeight = response.fullHeight;
    result.previewScaleFactor = response.previewScaleFactor;
    result.valid = true;
    qDebug().noquote()
            << QStringLiteral("[remote-image] fetched mode=%1 dims=%2x%3 full=%4x%5 scale=%6")
                       .arg(result.isPreview ? QStringLiteral("preview") : QStringLiteral("full"))
                       .arg(response.width)
                       .arg(response.height)
                       .arg(response.fullWidth)
                       .arg(response.fullHeight)
                       .arg(response.previewScaleFactor, 0, 'f', 3);
    return result;
}

// ── WCS TAN projection helpers ────────────────────────────────────────────────
// Implements gnomonic (TAN) forward and inverse transforms matching the math
// used by AstroUtils / libwcs.  All angles in degrees; pixels are 0-indexed.
//
// crpix is the FITS 1-indexed reference pixel → we subtract 1 to get 0-indexed.
// cdelt is degrees per pixel (typically negative for RA axis).

/// 0-indexed pixel → celestial (lon, lat) in degrees via TAN projection.
static void tanPixToSky(double pixX, double pixY,
                        const std::array<double, 3> &crpix,
                        const std::array<double, 3> &crval,
                        const std::array<double, 3> &cdelt,
                        double &lonDeg, double &latDeg)
{
    const double deg2rad = vtkMath::Pi() / 180.0;
    const double rad2deg = 180.0 / vtkMath::Pi();

    // Offset from reference pixel (0-indexed)
    const double xi  = cdelt[0] * (pixX - (crpix[0] - 1.0));  // degrees
    const double eta = cdelt[1] * (pixY - (crpix[1] - 1.0));  // degrees

    const double xi_r  = xi  * deg2rad;
    const double eta_r = eta * deg2rad;
    const double lon0  = crval[0] * deg2rad;
    const double lat0  = crval[1] * deg2rad;

    const double D = std::cos(lat0) - eta_r * std::sin(lat0);
    lonDeg = (lon0 + std::atan2(xi_r, D)) * rad2deg;
    latDeg = std::atan2(std::sin(lat0) + eta_r * std::cos(lat0),
                        std::sqrt(xi_r * xi_r + D * D)) * rad2deg;
}

/// Celestial (lon, lat) in degrees → 0-indexed pixel via TAN projection.
static void tanSkyToPix(double lonDeg, double latDeg,
                        const std::array<double, 3> &crpix,
                        const std::array<double, 3> &crval,
                        const std::array<double, 3> &cdelt,
                        double &pixX, double &pixY)
{
    const double deg2rad = vtkMath::Pi() / 180.0;

    const double lon   = lonDeg  * deg2rad;
    const double lat   = latDeg  * deg2rad;
    const double lon0  = crval[0] * deg2rad;
    const double lat0  = crval[1] * deg2rad;

    const double dlon  = lon - lon0;
    const double denom = std::sin(lat0) * std::sin(lat)
                       + std::cos(lat0) * std::cos(lat) * std::cos(dlon);
    const double xi_r  = std::cos(lat) * std::sin(dlon) / denom;
    const double eta_r = (std::cos(lat0) * std::sin(lat)
                        - std::sin(lat0) * std::cos(lat) * std::cos(dlon)) / denom;

    const double rad2deg = 180.0 / vtkMath::Pi();
    pixX = xi_r  * rad2deg / cdelt[0] + (crpix[0] - 1.0);
    pixY = eta_r * rad2deg / cdelt[1] + (crpix[1] - 1.0);
}

// ── (fetchVlkbAdditionalLayer removed) ───────────────────────────────────────
// The VLKB add-layer path now reuses addLayerImage() directly, which calls
// loadImageLayer() → AstroUtils/libwcs for WCS alignment.  This is the same
// pipeline as "File → Add New Layer" and handles CD-matrix FITS correctly.
// ─────────────────────────────────────────────────────────────────────────────
#if 0  // dead code block kept for reference, not compiled
static ImageLayerLoadResult fetchVlkbAdditionalLayer(const QString &backendUrl,
                                                      const QString &backendToken,
                                                      const QString &backendPath,
                                                      const QString &displayName,
                                                      const std::array<double, 3> &masterCrpix,
                                                      const std::array<double, 3> &masterCrval,
                                                      const std::array<double, 3> &masterCdelt,
                                                      const std::array<QString, 3> &masterCtype)
{
    ImageLayerLoadResult result;
    result.filepath = displayName.toStdString(); // used as the layer display name

    BackendClient client(backendUrl, backendToken);
    const auto opened = client.openDataset(backendPath);
    if (!opened.valid) {
        result.errorMessage = opened.error.isEmpty() ? "Could not open VLKB layer on backend."
                                                     : opened.error.toStdString();
        return result;
    }
    if (opened.kind != u"image"_s) {
        result.errorMessage = "VLKB entry is not a 2D image (kind=" + opened.kind.toStdString() + ").";
        return result;
    }

    const auto response = client.requestImage(opened.datasetId);
    if (!response.valid) {
        result.errorMessage = response.error.isEmpty() ? "Remote image request failed."
                                                       : response.error.toStdString();
        return result;
    }
    if (response.scalarType != u"float32"_s) {
        result.errorMessage = "Unsupported remote image scalar type.";
        return result;
    }
    const qsizetype expectedBytes =
            static_cast<qsizetype>(response.width) * response.height
            * static_cast<qsizetype>(sizeof(float));
    if (response.width <= 0 || response.height <= 0 || response.data.size() != expectedBytes) {
        result.errorMessage = "Invalid remote image payload.";
        return result;
    }

    // Build the raw layer image from the backend payload (no resampling).
    auto rawImage = vtkSmartPointer<vtkImageData>::New();
    rawImage->SetExtent(0, response.width - 1, 0, response.height - 1, 0, 0);
    rawImage->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(rawImage->GetScalarPointer(), response.data.constData(),
                static_cast<std::size_t>(expectedBytes));

    result.imageData  = rawImage;
    result.scalarRange = { response.rangeMin, response.rangeMax };

    // ── Legacy WCS alignment: spacing + origin + rotation ────────────────────
    // Mirrors ImageLayerSet::registerLayerToMaster exactly:
    //
    //   1. Isotropic pixel-scale ratio  →  VTK spacing
    //   2. WCS round-trip pixel (0,0) : layer→sky→master pixel  →  VTK origin
    //   3. WCS round-trip pixel (0,100): slope → rotation angle  →  vtkTransform
    //
    // Frame conversion between layer and master (e.g. FK5 ↔ Galactic) is
    // handled by wcscon if the CTYPE pairs differ.
    //
    // IMPORTANT: secpixLayer uses opened.pixelScaleArcsec (from astropy, handles
    // CD matrix) instead of computing sqrt(|CDELT1*CDELT2|)*3600 from raw header
    // CDELT which defaults to 1.0 (3600 arcsec/pix) for CD-matrix-only FITS.
    // Similarly, sky coordinates for origin/rotation come from opened.wcsRefSky*
    // (astropy WCS pixel_to_world) rather than our manual tanPixToSky, which also
    // requires correct CDELT.

    // ── Debug: master WCS params ──────────────────────────────────────────────
    // masterCdelt here is already the effective CDELT (from addLayerFromBackendPath
    // which substitutes remoteDatasetEffectiveCdelt when valid).
    const double secpixMaster = std::sqrt(std::abs(masterCdelt[0] * masterCdelt[1])) * 3600.0;
    qDebug().noquote()
            << QStringLiteral("[vlkb-layer] MASTER  cdelt=(%1,%2) secpix=%3 arcsec/px"
                               "  crpix=(%4,%5) crval=(%6,%7) ctype=(%8,%9)")
                       .arg(masterCdelt[0], 0, 'g', 8).arg(masterCdelt[1], 0, 'g', 8)
                       .arg(secpixMaster, 0, 'g', 6)
                       .arg(masterCrpix[0], 0, 'f', 2).arg(masterCrpix[1], 0, 'f', 2)
                       .arg(masterCrval[0], 0, 'f', 6).arg(masterCrval[1], 0, 'f', 6)
                       .arg(masterCtype[0]).arg(masterCtype[1]);

    // ── Debug: overlay WCS params ─────────────────────────────────────────────
    const double secpixLayerFromCdelt =
            std::sqrt(std::abs(opened.cdelt[0] * opened.cdelt[1])) * 3600.0;
    qDebug().noquote()
            << QStringLiteral("[vlkb-layer] OVERLAY cdelt=(%1,%2) secpix_cdelt=%3 secpix_astropy=%4"
                               "  crpix=(%5,%6) crval=(%7,%8) ctype=(%9,%10)"
                               "  wcsRefValid=%11")
                       .arg(opened.cdelt[0], 0, 'g', 8).arg(opened.cdelt[1], 0, 'g', 8)
                       .arg(secpixLayerFromCdelt, 0, 'g', 6)
                       .arg(opened.pixelScaleArcsec, 0, 'g', 6)
                       .arg(opened.crpix[0], 0, 'f', 2).arg(opened.crpix[1], 0, 'f', 2)
                       .arg(opened.crval[0], 0, 'f', 6).arg(opened.crval[1], 0, 'f', 6)
                       .arg(opened.ctype[0]).arg(opened.ctype[1])
                       .arg(opened.wcsRefSkyValid ? QStringLiteral("yes") : QStringLiteral("no"));
    if (opened.wcsRefSkyValid) {
        qDebug().noquote()
                << QStringLiteral("[vlkb-layer] OVERLAY wcsRef: pix(0,0)->sky(%1,%2)"
                                   "  pix(0,100)->sky(%3,%4)")
                           .arg(opened.wcsRefSky00[0], 0, 'f', 8).arg(opened.wcsRefSky00[1], 0, 'f', 8)
                           .arg(opened.wcsRefSky0100[0], 0, 'f', 8).arg(opened.wcsRefSky0100[1], 0, 'f', 8);
    }

    // Use astropy pixel scale when available; fall back to CDELT only as last resort.
    const double secpixLayer = (opened.pixelScaleArcsec > 0.0 && std::isfinite(opened.pixelScaleArcsec))
                                ? opened.pixelScaleArcsec
                                : secpixLayerFromCdelt;

    if (secpixLayer > 0.0 && secpixMaster > 0.0
            && std::isfinite(secpixLayer) && std::isfinite(secpixMaster)) {

        // 1. Isotropic spacing
        const double scaledPixel = secpixLayer / secpixMaster;
        result.spacing = { scaledPixel, scaledPixel, 1.0 };

        // Celestial frame codes (needed for cross-frame conversion via wcscon)
        const int layerFrame  = inferCelestialFrameFromCtypePair(opened.ctype);
        const int masterFrame = inferCelestialFrameFromCtypePair(masterCtype);
        const bool needFrameConv = (layerFrame >= 0 && masterFrame >= 0
                                    && layerFrame != masterFrame);

        // Helper: convert sky coords from layer's frame to master's frame
        auto toMasterFrame = [&](double &lon, double &lat) {
            if (needFrameConv) {
                wcscon(layerFrame, masterFrame, 2000.0, 2000.0, &lon, &lat, 2000.0);
            }
        };

        // 2. Origin: layer pixel (0,0) → sky → master pixel
        //    Prefer astropy-computed sky coord (wcsRefSky00) over manual tanPixToSky
        //    to avoid CDELT=1.0 corruption in CD-matrix FITS.
        double lon0, lat0;
        if (opened.wcsRefSkyValid) {
            lon0 = opened.wcsRefSky00[0];
            lat0 = opened.wcsRefSky00[1];
        } else {
            tanPixToSky(0.0, 0.0, opened.crpix, opened.crval, opened.cdelt, lon0, lat0);
        }
        toMasterFrame(lon0, lat0);
        double mx0, my0;
        tanSkyToPix(lon0, lat0, masterCrpix, masterCrval, masterCdelt, mx0, my0);
        result.origin = { mx0, my0, 0.0 };

        // 3. Rotation: layer pixel (0,100) → sky → master pixel → slope → angle
        double lon1, lat1;
        if (opened.wcsRefSkyValid) {
            lon1 = opened.wcsRefSky0100[0];
            lat1 = opened.wcsRefSky0100[1];
        } else {
            tanPixToSky(0.0, 100.0, opened.crpix, opened.crval, opened.cdelt, lon1, lat1);
        }
        toMasterFrame(lon1, lat1);
        double mx1, my1;
        tanSkyToPix(lon1, lat1, masterCrpix, masterCrval, masterCdelt, mx1, my1);

        const double dx = mx1 - mx0;
        const double dy = my1 - my0;
        if (std::abs(dx) > 1e-9) {
            // Same formula as legacy vtkwindow_new::addLayerImage / registerLayerToMaster
            const double m = std::abs(dy / dx);
            result.rotationDegrees = 90.0 - std::atan(m) * 180.0 / M_PI;
        }
        // dx ≈ 0: overlay column maps to master column → no rotation

        qDebug().noquote()
                << QStringLiteral("[vlkb-layer] COMPUTED scaledPixel=%1"
                                   " origin=(%2,%3) angle=%4°"
                                   " frameConv=%5 skySource=%6")
                           .arg(scaledPixel, 0, 'g', 6)
                           .arg(mx0, 0, 'g', 8).arg(my0, 0, 'g', 8)
                           .arg(result.rotationDegrees, 0, 'f', 4)
                           .arg(needFrameConv ? QStringLiteral("yes") : QStringLiteral("no"))
                           .arg(opened.wcsRefSkyValid ? QStringLiteral("astropy") : QStringLiteral("tanPixToSky"));
    } else {
        qWarning().noquote()
                << QStringLiteral("[vlkb-layer] degenerate pixel scale for \"%1\","
                                   " secpixLayer=%2 secpixMaster=%3"
                                   " (layer cdelt=(%4,%5) astropy=%6);"
                                   " using raw image without WCS alignment")
                           .arg(displayName)
                           .arg(secpixLayer).arg(secpixMaster)
                           .arg(opened.cdelt[0]).arg(opened.cdelt[1])
                           .arg(opened.pixelScaleArcsec);
    }

    result.valid = true;
    return result;
}
#endif  // dead code block end

void buildArrowGeometry(vtkPoints *points, vtkCellArray *lines,
                        double x1, double y1, double x2, double y2)
{
    points->Reset();
    lines->Reset();

    // Shaft: from label pos (x1,y1) to tip (x2,y2)
    points->InsertNextPoint(x1, y1, 0.0); // 0
    points->InsertNextPoint(x2, y2, 0.0); // 1
    lines->InsertNextCell(2);
    lines->InsertCellPoint(0);
    lines->InsertCellPoint(1);

    // Arrowhead at (x2, y2) — fixed size, never larger than 30% of shaft
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len > 1e-6) {
        constexpr double fixedHeadLen = 8.0;
        const double headLen = std::min(fixedHeadLen, len * 0.3);
        const double ndx = dx / len;
        const double ndy = dy / len;
        const double px = -ndy;
        const double py = ndx;
        const double headWidth = headLen * 0.4;
        const double bx = x2 - ndx * headLen;
        const double by = y2 - ndy * headLen;

        points->InsertNextPoint(bx + px * headWidth, by + py * headWidth, 0.0); // 2
        points->InsertNextPoint(bx - px * headWidth, by - py * headWidth, 0.0); // 3

        lines->InsertNextCell(2);
        lines->InsertCellPoint(1);
        lines->InsertCellPoint(2);

        lines->InsertNextCell(2);
        lines->InsertCellPoint(1);
        lines->InsertCellPoint(3);
    }
}

QString formatImageBoundsSummary(vtkImageData *imageData)
{
    if (!imageData) {
        return u"unavailable"_s;
    }

    double bounds[6];
    imageData->GetBounds(bounds);
    return u"x=%1..%2 y=%3..%4"_s.arg(std::lround(bounds[0]))
            .arg(std::lround(bounds[1]))
            .arg(std::lround(bounds[2]))
            .arg(std::lround(bounds[3]));
}
}

vtkWindowImage::vtkWindowImage(const QString &filepath, const QString &backendUrl,
                               const QString &datasetId,
                               const std::array<QString, 3> &remoteCtype,
                               const std::array<QString, 3> &remoteCunit,
                               const std::array<double, 3> &remoteCrval,
                               const std::array<double, 3> &remoteCrpix,
                               const std::array<double, 3> &remoteCdelt,
                               const QString &remoteDegenerateAxesSummary,
                               const QString &remoteWcsStatus,
                               const QString &remoteWcsWarningMessage,
                               const QString &remoteSessionId,
                               const QString &remoteBackendToken, QWidget *parent)
    : QMainWindow(parent),
      ui(new Ui::vtkWindowImage),
      filepath(filepath),
      remoteBackendUrl(backendUrl),
      remoteDatasetId(datasetId),
      remoteSessionId(remoteSessionId),
      remoteBackendToken(remoteBackendToken),
      remoteDatasetCtype(remoteCtype),
      remoteDatasetCunit(remoteCunit),
      remoteDatasetCrval(remoteCrval),
      remoteDatasetCrpix(remoteCrpix),
      remoteDatasetCdelt(remoteCdelt),
      remoteDegenerateAxesSummary(remoteDegenerateAxesSummary),
      remoteWcsStatus(remoteWcsStatus),
      remoteWcsWarningMessage(remoteWcsWarningMessage),
      lutCustomizer(nullptr),
      profileWidget(nullptr),
      layers(nullptr),
      importService(std::make_unique<ImageLayerImportService>())
{
    ui->setupUi(this);
    this->setupViewerToolbar();
    // setupSidebar() is called later, after all QActions are initialised
    qDebug().noquote()
            << QStringLiteral("[wcs] remote metadata loaded ctype=%1,%2 cdelt=%3,%4")
                       .arg(this->remoteDatasetCtype[0], this->remoteDatasetCtype[1])
                       .arg(this->remoteDatasetCdelt[0], 0, 'g', 12)
                       .arg(this->remoteDatasetCdelt[1], 0, 'g', 12);
    // The file path is shown in the Layout B path-pill toolbar.
    this->setWindowTitle(u"VisIVO Visual Analytics"_s);
    this->setAttribute(Qt::WA_DeleteOnClose);
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
    this->catalogueInfoLabel = new QLabel(this);
    this->catalogueInfoLabel->setStyleSheet(u"QLabel { padding-left: 8px; color: palette(window-text); }"_s);
    this->catalogueInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addPermanentWidget(this->catalogueInfoLabel);
    this->updateCatalogueInfoPanel();
    this->statusMessageClearTimer.setSingleShot(true);
    QObject::connect(&this->statusMessageClearTimer, &QTimer::timeout, this, [this]() {
        this->persistentStatusActive = false;
        this->statusBar()->clearMessage();
    });
    QObject::connect(&this->layerLoadWatcher, &QFutureWatcher<ImageLayerLoadResult>::finished, this,
                     [this]() {
                         this->setLayerImportEnabled(true);

                         const auto result = this->layerLoadWatcher.result();
                         if (!result.valid) {
                             if (!result.errorMessage.empty()) {
                                 this->persistentStatusActive = false;
                                 this->statusMessageClearTimer.stop();
                                 QMessageBox::warning(this, u"Import FITS file"_s,
                                                      QString::fromStdString(result.errorMessage));
                             }
                             m_pendingVlkbDisplayName.clear();
                             this->clearPersistentStatusMessage();
                             return;
                         }

                         QElapsedTimer applyTimer;
                         applyTimer.start();
                         this->applyLoadedLayer(result);
                         qDebug().noquote()
                                 << QStringLiteral("[perf][layer] UI apply: %1 ms")
                                            .arg(applyTimer.elapsed());

                         // If this load was triggered by addLayerFromBackendPath (VLKB flow)
                         // or by the Stokes companion loader, rename + tag the new layer.
                         if (!m_pendingVlkbDisplayName.isEmpty()) {
                             const int lastIdx = this->layers ? this->layers->rowCount() - 1 : -1;
                             if (lastIdx > 0) {
                                 this->setLayerDisplayName(lastIdx, m_pendingVlkbDisplayName);
                                 // Stokes companion tagging: names "Stokes Q/U/V"
                                 if (m_pendingVlkbDisplayName == u"Stokes Q"_s)
                                     m_stokesRoles.insert(lastIdx, StokesRole::Q);
                                 else if (m_pendingVlkbDisplayName == u"Stokes U"_s)
                                     m_stokesRoles.insert(lastIdx, StokesRole::U);
                                 else if (m_pendingVlkbDisplayName == u"Stokes V"_s)
                                     m_stokesRoles.insert(lastIdx, StokesRole::V);
                             }
                             m_pendingVlkbDisplayName.clear();
                         }

                         this->clearPersistentStatusMessage();
                     });
    QObject::connect(&this->remoteImageWatcher, &QFutureWatcher<ImageLayerLoadResult>::finished, this,
                     [this]() {
                         const auto result = this->remoteImageWatcher.result();
                         if (result.requestGeneration != this->remoteLoadGeneration) {
                             qDebug().noquote()
                                     << QStringLiteral("[remote-image] discarded stale preview generation=%1 current=%2")
                                                .arg(result.requestGeneration)
                                                .arg(this->remoteLoadGeneration);
                             return;
                         }
                         if (!result.valid || !result.imageData) {
                             qDebug().noquote()
                                     << QStringLiteral("[remote-image] preview failed: %1")
                                                .arg(QString::fromStdString(result.errorMessage));
                             this->showPersistentStatusMessage(result.errorMessage.empty()
                                                                       ? u"Preview unavailable; loading full-resolution remote image..."_s
                                                                       : u"Remote preview failed; loading full resolution..."_s);
                             this->startRemoteFullResolutionLoad();
                             return;
                         }

                         this->applyRemoteMasterLayer(result);
                         if (result.isPreview) {
                             this->showPersistentStatusMessage(
                                     u"Remote preview ready; loading full-resolution image..."_s, 0);
                             this->startRemoteFullResolutionLoad();
                         } else {
                             this->clearPersistentStatusMessage();
                         }
                     });
    QObject::connect(&this->remoteFullImageWatcher, &QFutureWatcher<ImageLayerLoadResult>::finished, this,
                     [this]() {
                         const auto result = this->remoteFullImageWatcher.result();
                         if (result.requestGeneration != this->remoteLoadGeneration) {
                             qDebug().noquote()
                                     << QStringLiteral("[remote-image] discarded stale full generation=%1 current=%2")
                                                .arg(result.requestGeneration)
                                                .arg(this->remoteLoadGeneration);
                             return;
                         }
                         if (!result.valid || !result.imageData) {
                             qDebug().noquote()
                                     << QStringLiteral("[remote-image] full-resolution load failed: %1")
                                                .arg(QString::fromStdString(result.errorMessage));
                             this->m_loadingActor->VisibilityOff();
                             this->persistentStatusActive = false;
                             this->statusMessageClearTimer.stop();
                             this->statusBar()->showMessage(result.errorMessage.empty()
                                                                    ? u"Could not load full-resolution remote image."_s
                                                                    : QString::fromStdString(result.errorMessage),
                                                            4000);
                             return;
                         }

                         this->transitioningToFull = false;
                         this->suspendLutEditorUpdates = false;
                         this->applyRemoteMasterLayer(result);
                         this->clearPersistentStatusMessage();
                     });

    // Backend-authoritative: initialise with a placeholder; real image data
    // arrives via the remote full-image watcher.
    this->layers = new LayerListModel(createPlaceholderRemoteLayerResult(this->filepath), this);

    this->setupRenderer();
    this->setupContourPipeline();
    this->applyInitialAutoscale(this->layers->getImageData(this->layers->getMasterIndex()),
                                this->layers->getLookupTable(this->layers->getMasterIndex()),
                                "initial master");
    this->updateLargeImageMode(this->layers->getImageData(this->layers->getMasterIndex()), "initial");
    this->updateDataStatePanel();
    this->updateSanityPanel();
    this->updateWcsStatusIndicator();

    // Setup Menu File
    QObject::connect(ui->actionAddFITS, &QAction::triggered, this, &vtkWindowImage::addLocalFile);

    // Setup Menu WCS
    auto groupWCS = new QActionGroup(this);
    groupWCS->addAction(ui->actionGalactic);
    groupWCS->addAction(ui->actionFK5);
    groupWCS->addAction(ui->actionEcliptic);
    QObject::connect(groupWCS, &QActionGroup::triggered, this, &vtkWindowImage::changeLegendWCS);
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
        qDebug().noquote()
                << QStringLiteral("[wcs] format changed mode=%1")
                           .arg(this->useSexagesimalWcsFormat ? QStringLiteral("sexagesimal")
                                                              : QStringLiteral("decimal"));
        this->refreshWcsOverlayImmediately();
    });
    this->applyDefaultWcsFormatForSelectedFrame();

    // Setup Menu Tools
    ui->actionProfile->setCheckable(true);
    QObject::connect(ui->actionProfile, &QAction::toggled, this, &vtkWindowImage::setProbeModeActive);
    this->actionExtractSpectrum = ui->menuTools->addAction(u"Extract Spectrum"_s);
    this->actionExtractSpectrum->setEnabled(false);
    this->actionExtractSpectrum->setToolTip(
            u"Spectrum extraction is only available for cube views."_s);
    this->actionExtractSpectrum->setStatusTip(
            u"Spectrum extraction is only available for cube views."_s);
    ui->menuTools->addSeparator();
    this->actionBoxRegion = ui->menuTools->addAction(u"Box Region Stats"_s);
    this->actionCircleRegion = ui->menuTools->addAction(u"Circle Region Stats"_s);
    this->actionPolygonRegion = ui->menuTools->addAction(u"Polygon Region Analysis"_s);
    this->actionAnnulusRegion = ui->menuTools->addAction(u"Annulus Region Analysis"_s);
    this->actionBoxRegion->setCheckable(true);
    this->actionCircleRegion->setCheckable(true);
    this->actionPolygonRegion->setCheckable(true);
    this->actionAnnulusRegion->setCheckable(true);
    auto *regionGroup = new QActionGroup(this);
    regionGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
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
    this->actionShowCatalogueOverlay->setChecked(false);
    this->actionShowCatalogueLabels->setEnabled(false);
    this->actionShowCatalogueLabels->setChecked(false); // no catalogue loaded yet
    this->actionClearCatalogueOverlay->setEnabled(false);
    QObject::connect(this->actionLoadCatalogueOverlay, &QAction::triggered, this,
                     &vtkWindowImage::loadCatalogueOverlay);
    QObject::connect(this->actionShowCatalogueOverlay, &QAction::toggled, this,
                     &vtkWindowImage::setCatalogueOverlayVisible);
    QObject::connect(this->actionShowCatalogueLabels, &QAction::toggled, this,
                     [this](bool) { this->updateCatalogueOverlayLabels(); this->vtkRender(); });
    QObject::connect(this->actionClearCatalogueOverlay, &QAction::triggered, this,
                     &vtkWindowImage::clearCatalogueOverlay);
    this->ensureCatalogueDock();

    // FITS export → Workspace
    ui->menuTools->addSeparator();
    m_actExportToWorkspace = ui->menuTools->addAction(u"Export to Workspace as FITS…"_s);
    m_actExportToWorkspace->setToolTip(
            u"Copy this dataset's FITS file into the Workspace Exports "
            u"directory for later download / re-open."_s);
    QObject::connect(m_actExportToWorkspace, &QAction::triggered, this, [this]() {
        bool ok = false;
        const QString basename = QInputDialog::getText(
                this, u"Export to Workspace"_s,
                u"Save in Workspace Exports as:"_s,
                QLineEdit::Normal,
                QFileInfo(this->filepath).fileName(), &ok);
        if (!ok || basename.trimmed().isEmpty()) return;
        const QString url   = this->remoteBackendUrl;
        const QString token = this->remoteBackendToken;
        const QString dsId  = this->remoteDatasetId;
        const QString bn    = basename.trimmed();
        this->statusBar()->showMessage(u"Exporting to workspace…"_s, 0);
        QPointer<vtkWindowImage> self(this);
        auto *watcher = new QFutureWatcher<QPair<bool,QString>>(this);
        QObject::connect(watcher, &QFutureWatcher<QPair<bool,QString>>::finished,
                         this, [self, watcher]() {
                             const auto r = watcher->result();
                             watcher->deleteLater();
                             if (!self) return;
                             self->statusBar()->clearMessage();
                             if (r.first) {
                                 QMessageBox::information(self, u"Export"_s,
                                         u"Saved to Workspace Exports:\n%1"_s.arg(r.second));
                             } else {
                                 QMessageBox::warning(self, u"Export"_s,
                                         u"Export failed:\n%1"_s.arg(r.second));
                             }
                         });
        watcher->setFuture(QtConcurrent::run([url, token, dsId, bn]() -> QPair<bool,QString> {
            BackendClient c(url, token);
            auto r = c.copyDatasetToWorkspace(dsId, bn);
            if (!r.valid) return { false, r.error };
            return { true, r.deletedFilename };
        }));
    });

    // Contour overlay — same approach as the cube viewer: toggle + Level/Lower/Upper
    ui->menuTools->addSeparator();
    m_actToggleContours = ui->menuTools->addAction(u"Show Contours"_s);
    m_actToggleContours->setCheckable(true);
    m_actToggleContours->setChecked(false);
    QObject::connect(m_actToggleContours, &QAction::toggled, this,
                     &vtkWindowImage::setContoursVisible);

    m_actLoadExternalContour = ui->menuTools->addAction(u"Load Contour from FITS…"_s);
    m_actLoadExternalContour->setToolTip(
            u"Overlay iso-contours from an external FITS file (e.g. a moment "
            u"map exported from a cube viewer)."_s);
    QObject::connect(m_actLoadExternalContour, &QAction::triggered, this,
                     &vtkWindowImage::addExternalContours);

    m_actClearAllContours = ui->menuTools->addAction(u"Clear All Contours"_s);
    QObject::connect(m_actClearAllContours, &QAction::triggered, this,
                     &vtkWindowImage::clearAllContours);

    // ── Measurement tools ─────────────────────────────────────────────────
    ui->menuTools->addSeparator();
    m_actRuler = ui->menuTools->addAction(u"Ruler (Distance)"_s);
    m_actRuler->setCheckable(true);
    m_actAngle = ui->menuTools->addAction(u"Angle (3 Points)"_s);
    m_actAngle->setCheckable(true);
    m_actClearMeas = ui->menuTools->addAction(u"Clear Measurement"_s);

    auto *measGroup = new QActionGroup(this);
    measGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    measGroup->addAction(m_actRuler);
    measGroup->addAction(m_actAngle);
    QObject::connect(m_actRuler, &QAction::toggled, this,
                     [this](bool c) { setMeasurementMode(c ? MeasurementMode::Ruler : MeasurementMode::None); });
    QObject::connect(m_actAngle, &QAction::toggled, this,
                     [this](bool c) { setMeasurementMode(c ? MeasurementMode::Angle : MeasurementMode::None); });
    QObject::connect(m_actClearMeas, &QAction::triggered, this,
                     &vtkWindowImage::clearMeasurement);

    // ── Histogram ─────────────────────────────────────────────────────────
    ui->menuTools->addSeparator();
    auto *actHistogram = ui->menuTools->addAction(u"Pixel Histogram…"_s);
    QObject::connect(actHistogram, &QAction::triggered, this,
                     &vtkWindowImage::showHistogramPanel);

    // ── Annotations ──────────────────────────────────────────────────────
    ui->menuTools->addSeparator();
    auto *actAddText = ui->menuTools->addAction(u"Add Text Annotation…"_s);
    auto *actAddArrow = ui->menuTools->addAction(u"Add Arrow Annotation…"_s);
    auto *actClearAnnot = ui->menuTools->addAction(u"Clear Annotations"_s);
    auto *actSaveAnnot = ui->menuTools->addAction(u"Save Annotations"_s);
    auto *actLoadAnnot = ui->menuTools->addAction(u"Load Annotations"_s);
    QObject::connect(actAddText, &QAction::triggered, this, &vtkWindowImage::addTextAnnotation);
    QObject::connect(actAddArrow, &QAction::triggered, this, &vtkWindowImage::addArrowAnnotation);
    QObject::connect(actClearAnnot, &QAction::triggered, this, &vtkWindowImage::clearAnnotations);
    QObject::connect(actSaveAnnot, &QAction::triggered, this, &vtkWindowImage::saveAnnotationsJson);
    QObject::connect(actLoadAnnot, &QAction::triggered, this, &vtkWindowImage::loadAnnotationsJson);

    // ── Stokes / Radio polarimetry ──────────────────────────────────────
    ui->menuTools->addSeparator();
    m_actLoadStokesCompanions = ui->menuTools->addAction(u"Load Stokes Q/U/V Companions…"_s);
    m_actLoadStokesCompanions->setToolTip(
            u"Auto-detect and load Stokes Q, U, V files matching the current Stokes I."_s);
    QObject::connect(m_actLoadStokesCompanions, &QAction::triggered, this,
                     &vtkWindowImage::detectAndLoadStokesCompanions);

    m_actComputeStokesP = ui->menuTools->addAction(u"Compute Pol. Intensity (P)"_s);
    m_actComputeStokesP->setToolTip(
            u"Compute P = √(Q² + U²) and add as a new layer."_s);
    QObject::connect(m_actComputeStokesP, &QAction::triggered, this,
                     &vtkWindowImage::computeStokesP);

    m_actComputeStokesPA = ui->menuTools->addAction(u"Compute Pol. Angle (PA)"_s);
    m_actComputeStokesPA->setToolTip(
            u"Compute PA = ½ · atan2(U, Q) in degrees and add as a new layer."_s);
    QObject::connect(m_actComputeStokesPA, &QAction::triggered, this,
                     &vtkWindowImage::computeStokesPA);

    m_actComputeStokesFrac = ui->menuTools->addAction(u"Compute Frac. Pol. (P/I)"_s);
    m_actComputeStokesFrac->setToolTip(
            u"Compute fractional linear polarization P/I and add as a new layer."_s);
    QObject::connect(m_actComputeStokesFrac, &QAction::triggered, this,
                     &vtkWindowImage::computeStokesFractional);

    m_actComputeStokesPdebiased = ui->menuTools->addAction(u"Compute Debiased P"_s);
    m_actComputeStokesPdebiased->setToolTip(
            u"Compute bias-corrected P = √(max(0, Q² + U² − σ²)); σ from MAD of Q & U."_s);
    QObject::connect(m_actComputeStokesPdebiased, &QAction::triggered, this,
                     &vtkWindowImage::computeStokesPdebiased);

    m_actComputeSpectralIndex = ui->menuTools->addAction(u"Compute Spectral Index…"_s);
    m_actComputeSpectralIndex->setToolTip(
            u"α = log(S_B / S_A) / log(ν_B / ν_A) from two layers at distinct frequencies."_s);
    QObject::connect(m_actComputeSpectralIndex, &QAction::triggered, this,
                     &vtkWindowImage::computeSpectralIndex);

    m_actComputeFaradayRM = ui->menuTools->addAction(u"Compute Faraday RM…"_s);
    m_actComputeFaradayRM->setToolTip(
            u"Linear fit of PA vs λ² from ≥ 3 (Q, U, ν) triplets — low-RM regime only."_s);
    QObject::connect(m_actComputeFaradayRM, &QAction::triggered, this,
                     &vtkWindowImage::computeFaradayRM);

    m_actTogglePolVecs = ui->menuTools->addAction(u"Show Polarization Vectors"_s);
    m_actTogglePolVecs->setCheckable(true);
    m_actTogglePolVecs->setToolTip(
            u"Overlay polarization vectors (oriented along PA, length ∝ P) on the image."_s);
    QObject::connect(m_actTogglePolVecs, &QAction::toggled, this,
                     &vtkWindowImage::setPolVectorsVisible);

    // Setup Menu View
    this->actionShowWcsAxes = new QAction(u"Show WCS Axes"_s, this);
    this->actionShowWcsAxes->setCheckable(true);
    this->actionShowWcsAxes->setChecked(this->showWcsAxes);
    ui->menuView->addAction(this->actionShowWcsAxes);
    QObject::connect(this->actionShowWcsAxes, &QAction::toggled, this, [this](bool checked) {
        this->showWcsAxes = checked;
        this->lastOverlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
                                           std::numeric_limits<double>::quiet_NaN(),
                                           std::numeric_limits<double>::quiet_NaN(),
                                           std::numeric_limits<double>::quiet_NaN() };
        this->lastOverlayViewportSize = { -1, -1 };
        this->setWcsOverlayVisible(checked);
        this->requestWcsOverlayRender();
    });

    // ── Blink / Compare ─────────────────────────────────────────────────
    ui->menuView->addSeparator();
    m_actBlink = new QAction(u"Blink Layers"_s, this);
    m_actBlink->setCheckable(true);
    m_actBlink->setToolTip(u"Rapidly alternate between layers 0 and 1."_s);
    ui->menuView->addAction(m_actBlink);
    QObject::connect(m_actBlink, &QAction::toggled, this, &vtkWindowImage::toggleBlink);
    m_blinkTimer.setInterval(m_blinkIntervalMs);
    QObject::connect(&m_blinkTimer, &QTimer::timeout, this, &vtkWindowImage::blinkTick);

    // ── Diagnostics + Command palette (replicated across windows) ─────────
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
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w->inherits("MainWindow")) {
                QMetaObject::invokeMethod(w, "openCommandPalette", Qt::QueuedConnection);
                return;
            }
        }
    });

    // All QActions are now initialised — safe to build the sidebar
    this->setupSidebar();

    // Color Maps Combobox — with gradient preview icon
    ui->comboLut->setIconSize(QSize(80, 14));
    auto cmaps = ColorMaps::GetColorMapNames();
    std::for_each(cmaps.cbegin(), cmaps.cend(), [this](const std::string &name) {
        ui->comboLut->addItem(makeColormapIcon(name), QString::fromStdString(name));
    });
    ui->comboLut->setCurrentText(QString::fromStdString(ColorMaps::DefaultColorMap));
    QObject::connect(ui->comboLut, &QComboBox::textActivated, this,
                     &vtkWindowImage::changeCurrentColorMap);

    // LUT Customizer — give the button a clear label and tooltip
    ui->btnLutEdit->setText(u"Advanced…"_s);
    ui->btnLutEdit->setToolTip(u"Open advanced LUT editor (histogram, manual range adjustment)"_s);
    QObject::connect(ui->btnLutEdit, &QPushButton::clicked, this,
                     &vtkWindowImage::showLUTCustomizer);

    // Scale: SegmentedToggle replaces the old radio buttons for visual
    // consistency with the cube viewer's mode selectors.
    ui->radioLinear->hide();
    ui->radioLog->hide();
    auto *scaleToggle = new SegmentedToggle(ui->frameLayerControls);
    scaleToggle->addItem(u"Linear"_s, u"Linear colour scale"_s);
    scaleToggle->addItem(u"Log"_s,    u"Logarithmic colour scale"_s);
    scaleToggle->setCurrentIndex(ui->radioLog->isChecked() ? 1 : 0);
    // Insert right where the radios were (after the LUT combo row).
    if (auto *fl = qobject_cast<QVBoxLayout *>(ui->frameLayerControls->layout())) {
        const int radioIdx = fl->indexOf(ui->radioLinear);
        if (radioIdx >= 0)
            fl->insertWidget(radioIdx, scaleToggle);
        else
            fl->addWidget(scaleToggle);
    }
    QObject::connect(scaleToggle, &SegmentedToggle::currentIndexChanged, this,
                     [this](int idx) {
                         const QSignalBlocker bL(ui->radioLinear);
                         const QSignalBlocker bG(ui->radioLog);
                         ui->radioLinear->setChecked(idx == 0);
                         ui->radioLog->setChecked(idx == 1);
                         this->changeCurrentColorScale();
                     });

    // Layer Opacity — label + tooltip
    ui->sliderOpacity->setToolTip(u"Layer opacity (0 = transparent, 100 = opaque)"_s);
    if (auto *fl = qobject_cast<QVBoxLayout *>(ui->frameLayerControls->layout())) {
        const int sliderIdx = fl->indexOf(ui->sliderOpacity);
        if (sliderIdx >= 0) {
            auto *opacityLbl = new QLabel(u"Layer opacity"_s, ui->frameLayerControls);
            opacityLbl->setStyleSheet(
                    QStringLiteral("QLabel { color: %1; font-size: 9px; "
                                   "font-weight: 600; margin-top: 4px; }")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
            fl->insertWidget(sliderIdx, opacityLbl);
        }
    }
    QObject::connect(ui->sliderOpacity, &QSlider::actionTriggered, this,
                     &vtkWindowImage::changeCurrentLayerOpacity);

    // Setup Layer List View
    ui->listLayer->setAcceptDrops(true);
    ui->listLayer->setModel(this->layers);
    ui->listLayer->setCurrentIndex(this->layers->index(0, 0));
    QObject::connect(this->layers, &LayerListModel::dataChanged, this, &vtkWindowImage::vtkRender);
    QObject::connect(ui->listLayer->selectionModel(), &QItemSelectionModel::currentChanged, this,
                     &vtkWindowImage::showCurrentLayerSettings);
    QObject::connect(ui->listLayer->selectionModel(), &QItemSelectionModel::currentChanged, this,
                     &vtkWindowImage::updateLUTCustomizer);

    // Backend-authoritative: kick off the preview load immediately.
    this->showPersistentStatusMessage(u"Loading image preview..."_s);
    this->remoteImageWatcher.setFuture(
            QtConcurrent::run(&fetchRemoteImageLayer, this->remoteBackendUrl,
                              this->remoteDatasetId, this->filepath, true, 1536,
                              ++this->remoteLoadGeneration, this->remoteSessionId,
                              this->remoteBackendToken));
}

vtkWindowImage::~vtkWindowImage()
{
    delete ui;
}

void vtkWindowImage::closeEvent(QCloseEvent *event)
{
    if (this->isBusy()) {
        this->showPersistentStatusMessage(u"Loading image..."_s);
        event->ignore();
        return;
    }

    // Cache the last-view thumbnail for the Data Hub recents panel.
    if (ui && ui->vtk) {
        ThumbnailCache::captureFromWidget(ui->vtk, this->filepath);
    }

    QMainWindow::closeEvent(event);
}

void vtkWindowImage::showLUTCustomizer()
{
    const int index = this->currentLayerIndex();
    if (!this->layers || index < 0 || index >= this->layers->rowCount()) {
        return;
    }
    qDebug().noquote() << QStringLiteral("[lut] open request index=%1 existing=%2")
                                  .arg(index)
                                  .arg(this->lutCustomizer != nullptr);
    if (!this->lutCustomizer) {
        this->lutCustomizer = new LUTCustomizerDialog(this);
        QObject::connect(this->lutCustomizer, &LUTCustomizerDialog::lutUpdated, this,
                         [this]() {
                             this->markDisplayStateAdjusted("lut editor");
                             this->showCurrentLayerSettings();
                         });
    }
    this->lutCustomizer->show();
    this->lutCustomizer->raise();
    this->lutCustomizer->activateWindow();
    qDebug().noquote() << QStringLiteral("[lut] dialog shown/raised");
    if (this->suspendLutEditorUpdates) {
        return;
    }
    QPointer<LUTCustomizerDialog> dialog = this->lutCustomizer;
    auto *dataset = this->layers->getImageData(index);
    auto *lut = this->layers->getLookupTable(index);
    QTimer::singleShot(0, this, [this, dialog, dataset, lut]() {
        if (!dialog || this->suspendLutEditorUpdates || !dataset || !lut) {
            return;
        }
        qDebug().noquote() << QStringLiteral("[lut] deferred init started");
        dialog->init(dataset, lut);
    });
}

void vtkWindowImage::updateLUTCustomizer()
{
    if (this->lutCustomizer && !this->suspendLutEditorUpdates) {
        const int index = this->currentLayerIndex();
        this->lutCustomizer->init(this->layers->getImageData(index),
                                  this->layers->getLookupTable(index));
    }
}

void vtkWindowImage::addLocalFile()
{
    if (this->isBusy()) {
        return;
    }

    RemoteFileBrowserDialog dialog(this->remoteBackendUrl, this->remoteBackendToken, this);
    dialog.setExtensionFilter({ u".fits"_s, u".fit"_s }, u"FITS files only"_s);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString path = dialog.selectedFilePath();
    if (path.isEmpty()) {
        return;
    }

    this->addLayerImage(path.toStdString());
}

void vtkWindowImage::setupRenderer()
{
    this->wcsOverlayInitialized = false;
    vtkNew<vtkRenderer> ren;
    ren->SetBackground(0.21, 0.23, 0.25);
    ren->GetActiveCamera()->ParallelProjectionOn();

    vtkNew<vtkGenericOpenGLRenderWindow> win;
    win->AddRenderer(ren);
    ui->vtk->setRenderWindow(win);
    ui->vtk->setEnableTouchEventProcessing(false);

    vtkNew<vtkInteractorStyleImage> style;
    win->GetInteractor()->SetInteractorStyle(style);
    win->GetInteractor()->AddObserver(vtkCommand::MouseMoveEvent, this,
                                      &vtkWindowImage::mouseCallback);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonPressEvent, this,
                                      &vtkWindowImage::beginImageInteraction);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonPressEvent, this,
                                      &vtkWindowImage::toggleProbeFreeze);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                                      &vtkWindowImage::endImageInteraction);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                                      &vtkWindowImage::finishRegionInteraction);
    win->GetInteractor()->AddObserver(vtkCommand::RightButtonPressEvent, this,
                                      &vtkWindowImage::finishRegionInteraction);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, this,
                                      &vtkWindowImage::finishRegionInteraction);

    this->coordinate->SetCoordinateSystemToDisplay();
    this->coordinate->SetViewport(ren);

    // Stack
    this->stack->AddImage(this->layers->getMasterLayerActor());
    this->stack->SetActiveLayer(0);
    ren->AddViewProp(this->stack);

    // Color bar
    this->colorbar->SetMaximumWidthInPixels(120);
    this->colorbar->SetPosition(0.9, 0.1);
    this->colorbar->SetLookupTable(this->layers->getLookupTable(this->layers->getMasterIndex()));
    ren->AddViewProp(this->colorbar);

    this->layerController =
            std::make_unique<ImageLayerController>(*(this->layers), this->stack, this->colorbar);

    // Legend: local AstroUtils-based legend not used in backend mode; remote WCS overlay actors are used instead.
    ren->AddViewProp(this->overlayXAxis);
    ren->AddViewProp(this->overlayYAxis);
    ren->AddViewProp(this->overlayXTitleActor);
    ren->AddViewProp(this->overlayYTitleActor);
    vtkNew<vtkPolyDataMapper> probeHorizontalMapper;
    probeHorizontalMapper->SetInputConnection(this->probeHorizontalLine->GetOutputPort());
    this->probeHorizontalActor->SetMapper(probeHorizontalMapper);
    this->probeHorizontalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->probeHorizontalActor->GetProperty()->SetLineWidth(1.5);
    this->probeHorizontalActor->VisibilityOff();
    ren->AddActor(this->probeHorizontalActor);
    vtkNew<vtkPolyDataMapper> probeVerticalMapper;
    probeVerticalMapper->SetInputConnection(this->probeVerticalLine->GetOutputPort());
    this->probeVerticalActor->SetMapper(probeVerticalMapper);
    this->probeVerticalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->probeVerticalActor->GetProperty()->SetLineWidth(1.5);
    this->probeVerticalActor->VisibilityOff();
    ren->AddActor(this->probeVerticalActor);
    // ── Measurement actors ─────────────────────────────────────────────────
    {
        const auto configureMeasActor = [&ren](vtkLineSource *line, vtkActor *actor) {
            vtkNew<vtkPolyDataMapper> mapper;
            mapper->SetInputConnection(line->GetOutputPort());
            actor->SetMapper(mapper);
            actor->GetProperty()->SetColor(1.0, 0.65, 0.0);
            actor->GetProperty()->SetLineWidth(2.0);
            actor->GetProperty()->SetLineStipplePattern(0xF0F0);
            actor->VisibilityOff();
            ren->AddActor(actor);
        };
        configureMeasActor(this->m_measLine1, this->m_measLineActor1);
        configureMeasActor(this->m_measLine2, this->m_measLineActor2);

        this->m_measLabelActor = vtkSmartPointer<vtkTextActor>::New();
        this->m_measLabelActor->GetTextProperty()->SetColor(1.0, 0.85, 0.1);
        this->m_measLabelActor->GetTextProperty()->SetFontSize(14);
        this->m_measLabelActor->GetTextProperty()->SetBold(true);
        this->m_measLabelActor->GetTextProperty()->SetShadow(true);
        this->m_measLabelActor->VisibilityOff();
        ren->AddViewProp(this->m_measLabelActor);
    }

    // ── Polarization vector overlay actor ─────────────────────────────────
    {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(this->m_polVecData);
        this->m_polVecActor->SetMapper(mapper);
        this->m_polVecActor->GetProperty()->SetColor(0.95, 0.95, 0.25);
        this->m_polVecActor->GetProperty()->SetLineWidth(1.2);
        this->m_polVecActor->VisibilityOff();
        ren->AddActor(this->m_polVecActor);
    }

    // ── Annotation placement preview actor ────────────────────────────────
    {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(this->m_annotPreviewData);
        this->m_annotPreviewActor->SetMapper(mapper);
        this->m_annotPreviewActor->GetProperty()->SetColor(1.0, 1.0, 0.3);
        this->m_annotPreviewActor->GetProperty()->SetLineWidth(2.0);
        this->m_annotPreviewActor->GetProperty()->SetOpacity(0.7);
        this->m_annotPreviewActor->VisibilityOff();
        ren->AddActor(this->m_annotPreviewActor);
    }

    vtkRenderer *renderer = ren;
    const auto configureRegionLineActor = [renderer](vtkLineSource *line, vtkActor *actor) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.2, 0.9, 1.0);
        actor->GetProperty()->SetLineWidth(2.0);
        actor->VisibilityOff();
        renderer->AddActor(actor);
    };
    configureRegionLineActor(this->regionTopLine, this->regionTopActor);
    configureRegionLineActor(this->regionBottomLine, this->regionBottomActor);
    configureRegionLineActor(this->regionLeftLine, this->regionLeftActor);
    configureRegionLineActor(this->regionRightLine, this->regionRightActor);
    this->regionCircleSource->SetNumberOfSides(96);
    this->regionCircleSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> regionCircleMapper;
    regionCircleMapper->SetInputConnection(this->regionCircleSource->GetOutputPort());
    this->regionCircleActor->SetMapper(regionCircleMapper);
    this->regionCircleActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionCircleActor->GetProperty()->SetLineWidth(2.0);
    this->regionCircleActor->VisibilityOff();
    ren->AddActor(this->regionCircleActor);
    this->regionAnnulusOuterSource->SetNumberOfSides(96);
    this->regionAnnulusOuterSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> regionAnnulusOuterMapper;
    regionAnnulusOuterMapper->SetInputConnection(this->regionAnnulusOuterSource->GetOutputPort());
    this->regionAnnulusOuterActor->SetMapper(regionAnnulusOuterMapper);
    this->regionAnnulusOuterActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionAnnulusOuterActor->GetProperty()->SetLineWidth(2.0);
    this->regionAnnulusOuterActor->GetProperty()->SetRepresentationToWireframe();
    this->regionAnnulusOuterActor->VisibilityOff();
    ren->AddActor(this->regionAnnulusOuterActor);
    this->regionAnnulusInnerSource->SetNumberOfSides(96);
    this->regionAnnulusInnerSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> regionAnnulusInnerMapper;
    regionAnnulusInnerMapper->SetInputConnection(this->regionAnnulusInnerSource->GetOutputPort());
    this->regionAnnulusInnerActor->SetMapper(regionAnnulusInnerMapper);
    this->regionAnnulusInnerActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionAnnulusInnerActor->GetProperty()->SetLineWidth(1.5);
    this->regionAnnulusInnerActor->GetProperty()->SetRepresentationToWireframe();
    this->regionAnnulusInnerActor->VisibilityOff();
    ren->AddActor(this->regionAnnulusInnerActor);
    vtkNew<vtkPolyDataMapper> regionAnnulusFillMapper;
    regionAnnulusFillMapper->SetInputData(this->regionAnnulusFillData);
    this->regionAnnulusFillActor->SetMapper(regionAnnulusFillMapper);
    this->regionAnnulusFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionAnnulusFillActor->GetProperty()->SetOpacity(0.18);
    this->regionAnnulusFillActor->GetProperty()->SetRepresentationToSurface();
    this->regionAnnulusFillActor->VisibilityOff();
    ren->AddActor(this->regionAnnulusFillActor);
    this->regionPolygonTriangulator->SetInputData(this->regionPolygonFillData);
    vtkNew<vtkPolyDataMapper> regionPolygonMapper;
    regionPolygonMapper->SetInputData(this->regionPolygonData);
    this->regionPolygonActor->SetMapper(regionPolygonMapper);
    this->regionPolygonActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionPolygonActor->GetProperty()->SetLineWidth(2.0);
    this->regionPolygonActor->VisibilityOff();
    ren->AddActor(this->regionPolygonActor);
    vtkNew<vtkPolyDataMapper> regionPolygonFillMapper;
    regionPolygonFillMapper->SetInputConnection(this->regionPolygonTriangulator->GetOutputPort());
    this->regionPolygonFillActor->SetMapper(regionPolygonFillMapper);
    this->regionPolygonFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionPolygonFillActor->GetProperty()->SetOpacity(0.18);
    this->regionPolygonFillActor->VisibilityOff();
    ren->AddActor(this->regionPolygonFillActor);
    this->catalogueOverlayData->SetPoints(this->catalogueOverlayPoints);
    this->catalogueOverlayData->SetLines(this->catalogueOverlayCells);
    vtkNew<vtkPolyDataMapper> catalogueOverlayMapper;
    catalogueOverlayMapper->SetInputData(this->catalogueOverlayData);
    this->catalogueOverlayActor->SetMapper(catalogueOverlayMapper);
    this->catalogueOverlayActor->GetProperty()->SetColor(1.0, 0.45, 0.15);
    this->catalogueOverlayActor->GetProperty()->SetLineWidth(1.5);
    this->catalogueOverlayActor->VisibilityOff();
    ren->AddActor(this->catalogueOverlayActor);
    this->catalogueHoverOverlayData->SetPoints(this->catalogueHoverOverlayPoints);
    this->catalogueHoverOverlayData->SetLines(this->catalogueHoverOverlayCells);
    vtkNew<vtkPolyDataMapper> catalogueHoverMapper;
    catalogueHoverMapper->SetInputData(this->catalogueHoverOverlayData);
    this->catalogueHoverOverlayActor->SetMapper(catalogueHoverMapper);
    this->catalogueHoverOverlayActor->GetProperty()->SetColor(1.0, 0.95, 0.45);
    this->catalogueHoverOverlayActor->GetProperty()->SetLineWidth(2.5);
    this->catalogueHoverOverlayActor->VisibilityOff();
    ren->AddActor(this->catalogueHoverOverlayActor);
    this->catalogueSelectionOverlayData->SetPoints(this->catalogueSelectionOverlayPoints);
    this->catalogueSelectionOverlayData->SetLines(this->catalogueSelectionOverlayCells);
    vtkNew<vtkPolyDataMapper> catalogueSelectionMapper;
    catalogueSelectionMapper->SetInputData(this->catalogueSelectionOverlayData);
    this->catalogueSelectionOverlayActor->SetMapper(catalogueSelectionMapper);
    this->catalogueSelectionOverlayActor->GetProperty()->SetColor(0.15, 0.95, 1.0);
    this->catalogueSelectionOverlayActor->GetProperty()->SetLineWidth(3.2);
    this->catalogueSelectionOverlayActor->VisibilityOff();
    ren->AddActor(this->catalogueSelectionOverlayActor);
    this->overlayXAxis->GetTitleTextProperty()->SetFontSize(16);
    this->overlayXAxis->GetTitleTextProperty()->SetBold(false);
    this->overlayYAxis->GetTitleTextProperty()->SetFontSize(16);
    this->overlayYAxis->GetTitleTextProperty()->SetOrientation(90.);
    this->overlayYAxis->GetTitleTextProperty()->SetBold(false);
    this->overlayXTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->overlayXTitleActor->GetTextProperty()->SetFontSize(14);
    this->overlayXTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->overlayXTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->overlayYTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->overlayYTitleActor->GetTextProperty()->SetFontSize(14);
    this->overlayYTitleActor->GetTextProperty()->SetOrientation(90.);
    this->overlayYTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->overlayYTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->ensureOverlayTickActors(ren);
    this->wcsOverlayInitialized = true;
    this->setWcsOverlayVisible(this->showWcsAxes);
    win->AddObserver(vtkCommand::EndEvent, this, &vtkWindowImage::updateWcsOverlay);
    win->AddObserver(vtkCommand::EndEvent, this, &vtkWindowImage::updateCatalogueOverlayLabels);

    // Loading placeholder — visible until first image data arrives.
    {
        auto *prop = this->m_loadingActor->GetTextProperty();
        prop->SetFontSize(16);
        double r, g, b;
        ThemeManager::instance().vtkTextColor(r, g, b);
        prop->SetColor(r, g, b);
        prop->SetJustificationToCentered();
        prop->SetVerticalJustificationToCentered();
        prop->BoldOff();
        prop->ShadowOff();
        this->m_loadingActor->SetInput("Loading image\xe2\x80\xa6");
        this->m_loadingActor->GetPositionCoordinate()
                ->SetCoordinateSystemToNormalizedViewport();
        this->m_loadingActor->SetPosition(0.5, 0.5);
        this->m_loadingActor->VisibilityOn();
        ren->AddActor2D(this->m_loadingActor);

        // Track runtime theme changes so the loading text stays readable.
        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                         this, [this](EffectiveTheme) {
                             double tr, tg, tb;
                             ThemeManager::instance().vtkTextColor(tr, tg, tb);
                             this->m_loadingActor->GetTextProperty()->SetColor(tr, tg, tb);
                         });
    }

    ren->ResetCamera();
    win->Render();
}

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

void vtkWindowImage::finishRegionInteraction()
{
    if (this->m_annotPlaceMode != AnnotPlaceMode::None) {
        this->cancelAnnotationPlacement();
        return;
    }

    if (this->isBusy() || this->regionMode == RegionMode::None || !this->regionDragging) {
        return;
    }

    if (this->regionMode == RegionMode::Polygon) {
        if (this->ignoreNextPolygonRelease) {
            this->ignoreNextPolygonRelease = false;
            return;
        }
        if (this->finalizePolygonRegion()) {
            ui->vtk->renderWindow()->Render();
        }
        return;
    }

    const int *position = ui->vtk->renderWindow()->GetInteractor()->GetEventPosition();
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
            ui->vtk->renderWindow()->Render();
            return;
        }
        this->regionAnnulusInnerRadius = innerRadius;
        this->regionDragging = false;
        this->regionValid = true;
        this->refreshRegionOverlay();
        ui->vtk->renderWindow()->Render();
    }
    this->regionDragging = false;
    this->regionValid = true;
    this->refreshRegionOverlay();
    this->analyzeCurrentRegion();
    ui->vtk->renderWindow()->Render();
    // Note: the region action is intentionally NOT unchecked here.
    // The overlay stays visible while the result dialog is open, and is cleared
    // (along with the action check state) when the dialog is closed.
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

    if (this->probeValid && this->probeVoxel[0] == voxelX && this->probeVoxel[1] == voxelY) {
        return true;
    }

    this->probeValid = true;
    this->probeVoxel = { voxelX, voxelY };

    const float value = this->layers->getPixelValue(this->layers->getMasterIndex(), voxelX, voxelY);
    QString valueText = std::isfinite(value) ? QString::number(value, 'g', 8) : u"NaN"_s;
    QString message = u"X=%1  Y=%2  Value=%3"_s.arg(voxelX).arg(voxelY).arg(valueText);
    {
        QString axis0 = this->remoteFormatAxisCoordinate(0, voxelX);
        QString axis1 = this->remoteFormatAxisCoordinate(1, voxelY);
        if (this->remoteHasCelestialAxes()) {
            bool ok0 = false;
            bool ok1 = false;
            const double nativeX = this->remoteVoxelToWcs(0, voxelX, &ok0);
            const double nativeY = this->remoteVoxelToWcs(1, voxelY, &ok1);
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
    this->probeHorizontalLine->SetPoint1(bounds[0], static_cast<double>(this->probeVoxel[1]), 0.0);
    this->probeHorizontalLine->SetPoint2(bounds[1], static_cast<double>(this->probeVoxel[1]), 0.0);
    this->probeVerticalLine->SetPoint1(static_cast<double>(this->probeVoxel[0]), bounds[2], 0.0);
    this->probeVerticalLine->SetPoint2(static_cast<double>(this->probeVoxel[0]), bounds[3], 0.0);
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

void vtkWindowImage::setRegionMode(RegionMode mode, bool active)
{
    if (!active) {
        if (this->regionMode == mode) {
            this->regionMode = RegionMode::None;
            this->regionDragging = false;
            this->clearRegion();
            this->setInteractorStyleImage();
            ui->vtk->setCursor(this->probeModeActive ? Qt::CrossCursor : Qt::ArrowCursor);
            if (ui->vtk->renderWindow()) {
                ui->vtk->renderWindow()->Render();
            }
        }
        return;
    }

    if (this->probeModeActive && ui->actionProfile->isChecked()) {
        ui->actionProfile->setChecked(false);
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
    ui->vtk->setCursor(Qt::CrossCursor);
}

bool vtkWindowImage::updateRegionFromDisplayPosition(int displayX, int displayY)
{
    auto *renderer = ui->vtk->renderWindow() ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                                             : nullptr;
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
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

    this->regionCurrentVoxel = { voxelX, voxelY };
    this->refreshRegionOverlay();
    return true;
}

void vtkWindowImage::refreshRegionOverlay()
{
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!imageData || this->regionMode == RegionMode::None || !this->regionValid) {
        this->clearRegion();
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
    this->regionTopActor->SetVisibility(showBox);
    this->regionBottomActor->SetVisibility(showBox);
    this->regionLeftActor->SetVisibility(showBox);
    this->regionRightActor->SetVisibility(showBox);
    this->regionCircleActor->SetVisibility(showCircle);
    this->regionAnnulusOuterActor->SetVisibility(showAnnulus);
    this->regionAnnulusInnerActor->SetVisibility(showAnnulus && !this->regionDragging);
    this->regionAnnulusFillActor->SetVisibility(showAnnulus && !this->regionDragging);
    this->regionPolygonActor->SetVisibility(showPolygon);
    this->regionPolygonFillActor->SetVisibility(showPolygon);

    if (showBox) {
        this->regionTopLine->SetPoint1(xmin, ymax, 0.0);
        this->regionTopLine->SetPoint2(xmax, ymax, 0.0);
        this->regionBottomLine->SetPoint1(xmin, ymin, 0.0);
        this->regionBottomLine->SetPoint2(xmax, ymin, 0.0);
        this->regionLeftLine->SetPoint1(xmin, ymin, 0.0);
        this->regionLeftLine->SetPoint2(xmin, ymax, 0.0);
        this->regionRightLine->SetPoint1(xmax, ymin, 0.0);
        this->regionRightLine->SetPoint2(xmax, ymax, 0.0);
    } else if (showCircle || showAnnulus) {
        const double dx = static_cast<double>(this->regionCurrentVoxel[0] - this->regionAnchorVoxel[0]);
        const double dy = static_cast<double>(this->regionCurrentVoxel[1] - this->regionAnchorVoxel[1]);
        const double outerRadius = std::sqrt(dx * dx + dy * dy);
        if (showCircle) {
            this->regionCircleSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
            this->regionCircleSource->SetRadius(outerRadius);
        } else {
            this->regionAnnulusOuterSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
            this->regionAnnulusOuterSource->SetRadius(outerRadius);
            if (!this->regionDragging) {
                this->regionAnnulusInnerSource->SetCenter(this->regionAnchorVoxel[0], this->regionAnchorVoxel[1], 0.0);
                const double innerRadius = std::max(0.0, std::min(this->regionAnnulusInnerRadius, outerRadius));
                this->regionAnnulusInnerSource->SetRadius(innerRadius);
                buildAnnulusFill(this->regionAnnulusFillPoints, this->regionAnnulusFillCells,
                                 this->regionAnnulusFillData, this->regionAnchorVoxel, innerRadius,
                                 outerRadius, 96);
                this->regionAnnulusFillActor->SetVisibility(innerRadius < outerRadius);
            } else {
                this->regionAnnulusFillData->Initialize();
                this->regionAnnulusInnerActor->SetVisibility(false);
                this->regionAnnulusFillActor->SetVisibility(false);
            }
        }
    } else if (showPolygon) {
        std::vector<std::array<int, 2>> drawVertices = this->regionPolygonVertices;
        if (this->regionDragging && (drawVertices.empty() || drawVertices.back() != this->regionCurrentVoxel)) {
            drawVertices.push_back(this->regionCurrentVoxel);
        }
        const bool previewClosure = this->regionDragging && drawVertices.size() >= 3
                && distance2d(this->regionCurrentVoxel, drawVertices.front()) <= polygonClosureTolerance;
        this->regionPolygonPoints->Reset();
        this->regionPolygonCells->Reset();
        this->regionPolygonFillData->Initialize();
        if (drawVertices.size() >= 2) {
            const bool closed = !this->regionDragging || previewClosure;
            const vtkIdType count = static_cast<vtkIdType>(drawVertices.size() + (closed ? 1 : 0));
            this->regionPolygonCells->InsertNextCell(count);
            for (vtkIdType i = 0; i < static_cast<vtkIdType>(drawVertices.size()); ++i) {
                this->regionPolygonPoints->InsertNextPoint(drawVertices[static_cast<std::size_t>(i)][0],
                                                           drawVertices[static_cast<std::size_t>(i)][1], 0.0);
                this->regionPolygonCells->InsertCellPoint(i);
            }
            if (closed) {
                this->regionPolygonPoints->InsertNextPoint(drawVertices.front()[0], drawVertices.front()[1], 0.0);
                this->regionPolygonCells->InsertCellPoint(static_cast<vtkIdType>(drawVertices.size()));
            }
            this->regionPolygonData->SetPoints(this->regionPolygonPoints);
            this->regionPolygonData->SetLines(this->regionPolygonCells);
            this->regionPolygonData->Modified();
            if (drawVertices.size() >= 3 && closed) {
                this->regionPolygonFillData->SetPoints(this->regionPolygonPoints);
                this->regionPolygonFillData->SetLines(this->regionPolygonCells);
                this->regionPolygonFillData->Modified();
                this->regionPolygonTriangulator->Update();
                this->regionPolygonFillActor->SetVisibility(true);
            } else {
                this->regionPolygonFillActor->SetVisibility(false);
            }
        } else {
            this->regionPolygonFillActor->SetVisibility(false);
        }
    } else {
        this->regionAnnulusFillActor->SetVisibility(false);
        this->regionPolygonFillActor->SetVisibility(false);
    }
}

void vtkWindowImage::clearRegion()
{
    this->regionValid = false;
    this->regionDragging = false;
    this->ignoreNextPolygonRelease = false;
    this->regionPolygonVertices.clear();
    this->regionAnnulusInnerRadius = 0.0;
    this->regionTopActor->VisibilityOff();
    this->regionBottomActor->VisibilityOff();
    this->regionLeftActor->VisibilityOff();
    this->regionRightActor->VisibilityOff();
    this->regionCircleActor->VisibilityOff();
    this->regionAnnulusOuterActor->VisibilityOff();
    this->regionAnnulusInnerActor->VisibilityOff();
    this->regionAnnulusFillActor->VisibilityOff();
    this->regionPolygonActor->VisibilityOff();
    this->regionPolygonFillActor->VisibilityOff();
}

void vtkWindowImage::analyzeCurrentRegion()
{
    if (this->regionMode == RegionMode::None || !this->regionValid || !this->layers) {
        return;
    }

    // Close any previous result dialog (its stale generation prevents cleanup races)
    if (this->m_regionResultDialog) {
        this->m_regionResultDialog->close();
    }

    auto *imageData = this->layers->getImageData(this->layers->getMasterIndex());
    const auto stats = computeRegionStatistics2D(imageData, this->regionMode, this->regionAnchorVoxel,
                                                 this->regionCurrentVoxel, this->regionPolygonVertices,
                                                 this->regionAnnulusInnerRadius);

    const int myGeneration = ++this->m_regionDialogGeneration;
    const RegionMode capturedMode = this->regionMode;

    auto *dlg = new ToolResultDialog(this);
    this->m_regionResultDialog = dlg;

    dlg->setTitle(u"Region Analysis"_s);
    dlg->setSubtitle(
            QStringLiteral("%1  ·  computed on the currently loaded data block")
                    .arg(regionModeLabel(capturedMode)));

    if (!stats.valid) {
        dlg->addSection(u"Statistics"_s,
                        { { u"Result"_s, u"No valid pixels in the selected region."_s },
                          { u"Note"_s,   u"Blanked / NaN pixels are ignored."_s } });
    } else {
        dlg->addSection(u"Statistics"_s,
                        { { u"Valid pixels"_s,
                            QStringLiteral("%1 / %2").arg(stats.validCount).arg(stats.totalCount) },
                          { u"Blanked / NaN"_s, QString::number(stats.blankedCount) },
                          { u"Min"_s,    QStringLiteral("%1").arg(stats.minValue, 0, 'g', 8) },
                          { u"Max"_s,    QStringLiteral("%1").arg(stats.maxValue, 0, 'g', 8) },
                          { u"Mean"_s,   QStringLiteral("%1").arg(stats.mean,     0, 'g', 8) },
                          { u"Median"_s, QStringLiteral("%1").arg(stats.median,   0, 'g', 8) },
                          { u"Std Dev"_s,QStringLiteral("%1").arg(stats.stddev,   0, 'g', 8) },
                          { u"σ (MAD)"_s,QStringLiteral("%1").arg(stats.madSigma, 0, 'g', 8) } });

        // ── Radio-specific: integrated flux if beam known ────────────────
        if (this->hasBeamInfo()) {
            const double Omega_beam_px = this->beamAreaPixels();
            if (Omega_beam_px > 0.0) {
                const double fluxJy = stats.sum / Omega_beam_px;
                const double snrPeak = (stats.madSigma > 0.0)
                        ? (stats.maxValue / stats.madSigma) : 0.0;
                dlg->addSection(u"Radio (beam-aware)"_s,
                        { { u"Beam (BMAJ × BMIN)"_s,
                            QStringLiteral("%1\" × %2\"")
                                    .arg(this->m_beamMajorDeg * 3600.0, 0, 'f', 2)
                                    .arg(this->m_beamMinorDeg * 3600.0, 0, 'f', 2) },
                          { u"Beam area (pixels)"_s,
                            QStringLiteral("%1").arg(Omega_beam_px, 0, 'f', 2) },
                          { u"Integrated flux"_s,
                            QStringLiteral("%1 Jy").arg(fluxJy, 0, 'g', 6) },
                          { u"Peak SNR (peak/σ_MAD)"_s,
                            QStringLiteral("%1").arg(snrPeak, 0, 'f', 1) } });
            }
        }

        // ── Per-Stokes flux when a Stokes set is loaded ──────────────────
        if (!m_stokesRoles.isEmpty()) {
            QList<QPair<QString, QString>> rows;
            const StokesRole order[] = {
                StokesRole::I, StokesRole::Q, StokesRole::U, StokesRole::V,
                StokesRole::DerivedP, StokesRole::DerivedPdb,
                StokesRole::DerivedPA, StokesRole::DerivedFrac,
                StokesRole::DerivedAlpha, StokesRole::DerivedRM };
            const double Omega = this->hasBeamInfo() ? this->beamAreaPixels() : 0.0;
            for (StokesRole role : order) {
                const int idx = this->findLayerByStokesRole(role);
                if (idx < 0) continue;
                auto *roleData = this->layers->getImageData(idx);
                if (!roleData || roleData == imageData) continue;  // skip if same as master
                const auto roleStats = computeRegionStatistics2D(
                        roleData, this->regionMode, this->regionAnchorVoxel,
                        this->regionCurrentVoxel, this->regionPolygonVertices,
                        this->regionAnnulusInnerRadius);
                if (!roleStats.valid) continue;
                const QString roleName = this->stokesRoleLabel(role);
                if (role == StokesRole::DerivedPA) {
                    rows.append({ roleName + u" (mean)"_s,
                            QStringLiteral("%1°").arg(roleStats.mean, 0, 'f', 2) });
                } else if (role == StokesRole::DerivedFrac) {
                    rows.append({ roleName + u" (mean)"_s,
                            QStringLiteral("%1 %").arg(roleStats.mean * 100.0, 0, 'f', 2) });
                } else if (role == StokesRole::DerivedAlpha) {
                    rows.append({ roleName + u" (mean)"_s,
                            QStringLiteral("%1").arg(roleStats.mean, 0, 'f', 2) });
                } else if (role == StokesRole::DerivedRM) {
                    rows.append({ roleName + u" (mean)"_s,
                            QStringLiteral("%1 rad m⁻²").arg(roleStats.mean, 0, 'f', 1) });
                } else if (Omega > 0.0) {
                    rows.append({ roleName,
                            QStringLiteral("%1 Jy").arg(roleStats.sum / Omega, 0, 'g', 6) });
                } else {
                    rows.append({ roleName + u" (sum)"_s,
                            QStringLiteral("%1").arg(roleStats.sum, 0, 'g', 6) });
                }
            }
            if (!rows.isEmpty()) {
                dlg->addSection(u"Stokes (per-layer)"_s, rows);
            }
        }

        dlg->addSection(u"Note"_s,
                        { { QString(), u"Computed on valid pixels only."_s } });
    }

    // When the dialog is closed, clear the overlay and deactivate the region tool.
    // The generation guard prevents the cleanup from running if a newer analysis has
    // already replaced this dialog (e.g., the user drew a second region while this
    // dialog was still open).
    QObject::connect(dlg, &QObject::destroyed, this, [this, myGeneration, capturedMode]() {
        if (this->m_regionDialogGeneration != myGeneration) {
            return; // stale — a newer analysis supersedes this one
        }
        this->clearRegion();
        // Deactivate the region action; setRegionMode() will restore the image interactor.
        QAction *regionAction = nullptr;
        switch (capturedMode) {
        case RegionMode::Box:     regionAction = this->actionBoxRegion;     break;
        case RegionMode::Circle:  regionAction = this->actionCircleRegion;  break;
        case RegionMode::Polygon: regionAction = this->actionPolygonRegion; break;
        case RegionMode::Annulus: regionAction = this->actionAnnulusRegion; break;
        default: break;
        }
        if (regionAction && regionAction->isChecked()) {
            regionAction->setChecked(false); // triggers setRegionMode(mode, false)
        } else {
            // Action was already unchecked; at minimum restore the interactor and render.
            this->setInteractorStyleImage();
            if (ui && ui->vtk && ui->vtk->renderWindow()) {
                ui->vtk->renderWindow()->Render();
            }
        }
    });

    dlg->show();
}

bool vtkWindowImage::finalizePolygonRegion()
{
    const int *position = ui->vtk->renderWindow()->GetInteractor()->GetEventPosition();
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
    // Note: polygon action is NOT unchecked here — the overlay stays visible while
    // the result dialog is open and is cleared when the dialog closes.
    return true;
}

void vtkWindowImage::loadCatalogueOverlay()
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
                             u"No sources could be projected onto the current image WCS."_s);
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
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::clearCatalogueOverlay()
{
    this->catalogueOverlayLoaded = false;
    this->catalogueOverlayPixels.clear();
    this->catalogueOverlayEntries.clear();
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
    if (auto *renderer = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()) {
        for (const auto &actor : this->catalogueOverlayLabelActors) {
            renderer->RemoveViewProp(actor);
        }
    }
    this->catalogueOverlayLabelActors.clear();
    this->catalogueOverlayActor->VisibilityOff();
    this->catalogueHoverOverlayActor->VisibilityOff();
    this->catalogueSelectionOverlayActor->VisibilityOff();
    if (this->actionShowCatalogueOverlay) {
        const QSignalBlocker blocker(this->actionShowCatalogueOverlay);
        this->actionShowCatalogueOverlay->setChecked(false);
        this->actionShowCatalogueOverlay->setEnabled(false);
    }
    if (this->actionShowCatalogueLabels) {
        const QSignalBlocker blocker(this->actionShowCatalogueLabels);
        this->actionShowCatalogueLabels->setChecked(false);
        this->actionShowCatalogueLabels->setEnabled(false);
    }
    if (this->actionClearCatalogueOverlay) {
        this->actionClearCatalogueOverlay->setEnabled(false);
    }
    this->refreshCatalogueTable();
    this->updateCatalogueInfoPanel();
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::setCatalogueOverlayVisible(bool visible)
{
    const bool effectiveVisible = visible && this->catalogueOverlayLoaded;
    this->catalogueOverlayActor->SetVisibility(effectiveVisible ? 1 : 0);
    if (effectiveVisible) {
        this->updateCatalogueOverlayLabels();
    } else {
        for (const auto &actor : this->catalogueOverlayLabelActors) {
            actor->SetVisibility(0);
        }
        this->catalogueHoverOverlayActor->VisibilityOff();
        this->catalogueSelectionOverlayActor->VisibilityOff();
    }
    if (ui && ui->vtk && ui->vtk->renderWindow()) {
        ui->vtk->renderWindow()->Render();
    }
}

void vtkWindowImage::rebuildCatalogueOverlay()
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

    auto *renderer = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer();
    if (!renderer) {
        return;
    }
    for (const auto &actor : this->catalogueOverlayLabelActors) {
        renderer->RemoveViewProp(actor);
    }
    this->catalogueOverlayLabelActors.clear();
    for (int index : this->catalogueOverlayLabelIndices) {
        auto actor = vtkSmartPointer<vtkTextActor>::New();
        actor->SetInput(this->catalogueOverlayLabels.value(index).toUtf8().constData());
        actor->GetTextProperty()->SetColor(1.0, 0.65, 0.25);
        actor->GetTextProperty()->SetFontSize(12);
        actor->GetTextProperty()->SetBold(false);
        actor->GetTextProperty()->SetShadow(true);
        actor->SetVisibility(this->actionShowCatalogueOverlay && this->actionShowCatalogueOverlay->isChecked());
        renderer->AddViewProp(actor);
        this->catalogueOverlayLabelActors.push_back(actor);
    }
    this->rebuildCatalogueHighlightOverlay(this->catalogueHoverOverlayPoints,
                                           this->catalogueHoverOverlayCells,
                                           this->catalogueHoverOverlayData,
                                           this->hoveredCatalogueSourceIndex);
    this->rebuildCatalogueHighlightOverlay(this->catalogueSelectionOverlayPoints,
                                           this->catalogueSelectionOverlayCells,
                                           this->catalogueSelectionOverlayData,
                                           this->selectedCatalogueSourceIndex);
    this->catalogueHoverOverlayActor->SetVisibility(this->hoveredCatalogueSourceIndex >= 0 ? 1 : 0);
    this->catalogueSelectionOverlayActor->SetVisibility(this->selectedCatalogueSourceIndex >= 0 ? 1 : 0);
    this->updateCatalogueOverlayLabels();
}

void vtkWindowImage::updateCatalogueOverlayLabels()
{
    if (this->suspendHeavyOverlayUpdates) {
        return;
    }
    if (!this->catalogueOverlayLoaded || !this->actionShowCatalogueOverlay
        || !this->actionShowCatalogueOverlay->isChecked()) {
        for (const auto &actor : this->catalogueOverlayLabelActors) {
            actor->SetVisibility(0);
        }
        this->catalogueHoverOverlayActor->VisibilityOff();
        this->catalogueSelectionOverlayActor->VisibilityOff();
        return;
    }
    auto *renderer = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer();
    if (!renderer) {
        return;
    }

    const auto visible = computeVisibleImageBounds2D(
            renderer, this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr);
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
    const bool labelsEnabled = this->actionShowCatalogueLabels && this->actionShowCatalogueLabels->isChecked();
    const bool smartVisible = labelsEnabled && visibleSources <= smartCatalogueLabelMaxVisible
            && std::max(pixelsPerUnitX, pixelsPerUnitY) >= smartCatalogueLabelMinPixelsPerUnit;

    auto *interactor = ui->vtk->renderWindow()->GetInteractor();
    const int *eventPos = interactor ? interactor->GetEventPosition() : nullptr;
    vtkNew<vtkCoordinate> coordinate;
    coordinate->SetCoordinateSystemToWorld();
    if (eventPos && this->hoveredCatalogueSourceIndex < 0) {
        double distancePx = 0.0;
        this->hoveredCatalogueSourceIndex =
                this->catalogueSourceIndexNearDisplayPosition(eventPos[0], eventPos[1], renderer, &distancePx);
    }
    const bool showHoveredLabel = this->hoveredCatalogueSourceIndex >= 0;

    for (std::size_t i = 0; i < this->catalogueOverlayLabelActors.size()
                             && i < this->catalogueOverlayLabelIndices.size();
         ++i) {
        const int pointIndex = this->catalogueOverlayLabelIndices[i];
        if (pointIndex < 0 || static_cast<std::size_t>(pointIndex) >= this->catalogueOverlayPixels.size()) {
            this->catalogueOverlayLabelActors[i]->SetVisibility(0);
            continue;
        }
        coordinate->SetValue(this->catalogueOverlayPixels[pointIndex][0],
                             this->catalogueOverlayPixels[pointIndex][1], 0.0);
        int *display = coordinate->GetComputedDisplayValue(renderer);
        if (!display) {
            this->catalogueOverlayLabelActors[i]->SetVisibility(0);
            continue;
        }
        this->catalogueOverlayLabelActors[i]->SetDisplayPosition(display[0] + 4, display[1] + 4);
        const bool showThis = smartVisible || (showHoveredLabel && pointIndex == this->hoveredCatalogueSourceIndex)
                || pointIndex == this->selectedCatalogueSourceIndex;
        this->catalogueOverlayLabelActors[i]->SetVisibility(showThis ? 1 : 0);
    }
    this->catalogueHoverOverlayActor->SetVisibility(
            this->hoveredCatalogueSourceIndex >= 0 && this->hoveredCatalogueSourceIndex != this->selectedCatalogueSourceIndex
                    ? 1
                    : 0);
    this->catalogueSelectionOverlayActor->SetVisibility(this->selectedCatalogueSourceIndex >= 0 ? 1 : 0);
}

void vtkWindowImage::refreshCatalogueOverlayInteraction(int displayX, int displayY, bool fromClick)
{
    if (!this->catalogueOverlayLoaded || !this->actionShowCatalogueOverlay
        || !this->actionShowCatalogueOverlay->isChecked()) {
        return;
    }

    auto *renderer = ui && ui->vtk && ui->vtk->renderWindow()
            ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    if (!renderer) {
        return;
    }

    double hoverDistance = 0.0;
    const int hoveredIndex =
            this->catalogueSourceIndexNearDisplayPosition(displayX, displayY, renderer, &hoverDistance);
    if (hoveredIndex != this->hoveredCatalogueSourceIndex) {
        qDebug().noquote() << QStringLiteral("[catalogue:image] hover index=%1 distance_px=%2")
                                      .arg(hoveredIndex)
                                      .arg(hoverDistance, 0, 'f', 2);
        this->hoveredCatalogueSourceIndex = hoveredIndex;
        this->rebuildCatalogueHighlightOverlay(this->catalogueHoverOverlayPoints,
                                               this->catalogueHoverOverlayCells,
                                               this->catalogueHoverOverlayData,
                                               this->hoveredCatalogueSourceIndex);
    }

    if (fromClick) {
        qDebug().noquote() << QStringLiteral("[catalogue:image] select index=%1").arg(hoveredIndex);
        this->setSelectedCatalogueSourceIndex(hoveredIndex);
    }

    this->updateCatalogueOverlayLabels();
}

int vtkWindowImage::catalogueSourceIndexNearDisplayPosition(int displayX, int displayY,
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

void vtkWindowImage::rebuildCatalogueHighlightOverlay(vtkPoints *points, vtkCellArray *cells, vtkPolyData *data,
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

void vtkWindowImage::updateCatalogueInfoPanel()
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

QString vtkWindowImage::catalogueSourceSummary(int sourceIndex) const
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

void vtkWindowImage::ensureCatalogueDock()
{
    if (this->catalogueDock) {
        return;
    }

    this->catalogueDock = new QDockWidget(u"Catalogue"_s, this);
    this->catalogueDock->setObjectName(u"CatalogueDockImage"_s);
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

    QObject::connect(this, &vtkWindowImage::catalogueSourceSelectionChanged, this,
                     &vtkWindowImage::syncCatalogueTableSelection);
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

void vtkWindowImage::refreshCatalogueTable()
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

void vtkWindowImage::setSelectedCatalogueSourceIndex(int index)
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
    if (ui && ui->vtk && ui->vtk->renderWindow()) {
        ui->vtk->renderWindow()->Render();
    }
    emit this->catalogueSourceSelectionChanged(index);
}

void vtkWindowImage::centerViewOnCatalogueSource(int index, double zoomFactor)
{
    if (index < 0 || static_cast<std::size_t>(index) >= this->catalogueOverlayPixels.size()) {
        return;
    }
    auto *renderer = ui && ui->vtk && ui->vtk->renderWindow()
            ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;
    auto *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!renderer || !camera) {
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
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::syncCatalogueTableSelection(int index)
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

bool vtkWindowImage::catalogueWorldToPixel(double raDeg, double decDeg, std::array<double, 2> &pixel) const
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
    // Use the full TAN (gnomonic) inverse projection instead of the linear
    // approximation that was here before.  The linear formula diverges from
    // the true sky position by ~1 arcsec per degree from the reference pixel,
    // which matters for fields wider than ~1 degree.
    tanSkyToPix(nativeX, nativeY, this->remoteDatasetCrpix, this->remoteDatasetCrval,
                this->remoteDatasetCdelt, pixel[0], pixel[1]);
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]);
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
        this->profileWidget->setupImagePlots(u"Pixel"_s, u"Value"_s);
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
    imageData->GetExtent(extent);
    if (this->probeVoxel[0] < extent[0] || this->probeVoxel[0] > extent[1] || this->probeVoxel[1] < extent[2]
        || this->probeVoxel[1] > extent[3]) {
        return;
    }

    const int width = extent[1] - extent[0] + 1;
    const int height = extent[3] - extent[2] + 1;
    QVector<double> keyX(width), valuesX(width), keyY(height), valuesY(height);
    for (int x = extent[0]; x <= extent[1]; ++x) {
        const int idx = x - extent[0];
        keyX[idx] = x;
        valuesX[idx] = this->layers->getPixelValue(this->layers->getMasterIndex(), x, this->probeVoxel[1]);
    }
    for (int y = extent[2]; y <= extent[3]; ++y) {
        const int idx = y - extent[2];
        keyY[idx] = y;
        valuesY[idx] = this->layers->getPixelValue(this->layers->getMasterIndex(), this->probeVoxel[0], y);
    }
    this->profileWidget->updateImageProfiles(keyX, valuesX, keyY, valuesY, this->probeVoxel[0],
                                             this->probeVoxel[1], !this->probeFrozen);
}

void vtkWindowImage::setInteractorStyleImage()
{
    vtkNew<vtkInteractorStyleImage> style;
    ui->vtk->renderWindow()->GetInteractor()->SetInteractorStyle(style);
    this->vtkRender();
}

void vtkWindowImage::setInteractorStyleRegion()
{
    vtkNew<vtkInteractorStyleUser> style;
    ui->vtk->renderWindow()->GetInteractor()->SetInteractorStyle(style);
    this->vtkRender();
}

void vtkWindowImage::setInteractorStyleProfile()
{
    if (!this->profileWidget) {
        vtkNew<vtkInteractorStyleProfile> style;
        ui->vtk->renderWindow()->GetInteractor()->SetInteractorStyle(style);
        this->vtkRender();

        this->profileWidget =
                new ProfileWidget(style, this->layers->getImageData(this->layers->getMasterIndex()),
                                  this->filepath.toStdString(), this);
        this->profileWidget->setUsageMode(ProfileWidget::UsageMode::ProbeLive, u"Profile"_s);
        this->profileWidget->setupImagePlots();
        QObject::connect(this->profileWidget, &ProfileWidget::destroyed, this,
                         &vtkWindowImage::setInteractorStyleImage, Qt::QueuedConnection);
    }
}

int vtkWindowImage::currentLayerIndex() const
{
    return ui->listLayer->currentIndex().row();
}

void vtkWindowImage::addLayerImage(const std::string &filepath)
{
    this->setLayerImportEnabled(false);
    this->showPersistentStatusMessage(u"Loading layer..."_s);
    this->layerLoadWatcher.setFuture(QtConcurrent::run(
            &loadImageLayer, ImageLayerLoadRequest { this->filepath.toStdString(), filepath }));
}

void vtkWindowImage::applyLoadedLayer(const ImageLayerLoadResult &result)
{
    QElapsedTimer timer;
    timer.start();
    this->stack->AddImage(this->layers->addLayer(result));
    const int masterIndex = this->layers->getMasterIndex();
    const int newLayerIndex = this->layers->rowCount() - 1;

    if (newLayerIndex > masterIndex) {
        // Additional layer: autoscale it independently; leave master untouched
        // so the user's existing display settings are preserved.
        //
        // NaN transparency: resampled VLKB layers fill out-of-bounds pixels with
        // NaN.  If NanColor were white/opaque (the default), those regions would
        // render as a white haze over the master image.  Setting alpha = 0 makes
        // them fully transparent so the master shows through unaffected.
        if (auto *newLut = this->layers->getLookupTable(newLayerIndex)) {
            newLut->SetNanColor(0., 0., 0., 0.);
        }
        this->applyInitialAutoscale(this->layers->getImageData(newLayerIndex),
                                    this->layers->getLookupTable(newLayerIndex),
                                    "vlkb additional layer");
        // Ensure savedLinearDisplayRanges covers all layers.
        if (static_cast<int>(this->savedLinearDisplayRanges.size()) <= newLayerIndex) {
            this->savedLinearDisplayRanges.resize(static_cast<std::size_t>(newLayerIndex + 1),
                                                  { std::numeric_limits<double>::quiet_NaN(),
                                                    std::numeric_limits<double>::quiet_NaN() });
        }
    } else {
        this->applyInitialAutoscale(this->layers->getImageData(masterIndex),
                                    this->layers->getLookupTable(masterIndex), "local apply");
    }
    this->displayStateInitialized = true;
    this->updateLargeImageMode(this->layers->getImageData(masterIndex), "local apply");
    if (this->probeValid) {
        this->refreshProbeOverlay();
    }
    this->updateDataStatePanel();
    this->updateSanityPanel();
    this->vtkRender();
    qDebug().noquote()
            << QStringLiteral("[perf][layer] render after apply: %1 ms").arg(timer.elapsed());
}

void vtkWindowImage::applyInitialAutoscale(vtkImageData *imageData, vtkLookupTable *lut,
                                           const char *context)
{
    if (!imageData || !lut) {
        return;
    }

    int extent[6];
    imageData->GetExtent(extent);
    const int width = extent[1] - extent[0] + 1;
    const int height = extent[3] - extent[2] + 1;
    if (width <= 0 || height <= 0) {
        return;
    }

    const qint64 pixels = static_cast<qint64>(width) * static_cast<qint64>(height);
    const int stride =
            std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(pixels)
                                                             / initialAutoscaleTargetSamples))));

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(
            std::min<qint64>(pixels, static_cast<qint64>(initialAutoscaleTargetSamples))));
    for (int y = extent[2]; y <= extent[3]; y += stride) {
        for (int x = extent[0]; x <= extent[1]; x += stride) {
            const double value = imageData->GetScalarComponentAsDouble(x, y, 0, 0);
            if (std::isfinite(value)) {
                samples.push_back(value);
            }
        }
    }

    if (samples.empty()) {
        return;
    }

    std::sort(samples.begin(), samples.end());
    double low = percentileFromSorted(samples, initialAutoscaleLowPercent);
    double high = percentileFromSorted(samples, initialAutoscaleHighPercent);

    const double *rawRange = imageData->GetScalarRange();
    const double rawMin = rawRange ? rawRange[0] : low;
    const double rawMax = rawRange ? rawRange[1] : high;
    if (!std::isfinite(low) || !std::isfinite(high) || low >= high) {
        low = rawMin;
        high = rawMax;
    }
    if (!std::isfinite(low) || !std::isfinite(high) || low >= high) {
        return;
    }

    lut->SetTableRange(low, high);
    lut->Build();
    qDebug().noquote()
            << QStringLiteral("[image-autoscale] %1 samples=%2 stride=%3 percentiles=%4..%5 range=%6..%7 raw=%8..%9")
                       .arg(QString::fromUtf8(context))
                       .arg(samples.size())
                       .arg(stride)
                       .arg(initialAutoscaleLowPercent, 0, 'f', 1)
                       .arg(initialAutoscaleHighPercent, 0, 'f', 1)
                       .arg(low, 0, 'g', 12)
                       .arg(high, 0, 'g', 12)
                       .arg(rawMin, 0, 'g', 12)
                       .arg(rawMax, 0, 'g', 12);
}

void vtkWindowImage::updateLargeImageMode(vtkImageData *imageData, const char *context)
{
    const qint64 pixels = imagePixelCount(imageData);
    const bool large = pixels >= largeImagePixelThreshold;
    this->largeImageModeActive = large;
    if (!imageData) {
        return;
    }
    int extent[6];
    imageData->GetExtent(extent);
    const qint64 width = std::max(0, extent[1] - extent[0] + 1);
    const qint64 height = std::max(0, extent[3] - extent[2] + 1);
    const double estimatedMb = static_cast<double>(pixels) * sizeof(float) / (1024.0 * 1024.0);
    qDebug().noquote()
            << QStringLiteral("[image] %1 dims=%2x%3 pixels=%4 est_float_mb=%5 large_mode=%6")
                       .arg(QString::fromUtf8(context))
                       .arg(width)
                       .arg(height)
                       .arg(pixels)
                       .arg(estimatedMb, 0, 'f', 1)
                       .arg(large);
}

void vtkWindowImage::applyRemoteMasterLayer(const ImageLayerLoadResult &result)
{
    auto *newActor = this->layers->updateMasterLayer(result);
    if (!newActor) {
        return;
    }

    this->stack->SetActiveLayer(this->layers->getMasterIndex());
    this->layerController->activateLayer(this->layers->getMasterIndex());
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    auto *lut = this->layers ? this->layers->getLookupTable(this->layers->getMasterIndex()) : nullptr;
    // Radio convention: render NaN pixels as transparent so the mosaic
    // footprint doesn't show as a bright white border around the data.
    if (lut) {
        lut->SetNanColor(0., 0., 0., 0.);
    }
    this->remoteDisplayingPreview = result.isPreview;
    this->remoteFullImageDims = { result.fullWidth, result.fullHeight };
    if (!this->displayStateInitialized && !this->userAdjustedDisplayState) {
        this->applyInitialAutoscale(imageData, lut, result.isPreview ? "remote preview" : "remote full");
        this->displayStateInitialized = true;
    } else if (lut) {
        lut->Build();
    }
    this->updateLargeImageMode(imageData, "remote apply");
    auto *renderWindow = ui && ui->vtk ? ui->vtk->renderWindow() : nullptr;
    auto *renderer = renderWindow ? renderWindow->GetRenderers()->GetFirstRenderer() : nullptr;
    const bool validExtent = isImageExtentUsable(imageData);
    qDebug().noquote()
            << QStringLiteral("[remote-image] apply extents x=%1..%2 y=%3..%4 valid=%5")
                       .arg(imageData ? imageData->GetExtent()[0] : -1)
                       .arg(imageData ? imageData->GetExtent()[1] : -1)
                       .arg(imageData ? imageData->GetExtent()[2] : -1)
                       .arg(imageData ? imageData->GetExtent()[3] : -1)
                       .arg(validExtent);
    if (renderer) {
        renderer->ResetCamera();
        renderer->ResetCameraClippingRange();
    }
    this->invalidateWcsOverlayCache();
    if (!validExtent) {
        hideCustomWcsOverlay(this->overlayXAxis, this->overlayYAxis, this->overlayXTitleActor,
                             this->overlayYTitleActor, this->overlayXTickActors,
                             this->overlayYTickActors);
    }
    if (this->probeValid) {
        this->refreshProbeOverlay();
    }
    this->m_loadingActor->VisibilityOff();

    // ── Defer heavy work by one event-loop tick ─────────────────────────
    // The synchronous parts above (data swap, NaN-color, autoscale on a
    // strided sample) are cheap. The expensive parts are:
    //   - setupContourPipeline() — calls vtkImageData::GetScalarRange() over
    //     the full image (O(N) on tens of millions of pixels)
    //   - updateLUTCustomizer() — re-inits the LUT editor with the new data
    //   - renderWindow->Render() — GPU texture upload of the full-res image
    //   - requestWcsOverlayRender() — recomputes the WCS axis ticks
    // For a 7500×7500 image the GPU upload alone can take 200–500 ms and
    // freezes the UI thread the same way the cube viewer's volume upload
    // used to. Mirror the cube viewer fix (applyCubeOpenResult): pump
    // pending paint / status events first, then let the upload happen.
    QPointer<vtkWindowImage> self(this);
    QTimer::singleShot(50, this, [self]() {
        if (!self) return;
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        QElapsedTimer renderTimer;
        renderTimer.start();
        // Reconnect the contour filter to the new real image data (the
        // initial call at construction time ran on a placeholder with no
        // pixels). Rebuilt only when contours are already visible — in the
        // common case (contours off during preview→full swap) we just mark
        // the pipeline dirty so setContoursVisible(true) rebuilds lazily.
        // This avoids a 10–20 s vtkFlyingEdges2D pass on multi-megapixel
        // images while the user never even asked for contours.
        if (self->m_contoursVisible) {
            self->setupContourPipeline();
        } else {
            self->m_contourPipelineDirty = true;
        }
        self->updateLUTCustomizer();
        if (self->ui && self->ui->vtk && self->ui->vtk->renderWindow()) {
            self->ui->vtk->renderWindow()->Render();
        }
        self->requestWcsOverlayRender();
        // Status-bar panels — computeBlankFraction is now stride-sampled
        // (~250k samples), so updateSanityPanel is cheap even on huge
        // images, but we still keep it here so the GPU upload above gets
        // first priority.
        self->updateDataStatePanel();
        self->updateSanityPanel();
        qDebug().noquote()
                << QStringLiteral("[perf][image] deferred apply (GPU upload): %1 ms")
                           .arg(renderTimer.elapsed());
    });
    qDebug().noquote()
            << QStringLiteral("[remote-image] applied mode=%1 display=%2x%3 full=%4x%5")
                       .arg(result.isPreview ? QStringLiteral("preview") : QStringLiteral("full"))
                       .arg(imageData ? imageData->GetDimensions()[0] : 0)
                       .arg(imageData ? imageData->GetDimensions()[1] : 0)
                       .arg(result.fullWidth)
                       .arg(result.fullHeight);
}

void vtkWindowImage::startRemoteFullResolutionLoad()
{
    if (this->remoteFullImageWatcher.isRunning()) {
        return;
    }
    this->transitioningToFull = true;
    this->suspendLutEditorUpdates = true;
    qDebug().noquote()
            << QStringLiteral("[remote-image] requesting full-resolution image dataset=%1")
                       .arg(this->remoteDatasetId);
    this->remoteFullImageWatcher.setFuture(
            QtConcurrent::run(&fetchRemoteImageLayer, this->remoteBackendUrl, this->remoteDatasetId,
                              this->filepath, false, 1536, this->remoteLoadGeneration,
                              this->remoteSessionId, this->remoteBackendToken));
}

bool vtkWindowImage::isBusy() const
{
    return this->layerLoadWatcher.isRunning() || this->remoteImageWatcher.isRunning()
            || this->remoteFullImageWatcher.isRunning();
}

void vtkWindowImage::showPersistentStatusMessage(const QString &text, int minDurationMs)
{
    this->statusMessageClearTimer.stop();
    this->persistentStatusActive = true;
    this->statusMessageMinDurationMs = minDurationMs;
    this->statusMessageElapsed.restart();
    this->statusBar()->showMessage(text);
}

void vtkWindowImage::clearPersistentStatusMessage()
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

void vtkWindowImage::setLayerImportEnabled(bool enabled)
{
    // Layer stacking via local FITS files is a visualization-only feature that
    // reads files directly on the client desktop for rendering purposes only.
    // It is not an authoritative data operation and is kept as an adapter.
    ui->actionAddFITS->setEnabled(enabled);
}

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

void vtkWindowImage::updateDataStatePanel()
{
    if (!this->dataStateLabel) {
        return;
    }

    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    const QString representation = this->remoteDisplayingPreview ? u"Preview image"_s : u"Full image"_s;
    const QString loadedBounds = formatImageBoundsSummary(imageData);
    const QString datasetBounds = (this->remoteFullImageDims[0] > 0 && this->remoteFullImageDims[1] > 0)
            ? u"x=0..%1 y=0..%2"_s.arg(this->remoteFullImageDims[0] - 1).arg(this->remoteFullImageDims[1] - 1)
            : loadedBounds;
    const QString axis3 = u"Axis3: n/a"_s;
    const QString degenerateSummary = this->remoteDegenerateAxesSummary;
    this->dataStateLabel->setText(
            u"%1 | Loaded: %2 | Dataset: %3 | WCS: %4 | %5%6"_s.arg(representation,
                                                                     loadedBounds,
                                                                     datasetBounds,
                                                                     this->currentWcsFrameLabel(),
                                                                     axis3,
                                                                     degenerateSummary.isEmpty()
                                                                             ? QString()
                                                                             : u" | Collapsed axes"_s));
    this->dataStateLabel->setToolTip(
            degenerateSummary.isEmpty()
                    ? u"Persistent data state: origin, representation, loaded bounds, dataset bounds, current WCS frame."_s
                    : u"Persistent data state: origin, representation, loaded bounds, dataset bounds, current WCS frame.\n%1"_s
                              .arg(degenerateSummary));
}

void vtkWindowImage::updateWcsStatusIndicator()
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
            ? (degraded ? u"Remote dataset opened with degraded WCS metadata."_s
                        : u"Remote dataset opened with sanitized WCS metadata."_s)
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
                DiagnosticsCategory::WCS, QStringLiteral("vtkWindowImage"), message,
                this->remoteDatasetId, this->remoteSessionId, QStringLiteral("dataset-open"));
        this->wcsDiagnosticPublished = true;
    }
}

void vtkWindowImage::updateSanityPanel()
{
    if (!this->sanityLabel) {
        return;
    }

    // In dynamic-spectrum mode, the celestial-WCS checks would always
    // complain ("RA/Dec missing") because the axes are time/frequency.
    // setDynamicSpectrumMode() already pinned a fixed label; don't
    // overwrite it.
    if (m_dynamicSpectrumMode) {
        return;
    }

    std::array<QString, 3> ctype = this->remoteDatasetCtype;
    std::array<QString, 3> cunit = this->remoteDatasetCunit;
    std::array<double, 3> crval = this->remoteDatasetCrval;
    std::array<double, 3> crpix = this->remoteDatasetCrpix;
    std::array<double, 3> cdelt = this->remoteDatasetCdelt;
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    const auto report = buildImageSanityReport(ctype, cunit,
                                               crval, crpix, cdelt, imageData);
    this->sanityLabel->setText(report.summary);
    this->sanityLabel->setToolTip(report.details);
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
    return QString::number(value, 'f', 2);
}

QString vtkWindowImage::selectedFrameAxisTitle(int axis) const
{
    return this->remoteOverlayAxisTitle(axis);
}

void vtkWindowImage::vtkRender()
{
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::changeLegendWCS()
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
    this->refreshWcsOverlayImmediately();
}

void vtkWindowImage::changeCurrentColorMap()
{
    qDebug().noquote() << QStringLiteral("[image] color map change index=%1 map=%2")
                                  .arg(this->currentLayerIndex())
                                  .arg(ui->comboLut->currentText());
    this->layerController->setCurrentColorMap(this->currentLayerIndex(),
                                              ui->comboLut->currentText().toStdString());
    this->markDisplayStateAdjusted("color map");
    this->vtkRender();
}

void vtkWindowImage::changeCurrentColorScale()
{
    const int index = this->currentLayerIndex();
    const bool requestedLog = ui && ui->radioLog && ui->radioLog->isChecked();
    qDebug().noquote() << QStringLiteral("[image] color scale request index=%1 log=%2 large_mode=%3")
                                  .arg(index)
                                  .arg(requestedLog)
                                  .arg(this->largeImageModeActive);

    if (!this->layers || !this->layerController || index < 0 || index >= this->layers->rowCount()) {
        qWarning().noquote()
                << QStringLiteral("[image] color scale rejected invalid state index=%1 rows=%2 layers=%3 controller=%4")
                           .arg(index)
                           .arg(this->layers ? this->layers->rowCount() : -1)
                           .arg(reinterpret_cast<quintptr>(this->layers), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->layerController.get()), 0, 16);
        return;
    }

    auto *imageData = this->layers->getImageData(index);
    auto *lut = this->layers->getLookupTable(index);
    if (!imageData || !lut) {
        qWarning().noquote()
                << QStringLiteral("[image] color scale rejected missing objects image=%1 lut=%2")
                           .arg(reinterpret_cast<quintptr>(imageData), 0, 16)
                           .arg(reinterpret_cast<quintptr>(lut), 0, 16);
        return;
    }

    const double *lutRange = lut->GetTableRange();
    const double rangeMin = lutRange ? lutRange[0] : 0.0;
    const double rangeMax = lutRange ? lutRange[1] : 0.0;
    if (static_cast<int>(this->savedLinearDisplayRanges.size()) <= index) {
        this->savedLinearDisplayRanges.resize(static_cast<std::size_t>(index + 1),
                                              { std::numeric_limits<double>::quiet_NaN(),
                                                std::numeric_limits<double>::quiet_NaN() });
    }
    qDebug().noquote()
            << QStringLiteral("[image] color scale state index=%1 range=%2..%3")
                       .arg(index)
                       .arg(rangeMin, 0, 'g', 12)
                       .arg(rangeMax, 0, 'g', 12);

    if (requestedLog) {
        if (!lut->UsingLogScale()) {
            this->savedLinearDisplayRanges[static_cast<std::size_t>(index)] = { rangeMin, rangeMax };
        }
        const double positiveFloor = sampledPositiveFloor(imageData, initialAutoscaleTargetSamples);
        if (positiveFloor <= 0.0 || !std::isfinite(positiveFloor)) {
            qWarning().noquote()
                    << QStringLiteral("[image] log scale rejected index=%1 raw_range=%2..%3 no positive samples")
                               .arg(index)
                               .arg(rangeMin, 0, 'g', 12)
                               .arg(rangeMax, 0, 'g', 12);
            if (ui && ui->radioLinear) {
                const QSignalBlocker blockLog(ui->radioLog);
                const QSignalBlocker blockLinear(ui->radioLinear);
                ui->radioLinear->setChecked(true);
                ui->radioLog->setChecked(false);
            }
            this->statusBar()->showMessage(
                    u"Log scale requires at least some positive values in the visible range."_s, 3000);
            this->layerController->setCurrentLogScale(index, false);
            this->vtkRender();
            return;
        }

        const double adjustedMax =
                std::max({ rangeMax, positiveFloor * 10.0, positiveFloor * (1.0 + 1e-6) });
        lut->SetTableRange(positiveFloor, adjustedMax);
        lut->Build();
        qDebug().noquote()
                << QStringLiteral("[image] log scale applied index=%1 raw_range=%2..%3 positive_floor=%4 adjusted=%5..%6 clamped=%7")
                           .arg(index)
                           .arg(rangeMin, 0, 'g', 12)
                           .arg(rangeMax, 0, 'g', 12)
                           .arg(positiveFloor, 0, 'g', 12)
                           .arg(positiveFloor, 0, 'g', 12)
                           .arg(adjustedMax, 0, 'g', 12)
                           .arg(rangeMin <= 0.0);
    } else if (lut->UsingLogScale()) {
        const auto saved = this->savedLinearDisplayRanges[static_cast<std::size_t>(index)];
        if (std::isfinite(saved[0]) && std::isfinite(saved[1]) && saved[0] < saved[1]) {
            lut->SetTableRange(saved[0], saved[1]);
            lut->Build();
            qDebug().noquote()
                    << QStringLiteral("[image] linear scale restore index=%1 restored=%2..%3")
                               .arg(index)
                               .arg(saved[0], 0, 'g', 12)
                               .arg(saved[1], 0, 'g', 12);
        }
    }

    qDebug().noquote() << QStringLiteral("[image] color scale accepted index=%1 log=%2")
                                  .arg(index)
                                  .arg(requestedLog);
    this->layerController->setCurrentLogScale(index, requestedLog);
    this->markDisplayStateAdjusted(requestedLog ? "log scale" : "linear scale");
    this->vtkRender();
}

void vtkWindowImage::changeCurrentLayerOpacity()
{
    const double opacity = ui->sliderOpacity->sliderPosition() / 100.;
    qDebug().noquote() << QStringLiteral("[image] opacity change index=%1 opacity=%2")
                                  .arg(this->currentLayerIndex())
                                  .arg(opacity, 0, 'g', 6);
    this->layerController->setCurrentOpacity(this->currentLayerIndex(), opacity);
    this->markDisplayStateAdjusted("opacity");
    this->vtkRender();
}

void vtkWindowImage::showCurrentLayerSettings()
{
    int index = this->currentLayerIndex();

    // If nothing is selected but layers exist, auto-select the first one
    if (index < 0 && this->layers && this->layers->rowCount() > 0) {
        ui->listLayer->setCurrentIndex(this->layers->index(0, 0));
        return; // currentChanged signal will re-invoke this function
    }

    const bool hasLayers = this->layers && this->layers->rowCount() > 0;
    ui->frameLayerControls->setEnabled(hasLayers);
    if (!hasLayers) {
        return;
    }

    const auto state = this->layerController->layerViewState(index);
    if (!state.valid) {
        return;
    }

    const QSignalBlocker blockCombo(ui->comboLut);
    const QSignalBlocker blockLog(ui->radioLog);
    const QSignalBlocker blockLinear(ui->radioLinear);
    const QSignalBlocker blockOpacity(ui->sliderOpacity);
    ui->comboLut->setCurrentText(QString::fromStdString(state.colorMapName));
    if (state.usingLogScale) {
        ui->radioLog->setChecked(true);
    } else {
        ui->radioLinear->setChecked(true);
    }
    ui->sliderOpacity->setValue(state.opacityPercent);

    this->layerController->activateLayer(index);
    this->vtkRender();
}

void vtkWindowImage::markDisplayStateAdjusted(const char *reason)
{
    this->userAdjustedDisplayState = true;
    qDebug().noquote() << QStringLiteral("[image] display state adjusted reason=%1")
                                  .arg(QString::fromUtf8(reason ? reason : "unknown"));
}

// ── FITS header dialog (shown on primary tag click) ──────────────────────────

void vtkWindowImage::showFitsHeaderDialog()
{
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

// ── Layout B viewer toolbar ──────────────────────────────────────────────────

void vtkWindowImage::setupViewerToolbar()
{
    VisivoTheme::ViewerToolbarSpec spec;
    spec.filePath = this->filepath;
    spec.primaryTag = QStringLiteral("IMAGE");
    spec.secondaryTag = QStringLiteral("remote · full-res");

    QToolButton *findBtn = nullptr;
    QToolButton *cmdBtn  = nullptr;
    QToolButton *expBtn  = nullptr;
    QToolButton *tagBtn  = nullptr;
    QWidget *bar = VisivoTheme::makeViewerToolbar(
            spec, &findBtn, &cmdBtn, &expBtn, &tagBtn, this);

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

    Q_UNUSED(findBtn);
    Q_UNUSED(cmdBtn);
    Q_UNUSED(expBtn);
}

// ── Sidebar ───────────────────────────────────────────────────────────────────

// ── Contour overlay — same pipeline as vtkWindowCube ────────────────────────
// vtkFlyingEdges2D connected to the master layer image data. GenerateValues
// produces N evenly-spaced levels between lower and upper, matching the
// cube viewer's Level / Lower / Upper UI pattern. The actor is permanently
// in the renderer; visibility is toggled via the "Show Contours" button.

void vtkWindowImage::setupContourPipeline()
{
    auto *imageData = this->layers
            ? this->layers->getImageData(this->layers->getMasterIndex())
            : nullptr;
    if (!imageData) return;

    double range[2];
    imageData->GetScalarRange(range);
    m_contourLower = range[0];
    m_contourUpper = range[1];

    const bool firstTime = !m_contourFilter;
    if (firstTime) {
        m_contourFilter = vtkSmartPointer<vtkFlyingEdges2D>::New();
        m_contourActor  = vtkSmartPointer<vtkActor>::New();
    }

    m_contourFilter->SetInputData(imageData);
    m_contourFilter->GenerateValues(m_contourLevel, m_contourLower, m_contourUpper);
    m_contourFilter->Update();

    if (firstTime) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(m_contourFilter->GetOutputPort());
        mapper->SetScalarRange(m_contourLower, m_contourUpper);
        m_contourActor->SetMapper(mapper);
        m_contourActor->VisibilityOff();

        auto *ren = ui->vtk->renderWindow()
                    ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                    : nullptr;
        if (ren) ren->AddViewProp(m_contourActor);
    } else if (m_contourActor->GetMapper()) {
        static_cast<vtkPolyDataMapper *>(m_contourActor->GetMapper())
                ->SetScalarRange(m_contourLower, m_contourUpper);
    }

    // Sync the sidebar line edits so the user sees the actual range from
    // the real image data, not the stale defaults from construction time.
    if (m_contourLowerEdit) {
        const QSignalBlocker b(m_contourLowerEdit);
        m_contourLowerEdit->setText(QString::number(m_contourLower, 'g', 6));
    }
    if (m_contourUpperEdit) {
        const QSignalBlocker b(m_contourUpperEdit);
        m_contourUpperEdit->setText(QString::number(m_contourUpper, 'g', 6));
    }

    m_contourActor->SetVisibility(m_contoursVisible);
    m_contourPipelineDirty = false;
}

void vtkWindowImage::updateContours()
{
    if (!m_contourFilter) return;
    // Guard: lower must be < upper for GenerateValues to produce valid levels.
    if (m_contourLower >= m_contourUpper) return;
    m_contourFilter->GenerateValues(m_contourLevel, m_contourLower, m_contourUpper);
    m_contourFilter->Modified();
    m_contourFilter->Update();
    if (m_contourActor && m_contourActor->GetMapper()) {
        static_cast<vtkPolyDataMapper *>(m_contourActor->GetMapper())
                ->SetScalarRange(m_contourLower, m_contourUpper);
    }
    if (ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::setContoursVisible(bool vis)
{
    // Lazy pipeline rebuild: applyRemoteMasterLayer skips setupContourPipeline
    // on the preview→full swap when contours are invisible, so we have to
    // wire the filter to the current master image data the first time the
    // user actually turns contours on. setupContourPipeline clears the dirty
    // flag, so subsequent toggles are cheap (just SetVisibility + Render).
    if (vis && m_contourPipelineDirty) {
        setupContourPipeline();
    }
    m_contoursVisible = vis;
    if (m_contourActor) {
        m_contourActor->SetVisibility(vis);
    }
    if (ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::addExternalContours()
{
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                : nullptr;
    if (!ren) return;

    const QString path = QFileDialog::getOpenFileName(
            this, u"Load Contour FITS"_s, QString(),
            u"FITS files (*.fits *.fit);;All files (*)"_s);
    if (path.isEmpty()) return;

    bool ok = false;
    const QString levelsStr = QInputDialog::getText(
            this, u"Contour Levels"_s,
            u"Enter contour levels (comma-separated):"_s,
            QLineEdit::Normal,
            u"0.1, 0.5, 1.0, 2.0, 5.0"_s, &ok);
    if (!ok || levelsStr.trimmed().isEmpty()) return;

    QVector<double> levels;
    for (const auto &s : levelsStr.split(u','))  {
        bool lok = false;
        const double v = s.trimmed().toDouble(&lok);
        if (lok && std::isfinite(v)) levels.append(v);
    }
    if (levels.isEmpty()) return;

    vtkNew<vtkFITSReader> reader;
    reader->SetFileName(path.toStdString().c_str());
    reader->Update();
    auto *extData = reader->GetOutput();
    if (!extData || extData->GetNumberOfPoints() == 0) {
        QMessageBox::warning(this, u"Contour"_s,
                u"Could not read FITS data from:\n%1"_s.arg(path));
        return;
    }

    vtkNew<vtkContourFilter> contour;
    contour->SetInputData(extData);
    contour->SetNumberOfContours(levels.size());
    for (int i = 0; i < levels.size(); ++i)
        contour->SetValue(i, levels[i]);
    contour->Update();

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(contour->GetOutputPort());
    mapper->ScalarVisibilityOff();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(1.0, 0.4, 0.2);
    actor->GetProperty()->SetLineWidth(1.5);
    actor->VisibilityOn();

    ren->AddActor(actor);
    m_externalContourLayers.append({ QFileInfo(path).fileName(), actor });
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::overlayExternalContourData(const QString &label, vtkImageData *data)
{
    if (!data || data->GetNumberOfPoints() == 0) return;
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!ren) return;

    double range[2];
    data->GetScalarRange(range);
    if (range[1] <= range[0]) return;

    constexpr int nLevels = 8;
    vtkNew<vtkFlyingEdges2D> contour;
    contour->SetInputData(data);
    contour->GenerateValues(nLevels, range[0] + (range[1] - range[0]) * 0.1,
                            range[0] + (range[1] - range[0]) * 0.9);
    contour->Update();

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(contour->GetOutputPort());
    mapper->ScalarVisibilityOff();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(0.2, 1.0, 0.4);
    actor->GetProperty()->SetLineWidth(1.5);
    actor->VisibilityOn();

    ren->AddActor(actor);
    m_externalContourLayers.append({ label, actor });
    statusBar()->showMessage(
            QStringLiteral("Contour overlay from cube: %1").arg(label), 3000);
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::clearAllContours()
{
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                : nullptr;
    for (auto &layer : m_externalContourLayers) {
        if (ren && layer.actor) ren->RemoveActor(layer.actor);
    }
    m_externalContourLayers.clear();
    setContoursVisible(false);
    if (ren && ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

// ── Beam ellipse indicator ───────────────────────────────────────────────────

void vtkWindowImage::setBeamInfo(double majorDeg, double minorDeg, double paDeg)
{
    m_beamMajorDeg = majorDeg;
    m_beamMinorDeg = minorDeg;
    m_beamPaDeg    = paDeg;
    if (!std::isfinite(majorDeg) || !std::isfinite(minorDeg)
        || majorDeg <= 0.0 || minorDeg <= 0.0) {
        if (m_beamActor) m_beamActor->VisibilityOff();
        return;
    }
    const double cdelt = std::abs(this->remoteDatasetCdelt[0]);
    if (cdelt <= 0.0) return;
    const double majPx = majorDeg / cdelt;
    const double minPx = minorDeg / cdelt;
    const int nSides = 64;
    const double paRad = paDeg * M_PI / 180.0;

    vtkNew<vtkPoints> points;
    points->SetNumberOfPoints(nSides);
    vtkNew<vtkCellArray> poly;
    poly->InsertNextCell(nSides);
    for (int i = 0; i < nSides; ++i) {
        const double theta = 2.0 * M_PI * i / nSides;
        const double lx = (minPx / 2.0) * std::cos(theta);
        const double ly = (majPx / 2.0) * std::sin(theta);
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

    if (!m_beamActor) m_beamActor = vtkSmartPointer<vtkActor>::New();
    m_beamActor->SetMapper(mapper);
    m_beamActor->GetProperty()->SetColor(1.0, 1.0, 1.0);
    m_beamActor->GetProperty()->SetOpacity(0.6);
    m_beamActor->GetProperty()->EdgeVisibilityOn();
    m_beamActor->GetProperty()->SetEdgeColor(1.0, 1.0, 1.0);
    m_beamActor->SetPosition(majPx / 2.0 + 5.0, majPx / 2.0 + 5.0, 0.0);
    m_beamActor->VisibilityOn();

    auto *ren = ui->vtk->renderWindow()
                        ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                        : nullptr;
    if (ren && !ren->HasViewProp(m_beamActor)) {
        ren->AddActor(m_beamActor);
    }
    if (ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

// ── Dynamic spectrum / beamformed mode ──────────────────────────────────────

void vtkWindowImage::setDynamicSpectrumMode(bool enabled)
{
    if (m_dynamicSpectrumMode == enabled) return;
    m_dynamicSpectrumMode = enabled;

    // Hide celestial-WCS specific tools and overlays. They make no sense for
    // a time × frequency waterfall: no beam, no Galactic / FK5 frame, no
    // polarimetry workflow, no catalogue overlay against sky coords.
    const auto setActionEnabled = [enabled](QAction *act) {
        if (!act) return;
        act->setEnabled(!enabled);
        act->setVisible(!enabled);
    };
    setActionEnabled(actionWcsSexagesimal);
    setActionEnabled(actionWcsDecimal);
    setActionEnabled(actionLoadCatalogueOverlay);
    setActionEnabled(actionShowCatalogueOverlay);
    setActionEnabled(actionShowCatalogueLabels);
    setActionEnabled(actionClearCatalogueOverlay);
    setActionEnabled(m_actLoadStokesCompanions);
    setActionEnabled(m_actComputeStokesP);
    setActionEnabled(m_actComputeStokesPA);
    setActionEnabled(m_actComputeStokesFrac);
    setActionEnabled(m_actComputeStokesPdebiased);
    setActionEnabled(m_actComputeSpectralIndex);
    setActionEnabled(m_actComputeFaradayRM);
    setActionEnabled(m_actTogglePolVecs);
    if (ui->actionGalactic)  ui->actionGalactic->setEnabled(!enabled);
    if (ui->actionFK5)       ui->actionFK5->setEnabled(!enabled);
    if (ui->actionEcliptic)  ui->actionEcliptic->setEnabled(!enabled);

    // Sidebar cards: hide celestial-only ones and reveal the Time-Series
    // Tools placeholder. Done outside the if/else so the inverse direction
    // (re-enabling celestial mode after a dynspec was loaded) also works.
    for (const auto &card : std::as_const(m_dynspecHiddenCards)) {
        if (card) card->setVisible(!enabled);
    }
    if (m_dynspecToolsCard) {
        m_dynspecToolsCard->setVisible(enabled);
    }

    if (enabled) {
        // Beam indicator is meaningless for beamformed data.
        m_beamMajorDeg = std::numeric_limits<double>::quiet_NaN();
        m_beamMinorDeg = std::numeric_limits<double>::quiet_NaN();
        m_beamPaDeg    = std::numeric_limits<double>::quiet_NaN();
        if (m_beamActor) m_beamActor->VisibilityOff();
        // Status-bar panels: replace sanity messaging with a stable note.
        if (this->sanityLabel) {
            this->sanityLabel->setText(u"Dynamic Spectrum"_s);
            this->sanityLabel->setToolTip(
                    u"This dataset is a beamformed / dynamic-spectrum product. "
                    u"Time × frequency axes; celestial-WCS tools are disabled."_s);
        }
    }

    if (ui->vtk && ui->vtk->renderWindow()) {
        this->invalidateWcsOverlayCache();
        this->requestWcsOverlayRender();
    }
}

// ── Dynamic-spectrum tool slots ─────────────────────────────────────────────

void vtkWindowImage::runDynspecDedispersion()
{
    if (!m_dynamicSpectrumMode) return;
    auto *edit = this->findChild<QLineEdit *>(u"dynspecDmEdit"_s);
    if (!edit) return;
    const double dm = edit->text().toDouble();
    if (dm <= 0.0) {
        QMessageBox::information(this, u"Dedispersion"_s,
                u"Set a positive Dispersion Measure (pc cm⁻³) before computing."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Dedispersing at DM=%1 pc cm⁻³…").arg(dm), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecDedisperseResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecDedisperseResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Dedispersion"_s,
                    u"Dedispersion failed:\n\n%1"_s.arg(r.error));
            return;
        }
        QMessageBox::information(self, u"Dedispersion"_s,
                u"Dedispersed dynspec saved.\n\n"
                u"Filename: %1\nDM: %2 pc cm⁻³\nRef. freq.: %3 MHz\n"
                u"Max channel shift: %4 samples\n\n"
                u"Open the file in a new viewer or as a layer to inspect "
                u"the result."_s
                .arg(r.workspaceFilename)
                .arg(r.dmPcCm3, 0, 'f', 4)
                .arg(r.refFreqMhz, 0, 'f', 3)
                .arg(r.maxShiftSamples));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, dm]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.dedisperseDynspec(dsId, dm, /*refFreqMhz=*/0.0, QString());
    }));
}

void vtkWindowImage::runDynspecPulseProfile()
{
    if (!m_dynamicSpectrumMode) return;
    auto *pEdit = this->findChild<QLineEdit *>(u"dynspecPeriodEdit"_s);
    auto *dmEdit = this->findChild<QLineEdit *>(u"dynspecDmEdit"_s);
    if (!pEdit) return;
    const double period = pEdit->text().toDouble();
    const double dm = dmEdit ? dmEdit->text().toDouble() : 0.0;
    if (period <= 0.0) {
        QMessageBox::information(this, u"Pulse Profile"_s,
                u"Set a positive period (seconds) before folding."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Folding at P=%1 s (DM=%2)…")
                    .arg(period, 0, 'g', 6).arg(dm, 0, 'f', 3), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecPulseProfileResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecPulseProfileResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Pulse Profile"_s,
                    u"Pulse profile computation failed:\n\n%1"_s.arg(r.error));
            return;
        }

        // Floating QCustomPlot window with the folded profile
        auto *win = new QWidget(nullptr, Qt::Window);
        win->setAttribute(Qt::WA_DeleteOnClose);
        win->setWindowTitle(
                QStringLiteral("Pulse Profile — P=%1 s · DM=%2 · SNR=%3")
                        .arg(r.periodS, 0, 'g', 6)
                        .arg(r.dmPcCm3, 0, 'f', 3)
                        .arg(r.snrPeak, 0, 'f', 1));
        win->resize(640, 360);
        auto *layout = new QVBoxLayout(win);
        layout->setContentsMargins(4, 4, 4, 4);

        auto *info = new QLabel(QStringLiteral(
                "Period: %1 s    DM: %2 pc cm⁻³    Bins: %3    "
                "Peak: %4    σ_MAD: %5    SNR (peak/σ): %6")
                        .arg(r.periodS, 0, 'g', 6).arg(r.dmPcCm3, 0, 'f', 3)
                        .arg(r.nBins)
                        .arg(r.peak, 0, 'g', 5).arg(r.sigmaMad, 0, 'g', 5)
                        .arg(r.snrPeak, 0, 'f', 2), win);
        layout->addWidget(info);

        auto *plot = new QCustomPlot(win);
        layout->addWidget(plot);
        plot->addGraph();
        plot->graph(0)->setData(r.phase, r.profile);
        plot->graph(0)->setPen(QPen(QColor(70, 130, 180), 1.5));
        plot->graph(0)->setBrush(QBrush(QColor(70, 130, 180, 40)));
        plot->xAxis->setLabel(u"Pulse phase"_s);
        plot->yAxis->setLabel(u"Mean intensity"_s);
        if (!r.phase.isEmpty()) {
            plot->xAxis->setRange(0.0, 1.0);
            double minY = std::numeric_limits<double>::infinity();
            double maxY = -std::numeric_limits<double>::infinity();
            for (double v : r.profile) {
                if (std::isfinite(v)) {
                    minY = std::min(minY, v);
                    maxY = std::max(maxY, v);
                }
            }
            if (std::isfinite(minY) && std::isfinite(maxY) && minY < maxY) {
                const double pad = (maxY - minY) * 0.10;
                plot->yAxis->setRange(minY - pad, maxY + pad);
            }
        }
        plot->replot();
        win->show();
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, period, dm]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.pulseProfile(dsId, period, /*nBins=*/64, dm,
                              /*fLo*/ 0.0, /*fHi*/ 0.0);
    }));
}

void vtkWindowImage::runDynspecRfiMask()
{
    if (!m_dynamicSpectrumMode) return;
    auto *edit = this->findChild<QLineEdit *>(u"dynspecSigmaEdit"_s);
    if (!edit) return;
    const double sigma = edit->text().toDouble();
    if (sigma <= 0.0) {
        QMessageBox::information(this, u"RFI Mask"_s,
                u"Set a positive σ threshold before flagging."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Flagging RFI at %1 σ…").arg(sigma), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecRfiMaskResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecRfiMaskResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"RFI Mask"_s,
                    u"RFI mask computation failed:\n\n%1"_s.arg(r.error));
            return;
        }
        QMessageBox::information(self, u"RFI Mask"_s,
                u"RFI mask saved to Workspace Exports.\n\n"
                u"Filename: %1\n"
                u"Flagged fraction: %2 %\n"
                u"σ threshold: %3 (across %4 axis)\n\n"
                u"Open it as a layer over the source dynspec to highlight "
                u"the flagged pixels."_s
                .arg(r.workspaceFilename)
                .arg(r.flaggedFraction * 100.0, 0, 'f', 2)
                .arg(r.nSigma, 0, 'f', 1)
                .arg(r.axis));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, sigma]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.rfiMask(dsId, sigma, QStringLiteral("channel"), QString());
    }));
}

// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowImage::setupSidebar()
{
    // Remove the UI-file sidebar container and replace it with SidebarPanel.
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->sidebarContainer);
    ui->sidebarContainer->hide();

    // ── Shared style constants (same palette as VBT / catalogue viewers) ──────
    const QString hdrStyle     = VisivoTheme::sidebarHeaderStyleSheet();
    const QString cardStyle    = VisivoTheme::sidebarCardStyleSheet();
    const QString subHdrStyle  = VisivoTheme::sidebarSubHeaderStyleSheet();
    const QString keyStyle     = VisivoTheme::infoKeyStyleSheet();
    const QString valStyle     = VisivoTheme::infoValueStyleSheet();
    const QString toolBtnStyle = VisivoTheme::sidebarToolButtonStyleSheet();

    // ── Page 0: Layer Settings (from UI file) ──────────────────────────────────
    // Apply styles to existing UI-file page widgets (page3D is hidden; not styled)
    for (QLabel *lbl : { ui->labelLayerHdr, ui->labelCatalogueHdr }) {
        lbl->setStyleSheet(hdrStyle);
    }
    for (QLabel *lbl : { ui->labelLayerListHdr, ui->labelInventoryHdr, ui->labelObjectsHdr }) {
        lbl->setStyleSheet(subHdrStyle);
    }
    for (QFrame *f : { ui->frameLayerControls, ui->frameCatalogueActions }) {
        f->setStyleSheet(cardStyle);
    }

    // ── Page 1: Tools ─────────────────────────────────────────────────────────
    auto *pageTools = new QWidget;
    auto *toolsLayout = new QVBoxLayout(pageTools);
    toolsLayout->setContentsMargins(8, 8, 8, 8);
    toolsLayout->setSpacing(8);

    {
        auto *hdr = new QLabel(u"TOOLS"_s, pageTools);
        hdr->setStyleSheet(hdrStyle);
        toolsLayout->addWidget(hdr);
    }

    // Helper: create a full-width tool button bound to an action
    const auto makeToolBtn = [&toolBtnStyle](QAction *action, QWidget *parent) -> QToolButton * {
        auto *btn = new QToolButton(parent);
        btn->setDefaultAction(action);
        btn->setStyleSheet(toolBtnStyle);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return btn;
    };

    // Card: Analysis Tools (probe + region stats)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Analysis Tools"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(ui->actionProfile,          card));
        cl->addWidget(makeToolBtn(this->actionBoxRegion,      card));
        cl->addWidget(makeToolBtn(this->actionCircleRegion,   card));
        cl->addWidget(makeToolBtn(this->actionPolygonRegion,  card));
        cl->addWidget(makeToolBtn(this->actionAnnulusRegion,  card));
        toolsLayout->addWidget(card);
    }

    // Card: Catalogue Overlay
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Catalogue Overlay"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(this->actionLoadCatalogueOverlay,  card));
        cl->addWidget(makeToolBtn(this->actionShowCatalogueOverlay,  card));
        cl->addWidget(makeToolBtn(this->actionShowCatalogueLabels,   card));
        cl->addWidget(makeToolBtn(this->actionClearCatalogueOverlay, card));
        toolsLayout->addWidget(card);
        m_dynspecHiddenCards.append(QPointer<QFrame>(card));
    }

    // Card: Measurement Tools (Ruler / Angle)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Measurement"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actRuler,     card));
        cl->addWidget(makeToolBtn(m_actAngle,     card));
        cl->addWidget(makeToolBtn(m_actClearMeas, card));
        toolsLayout->addWidget(card);
    }

    // Card: Contours (same Level / Lower / Upper as the cube viewer)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *cardHdr = new QLabel(u"Contours"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actToggleContours, card));

        auto *formLayout = new QFormLayout();
        formLayout->setHorizontalSpacing(8);
        formLayout->setVerticalSpacing(4);

        m_contourLevelSpin = new QSpinBox(card);
        m_contourLevelSpin->setRange(1, 100);
        m_contourLevelSpin->setValue(m_contourLevel);
        formLayout->addRow(u"Level"_s, m_contourLevelSpin);

        m_contourLowerEdit = new QLineEdit(QString::number(m_contourLower, 'g', 6), card);
        m_contourLowerEdit->setValidator(new QDoubleValidator(card));
        formLayout->addRow(u"Lower"_s, m_contourLowerEdit);

        m_contourUpperEdit = new QLineEdit(QString::number(m_contourUpper, 'g', 6), card);
        m_contourUpperEdit->setValidator(new QDoubleValidator(card));
        formLayout->addRow(u"Upper"_s, m_contourUpperEdit);
        QSpinBox  *levelSpin = m_contourLevelSpin;
        QLineEdit *lowerEdit = m_contourLowerEdit;
        QLineEdit *upperEdit = m_contourUpperEdit;

        cl->addLayout(formLayout);
        toolsLayout->addWidget(card);

        QObject::connect(levelSpin, qOverload<int>(&QSpinBox::valueChanged), this,
                         [this](int v) { m_contourLevel = v; updateContours(); });
        QObject::connect(lowerEdit, &QLineEdit::editingFinished, this,
                         [this, lowerEdit]() {
                             m_contourLower = lowerEdit->text().toDouble();
                             updateContours();
                         });
        QObject::connect(upperEdit, &QLineEdit::editingFinished, this,
                         [this, upperEdit]() {
                             m_contourUpper = upperEdit->text().toDouble();
                             updateContours();
                         });

        // External contour overlay actions (mirror the Tools menu)
        cl->addWidget(makeToolBtn(m_actLoadExternalContour, card));
        cl->addWidget(makeToolBtn(m_actClearAllContours,    card));
    }

    // Card: I/O — Export to Workspace (mirror the Tools menu)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"I/O"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actExportToWorkspace, card));
        toolsLayout->addWidget(card);
    }

    // Card: WCS Display — toggle button for axes + SegmentedToggle for
    // format (Sexagesimal / Decimal) and frame (Galactic / FK5 / Ecliptic),
    // consistent with the cube viewer's mode selectors.
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *cardHdr = new QLabel(u"WCS Display"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(this->actionShowWcsAxes, card));

        const QString secStyle =
                QStringLiteral("QLabel { color: %1; font-size: 9px; margin-top: 4px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

        // Coordinate format: Sexagesimal | Decimal
        auto *fmtHdr = new QLabel(u"Coordinate format"_s, card);
        fmtHdr->setStyleSheet(secStyle);
        cl->addWidget(fmtHdr);
        auto *fmtToggle = new SegmentedToggle(card);
        fmtToggle->addItem(u"Sexagesimal"_s, u"HMS / DMS"_s);
        fmtToggle->addItem(u"Decimal"_s,     u"Decimal degrees"_s);
        fmtToggle->setCurrentIndex(
                this->actionWcsDecimal->isChecked() ? 1 : 0);
        cl->addWidget(fmtToggle);
        QObject::connect(fmtToggle, &SegmentedToggle::currentIndexChanged, this,
                         [this](int idx) {
                             if (idx == 0 && !this->actionWcsSexagesimal->isChecked())
                                 this->actionWcsSexagesimal->trigger();
                             else if (idx == 1 && !this->actionWcsDecimal->isChecked())
                                 this->actionWcsDecimal->trigger();
                         });

        // Coordinate frame: Galactic | FK5 | Ecliptic
        auto *frameHdr = new QLabel(u"Coordinate frame"_s, card);
        frameHdr->setStyleSheet(secStyle);
        cl->addWidget(frameHdr);
        auto *frameToggle = new SegmentedToggle(card);
        frameToggle->addItem(u"Galactic"_s, u"Galactic l, b"_s);
        frameToggle->addItem(u"FK5"_s,      u"RA, Dec (J2000)"_s);
        frameToggle->addItem(u"Ecliptic"_s, u"Ecliptic λ, β"_s);
        if (ui->actionGalactic->isChecked())       frameToggle->setCurrentIndex(0);
        else if (ui->actionFK5->isChecked())       frameToggle->setCurrentIndex(1);
        else if (ui->actionEcliptic->isChecked())  frameToggle->setCurrentIndex(2);
        cl->addWidget(frameToggle);
        QObject::connect(frameToggle, &SegmentedToggle::currentIndexChanged, this,
                         [this](int idx) {
                             QAction *targets[] = { ui->actionGalactic,
                                                    ui->actionFK5,
                                                    ui->actionEcliptic };
                             if (idx >= 0 && idx < 3 && !targets[idx]->isChecked())
                                 targets[idx]->trigger();
                         });

        toolsLayout->addWidget(card);
        m_dynspecHiddenCards.append(QPointer<QFrame>(card));
    }

    // Card: Histogram
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Histogram"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        auto *btn = new QToolButton(card);
        btn->setText(u"Pixel Histogram…"_s);
        btn->setStyleSheet(toolBtnStyle);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btn, &QToolButton::clicked, this, &vtkWindowImage::showHistogramPanel);
        cl->addWidget(btn);
        toolsLayout->addWidget(card);
    }

    // Card: Annotations
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Annotations"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        auto *btnText = new QToolButton(card);
        btnText->setText(u"Add Text…"_s);
        btnText->setStyleSheet(toolBtnStyle);
        btnText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnText->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnText, &QToolButton::clicked, this, &vtkWindowImage::addTextAnnotation);
        cl->addWidget(btnText);

        auto *btnArrow = new QToolButton(card);
        btnArrow->setText(u"Add Arrow…"_s);
        btnArrow->setStyleSheet(toolBtnStyle);
        btnArrow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnArrow->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnArrow, &QToolButton::clicked, this, &vtkWindowImage::addArrowAnnotation);
        cl->addWidget(btnArrow);

        auto *btnClear = new QToolButton(card);
        btnClear->setText(u"Clear All"_s);
        btnClear->setStyleSheet(toolBtnStyle);
        btnClear->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnClear->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnClear, &QToolButton::clicked, this, &vtkWindowImage::clearAnnotations);
        cl->addWidget(btnClear);

        auto *rowIO = new QHBoxLayout;
        rowIO->setSpacing(4);
        auto *btnSave = new QToolButton(card);
        btnSave->setText(u"Save"_s);
        btnSave->setStyleSheet(toolBtnStyle);
        btnSave->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnSave->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnSave, &QToolButton::clicked, this, &vtkWindowImage::saveAnnotationsJson);
        rowIO->addWidget(btnSave);

        auto *btnLoad = new QToolButton(card);
        btnLoad->setText(u"Load"_s);
        btnLoad->setStyleSheet(toolBtnStyle);
        btnLoad->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnLoad->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnLoad, &QToolButton::clicked, this, &vtkWindowImage::loadAnnotationsJson);
        rowIO->addWidget(btnLoad);

        cl->addLayout(rowIO);
        toolsLayout->addWidget(card);
    }

    // Card: Blink / Compare
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Blink / Compare"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actBlink, card));

        auto *speedLbl = new QLabel(u"Speed"_s, card);
        speedLbl->setStyleSheet(
                QStringLiteral("QLabel { color: %1; font-size: 9px; "
                               "font-weight: 600; margin-top: 2px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        cl->addWidget(speedLbl);

        auto *speedSlider = new QSlider(Qt::Horizontal, card);
        speedSlider->setRange(50, 1000);
        speedSlider->setValue(m_blinkIntervalMs);
        speedSlider->setToolTip(u"Blink interval in ms (50–1000)"_s);
        cl->addWidget(speedSlider);
        QObject::connect(speedSlider, &QSlider::valueChanged, this,
                         [this](int val) {
                             m_blinkIntervalMs = val;
                             if (m_blinkTimer.isActive())
                                 m_blinkTimer.setInterval(val);
                         });

        toolsLayout->addWidget(card);
    }

    // Card: Stokes Analysis (radio polarimetry workflow)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Stokes Analysis"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actLoadStokesCompanions, card));
        cl->addWidget(makeToolBtn(m_actComputeStokesP, card));
        cl->addWidget(makeToolBtn(m_actComputeStokesPdebiased, card));
        cl->addWidget(makeToolBtn(m_actComputeStokesPA, card));
        cl->addWidget(makeToolBtn(m_actComputeStokesFrac, card));
        cl->addWidget(makeToolBtn(m_actComputeSpectralIndex, card));
        cl->addWidget(makeToolBtn(m_actComputeFaradayRM, card));
        cl->addWidget(makeToolBtn(m_actTogglePolVecs, card));

        // Pol-vector parameters
        const QString secStyle =
                QStringLiteral("QLabel { color: %1; font-size: 9px; "
                               "font-weight: 600; margin-top: 4px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

        auto *gridLbl = new QLabel(u"Vector grid step (px)"_s, card);
        gridLbl->setStyleSheet(secStyle);
        cl->addWidget(gridLbl);
        auto *gridSpin = new QSpinBox(card);
        gridSpin->setRange(2, 200);
        gridSpin->setValue(m_polVecGridStep);
        cl->addWidget(gridSpin);
        QObject::connect(gridSpin, qOverload<int>(&QSpinBox::valueChanged),
                         this, [this](int v) {
            m_polVecGridStep = v;
            if (m_polVecsVisible) rebuildPolarizationVectors();
            if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
        });

        auto *snrLbl = new QLabel(u"SNR threshold (σ_MAD multiples)"_s, card);
        snrLbl->setStyleSheet(secStyle);
        cl->addWidget(snrLbl);
        auto *snrEdit = new QLineEdit(QString::number(m_polVecSnrMin, 'f', 1), card);
        snrEdit->setValidator(new QDoubleValidator(0.0, 100.0, 2, card));
        cl->addWidget(snrEdit);
        QObject::connect(snrEdit, &QLineEdit::editingFinished,
                         this, [this, snrEdit]() {
            m_polVecSnrMin = snrEdit->text().toDouble();
            if (m_polVecsVisible) rebuildPolarizationVectors();
            if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
        });

        auto *scaleLbl = new QLabel(u"Vector length scale"_s, card);
        scaleLbl->setStyleSheet(secStyle);
        cl->addWidget(scaleLbl);
        auto *scaleSlider = new QSlider(Qt::Horizontal, card);
        scaleSlider->setRange(10, 300);
        scaleSlider->setValue(static_cast<int>(m_polVecScale * 100.0));
        cl->addWidget(scaleSlider);
        QObject::connect(scaleSlider, &QSlider::valueChanged,
                         this, [this](int v) {
            m_polVecScale = v / 100.0;
            if (m_polVecsVisible) rebuildPolarizationVectors();
            if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
        });

        toolsLayout->addWidget(card);
        m_dynspecHiddenCards.append(QPointer<QFrame>(card));
    }

    // Card: Time-Series Tools (only visible in dynamic-spectrum mode)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *cardHdr = new QLabel(u"Time-Series Tools"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        const QString miniHdrStyle =
                QStringLiteral("QLabel { color: %1; font-size: 9px; "
                               "font-weight: 600; margin-top: 4px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

        // ── Incoherent Dedispersion ────────────────────────────────────
        cl->addWidget(([&]() {
            auto *l = new QLabel(u"Dedispersion (DM, pc cm⁻³)"_s, card);
            l->setStyleSheet(miniHdrStyle);
            return l;
        })());
        auto *dmEdit = new QLineEdit(u"0.0"_s, card);
        dmEdit->setValidator(new QDoubleValidator(0.0, 1e6, 4, card));
        dmEdit->setObjectName(u"dynspecDmEdit"_s);
        cl->addWidget(dmEdit);
        auto *dedispBtn = new QToolButton(card);
        dedispBtn->setText(u"Compute Dedispersion"_s);
        dedispBtn->setStyleSheet(toolBtnStyle);
        dedispBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        dedispBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(dedispBtn, &QToolButton::clicked, this,
                         &vtkWindowImage::runDynspecDedispersion);
        cl->addWidget(dedispBtn);

        // ── Pulse profile folding ──────────────────────────────────────
        cl->addWidget(([&]() {
            auto *l = new QLabel(u"Pulse profile (period, s)"_s, card);
            l->setStyleSheet(miniHdrStyle);
            return l;
        })());
        auto *periodEdit = new QLineEdit(u"1.0"_s, card);
        periodEdit->setValidator(new QDoubleValidator(1e-9, 1e6, 9, card));
        periodEdit->setObjectName(u"dynspecPeriodEdit"_s);
        cl->addWidget(periodEdit);
        auto *foldBtn = new QToolButton(card);
        foldBtn->setText(u"Compute Pulse Profile"_s);
        foldBtn->setStyleSheet(toolBtnStyle);
        foldBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        foldBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(foldBtn, &QToolButton::clicked, this,
                         &vtkWindowImage::runDynspecPulseProfile);
        cl->addWidget(foldBtn);

        // ── RFI flagging mask ──────────────────────────────────────────
        cl->addWidget(([&]() {
            auto *l = new QLabel(u"RFI mask (σ-clip, σ-MAD multiple)"_s, card);
            l->setStyleSheet(miniHdrStyle);
            return l;
        })());
        auto *sigmaEdit = new QLineEdit(u"5.0"_s, card);
        sigmaEdit->setValidator(new QDoubleValidator(0.5, 50.0, 2, card));
        sigmaEdit->setObjectName(u"dynspecSigmaEdit"_s);
        cl->addWidget(sigmaEdit);
        auto *rfiBtn = new QToolButton(card);
        rfiBtn->setText(u"Compute RFI Mask"_s);
        rfiBtn->setStyleSheet(toolBtnStyle);
        rfiBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        rfiBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(rfiBtn, &QToolButton::clicked, this,
                         &vtkWindowImage::runDynspecRfiMask);
        cl->addWidget(rfiBtn);

        toolsLayout->addWidget(card);
        card->setVisible(false);  // shown only when setDynamicSpectrumMode(true)
        m_dynspecToolsCard = card;
    }

    toolsLayout->addStretch(1);

    // ── Page 2: Info / Stats ─────────────────────────────────────────────────
    auto *pageInfo = new QWidget;
    auto *infoLayout = new QVBoxLayout(pageInfo);
    infoLayout->setContentsMargins(8, 6, 8, 6);
    infoLayout->setSpacing(4);

    {
        auto *hdr = new QLabel(u"INFO / STATS"_s, pageInfo);
        hdr->setStyleSheet(hdrStyle);
        infoLayout->addWidget(hdr);
    }

    // Field-name style: small, muted, uppercase feel
    const QString fnHdrStyle =
            QStringLiteral("QLabel { color: %1; font-size: 9px; font-weight: 600; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));
    // Field-value style: readable, proportional (not monospace — avoids extra width)
    const QString fnValStyle =
            QStringLiteral("QLabel { color: %1; font-size: 10px; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface()));

    // ── Card 1: Dataset (stacked label-over-value — no two-column layout) ─────
    // Each field is a small header label stacked above its value label.
    // Every value is full-card-width, word-wrapped where needed.
    // This eliminates all horizontal overflow regardless of filename length.
    {
        auto *card = new QFrame(pageInfo);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(10, 8, 10, 8);
        cl->setSpacing(0);

        // Helper: add one stacked field (header + value) with optional word-wrap/tooltip
        const auto addField = [&](const QString &header, const QString &value,
                                  const QString &tip = {}, bool wrap = false) {
            auto *hdrLbl = new QLabel(header);
            hdrLbl->setStyleSheet(fnHdrStyle);
            cl->addWidget(hdrLbl);

            auto *valLbl = new QLabel(value);
            valLbl->setStyleSheet(fnValStyle);
            valLbl->setWordWrap(wrap);
            valLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            if (!tip.isEmpty())
                valLbl->setToolTip(tip);
            cl->addWidget(valLbl);
        };

        const QFileInfo fi(this->filepath);
        const int nLayers = this->layers ? this->layers->rowCount() : 1;

        addField(u"Active Layer"_s, fi.fileName(), fi.absoluteFilePath(), /*wrap=*/true);
        cl->addSpacing(5);
        addField(u"Layers"_s, QString::number(nLayers));
        cl->addSpacing(5);
        addField(u"Directory"_s, compactDirPath(fi.absolutePath()), fi.absolutePath(), /*wrap=*/true);
        cl->addSpacing(5);
        addField(u"WCS"_s, this->remoteWcsStatus);

        infoLayout->addWidget(card);
    }

    // ── Card 2: Axes (compact grid: Ax# | Type [unit] | Δ) ───────────────────
    // Axis type names are always short — a 3-column grid stays within sidebar width.
    {
        bool hasAnyAxis = false;
        for (int ax = 0; ax < 3 && !hasAnyAxis; ++ax)
            hasAnyAxis = !this->remoteDatasetCtype[ax].isEmpty();

        if (hasAnyAxis) {
            auto *card = new QFrame(pageInfo);
            card->setStyleSheet(cardStyle);
            auto *gl = new QGridLayout(card);
            gl->setContentsMargins(10, 6, 10, 6);
            gl->setSpacing(2);
            gl->setColumnStretch(1, 1); // type column absorbs available width

            // Tiny column headers
            const auto gridHdr = [&fnHdrStyle](const QString &t,
                                               Qt::Alignment align = Qt::AlignLeft) -> QLabel * {
                auto *l = new QLabel(t);
                l->setStyleSheet(fnHdrStyle);
                l->setAlignment(align);
                return l;
            };
            gl->addWidget(gridHdr(u"Ax"_s),                     0, 0);
            gl->addWidget(gridHdr(u"Type [unit]"_s),            0, 1);
            gl->addWidget(gridHdr(u"\u0394"_s, Qt::AlignRight), 0, 2);

            auto *sep = new QFrame;
            sep->setFrameShape(QFrame::HLine);
            sep->setStyleSheet(
                    u"QFrame { border: none; border-top: 1px solid rgba(4,138,191,0.14);"
                    u"         margin-bottom: 1px; }"_s);
            gl->addWidget(sep, 1, 0, 1, 3);

            int gridRow = 2;
            for (int ax = 0; ax < 3; ++ax) {
                const QString &ctype = this->remoteDatasetCtype[ax];
                if (ctype.isEmpty())
                    continue;
                const QString &cunit = this->remoteDatasetCunit[ax];
                const QString typeStr = cunit.isEmpty()
                        ? ctype
                        : QStringLiteral("%1 [%2]").arg(ctype, cunit);
                const QString deltaStr =
                        QStringLiteral("%1").arg(this->remoteDatasetCdelt[ax], 0, 'g', 5);

                auto *axLbl = new QLabel(QString::number(ax + 1));
                axLbl->setStyleSheet(fnHdrStyle);

                auto *typeLbl = new QLabel(typeStr);
                typeLbl->setStyleSheet(fnValStyle);
                typeLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);

                auto *deltaLbl = new QLabel(deltaStr);
                deltaLbl->setStyleSheet(fnValStyle);
                deltaLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                deltaLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);

                gl->addWidget(axLbl,    gridRow, 0);
                gl->addWidget(typeLbl,  gridRow, 1);
                gl->addWidget(deltaLbl, gridRow, 2);
                ++gridRow;
            }

            infoLayout->addWidget(card);
        }
    }

    // ── WCS Warning card (amber, only when present) ───────────────────────────
    if (!this->remoteWcsWarningMessage.isEmpty()) {
        auto *warnCard = new QFrame(pageInfo);
        warnCard->setStyleSheet(cardStyle);
        auto *wl = new QHBoxLayout(warnCard);
        wl->setContentsMargins(10, 6, 10, 6);
        wl->setSpacing(6);
        auto *warnIcon = new QLabel(u"\u26A0"_s); // ⚠
        warnIcon->setStyleSheet(u"QLabel { color: #CC8800; font-size: 12px; }"_s);
        warnIcon->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        auto *warnMsg = new QLabel(this->remoteWcsWarningMessage);
        warnMsg->setStyleSheet(u"QLabel { color: #BB9900; font-size: 9px; }"_s);
        warnMsg->setWordWrap(true);
        wl->addWidget(warnIcon);
        wl->addWidget(warnMsg, 1);
        infoLayout->addWidget(warnCard);
    }

    infoLayout->addStretch(1);

    // ── Wire pages into SidebarPanel ──────────────────────────────────────────
    this->m_sidebar = new SidebarPanel(272, this);
    this->m_sidebar->addPage(u":/icons/rail_layers.svg"_s,    u"Layer Settings"_s, ui->pageLayer);
    this->m_sidebar->addPage(u":/icons/rail_catalogue.svg"_s, u"Catalogue"_s,       ui->pageCatalogue);
    this->m_sidebar->addPage(u":/icons/rail_viz.svg"_s,        u"Tools"_s,           pageTools);
    this->m_sidebar->addPage(u":/icons/rail_stats.svg"_s,      u"Info / Stats"_s,    pageInfo);
    // Note: the "3D View" page (ui->page3D) is intentionally omitted — not relevant
    // for the 2D image viewer. The widget remains in the hidden sidebarContainer.
    this->m_sidebar->finalize(0);
    mainLayout->insertWidget(0, this->m_sidebar);

    this->actionShowControlsSidebar = this->m_sidebar->createToggleAction(
            u"Controls Sidebar"_s, this);
    ui->menuView->addAction(this->actionShowControlsSidebar);
}

// ── Measurement tools (Ruler / Angle) ────────────────────────────────────────

void vtkWindowImage::setMeasurementMode(MeasurementMode mode)
{
    if (m_measMode == mode) return;

    if (this->probeModeActive && ui->actionProfile->isChecked())
        ui->actionProfile->setChecked(false);
    if (this->regionMode != RegionMode::None) {
        QAction *actions[] = { actionBoxRegion, actionCircleRegion,
                               actionPolygonRegion, actionAnnulusRegion };
        for (auto *a : actions) {
            if (a && a->isChecked()) a->setChecked(false);
        }
    }

    m_measMode = mode;
    clearMeasurement();
    if (mode == MeasurementMode::None) {
        setInteractorStyleImage();
        ui->vtk->setCursor(Qt::ArrowCursor);
    } else {
        setInteractorStyleRegion();
        ui->vtk->setCursor(Qt::CrossCursor);
    }
}

void vtkWindowImage::addMeasurementPoint(int voxelX, int voxelY)
{
    m_measPoints.push_back({ voxelX, voxelY });

    const int needed = (m_measMode == MeasurementMode::Ruler) ? 2 : 3;
    if (static_cast<int>(m_measPoints.size()) >= needed) {
        refreshMeasurementOverlay();
    } else {
        if (m_measLabelActor) {
            const QString hint = (m_measMode == MeasurementMode::Ruler)
                    ? u"Click second point…"_s
                    : QStringLiteral("Click point %1 of 3…").arg(m_measPoints.size() + 1);
            m_measLabelActor->SetInput(hint.toStdString().c_str());
            m_measLabelActor->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
            m_measLabelActor->SetPosition(0.5, 0.96);
            m_measLabelActor->VisibilityOn();
        }
    }
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::refreshMeasurementOverlay()
{
    if (m_measMode == MeasurementMode::Ruler && m_measPoints.size() >= 2) {
        const auto &a = m_measPoints[0];
        const auto &b = m_measPoints[1];
        m_measLine1->SetPoint1(a[0], a[1], 0.0);
        m_measLine1->SetPoint2(b[0], b[1], 0.0);
        m_measLineActor1->VisibilityOn();
        m_measLineActor2->VisibilityOff();

        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double pixDist = std::sqrt(dx * dx + dy * dy);
        QString label = QStringLiteral("%1 px").arg(pixDist, 0, 'f', 1);

        const double angDeg = angularSeparationDeg(a[0], a[1], b[0], b[1]);
        if (angDeg >= 0.0) {
            const double arcsec = angDeg * 3600.0;
            if (arcsec < 60.0)
                label += QStringLiteral("  (%1\")").arg(arcsec, 0, 'f', 2);
            else if (arcsec < 3600.0)
                label += QStringLiteral("  (%1')").arg(arcsec / 60.0, 0, 'f', 2);
            else
                label += QStringLiteral("  (%1°)").arg(angDeg, 0, 'f', 4);
        }

        if (m_measLabelActor) {
            m_measLabelActor->SetInput(label.toStdString().c_str());
            m_measLabelActor->GetPositionCoordinate()->SetCoordinateSystemToWorld();
            m_measLabelActor->SetPosition((a[0] + b[0]) / 2.0 + 3.0,
                                           (a[1] + b[1]) / 2.0 + 3.0);
            m_measLabelActor->VisibilityOn();
        }
    } else if (m_measMode == MeasurementMode::Angle && m_measPoints.size() >= 3) {
        const auto &a = m_measPoints[0];
        const auto &b = m_measPoints[1];
        const auto &c = m_measPoints[2];
        m_measLine1->SetPoint1(a[0], a[1], 0.0);
        m_measLine1->SetPoint2(b[0], b[1], 0.0);
        m_measLine2->SetPoint1(b[0], b[1], 0.0);
        m_measLine2->SetPoint2(c[0], c[1], 0.0);
        m_measLineActor1->VisibilityOn();
        m_measLineActor2->VisibilityOn();

        const double bax = a[0] - b[0], bay = a[1] - b[1];
        const double bcx = c[0] - b[0], bcy = c[1] - b[1];
        const double dot = bax * bcx + bay * bcy;
        const double magBA = std::sqrt(bax * bax + bay * bay);
        const double magBC = std::sqrt(bcx * bcx + bcy * bcy);
        double angleDeg = 0.0;
        if (magBA > 0.0 && magBC > 0.0)
            angleDeg = std::acos(std::clamp(dot / (magBA * magBC), -1.0, 1.0)) * 180.0 / M_PI;

        const QString label = QStringLiteral("%1°").arg(angleDeg, 0, 'f', 2);
        if (m_measLabelActor) {
            m_measLabelActor->SetInput(label.toStdString().c_str());
            m_measLabelActor->GetPositionCoordinate()->SetCoordinateSystemToWorld();
            m_measLabelActor->SetPosition(b[0] + 5.0, b[1] + 5.0);
            m_measLabelActor->VisibilityOn();
        }
    }
}

void vtkWindowImage::clearMeasurement()
{
    m_measPoints.clear();
    m_measLineActor1->VisibilityOff();
    m_measLineActor2->VisibilityOff();
    if (m_measLabelActor) m_measLabelActor->VisibilityOff();
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

double vtkWindowImage::angularSeparationDeg(double vx1, double vy1, double vx2, double vy2) const
{
    if (!remoteHasCelestialAxes()) return -1.0;
    bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
    const double lon1 = remoteVoxelToWcs(0, vx1, &ok1);
    const double lat1 = remoteVoxelToWcs(1, vy1, &ok2);
    const double lon2 = remoteVoxelToWcs(0, vx2, &ok3);
    const double lat2 = remoteVoxelToWcs(1, vy2, &ok4);
    if (!ok1 || !ok2 || !ok3 || !ok4) return -1.0;

    const double deg2rad = M_PI / 180.0;
    const double dlon = (lon2 - lon1) * deg2rad;
    const double la1 = lat1 * deg2rad;
    const double la2 = lat2 * deg2rad;
    const double a = std::sin(dlon / 2.0);
    const double b = std::sin((la2 - la1) / 2.0);
    const double h = b * b + std::cos(la1) * std::cos(la2) * a * a;
    return 2.0 * std::asin(std::sqrt(std::clamp(h, 0.0, 1.0))) * 180.0 / M_PI;
}

// ── Histogram panel ──────────────────────────────────────────────────────────

void vtkWindowImage::showHistogramPanel()
{
    if (m_histogramWindow) {
        m_histogramWindow->raise();
        m_histogramWindow->activateWindow();
        return;
    }

    auto *imageData = this->layers
            ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!imageData) return;

    int extent[6];
    imageData->GetExtent(extent);
    const int width = extent[1] - extent[0] + 1;
    const int height = extent[3] - extent[2] + 1;
    if (width <= 0 || height <= 0) return;

    std::vector<double> values;
    const qint64 pixels = static_cast<qint64>(width) * height;
    const int stride = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(pixels) / 500000.0)));
    values.reserve(static_cast<std::size_t>(pixels / std::max(1, stride * stride)));
    for (int y = extent[2]; y <= extent[3]; y += stride) {
        for (int x = extent[0]; x <= extent[1]; x += stride) {
            const double v = imageData->GetScalarComponentAsDouble(x, y, 0, 0);
            if (std::isfinite(v)) values.push_back(v);
        }
    }
    if (values.empty()) return;

    std::sort(values.begin(), values.end());
    const double vmin = values.front();
    const double vmax = values.back();
    if (vmax <= vmin) return;

    constexpr int nBins = 256;
    const double binWidth = (vmax - vmin) / nBins;
    QVector<double> binCenters(nBins), counts(nBins, 0.0);
    for (int i = 0; i < nBins; ++i)
        binCenters[i] = vmin + (i + 0.5) * binWidth;
    for (const double v : values) {
        int bin = static_cast<int>((v - vmin) / binWidth);
        if (bin >= nBins) bin = nBins - 1;
        counts[bin] += 1.0;
    }

    auto *win = new QWidget(nullptr, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(u"Pixel Histogram"_s);
    win->resize(600, 360);
    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(4, 4, 4, 4);

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
    plot->yAxis->setRange(0, *std::max_element(counts.begin(), counts.end()) * 1.05);

    auto *lut = this->layers->getLookupTable(this->layers->getMasterIndex());
    const double *lutRange = lut ? lut->GetTableRange() : nullptr;
    const double clipLow  = lutRange ? lutRange[0] : vmin;
    const double clipHigh = lutRange ? lutRange[1] : vmax;

    auto *lowLine = new QCPItemLine(plot);
    lowLine->setPen(QPen(QColor(220, 50, 50), 2, Qt::DashLine));
    lowLine->start->setType(QCPItemPosition::ptPlotCoords);
    lowLine->end->setType(QCPItemPosition::ptPlotCoords);
    lowLine->start->setCoords(clipLow, 0);
    lowLine->end->setCoords(clipLow, *std::max_element(counts.begin(), counts.end()) * 1.05);

    auto *highLine = new QCPItemLine(plot);
    highLine->setPen(QPen(QColor(50, 180, 50), 2, Qt::DashLine));
    highLine->start->setType(QCPItemPosition::ptPlotCoords);
    highLine->end->setType(QCPItemPosition::ptPlotCoords);
    highLine->start->setCoords(clipHigh, 0);
    highLine->end->setCoords(clipHigh, *std::max_element(counts.begin(), counts.end()) * 1.05);

    auto *lowLabel = new QCPItemText(plot);
    lowLabel->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    lowLabel->position->setType(QCPItemPosition::ptPlotCoords);
    lowLabel->position->setCoords(clipLow, *std::max_element(counts.begin(), counts.end()) * 1.02);
    lowLabel->setText(QStringLiteral("Low: %1").arg(clipLow, 0, 'g', 5));
    lowLabel->setColor(QColor(220, 50, 50));

    auto *highLabel = new QCPItemText(plot);
    highLabel->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    highLabel->position->setType(QCPItemPosition::ptPlotCoords);
    highLabel->position->setCoords(clipHigh, *std::max_element(counts.begin(), counts.end()) * 1.02);
    highLabel->setText(QStringLiteral("High: %1").arg(clipHigh, 0, 'g', 5));
    highLabel->setColor(QColor(50, 180, 50));

    QPointer<vtkWindowImage> self(this);
    QPointer<QCPItemLine> lowLinePtr(lowLine);
    QPointer<QCPItemLine> highLinePtr(highLine);
    QPointer<QCPItemText> lowLabelPtr(lowLabel);
    QPointer<QCPItemText> highLabelPtr(highLabel);

    bool *draggingLow = new bool(false);
    bool *draggingHigh = new bool(false);
    const double maxCount = *std::max_element(counts.begin(), counts.end()) * 1.05;

    QObject::connect(plot, &QCustomPlot::mousePress, win,
                     [plot, lowLinePtr, highLinePtr, draggingLow, draggingHigh](QMouseEvent *event) {
        const double x = plot->xAxis->pixelToCoord(event->pos().x());
        if (lowLinePtr && std::abs(x - lowLinePtr->start->coords().x()) < (plot->xAxis->range().size() * 0.015))
            *draggingLow = true;
        else if (highLinePtr && std::abs(x - highLinePtr->start->coords().x()) < (plot->xAxis->range().size() * 0.015))
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
                if (hi > lo) {
                    auto *lut2 = self->layers->getLookupTable(self->layers->getMasterIndex());
                    if (lut2) {
                        lut2->SetTableRange(lo, hi);
                        lut2->Build();
                        self->markDisplayStateAdjusted("histogram drag");
                        self->vtkRender();
                    }
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

// ── Annotations ──────────────────────────────────────────────────────────────

void vtkWindowImage::addTextAnnotation()
{
    bool ok = false;
    const QString text = QInputDialog::getText(
            this, u"Add Text Annotation"_s, u"Text:"_s, QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty()) return;

    m_pendingAnnotText = text.trimmed();
    m_annotPlaceMode = AnnotPlaceMode::Text;
    ui->vtk->setCursor(Qt::CrossCursor);
    statusBar()->showMessage(u"Click on the image to place the text annotation."_s, 0);
}

void vtkWindowImage::addArrowAnnotation()
{
    bool ok = false;
    const QString text = QInputDialog::getText(
            this, u"Add Arrow Annotation"_s, u"Label (optional):"_s,
            QLineEdit::Normal, QString(), &ok);
    if (!ok) return;

    m_pendingAnnotText = text.trimmed();
    m_annotPlaceMode = AnnotPlaceMode::ArrowTip;
    ui->vtk->setCursor(Qt::CrossCursor);
    statusBar()->showMessage(u"Click on the image to place the arrow tip."_s, 0);
}

void vtkWindowImage::placeAnnotationAt(double worldX, double worldY)
{
    switch (m_annotPlaceMode) {
    case AnnotPlaceMode::Text: {
        Annotation ann;
        ann.type = Annotation::Text;
        ann.text = m_pendingAnnotText;
        ann.worldX = worldX;
        ann.worldY = worldY;
        m_annotations.push_back(ann);
        cancelAnnotationPlacement();
        refreshAnnotationOverlay();
        break;
    }
    case AnnotPlaceMode::ArrowTip:
        m_pendingArrowTipX = worldX;
        m_pendingArrowTipY = worldY;
        m_annotPlaceMode = AnnotPlaceMode::ArrowLabel;
        updateAnnotationPreview(worldX, worldY);
        statusBar()->showMessage(u"Move mouse to see the arrow, click to place the label."_s, 0);
        break;
    case AnnotPlaceMode::ArrowLabel: {
        Annotation ann;
        ann.type = Annotation::Arrow;
        ann.text = m_pendingAnnotText;
        ann.worldX = worldX;
        ann.worldY = worldY;
        ann.arrowEndX = m_pendingArrowTipX;
        ann.arrowEndY = m_pendingArrowTipY;
        m_annotations.push_back(ann);
        cancelAnnotationPlacement();
        refreshAnnotationOverlay();
        break;
    }
    default:
        break;
    }
}

void vtkWindowImage::updateAnnotationPreview(double worldX, double worldY)
{
    if (m_annotPlaceMode != AnnotPlaceMode::ArrowLabel) {
        m_annotPreviewActor->VisibilityOff();
        return;
    }

    vtkNew<vtkPoints> pts;
    vtkNew<vtkCellArray> cells;
    buildArrowGeometry(pts, cells, worldX, worldY, m_pendingArrowTipX, m_pendingArrowTipY);
    m_annotPreviewData->SetPoints(pts);
    m_annotPreviewData->SetLines(cells);
    m_annotPreviewData->Modified();
    m_annotPreviewActor->VisibilityOn();
}

void vtkWindowImage::cancelAnnotationPlacement()
{
    m_annotPlaceMode = AnnotPlaceMode::None;
    m_pendingAnnotText.clear();
    m_annotPreviewActor->VisibilityOff();
    const bool needsCross = this->probeModeActive
            || this->regionMode != RegionMode::None
            || this->m_measMode != MeasurementMode::None;
    ui->vtk->setCursor(needsCross ? Qt::CrossCursor : Qt::ArrowCursor);
    statusBar()->clearMessage();
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::clearAnnotations()
{
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (ren) {
        for (auto &a : m_annotTextActors) if (a) ren->RemoveViewProp(a);
        for (auto &a : m_annotArrowActors) if (a) ren->RemoveActor(a);
    }
    m_annotations.clear();
    m_annotTextActors.clear();
    m_annotArrowActors.clear();
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::refreshAnnotationOverlay()
{
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!ren) return;

    for (auto &a : m_annotTextActors) if (a) ren->RemoveViewProp(a);
    for (auto &a : m_annotArrowActors) if (a) ren->RemoveActor(a);
    m_annotTextActors.clear();
    m_annotArrowActors.clear();

    for (const auto &ann : m_annotations) {
        auto textActor = vtkSmartPointer<vtkTextActor>::New();
        textActor->SetInput(ann.text.toStdString().c_str());
        textActor->GetTextProperty()->SetColor(1.0, 1.0, 0.3);
        textActor->GetTextProperty()->SetFontSize(14);
        textActor->GetTextProperty()->SetBold(true);
        textActor->GetTextProperty()->SetShadow(true);
        textActor->GetPositionCoordinate()->SetCoordinateSystemToWorld();
        textActor->SetPosition(ann.worldX, ann.worldY);
        ren->AddViewProp(textActor);
        m_annotTextActors.push_back(textActor);

        if (ann.type == Annotation::Arrow) {
            vtkNew<vtkPoints> pts;
            vtkNew<vtkCellArray> cells;
            buildArrowGeometry(pts, cells,
                               ann.worldX, ann.worldY,
                               ann.arrowEndX, ann.arrowEndY);
            vtkNew<vtkPolyData> arrowData;
            arrowData->SetPoints(pts);
            arrowData->SetLines(cells);
            vtkNew<vtkPolyDataMapper> mapper;
            mapper->SetInputData(arrowData);
            auto arrowActor = vtkSmartPointer<vtkActor>::New();
            arrowActor->SetMapper(mapper);
            arrowActor->GetProperty()->SetColor(1.0, 1.0, 0.3);
            arrowActor->GetProperty()->SetLineWidth(2.0);
            ren->AddActor(arrowActor);
            m_annotArrowActors.push_back(arrowActor);
        } else {
            m_annotArrowActors.push_back(nullptr);
        }
    }

    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::saveAnnotationsJson()
{
    if (m_annotations.empty()) {
        statusBar()->showMessage(u"No annotations to save."_s, 2000);
        return;
    }

    const QString defaultPath = annotationsJsonPath();
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save Annotations"_s,
            defaultPath.isEmpty() ? QDir::homePath() : defaultPath,
            u"JSON files (*.json);;All files (*)"_s);
    if (path.isEmpty()) return;

    QJsonArray arr;
    for (const auto &ann : m_annotations) {
        QJsonObject obj;
        obj[u"type"_s] = (ann.type == Annotation::Arrow) ? u"arrow"_s : u"text"_s;
        obj[u"text"_s] = ann.text;
        obj[u"x"_s] = ann.worldX;
        obj[u"y"_s] = ann.worldY;
        if (ann.type == Annotation::Arrow) {
            obj[u"arrowEndX"_s] = ann.arrowEndX;
            obj[u"arrowEndY"_s] = ann.arrowEndY;
        }
        arr.append(obj);
    }

    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        statusBar()->showMessage(
                QStringLiteral("Saved %1 annotations to %2")
                        .arg(m_annotations.size())
                        .arg(QFileInfo(path).fileName()),
                3000);
    } else {
        QMessageBox::warning(this, u"Save Annotations"_s,
                u"Could not write to:\n%1"_s.arg(path));
    }
}

void vtkWindowImage::loadAnnotationsJson()
{
    const QString defaultDir = filepath.isEmpty()
            ? QDir::homePath()
            : QFileInfo(filepath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
            this, u"Load Annotations"_s, defaultDir,
            u"JSON files (*.json);;All files (*)"_s);
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, u"Load Annotations"_s,
                u"Could not read:\n%1"_s.arg(path));
        return;
    }

    const QJsonArray arr = QJsonDocument::fromJson(file.readAll()).array();
    if (arr.isEmpty()) {
        statusBar()->showMessage(u"No annotations found in file."_s, 2000);
        return;
    }

    clearAnnotations();
    for (const auto &val : arr) {
        const QJsonObject obj = val.toObject();
        Annotation ann;
        ann.type = obj[u"type"_s].toString() == u"arrow"_s ? Annotation::Arrow : Annotation::Text;
        ann.text = obj[u"text"_s].toString();
        ann.worldX = obj[u"x"_s].toDouble();
        ann.worldY = obj[u"y"_s].toDouble();
        ann.arrowEndX = obj[u"arrowEndX"_s].toDouble();
        ann.arrowEndY = obj[u"arrowEndY"_s].toDouble();
        m_annotations.push_back(ann);
    }
    refreshAnnotationOverlay();
    statusBar()->showMessage(
            QStringLiteral("Loaded %1 annotations from %2")
                    .arg(m_annotations.size())
                    .arg(QFileInfo(path).fileName()),
                3000);
}

QString vtkWindowImage::annotationsJsonPath() const
{
    if (filepath.isEmpty()) return {};
    return filepath + u".annotations.json"_s;
}

// ── Blink / Compare ──────────────────────────────────────────────────────────

void vtkWindowImage::toggleBlink(bool active)
{
    if (!this->layers || this->layers->rowCount() < 2) {
        if (active) {
            statusBar()->showMessage(u"Blink requires at least two layers."_s, 2000);
            if (m_actBlink) m_actBlink->setChecked(false);
        }
        return;
    }

    if (active) {
        m_blinkState = false;
        m_blinkTimer.start(m_blinkIntervalMs);
    } else {
        m_blinkTimer.stop();
        for (int i = 0; i < this->layers->rowCount(); ++i) {
            auto *actor = this->layers->getLayerActor(i);
            if (actor) actor->SetVisibility(true);
        }
        vtkRender();
    }
}

void vtkWindowImage::blinkTick()
{
    if (!this->layers || this->layers->rowCount() < 2) {
        m_blinkTimer.stop();
        return;
    }

    m_blinkState = !m_blinkState;
    for (int i = 0; i < this->layers->rowCount(); ++i) {
        auto *actor = this->layers->getLayerActor(i);
        if (!actor) continue;
        if (i == 0)
            actor->SetVisibility(!m_blinkState);
        else if (i == 1)
            actor->SetVisibility(m_blinkState);
        else
            actor->SetVisibility(false);
    }
    vtkRender();
}

// ── Stokes / Radio polarimetry analysis ─────────────────────────────────────

bool vtkWindowImage::hasBeamInfo() const
{
    return std::isfinite(m_beamMajorDeg) && std::isfinite(m_beamMinorDeg)
            && m_beamMajorDeg > 0.0 && m_beamMinorDeg > 0.0
            && std::isfinite(this->remoteDatasetCdelt[0])
            && std::abs(this->remoteDatasetCdelt[0]) > 0.0;
}

double vtkWindowImage::beamAreaPixels() const
{
    if (!hasBeamInfo()) return 0.0;
    const double cdelt = std::abs(this->remoteDatasetCdelt[0]);
    const double bmaj_px = m_beamMajorDeg / cdelt;
    const double bmin_px = m_beamMinorDeg / cdelt;
    return M_PI * bmaj_px * bmin_px / (4.0 * std::log(2.0));
}

QString vtkWindowImage::stokesRoleLabel(StokesRole r) const
{
    switch (r) {
    case StokesRole::I:          return u"Stokes I"_s;
    case StokesRole::Q:          return u"Stokes Q"_s;
    case StokesRole::U:          return u"Stokes U"_s;
    case StokesRole::V:          return u"Stokes V"_s;
    case StokesRole::DerivedP:    return u"Pol. intensity (P)"_s;
    case StokesRole::DerivedPA:   return u"Pol. angle (PA)"_s;
    case StokesRole::DerivedFrac: return u"Frac. pol. (P/I)"_s;
    case StokesRole::DerivedPdb:  return u"P (debiased)"_s;
    case StokesRole::DerivedAlpha:return u"Spectral index (α)"_s;
    case StokesRole::DerivedRM:   return u"Faraday RM"_s;
    case StokesRole::None:
    default:                      return QString();
    }
}

int vtkWindowImage::findLayerByStokesRole(StokesRole role) const
{
    if (!this->layers) return -1;
    for (auto it = m_stokesRoles.constBegin(); it != m_stokesRoles.constEnd(); ++it) {
        if (it.value() == role && it.key() >= 0 && it.key() < this->layers->rowCount())
            return it.key();
    }
    return -1;
}

QString vtkWindowImage::findStokesCompanionPath(const QString &iPath, const QString &role) const
{
    // Pattern strict: replace "StokesI" → "Stokes<role>" in the basename.
    // Handles common variants: _StokesI_, -StokesI-, _I.fits, _I_PB.fits.
    const QFileInfo fi(iPath);
    const QString dir = fi.absolutePath();
    const QString name = fi.fileName();

    QStringList candidateNames;
    candidateNames << QString(name).replace(u"StokesI"_s, u"Stokes"_s + role);
    candidateNames << QString(name).replace(u"stokesI"_s, u"stokes"_s + role.toLower());
    candidateNames << QString(name).replace(u"STOKESI"_s, u"STOKES"_s + role);
    // ASKAP/MeerKAT patterns: _I.fits or _I_PB.fits
    candidateNames << QString(name).replace(u"_I.fits"_s, u"_"_s + role + u".fits"_s);
    candidateNames << QString(name).replace(u"_I_PB.fits"_s, u"_"_s + role + u"_PB.fits"_s);
    candidateNames << QString(name).replace(u"_I.fits.gz"_s, u"_"_s + role + u".fits.gz"_s);
    candidateNames << QString(name).replace(u"_I_PB.fits.gz"_s, u"_"_s + role + u"_PB.fits.gz"_s);
    candidateNames.removeDuplicates();
    candidateNames.removeAll(name);  // never match the I file itself

    for (const QString &candidate : candidateNames) {
        const QString full = dir + u"/"_s + candidate;
        if (QFile::exists(full)) return full;
    }
    return QString();
}

void vtkWindowImage::detectAndLoadStokesCompanions()
{
    // The master layer is treated as Stokes I.
    const int masterIdx = this->layers->getMasterIndex();
    if (!m_stokesRoles.contains(masterIdx))
        m_stokesRoles.insert(masterIdx, StokesRole::I);

    const QStringList roles = { u"Q"_s, u"U"_s, u"V"_s };
    QStringList missing;
    QStringList autoFound;

    for (const QString &role : roles) {
        if (findLayerByStokesRole(
                role == u"Q"_s ? StokesRole::Q
                : role == u"U"_s ? StokesRole::U : StokesRole::V) >= 0)
            continue;

        QString path = findStokesCompanionPath(this->filepath, role);
        if (path.isEmpty()) {
            missing.append(role);
        } else {
            autoFound.append(QStringLiteral("%1: %2").arg(role, QFileInfo(path).fileName()));
            // Tag the slot for the role-assignment hook after the layer loads
            m_pendingVlkbDisplayName = u"Stokes "_s + role;
            this->addLayerImage(path.toStdString());
            // The role will be assigned in the load-watcher callback by matching
            // m_pendingVlkbDisplayName. For now, we just record an intent.
            // Simpler: after addLayerImage we know rowCount will increase, but the
            // load is async. We'll defer role assignment to the load completion.
        }
    }

    if (!autoFound.isEmpty()) {
        statusBar()->showMessage(
                QStringLiteral("Stokes companions found: %1").arg(autoFound.join(u", "_s)),
                4000);
    }

    if (!missing.isEmpty()) {
        for (const QString &role : missing) {
            const QString path = QFileDialog::getOpenFileName(
                    this,
                    QStringLiteral("Locate Stokes %1 file").arg(role),
                    QFileInfo(this->filepath).absolutePath(),
                    u"FITS files (*.fits *.fit *.fits.gz);;All files (*)"_s);
            if (path.isEmpty()) continue;
            m_pendingVlkbDisplayName = u"Stokes "_s + role;
            this->addLayerImage(path.toStdString());
        }
    }
}

void vtkWindowImage::computeStokesP()
{
    const int qIdx = findLayerByStokesRole(StokesRole::Q);
    const int uIdx = findLayerByStokesRole(StokesRole::U);
    if (qIdx < 0 || uIdx < 0) {
        QMessageBox::warning(this, u"Polarized Intensity"_s,
                u"Need both Stokes Q and Stokes U layers loaded."_s);
        return;
    }
    auto *qData = this->layers->getImageData(qIdx);
    auto *uData = this->layers->getImageData(uIdx);
    if (!qData || !uData) return;

    int ext[6];
    qData->GetExtent(ext);
    auto pData = vtkSmartPointer<vtkImageData>::New();
    pData->SetExtent(ext);
    pData->SetOrigin(qData->GetOrigin());
    pData->SetSpacing(qData->GetSpacing());
    pData->AllocateScalars(VTK_FLOAT, 1);

    double pMin = std::numeric_limits<double>::infinity();
    double pMax = -std::numeric_limits<double>::infinity();
    auto *outPtr = static_cast<float *>(pData->GetScalarPointer());
    const int W = ext[1] - ext[0] + 1;
    for (int y = ext[2]; y <= ext[3]; ++y) {
        for (int x = ext[0]; x <= ext[1]; ++x) {
            const float q = qData->GetScalarComponentAsFloat(x, y, 0, 0);
            const float u = uData->GetScalarComponentAsFloat(x, y, 0, 0);
            float p;
            if (std::isfinite(q) && std::isfinite(u)) {
                p = std::sqrt(q * q + u * u);
                if (std::isfinite(p)) {
                    pMin = std::min(pMin, static_cast<double>(p));
                    pMax = std::max(pMax, static_cast<double>(p));
                }
            } else {
                p = std::numeric_limits<float>::quiet_NaN();
            }
            outPtr[(y - ext[2]) * W + (x - ext[0])] = p;
        }
    }
    if (!std::isfinite(pMin) || !std::isfinite(pMax) || pMin >= pMax) {
        QMessageBox::warning(this, u"Polarized Intensity"_s,
                u"Could not compute valid P map (no overlap?)."_s);
        return;
    }
    addDerivedLayer(pData, u"Stokes P"_s, StokesRole::DerivedP,
                    u"Inferno"_s, pMin, pMax);
}

void vtkWindowImage::computeStokesPA()
{
    const int qIdx = findLayerByStokesRole(StokesRole::Q);
    const int uIdx = findLayerByStokesRole(StokesRole::U);
    if (qIdx < 0 || uIdx < 0) {
        QMessageBox::warning(this, u"Polarization Angle"_s,
                u"Need both Stokes Q and Stokes U layers loaded."_s);
        return;
    }
    auto *qData = this->layers->getImageData(qIdx);
    auto *uData = this->layers->getImageData(uIdx);
    if (!qData || !uData) return;

    int ext[6];
    qData->GetExtent(ext);
    auto paData = vtkSmartPointer<vtkImageData>::New();
    paData->SetExtent(ext);
    paData->SetOrigin(qData->GetOrigin());
    paData->SetSpacing(qData->GetSpacing());
    paData->AllocateScalars(VTK_FLOAT, 1);

    auto *outPtr = static_cast<float *>(paData->GetScalarPointer());
    const int W = ext[1] - ext[0] + 1;
    for (int y = ext[2]; y <= ext[3]; ++y) {
        for (int x = ext[0]; x <= ext[1]; ++x) {
            const float q = qData->GetScalarComponentAsFloat(x, y, 0, 0);
            const float u = uData->GetScalarComponentAsFloat(x, y, 0, 0);
            float pa;
            if (std::isfinite(q) && std::isfinite(u) && (q != 0.0f || u != 0.0f)) {
                pa = static_cast<float>(0.5 * std::atan2(static_cast<double>(u),
                                                          static_cast<double>(q))
                                        * 180.0 / M_PI);
            } else {
                pa = std::numeric_limits<float>::quiet_NaN();
            }
            outPtr[(y - ext[2]) * W + (x - ext[0])] = pa;
        }
    }
    addDerivedLayer(paData, u"Pol. Angle"_s, StokesRole::DerivedPA,
                    u"Spectrum"_s, -90.0, 90.0);
}

void vtkWindowImage::computeStokesFractional()
{
    const int iIdx = findLayerByStokesRole(StokesRole::I);
    const int qIdx = findLayerByStokesRole(StokesRole::Q);
    const int uIdx = findLayerByStokesRole(StokesRole::U);
    if (iIdx < 0 || qIdx < 0 || uIdx < 0) {
        QMessageBox::warning(this, u"Fractional Polarization"_s,
                u"Need Stokes I, Q, and U layers loaded."_s);
        return;
    }
    auto *iData = this->layers->getImageData(iIdx);
    auto *qData = this->layers->getImageData(qIdx);
    auto *uData = this->layers->getImageData(uIdx);
    if (!iData || !qData || !uData) return;

    int ext[6];
    iData->GetExtent(ext);
    auto fData = vtkSmartPointer<vtkImageData>::New();
    fData->SetExtent(ext);
    fData->SetOrigin(iData->GetOrigin());
    fData->SetSpacing(iData->GetSpacing());
    fData->AllocateScalars(VTK_FLOAT, 1);

    auto *outPtr = static_cast<float *>(fData->GetScalarPointer());
    const int W = ext[1] - ext[0] + 1;
    double fMin = std::numeric_limits<double>::infinity();
    double fMax = -std::numeric_limits<double>::infinity();
    for (int y = ext[2]; y <= ext[3]; ++y) {
        for (int x = ext[0]; x <= ext[1]; ++x) {
            const float i = iData->GetScalarComponentAsFloat(x, y, 0, 0);
            const float q = qData->GetScalarComponentAsFloat(x, y, 0, 0);
            const float u = uData->GetScalarComponentAsFloat(x, y, 0, 0);
            float f;
            if (std::isfinite(i) && std::isfinite(q) && std::isfinite(u)
                && std::abs(i) > 1e-30f) {
                const float p = std::sqrt(q * q + u * u);
                f = p / i;
                if (std::isfinite(f)) {
                    fMin = std::min(fMin, static_cast<double>(f));
                    fMax = std::max(fMax, static_cast<double>(f));
                }
            } else {
                f = std::numeric_limits<float>::quiet_NaN();
            }
            outPtr[(y - ext[2]) * W + (x - ext[0])] = f;
        }
    }
    if (!std::isfinite(fMin) || !std::isfinite(fMax) || fMin >= fMax) {
        QMessageBox::warning(this, u"Fractional Polarization"_s,
                u"Could not compute valid P/I map."_s);
        return;
    }
    // Clamp display range to a reasonable polarization fraction [0, 1].
    fMax = std::min(fMax, 1.0);
    addDerivedLayer(fData, u"Frac. Pol."_s, StokesRole::DerivedFrac,
                    u"Viridis"_s, std::max(0.0, fMin), fMax);
}

// ── Bias-corrected P: P_debiased = √(max(0, Q² + U² − σ²)) ─────────────────
// σ is the per-channel noise. We estimate it as the MAD-σ of the Q and U
// pixel distributions averaged together. This is the simplest "ad-hoc"
// debiasing; for rigorous work the noise map should be supplied.
void vtkWindowImage::computeStokesPdebiased()
{
    const int qIdx = findLayerByStokesRole(StokesRole::Q);
    const int uIdx = findLayerByStokesRole(StokesRole::U);
    if (qIdx < 0 || uIdx < 0) {
        QMessageBox::warning(this, u"Debiased P"_s,
                u"Need both Stokes Q and Stokes U layers loaded."_s);
        return;
    }
    auto *qData = this->layers->getImageData(qIdx);
    auto *uData = this->layers->getImageData(uIdx);
    if (!qData || !uData) return;

    const auto madSigmaOf = [](vtkImageData *d) -> double {
        int ext[6]; d->GetExtent(ext);
        std::vector<double> vals;
        vals.reserve(static_cast<std::size_t>((ext[1] - ext[0] + 1)
                                              * (ext[3] - ext[2] + 1)));
        for (int y = ext[2]; y <= ext[3]; ++y)
            for (int x = ext[0]; x <= ext[1]; ++x) {
                const double v = d->GetScalarComponentAsDouble(x, y, 0, 0);
                if (std::isfinite(v)) vals.push_back(v);
            }
        if (vals.empty()) return 0.0;
        std::sort(vals.begin(), vals.end());
        const double med = vals[vals.size() / 2];
        std::vector<double> dev;
        dev.reserve(vals.size());
        for (double v : vals) dev.push_back(std::abs(v - med));
        std::sort(dev.begin(), dev.end());
        return 1.4826 * dev[dev.size() / 2];
    };
    const double sigmaQ = madSigmaOf(qData);
    const double sigmaU = madSigmaOf(uData);
    const double sigma = 0.5 * (sigmaQ + sigmaU);
    const double sigma2 = sigma * sigma;

    if (sigma <= 0.0) {
        QMessageBox::warning(this, u"Debiased P"_s,
                u"Could not estimate noise from Q/U layers."_s);
        return;
    }

    int ext[6];
    qData->GetExtent(ext);
    auto pData = vtkSmartPointer<vtkImageData>::New();
    pData->SetExtent(ext);
    pData->SetOrigin(qData->GetOrigin());
    pData->SetSpacing(qData->GetSpacing());
    pData->AllocateScalars(VTK_FLOAT, 1);

    auto *outPtr = static_cast<float *>(pData->GetScalarPointer());
    const int W = ext[1] - ext[0] + 1;
    double pMin = std::numeric_limits<double>::infinity();
    double pMax = -std::numeric_limits<double>::infinity();
    for (int y = ext[2]; y <= ext[3]; ++y) {
        for (int x = ext[0]; x <= ext[1]; ++x) {
            const float q = qData->GetScalarComponentAsFloat(x, y, 0, 0);
            const float u = uData->GetScalarComponentAsFloat(x, y, 0, 0);
            float p;
            if (std::isfinite(q) && std::isfinite(u)) {
                const double raw = q * q + u * u - sigma2;
                p = (raw > 0.0) ? static_cast<float>(std::sqrt(raw)) : 0.0f;
                pMin = std::min(pMin, static_cast<double>(p));
                pMax = std::max(pMax, static_cast<double>(p));
            } else {
                p = std::numeric_limits<float>::quiet_NaN();
            }
            outPtr[(y - ext[2]) * W + (x - ext[0])] = p;
        }
    }
    if (!std::isfinite(pMin) || pMin >= pMax) {
        QMessageBox::warning(this, u"Debiased P"_s,
                u"Could not compute a valid debiased P map."_s);
        return;
    }
    addDerivedLayer(pData,
            QStringLiteral("P_deb (σ=%1)").arg(sigma, 0, 'g', 4),
            StokesRole::DerivedPdb, u"Inferno"_s, pMin, pMax);
    statusBar()->showMessage(
            QStringLiteral("Debiased P: σ_Q=%1, σ_U=%2, σ=%3")
                    .arg(sigmaQ, 0, 'g', 4).arg(sigmaU, 0, 'g', 4).arg(sigma, 0, 'g', 4),
            5000);
}

// ── Spectral index map: α = log(S_B/S_A) / log(ν_B/ν_A) ───────────────────
// Per-pixel formula on two layers at different frequencies. Both layers
// must share the same pixel grid (same WCS, same extent).
void vtkWindowImage::computeSpectralIndex()
{
    if (!this->layers || this->layers->rowCount() < 2) {
        QMessageBox::warning(this, u"Spectral Index"_s,
                u"Need at least two layers loaded."_s);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(u"Compute Spectral Index"_s);
    auto *form = new QFormLayout(&dlg);
    auto *comboA = new QComboBox(&dlg);
    auto *comboB = new QComboBox(&dlg);
    for (int i = 0; i < this->layers->rowCount(); ++i) {
        const QString name = this->layers->data(
                this->layers->index(i, 0), Qt::DisplayRole).toString();
        comboA->addItem(name);
        comboB->addItem(name);
    }
    comboA->setCurrentIndex(0);
    comboB->setCurrentIndex(std::min(1, this->layers->rowCount() - 1));

    auto *freqA = new QLineEdit(u"1.284"_s, &dlg);
    freqA->setValidator(new QDoubleValidator(1e-9, 1e9, 6, &dlg));
    auto *freqB = new QLineEdit(u"1.400"_s, &dlg);
    freqB->setValidator(new QDoubleValidator(1e-9, 1e9, 6, &dlg));

    form->addRow(u"Layer A:"_s, comboA);
    form->addRow(u"Frequency A (GHz):"_s, freqA);
    form->addRow(u"Layer B:"_s, comboB);
    form->addRow(u"Frequency B (GHz):"_s, freqB);

    auto *btns = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(btns);

    if (dlg.exec() != QDialog::Accepted) return;

    const int idxA = comboA->currentIndex();
    const int idxB = comboB->currentIndex();
    const double nuA = freqA->text().toDouble();
    const double nuB = freqB->text().toDouble();
    if (idxA == idxB || nuA <= 0.0 || nuB <= 0.0 || std::abs(nuA - nuB) < 1e-9) {
        QMessageBox::warning(this, u"Spectral Index"_s,
                u"Pick two distinct layers and two distinct positive frequencies."_s);
        return;
    }

    auto *dataA = this->layers->getImageData(idxA);
    auto *dataB = this->layers->getImageData(idxB);
    if (!dataA || !dataB) return;

    int extA[6], extB[6];
    dataA->GetExtent(extA);
    dataB->GetExtent(extB);
    if (extA[0] != extB[0] || extA[1] != extB[1]
        || extA[2] != extB[2] || extA[3] != extB[3]) {
        QMessageBox::warning(this, u"Spectral Index"_s,
                u"Layers must share the same pixel grid. Use the cube viewer's "
                u"moment-map export to produce co-registered images."_s);
        return;
    }

    auto aData = vtkSmartPointer<vtkImageData>::New();
    aData->SetExtent(extA);
    aData->SetOrigin(dataA->GetOrigin());
    aData->SetSpacing(dataA->GetSpacing());
    aData->AllocateScalars(VTK_FLOAT, 1);

    const double logNuRatio = std::log(nuB / nuA);
    auto *outPtr = static_cast<float *>(aData->GetScalarPointer());
    const int W = extA[1] - extA[0] + 1;
    double aMin = std::numeric_limits<double>::infinity();
    double aMax = -std::numeric_limits<double>::infinity();
    int nValid = 0;
    for (int y = extA[2]; y <= extA[3]; ++y) {
        for (int x = extA[0]; x <= extA[1]; ++x) {
            const float ia = dataA->GetScalarComponentAsFloat(x, y, 0, 0);
            const float ib = dataB->GetScalarComponentAsFloat(x, y, 0, 0);
            float alpha;
            if (std::isfinite(ia) && std::isfinite(ib) && ia > 0.0f && ib > 0.0f) {
                alpha = static_cast<float>(
                        std::log(static_cast<double>(ib) / static_cast<double>(ia))
                        / logNuRatio);
                if (std::isfinite(alpha)) {
                    aMin = std::min(aMin, static_cast<double>(alpha));
                    aMax = std::max(aMax, static_cast<double>(alpha));
                    ++nValid;
                }
            } else {
                alpha = std::numeric_limits<float>::quiet_NaN();
            }
            outPtr[(y - extA[2]) * W + (x - extA[0])] = alpha;
        }
    }
    if (nValid == 0) {
        QMessageBox::warning(this, u"Spectral Index"_s,
                u"No pixel with positive flux in both layers."_s);
        return;
    }
    // Clamp display to physically reasonable α range for synchrotron + thermal.
    addDerivedLayer(aData,
            QStringLiteral("α (%1↔%2 GHz)").arg(nuA, 0, 'f', 2).arg(nuB, 0, 'f', 2),
            StokesRole::DerivedAlpha, u"Spectrum"_s, -2.5, 1.5);
    statusBar()->showMessage(
            QStringLiteral("Spectral index: %1 valid pixels, range %2 — %3")
                    .arg(nValid).arg(aMin, 0, 'f', 2).arg(aMax, 0, 'f', 2),
            5000);
}

// ── Faraday Rotation Measure: per-pixel linear fit of PA vs λ² ─────────────
// Requires ≥ 3 frequency pairs (Q_i, U_i, ν_i). For each pixel:
//   PA_i = ½·atan2(U_i, Q_i)
//   λ_i² = (c/ν_i)²
//   linear fit:  PA = PA₀ + RM · λ²   →  slope = RM (rad m⁻²)
// NOTE: no PA unwrapping — only valid in the "low-RM" regime where the n·π
// ambiguity does not flip between channels. For aggressive RM use the
// future backend-side RM synthesis tool.
void vtkWindowImage::computeFaradayRM()
{
    if (!this->layers || this->layers->rowCount() < 6) {  // ≥ 3 pairs × 2
        QMessageBox::warning(this, u"Faraday RM"_s,
                u"Need at least 3 Q/U pairs (6 layers minimum) at different frequencies."_s);
        return;
    }

    // Modal dialog with a table of (Q layer, U layer, frequency GHz) rows
    QDialog dlg(this);
    dlg.setWindowTitle(u"Compute Faraday Rotation Measure"_s);
    auto *vl = new QVBoxLayout(&dlg);

    auto *intro = new QLabel(
            u"Add ≥ 3 (Q, U, ν) triplets at different frequencies. The tool will "
            u"compute PA per pixel for each, then fit a line PA vs λ². The slope "
            u"is RM (rad m⁻²). No PA unwrapping — works only for low-RM regimes."_s,
            &dlg);
    intro->setWordWrap(true);
    vl->addWidget(intro);

    auto *table = new QTableWidget(0, 3, &dlg);
    table->setHorizontalHeaderLabels({ u"Stokes Q layer"_s, u"Stokes U layer"_s,
                                       u"ν (GHz)"_s });
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    vl->addWidget(table);

    auto *btnRow = new QHBoxLayout();
    auto *btnAdd = new QPushButton(u"Add row"_s, &dlg);
    auto *btnDel = new QPushButton(u"Remove row"_s, &dlg);
    btnRow->addWidget(btnAdd);
    btnRow->addWidget(btnDel);
    btnRow->addStretch(1);
    vl->addLayout(btnRow);

    const auto addRow = [this, table]() {
        const int r = table->rowCount();
        table->insertRow(r);
        auto *cQ = new QComboBox();
        auto *cU = new QComboBox();
        for (int i = 0; i < this->layers->rowCount(); ++i) {
            const QString name = this->layers->data(
                    this->layers->index(i, 0), Qt::DisplayRole).toString();
            cQ->addItem(name, i);
            cU->addItem(name, i);
        }
        table->setCellWidget(r, 0, cQ);
        table->setCellWidget(r, 1, cU);
        auto *e = new QLineEdit(u"1.284"_s);
        e->setValidator(new QDoubleValidator(1e-9, 1e9, 6));
        table->setCellWidget(r, 2, e);
    };
    QObject::connect(btnAdd, &QPushButton::clicked, &dlg, addRow);
    QObject::connect(btnDel, &QPushButton::clicked, &dlg, [table]() {
        const int r = table->currentRow();
        if (r >= 0) table->removeRow(r);
    });

    // Seed 3 rows
    addRow(); addRow(); addRow();

    auto *btns = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    vl->addWidget(btns);
    dlg.resize(520, 340);

    if (dlg.exec() != QDialog::Accepted) return;

    struct FreqPair {
        vtkImageData *q{ nullptr };
        vtkImageData *u{ nullptr };
        double lambda2{ 0.0 };  // m²
    };
    std::vector<FreqPair> pairs;
    for (int r = 0; r < table->rowCount(); ++r) {
        auto *cQ = qobject_cast<QComboBox *>(table->cellWidget(r, 0));
        auto *cU = qobject_cast<QComboBox *>(table->cellWidget(r, 1));
        auto *eF = qobject_cast<QLineEdit *>(table->cellWidget(r, 2));
        if (!cQ || !cU || !eF) continue;
        const int qi = cQ->currentData().toInt();
        const int ui = cU->currentData().toInt();
        const double nuGHz = eF->text().toDouble();
        if (qi == ui || nuGHz <= 0.0) continue;
        const double nuHz = nuGHz * 1e9;
        constexpr double c_m_per_s = 299792458.0;
        const double lambda = c_m_per_s / nuHz;
        FreqPair fp;
        fp.q = this->layers->getImageData(qi);
        fp.u = this->layers->getImageData(ui);
        fp.lambda2 = lambda * lambda;
        if (fp.q && fp.u) pairs.push_back(fp);
    }
    if (pairs.size() < 3) {
        QMessageBox::warning(this, u"Faraday RM"_s,
                u"Need at least 3 valid Q/U pairs at distinct frequencies."_s);
        return;
    }

    // Verify all share the same extent
    int ext0[6];
    pairs[0].q->GetExtent(ext0);
    for (const auto &p : pairs) {
        int e[6];
        p.q->GetExtent(e);
        if (e[0] != ext0[0] || e[1] != ext0[1] || e[2] != ext0[2] || e[3] != ext0[3]) {
            QMessageBox::warning(this, u"Faraday RM"_s,
                    u"All selected layers must share the same pixel grid."_s);
            return;
        }
    }

    // Pre-compute λ² stats for the linear fit
    const int N = static_cast<int>(pairs.size());
    double meanL2 = 0.0;
    for (const auto &p : pairs) meanL2 += p.lambda2;
    meanL2 /= N;
    double sumL2_dev2 = 0.0;
    for (const auto &p : pairs) {
        const double d = p.lambda2 - meanL2;
        sumL2_dev2 += d * d;
    }
    if (sumL2_dev2 <= 0.0) {
        QMessageBox::warning(this, u"Faraday RM"_s,
                u"Selected frequencies are too close to fit a linear RM."_s);
        return;
    }

    auto rmData = vtkSmartPointer<vtkImageData>::New();
    rmData->SetExtent(ext0);
    rmData->SetOrigin(pairs[0].q->GetOrigin());
    rmData->SetSpacing(pairs[0].q->GetSpacing());
    rmData->AllocateScalars(VTK_FLOAT, 1);

    auto *outPtr = static_cast<float *>(rmData->GetScalarPointer());
    const int W = ext0[1] - ext0[0] + 1;
    double rmMin = std::numeric_limits<double>::infinity();
    double rmMax = -std::numeric_limits<double>::infinity();
    int nValid = 0;

    std::vector<double> paBuf(static_cast<std::size_t>(N));
    for (int y = ext0[2]; y <= ext0[3]; ++y) {
        for (int x = ext0[0]; x <= ext0[1]; ++x) {
            bool allFinite = true;
            for (int k = 0; k < N; ++k) {
                const float q = pairs[k].q->GetScalarComponentAsFloat(x, y, 0, 0);
                const float u = pairs[k].u->GetScalarComponentAsFloat(x, y, 0, 0);
                if (!std::isfinite(q) || !std::isfinite(u)) {
                    allFinite = false;
                    break;
                }
                paBuf[k] = 0.5 * std::atan2(static_cast<double>(u),
                                            static_cast<double>(q));
            }
            float rm;
            if (allFinite) {
                // Linear fit slope:  Σ(x_i − x̄)(y_i − ȳ) / Σ(x_i − x̄)²
                double meanPA = 0.0;
                for (double v : paBuf) meanPA += v;
                meanPA /= N;
                double num = 0.0;
                for (int k = 0; k < N; ++k) {
                    num += (pairs[k].lambda2 - meanL2) * (paBuf[k] - meanPA);
                }
                const double slope = num / sumL2_dev2;
                rm = static_cast<float>(slope);  // rad m⁻²
                rmMin = std::min(rmMin, static_cast<double>(rm));
                rmMax = std::max(rmMax, static_cast<double>(rm));
                ++nValid;
            } else {
                rm = std::numeric_limits<float>::quiet_NaN();
            }
            outPtr[(y - ext0[2]) * W + (x - ext0[0])] = rm;
        }
    }
    if (nValid == 0) {
        QMessageBox::warning(this, u"Faraday RM"_s,
                u"No pixel had finite Q and U at all selected frequencies."_s);
        return;
    }
    // Clip the display range to ±5σ_MAD of the RM distribution to keep colours
    // useful even when a handful of pixels have outlier RM values.
    addDerivedLayer(rmData,
            QStringLiteral("RM (N=%1)").arg(N),
            StokesRole::DerivedRM, u"Spectrum"_s,
            std::max(-500.0, rmMin), std::min(500.0, rmMax));
    statusBar()->showMessage(
            QStringLiteral("Faraday RM: %1 pixels, range %2 — %3 rad m⁻² (clipped to ±500 for display)")
                    .arg(nValid).arg(rmMin, 0, 'f', 1).arg(rmMax, 0, 'f', 1),
            6000);
}

void vtkWindowImage::addDerivedLayer(vtkSmartPointer<vtkImageData> data,
                                      const QString &name, StokesRole role,
                                      const QString &colorMap,
                                      double rangeMin, double rangeMax)
{
    ImageLayerLoadResult result;
    result.valid = true;
    result.filepath = name.toStdString();
    result.imageData = data;
    result.scalarRange = { rangeMin, rangeMax };

    this->applyLoadedLayer(result);

    const int newIdx = this->layers->rowCount() - 1;
    m_stokesRoles.insert(newIdx, role);
    this->setLayerDisplayName(newIdx, name);

    if (auto *lut = this->layers->getLookupTable(newIdx)) {
        ColorMaps::SetColorMap(lut, colorMap.toStdString());
        lut->SetTableRange(rangeMin, rangeMax);
        lut->SetNanColor(0.0, 0.0, 0.0, 0.0);
        lut->Build();
    }
    this->vtkRender();
    statusBar()->showMessage(
            QStringLiteral("Derived map added: %1 (range %2 — %3)")
                    .arg(name)
                    .arg(rangeMin, 0, 'g', 4)
                    .arg(rangeMax, 0, 'g', 4),
            4000);
}

void vtkWindowImage::rebuildPolarizationVectors()
{
    const int qIdx = findLayerByStokesRole(StokesRole::Q);
    const int uIdx = findLayerByStokesRole(StokesRole::U);
    if (qIdx < 0 || uIdx < 0) {
        m_polVecActor->VisibilityOff();
        return;
    }
    auto *qData = this->layers->getImageData(qIdx);
    auto *uData = this->layers->getImageData(uIdx);
    if (!qData || !uData) {
        m_polVecActor->VisibilityOff();
        return;
    }

    int ext[6];
    qData->GetExtent(ext);

    m_polVecPoints->Reset();
    m_polVecCells->Reset();

    // First pass: collect P values, find robust max for normalization.
    double pMax = 0.0;
    std::vector<double> pSamples;
    for (int y = ext[2]; y <= ext[3]; y += m_polVecGridStep) {
        for (int x = ext[0]; x <= ext[1]; x += m_polVecGridStep) {
            const float q = qData->GetScalarComponentAsFloat(x, y, 0, 0);
            const float u = uData->GetScalarComponentAsFloat(x, y, 0, 0);
            if (std::isfinite(q) && std::isfinite(u)) {
                const double p = std::sqrt(q * q + u * u);
                pSamples.push_back(p);
                pMax = std::max(pMax, p);
            }
        }
    }
    if (pSamples.empty() || pMax <= 0.0) {
        m_polVecActor->VisibilityOff();
        return;
    }

    // Robust noise estimate from the sample set (median absolute deviation).
    std::vector<double> sorted = pSamples;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    std::vector<double> dev;
    dev.reserve(sorted.size());
    for (double v : sorted) dev.push_back(std::abs(v - median));
    std::sort(dev.begin(), dev.end());
    const double sigma = 1.4826 * dev[dev.size() / 2];
    const double pThreshold = sigma * m_polVecSnrMin;

    const double maxLen = m_polVecGridStep * 0.9 * m_polVecScale;

    int nDrawn = 0;
    for (int y = ext[2]; y <= ext[3]; y += m_polVecGridStep) {
        for (int x = ext[0]; x <= ext[1]; x += m_polVecGridStep) {
            const float q = qData->GetScalarComponentAsFloat(x, y, 0, 0);
            const float u = uData->GetScalarComponentAsFloat(x, y, 0, 0);
            if (!std::isfinite(q) || !std::isfinite(u)) continue;
            const double p = std::sqrt(q * q + u * u);
            if (p < pThreshold) continue;

            // Radio convention: PA measured from N through E.
            // In image pixel coords with +y = north and +x = east (typical
            // for FITS with CDELT1<0), the vector orientation is:
            //   dx = -sin(PA), dy = +cos(PA)
            const double pa_rad = 0.5 * std::atan2(static_cast<double>(u),
                                                    static_cast<double>(q));
            const double len = 0.5 * (p / pMax) * maxLen;
            const double dx = -len * std::sin(pa_rad);
            const double dy = +len * std::cos(pa_rad);

            const vtkIdType id1 = m_polVecPoints->InsertNextPoint(x - dx, y - dy, 0.0);
            const vtkIdType id2 = m_polVecPoints->InsertNextPoint(x + dx, y + dy, 0.0);
            m_polVecCells->InsertNextCell(2);
            m_polVecCells->InsertCellPoint(id1);
            m_polVecCells->InsertCellPoint(id2);
            ++nDrawn;
        }
    }

    m_polVecData->SetPoints(m_polVecPoints);
    m_polVecData->SetLines(m_polVecCells);
    m_polVecData->Modified();

    statusBar()->showMessage(
            QStringLiteral("Polarization vectors: %1 (step=%2 px, SNR≥%3, σ_P≈%4)")
                    .arg(nDrawn).arg(m_polVecGridStep)
                    .arg(m_polVecSnrMin, 0, 'f', 1).arg(sigma, 0, 'g', 4),
            5000);
}

void vtkWindowImage::setPolVectorsVisible(bool vis)
{
    m_polVecsVisible = vis;
    if (vis) {
        rebuildPolarizationVectors();
    }
    m_polVecActor->SetVisibility(vis);
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

// ── VLKB layer support ────────────────────────────────────────────────────────

void vtkWindowImage::addLayerFromBackendPath(const QString &backendPath,
                                              const QString &displayName)
{
    if (this->isBusy()) {
        qWarning().noquote()
                << QStringLiteral("[vlkb-layer] viewer busy, ignoring addLayerFromBackendPath");
        return;
    }
    // Reuse the standard addLayerImage pipeline (vtkFITSReader + AstroUtils/libwcs).
    // libwcs handles CD matrix correctly — no custom WCS math needed.
    // The VLKB cutout FITS is saved on the backend filesystem; when the backend
    // runs on the same machine as the client (the common case), backendPath is
    // directly accessible as a local filesystem path.
    m_pendingVlkbDisplayName = displayName;
    this->addLayerImage(backendPath.toStdString());
    // Override the generic "Loading layer..." message with the VLKB-specific label.
    this->showPersistentStatusMessage(u"Loading VLKB layer \"%1\"..."_s.arg(displayName));
}

void vtkWindowImage::setLayerDisplayName(int row, const QString &name)
{
    if (this->layers)
        this->layers->setLayerDisplayName(row, name);
}
