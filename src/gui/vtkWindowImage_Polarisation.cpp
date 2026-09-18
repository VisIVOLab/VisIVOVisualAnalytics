#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
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
#include "WorkspaceChrome.h"
#include "InspectorPanel.h"
#include "ProfileWidget.h"
#include "RemoteFileBrowserDialog.h"
#include "StokesCompanion.h"
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
#include <QDir>
#include <QScopeGuard>
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
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

// ── Polarisation / Stokes derived products ───────────────────────────────────
// Extracted verbatim from vtkWindowImage.cpp. Stokes companion detection and
// the derived P / PA / fractional / debiased / spectral-index / Faraday-RM
// layers + polarisation vector overlay.

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

namespace {

/// Do these images occupy the same pixel grid? Every per-pixel Stokes
/// computation below walks one image's extent and reads the others at the SAME
/// indices, which is only meaningful when the three grids coincide. It used to
/// be guaranteed because every layer was read from disk at full resolution;
/// a layer now arrives from the backend decimated to its own size, so two
/// perfectly co-registered companions can land on different grids and the loop
/// would silently combine different places on the sky.
bool sameSampledGrid(vtkImageData *a, vtkImageData *b)
{
    if (!a || !b)
        return false;
    int ea[6], eb[6];
    double oa[3], ob[3], sa[3], sb[3];
    a->GetExtent(ea);
    b->GetExtent(eb);
    a->GetOrigin(oa);
    b->GetOrigin(ob);
    a->GetSpacing(sa);
    b->GetSpacing(sb);
    for (int i = 0; i < 4; ++i)
        if (ea[i] != eb[i])
            return false;
    for (int i = 0; i < 2; ++i) {
        if (std::abs(oa[i] - ob[i]) > 1e-6 || std::abs(sa[i] - sb[i]) > 1e-6)
            return false;
    }
    return true;
}

/// Message for the refusal above: it is a real situation with a real remedy,
/// not a failure to compute.
QString differentGridsMessage(const QString &what)
{
    return QStringLiteral(
                   "%1 needs its input layers on the same pixel grid, and these are not: "
                   "they were loaded at different resolutions (a large layer is fetched "
                   "decimated). Reload them at the same resolution, or crop to a common "
                   "region, before combining them.")
            .arg(what);
}

}   // namespace

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

namespace {

// Runs on a WORKER thread: BackendClient is synchronous, and a remote backend
// that takes its time answering must not freeze the window. Nothing here
// touches the viewer, so there is no `this` to outlive the call.
vtkWindowImage::StokesListing listStokesDirectory(const QString &backendUrl,
                                                  const QString &backendToken,
                                                  const QString &path)
{
    vtkWindowImage::StokesListing out;
    const QString dir = QFileInfo(path).absolutePath();

    // Datasets live on the backend's filesystem, which is only the same as ours
    // when the backend runs locally. Ask the backend what is in the directory,
    // exactly as the file browser does, so the auto-detection is not silently
    // empty against a remote server.
    if (!backendUrl.isEmpty()) {
        BackendClient client(backendUrl, backendToken);
        const auto listing = client.listFiles(dir);
        if (!listing.valid) {
            out.error = listing.error.isEmpty() ? u"backend listing failed"_s : listing.error;
            return out;
        }
        for (const auto &entry : listing.entries)
            out.names.insert(entry.name);
        return out;
    }

    for (const QFileInfo &fi : QDir(dir).entryInfoList(QDir::Files))
        out.names.insert(fi.fileName());
    return out;
}

}   // namespace

void vtkWindowImage::pumpLayerQueue()
{
    if (m_pendingLayerLoads.isEmpty() || this->isBusy())
        return;
    startNextQueuedLayerLoad();
}

void vtkWindowImage::startNextQueuedLayerLoad()
{
    if (m_pendingLayerLoads.isEmpty())
        return;
    const auto next = m_pendingLayerLoads.takeFirst();
    // The role is assigned when the load finishes, by matching this label.
    m_pendingVlkbDisplayName = next.displayName;
    m_currentLoadPartOfSet = next.partOfSet;
    this->addLayerImage(next.path.toStdString());
}

