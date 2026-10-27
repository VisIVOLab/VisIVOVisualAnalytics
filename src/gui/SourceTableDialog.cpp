#include "SourceTableDialog.h"

#include "VlkbPosString.h"
#include "vlkb/VlkbQueryService.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QProgressBar>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;
namespace vk = visivo::vlkb;

namespace {
// The same allowance the VLKB panel gives a single search.
constexpr int kSearchTimeoutMs = 120000;
// What the archive's batch job accepts (VISIVO_VLKB_MCUTOUT_MAX_ITEMS default).
constexpr int kMaxCutouts = 500;

enum Tab { SourcesTab = 0, ImagesTab = 1, CubesTab = 2 };

QColor coverageColor(vk::Coverage c)
{
    // Translucent, so the cells stay readable in light and dark themes.
    switch (c) {
    case vk::Coverage::Full:
        return QColor(46, 160, 67, 150);
    case vk::Coverage::Partial:
        return QColor(210, 160, 0, 150);
    case vk::Coverage::None:
        break;
    }
    return QColor(200, 60, 60, 120);
}

QTableWidgetItem *checkItem(bool checked)
{
    auto *item = new QTableWidgetItem();
    item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

QTableWidgetItem *textItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}
/// Tick or untick column 0 of every row.
void setAllTicks(QTableWidget *table, bool on)
{
    for (int r = 0; r < table->rowCount(); ++r)
        if (QTableWidgetItem *it = table->item(r, 0))
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

/// @p table with a title and "All" / "None" buttons above it.
QWidget *withTickButtons(QTableWidget *table, const QString &title)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(title));
    row->addStretch(1);
    auto *all = new QToolButton;
    all->setText(u"All"_s);
    auto *none = new QToolButton;
    none->setText(u"None"_s);
    QObject::connect(all, &QToolButton::clicked, table, [table]() { setAllTicks(table, true); });
    QObject::connect(none, &QToolButton::clicked, table, [table]() { setAllTicks(table, false); });
    row->addWidget(all);
    row->addWidget(none);
    v->addLayout(row);
    v->addWidget(table, 1);
    return w;
}
}   // namespace

SourceTableDialog::SourceTableDialog(const QString &vlkbUrl, AuthWrapper *auth, QWidget *parent)
    : QDialog(parent), m_vlkbUrl(vlkbUrl), m_auth(auth)
{
    setWindowTitle(u"VLKB Cutouts for a Source Table"_s);
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    resize(1000, 640);
    buildUi();
}

SourceTableDialog::~SourceTableDialog()
{
    if (m_service)
        m_service->disconnect(this);
}

void SourceTableDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    root->addWidget(m_tabs);

    // ── Sources ──────────────────────────────────────────────────────────────
    // Top to bottom, in the order the user works: the table, how to search it,
    // then the sources themselves, which get all the remaining room.
    auto *sources = new QWidget;
    auto *sourcesLayout = new QVBoxLayout(sources);

    auto *fileBox = new QGroupBox(u"Source table"_s);
    auto *fileLayout = new QGridLayout(fileBox);
    m_openButton = new QPushButton(u"Open file…"_s);
    connect(m_openButton, &QPushButton::clicked, this, &SourceTableDialog::openFile);
    m_fileLabel = new QLabel(u"No table yet — open one, or drop the file on this window."_s);
    m_fileLabel->setWordWrap(true);
    auto *hint = new QLabel(u"A header line, then one source per line with an identifier and "
                            u"galactic l, b. Tab, comma, semicolon or space separated; lines "
                            u"starting with # are ignored."_s);
    hint->setWordWrap(true);
    hint->setEnabled(false);   // secondary text, in the theme's muted colour
    fileLayout->addWidget(m_openButton, 0, 0);
    fileLayout->addWidget(m_fileLabel, 0, 1);
    fileLayout->addWidget(hint, 1, 0, 1, 2);
    fileLayout->setColumnStretch(1, 1);
    sourcesLayout->addWidget(fileBox);

    auto *searchBox = new QGroupBox(u"Search"_s);
    auto *grid = new QGridLayout(searchBox);
    m_idBox = new QComboBox;
    m_lonBox = new QComboBox;
    m_latBox = new QComboBox;
    const auto label = [](const QString &text, QWidget *buddy) {
        auto *l = new QLabel(text);
        l->setBuddy(buddy);
        return l;
    };
    grid->addWidget(label(u"ID"_s, m_idBox), 0, 0);
    grid->addWidget(m_idBox, 0, 1);
    grid->addWidget(label(u"Longitude (l)"_s, m_lonBox), 0, 2);
    grid->addWidget(m_lonBox, 0, 3);
    grid->addWidget(label(u"Latitude (b)"_s, m_latBox), 0, 4);
    grid->addWidget(m_latBox, 0, 5);

    m_selectionBox = new QComboBox;
    m_selectionBox->addItems({ u"Cone around each source"_s, u"Box around each source"_s });
    const auto degrees = [](double value) {
        auto *s = new QDoubleSpinBox;
        s->setDecimals(4);
        s->setRange(0.0001, 10.0);
        s->setSingleStep(0.05);
        s->setSuffix(u"°"_s);
        s->setValue(value);
        return s;
    };
    m_regionStack = new QStackedWidget;
    auto *conePage = new QWidget;
    auto *coneRow = new QHBoxLayout(conePage);
    coneRow->setContentsMargins(0, 0, 0, 0);
    m_radius = degrees(0.1);
    coneRow->addWidget(label(u"radius"_s, m_radius));
    coneRow->addWidget(m_radius);
    coneRow->addStretch(1);
    auto *boxPage = new QWidget;
    auto *boxRow = new QHBoxLayout(boxPage);
    boxRow->setContentsMargins(0, 0, 0, 0);
    m_dl = degrees(0.1);
    m_db = degrees(0.1);
    m_dl->setToolTip(u"Half-width of the box in galactic longitude."_s);
    m_db->setToolTip(u"Half-width of the box in galactic latitude."_s);
    boxRow->addWidget(label(u"half-width dl"_s, m_dl));
    boxRow->addWidget(m_dl);
    boxRow->addWidget(label(u"db"_s, m_db));
    boxRow->addWidget(m_db);
    boxRow->addStretch(1);
    m_regionStack->addWidget(conePage);
    m_regionStack->addWidget(boxPage);
    m_regionStack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    connect(m_selectionBox, &QComboBox::currentIndexChanged, this,
            [this](int) { updateRegionControls(); });
    grid->addWidget(label(u"Region"_s, m_selectionBox), 1, 0);
    grid->addWidget(m_selectionBox, 1, 1);
    grid->addWidget(m_regionStack, 1, 2, 1, 2);

    m_queryButton = new QPushButton(u"Query"_s);
    m_queryButton->setEnabled(false);
    m_queryButton->setDefault(true);
    connect(m_queryButton, &QPushButton::clicked, this, &SourceTableDialog::startQuery);
    m_cancelButton = new QPushButton(u"Stop"_s);
    m_cancelButton->setEnabled(false);
    connect(m_cancelButton, &QPushButton::clicked, this, &SourceTableDialog::cancelQuery);
    auto *queryRow = new QHBoxLayout;
    queryRow->addStretch(1);
    queryRow->addWidget(m_cancelButton);
    queryRow->addWidget(m_queryButton);
    grid->addLayout(queryRow, 1, 4, 1, 2);
    for (int c : { 1, 3, 5 })
        grid->setColumnStretch(c, 1);
    sourcesLayout->addWidget(searchBox);

    m_sourcesTable = new QTableWidget;
    m_sourcesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sourcesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sourcesTable->verticalHeader()->setVisible(false);
    m_sourcesTable->horizontalHeader()->setStretchLastSection(true);
    sourcesLayout->addWidget(withTickButtons(m_sourcesTable, u"Sources to query"_s), 1);
    m_tabs->addTab(sources, u"Sources"_s);

    // ── Images / Cubes: the coverage preview ─────────────────────────────────
    const auto previewTab = [this](QPointer<QTableWidget> &table, QPointer<QTreeWidget> &tree,
                                   QList<QPair<vk::ProductKey, QTreeWidgetItem *>> *checks) {
        auto *split = new QSplitter(Qt::Horizontal);
        split->setChildrenCollapsible(false);

        // Left: the products to request, as a compact tree — one checkable
        // survey per branch, its species/transitions as leaves; ticking a
        // survey ticks all of its products.
        auto *left = new QWidget;
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->setContentsMargins(0, 0, 6, 0);
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(u"Products to request"_s));
        row->addStretch(1);
        auto *all = new QToolButton;
        all->setText(u"All"_s);
        auto *none = new QToolButton;
        none->setText(u"None"_s);
        const auto tickProducts = [this, checks](bool on) {
            for (const auto &pair : *checks)
                pair.second->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
            updateRequestCount();
        };
        connect(all, &QToolButton::clicked, this, [tickProducts]() { tickProducts(true); });
        connect(none, &QToolButton::clicked, this, [tickProducts]() { tickProducts(false); });
        row->addWidget(all);
        row->addWidget(none);
        leftLayout->addLayout(row);
        tree = new QTreeWidget;
        tree->setHeaderHidden(true);
        tree->setRootIsDecorated(true);
        tree->setUniformRowHeights(true);
        tree->setMinimumWidth(220);
        connect(tree, &QTreeWidget::itemChanged, this, [this]() { updateRequestCount(); });
        leftLayout->addWidget(tree, 1);
        split->addWidget(left);

        // Right: the coverage matrix, with All / None and a legend.
        table = new QTableWidget;
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->verticalHeader()->setVisible(false);
        connect(table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
            if (item && item->column() == 0)
                updateRequestCount();
        });
        auto *right = withTickButtons(table, u"Sources to cut out"_s);
        right->layout()->setContentsMargins(6, 0, 0, 0);
        const auto swatch = [](vk::Coverage c) {
            const QColor col = coverageColor(c);
            return u"<span style=\"background-color: rgba(%1,%2,%3,%4)\">&nbsp;&nbsp;&nbsp;&nbsp;</span>"_s
                    .arg(col.red()).arg(col.green()).arg(col.blue()).arg(col.alphaF(), 0, 'f', 2);
        };
        auto *legend = new QLabel(
                u"%1 ✓ every product of the survey covers the source &nbsp; %2 ◐ some do "
                u"&nbsp; %3 – none does. Hover a cell for the details."_s
                        .arg(swatch(vk::Coverage::Full), swatch(vk::Coverage::Partial),
                             swatch(vk::Coverage::None)));
        legend->setTextFormat(Qt::RichText);
        legend->setWordWrap(true);
        right->layout()->addWidget(legend);
        split->addWidget(right);
        split->setStretchFactor(1, 1);
        split->setSizes({ 260, 740 });
        return split;
    };
    m_tabs->addTab(previewTab(m_imagesTable, m_imageProducts, &m_imageChecks), u"Images"_s);
    m_tabs->addTab(previewTab(m_cubesTable, m_cubeProducts, &m_cubeChecks), u"Cubes"_s);
    m_tabs->setTabEnabled(ImagesTab, false);
    m_tabs->setTabEnabled(CubesTab, false);

    // ── Bottom bar ───────────────────────────────────────────────────────────
    auto *bottom = new QHBoxLayout;
    m_status = new QLabel;
    m_status->setWordWrap(true);
    bottom->addWidget(m_status, 1);
    m_progress = new QProgressBar;
    m_progress->setMaximumWidth(200);
    m_progress->hide();
    bottom->addWidget(m_progress);
    m_requestButton = new QPushButton(u"Request cutouts…"_s);
    m_requestButton->setEnabled(false);
    m_requestButton->setToolTip(u"Send the best dataset of every ticked product, for every "
                                u"ticked source of the Images and Cubes tabs, as one "
                                u"asynchronous archive job."_s);
    connect(m_requestButton, &QPushButton::clicked, this, &SourceTableDialog::requestCutouts);
    auto *close = new QPushButton(u"Close"_s);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    bottom->addWidget(m_requestButton);
    bottom->addWidget(close);
    root->addLayout(bottom);

    updateRegionControls();
}

