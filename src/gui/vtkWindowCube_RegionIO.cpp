// Region IO (CRTF / DS9) on the cube's 2-D view — partial class of vtkWindowCube.
//
// The cube viewer already has interactive region analysis (box / circle /
// polygon / annulus → stats + spectral profile). This adds file interop so a
// drawn region can be saved as a CASA CRTF or DS9 (.reg) file, and a region
// file can be loaded back onto the 2-D view and analysed — parity with the
// image viewer's region IO, and with CARTA's region import/export.
//
// Both directions go through the backend's /v1/regions endpoints (the same that
// the image viewer uses), run off the GUI thread via QFutureWatcher so the
// synchronous BackendClient never blocks the UI or risks a nested-event-loop
// use-after-free if the window is closed mid-request.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "app/BackendClient.h"

#include <vtkImageData.h>
#include <vtkRenderWindow.h>

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QtConcurrentRun>

#include <array>
#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

QJsonObject vtkWindowCube::currentRegionToJson() const
{
    // Canonical image-frame region in 1-based pixel coordinates (the FITS /
    // ds9 / CRTF convention). The viewer stores 0-based voxels, so +1 here.
    QJsonObject r;
    r[u"frame"_s] = u"image"_s;
    r[u"unit"_s] = u"pix"_s;
    const auto px = [](int v) { return double(v) + 1.0; };
    // After analysis the live regionMode is reset to None while the geometry
    // persists; fall back to the last finalised shape so export still works.
    const RegionMode shape =
            this->regionMode != RegionMode::None ? this->regionMode : this->m_exportableRegionShape;
    switch (shape) {
    case RegionMode::Box: {
        const double ax = px(regionAnchorVoxel[0]), ay = px(regionAnchorVoxel[1]);
        const double bx = px(regionCurrentVoxel[0]), by = px(regionCurrentVoxel[1]);
        r[u"shape"_s] = u"box"_s;
        r[u"center"_s] = QJsonArray{ (ax + bx) / 2.0, (ay + by) / 2.0 };
        r[u"size"_s] = QJsonArray{ std::abs(bx - ax), std::abs(by - ay) };
        r[u"angle_deg"_s] = 0.0;
        break;
    }
    case RegionMode::Circle: {
        const double dx = regionCurrentVoxel[0] - regionAnchorVoxel[0];
        const double dy = regionCurrentVoxel[1] - regionAnchorVoxel[1];
        r[u"shape"_s] = u"circle"_s;
        r[u"center"_s] = QJsonArray{ px(regionAnchorVoxel[0]), px(regionAnchorVoxel[1]) };
        r[u"radius"_s] = std::sqrt(dx * dx + dy * dy);
        break;
    }
    case RegionMode::Annulus: {
        const double dx = regionCurrentVoxel[0] - regionAnchorVoxel[0];
        const double dy = regionCurrentVoxel[1] - regionAnchorVoxel[1];
        r[u"shape"_s] = u"annulus"_s;
        r[u"center"_s] = QJsonArray{ px(regionAnchorVoxel[0]), px(regionAnchorVoxel[1]) };
        r[u"radii"_s] = QJsonArray{ regionAnnulusInnerRadius, std::sqrt(dx * dx + dy * dy) };
        break;
    }
    case RegionMode::Polygon: {
        r[u"shape"_s] = u"polygon"_s;
        QJsonArray pts;
        for (const auto &v : regionPolygonVertices)
            pts.append(QJsonArray{ px(v[0]), px(v[1]) });
        r[u"points"_s] = pts;
        break;
    }
    default:
        break;
    }
    return r;
}

