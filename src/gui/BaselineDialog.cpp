#include "BaselineDialog.h"
#include "BackendClient.h"
#include "DiagnosticsManager.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>

#include <algorithm>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

BaselineDialog::BaselineDialog(const QString &backendUrl,
                               const QString &backendToken,
                               const QString &sessionId,
                               const QString &datasetId,
                               int depth,
                               QWidget *parent)
    : QMainWindow(parent)
    , m_backendUrl(backendUrl)
    , m_backendToken(backendToken)
    , m_sessionId(sessionId)
    , m_datasetId(datasetId)
    , m_depth(std::max(1, depth))
{
    setWindowTitle(u"Spectral Baseline Subtraction"_s);
    setMinimumWidth(520);

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
    m_methodCombo->addItem(u"Polynomial"_s, u"polynomial"_s);
    m_methodCombo->addItem(u"Median"_s,     u"median"_s);
    form->addRow(u"Method"_s, m_methodCombo);

    // ── Polynomial order ──────────────────────────────────────────────────────
    m_polyOrderLabel = new QLabel(u"Polynomial order"_s, central);
    m_polyOrderSpin  = new QSpinBox(central);
    m_polyOrderSpin->setRange(0, 10);
    m_polyOrderSpin->setValue(1);
    form->addRow(m_polyOrderLabel, m_polyOrderSpin);

    // ── Line-free channel ranges ──────────────────────────────────────────────
    m_lineFreeEdit = new QLineEdit(central);
    m_lineFreeEdit->setPlaceholderText(u"e.g.  1-50, 200-300  (1-based, inclusive)"_s);
    auto *hintLabel = new QLabel(
            u"<i><b>Required.</b> Enter spectral ranges that contain <b>no signal</b> "
            "(line-free regions). The baseline is estimated from these channels only "
            "and then subtracted from the full spectrum. "
            "Fitting on all channels will absorb real line emission.</i>"_s, central);
    hintLabel->setWordWrap(true);
    form->addRow(u"Line-free ranges"_s, m_lineFreeEdit);
    form->addRow(QString(), hintLabel);

    // Disable poly-order for median mode.
    auto updatePolyOrder = [this]() {
        const bool isPoly = m_methodCombo->currentData().toString() == u"polynomial"_s;
        m_polyOrderLabel->setEnabled(isPoly);
        m_polyOrderSpin->setEnabled(isPoly);
    };
    updatePolyOrder();
    connect(m_methodCombo, &QComboBox::currentIndexChanged, this, updatePolyOrder);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_computeBtn   = new QPushButton(u"Subtract Baseline"_s, central);
    auto *closeBtn = new QPushButton(u"Close"_s, central);
    m_computeBtn->setDefault(true);
    // Explicitly NOT checkable — must behave as a plain command button.
    m_computeBtn->setCheckable(false);
    btnLayout->addWidget(m_computeBtn);
    btnLayout->addWidget(closeBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_computeBtn, &QPushButton::clicked, this, &BaselineDialog::onCompute);
    connect(closeBtn,     &QPushButton::clicked, this, &QMainWindow::close);
}

// ─────────────────────────────────────────────────────────────────────────────

void BaselineDialog::closeEvent(QCloseEvent *event)
{
    // Accept immediately.  If a worker is in-flight it completes on the thread
    // pool, but the finished() handler checks QPointer<BaselineDialog> and
    // returns without touching any UI once this object is gone.
    event->accept();
}

// ─────────────────────────────────────────────────────────────────────────────