void SourceTableDialog::updateRegionControls()
{
    m_regionStack->setCurrentIndex(m_selectionBox->currentIndex() == 0 ? 0 : 1);
}

void SourceTableDialog::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
            this, u"Open a source table"_s, QDir::homePath(),
            u"Source tables (*.tsv *.txt *.tab *.csv *.dat);;All files (*)"_s);
    if (!path.isEmpty())
        loadFile(path);
}

bool SourceTableDialog::loadFile(const QString &path)
{
    // A new table replaces the sources the running queries belong to: refuse
    // rather than mix two runs' answers.
    if (m_cancelButton && m_cancelButton->isEnabled()) {
        QMessageBox::information(this, windowTitle(),
                                 u"Stop the running query before loading another table."_s);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, windowTitle(), file.errorString());
        return false;
    }
    vk::SourceTable table = vk::parseSourceTable(file.readAll());
    if (!table.error.isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
                             u"%1\n\n%2"_s.arg(QFileInfo(path).fileName(), table.error));
        return false;
    }
    m_table = table;
    m_results.clear();
    m_tabs->setTabEnabled(ImagesTab, false);
    m_tabs->setTabEnabled(CubesTab, false);
    m_requestButton->setEnabled(false);

    for (auto *box : { m_idBox.data(), m_lonBox.data(), m_latBox.data() }) {
        box->clear();
        box->addItems(m_table.columns);
    }
    const auto pick = [this](QComboBox *box, const QString &role, int fallback) {
        const int guess = vk::guessColumn(m_table.columns, role);
        box->setCurrentIndex(guess >= 0 ? guess : std::min(fallback, int(m_table.columns.size()) - 1));
    };
    pick(m_idBox, u"id"_s, 0);
    pick(m_lonBox, u"glon"_s, 1);
    pick(m_latBox, u"glat"_s, 2);

    m_fileLabel->setText(u"%1 — %2 sources, %3-separated"_s.arg(QFileInfo(path).fileName())
                                 .arg(m_table.rows.size())
                                 .arg(m_table.delimiter));
    m_fileLabel->setToolTip(path);
    showTable();
    m_queryButton->setEnabled(true);
    m_tabs->setCurrentIndex(SourcesTab);
    m_status->setText(m_table.warnings.isEmpty()
                              ? QString()
                              : u"Read with warnings: %1"_s.arg(m_table.warnings.join(u"; "_s)));
    return true;
}

