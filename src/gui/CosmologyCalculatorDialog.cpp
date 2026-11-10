// CosmologyCalculatorDialog.cpp
// See CosmologyCalculatorDialog.h.

#include "CosmologyCalculatorDialog.h"

#include "BackendClient.h"
#include "theme/VisivoTheme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::StringLiterals;
namespace cosmo = visivo::cosmology;

// ── The stored custom model ──────────────────────────────────────────────────

CosmologyChoiceHub &CosmologyChoiceHub::instance()
{
    static CosmologyChoiceHub hub;
    return hub;
}

void CosmologyChoiceHub::setCustomModel(const cosmo::Model &model)
{
    QSettings settings;
    cosmo::saveCustom(settings, model);
    emit customModelChanged();
}

cosmo::Model CosmologyChoiceHub::customModel() const
{
    QSettings settings;
    return cosmo::loadCustom(settings);
}

// ── The calculation (worker thread) ──────────────────────────────────────────

namespace {

/// Every quantity the backend gives for @p model at each redshift. Runs on a
/// pool thread with a client of its own; @p cancel stops it between requests.
QList<cosmo::Row> computeRows(const QString &url, const QString &token, const cosmo::Model &model,
                              const QVector<double> &redshifts,
                              std::shared_ptr<std::atomic_bool> cancel)
{
    BackendClient client(url, token);
    QList<cosmo::Row> rows;
    rows.reserve(redshifts.size());
    BackendCosmologyParams params;
    params.h0 = model.h0;
    params.om0 = model.om0;
    params.ode0 = model.ode0;
    for (double z : redshifts) {
        if (cancel && cancel->load())
            break;
        cosmo::Row row;
        row.z = z;
        if (model.isCustom()) {
            if (z > cosmo::kRedshiftMaxCustom) {
                row.error = u"the custom-model route covers z ≤ %1"_s
                                    .arg(cosmo::kRedshiftMaxCustom);
                rows.append(row);
                continue;
            }
            const auto r = client.requestCosmologyDistances(z, &params);
            if (!r.valid) {
                row.error = r.error.isEmpty() ? u"refused by the server"_s : r.error;
            } else {
                row.comovingMpc = r.comovingMpc;
                row.luminosityMpc = r.luminosityMpc;
                row.angularDiameterMpc = r.angularDiameterMpc;
                row.lookbackGyr = r.lookbackGyr;
                row.ageGyr = r.ageGyr;
                row.kpcPerArcsec = cosmo::kpcPerArcsecFromAngularDiameter(r.angularDiameterMpc);
                // The server answers a quantity with no finite value as null:
                // with no matter (de Sitter) there is no Big Bang to count from.
                if (!std::isfinite(r.ageGyr))
                    row.error = u"age is not finite for this model (no Big Bang)"_s;
            }
            rows.append(row);
            continue;
        }

        const auto d = client.requestCosmologyDistance(z, model.preset);
        if (!d.valid) {
            row.error = d.error.isEmpty() ? u"refused by the server"_s : d.error;
            rows.append(row);
            continue;
        }
        row.comovingMpc = d.comovingMpc;
        row.luminosityMpc = d.luminosityMpc;
        row.kpcPerArcsec = d.kpcPerArcsec;
        // The full route computes the server's Planck18 when it is given no
        // parameters, and only that preset: the others stay without D_A,
        // lookback time and age rather than borrowing another model's.
        if (model.preset == u"Planck18"_s) {
            if (z <= cosmo::kRedshiftMaxCustom) {
                const auto f = client.requestCosmologyDistances(z, nullptr);
                if (f.valid) {
                    row.angularDiameterMpc = f.angularDiameterMpc;
                    row.lookbackGyr = f.lookbackGyr;
                    row.ageGyr = f.ageGyr;
                } else {
                    row.error = u"D_A, lookback and age: %1"_s.arg(f.error);
                }
            } else {
                row.error = u"D_A, lookback and age are computed for z ≤ %1"_s
                                    .arg(cosmo::kRedshiftMaxCustom);
            }
        }
        rows.append(row);
    }
    return rows;
}

}   // namespace

