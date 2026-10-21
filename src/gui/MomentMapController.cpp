// MomentMapController.cpp
// Owns the moment-map computation flow that was previously split across
// vtkWindowCube.cpp: momentComputeWatcher connection, setMomentOrder(),
// configureMomentRequest(), describeMomentOrder(), describeMomentScope(),
// and formatMomentChannelRange().

#include "MomentMapController.h"

#include "ToolProgressSheet.h"

#include "AnalysisParamStore.h"
#include "app/DiagnosticsManager.h"
#include "theme/ToolDialogStyle.h"
#include "theme/VisivoTheme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <vtkImageData.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <utility>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// MomentMapController
// ─────────────────────────────────────────────────────────────────────────────

MomentMapController::MomentMapController(
        const QString &filepath,
        const QString &backendUrl,
        const QString &datasetId,
        const QString &sessionId,
        const QString &backendToken,
        int depth,
        double initialThreshold,
        QWidget *parentWindow,
        std::function<void()> onRunFinished,
        std::function<void(const MomentMapComputeResult &,
                           const Config &,
                           const QString &,
                           const QString &)> onCompleted,
        QObject *parent)
    : QObject(parent)
    , m_filepath(filepath)
    , m_backendUrl(backendUrl)
    , m_datasetId(datasetId)
    , m_sessionId(sessionId)
    , m_backendToken(backendToken)
    , m_depth(depth)
    , m_parentWindow(parentWindow)
    , m_onRunFinished(std::move(onRunFinished))
    , m_onCompleted(std::move(onCompleted))
{
    m_currentConfig.thresholdValue = initialThreshold;
}

bool MomentMapController::isBusy() const
{
    return m_busy;
}

void MomentMapController::startComputation(int defaultOrder)
{
    if (m_busy) {
        return; // guard against re-entrancy
    }

    Config config = m_currentConfig;
    config.order = defaultOrder;
    // Restore the channel range + mask settings last used for THIS dataset so the
    // dialog re-opens with them across sessions (order comes from the menu action,
    // so it is NOT persisted).
    const QVariantMap saved = AnalysisParamStore().load(m_filepath, u"moment"_s);
    const bool restoredFromLastSession = !saved.isEmpty();
    {
        if (saved.contains(u"channelStart"_s))  config.channelStart  = saved.value(u"channelStart"_s).toInt();
        if (saved.contains(u"channelEnd"_s))    config.channelEnd    = saved.value(u"channelEnd"_s).toInt();
        if (saved.contains(u"maskEnabled"_s))   config.maskEnabled   = saved.value(u"maskEnabled"_s).toBool();
        if (saved.contains(u"thresholdAuto"_s)) config.thresholdAuto = saved.value(u"thresholdAuto"_s).toBool();
        if (saved.contains(u"thresholdValue"_s)) config.thresholdValue = saved.value(u"thresholdValue"_s).toDouble();
    }
    // Clamp restored channels to the current cube depth (defensive against a
    // corrupted/hand-edited settings file).
    {
        const int maxCh = std::max(0, m_depth - 1);
        config.channelStart = std::clamp(config.channelStart, 0, maxCh);
        config.channelEnd   = std::clamp(config.channelEnd, 0, maxCh);
    }
    // If the stored channel range is empty/uninitialised, default to the full cube.
    if (config.channelEnd <= config.channelStart) {
        const int totalChannels = std::max(1, m_depth);
        config.channelStart = 0;
        config.channelEnd = totalChannels - 1;
    }
    // The window owns the run from here: it validates, persists, raises its own
    // progress sheet and dispatches, and stays open afterwards. dispatch() is
    // also the copilot's entry point, so it creates no sheet of its own — an
    // agent-driven compute must not put a modal sheet in anyone's way.
    this->openMomentDialog(defaultOrder, config, restoredFromLastSession);
}

