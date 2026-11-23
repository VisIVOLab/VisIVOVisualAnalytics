#include "LteCoDialog.h"

#include "BackendClient.h"
#include "DiagnosticsManager.h"
#include "ToolDialogClose.h"
#include "ToolProgressSheet.h"
#include "WindowCloseShortcut.h"
#include "theme/ToolDialogStyle.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Qt::StringLiterals;
namespace lte = visivo::lte;

namespace {

const QString kTitle = u"CO Isotopologues (LTE)"_s;

QLineEdit *numberField(QWidget *parent, const QString &placeholder, const QString &tip)
{
    auto *e = new QLineEdit(parent);
    e->setPlaceholderText(placeholder);
    e->setToolTip(tip);
    e->setClearButtonEnabled(true);
    return e;
}

QString kindText(const QString &kind)
{
    if (kind == u"cube"_s)
        return u"cube"_s;
    if (kind == u"image"_s)
        return u"2-D map"_s;
    return kind;
}

}   // namespace

LteCoDialog::LteCoDialog(const Context &context, QWidget *parent)
    : QDialog(parent), m_ctx(context)
{
    setWindowTitle(kTitle);
    setStyleSheet(ToolDialogStyle::inputCss());
    resize(760, 720);
    visivo::installCloseWindowShortcut(this);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(8);
    root->addWidget(ToolDialogStyle::makeBreadcrumb(this, kTitle));
    auto *intro = new QLabel(
            u"Optical depth from the ¹³CO/C¹⁸O ratio, excitation temperature, "
            u"opacity-corrected column densities, N(H₂) and mass, in LTE. Cubes are "
            u"integrated over the velocity window. A 2-D map is taken as ∫T dv in K km/s "
            u"(a ¹²CO map as its peak T in K), on the T_A* scale unless η_mb = 1."_s, this);
    intro->setWordWrap(true);
    intro->setStyleSheet(ToolDialogStyle::helperCss());
    root->addWidget(intro);

    m_tabs = new QTabWidget(this);
    root->addWidget(m_tabs, 1);

    // ── Parameters ───────────────────────────────────────────────────────────
    auto *scroll = new QScrollArea(m_tabs);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget(scroll);
    auto *col = new QVBoxLayout(page);
    col->setContentsMargins(4, 4, 8, 4);
    scroll->setWidget(page);
    m_tabs->addTab(scroll, u"Parameters"_s);

    auto *lines = ToolDialogStyle::addSection(col, u"Lines"_s, page);
    m_transition = new QComboBox(page);
    m_transition->addItem(u"J = 1–0"_s, u"1-0"_s);
    m_transition->addItem(u"J = 3–2"_s, u"3-2"_s);
    m_transition->setPlaceholderText(u"Choose the transition"_s);
    m_transition->setCurrentIndex(-1);
    const lte::LineGuess hostLine = lte::guessLine(m_ctx.hostRestFreqHz);
    if (!hostLine.transition.isEmpty())
        m_transition->setCurrentIndex(m_transition->findData(hostLine.transition));
    lines->addRow(u"Transition"_s, m_transition);
    m_ds13 = new QComboBox(page);
    m_ds18 = new QComboBox(page);
    m_ds12 = new QComboBox(page);
    for (QComboBox *c : { m_ds13, m_ds18, m_ds12 }) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(24);
        c->addItem(u"Listing the session's datasets…"_s);
        c->setEnabled(false);
    }
    lines->addRow(u"¹³CO"_s, m_ds13);
    lines->addRow(u"C¹⁸O"_s, m_ds18);
    lines->addRow(u"¹²CO (optional)"_s, m_ds12);
    m_datasetNote = new QLabel(page);
    m_datasetNote->setWordWrap(true);
    m_datasetNote->setStyleSheet(ToolDialogStyle::helperCss());
    lines->addRow(QString(), m_datasetNote);

    auto *windowRow = new QHBoxLayout();
    m_vMin = numberField(page, u"from"_s, u"Start of the line's window, km/s"_s);
    m_vMax = numberField(page, u"to"_s, u"End of the line's window, km/s"_s);
    if (m_ctx.vMinKms && m_ctx.vMaxKms) {
        m_vMin->setText(QString::number(*m_ctx.vMinKms, 'g', 10));
        m_vMax->setText(QString::number(*m_ctx.vMaxKms, 'g', 10));
    }
    windowRow->addWidget(m_vMin);
    windowRow->addWidget(new QLabel(u"–"_s, page));
    windowRow->addWidget(m_vMax);
    windowRow->addWidget(new QLabel(u"km/s"_s, page));
    lines->addRow(u"Velocity window"_s, windowRow);
    m_windowNote = new QLabel(page);
    m_windowNote->setWordWrap(true);
    m_windowNote->setStyleSheet(ToolDialogStyle::helperCss());
    m_windowNote->setText(m_ctx.windowSource.isEmpty()
                                  ? u"Needed for cubes: the channels summed into W, and those "
                                    u"outside it give the noise."_s
                                  : u"From %1. The channels outside it give the noise."_s
                                            .arg(m_ctx.windowSource));
    lines->addRow(QString(), m_windowNote);

    auto *cal = ToolDialogStyle::addSection(col, u"Main-beam efficiency (T_mb = T_A*/η_mb)"_s, page);
    const QString etaTip =
            u"Leave empty to take it from the header: 1 when TEMPSCAL, BUNIT or BTYPE says the "
            u"data are already T_mb, else BEAMEFF/FORWEFF or ETAMB. ETAL is never used: it is "
            u"the rear-spillover efficiency, already in T_A*. A typed value overrides the "
            u"header. CHIMPS (TEMPSCAL = TA*): 0.72, Rigby et al. 2016."_s;
    m_eta13 = numberField(page, u"from the header"_s, etaTip);
    m_eta18 = numberField(page, u"from the header"_s, etaTip);
    m_eta12 = numberField(page, u"from the header"_s, etaTip);
    cal->addRow(u"η_mb ¹³CO"_s, m_eta13);
    cal->addRow(u"η_mb C¹⁸O"_s, m_eta18);
    cal->addRow(u"η_mb ¹²CO"_s, m_eta12);
    m_etaNote = new QLabel(page);
    m_etaNote->setWordWrap(true);
    m_etaNote->setStyleSheet(ToolDialogStyle::helperCss());
    m_etaNote->setText(u"Empty: read from the header. If the header gives none, the run "
                       u"stops and asks for it."_s);
    cal->addRow(QString(), m_etaNote);

    auto *abund = ToolDialogStyle::addSection(col, u"Abundances and distance"_s, page);
    m_distance = numberField(page, u"none"_s,
                             u"Heliocentric distance. With the map's centre it gives R_GC "
                             u"(R₀ = 8.5 kpc); it is needed for the mass."_s);
    abund->addRow(u"Distance [kpc]"_s, m_distance);
    m_rGc = numberField(page, u"from the distance"_s,
                        u"Galactocentric radius, for X and the abundances relative to H₂."_s);
    abund->addRow(u"R_GC [kpc]"_s, m_rGc);
    m_xRatio = numberField(page, u"from the gradients at R_GC"_s,
                           u"X(¹³CO)/X(C¹⁸O) = (¹⁶O/¹⁸O)/(¹²C/¹³C). Without it, both ratios "
                           u"come from the galactocentric gradients at R_GC."_s);
    abund->addRow(u"X(¹³CO)/X(C¹⁸O)"_s, m_xRatio);
    m_gradient = new QComboBox(page);
    m_gradient->addItem(u"Milam et al. 2005: 6.21 R_GC + 18.71"_s, u"milam2005"_s);
    m_gradient->addItem(u"Wilson & Rood 1994: 7.5 R_GC + 7.6"_s, u"wilson_rood1994"_s);
    m_gradient->setToolTip(u"¹⁶O/¹⁸O is always 58.8 R_GC + 37.1 (Wilson & Rood 1994)."_s);
    abund->addRow(u"¹²C/¹³C gradient"_s, m_gradient);
    m_h2From = new QComboBox(page);
    m_h2From->addItem(u"C¹⁸O"_s, u"c18o"_s);
    m_h2From->addItem(u"¹³CO"_s, u"13co"_s);
    abund->addRow(u"N(H₂) from"_s, m_h2From);
    m_x12co = numberField(page, u"8.5e-5 (Frerking et al. 1982)"_s,
                          u"X(¹²CO) relative to H₂; the isotopologue's abundance is this over "
                          u"the isotopic ratio at R_GC."_s);
    abund->addRow(u"X(¹²CO)"_s, m_x12co);

    auto *exc = ToolDialogStyle::addSection(col, u"Excitation and noise"_s, page);
    m_tEx = numberField(page, u"measured"_s,
                        u"Empty: measured, from the ¹²CO peak (assumed optically thick and "
                        u"filling the beam) if given, else from the ¹³CO peak with τ from the "
                        u"ratio of the peaks (cubes only). A typed value is an assumption."_s);
    exc->addRow(u"Fixed T_ex [K]"_s, m_tEx);
    m_snr = new QDoubleSpinBox(page);
    m_snr->setRange(0.0, 100.0);
    m_snr->setDecimals(1);
    m_snr->setValue(3.0);
    m_snr->setSuffix(u" σ"_s);
    m_snr->setToolTip(u"A pixel, or a region's mean, is solved only where both W reach this "
                      u"many σ."_s);
    exc->addRow(u"S/N threshold"_s, m_snr);

    auto *reg = ToolDialogStyle::addSection(col, u"Regions"_s, page);
    m_regionList = new QListWidget(page);
    m_regionList->setMaximumHeight(110);
    for (const RegionChoice &r : m_ctx.regions) {
        auto *item = new QListWidgetItem(r.name, m_regionList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    reg->addRow(m_regionList);
    m_regionNote = new QLabel(page);
    m_regionNote->setWordWrap(true);
    m_regionNote->setStyleSheet(ToolDialogStyle::helperCss());
    reg->addRow(m_regionNote);
    col->addStretch(1);

    // ── Results ──────────────────────────────────────────────────────────────
    auto *results = new QWidget(m_tabs);
    auto *rcol = new QVBoxLayout(results);
    rcol->setContentsMargins(4, 4, 4, 4);
    m_resultNote = new QLabel(u"Run the analysis to see its results here."_s, results);
    m_resultNote->setWordWrap(true);
    m_resultNote->setStyleSheet(ToolDialogStyle::helperCss());
    rcol->addWidget(m_resultNote);
    m_table = new QTableWidget(results);
    m_table->setColumnCount(lte::tableColumns().size());
    m_table->setHorizontalHeaderLabels(lte::tableColumns());
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    rcol->addWidget(m_table, 2);
    auto *mapsHdr = new QLabel(u"Maps"_s, results);
    mapsHdr->setStyleSheet(ToolDialogStyle::sectionCss());
    rcol->addWidget(mapsHdr);
    m_mapList = new QListWidget(results);
    m_mapList->setMaximumHeight(140);
    m_mapList->setToolTip(u"Double-click a map to open it in a viewer."_s);
    rcol->addWidget(m_mapList, 1);
    auto *assHdr = new QLabel(u"Assumptions"_s, results);
    assHdr->setStyleSheet(ToolDialogStyle::sectionCss());
    rcol->addWidget(assHdr);
    m_assumptions = new QPlainTextEdit(results);
    m_assumptions->setReadOnly(true);
    m_assumptions->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    rcol->addWidget(m_assumptions, 2);
    m_tabs->addTab(results, u"Results"_s);

    // ── Buttons ──────────────────────────────────────────────────────────────
    auto *buttons = new QHBoxLayout();
    m_openMapBtn = new QPushButton(u"Open Map"_s, this);
    m_openMapBtn->setEnabled(false);
    m_csvBtn = new QPushButton(u"Save CSV…"_s, this);
    m_csvBtn->setEnabled(false);
    m_runBtn = new QPushButton(u"Run"_s, this);
    m_runBtn->setDefault(true);
    m_runBtn->setEnabled(false);
    auto *closeBtn = new QPushButton(u"Close"_s, this);
    buttons->addWidget(m_openMapBtn);
    buttons->addWidget(m_csvBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    buttons->addWidget(m_runBtn);
    root->addLayout(buttons);

    connect(m_runBtn, &QPushButton::clicked, this, &LteCoDialog::run);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(m_csvBtn, &QPushButton::clicked, this, &LteCoDialog::saveCsv);
    connect(m_openMapBtn, &QPushButton::clicked, this, &LteCoDialog::openSelectedMap);
    connect(m_mapList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { this->openSelectedMap(); });
    connect(m_mapList, &QListWidget::currentRowChanged, this,
            [this](int row) { m_openMapBtn->setEnabled(!m_busy && row >= 0); });
    connect(m_ds13, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { this->refreshRegionsEnabled(); });
    for (QLineEdit *e : { m_eta13, m_eta18, m_eta12 })
        connect(e, &QLineEdit::textEdited, e, [e](const QString &) { e->setStyleSheet(QString()); });

    refreshRegionsEnabled();
    listDatasets();
}

void LteCoDialog::refreshHostContext(const Context &context)
{
    if (m_busy)
        return;
    QHash<QString, Qt::CheckState> was;
    for (int i = 0; i < m_regionList->count(); ++i)
        was.insert(m_regionList->item(i)->text(), m_regionList->item(i)->checkState());
    m_ctx.regions = context.regions;
    m_regionList->clear();
    for (const RegionChoice &r : m_ctx.regions) {
        auto *item = new QListWidgetItem(r.name, m_regionList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(was.value(r.name, Qt::Checked));
    }
    if (m_vMin->text().trimmed().isEmpty() && m_vMax->text().trimmed().isEmpty()
        && context.vMinKms && context.vMaxKms) {
        m_vMin->setText(QString::number(*context.vMinKms, 'g', 10));
        m_vMax->setText(QString::number(*context.vMaxKms, 'g', 10));
        m_ctx.windowSource = context.windowSource;
        m_windowNote->setText(u"From %1. The channels outside it give the noise."_s
                                      .arg(context.windowSource));
    }
    refreshRegionsEnabled();
    listDatasets();
}

void LteCoDialog::listDatasets()
{
    const QString url = m_ctx.backendUrl, token = m_ctx.backendToken,
                  session = m_ctx.sessionId;
    auto *watcher = new QFutureWatcher<BackendSessionDatasetsResult>();
    QPointer<LteCoDialog> self(this);
    connect(watcher, &QFutureWatcher<BackendSessionDatasetsResult>::finished, watcher,
            [self, watcher]() {
                const auto listing = watcher->result();
                watcher->deleteLater();
                if (!self)
                    return;
                if (!listing.valid) {
                    self->m_datasetNote->setText(
                            u"Could not list the session's datasets: %1"_s.arg(listing.error));
                    return;
                }
                QVector<DatasetChoice> out;
                QHash<QString, int> byPath;
                for (const auto &d : listing.datasets) {
                    if (d.kind != u"cube"_s && d.kind != u"image"_s)
                        continue;
                    const QString name = QFileInfo(d.path).fileName();
                    const QString shape = d.kind == u"cube"_s
                            ? u"%1×%2×%3"_s.arg(d.width).arg(d.height).arg(d.depth)
                            : u"%1×%2"_s.arg(d.width).arg(d.height);
                    DatasetChoice c{ d.datasetId, d.kind, d.path,
                                     u"%1  (%2, %3)"_s.arg(name.isEmpty() ? d.datasetId : name,
                                                           kindText(d.kind), shape) };
                    // One row per file: a file opened twice is registered twice.
                    // The host's own id wins, so its regions stay usable.
                    const QString key = d.path.isEmpty() ? d.datasetId : d.path;
                    const auto it = byPath.constFind(key);
                    if (it == byPath.cend()) {
                        byPath.insert(key, out.size());
                        out.append(c);
                    } else if (d.datasetId == self->m_ctx.hostDatasetId) {
                        out[it.value()] = c;
                    }
                }
                self->fillDatasetCombos(out);
            });
    watcher->setFuture(QtConcurrent::run([url, token, session]() {
        BackendClient c(url, token);
        c.setSessionId(session);
        return c.listSessionDatasets(session);
    }));
}

void LteCoDialog::fillDatasetCombos(const QVector<DatasetChoice> &datasets)
{
    m_datasets = datasets;
    const lte::LineGuess hostLine = lte::guessLine(m_ctx.hostRestFreqHz);
    QHash<QString, int> guessed;
    int hostIndex = -1;
    for (int i = 0; i < datasets.size(); ++i) {
        if (datasets.at(i).id == m_ctx.hostDatasetId)
            hostIndex = i;
        const QString species = lte::guessSpecies(QFileInfo(datasets.at(i).path).fileName());
        if (!species.isEmpty() && !guessed.contains(species))
            guessed.insert(species, i);
    }
    // The rest frequency of the window's own data outranks a file name: a
    // moment map named "…13co…" listed first must not take the host's place,
    // or the host's regions would be disabled.
    if (hostIndex >= 0 && !hostLine.species.isEmpty()) {
        for (auto it = guessed.begin(); it != guessed.end();)
            it = it.value() == hostIndex ? guessed.erase(it) : std::next(it);
        guessed.insert(hostLine.species, hostIndex);
    }
    const QString kept13 = m_ds13->currentData().toString();
    const QString kept18 = m_ds18->currentData().toString();
    const QString kept12 = m_ds12->currentData().toString();
    const bool refill = m_runBtn->isEnabled();
    const auto fill = [&](QComboBox *combo, const QString &species, bool optional) {
        QSignalBlocker block(combo);
        combo->clear();
        if (optional)
            combo->addItem(u"None"_s);
        for (const DatasetChoice &d : datasets)
            combo->addItem(d.label, d.id);
        combo->setEnabled(true);
        const int at = guessed.value(species, -1);
        combo->setCurrentIndex(at >= 0 ? at + (optional ? 1 : 0) : (optional ? 0 : -1));
        if (!optional)
            combo->setPlaceholderText(u"Choose a dataset"_s);
    };
    fill(m_ds13, lte::k13co, false);
    fill(m_ds18, lte::kC18o, false);
    fill(m_ds12, lte::k12co, true);
    if (m_ds13->currentIndex() < 0 && !m_ctx.hostDatasetId.isEmpty()) {
        const int host = m_ds13->findData(m_ctx.hostDatasetId);
        bool hostTaken = false;
        for (QComboBox *c : { m_ds18, m_ds12 })
            hostTaken = hostTaken || c->currentData().toString() == m_ctx.hostDatasetId;
        if (host >= 0 && !hostTaken)
            m_ds13->setCurrentIndex(host);
    }
    // A second listing (the dialog raised again) keeps what the user chose.
    if (refill) {
        for (auto [combo, id] : { std::pair{ m_ds13, kept13 }, std::pair{ m_ds18, kept18 },
                                  std::pair{ m_ds12, kept12 } }) {
            const int at = combo->findData(id);
            if (at >= 0) {
                QSignalBlocker block(combo);
                combo->setCurrentIndex(at);
            }
        }
    }
    m_datasetNote->setText(
            datasets.isEmpty()
                    ? u"No cube or 2-D map is open in this session."_s
                    : u"Cubes and 2-D maps open in this session. C¹⁸O and ¹²CO are "
                      u"reprojected onto the ¹³CO grid when their sky grids differ. Beams "
                      u"are not matched: smooth to a common resolution first."_s);
    m_runBtn->setEnabled(!datasets.isEmpty());
    refreshRegionsEnabled();
}

void LteCoDialog::refreshRegionsEnabled()
{
    const bool onHostGrid = !m_ctx.hostDatasetId.isEmpty()
            && m_ds13->currentData().toString() == m_ctx.hostDatasetId;
    m_regionList->setEnabled(onHostGrid && !m_ctx.regions.isEmpty());
    if (m_ctx.regions.isEmpty())
        m_regionNote->setText(u"No regions drawn in this window: the results come as maps "
                              u"only. Draw regions on the ¹³CO data for a table per region."_s);
    else if (!onHostGrid)
        m_regionNote->setText(u"Regions are measured on the ¹³CO grid: this window's regions "
                              u"are used when ¹³CO is this window's dataset."_s);
    else
        m_regionNote->setText(u"One row per region, from the region's mean W of each line; "
                              u"σ counts one independent value per beam."_s);
}

LteCoDialog::DatasetChoice LteCoDialog::chosen(QComboBox *combo) const
{
    const QString id = combo->currentData().toString();
    for (const DatasetChoice &d : m_datasets)
        if (d.id == id)
            return d;
    return {};
}

void LteCoDialog::run()
{
    if (m_busy)
        return;
    lte::Inputs in;
    in.transition = m_transition->currentData().toString();
    const DatasetChoice d13 = chosen(m_ds13), d18 = chosen(m_ds18), d12 = chosen(m_ds12);
    in.dataset13co = d13.id;
    in.datasetC18o = d18.id;
    in.dataset12co = d12.id;

    struct Field { QLineEdit *edit; std::optional<double> *target; QString name; };
    const Field fields[] = {
        { m_vMin, &in.vMinKms, u"the velocity window"_s },
        { m_vMax, &in.vMaxKms, u"the velocity window"_s },
        { m_eta13, &in.eta13co, u"η_mb ¹³CO"_s },
        { m_eta18, &in.etaC18o, u"η_mb C¹⁸O"_s },
        { m_eta12, &in.eta12co, u"η_mb ¹²CO"_s },
        { m_xRatio, &in.xRatio, u"X(¹³CO)/X(C¹⁸O)"_s },
        { m_rGc, &in.rGcKpc, u"R_GC"_s },
        { m_distance, &in.distanceKpc, u"the distance"_s },
        { m_tEx, &in.tExK, u"T_ex"_s },
        { m_x12co, &in.x12co, u"X(¹²CO)"_s },
    };
    for (const Field &f : fields) {
        *f.target = lte::optionalNumber(f.edit->text());
        if (!f.edit->text().trimmed().isEmpty() && !*f.target) {
            QMessageBox::warning(this, kTitle, u"%1 is not a number."_s.arg(f.name));
            return;
        }
    }
    in.carbonGradient = m_gradient->currentData().toString();
    in.h2From = m_h2From->currentData().toString();
    in.snrMin = m_snr->value();

    QStringList regionNames;
    if (m_regionList->isEnabled()) {
        for (int i = 0; i < m_regionList->count() && i < m_ctx.regions.size(); ++i) {
            if (m_regionList->item(i)->checkState() != Qt::Checked)
                continue;
            QJsonObject canonical = m_ctx.regions.at(i).canonical;
            canonical.remove(u"props"_s);
            in.regions.append(canonical);
            regionNames.append(m_ctx.regions.at(i).name);
        }
    }

    const QString problem = lte::validate(in, d13.kind, d18.kind, d12.kind);
    if (!problem.isEmpty()) {
        QMessageBox::warning(this, kTitle, problem);
        return;
    }

    const QJsonObject body = lte::requestBody(in);
    QStringList prov{
        u"VisIVO Visual Analytics: LTE analysis of CO isotopologues (POST /v1/products/lte_co)"_s,
        u"Date: %1"_s.arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)),
        u"Backend: %1"_s.arg(m_ctx.backendUrl),
        u"13CO: %1"_s.arg(d13.path.isEmpty() ? d13.id : d13.path),
        u"C18O: %1"_s.arg(d18.path.isEmpty() ? d18.id : d18.path),
    };
    if (!d12.id.isEmpty())
        prov << u"12CO: %1"_s.arg(d12.path.isEmpty() ? d12.id : d12.path);
    if (in.vMinKms && in.vMaxKms)
        prov << u"Velocity window: %1 to %2 km/s"_s.arg(*in.vMinKms, 0, 'g', 10)
                        .arg(*in.vMaxKms, 0, 'g', 10);
    if (!regionNames.isEmpty())
        prov << u"Regions: %1"_s.arg(regionNames.join(u"; "_s));
    prov << u"Request: %1"_s.arg(QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact)));

    const QString url = m_ctx.backendUrl, token = m_ctx.backendToken, session = m_ctx.sessionId;
    const int requestId = ++m_requestId;
    setBusy(true);
    m_progressSheet = visivo::showToolProgressSheet(this, u"Computing the LTE analysis"_s);

    auto *watcher = new QFutureWatcher<BackendLteCoResult>();
    QPointer<LteCoDialog> self(this);
    connect(watcher, &QFutureWatcher<BackendLteCoResult>::finished, watcher,
            [self, watcher, requestId, in, session, regionNames, prov]() {
                const BackendLteCoResult result = watcher->result();
                watcher->deleteLater();
                if (!self)
                    return;
                visivo::closeToolProgressSheet(self->m_progressSheet);
                if (requestId != self->m_requestId)
                    return;
                self->setBusy(false);
                if (!result.valid) {
                    DiagnosticsManager::instance()->publish(
                            DiagnosticsLevel::Error, DiagnosticsCategory::Scientific,
                            u"LteCoDialog"_s, result.error, in.dataset13co, session, u"lte_co"_s);
                    self->showFailure(result.error);
                    return;
                }
                self->m_lastInputs = in;
                self->m_lastRegionNames = regionNames;
                self->m_lastProvenance = prov;
                self->showResult(lte::parseResponse(result.payload), in);
            });
    watcher->setFuture(QtConcurrent::run([url, token, session, body]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        return client.lteCo(body);
    }));
}

