// vtkWindowCube_Polarisation.cpp
// Stokes / radio polarimetry for the CUBE viewer.
//
// Why this is not the image viewer's code
// ---------------------------------------
// vtkWindowImage_Polarisation.cpp does polarimetry client-side: it loads Q, U
// and V as extra layers and walks the pixels. That works because an image is
// one plane. It cannot work here. A MeerKAT MGCLS field is 7500 × 7500 × 16
// per Stokes plane — 3.4 GB each uncompressed — and RM synthesis needs Q and U
// in full, across every sub-band. Pulling them into the viewer is not a slow
// path, it is an impossible one.
//
// So the division of labour is the opposite way round: this window finds the
// companion FILES, opens them as datasets in its own session, and tells the
// backend which dataset holds which Stokes plane. The backend slices the
// memmaps and sends back finished maps, which are registered as ordinary 2-D
// products — the same objects a moment map or a line-width map produces, so
// they mount in panes, carry statistics and export like everything else.
//
// The three shapes of data this has to cope with, all seen in published
// surveys:
//
//   * One cube with a STOKES axis of length ≥ 2 — Q and U are already here and
//     no companion is needed. m_hasOwnStokesAxis.
//   * One cube PER Stokes plane, each with a degenerate STOKES axis: MGCLS,
//     ASKAP/POSSUM, anything from Obit MFImage. This is the case that needed
//     the work.
//   * Companions that exist but are not named by any rule we know, which is
//     what the manual "Locate…" fallback is for.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "RemoteFileBrowserDialog.h"
#include "StokesCompanion.h"
#include "app/BackendClient.h"

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

using namespace Qt::StringLiterals;

namespace {

using CompanionScan = vtkWindowCube::StokesCompanionScan;

/// List the directory, pick the companions by name and open each as a dataset.
///
/// Runs on a WORKER thread: BackendClient is synchronous and both steps are
/// network calls against a backend that may be remote and slow. Nothing here
/// touches the viewer.
///
/// The directory is listed through the BACKEND, not through QDir, because
/// datasets live on the backend's filesystem — the same as ours only when it
/// runs locally. Listing locally would make auto-detection silently empty
/// against a remote server, which looks exactly like "this survey has no
/// polarisation".
CompanionScan scanAndOpenCompanions(const QString &backendUrl,
                                    const QString &backendToken,
                                    const QString &sessionId,
                                    const QString &primaryPath,
                                    const QStringList &roles)
{
    CompanionScan out;
    const QFileInfo primary(primaryPath);
    const QString dir = primary.absolutePath();

    QSet<QString> listing;
    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    if (!backendUrl.isEmpty()) {
        const auto files = client.listFiles(dir);
        if (!files.valid) {
            out.error = files.error.isEmpty() ? u"backend listing failed"_s : files.error;
            return out;
        }
        for (const auto &entry : files.entries)
            listing.insert(entry.name);
    } else {
        for (const QFileInfo &fi : QDir(dir).entryInfoList(QDir::Files))
            listing.insert(fi.fileName());
    }

    for (const QString &role : roles) {
        const QString name = visivo::pickStokesCompanion(primary.fileName(), role, listing);
        if (name.isEmpty()) {
            out.missing.append(role);
            continue;
        }
        // Opened in THIS window's session: the polarisation endpoints resolve a
        // companion dataset id through the session like any other, so a file
        // opened in a session of its own would come back as "unknown dataset".
        const auto opened = client.openDataset(dir + u"/"_s + name);
        if (!opened.valid || opened.datasetId.isEmpty()) {
            out.failed.insert(role, opened.error.isEmpty() ? u"could not be opened"_s
                                                           : opened.error);
            continue;
        }
        out.ids.insert(role, opened.datasetId);
        out.names.insert(role, name);
    }
    return out;
}

/// Open files the user picked by hand. Same session rule as above.
CompanionScan openChosenCompanions(const QString &backendUrl,
                                   const QString &backendToken,
                                   const QString &sessionId,
                                   const QHash<QString, QString> &chosen)
{
    CompanionScan out;
    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);
    for (auto it = chosen.constBegin(); it != chosen.constEnd(); ++it) {
        const auto opened = client.openDataset(it.value());
        if (!opened.valid || opened.datasetId.isEmpty()) {
            out.failed.insert(it.key(), opened.error.isEmpty() ? u"could not be opened"_s
                                                               : opened.error);
            continue;
        }
        out.ids.insert(it.key(), opened.datasetId);
        out.names.insert(it.key(), QFileInfo(it.value()).fileName());
    }
    return out;
}