// ── The window ───────────────────────────────────────────────────────────────

CosmologyCalculatorDialog *CosmologyCalculatorDialog::showCalculator(
        QWidget *parent, const QString &backendUrl, const QString &backendToken,
        const cosmo::Model *initial)
{
    // One window for the application. It is parentless on purpose: it belongs
    // to no viewer, and closing the catalogue that opened it must not take the
    // calculator (and the numbers in it) along.
    static QPointer<CosmologyCalculatorDialog> s_dialog;
    Q_UNUSED(parent);
    if (!s_dialog)
        s_dialog = new CosmologyCalculatorDialog(backendUrl, backendToken, nullptr);
    else
        s_dialog->setBackend(backendUrl, backendToken);
    if (initial)
        s_dialog->selectModel(*initial);
    s_dialog->show();
    s_dialog->raise();
    s_dialog->activateWindow();
    return s_dialog;
}

CosmologyCalculatorDialog::CosmologyCalculatorDialog(const QString &backendUrl,
                                                     const QString &backendToken, QWidget *parent)
    : QDialog(parent), m_backendUrl(backendUrl), m_backendToken(backendToken)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(u"Cosmology Calculator"_s);
    setModal(false);
    resize(860, 600);

    auto *root = new QVBoxLayout(this);
    root->setSpacing(8);

    // ── Model ────────────────────────────────────────────────────────────────
    auto *modelCard = new QFrame(this);
    modelCard->setStyleSheet(VisivoTheme::sidebarCardStyleSheet());
    auto *modelLayout = new QVBoxLayout(modelCard);
    modelLayout->setContentsMargins(10, 8, 10, 8);
    auto *modelHdr = new QLabel(u"COSMOLOGY"_s, modelCard);
    modelHdr->setStyleSheet(VisivoTheme::sidebarSubHeaderStyleSheet());
    modelLayout->addWidget(modelHdr);

    auto *modelForm = new QFormLayout;
    m_modelCombo = new QComboBox(modelCard);
    for (const QString &name : cosmo::presetNames())
        m_modelCombo->addItem(name, name);
    m_modelCombo->addItem(u"Custom ΛCDM"_s, cosmo::customName());
    m_modelCombo->setToolTip(
            u"A preset is astropy's own model (radiation and neutrinos included).\n"
            u"Custom: H₀, Ωm and ΩΛ of your choice, matter + Λ "
            u"(+ curvature), no radiation. A custom model with Planck18's H₀ and "
            u"Ωm does not reproduce the Planck18 preset: without radiation its "
            u"distances are 0.1–0.2 % larger and its ages up to 1 % larger (z ≤ 20)."_s);
    modelForm->addRow(u"Model"_s, m_modelCombo);
    modelLayout->addLayout(modelForm);

    m_customBox = new QWidget(modelCard);
    auto *customForm = new QFormLayout(m_customBox);
    customForm->setContentsMargins(0, 0, 0, 0);
    m_h0 = new QDoubleSpinBox(m_customBox);
    m_h0->setRange(cosmo::kH0Min, cosmo::kH0Max);
    m_h0->setDecimals(2);
    m_h0->setSingleStep(0.5);
    m_h0->setSuffix(u" km/s/Mpc"_s);
    m_h0->setToolTip(u"Hubble constant today. The server accepts 40 to 120 km/s/Mpc."_s);
    m_om0 = new QDoubleSpinBox(m_customBox);
    // 1, not the server's 1.5: with ΩΛ ≥ 0, Ωm > 1 is always a closed model,
    // which the server refuses.
    m_om0->setRange(0.0, 1.0);
    m_om0->setDecimals(4);
    m_om0->setSingleStep(0.01);
    m_om0->setToolTip(u"Matter density today (baryons + dark matter), in units of the "
                      u"critical density."_s);
    m_ode0 = new QDoubleSpinBox(m_customBox);
    m_ode0->setRange(0.0, 1.0);
    m_ode0->setDecimals(4);
    m_ode0->setSingleStep(0.01);
    m_ode0->setToolTip(u"Dark-energy (cosmological constant) density today, in units of "
                       u"the critical density."_s);
    m_flat = new QCheckBox(u"Flat: ΩΛ = 1 − Ωm"_s, m_customBox);
    m_flat->setChecked(true);
    m_flat->setToolTip(u"Untick to set ΩΛ yourself: the model is open if "
                       u"Ωm + ΩΛ < 1, flat if the sum is 1. Closed models "
                       u"(sum > 1) are not supported by the server."_s);
    customForm->addRow(u"H₀"_s, m_h0);
    customForm->addRow(u"Ωm"_s, m_om0);
    customForm->addRow(QString(), m_flat);
    customForm->addRow(u"ΩΛ"_s, m_ode0);
    modelLayout->addWidget(m_customBox);

    m_modelLine = new QLabel(modelCard);
    m_modelLine->setWordWrap(true);
    m_modelLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    modelLayout->addWidget(m_modelLine);
    m_routeNote = new QLabel(modelCard);
    m_routeNote->setWordWrap(true);
    m_routeNote->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    modelLayout->addWidget(m_routeNote);
    root->addWidget(modelCard);

    // ── Redshifts ────────────────────────────────────────────────────────────
    auto *zRow = new QHBoxLayout;
    m_redshifts = new QPlainTextEdit(this);
    m_redshifts->setPlaceholderText(
            u"Redshifts: one or many, separated by commas, spaces or new lines "
            u"(e.g. 0.1, 0.5, 1, 2), or paste a column."_s);
    m_redshifts->setPlainText(u"0.1, 0.5, 1, 2"_s);
    m_redshifts->setMaximumHeight(70);
    zRow->addWidget(m_redshifts, 1);
    auto *zButtons = new QVBoxLayout;
    m_computeButton = new QPushButton(u"Compute"_s, this);
    m_computeButton->setDefault(true);
    zButtons->addWidget(m_computeButton);
    zButtons->addStretch(1);
    zRow->addLayout(zButtons);
    root->addLayout(zRow);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    // ── Results ──────────────────────────────────────────────────────────────
    m_table = new QTableWidget(this);
    QStringList headers{ u"z"_s };
    for (const cosmo::Column &c : cosmo::columns())
        headers.append(c.header);
    headers.append(u"Note"_s);
    m_table->setColumnCount(headers.size());
    m_table->setHorizontalHeaderLabels(headers);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(headers.size() - 1, QHeaderView::Stretch);
    root->addWidget(m_table, 1);

    auto *buttons = new QHBoxLayout;
    m_useButton = new QPushButton(u"Use in 3-D Catalogues"_s, this);
    m_useButton->setToolTip(
            u"Make this custom model the one a 3-D catalogue viewer uses when its "
            u"cosmology is set to Custom. Windows already on Custom re-place their "
            u"sources."_s);
    m_copyButton = new QPushButton(u"Copy"_s, this);
    m_copyButton->setToolTip(u"Copy the table, tab-separated, with the model on the first line."_s);
    m_exportButton = new QPushButton(u"Export CSV…"_s, this);
    auto *closeButton = new QPushButton(u"Close"_s, this);
    buttons->addWidget(m_useButton);
    buttons->addStretch(1);
    buttons->addWidget(m_copyButton);
    buttons->addWidget(m_exportButton);
    buttons->addWidget(closeButton);
    root->addLayout(buttons);
    m_copyButton->setEnabled(false);
    m_exportButton->setEnabled(false);

    // ── Wiring ───────────────────────────────────────────────────────────────
    QObject::connect(m_modelCombo, &QComboBox::currentIndexChanged, this,
                     [this](int) { refreshModelState(); });
    QObject::connect(m_h0, &QDoubleSpinBox::valueChanged, this, [this](double) {
        refreshModelState();
    });
    QObject::connect(m_om0, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_flat && m_flat->isChecked() && m_ode0) {
            // Flat follows Ωm; a value the server cannot take (Ωm > 1 leaves no
            // room for a positive ΩΛ) is clamped by the spin box's range at 0.
            const QSignalBlocker block(m_ode0);
            m_ode0->setValue(std::max(0.0, 1.0 - v));
        }
        refreshModelState();
    });
    QObject::connect(m_ode0, &QDoubleSpinBox::valueChanged, this, [this](double) {
        refreshModelState();
    });
    QObject::connect(m_flat, &QCheckBox::toggled, this, [this](bool on) {
        if (on && m_om0 && m_ode0) {
            const QSignalBlocker block(m_ode0);
            m_ode0->setValue(std::max(0.0, 1.0 - m_om0->value()));
        }
        refreshModelState();
    });
    QObject::connect(m_computeButton, &QPushButton::clicked, this,
                     &CosmologyCalculatorDialog::compute);
    QObject::connect(m_copyButton, &QPushButton::clicked, this,
                     &CosmologyCalculatorDialog::copyTable);
    QObject::connect(m_exportButton, &QPushButton::clicked, this,
                     &CosmologyCalculatorDialog::exportCsv);
    QObject::connect(m_useButton, &QPushButton::clicked, this,
                     &CosmologyCalculatorDialog::useForCatalogues);
    QObject::connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    // Start from the custom model the user last handed to the catalogues, so
    // switching the combo to Custom shows that model and not a default.
    const cosmo::Model stored = CosmologyChoiceHub::instance().customModel();
    {
        const QSignalBlocker b1(m_h0), b2(m_om0), b3(m_ode0), b4(m_flat);
        m_h0->setValue(stored.h0);
        m_om0->setValue(stored.om0);
        m_ode0->setValue(stored.ode0);
        m_flat->setChecked(stored.isFlat());
    }
    refreshModelState();
}

