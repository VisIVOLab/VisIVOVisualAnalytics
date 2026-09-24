// ArepoExtractDialog.cpp — see ArepoExtractDialog.h.

#include "ArepoExtractDialog.h"

#include "ToolProgressSheet.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace {

QString formatBytes(qint64 bytes)
{
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    if (bytes >= kGiB)
        return u"%1 GiB"_s.arg(bytes / kGiB, 0, 'f', 2);
    if (bytes >= kMiB)
        return u"%1 MiB"_s.arg(bytes / kMiB, 0, 'f', 1);
    return u"%1 KiB"_s.arg(bytes / 1024.0, 0, 'f', 0);
}

}   // namespace

ArepoExtractDialog::ArepoExtractDialog(const QString &snapshotPath, const QString &backendUrl,
                                       const QString &backendToken, QWidget *parent)
    : QDialog(parent),
      m_path(snapshotPath),
      m_backendUrl(backendUrl),
      m_backendToken(backendToken)
{
    setWindowTitle(u"AREPO snapshot — %1"_s.arg(QFileInfo(snapshotPath).fileName()));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(960, 620);

    buildUi();

    connect(&m_inspectWatcher, &QFutureWatcher<BackendArepoInspectResult>::finished,
            this, &ArepoExtractDialog::onInspectFinished);
    connect(&m_planWatcher, &QFutureWatcher<BackendArepoPlan>::finished,
            this, &ArepoExtractDialog::onPlanFinished);
    connect(&m_extractWatcher, &QFutureWatcher<BackendArepoExtractResult>::finished,
            this, &ArepoExtractDialog::onExtractFinished);

    setBusy(true, u"Reading the snapshot…"_s);
    const QString path = m_path;
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    m_inspectWatcher.setFuture(QtConcurrent::run([path, url, token]() {
        BackendClient client(url, token);
        return client.inspectArepoSnapshot(path);
    }));
}

ArepoExtractDialog::~ArepoExtractDialog()
{
    // Nothing to wait for: every worker captures by value, so a request still in
    // flight simply finishes into a watcher whose connections Qt has dropped.
    m_inspectWatcher.disconnect(this);
    m_planWatcher.disconnect(this);
    m_extractWatcher.disconnect(this);
}

void ArepoExtractDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter, 1);

    m_tree = new QTreeWidget(splitter);
    m_tree->setHeaderLabels({ u"Contents"_s, u"Value"_s });
    m_tree->header()->setStretchLastSection(true);
    m_tree->setMinimumWidth(320);
    splitter->addWidget(m_tree);

    auto *panel = new QWidget(splitter);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(8, 0, 0, 0);
    splitter->addWidget(panel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    auto *what = new QGroupBox(u"What to extract"_s, panel);
    auto *whatForm = new QFormLayout(what);

    m_level = new QSpinBox(what);
    m_level->setRange(0, 14);
    m_level->setValue(7);
    m_level->setToolTip(
            u"yt refinement level of the whole simulation domain: the window gets its own "
            u"fraction of 2^level cells, so a level means the same resolution whatever "
            u"window you ask for."_s);
    whatForm->addRow(u"Level"_s, m_level);

    m_particle = new QLineEdit(u"gas"_s, what);
    whatForm->addRow(u"Particle type"_s, m_particle);

    m_fields = new QLineEdit(u"density"_s, what);
    m_fields->setToolTip(u"One or more field names, separated by spaces or commas. "
                         u"Each one becomes its own FITS cube."_s);
    whatForm->addRow(u"Fields"_s, m_fields);
    panelLayout->addWidget(what);

    const auto spinBox = [](QWidget *parent, double value, const QString &suffix) {
        auto *box = new QDoubleSpinBox(parent);
        box->setRange(-1e6, 1e6);
        box->setDecimals(3);
        box->setValue(value);
        box->setSuffix(suffix);
        return box;
    };

    auto *window = new QGroupBox(u"Window"_s, panel);
    auto *windowForm = new QFormLayout(window);
    m_sizeX = spinBox(window, 20.0, u" kpc"_s);
    m_sizeY = spinBox(window, 20.0, u" kpc"_s);
    m_sizeZ = spinBox(window, 20.0, u" kpc"_s);
    m_sizeX->setMinimum(1e-3);
    m_sizeY->setMinimum(1e-3);
    m_sizeZ->setMinimum(1e-3);
    auto *sizeRow = new QWidget(window);
    auto *sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->addWidget(m_sizeX);
    sizeLayout->addWidget(m_sizeY);
    sizeLayout->addWidget(m_sizeZ);
    windowForm->addRow(u"Size (x, y, z)"_s, sizeRow);

    m_offsetX = spinBox(window, 0.0, u" kpc"_s);
    m_offsetY = spinBox(window, 0.0, u" kpc"_s);
    m_offsetZ = spinBox(window, 0.0, u" kpc"_s);
    auto *offsetRow = new QWidget(window);
    auto *offsetLayout = new QHBoxLayout(offsetRow);
    offsetLayout->setContentsMargins(0, 0, 0, 0);
    offsetLayout->addWidget(m_offsetX);
    offsetLayout->addWidget(m_offsetY);
    offsetLayout->addWidget(m_offsetZ);
    windowForm->addRow(u"Offset from centre"_s, offsetRow);

    m_boxSizeLabel = new QLabel(u"Box size: —"_s, window);
    windowForm->addRow(QString(), m_boxSizeLabel);
    panelLayout->addWidget(window);

    // The cost of the job, before the job. The legacy asked "level 9+ may take
    // minutes and produce 1+ GB, continue?" from a fixed threshold that knew
    // nothing about the window actually requested.
    m_planLabel = new QLabel(u"Grid: —"_s, panel);
    m_planLabel->setWordWrap(true);
    m_planLabel->setTextFormat(Qt::RichText);
    panelLayout->addWidget(m_planLabel);
    panelLayout->addStretch(1);

    for (QDoubleSpinBox *box : { m_sizeX.data(), m_sizeY.data(), m_sizeZ.data() })
        connect(box, &QDoubleSpinBox::valueChanged, this, &ArepoExtractDialog::refreshPlan);
    connect(m_level, &QSpinBox::valueChanged, this, &ArepoExtractDialog::refreshPlan);

    auto *actions = new QWidget(this);
    auto *actionLayout = new QHBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    m_statusLabel = new QLabel(QString(), actions);
    actionLayout->addWidget(m_statusLabel, 1);

    m_extractButton = new QPushButton(u"Extract"_s, actions);
    m_extractButton->setDefault(true);
    connect(m_extractButton, &QPushButton::clicked, this, &ArepoExtractDialog::extract);
    actionLayout->addWidget(m_extractButton);

    auto *closeButton = new QPushButton(u"Close"_s, actions);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    actionLayout->addWidget(closeButton);
    layout->addWidget(actions);
}