void vtkWindowImage::detectAndLoadStokesCompanions()
{
    if (this->isBusy()) {
        statusBar()->showMessage(u"Viewer busy, try again in a moment."_s, 3000);
        return;
    }
    if (!m_pendingLayerLoads.isEmpty()) {
        statusBar()->showMessage(u"Stokes companions are still loading."_s, 3000);
        return;
    }
    // The file choosers below still spin a nested event loop, so the menu action
    // stays clickable while this runs. A second pass would build its own queue on
    // top of ours and overwrite m_pendingVlkbDisplayName.
    if (m_stokesDiscoveryActive)
        return;
    m_stokesDiscoveryActive = true;

    // The master layer is treated as Stokes I.
    const int masterIdx = this->layers->getMasterIndex();
    if (!m_stokesRoles.contains(masterIdx))
        m_stokesRoles.insert(masterIdx, StokesRole::I);

    // The listing goes off-thread; the rest of the work resumes in
    // onStokesListingReady(). The watcher is a member of this window, so if the
    // window goes away the callback goes with it.
    statusBar()->showMessage(u"Looking for Stokes companions..."_s);
    m_stokesListingWatcher.setFuture(QtConcurrent::run(&listStokesDirectory,
                                                       this->remoteBackendUrl,
                                                       this->remoteBackendToken, this->filepath));
}

