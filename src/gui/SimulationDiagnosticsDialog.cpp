#include "SimulationDiagnosticsDialog.h"

#include "qcustomplot.h"
#include "theme/VisivoTheme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QFutureWatcher>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Qt::StringLiterals;

SimulationDiagnosticsDialog::SimulationDiagnosticsDialog(
        const QString &backendUrl, const QString &backendToken, const QString &sessionId,
        const QString &datasetId, const QStringList &numericFields,
        const std::vector<BackendTabularFilter> &filters, QWidget *parent)
    : QDialog(parent), m_backendUrl(backendUrl), m_backendToken(backendToken),
      m_sessionId(sessionId), m_datasetId(datasetId), m_filters(filters)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(u"Selection diagnostics"_s);
    this->resize(780, 620);
    auto *outer = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_field = new QComboBox(this);
    m_field->addItems(numericFields);
    m_weight = new QComboBox(this);
    m_weight->addItem(u"None (particle count)"_s, QString());
    for (const QString &field : numericFields)
        m_weight->addItem(field, field);
    m_bins = new QSpinBox(this);
    m_bins->setRange(4, 512);
    m_bins->setValue(64);
    form->addRow(u"Quantity"_s, m_field);
    form->addRow(u"Weight"_s, m_weight);
    form->addRow(u"Bins"_s, m_bins);
    outer->addLayout(form);

    m_population = new QLabel(this);
    m_population->setWordWrap(true);
    m_population->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    outer->addWidget(m_population);
    this->updatePopulation();

    auto *runRow = new QHBoxLayout;
    m_run = new QPushButton(u"Compute exact diagnostics"_s, this);
    QObject::connect(m_run, &QPushButton::clicked, this, &SimulationDiagnosticsDialog::run);
    runRow->addWidget(m_run);
    // Two passes over every particle of a large snapshot take minutes: the
    // bar says it is still going, Cancel stops waiting for it.
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setVisible(false);
    runRow->addWidget(m_progress, 1);
    m_cancel = new QPushButton(u"Cancel"_s, this);
    m_cancel->setVisible(false);
    QObject::connect(m_cancel, &QPushButton::clicked, this, &SimulationDiagnosticsDialog::cancel);
    runRow->addWidget(m_cancel);
    outer->addLayout(runRow);
    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    QObject::connect(m_tick, &QTimer::timeout, this, [this]() {
        m_summary->setText(u"Computing over the complete selected population… %1 s"_s
                                   .arg(m_elapsed.elapsed() / 1000));
    });

    m_summary = new QLabel(u"Choose a quantity and compute."_s, this);
    m_summary->setWordWrap(true);
    outer->addWidget(m_summary);
    m_plot = new QCustomPlot(this);
    m_plot->setMinimumHeight(300);
    m_plot->xAxis->setLabel(u"Quantity"_s);
    m_plot->yAxis->setLabel(u"Count"_s);
    outer->addWidget(m_plot, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_saveCsv = buttons->addButton(u"Save CSV…"_s, QDialogButtonBox::ActionRole);
    m_savePng = buttons->addButton(u"Save PNG…"_s, QDialogButtonBox::ActionRole);
    m_saveCsv->setEnabled(false);
    m_savePng->setEnabled(false);
    QObject::connect(m_saveCsv, &QPushButton::clicked, this, &SimulationDiagnosticsDialog::saveCsv);
    QObject::connect(m_savePng, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(
                this, u"Save diagnostics plot"_s, u"selection-diagnostics.png"_s,
                u"PNG image (*.png)"_s);
        if (!path.isEmpty())
            m_plot->savePng(path, 1600, 1000, 1.0, -1);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);
}

void SimulationDiagnosticsDialog::updatePopulation()
{
    if (m_filters.empty()) {
        m_population->setText(u"Population: complete dataset"_s);
        return;
    }
    QStringList regions;
    for (const BackendTabularFilter &filter : m_filters)
        if (filter.isSpatial() && !filter.name.trimmed().isEmpty())
            regions << filter.name.trimmed();
    QString text = u"Population: %1 applied selection predicate(s), evaluated on the full "
                   u"dataset"_s.arg(m_filters.size());
    if (!regions.isEmpty())
        text += u" (regions: %1)"_s.arg(regions.join(u", "_s));
    m_population->setText(text);
}

void SimulationDiagnosticsDialog::setFilters(const std::vector<BackendTabularFilter> &filters)
{
    m_filters = filters;
    this->updatePopulation();
    // A histogram of the old population next to the new population line
    // would be read as the new one's: computed again, or cleared.
    if (m_running || m_hasResult)
        this->run();
}