CosmologyCalculatorDialog::~CosmologyCalculatorDialog()
{
    if (m_cancel)
        m_cancel->store(true);
}

void CosmologyCalculatorDialog::closeEvent(QCloseEvent *event)
{
    // Nothing to ask: the requests are short reads with no side effect on the
    // server, so closing simply stops asking for the rest.
    if (m_cancel)
        m_cancel->store(true);
    QDialog::closeEvent(event);
}

void CosmologyCalculatorDialog::setBackend(const QString &backendUrl, const QString &backendToken)
{
    if (!backendUrl.isEmpty()) {
        m_backendUrl = backendUrl;
        m_backendToken = backendToken;
    }
}

void CosmologyCalculatorDialog::selectModel(const cosmo::Model &model)
{
    if (!m_modelCombo)
        return;
    if (!model.isCustom()) {
        const int idx = m_modelCombo->findData(model.preset);
        if (idx >= 0)
            m_modelCombo->setCurrentIndex(idx);
        return;
    }
    {
        const QSignalBlocker b1(m_h0), b2(m_om0), b3(m_ode0), b4(m_flat);
        m_h0->setValue(model.h0);
        m_om0->setValue(model.om0);
        m_ode0->setValue(model.ode0);
        m_flat->setChecked(model.isFlat());
    }
    m_modelCombo->setCurrentIndex(m_modelCombo->findData(cosmo::customName()));
    refreshModelState();
}

