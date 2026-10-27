// McutoutJobDialog.cpp — see McutoutJobDialog.h.

#include "McutoutJobDialog.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;

namespace {

/// How often to ask the archive where the job has got to. The legacy polled
/// every 2 s; a batch takes minutes, and nothing is gained by asking faster.
constexpr int kPollIntervalMs = 2000;
/// Opening this many files at once asks first.
constexpr int kConfirmOpenAbove = 12;

enum Column { ColDataset = 0, ColStatus, ColumnCount };

QString formatElapsed(qint64 ms)
{
    const qint64 s = ms / 1000;
    return s < 60 ? u"%1 s"_s.arg(s) : u"%1 min %2 s"_s.arg(s / 60).arg(s % 60, 2, 10, u'0');
}

bool isLocalBackend(const QString &url)
{
    const QString host = QUrl(url).host();
    return host == u"127.0.0.1"_s || host == u"localhost"_s || host == u"::1"_s;
}

}   // namespace

McutoutJobDialog::McutoutJobDialog(const QVector<BackendMcutoutItem> &items,
                                   const QStringList &labels, const QString &serviceUrl,
                                   const QString &vlkbBearer, const QString &backendUrl,
                                   const QString &backendToken, QWidget *parent)
    : QDialog(parent),
      m_items(items),
      m_labels(labels),
      m_serviceUrl(serviceUrl),
      m_vlkbBearer(vlkbBearer),
      m_backendUrl(backendUrl),
      m_backendToken(backendToken)
{
    setWindowTitle(u"VLKB batch — %1 cutouts"_s.arg(items.size()));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 480);
    m_reportFiles = QStringList(items.size(), QString());
    buildUi(labels);

    connect(&m_submitWatcher, &QFutureWatcher<BackendMcutoutSubmitResult>::finished,
            this, &McutoutJobDialog::onSubmitFinished);
    connect(&m_phaseWatcher, &QFutureWatcher<BackendMcutoutPhaseResult>::finished,
            this, &McutoutJobDialog::onPhaseFinished);
    connect(&m_reportWatcher, &QFutureWatcher<BackendMcutoutReportResult>::finished,
            this, &McutoutJobDialog::onReportFinished);
    connect(&m_fetchWatcher, &QFutureWatcher<BackendMcutoutFetchResult>::finished,
            this, &McutoutJobDialog::onFetchFinished);
    connect(&m_abortWatcher, &QFutureWatcher<BackendMcutoutPhaseResult>::finished,
            this, &McutoutJobDialog::onAbortFinished);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &McutoutJobDialog::poll);
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    connect(m_tickTimer, &QTimer::timeout, this, &McutoutJobDialog::tick);

    setStatus(u"Submitting the batch…"_s);
    const QVector<BackendMcutoutItem> request = m_items;
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString service = m_serviceUrl;
    const QString bearer = m_vlkbBearer;
    m_submitWatcher.setFuture(QtConcurrent::run([url, token, request, service, bearer]() {
        BackendClient client(url, token);
        return client.submitVlkbMcutout(request, service, bearer);
    }));
}

McutoutJobDialog::~McutoutJobDialog()
{
    // Whatever is still in flight finishes on its own; nobody reads it.
    m_submitWatcher.disconnect(this);
    m_phaseWatcher.disconnect(this);
    m_reportWatcher.disconnect(this);
    m_fetchWatcher.disconnect(this);
    m_abortWatcher.disconnect(this);
}

