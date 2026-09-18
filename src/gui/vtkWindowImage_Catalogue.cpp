#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeViewportGeometry.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "CatalogueTableModel.h"
#include "WorkspaceChrome.h"
#include "InspectorPanel.h"
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
#include "Settings.h"

#include <vtkCamera.h>
#include <vtkActor2D.h>
#include <vtkAxisActor2D.h>
#include <vtkCommand.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkContourTriangulator.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageSlice.h>
#include <vtkImageStack.h>
#include <vtkInteractorObserver.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleUser.h>
#include <vtkLineSource.h>
#include <vtkLookupTable.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkUnsignedCharArray.h>
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
#include <QColor>
#include <QDebug>
#include <QKeySequence>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QSaveFile>
#include <QTextStream>
#include <QFileInfo>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QtConcurrent>
#include <QInputDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QUrl>
#include <QUrlQuery>
#include <QSettings>
#include <QDir>
#include <QTimer>
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
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

using namespace Qt::StringLiterals;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

// ── Catalogue overlay ────────────────────────────────────────────────────────
// Extracted verbatim from vtkWindowImage.cpp. DS9/CSV/CRTF source catalogues as
// an interactive overlay (table, labels, hover, selection). Geometry helpers
// (computeVisibleImageBounds2D, pointInVisibleBounds) come from CubeGeom.

namespace {
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
    switch (shape) {
    case CatalogueOverlayEntry::Shape::Ellipse:  return QStringLiteral("ellipse");
    case CatalogueOverlayEntry::Shape::Polyline: return QStringLiteral("polyline");
    default:                                     return QStringLiteral("marker");
    }
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

// tanSkyToPix duplicated (also kept in main for the master-layer WCS code).

void tanSkyToPix(double lonDeg, double latDeg,
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

// Project a native-frame world coordinate to pixels honouring the projection:
// gnomonic (TAN) for CTYPE "…-TAN", else the app's LINEAR model (crval + …·cdelt),
// which is exact for CAR — the projection the Hi-GAL cutouts use — and matches the
// displayed axes (remoteVoxelToWcs is linear).
void nativeWorldToPixel(const QString &ctype0, double nx, double ny,
                        const std::array<double, 3> &crpix,
                        const std::array<double, 3> &crval,
                        const std::array<double, 3> &cdelt,
                        double &pixX, double &pixY)
{
    if (ctype0.contains(u"-TAN"_s, Qt::CaseInsensitive)) {
        tanSkyToPix(nx, ny, crpix, crval, cdelt, pixX, pixY);
    } else {
        pixX = (nx - crval[0]) / cdelt[0] + (crpix[0] - 1.0);
        pixY = (ny - crval[1]) / cdelt[1] + (crpix[1] - 1.0);
    }
}

// Convert canonical region dicts (from the backend CRTF/DS9 parser) into
// catalogue-overlay entries, so Load Catalogue Overlay is the single loader for
// region files — including CASA CRTF, which CatalogueOverlayUtils cannot parse.
// Image-frame pixel values are kept as-is (matching the DS9 path); ellipse and
// circle become drawable ellipses, other shapes fall back to a centre marker.
CatalogueOverlayParseResult catalogueResultFromRegions(const QJsonArray &regions)
{
    CatalogueOverlayParseResult res;
    res.valid = true;
    res.frameLabel = QStringLiteral("CRTF");
    res.sourceLabel = QStringLiteral("CRTF regions");
    for (const QJsonValue &rv : regions) {
        const QJsonObject r = rv.toObject();
        const QJsonArray c = r.value(u"center"_s).toArray();
        if (c.size() < 2) continue;
        CatalogueOverlayEntry e;
        const bool image = r.value(u"frame"_s).toString() == u"image"_s;
        e.frame = image ? CatalogueOverlayEntry::Frame::Image : CatalogueOverlayEntry::Frame::Sky;
        if (image) { e.pixelX = c.at(0).toDouble(); e.pixelY = c.at(1).toDouble(); }
        else       { e.raDeg = c.at(0).toDouble(); e.decDeg = c.at(1).toDouble(); }
        const QString shape = r.value(u"shape"_s).toString();
        if (shape == u"ellipse"_s) {
            const QJsonArray a = r.value(u"semi_axes"_s).toArray();
            e.shape = CatalogueOverlayEntry::Shape::Ellipse;
            e.radiusX = a.at(0).toDouble();
            e.radiusY = a.at(1).toDouble();
            e.angleDeg = r.value(u"angle_deg"_s).toDouble();
        } else if (shape == u"circle"_s) {
            e.shape = CatalogueOverlayEntry::Shape::Ellipse;
            e.radiusX = e.radiusY = r.value(u"radius"_s).toDouble();
        } else {
            e.shape = CatalogueOverlayEntry::Shape::Marker;
        }
        e.label = r.value(u"props"_s).toObject().value(u"text"_s).toString();
        res.entries.push_back(e);
    }
    return res;
}

}  // namespace

void vtkWindowImage::loadCatalogueOverlay()
{
    const QString path = QFileDialog::getOpenFileName(
            this, u"Load Catalogue Overlay"_s, QString(),
            u"Catalogue / region files (*.reg *.crtf *.csv);;DS9 region (*.reg);;"
            u"CASA CRTF (*.crtf);;CSV (*.csv)"_s);
    if (path.isEmpty()) {
        return;
    }

    // DS9 (.reg) and CSV are parsed locally; CASA CRTF is parsed by the backend
    // (the only place with a CRTF parser) and converted to catalogue entries.
    CatalogueOverlayParseResult parsed;
    if (path.endsWith(u".crtf"_s, Qt::CaseInsensitive)) {
        QFile crtfFile(path);
        if (!crtfFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, u"Catalogue Overlay"_s, u"Cannot open file:\n%1"_s.arg(path));
            return;
        }
        const QString text = QString::fromUtf8(crtfFile.readAll());
        crtfFile.close();
        this->statusBar()->showMessage(u"Parsing CRTF…"_s, 0);
        BackendClient c(this->remoteBackendUrl, this->remoteBackendToken);
        const BackendRegionParseResult rr = c.parseRegions(text, u"crtf"_s, QString(), QString());
        this->statusBar()->clearMessage();
        if (!rr.valid) {
            QMessageBox::warning(this, u"Catalogue Overlay"_s,
                                 u"CRTF parse failed:\n%1"_s.arg(rr.error));
            return;
        }
        parsed = catalogueResultFromRegions(rr.regions);
    } else {
        parsed = CatalogueOverlayUtils::parseFile(path);
    }
    if (!parsed.valid) {
        QMessageBox::warning(this, u"Catalogue Overlay"_s, parsed.errorMessage);
        return;
    }
    this->applyCatalogueOverlayResult(parsed);
}

