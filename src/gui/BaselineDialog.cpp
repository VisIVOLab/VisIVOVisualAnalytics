#include "BaselineDialog.h"

#include "BackendClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────

BaselineDialog::BaselineDialog(BackendClient *client,
                               const QString &sessionId,
                               const QString &datasetId,
                               int depth,
                               QWidget *parent)
    : QDialog(parent)
    , m_client(client)
    , m_sessionId(sessionId)
    , m_datasetId(datasetId)
    , m_depth(std::max(1, depth))
{
    setWindowTitle(u"Spectral Baseline Subtraction"_s);
    setMinimumWidth(520);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    mainLayout->addLayout(form);

    // ── Method ───────────────────────────────────────────────────────────────
    m_methodCombo = new QComboBox(this);
    m_methodCombo->addItem(u"Polynomial"_s, u"polynomial"_s);
    m_methodCombo->addItem(u"Median"_s,     u"median"_s);
    form->addRow(u"Method"_s, m_methodCombo);

    // ── Polynomial order ──────────────────────────────────────────────────────
    m_polyOrderLabel = new QLabel(u"Polynomial order"_s, this);
    m_polyOrderSpin  = new QSpinBox(this);
    m_polyOrderSpin->setRange(0, 10);
    m_polyOrderSpin->setValue(1);
    form->addRow(m_polyOrderLabel, m_polyOrderSpin);

    // ── Line-free channel ranges ──────────────────────────────────────────────
    m_lineFreeEdit = new QLineEdit(this);
    m_lineFreeEdit->setPlaceholderText(u"e.g.  1-50, 200-300  (1-based, inclusive)"_s);
    m_lineFreeHint = new QLabel(
            u"<i>Leave blank to use the entire spectral axis for the fit.</i>"_s, this);
    m_lineFreeHint->setWordWrap(true);
    form->addRow(u"Line-free ranges"_s, m_lineFreeEdit);
    form->addRow(QString(), m_lineFreeHint);

    // Disable poly-order for median mode
    auto updatePolyOrder = [this]() {
        const bool isPoly = m_methodCombo->currentData().toString() == u"polynomial"_s;
        m_polyOrderLabel->setEnabled(isPoly);
        m_polyOrderSpin->setEnabled(isPoly);
    };
    updatePolyOrder();
    connect(m_methodCombo, &QComboBox::currentIndexChanged, this, updatePolyOrder);

    // ── Buttons ───────────────────────────────────────────────────────────────
    m_buttons = new QDialogButtonBox(this);
    auto *computeBtn = m_buttons->addButton(u"Subtract Baseline"_s, QDialogButtonBox::AcceptRole);
    m_buttons->addButton(QDialogButtonBox::Cancel);
    mainLayout->addWidget(m_buttons);

    connect(computeBtn, &QPushButton::clicked,      this, &BaselineDialog::onCompute);
    connect(m_buttons,  &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ─────────────────────────────────────────────────────────────────────────────

void BaselineDialog::onCompute()
{
    const QString method   = m_methodCombo->currentData().toString();
    const int     polyOrder = m_polyOrderSpin->value();
    const QString rangeText = m_lineFreeEdit->text().trimmed();

    QString parseError;
    QVector<QPair<int,int>> lineFreeRanges;
    if (!rangeText.isEmpty()) {
        lineFreeRanges = parseLineFreeRanges(rangeText, m_depth, parseError);
        if (!parseError.isEmpty()) {
            QMessageBox::warning(this, u"Baseline Subtraction"_s, parseError);
            return;
        }
    }

    const QString datasetId = m_datasetId;
    setUiBusy(true);

    m_progress = new QProgressDialog(u"Subtracting baseline…"_s,
                                     QString(), 0, 0, this);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(400);
    m_progress->show();

    BackendClient *client = m_client;
    client->setSessionId(m_sessionId);  // FIX 5: ensure session ID is set before request

    auto *watcher = new QFutureWatcher<BackendBaselineResult>(this);
    QPointer<BaselineDialog> self(this);

    connect(watcher, &QFutureWatcher<BackendBaselineResult>::finished,
            watcher, [self, watcher]() {
        const auto result = watcher->result();
        watcher->deleteLater();

        if (!self) {
            return;
        }

        if (self->m_progress) {
            self->m_progress->close();
            self->m_progress->deleteLater();
        }
        self->setUiBusy(false);

        if (!result.valid) {
            QMessageBox::critical(self, u"Baseline Subtraction"_s,
                                  result.error.isEmpty()
                                          ? u"Backend returned an error."_s
                                          : result.error);
            return;
        }

        const QString summary =
                u"Baseline subtraction complete.\n\n"
                "Method: %1\n"
                "New dataset ID: %2\n"
                "Output path: %3\n"
                "RMS before: %4\n"
                "RMS after:  %5\n"
                "Dimensions: %6 × %7 × %8 (W×H×D)"_s
                        .arg(result.method.isEmpty() ? u"n/a"_s : result.method)
                        .arg(result.newDatasetId)
                        .arg(result.outputPath.isEmpty() ? u"n/a"_s : result.outputPath)
                        .arg(result.rmsBefore, 0, 'g', 6)
                        .arg(result.rmsAfter,  0, 'g', 6)
                        .arg(result.width).arg(result.height).arg(result.depth);

        QMessageBox::information(self, u"Baseline Subtraction"_s, summary);
        self->accept();
    });

    watcher->setFuture(QtConcurrent::run(
            [client, datasetId, polyOrder, method, lineFreeRanges]() {
                // Channel range: full cube (the server uses lineFreeRanges for fitting)
                return client->requestBaseline(datasetId,
                                               0, 0,        // channelStart/End unused when lineFreeRanges provided
                                               polyOrder,
                                               method,
                                               lineFreeRanges);
            }));
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helper – parse "1-50, 200-300" into 0-based pairs
// ─────────────────────────────────────────────────────────────────────────────

QVector<QPair<int,int>> BaselineDialog::parseLineFreeRanges(const QString &text,
                                                             int depth,
                                                             QString &errorOut)
{
    QVector<QPair<int,int>> result;
    // Split by comma or semicolon
    const QStringList parts = text.split(QRegularExpression(u"[,;]+"_s), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        // Accept "N-M" or "N..M" or "N:M"
        const QRegularExpression re(u"^(\\d+)\\s*[-..:]+(\\s*(\\d+))?$"_s);
        const auto match = re.match(trimmed);
        if (!match.hasMatch()) {
            // Try single channel "N"
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

void BaselineDialog::setUiBusy(bool busy)
{
    m_methodCombo->setEnabled(!busy);
    m_polyOrderSpin->setEnabled(!busy && m_methodCombo->currentData().toString() == u"polynomial"_s);
    m_lineFreeEdit->setEnabled(!busy);
    m_buttons->setEnabled(!busy);
}
