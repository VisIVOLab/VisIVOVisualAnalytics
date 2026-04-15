#include "LinewidthDialog.h"
#include "ScienceMapWindow.h"
#include "BackendClient.h"
#include "DiagnosticsManager.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
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

LinewidthDialog::LinewidthDialog(const QString &backendUrl,
                                 const QString &backendToken,
                                 const QString &sessionId,
                                 const QString &datasetId,
                                 int depth,
                                 double initialThreshold,
                                 QWidget *parent)
    : QMainWindow(parent)
    , m_backendUrl(backendUrl)
    , m_backendToken(backendToken)
    , m_sessionId(sessionId)
    , m_datasetId(datasetId)
    , m_depth(std::max(1, depth))
{
    setWindowTitle(u"Line-Width Map"_s);
    setMinimumWidth(480);

    // ── Central widget ────────────────────────────────────────────────────────
    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    mainLayout->addLayout(form);

    // ── Method ───────────────────────────────────────────────────────────────
    m_methodCombo = new QComboBox(central);
    m_methodCombo->addItem(u"FWHM"_s,             u"fwhm"_s);
    m_methodCombo->addItem(u"Equivalent Width"_s, u"ew"_s);
    form->addRow(u"Method"_s, m_methodCombo);

    // ── Rest frequency ────────────────────────────────────────────────────────
    m_restFreqSpin = new QDoubleSpinBox(central);
    m_restFreqSpin->setRange(0.0, 1000.0);
    m_restFreqSpin->setDecimals(6);
    m_restFreqSpin->setSuffix(u" GHz"_s);
    m_restFreqSpin->setValue(0.0);
    // Only relevant for FWHM (velocity-axis conversion).
    form->addRow(u"Rest frequency"_s, m_restFreqSpin);

    auto updateRestFreq = [this]() {
        m_restFreqSpin->setEnabled(!m_busy
                                   && m_methodCombo->currentData().toString() == u"fwhm"_s);
    };
    updateRestFreq();
    connect(m_methodCombo, &QComboBox::currentIndexChanged, this, updateRestFreq);

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
    form->addRow(u"Mask threshold"_s, m_thresholdSpin);

    connect(m_maskCheck, &QCheckBox::toggled, m_thresholdSpin, &QWidget::setEnabled);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_computeBtn    = new QPushButton(u"Compute"_s, central);
    auto *closeBtn  = new QPushButton(u"Close"_s,   central);
    m_computeBtn->setDefault(true);
    btnLayout->addWidget(m_computeBtn);
    btnLayout->addWidget(closeBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_computeBtn, &QPushButton::clicked, this, &LinewidthDialog::onCompute);
    connect(closeBtn,     &QPushButton::clicked, this, &QMainWindow::close);
}

// ─────────────────────────────────────────────────────────────────────────────

void LinewidthDialog::closeEvent(QCloseEvent *event)
{
    // Accept the close immediately.  If a worker is in-flight it completes on
    // the thread pool but the finished() handler checks QPointer<LinewidthDialog>
    // and returns without touching any UI once this object is gone.
    event->accept();
}

// ─────────────────────────────────────────────────────────────────────────────

void LinewidthDialog::onCompute()
{
    if (m_busy) {
        return; // guard against accidental double-click re-entrancy
    }

    const int chanStart = m_chanStartSpin->value() - 1; // convert to 0-based
    const int chanEnd   = m_chanEndSpin->value()   - 1;
    if (chanStart > chanEnd) {
        QMessageBox::warning(this, u"Line-Width Map"_s,
                             u"Start channel must be ≤ end channel."_s);
        return;
    }

    // ── Capture all parameters by value on the UI thread ─────────────────────
    // Nothing inside the worker lambda may read QWidget state.
    const QString backendUrl   = m_backendUrl;
    const QString backendToken = m_backendToken;
    const QString sessionId    = m_sessionId;
    const QString datasetId    = m_datasetId;
    const bool    maskEnabled  = m_maskCheck->isChecked();
    const float   threshold    = static_cast<float>(m_thresholdSpin->value());
    const QString method       = m_methodCombo->currentData().toString();

    m_busy = true;
    const int requestId = ++m_currentRequestId;
    setUiBusy(true);
    statusBar()->showMessage(u"Computing line-width map…"_s);

    // ── Watcher: no parent so its lifetime is controlled by deleteLater() ─────
    auto *watcher = new QFutureWatcher<BackendLinewidthResult>();
    QPointer<LinewidthDialog> self(this);

    connect(watcher, &QFutureWatcher<BackendLinewidthResult>::finished,
            watcher, [self, watcher, requestId, method, datasetId, sessionId]() {

        const auto result = watcher->result();
        watcher->deleteLater();

        // ── Window was closed while we were running — discard cleanly ─────────
        if (!self) {
            return;
        }

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

        const bool wantFwhm               = (method == u"fwhm"_s);
        const BinaryArrayResult &frame    = wantFwhm ? fwhmFrame : ewFrame;
        const bool frameOk                = wantFwhm ? ok1 : ok2;

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

        // ── Guard against empty result (all pixels failed to fit) ─────────────
        if (result.width == 0 || result.height == 0) {
            const QString emptyMsg =
                    u"Result is empty — all pixels failed to fit.\n"
                    "Try widening the channel range or lowering the threshold."_s;
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

        auto *win = new ScienceMapWindow(label,
                                         frame.data,
                                         result.width,
                                         result.height,
                                         rangeMin,
                                         rangeMax,
                                         /* parent */ nullptr);
        win->show();

        self->statusBar()->showMessage(
                u"Done — %1 map: %2 × %3 px"_s.arg(label).arg(result.width).arg(result.height),
                8000);
    });

    // ── Worker: creates its own BackendClient, captures only value types ──────
    // No raw pointers, no `this`, no `[&]`, no shared mutable state.
    watcher->setFuture(QtConcurrent::run(
            [backendUrl, backendToken, sessionId, datasetId,
             chanStart, chanEnd, maskEnabled, threshold]() -> BackendLinewidthResult {
                BackendClient localClient(backendUrl, backendToken);
                localClient.setSessionId(sessionId);
                return localClient.fetchLinewidthBinary(datasetId,
                                                        chanStart, chanEnd,
                                                        maskEnabled, threshold);
            }));
}

// ─────────────────────────────────────────────────────────────────────────────

void LinewidthDialog::setUiBusy(bool busy)
{
    m_computeBtn->setEnabled(!busy);
    m_methodCombo->setEnabled(!busy);
    m_restFreqSpin->setEnabled(!busy && m_methodCombo->currentData().toString() == u"fwhm"_s);
    m_chanStartSpin->setEnabled(!busy);
    m_chanEndSpin->setEnabled(!busy);
    m_maskCheck->setEnabled(!busy);
    m_thresholdSpin->setEnabled(!busy && m_maskCheck->isChecked());
}
