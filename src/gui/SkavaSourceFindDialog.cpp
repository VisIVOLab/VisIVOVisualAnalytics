#include "SkavaSourceFindDialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

namespace {

/// The two finders VisIVO backends ship, for a node that could not be asked.
QVector<BackendFinderInfo> defaultFinders()
{
    BackendFinderInfo sofia;
    sofia.name = u"sofia2"_s;
    sofia.kinds = { u"cube"_s };
    sofia.description = u"SoFiA-2 smooth-and-clip source finder for spectral-line cubes."_s;
    sofia.parameters = { { u"scfind.threshold"_s, u"S/N threshold of the smooth-and-clip finder (default 5.0)"_s },
                         { u"scfind.kernelsXY"_s, u"spatial smoothing kernels in pixels, e.g. '0, 3, 6'"_s },
                         { u"scfind.kernelsZ"_s, u"spectral smoothing kernels in channels, e.g. '0, 3, 7'"_s },
                         { u"linker.minSizeXY"_s, u"minimum spatial size of a source in pixels"_s },
                         { u"linker.minSizeZ"_s, u"minimum spectral size of a source in channels"_s },
                         { u"reliability.enable"_s, u"true to filter detections by reliability"_s },
                         { u"reliability.threshold"_s, u"reliability threshold (default 0.9)"_s } };
    sofia.available = true;
    BackendFinderInfo photutils;
    photutils.name = u"photutils"_s;
    photutils.kinds = { u"image"_s };
    photutils.description = u"Threshold segmentation of a 2-D image (photutils)."_s;
    photutils.parameters = { { u"nsigma"_s, u"detection threshold above the clipped median, in sigma (default 3.0)"_s },
                             { u"npixels"_s, u"minimum connected pixels of a source (default 5)"_s },
                             { u"connectivity"_s, u"4 or 8 (default 8)"_s } };
    photutils.available = true;
    return { sofia, photutils };
}

/// A typed value for a parameter the user typed: a number, a boolean, else
/// the text (SoFiA takes "0, 3, 6" as text; photutils wants numbers).
QJsonValue typedValue(const QString &text)
{
    const QString t = text.trimmed();
    bool ok = false;
    if (!t.contains(u',') && !t.contains(u' ')) {
        const double d = t.toDouble(&ok);
        if (ok)
            return d;
    }
    if (t.compare(u"true"_s, Qt::CaseInsensitive) == 0)
        return true;
    if (t.compare(u"false"_s, Qt::CaseInsensitive) == 0)
        return false;
    return t;
}

}  // namespace