void SimulationDiagnosticsDialog::run()
{
    if (m_field->currentText().isEmpty())
        return;
    BackendCatalogueDiagnosticsRequest request;
    request.datasetId = m_datasetId;
    request.field = m_field->currentText();
    request.weightField = m_weight->currentData().toString();
    request.bins = m_bins->value();
    request.filters = m_filters;
    const quint64 runId = ++m_runId;
    m_running = true;
    m_run->setEnabled(false);
    m_progress->setVisible(true);
    m_cancel->setVisible(true);
    m_elapsed.start();
    m_tick->start();
    m_summary->setText(u"Computing over the complete selected population…"_s);
    // Its own watcher per run: one abandoned by Cancel may still be waiting
    // for the backend while the next is out.
    auto *watcher = new QFutureWatcher<BackendCatalogueDiagnosticsResponse>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, runId]() {
        watcher->deleteLater();
        if (runId != m_runId)
            return;   // cancelled, or run again with other settings meanwhile
        this->finishRun();
        this->present(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(
            [url = m_backendUrl, token = m_backendToken, session = m_sessionId, request]() {
                BackendClient client(url, token);
                client.setSessionId(session);
                return client.catalogueDiagnostics(request);
            }));
}

void SimulationDiagnosticsDialog::finishRun()
{
    m_running = false;
    m_tick->stop();
    m_progress->setVisible(false);
    m_cancel->setVisible(false);
    m_run->setEnabled(true);
}

void SimulationDiagnosticsDialog::cancel()
{
    if (!m_running)
        return;
    ++m_runId;   // its answer, when it comes, is dropped
    this->finishRun();
    m_summary->setText(u"Cancelled. The backend may finish the pass it started; its "
                       u"answer is ignored."_s);
}

void SimulationDiagnosticsDialog::present(const BackendCatalogueDiagnosticsResponse &result)
{
    if (!result.valid) {
        m_summary->setText(u"Diagnostics failed: %1"_s.arg(result.error));
        // What is plotted is not this population's.
        m_hasResult = false;
        m_plot->clearPlottables();
        m_plot->replot();
        m_saveCsv->setEnabled(false);
        m_savePng->setEnabled(false);
        return;
    }
    m_result = result;
    m_hasResult = true;
    QString text = u"%L1 selected rows; %L2 finite. %3 [%4]: min %5, max %6, mean %7, σ %8. %9."_s
            .arg(result.selectedRows).arg(result.finiteRows).arg(result.field)
            .arg(result.fieldUnit).arg(result.minimum, 0, 'g', 6)
            .arg(result.maximum, 0, 'g', 6).arg(result.mean, 0, 'g', 6)
            .arg(result.stddev, 0, 'g', 6).arg(result.weighting);
    if (result.weightedMean)
        text += u" Weighted mean %1; total weight %2 [%3]."_s
                        .arg(*result.weightedMean, 0, 'g', 6)
                        .arg(result.weightSum.value_or(0.0), 0, 'g', 6)
                        .arg(result.weightUnit);
    text += u" Selection: %1."_s.arg(result.selection.join(u" AND "_s));
    m_summary->setText(text);
    m_plot->clearPlottables();
    auto *bars = new QCPBars(m_plot->xAxis, m_plot->yAxis);
    QVector<double> centres, widths;
    centres.reserve(result.counts.size());
    widths.reserve(result.counts.size());
    for (int i = 0; i < result.counts.size(); ++i) {
        centres.append(0.5 * (result.binEdges[i] + result.binEdges[i + 1]));
        widths.append(result.binEdges[i + 1] - result.binEdges[i]);
    }
    bars->setData(centres, result.counts);
    if (!widths.isEmpty())
        bars->setWidth(widths.front());
    bars->setPen(QPen(QColor(QString::fromLatin1(VisivoTheme::kPrimary()))));
    bars->setBrush(QColor(QString::fromLatin1(VisivoTheme::kPrimary())).lighter(125));
    m_plot->xAxis->setLabel(u"%1 [%2]"_s.arg(result.field, result.fieldUnit));
    m_plot->yAxis->setLabel(result.weightField.isEmpty()
                                   ? u"Particle count"_s
                                   : u"Sum of %1 [%2]"_s.arg(result.weightField, result.weightUnit));
    m_plot->rescaleAxes();
    m_plot->replot();
    m_saveCsv->setEnabled(true);
    m_savePng->setEnabled(true);
}

void SimulationDiagnosticsDialog::saveCsv()
{
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save diagnostics table"_s, u"selection-diagnostics.csv"_s,
            u"CSV table (*.csv)"_s);
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, u"Save diagnostics"_s, file.errorString());
        return;
    }
    QTextStream out(&file);
    out << "# field," << m_result.field << '\n';
    out << "# field_unit," << m_result.fieldUnit << '\n';
    out << "# weighting," << m_result.weighting << '\n';
    out << "# selection," << m_result.selection.join(" AND ") << '\n';
    out << "bin_low,bin_high,count\n";
    for (int i = 0; i < m_result.counts.size(); ++i)
        out << QString::number(m_result.binEdges[i], 'g', 17) << ','
            << QString::number(m_result.binEdges[i + 1], 'g', 17) << ','
            << QString::number(m_result.counts[i], 'g', 17) << '\n';
}
