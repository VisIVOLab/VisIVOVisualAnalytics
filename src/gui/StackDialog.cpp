#include "StackDialog.h"
#include "theme/ToolDialogStyle.h"
#include "theme/VisivoTheme.h"

#include "BackendClient.h"

#include "qcustomplot.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include "ToolDialogClose.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────

StackDialog::StackDialog(std::shared_ptr<BackendClient> client,
                         const QString &sessionId,
                         const QStringList &sessionDatasets,
                         QWidget *parent)
    : StackDialog(std::move(client), sessionId,
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

StackDialog::StackDialog(std::shared_ptr<BackendClient> client,
                         const QString &sessionId,
                         const std::vector<DatasetEntry> &datasets,
                         const QString &currentId,
                         QWidget *parent)
    : QDialog(parent)
    , m_client(std::move(client))
    , m_sessionId(sessionId)
{
    setWindowTitle(u"Stack Cubes to a Spectrum"_s);
    setMinimumWidth(540);
    resize(540, 520);
    // Shared input theme — see ToolDialogStyle.
    setStyleSheet(ToolDialogStyle::inputCss());

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 14, 16, 14);
    mainLayout->setSpacing(10);
    mainLayout->addWidget(ToolDialogStyle::makeBreadcrumb(this, u"Stack Cubes to a Spectrum"_s));

    // ── Intro line (matches the muted-helper look of MomentMap / Noise) ──
    // Says what comes OUT, first thing. "Stack Spectral Cubes" was read as
    // "merge these cubes into one cube" and the 1-D result came as a surprise
    // (user report) — which is fair, because the word "stack" only means
    // co-adding spectra if you already know the technique.
    auto *infoLabel = new QLabel(
            u"Co-adds the selected cubes pixel-by-pixel into a SINGLE 1-D "
            u"spectrum, so a signal too faint in any one cube can show up in "
            u"their average. The cubes are not merged into a larger cube.\n\n"
            u"Select two or more. All must share the same spectral grid and "
            u"spatial shape (incompatible cubes are listed as disabled below)."_s, this);
    infoLabel->setStyleSheet(ToolDialogStyle::helperCss());
    infoLabel->setWordWrap(true);
    mainLayout->addWidget(infoLabel);

    // ── CUBES SECTION (header + list + selection counter) ───────────────
    auto *cubesHdr = new QLabel(u"Cubes in session"_s, this);
    cubesHdr->setStyleSheet(ToolDialogStyle::sectionCss());
    mainLayout->addWidget(cubesHdr);

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
                    u"Shape %1×%2×%3 differs from reference %4×%5×%6, cannot be stacked."_s
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
                u"(no cube datasets open in this session - open ≥ 2 cubes first)"_s,
                m_datasetList);
        empty->setFlags(Qt::NoItemFlags);
    }
    mainLayout->addWidget(m_datasetList, 1);

    // Live selection counter — muted helper styling for consistency.
    m_selectionLabel = new QLabel(u"Selected: 0 cubes"_s, this);
    m_selectionLabel->setStyleSheet(ToolDialogStyle::helperCss());
    mainLayout->addWidget(m_selectionLabel);

    // ── COMBINATION SECTION (header + method + weight) ──────────────────
    auto *form = ToolDialogStyle::addSection(mainLayout, u"Combination"_s, this);

    m_methodCombo = new QComboBox(this);
    m_methodCombo->addItem(u"Mean"_s,          u"mean"_s);
    m_methodCombo->addItem(u"Median"_s,        u"median"_s);
    m_methodCombo->addItem(u"Weighted Mean"_s, u"weighted_mean"_s);
    form->addRow(u"Method"_s, m_methodCombo);

    m_weightByCombo = new QComboBox(this);
    m_weightByCombo->addItem(u"Uniform"_s, u"uniform"_s);
    m_weightByCombo->addItem(u"RMS"_s,     u"rms"_s);
    m_weightByCombo->addItem(u"Peak"_s,    u"peak"_s);
    form->addRow(u"Weight by"_s, m_weightByCombo);

    auto *methodHint = new QLabel(
            u"Mean: best SNR for similar-noise cubes. Median: robust to "
            u"outliers / spikes. Weighted Mean: enable when cubes have "
            u"different noise levels (RMS) or peak brightness."_s, this);
    methodHint->setStyleSheet(ToolDialogStyle::helperCss());
    methodHint->setWordWrap(true);
    form->addRow(QString(), methodHint);

    // weight_by only matters for weighted_mean
    auto updateWeightBy = [this]() {
        m_weightByCombo->setEnabled(
                m_methodCombo->currentData().toString() == u"weighted_mean"_s);
    };
    updateWeightBy();
    connect(m_methodCombo, &QComboBox::currentIndexChanged, this, updateWeightBy);

    // ── METHODOLOGY footer ───────────────────────────────────────────────
    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(
            QStringLiteral("color: %1;")
                    .arg(QString::fromLatin1(VisivoTheme::kOutline())));
    mainLayout->addWidget(divider);

    auto *methodologyLabel = new QLabel(
            u"Stacking is spatial pixel-by-pixel · cubes must share the same "
            u"WCS / shape · output is a 1-D combined spectrum, which lands in "
            u"Session Data as a SPEC product."_s, this);
    methodologyLabel->setStyleSheet(ToolDialogStyle::helperCss());
    methodologyLabel->setWordWrap(true);
    mainLayout->addWidget(methodologyLabel);

    // ── Buttons row ───────────────────────────────────────────────────────
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();

    auto *cancelBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, this);
    m_stackBtn      = VisivoTheme::makePrimaryButton(u"Stack"_s, this);
    m_stackBtn->setEnabled(false);   // disabled until ≥ 2 checked
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(m_stackBtn);
    mainLayout->addLayout(btnRow);

    connect(m_datasetList, &QListWidget::itemChanged, this, &StackDialog::onItemChanged);
    connect(m_stackBtn,    &QPushButton::clicked,     this, &StackDialog::onCompute);
    connect(cancelBtn,     &QPushButton::clicked,     this, &QDialog::reject);

    // Count what the list was BUILT with. The current cube is pre-checked up
    // there, but the counter and the Stack button only ever updated on
    // itemChanged — which is connected here, after the population — so the
    // dialog opened saying "Selected: 0 cubes" with a box visibly ticked
    // (user report).
    updateSelectionLabel();
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

    std::shared_ptr<BackendClient> client = m_client;   // shared with the worker
    client->setSessionId(m_sessionId);  // FIX 5

    auto *watcher = new QFutureWatcher<BackendStackResult>(this);
    QPointer<StackDialog> self(this);

    connect(watcher, &QFutureWatcher<BackendStackResult>::finished,
            watcher, [self, watcher, method, weightBy, selected]() {
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

        // #5: promote the stacked spectrum to a first-class SPEC pane product instead of a
        // standalone QCustomPlot window — hand the channel values + a descriptive label to the
        // caller (vtkWindowCube::addSpectrumProduct).
        QVector<double> values(nPts);
        for (int i = 0; i < nPts; ++i)
            values[i] = static_cast<double>(ptr[i]);

        const QString label = u"Stacked (%1, N=%2)"_s.arg(method).arg(result.nCubes);

        // Provenance for the SPEC product's recipe: method, weighting, source cubes,
        // channel count. Weighting only applies to weighted_mean.
        QVariantMap meta;
        meta.insert(u"Method"_s, method);
        if (method == u"weighted_mean"_s && !weightBy.isEmpty())
            meta.insert(u"Weighting"_s, weightBy);
        meta.insert(u"Cubes"_s, QString::number(result.nCubes));
        meta.insert(u"Datasets"_s, selected.join(u", "_s));
        meta.insert(u"Channels"_s, QString::number(nPts));
        emit self->spectrumReady(values, label, meta);

        if (self) {
            self->accept();
        }
    });

    watcher->setFuture(QtConcurrent::run(
            [client, selected, method, weightBy]() {
                return client->fetchStackBinary(selected, method, {}, weightBy);
            }));
}

void StackDialog::closeEvent(QCloseEvent *event)
{
    // See ToolDialogClose.h: a compute in flight cannot be cancelled, so the
    // choice is between waiting and discarding the result — said out loud.
    if (m_busy && !visivo::confirmAbandonRunningCompute(this, u"Stack Spectral Cubes"_s, u"The spectral stacking"_s)) {
        event->ignore();
        return;
    }
    event->accept();
}

void StackDialog::setUiBusy(bool busy)
{
    m_busy = busy;
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