// Project a parsed/queried catalogue onto the current image WCS and display it —
// the shared back end for both file overlays and the catalogue cross-match.
void vtkWindowImage::applyCatalogueOverlayResult(const CatalogueOverlayParseResult &parsed)
{
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
    this->catalogueOverlayPolylineColors.clear();
    this->catalogueOverlayPolylineBand.clear();
    this->m_vlkbHiddenBands.clear();
    this->catalogueOverlaySourceFirstPolyline.clear();
    this->catalogueOverlaySourcePolylineCount.clear();
    this->catalogueOverlayLabels.clear();
    this->catalogueOverlayLabelIndices.clear();
    this->hoveredCatalogueSourceIndex = -1;
    this->selectedCatalogueSourceIndex = -1;
    int skippedProjection = 0;
    // Sky-frame markers are drawn as a small cross in full-res world units. A fixed
    // 3-px cross is invisible on a multi-thousand-pixel image (and on a decimated LOD
    // the world span is the full-res width, e.g. ~16000), so scale the cross to a small
    // fraction of the image so markers are actually visible. Keep a sensible floor for
    // tiny images. Uses full-res world extent (extent * spacing) so it is correct on
    // both decimated and full-res images.
    double markerHalfSize = catalogueMarkerHalfSize;
    if (this->layers) {
        if (auto *img = this->layers->getImageData(this->layers->getMasterIndex())) {
            int ext[6]; double sp[3];
            img->GetExtent(ext);
            img->GetSpacing(sp);
            const double worldW = (ext[1] - ext[0]) * sp[0];
            const double worldH = (ext[3] - ext[2]) * sp[1];
            markerHalfSize = std::max(catalogueMarkerHalfSize, 0.004 * std::max(worldW, worldH));
        }
    }
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
            } else if (entry.shape == CatalogueOverlayEntry::Shape::Polyline) {
                if (entry.polyline.size() < 2) {
                    ++skippedProjection;
                    continue;
                }
                // Anchor at the polyline centroid so the label/pick lands on it.
                double cx = 0.0, cy = 0.0;
                for (const auto &p : entry.polyline) { cx += p[0]; cy += p[1]; }
                anchor = { cx / entry.polyline.size(), cy / entry.polyline.size() };
                this->catalogueOverlayPolylines.push_back(entry.polyline);
            }
        } else {
            if (!this->catalogueWorldToPixel(entry.raDeg, entry.decDeg, anchor)) {
                ++skippedProjection;
                continue;
            }

            this->catalogueOverlayPolylines.push_back(
                    { { anchor[0] - markerHalfSize, anchor[1] },
                      { anchor[0] + markerHalfSize, anchor[1] } });
            this->catalogueOverlayPolylines.push_back(
                    { { anchor[0], anchor[1] - markerHalfSize },
                      { anchor[0], anchor[1] + markerHalfSize } });
        }
        storedEntry.pixelX = anchor[0];
        storedEntry.pixelY = anchor[1];
        this->catalogueOverlayEntries.push_back(storedEntry);
        this->catalogueOverlayPixels.push_back(anchor);
        this->catalogueOverlaySourceFirstPolyline.push_back(firstPolyline);
        this->catalogueOverlaySourcePolylineCount.push_back(
                static_cast<int>(this->catalogueOverlayPolylines.size()) - firstPolyline);
        this->catalogueOverlayLabels.push_back(entry.label);
        // Per-polyline colour + band tag (parallel to catalogueOverlayPolylines).
        const bool custom = entry.color[0] >= 0.0;
        const std::array<unsigned char, 3> rgb = custom
                ? std::array<unsigned char, 3>{
                        static_cast<unsigned char>(std::clamp(entry.color[0], 0.0, 1.0) * 255.0),
                        static_cast<unsigned char>(std::clamp(entry.color[1], 0.0, 1.0) * 255.0),
                        static_cast<unsigned char>(std::clamp(entry.color[2], 0.0, 1.0) * 255.0) }
                : std::array<unsigned char, 3>{ 255, 115, 38 };   // default overlay orange
        for (int p = firstPolyline;
             p < static_cast<int>(this->catalogueOverlayPolylines.size()); ++p) {
            this->catalogueOverlayPolylineColors.push_back(rgb);
            this->catalogueOverlayPolylineBand.push_back(entry.band);
        }
    }

    if (this->catalogueOverlayPixels.empty()) {
        // Nothing landed on the image: take the PREVIOUS overlay down as well.
        // Clearing only the table left the old actors drawn over the image with
        // an empty list beside them - the one state that says two things at once.
        // clearCatalogueOverlay() is the single place that knows what an overlay
        // consists of (actors, labels, highlights, band panel, re-render).
        this->catalogueOverlayExtraFields.clear();
        this->clearCatalogueOverlay();
        this->refreshCatalogueTable();
        QMessageBox::warning(this, u"Catalogue Overlay"_s,
                             u"No sources could be projected onto the current image WCS."_s);
        return;
    }

    this->catalogueOverlayLoaded = true;
    this->catalogueOverlayFrameLabel = parsed.frameLabel;
    this->catalogueOverlayExtraFields = parsed.extraFields;
    this->catalogueOverlaySummary =
            u"%1 sources from %2 (%3)"_s.arg(this->catalogueOverlayPixels.size())
                    .arg(parsed.sourceLabel, parsed.frameLabel);
    this->rebuildCatalogueOverlay();
    // The VLKB band dock only applies to multi-band source overlays: hide it for
    // any overlay without band tags (CSV/ds9/cross-match/filaments). For VLKB
    // sources onVlkbCatalogueReady re-shows it via ensureVlkbBandPanel().
    const bool hasBands =
            std::any_of(this->catalogueOverlayPolylineBand.cbegin(),
                        this->catalogueOverlayPolylineBand.cend(),
                        [](const QString &b) { return !b.isEmpty(); });
    if (!hasBands) {
        releaseToolSettings(u"Catalogue overlay"_s);
        refreshToolSettingsPanel(/*reveal=*/false);
    }
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
    // One message, caveat included: a warning shown before this line was
    // overwritten by it in the same synchronous flow.
    const QString loaded =
            u"Catalogue overlay loaded: %1 sources (%2)."_s
                    .arg(this->catalogueOverlayPixels.size())
                    .arg(parsed.frameLabel);
    this->showPersistentStatusMessage(
            parsed.errorMessage.isEmpty() ? loaded : loaded + u" "_s + parsed.errorMessage,
            parsed.errorMessage.isEmpty() ? 3000 : 8000);
    if (skippedProjection > 0 || parsed.skippedEntries > 0) {
        qDebug().noquote() << QStringLiteral("[catalogue] loaded=%1 skipped_parse=%2 skipped_wcs=%3")
                                          .arg(this->catalogueOverlayPixels.size())
                                          .arg(parsed.skippedEntries)
                                          .arg(skippedProjection);
    }
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::crossmatchCatalogue()
{
    if (this->m_crossmatchWatcher.isRunning())
        return;
    if (!this->remoteHasCelestialAxes()) {
        QMessageBox::warning(this, u"Cross-match"_s,
                             u"Cross-match requires celestial WCS information."_s);
        return;
    }
    if (this->remoteDatasetId.isEmpty())
        return;

    // Parameter dialog: which external catalogue + search radius.
    QDialog dlg(this);
    dlg.setWindowTitle(u"Cross-match with Catalogue"_s);
    auto *form = new QFormLayout(&dlg);
    auto *combo = new QComboBox(&dlg);
    combo->addItems({ u"simbad"_s, u"2MASS"_s, u"NVSS"_s, u"FIRST"_s });
    form->addRow(u"Catalogue"_s, combo);
    auto *spin = new QDoubleSpinBox(&dlg);
    spin->setRange(1.0, 3600.0);
    spin->setValue(60.0);
    spin->setSuffix(u" arcsec"_s);
    form->addRow(u"Search radius"_s, spin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // Query off the GUI thread — external catalogue services can be slow. The
    // worker converts the result into a CatalogueOverlayParseResult (valid/error
    // encoded there) so the finished handler just projects it like a file overlay.
    const QString url = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid = this->remoteSessionId;
    const QString dsid = this->remoteDatasetId;
    const QString catalogue = combo->currentText();
    const double radius = spin->value();

    if (this->actionCrossmatch)
        this->actionCrossmatch->setEnabled(false);
    // The result handler is generic (it just projects a parse result), so the
    // parameters that produced it have to be kept here or the product could only
    // say "some catalogue, some radius".
    this->m_crossmatchCatalogue = catalogue;
    this->m_crossmatchRadiusArcsec = radius;
    this->statusBar()->showMessage(u"Cross-matching against %1…"_s.arg(catalogue));

    this->m_crossmatchWatcher.setFuture(QtConcurrent::run(
            [url, token, sid, dsid, radius, catalogue]() -> CatalogueOverlayParseResult {
                CatalogueOverlayParseResult out;
                try {
                    BackendClient c(url, token);
                    if (!sid.isEmpty())
                        c.setSessionId(sid);
                    const BackendCrossmatchResult r = c.requestCrossmatch(dsid, radius, catalogue);
                    if (!r.valid) {
                        out.errorMessage = r.error.isEmpty() ? QStringLiteral("Cross-match failed.")
                                                             : r.error;
                        return out;
                    }
                    out.valid = true;
                    out.frameLabel = QStringLiteral("ICRS");
                    out.sourceLabel = QStringLiteral("Cross-match (%1)").arg(catalogue);
                    out.entries.reserve(r.sources.size());
                    for (const BackendCrossmatchSource &s : r.sources) {
                        CatalogueOverlayEntry e;
                        e.frame = CatalogueOverlayEntry::Frame::Sky;
                        e.shape = CatalogueOverlayEntry::Shape::Marker;
                        e.raDeg = s.raDeg;
                        e.decDeg = s.decDeg;
                        e.label = s.name;
                        out.entries.push_back(e);
                    }
                } catch (const std::exception &e) {
                    out.valid = false;
                    out.errorMessage = QString::fromUtf8(e.what());
                } catch (...) {
                    out.valid = false;
                    out.errorMessage = QStringLiteral("unknown error during cross-match");
                }
                return out;
            }));
}

void vtkWindowImage::onCrossmatchReady()
{
    if (this->actionCrossmatch)
        this->actionCrossmatch->setEnabled(true);
    this->statusBar()->clearMessage();
    const CatalogueOverlayParseResult parsed = this->m_crossmatchWatcher.result();
    if (!parsed.valid) {
        QMessageBox::warning(this, u"Cross-match"_s,
                             u"Cross-match failed:\n%1"_s.arg(parsed.errorMessage));
        return;
    }
    if (parsed.entries.empty()) {
        this->showPersistentStatusMessage(
                u"Cross-match: no catalogue sources in this image footprint."_s, 4000);
        // Still a result, and one worth keeping: "nothing within 60 arcsec of
        // this field" is an answer, and without a row the run left no trace at
        // all once the status message timed out.
        this->recordCrossmatch(0);
        return;
    }
    this->applyCatalogueOverlayResult(parsed);
    this->recordCrossmatch(static_cast<int>(parsed.entries.size()));
}

void vtkWindowImage::recordCrossmatch(int matches)
{
    const QString label = u"Cross-match with %1"_s.arg(
            m_crossmatchCatalogue.isEmpty() ? u"catalogue"_s : m_crossmatchCatalogue);
    QVariantMap params;
    params.insert(u"Catalogue"_s, m_crossmatchCatalogue);
    params.insert(u"Search radius"_s, u"%1 arcsec"_s.arg(m_crossmatchRadiusArcsec, 0, 'g', 4));
    params.insert(u"Matches"_s, QString::number(matches));
    params.insert(u"Scope"_s, u"this image's footprint"_s);

    const QVariantMap record = ToolResultDialog::makeRecord(
            u"Cross-match"_s,
            u"%1, within %2 arcsec of this image's footprint"_s.arg(m_crossmatchCatalogue)
                    .arg(m_crossmatchRadiusArcsec, 0, 'g', 4),
            { { u"Result"_s,
                { { u"Catalogue"_s, m_crossmatchCatalogue },
                  { u"Search radius"_s,
                    u"%1 arcsec"_s.arg(m_crossmatchRadiusArcsec, 0, 'g', 4) },
                  { u"Sources matched"_s, QString::number(matches) } } } });

    // One row per run, not one per catalogue: two radii against the same
    // catalogue are two different answers.
    this->registerAnalysisProduct(u"crossmatch"_s, label, params,
                                  this->scopedProductId(u"crossmatch-%1"_s.arg(
                                          ++this->m_crossmatchRunCount)),
                                  record);
}

void vtkWindowImage::clearCatalogueOverlay()
{
    this->catalogueOverlayLoaded = false;
    this->catalogueOverlayPixels.clear();
    this->catalogueOverlayEntries.clear();
    this->catalogueOverlayPolylines.clear();
    this->catalogueOverlayPolylineColors.clear();
    this->catalogueOverlayPolylineBand.clear();
    this->m_vlkbHiddenBands.clear();
    releaseToolSettings(u"Catalogue overlay"_s);
    refreshToolSettingsPanel(/*reveal=*/false);
    this->catalogueOverlayData->GetCellData()->SetScalars(nullptr);
    if (auto *m = this->catalogueOverlayActor->GetMapper())
        m->ScalarVisibilityOff();
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
    auto cellColors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    cellColors->SetNumberOfComponents(3);
    cellColors->SetName("colors");
    for (std::size_t p = 0; p < this->catalogueOverlayPolylines.size(); ++p) {
        const auto &polyline = this->catalogueOverlayPolylines[p];
        if (polyline.size() < 2) {
            continue;
        }
        // Skip polylines whose band is toggled off.
        if (p < this->catalogueOverlayPolylineBand.size()) {
            const QString &band = this->catalogueOverlayPolylineBand[p];
            if (!band.isEmpty() && this->m_vlkbHiddenBands.contains(band))
                continue;
        }
        this->catalogueOverlayCells->InsertNextCell(static_cast<vtkIdType>(polyline.size()));
        for (const auto &point : polyline) {
            const vtkIdType id = this->catalogueOverlayPoints->InsertNextPoint(point[0], point[1], 0.0);
            this->catalogueOverlayCells->InsertCellPoint(id);
        }
        const std::array<unsigned char, 3> rgb = (p < this->catalogueOverlayPolylineColors.size())
                ? this->catalogueOverlayPolylineColors[p]
                : std::array<unsigned char, 3>{ 255, 115, 38 };
        cellColors->InsertNextTypedTuple(rgb.data());
    }
    // Per-cell colours are only used when the overlay carries per-band tags
    // (VLKB multi-band sources). For plain CSV/ds9 overlays and filaments the band
    // is empty, so keep the single actor property colour: their appearance is
    // unchanged. (catalogueOverlayPolylineColors is always populated with a default
    // orange, so it cannot be used to distinguish the two cases.)
    const bool usePerCellColor =
            std::any_of(this->catalogueOverlayPolylineBand.cbegin(),
                        this->catalogueOverlayPolylineBand.cend(),
                        [](const QString &b) { return !b.isEmpty(); });
    if (usePerCellColor) {
        this->catalogueOverlayData->GetCellData()->SetScalars(cellColors);
    } else {
        this->catalogueOverlayData->GetCellData()->SetScalars(nullptr);
    }
    if (auto *m = this->catalogueOverlayActor->GetMapper()) {
        if (usePerCellColor) {
            m->SetScalarModeToUseCellData();
            m->SetColorModeToDirectScalars();
            m->ScalarVisibilityOn();
        } else {
            m->ScalarVisibilityOff();
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
        // Suppress labels of sources whose band is toggled off.
        if (static_cast<std::size_t>(pointIndex) < this->catalogueOverlaySourceFirstPolyline.size()) {
            const int first = this->catalogueOverlaySourceFirstPolyline[pointIndex];
            if (first >= 0 && first < static_cast<int>(this->catalogueOverlayPolylineBand.size())) {
                const QString &band = this->catalogueOverlayPolylineBand[first];
                if (!band.isEmpty() && this->m_vlkbHiddenBands.contains(band)) {
                    this->catalogueOverlayLabelActors[i]->SetVisibility(0);
                    continue;
                }
            }
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
        // Sources of a band toggled off are not pickable/hoverable.
        if (i < this->catalogueOverlaySourceFirstPolyline.size()) {
            const int first = this->catalogueOverlaySourceFirstPolyline[i];
            if (first >= 0 && first < static_cast<int>(this->catalogueOverlayPolylineBand.size())) {
                const QString &band = this->catalogueOverlayPolylineBand[first];
                if (!band.isEmpty() && this->m_vlkbHiddenBands.contains(band))
                    continue;
            }
        }
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
        // Do not highlight a source whose band is toggled off.
        if (first >= 0 && first < static_cast<int>(this->catalogueOverlayPolylineBand.size())) {
            const QString &band = this->catalogueOverlayPolylineBand[first];
            if (!band.isEmpty() && this->m_vlkbHiddenBands.contains(band)) {
                data->SetPoints(points);
                data->SetLines(cells);
                data->Modified();
                return;
            }
        }
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

    // BOTTOM dock, not the right column. A table wants width, and Qt gives every
    // dock in an area the same width: sharing the right column with the
    // Inspector meant collapsing the Inspector squeezed this to the rail width
    // (user report on the layout). The bottom area spans the window and is what
    // the 3-D catalogue viewer already does with its own table.
    this->catalogueDock = new QDockWidget(u"Catalogue"_s, this);
    this->catalogueDock->setObjectName(u"CatalogueDockImage"_s);
    // A TREE, because a VLKB source and its per-band detections are one thing
    // with parts: the band-merged row carries the arrow, the bands sit under it.
    // For a catalogue with no such relation the model stays flat and this looks
    // and behaves exactly like the table it replaces.
    this->catalogueTableView = new QTreeView(this->catalogueDock);
    this->catalogueTableModel = new CatalogueTableModel(this->catalogueDock);
    this->catalogueTableView->setModel(this->catalogueTableModel);
    this->catalogueTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->catalogueTableView->setSelectionMode(QAbstractItemView::SingleSelection);
    this->catalogueTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->catalogueTableView->setSortingEnabled(false);
    this->catalogueTableView->setRootIsDecorated(true);
    this->catalogueTableView->setUniformRowHeights(true);
    this->catalogueTableView->setAllColumnsShowFocus(true);
    this->catalogueTableView->setExpandsOnDoubleClick(false);   // double-click zooms
    this->catalogueTableView->header()->setStretchLastSection(true);
    this->catalogueDock->setWidget(this->catalogueTableView);
    this->addDockWidget(Qt::BottomDockWidgetArea, this->catalogueDock);
    this->catalogueDock->hide();   // shown when a catalogue loads

    QObject::connect(this, &vtkWindowImage::catalogueSourceSelectionChanged, this,
                     &vtkWindowImage::syncCatalogueTableSelection);
    QObject::connect(this->catalogueTableView->selectionModel(), &QItemSelectionModel::currentRowChanged,
                     this, [this](const QModelIndex &current) {
        if (this->syncingCatalogueSelection) {
            return;
        }
        // Mark the origin so the sync below does not scroll the row out from
        // under the pointer that just clicked it.
        this->m_catalogueSelectionFromTable = true;
        this->setSelectedCatalogueSourceIndex(
                this->catalogueTableModel->entryIndexFor(current));
        this->m_catalogueSelectionFromTable = false;
    });
    QObject::connect(this->catalogueTableView, &QTableView::doubleClicked, this,
                     [this](const QModelIndex &index) {
        const int entry = this->catalogueTableModel->entryIndexFor(index);
        if (entry >= 0) {
            this->setSelectedCatalogueSourceIndex(entry);
            this->centerViewOnCatalogueSource(entry);
        }
    });
    this->refreshCatalogueTable();
}

void vtkWindowImage::refreshCatalogueTable()
{
    if (!this->catalogueTableModel) {
        return;
    }
    this->catalogueTableModel->setFrameLabel(this->catalogueOverlayFrameLabel);
    this->catalogueTableModel->setExtraFields(this->catalogueOverlayExtraFields);
    this->catalogueTableModel->setEntries(&this->catalogueOverlayEntries);
    if (this->catalogueDock)
        this->catalogueDock->setVisible(!this->catalogueOverlayEntries.empty());
    if (this->catalogueTableView) {
        // No expander column for a catalogue without children: a plain CSV or
        // ds9 file must look exactly like the flat table it always was.
        const bool grouped = this->catalogueTableModel->isGrouped();
        this->catalogueTableView->setRootIsDecorated(grouped);
        this->catalogueTableView->setIndentation(grouped ? 16 : 0);
        for (int c = 0; c < this->catalogueTableModel->columnCount(); ++c)
            this->catalogueTableView->resizeColumnToContents(c);
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

void vtkWindowImage::exportCatalogueOverlay()
{
    if (this->catalogueOverlayEntries.empty()) {
        statusBar()->showMessage(u"No catalogue overlay to export."_s, 4000);
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
            this, u"Export Catalogue"_s,
            QFileInfo(this->filepath).completeBaseName() + u"_catalogue.csv"_s,
            u"CSV (*.csv);;All files (*)"_s);
    if (path.isEmpty())
        return;

    // Written to a temporary and renamed into place: a failure halfway (a full
    // disk) must not leave a truncated catalogue where a good one was, and must
    // not be announced as a successful export.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, u"Export Catalogue"_s,
                             u"Could not write %1: %2"_s.arg(path, f.errorString()));
        return;
    }
    QTextStream out(&f);
    // Quote on CR/LF too, not only commas and quotes: a value with a newline was
    // written as two records, which silently corrupts the file downstream. Used
    // for headers as well - a field name can carry anything a catalogue had.
    const auto csv = [](QString v) {
        const bool needsQuotes = v.contains(u',') || v.contains(u'"') || v.contains(u'\n')
                || v.contains(u'\r');
        v.replace(u'"', u"\"\""_s);
        return needsQuotes ? u"\""_s + v + u"\""_s : v;
    };

    // The frame is in the HEADER, not implied: these columns are Galactic for a
    // VLKB overlay and equatorial for a loaded catalogue, and a file that does
    // not say which is a file nobody can use later.
    const QString frame = this->catalogueOverlayFrameLabel.isEmpty()
            ? u"unknown"_s : this->catalogueOverlayFrameLabel;
    const bool galactic = frame.compare(u"Galactic"_s, Qt::CaseInsensitive) == 0;
    out << u"# catalogue exported from VisIVO Visual Analytics\n"_s
        << u"# dataset: "_s << this->filepath << u"\n"_s
        << u"# overlay: "_s << this->catalogueOverlaySummary << u"\n"_s
        << u"# sky frame: "_s << frame << u"\n"_s;
    out << (galactic ? u"name,type,band,glon_deg,glat_deg,"_s : u"name,type,band,ra_deg,dec_deg,"_s)
        << u"x_pix,y_pix,radius_x_pix,radius_y_pix,angle_deg_or_vertices"_s;
    for (const QString &field : this->catalogueOverlayExtraFields)
        out << u',' << csv(field);
    out << u'\n';

    int n = 0;
    for (std::size_t i = 0; i < this->catalogueOverlayEntries.size(); ++i) {
        const CatalogueOverlayEntry &e = this->catalogueOverlayEntries[i];
        const QString name = this->catalogueOverlayLabels.value(static_cast<qsizetype>(i)).trimmed();
        out << csv(name.isEmpty() ? u"source %1"_s.arg(i + 1) : name) << u','
            << (e.shape == CatalogueOverlayEntry::Shape::Ellipse    ? u"ellipse"_s
                : e.shape == CatalogueOverlayEntry::Shape::Polyline ? u"polyline"_s
                                                                   : u"marker"_s)
            << u',' << csv(e.band) << u',';
        if (e.frame == CatalogueOverlayEntry::Frame::Sky || e.hasSkyPosition)
            out << QString::number(e.raDeg, 'f', 6) << u',' << QString::number(e.decDeg, 'f', 6);
        else
            out << u","_s;
        out << u',' << QString::number(e.pixelX, 'f', 3)
            << u',' << QString::number(e.pixelY, 'f', 3) << u',';
        // Radii/angle whenever the source HAS them - a VLKB ellipse is drawn as a
        // sampled polyline but is still an ellipse - and the vertex count for a
        // genuine outline, so a filament is not exported as a bare centroid.
        if (e.radiusX > 0.0 || e.radiusY > 0.0)
            out << QString::number(e.radiusX, 'f', 3) << u','
                << QString::number(e.radiusY, 'f', 3) << u','
                << QString::number(e.angleDeg, 'f', 2);
        else if (e.shape == CatalogueOverlayEntry::Shape::Polyline)
            out << u",,"_s << QString::number(static_cast<int>(e.polyline.size()));
        else
            out << u",,"_s;
        // …and the catalogue's own fields, in the same order as the header.
        for (const QString &field : this->catalogueOverlayExtraFields) {
            QString value;
            for (const auto &kv : e.extra) {
                if (kv.first == field) { value = kv.second; break; }
            }
            out << u',' << csv(value);
        }
        out << u'\n';
        ++n;
    }
    out.flush();
    if (out.status() != QTextStream::Ok || !f.commit()) {
        QMessageBox::warning(this, u"Export Catalogue"_s,
                             u"The catalogue could not be written completely to %1: %2"_s
                                     .arg(path, f.errorString()));
        return;
    }
    statusBar()->showMessage(u"Exported %1 catalogue entries to %2"_s.arg(n).arg(
                                     QFileInfo(path).fileName()),
                             5000);
}

void vtkWindowImage::zoomToCatalogueSourceUnderCursor()
{
    if (!this->catalogueOverlayLoaded || this->regionMode != RegionMode::None)
        return;
    auto *iren = ui && ui->vtk && ui->vtk->renderWindow()
            ? ui->vtk->renderWindow()->GetInteractor() : nullptr;
    const int *pos = iren ? iren->GetEventPosition() : nullptr;
    if (!pos)
        return;
    // Pick under the cursor exactly as a single click does, then act only if it
    // actually landed on a source: a double-click on empty sky must not fling
    // the camera somewhere.
    // Clear first: with the overlay hidden the pick returns without touching the
    // selection, and a double-click on empty sky would zoom to whatever was
    // selected before - looking like a pick that never happened.
    const int before = this->selectedCatalogueSourceIndex;
    this->setSelectedCatalogueSourceIndex(-1);
    this->refreshCatalogueOverlayInteraction(pos[0], pos[1], true);
    if (this->selectedCatalogueSourceIndex < 0) {
        this->setSelectedCatalogueSourceIndex(before);   // nothing hit: leave it as it was
        return;
    }
    this->centerViewOnCatalogueSource(this->selectedCatalogueSourceIndex);
    const QString name = this->catalogueOverlayLabels
                                 .value(this->selectedCatalogueSourceIndex)
                                 .trimmed();
    this->statusBar()->showMessage(
            name.isEmpty() ? u"Zoomed to the selected source."_s
                           : u"Zoomed to %1"_s.arg(name),
            4000);
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
    if (index < 0 || !this->catalogueTableModel->indexForEntry(index).isValid()) {
        this->catalogueTableView->clearSelection();
    } else {
        const QModelIndex modelIndex = this->catalogueTableModel->indexForEntry(index);
        if (!modelIndex.isValid()) {
            this->syncingCatalogueSelection = false;
            return;
        }
        // Open the parent, or selecting a band would highlight a row inside a
        // collapsed group and look like nothing happened.
        if (modelIndex.parent().isValid())
            this->catalogueTableView->expand(modelIndex.parent());
        this->catalogueTableView->selectionModel()->setCurrentIndex(
                modelIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        // Do NOT scroll when the click came from the table itself: recentring
        // the row under the cursor moves it away, and the second click of a
        // double-click lands on a different source. And when the selection does
        // come from the image, only scroll if the row is not already in view -
        // EnsureVisible, not PositionAtCenter, so the table stops jumping.
        if (!this->m_catalogueSelectionFromTable) {
            const QRect rowRect = this->catalogueTableView->visualRect(modelIndex);
            const QRect viewport = this->catalogueTableView->viewport()->rect();
            if (!rowRect.isValid() || !viewport.contains(rowRect.center()))
                this->catalogueTableView->scrollTo(modelIndex,
                                                   QAbstractItemView::EnsureVisible);
        }
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
    // Projection-aware: gnomonic for TAN images, linear for CAR (Hi-GAL cutouts)
    // and other linear WCS — so the overlay lands on the data on both.
    nativeWorldToPixel(this->remoteDatasetCtype[0], nativeX, nativeY,
                       this->remoteDatasetCrpix, this->remoteDatasetCrval,
                       this->remoteDatasetCdelt, pixel[0], pixel[1]);
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]);
}

// ── VLKB catalogue overlays (compact sources / filaments over the footprint) ──
namespace {

// Blocking HTTP GET on a worker thread (spins a local event loop).
QByteArray vlkbGet(const QString &url, QString &err)
{
    QNetworkAccessManager nam;
    QEventLoop loop;
    QNetworkRequest req{ QUrl(url) };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);   // don't leave the worker (and the buttons) stuck
    QNetworkReply *reply = nam.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray body;
    if (reply->error() != QNetworkReply::NoError)
        err = reply->errorString();
    else
        body = reply->readAll();
    reply->deleteLater();
    return body;
}

// The band-merged column suffix in compactsources.sed_view_final (glonft/glatft,
// fwhmaft/fwhmbft/paft). This is the reference "band-merged" source shown by default.
const QString kVlkbMergedBand = u"ft"_s;

// Every band whose ellipse (fwhma{b},fwhmb{b},pa{b}) exists in sed_view_final,
// ordered merged-first then by ascending wavelength (µm). One query fetches them
// all; the band panel only exposes bands that actually returned a detection, and
// each band can be toggled on/off. Verified against TAP_SCHEMA.columns.
const QStringList kVlkbBands = {
    u"ft"_s,                                             // band-merged (default)
    u"21"_s, u"22"_s, u"24"_s,                           // MSX 21, WISE 22, MIPSGAL 24
    u"70"_s, u"160"_s, u"250"_s, u"350"_s, u"500"_s,     // Herschel Hi-GAL
    u"870"_s, u"1100"_s,                                 // ATLASGAL 870, BGPS 1.1mm
};

std::array<double, 3> vlkbBandColor(const QString &band)
{
    if (band == u"ft"_s)   return { 1.00, 1.00, 1.00 };   // merged: white
    if (band == u"21"_s)   return { 0.80, 0.30, 0.90 };   // violet
    if (band == u"22"_s)   return { 0.60, 0.35, 0.95 };   // indigo
    if (band == u"24"_s)   return { 0.95, 0.35, 0.65 };   // pink
    if (band == u"70"_s)   return { 1.00, 0.30, 0.30 };   // red
    if (band == u"160"_s)  return { 1.00, 0.60, 0.10 };   // orange
    if (band == u"250"_s)  return { 1.00, 0.92, 0.25 };   // yellow
    if (band == u"350"_s)  return { 0.35, 0.95, 0.45 };   // green
    if (band == u"500"_s)  return { 0.45, 0.65, 1.00 };   // blue
    if (band == u"870"_s)  return { 0.20, 0.85, 0.85 };   // teal
    if (band == u"1100"_s) return { 0.95, 0.45, 0.95 };   // magenta
    return { 0.90, 0.90, 0.90 };
}

// Human-readable label for a band tag, used on the toggle panel checkboxes.
QString vlkbBandLabel(const QString &band)
{
    if (band == u"ft"_s) return u"Band-merged"_s;
    return u"Band %1 µm"_s.arg(band);
}

// Read the configured VLKB base URL straight from the Settings.ini key, WITHOUT
// constructing a Settings object (whose ctor probes python3 synchronously).
QString vlkbBaseUrl()
{
    const QString ini = QDir(QDir::home().absoluteFilePath(qApp->applicationName()))
                                .absoluteFilePath(u"Settings.ini"_s);
    const QString url = QSettings(ini, QSettings::IniFormat)
                                .value(u"VLKB/url"_s).toString().trimmed();
    return url.isEmpty() ? u"https://vlkb.ia2.inaf.it"_s : url;
}

QString vlkbTapUrl(const QString &base, const QString &adql)
{
    QUrl url(base + QStringLiteral("/tap/sync"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("doQuery"));
    q.addQueryItem(QStringLiteral("LANG"), QStringLiteral("ADQL"));
    q.addQueryItem(QStringLiteral("FORMAT"), QStringLiteral("csv"));
    q.addQueryItem(QStringLiteral("QUERY"), adql);
    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded);
}

// Worker: compact sources for EVERY band in one query. The ADQL selects
// glonft,glatft (band-merged position, used as fallback) then, per band in `bands`
// order, (glon{b},glat{b},fwhma{b},fwhmb{b},pa{b}). Each row yields one ellipse per
// band it was detected in (fwhm>0), positioned at that band's OWN centroid (the
// legacy client used per-band positions — the merged centroid can sit a couple of
// pixels off the actual per-band peak, and one merged source can group several
// distinct per-band detections). Dedup is therefore on band + per-band position.
VlkbCatalogueRaw fetchVlkbSources(const QString &base, const QString &adql,
                                  const QStringList &bands)
{
    VlkbCatalogueRaw out;
    out.sourceLabel = QStringLiteral("VLKB compact sources");
    QString err;
    const QByteArray csv = vlkbGet(vlkbTapUrl(base, adql), err);
    if (!err.isEmpty()) { out.error = err; return out; }
    const QList<QByteArray> lines = csv.split('\n');
    if (lines.isEmpty()) { out.error = QStringLiteral("Empty VLKB response."); return out; }
    const int nb = bands.size();

    // Parse by NAME, from the response's own header, not by position: the SELECT
    // is now built from the view's real column list, so positions are whatever
    // the service returns - and the columns we do not interpret become fields of
    // the source rather than being skipped.
    // The response header obeys the same quoting rules as its rows.
    int headerEnd = 0;
    QString headerRecord = QString::fromUtf8(lines[0]);
    while (CatalogueOverlayUtils::csvRecordIsIncomplete(headerRecord)
           && headerEnd + 1 < lines.size())
        headerRecord += u'\n' + QString::fromUtf8(lines[++headerEnd]);
    if (CatalogueOverlayUtils::csvRecordIsIncomplete(headerRecord)) {
        out.error = QStringLiteral("The VLKB response header ends inside a quoted field.");
        return out;
    }
    const QStringList header =
            CatalogueOverlayUtils::splitSimpleCsvLine(headerRecord.trimmed());
    QHash<QString, int> col;
    for (int i = 0; i < header.size(); ++i)
        col.insert(header[i].trimmed().toLower(), i);
    const auto idx = [&col](const QString &name) { return col.value(name.toLower(), -1); };
    const auto num = [](const QStringList &c, int i, bool *ok) {
        if (i < 0 || i >= c.size()) { *ok = false; return 0.0; }
        return c[i].trimmed().toDouble(ok);
    };
    // Which band does a column belong to? Longest suffix wins, so fint1100 is
    // not read as band "00".
    QStringList bandsByLength = bands;
    std::sort(bandsByLength.begin(), bandsByLength.end(),
              [](const QString &a, const QString &b) { return a.size() > b.size(); });
    // A suffix alone does not make a column band-specific: `shift` ends with the
    // band tag "ft" and `quality2024` with "24". What DOES distinguish them is
    // that a real per-band column has siblings - fint70, fint160, fint250 … - so
    // a stem is only treated as banded when the header carries it for at least
    // two bands. Everything else describes the source and belongs to every band.
    QSet<QString> bandedStems;
    {
        QHash<QString, int> stemCount;
        for (const QString &name : header) {
            const QString n = name.trimmed();
            for (const QString &b : bandsByLength) {
                if (n.size() > b.size() && n.endsWith(b, Qt::CaseInsensitive)) {
                    stemCount[n.left(n.size() - b.size()).toLower()] += 1;
                    break;
                }
            }
        }
        for (auto it = stemCount.cbegin(); it != stemCount.cend(); ++it) {
            if (it.value() >= 2)
                bandedStems.insert(it.key());
        }
    }
    const auto bandOfColumn = [&bandsByLength, &bandedStems](const QString &name) {
        for (const QString &b : bandsByLength) {
            if (name.size() > b.size() && name.endsWith(b, Qt::CaseInsensitive)
                && bandedStems.contains(name.left(name.size() - b.size()).toLower()))
                return b;
        }
        return QString();
    };
    static const QStringList kGeometryStems = { u"glon"_s, u"glat"_s, u"fwhma"_s, u"fwhmb"_s,
                                                u"pa"_s };
    const auto isGeometry = [&bandOfColumn](const QString &name) {
        const QString band = bandOfColumn(name);
        const QString stem = band.isEmpty() ? name : name.left(name.size() - band.size());
        return kGeometryStems.contains(stem, Qt::CaseInsensitive);
    };

    const int iGlonFt = idx(u"glonft"_s), iGlatFt = idx(u"glatft"_s);
    QSet<QString> seen;   // dedupe the SED view's repeated rows by band + position
    for (int i = headerEnd + 1; i < lines.size(); ++i) {   // after the header record
        QString line = QString::fromUtf8(lines[i]);
        // Same rule as the file parser: a text column returned by SELECT * can
        // contain a newline inside quotes, and that is still one record.
        while (CatalogueOverlayUtils::csvRecordIsIncomplete(line) && i + 1 < lines.size())
            line += u'\n' + QString::fromUtf8(lines[++i]);
        if (CatalogueOverlayUtils::csvRecordIsIncomplete(line)) {
            // Truncated response: stop rather than read fields out of a fragment.
            qWarning().noquote()
                    << QStringLiteral("[vlkb] response ends inside a quoted field; "
                                      "the last record was dropped");
            break;
        }
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const QStringList c = CatalogueOverlayUtils::splitSimpleCsvLine(line);
        if (c.size() < 2) continue;
        bool okXft = false, okYft = false;
        const double glonft = num(c, iGlonFt, &okXft), glatft = num(c, iGlatFt, &okYft);
        for (int b = 0; b < nb; ++b) {
            const QString suffix = bands[b];
            bool gx = false, gy = false, a = false, bb = false, p = false;
            double glon = num(c, idx(u"glon"_s + suffix), &gx);
            double glat = num(c, idx(u"glat"_s + suffix), &gy);
            const double fa = num(c, idx(u"fwhma"_s + suffix), &a);
            const double fb = num(c, idx(u"fwhmb"_s + suffix), &bb);
            const double pa = num(c, idx(u"pa"_s + suffix), &p);
            if (!a || !bb || !p || fa <= 0.0 || fb <= 0.0) continue;   // not in this band
            // Fall back to the band-merged centroid if this band lacks its own.
            if (!gx || !gy || glon <= -9990.0 || glat <= -9990.0) {
                if (!okXft || !okYft) continue;
                glon = glonft; glat = glatft;
            }
            // Identity of the SOURCE this detection belongs to: the band-merged
            // designation, or idcss when the view publishes no designation.
            const int iDesignation = idx(u"designationft"_s);
            const QString published = (iDesignation >= 0 && iDesignation < c.size())
                    ? c[iDesignation].trimmed()
                    : QString();
            const int iIdcss = idx(u"idcss"_s);
            QString group = published;
            if (group.isEmpty() && iIdcss >= 0 && iIdcss < c.size())
                group = c[iIdcss].trimmed();

            // The group is part of the identity: the SED view can hand the same
            // per-band detection to two different band-merged sources, and a key
            // of band+position alone gave it to whichever row came first - the
            // other source silently lost that band from its tree.
            const QString key = QStringLiteral("%1_%2_%3_%4").arg(bands[b])
                    .arg(glon, 0, 'f', 5).arg(glat, 0, 'f', 5).arg(group);
            if (seen.contains(key)) continue;
            seen.insert(key);
            // A NAME, not "Source 64": the published designation when there is
            // one, otherwise the standard Galactic designation written from the
            // position (G039.884-0.163), plus the band, since one physical source
            // yields one ellipse per band. This is also what makes "Show
            // Catalogue Labels" draw anything - it only draws non-empty labels.
            const QString designation = !published.isEmpty()
                    ? QStringLiteral("%1 (%2)").arg(published, vlkbBandLabel(bands[b]))
                    : QStringLiteral("G%1%2%3 (%4)")
                            .arg(glon, 7, 'f', 3, QLatin1Char('0'))
                            .arg(glat < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                            .arg(std::abs(glat), 6, 'f', 3, QLatin1Char('0'))
                            .arg(vlkbBandLabel(bands[b]));
            // Everything else this row carries: the columns of THIS band, plus
            // the ones that belong to no band (they describe the source).
            QList<QPair<QString, QString>> fields;
            for (int k = 0; k < header.size() && k < c.size(); ++k) {
                const QString name = header[k].trimmed();
                if (name.isEmpty() || isGeometry(name))
                    continue;
                const QString owner = bandOfColumn(name);
                if (!owner.isEmpty() && owner != suffix)
                    continue;   // another band's column
                const QString value = c[k].trimmed();
                if (value.isEmpty() || value == u"-9999"_s || value == u"-9999.0"_s)
                    continue;
                fields.append({ name, value });
            }
            out.ellipses.push_back({ glon, glat, fa, fb, pa, bands[b], designation, fields,
                                     group });
        }
    }
    out.valid = true;
    return out;
}

// Worker: filament outlines. Each row is "idfil,\"glon1_..._,glat1_..._\"".
VlkbCatalogueRaw fetchVlkbFilaments(const QString &base, const QString &adql)
{
    VlkbCatalogueRaw out;
    out.sourceLabel = QStringLiteral("VLKB filaments");
    QString err;
    const QByteArray csv = vlkbGet(vlkbTapUrl(base, adql), err);
    if (!err.isEmpty()) { out.error = err; return out; }
    const QString all = QString::fromUtf8(csv);
    const QStringList rows = all.split(u'\n');
    for (int i = 1; i < rows.size(); ++i) {          // skip header
        const QString row = rows[i];
        const int firstComma = row.indexOf(u',');
        if (firstComma < 0) continue;
        const QString id = row.left(firstComma).trimmed();
        QString contour = row.mid(firstComma + 1).trimmed();
        if (contour.startsWith(u'"')) contour = contour.mid(1);
        if (contour.endsWith(u'"')) contour.chop(1);
        // contour = "glonlist , glatlist" with each list underscore-separated.
        const int sep = contour.indexOf(u',');
        if (sep < 0) continue;
        const auto glons = contour.left(sep).split(u'_', Qt::SkipEmptyParts);
        const auto glats = contour.mid(sep + 1).split(u'_', Qt::SkipEmptyParts);
        const int n = std::min(glons.size(), glats.size());
        if (n < 2) continue;
        VlkbCatalogueRaw::Polyline poly;
        poly.label = QStringLiteral("filament %1").arg(id);
        poly.pts.reserve(n);
        for (int k = 0; k < n; ++k) {
            bool a = false, b = false;
            const double g = glons[k].toDouble(&a), l = glats[k].toDouble(&b);
            if (a && b) poly.pts.push_back({ g, l });
        }
        if (poly.pts.size() >= 2) out.polylines.push_back(std::move(poly));
    }
    out.valid = true;
    return out;
}

}  // namespace

bool vtkWindowImage::galacticToImagePixel(double glon, double glat, std::array<double, 2> &pixel) const
{
    if (!this->remoteHasCelestialAxes()) return false;
    const int nf = this->remoteNativeCelestialFrame();
    if (nf < 0) return false;
    double nx = glon, ny = glat;
    if (nf != WCS_GALACTIC)
        wcscon(WCS_GALACTIC, nf, 2000.0, 2000.0, &nx, &ny, 2000.0);   // Galactic → native
    if (std::abs(this->remoteDatasetCdelt[0]) <= std::numeric_limits<double>::epsilon()
        || std::abs(this->remoteDatasetCdelt[1]) <= std::numeric_limits<double>::epsilon())
        return false;
    // Projection-aware (TAN gnomonic / CAR linear), consistent with the displayed
    // axes and with catalogueWorldToPixel.
    nativeWorldToPixel(this->remoteDatasetCtype[0], nx, ny,
                       this->remoteDatasetCrpix, this->remoteDatasetCrval,
                       this->remoteDatasetCdelt, pixel[0], pixel[1]);
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]);
}

bool vtkWindowImage::imageGalacticFootprint(double &glonMin, double &glonMax,
                                            double &glatMin, double &glatMax) const
{
    if (!this->remoteHasCelestialAxes()) return false;
    const int nf = this->remoteNativeCelestialFrame();
    if (nf < 0) return false;
    if (!this->layers) return false;
    auto *img = this->layers->getImageData(this->layers->getMasterIndex());
    if (!img) return false;
    int ext[6]; double sp[3], org[3];
    img->GetExtent(ext); img->GetSpacing(sp); img->GetOrigin(org);
    // The four image corners in the overlay's (full-res-pixel) world coordinates.
    const double xs[2] = { org[0] + ext[0] * sp[0], org[0] + ext[1] * sp[0] };
    const double ys[2] = { org[1] + ext[2] * sp[1], org[1] + ext[3] * sp[1] };
    double lmin = 1e30, lmax = -1e30, bmin = 1e30, bmax = -1e30;
    for (double x : xs) {
        for (double y : ys) {
            bool okx = false, oky = false;
            // remoteVoxelToWcs is the app's own (linear) pixel→world — using it keeps
            // the query box centred on the ACTUAL image, not on a CRVAL that (for a
            // mosaic-referenced cutout) can sit far off the cutout.
            double glon = this->remoteVoxelToWcs(0, x, &okx);
            double glat = this->remoteVoxelToWcs(1, y, &oky);
            if (!okx || !oky) continue;
            if (nf != WCS_GALACTIC)
                wcscon(nf, WCS_GALACTIC, 2000.0, 2000.0, &glon, &glat, 2000.0);
            lmin = std::min(lmin, glon); lmax = std::max(lmax, glon);
            bmin = std::min(bmin, glat); bmax = std::max(bmax, glat);
        }
    }
    if (lmax < lmin || bmax < bmin) return false;
    const double ml = 0.1 * (lmax - lmin) + 0.01;   // small margin
    const double mb = 0.1 * (bmax - bmin) + 0.01;
    glonMin = lmin - ml; glonMax = lmax + ml;
    glatMin = bmin - mb; glatMax = bmax + mb;
    return true;
}

void vtkWindowImage::overlayVlkbSources()
{
    if (this->m_vlkbCatWatcher.isRunning() || this->m_vlkbSelectActive) return;
    if (!this->remoteHasCelestialAxes()) {
        QMessageBox::warning(this, u"VLKB sources"_s,
                             u"This image has no celestial WCS to place sources on."_s);
        return;
    }
    // Query ALL Hi-GAL bands in one go; per-band visibility is toggled afterwards.
    this->beginVlkbRegionSelect(false);
}

void vtkWindowImage::overlayVlkbFilaments()
{
    if (this->m_vlkbCatWatcher.isRunning() || this->m_vlkbSelectActive) return;
    if (!this->remoteHasCelestialAxes()) {
        QMessageBox::warning(this, u"VLKB filaments"_s,
                             u"This image has no celestial WCS to place filaments on."_s);
        return;
    }
    this->beginVlkbRegionSelect(true);
}

// Build the ADQL for the current mode over the given Galactic box and run the query.
void vtkWindowImage::runVlkbCatalogueQuery(double glonMin, double glonMax,
                                           double glatMin, double glatMax)
{
    if (this->m_vlkbCatWatcher.isRunning()) return;
    const QString vlkbUrl = vlkbBaseUrl();
    if (this->m_vlkbCatIsFilament) {
        const QString adql = QStringLiteral(
                "SELECT idfil,contour FROM filaments.filaments "
                "WHERE glon BETWEEN %1 AND %2 AND glat BETWEEN %3 AND %4")
                .arg(glonMin, 0, 'f', 6).arg(glonMax, 0, 'f', 6)
                .arg(glatMin, 0, 'f', 6).arg(glatMax, 0, 'f', 6);
        this->statusBar()->showMessage(u"Querying VLKB filaments…"_s);
        this->m_vlkbCatWatcher.setFuture(QtConcurrent::run(
                [vlkbUrl, adql]() { return fetchVlkbFilaments(vlkbUrl, adql); }));
    } else {
        // One query returns every band's position + ellipse columns; toggled per
        // band later. Per-band glon/glat centre each band's ellipse on its own peak.
        // SELECT *: the whole row, which is what the table and the export want -
        // the SED (fluxes per band) is the reason this catalogue exists, and the
        // parser below reads the response BY NAME from its own header, so it does
        // not care what the service returns or in which order.
        //
        // The two roads not taken: naming 203 columns explicitly needs a
        // TAP_SCHEMA round trip and breaks on `distance`, a column here whose
        // name is an ADQL reserved word (unquoted it makes the service answer
        // 400); and guessing a subset would silently omit whatever the schema
        // gains next.
        const QStringList bands = kVlkbBands;
        const QString adql =
                QStringLiteral("SELECT * FROM compactsources.sed_view_final "
                               "WHERE glonft BETWEEN %1 AND %2 AND glatft BETWEEN %3 AND %4")
                        .arg(glonMin, 0, 'f', 6).arg(glonMax, 0, 'f', 6)
                        .arg(glatMin, 0, 'f', 6).arg(glatMax, 0, 'f', 6);
        this->statusBar()->showMessage(u"Querying VLKB compact sources (all bands)…"_s);
        this->m_vlkbCatWatcher.setFuture(QtConcurrent::run(
                [vlkbUrl, adql, bands]() { return fetchVlkbSources(vlkbUrl, adql, bands); }));
    }
}

void vtkWindowImage::runVlkbCatalogueQueryWholeImage(bool isFilament)
{
    this->m_vlkbCatIsFilament = isFilament;
    double glonMin, glonMax, glatMin, glatMax;
    if (!this->imageGalacticFootprint(glonMin, glonMax, glatMin, glatMax)) {
        QMessageBox::warning(this, u"VLKB catalogue"_s, u"Could not determine the image footprint."_s);
        return;
    }
    this->runVlkbCatalogueQuery(glonMin, glonMax, glatMin, glatMax);
}

// ── Rubber-band region selection over the image ─────────────────────────────

void vtkWindowImage::beginVlkbRegionSelect(bool isFilament)
{
    auto *iren = ui->vtk && ui->vtk->renderWindow()
            ? ui->vtk->renderWindow()->GetInteractor() : nullptr;
    if (!iren) { this->runVlkbCatalogueQueryWholeImage(isFilament); return; }
    this->m_vlkbCatIsFilament = isFilament;
    this->m_vlkbSelectActive = true;
    this->m_vlkbSelectDragging = false;
    // Take over the pointer with a passive style so a drag draws the rectangle
    // instead of panning/window-levelling; the EXACT prior style is restored in
    // endVlkbRegionSelect() so profile/region/measurement modes survive.
    this->m_vlkbPrevStyle = iren->GetInteractorStyle();
    vtkNew<vtkInteractorStyleUser> passive;
    iren->SetInteractorStyle(passive);
    if (!this->m_vlkbPressTag)
        this->m_vlkbPressTag = iren->AddObserver(vtkCommand::LeftButtonPressEvent, this,
                                                 &vtkWindowImage::onVlkbSelectPress, 1.0);
    if (!this->m_vlkbMoveTag)
        this->m_vlkbMoveTag = iren->AddObserver(vtkCommand::MouseMoveEvent, this,
                                                &vtkWindowImage::onVlkbSelectMove, 1.0);
    if (!this->m_vlkbReleaseTag)
        this->m_vlkbReleaseTag = iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                                                   &vtkWindowImage::onVlkbSelectRelease, 1.0);
    if (ui->vtk) ui->vtk->setCursor(Qt::CrossCursor);
    this->statusBar()->showMessage(
            u"Drag a rectangle to query %1 in that area (or click once for the whole image)."_s
                    .arg(isFilament ? u"filaments"_s : u"sources"_s), 0);
}

void vtkWindowImage::endVlkbRegionSelect()
{
    auto *iren = ui->vtk && ui->vtk->renderWindow()
            ? ui->vtk->renderWindow()->GetInteractor() : nullptr;
    if (iren) {
        if (this->m_vlkbPressTag)   { iren->RemoveObserver(this->m_vlkbPressTag);   this->m_vlkbPressTag = 0; }
        if (this->m_vlkbMoveTag)    { iren->RemoveObserver(this->m_vlkbMoveTag);    this->m_vlkbMoveTag = 0; }
        if (this->m_vlkbReleaseTag) { iren->RemoveObserver(this->m_vlkbReleaseTag); this->m_vlkbReleaseTag = 0; }
        if (this->m_vlkbPrevStyle) {
            iren->SetInteractorStyle(this->m_vlkbPrevStyle);
        } else {
            vtkNew<vtkInteractorStyleImage> imgStyle;
            iren->SetInteractorStyle(imgStyle);
        }
    }
    this->m_vlkbPrevStyle = nullptr;
    if (this->m_vlkbRubberActor) {
        if (auto *ren = ui->vtk && ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr)
            ren->RemoveActor2D(this->m_vlkbRubberActor);
        this->m_vlkbRubberActor = nullptr;
    }
    this->m_vlkbSelectDragging = false;
    // Clear the "active" flag only AFTER the current event turn, so the permanent
    // lower-priority release observers (endImageInteraction / finishRegionInteraction)
    // still see it true and stay suppressed for the release that ended the selection.
    QTimer::singleShot(0, this, [this]() { this->m_vlkbSelectActive = false; });
    if (ui->vtk) ui->vtk->setCursor(Qt::ArrowCursor);
    this->statusBar()->clearMessage();
    if (ui->vtk && ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::onVlkbSelectPress()
{
    auto *iren = ui->vtk->renderWindow()->GetInteractor();
    const int *p = iren->GetEventPosition();
    this->m_vlkbSelStart[0] = p[0]; this->m_vlkbSelStart[1] = p[1];
    this->m_vlkbSelEnd[0] = p[0];   this->m_vlkbSelEnd[1] = p[1];
    this->m_vlkbSelectDragging = true;
}

void vtkWindowImage::onVlkbSelectMove()
{
    if (!this->m_vlkbSelectDragging) return;
    auto *iren = ui->vtk->renderWindow()->GetInteractor();
    const int *p = iren->GetEventPosition();
    this->m_vlkbSelEnd[0] = p[0]; this->m_vlkbSelEnd[1] = p[1];
    auto *ren = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer();
    if (!ren) return;
    // Draw the rubber band as a display-space rectangle.
    auto pts = vtkSmartPointer<vtkPoints>::New();
    const double z = 0.0;
    pts->InsertNextPoint(this->m_vlkbSelStart[0], this->m_vlkbSelStart[1], z);
    pts->InsertNextPoint(this->m_vlkbSelEnd[0],   this->m_vlkbSelStart[1], z);
    pts->InsertNextPoint(this->m_vlkbSelEnd[0],   this->m_vlkbSelEnd[1],   z);
    pts->InsertNextPoint(this->m_vlkbSelStart[0], this->m_vlkbSelEnd[1],   z);
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    lines->InsertNextCell(5);
    for (vtkIdType i = 0; i < 4; ++i) lines->InsertCellPoint(i);
    lines->InsertCellPoint(0);
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    pd->SetPoints(pts); pd->SetLines(lines);
    auto mapper = vtkSmartPointer<vtkPolyDataMapper2D>::New();
    mapper->SetInputData(pd);
    auto coord = vtkSmartPointer<vtkCoordinate>::New();
    coord->SetCoordinateSystemToDisplay();
    mapper->SetTransformCoordinate(coord);
    if (!this->m_vlkbRubberActor) {
        this->m_vlkbRubberActor = vtkSmartPointer<vtkActor2D>::New();
        ren->AddActor2D(this->m_vlkbRubberActor);
    }
    this->m_vlkbRubberActor->SetMapper(mapper);
    this->m_vlkbRubberActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->m_vlkbRubberActor->GetProperty()->SetLineWidth(1.5);
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::onVlkbSelectRelease()
{
    if (!this->m_vlkbSelectActive) return;
    if (auto *iren = ui->vtk && ui->vtk->renderWindow()
            ? ui->vtk->renderWindow()->GetInteractor() : nullptr) {
        const int *p = iren->GetEventPosition();   // use the actual release corner
        this->m_vlkbSelEnd[0] = p[0]; this->m_vlkbSelEnd[1] = p[1];
    }
    const bool isFilament = this->m_vlkbCatIsFilament;
    const int dx = std::abs(this->m_vlkbSelEnd[0] - this->m_vlkbSelStart[0]);
    const int dy = std::abs(this->m_vlkbSelEnd[1] - this->m_vlkbSelStart[1]);
    auto *ren = ui->vtk && ui->vtk->renderWindow()
            ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    // Snapshot the two display corners before endVlkbRegionSelect() clears state.
    const int s0 = this->m_vlkbSelStart[0], s1 = this->m_vlkbSelStart[1];
    const int e0 = this->m_vlkbSelEnd[0],   e1 = this->m_vlkbSelEnd[1];
    this->endVlkbRegionSelect();

    if (dx < 5 || dy < 5 || !ren) {          // a click, not a drag → whole image
        this->runVlkbCatalogueQueryWholeImage(isFilament);
        return;
    }
    // Display corners → world (full-res pixel) → native lon/lat → Galactic box.
    const int nf = this->remoteNativeCelestialFrame();
    double lmin = 1e30, lmax = -1e30, bmin = 1e30, bmax = -1e30;
    const int xs[2] = { s0, e0 }, ys[2] = { s1, e1 };
    bool any = false;
    for (int xi = 0; xi < 2; ++xi) {
        for (int yi = 0; yi < 2; ++yi) {
            this->coordinate->SetValue(xs[xi], ys[yi], 0.0);
            const double *w = this->coordinate->GetComputedWorldValue(ren);
            if (!w) continue;
            bool okx = false, oky = false;
            double glon = this->remoteVoxelToWcs(0, w[0], &okx);
            double glat = this->remoteVoxelToWcs(1, w[1], &oky);
            if (!okx || !oky) continue;
            if (nf >= 0 && nf != WCS_GALACTIC)
                wcscon(nf, WCS_GALACTIC, 2000.0, 2000.0, &glon, &glat, 2000.0);
            lmin = std::min(lmin, glon); lmax = std::max(lmax, glon);
            bmin = std::min(bmin, glat); bmax = std::max(bmax, glat);
            any = true;
        }
    }
    this->m_vlkbCatIsFilament = isFilament;
    if (!any || lmax < lmin) { this->runVlkbCatalogueQueryWholeImage(isFilament); return; }
    this->runVlkbCatalogueQuery(lmin, lmax, bmin, bmax);
}

void vtkWindowImage::onVlkbCatalogueReady()
{
    const VlkbCatalogueRaw raw = this->m_vlkbCatWatcher.result();
    this->statusBar()->clearMessage();
    if (!raw.valid) {
        QMessageBox::warning(this, u"VLKB catalogue"_s,
                             u"VLKB query failed:\n\n%1"_s.arg(raw.error));
        return;
    }
    CatalogueOverlayParseResult parsed;
    parsed.valid = true;
    parsed.frameLabel = u"Galactic"_s;
    parsed.sourceLabel = raw.sourceLabel;

    // Draw each source as its FWHM ellipse. Project only the source CENTRE to
    // pixels, then build the ellipse in PIXEL space: semi-major (fwhma/2) along the
    // pixel +X axis, semi-minor (fwhmb/2) along +Y, rotated counter-clockwise by the
    // catalogue position angle. This replicates the legacy VLKB client (vtkEllipse)
    // exactly and was confirmed against the 250 µm cutout data (θ = +PA gave ~0°
    // residual). A sky-space "PA from North" build is rotated wrong on these GLON-TAN
    // cutouts whose tangent point sits far off the image.
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    constexpr int kEllipseSamples = 48;
    const double arcpix = 0.5
            * (std::abs(this->remoteDatasetCdelt[0]) + std::abs(this->remoteDatasetCdelt[1]))
            * 3600.0;   // arcsec per pixel
    for (const auto &e : raw.ellipses) {
        if (e.fwhmaArcsec <= 0.0 || e.fwhmbArcsec <= 0.0 || arcpix <= 0.0) {
            ++parsed.skippedEntries;
            continue;
        }
        std::array<double, 2> c{ 0.0, 0.0 };
        if (!this->galacticToImagePixel(e.glon, e.glat, c)) { ++parsed.skippedEntries; continue; }
        const double aPx = 0.5 * e.fwhmaArcsec / arcpix;   // semi-major in pixels (along +X)
        const double bPx = 0.5 * e.fwhmbArcsec / arcpix;   // semi-minor in pixels (along +Y)
        // Kept on the entry even though the SHAPE is a sampled polyline: without
        // them the table's Size/Angle columns were empty for every VLKB source
        // and the export wrote a catalogue with no shape at all.
        const double th = e.paDeg * kDeg2Rad;
        const double cth = std::cos(th), sth = std::sin(th);
        CatalogueOverlayEntry ce;
        ce.frame = CatalogueOverlayEntry::Frame::Image;
        ce.shape = CatalogueOverlayEntry::Shape::Polyline;
        // The SHAPE is in pixels (that is how the ellipse is built), but the
        // source still has a sky position, and the table had nothing to show in
        // its two sky columns because nobody carried it across.
        ce.raDeg = e.glon;
        ce.decDeg = e.glat;
        ce.hasSkyPosition = true;
        ce.band = e.band;
        ce.radiusX = aPx;
        ce.radiusY = bPx;
        ce.angleDeg = e.paDeg;
        ce.extra = e.fields;
        ce.groupKey = e.groupKey;
        for (const auto &kv : e.fields) {
            if (!parsed.extraFields.contains(kv.first))
                parsed.extraFields.append(kv.first);
        }
        ce.color = vlkbBandColor(e.band);
        ce.label = e.label;
        for (int i = 0; i <= kEllipseSamples; ++i) {
            const double t = (2.0 * 3.14159265358979323846 * i) / kEllipseSamples;
            const double ex = aPx * std::cos(t);
            const double ey = bPx * std::sin(t);
            ce.polyline.push_back({ c[0] + ex * cth - ey * sth,
                                    c[1] + ey * cth + ex * sth });
        }
        if (ce.polyline.size() >= 3) parsed.entries.push_back(std::move(ce));
        else ++parsed.skippedEntries;
    }
    for (const auto &p : raw.polylines) {
        CatalogueOverlayEntry ce;
        ce.frame = CatalogueOverlayEntry::Frame::Image;
        ce.shape = CatalogueOverlayEntry::Shape::Polyline;
        ce.label = p.label;
        ce.polyline.reserve(p.pts.size());
        for (const auto &g : p.pts) {
            std::array<double, 2> px{ 0.0, 0.0 };
            if (this->galacticToImagePixel(g[0], g[1], px)) ce.polyline.push_back(px);
        }
        if (ce.polyline.size() >= 2) parsed.entries.push_back(std::move(ce));
        else ++parsed.skippedEntries;
    }

    if (parsed.entries.empty()) {
        QMessageBox::information(this, u"VLKB catalogue"_s,
                                 u"No %1 found in this image footprint."_s.arg(
                                         this->m_vlkbCatIsFilament ? u"filaments"_s : u"sources"_s));
        return;
    }
    this->applyCatalogueOverlayResult(parsed);
    if (!this->m_vlkbCatIsFilament) {
        // Legacy behaviour: show only the band-merged sources by default; the user
        // enables individual bands from the panel. Hide every present band except
        // the merged one (applyCatalogueOverlayResult cleared m_vlkbHiddenBands).
        for (const QString &band : this->catalogueOverlayPolylineBand) {
            if (!band.isEmpty() && band != kVlkbMergedBand)
                this->m_vlkbHiddenBands.insert(band);
        }
        this->rebuildCatalogueOverlay();
        if (ui && ui->vtk && ui->vtk->renderWindow())
            ui->vtk->renderWindow()->Render();
        this->ensureVlkbBandPanel();
    } else {
        releaseToolSettings(u"Catalogue overlay"_s);
        refreshToolSettingsPanel(/*reveal=*/false);
    }
}

// Build (or refresh) a small dock with one coloured checkbox per band present in
// the current source overlay, so the user can toggle each band's visibility.
// Bands are the VLKB Hi-GAL/companion bands collected from the overlay polylines.
void vtkWindowImage::ensureVlkbBandPanel()
{
    // Collect the bands actually present, in the canonical order.
    QStringList present;
    for (const QString &band : this->catalogueOverlayPolylineBand) {
        if (!band.isEmpty() && !present.contains(band))
            present.append(band);
    }
    QStringList ordered;
    for (const QString &qb : kVlkbBands) {
        if (present.contains(qb)) ordered.append(qb);
    }
    for (const QString &b : present)   // any extra bands not in the canonical list
        if (!ordered.contains(b)) ordered.append(b);

    // Which bands are drawn is a PARAMETER of the catalogue overlay, so it goes
    // where every other tool's parameters go - the Inspector's Parameters tab -
    // and not in a third dock sharing the right column with the Inspector and
    // the source table.
    if (ordered.isEmpty()) {
        releaseToolSettings(u"Catalogue overlay"_s);
        refreshToolSettingsPanel(/*reveal=*/false);
        return;
    }

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);
    // The rows were 4 px apart with the box hard against the label: nine bands
    // read as one block. Give them room to be nine separate switches.
    layout->setSpacing(8);

    for (const QString &band : ordered) {
        auto *cb = new QCheckBox(vlkbBandLabel(band), content);
        cb->setChecked(!this->m_vlkbHiddenBands.contains(band));
        const std::array<double, 3> c = vlkbBandColor(band);
        QColor col(qBound(0, int(c[0] * 255.0), 255),
                   qBound(0, int(c[1] * 255.0), 255),
                   qBound(0, int(c[2] * 255.0), 255));
        // The overlay colours are tuned for a dark image (e.g. band-merged is white);
        // as label text on the light panel they can be invisible. Darken any colour
        // that is too light so the text stays readable while keeping its band hue.
        for (int guard = 0; col.lightnessF() > 0.6 && guard < 8; ++guard)
            col = col.darker(130);
        cb->setStyleSheet(u"QCheckBox { color: %1; font-weight: bold; spacing: 8px; }"_s
                                  .arg(col.name()));
        QObject::connect(cb, &QCheckBox::toggled, this,
                         [this, band](bool on) { this->toggleVlkbBand(band, on); });
        layout->addWidget(cb);
    }
    layout->addStretch(1);
    // Rebuilt on every refresh (the band list can change), so the previous one
    // is retired rather than left to accumulate hidden.
    QWidget *oldContent = this->m_vlkbBandContent;
    this->m_vlkbBandContent = content;
    claimToolSettings(u"Catalogue overlay"_s, content);
    if (oldContent && oldContent != content)
        oldContent->deleteLater();
}

// Show/hide all sources of a given band and re-render.
void vtkWindowImage::toggleVlkbBand(const QString &band, bool visible)
{
    if (visible)
        this->m_vlkbHiddenBands.remove(band);
    else
        this->m_vlkbHiddenBands.insert(band);
    this->rebuildCatalogueOverlay();   // rebuilds geometry only — does NOT render
    if (ui && ui->vtk && ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}
