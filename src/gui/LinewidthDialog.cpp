#include "LinewidthDialog.h"
#include "ScienceMapWindow.h"
#include "BackendClient.h"
#include "DiagnosticsManager.h"
#include "theme/ToolDialogStyle.h"
#include "theme/VisivoTheme.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include "ToolDialogClose.h"
#include "ToolProgressSheet.h"

#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Static helpers
// ─────────────────────────────────────────────────────────────────────────────

LinewidthDialog::SpectralAxisType
LinewidthDialog::classifySpectralAxis(const QString &axisType, const QString &axisUnit)
{
    const QString t = axisType.trimmed().toUpper();
    const QString u = axisUnit.trimmed().toLower();

    // Velocity — FITS standard types and unit-based fallback.
    if (t.startsWith(u"VRAD"_s) || t.startsWith(u"VOPT"_s)
            || t.startsWith(u"FELO"_s) || t.startsWith(u"VELO"_s)
            || t.contains(u"VELO"_s)
            || u == u"m/s"_s || u == u"km/s"_s) {
        return SpectralAxisType::Velocity;
    }
    // Frequency — FITS FREQ type and common Hz-family units.
    if (t.startsWith(u"FREQ"_s)
            || u == u"hz"_s || u == u"khz"_s || u == u"mhz"_s || u == u"ghz"_s) {
        return SpectralAxisType::Frequency;
    }
    // Wavelength — FITS WAVE / AWAV and common length units.
    if (t.startsWith(u"WAVE"_s) || t.startsWith(u"AWAV"_s)
            || u == u"m"_s || u == u"nm"_s || u == u"um"_s
            || u == u"angstrom"_s || u == u"aa"_s) {
        return SpectralAxisType::Wavelength;
    }
    return SpectralAxisType::Unknown;
}