void LteCoDialog::showFailure(const QString &error)
{
    const QStringList missing = lte::etaRequiredFor(error);
    if (!missing.isEmpty()) {
        markEtaRequired(missing);
        QStringList names;
        for (const QString &s : missing)
            names << lte::speciesLabel(s);
        QMessageBox::warning(
                this, kTitle,
                u"η_mb is required for %1: the header names no main-beam efficiency.\n\n"
                u"Type it under Main-beam efficiency (for CHIMPS ¹³CO and C¹⁸O 3–2, Rigby "
                u"et al. 2016 adopt 0.72), or 1 if the data are already T_mb."_s
                        .arg(names.join(u", "_s)));
        m_tabs->setCurrentIndex(0);
        return;
    }
    // The backend names its request fields; the dialog has no noise fields.
    if (error.contains(u"rms_13co"_s)) {
        QMessageBox::warning(this, kTitle,
                             u"Fewer than 10 channels lie outside the velocity window, so the "
                             u"noise cannot be measured: narrow the window, or use a cube with "
                             u"more line-free channels."_s);
        return;
    }
    QMessageBox::warning(this, kTitle, lte::prettySpecies(error));
}

void LteCoDialog::markEtaRequired(const QStringList &species)
{
    const QHash<QString, QLineEdit *> edits{ { lte::k13co, m_eta13 },
                                             { lte::kC18o, m_eta18 },
                                             { lte::k12co, m_eta12 } };
    for (const QString &s : species) {
        if (QLineEdit *e = edits.value(s)) {
            e->setPlaceholderText(u"required: the header names none"_s);
            e->setStyleSheet(u"QLineEdit { border: 1px solid #d9534f; }"_s);
        }
    }
    if (QLineEdit *first = edits.value(species.value(0)))
        first->setFocus();
}