// Headless compute (no dialog): the scientific copilot builds a Config from its
// action parameters and computes+displays the moment via the same worker + the
// same onCompleted product-registration path as the interactive dialog.
void MomentMapController::startComputationHeadless(Config config)
{
    if (m_busy)
        return;
    const int maxCh = std::max(0, m_depth - 1);
    config.channelStart = std::clamp(config.channelStart, 0, maxCh);
    config.channelEnd = std::clamp(config.channelEnd, 0, maxCh);
    if (config.channelEnd < config.channelStart) {   // inverted range → full cube (defensive)
        config.channelStart = 0;
        config.channelEnd = std::max(1, m_depth) - 1;
    }
    m_currentConfig = config;
    AnalysisParamStore().save(m_filepath, u"moment"_s, {
            { u"channelStart"_s, config.channelStart }, { u"channelEnd"_s, config.channelEnd },
            { u"maskEnabled"_s, config.maskEnabled }, { u"thresholdAuto"_s, config.thresholdAuto },
            { u"thresholdValue"_s, config.thresholdValue } });
    this->dispatch(config);
}

// Dispatch the async moment computation for an already-resolved Config. Shared by
// the interactive startComputation() and the headless copilot path.
void MomentMapController::dispatch(const Config &config)
{
    const int requestId = ++m_currentRequestId;

    if (auto *win = qobject_cast<QMainWindow *>(m_parentWindow)) {
        win->statusBar()->showMessage(u"Computing moment map..."_s);
    }

    m_busy = true;

    // Notify immediately so vtkWindowCube can disable moment actions.
    if (m_onRunFinished) {
        m_onRunFinished();
    }

    // Capture by value – safe across thread boundary and if window closes.
    const QString filepath     = m_filepath;
    const QString backendUrl   = m_backendUrl;
    const QString datasetId    = m_datasetId;
    const QString sessionId    = m_sessionId;
    const QString backendToken = m_backendToken;

    QPointer<MomentMapController> self(this);
    auto *watcher = new QFutureWatcher<MomentMapComputeResult>();
    QObject::connect(watcher, &QFutureWatcher<MomentMapComputeResult>::finished,
                     watcher, [self, watcher, requestId, config]() {
        const auto result = watcher->result();
        watcher->deleteLater();

        if (!self) {
            return; // controller was destroyed while request was in-flight
        }

        if (requestId != self->m_currentRequestId) {
            return; // stale result from a superseded request – discard
        }
        // After the staleness check, never before: a superseded result must not
        // close the sheet belonging to the request that replaced it.
        visivo::closeToolProgressSheet(self->m_progressSheet);

        self->m_busy = false;

        // Re-enable moment actions first (same ordering as the original code).
        if (self->m_onRunFinished) {
            self->m_onRunFinished();
        }

        if (!result.valid || !result.imageData) {
            self->m_lastError = result.errorMessage.isEmpty()
                    ? u"MomentMap M%1: backend returned an error with no detail."_s.arg(config.order)
                    : u"MomentMap M%1: %2"_s.arg(config.order).arg(result.errorMessage);
            if (auto *win = qobject_cast<QMainWindow *>(self->m_parentWindow)) {
                win->statusBar()->showMessage(self->m_lastError);
            }
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error, DiagnosticsCategory::Scientific,
                    QStringLiteral("MomentMapController"),
                    self->m_lastError, self->m_datasetId, self->m_sessionId,
                    QStringLiteral("moment"));
            return;
        }
        self->m_lastError.clear();
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Scientific,
                QStringLiteral("MomentMapController"),
                QStringLiteral("Moment M%1 computed (channels %2-%3, mask=%4, threshold=%5).")
                        .arg(config.order)
                        .arg(config.channelStart)
                        .arg(config.channelEnd)
                        .arg(config.maskEnabled ? QStringLiteral("on") : QStringLiteral("off"))
                        .arg(config.thresholdValue, 0, 'g', 6),
                self->m_datasetId, self->m_sessionId, QStringLiteral("moment"));

        // Build provenance strings (pure string work – no UI access needed here).
        // vtkWindowCube's onCompleted lambda appends the two WCS-specific fields
        // (spectralAxisTitle, currentWcsFrameLabel) that require window state.
        const QString maskSummary = describeMask(config);
        const QString unitLabel = result.momentUnit.isEmpty() ? u"-"_s : result.momentUnit;

        const QString summary =
                u"Moment map: %1 | Unit: %2 | Ch: %3 | Mask: %4 | Scope: %5"_s
                        .arg(describeMomentOrder(config.order))
                        .arg(unitLabel)
                        .arg(formatMomentChannelRange(config))
                        .arg(maskSummary)
                        .arg(describeMomentScope());

        // The caller (vtkWindowCube) appends "\nAxis 3 semantics: …\nWCS frame: …"
        // to complete the tooltip, using spectralAxisTitle() / currentWcsFrameLabel().
        const QString details =
                u"Moment type: %1\n"
                u"Unit: %2\n"
                u"BUNIT: %3\n"
                u"Spectral axis: %4 [%5]\n"
                u"Channel range: %6\n"
                u"Mask: %7\n"
                u"Threshold: %8\n"
                u"NaN/blanking: excluded from computation\n"
                u"Valid-pixel policy: only finite unmasked voxels contribute\n"
                u"Source scope: %9"_s
                        .arg(describeMomentOrder(config.order))
                        .arg(unitLabel)
                        .arg(result.bunit.isEmpty() ? u"-"_s : result.bunit)
                        .arg(result.spectralAxisType.isEmpty() ? u"-"_s
                                                               : result.spectralAxisType)
                        .arg(result.spectralAxisUnit.isEmpty() ? u"-"_s
                                                               : result.spectralAxisUnit)
                        .arg(formatMomentChannelRange(config))
                        .arg(config.maskEnabled ? u"enabled"_s : u"none"_s)
                        .arg(config.maskEnabled
                                     ? (config.thresholdAuto
                                                ? u"auto (≈ 3σ, from MAD)"_s
                                                : QString::number(config.thresholdValue, 'g', 8))
                                     : u"n/a"_s)
                        .arg(describeMomentScope());

        if (self->m_onCompleted) {
            self->m_onCompleted(result, config, summary, details);
        }
    });

    watcher->setFuture(QtConcurrent::run(
            &computeMomentMap,
            MomentMapComputeRequest{ filepath,
                                     datasetId,
                                     backendUrl,
                                     sessionId,
                                     backendToken,
                                     config.order,
                                     config.channelStart,
                                     config.channelEnd,
                                     config.maskEnabled,
                                     config.thresholdValue,
                                     config.thresholdAuto }));
}