SkavaSourceFindDialog::SkavaSourceFindDialog(const QVector<BackendFinderInfo> &finders,
                                             const QString &datasetLabel, const QString &kind,
                                             const QString &where, Runner runner, QWidget *parent)
    : QDialog(parent),
      m_finders(finders.isEmpty() ? defaultFinders() : finders),
      m_runner(std::move(runner)),
      m_cancel(std::make_shared<std::atomic_bool>(false))
{
    setWindowTitle(u"Source finder next to the data - %1"_s.arg(datasetLabel));
    resize(720, 620);
    auto *root = new QVBoxLayout(this);

    auto *whereLabel = new QLabel(where, this);
    whereLabel->setWordWrap(true);
    root->addWidget(whereLabel);

    auto *form = new QFormLayout;
    m_finderCombo = new QComboBox(this);
    int preselect = -1;
    for (int i = 0; i < m_finders.size(); ++i) {
        const auto &f = m_finders[i];
        m_finderCombo->addItem(f.available ? f.name : u"%1 (unavailable)"_s.arg(f.name));
        m_finderCombo->setItemData(i, f.description, Qt::ToolTipRole);
        if (preselect < 0 && f.available && (kind.isEmpty() || f.kinds.contains(kind)))
            preselect = i;
    }
    form->addRow(u"Finder"_s, m_finderCombo);
    m_finderNote = new QLabel(this);
    m_finderNote->setWordWrap(true);
    form->addRow(QString(), m_finderNote);
    root->addLayout(form);

    // The finder's parameters: a value typed in a row is sent, a blank row
    // leaves the finder's default.
    m_params = new QTableWidget(0, 2, this);
    m_params->setHorizontalHeaderLabels({ u"Parameter"_s, u"Value (blank = default)"_s });
    m_params->horizontalHeader()->setStretchLastSection(true);
    m_params->verticalHeader()->setVisible(false);
    m_params->setSelectionMode(QAbstractItemView::NoSelection);
    root->addWidget(m_params, 1);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setPlaceholderText(u"The run's log appears here."_s);
    root->addWidget(m_log, 1);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_summary);

    auto *buttons = new QHBoxLayout;
    m_runBtn = new QPushButton(u"Run"_s, this);
    m_runBtn->setDefault(true);
    m_cancelBtn = new QPushButton(u"Cancel run"_s, this);
    m_cancelBtn->setEnabled(false);
    m_openMaskBtn = new QPushButton(u"Open mask cube"_s, this);
    m_openMaskBtn->setEnabled(false);
    m_openMaskBtn->setToolTip(u"Open the finder's label mask from the node, as a cube."_s);
    m_openCatBtn = new QPushButton(u"Open catalogue"_s, this);
    m_openCatBtn->setEnabled(false);
    m_openCatBtn->setToolTip(u"Open the finder's catalogue (VOTable) from the node."_s);
    auto *closeBtn = new QPushButton(u"Close"_s, this);
    buttons->addWidget(m_runBtn);
    buttons->addWidget(m_cancelBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_openMaskBtn);
    buttons->addWidget(m_openCatBtn);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    QObject::connect(m_finderCombo, &QComboBox::currentIndexChanged, this, &SkavaSourceFindDialog::fillParameters);
    QObject::connect(m_runBtn, &QPushButton::clicked, this, &SkavaSourceFindDialog::run);
    QObject::connect(m_cancelBtn, &QPushButton::clicked, this, &SkavaSourceFindDialog::cancel);
    QObject::connect(m_openMaskBtn, &QPushButton::clicked, this, [this]() { emit openMaskRequested(m_outcome); });
    QObject::connect(m_openCatBtn, &QPushButton::clicked, this, [this]() { emit openCatalogueRequested(m_outcome); });
    QObject::connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    m_finderCombo->setCurrentIndex(std::max(0, preselect));
    fillParameters(m_finderCombo->currentIndex());
}

SkavaSourceFindDialog::~SkavaSourceFindDialog()
{
    // However the dialog went away, the run must not go on without it.
    m_cancel->store(true);
}

void SkavaSourceFindDialog::reject()
{
    // Escape and the window's close button: through closeEvent, which asks
    // before abandoning a run.
    close();
}

void SkavaSourceFindDialog::fillParameters(int finderIndex)
{
    m_params->setRowCount(0);
    if (finderIndex < 0 || finderIndex >= m_finders.size())
        return;
    const auto &f = m_finders[finderIndex];
    m_finderNote->setText(f.available ? f.description
                                      : u"%1 is not available on this node: %2"_s.arg(f.name, f.reason));
    m_runBtn->setEnabled(f.available && !m_busy);
    for (auto it = f.parameters.cbegin(); it != f.parameters.cend(); ++it) {
        const int row = m_params->rowCount();
        m_params->insertRow(row);
        auto *name = new QTableWidgetItem(it.key());
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        name->setToolTip(it.value());
        m_params->setItem(row, 0, name);
        auto *value = new QTableWidgetItem(QString());
        value->setToolTip(it.value());
        m_params->setItem(row, 1, value);
    }
    m_params->resizeColumnToContents(0);
}

QJsonObject SkavaSourceFindDialog::parameters() const
{
    QJsonObject params;
    for (int row = 0; row < m_params->rowCount(); ++row) {
        const QTableWidgetItem *name = m_params->item(row, 0);
        const QTableWidgetItem *value = m_params->item(row, 1);
        if (!name || !value || value->text().trimmed().isEmpty())
            continue;
        params.insert(name->text(), typedValue(value->text()));
    }
    return params;
}

void SkavaSourceFindDialog::setBusy(bool busy)
{
    m_busy = busy;
    const int idx = m_finderCombo->currentIndex();
    const bool available = idx >= 0 && idx < m_finders.size() && m_finders[idx].available;
    m_runBtn->setEnabled(!busy && available);
    m_cancelBtn->setEnabled(busy);
    m_finderCombo->setEnabled(!busy);
    m_params->setEnabled(!busy);
}

