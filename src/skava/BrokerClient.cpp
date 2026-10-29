#include "BrokerClient.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QThreadStorage>
#include <QTimer>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

QNetworkAccessManager *threadLocalNam()
{
    static QThreadStorage<QNetworkAccessManager *> storage;
    if (!storage.hasLocalData())
        storage.setLocalData(new QNetworkAccessManager());
    return storage.localData();
}

/// Two bounds on every request: a connection that goes quiet, and one that
/// trickles bytes forever.
constexpr int kInactivityTimeoutMs = 30'000;
constexpr int kRequestDeadlineMs = 60'000;
constexpr int kPollMs = 1'000;
constexpr int kCancelCheckMs = 100;
/// Consecutive polls that may fail before the job is given up as lost.
constexpr int kMaxPollFailures = 10;
/// The whole of a cancellation: the request and the broker settling the job.
constexpr int kCancelBudgetMs = 30'000;

/// The job states the broker's API has.
const QStringList &jobStates()
{
    static const QStringList states{ u"PENDING"_s, u"RUNNING"_s, u"COMPLETE"_s, u"FAILED"_s, u"CANCELLED"_s };
    return states;
}

/// An HTTP status that will not change by asking again.
bool permanent(int status)
{
    return status == 400 || status == 401 || status == 403 || status == 404 || status == 410;
}

/// The broker's error detail, when its body carries one.
QString detailOf(const QByteArray &body)
{
    const QJsonObject obj = QJsonDocument::fromJson(body).object();
    const QJsonValue detail = obj.value(u"detail"_s);
    if (detail.isString())
        return detail.toString();
    const QString error = obj.value(u"error"_s).toString();
    return error.isEmpty() ? QString::fromUtf8(body).trimmed().left(300) : error;
}

QString encodedId(const QString &jobId)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(jobId));
}

}  // namespace

BrokerClient::BrokerClient(const QString &baseUrl, const QString &bearerToken)
    : m_baseUrl(baseUrl.trimmed()), m_token(bearerToken)
{
    while (m_baseUrl.endsWith(u'/'))
        m_baseUrl.chop(1);
}

BrokerJob BrokerClient::parseJob(const QByteArray &body)
{
    BrokerJob job;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    const QString status = doc.isObject() ? doc.object().value(u"status"_s).toString() : QString();
    if (status.isEmpty()) {
        job.error = u"The broker's answer is not a job."_s;
        return job;
    }
    if (!jobStates().contains(status)) {
        // Not a state the API has: not one to wait on.
        job.error = u"The broker's answer has a job state the API does not define: %1"_s.arg(status.left(40));
        return job;
    }
    const QJsonObject obj = doc.object();
    job.raw = obj;
    job.valid = true;
    job.id = obj.value(u"id"_s).toString();
    job.status = status;
    job.error = obj.value(u"error"_s).toString();
    const QJsonObject placement = obj.value(u"placement"_s).toObject();
    job.node = placement.value(u"node"_s).toString();
    job.rankReason = placement.value(u"rank_reason"_s).toString();
    job.backend = obj.value(u"backend"_s).toString();
    if (job.backend.isEmpty())
        job.backend = placement.value(u"backend"_s).toString();
    job.taskId = obj.value(u"task_id"_s).toString();
    const QJsonObject result = obj.value(u"result"_s).toObject();
    if (!result.isEmpty()) {
        job.finder = result.value(u"finder"_s).toString();
        job.nSources = result.value(u"n_sources"_s).toInt(-1);
        job.maskPath = result.value(u"mask_path"_s).toString();
        job.catalogPath = result.value(u"catalog_path"_s).toString();
        if (job.node.isEmpty())
            job.node = result.value(u"node"_s).toString();
        if (job.taskId.isEmpty())
            job.taskId = result.value(u"task_id"_s).toString();
    }
    if (job.finder.isEmpty()) {
        const QJsonObject parameters = obj.value(u"request"_s).toObject().value(u"parameters"_s).toObject();
        job.finder = parameters.value(u"finder"_s).toString();
    }
    return job;
}