// ── inspection ────────────────────────────────────────────────────────────────

void ArepoExtractDialog::onInspectFinished()
{
    setBusy(false);
    const BackendArepoInspectResult result = m_inspectWatcher.result();
    if (!result.valid) {
        QMessageBox::warning(this, u"AREPO snapshot"_s, result.error);
        reject();
        return;
    }
    applyInspection(result);
    refreshPlan();
}

void ArepoExtractDialog::applyInspection(const BackendArepoInspectResult &result)
{
    m_tree->clear();
    auto *root = new QTreeWidgetItem(m_tree, { u"/"_s });
    populateTree(result.tree, root);
    root->setExpanded(true);
    m_tree->resizeColumnToContents(0);

    if (std::isfinite(result.boxSizeKpc)) {
        m_boxSizeLabel->setText(u"Box size: %1 kpc"_s.arg(result.boxSizeKpc, 0, 'g', 6));
        // A window larger than the box is not a window.
        for (QDoubleSpinBox *box : { m_sizeX.data(), m_sizeY.data(), m_sizeZ.data() })
            box->setMaximum(result.boxSizeKpc);
        const double suggested = std::min(20.0, result.boxSizeKpc);
        for (QDoubleSpinBox *box : { m_sizeX.data(), m_sizeY.data(), m_sizeZ.data() })
            box->setValue(suggested);
    } else {
        m_boxSizeLabel->setText(u"Box size: not declared by this snapshot"_s);
    }

    // The first particle type the file carries is a better default than "gas"
    // when the snapshot does not have one.
    if (!result.particleTypes.isEmpty() && !result.particleTypes.contains(u"PartType0"_s))
        m_particle->setText(result.particleTypes.firstKey());

    m_ytAvailable = result.ytAvailable;
    if (!m_ytAvailable) {
        m_extractButton->setEnabled(false);
        m_statusLabel->setText(
                u"This backend cannot extract: the 'yt' package is not installed on it. "
                u"Browsing and planning still work."_s);
    }
}

void ArepoExtractDialog::populateTree(const BackendArepoNode &node, QTreeWidgetItem *parent)
{
    for (const auto &attribute : node.attributes) {
        auto *item = new QTreeWidgetItem(parent, { attribute.first, attribute.second });
        item->setForeground(0, QBrush(QColor(154, 174, 196)));
    }
    for (const BackendArepoNode &child : node.children) {
        QString summary;
        if (child.kind == u"dataset"_s) {
            QStringList dims;
            for (int extent : child.shape)
                dims.append(QString::number(extent));
            summary = u"%1  [%2]"_s.arg(child.dtype, dims.join(u" × "_s));
        }
        auto *item = new QTreeWidgetItem(parent, { child.name, summary });
        populateTree(child, item);
    }
}

// ── planning ──────────────────────────────────────────────────────────────────

QVector<double> ArepoExtractDialog::windowKpc() const
{
    return { m_sizeX->value(), m_sizeY->value(), m_sizeZ->value() };
}

QVector<double> ArepoExtractDialog::offsetKpc() const
{
    return { m_offsetX->value(), m_offsetY->value(), m_offsetZ->value() };
}