void vtkWindowCube::exportRegionsToFile()
{
    const RegionMode shape =
            this->regionMode != RegionMode::None ? this->regionMode : this->m_exportableRegionShape;
    if (shape == RegionMode::None || !this->regionValid) {
        QMessageBox::information(
                this, u"Export Region"_s,
                u"Draw a region first (Tools ▸ Box/Circle/Polygon/Annulus Region Analysis)."_s);
        return;
    }
    const QStringList choices{ u"DS9 — sky (FK5)"_s, u"DS9 — pixel"_s,
                              u"CASA CRTF — sky (FK5)"_s, u"CASA CRTF — pixel"_s };
    bool ok = false;
    const QString choice = QInputDialog::getItem(
            this, u"Export Region"_s, u"Format and coordinate frame:"_s, choices, 0, false, &ok);
    if (!ok)
        return;
    const bool isCrtf = choice.startsWith(u"CASA"_s);
    const QString format = isCrtf ? u"crtf"_s : u"ds9"_s;
    const QString toFrame = choice.contains(u"sky"_s) ? u"fk5"_s : QString();
    const QString filter = isCrtf ? u"CRTF region (*.crtf)"_s : u"DS9 region (*.reg)"_s;
    const QString suffix = isCrtf ? u".crtf"_s : u".reg"_s;
    QString path = QFileDialog::getSaveFileName(this, u"Export Region File"_s,
                                                QFileInfo(this->filepath).baseName() + suffix, filter);
    if (path.isEmpty())
        return;
    if (!path.endsWith(suffix))
        path += suffix;

    const QJsonArray arr{ this->currentRegionToJson() };
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken,
                  dsId = this->remoteDatasetId;
    this->statusBar()->showMessage(u"Serializing region…"_s, 0);
    QPointer<vtkWindowCube> self(this);
    auto *watcher = new QFutureWatcher<BackendRegionExportResult>(this);
    QObject::connect(
            watcher, &QFutureWatcher<BackendRegionExportResult>::finished, this,
            [self, watcher, path]() {
                const BackendRegionExportResult r = watcher->result();
                watcher->deleteLater();
                if (!self)
                    return;
                self->statusBar()->clearMessage();
                if (!r.valid) {
                    QMessageBox::warning(self, u"Export Region"_s,
                                         u"Export failed:\n%1"_s.arg(r.error));
                    return;
                }
                QFile out(path);
                if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    QMessageBox::warning(self, u"Export Region"_s,
                                         u"Cannot write file:\n%1"_s.arg(path));
                    return;
                }
                out.write(r.text.toUtf8());
                out.close();
                QMessageBox::information(self, u"Export Region"_s,
                                        u"Saved region to:\n%1"_s.arg(path));
            });
    watcher->setFuture(QtConcurrent::run(
            [url, token, arr, format, dsId, toFrame]() -> BackendRegionExportResult {
                BackendClient c(url, token);
                return c.exportRegions(arr, format, toFrame.isEmpty() ? QString() : dsId, toFrame);
            }));
}

void vtkWindowCube::importRegionsFromFile()
{
    const QString path = QFileDialog::getOpenFileName(
            this, u"Import Region File"_s, QFileInfo(this->filepath).absolutePath(),
            u"Region files (*.crtf *.reg);;CRTF (*.crtf);;DS9 (*.reg);;All files (*)"_s);
    if (path.isEmpty())
        return;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, u"Import Region"_s, u"Cannot open file:\n%1"_s.arg(path));
        return;
    }
    const QString text = QString::fromUtf8(in.readAll());
    in.close();

    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken,
                  dsId = this->remoteDatasetId;
    this->statusBar()->showMessage(u"Parsing region file…"_s, 0);
    QPointer<vtkWindowCube> self(this);
    auto *watcher = new QFutureWatcher<BackendRegionParseResult>(this);
    QObject::connect(
            watcher, &QFutureWatcher<BackendRegionParseResult>::finished, this,
            [self, watcher]() {
                const BackendRegionParseResult r = watcher->result();
                watcher->deleteLater();
                if (!self)
                    return;
                self->statusBar()->clearMessage();
                if (!r.valid) {
                    QMessageBox::warning(self, u"Import Region"_s,
                                         u"Parse failed:\n%1"_s.arg(r.error));
                    return;
                }
                self->applyImportedRegion(r.regions);
            });
    // Reframe to image pixels (needs the dataset's WCS) so the region maps onto
    // the cube grid regardless of whether the file is in sky or pixel coords.
    watcher->setFuture(QtConcurrent::run(
            [url, token, text, dsId]() -> BackendRegionParseResult {
                BackendClient c(url, token);
                return c.parseRegions(text, u"auto"_s, dsId, u"image"_s);
            }));
}