/// A float32 payload from the backend as a 2-D vtkImageData, or null when the
/// payload does not match the announced shape.
vtkSmartPointer<vtkImageData> imageFromFloats(const QByteArray &data, int w, int h)
{
    if (w <= 0 || h <= 0
        || data.size() != static_cast<qsizetype>(w) * h * static_cast<qsizetype>(sizeof(float)))
        return nullptr;
    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetExtent(0, w - 1, 0, h - 1, 0, 0);
    img->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(img->GetScalarPointer(), data.constData(),
                static_cast<std::size_t>(w) * h * sizeof(float));
    return img;
}

}   // namespace

// ── Menu ─────────────────────────────────────────────────────────────────────

void vtkWindowCube::setupPolarisationTools()
{
    // A STOKES axis among the three ACTIVE axes means this cube carries several
    // planes itself, so Q and U are already here. A collapsed STOKES axis means
    // the opposite — the file is one plane, and the others are siblings — which
    // is the MGCLS / POSSUM case ownStokesPlane() reads.
    for (const QString &ctype : this->remoteDatasetCtype) {
        if (ctype.trimmed().compare(u"STOKES"_s, Qt::CaseInsensitive) == 0)
            m_hasOwnStokesAxis = true;
    }

    m_actLoadStokesCompanions = ui->menuTools->addAction(u"Load Stokes Q/U/V Companions…"_s);
    QObject::connect(m_actLoadStokesCompanions, &QAction::triggered,
                     this, &vtkWindowCube::loadStokesCompanions);

    m_actPolarisedIntensity = ui->menuTools->addAction(
            u"Polarised Intensity && Angle (current channel)"_s);
    QObject::connect(m_actPolarisedIntensity, &QAction::triggered,
                     this, &vtkWindowCube::computePolarisedIntensityMap);

    m_actRmSynthesis = ui->menuTools->addAction(u"Rotation Measure Synthesis…"_s);
    QObject::connect(m_actRmSynthesis, &QAction::triggered,
                     this, &vtkWindowCube::runRmSynthesis);

    updatePolarisationActions();
}

/// "Collapsed axes: STOKES=2 (1)" → "Q". Empty when there is no collapsed
/// Stokes axis, or when its value is not one of the four we handle.
///
/// The value is advisory, not load-bearing: the MGCLS V cubes carry STOKES=1,
/// which decodes as I. It is used to LABEL what the user opened, never to
/// decide what a file contains — that stays with the file name and the role the
/// user assigned it.
QString vtkWindowCube::ownStokesPlane() const
{
    static const QRegularExpression re(u"STOKES\\s*=\\s*(-?\\d+)"_s,
                                       QRegularExpression::CaseInsensitiveOption);
    const auto match = re.match(this->remoteDegenerateAxesSummary);
    if (!match.hasMatch())
        return {};
    switch (match.captured(1).toInt()) {
    case 1:  return u"I"_s;
    case 2:  return u"Q"_s;
    case 3:  return u"U"_s;
    case 4:  return u"V"_s;
    default: return {};
    }
}