void SkavaSourceFindDialog::appendLog(const QString &line)
{
    m_log->appendPlainText(line);
}

void SkavaSourceFindDialog::run()
{
    if (m_busy || !m_runner)
        return;
    const int idx = m_finderCombo->currentIndex();
    if (idx < 0 || idx >= m_finders.size())
        return;
    const QString finder = m_finders[idx].name;
    const QJsonObject params = parameters();
    m_cancel->store(false);
    m_outcome = {};
    m_openMaskBtn->setEnabled(false);
    m_openCatBtn->setEnabled(false);
    m_summary->clear();
    m_log->clear();
    appendLog(u"Running %1…"_s.arg(finder));
    setBusy(true);

    // The log sink runs on the worker: hand each line to the UI thread, and
    // only while the dialog is alive.
    // Dispatched through the application object, which outlives the dialog:
    // the dialog may be deleted on the UI thread at any moment, so whether
    // it still exists is checked there, when the call runs.
    QPointer<SkavaSourceFindDialog> self(this);
    auto log = [self](const QString &line) {
        QMetaObject::invokeMethod(qApp, [self, line]() {
            if (self)
                self->appendLog(line);
        }, Qt::QueuedConnection);
    };
    auto *watcher = new QFutureWatcher<SkavaFinderOutcome>(this);
    QObject::connect(watcher, &QFutureWatcher<SkavaFinderOutcome>::finished, this, [self, watcher]() {
        const SkavaFinderOutcome outcome = watcher->result();
        watcher->deleteLater();
        if (self)
            self->showOutcome(outcome);
    });
    const Runner runner = m_runner;
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    watcher->setFuture(QtConcurrent::run([runner, finder, params, cancel, log]() {
        return runner(finder, params, cancel.get(), log);
    }));
}

void SkavaSourceFindDialog::cancel()
{
    if (!m_busy)
        return;
    m_cancel->store(true);
    m_cancelBtn->setEnabled(false);
    appendLog(u"Cancelling…"_s);
}

void SkavaSourceFindDialog::showOutcome(const SkavaFinderOutcome &outcome)
{
    setBusy(false);
    m_outcome = outcome;
    if (!outcome.valid) {
        appendLog(u"Failed: %1"_s.arg(outcome.error));
        m_summary->setText(u"<b>Failed.</b> %1"_s.arg(outcome.error.toHtmlEscaped()));
        return;
    }
    appendLog(u"Finished: %1 source(s)."_s.arg(outcome.nSources));
    QString text = u"<b>%1 source(s)</b> found by %2 on node %3."_s.arg(outcome.nSources).arg(
            outcome.finder.toHtmlEscaped(), outcome.node.toHtmlEscaped());
    if (!outcome.rankReason.isEmpty())
        text += u"<br>Placed there because: %1"_s.arg(outcome.rankReason.toHtmlEscaped());
    if (!outcome.maskPath.isEmpty())
        text += u"<br>Mask: %1"_s.arg(outcome.maskPath.toHtmlEscaped());
    if (!outcome.catalogPath.isEmpty())
        text += u"<br>Catalogue: %1"_s.arg(outcome.catalogPath.toHtmlEscaped());
    if (outcome.backendUrl.isEmpty())
        text += u"<br><i>The node's backend is not registered in Settings ▸ Backends: the products "
                u"stay on the node, where the paths above point.</i>"_s;
    m_summary->setText(text);
    const bool canOpen = !outcome.backendUrl.isEmpty();
    m_openMaskBtn->setEnabled(canOpen && !outcome.maskPath.isEmpty());
    m_openCatBtn->setEnabled(canOpen && !outcome.catalogPath.isEmpty());
}

void SkavaSourceFindDialog::closeEvent(QCloseEvent *event)
{
    if (m_busy) {
        const auto answer = QMessageBox::question(
                this, u"Source finder running"_s,
                u"The finder is still running on the node. Cancel it and close?"_s,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_cancel->store(true);
    }
    event->accept();
}