void vtkWindowCube::applyImportedRegion(const QJsonArray &regions)
{
    if (regions.isEmpty()) {
        QMessageBox::information(this, u"Import Region"_s, u"No regions found in the file."_s);
        return;
    }
    const QJsonObject reg = regions.first().toObject();
    const QString shape = reg.value(u"shape"_s).toString();
    // image-frame canonical coords are 1-based px; the viewer stores 0-based voxels.
    const auto vox = [](double p) { return int(std::lround(p - 1.0)); };
    const auto arr2 = [&reg](const QString &k) { return reg.value(k).toArray(); };

    QString note;
    if (shape == u"box"_s) {
        const QJsonArray c = arr2(u"center"_s), s = arr2(u"size"_s);
        if (c.size() < 2 || s.size() < 2) { note = u"malformed box"_s; }
        else {
            const double cx = c[0].toDouble(), cy = c[1].toDouble();
            const double w = s[0].toDouble(), h = s[1].toDouble();
            this->regionAnchorVoxel = { vox(cx - w / 2.0), vox(cy - h / 2.0) };
            this->regionCurrentVoxel = { vox(cx + w / 2.0), vox(cy + h / 2.0) };
            this->regionMode = RegionMode::Box;
            if (std::abs(reg.value(u"angle_deg"_s).toDouble()) > 0.01)
                note = u" (rotation ignored — box is axis-aligned)"_s;
        }
    } else if (shape == u"circle"_s) {
        const QJsonArray c = arr2(u"center"_s);
        const double rad = reg.value(u"radius"_s).toDouble();
        if (c.size() < 2 || rad <= 0.0) { note = u"malformed circle"_s; }
        else {
            const double cx = c[0].toDouble(), cy = c[1].toDouble();
            this->regionAnchorVoxel = { vox(cx), vox(cy) };
            this->regionCurrentVoxel = { vox(cx + rad), vox(cy) };
            this->regionMode = RegionMode::Circle;
        }
    } else if (shape == u"annulus"_s) {
        const QJsonArray c = arr2(u"center"_s), rr = arr2(u"radii"_s);
        if (c.size() < 2 || rr.size() < 2) { note = u"malformed annulus"_s; }
        else {
            const double cx = c[0].toDouble(), cy = c[1].toDouble();
            const double rin = rr[0].toDouble(), rout = rr[1].toDouble();
            if (rout <= 0.0 || rin < 0.0 || rin >= rout) { note = u"invalid annulus radii"_s; }
            else {
                this->regionAnchorVoxel = { vox(cx), vox(cy) };
                this->regionCurrentVoxel = { vox(cx + rout), vox(cy) };
                this->regionAnnulusInnerRadius = rin;
                this->regionMode = RegionMode::Annulus;
            }
        }
    } else if (shape == u"polygon"_s) {
        const QJsonArray pts = arr2(u"points"_s);
        std::vector<std::array<int, 2>> verts;
        for (const auto &pv : pts) {
            const QJsonArray p = pv.toArray();
            if (p.size() >= 2)
                verts.push_back({ vox(p[0].toDouble()), vox(p[1].toDouble()) });
        }
        if (verts.size() < 3) { note = u"polygon needs ≥3 valid points"_s; }
        else {
            this->regionPolygonVertices = std::move(verts);
            this->regionMode = RegionMode::Polygon;
        }
    } else {
        QMessageBox::warning(this, u"Import Region"_s,
                             u"Unsupported region shape '%1' (supported: box, circle, "
                             u"annulus, polygon)."_s.arg(shape.isEmpty() ? u"?"_s : shape));
        return;
    }

    if (!note.isEmpty() && !note.startsWith(u" "_s)) {   // a hard error (malformed)
        QMessageBox::warning(this, u"Import Region"_s, u"Could not import region: %1."_s.arg(note));
        return;
    }

    // Clear any in-progress interaction state so the imported region renders as
    // a clean finalised region (matches the normal post-draw path).
    this->regionDragging = false;
    this->ignoreNextPolygonRelease = false;
    this->regionValid = true;
    this->refreshRegionOverlay();
    if (auto *rw = ui->vtkImage->renderWindow())
        rw->Render();
    // analyzeCurrentRegion() opens the stats/spectrum windows AND records the
    // shape in m_exportableRegionShape (so the imported region is re-exportable).
    this->analyzeCurrentRegion();
    // Finalise like the normal post-analysis state: drop regionMode to None so
    // clicks aren't intercepted as region edits and the overlay is cleaned up by
    // maybeClearRegionAfterDisplay() once the result windows close. The geometry
    // + regionValid stay, and the overlay actors keep their visibility.
    this->regionMode = RegionMode::None;

    QString msg = u"Imported %1 region%2."_s.arg(shape).arg(note);
    if (regions.size() > 1)
        msg += u" (%1 regions in file — only the first was loaded.)"_s.arg(regions.size());
    this->statusBar()->showMessage(msg, 8000);
}