QStringList vtkWindowCube::availableStokesRoles() const
{
    QStringList roles;
    if (m_hasOwnStokesAxis) {
        // Several planes on the active Stokes axis: the backend reads them all
        // out of this one file, so every role it could hold is available.
        roles << u"I"_s << u"Q"_s << u"U"_s << u"V"_s;
    } else if (const QString own = ownStokesPlane(); !own.isEmpty()) {
        roles.append(own);
    }
    for (const QString &role : { u"I"_s, u"Q"_s, u"U"_s, u"V"_s }) {
        const QString id = role == u"I"_s   ? m_stokesCompanions.i
                         : role == u"Q"_s   ? m_stokesCompanions.q
                         : role == u"U"_s   ? m_stokesCompanions.u
                                            : m_stokesCompanions.v;
        if (!id.isEmpty() && !roles.contains(role))
            roles.append(role);
    }
    roles.sort();
    return roles;
}

QString vtkWindowCube::stokesCompanionSummary() const
{
    QStringList parts;
    for (const QString &role : { u"Q"_s, u"U"_s, u"V"_s }) {
        const QString name = m_stokesCompanionFiles.value(role);
        if (!name.isEmpty())
            parts.append(u"%1 (%2)"_s.arg(role, name));
    }
    return parts.join(u", "_s);
}

void vtkWindowCube::updatePolarisationActions()
{
    const QStringList have = availableStokesRoles();
    const bool haveQU = have.contains(u"Q"_s) && have.contains(u"U"_s);
    const bool spectral = this->remoteDatasetDepth > 1;

    if (m_actLoadStokesCompanions) {
        m_actLoadStokesCompanions->setEnabled(!m_hasOwnStokesAxis && !m_stokesDiscoveryActive);
        m_actLoadStokesCompanions->setToolTip(
                m_hasOwnStokesAxis
                        ? u"This cube already carries its own Stokes axis (%1); no companion "
                          u"files are needed."_s.arg(have.join(u", "_s))
                        : u"Find the sibling files holding Stokes Q, U and V — FIELD_Q.fits "
                          u"beside FIELD_I.fits, as MeerKAT, ASKAP and Obit MFImage publish "
                          u"them — and open them alongside this cube."_s);
    }

    // Every refusal says WHY, and never by doing nothing when clicked.
    if (m_actPolarisedIntensity) {
        m_actPolarisedIntensity->setEnabled(haveQU);
        m_actPolarisedIntensity->setToolTip(
                haveQU ? u"Bias-corrected polarised intensity √(Q²+U²−σ²) and position angle "
                         u"½·atan2(U,Q) for the channel on screen, as two 2-D products."_s
                       : u"Needs Stokes Q and U. This cube has %1 — load the companions "
                         u"first."_s.arg(have.isEmpty() ? u"neither"_s : have.join(u", "_s)));
    }
    if (m_actRmSynthesis) {
        m_actRmSynthesis->setEnabled(haveQU && spectral);
        m_actRmSynthesis->setToolTip(
                !haveQU  ? u"Needs Stokes Q and U — load the companions first."_s
                : !spectral ? u"Needs a frequency axis with more than one channel; this "
                              u"dataset has %1."_s.arg(this->remoteDatasetDepth)
                            : u"Faraday dispersion F(φ) over the frequency axis "
                              u"(Brentjens & de Bruyn 2005), reduced to the peak-|F| and "
                              u"Faraday-depth maps."_s);
    }
}

// ── Companion discovery ──────────────────────────────────────────────────────