BrokerClient::Exchange BrokerClient::request(const QByteArray &verb, const QString &path,
                                             const QByteArray &json, std::atomic_bool *cancel,
                                             int deadlineMs) const
{
    Exchange x;
    if (deadlineMs <= 0) {
        x.error = u"Broker at %1: no time left for the request"_s.arg(m_baseUrl);
        return x;
    }
    QNetworkRequest req{ QUrl(m_baseUrl + path) };
    req.setRawHeader("Accept", "application/json");
    if (!m_token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    req.setTransferTimeout(std::min(kInactivityTimeoutMs, deadlineMs));
    if (verb == "POST")
        req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    auto *nam = threadLocalNam();
    QNetworkReply *reply = verb == "POST" ? nam->post(req, json) : nam->get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    bool timedOut = false;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();
    });
    deadline.start(deadlineMs);
    QTimer cancelPoll;
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
        if (cancel && cancel->load()) {
            x.cancelled = true;
            reply->abort();
        }
    });
    if (cancel)
        cancelPoll.start(kCancelCheckMs);
    loop.exec();
    deadline.stop();
    cancelPoll.stop();
    x.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    x.body = reply->readAll();
    if (x.cancelled) {
        x.error = u"cancelled"_s;
    } else if (timedOut) {
        x.error = u"Broker at %1: no answer within %2 s"_s.arg(m_baseUrl).arg((deadlineMs + 999) / 1000);
    } else if (reply->error() != QNetworkReply::NoError) {
        const QString detail = detailOf(x.body);
        x.error = x.status > 0 ? u"Broker HTTP %1: %2"_s.arg(x.status).arg(detail.isEmpty() ? reply->errorString() : detail)
                               : u"Broker at %1: %2"_s.arg(m_baseUrl, reply->errorString());
    }
    reply->deleteLater();
    return x;
}

BrokerJob BrokerClient::submit(const QString &did, const QString &finder, const QJsonObject &params,
                               int cpu, const QString &memory) const
{
    BrokerJob job;
    if (m_baseUrl.isEmpty()) {
        job.error = u"No Computing Broker is configured."_s;
        return job;
    }
    // The broker's own field names (SRCNet job API): the dataset by its
    // identifier, the finder as a required capability, its parameters. No
    // memory figure unless the caller has one: the broker then sizes it
    // from what the node states for the finder and the dataset's size.
    QJsonObject body{
        { u"image"_s, u"inaf:visivo-finders:1.0"_s },
        { u"cmd"_s, QJsonArray{ u"sourcefind"_s } },
        { u"rucio_did"_s, did },
        { u"requires_capability"_s, QJsonArray{ u"finder:"_s + finder } },
        { u"cpu"_s, cpu },
        { u"parameters"_s, QJsonObject{ { u"finder"_s, finder }, { u"params"_s, params } } },
    };
    if (!memory.isEmpty())
        body.insert(u"memory"_s, memory);
    const Exchange answer = request("POST", u"/v1/jobs"_s, QJsonDocument(body).toJson(QJsonDocument::Compact),
                                    nullptr, kRequestDeadlineMs);
    if (!answer.error.isEmpty()) {
        job.error = answer.error;
        return job;
    }
    job = parseJob(answer.body);
    if (job.valid && job.id.isEmpty()) {
        job.valid = false;
        job.error = u"The broker created a job without an id."_s;
    }
    return job;
}

BrokerJob BrokerClient::status(const QString &jobId) const
{
    const Exchange answer = request("GET", u"/v1/jobs/"_s + encodedId(jobId), {}, nullptr, kRequestDeadlineMs);
    if (!answer.error.isEmpty()) {
        BrokerJob job;
        job.error = answer.error;
        return job;
    }
    return parseJob(answer.body);
}

BrokerJob BrokerClient::cancel(const QString &jobId) const
{
    const Exchange answer = request("POST", u"/v1/jobs/%1/cancel"_s.arg(encodedId(jobId)), "{}", nullptr,
                                    kRequestDeadlineMs);
    if (!answer.error.isEmpty()) {
        BrokerJob job;
        job.error = answer.error;
        return job;
    }
    BrokerJob job = parseJob(answer.body);
    if (!job.valid)
        job.error = u"the broker accepted the cancellation without saying what became of the job"_s;
    return job;
}

QStringList BrokerClient::logs(const QString &jobId) const
{
    bool cancelled = false;
    return logs(jobId, nullptr, cancelled);
}