void LteCoDialog::showResult(const lte::Response &response, const lte::Inputs &inputs)
{
    m_last = response;

    m_table->setRowCount(response.regions.size());
    for (int row = 0; row < response.regions.size(); ++row) {
        const lte::RegionResult &r = response.regions.at(row);
        const QString name = row < m_lastRegionNames.size() ? m_lastRegionNames.at(row)
                                                            : u"Region %1"_s.arg(r.index + 1);
        const QStringList cells = lte::tableRow(r, name);
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells.at(c));
            if (c == cells.size() - 1 && !r.note.isEmpty())
                item->setToolTip(lte::prettySpecies(r.note));
            m_table->setItem(row, c, item);
        }
    }

    m_mapList->clear();
    for (const lte::MapResult &m : response.maps) {
        const QString range = m.quantity == u"flag"_s
                ? QString()
                : u"  %1 … %2"_s.arg(lte::fmt(m.rangeMin), lte::fmt(m.rangeMax));
        auto *item = new QListWidgetItem(lte::mapLabel(m.quantity) + range, m_mapList);
        item->setData(Qt::UserRole, m.outputPath);
        item->setData(Qt::UserRole + 1, lte::mapLabel(m.quantity));
        item->setToolTip(m.outputPath);
    }
    if (m_mapList->count() > 0)
        m_mapList->setCurrentRow(0);

    QStringList text = lte::assumptionLines(response.assumptions);
    if (!response.pixelFlags.isEmpty()) {
        QStringList counts;
        for (const auto &f : response.pixelFlags)
            counts << u"%1 %2"_s.arg(lte::pixelFlagLabel(f.first)).arg(f.second);
        text << u"Pixels: %1"_s.arg(counts.join(u", "_s));
    }
    for (const QString &w : response.warnings)
        text << u"Warning: %1"_s.arg(lte::prettySpecies(w));
    m_assumptions->setPlainText(text.join(u'\n'));

    QStringList used;
    for (const lte::EtaUsed &e : lte::etaResolution(response.assumptions)) {
        used << u"%1 %2 (%3)"_s.arg(e.species == u"all"_s ? u"all lines"_s
                                                           : lte::speciesLabel(e.species),
                                    lte::fmt(e.value, 4), lte::prettySpecies(e.source));
    }
    if (!used.isEmpty())
        m_etaNote->setText(u"Last run used: %1."_s.arg(used.join(u"; "_s)));

    QString summary = response.regions.isEmpty()
            ? u"%1 maps written (%2 × %3 pixels); no regions were measured."_s
                      .arg(response.maps.size()).arg(response.width).arg(response.height)
            : u"%1 region(s) measured, %2 maps written. Values are on the T_mb scale."_s
                      .arg(response.regions.size()).arg(response.maps.size());
    if (!inputs.distanceKpc && !response.regions.isEmpty())
        summary += u" Give a distance for the mass."_s;
    m_resultNote->setText(summary);
    m_csvBtn->setEnabled(true);
    m_tabs->setCurrentIndex(1);

    QVariantMap params;
    params.insert(u"Transition"_s, lte::transitionLabel(inputs.transition));
    params.insert(u"¹³CO"_s, chosen(m_ds13).label);
    params.insert(u"C¹⁸O"_s, chosen(m_ds18).label);
    if (!inputs.dataset12co.isEmpty())
        params.insert(u"¹²CO"_s, chosen(m_ds12).label);
    if (inputs.vMinKms && inputs.vMaxKms)
        params.insert(u"Velocity window"_s,
                      u"%1 to %2 km/s"_s.arg(*inputs.vMinKms, 0, 'g', 10)
                              .arg(*inputs.vMaxKms, 0, 'g', 10));
    params.insert(u"Assumptions"_s, lte::assumptionLines(response.assumptions).join(u"\n"_s));
    QStringList maps;
    for (const lte::MapResult &m : response.maps)
        maps << m.outputPath;
    params.insert(u"Maps"_s, maps.join(u"\n"_s));
    if (!m_lastRegionNames.isEmpty())
        params.insert(u"Regions"_s, m_lastRegionNames.join(u", "_s));
    emit runRecorded(u"CO isotopologues (LTE), %1"_s.arg(lte::transitionLabel(inputs.transition)),
                     params);
}