void vtkWindowCube::loadStokesCompanions()
{
    if (m_stokesDiscoveryActive || this->isBusy())
        return;
    if (m_hasOwnStokesAxis) {
        QMessageBox::information(this, u"Stokes Companions"_s,
                u"This cube carries its own Stokes axis, so Q and U are already here."_s);
        return;
    }
    if (this->remoteBackendUrl.isEmpty() || this->remoteSessionId.isEmpty()) {
        QMessageBox::warning(this, u"Stokes Companions"_s,
                u"No backend session; the companion files are opened on the backend."_s);
        return;
    }

    m_stokesDiscoveryActive = true;
    updatePolarisationActions();
    this->statusBar()->showMessage(u"Looking for Stokes companions…"_s, 0);

    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, path = this->filepath;
    auto *watcher = new QFutureWatcher<CompanionScan>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<CompanionScan>::finished, this,
                     [self, watcher]() {
                         const CompanionScan scan = watcher->result();
                         watcher->deleteLater();
                         if (!self)
                             return;
                         // The flag stays up across applyStokesCompanionScan(),
                         // which may start the manual pass; it is cleared by
                         // whichever pass actually finishes the work.
                         self->applyStokesCompanionScan(scan, /*offerManual=*/true);
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, session, path]() {
                return scanAndOpenCompanions(url, token, session, path,
                                             { u"Q"_s, u"U"_s, u"V"_s });
            }));
}

void vtkWindowCube::applyStokesCompanionScan(const CompanionScan &scan, bool offerManual)
{
    this->statusBar()->clearMessage();
    // Whatever leaves this function without starting another worker has
    // finished discovery — set here, and re-raised below only for the manual
    // pass, so no early return can leave the menu entry dead for the session.
    m_stokesDiscoveryActive = false;
    if (!scan.error.isEmpty()) {
        updatePolarisationActions();
        QMessageBox::warning(this, u"Stokes Companions"_s,
                u"Could not list the directory to look for companions:\n%1"_s.arg(scan.error));
        return;
    }

    for (auto it = scan.ids.constBegin(); it != scan.ids.constEnd(); ++it) {
        const QString &role = it.key();
        if (role == u"Q"_s)      m_stokesCompanions.q = it.value();
        else if (role == u"U"_s) m_stokesCompanions.u = it.value();
        else if (role == u"V"_s) m_stokesCompanions.v = it.value();
        else if (role == u"I"_s) m_stokesCompanions.i = it.value();
        m_stokesCompanionFiles.insert(role, scan.names.value(role));
    }
    updatePolarisationActions();

    QStringList lines;
    if (!scan.ids.isEmpty())
        lines << u"Loaded: %1"_s.arg(stokesCompanionSummary());
    for (auto it = scan.failed.constBegin(); it != scan.failed.constEnd(); ++it)
        lines << u"Stokes %1: %2"_s.arg(it.key(), it.value());

    // Roles still absent AFTER this pass — a role found now is no longer missing
    // even if an earlier pass listed it.
    QStringList stillMissing;
    for (const QString &role : scan.missing) {
        if (!availableStokesRoles().contains(role))
            stillMissing.append(role);
    }
    if (!stillMissing.isEmpty()) {
        lines << u"Not found by name: %1"_s.arg(stillMissing.join(u", "_s));
    }
    if (lines.isEmpty())
        lines << u"Nothing to load."_s;

    if (!offerManual || stillMissing.isEmpty()) {
        QMessageBox::information(this, u"Stokes Companions"_s, lines.join(u"\n"_s));
        return;
    }

    // Say what happened BEFORE opening a file chooser: a chooser appearing with
    // no explanation reads as "the tool did nothing and then asked me something".
    lines << QString()
          << u"Locate the missing file(s) yourself?"_s;
    // Every dialog from here down spins a nested event loop, during which this
    // window can be closed. A QPointer to ourselves is the only thing that can
    // tell afterwards, and every touch of a member has to come after a check.
    QPointer<vtkWindowCube> alive(this);
    const auto answer = QMessageBox::question(
            this, u"Stokes Companions"_s, lines.join(u"\n"_s),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (!alive || answer != QMessageBox::Yes)
        return;

    QHash<QString, QString> chosen;
    for (int i = 0; i < stillMissing.size(); ++i) {
        const QString role = stillMissing.at(i);
        const QString path = askForStokesCompanionPath(role, i + 1, stillMissing.size());
        if (!alive)
            return;
        if (path.isEmpty())
            continue;
        chosen.insert(role, path);
    }
    if (chosen.isEmpty())
        return;

    // Up again for the manual pass: a second discovery started while these
    // files are being opened would race this one, and whichever finished last
    // would decide which companion ids survive.
    m_stokesDiscoveryActive = true;
    updatePolarisationActions();
    this->statusBar()->showMessage(u"Opening the Stokes companions…"_s, 0);
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId;
    auto *watcher = new QFutureWatcher<CompanionScan>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<CompanionScan>::finished, this,
                     [self, watcher]() {
                         const CompanionScan scan = watcher->result();
                         watcher->deleteLater();
                         if (self)
                             self->applyStokesCompanionScan(scan, /*offerManual=*/false);
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, session, chosen]() {
                return openChosenCompanions(url, token, session, chosen);
            }));
}