void ArepoExtractDialog::refreshPlan()
{
    // Anything shown now describes settings that have just changed, so it is
    // stale until the answer comes back.
    m_planLabel->setText(u"<b>Grid:</b> …"_s);
    m_extractButton->setEnabled(false);
    if (m_planWatcher.isRunning()) {
        m_planDirty = true;   // ask again the moment this one lands
        return;
    }
    m_planDirty = false;
    const QString path = m_path;
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const int level = m_level->value();
    const QVector<double> window = windowKpc();
    const QVector<double> offset = offsetKpc();
    m_planWatcher.setFuture(QtConcurrent::run([path, url, token, level, window, offset]() {
        BackendClient client(url, token);
        return client.planArepoExtraction(path, level, window, offset);
    }));
}

void ArepoExtractDialog::onPlanFinished()
{
    if (m_planDirty) {
        // The parameters moved while this was in flight: its answer describes
        // settings nobody is looking at any more.
        refreshPlan();
        return;
    }
    const BackendArepoPlan plan = m_planWatcher.result();
    if (!plan.valid) {
        m_planLabel->setText(u"<b>Grid:</b> %1"_s.arg(plan.error.toHtmlEscaped()));
        m_extractButton->setEnabled(false);
        return;
    }
    m_extractButton->setEnabled(m_ytAvailable);

    QStringList dims;
    for (int extent : plan.shape)
        dims.append(QString::number(extent));
    QStringList cells;
    for (double size : plan.cellSizePc)
        cells.append(QString::number(size, 'f', 2));
    QString text = u"<b>Grid:</b> %1 cells &nbsp; <b>Cell:</b> %2 pc &nbsp; "
                   u"<b>Per field:</b> %3"_s.arg(dims.join(u" × "_s), cells.join(u" × "_s),
                                                formatBytes(plan.bytesEstimate));
    // The cell size is fixed by the level, so the extracted extent is the
    // requested window rounded to whole cells. Say so when they differ, rather
    // than letting the cube quietly cover a slightly different volume.
    QStringList extent;
    bool rounded = false;
    for (int i = 0; i < plan.extentKpc.size() && i < plan.windowKpc.size(); ++i) {
        extent.append(QString::number(plan.extentKpc[i], 'f', 3));
        rounded = rounded || std::abs(plan.extentKpc[i] - plan.windowKpc[i]) > 1e-6;
    }
    if (rounded)
        text += u"<br><b>Extracted extent:</b> %1 kpc"_s.arg(extent.join(u" × "_s));
    m_planLabel->setText(text);
}

// ── extraction ────────────────────────────────────────────────────────────────

void ArepoExtractDialog::extract()
{
    if (m_extractWatcher.isRunning())
        return;
    const QStringList fields = m_fields->text().split(QRegularExpression(u"[\\s,]+"_s),
                                                      Qt::SkipEmptyParts);
    if (fields.isEmpty()) {
        QMessageBox::warning(this, u"AREPO snapshot"_s,
                             u"Name at least one field to extract."_s);
        return;
    }

    setBusy(true, u"Extracting…"_s);
    m_progressSheet = visivo::showToolProgressSheet(this, u"Extracting the AREPO window"_s);

    const QString path = m_path;
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const int level = m_level->value();
    const QString particle = m_particle->text().trimmed();
    const QVector<double> window = windowKpc();
    const QVector<double> offset = offsetKpc();
    m_extractWatcher.setFuture(
            QtConcurrent::run([path, url, token, level, particle, fields, window, offset]() {
                BackendClient client(url, token);
                return client.extractArepoCubes(path, level, particle, fields, window, offset);
            }));
}

void ArepoExtractDialog::onExtractFinished()
{
    visivo::closeToolProgressSheet(m_progressSheet);
    setBusy(false);
    const BackendArepoExtractResult result = m_extractWatcher.result();
    if (!result.valid) {
        QMessageBox::warning(this, u"AREPO snapshot"_s, result.error);
        return;
    }

    QStringList lines;
    for (const BackendArepoCube &cube : result.outputs) {
        lines.append(u"%1 (%2)"_s.arg(cube.field,
                                      cube.bunit.isEmpty() ? u"no unit"_s : cube.bunit));
    }
    const auto answer = QMessageBox::information(
            this, u"AREPO snapshot"_s,
            u"Extracted %1 cube(s):\n\n%2\n\nOpen them now?"_s.arg(result.outputs.size())
                    .arg(lines.join(u"\n"_s)),
            QMessageBox::Open | QMessageBox::Close, QMessageBox::Open);
    if (answer == QMessageBox::Open) {
        for (const BackendArepoCube &cube : result.outputs)
            emit cubeReady(cube.path);
    }
    m_statusLabel->setText(u"Extracted %1 cube(s)."_s.arg(result.outputs.size()));
}

void ArepoExtractDialog::setBusy(bool busy, const QString &message)
{
    if (m_extractButton)
        m_extractButton->setEnabled(!busy && m_ytAvailable);
    if (m_statusLabel && !message.isEmpty())
        m_statusLabel->setText(message);
    else if (m_statusLabel && !busy && m_ytAvailable)
        m_statusLabel->clear();
    setCursor(busy ? Qt::BusyCursor : Qt::ArrowCursor);
}
