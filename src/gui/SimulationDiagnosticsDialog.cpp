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
#include <QPushButton>
#include <QSpinBox>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Qt::StringLiterals;

SimulationDiagnosticsDialog::SimulationDiagnosticsDialog(
        const QString &backendUrl, const QString &backendToken, const QString &sessionId,
        const QString &datasetId, const QStringList &numericFields, const QJsonObject &units,
        const std::vector<BackendTabularFilter> &filters, QWidget *parent)
    : QDialog(parent), m_backendUrl(backendUrl), m_backendToken(backendToken),
      m_sessionId(sessionId), m_datasetId(datasetId), m_units(units), m_filters(filters)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(u"Selection diagnostics"_s);
    this->resize(780, 620);
    auto *outer = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_field = new QComboBox(this);
    m_field->addItems(numericFields);
    m_yField = new QComboBox(this);
    m_yField->addItem(u"None (1-D histogram)"_s, QString());
    for (const QString &field : numericFields)
        m_yField->addItem(field, field);
    m_weight = new QComboBox(this);
    m_weight->addItem(u"None (particle count)"_s, QString());
    for (const QString &field : numericFields)
        m_weight->addItem(field, field);
    m_bins = new QSpinBox(this);
    m_bins->setRange(4, 512);
    m_bins->setValue(64);
    form->addRow(u"Quantity"_s, m_field);
    form->addRow(u"Y quantity"_s, m_yField);
    form->addRow(u"Weight"_s, m_weight);
    form->addRow(u"Bins"_s, m_bins);
    outer->addLayout(form);

    const QString population = filters.empty()
            ? u"Population: complete dataset"_s
            : u"Population: %1 active selection predicate(s), evaluated on the full dataset"_s
                      .arg(filters.size());
    auto *populationLabel = new QLabel(population, this);
    populationLabel->setWordWrap(true);
    populationLabel->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    outer->addWidget(populationLabel);

    m_run = new QPushButton(u"Compute exact diagnostics"_s, this);
    QObject::connect(m_run, &QPushButton::clicked, this, &SimulationDiagnosticsDialog::run);
    outer->addWidget(m_run);
    m_summary = new QLabel(u"Choose a quantity and compute."_s, this);
    m_summary->setWordWrap(true);
    outer->addWidget(m_summary);
    m_plot = new QCustomPlot(this);
    m_plot->setMinimumHeight(300);
    m_colorScale = new QCPColorScale(m_plot);
    m_colorScale->setType(QCPAxis::atRight);
    m_colorScale->setVisible(false);
    m_plot->plotLayout()->addElement(0, 1, m_colorScale);
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
    QObject::connect(&m_watcher, &QFutureWatcherBase::finished, this, [this]() {
        m_run->setEnabled(true);
        this->present(m_watcher.result());
    });
}

void SimulationDiagnosticsDialog::run()
{
    if (m_field->currentText().isEmpty())
        return;
    BackendCatalogueDiagnosticsRequest request;
    request.datasetId = m_datasetId;
    request.field = m_field->currentText();
    request.yField = m_yField->currentData().toString();
    request.weightField = m_weight->currentData().toString();
    request.bins = m_bins->value();
    request.yBins = m_bins->value();
    request.filters = m_filters;
    m_run->setEnabled(false);
    m_summary->setText(u"Computing over the complete selected population…"_s);
    m_watcher.setFuture(QtConcurrent::run(
            [url = m_backendUrl, token = m_backendToken, session = m_sessionId, request]() {
                BackendClient client(url, token);
                client.setSessionId(session);
                return client.catalogueDiagnostics(request);
            }));
}