void vtkWindowImage::onStokesListingReady(const StokesListing &result)
{
    const auto discoveryGuard = qScopeGuard([this] { m_stokesDiscoveryActive = false; });

    // Discovery does not block the window, so an import can have started while
    // the listing was out. Queueing on top of it would replace the single layer
    // watcher's future and lose whatever it was loading.
    if (this->isBusy() || !m_pendingLayerLoads.isEmpty()) {
        // Only if the bar is free: the operation that made us busy is saying
        // something more useful there, and replacing it would also wipe it when
        // this message times out.
        if (!this->persistentStatusActive)
            statusBar()->showMessage(
                    u"Another layer is loading - try Load Stokes Companions again in a moment."_s,
                    5000);
        return;
    }

    const QStringList roles = { u"Q"_s, u"U"_s, u"V"_s };
    QStringList missing;
    QStringList autoFound;
    QStringList alreadyLoaded;

    const QString listingError = result.error;
    const QSet<QString> &listing = result.names;
    const QFileInfo iInfo(this->filepath);
    const QString searchDir = iInfo.absolutePath();

    for (const QString &role : roles) {
        if (findLayerByStokesRole(
                role == u"Q"_s ? StokesRole::Q
                : role == u"U"_s ? StokesRole::U : StokesRole::V) >= 0) {
            alreadyLoaded.append(role);
            continue;
        }

        const QString name = visivo::pickStokesCompanion(iInfo.fileName(), role, listing);
        if (name.isEmpty()) {
            missing.append(role);
        } else {
            autoFound.append(QStringLiteral("%1 (%2)").arg(role, name));
            m_pendingLayerLoads.append({ searchDir + u"/"_s + name, u"Stokes "_s + role, true });
        }
    }

    // Say what was found BEFORE opening any file chooser. Without this the
    // manual step reads as a popup that keeps coming back, with no clue that
    // each one is asking for a different component.
    if (!missing.isEmpty()) {
        QStringList lines;
        if (!alreadyLoaded.isEmpty())
            lines << QStringLiteral("Already loaded: %1").arg(alreadyLoaded.join(u", "_s));
        if (!autoFound.isEmpty())
            lines << QStringLiteral("Found next to Stokes I: %1").arg(autoFound.join(u", "_s));
        lines << QStringLiteral("Not found: %1").arg(missing.join(u", "_s));
        if (!listingError.isEmpty())
            lines << QStringLiteral("(could not list %1: %2)").arg(searchDir, listingError);
        lines << QString();
        lines << QStringLiteral("Locate the missing %1 yourself? You will get one file "
                                "browser per component, in order (%2). Cancel in any of them "
                                "stops the sequence.")
                         .arg(missing.size() == 1 ? u"file"_s : u"files"_s,
                              missing.join(u", "_s));

        const auto answer = QMessageBox::question(
                this, u"Load Stokes Companions"_s, lines.join(u"\n"_s),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            for (int i = 0; i < missing.size(); ++i) {
                // The backend file browser, not a local QFileDialog: the paths
                // the loader needs are the backend's, and with a remote backend
                // a local chooser would show the wrong filesystem entirely.
                RemoteFileBrowserDialog dialog(this->remoteBackendUrl, this->remoteBackendToken,
                                               this);
                // .fits.gz too: auto-detection matches gzipped companions, so the
                // manual chooser must not hide the very files it suggests.
                dialog.setExtensionFilter({ u".fits"_s, u".fit"_s, u".fits.gz"_s, u".fit.gz"_s },
                                          u"FITS files only"_s);
                dialog.setWindowTitle(QStringLiteral("Locate Stokes %1  (%2 of %3)")
                                              .arg(missing.at(i))
                                              .arg(i + 1)
                                              .arg(missing.size()));
                if (dialog.exec() != QDialog::Accepted)
                    break;   // Cancel abandons the remaining components too
                const QString path = dialog.selectedFilePath();
                if (path.isEmpty())
                    break;
                m_pendingLayerLoads.append({ path, u"Stokes "_s + missing.at(i), true });
            }
        }
    }

    if (m_pendingLayerLoads.isEmpty()) {
        // The listing has finished and started nothing: anything that queued
        // itself while it ran is free to go.
        this->pumpLayerQueue();
        statusBar()->showMessage(
                alreadyLoaded.size() == roles.size()
                        ? u"Stokes Q, U and V are already loaded."_s
                        : u"No Stokes companion loaded."_s,
                4000);
        return;
    }

    statusBar()->showMessage(
            QStringLiteral("Loading %1 Stokes companion(s)...").arg(m_pendingLayerLoads.size()),
            4000);
    // One at a time: the layer loader has a single watcher, so starting three
    // loads at once would keep only the last.
    startNextQueuedLayerLoad();
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
    if (!sameSampledGrid(qData, uData)) {
        QMessageBox::warning(this, u"Stokes"_s, differentGridsMessage(u"This computation"_s));
        return;
    }

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
    if (!sameSampledGrid(qData, uData)) {
        QMessageBox::warning(this, u"Stokes"_s, differentGridsMessage(u"This computation"_s));
        return;
    }

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
    if (!sameSampledGrid(iData, qData) || !sameSampledGrid(iData, uData)) {
        QMessageBox::warning(this, u"Fractional Polarization"_s,
                             differentGridsMessage(u"Fractional polarization"_s));
        return;
    }

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
    if (!sameSampledGrid(qData, uData)) {
        QMessageBox::warning(this, u"Stokes"_s, differentGridsMessage(u"This computation"_s));
        return;
    }

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
    // Extent alone is not the grid: two layers can share an extent and still
    // sample different sky, if one was fetched decimated.
    if (!sameSampledGrid(dataA, dataB)) {
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
            QStringLiteral("Spectral index: %1 valid pixels, range %2 - %3")
                    .arg(nValid).arg(aMin, 0, 'f', 2).arg(aMax, 0, 'f', 2),
            5000);
}

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
            u"is RM (rad m⁻²). No PA unwrapping, works only for low-RM regimes."_s,
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
            QStringLiteral("Faraday RM: %1 pixels, range %2, %3 rad m⁻² (clipped to ±500 for display)")
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
            QStringLiteral("Derived map added: %1 (range %2 - %3)")
                    .arg(name)
                    .arg(rangeMin, 0, 'g', 4)
                    .arg(rangeMax, 0, 'g', 4),
            4000);

    // Every derived map is a PRODUCT of this window: it is a new map computed
    // from the loaded ones, and until now it existed only as a row in the layer
    // list, with nothing in Session Data to say where it came from. The product
    // is bound to the layer, so opening its row selects that layer.
    QVariantMap params;
    params.insert(u"Derived from"_s, QFileInfo(this->filepath).fileName());
    params.insert(u"Layer"_s, name);
    params.insert(u"Value range"_s, u"%1 … %2"_s.arg(rangeMin, 0, 'g', 6).arg(rangeMax, 0, 'g', 6));
    params.insert(u"Colour map"_s, colorMap);
    this->registerAnalysisProduct(u"derived"_s, name, params,
                                  this->scopedProductId(u"derived-%1"_s.arg(name)),
                                  /*resultView=*/{}, /*layerIndex=*/newIdx);
}