void SourceTableDialog::showTable()
{
    m_sourcesTable->clear();
    QStringList headers{ QString() };
    headers << m_table.columns;
    m_sourcesTable->setColumnCount(int(headers.size()));
    m_sourcesTable->setHorizontalHeaderLabels(headers);
    m_sourcesTable->setRowCount(int(m_table.rows.size()));
    for (int r = 0; r < m_table.rows.size(); ++r) {
        m_sourcesTable->setItem(r, 0, checkItem(true));
        for (int c = 0; c < m_table.columns.size(); ++c)
            m_sourcesTable->setItem(r, c + 1, textItem(m_table.rows[r].value(c)));
    }
    m_sourcesTable->resizeColumnsToContents();
}

QString SourceTableDialog::regionFor(const vk::UserSource &s) const
{
    return m_selectionBox->currentIndex() == 0
                   ? visivo::circlePosString(s.glon, s.glat, m_radius->value())
                   : visivo::rangePosString(s.glon, s.glat, m_dl->value(), m_db->value());
}

void SourceTableDialog::setBusy(bool busy)
{
    m_queryButton->setEnabled(!busy && !m_table.rows.isEmpty());
    m_cancelButton->setEnabled(busy);
    m_progress->setVisible(busy);
    if (busy)
        m_requestButton->setEnabled(false);
    else
        updateRequestCount();
    for (auto *w : std::initializer_list<QWidget *>{ m_openButton, m_idBox, m_lonBox, m_latBox,
                                                     m_selectionBox, m_regionStack,
                                                     m_sourcesTable })
        w->setEnabled(!busy);
}