void LteCoDialog::openSelectedMap()
{
    QListWidgetItem *item = m_mapList->currentItem();
    if (!item)
        return;
    emit openMapRequested(item->data(Qt::UserRole).toString(),
                          u"LTE %1"_s.arg(item->data(Qt::UserRole + 1).toString()));
}

void LteCoDialog::saveCsv()
{
    if (!m_last.valid)
        return;
    QString base = m_lastInputs.dataset13co;
    for (const DatasetChoice &d : m_datasets)
        if (d.id == m_lastInputs.dataset13co && !d.path.isEmpty())
            base = QFileInfo(d.path).completeBaseName();
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save the LTE Table"_s, base + u"_lte.csv"_s, u"CSV (*.csv)"_s);
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, kTitle, u"Could not write %1: %2"_s.arg(path, file.errorString()));
        return;
    }
    file.write(lte::toCsv(m_last, m_lastRegionNames, m_lastProvenance).toUtf8());
    if (!file.commit())
        QMessageBox::warning(this, kTitle, u"Could not write %1: %2"_s.arg(path, file.errorString()));
}

void LteCoDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_tabs->widget(0)->setEnabled(!busy);
    m_runBtn->setEnabled(!busy && !m_datasets.isEmpty());
    m_openMapBtn->setEnabled(!busy && m_mapList->currentRow() >= 0);
    m_csvBtn->setEnabled(!busy && m_last.valid);
}

void LteCoDialog::closeEvent(QCloseEvent *event)
{
    if (m_busy && !visivo::confirmAbandonRunningCompute(this, kTitle, u"The LTE analysis"_s)) {
        event->ignore();
        return;
    }
    event->accept();
}