bool vtkWindowImage::rebuildPolarizationVectors()
{
    const int qIdx = findLayerByStokesRole(StokesRole::Q);
    const int uIdx = findLayerByStokesRole(StokesRole::U);
    if (qIdx < 0 || uIdx < 0) {
        m_polVecActor->VisibilityOff();
        return false;
    }
    auto *qData = this->layers->getImageData(qIdx);
    auto *uData = this->layers->getImageData(uIdx);
    if (!qData || !uData) {
        m_polVecActor->VisibilityOff();
        return false;
    }
    if (!sameSampledGrid(qData, uData)) {
        m_polVecActor->VisibilityOff();
        statusBar()->showMessage(
                u"Polarisation vectors need Q and U on the same pixel grid."_s, 5000);
        return false;
    }
    // The vectors are built in Q's VOXEL indices; Q may be displayed on a
    // decimated grid, so the actor carries the same voxel -> world mapping the
    // region overlay uses, or the vectors would be drawn at a fraction of the
    // position and length of the pixels they describe.
    {
        double qo[3], qs[3];
        int qe[6];
        qData->GetOrigin(qo);
        qData->GetSpacing(qs);
        qData->GetExtent(qe);
        m_polVecActor->SetScale(qs[0], qs[1], 1.0);
        m_polVecActor->SetPosition(qo[0] - qe[0] * qs[0], qo[1] - qe[2] * qs[1], 0.0);
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
        return false;
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
    return true;
}

void vtkWindowImage::setPolVectorsVisible(bool vis)
{
    // Without Q and U there is nothing to draw, and the toggle used to switch on
    // and show an empty image with its parameters beside it - the tool looked
    // broken rather than inapplicable. Say which layers are missing, and put the
    // toggle back where it was.
    if (vis) {
        const bool haveQ = findLayerByStokesRole(StokesRole::Q) >= 0;
        const bool haveU = findLayerByStokesRole(StokesRole::U) >= 0;
        if (!haveQ || !haveU) {
            const QString missing = !haveQ && !haveU ? u"Q and U"_s : (!haveQ ? u"Q"_s : u"U"_s);
            statusBar()->showMessage(
                    u"Polarisation vectors need Stokes %1: load the companions first "
                    u"(Tools ▸ Load Stokes Q/U/V Companions…)."_s.arg(missing), 6000);
            if (m_actTogglePolVecs && m_actTogglePolVecs->isChecked()) {
                const QSignalBlocker block(m_actTogglePolVecs);
                m_actTogglePolVecs->setChecked(false);
            }
            m_polVecsVisible = false;
            m_polVecActor->VisibilityOff();
            if (ui->vtk->renderWindow())
                ui->vtk->renderWindow()->Render();
            return;
        }
    }
    m_polVecsVisible = vis;
    if (vis && !rebuildPolarizationVectors()) {
        // The rebuild refused (incompatible Q/U grids, no data): do not switch the
        // actor on over whatever geometry was left from last time, and put the
        // toggle back - it said the tool was on while nothing was drawn.
        m_polVecsVisible = false;
        m_polVecActor->VisibilityOff();
        if (m_actTogglePolVecs && m_actTogglePolVecs->isChecked()) {
            const QSignalBlocker block(m_actTogglePolVecs);
            m_actTogglePolVecs->setChecked(false);
        }
        releaseToolSettings(u"Polarisation vectors"_s);
        refreshToolSettingsPanel(/*reveal=*/false);
        if (ui->vtk->renderWindow())
            ui->vtk->renderWindow()->Render();
        return;
    }
    m_polVecActor->SetVisibility(vis);
    // Grid step / SNR / length are this tool's parameters: they appear with it.
    // The panel follows the tools that are on: this one claims it while it is
    // shown (and the tab is revealed, or the settings would appear where nobody
    // is looking), and hands it back to whichever tool is still on when it goes.
    if (vis && m_polVecSettingsBlock)
        claimToolSettings(u"Polarisation vectors"_s, m_polVecSettingsBlock);
    else {
        releaseToolSettings(u"Polarisation vectors"_s);
        refreshToolSettingsPanel(/*reveal=*/false);
    }
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}