// ─────────────────────────────────────────────────────────────────────────────
// Dialog – moved verbatim from vtkWindowCube::configureMomentRequest()
// ─────────────────────────────────────────────────────────────────────────────

void MomentMapController::openMomentDialog(int defaultOrder, Config config,
                                           bool restoredFromLastSession)
{
    // The settings window STAYS OPEN across the compute, like every other tool
    // dialog: Compute does not close it, the progress sheet is window-modal on
    // IT (not on the cube window), and when the result lands the same window is
    // there to run again with different parameters. It used to be a modal
    // QDialog::exec() that closed on Accept, so the compute it started had no
    // window of its own and the sheet had to go on the viewer, which is not
    // where the user was looking (user report).
    if (m_configDialog) {
        m_configDialog->show();
        m_configDialog->raise();
        m_configDialog->activateWindow();
        return;
    }

    // Shared visual chrome (input theming + section headers + helper text)
    // is in ToolDialogStyle so every parameter dialog looks the same.
    const QString tBorder = QString::fromLatin1(VisivoTheme::kOutline());
    const QString tMuted  = QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());

    auto *dlg = new QDialog(m_parentWindow);
    m_configDialog = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(u"Moment Map Settings"_s);
    dlg->setMinimumWidth(580);
    dlg->setStyleSheet(ToolDialogStyle::inputCss());
    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    layout->addWidget(ToolDialogStyle::makeBreadcrumb(dlg, u"Compute Moment"_s));

    if (restoredFromLastSession) {
        auto *restoredHint = new QLabel(
                u"↺ Channel range and mask restored from your last session for this cube."_s,
                dlg);
        restoredHint->setStyleSheet(QStringLiteral("color: %1; font-style: italic;").arg(tMuted));
        restoredHint->setWordWrap(true);
        layout->addWidget(restoredHint);
    }

    // ── COMPUTE: moment order + per-order helper text ──────────────────────
    auto *computeForm = ToolDialogStyle::addSection(layout, u"Compute"_s, dlg);

    auto *momentCombo = new QComboBox(dlg);
    const std::array<std::pair<int, QString>, 9> orders = {
            std::pair{ 0,  u"Moment 0: Integrated intensity (M0)"_s },
            std::pair{ 1,  u"Moment 1: Intensity-weighted coordinate (M1)"_s },
            std::pair{ 2,  u"Moment 2: Spectral dispersion σ (M2)"_s },
            std::pair{ 3,  u"Moment 3: Skewness (M3)"_s },
            std::pair{ 4,  u"Moment 4: Excess kurtosis (M4)"_s },
            std::pair{ 5,  u"Moment 5: 5th-order diagnostic (M5)"_s },
            std::pair{ 6,  u"Moment 6: RMS (M6)"_s },
            std::pair{ 8,  u"Moment 8: Maximum (M8)"_s },
            std::pair{ 10, u"Moment 10: Minimum (M10)"_s },
    };
    // One-liner per moment order — gives the user the *why* without having
    // to open the docs. Phrased for working astronomers, not specs.
    auto orderHint = [](int order) -> QString {
        switch (order) {
        case 0:  return u"Sum I·Δv over the channel range, total line flux per pixel."_s;
        case 1:  return u"Intensity-weighted spectral centroid - reveals velocity gradients (rotation, outflows)."_s;
        case 2:  return u"Dispersion σ (the square root of the variance) in the cube's own "
                        u"spectral unit - km/s for a velocity axis, Hz for a frequency one. "
                        u"NOT FWHM: use the Line-width Map tool for FWHM/EW."_s;
        case 3:  return u"Asymmetry of the line profile, positive = high-velocity tail."_s;
        case 4:  return u"Peakedness vs Gaussian, positive = leptokurtic; negative = flat / double-peaked."_s;
        case 5:  return u"Standardised 5th moment, sensitive to asymmetric tails on top of skewness."_s;
        case 6:  return u"Root-mean-square brightness, quick signal-strength map."_s;
        case 8:  return u"Maximum intensity per spectrum - robust for narrow lines, equivalent to a peak map."_s;
        case 10: return u"Minimum intensity per spectrum, useful for absorption studies."_s;
        }
        return {};
    };
    int defaultIndex = 0;
    for (int i = 0; i < static_cast<int>(orders.size()); ++i) {
        momentCombo->addItem(orders[static_cast<std::size_t>(i)].second,
                             orders[static_cast<std::size_t>(i)].first);
        if (orders[static_cast<std::size_t>(i)].first == defaultOrder) {
            defaultIndex = i;
        }
    }
    momentCombo->setCurrentIndex(defaultIndex);

    auto *orderHintLabel = new QLabel(orderHint(defaultOrder), dlg);
    orderHintLabel->setStyleSheet(ToolDialogStyle::helperCss());
    orderHintLabel->setWordWrap(true);
    QObject::connect(momentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     orderHintLabel, [orderHintLabel, momentCombo, orderHint](int /*idx*/) {
                         orderHintLabel->setText(orderHint(momentCombo->currentData().toInt()));
                     });

    computeForm->addRow(u"Moment order"_s, momentCombo);
    computeForm->addRow(QString(), orderHintLabel);

    // ── CHANNEL RANGE: start/end on one row + range size hint ──────────────
    auto *rangeForm = ToolDialogStyle::addSection(layout, u"Channel range"_s, dlg);

    const int totalChannels = std::max(1, m_depth);
    auto *channelStartSpin = new QSpinBox(dlg);
    auto *channelEndSpin   = new QSpinBox(dlg);
    channelStartSpin->setRange(1, totalChannels);
    channelEndSpin->setRange(1, totalChannels);
    channelStartSpin->setValue(std::clamp(config.channelStart + 1, 1, totalChannels));
    channelEndSpin->setValue(std::clamp(
            config.channelEnd > 0 ? config.channelEnd + 1 : totalChannels, 1, totalChannels));

    // Put both spinboxes on the same row with a tiny "to" separator so the
    // form is more compact and the relationship between the two is clear.
    auto *rangeRow = new QWidget(dlg);
    auto *rangeLay = new QHBoxLayout(rangeRow);
    rangeLay->setContentsMargins(0, 0, 0, 0);
    rangeLay->setSpacing(8);
    rangeLay->addWidget(channelStartSpin, 1);
    auto *toLbl = new QLabel(u"→"_s, rangeRow);
    toLbl->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(tMuted));
    rangeLay->addWidget(toLbl);
    rangeLay->addWidget(channelEndSpin, 1);

    auto *rangeHintLabel = new QLabel(dlg);
    rangeHintLabel->setStyleSheet(ToolDialogStyle::helperCss());
    rangeHintLabel->setWordWrap(true);
    auto refreshRangeHint = [rangeHintLabel, channelStartSpin, channelEndSpin, totalChannels]() {
        const int s = channelStartSpin->value();
        const int e = channelEndSpin->value();
        const int n = (e >= s) ? (e - s + 1) : 0;
        const QString warn = (e < s)
                ? u"  ⚠ start must be ≤ end"_s
                : QString();
        rangeHintLabel->setText(u"%1 of %2 channels selected%3"_s
                                        .arg(n).arg(totalChannels).arg(warn));
    };
    QObject::connect(channelStartSpin, &QSpinBox::valueChanged, dlg, refreshRangeHint);
    QObject::connect(channelEndSpin,   &QSpinBox::valueChanged, dlg, refreshRangeHint);
    refreshRangeHint();

    rangeForm->addRow(u"Start → End"_s, rangeRow);
    rangeForm->addRow(QString(), rangeHintLabel);

    // ── MASKING: checkbox + threshold value ────────────────────────────────
    auto *maskForm = ToolDialogStyle::addSection(layout, u"Masking"_s, dlg);

    auto *maskCheck = new QCheckBox(u"Enable threshold mask"_s, dlg);
    maskCheck->setChecked(config.maskEnabled);

    auto *autoCheck = new QCheckBox(u"Auto threshold (≈ 3σ from MAD)"_s, dlg);
    autoCheck->setChecked(config.thresholdAuto);
    autoCheck->setEnabled(config.maskEnabled);

    auto *thresholdSpin = new QDoubleSpinBox(dlg);
    thresholdSpin->setRange(-1.0e30, 1.0e30);
    thresholdSpin->setDecimals(6);
    thresholdSpin->setValue(config.thresholdValue);
    // The session's noise estimate, when there is one, is the threshold the
    // user already measured: 3σ of it, offered first (and chosen by default).
    // The backend's Auto derives its own σ from the whole cube instead.
    QCheckBox *noiseCheck = nullptr;
    // Snapshot: a re-estimate while this window is open must not pair this
    // threshold with another region's description.
    const double noiseThreshold = 3.0 * m_noiseSigma;
    const QString noiseWhere = m_noiseWhere;
    if (m_noiseSigma > 0.0) {
        noiseCheck = new QCheckBox(
                u"3σ of the noise estimate (%1)"_s.arg(noiseThreshold, 0, 'g', 4), dlg);
        noiseCheck->setToolTip(u"σ = %1, measured with Tools › Estimate Noise on %2."_s
                                       .arg(m_noiseSigma, 0, 'g', 4).arg(noiseWhere));
        // Enough decimals to show a small σ as itself, not as 0.000000.
        if (noiseThreshold > 0.0)
            thresholdSpin->setDecimals(std::clamp(
                    static_cast<int>(std::ceil(-std::log10(noiseThreshold))) + 4, 6, 15));
        noiseCheck->setChecked(config.maskEnabled);
        if (noiseCheck->isChecked()) {
            autoCheck->setChecked(false);
            thresholdSpin->setValue(noiseThreshold);
        }
    }

    // The manual threshold is only editable when masking is on, and neither the
    // estimate nor Auto supplies it.
    auto updateThresholdEnabled = [maskCheck, autoCheck, thresholdSpin, noiseCheck]() {
        const bool on = maskCheck->isChecked();
        const bool fromNoise = noiseCheck && noiseCheck->isChecked();
        if (noiseCheck)
            noiseCheck->setEnabled(on);
        autoCheck->setEnabled(on && !fromNoise);
        thresholdSpin->setEnabled(on && !fromNoise && !autoCheck->isChecked());
    };
    updateThresholdEnabled();
    QObject::connect(maskCheck, &QCheckBox::toggled, dlg,
                     [updateThresholdEnabled](bool) { updateThresholdEnabled(); });
    QObject::connect(autoCheck, &QCheckBox::toggled, dlg,
                     [updateThresholdEnabled](bool) { updateThresholdEnabled(); });
    if (noiseCheck)
        QObject::connect(noiseCheck, &QCheckBox::toggled, dlg,
                         [updateThresholdEnabled, autoCheck, thresholdSpin, noiseThreshold](bool on) {
                             if (on) {
                                 autoCheck->setChecked(false);
                                 thresholdSpin->setValue(noiseThreshold);
                             }
                             updateThresholdEnabled();
                         });

    auto *maskHintLabel = new QLabel(
            (noiseCheck ? u"Only voxels above the threshold contribute to the moment - this "
                          u"cleans up M1/M2. The noise estimate gives 3σ of the σ you measured "
                          u"on a line-free region; Auto derives one from the whole cube "
                          u"(median + 3·σ via MAD); untick both to set a value by hand."_s
                        : u"Only voxels above the threshold contribute to the moment - this "
                          u"cleans up M1/M2. With Auto, the backend derives the threshold "
                          u"from the noise (median + 3·σ via MAD); untick it to set a value "
                          u"by hand. Tools › Estimate Noise measures σ on a region you choose, "
                          u"and it is then offered here."_s),
            dlg);
    maskHintLabel->setStyleSheet(ToolDialogStyle::helperCss());
    maskHintLabel->setWordWrap(true);

    maskForm->addRow(u"Mask"_s,           maskCheck);
    if (noiseCheck)
        maskForm->addRow(QString(),       noiseCheck);
    maskForm->addRow(QString(),           autoCheck);
    maskForm->addRow(u"Mask threshold"_s, thresholdSpin);
    maskForm->addRow(QString(),           maskHintLabel);

    // ── FOOTER: combined NaN/blanking + source-scope info ──────────────────
    // The old two-row form was visually noisy for two static facts. One
    // compact muted line keeps the same information density without
    // dominating the dlg->
    auto *divider = new QFrame(dlg);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: %1;").arg(tBorder));
    layout->addWidget(divider);

    auto *contextLabel = new QLabel(
            u"NaN / blanked voxels are excluded automatically · %1"_s
                    .arg(describeMomentScope()), dlg);
    contextLabel->setStyleSheet(ToolDialogStyle::helperCss());
    contextLabel->setWordWrap(true);
    layout->addWidget(contextLabel);

    // ── BUTTONS: Close (secondary) + Compute (primary brand-blue),
    // styled via VisivoTheme so they match every other tool dlg-> ─────
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, dlg);
    auto *okBtn    = VisivoTheme::makePrimaryButton(u"Compute"_s, dlg);
    okBtn->setDefault(true);
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(okBtn);
    layout->addLayout(btnRow);
    QObject::connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    QObject::connect(okBtn, &QPushButton::clicked, this,
                     [this, dlg, config, momentCombo, channelStartSpin, channelEndSpin,
                      maskCheck, autoCheck, thresholdSpin, noiseCheck, noiseThreshold,
                      noiseWhere]() mutable {
        if (m_busy)
            return;   // the sheet blocks the dialog, but be explicit
        const int channelStart = channelStartSpin->value() - 1;
        const int channelEnd   = channelEndSpin->value() - 1;
        if (channelStart > channelEnd) {
            QMessageBox::warning(dlg, u"Moment Map"_s,
                                 u"Start channel must be less than or equal to end channel."_s);
            return;
        }
        config.order          = momentCombo->currentData().toInt();
        config.channelStart   = channelStart;
        config.channelEnd     = channelEnd;
        config.maskEnabled    = maskCheck->isChecked();
        config.thresholdAuto  = autoCheck->isChecked();
        config.thresholdValue = thresholdSpin->value();
        const bool fromNoise = noiseCheck && noiseCheck->isChecked() && config.maskEnabled;
        if (fromNoise)
            config.thresholdValue = noiseThreshold;   // exact, not the box's rounding
        config.thresholdSource = fromNoise
                ? u"3σ of the noise estimate, %1"_s.arg(noiseWhere)
                : QString();

        m_currentConfig = config;
        AnalysisParamStore().save(m_filepath, u"moment"_s, {
                { u"channelStart"_s, config.channelStart },
                { u"channelEnd"_s, config.channelEnd },
                { u"maskEnabled"_s, config.maskEnabled },
                { u"thresholdAuto"_s, config.thresholdAuto },
                { u"thresholdValue"_s, config.thresholdValue } });

        // On the DIALOG, so the work is reported where it was asked for. The
        // sheet is window-modal, which also blocks the controls underneath it
        // for the duration, so nothing else needs disabling.
        m_progressSheet = visivo::showToolProgressSheet(dlg, u"Computing the moment map"_s);
        emit this->computationStarted(config);
        this->dispatch(config);
    });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