QString vtkWindowCube::askForStokesCompanionPath(const QString &role, int index, int total)
{
    // The backend's filesystem, not ours: the file will be opened by the
    // backend, so with a remote server a local chooser would offer paths it
    // cannot read — and the ones it can read would not be shown.
    //
    // On the heap, behind a QPointer, although it is used and discarded right
    // here. exec() runs a nested event loop in which this window can be closed,
    // and closing it destroys its children: a stack-allocated child parented to
    // `this` would then be deleted twice. The QPointer goes null instead, which
    // is also how the caller learns the window is gone.
    QPointer<RemoteFileBrowserDialog> dialog(
            new RemoteFileBrowserDialog(this->remoteBackendUrl, this->remoteBackendToken, this));
    // .fits.gz too, because auto-detection matches gzipped companions and a
    // chooser that hides them would contradict the names it just suggested.
    dialog->setExtensionFilter({ u".fits"_s, u".fit"_s, u".fits.gz"_s, u".fit.gz"_s },
                               u"FITS files only"_s);
    dialog->setWindowTitle(u"Locate Stokes %1  (%2 of %3)"_s.arg(role).arg(index).arg(total));
    const int outcome = dialog->exec();
    if (!dialog)
        return {};                      // the window went away, taking it with it
    const QString path = outcome == QDialog::Accepted ? dialog->selectedFilePath() : QString();
    delete dialog;
    return path;
}

// ── PI / PA ──────────────────────────────────────────────────────────────────

void vtkWindowCube::computePolarisedIntensityMap()
{
    if (this->isBusy())
        return;
    const int channel = std::clamp(ui->sliderSlice->value() - 1,
                                   0, std::max(0, this->remoteDatasetDepth - 1));

    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, datasetId = this->remoteDatasetId;
    const BackendStokesCompanionIds companions = m_stokesCompanions;

    this->statusBar()->showMessage(
            u"Computing polarised intensity for channel %1…"_s.arg(channel + 1), 0);

    auto *watcher = new QFutureWatcher<BackendPolarisationPiResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendPolarisationPiResult>::finished, this,
                     [self, watcher, channel]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self)
                             return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Polarised Intensity"_s,
                                     r.error.isEmpty() ? u"The computation failed."_s : r.error);
                             return;
                         }
                         self->publishPolarisationMaps(r, channel);
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, session, datasetId, channel, companions]() {
                BackendClient c(url, token);
                c.setSessionId(session);
                return c.requestPolarisationPi(datasetId, channel, {}, companions);
            }));
}