QStringList BrokerClient::logs(const QString &jobId, std::atomic_bool *cancel, bool &cancelled) const
{
    const Exchange answer = request("GET", u"/v1/jobs/%1/logs"_s.arg(encodedId(jobId)), {}, cancel, kRequestDeadlineMs);
    cancelled = answer.cancelled;
    if (!answer.error.isEmpty())
        return {};
    return QString::fromUtf8(answer.body).split(u'\n', Qt::SkipEmptyParts);
}

BrokerJob BrokerClient::cancelAndSettle(const QString &jobId, const BrokerJob &lastKnown) const
{
    // The whole of the cancellation has one time budget: the request, then
    // the broker settling the job (the node has to stop the task).
    QElapsedTimer budget;
    budget.start();
    auto left = [&]() { return static_cast<int>(kCancelBudgetMs - budget.elapsed()); };
    const Exchange answer = request("POST", u"/v1/jobs/%1/cancel"_s.arg(encodedId(jobId)), "{}", nullptr, left());
    BrokerJob job = answer.error.isEmpty() ? parseJob(answer.body) : BrokerJob{};
    if (!job.valid) {
        // Said honestly: the job may go on.
        BrokerJob unconfirmed = lastKnown;
        unconfirmed.error = u"cancelled, but the broker did not confirm it (%1); the job may still be running"_s
                                    .arg(answer.error.isEmpty() ? u"its answer was not the job"_s : answer.error);
        return unconfirmed;
    }
    while (!job.ended() && left() > 0) {
        QThread::msleep(std::min(kPollMs, std::max(0, left())));
        const Exchange poll = request("GET", u"/v1/jobs/"_s + encodedId(jobId), {}, nullptr, left());
        if (poll.error.isEmpty()) {
            const BrokerJob now = parseJob(poll.body);
            if (now.valid)
                job = now;
        }
    }
    if (job.status == "CANCELLED"_L1 && job.error.isEmpty())
        job.error = u"cancelled"_s;
    else if (!job.ended())
        job.error = u"cancellation requested; the broker has not confirmed it yet (job %1)"_s.arg(job.status);
    return job;
}

BrokerJob BrokerClient::run(const QString &did, const QString &finder, const QJsonObject &params,
                            std::atomic_bool *cancel, const std::function<void(const QString &)> &onLog,
                            int cpu, const QString &memory) const
{
    if (cancel && cancel->load()) {
        BrokerJob job;
        job.error = u"cancelled"_s;
        return job;
    }
    BrokerJob job = submit(did, finder, params, cpu, memory);
    if (!job.valid)
        return job;
    const QString jobId = job.id;
    int seen = 0;
    int failures = 0;
    for (;;) {
        for (int waited = 0; waited < kPollMs; waited += kCancelCheckMs) {
            if (cancel && cancel->load())
                return cancelAndSettle(jobId, job);
            QThread::msleep(kCancelCheckMs);
        }
        const Exchange poll = request("GET", u"/v1/jobs/"_s + encodedId(jobId), {}, cancel, kRequestDeadlineMs);
        if (poll.cancelled)
            return cancelAndSettle(jobId, job);
        BrokerJob now;
        if (poll.error.isEmpty())
            now = parseJob(poll.body);
        if (!poll.error.isEmpty() || !now.valid) {
            const QString why = poll.error.isEmpty() ? now.error : poll.error;
            // A refusal that asking again will not change: the job is gone
            // (or was never ours). A transient failure is tried again, within bounds.
            if (permanent(poll.status) || ++failures >= kMaxPollFailures) {
                BrokerJob lost = job;
                lost.valid = false;
                lost.error = u"lost track of the broker's job %1: %2"_s.arg(jobId, why);
                return lost;
            }
            if (onLog)
                onLog(u"(poll) "_s + why);
            continue;
        }
        failures = 0;
        job = now;
        if (onLog) {
            bool cancelled = false;
            const QStringList lines = logs(jobId, cancel, cancelled);
            if (cancelled)
                return cancelAndSettle(jobId, job);
            for (; seen < lines.size(); ++seen)
                onLog(lines[seen]);
        }
        if (job.ended())
            return job;
    }
}
