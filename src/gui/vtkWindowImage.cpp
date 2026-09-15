#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeWcsUtil.h"
#include "CubeViewportGeometry.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "CatalogueTableModel.h"
#include "ColorMaps.h"
#include "ColormapIcon.h"
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
#include "WorkspaceChrome.h"
#include "ProductRegistry.h"
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
#include <QUrl>
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
#include <QThreadPool>
#include <QtConcurrentRun>

#include "AstroUtils.h"
#include "vtk/ImageLod.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <QCoreApplication>
#include <QRandomGenerator>

using namespace Qt::StringLiterals;
using namespace CubeWcs;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

namespace {

/// Returns at most the last two path segments of @p fullPath, preceded by "…/".
/// E.g. "/home/user/data/fits" → "…/data/fits".  Paths with ≤ 2 segments are
/// returned unchanged.

constexpr qint64 largeImagePixelThreshold = 25000000;
constexpr int initialAutoscaleTargetSamples = 250000;
constexpr double initialAutoscaleLowPercent = 1.0;
constexpr double initialAutoscaleHighPercent = 99.5;


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
    // The mapping displayed -> source, as the BACKEND actually sampled: origin
    // too, not just spacing. Everything else in this window (overlays, probe
    // readout, WCS labels, extracted spectra) reads this geometry, so this is
    // the single place that decides whether they are on the right pixel.
    ImageLod::SourceMapping mapping;
    if (response.isPreview && response.width > 0 && response.height > 0 && response.fullWidth > 0
        && response.fullHeight > 0) {
        mapping = ImageLod::previewMapping(response.fullWidth, response.fullHeight, response.width,
                                           response.height);
    }
    image->SetOrigin(mapping.originX, mapping.originY, 0.);
    image->SetSpacing(mapping.scaleX, mapping.scaleY, 1.);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(image->GetScalarPointer(), response.data.constData(),
                static_cast<std::size_t>(expectedBytes));

    result.imageData = image;
    result.scalarRange = { response.rangeMin, response.rangeMax };
    // …and on the RESULT, not only on the image: ImageLayer::applyLoadResult
    // re-applies result.spacing/origin over whatever the image carries, so
    // leaving them at the {1,1,1}/{0,0,0} defaults threw the preview's
    // decimation away entirely - every overlay and readout on a preview was off
    // by the whole decimation factor, not by the half cell the spacing formula
    // was wrong by.
    result.spacing = { mapping.scaleX, mapping.scaleY, 1.0 };
    result.origin = { mapping.originX, mapping.originY, 0.0 };
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

// Outcome of assembling a set of tiles: ok/error + the folded finite value range
// (degenerate all-NaN [0,0] tiles are skipped so they don't drag the range to 0).
struct TileAssembleOutcome
{
    bool ok{ true };
    QString error;
    double rmin{ 0.0 };
    double rmax{ 0.0 };
    bool haveRange{ false };
};

// Fetch every tile in `coords` at `level` and blit it via `blit`, fanned out
// across up to 8 parallel chunks (each its own BackendClient — value-copied
// inputs only; blits are lock-free since every tile writes a disjoint rectangle
// of the shared destination buffer). `blit(tile)` returns false on a malformed
// tile. Shared by the whole-image and viewport assemblers. On a chunk failure
// the outcome carries the first error but ALL chunks are still joined before
// returning (the caller discards the half-built image).
template <typename BlitFn>
TileAssembleOutcome fetchAndBlitTiles(const QString &backendUrl, const QString &datasetId,
                                      const QString &sessionId, const QString &backendToken,
                                      int level, int tileSize,
                                      const QVector<QPair<int, int>> &coords, BlitFn blit)
{
    TileAssembleOutcome out;
    if (coords.isEmpty()) {
        return out;
    }

    struct ChunkResult
    {
        bool ok{ true };
        QString error;
        double rmin{ 0.0 };
        double rmax{ 0.0 };
        bool haveRange{ false };
    };
    const auto processChunk =
            [backendUrl, datasetId, sessionId, backendToken, tileSize, level, blit](
                    const QVector<QPair<int, int>> &chunk) -> ChunkResult {
        ChunkResult cr;
        BackendClient c(backendUrl, backendToken);
        c.setSessionId(sessionId);
        for (const auto &xy : chunk) {
            const auto t = c.requestImageTile(datasetId, level, xy.first, xy.second, tileSize);
            if (!t.valid || !blit(t)) {
                cr.ok = false;
                cr.error = t.error.isEmpty() ? u"Remote image tile assembly failed."_s : t.error;
                return cr;
            }
            if (t.rangeMin < t.rangeMax) {
                if (!cr.haveRange) {
                    cr.rmin = t.rangeMin;
                    cr.rmax = t.rangeMax;
                    cr.haveRange = true;
                } else {
                    cr.rmin = std::min(cr.rmin, t.rangeMin);
                    cr.rmax = std::max(cr.rmax, t.rangeMax);
                }
            }
        }
        return cr;
    };

    const auto foldRange = [&](double lo, double hi, bool have) {
        if (!have) {
            return;
        }
        if (!out.haveRange) {
            out.rmin = lo;
            out.rmax = hi;
            out.haveRange = true;
        } else {
            out.rmin = std::min(out.rmin, lo);
            out.rmax = std::max(out.rmax, hi);
        }
    };

    constexpr int kConcurrency = 8;
    const int nChunks = std::min(kConcurrency, static_cast<int>(coords.size()));
    QVector<QVector<QPair<int, int>>> chunks(nChunks);
    for (int i = 0; i < coords.size(); ++i) {
        chunks[i % nChunks].append(coords[i]);
    }
    QThreadPool pool;
    pool.setMaxThreadCount(nChunks);
    QVector<QFuture<ChunkResult>> futures;
    futures.reserve(nChunks);
    for (const auto &chunk : chunks) {
        futures.append(QtConcurrent::run(&pool, processChunk, chunk));
    }
    for (auto &f : futures) {
        f.waitForFinished();
        const ChunkResult cr = f.result();
        if (!cr.ok) {
            out.ok = false;
            if (out.error.isEmpty()) {
                out.error = cr.error;
            }
        } else {
            foldRange(cr.rmin, cr.rmax, cr.haveRange);
        }
    }
    return out;
}

// Assemble the full-resolution image from level-0 pyramid tiles. Each tile is a
// bounded out-of-core read on the backend, so a mosaic too large for the backend
// to load in one /v1/image/full call can still be served tile-by-tile. The
// client-side vtkImageData is full size (same as the single-request full load);
// display downsampling is handled downstream by ImageLayer.
ImageLayerLoadResult fetchRemoteImageLayerTiled(const QString &backendUrl, const QString &datasetId,
                                                const QString &datasetPath, int tileSize,
                                                int requestGeneration, const QString &sessionId,
                                                const QString &backendToken)
{
    ImageLayerLoadResult result;
    result.filepath = datasetPath.toStdString();
    result.requestGeneration = requestGeneration;

    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);