void vtkWindowCube::publishPolarisationMaps(const BackendPolarisationPiResult &r, int channel)
{
    auto pi = imageFromFloats(r.piData, r.width, r.height);
    auto pa = imageFromFloats(r.paData, r.width, r.height);
    if (!pi || !pa) {
        QMessageBox::warning(this, u"Polarised Intensity"_s,
                u"The backend returned a payload that does not match the announced "
                u"%1 × %2 shape."_s.arg(r.width).arg(r.height));
        return;
    }

    QVariantMap params;
    params.insert(u"Channel"_s, channel + 1);   // from 1, as on the channel bar
    params.insert(u"Stokes from"_s,
                  m_hasOwnStokesAxis ? u"this cube's Stokes axis"_s : stokesCompanionSummary());
    if (r.sigmaQu2 > 0.0)
        params.insert(u"σ²_QU"_s, r.sigmaQu2);

    // The stable keys carry the channel, so PI at two channels are two products
    // rather than one that silently overwrites the other — and recomputing the
    // same channel refreshes every pane already showing it.
    const QString unit = this->m_brightnessUnit;
    addImageProduct(u"polarisation"_s, u"stokes_pi_ch%1"_s.arg(channel),
                    u"Polarised intensity (ch %1)"_s.arg(channel + 1), pi, unit, params,
                    u"Inferno"_s);
    // PA is an angle in −90..+90: a diverging map, not a sequential one, or the
    // wrap at ±90 reads as a real gradient.
    addImageProduct(u"polarisation"_s, u"stokes_pa_ch%1"_s.arg(channel),
                    u"Polarisation angle (ch %1)"_s.arg(channel + 1), pa, u"deg"_s, params,
                    u"CoolWarm"_s);
}

// ── RM synthesis ─────────────────────────────────────────────────────────────

void vtkWindowCube::runRmSynthesis()
{
    if (this->isBusy())
        return;

    // Bounds pre-filled from a drawn region, exactly as Export Sub-Cube does.
    const auto bounds = currentRegionBounds2D();
    const int x0 = bounds[0], x1 = bounds[1], y0 = bounds[2], y1 = bounds[3];
    const bool haveRegion = this->regionValid && this->regionMode != RegionMode::None;

    QDialog dlg(this);
    dlg.setWindowTitle(u"Rotation Measure Synthesis"_s);
    auto *outer = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(
            u"Faraday dispersion F(φ) = (1/N) Σ P(λ²) e^(−2iφλ²) over the cube's "
            u"frequency axis (Brentjens & de Bruyn 2005). λ² is taken from the file "
            u"that supplies Stokes Q, which is not always this one."_s, &dlg);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto *form = new QFormLayout();
    auto *phiMin = new QDoubleSpinBox(&dlg);
    phiMin->setRange(-1e5, 1e5);
    phiMin->setValue(-100.0);
    phiMin->setSuffix(u" rad m⁻²"_s);
    auto *phiMax = new QDoubleSpinBox(&dlg);
    phiMax->setRange(-1e5, 1e5);
    phiMax->setValue(100.0);
    phiMax->setSuffix(u" rad m⁻²"_s);
    auto *dPhi = new QDoubleSpinBox(&dlg);
    dPhi->setRange(1e-3, 1e4);
    dPhi->setDecimals(3);
    dPhi->setValue(1.0);
    dPhi->setSuffix(u" rad m⁻²"_s);
    form->addRow(u"φ min"_s, phiMin);
    form->addRow(u"φ max"_s, phiMax);
    form->addRow(u"φ step"_s, dPhi);

    auto *useRegion = new QCheckBox(u"Restrict to the drawn region"_s, &dlg);
    useRegion->setEnabled(haveRegion);
    useRegion->setChecked(haveRegion);
    useRegion->setToolTip(haveRegion
            ? u"Compute over %1 × %2 pixels instead of the whole image."_s
                      .arg(x1 - x0 + 1).arg(y1 - y0 + 1)
            : u"Draw a region on the 2-D slice first to enable this."_s);
    form->addRow(QString(), useRegion);
    outer->addLayout(form);

    // The φ count is the parameter people get wrong: it multiplies the work and
    // the memory, and at survey size the difference between a step of 1 and of
    // 0.1 is between a minute and a refusal.
    auto *cost = new QLabel(&dlg);
    cost->setWordWrap(true);
    outer->addWidget(cost);
    auto refreshCost = [&]() {
        const double span = phiMax->value() - phiMin->value();
        // Both ends are sampled: the backend's phi_sample_count(), same tolerance.
        const double step = dPhi->value();
        const double tol = std::min(
                1e-9 + 8 * std::numeric_limits<double>::epsilon()
                        * std::max({ std::abs(phiMin->value()), std::abs(phiMax->value()), step }) / step,
                0.5);
        const int nPhi = span > 0.0 && step > 0.0
                ? static_cast<int>(std::floor(span / step + tol)) + 1 : 0;
        const int w = useRegion->isChecked() ? (x1 - x0 + 1) : this->remoteDatasetWidth;
        const int h = useRegion->isChecked() ? (y1 - y0 + 1) : this->remoteDatasetHeight;
        cost->setText(nPhi <= 0
                ? u"φ range and step give no samples."_s
                : u"%1 φ samples over %2 × %3 pixels. Two maps come back "
                  u"(peak |F| and the Faraday depth at which it peaks); the full "
                  u"dispersion cube would be %4 MiB and is not transferred."_s
                          .arg(nPhi).arg(w).arg(h)
                          .arg(static_cast<double>(nPhi) * w * h * 4.0 / (1024.0 * 1024.0),
                               0, 'f', 0));
    };
    QObject::connect(phiMin, &QDoubleSpinBox::valueChanged, &dlg, [&](double) { refreshCost(); });
    QObject::connect(phiMax, &QDoubleSpinBox::valueChanged, &dlg, [&](double) { refreshCost(); });
    QObject::connect(dPhi, &QDoubleSpinBox::valueChanged, &dlg, [&](double) { refreshCost(); });
    QObject::connect(useRegion, &QCheckBox::toggled, &dlg, [&](bool) { refreshCost(); });
    refreshCost();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;
    if (phiMax->value() <= phiMin->value()) {
        QMessageBox::warning(this, u"RM Synthesis"_s, u"φ max must be greater than φ min."_s);
        return;
    }

    const QRect region = useRegion->isChecked()
            ? QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1) : QRect();
    const double lo = phiMin->value(), hi = phiMax->value(), step = dPhi->value();
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, datasetId = this->remoteDatasetId;
    const BackendStokesCompanionIds companions = m_stokesCompanions;

    this->statusBar()->showMessage(u"Running RM synthesis…"_s, 0);
    auto *watcher = new QFutureWatcher<BackendRmSynthesisResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendRmSynthesisResult>::finished, this,
                     [self, watcher, region]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self)
                             return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"RM Synthesis"_s,
                                     r.error.isEmpty() ? u"The computation failed."_s : r.error);
                             return;
                         }
                         self->publishRmSynthesisMaps(r, region);
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, session, datasetId, lo, hi, step, companions, region]() {
                BackendClient c(url, token);
                c.setSessionId(session);
                return c.requestRmSynthesis(datasetId, lo, hi, step, companions,
                                            /*peakOnly=*/true, region);
            }));
}