void McutoutJobDialog::buildUi(const QStringList &labels)
{
    auto *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(int(m_items.size()), ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ u"Dataset"_s, u"Status"_s });
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    for (int row = 0; row < m_items.size(); ++row) {
        const QString label = row < labels.size() && !labels.at(row).isEmpty()
                ? labels.at(row)
                : m_items.at(row).id;
        auto *name = new QTableWidgetItem(label);
        name->setToolTip(u"%1\n%2"_s.arg(m_items.at(row).id, m_items.at(row).pos));
        m_table->setItem(row, ColDataset, name);
        m_table->setItem(row, ColStatus, new QTableWidgetItem(u"queued"_s));
    }
    m_table->resizeColumnsToContents();
    layout->addWidget(m_table, 1);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);   // the archive reports a phase, not a percentage
    layout->addWidget(m_progress);

    m_status = new QLabel(QString(), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *actions = new QHBoxLayout;
    m_abortButton = new QPushButton(u"Abort job"_s, this);
    m_abortButton->setToolTip(u"Stop the job on the archive. Cutouts it has already made are "
                              u"discarded."_s);
    connect(m_abortButton, &QPushButton::clicked, this, &McutoutJobDialog::abortJob);
    actions->addWidget(m_abortButton);
    actions->addStretch(1);

    m_retryButton = new QPushButton(u"Retry report"_s, this);
    m_retryButton->setVisible(false);
    connect(m_retryButton, &QPushButton::clicked, this, &McutoutJobDialog::requestReport);
    actions->addWidget(m_retryButton);

    m_folderButton = new QPushButton(u"Show folder"_s, this);
    m_folderButton->setVisible(false);
    connect(m_folderButton, &QPushButton::clicked, this, &McutoutJobDialog::showFolder);
    actions->addWidget(m_folderButton);

    m_downloadButton = new QPushButton(u"Download"_s, this);
    m_downloadButton->setToolTip(u"Download and unpack the cutouts without opening them."_s);
    connect(m_downloadButton, &QPushButton::clicked, this, &McutoutJobDialog::download);
    actions->addWidget(m_downloadButton);

    m_openButton = new QPushButton(u"Open…"_s, this);
    m_openButton->setToolTip(u"Open the cutouts: the images of each source as layers of one "
                             u"viewer, each cube in its own window."_s);
    m_openButton->setDefault(true);
    connect(m_openButton, &QPushButton::clicked, this, &McutoutJobDialog::openResults);
    actions->addWidget(m_openButton);

    auto *close = new QPushButton(u"Close"_s, this);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    actions->addWidget(close);
    layout->addLayout(actions);
    updateButtons();
}

void McutoutJobDialog::setStatus(const QString &text)
{
    if (m_status)
        m_status->setText(text);
}

void McutoutJobDialog::setRowStatus(int row, const QString &text, bool ok, const QString &detail)
{
    if (!m_table || row < 0 || row >= m_table->rowCount())
        return;
    auto *item = m_table->item(row, ColStatus);
    if (!item)
        return;
    item->setText(text);
    item->setForeground(QBrush(ok ? QColor(120, 200, 120) : QColor(230, 130, 130)));
    if (!detail.isEmpty())
        item->setToolTip(detail);
}

bool McutoutJobDialog::jobRunning() const
{
    return !m_jobId.isEmpty() && !m_terminal;
}

void McutoutJobDialog::updateButtons()
{
    const bool busy = m_fetchWatcher.isRunning() || m_abortWatcher.isRunning();
    m_abortButton->setEnabled(jobRunning() && !busy);
    m_abortButton->setVisible(m_jobId.isEmpty() || !m_terminal);
    const bool collectable = m_terminal && m_phase == u"COMPLETED"_s && m_reportOk;
    m_downloadButton->setEnabled(collectable && m_files.isEmpty() && !busy);
    m_openButton->setEnabled(collectable && !busy);
    m_folderButton->setVisible(!m_directory.isEmpty());
}

// ── the job ───────────────────────────────────────────────────────────────────

