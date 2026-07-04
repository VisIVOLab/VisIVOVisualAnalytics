// MomentMapController.cpp
// Owns the moment-map computation flow that was previously split across
// vtkWindowCube.cpp: momentComputeWatcher connection, setMomentOrder(),
// configureMomentRequest(), describeMomentOrder(), describeMomentScope(),
// and formatMomentChannelRange().

#include "MomentMapController.h"

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
    {
        const QVariantMap saved = AnalysisParamStore().load(m_filepath, u"moment"_s);
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
    if (!configureMomentDialog(defaultOrder, config)) {
        return;
    }
    m_currentConfig = config;
    // Persist for the next session.
    AnalysisParamStore().save(m_filepath, u"moment"_s, {
            { u"channelStart"_s, config.channelStart }, { u"channelEnd"_s, config.channelEnd },
            { u"maskEnabled"_s, config.maskEnabled }, { u"thresholdAuto"_s, config.thresholdAuto },
            { u"thresholdValue"_s, config.thresholdValue } });

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
        const QString maskSummary = config.maskEnabled
                ? (config.thresholdAuto
                           ? u"auto (≈ 3σ)"_s
                           : u">= %1"_s.arg(config.thresholdValue, 0, 'g', 8))
                : u"none"_s;
        const QString unitLabel = result.momentUnit.isEmpty() ? u"\u2014"_s : result.momentUnit;

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
                        .arg(result.bunit.isEmpty() ? u"\u2014"_s : result.bunit)
                        .arg(result.spectralAxisType.isEmpty() ? u"\u2014"_s
                                                               : result.spectralAxisType)
                        .arg(result.spectralAxisUnit.isEmpty() ? u"\u2014"_s
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

bool MomentMapController::configureMomentDialog(int defaultOrder, Config &config)
{
    // Shared visual chrome (input theming + section headers + helper text)
    // is in ToolDialogStyle so every parameter dialog looks the same.
    const QString tBorder = QString::fromLatin1(VisivoTheme::kOutline());
    const QString tMuted  = QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());

    QDialog dialog(m_parentWindow);
    dialog.setWindowTitle(u"Moment Map Settings"_s);
    dialog.setMinimumWidth(580);
    dialog.setStyleSheet(ToolDialogStyle::inputCss());
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    layout->addWidget(ToolDialogStyle::makeBreadcrumb(&dialog, u"Compute Moment"_s));

    // ── COMPUTE: moment order + per-order helper text ──────────────────────
    auto *computeForm = ToolDialogStyle::addSection(layout, u"Compute"_s, &dialog);

    auto *momentCombo = new QComboBox(&dialog);
    const std::array<std::pair<int, QString>, 9> orders = {
            std::pair{ 0,  u"Moment 0: Integrated intensity (M0)"_s },
            std::pair{ 1,  u"Moment 1: Intensity-weighted coordinate (M1)"_s },
            std::pair{ 2,  u"Moment 2: Variance – NOT velocity dispersion (M2)"_s },
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
        case 0:  return u"Sum I·Δv over the channel range — total line flux per pixel."_s;
        case 1:  return u"Intensity-weighted spectral centroid — reveals velocity gradients (rotation, outflows)."_s;
        case 2:  return u"Spectral variance, NOT FWHM. Use the Line-width Map tool for FWHM/EW."_s;
        case 3:  return u"Asymmetry of the line profile — positive = high-velocity tail."_s;
        case 4:  return u"Peakedness vs Gaussian — positive = leptokurtic; negative = flat / double-peaked."_s;
        case 5:  return u"Standardised 5th moment — sensitive to asymmetric tails on top of skewness."_s;
        case 6:  return u"Root-mean-square brightness — quick signal-strength map."_s;
        case 8:  return u"Maximum intensity per spectrum — robust for narrow lines, equivalent to a peak map."_s;
        case 10: return u"Minimum intensity per spectrum — useful for absorption studies."_s;
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

    auto *orderHintLabel = new QLabel(orderHint(defaultOrder), &dialog);
    orderHintLabel->setStyleSheet(ToolDialogStyle::helperCss());
    orderHintLabel->setWordWrap(true);
    QObject::connect(momentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     orderHintLabel, [orderHintLabel, momentCombo, orderHint](int /*idx*/) {
                         orderHintLabel->setText(orderHint(momentCombo->currentData().toInt()));
                     });

    computeForm->addRow(u"Moment order"_s, momentCombo);
    computeForm->addRow(QString(), orderHintLabel);

    // ── CHANNEL RANGE: start/end on one row + range size hint ──────────────
    auto *rangeForm = ToolDialogStyle::addSection(layout, u"Channel range"_s, &dialog);

    const int totalChannels = std::max(1, m_depth);
    auto *channelStartSpin = new QSpinBox(&dialog);
    auto *channelEndSpin   = new QSpinBox(&dialog);
    channelStartSpin->setRange(1, totalChannels);
    channelEndSpin->setRange(1, totalChannels);
    channelStartSpin->setValue(std::clamp(config.channelStart + 1, 1, totalChannels));
    channelEndSpin->setValue(std::clamp(
            config.channelEnd > 0 ? config.channelEnd + 1 : totalChannels, 1, totalChannels));

    // Put both spinboxes on the same row with a tiny "to" separator so the
    // form is more compact and the relationship between the two is clear.
    auto *rangeRow = new QWidget(&dialog);
    auto *rangeLay = new QHBoxLayout(rangeRow);
    rangeLay->setContentsMargins(0, 0, 0, 0);
    rangeLay->setSpacing(8);
    rangeLay->addWidget(channelStartSpin, 1);
    auto *toLbl = new QLabel(u"→"_s, rangeRow);
    toLbl->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(tMuted));
    rangeLay->addWidget(toLbl);
    rangeLay->addWidget(channelEndSpin, 1);

    auto *rangeHintLabel = new QLabel(&dialog);
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
    QObject::connect(channelStartSpin, &QSpinBox::valueChanged, &dialog, refreshRangeHint);
    QObject::connect(channelEndSpin,   &QSpinBox::valueChanged, &dialog, refreshRangeHint);
    refreshRangeHint();

    rangeForm->addRow(u"Start → End"_s, rangeRow);
    rangeForm->addRow(QString(), rangeHintLabel);

    // ── MASKING: checkbox + threshold value ────────────────────────────────
    auto *maskForm = ToolDialogStyle::addSection(layout, u"Masking"_s, &dialog);

    auto *maskCheck = new QCheckBox(u"Enable threshold mask"_s, &dialog);
    maskCheck->setChecked(config.maskEnabled);

    auto *autoCheck = new QCheckBox(u"Auto threshold (≈ 3σ from MAD)"_s, &dialog);
    autoCheck->setChecked(config.thresholdAuto);
    autoCheck->setEnabled(config.maskEnabled);

    auto *thresholdSpin = new QDoubleSpinBox(&dialog);
    thresholdSpin->setRange(-1.0e30, 1.0e30);
    thresholdSpin->setDecimals(6);
    thresholdSpin->setValue(config.thresholdValue);
    // The manual threshold is only editable when masking is on AND auto is off.
    thresholdSpin->setEnabled(config.maskEnabled && !config.thresholdAuto);

    auto updateThresholdEnabled = [maskCheck, autoCheck, thresholdSpin]() {
        const bool on = maskCheck->isChecked();
        autoCheck->setEnabled(on);
        thresholdSpin->setEnabled(on && !autoCheck->isChecked());
    };
    QObject::connect(maskCheck, &QCheckBox::toggled, &dialog,
                     [updateThresholdEnabled](bool) { updateThresholdEnabled(); });
    QObject::connect(autoCheck, &QCheckBox::toggled, &dialog,
                     [updateThresholdEnabled](bool) { updateThresholdEnabled(); });

    auto *maskHintLabel = new QLabel(
            u"Only voxels above the threshold contribute to the moment — this "
            u"cleans up M1/M2. With Auto, the backend derives the threshold "
            u"from the noise (median + 3·σ via MAD); untick it to set a value "
            u"by hand."_s, &dialog);
    maskHintLabel->setStyleSheet(ToolDialogStyle::helperCss());
    maskHintLabel->setWordWrap(true);

    maskForm->addRow(u"Mask"_s,           maskCheck);
    maskForm->addRow(QString(),           autoCheck);
    maskForm->addRow(u"Mask threshold"_s, thresholdSpin);
    maskForm->addRow(QString(),           maskHintLabel);

    // ── FOOTER: combined NaN/blanking + source-scope info ──────────────────
    // The old two-row form was visually noisy for two static facts. One
    // compact muted line keeps the same information density without
    // dominating the dialog.
    auto *divider = new QFrame(&dialog);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: %1;").arg(tBorder));
    layout->addWidget(divider);

    auto *contextLabel = new QLabel(
            u"NaN / blanked voxels are excluded automatically · %1"_s
                    .arg(describeMomentScope()), &dialog);
    contextLabel->setStyleSheet(ToolDialogStyle::helperCss());
    contextLabel->setWordWrap(true);
    layout->addWidget(contextLabel);

    // ── BUTTONS: Close (secondary) + Compute (primary brand-blue),
    // styled via VisivoTheme so they match every other tool dialog. ─────
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, &dialog);
    auto *okBtn    = VisivoTheme::makePrimaryButton(u"Compute"_s, &dialog);
    okBtn->setDefault(true);
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(okBtn);
    layout->addLayout(btnRow);
    QObject::connect(okBtn,    &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    const int channelStart = channelStartSpin->value() - 1;
    const int channelEnd = channelEndSpin->value() - 1;
    if (channelStart > channelEnd) {
        QMessageBox::warning(m_parentWindow, u"Moment Map"_s,
                             u"Start channel must be less than or equal to end channel."_s);
        return false;
    }

    config.order = momentCombo->currentData().toInt();
    config.channelStart = channelStart;
    config.channelEnd = channelEnd;
    config.maskEnabled = maskCheck->isChecked();
    config.thresholdAuto = autoCheck->isChecked();
    config.thresholdValue = thresholdSpin->value();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Static string helpers – moved from vtkWindowCube
// ─────────────────────────────────────────────────────────────────────────────

QString MomentMapController::describeMomentOrder(int order)
{
    switch (order) {
    case 0:  return u"Moment 0 (Integrated intensity)"_s;
    case 1:  return u"Moment 1 (Intensity-weighted coordinate)"_s;
    case 2:  return u"Moment 2 (Variance \u2013 NOT dispersion)"_s;
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