void vtkWindowCube::publishRmSynthesisMaps(const BackendRmSynthesisResult &r, const QRect &region)
{
    auto peak = imageFromFloats(r.fdfData, r.width, r.height);
    auto depth = imageFromFloats(r.phiPeakData, r.width, r.height);
    if (!peak || !depth) {
        QMessageBox::warning(this, u"RM Synthesis"_s,
                u"The backend returned a payload that does not match the announced "
                u"%1 × %2 shape."_s.arg(r.width).arg(r.height));
        return;
    }

    QVariantMap params;
    params.insert(u"φ range"_s, u"%1 … %2 rad m⁻²"_s.arg(r.phiMin).arg(r.phiMax));
    params.insert(u"φ step"_s, r.dPhi);
    params.insert(u"φ samples"_s, r.nPhi);
    params.insert(u"Channels used"_s, r.nFreq);
    if (!r.droppedPlanes.isEmpty()) {
        // A release can leave a sub-band blank; the service leaves it out and
        // says so, and the product must say so too or "Channels used" lies.
        QStringList planes, mhz;
        for (int p : r.droppedPlanes) planes << QString::number(p);
        for (double f : r.droppedFreqHz) mhz << QString::number(f / 1e6, 'f', 0);
        params.insert(u"Sub-bands dropped"_s,
                      u"%1 MHz (planes %2, no data)"_s.arg(mhz.join(u", "_s), planes.join(u", "_s)));
    }
    if (region.isValid()) {
        params.insert(u"Region"_s, u"x %1–%2, y %3–%4"_s
                              .arg(region.left()).arg(region.right())
                              .arg(region.top()).arg(region.bottom()));
    }
    params.insert(u"Stokes from"_s,
                  m_hasOwnStokesAxis ? u"this cube's Stokes axis"_s : stokesCompanionSummary());

    // The region is part of the product's identity: without it, the same φ
    // parameters over a different box would silently replace the earlier maps
    // and refresh every pane showing them, relabelling one patch of sky as
    // another.
    const QString key = u"rm_%1_%2_%3_%4"_s.arg(r.phiMin).arg(r.phiMax).arg(r.dPhi)
                                .arg(region.isValid()
                                             ? u"x%1y%2w%3h%4"_s.arg(region.x()).arg(region.y())
                                                       .arg(region.width()).arg(region.height())
                                             : u"full"_s);
    const QString peakId = addImageProduct(
            u"polarisation"_s, u"fdf_peak_%1"_s.arg(key),
            u"Peak |F(φ)|"_s, peak, this->m_brightnessUnit, params, u"Inferno"_s);
    // Faraday depth is signed and centred on zero — a diverging map, like a
    // velocity field, so the sign is readable at a glance.
    const QString depthId = addImageProduct(
            u"polarisation"_s, u"fdf_depth_%1"_s.arg(key),
            u"Faraday depth (RM)"_s, depth, u"rad m⁻²"_s, params, u"CoolWarm"_s,
            /*symmetricRange=*/true);

    // The Faraday depth where it is measured: only where the peak stands above
    // the noise of the peak map. Elsewhere the "depth" is the φ of the largest
    // noise excursion, and a map of those reads like structure.
    QString significantId;
    constexpr double kSignificance = 6.0;
    if (std::isfinite(r.peakSigma) && r.peakSigma > 0.0
        && r.fdfData.size() == r.phiPeakData.size()) {
        QByteArray masked = r.phiPeakData;
        const auto *pk = reinterpret_cast<const float *>(r.fdfData.constData());
        auto *ph = reinterpret_cast<float *>(masked.data());
        const qsizetype n = masked.size() / qsizetype(sizeof(float));
        const double cut = kSignificance * r.peakSigma;
        for (qsizetype k = 0; k < n; ++k)
            if (!(std::isfinite(pk[k]) && pk[k] > cut))
                ph[k] = std::numeric_limits<float>::quiet_NaN();
        if (auto sig = imageFromFloats(masked, r.width, r.height); sig) {
            QVariantMap sp = params;
            sp.insert(u"Shown where"_s, u"peak |F| > %1σ (σ = %2, 1.4826·MAD of the peak map)"_s
                                               .arg(kSignificance)
                                               .arg(r.peakSigma, 0, 'g', 4));
            significantId = addImageProduct(
                    u"polarisation"_s, u"fdf_depth_sig_%1"_s.arg(key),
                    u"Faraday depth (peak |F| > %1σ)"_s.arg(kSignificance), sig, u"rad m⁻²"_s, sp,
                    u"CoolWarm"_s, /*symmetricRange=*/true);
        }
    }

    if (significantId.isEmpty())   // no noise this time: an earlier mask must not stay as this one's
        this->removeImageProduct(u"fdf_depth_sig_%1"_s.arg(key));

    // Where these pixels sit in the source grid, so exporting them writes the
    // astrometry of the crop and not of the whole field.
    if (region.isValid()) {
        for (const QString &id : { peakId, depthId, significantId }) {
            if (auto it = m_products2d.find(id); it != m_products2d.end())
                it->regionOrigin = region.topLeft();
        }
    }
}
