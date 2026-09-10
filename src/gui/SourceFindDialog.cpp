#include "SourceFindDialog.h"

#include "BackendClient.h"
#include "theme/ToolDialogStyle.h"
#include "theme/VisivoTheme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include "ToolDialogClose.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────

SourceFindDialog::SourceFindDialog(std::shared_ptr<BackendClient> client,
                                   const QString &datasetId,
                                   QWidget *parent)
    : QDialog(parent)
    , m_client(std::move(client))
    , m_datasetId(datasetId)
{
    setWindowTitle(u"SoFiA-2 Source Finding"_s);
    setMinimumWidth(700);
    resize(800, 600);
    // Shared input theme — see ToolDialogStyle.
    setStyleSheet(ToolDialogStyle::inputCss());

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);
    mainLayout->addWidget(ToolDialogStyle::makeBreadcrumb(this, u"Source Finding"_s));

    // ── Parameters form ───────────────────────────────────────────────────────
    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    mainLayout->addLayout(form);

    m_thresholdSpin = new QDoubleSpinBox(this);
    m_thresholdSpin->setRange(0.1, 100.0);
    m_thresholdSpin->setDecimals(2);
    m_thresholdSpin->setValue(5.0);
    m_thresholdSpin->setSuffix(u" σ"_s);
    form->addRow(u"S+C threshold"_s, m_thresholdSpin);

    m_kernelXSpin = new QSpinBox(this);
    m_kernelXSpin->setRange(0, 31);
    m_kernelXSpin->setSingleStep(2);
    m_kernelXSpin->setValue(3);
    form->addRow(u"Kernel X (pix)"_s, m_kernelXSpin);

    m_kernelYSpin = new QSpinBox(this);
    m_kernelYSpin->setRange(0, 31);
    m_kernelYSpin->setSingleStep(2);
    m_kernelYSpin->setValue(3);
    form->addRow(u"Kernel Y (pix)"_s, m_kernelYSpin);

    m_kernelZSpin = new QSpinBox(this);
    m_kernelZSpin->setRange(0, 31);
    m_kernelZSpin->setSingleStep(2);
    m_kernelZSpin->setValue(3);
    form->addRow(u"Kernel Z (chan)"_s, m_kernelZSpin);

    m_mergeCombo = new QComboBox(this);
    m_mergeCombo->addItem(u"True"_s,  u"true"_s);
    m_mergeCombo->addItem(u"False"_s, u"false"_s);
    form->addRow(u"Merge sources"_s, m_mergeCombo);

    // ── Results table ─────────────────────────────────────────────────────────
    mainLayout->addWidget(new QLabel(u"Detected sources:"_s, this));
    m_table = new QTableWidget(0, 0, this);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mainLayout->addWidget(m_table, 1);

    // ── Footer: Close (secondary) + Run (primary), right-aligned. The
    // old bottom-left breadcrumb pill is gone — the top breadcrumb
    // header (ToolDialogStyle::makeBreadcrumb) carries that info now.
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, this);
    m_openMaskBtn  = VisivoTheme::makeSecondaryButton(u"Open mask in 3D"_s, this);
    m_openMaskBtn->setEnabled(false);
    m_openMaskBtn->setToolTip(
            u"Open the SoFiA-2 binary mask cube in the 3D viewer to inspect "
            u"the detections in 3D. Run SoFiA-2 first."_s);
    m_runBtn       = VisivoTheme::makePrimaryButton(u"Run SoFiA-2"_s, this);
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(m_openMaskBtn);
    btnRow->addWidget(m_runBtn);
    mainLayout->addLayout(btnRow);

    connect(m_runBtn,  &QPushButton::clicked, this, &SourceFindDialog::onRun);
    connect(closeBtn,  &QPushButton::clicked, this, &QDialog::reject);
    connect(m_openMaskBtn, &QPushButton::clicked, this, [this]() {
        if (!m_maskPath.isEmpty())
            emit openMaskCubeRequested(m_maskPath);
    });
}

// ─────────────────────────────────────────────────────────────────────────────

