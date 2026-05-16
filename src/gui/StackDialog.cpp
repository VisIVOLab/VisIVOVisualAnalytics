#include "StackDialog.h"
#include "theme/VisivoTheme.h"

#include "BackendClient.h"

#include "qcustomplot.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────

StackDialog::StackDialog(BackendClient *client,
                         const QString &sessionId,
                         const QStringList &sessionDatasets,
                         QWidget *parent)
    : QDialog(parent)
    , m_client(client)
    , m_sessionId(sessionId)
{
    setWindowTitle(u"Stack spectral cubes"_s);
    setMinimumWidth(540);
    resize(540, 520);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    // ── Explanation ───────────────────────────────────────────────────────────
    auto *infoLabel = new QLabel(
            u"Select two or more cubes to stack. All cubes must share the same "
            "spectral grid and spatial shape."_s, this);
    infoLabel->setWordWrap(true);
    mainLayout->addWidget(infoLabel);

    // ── Checkable dataset list ────────────────────────────────────────────────
    mainLayout->addWidget(new QLabel(u"Datasets in session:"_s, this));
    m_datasetList = new QListWidget(this);
    m_datasetList->setSelectionMode(QAbstractItemView::NoSelection);
    for (const QString &id : sessionDatasets) {
        auto *item = new QListWidgetItem(id, m_datasetList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    mainLayout->addWidget(m_datasetList, 1);

    // ── Live selection counter ────────────────────────────────────────────────
    m_selectionLabel = new QLabel(u"Selected: 0 cubes"_s, this);
    mainLayout->addWidget(m_selectionLabel);

    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    mainLayout->addLayout(form);

    // ── Stacking method ────────────────────────────────────────────────────────
    m_methodCombo = new QComboBox(this);
    m_methodCombo->addItem(u"Mean"_s,          u"mean"_s);
    m_methodCombo->addItem(u"Median"_s,        u"median"_s);
    m_methodCombo->addItem(u"Weighted Mean"_s, u"weighted_mean"_s);
    form->addRow(u"Method"_s, m_methodCombo);

    // ── Weight-by ─────────────────────────────────────────────────────────────
    m_weightByCombo = new QComboBox(this);
    m_weightByCombo->addItem(u"Uniform"_s, u"uniform"_s);
    m_weightByCombo->addItem(u"RMS"_s,     u"rms"_s);
    m_weightByCombo->addItem(u"Peak"_s,    u"peak"_s);
    form->addRow(u"Weight by"_s, m_weightByCombo);

    // weight_by only matters for weighted_mean
    auto updateWeightBy = [this]() {
        m_weightByCombo->setEnabled(
                m_methodCombo->currentData().toString() == u"weighted_mean"_s);
    };
    updateWeightBy();
    connect(m_methodCombo, &QComboBox::currentIndexChanged, this, updateWeightBy);

    // ── Footer (Layout B tool-dlg pattern) ────────────────────────────────────
    auto *btnRow = new QHBoxLayout();
    btnRow->addWidget(VisivoTheme::makeBreadcrumbTag(u"Tools → Stack…"_s, this));
    btnRow->addStretch();

    auto *cancelBtn = VisivoTheme::makeSecondaryButton(u"Cancel"_s, this);
    m_stackBtn      = VisivoTheme::makePrimaryButton(u"Stack"_s, this);
    m_stackBtn->setEnabled(false);   // disabled until ≥ 2 checked
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(m_stackBtn);
    mainLayout->addLayout(btnRow);

    connect(m_datasetList, &QListWidget::itemChanged, this, &StackDialog::onItemChanged);
    connect(m_stackBtn,    &QPushButton::clicked,     this, &StackDialog::onCompute);
    connect(cancelBtn,     &QPushButton::clicked,     this, &QDialog::reject);
}

// ─────────────────────────────────────────────────────────────────────────────

void StackDialog::onItemChanged()
{
    updateSelectionLabel();
}

void StackDialog::updateSelectionLabel()
{
    int checked = 0;
    for (int i = 0; i < m_datasetList->count(); ++i) {
        if (m_datasetList->item(i)->checkState() == Qt::Checked) {
            ++checked;
        }
    }
    m_selectionLabel->setText(u"Selected: %1 cube%2"_s
                                       .arg(checked)
                                       .arg(checked == 1 ? u""_s : u"s"_s));

    // Red when too few; normal colour otherwise
    QPalette pal = m_selectionLabel->palette();
    pal.setColor(QPalette::WindowText,
                 checked < 2 ? QColor(Qt::red) : QColor());
    m_selectionLabel->setPalette(pal);

    m_stackBtn->setEnabled(checked >= 2);
}

// ─────────────────────────────────────────────────────────────────────────────

void StackDialog::onCompute()
{
    QStringList selected;
    for (int i = 0; i < m_datasetList->count(); ++i) {
        if (m_datasetList->item(i)->checkState() == Qt::Checked) {
            selected << m_datasetList->item(i)->text();
        }
    }
    if (selected.size() < 2) {
        QMessageBox::warning(this, u"Stack spectral cubes"_s,
                             u"Please check at least two datasets to stack."_s);
        return;
    }

    const QString method   = m_methodCombo->currentData().toString();
    const QString weightBy = m_weightByCombo->currentData().toString();

    setUiBusy(true);

    m_progress = new QProgressDialog(u"Stacking spectra…"_s, QString(), 0, 0, this);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(400);
    m_progress->show();

    BackendClient *client = m_client;
    client->setSessionId(m_sessionId);  // FIX 5

    auto *watcher = new QFutureWatcher<BackendStackResult>(this);
    QPointer<StackDialog> self(this);

    connect(watcher, &QFutureWatcher<BackendStackResult>::finished,
            watcher, [self, watcher, method]() {
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
            QMessageBox::critical(self, u"Stack spectral cubes"_s,
                                  result.error.isEmpty()
                                          ? u"Backend returned an error."_s
                                          : result.error);
            return;
        }

        // Interpret spectrum bytes as float32
        const QByteArray &data = result.spectrum.data;
        const int nPts = data.size() / static_cast<int>(sizeof(float));
        if (nPts <= 0) {
            QMessageBox::warning(self, u"Stack spectral cubes"_s,
                                 u"The server returned an empty spectrum."_s);
            return;
        }
        const float *ptr = reinterpret_cast<const float *>(data.constData());

        // ── Show result in a QCustomPlot window ───────────────────────────────
        auto *plotWin = new QMainWindow(/* parent */ nullptr);
        plotWin->setAttribute(Qt::WA_DeleteOnClose);
        plotWin->setWindowTitle(u"Stacked Spectrum"_s);
        plotWin->resize(800, 400);

        auto *plot = new QCustomPlot(plotWin);
        plotWin->setCentralWidget(plot);

        QVector<double> xData(nPts), yData(nPts);
        for (int i = 0; i < nPts; ++i) {
            xData[i] = static_cast<double>(i);
            yData[i] = static_cast<double>(ptr[i]);
        }

        plot->addGraph();
        plot->graph(0)->setData(xData, yData);
        plot->graph(0)->setName(
                u"Stacked (%1, N=%2)"_s.arg(method).arg(result.nCubes));
        plot->xAxis->setLabel(u"Channel"_s);
        plot->yAxis->setLabel(u"Flux"_s);
        plot->legend->setVisible(true);
        plot->rescaleAxes();
        plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

        plotWin->show();

        if (self) {
            self->accept();
        }
    });

    watcher->setFuture(QtConcurrent::run(
            [client, selected, method, weightBy]() {
                return client->fetchStackBinary(selected, method, {}, weightBy);
            }));
}

void StackDialog::setUiBusy(bool busy)
{
    m_datasetList->setEnabled(!busy);
    m_methodCombo->setEnabled(!busy);
    m_weightByCombo->setEnabled(!busy && m_methodCombo->currentData().toString() == u"weighted_mean"_s);
    // Disable the primary action while a compute is running. The cancel
    // button stays enabled so the user can still abort the dialog.
    if (busy) {
        if (m_stackBtn) m_stackBtn->setEnabled(false);
    } else {
        // Recompute "≥ 2 checked" enabled state via the existing handler.
        onItemChanged();
    }
}
