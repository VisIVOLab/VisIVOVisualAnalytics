#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"
#include "CubeWcsUtil.h"
#include "CubeViewportGeometry.h"
#include "SpectralAxisInfer.h"

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
#include <vtkTextRenderer.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <QDir>
#include <QHostAddress>
#include <QSettings>
#include <QUrl>
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

#include <QFont>
#include <QFontMetrics>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

using namespace Qt::StringLiterals;
using namespace CubeWcs;
using namespace CubeGeom;

// ── WCS / spectral-axis formatting + overlay ─────────────────────────
// Extracted verbatim from vtkWindowCube.cpp. Spectral-axis descriptors, WCS
// coordinate formatting, and the slice/moment celestial overlays.
// Shared axis/coordinate-format helpers come from CubeWcs (CubeWcsUtil).

namespace {
constexpr int overlayTickCount = 5;  // moved here with the WCS overlay methods

// Spectral-axis inference (inferSpectralAxisDescriptor + its label/clean helpers)
// now lives in SpectralAxisInfer.h (E-04) so it can be unit-tested; callers below
// use spectralaxis::inferSpectralAxisDescriptor.
}  // namespace

vtkWindowCube::SpectralAxisDescriptor vtkWindowCube::spectralAxisDescriptor() const
{
    const bool hasBackendSpectralMetadata = !this->remoteSpectralAxisType.trimmed().isEmpty()
            || !this->remoteSpectralAxisUnit.trimmed().isEmpty();
    if (hasBackendSpectralMetadata) {
        auto descriptor =
                spectralaxis::inferSpectralAxisDescriptor(this->remoteSpectralAxisType,
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
    return spectralaxis::inferSpectralAxisDescriptor(this->remoteDatasetCtype[2],
                                       this->remoteDatasetCunit[2],
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

void vtkWindowCube::pushSpectralAxisContext(ProfileWidget *plot) const
{
    if (!plot)
        return;
    // The CTYPE, not the pretty label: the line overlay has to know the velocity
    // CONVENTION (VOPT vs VRAD vs VELO), and "Optical Velocity" is a label, not a
    // convention. The unit is the descriptor's, which is what the plotted values
    // are in (remoteVoxelToWcs returns header units).
    const auto d = this->spectralAxisDescriptor();
    QString ctype = d.sourceLabel.trimmed();
    if (ctype.isEmpty())
        ctype = this->remoteSpectralAxisType.trimmed();
    if (ctype.isEmpty())
        ctype = this->remoteDatasetCtype[2].trimmed();
    plot->setSpectralAxisContext(ctype, d.unit, this->m_restFreqHz);
    plot->setDatasetPersistKey(this->datasetPersistKey());
}

QString vtkWindowCube::settingsIniPath()
{
    return QDir(QDir::home().absoluteFilePath(qApp->applicationName()))
            .absoluteFilePath(QStringLiteral("Settings.ini"));
}

QStringList vtkWindowCube::remoteRecommendDismissed()
{
    QSettings raw(settingsIniPath(), QSettings::IniFormat);
    return raw.value(QStringLiteral("Cube/remote_recommend_dismissed")).toStringList();
}

void vtkWindowCube::dismissRemoteRecommend(const QString &datasetKey)
{
    if (datasetKey.isEmpty())
        return;
    QSettings raw(settingsIniPath(), QSettings::IniFormat);
    QStringList all =
            raw.value(QStringLiteral("Cube/remote_recommend_dismissed")).toStringList();
    if (all.contains(datasetKey))
        return;
    all << datasetKey;
    // Bounded: this is a convenience list, not a history.
    while (all.size() > 64)
        all.removeFirst();
    raw.setValue(QStringLiteral("Cube/remote_recommend_dismissed"), all);
}

QString vtkWindowCube::remoteRecommendKey() const
{
    const QUrl url(this->remoteBackendUrl);
    // Scheme-aware default, so "https://host" and "https://host:443" are the
    // same backend and not two keys.
    const int defaultPort =
            url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
    const QString backend = url.host().isEmpty()
            ? this->remoteBackendUrl
            : QStringLiteral("%1:%2").arg(url.host().toLower()).arg(url.port(defaultPort));
    return backend + QLatin1Char('|') + this->datasetPersistKey();
}

bool vtkWindowCube::backendIsLocal() const
{
    const QUrl url(this->remoteBackendUrl);
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return false;
    if (host == QStringLiteral("localhost"))
        return true;
    // 127.0.0.0/8 and ::1, whatever spelling the user typed (QUrl::host()
    // already strips the brackets of an IPv6 literal). QHostAddress parses
    // numeric addresses only — it does NOT resolve names.
    const QHostAddress addr(host);
    if (!addr.isNull() && addr.isLoopback())
        return true;
    // …which leaves a hosts-file alias for 127.0.0.1. Rather than
    // resolve a name on a UI path, compare against the URL the app calls its
    // LOCAL backend — the authoritative answer.
    //
    // Read raw with QSettings, NOT through Settings: that constructor launches
    // python3 (and possibly python) with 30-second synchronous waits to locate
    // an interpreter, which is not something to do while composing a banner
    // (main.cpp avoids it for the same reason when reading the theme).
    const QString iniPath =
            QDir(QDir::home().absoluteFilePath(qApp->applicationName()))
                    .absoluteFilePath(QStringLiteral("Settings.ini"));
    QSettings raw(iniPath, QSettings::IniFormat);
    const QUrl localUrl(raw.value(QStringLiteral("Backend/url"),
                                  QStringLiteral("http://127.0.0.1:8000"))
                                .toString());
    // Scheme-aware default port, so an implicit https URL and an explicit :443
    // are the same endpoint.
    const auto portOf = [](const QUrl &u) {
        return u.port(u.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443
                                                                                            : 80);
    };
    return !localUrl.host().isEmpty() && localUrl.host().toLower() == host
            && portOf(localUrl) == portOf(url);
}

void vtkWindowCube::setServerSideRenderingAvailable(bool available)
{
    this->m_serverSideRenderingAvailable = available;
    // The render-mode choice is a capability, not a preference: with no EGL on
    // the backend, Remote cannot start and the menu entry must say so instead of
    // failing on click.
    if (this->m_actionRenderRemote) {
        this->m_actionRenderRemote->setEnabled(available);
        if (!available) {
            const QString why =
                    u"This backend cannot render server-side (no EGL context - a CPU-only "
                    u"or macOS backend). Rendering happens here."_s;
            this->m_actionRenderRemote->setToolTip(why);
            this->m_actionRenderRemote->setStatusTip(why);
        }
    }
    // Re-compose the banner: the constructor built it with the default (capable)
    // before this arrived, so a backend without EGL was left showing a Switch
    // button — or, on a remote one, neither action at all.
    this->refreshRemoteRecommendBanner();
}

void vtkWindowCube::setRestFrequency(double restFreqHz)
{
    this->m_restFreqHz = restFreqHz > 0.0 ? restFreqHz : 0.0;
    // Widgets built before this arrives (MainWindow sets it right after the
    // constructor) get the value now — but only those actually showing a product
    // on the cube's own spectral axis. A blanket push handed the cube's CTYPE /
    // unit / RESTFRQ to a channel-axis product too, and "Load Lines…" then
    // converted to velocity and drew the result as channel indices.
    for (int i = 0; i < 4; ++i) {
        if (!this->m_paneSpectra[i])
            continue;
        const PaneView &v = this->m_slots[i].view;
        const auto it = (v.kind == ViewId::Product) ? this->m_products1d.constFind(v.productId)
                                                    : this->m_products1d.constEnd();
        // No product in this slot (a parked widget) → CLEAR. Treating "unknown"
        // as "the cube's axis" contaminated widgets that show nothing, and the
        // next product mounted there would inherit it.
        const bool onCubeAxis = (it != this->m_products1d.constEnd())
                && (it->xLabel.isEmpty() || it->xLabel == this->spectralAxisTitle());
        if (onCubeAxis)
            this->pushSpectralAxisContext(this->m_paneSpectra[i]);
        else
            this->m_paneSpectra[i]->setSpectralAxisContext(QString(), QString(), 0.0);
    }
    // The region spectrum is always on the cube's own axis.
    this->pushSpectralAxisContext(this->probePlotWidget);
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

QString vtkWindowCube::formatRemoteOverlayCoordinate(int axis, double value, double axisSpan) const
{
    if (!this->remoteHasCelestialAxes()) {
        return QString::number(value, 'g', 8);
    }
    if (!this->useSexagesimalWcsFormat) {
        return this->formatDegreeCoordinate(value, axisSpan);
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

QString vtkWindowCube::formatDegreeCoordinate(double value, double axisSpan) const
{
    // (b) Adaptive precision: over an axis span S with ~5 ticks the spacing is ~S/4, so pick
    // enough decimals that adjacent ticks differ — otherwise a 0.007° field prints "47.21"
    // on every tick. Default (span ≤ 0, e.g. the hover readout) keeps the old 2 decimals.
    int decimals = 2;
    if (axisSpan > 0.0 && std::isfinite(axisSpan)) {
        const double spacing = axisSpan / 4.0;
        if (spacing > 0.0)
            decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(spacing))) + 1, 2, 8);
    }
    QString text = QString::number(value, 'f', decimals);
    // A tiny negative longitude rounds to "-0.000": the tick at the origin.
    if (text.startsWith(u'-') && text.mid(1).toDouble() == 0.0)
        text.remove(0, 1);
    return text;
}

QString vtkWindowCube::currentWcsFrameLabel() const
{
    const int frame = this->selectedWcsFrame();
    return frame == WCS_GALACTIC ? u"Galactic"_s
            : (frame == WCS_J2000 ? u"FK5"_s : u"Ecliptic"_s);
}

QString vtkWindowCube::selectedFrameAxisTitle(int axis) const
{
    return this->remoteOverlayAxisTitle(axis);
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
                } else if (this->active2dWin()) {
                    this->active2dWin()->Render();
                }
                if (auto *w2d = this->active2dWidget()) w2d->update();
            },
            Qt::QueuedConnection);
}

double vtkWindowCube::dprFor(bool slice) const
{
    QVTKOpenGLNativeWidget *w = slice ? ui->vtkImage : m_vtkMoment.data();
    return (w && w->devicePixelRatioF() > 0.) ? w->devicePixelRatioF() : 1.;
}

bool vtkWindowCube::sliceChannelBarShown() const
{
    return this->channelOverlay2d && this->channelOverlay2d->parentWidget() == ui->vtkImage
            && m_sliceBackdropProductId.isEmpty();
}

CubeGeom::ImageGutter vtkWindowCube::gutter2dFor(bool slice) const
{
    auto *lut = slice ? this->lutSlice.GetPointer() : this->lutMoment.GetPointer();
    double range[2] = { 0., 0. };
    if (lut)
        lut->GetTableRange(range);

    // The gutter lives in RENDER-WINDOW (device) pixels — that is what
    // renderer->GetSize() reports. VTK draws the labels at the window's DPI, which
    // QVTKOpenGLNativeWidget sets to logicalDpi × devicePixelRatio, so a column sized
    // from logical-pixel metrics is half the width it needs on a retina panel. That,
    // not any Qt↔VTK font divergence, was clipping the Moment-0 labels (§3.0b).
    const double dpr = this->dprFor(slice);

    // Measure with VTK's OWN text engine, at the property the colorbar will use, so
    // there is nothing left to over-provision: the number below IS the drawn width.
    auto *win = slice ? this->sliceWin.GetPointer() : this->momentWin.GetPointer();
    // Harden against an unset (plain 72) DPI on a retina panel — never measure below
    // the DPR-scaled DPI, or the label column comes out half-width (§3.0b).
    const int dpi = std::max(win ? win->GetDPI() : 72, static_cast<int>(72. * dpr));
    vtkNew<vtkTextProperty> tp;
    tp->SetFontSize(CubeGeom::kColorbarLabelFontSize);
    tp->BoldOff();
    tp->ShadowOff();
    double labelW = 0.;
    if (auto *tr = vtkTextRenderer::GetInstance()) {
        for (double v : { range[0], range[1] }) {
            int bbox[4] = { 0, 0, 0, 0 };
            const std::string s = QString::asprintf("%.3g", v).toStdString();
            if (tr->GetBoundingBox(tp, s, bbox, dpi))
                labelW = std::max<double>(labelW, bbox[1] - bbox[0] + 1);
        }
    }
    if (labelW <= 0.)               // headless / no text engine
        labelW = 7. * 7. * dpr;     // 7 chars of a %.3g exponent, device px

    const double rightGutter = CubeGeom::kColorbarGap * dpr       // data area → bar
            + CubeGeom::kColorbarBarWidth * dpr                   // the swatch
            + CubeGeom::kColorbarTextPad * dpr                    // bar → label
            + labelW                                              // already device px
            + CubeGeom::kColorbarMargin * dpr;                    // label → pane edge
    // The channel bar sits over the slice's bottom edge while it shows channels:
    // the axes go above it, not under it.
    const bool bar = slice && this->sliceChannelBarShown();
    return CubeGeom::fitGutterWithBottomReserve(CubeGeom::imageGutter(this->showWcsAxes, rightGutter, dpr),
                                                win ? win->GetSize() : nullptr,
                                                bar ? ChannelOverlay::reservedHeight() * dpr : 0.);
}

void vtkWindowCube::layout2dPane(bool force)
{
    // Resize / mode-switch: the currently-visible pane IS the one to re-fit.
    this->layout2dPaneFor(this->viewingSlice(), force);
}

void vtkWindowCube::layout2dPaneFor(bool slice, bool force)
{
    auto *win = slice ? this->sliceWin.GetPointer() : this->momentWin.GetPointer();
    if (!win) {
        return;
    }
    auto *renderers = win->GetRenderers();
    auto *renderer = renderers ? renderers->GetFirstRenderer() : nullptr;
    if (!renderer) {
        return;
    }
    const int *size = renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0) {
        return;
    }
    // The gutter (hence the data-area size) depends on the LUT range via the
    // measured colorbar column, so a range change must re-fit even at the same
    // viewport (§3.0b). Key the guard on both — but skip when NEITHER changed, so a
    // bare render (or a range drag that doesn't move the label width) keeps the zoom.
    const auto gutter = this->gutter2dFor(slice);
    const std::array<int, 2> viewport = { size[0], size[1] };
    auto &lastFit = slice ? this->m_slice2dFitSize : this->m_moment2dFitSize;
    auto &lastRight = slice ? this->m_slice2dFitRight : this->m_moment2dFitRight;
    auto &lastBottom = slice ? this->m_slice2dFitBottom : this->m_moment2dFitBottom;
    if (!force && viewport == lastFit && gutter.right == lastRight && gutter.bottom == lastBottom) {
        return;   // same geometry — do NOT touch the camera, the user may have zoomed
    }

    auto *image = slice
            ? vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0))
            : vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    CubeGeom::ImageRect dataArea;
    const bool fitted = CubeGeom::fitImageInGutter(renderer, image, gutter, &dataArea);
    // Only re-lay the colorbar when we actually fit (valid data area). If the fit
    // failed (e.g. the widget was just shown and its size is still stale), keep the
    // previous colorbar rather than fall back to the inner box — that fallback made
    // the moment swatch start below the data top when switching views (§3.0c). The
    // next valid layout (resize / mode-switch) lays it against the real data area.
    if (fitted) {
        CubeGeom::layoutColorbarInGutter(slice ? this->m_sliceColorbar : this->m_momentColorbar,
                                         renderer, gutter, dataArea, this->dprFor(slice));
        lastFit = viewport;   // only remember a viewport we actually fit into
        lastRight = gutter.right;
        lastBottom = gutter.bottom;
        // STEP6-P1/P2: the slice just re-letterboxed to its own bounds, discarding any shared
        // Linked-2D zoom. If Linked·camera holds a shared view, re-adopt it so the slice rejoins
        // the group instead of snapping to full-frame. (Only the SLICE viewport; the singular
        // moment path never participates in the pane camera link.)
        if (slice && this->m_linkViews && this->m_linkCamera && this->m_linked2DValid)
            this->applySliceCameraNormalized(this->m_linked2DCenter, this->m_linked2DWorldHeight,
                                             /*render=*/false);   // win->Render() runs just below
        // STEP6-P1: a fit / slice reload moves the slice camera WITHOUT going through
        // applySliceCameraNormalized, so record the post-fit pan/zoom pose here too — otherwise
        // the first window/level drag after a reset would look like a camera move and spuriously
        // propagate. Reflects the fit (or the re-assert above).
        if (slice)
            this->captureSliceCamPose();
    }
    this->invalidateWcsOverlayCache();   // keep last — makes the next EndEvent recompute
    win->Render();
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
    // §3.1: this is the SLICE window's own overlay — load slice data unconditionally
    // (not gated by viewingSlice()), or at 4 panes the slice pane loses its axes
    // whenever the moment pane is the active one.
    auto *imageData = vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
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
    // Per-actor visibility is decided by the full update below (leftmost label,
    // crowded labels, a title with no room); switching everything on here undid
    // it on every render that hits the cache. Hidden axes hide everything.
    if (!this->showWcsAxes || !renderer || !imageData) {
        if (!this->showWcsAxes)
            this->set2dWcsOverlayVisible(false);
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
    // Camera MTime is part of the key: a pan/zoom that keeps the full image visible
    // leaves visibleBounds unchanged but moves the projected axis rect, so the axes
    // must recompute anyway.
    const unsigned long camMTime = renderer->GetActiveCamera()->GetMTime();
    if (visibleBounds == this->lastSliceOverlayVisibleBounds
        && viewportSize == this->lastSliceOverlayViewportSize
        && camMTime == this->lastSliceOverlayCameraMTime) {
        return;
    }
    this->lastSliceOverlayVisibleBounds = visibleBounds;
    this->lastSliceOverlayViewportSize = viewportSize;
    this->lastSliceOverlayCameraMTime = camMTime;

    const auto gutter = this->gutter2dFor(/*slice=*/true);
    const double dpr = this->dprFor(/*slice=*/true);   // offsets below are logical px × dpr
    // Derive the data area at the CURRENT viewport size — never a cached member,
    // which goes stale across the multiple resizes a dock toggle produces (§2.7).
    const auto data = CubeGeom::dataAreaFor(size, imageData, gutter);
    const double axisX = data.valid ? data.x : gutter.left;
    const double xRight = data.valid ? data.x + data.width : size[0] - gutter.right;
    const double yTop = data.valid ? data.y + data.height : size[1] - gutter.top;
    const double bottomMargin = data.valid ? data.y : gutter.bottom;
    configureAxisActor(this->sliceOverlayXAxis, axisX, bottomMargin, xRight, bottomMargin);
    configureAxisActor(this->sliceOverlayYAxis, axisX, yTop, axisX, bottomMargin);
    this->sliceOverlayXAxis->SetTitle("");
    this->sliceOverlayXTitleActor->SetInput(this->remoteOverlayAxisTitle(0).toStdString().c_str());
    // Titles travel with their axis (§2.7): centred under the tick row (data.y-18),
    // and left of the tick column (data.x-10); floored so they stay on screen.
    this->sliceOverlayXTitleActor->SetDisplayPosition(static_cast<int>((axisX + xRight) / 2.),
                                                      static_cast<int>(std::max(bottomMargin - 38. * dpr, 10. * dpr)));
    this->sliceOverlayYAxis->SetTitle("");
    this->sliceOverlayYTitleActor->SetInput(this->remoteOverlayAxisTitle(1).toStdString().c_str());
    this->sliceOverlayYTitleActor->SetDisplayPosition(static_cast<int>(std::max(axisX - 74. * dpr, 12. * dpr)),
                                                      static_cast<int>((bottomMargin + yTop) / 2.));

    // The image exactly fills the data area, so its full visible extent maps to the
    // axis endpoints — range and ticks come straight from the visible bounds.
    const double rxMin = visible.xmin;
    const double rxMax = visible.xmax;
    const double ryMin = visible.ymin;
    const double ryMax = visible.ymax;
    bool xOk = false;
    bool yOk = false;
    const double xMin = this->remoteVoxelToWcs(0, rxMin, &xOk);
    const double xMax = this->remoteVoxelToWcs(0, rxMax, &xOk);
    const double yMin = this->remoteVoxelToWcs(1, ryMin, &yOk);
    const double yMax = this->remoteVoxelToWcs(1, ryMax, &yOk);
    this->sliceOverlayXAxis->SetRange(xOk ? xMin : rxMin, xOk ? xMax : rxMax);
    this->sliceOverlayYAxis->SetRange(yOk ? yMax : ryMax, yOk ? yMin : ryMin);
    this->sliceOverlayXAxis->LabelVisibilityOff();
    this->sliceOverlayYAxis->LabelVisibilityOff();
    this->sliceOverlayXAxis->VisibilityOn();
    this->sliceOverlayYAxis->VisibilityOn();
    // A narrow pane squeezes the axis column (fitGutterToViewport): the numbers
    // stay, the rotated title would sit on top of them.
    const bool narrowAxis = gutter.left < 0.8 * CubeGeom::ImageGutter{}.left * dpr;
    // A short pane above the channel bar: the title would land on the bar.
    const double barRoom = this->sliceChannelBarShown() ? ChannelOverlay::reservedHeight() * dpr : 0.;
    const bool shortAxis = barRoom > 0. && bottomMargin - barRoom < 46. * dpr;
    this->sliceOverlayXTitleActor->SetVisibility(!shortAxis);
    this->sliceOverlayYTitleActor->SetVisibility(!narrowAxis);
    // (b) axis WCS span (from the tick extremes, honouring the paired celestial conversion)
    // so the degree formatter uses enough precision that adjacent ticks never coincide.
    auto frameAt = [&](double tt, double &fx, double &fy) {
        const double vx = rxMin + tt * (rxMax - rxMin);
        const double vy = ryMin + tt * (ryMax - ryMin);
        bool ox = false, oy = false;
        double tx = this->remoteVoxelToWcs(0, vx, &ox);
        double ty = this->remoteVoxelToWcs(1, vy, &oy);
        fx = tx;
        fy = ty;
        if (ox && oy && this->remoteHasCelestialAxes())
            this->convertRemoteCelestialCoordinates(tx, ty, fx, fy);
    };
    double fx0 = 0., fy0 = 0., fx1 = 0., fy1 = 0.;
    frameAt(0.0, fx0, fy0);
    frameAt(1.0, fx1, fy1);
    const double sliceSpanX = std::fabs(fx1 - fx0);
    const double sliceSpanY = std::fabs(fy1 - fy0);
    for (int i = 0; i < overlayTickCount; ++i) {
        const double t = overlayTickCount == 1 ? 0.0
                                               : static_cast<double>(i)
                        / static_cast<double>(overlayTickCount - 1);
        const double voxelX = rxMin + t * (rxMax - rxMin);
        const double voxelY = ryMin + t * (ryMax - ryMin);
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
                this->formatRemoteOverlayCoordinate(0, frameX, sliceSpanX).toStdString().c_str());
        this->sliceOverlayXTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX + t * (xRight - axisX)),
                static_cast<int>(bottomMargin - 18. * dpr));
        // Drop the leftmost x label — it collides with the bottom y label in the
        // corner, and that value is readable from the y axis anyway.
        // In a narrow pane the labels collide: keep every other one.
        const bool crowded = overlayTickCount > 1
                && (xRight - axisX) / (overlayTickCount - 1) < 62. * dpr;
        this->sliceOverlayXTickActors[static_cast<std::size_t>(i)]->SetVisibility(
                i != 0 && (!crowded || i % 2 == 0));
        this->sliceOverlayYTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(1, frameY, sliceSpanY).toStdString().c_str());
        this->sliceOverlayYTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX - 10. * dpr),
                static_cast<int>(bottomMargin + t * (yTop - bottomMargin)));
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
                       .arg(yTop, 0, 'g', 12)
                       .arg(axisX, 0, 'g', 12)
                       .arg(bottomMargin, 0, 'g', 12)
                       .arg(xRight, 0, 'g', 12);
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
    // Per-actor visibility is decided by the full update below (leftmost label,
    // crowded labels, a title with no room); switching everything on here undid
    // it on every render that hits the cache. Hidden axes hide everything.
    if (!this->showWcsAxes || !renderer || !imageData) {
        if (!this->showWcsAxes)
            this->set2dWcsOverlayVisible(false);
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
    // Camera MTime is part of the key (see updateSliceWcsOverlay): a pan/zoom that
    // keeps the full image visible still moves the projected axis rect.
    const unsigned long camMTime = renderer->GetActiveCamera()->GetMTime();
    if (visibleBounds == this->lastMomentOverlayVisibleBounds
        && viewportSize == this->lastMomentOverlayViewportSize
        && camMTime == this->lastMomentOverlayCameraMTime) {
        return;
    }
    this->lastMomentOverlayVisibleBounds = visibleBounds;
    this->lastMomentOverlayViewportSize = viewportSize;
    this->lastMomentOverlayCameraMTime = camMTime;

    const auto gutter = this->gutter2dFor(/*slice=*/false);
    const double dpr = this->dprFor(/*slice=*/false);   // offsets below are logical px × dpr
    // Derive the data area at the CURRENT viewport size — never a cached member (§2.7).
    const auto data = CubeGeom::dataAreaFor(size, imageData, gutter);
    const double axisX = data.valid ? data.x : gutter.left;
    const double xRight = data.valid ? data.x + data.width : size[0] - gutter.right;
    const double yTop = data.valid ? data.y + data.height : size[1] - gutter.top;
    const double bottomMargin = data.valid ? data.y : gutter.bottom;
    configureAxisActor(this->momentOverlayXAxis, axisX, bottomMargin, xRight, bottomMargin);
    configureAxisActor(this->momentOverlayYAxis, axisX, yTop, axisX, bottomMargin);
    this->momentOverlayXAxis->SetTitle("");
    this->momentOverlayXTitleActor->SetInput(this->remoteOverlayAxisTitle(0).toStdString().c_str());
    // Titles travel with their axis (§2.7).
    this->momentOverlayXTitleActor->SetDisplayPosition(static_cast<int>((axisX + xRight) / 2.),
                                                       static_cast<int>(std::max(bottomMargin - 38. * dpr, 10. * dpr)));
    this->momentOverlayYAxis->SetTitle("");
    this->momentOverlayYTitleActor->SetInput(this->remoteOverlayAxisTitle(1).toStdString().c_str());
    this->momentOverlayYTitleActor->SetDisplayPosition(static_cast<int>(std::max(axisX - 74. * dpr, 12. * dpr)),
                                                       static_cast<int>((bottomMargin + yTop) / 2.));

    // The image exactly fills the data area — range and ticks from the visible bounds.
    const double rxMin = visible.xmin;
    const double rxMax = visible.xmax;
    const double ryMin = visible.ymin;
    const double ryMax = visible.ymax;
    bool xOk = false;
    bool yOk = false;
    const double xMin = this->remoteVoxelToWcs(0, rxMin, &xOk);
    const double xMax = this->remoteVoxelToWcs(0, rxMax, &xOk);
    const double yMin = this->remoteVoxelToWcs(1, ryMin, &yOk);
    const double yMax = this->remoteVoxelToWcs(1, ryMax, &yOk);
    this->momentOverlayXAxis->SetRange(xOk ? xMin : rxMin, xOk ? xMax : rxMax);
    this->momentOverlayYAxis->SetRange(yOk ? yMax : ryMax, yOk ? yMin : ryMin);
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
        const double voxelX = rxMin + t * (rxMax - rxMin);
        const double voxelY = ryMin + t * (ryMax - ryMin);
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
                static_cast<int>(axisX + t * (xRight - axisX)),
                static_cast<int>(bottomMargin - 18. * dpr));
        // Drop the leftmost x label — corner collision with the bottom y label.
        this->momentOverlayXTickActors[static_cast<std::size_t>(i)]->SetVisibility(i != 0);
        this->momentOverlayYTickActors[static_cast<std::size_t>(i)]->SetInput(
                this->formatRemoteOverlayCoordinate(1, frameY).toStdString().c_str());
        this->momentOverlayYTickActors[static_cast<std::size_t>(i)]->SetDisplayPosition(
                static_cast<int>(axisX - 10. * dpr),
                static_cast<int>(bottomMargin + t * (yTop - bottomMargin)));
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
                       .arg(yTop, 0, 'g', 12)
                       .arg(axisX, 0, 'g', 12)
                       .arg(bottomMargin, 0, 'g', 12)
                       .arg(xRight, 0, 'g', 12);
}