void McutoutJobDialog::onSubmitFinished()
{
    const BackendMcutoutSubmitResult result = m_submitWatcher.result();
    if (!result.valid) {
        m_progress->hide();
        m_terminal = true;
        if (m_pendingClose != PendingClose::None) {   // closed while submitting: nothing to stop
            QDialog::reject();
            return;
        }
        setStatus(result.error);
        updateButtons();
        QMessageBox box(QMessageBox::Critical, u"VLKB batch"_s, result.error,
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }
    m_jobId = result.jobId;
    m_phase = result.phase;
    m_elapsed.start();
    tick();
    m_pollTimer->start();
    m_tickTimer->start();
    updateButtons();

    // The user closed the window while the job was being submitted.
    if (m_pendingClose == PendingClose::Leave) {
        QDialog::reject();
    } else if (m_pendingClose == PendingClose::Stop) {
        m_closeAfterAbort = true;
        abortJob();
    }
}

void McutoutJobDialog::tick()
{
    if (m_jobId.isEmpty() || m_terminal)
        return;
    QString text = u"Job %1 — %2 · %3"_s.arg(m_jobId, m_phase.isEmpty() ? u"…"_s : m_phase,
                                                formatElapsed(m_elapsed.elapsed()));
    if (m_phase == u"EXECUTING"_s || m_phase == u"QUEUED"_s)
        text += u"\nThe archive reports only the phase of a job, not how far it has got; "
                u"each dataset's outcome appears when the job ends."_s;
    setStatus(text);
}

void McutoutJobDialog::poll()
{
    if (m_jobId.isEmpty() || m_phaseWatcher.isRunning() || m_terminal)
        return;
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString job = m_jobId;
    const QString service = m_serviceUrl;
    const QString bearer = m_vlkbBearer;
    m_phaseWatcher.setFuture(QtConcurrent::run([url, token, job, service, bearer]() {
        BackendClient client(url, token);
        return client.vlkbMcutoutPhase(job, service, bearer);
    }));
}

void McutoutJobDialog::onPhaseFinished()
{
    const BackendMcutoutPhaseResult result = m_phaseWatcher.result();
    if (m_terminal)
        return;   // an abort or an earlier poll already ended it
    if (!result.valid) {
        // A poll that fails is not the job failing: keep polling, and say so.
        setStatus(u"Job %1 — %2"_s.arg(m_jobId, result.error));
        return;
    }
    m_phase = result.phase;
    tick();
    if (!result.terminal)
        return;

    m_terminal = true;
    m_pollTimer->stop();
    m_tickTimer->stop();
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    updateButtons();
    if (m_closeAfterAbort) {   // the stop the user asked for on closing has taken effect
        QDialog::reject();
        return;
    }
    if (result.phase != u"COMPLETED"_s) {
        setStatus(u"Job %1 ended as %2 after %3."_s.arg(m_jobId, result.phase,
                                                        formatElapsed(m_elapsed.elapsed())));
        return;
    }
    requestReport();
}

void McutoutJobDialog::requestReport()
{
    if (m_reportWatcher.isRunning() || m_jobId.isEmpty())
        return;
    m_retryButton->setVisible(false);
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString job = m_jobId;
    const QString service = m_serviceUrl;
    const QString bearer = m_vlkbBearer;
    m_reportWatcher.setFuture(QtConcurrent::run([url, token, job, service, bearer]() {
        BackendClient client(url, token);
        return client.vlkbMcutoutReport(job, service, bearer);
    }));
}

void McutoutJobDialog::onReportFinished()
{
    const BackendMcutoutReportResult result = m_reportWatcher.result();
    if (!result.valid) {
        // The job finished on the archive; only the asking failed, and the
        // results are still there to be collected.
        setStatus(u"%1 The job is finished — you can ask again."_s.arg(result.error));
        m_retryButton->setVisible(true);
        m_reportOk = true;   // the results can be collected even without the report
        updateButtons();
        return;
    }

    int ok = 0;
    for (const BackendMcutoutOutcome &outcome : result.results) {
        if (outcome.ok)
            ++ok;
        if (outcome.index >= 0 && outcome.index < m_reportFiles.size() && outcome.ok)
            m_reportFiles[outcome.index] = QFileInfo(outcome.filename).fileName();
        setRowStatus(outcome.index, outcome.ok ? u"ready"_s : u"failed"_s, outcome.ok,
                     outcome.detail);
    }
    m_reportOk = ok > 0;
    setStatus(ok == result.results.size()
                      ? u"All %1 cutouts are ready (%2). Download keeps them; Open… also "
                        u"opens them."_s.arg(ok).arg(formatElapsed(m_elapsed.elapsed()))
                      : u"%1 of %2 cutouts are ready; hover a failed row for the "
                        u"archive's reason."_s.arg(ok).arg(result.results.size()));
    updateButtons();
}

// ── collecting the results ─────────────────────────────────────────────────

void McutoutJobDialog::download()
{
    fetch(/*openAfter=*/false);
}

void McutoutJobDialog::openResults()
{
    if (!m_files.isEmpty()) {
        const QVector<McutoutResultFile> files = resultFiles();
        if (files.size() > kConfirmOpenAbove) {
            const auto answer = QMessageBox::question(
                    this, u"VLKB batch"_s,
                    u"Open %1 cutouts? The images of each source open as layers of one "
                    u"viewer, and each cube in a window of its own."_s.arg(files.size()));
            if (answer != QMessageBox::Yes)
                return;
        }
        emit openRequested(files);
        return;
    }
    fetch(/*openAfter=*/true);
}

void McutoutJobDialog::fetch(bool openAfter)
{
    if (m_fetchWatcher.isRunning() || m_jobId.isEmpty())
        return;
    m_openAfterFetch = openAfter;
    m_progress->setRange(0, 0);
    m_progress->show();
    setStatus(u"Downloading and unpacking the results…"_s);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString job = m_jobId;
    const QString service = m_serviceUrl;
    const QString bearer = m_vlkbBearer;
    m_fetchWatcher.setFuture(QtConcurrent::run([url, token, job, service, bearer]() {
        BackendClient client(url, token);
        return client.vlkbMcutoutFetch(job, service, bearer);
    }));
    updateButtons();
}

void McutoutJobDialog::onFetchFinished()
{
    const BackendMcutoutFetchResult result = m_fetchWatcher.result();
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    if (!result.valid) {
        setStatus(result.error);
        updateButtons();
        QMessageBox box(QMessageBox::Critical, u"VLKB batch"_s, result.error,
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }

    m_files = result.files;
    m_directory = result.directory;
    for (int row = 0; row < m_items.size(); ++row)
        if (!m_reportFiles.value(row).isEmpty())
            setRowStatus(row, u"downloaded"_s, true);
    setStatus(m_files.isEmpty()
                      ? u"The archive's results contained no cutouts."_s
                      : u"%1 cutout(s) unpacked in %2"_s.arg(m_files.size()).arg(m_directory));
    updateButtons();
    if (m_openAfterFetch && !m_files.isEmpty())
        openResults();
}

QVector<McutoutResultFile> McutoutJobDialog::resultFiles() const
{
    // Which row each file answers: by the file name the report gave for the
    // row, else by the "<row>_" prefix the archive puts on its file names.
    static const QRegularExpression rowPrefix(u"^(\\d+)_"_s);
    QVector<McutoutResultFile> out;
    for (const QString &path : m_files) {
        const QString name = QFileInfo(path).fileName();
        int row = int(m_reportFiles.indexOf(name));
        if (row < 0) {
            const auto m = rowPrefix.match(name);
            if (m.hasMatch())
                row = m.captured(1).toInt();
        }
        McutoutResultFile f;
        f.path = path;
        if (row >= 0 && row < m_items.size()) {
            f.label = m_labels.value(row, name);
            f.type = m_items.at(row).type;
            f.group = m_items.at(row).group;
        } else {
            f.label = name;
        }
        out << f;
    }
    return out;
}

void McutoutJobDialog::showFolder()
{
    if (m_directory.isEmpty())
        return;
    if (isLocalBackend(m_backendUrl)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_directory));
        return;
    }
    QMessageBox box(QMessageBox::Information, u"VLKB batch"_s,
                    u"The cutouts are on the backend, in:\n\n%1\n\nThey are also listed in the "
                    u"Data Hub's workspace."_s.arg(m_directory),
                    QMessageBox::Ok, this);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();
}

