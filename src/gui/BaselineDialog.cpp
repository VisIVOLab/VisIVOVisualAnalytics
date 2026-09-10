#include "BaselineDialog.h"
#include "BackendClient.h"
#include "DiagnosticsManager.h"
#include "theme/ToolDialogStyle.h"
#include "theme/VisivoTheme.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include "ToolDialogClose.h"

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
    // Shared input theme (chevron dropdown, consistent border/hover/focus
    // on combos and spinboxes) — see ToolDialogStyle for the rationale.
    setStyleSheet(ToolDialogStyle::inputCss());

    // ── Central widget ────────────────────────────────────────────────────────
    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(16, 14, 16, 14);
    mainLayout->setSpacing(10);
    mainLayout->addWidget(ToolDialogStyle::makeBreadcrumb(central, u"Baseline Subtraction"_s));

    // ── METHOD: fit type + polynomial order ──────────────────────────────
    auto *methodForm = ToolDialogStyle::addSection(mainLayout, u"Method"_s, central);

    m_methodCombo = new QComboBox(central);
    m_methodCombo->addItem(u"Polynomial"_s, u"polynomial"_s);
    m_methodCombo->addItem(u"Median"_s,     u"median"_s);
    methodForm->addRow(u"Fit"_s, m_methodCombo);

    m_polyOrderLabel = new QLabel(u"Polynomial order"_s, central);
    m_polyOrderSpin  = new QSpinBox(central);
    m_polyOrderSpin->setRange(0, 10);
    m_polyOrderSpin->setValue(1);
    methodForm->addRow(m_polyOrderLabel, m_polyOrderSpin);

    auto *methodHint = new QLabel(
            u"Polynomial of the chosen order is fit to the line-free channels "
            u"and subtracted from the full spectrum. Median collapses the "
            u"line-free range to a single per-pixel level (equivalent to "
            u"poly_order = 0). Higher orders chase instrumental ripples."_s, central);
    methodHint->setStyleSheet(ToolDialogStyle::helperCss());
    methodHint->setWordWrap(true);
    methodForm->addRow(QString(), methodHint);

    // ── LINE-FREE CHANNELS: the input the user MUST give ─────────────────
    auto *channelsForm = ToolDialogStyle::addSection(mainLayout,
                                                     u"Line-free channels"_s, central);

    m_lineFreeEdit = new QLineEdit(central);
    m_lineFreeEdit->setPlaceholderText(u"e.g.  1-50, 200-300  (1-based, inclusive)"_s);
    channelsForm->addRow(u"Ranges"_s, m_lineFreeEdit);

    auto *channelsHint = new QLabel(
            u"<b>Required.</b> Enter spectral ranges that contain no signal. "
            u"The baseline is estimated from these channels only — fitting on "
            u"all channels will absorb real line emission."_s, central);
    channelsHint->setStyleSheet(ToolDialogStyle::helperCss());
    channelsHint->setWordWrap(true);
    channelsForm->addRow(QString(), channelsHint);

    // Disable poly-order for median mode.
    auto updatePolyOrder = [this]() {
        const bool isPoly = m_methodCombo->currentData().toString() == u"polynomial"_s;
        m_polyOrderLabel->setEnabled(isPoly);
        m_polyOrderSpin->setEnabled(isPoly);
    };
    updatePolyOrder();
    connect(m_methodCombo, &QComboBox::currentIndexChanged, this, updatePolyOrder);

    // ── METHODOLOGY footer ───────────────────────────────────────────────
    auto *divider = new QFrame(central);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(
            QStringLiteral("color: %1;")
                    .arg(QString::fromLatin1(VisivoTheme::kOutline())));
    mainLayout->addWidget(divider);

    auto *methodologyLabel = new QLabel(
            u"Fit runs per pixel · output is a new cube dataset registered in "
            u"the current session, openable in a fresh cube viewer."_s, central);
    methodologyLabel->setStyleSheet(ToolDialogStyle::helperCss());
    methodologyLabel->setWordWrap(true);
    mainLayout->addWidget(methodologyLabel);

    // ── Footer: Cancel + primary action, aligned right. The old
    // bottom-left breadcrumb pill is gone — the top breadcrumb header
    // (added by ToolDialogStyle::makeBreadcrumb) carries that info.
    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, central);
    m_computeBtn   = VisivoTheme::makePrimaryButton(u"Subtract Baseline"_s, central);
    m_computeBtn->setDefault(true);
    m_computeBtn->setCheckable(false);
    btnLayout->addWidget(closeBtn);
    btnLayout->addWidget(m_computeBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_computeBtn, &QPushButton::clicked, this, &BaselineDialog::onCompute);
    connect(closeBtn,     &QPushButton::clicked, this, &QMainWindow::close);
}

// ─────────────────────────────────────────────────────────────────────────────

void BaselineDialog::closeEvent(QCloseEvent *event)
{
    // A worker in flight completes on the thread pool and its finished() handler
    // discards the result (the QPointer to this dialog is null by then). That is
    // correct but used to be silent — see ToolDialogClose.h for what the user is
    // now told, and why "cancel" is not among the options.
    if (m_busy && !visivo::confirmAbandonRunningCompute(this, u"Baseline Subtraction"_s, u"The baseline subtraction"_s)) {
        event->ignore();
        return;
    }
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
            u"Enter at least one spectral range that contains no signal "
            u"(e.g. \"1-50, 200-300\"). The baseline is fitted on those channels "
            u"only and subtracted from the full spectrum.\n\n"
            u"Fitting on all channels would absorb real line emission and "
            u"produce an over-subtracted, nearly empty cube."_s);
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
                u"Method: %1\n"
                u"New dataset ID: %2\n"
                u"Output path: %3\n"
                u"RMS before: %4\n"
                u"RMS after:  %5\n"
                u"Fit channels: %6\n"
                u"Dimensions: %7 × %8 × %9 (W×H×D)"_s
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