QString LinewidthDialog::spectralAxisTypeLabel(SpectralAxisType type)
{
    switch (type) {
    case SpectralAxisType::Velocity:   return u"Velocity"_s;
    case SpectralAxisType::Frequency:  return u"Frequency"_s;
    case SpectralAxisType::Wavelength: return u"Wavelength"_s;
    case SpectralAxisType::Unknown:    return u"Unknown"_s;
    }
    return u"Unknown"_s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

LinewidthDialog::LinewidthDialog(const QString &backendUrl,
                                 const QString &backendToken,
                                 const QString &sessionId,
                                 const QString &datasetId,
                                 int depth,
                                 double initialThreshold,
                                 const QString &spectralAxisType,
                                 const QString &spectralAxisUnit,
                                 QWidget *parent)
    : QMainWindow(parent)
    , m_backendUrl(backendUrl)
    , m_backendToken(backendToken)
    , m_sessionId(sessionId)
    , m_datasetId(datasetId)
    , m_depth(std::max(1, depth))
    , m_spectralAxisType(classifySpectralAxis(spectralAxisType, spectralAxisUnit))
    , m_spectralAxisUnit(spectralAxisUnit.trimmed())
{
    setWindowTitle(u"Line-Width Map"_s);
    setMinimumWidth(500);
    // Shared input theme — see ToolDialogStyle.
    setStyleSheet(ToolDialogStyle::inputCss());

    // ── Central widget ────────────────────────────────────────────────────────
    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);
    mainLayout->addWidget(ToolDialogStyle::makeBreadcrumb(central, u"Line-Width Map"_s));

    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    mainLayout->addLayout(form);

    // ── Spectral axis info (read-only) ────────────────────────────────────────
    m_axisInfoLabel = new QLabel(central);
    {
        const QString typeStr  = spectralAxisTypeLabel(m_spectralAxisType);
        const QString unitPart = m_spectralAxisUnit.isEmpty()
                ? QString()
                : u" (%1)"_s.arg(m_spectralAxisUnit);
        m_axisInfoLabel->setText(typeStr + unitPart);
    }
    m_axisInfoLabel->setToolTip(
            u"Spectral (third) axis classification derived from dataset WCS metadata.\n"
            u"This determines whether a rest frequency is required for computation."_s);
    // Style: slightly subdued to distinguish it from editable fields.
    m_axisInfoLabel->setStyleSheet(u"color: palette(mid);"_s);
    form->addRow(u"Spectral axis"_s, m_axisInfoLabel);

    // ── Method ───────────────────────────────────────────────────────────────
    m_methodCombo = new QComboBox(central);
    // Three methods, because the cost differs by orders of magnitude and the
    // dialog used to hide that: the Gaussian fit runs one scipy curve_fit PER
    // PIXEL (~10⁶ on a 1046×952 cube — minutes), while both other entries are
    // vectorised over the whole chunk (seconds). The equivalent width never
    // needed a fit at all: it is M0/peak, computed the same way on both paths,
    // so asking for it used to pay for a million fits whose result was then
    // discarded by this very dialog (audit).
    m_methodCombo->addItem(u"FWHM, Gaussian fit (slow, exact)"_s, u"fwhm"_s);
    m_methodCombo->addItem(u"FWHM, moment-2 dispersion (fast)"_s, u"fwhm_fast"_s);
    m_methodCombo->addItem(u"Equivalent width (fast, no fit)"_s,   u"ew"_s);
    m_methodCombo->setToolTip(
            u"FWHM (Gaussian fit): 2.3548·σ from a per-pixel Gaussian + baseline fit. "
            u"Exact for Gaussian-like emission, and the only one that costs a fit per "
            u"pixel.\n\n"
            u"FWHM (moment-2): 2.3548·√M2, the intensity-weighted velocity dispersion. "
            u"Vectorised, so seconds instead of minutes; it differs from the fit where "
            u"the profile is non-Gaussian (double-horned, self-absorbed, blended).\n\n"
            u"Equivalent width: M0/peak, a model-free width. Needs no fit either way."_s);
    form->addRow(u"Method"_s, m_methodCombo);

    // ── Rest frequency ────────────────────────────────────────────────────────
    // Only shown for frequency / wavelength / unknown axes; hidden for velocity.
    m_restFreqLabel = new QLabel(u"Rest frequency"_s, central);
    m_restFreqSpin  = new QDoubleSpinBox(central);
    m_restFreqSpin->setRange(0.0, 1'000.0);
    m_restFreqSpin->setDecimals(6);
    m_restFreqSpin->setSuffix(u" GHz"_s);
    m_restFreqSpin->setValue(0.0);
    m_restFreqSpin->setToolTip(
            u"Rest frequency used to convert from frequency to velocity.\n"
            u"Required for frequency and wavelength axes."_s);
    form->addRow(m_restFreqLabel, m_restFreqSpin);

    const bool isVelocity = (m_spectralAxisType == SpectralAxisType::Velocity);
    m_restFreqLabel->setVisible(!isVelocity);
    m_restFreqSpin->setVisible(!isVelocity);

    // ── Channel range ─────────────────────────────────────────────────────────
    m_chanStartSpin = new QSpinBox(central);
    m_chanEndSpin   = new QSpinBox(central);
    m_chanStartSpin->setRange(1, m_depth);
    m_chanEndSpin->setRange(1, m_depth);
    m_chanStartSpin->setValue(1);
    m_chanEndSpin->setValue(m_depth);
    form->addRow(u"Start channel"_s, m_chanStartSpin);
    form->addRow(u"End channel"_s,   m_chanEndSpin);

    // ── Threshold mask ────────────────────────────────────────────────────────
    m_maskCheck = new QCheckBox(u"Enable threshold mask"_s, central);
    m_maskCheck->setChecked(false);
    form->addRow(u"Mask"_s, m_maskCheck);

    m_thresholdSpin = new QDoubleSpinBox(central);
    m_thresholdSpin->setRange(-1.0e30, 1.0e30);
    m_thresholdSpin->setDecimals(6);
    m_thresholdSpin->setValue(initialThreshold);
    m_thresholdSpin->setEnabled(false);
    form->addRow(u"Threshold"_s, m_thresholdSpin);

    connect(m_maskCheck, &QCheckBox::toggled, m_thresholdSpin, &QWidget::setEnabled);

    // ── Buttons: Close (secondary) + Compute (primary), right-aligned.
    // The old bottom-left breadcrumb pill is gone — the top breadcrumb
    // header added by ToolDialogStyle::makeBreadcrumb carries that info.
    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, central);
    m_computeBtn   = VisivoTheme::makePrimaryButton(u"Compute"_s, central);
    m_computeBtn->setDefault(true);
    btnLayout->addWidget(closeBtn);
    btnLayout->addWidget(m_computeBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_computeBtn, &QPushButton::clicked, this, &LinewidthDialog::onCompute);
    connect(closeBtn,     &QPushButton::clicked, this, &QMainWindow::close);
}

// ─────────────────────────────────────────────────────────────────────────────

void LinewidthDialog::closeEvent(QCloseEvent *event)
{
    // A worker in flight completes on the thread pool and its finished() handler
    // discards the result (the QPointer to this dialog is null by then). That is
    // correct but used to be silent — see ToolDialogClose.h for what the user is
    // now told, and why "cancel" is not among the options.
    if (m_busy && !visivo::confirmAbandonRunningCompute(this, u"Line-Width Map"_s, u"The line-width map"_s)) {
        event->ignore();
        return;
    }
    event->accept();
}