// ── stopping ─────────────────────────────────────────────────────────────────

void McutoutJobDialog::abortJob()
{
    if (!jobRunning() || m_abortWatcher.isRunning())
        return;
    setStatus(u"Stopping job %1…"_s.arg(m_jobId));
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString job = m_jobId;
    const QString service = m_serviceUrl;
    const QString bearer = m_vlkbBearer;
    m_abortWatcher.setFuture(QtConcurrent::run([url, token, job, service, bearer]() {
        BackendClient client(url, token);
        return client.vlkbMcutoutAbort(job, service, bearer);
    }));
    updateButtons();
}

void McutoutJobDialog::onAbortFinished()
{
    const BackendMcutoutPhaseResult result = m_abortWatcher.result();
    if (!result.valid) {
        setStatus(u"The job could not be stopped: %1"_s.arg(result.error));
        m_closeAfterAbort = false;
        updateButtons();
        return;
    }
    m_phase = result.phase;
    if (!result.terminal) {
        // The archive accepted the request but has not stopped yet: keep
        // polling, and close (if that was asked) only once it has.
        setStatus(u"Stopping job %1 — the archive reports %2…"_s.arg(m_jobId, result.phase));
        updateButtons();
        return;
    }
    {
        m_terminal = true;
        m_pollTimer->stop();
        m_tickTimer->stop();
        m_progress->setRange(0, 1);
        m_progress->setValue(1);
        for (int row = 0; row < m_items.size(); ++row)
            setRowStatus(row, u"aborted"_s, false);
        setStatus(u"Job %1 stopped (%2)."_s.arg(m_jobId, result.phase));
        if (result.phase == u"COMPLETED"_s)   // it finished just before the abort arrived
            requestReport();
    }
    updateButtons();
    if (m_closeAfterAbort)
        QDialog::reject();
}