    // Probe tile (0,0) to learn full dims + tile grid before allocating.
    const auto probe = client.requestImageTile(datasetId, 0, 0, 0, tileSize);
    if (!probe.valid) {
        result.errorMessage = probe.error.isEmpty() ? "Remote image tile request failed."
                                                     : probe.error.toStdString();
        return result;
    }
    if (probe.scalarType != u"float32"_s) {
        result.errorMessage = "Unsupported remote image scalar type.";
        return result;
    }
    const int fullW = probe.fullWidth;
    const int fullH = probe.fullHeight;
    const int ts = probe.tileSize > 0 ? probe.tileSize : tileSize;
    if (fullW <= 0 || fullH <= 0 || ts <= 0) {
        result.errorMessage = "Invalid remote image dimensions.";
        return result;
    }

    // Assemble the WHOLE image at the coarsest pyramid level that fits a pixel
    // budget (the display is downsampled to ≤1536px anyway, so a huge mosaic
    // need not materialise full-res; this also lets arbitrarily large mosaics
    // open). 64 MP ≈ 256 MB float32; images ≤ budget keep level 0 (full res).
    // Coordinate readouts (probe / region / measurement) map the decimated grid
    // back to full-res pixels, so science stays correct.
    constexpr long long kMaxAssemblePixels = 64'000'000LL;
    const int level =
            ImageLod::chooseAssemblyLevel(fullW, fullH, probe.numLevels, kMaxAssemblePixels);
    const int step = 1 << level;
    const int downW = (fullW + step - 1) / step;
    const int downH = (fullH + step - 1) / step;
    // Spacing maps the decimated grid onto the full-resolution world extent
    // (preview convention). Set on the image AND result.spacing, since
    // ImageLayer::applyLoadResult re-applies result.spacing to the image.
    // Decimated tiles are a STRIDE read on the backend: displayed pixel i is
    // source pixel i·2^level, exactly. Stretching the spacing to land the last
    // displayed pixel on source fullW-1 (the old formula) put every coordinate
    // slightly off, by up to one source pixel at the far edge.
    const auto mapping = ImageLod::assemblyMapping(level);
    const double sx = mapping.scaleX;
    const double sy = mapping.scaleY;

    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, downW - 1, 0, downH - 1, 0, 0);
    image->SetOrigin(mapping.originX, mapping.originY, 0.);
    image->SetSpacing(sx, sy, 1.);
    image->AllocateScalars(VTK_FLOAT, 1);
    float *const buf = static_cast<float *>(image->GetScalarPointer());

    // Copy a tile into the decimated buffer at its (tileX, tileY) offset (in
    // level-`level` pixels). Captured by VALUE (buf pointer + dims) so it is safe
    // from worker threads: every tile writes a disjoint rectangle, no locking.
    const auto blit = [buf, downW, downH, ts](const BackendImageTileResult &t) -> bool {
        const qsizetype expected =
                static_cast<qsizetype>(t.width) * t.height * static_cast<qsizetype>(sizeof(float));
        if (t.width <= 0 || t.height <= 0 || t.data.size() != expected) {
            return false;
        }
        const int x0 = t.tileX * ts;
        const int y0 = t.tileY * ts;
        if (x0 + t.width > downW || y0 + t.height > downH) {
            return false;
        }
        const float *src = reinterpret_cast<const float *>(t.data.constData());
        for (int r = 0; r < t.height; ++r) {
            std::memcpy(buf + (static_cast<qsizetype>(y0 + r) * downW + x0),
                        src + static_cast<qsizetype>(r) * t.width,
                        static_cast<std::size_t>(t.width) * sizeof(float));
        }
        return true;
    };

    // Every tile at the chosen level (the probe was level-0 metadata only).
    const auto counts = ImageLod::tileCounts(level, { fullW, fullH, ts, probe.numLevels });
    const int cols = counts.first;
    const int rows = counts.second;
    QVector<QPair<int, int>> coords;
    coords.reserve(cols * rows);
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            coords.append({ tx, ty });
        }
    }

    // Parallel fan-out: the backend serves tile POSTs concurrently, so a bounded
    // burst turns hundreds of serial round-trips into ~wall-clock/kConcurrency.
    const auto outcome = fetchAndBlitTiles(backendUrl, datasetId, sessionId, backendToken,
                                           level, ts, coords, blit);
    if (!outcome.ok) {
        result.errorMessage = outcome.error.toStdString();
        return result;
    }
    const int concurrency = coords.isEmpty() ? 1 : std::min(8, static_cast<int>(coords.size()));

    result.imageData = image;
    result.scalarRange = { outcome.haveRange ? outcome.rmin : 0.0,
                           outcome.haveRange ? outcome.rmax : 0.0 };
    // Final image (not a preview in the reload-chain sense — must NOT trigger
    // another fetch). Decimation is conveyed via spacing/origin.
    result.isPreview = false;
    result.spacing = { sx, sy, 1.0 };
    result.origin = { mapping.originX, mapping.originY, 0.0 };
    result.fullWidth = fullW;
    result.fullHeight = fullH;
    result.previewScaleFactor = static_cast<double>(step);
    result.numLevels = probe.numLevels;
    result.valid = true;
    qDebug().noquote()
            << QStringLiteral("[remote-image] assembled level=%1 dims=%2x%3 (full %4x%5) "
                              "from %6x%7 tiles (size %8, %9-way)")
                       .arg(level).arg(downW).arg(downH).arg(fullW).arg(fullH)
                       .arg(cols).arg(rows).arg(ts).arg(concurrency);
    return result;
}