void BaselineDialog::onCompute()
{
    if (m_busy) {
        return; // guard against accidental double-click re-entrancy
    }

    // ── Parse and validate line-free channel ranges ───────────────────────────
    const QString rangeText = m_lineFreeEdit->text().trimmed();
    if (rangeText.isEmpty()) {
        QMessageBox::warning(
            this,
            u"Baseline Subtraction"_s,
            u"Line-free channels are required.\n\n"
            "Enter at least one spectral range that contains no signal "
            "(e.g. \"1-50, 200-300\"). The baseline is fitted on those channels "
            "only and subtracted from the full spectrum.\n\n"
            "Fitting on all channels would absorb real line emission and "
            "produce an over-subtracted, nearly empty cube."_s);
        m_lineFreeEdit->setFocus();
        return;
    }
    QString parseError;
    QVector<QPair<int,int>> lineFreeRanges;
    lineFreeRanges = parseLineFreeRanges(rangeText, m_depth, parseError);
    if (!parseError.isEmpty()) {
        QMessageBox::warning(this, u"Baseline Subtraction"_s, parseError);
        return;
    }

    // ── Capture all parameters by value on the UI thread ─────────────────────
    // Nothing inside the worker lambda may read QWidget state.
    const QString backendUrl   = m_backendUrl;
    const QString backendToken = m_backendToken;
    const QString sessionId    = m_sessionId;
    const QString datasetId    = m_datasetId;
    const QString method       = m_methodCombo->currentData().toString();
    const int     polyOrder    = m_polyOrderSpin->value();

    m_busy = true;
    const int requestId = ++m_currentRequestId;
    setUiBusy(true);
    statusBar()->showMessage(u"Subtracting baseline…"_s);

    // ── Watcher: no parent so its lifetime is controlled by deleteLater() ─────
    auto *watcher = new QFutureWatcher<BackendBaselineResult>();
    QPointer<BaselineDialog> self(this);

    connect(watcher, &QFutureWatcher<BackendBaselineResult>::finished,
            watcher, [self, watcher, requestId, datasetId, sessionId]() {

        const auto result = watcher->result();
        watcher->deleteLater();

        // ── Window closed while running — discard cleanly ─────────────────────
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
            QMessageBox::critical(self, u"Baseline Subtraction"_s, errMsg);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error,
                    DiagnosticsCategory::Scientific,
                    u"BaselineDialog"_s,
                    errMsg,
                    datasetId,
                    sessionId,
                    u"baseline"_s);
            return;
        }

        // ── Success: build summary and notify ─────────────────────────────────
        const QString summary =
                u"Baseline subtraction complete.\n\n"
                "Method: %1\n"
                "New dataset ID: %2\n"
                "Output path: %3\n"
                "RMS before: %4\n"
                "RMS after:  %5\n"
                "Fit channels: %6\n"
                "Dimensions: %7 × %8 × %9 (W×H×D)"_s
                        .arg(result.method.isEmpty() ? u"n/a"_s : result.method)
                        .arg(result.newDatasetId)
                        .arg(result.outputPath.isEmpty() ? u"n/a"_s : result.outputPath)
                        .arg(result.rmsBefore, 0, 'g', 6)
                        .arg(result.rmsAfter,  0, 'g', 6)
                        .arg(result.fitChannels)
                        .arg(result.width).arg(result.height).arg(result.depth);

        // Persist in diagnostics log.
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info,
                DiagnosticsCategory::Scientific,
                u"BaselineDialog"_s,
                u"Baseline subtraction completed. New dataset: %1"_s.arg(result.newDatasetId),
                datasetId,
                sessionId,
                u"baseline"_s);

        // Show result in status bar (short form) and notify connected listeners.
        const QString shortMsg =
                u"Done — new dataset: %1  (RMS %2 → %3)"_s
                .arg(result.newDatasetId)
                .arg(result.rmsBefore, 0, 'g', 4)
                .arg(result.rmsAfter,  0, 'g', 4);
        self->statusBar()->showMessage(shortMsg, 12000);

        // Emit signal so connected callers (vtkWindowCube, MainWindow) can load
        // the new dataset into the viewer without this tool window needing to
        // know about the viewer's internals.
        emit self->baselineResultReady(result.newDatasetId, summary);

        // Show a non-blocking information summary.
        QMessageBox::information(self, u"Baseline Subtraction"_s, summary);
        // NOTE: do NOT call accept() or close() here — the window stays open
        // so the user can re-run with different parameters.
    });

    // ── Worker: creates its own BackendClient, captures only value types ──────
    // No raw pointers, no `this`, no `[&]`, no shared mutable state.
    watcher->setFuture(QtConcurrent::run(
            [backendUrl, backendToken, sessionId, datasetId,
             method, polyOrder, lineFreeRanges]() -> BackendBaselineResult {
                BackendClient localClient(backendUrl, backendToken);
                localClient.setSessionId(sessionId);
                return localClient.requestBaseline(datasetId,
                                                   0, 0,  // channelStart/End unused
                                                   polyOrder,
                                                   method,
                                                   lineFreeRanges);
            }));
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helper — parse "1-50, 200-300" into 0-based [start, end] pairs
// ─────────────────────────────────────────────────────────────────────────────

QVector<QPair<int,int>> BaselineDialog::parseLineFreeRanges(const QString &text,
                                                             int depth,
                                                             QString &errorOut)
{
    QVector<QPair<int,int>> result;
    const QStringList parts = text.split(QRegularExpression(u"[,;]+"_s), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        // Accept "N-M", "N..M", or "N:M"
        const QRegularExpression re(u"^(\\d+)\\s*[-..:]+(\\s*(\\d+))?$"_s);
        const auto match = re.match(trimmed);
        if (!match.hasMatch()) {
            bool ok = false;
            const int ch = trimmed.toInt(&ok);
            if (!ok || ch < 1 || ch > depth) {
                errorOut = u"Cannot parse channel range: \"%1\""_s.arg(trimmed);
                return {};
            }
            result.append({ ch - 1, ch - 1 });
            continue;
        }
        bool ok1 = false, ok2 = false;
        const int start = match.captured(1).toInt(&ok1);
        const int end   = match.captured(3).isEmpty()
                        ? start
                        : match.captured(3).toInt(&ok2);
        if (!ok1 || (!match.captured(3).isEmpty() && !ok2)) {
            errorOut = u"Cannot parse channel range: \"%1\""_s.arg(trimmed);
            return {};
        }
        if (start < 1 || end < 1 || start > depth || end > depth) {
            errorOut = u"Channel range out of bounds (1–%1): \"%2\""_s.arg(depth).arg(trimmed);
            return {};
        }
        if (start > end) {
            errorOut = u"Start channel must be ≤ end channel in: \"%1\""_s.arg(trimmed);
            return {};
        }
        result.append({ start - 1, end - 1 });   // convert to 0-based
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────

void BaselineDialog::setUiBusy(bool busy)
{
    m_computeBtn->setEnabled(!busy);
    m_methodCombo->setEnabled(!busy);
    const bool isPoly = m_methodCombo->currentData().toString() == u"polynomial"_s;
    m_polyOrderLabel->setEnabled(!busy && isPoly);
    m_polyOrderSpin->setEnabled(!busy && isPoly);
    m_lineFreeEdit->setEnabled(!busy);
}