cosmo::Model CosmologyCalculatorDialog::currentModel() const
{
    const QString key = m_modelCombo ? m_modelCombo->currentData().toString() : QString();
    if (key != cosmo::customName())
        return cosmo::Model::fromPreset(key.isEmpty() ? u"Planck18"_s : key);
    return cosmo::Model::custom(m_h0->value(), m_om0->value(),
                                m_flat->isChecked() ? 1.0 - m_om0->value() : m_ode0->value());
}

void CosmologyCalculatorDialog::refreshModelState()
{
    const cosmo::Model m = currentModel();
    const bool custom = m.isCustom();
    m_customBox->setVisible(custom);
    m_ode0->setEnabled(!m_flat->isChecked());
    m_useButton->setVisible(custom);

    const QString problem = cosmo::validate(m);
    QString line = cosmo::describe(m);
    if (custom && !m.isFlat() && problem.isEmpty()) {
        // Said in words as well as in the parameter list: a non-flat universe
        // is a choice, and one easy to make by accident with Flat unticked.
        line += u"<br><b>Not flat:</b> Ωm + ΩΛ = %1, so this is an %2 "
                u"universe with Ωk = %3. Every distance below includes that "
                u"curvature."_s.arg(m.om0 + m.ode0, 0, 'f', 4)
                        .arg(cosmo::curvatureWord(m))
                        .arg(m.omegaK(), 0, 'f', 4);
    }
    if (!problem.isEmpty())
        line += u"<br><span style='color:#dc2626'>%1</span>"_s.arg(problem.toHtmlEscaped());
    m_modelLine->setText(line);
    m_computeButton->setEnabled(problem.isEmpty() && !m_running);
    m_useButton->setEnabled(problem.isEmpty());

    if (custom) {
        m_routeNote->setText(
                u"Custom: astropy LambdaCDM(H₀, Ωm, ΩΛ) with no radiation "
                u"(Tcmb0 = 0), from GET /v1/cosmology/distances, which accepts z up to 20. "
                u"The proper scale in kpc/″ is the returned D_A expressed per arcsecond. "
                u"With Planck18's H₀ and Ωm it is not the Planck18 preset: without "
                u"radiation, distances come out 0.1–0.2 % larger and ages up to 1 % "
                u"larger."_s);
    } else if (m.preset == u"Planck18"_s) {
        m_routeNote->setText(
                u"Planck18: D_C, D_L and the scale from POST /v1/cosmology/distance; D_A, "
                u"lookback time and age from GET /v1/cosmology/distances (z up to 20)."_s);
    } else {
        m_routeNote->setText(
                u"%1: the server computes D_C, D_L and the scale in kpc/″ "
                u"(POST /v1/cosmology/distance). It has no route for D_A, lookback "
                u"time or age under this preset, so those columns stay empty."_s
                        .arg(m.preset));
    }
}