// Assemble ONLY the tiles covering the currently-visible source rectangle, at the
// pyramid level chosen for the current zoom (viewport-driven LOD). Unlike the
// whole-image assembler this produces a sub-image with a non-zero origin: the VTK
// image's world coords still equal full-res source pixels (origin + voxel*spacing),
// so every overlay/readout stays correct. RAM is bounded by the viewport, not the
// image, enabling smooth pan/zoom on gigapixel mosaics. `rect*` is the visible
// source-pixel rectangle (full res) and `viewportHeightPx` the on-screen height.
ImageLayerLoadResult fetchRemoteImageLayerViewport(
        const QString &backendUrl, const QString &datasetId, const QString &datasetPath,
        int fullW, int fullH, int numLevels, int tileSize,
        double rectX0, double rectY0, double rectX1, double rectY1, double viewportHeightPx,
        int requestGeneration, const QString &sessionId, const QString &backendToken)
{
    ImageLayerLoadResult result;
    result.filepath = datasetPath.toStdString();
    result.requestGeneration = requestGeneration;

    const int ts = tileSize > 0 ? tileSize : 256;
    const ImageLod::TileGrid grid{ fullW, fullH, ts, numLevels };
    const ImageLod::ViewportAssembly va =
            ImageLod::viewportAssembly(rectX0, rectY0, rectX1, rectY1, viewportHeightPx, grid, 1);
    if (!va.valid) {
        result.errorMessage = "Invalid viewport LOD geometry.";
        return result;
    }

    const int unionW = va.width;
    const int unionH = va.height;
    const int x0px = va.tileOriginX;   // level-pixel offset of the tile union
    const int y0px = va.tileOriginY;

    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, unionW - 1, 0, unionH - 1, 0, 0);
    image->SetOrigin(va.originX, va.originY, 0.);
    image->SetSpacing(va.spacing, va.spacing, 1.);
    image->AllocateScalars(VTK_FLOAT, 1);
    float *const buf = static_cast<float *>(image->GetScalarPointer());

    // Blit a tile into the union buffer at its offset relative to the union origin
    // (level pixels). Captured by value (buf + dims + offsets) so it is safe across
    // worker threads: every tile writes a disjoint rectangle, no locking.
    const auto blit = [buf, unionW, unionH, ts, x0px, y0px](
                              const BackendImageTileResult &t) -> bool {
        const qsizetype expected =
                static_cast<qsizetype>(t.width) * t.height * static_cast<qsizetype>(sizeof(float));
        if (t.width <= 0 || t.height <= 0 || t.data.size() != expected) {
            return false;
        }
        const int cx = t.tileX * ts - x0px;   // column offset within the union
        const int cy = t.tileY * ts - y0px;   // row offset within the union
        if (cx < 0 || cy < 0 || cx + t.width > unionW || cy + t.height > unionH) {
            return false;
        }
        const float *src = reinterpret_cast<const float *>(t.data.constData());
        for (int r = 0; r < t.height; ++r) {
            std::memcpy(buf + (static_cast<qsizetype>(cy + r) * unionW + cx),
                        src + static_cast<qsizetype>(r) * t.width,
                        static_cast<std::size_t>(t.width) * sizeof(float));
        }
        return true;
    };

    QVector<QPair<int, int>> coords;
    coords.reserve((va.txMax - va.txMin + 1) * (va.tyMax - va.tyMin + 1));
    for (int ty = va.tyMin; ty <= va.tyMax; ++ty) {
        for (int tx = va.txMin; tx <= va.txMax; ++tx) {
            coords.append({ tx, ty });
        }
    }

    const auto outcome = fetchAndBlitTiles(backendUrl, datasetId, sessionId, backendToken,
                                           va.level, ts, coords, blit);
    if (!outcome.ok) {
        result.errorMessage = outcome.error.toStdString();
        return result;
    }
    const int concurrency = coords.isEmpty() ? 1 : std::min(8, static_cast<int>(coords.size()));

    result.imageData = image;
    result.scalarRange = { outcome.haveRange ? outcome.rmin : 0.0,
                           outcome.haveRange ? outcome.rmax : 0.0 };
    result.isPreview = false;
    result.spacing = { va.spacing, va.spacing, 1.0 };
    result.origin = { va.originX, va.originY, 0.0 };
    result.fullWidth = fullW;
    result.fullHeight = fullH;
    result.numLevels = numLevels;
    result.previewScaleFactor = static_cast<double>(1 << va.level);
    result.valid = true;
    qDebug().noquote()
            << QStringLiteral("[remote-image] viewport assembled level=%1 dims=%2x%3 "
                              "origin=(%4,%5) tiles=%6 (%7-way) full=%8x%9")
                       .arg(va.level).arg(unionW).arg(unionH)
                       .arg(va.originX, 0, 'f', 0).arg(va.originY, 0, 'f', 0)
                       .arg(coords.size()).arg(concurrency).arg(fullW).arg(fullH);
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
// Its 190 lines of hand-rolled WCS alignment - the reason the backend-side layer
// path was abandoned - are gone rather than commented out: loadRemoteImageLayer()
// below does the same job with the SAME arithmetic as the local loader, because
// AstroUtils can now build its WCS from the header the backend sends instead of
// needing the file. The lesson (don't re-derive the alignment; reuse libwcs) is
// in that function's comment, which is where it is useful.
// ─────────────────────────────────────────────────────────────────────────────


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
                               const QString &remoteBunit,
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
      remoteDatasetBunit(remoteBunit),
      remoteDegenerateAxesSummary(remoteDegenerateAxesSummary),
      remoteWcsStatus(remoteWcsStatus),
      remoteWcsWarningMessage(remoteWcsWarningMessage),
      lutCustomizer(nullptr),
      profileWidget(nullptr),
      layers(nullptr),
      importService(std::make_unique<ImageLayerImportService>())
{
    // Per-window product scope (session-wide registry): so this image window's Session tree
    // ignores products a sibling cube/image window registered for the same dataset.
    this->m_windowScope = ProductRegistry::instance().newOwnerScope();
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
    // Install the additive status rail now that the window's own permanent
    // labels exist (so the rail lands to their right), then poll backend health.
    if (this->m_workspaceChrome) {
        this->m_workspaceChrome->installStatusRail();
        this->m_workspaceChrome->installSessionDock(
                this->filepath, QFileInfo(this->filepath).fileName(), u"image"_s,
                this->m_windowScope);
        // The dock was installed and an owner scope minted, but nothing here ever
        // registered a product: this window's Session Data listed the dataset and
        // stayed empty for the rest of the session, however many maps, statistics
        // and figures its tools produced. Same contract as the cube viewer now —
        // a tool that makes something the user could want later leaves a row.
        connect(this->m_workspaceChrome, &WorkspaceChrome::sessionRowContextMenu, this,
                &vtkWindowImage::showSessionRowContextMenu);
        connect(this->m_workspaceChrome, &WorkspaceChrome::sessionProductReopen, this,
                &vtkWindowImage::openProduct);
        // setupInspector()/start() run at the END of the constructor — the
        // Analysis-tab tools reference QActions created later below.
    }
    this->updateCatalogueInfoPanel();
    this->statusMessageClearTimer.setSingleShot(true);
    QObject::connect(&this->statusMessageClearTimer, &QTimer::timeout, this, [this]() {
        this->persistentStatusActive = false;
        this->statusBar()->clearMessage();
    });
    QObject::connect(&this->m_stokesListingWatcher,
                     &QFutureWatcher<StokesListing>::finished, this,
                     [this]() { this->onStokesListingReady(m_stokesListingWatcher.result()); });
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
                             // Drop the "Loading layer..." text first: clearing it
                             // after the message below would wipe it immediately.
                             this->clearPersistentStatusMessage();
                             // A companion that fails to load aborts the rest of
                             // the queue: silently going on to the next one would
                             // leave the user with a partial Stokes set and no
                             // idea which component is missing.
                             if (!m_pendingStokesLoads.isEmpty()) {
                                 m_pendingStokesLoads.clear();
                                 statusBar()->showMessage(
                                         u"Stokes companion loading stopped after an error."_s,
                                         5000);
                             }
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
                         // The loader has a single watcher, so queued Stokes
                         // companions go in one after the other from here.
                         this->startNextStokesLoad();
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
                         // Enable viewport-driven LOD once the multi-level pyramid
                         // image is loaded: further zoom/pan re-streams the visible
                         // region at the appropriate level.
                         this->m_lodFullWidth = result.fullWidth;
                         this->m_lodFullHeight = result.fullHeight;
                         this->m_lodNumLevels = result.numLevels;
                         this->m_viewportLodEnabled = (result.numLevels > 1
                                                       && result.fullWidth > 0 && result.fullHeight > 0);
                         this->m_lodApplied = { -1, -1, -1, -1, -1 };
                         qDebug().noquote()
                                 << QStringLiteral("[viewport-lod] %1 (full=%2x%3 levels=%4)")
                                            .arg(this->m_viewportLodEnabled ? u"enabled"_s : u"disabled"_s)
                                            .arg(result.fullWidth).arg(result.fullHeight).arg(result.numLevels);
                         this->clearPersistentStatusMessage();
                     });

    QObject::connect(&this->m_viewportLodWatcher, &QFutureWatcher<ImageLayerLoadResult>::finished,
                     this, &vtkWindowImage::onViewportLodReady);

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
    QObject::connect(ui->actionAddFITS, &QAction::triggered, this,
                     &vtkWindowImage::addLayerFromBrowser);

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
    this->actionExtractSpectrum->setEnabled(false);   // enabled in dynspec mode
    QObject::connect(this->actionExtractSpectrum, &QAction::triggered, this,
                     &vtkWindowImage::extractSpectrum2D);
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
    this->actionLoadCatalogueOverlay = ui->menuTools->addAction(u"Load Catalogue Overlay…"_s);
    this->actionCrossmatch = ui->menuTools->addAction(u"Cross-match with Catalogue…"_s);
    this->actionCrossmatch->setToolTip(
            u"Query an external catalogue (SIMBAD / 2MASS / NVSS / FIRST) over this image's "
            u"footprint and overlay the matches."_s);
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
    QObject::connect(this->actionCrossmatch, &QAction::triggered, this,
                     &vtkWindowImage::crossmatchCatalogue);
    QObject::connect(&this->m_crossmatchWatcher,
                     &QFutureWatcher<CatalogueOverlayParseResult>::finished, this,
                     &vtkWindowImage::onCrossmatchReady);
    QObject::connect(this->actionShowCatalogueOverlay, &QAction::toggled, this,
                     &vtkWindowImage::setCatalogueOverlayVisible);
    QObject::connect(this->actionShowCatalogueLabels, &QAction::toggled, this,
                     [this](bool) { this->updateCatalogueOverlayLabels(); this->vtkRender(); });
    QObject::connect(this->actionClearCatalogueOverlay, &QAction::triggered, this,
                     &vtkWindowImage::clearCatalogueOverlay);
    this->ensureCatalogueDock();
    // …and a way back to it. The dock shows itself when a catalogue loads and
    // hides when the overlay is cleared, but nothing could reopen it once the
    // user closed it — the source list was then unreachable for the rest of the
    // session, while the panel held two empty tables meant for the same job
    // (user report on the layout). The 3-D catalogue window already lists its
    // own dock this way.
    if (this->catalogueDock) {
        QAction *toggle = this->catalogueDock->toggleViewAction();
        toggle->setText(u"Catalogue Sources"_s);
        toggle->setToolTip(u"The table of sources in the loaded catalogue overlay. "
                           u"Selecting a row highlights that source on the image."_s);
        ui->menuView->addAction(toggle);
    }

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
                                 // A file product, so the export is findable
                                 // afterwards from this window rather than only
                                 // from the Data Hub's exports panel.
                                 QVariantMap params;
                                 params.insert(u"Workspace filename"_s, r.second);
                                 params.insert(u"Storage"_s, u"Workspace Exports"_s);
                                 params.insert(u"Contents"_s,
                                         u"the source dataset of this window, not its "
                                         u"derived layers"_s);
                                 self->registerAnalysisProduct(
                                         u"export"_s,
                                         u"Workspace export: %1"_s.arg(r.second),
                                         params, self->scopedProductId(u"wsexport"_s));
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

    // Region export — save the active drawn ROI for CASA/CARTA/ds9. Loading
    // region files (incl. CRTF) goes through Load Catalogue Overlay (one loader).
    ui->menuTools->addSeparator();
    m_actExportRegions = ui->menuTools->addAction(u"Export Region (CRTF/DS9)…"_s);
    m_actExportRegions->setToolTip(
            u"Save the active *drawn* region (Box/Circle/Polygon/Annulus) as a "
            u"CASA CRTF or DS9 region file. To *load* regions (incl. CRTF) use "
            u"Tools ▸ Load Catalogue Overlay."_s);
    QObject::connect(m_actExportRegions, &QAction::triggered, this,
                     [this]() { this->exportRegionsToFile(); });

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
    auto *actHistogram = ui->menuTools->addAction(u"Pixel Histogram"_s);
    this->m_actHistogram = actHistogram;
    QObject::connect(actHistogram, &QAction::triggered, this,
                     &vtkWindowImage::showHistogramPanel);

    // ── Annotations ──────────────────────────────────────────────────────
    ui->menuTools->addSeparator();
    auto *actAddText = ui->menuTools->addAction(u"Add Text Annotation…"_s);
    this->m_actAddText = actAddText;
    auto *actAddArrow = ui->menuTools->addAction(u"Add Arrow Annotation…"_s);
    this->m_actAddArrow = actAddArrow;
    auto *actClearAnnot = ui->menuTools->addAction(u"Clear Annotations"_s);
    this->m_actClearAnnot = actClearAnnot;
    auto *actSaveAnnot = ui->menuTools->addAction(u"Save Annotations…"_s);
    this->m_actSaveAnnot = actSaveAnnot;
    auto *actLoadAnnot = ui->menuTools->addAction(u"Load Annotations…"_s);
    this->m_actLoadAnnot = actLoadAnnot;
    QObject::connect(actAddText, &QAction::triggered, this, &vtkWindowImage::addTextAnnotation);
    QObject::connect(actAddArrow, &QAction::triggered, this, &vtkWindowImage::addArrowAnnotation);
    QObject::connect(actClearAnnot, &QAction::triggered, this, &vtkWindowImage::clearAnnotations);
    QObject::connect(actSaveAnnot, &QAction::triggered, this, &vtkWindowImage::saveAnnotationsJson);
    QObject::connect(actLoadAnnot, &QAction::triggered, this, &vtkWindowImage::loadAnnotationsJson);

    // ── Stokes / Radio polarimetry ──────────────────────────────────────
    ui->menuTools->addSeparator();
    m_actLoadStokesCompanions = ui->menuTools->addAction(u"Load Stokes Q/U/V Companions…"_s);
    m_actLoadStokesCompanions->setToolTip(
            u"Auto-detect Stokes Q, U, V files matching the current Stokes I; "
            u"anything not found can be picked from the backend file browser."_s);
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
            u"Linear fit of PA vs λ² from ≥ 3 (Q, U, ν) triplets, low-RM regime only."_s);
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
            u"Quick action search, same palette as the main window (⌘K)."_s);
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

    // Colour scale lives entirely in the LUT Customizer ("Advanced…"), matching
    // the cube viewer — a single source of truth with the full Linear / Log /
    // Sqrt / Square / Power(γ) set, instead of a reduced inline toggle. The old
    // radio buttons stay hidden (showCurrentLayerSettings still reflects state).
    ui->radioLinear->hide();
    ui->radioLog->hide();

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

    // The header names the layer these controls edit. "LAYER SETTINGS" sits
    // ABOVE the list whose current row it acts on, which reads as settings for
    // the panel rather than for a layer — harmless with one layer, a trap in the
    // window this is: load Stokes companions and compute P, PA, P/I, α and RM and
    // there are ten rows, any of whose colour map you can change without being
    // told which one you changed (user report on the layout).
    const auto nameLayerSettings = [this]() {
        if (!ui->labelLayerHdr)
            return;
        const int row = this->currentLayerIndex();
        const QString name = (row >= 0 && this->layers && row < this->layers->rowCount())
                ? this->layers->data(this->layers->index(row, 0), Qt::DisplayRole).toString()
                : QString();
        ui->labelLayerHdr->setText(name.isEmpty() ? u"LAYER SETTINGS"_s
                                                  : u"LAYER SETTINGS, %1"_s.arg(name));
        ui->labelLayerHdr->setToolTip(
                name.isEmpty() ? QString()
                               : u"These controls act on the layer selected below: %1"_s.arg(name));
    };
    QObject::connect(this->layers, &LayerListModel::dataChanged, this,
                     [nameLayerSettings]() { nameLayerSettings(); });

    // Setup Layer List View
    ui->listLayer->setAcceptDrops(true);
    ui->listLayer->setModel(this->layers);
    ui->listLayer->setCurrentIndex(this->layers->index(0, 0));
    // Only now does the view have a selection model: connecting before setModel()
    // subscribed to a null one, so the header kept naming the first layer no
    // matter which row the user selected.
    QObject::connect(ui->listLayer->selectionModel(), &QItemSelectionModel::currentChanged, this,
                     [nameLayerSettings](const QModelIndex &, const QModelIndex &) {
                         nameLayerSettings();
                     });
    nameLayerSettings();
    QObject::connect(this->layers, &LayerListModel::dataChanged, this, &vtkWindowImage::vtkRender);
    // Keep a layer always selected: if the current row is cleared while layers
    // remain, re-select the first one (deferred to avoid reentrancy).
    QObject::connect(ui->listLayer->selectionModel(), &QItemSelectionModel::currentChanged, this,
                     [this](const QModelIndex &current, const QModelIndex &) {
                         if (!current.isValid() && this->layers && this->layers->rowCount() > 0) {
                             QTimer::singleShot(0, this, [this]() {
                                 if (!ui->listLayer->currentIndex().isValid()
                                     && this->layers->rowCount() > 0) {
                                     ui->listLayer->setCurrentIndex(this->layers->index(0, 0));
                                 }
                             });
                         }
                     });
    QObject::connect(ui->listLayer->selectionModel(), &QItemSelectionModel::currentChanged, this,
                     &vtkWindowImage::showCurrentLayerSettings);
    QObject::connect(ui->listLayer->selectionModel(), &QItemSelectionModel::currentChanged, this,
                     &vtkWindowImage::updateLUTCustomizer);

    // Inspector shell + first health poll: done last so every Analysis-tab tool
    // action (created above) is available.
    if (this->m_workspaceChrome) {
        this->setupInspector();
        this->m_workspaceChrome->start();
    }

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
    if (m_stokesDiscoveryActive) {
        // The companion search may be suspended inside a file chooser's nested
        // event loop with `this` on the stack. Deleting the window now would
        // resume that call on freed memory. (The directory listing itself is no
        // longer blocking - it runs on a worker thread.)
        this->statusBar()->showMessage(
                u"Finish or cancel the Stokes companion search first."_s, 4000);
        event->ignore();
        return;
    }
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