// ─────────────────────────────────────────────────────────────────────────────
// Static string helpers – moved from vtkWindowCube
// ─────────────────────────────────────────────────────────────────────────────

QString MomentMapController::describeMask(const Config &config)
{
    if (!config.maskEnabled)
        return u"none"_s;
    if (config.thresholdAuto)
        return u"auto (≈ 3σ)"_s;
    QString text = u">= %1"_s.arg(config.thresholdValue, 0, 'g', 8);
    if (!config.thresholdSource.isEmpty())
        text += u" (%1)"_s.arg(config.thresholdSource);
    return text;
}

QString MomentMapController::describeMomentOrder(int order)
{
    switch (order) {
    case 0:  return u"Moment 0 (Integrated intensity)"_s;
    case 1:  return u"Moment 1 (Intensity-weighted coordinate)"_s;
    // σ, not σ². The backend funnels every order-2 map through one function that
    // returns sqrt(variance) by default, matching CASA immoments, CARTA and
    // SoFiA, and the client never sets the moment2_variance flag that would keep
    // the raw variance — so these two labels said the opposite of what the user
    // was handed, and contradicted the copilot's own description of M2.
    // "Spectral", not "Velocity": the backend works in the cube's native
    // spectral coordinate and converts nothing, so on a CTYPE3=FREQ cube this
    // map is a frequency dispersion.
    case 2:  return u"Moment 2 (Spectral dispersion σ)"_s;
    case 3:  return u"Moment 3 (Skewness)"_s;
    case 4:  return u"Moment 4 (Excess kurtosis)"_s;
    case 5:  return u"Moment 5 (5th-order diagnostic)"_s;
    case 6:  return u"Moment 6 (RMS)"_s;
    case 8:  return u"Moment 8 (Maximum)"_s;
    case 10: return u"Moment 10 (Minimum)"_s;
    default: return u"Moment %1"_s.arg(order);
    }
}

QString MomentMapController::describeMomentScope()
{
    return u"Remote full dataset"_s;
}

QString MomentMapController::formatMomentChannelRange(const Config &config)
{
    return u"%1..%2"_s.arg(config.channelStart + 1).arg(config.channelEnd + 1);
}