void CosmologyCalculatorDialog::compute()
{
    const cosmo::Model model = currentModel();
    const QString problem = cosmo::validate(model);
    if (!problem.isEmpty()) {
        m_status->setText(problem);
        return;
    }
    const cosmo::RedshiftParse parsed = cosmo::parseRedshifts(m_redshifts->toPlainText());
    if (parsed.redshifts.isEmpty()) {
        m_status->setText(parsed.rejected.isEmpty()
                                  ? u"Type at least one redshift."_s
                                  : u"No usable redshift: %1 (a redshift is a number ≥ 0)."_s
                                            .arg(parsed.rejected.join(u", "_s)));
        return;
    }
    if (parsed.redshifts.size() > kMaxRedshifts) {
        m_status->setText(u"%1 redshifts: the calculator takes up to %2 at a time. For a "
                          u"catalogue, open it in the 3-D catalogue viewer, which asks the "
                          u"server for all of them at once."_s
                                  .arg(parsed.redshifts.size())
                                  .arg(kMaxRedshifts));
        return;
    }
    if (m_backendUrl.isEmpty()) {
        m_status->setText(u"No backend: start the local backend or set one in Settings."_s);
        return;
    }

    if (m_cancel)
        m_cancel->store(true);   // a previous run still going stops here
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const int runId = ++m_runId;
    m_running = true;
    m_computeButton->setEnabled(false);
    QString note = u"Computing %1 redshift(s) with %2…"_s.arg(parsed.redshifts.size())
                           .arg(model.isCustom() ? u"the custom model"_s : model.preset);
    if (!parsed.rejected.isEmpty())
        note += u" Skipped (not a redshift ≥ 0): %1."_s.arg(parsed.rejected.join(u", "_s));
    m_status->setText(note);
    const QString skipped = parsed.rejected.join(u", "_s);

    auto *watcher = new QFutureWatcher<QList<cosmo::Row>>(this);
    QPointer<CosmologyCalculatorDialog> self(this);
    QObject::connect(watcher, &QFutureWatcher<QList<cosmo::Row>>::finished, this,
                     [self, watcher, runId, model, skipped]() {
                         watcher->deleteLater();
                         if (!self || runId != self->m_runId)
                             return;   // closed, or overtaken by a newer run
                         self->m_running = false;
                         self->showRows(model, watcher->result());
                         if (!skipped.isEmpty() && self->m_status)
                             self->m_status->setText(self->m_status->text()
                                                     + u" Skipped (not a redshift ≥ 0): %1."_s
                                                               .arg(skipped));
                         self->refreshModelState();
                     });
    watcher->setFuture(QtConcurrent::run(computeRows, m_backendUrl, m_backendToken, model,
                                         parsed.redshifts, m_cancel));
}