void vtkWindowImage::addLayerFromBrowser()
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














// ── Region interchange (CRTF / DS9) ───────────────────────────────────────────
























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
    const int row = ui->listLayer->currentIndex().row();
    // Fall back to the first layer when the list view has no current row but
    // layers exist (e.g. the selection was cleared) — settings always act on a
    // concrete layer rather than silently no-op.
    if (row < 0 && this->layers && this->layers->rowCount() > 0) {
        return 0;
    }
    return row;
}


// ── Loading an additional layer that lives on the backend ────────────────────
// The master image already opens remotely: its pixels come from the image
// endpoints and never touch this filesystem. An additional layer could not,
// for one reason only - loadImageLayer() aligns a layer to the master with
// AstroUtils, and AstroUtils could only build a WCS from a FILE. That is not a
// property of the alignment, it is a property of the constructor: the WCS lives
// in the header, and /v1/files/header hands the header over.
//
// So a remote layer is loaded the same way the master is: pixels from
// /v1/image/preview (which returns the plane unchanged when it is small enough,
// and a decimated one when it is not), WCS from the two headers. No file is
// copied to this machine, and the alignment is the SAME arithmetic as the local
// path - the numbers it works on just arrive over HTTP.
static ImageLayerLoadResult loadRemoteImageLayer(const QString &backendUrl,
                                                 const QString &backendToken,
                                                 const QString &sessionId,
                                                 const QString &layerPath,
                                                 const QString &masterPath)
{
    ImageLayerLoadResult result;
    result.filepath = layerPath.toStdString();

    BackendClient client(backendUrl, backendToken);
    if (!sessionId.isEmpty())
        client.setSessionId(sessionId);

    const auto opened = client.openDataset(layerPath);
    if (!opened.valid) {
        result.errorMessage = opened.error.isEmpty()
                ? ("The backend could not open " + layerPath.toStdString() + ".")
                : opened.error.toStdString();
        return result;
    }

    // One call for both cases: a plane within the budget comes back whole
    // (scale 1), a larger one decimated, exactly as the master's preview does.
    constexpr int kMaxLongestSide = 8192;
    const auto pixels = client.requestImagePreview(opened.datasetId, kMaxLongestSide);
    if (!pixels.valid) {
        result.errorMessage = pixels.error.isEmpty() ? "Remote layer request failed."
                                                     : pixels.error.toStdString();
        return result;
    }
    if (pixels.scalarType != u"float32"_s) {
        result.errorMessage = "Unsupported remote layer scalar type.";
        return result;
    }
    const qsizetype expectedBytes = static_cast<qsizetype>(pixels.width) * pixels.height
            * static_cast<qsizetype>(sizeof(float));
    if (pixels.width <= 0 || pixels.height <= 0 || pixels.data.size() != expectedBytes) {
        result.errorMessage = "Invalid remote layer payload.";
        return result;
    }

    const auto layerHeader = client.fileHeader(layerPath);
    const auto masterHeader = client.fileHeader(masterPath);
    if (!layerHeader.valid || !masterHeader.valid) {
        result.errorMessage = "Could not read the FITS headers needed to align this layer.";
        return result;
    }
    const auto toCards = [](const QStringList &cards) {
        std::vector<std::string> out;
        out.reserve(static_cast<std::size_t>(cards.size()));
        for (const QString &c : cards)
            out.push_back(c.toStdString());
        return out;
    };
    AstroUtils astroLayer(toCards(layerHeader.cards));
    AstroUtils astroMaster(toCards(masterHeader.cards));
    if (astroMaster.getSecPix() <= 0. || !std::isfinite(astroMaster.getSecPix())) {
        result.errorMessage = "The master image has no usable WCS to align this layer to.";
        return result;
    }
    // …and the LAYER's own WCS. Without it xy2sky() returns leaving `pos`
    // untouched, so the alignment below would read uninitialised sky coordinates
    // and hand back a layer with spacing 0 at an arbitrary place - marked valid.
    if (astroLayer.getSecPix() <= 0. || !std::isfinite(astroLayer.getSecPix())) {
        result.errorMessage = "This layer has no usable WCS, so it cannot be aligned to the "
                              "image: its FITS header carries no celestial solution.";
        return result;
    }

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, pixels.width - 1, 0, pixels.height - 1, 0, 0);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(image->GetScalarPointer(), pixels.data.constData(),
                static_cast<std::size_t>(expectedBytes));
    result.imageData = image;
    result.scalarRange = { pixels.rangeMin, pixels.rangeMax };

    // Two mappings compose here: the layer's own decimation (displayed pixel ->
    // layer source pixel) and the alignment (layer source pixel -> master pixel).
    const auto decim = pixels.isPreview
            ? ImageLod::previewMapping(pixels.fullWidth, pixels.fullHeight, pixels.width,
                                       pixels.height)
            : ImageLod::SourceMapping{};
    const double scalingFactor = astroLayer.getSecPix() / astroMaster.getSecPix();
    result.spacing = { decim.scaleX * scalingFactor, decim.scaleY * scalingFactor, 1. };

    // Origin: where the FIRST DISPLAYED pixel sits in master pixels. It is the
    // source pixel decim.origin, not source (0,0) - on a decimated layer those
    // are not the same place.
    double pos[2];
    double pix[2] = { decim.originX, decim.originY };
    astroLayer.xy2sky(pix, pos, WCS_J2000);
    astroMaster.sky2xy(pos, pix, WCS_J2000);
    result.origin = { pix[0], pix[1], 0. };

    // Rotation: the same measurement as the local path - the angle of the
    // layer's +Y axis in the master's frame, signed - over a span of 100
    // DISPLAYED pixels so a decimated layer measures the same arc.
    pix[0] = decim.originX;
    pix[1] = decim.originY + 100. * decim.scaleY;
    astroLayer.xy2sky(pix, pos, WCS_J2000);
    astroMaster.sky2xy(pos, pix, WCS_J2000);
    const double dx = pix[0] - result.origin[0];
    const double dy = pix[1] - result.origin[1];
    result.rotationDegrees = std::atan2(dx, dy) * 180. / vtkMath::Pi();

    if (!std::isfinite(result.spacing[0]) || !std::isfinite(result.spacing[1])
        || result.spacing[0] <= 0. || result.spacing[1] <= 0.
        || !std::isfinite(result.origin[0]) || !std::isfinite(result.origin[1])
        || !std::isfinite(result.rotationDegrees)) {
        result.imageData = nullptr;
        result.errorMessage = "The alignment of this layer to the image produced no usable "
                              "geometry (incompatible WCS?).";
        return result;
    }

    result.valid = true;
    qDebug().noquote()
            << QStringLiteral("[remote-layer] %1 %2x%3 (source %4x%5) spacing=%6 origin=(%7, %8)")
                       .arg(QFileInfo(layerPath).fileName())
                       .arg(pixels.width).arg(pixels.height)
                       .arg(pixels.fullWidth).arg(pixels.fullHeight)
                       .arg(result.spacing[0])
                       .arg(result.origin[0]).arg(result.origin[1]);
    return result;
}