void McutoutJobDialog::reject()
{
    if (!jobRunning() && !(m_jobId.isEmpty() && m_submitWatcher.isRunning())) {
        QDialog::reject();
        return;
    }
    QMessageBox box(QMessageBox::Question, u"VLKB batch"_s,
                    m_jobId.isEmpty() ? u"The job is being submitted to the archive."_s
                                      : u"Job %1 is still running on the archive."_s.arg(m_jobId),
                    QMessageBox::NoButton, this);
    box.setInformativeText(u"Stop it, or leave it running? A job left running finishes on "
                           u"the archive, but its results can no longer be collected from "
                           u"this window."_s);
    QPushButton *stop = box.addButton(u"Stop the job"_s, QMessageBox::DestructiveRole);
    QPushButton *leave = box.addButton(u"Leave it running"_s, QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    const QAbstractButton *choice = box.clickedButton();
    if (choice != stop && choice != leave)
        return;   // cancelled
    // The state may have moved while the question was open (the nested event
    // loop delivers the submit and poll answers): decide on what is true now.
    if (m_jobId.isEmpty() && m_submitWatcher.isRunning()) {
        // Still no id: act once the archive has named it.
        m_pendingClose = choice == stop ? PendingClose::Stop : PendingClose::Leave;
        setStatus(u"Waiting for the archive to accept the job, then closing…"_s);
        return;
    }
    if (choice == stop && jobRunning()) {
        m_closeAfterAbort = true;
        abortJob();
        return;
    }
    QDialog::reject();   // left running, or it ended meanwhile
}