void SourceTableDialog::startQuery()
{
    QStringList problems;
    const QList<vk::UserSource> all = vk::pickSources(
            m_table, m_idBox->currentIndex(), m_lonBox->currentIndex(), m_latBox->currentIndex(),
            &problems);
    QList<vk::UserSource> chosen;
    for (const vk::UserSource &s : all) {
        const QTableWidgetItem *tick = m_sourcesTable->item(s.row, 0);
        if (tick && tick->checkState() == Qt::Checked)
            chosen << s;
    }
    if (chosen.isEmpty()) {
        QMessageBox::information(this, windowTitle(),
                                 problems.isEmpty()
                                         ? u"Tick at least one source."_s
                                         : u"No source can be queried:\n\n%1"_s.arg(
                                                   problems.mid(0, 10).join(u'\n')));
        return;
    }
    if (!problems.isEmpty()) {
        const auto answer = QMessageBox::question(
                this, windowTitle(),
                u"%1 row(s) will be skipped:\n\n%2%3\n\nQuery the other %4?"_s
                        .arg(problems.size())
                        .arg(problems.mid(0, 10).join(u'\n'))
                        .arg(problems.size() > 10 ? u"\n…"_s : QString())
                        .arg(chosen.size()));
        if (answer != QMessageBox::Yes)
            return;
    }
    m_pending = chosen;
    m_results.clear();
    m_total = int(chosen.size());
    setBusy(true);
    queryNext();
}

void SourceTableDialog::queryNext()
{
    if (m_pending.isEmpty()) {
        finishQuery();
        return;
    }
    QueriedSource q;
    q.source = m_pending.takeFirst();
    q.pos = regionFor(q.source);
    m_progress->setRange(0, m_total);
    m_progress->setValue(m_total - int(m_pending.size()) - 1);
    m_status->setText(u"Querying the VLKB: source %1 of %2 (%3)…"_s
                              .arg(m_total - m_pending.size())
                              .arg(m_total)
                              .arg(q.source.id));

    auto *svc = new VlkbQueryService(m_vlkbUrl, m_auth, this);
    m_service = svc;
    if (!m_watchdog) {
        m_watchdog = new QTimer(this);
        m_watchdog->setSingleShot(true);
    }
    m_watchdog->disconnect();
    // Each outcome records the source once and moves on; whichever arrives
    // first disconnects the others.
    const auto done = [this, svc, q](const QList<vk::InventoryEntry> &entries,
                                     const QString &error) {
        m_watchdog->stop();
        svc->disconnect(this);
        svc->deleteLater();
        QueriedSource r = q;
        r.entries = entries;
        r.error = error;
        m_results << r;
        queryNext();
    };
    connect(svc, &VlkbQueryService::searchFinished, this, [done](const QByteArray &votable) {
        done(vk::parseInventory(votable), QString());
    });
    connect(svc, &VlkbQueryService::searchFailed, this,
            [done](const QString &error) { done({}, error); });
    connect(m_watchdog, &QTimer::timeout, this, [done]() {
        done({}, u"The VLKB did not answer within %1 s."_s.arg(kSearchTimeoutMs / 1000));
    });
    m_watchdog->start(kSearchTimeoutMs);
    svc->searchPos(q.pos);
}