void SourceFindDialog::onRun()
{
    const QVariantMap params{
        { u"scfind.threshold"_s,          m_thresholdSpin->value() },
        { u"scfind.kernelXY"_s,           m_kernelXSpin->value()   },
        { u"scfind.kernelZ"_s,            m_kernelZSpin->value()   },
        { u"merge.enable"_s,              m_mergeCombo->currentData().toString() },
    };

    const QString datasetId = m_datasetId;
    setUiBusy(true);

    m_progress = new QProgressDialog(u"Running SoFiA-2 source finder…"_s,
                                     QString(), 0, 0, this);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(400);
    m_progress->show();

    std::shared_ptr<BackendClient> client = m_client;   // shared with the worker

    auto *watcher = new QFutureWatcher<BackendSourceFindResult>(this);
    QPointer<SourceFindDialog> self(this);

    connect(watcher, &QFutureWatcher<BackendSourceFindResult>::finished,
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
            // 503 → sofia2 not found; give a helpful message
            const bool sofia2Missing =
                    result.error.contains(u"sofia2"_s, Qt::CaseInsensitive) ||
                    result.error.contains(u"503"_s)                         ||
                    result.error.contains(u"not found"_s, Qt::CaseInsensitive);
            if (sofia2Missing) {
                QMessageBox::critical(
                        self, u"SoFiA-2 Not Available"_s,
                        u"SoFiA-2 (sofia2) was not found on the server's PATH.\n\n"
                        u"Install SoFiA-2 and ensure it is accessible to the "
                        u"VisIVO backend process, then retry."_s);
            } else {
                QMessageBox::critical(self, u"Source Finding"_s,
                                      result.error.isEmpty()
                                              ? u"Backend returned an error."_s
                                              : result.error);
            }
            return;
        }

        // Offer the 3D mask overlay when SoFiA wrote a mask cube.
        self->m_maskPath = result.maskPath;
        if (self->m_openMaskBtn)
            self->m_openMaskBtn->setEnabled(!result.maskPath.isEmpty());

        if (result.nSources == 0) {
            QMessageBox::information(self, u"Source Finding"_s,
                                     u"SoFiA-2 found no sources with the current parameters."_s);
            return;
        }

        // Collect column names from all sources
        QStringList columns;
        for (const auto &src : result.sources) {
            for (const QString &key : src.fields.keys()) {
                if (!columns.contains(key)) {
                    columns << key;
                }
            }
        }
        columns.sort();

        QVector<QVariantMap> rows;
        rows.reserve(result.sources.size());
        for (const auto &src : result.sources) {
            rows << src.fields;
        }
        self->populateTable(rows);   // columns now derived inside populateTable
        // Re-populate with sorted columns
        self->m_table->setColumnCount(columns.size());
        self->m_table->setHorizontalHeaderLabels(columns);
        self->m_table->setRowCount(result.sources.size());
        for (int r = 0; r < result.sources.size(); ++r) {
            const QVariantMap &fields = result.sources[r].fields;
            for (int c = 0; c < columns.size(); ++c) {
                const QVariant val = fields.value(columns[c]);
                auto *item = new QTableWidgetItem(val.toString());
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                self->m_table->setItem(r, c, item);
            }
        }
        self->m_table->resizeColumnsToContents();
    });

    watcher->setFuture(QtConcurrent::run(
            [client, datasetId, params]() {
                return client->runSourceFind(datasetId, params);
            }));
}

// ─────────────────────────────────────────────────────────────────────────────

void SourceFindDialog::populateTable(const QVector<QVariantMap> &rows)
{
    if (rows.isEmpty()) {
        m_table->setRowCount(0);
        return;
    }
    QStringList columns;
    for (const QVariantMap &row : rows) {
        for (const QString &key : row.keys()) {
            if (!columns.contains(key)) {
                columns << key;
            }
        }
    }
    columns.sort();
    m_table->setColumnCount(columns.size());
    m_table->setHorizontalHeaderLabels(columns);
    m_table->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        for (int c = 0; c < columns.size(); ++c) {
            const QVariant val = rows[r].value(columns[c]);
            auto *item = new QTableWidgetItem(val.toString());
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_table->setItem(r, c, item);
        }
    }
    m_table->resizeColumnsToContents();
}

void SourceFindDialog::closeEvent(QCloseEvent *event)
{
    // See ToolDialogClose.h: a compute in flight cannot be cancelled, so the
    // choice is between waiting and discarding the result — said out loud.
    if (m_busy && !visivo::confirmAbandonRunningCompute(this, u"Source Finder"_s, u"The SoFiA-2 source finder"_s)) {
        event->ignore();
        return;
    }
    event->accept();
}

void SourceFindDialog::setUiBusy(bool busy)
{
    m_busy = busy;
    m_thresholdSpin->setEnabled(!busy);
    m_kernelXSpin->setEnabled(!busy);
    m_kernelYSpin->setEnabled(!busy);
    m_kernelZSpin->setEnabled(!busy);
    m_mergeCombo->setEnabled(!busy);
    m_runBtn->setEnabled(!busy);
}