// ─────────────────────────────────────────────────────────────────────────────

void LinewidthDialog::onCompute()
{
    if (m_busy) {
        return; // guard against accidental double-click re-entrancy
    }

    // ── Channel range validation ──────────────────────────────────────────────
    const int chanStart = m_chanStartSpin->value() - 1; // convert to 0-based
    const int chanEnd   = m_chanEndSpin->value()   - 1;
    if (chanStart < 0 || chanEnd >= m_depth || chanStart > chanEnd) {
        QMessageBox::warning(this, u"Line-Width Map"_s,
                             chanStart > chanEnd
                                     ? u"Start channel must be ≤ end channel."_s
                                     : u"Channel range is outside the valid range for this dataset."_s);
        return;
    }

    // ── Rest-frequency validation (frequency / wavelength axes only) ──────────
    double restFreqHz = 0.0;
    if (m_spectralAxisType != SpectralAxisType::Velocity) {
        const double restFreqGHz = m_restFreqSpin->value();
        if (m_spectralAxisType == SpectralAxisType::Frequency
                || m_spectralAxisType == SpectralAxisType::Wavelength) {
            // Strict validation for known non-velocity axes.
            if (restFreqGHz <= 0.0) {
                QMessageBox::warning(this, u"Line-Width Map"_s,
                                     u"A rest frequency > 0 is required for %1 axes.\n"
                                     u"Enter the line rest frequency in GHz."_s
                                     .arg(spectralAxisTypeLabel(m_spectralAxisType).toLower()));
                return;
            }
        }
        // Convert GHz → Hz for the backend.
        restFreqHz = restFreqGHz * 1.0e9;
    }

    // ── Capture all parameters by value on the UI thread ─────────────────────
    // Nothing inside the worker lambda may read QWidget state.
    const QString backendUrl   = m_backendUrl;
    const QString backendToken = m_backendToken;
    const QString sessionId    = m_sessionId;
    const QString datasetId    = m_datasetId;
    const bool    maskEnabled  = m_maskCheck->isChecked();
    const float   threshold    = static_cast<float>(m_thresholdSpin->value());
    const QString methodData   = m_methodCombo->currentData().toString();
    // "fwhm_fast" and "ew" both take the backend's vectorised path; only the
    // exact Gaussian fit pays per pixel.
    const bool    fastPath     = (methodData != u"fwhm"_s);
    // Which of the two returned frames to show: FWHM for both fwhm variants.
    const QString method       = methodData.startsWith(u"fwhm"_s) ? u"fwhm"_s : u"ew"_s;

    m_busy = true;
    const int requestId = ++m_currentRequestId;
    setUiBusy(true);
    statusBar()->showMessage(
            fastPath ? u"Computing line-width map (vectorised)…"_s
                     : u"Computing line-width map, one Gaussian fit per pixel, this takes "
                       u"minutes on a large cube…"_s);
    // …and the house progress sheet on top, so the work is visible without
    // reading the dialog's status bar (ToolProgressSheet.h).
    m_progressSheet = visivo::showToolProgressSheet(
            this, fastPath ? u"Computing the line-width map"_s
                           : u"Computing the line-width map, one Gaussian fit per pixel"_s);

    // ── Watcher: no parent so its lifetime is controlled by deleteLater() ─────
    auto *watcher = new QFutureWatcher<BackendLinewidthResult>();
    QPointer<LinewidthDialog> self(this);

    connect(watcher, &QFutureWatcher<BackendLinewidthResult>::finished,
            watcher, [self, watcher, requestId, method, datasetId, sessionId]() {

        const auto result = watcher->result();
        watcher->deleteLater();

        // ── Window closed while running — discard cleanly ─────────────────────
        if (!self) {
            return;
        }
        visivo::closeToolProgressSheet(self->m_progressSheet);

        // ── Newer request superseded this one — discard stale result ──────────
        if (requestId != self->m_currentRequestId) {
            return;
        }

        self->m_busy = false;
        self->setUiBusy(false);
        self->statusBar()->clearMessage();

        // ── Backend / network error ───────────────────────────────────────────
        if (!result.valid) {
            const QString errMsg = result.error.isEmpty()
                    ? u"Backend returned an error with no detail."_s
                    : result.error;
            QMessageBox::critical(self, u"Line-Width Map"_s, errMsg);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error,
                    DiagnosticsCategory::Scientific,
                    u"LinewidthDialog"_s,
                    errMsg,
                    datasetId,
                    sessionId,
                    u"linewidth"_s);
            return;
        }

        // ── Parse the two concatenated binary frames (FWHM then EW) ──────────
        int offset = 0;
        BinaryArrayResult fwhmFrame, ewFrame;
        const bool ok1 = BackendClient::parseBinaryFrame(result.binaryPayload, offset, fwhmFrame);
        const bool ok2 = BackendClient::parseBinaryFrame(result.binaryPayload, offset, ewFrame);

        const bool wantFwhm            = (method == u"fwhm"_s);
        const BinaryArrayResult &frame = wantFwhm ? fwhmFrame : ewFrame;
        const bool frameOk             = wantFwhm ? ok1 : ok2;

        if (!frameOk || frame.data.isEmpty()) {
            const QString parseErr =
                    u"Failed to parse the %1 result frame from the server."_s
                    .arg(wantFwhm ? u"FWHM"_s : u"EW"_s);
            QMessageBox::critical(self, u"Line-Width Map"_s, parseErr);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error,
                    DiagnosticsCategory::Scientific,
                    u"LinewidthDialog"_s,
                    parseErr,
                    datasetId,
                    sessionId,
                    u"linewidth"_s);
            return;
        }

        // ── Guard against empty result ────────────────────────────────────────
        if (result.width == 0 || result.height == 0) {
            const QString emptyMsg =
                    u"Result is empty, all pixels failed to fit.\n"
                    u"Try widening the channel range or lowering the threshold."_s;
            QMessageBox::warning(self, u"Line-Width Map"_s, emptyMsg);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning,
                    DiagnosticsCategory::Scientific,
                    u"LinewidthDialog"_s,
                    emptyMsg,
                    datasetId,
                    sessionId,
                    u"linewidth"_s);
            return;
        }

        // ── Compute scalar range (NaN/Inf-safe) ───────────────────────────────
        float rangeMin = 0.f, rangeMax = 1.f;
        const int nFloats = frame.data.size() / static_cast<int>(sizeof(float));
        if (nFloats > 0) {
            const float *ptr = reinterpret_cast<const float *>(frame.data.constData());
            float lo =  std::numeric_limits<float>::infinity();
            float hi = -std::numeric_limits<float>::infinity();
            for (int i = 0; i < nFloats; ++i) {
                const float v = ptr[i];
                if (std::isfinite(v)) {
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
            }
            if (std::isfinite(lo) && std::isfinite(hi)) {
                rangeMin = lo;
                rangeMax = (hi > lo) ? hi : lo + 1.f;
            }
        }

        // ── Open the result window ────────────────────────────────────────────
        const QString unit  = wantFwhm ? result.fwhmUnit : result.ewUnit;
        const QString label = wantFwhm
                ? u"FWHM"_s + (unit.isEmpty() ? QString() : u" ["_s + unit + u"]"_s)
                : u"EW"_s   + (unit.isEmpty() ? QString() : u" ["_s + unit + u"]"_s);

        // #5: the map is a first-class PANE product now, not a standalone window — hand the
        // raw float map to the caller (vtkWindowCube), which caches + registers + mounts it.
        emit self->mapReady(frame.data, result.width, result.height, rangeMin, rangeMax, label,
                            unit);

        self->statusBar()->showMessage(
                u"Done, %1 map: %2 × %3 px"_s.arg(label).arg(result.width).arg(result.height),
                8000);
    });

    // ── Worker: creates its own BackendClient, captures only value types ──────
    // No raw pointers, no `this`, no `[&]`, no shared mutable state.
    watcher->setFuture(QtConcurrent::run(
            [backendUrl, backendToken, sessionId, datasetId, chanStart, chanEnd, maskEnabled,
             threshold, restFreqHz, fastPath]() -> BackendLinewidthResult {
                BackendClient localClient(backendUrl, backendToken);
                localClient.setSessionId(sessionId);
                return localClient.fetchLinewidthBinary(datasetId,
                                                        chanStart, chanEnd,
                                                        maskEnabled, threshold,
                                                        restFreqHz, fastPath);
            }));
}

// ─────────────────────────────────────────────────────────────────────────────

void LinewidthDialog::setUiBusy(bool busy)
{
    m_computeBtn->setEnabled(!busy);
    m_methodCombo->setEnabled(!busy);
    // Keep rest-freq visible/hidden state; only toggle enabled when not velocity.
    if (m_spectralAxisType != SpectralAxisType::Velocity) {
        m_restFreqSpin->setEnabled(!busy);
    }
    m_chanStartSpin->setEnabled(!busy);
    m_chanEndSpin->setEnabled(!busy);
    m_maskCheck->setEnabled(!busy);
    m_thresholdSpin->setEnabled(!busy && m_maskCheck->isChecked());
}
