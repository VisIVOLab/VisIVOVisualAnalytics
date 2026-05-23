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
    : StackDialog(client, sessionId,
                  [&sessionDatasets]() {
                      std::vector<DatasetEntry> v;
                      v.reserve(static_cast<std::size_t>(sessionDatasets.size()));
                      for (const QString &id : sessionDatasets) {
                          v.push_back({ id, QStringLiteral("cube"), id, 0, 0, 0 });
                      }
                      return v;
                  }(),
                  /*currentId=*/QString(), parent)
{
}

StackDialog::StackDialog(BackendClient *client,
                         const QString &sessionId,
                         const std::vector<DatasetEntry> &datasets,
                         const QString &currentId,
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
            u"spectral grid and spatial shape — cubes with a different shape "
            u"than the current one are listed as disabled."_s, this);
    infoLabel->setWordWrap(true);
    mainLayout->addWidget(infoLabel);

    // ── Checkable dataset list ────────────────────────────────────────────────
    mainLayout->addWidget(new QLabel(u"Cubes in session:"_s, this));
    m_datasetList = new QListWidget(this);
    m_datasetList->setSelectionMode(QAbstractItemView::NoSelection);

    // Resolve the reference shape from currentId (when known) — used to grey
    // out cubes that cannot be stacked together. If currentId is empty or its
    // shape is zero we fall back to the first cube with a non-zero shape.
    int refW = 0, refH = 0, refD = 0;
    auto adoptShape = [&](const DatasetEntry &e) {
        if (!refW && !refH && !refD && e.width > 0 && e.height > 0 && e.depth > 0) {
            refW = e.width; refH = e.height; refD = e.depth;
        }
    };
    for (const auto &e : datasets) {
        if (e.datasetId == currentId) adoptShape(e);
    }
    if (refW == 0) {
        for (const auto &e : datasets) adoptShape(e);
    }

    int cubeCount = 0;
    for (const auto &e : datasets) {
        // Only cube datasets can be stacked. Skip "image" / unknown kinds so
        // the list stays focused on what the operator can actually use.
        if (!e.kind.isEmpty() && e.kind != QStringLiteral("cube")) {
            continue;
        }
        const QString label = e.label.isEmpty() ? e.datasetId : e.label;
        const bool shapeKnown = e.width > 0 && e.height > 0 && e.depth > 0;
        const bool compatible = refW == 0 || !shapeKnown
                || (e.width == refW && e.height == refH && e.depth == refD);
        const QString suffix = shapeKnown
                ? u"  (%1×%2×%3)"_s.arg(e.width).arg(e.height).arg(e.depth)
                : QString();
        auto *item = new QListWidgetItem(label + suffix, m_datasetList);
        item->setData(Qt::UserRole, e.datasetId);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        if (!compatible) {
            // Keep visible so the user understands why a known cube isn't
            // selectable, but disable + tooltip.
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setToolTip(
                    u"Shape %1×%2×%3 differs from reference %4×%5×%6 — cannot be stacked."_s
                            .arg(e.width).arg(e.height).arg(e.depth)
                            .arg(refW).arg(refH).arg(refD));
            item->setCheckState(Qt::Unchecked);
        } else {
            item->setCheckState(e.datasetId == currentId
                                    ? Qt::Checked : Qt::Unchecked);
        }
        ++cubeCount;
    }
    if (cubeCount == 0) {
        auto *empty = new QListWidgetItem(
                u"(no cube datasets open in this session — open ≥ 2 cubes first)"_s,
                m_datasetList);
        empty->setFlags(Qt::NoItemFlags);
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
        auto *item = m_datasetList->item(i);
        if (item->checkState() != Qt::Checked) continue;
        // The visible item text now also carries "(W×H×D)"; the real dataset
        // id is stored in Qt::UserRole. Fall back to text() for items added
        // via the legacy (QStringList) constructor that did not set UserRole.
        const QString id = item->data(Qt::UserRole).toString();
        selected << (id.isEmpty() ? item->text() : id);
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