void SourceTableDialog::cancelQuery()
{
    m_pending.clear();
    if (m_watchdog)
        m_watchdog->stop();
    if (m_service) {
        m_service->disconnect(this);
        m_service->deleteLater();
    }
    finishQuery();
}

void SourceTableDialog::finishQuery()
{
    setBusy(false);
    if (m_results.isEmpty()) {
        m_status->setText(u"Stopped before any source was queried."_s);
        return;
    }
    buildPreview(u"image"_s, m_imagesTable, m_imageProducts, m_imageChecks);
    buildPreview(u"cube"_s, m_cubesTable, m_cubeProducts, m_cubeChecks);
    m_tabs->setTabEnabled(ImagesTab, true);
    m_tabs->setTabEnabled(CubesTab, true);
    m_tabs->setCurrentIndex(ImagesTab);

    int failed = 0;
    for (const QueriedSource &r : m_results)
        failed += r.error.isEmpty() ? 0 : 1;
    QString text = u"%1 of %2 sources queried"_s.arg(m_results.size()).arg(m_total);
    if (failed)
        text += u", %1 failed (see the row tooltips)"_s.arg(failed);
    text += u". Tick sources and products, then Request cutouts."_s;
    m_status->setText(text);
    updateRequestCount();
}

void SourceTableDialog::buildPreview(const QString &type, QTableWidget *table,
                                     QTreeWidget *productTree,
                                     QList<QPair<vk::ProductKey, QTreeWidgetItem *>> &productChecks)
{
    // Products of this type found anywhere, grouped by survey on the left.
    QList<QList<vk::InventoryEntry>> perSource;
    for (const QueriedSource &r : m_results)
        perSource << r.entries;
    QList<vk::ProductKey> products;
    for (const vk::ProductKey &k : vk::productsFound(perSource))
        if (k.type == type)
            products << k;
    QStringList surveys;
    for (const vk::ProductKey &k : products)
        if (!surveys.contains(k.survey))
            surveys << k.survey;

    {
        const QSignalBlocker quietTree(productTree);   // filling is not the user ticking
        productTree->clear();
        productChecks.clear();
        for (const QString &survey : surveys) {
            auto *branch = new QTreeWidgetItem(productTree, { survey });
            branch->setFlags(branch->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            for (const vk::ProductKey &k : products) {
                if (k.survey != survey)
                    continue;
                const QString band = k.species == k.transition
                                             ? k.species
                                             : u"%1 — %2"_s.arg(k.species, k.transition);
                auto *leaf = new QTreeWidgetItem(branch, { band });
                leaf->setFlags(leaf->flags() | Qt::ItemIsUserCheckable);
                leaf->setCheckState(0, Qt::Checked);
                productChecks << qMakePair(k, leaf);
            }
        }
        if (surveys.isEmpty())
            new QTreeWidgetItem(productTree, { type == u"image"_s ? u"No images found."_s
                                                                  : u"No cubes found."_s });
        productTree->expandAll();
    }

    // One row per source: designation, l, b, how many datasets, then one
    // coverage cell per survey.
    const QSignalBlocker quiet(table);   // filling is not the user ticking
    table->clear();
    QStringList headers{ QString(), u"designation"_s, u"glon"_s, u"glat"_s, u"# cutouts"_s };
    headers << surveys;
    table->setColumnCount(int(headers.size()));
    table->setHorizontalHeaderLabels(headers);
    table->setRowCount(int(m_results.size()));
    for (int r = 0; r < m_results.size(); ++r) {
        const QueriedSource &q = m_results[r];
        int count = 0;
        for (const vk::InventoryEntry &e : q.entries)
            count += e.type == type ? 1 : 0;
        table->setItem(r, 0, checkItem(count > 0));
        auto *name = textItem(q.source.id);
        if (!q.error.isEmpty()) {
            name->setToolTip(q.error);
            name->setForeground(QColor(200, 60, 60));
        }
        table->setItem(r, 1, name);
        table->setItem(r, 2, textItem(QString::number(q.source.glon, 'f', 4)));
        table->setItem(r, 3, textItem(QString::number(q.source.glat, 'f', 4)));
        table->setItem(r, 4, textItem(q.error.isEmpty() ? QString::number(count) : u"error"_s));
        for (int s = 0; s < surveys.size(); ++s) {
            QString detail;
            const vk::Coverage c = vk::coverageOf(q.entries, type, surveys[s], products, &detail);
            // A symbol as well as the colour, for readers who cannot tell the
            // green from the red.
            auto *cell = textItem(c == vk::Coverage::Full      ? u"✓"_s
                                  : c == vk::Coverage::Partial ? u"◐"_s
                                                               : u"–"_s);
            cell->setTextAlignment(Qt::AlignCenter);
            cell->setBackground(coverageColor(c));
            cell->setToolTip(detail);
            table->setItem(r, 5 + s, cell);
        }
    }
    table->resizeColumnsToContents();
}

void SourceTableDialog::collectRequest(QVector<BackendMcutoutItem> &items,
                                       QStringList &labels) const
{
    const auto collect = [&](const QTableWidget *table,
                             const QList<QPair<vk::ProductKey, QTreeWidgetItem *>> &checks) {
        if (!table)
            return;
        for (int r = 0; r < m_results.size() && r < table->rowCount(); ++r) {
            const QTableWidgetItem *tick = table->item(r, 0);
            if (!tick || tick->checkState() != Qt::Checked)
                continue;
            const QueriedSource &q = m_results[r];
            for (const auto &[key, box] : checks) {
                if (box->checkState(0) != Qt::Checked)
                    continue;
                vk::InventoryEntry best;
                if (!vk::bestEntry(q.entries, key, &best))
                    continue;
                items.append({ best.ivoID, q.pos, key.type, q.source.id });
                labels << u"%1 — %2"_s.arg(q.source.id, key.label());
            }
        }
    };
    collect(m_imagesTable, m_imageChecks);
    collect(m_cubesTable, m_cubeChecks);
}

void SourceTableDialog::updateRequestCount()
{
    if (!m_requestButton)
        return;
    QVector<BackendMcutoutItem> items;
    QStringList labels;
    collectRequest(items, labels);
    const bool busy = m_cancelButton && m_cancelButton->isEnabled();
    m_requestButton->setText(items.isEmpty() ? u"Request cutouts…"_s
                                             : u"Request %1 cutouts…"_s.arg(items.size()));
    m_requestButton->setEnabled(!busy && !items.isEmpty());
}

void SourceTableDialog::requestCutouts()
{
    QVector<BackendMcutoutItem> items;
    QStringList labels;
    collectRequest(items, labels);

    if (items.isEmpty()) {
        QMessageBox::information(this, windowTitle(),
                                 u"Nothing to request: tick at least one source that has "
                                 u"data, and one product, in the Images or Cubes tab."_s);
        return;
    }
    if (items.size() > kMaxCutouts) {
        QMessageBox::warning(this, windowTitle(),
                             u"%1 cutouts selected; one archive job takes at most %2. "
                             u"Untick some sources or products."_s.arg(items.size())
                                     .arg(kMaxCutouts));
        return;
    }
    emit cutoutsRequested(items, labels);
}

void SourceTableDialog::dragEnterEvent(QDragEnterEvent *event)
{
    const bool busy = m_cancelButton && m_cancelButton->isEnabled();
    if (!busy && event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1
        && event->mimeData()->urls().first().isLocalFile())
        event->acceptProposedAction();
}

void SourceTableDialog::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile()) {
        event->acceptProposedAction();
        loadFile(urls.first().toLocalFile());
    }
}
