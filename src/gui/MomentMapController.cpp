// MomentMapController.cpp
// Owns the moment-map computation flow that was previously split across
// vtkWindowCube.cpp: momentComputeWatcher connection, setMomentOrder(),
// configureMomentRequest(), describeMomentOrder(), describeMomentScope(),
// and formatMomentChannelRange().

#include "MomentMapController.h"

#include "app/DiagnosticsManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
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
                ? u">= %1"_s.arg(config.thresholdValue, 0, 'g', 8)
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
                "Unit: %2\n"
                "BUNIT: %3\n"
                "Spectral axis: %4 [%5]\n"
                "Channel range: %6\n"
                "Mask: %7\n"
                "Threshold: %8\n"
                "NaN/blanking: excluded from computation\n"
                "Valid-pixel policy: only finite unmasked voxels contribute\n"
                "Source scope: %9"_s
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
                                     ? QString::number(config.thresholdValue, 'g', 8)
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
                                     config.thresholdValue }));
}

// ─────────────────────────────────────────────────────────────────────────────
// Dialog – moved verbatim from vtkWindowCube::configureMomentRequest()
// ─────────────────────────────────────────────────────────────────────────────

bool MomentMapController::configureMomentDialog(int defaultOrder, Config &config)
{
    QDialog dialog(m_parentWindow);
    dialog.setWindowTitle(u"Moment Map Settings"_s);
    dialog.setMinimumWidth(560);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    layout->addLayout(form);

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
    int defaultIndex = 0;
    for (int i = 0; i < static_cast<int>(orders.size()); ++i) {
        momentCombo->addItem(orders[static_cast<std::size_t>(i)].second,
                             orders[static_cast<std::size_t>(i)].first);
        if (orders[static_cast<std::size_t>(i)].first == defaultOrder) {
            defaultIndex = i;
        }
    }
    momentCombo->setCurrentIndex(defaultIndex);

    const int totalChannels = std::max(1, m_depth);
    auto *channelStartSpin = new QSpinBox(&dialog);
    auto *channelEndSpin = new QSpinBox(&dialog);
    channelStartSpin->setRange(1, totalChannels);
    channelEndSpin->setRange(1, totalChannels);
    channelStartSpin->setValue(std::clamp(config.channelStart + 1, 1, totalChannels));
    channelEndSpin->setValue(std::clamp(
            config.channelEnd > 0 ? config.channelEnd + 1 : totalChannels, 1, totalChannels));

    auto *maskCheck = new QCheckBox(u"Enable threshold mask"_s, &dialog);
    maskCheck->setChecked(config.maskEnabled);
    auto *thresholdSpin = new QDoubleSpinBox(&dialog);
    thresholdSpin->setRange(-1.0e30, 1.0e30);
    thresholdSpin->setDecimals(6);
    thresholdSpin->setValue(config.thresholdValue);
    thresholdSpin->setEnabled(config.maskEnabled);
    QObject::connect(maskCheck, &QCheckBox::toggled, thresholdSpin, &QWidget::setEnabled);

    auto *blankingLabel = new QLabel(u"NaN/blanked voxels excluded automatically"_s, &dialog);
    blankingLabel->setWordWrap(true);
    blankingLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    blankingLabel->setMinimumHeight(blankingLabel->fontMetrics().lineSpacing() * 2 + 8);
    auto *scopeLabel = new QLabel(describeMomentScope(), &dialog);
    scopeLabel->setWordWrap(true);
    scopeLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    scopeLabel->setMinimumHeight(scopeLabel->fontMetrics().lineSpacing() * 2 + 8);

    form->addRow(u"Moment order"_s, momentCombo);
    form->addRow(u"Start channel"_s, channelStartSpin);
    form->addRow(u"End channel"_s, channelEndSpin);
    form->addRow(u"Mask"_s, maskCheck);
    form->addRow(u"Mask threshold"_s, thresholdSpin);
    form->addRow(u"NaN/blanking"_s, blankingLabel);
    form->addRow(u"Source scope"_s, scopeLabel);

    auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

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