void CosmologyCalculatorDialog::showRows(const cosmo::Model &model,
                                         const QList<cosmo::Row> &rows)
{
    m_shownModel = model;
    m_shownRows = rows;
    const auto cols = cosmo::columns();
    m_table->setRowCount(rows.size());
    int refused = 0;
    for (int r = 0; r < rows.size(); ++r) {
        const cosmo::Row &row = rows.at(r);
        auto *zItem = new QTableWidgetItem(QString::number(row.z, 'g', 10));
        zItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(r, 0, zItem);
        for (int c = 0; c < cols.size(); ++c) {
            const double v = row.*(cols.at(c).field);
            auto *item = new QTableWidgetItem(std::isfinite(v)
                                                      ? cosmo::formatCell(v, cols.at(c).decimals)
                                                      : u"—"_s);
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (!std::isfinite(v))
                item->setToolTip(u"Not finite for this model (e.g. the age of a universe with no Big Bang), or not given by the server."_s);
            m_table->setItem(r, c + 1, item);
        }
        m_table->setItem(r, cols.size() + 1, new QTableWidgetItem(row.error));
        if (!row.error.isEmpty() && !std::isfinite(row.comovingMpc))
            ++refused;
    }
    const bool any = !rows.isEmpty();
    m_copyButton->setEnabled(any);
    m_exportButton->setEnabled(any);
    QString status = u"%1 redshift(s) under %2."_s.arg(rows.size()).arg(cosmo::describe(model));
    if (refused > 0)
        status += u" %1 refused: see the Note column."_s.arg(refused);
    m_status->setText(status);
}

void CosmologyCalculatorDialog::copyTable()
{
    if (m_shownRows.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(cosmo::formatTable(m_shownModel, m_shownRows, u'\t'));
    m_status->setText(u"Copied %1 row(s) to the clipboard."_s.arg(m_shownRows.size()));
}

void CosmologyCalculatorDialog::exportCsv()
{
    if (m_shownRows.isEmpty())
        return;
    const QString path = QFileDialog::getSaveFileName(
            this, u"Export Cosmology Table"_s, u"cosmology.csv"_s, u"CSV (*.csv)"_s);
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, u"Export Cosmology Table"_s,
                             u"Could not write %1:\n%2"_s.arg(path, file.errorString()));
        return;
    }
    QTextStream out(&file);
    out << cosmo::formatTable(m_shownModel, m_shownRows, u',');
    m_status->setText(u"Saved %1."_s.arg(path));
}

void CosmologyCalculatorDialog::useForCatalogues()
{
    const cosmo::Model model = currentModel();
    if (!model.isCustom() || !cosmo::validate(model).isEmpty())
        return;
    CosmologyChoiceHub::instance().setCustomModel(model);
    m_status->setText(u"3-D catalogue viewers set to Custom now use %1."_s
                              .arg(cosmo::describe(model)));
}