void vtkWindowImage::addLayerImage(const std::string &filepath)
{
    const QString path = QString::fromStdString(filepath);
    // WHOSE filesystem is this path on? Every dataset here is opened through the
    // backend, so a path is a BACKEND path. Asking QFile::exists() answers a
    // different question - "is there a file here with that name" - and on a
    // remote backend a client-side file of the same name would be loaded
    // instead, silently: the wrong pixels, and the wrong WCS with them.
    //
    // The filesystem is shared only when the backend is on this machine, which
    // the URL says. Anything else goes through the backend - same watcher, same
    // result, so every caller (Add New Layer, the VLKB cutouts, the Stokes
    // companions) is unaffected.
    const QString backendHost = QUrl(this->remoteBackendUrl).host();
    const bool backendIsLocal = backendHost.isEmpty() || backendHost == u"localhost"_s
            || backendHost == u"127.0.0.1"_s || backendHost == u"::1"_s
            || backendHost == u"0.0.0.0"_s;
    const bool haveBothLocally = QFile::exists(path) && QFile::exists(this->filepath);
    const bool useBackend = !this->remoteBackendUrl.isEmpty()
            && !(backendIsLocal && haveBothLocally);
    if (useBackend) {
        this->setLayerImportEnabled(false);
        this->showPersistentStatusMessage(
                u"Loading %1 from the backend..."_s.arg(QFileInfo(path).fileName()));
        this->layerLoadWatcher.setFuture(QtConcurrent::run(
                &loadRemoteImageLayer, this->remoteBackendUrl, this->remoteBackendToken,
                this->remoteSessionId, path, this->filepath));
        return;
    }
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

void vtkWindowImage::applyRemoteMasterLayer(const ImageLayerLoadResult &result, bool preserveView)
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
    // A viewport-LOD reload swaps in a small sub-region; large-image mode reflects
    // the FULL image (unchanged) so don't recompute it from the sub-image dims.
    if (!preserveView) {
        this->updateLargeImageMode(imageData, "remote apply");
    }
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
    // Reset the camera only on a fresh load; a viewport-LOD reload MUST keep the
    // user's current zoom/pan (that camera is what drove the tile request).
    if (renderer && !preserveView) {
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
            << QStringLiteral("[remote-image] requesting full-resolution image (tiled) dataset=%1")
                       .arg(this->remoteDatasetId);
    // Assemble the full-resolution image from pyramid tiles: each tile is a
    // bounded out-of-core read on the backend, so mosaics too large for a single
    // /v1/image/full call load without OOMing the backend. 512 px balances
    // request count against per-tile payload size.
    constexpr int kFullResTileSize = 512;
    this->remoteFullImageWatcher.setFuture(
            QtConcurrent::run(&fetchRemoteImageLayerTiled, this->remoteBackendUrl,
                              this->remoteDatasetId, this->filepath, kFullResTileSize,
                              this->remoteLoadGeneration, this->remoteSessionId,
                              this->remoteBackendToken));
}

bool vtkWindowImage::isBusy() const
{
    // The companion listing counts too: it ends in a layer load, and a second
    // import starting on top of it replaces the single layer watcher's future
    // and overwrites m_pendingVlkbDisplayName - layers silently lost or
    // mislabelled.
    return this->layerLoadWatcher.isRunning() || this->remoteImageWatcher.isRunning()
            || this->remoteFullImageWatcher.isRunning()
            || this->m_stokesListingWatcher.isRunning();
}

void vtkWindowImage::onImageInteractionSettled()
{
    // A camera pan/zoom ended — (re)arm the debounce so a burst of end-events
    // collapses into a single stream request once the view is quiet.
    if (this->m_viewportLodEnabled && this->m_lodSettleTimer) {
        this->m_lodSettleTimer->start();
    }
}

void vtkWindowImage::maybeStreamViewportLod()
{
    if (!this->m_viewportLodEnabled || this->isBusy() || this->m_viewportLodWatcher.isRunning()) {
        return;
    }
    // Don't re-stream while the user is drawing/probing (the master swap would
    // disturb the region/measurement in progress).
    if (this->probeModeActive || this->regionMode != RegionMode::None
        || this->m_measMode != MeasurementMode::None) {
        return;
    }
    auto *renderWindow = ui && ui->vtk ? ui->vtk->renderWindow() : nullptr;
    auto *renderer = renderWindow ? renderWindow->GetRenderers()->GetFirstRenderer() : nullptr;
    auto *camera = renderer ? renderer->GetActiveCamera() : nullptr;
    if (!renderer || !camera) {
        return;
    }
    const int *sz = renderWindow->GetSize();
    if (!sz || sz[0] <= 0 || sz[1] <= 0) {
        return;
    }
    // Raw viewport rectangle in WORLD coords (== full-res source pixels, since the
    // image world extent equals the full-res grid). NOT clamped to the currently
    // displayed sub-image — we must see the whole panned-to region to know which
    // tiles to fetch; viewportAssembly() clamps to the full image itself.
    const double *fp = camera->GetFocalPoint();
    const double halfH = camera->GetParallelScale();
    const double halfW = halfH * static_cast<double>(sz[0]) / static_cast<double>(sz[1]);
    const double rx0 = fp[0] - halfW, rx1 = fp[0] + halfW;
    const double ry0 = fp[1] - halfH, ry1 = fp[1] + halfH;
    const double viewportH = static_cast<double>(sz[1]);

    const ImageLod::TileGrid grid{ this->m_lodFullWidth, this->m_lodFullHeight,
                                   this->m_lodTileSize, this->m_lodNumLevels };
    const ImageLod::ViewportAssembly va =
            ImageLod::viewportAssembly(rx0, ry0, rx1, ry1, viewportH, grid, 1);
    if (!va.valid) {
        return;
    }
    const std::array<int, 5> sig{ { va.level, va.txMin, va.txMax, va.tyMin, va.tyMax } };
    if (sig == this->m_lodApplied) {
        return;   // same region + level already displayed
    }
    this->m_lodPending = sig;
    qDebug().noquote()
            << QStringLiteral("[viewport-lod] stream level=%1 tiles x[%2..%3] y[%4..%5] "
                              "view=[%6..%7, %8..%9] vpH=%10")
                       .arg(va.level).arg(va.txMin).arg(va.txMax).arg(va.tyMin).arg(va.tyMax)
                       .arg(rx0, 0, 'f', 0).arg(rx1, 0, 'f', 0)
                       .arg(ry0, 0, 'f', 0).arg(ry1, 0, 'f', 0)
                       .arg(viewportH, 0, 'f', 0);
    const int gen = ++this->m_lodGeneration;
    this->m_viewportLodWatcher.setFuture(QtConcurrent::run(
            &fetchRemoteImageLayerViewport, this->remoteBackendUrl, this->remoteDatasetId,
            this->filepath, this->m_lodFullWidth, this->m_lodFullHeight, this->m_lodNumLevels,
            this->m_lodTileSize, rx0, ry0, rx1, ry1,
            viewportH, gen, this->remoteSessionId, this->remoteBackendToken));
}

void vtkWindowImage::onViewportLodReady()
{
    const auto result = this->m_viewportLodWatcher.result();
    if (result.requestGeneration != this->m_lodGeneration) {
        qDebug().noquote() << QStringLiteral("[viewport-lod] discarded stale gen=%1 current=%2")
                                      .arg(result.requestGeneration).arg(this->m_lodGeneration);
        return;
    }
    if (!result.valid || !result.imageData) {
        qDebug().noquote() << QStringLiteral("[viewport-lod] stream failed: %1")
                                      .arg(QString::fromStdString(result.errorMessage));
        // The view may have moved while this (failed) fetch was in flight, and the
        // debounce that would have re-checked was dropped by the isRunning() guard.
        // Re-arm so a transient tile failure self-heals — but bounded so a dead
        // backend isn't polled forever.
        if (++this->m_lodConsecutiveFailures <= 3 && this->m_lodSettleTimer) {
            this->m_lodSettleTimer->start();
        }
        return;
    }
    this->m_lodConsecutiveFailures = 0;
    this->applyRemoteMasterLayer(result, /*preserveView=*/true);
    this->m_lodApplied = this->m_lodPending;   // this view is now displayed
    // The view may have moved again while the tiles were fetching — re-check.
    this->maybeStreamViewportLod();
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

// ── Copilot actions ──────────────────────────────────────────────────────────
// The image viewer handles the directive types that make sense in 2-D (colour
// map, camera). Cube-only types (goto_channel, render/blend mode, threshold) are
// ignored. Display-only and reversible: a snapshot is taken before each batch.

void vtkWindowImage::applyCopilotActions(const QList<QJsonObject> &actions)
{
    // Only these directive types apply in 2-D; if the batch has none (all
    // cube-only), do nothing — don't overwrite the undo slot or claim a change.
    // Map a scale name to a LutScale::Mode; returns false for an unknown scale.
    auto parseScale = [](const QString &raw, LutScale::Mode &out) -> bool {
        const QString s = raw.trimmed().toLower();
        if (s == u"linear"_s) { out = LutScale::Mode::Linear; return true; }
        if (s == u"log"_s)    { out = LutScale::Mode::Log;    return true; }
        if (s == u"sqrt"_s)   { out = LutScale::Mode::Sqrt;   return true; }
        if (s == u"square"_s) { out = LutScale::Mode::Square; return true; }
        if (s == u"power"_s)  { out = LutScale::Mode::Power;  return true; }
        return false;
    };

    int applicable = 0;
    bool batchSetsColormap = false;
    bool batchSetsScale = false;
    bool batchResetsCamera = false;
    for (const QJsonObject &a : actions) {
        const QString t = a.value(u"type"_s).toString();
        LutScale::Mode m{};
        if (t == u"set_colormap"_s) { ++applicable; batchSetsColormap = true; }
        else if (t == u"reset_camera"_s) { ++applicable; batchResetsCamera = true; }
        else if (t == u"set_scaling"_s && parseScale(a.value(u"scale"_s).toString(), m)) {
            ++applicable; batchSetsScale = true;
        }
    }
    if (applicable == 0)
        return;

    auto *renderer = (ui && ui->vtk && ui->vtk->renderWindow())
            ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
            : nullptr;

    this->m_copilotUndo = CopilotSnapshot{};
    this->m_copilotUndo.valid = true;
    // Snapshot the CURRENT layer index, so Undo restores that exact layer even if
    // the user switches layers before undoing. Capture only what the batch touches.
    this->m_copilotUndo.layerIndex = this->currentLayerIndex();
    const int idx = this->m_copilotUndo.layerIndex;
    if (batchSetsColormap && this->layerController) {
        this->m_copilotUndo.colormap = QString::fromStdString(
                this->layerController->layerViewState(idx).colorMapName);
        this->m_copilotUndo.colormapCaptured = true;
    }
    if (batchSetsScale && this->layers && idx >= 0 && idx < this->layers->rowCount()) {
        this->m_copilotUndo.scaleMode = this->layers->scaleMode(idx);
        this->m_copilotUndo.scaleGamma = this->layers->scaleGamma(idx);
        // Log clamps the LUT table range to positive; capture it so Undo can put
        // back a range that spanned negatives (e.g. a linear [-5, 10]).
        if (auto *lut = this->layers->getLookupTable(idx)) {
            const double *r = lut->GetTableRange();
            this->m_copilotUndo.lutRange[0] = r ? r[0] : 0.0;
            this->m_copilotUndo.lutRange[1] = r ? r[1] : 1.0;
        }
        this->m_copilotUndo.scaleCaptured = true;
    }
    if (batchResetsCamera && renderer) {
        if (auto *cam = renderer->GetActiveCamera()) {
            cam->GetPosition(this->m_copilotUndo.camPos);
            cam->GetFocalPoint(this->m_copilotUndo.camFoc);
            cam->GetViewUp(this->m_copilotUndo.camUp);
            this->m_copilotUndo.camScale = cam->GetParallelScale();
            this->m_copilotUndo.cameraCaptured = true;
        }
    }

    for (const QJsonObject &a : actions) {
        const QString type = a.value(u"type"_s).toString();
        if (type == u"set_colormap"_s) {
            const QString want = a.value(u"name"_s).toString().trimmed();
            for (const std::string &n : ColorMaps::GetColorMapNames()) {
                const QString qn = QString::fromStdString(n);
                if (qn.compare(want, Qt::CaseInsensitive) == 0) {
                    if (ui->comboLut) {
                        const QSignalBlocker b(ui->comboLut);
                        ui->comboLut->setCurrentText(qn);
                    }
                    this->changeCurrentColorMap();
                    break;
                }
            }
        } else if (type == u"set_scaling"_s && this->layers) {
            LutScale::Mode mode{};
            if (parseScale(a.value(u"scale"_s).toString(), mode)
                && idx >= 0 && idx < this->layers->rowCount()) {
                // Log needs positive values in range; skip (like the LUT editor)
                // rather than collapse to a degenerate range on an all-≤0 image.
                bool ok = true;
                if (mode == LutScale::Mode::Log) {
                    auto *img = this->layers->getImageData(idx);
                    const double floor = img
                            ? sampledPositiveFloor(img, initialAutoscaleTargetSamples)
                            : 0.0;
                    if (!(floor > 0.0) || !std::isfinite(floor)) {
                        ok = false;
                        this->statusBar()->showMessage(
                                tr("Log scale needs positive values in the visible range."), 3000);
                    }
                }
                if (ok) {
                    // Canonical per-layer stretch API (same one the LUT editor
                    // uses), keeping its gamma. dataChanged re-renders; Undo
                    // restores mode+gamma+range exactly.
                    this->layers->setScaleMode(idx, mode, this->layers->scaleGamma(idx));
                    this->updateLUTCustomizer();
                }
            }
        } else if (type == u"reset_camera"_s && renderer) {
            renderer->ResetCamera();
            renderer->ResetCameraClippingRange();
        }
        // Cube-only directive types are silently ignored on a 2-D image.
    }
    this->vtkRender();
    this->statusBar()->showMessage(
            tr("Copilot applied %n visualisation change(s), Undo available.", "", applicable),
            6000);
}

void vtkWindowImage::undoCopilotActions()
{
    if (!this->m_copilotUndo.valid)
        return;
    const CopilotSnapshot s = this->m_copilotUndo;
    this->m_copilotUndo.valid = false;

    if (s.colormapCaptured && !s.colormap.isEmpty() && this->layerController) {
        // Restore the captured layer's colormap directly (not the now-current one).
        this->layerController->setCurrentColorMap(s.layerIndex, s.colormap.toStdString());
        // Reflect in the combo only if that layer is still selected.
        if (ui->comboLut && this->currentLayerIndex() == s.layerIndex) {
            const QSignalBlocker b(ui->comboLut);
            ui->comboLut->setCurrentText(s.colormap);
        }
    }
    if (s.scaleCaptured && this->layers && s.layerIndex >= 0
        && s.layerIndex < this->layers->rowCount()) {
        // Restore the pre-batch LUT range FIRST (a log stretch clamped it to
        // positive), so setScaleMode rebuilds from the original range, then
        // restore the captured stretch (mode + gamma) exactly.
        //
        // Forced linear for that one call: the saved range is the unclamped one
        // and the table may still be logarithmic from the batch, in which case a
        // plain SetTableRange is refused and the restore silently does nothing —
        // setScaleMode would then rebuild from the clamped range this is trying
        // to undo. setScaleMode installs the real mode immediately after.
        if (auto *lut = this->layers->getLookupTable(s.layerIndex))
            LutScale::setRange(lut, LutScale::Mode::Linear, s.lutRange[0], s.lutRange[1]);
        this->layers->setScaleMode(s.layerIndex, s.scaleMode, s.scaleGamma);
        if (this->currentLayerIndex() == s.layerIndex)
            this->updateLUTCustomizer();
    }
    if (s.cameraCaptured && ui && ui->vtk && ui->vtk->renderWindow()) {
        if (auto *ren = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()) {
            if (auto *cam = ren->GetActiveCamera()) {
                cam->SetPosition(s.camPos[0], s.camPos[1], s.camPos[2]);
                cam->SetFocalPoint(s.camFoc[0], s.camFoc[1], s.camFoc[2]);
                cam->SetViewUp(s.camUp[0], s.camUp[1], s.camUp[2]);
                cam->SetParallelScale(s.camScale);
                ren->ResetCameraClippingRange();
            }
        }
    }
    this->vtkRender();
    this->statusBar()->showMessage(tr("Reverted the copilot's visualisation changes."), 4000);
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
            // The table is still logarithmic here — that is the branch condition —
            // and the saved range is the signed one from before log was turned on,
            // so it has to go in as linear or VTK refuses it.
            LutScale::setRange(lut, LutScale::Mode::Linear, saved[0], saved[1]);
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


// ── Layout B viewer toolbar ──────────────────────────────────────────────────


// ── Sidebar ───────────────────────────────────────────────────────────────────

// ── Contour overlay — same pipeline as vtkWindowCube ────────────────────────
// vtkFlyingEdges2D connected to the master layer image data. GenerateValues
// produces N evenly-spaced levels between lower and upper, matching the
// cube viewer's Level / Lower / Upper UI pattern. The actor is permanently
// in the renderer; visibility is toggled via the "Show Contours" button.







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


// ── Dynamic-spectrum tool slots ─────────────────────────────────────────────




// ─────────────────────────────────────────────────────────────────────────────


// ── Measurement tools (Ruler / Angle) ────────────────────────────────────────






// ── Histogram panel ──────────────────────────────────────────────────────────


// ── Annotations ──────────────────────────────────────────────────────────────











// ── Blink / Compare ──────────────────────────────────────────────────────────



// ── Stokes / Radio polarimetry analysis ─────────────────────────────────────










// ── Bias-corrected P: P_debiased = √(max(0, Q² + U² − σ²)) ─────────────────
// σ is the per-channel noise. We estimate it as the MAD-σ of the Q and U
// pixel distributions averaged together. This is the simplest "ad-hoc"
// debiasing; for rigorous work the noise map should be supplied.

// ── Spectral index map: α = log(S_B/S_A) / log(ν_B/ν_A) ───────────────────
// Per-pixel formula on two layers at different frequencies. Both layers
// must share the same pixel grid (same WCS, same extent).

// ── Faraday Rotation Measure: per-pixel linear fit of PA vs λ² ─────────────
// Requires ≥ 3 frequency pairs (Q_i, U_i, ν_i). For each pixel:
//   PA_i = ½·atan2(U_i, Q_i)
//   λ_i² = (c/ν_i)²
//   linear fit:  PA = PA₀ + RM · λ²   →  slope = RM (rad m⁻²)
// NOTE: no PA unwrapping — only valid in the "low-RM" regime where the n·π
// ambiguity does not flip between channels. For aggressive RM use the
// future backend-side RM synthesis tool.




// ── VLKB layer support ────────────────────────────────────────────────────────

void vtkWindowImage::addLayerFromBackendPath(const QString &backendPath,
                                              const QString &displayName)
{
    if (this->isBusy()) {
        qWarning().noquote()
                << QStringLiteral("[vlkb-layer] viewer busy, ignoring addLayerFromBackendPath");
        return;
    }
    // Reuse the standard addLayerImage pipeline, which picks the local reader or
    // the backend depending on whether this machine can see the file. The VLKB
    // cutout is written on the BACKEND's filesystem, so with a remote backend
    // that is the backend path - and it loads either way.
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