void SimulationDiagnosticsDialog::present(const BackendCatalogueDiagnosticsResponse &result)
{
    if (!result.valid) {
        m_summary->setText(u"Diagnostics failed: %1"_s.arg(result.error));
        return;
    }
    const bool validHistogram = result.yField.isEmpty()
            ? result.binEdges.size() == result.counts.size() + 1
            : (result.binEdges.size() > 1 && result.yBinEdges.size() > 1
               && result.counts.size()
                       == (result.binEdges.size() - 1) * (result.yBinEdges.size() - 1));
    if (!validHistogram) {
        m_summary->setText(u"Diagnostics failed: the backend returned inconsistent bins."_s);
        return;
    }
    m_result = result;
    QString text = u"%L1 selected rows; %L2 finite. %3 [%4]: min %5, max %6, mean %7, σ %8. %9."_s
            .arg(result.selectedRows).arg(result.finiteRows).arg(result.field)
            .arg(result.fieldUnit).arg(result.minimum, 0, 'g', 6)
            .arg(result.maximum, 0, 'g', 6).arg(result.mean, 0, 'g', 6)
            .arg(result.stddev, 0, 'g', 6).arg(result.weighting);
    if (!result.yField.isEmpty()) {
        text += u" %1 [%2]: min %3, max %4, mean %5, σ %6."_s
                        .arg(result.yField, result.yFieldUnit)
                        .arg(result.yMinimum.value_or(0.0), 0, 'g', 6)
                        .arg(result.yMaximum.value_or(0.0), 0, 'g', 6)
                        .arg(result.yMean.value_or(0.0), 0, 'g', 6)
                        .arg(result.yStddev.value_or(0.0), 0, 'g', 6);
    }
    if (result.weightedMean)
        text += u" Weighted mean %1; total weight %2 [%3]."_s
                        .arg(*result.weightedMean, 0, 'g', 6)
                        .arg(result.weightSum.value_or(0.0), 0, 'g', 6)
                        .arg(result.weightUnit);
    text += u" Selection: %1."_s.arg(result.selection.join(u" AND "_s));
    m_summary->setText(text);
    m_plot->clearPlottables();
    if (result.yField.isEmpty()) {
        m_colorScale->setVisible(false);
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
        m_plot->yAxis->setLabel(result.weightField.isEmpty()
                ? u"Particle count"_s
                : u"Sum of %1 [%2]"_s.arg(result.weightField, result.weightUnit));
    } else {
        const int nx = result.binEdges.size() - 1;
        const int ny = result.yBinEdges.size() - 1;
        auto *map = new QCPColorMap(m_plot->xAxis, m_plot->yAxis);
        map->data()->setSize(nx, ny);
        map->data()->setRange(
                QCPRange(0.5 * (result.binEdges[0] + result.binEdges[1]),
                         0.5 * (result.binEdges[nx - 1] + result.binEdges[nx])),
                QCPRange(0.5 * (result.yBinEdges[0] + result.yBinEdges[1]),
                         0.5 * (result.yBinEdges[ny - 1] + result.yBinEdges[ny])));
        for (int ix = 0; ix < nx; ++ix)
            for (int iy = 0; iy < ny; ++iy)
                map->data()->setCell(ix, iy, result.counts[ix * ny + iy]);
        map->setGradient(QCPColorGradient(QCPColorGradient::gpThermal));
        map->setColorScale(m_colorScale);
        map->rescaleDataRange();
        m_colorScale->setLabel(result.weightField.isEmpty()
                ? u"Particle count"_s
                : u"Sum of %1 [%2]"_s.arg(result.weightField, result.weightUnit));
        m_colorScale->setVisible(true);
        m_plot->yAxis->setLabel(u"%1 [%2]"_s.arg(result.yField, result.yFieldUnit));
    }
    m_plot->xAxis->setLabel(u"%1 [%2]"_s.arg(result.field, result.fieldUnit));
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
    if (m_result.yField.isEmpty()) {
        out << "bin_low,bin_high,count\n";
        for (int i = 0; i < m_result.counts.size(); ++i)
            out << QString::number(m_result.binEdges[i], 'g', 17) << ','
                << QString::number(m_result.binEdges[i + 1], 'g', 17) << ','
                << QString::number(m_result.counts[i], 'g', 17) << '\n';
    } else {
        out << "# y_field," << m_result.yField << '\n';
        out << "# y_field_unit," << m_result.yFieldUnit << '\n';
        out << "x_low,x_high,y_low,y_high,count\n";
        const int nx = m_result.binEdges.size() - 1;
        const int ny = m_result.yBinEdges.size() - 1;
        for (int ix = 0; ix < nx; ++ix)
            for (int iy = 0; iy < ny; ++iy)
                out << QString::number(m_result.binEdges[ix], 'g', 17) << ','
                    << QString::number(m_result.binEdges[ix + 1], 'g', 17) << ','
                    << QString::number(m_result.yBinEdges[iy], 'g', 17) << ','
                    << QString::number(m_result.yBinEdges[iy + 1], 'g', 17) << ','
                    << QString::number(m_result.counts[ix * ny + iy], 'g', 17) << '\n';
    }
}
