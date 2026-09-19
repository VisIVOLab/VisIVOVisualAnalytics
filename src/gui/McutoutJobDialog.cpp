// McutoutJobDialog.cpp — see McutoutJobDialog.h.

#include "McutoutJobDialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;

namespace {

/// How often to ask the archive where the job has got to. The legacy polled
/// every 2 s; a batch takes minutes, and nothing is gained by asking faster.
constexpr int kPollIntervalMs = 2000;

enum Column { ColDataset = 0, ColStatus, ColumnCount };

}   // namespace

McutoutJobDialog::McutoutJobDialog(const QVector<BackendMcutoutItem> &items,
                                   const QStringList &labels, const QString &serviceUrl,
                                   const QString &vlkbBearer, const QString &backendUrl,
                                   const QString &backendToken, QWidget *parent)
    : QDialog(parent),
      m_items(items),
      m_serviceUrl(serviceUrl),
      m_vlkbBearer(vlkbBearer),
      m_backendUrl(backendUrl),
      m_backendToken(backendToken)
{
    setWindowTitle(u"VLKB batch — %1 cutouts"_s.arg(items.size()));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(680, 460);
    buildUi(labels);

    connect(&m_submitWatcher, &QFutureWatcher<BackendMcutoutSubmitResult>::finished,
            this, &McutoutJobDialog::onSubmitFinished);
    connect(&m_phaseWatcher, &QFutureWatcher<BackendMcutoutPhaseResult>::finished,
            this, &McutoutJobDialog::onPhaseFinished);
    connect(&m_reportWatcher, &QFutureWatcher<BackendMcutoutReportResult>::finished,
            this, &McutoutJobDialog::onReportFinished);
    connect(&m_fetchWatcher, &QFutureWatcher<BackendMcutoutFetchResult>::finished,
            this, &McutoutJobDialog::onFetchFinished);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &McutoutJobDialog::poll);

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
    // The job keeps running on the archive whether or not this window watches
    // it — that is the point of an asynchronous job — so nothing is waited for.
    m_submitWatcher.disconnect(this);
    m_phaseWatcher.disconnect(this);
    m_reportWatcher.disconnect(this);
    m_fetchWatcher.disconnect(this);
}

void McutoutJobDialog::buildUi(const QStringList &labels)
{
    auto *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(m_items.size(), ColumnCount, this);
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
        m_table->setItem(row, ColStatus, new QTableWidgetItem(u"—"_s));
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
    actions->addStretch(1);
    m_collectButton = new QPushButton(u"Download && Open"_s, this);
    m_collectButton->setEnabled(false);
    m_collectButton->setDefault(true);
    connect(m_collectButton, &QPushButton::clicked, this, &McutoutJobDialog::collect);
    actions->addWidget(m_collectButton);

    m_retryButton = new QPushButton(u"Retry report"_s, this);
    m_retryButton->setVisible(false);
    connect(m_retryButton, &QPushButton::clicked, this, &McutoutJobDialog::requestReport);
    actions->addWidget(m_retryButton);

    auto *close = new QPushButton(u"Close"_s, this);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    actions->addWidget(close);
    layout->addLayout(actions);
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

// ── the job ───────────────────────────────────────────────────────────────────

void McutoutJobDialog::onSubmitFinished()
{
    const BackendMcutoutSubmitResult result = m_submitWatcher.result();
    if (!result.valid) {
        m_progress->hide();
        setStatus(result.error);
        QMessageBox box(QMessageBox::Critical, u"VLKB batch"_s, result.error,
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }
    m_jobId = result.jobId;
    setStatus(u"Job %1 — %2"_s.arg(m_jobId, result.phase));
    m_pollTimer->start();
}

void McutoutJobDialog::poll()
{
    if (m_jobId.isEmpty() || m_phaseWatcher.isRunning())
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
    if (!result.valid) {
        // A poll that fails is not the job failing: keep polling, and say so.
        setStatus(u"Job %1 — %2"_s.arg(m_jobId, result.error));
        return;
    }
    setStatus(u"Job %1 — %2"_s.arg(m_jobId, result.phase));
    if (!result.terminal)
        return;

    m_pollTimer->stop();
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    if (result.phase != u"COMPLETED"_s) {
        setStatus(u"Job %1 ended as %2."_s.arg(m_jobId, result.phase));
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
        m_collectButton->setEnabled(true);
        return;
    }

    int ok = 0;
    for (const BackendMcutoutOutcome &outcome : result.results) {
        if (outcome.ok)
            ++ok;
        setRowStatus(outcome.index, outcome.ok ? u"ready"_s : u"failed"_s, outcome.ok,
                     outcome.detail);
    }
    setStatus(ok == result.results.size()
                      ? u"All %1 cutouts are ready."_s.arg(ok)
                      : u"%1 of %2 cutouts are ready; hover a failed row for the "
                        "archive's reason."_s.arg(ok).arg(result.results.size()));
    m_collectButton->setEnabled(ok > 0);
}

void McutoutJobDialog::collect()
{
    if (m_fetchWatcher.isRunning() || m_jobId.isEmpty())
        return;
    m_collectButton->setEnabled(false);
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
}

void McutoutJobDialog::onFetchFinished()
{
    const BackendMcutoutFetchResult result = m_fetchWatcher.result();
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    if (!result.valid) {
        m_collectButton->setEnabled(true);
        setStatus(result.error);
        QMessageBox box(QMessageBox::Critical, u"VLKB batch"_s, result.error,
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }

    m_collected = true;
    setStatus(u"%1 file(s) unpacked on the backend."_s.arg(result.files.size()));
    for (const QString &path : result.files)
        emit cutoutReady(path);
    if (result.files.isEmpty()) {
        QMessageBox::information(this, u"VLKB batch"_s,
                                 u"The archive's results contained no files."_s);
    }
}
