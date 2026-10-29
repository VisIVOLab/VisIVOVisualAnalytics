#include "NodeSoda.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

/// Per-thread QNetworkAccessManager: it must live in the thread that calls it.
QNetworkAccessManager *threadLocalNam()
{
    static QThreadStorage<QNetworkAccessManager *> storage;
    if (!storage.hasLocalData())
        storage.setLocalData(new QNetworkAccessManager());
    return storage.localData();
}

/// Two bounds on every request: a connection that goes quiet, and one that
/// trickles bytes forever (the transfer timeout alone does not end it). A
/// job is waited for by polling, so no single request is long.
constexpr int kInactivityTimeoutMs = 30'000;
constexpr int kRequestDeadlineMs = 60'000;
/// Polling: quick at first (a small cutout ends within a second), then
/// slower, so a long product does not hammer the node.
constexpr int kFirstPollMs = 300;
constexpr int kMaxPollMs = 2'000;
constexpr int kCancelCheckMs = 100;
/// Consecutive polls that may fail (a transient error, a malformed answer)
/// before the job is given up as lost.
constexpr int kMaxPollFailures = 5;
/// The whole of a cancellation: the abort request and the confirmation.
constexpr int kAbortBudgetMs = 30'000;

/// The phases UWS 1.1 has.
const QStringList &uwsPhases()
{
    static const QStringList phases{ u"PENDING"_s, u"QUEUED"_s, u"EXECUTING"_s, u"COMPLETED"_s, u"ERROR"_s,
                                     u"ABORTED"_s, u"UNKNOWN"_s, u"HELD"_s, u"SUSPENDED"_s, u"ARCHIVED"_s };
    return phases;
}

/// The terminal UWS phases.
bool ended(const QString &phase)
{
    return phase == "COMPLETED"_L1 || phase == "ERROR"_L1 || phase == "ABORTED"_L1
           || phase == "ARCHIVED"_L1;
}

/// A DALI error body ("UsageError: …") as the message, else the body.
QString daliMessage(const QByteArray &body, const QString &fallback)
{
    const QString text = QString::fromUtf8(body).trimmed();
    if (text.isEmpty())
        return fallback;
    return text.section(u'\n', 0, 0).left(500);
}

int portOf(const QUrl &url)
{
    return url.port(url.scheme() == "https"_L1 ? 443 : 80);
}

/// Same scheme, host and port: the only place the backend's token may go.
bool sameOrigin(const QUrl &a, const QUrl &b)
{
    return a.scheme().toLower() == b.scheme().toLower() && a.host().toLower() == b.host().toLower()
           && portOf(a) == portOf(b);
}

/// ``jobUrl`` with ``segment`` appended to its path; the query stays, and
/// the path is handled encoded, so a percent-encoded segment of the node's
/// own path survives.
QString jobResource(const QString &jobUrl, const QString &segment)
{
    QUrl url(jobUrl);
    QString path = url.path(QUrl::FullyEncoded);
    while (path.endsWith(u'/'))
        path.chop(1);
    url.setPath(path + u'/' + segment, QUrl::StrictMode);
    return url.toString(QUrl::FullyEncoded);
}

}  // namespace

NodeSodaClient::NodeSodaClient(const QString &serviceUrl, const QString &backendToken)
    : m_serviceUrl(serviceUrl.trimmed()), m_token(backendToken)
{
    while (m_serviceUrl.endsWith(u'/'))
        m_serviceUrl.chop(1);
}

QString NodeSodaClient::serviceUrlOn(const QString &backendBaseUrl, const QString &advertisedUrl,
                                     const QString &advertisedBase)
{
    QString base = backendBaseUrl.trimmed();
    while (base.endsWith(u'/'))
        base.chop(1);
    if (base.isEmpty())
        return advertisedUrl;
    QString advBase = advertisedBase.trimmed();
    while (advBase.endsWith(u'/'))
        advBase.chop(1);
    const QString advertised = advertisedUrl.trimmed();
    QString suffix;
    // The service under its own backend: what follows the backend's base.
    const bool underBase = !advBase.isEmpty() && advertised.startsWith(advBase)
                           && (advertised.size() == advBase.size() || advertised[advBase.size()] == u'/'
                               || advertised[advBase.size()] == u'?');
    if (underBase) {
        suffix = advertised.mid(advBase.size());
    } else {
        const QUrl adv(advertised);
        suffix = adv.path(QUrl::FullyEncoded);
        if (adv.hasQuery())
            suffix += u'?' + adv.query(QUrl::FullyEncoded);
    }
    if (!suffix.isEmpty() && !suffix.startsWith(u'/') && !suffix.startsWith(u'?'))
        suffix.prepend(u'/');
    return base + suffix;
}

QByteArray NodeSodaClient::encodeForm(const Params &params)
{
    QByteArray body;
    for (const auto &[key, value] : params) {
        if (!body.isEmpty())
            body += '&';
        body += QUrl::toPercentEncoding(key) + '=' + QUrl::toPercentEncoding(value);
    }
    return body;
}

NodeSodaJobResult NodeSodaClient::parseJobJson(const QByteArray &body)
{
    NodeSodaJobResult r;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        r.error = u"The node's answer is not a JSON job."_s;
        return r;
    }
    const QJsonObject job = doc.object();
    const QString phase = job.value(u"phase"_s).toString();
    if (phase.isEmpty()) {
        r.error = u"The node's answer has no job phase."_s;
        return r;
    }
    if (!uwsPhases().contains(phase)) {
        // Not a phase UWS has: the answer is not one to wait on.
        r.error = u"The node's answer has a phase UWS does not define: %1"_s.arg(phase.left(40));
        return r;
    }
    r.phase = phase;
    r.outputPath = job.value(u"output_path"_s).toString();
    r.applied = job.value(u"applied"_s).toString();
    r.noData = job.value(u"no_data"_s).toString();
    const QJsonArray results = job.value(u"results"_s).toArray();
    if (!results.isEmpty())
        r.resultUrl = results.first().toObject().value(u"href"_s).toString();
    const QString error = job.value(u"error"_s).toString();
    if (r.phase == "COMPLETED"_L1) {
        // A product, or the service's word that the selection was empty.
        r.valid = !r.outputPath.isEmpty() || !r.noData.isEmpty();
        if (!r.valid)
            r.error = u"The job completed without a product."_s;
    } else if (r.phase == "ERROR"_L1) {
        r.error = error.isEmpty() ? u"The job failed on the node."_s : error;
    } else if (r.phase == "ABORTED"_L1) {
        r.error = error.isEmpty() ? u"The job was aborted."_s : error;
    }
    return r;
}

NodeSodaClient::Exchange NodeSodaClient::request(const QByteArray &verb, const QString &url,
                                                 const QByteArray &form, std::atomic_bool *cancel,
                                                 int deadlineMs) const
{
    Exchange x;
    if (deadlineMs <= 0) {
        x.error = u"no time left for the request"_s;
        return x;
    }
    QNetworkRequest req{ QUrl(url) };
    req.setRawHeader("Accept", "application/json");
    if (!m_token.isEmpty())
        req.setRawHeader("X-Visivo-Token", m_token.toUtf8());
    // The job's creation answers 303 to the job: read where, do not follow
    // (a redirect would turn the POST into a GET of the job's XML, and could
    // carry the token to another host).
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(std::min(kInactivityTimeoutMs, deadlineMs));
    if (verb == "POST")
        req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);

    auto *nam = threadLocalNam();
    QNetworkReply *reply = verb == "POST" ? nam->post(req, form) : nam->get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    bool timedOut = false;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();          // → finished() → loop.quit()
    });
    deadline.start(deadlineMs);
    // The cancel flag is set on the UI thread; the reply may only be touched
    // here, so it is polled from this loop.
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
    const QUrl target = reply->header(QNetworkRequest::LocationHeader).toUrl();
    x.location = target.isEmpty() ? QString() : QUrl(url).resolved(target).toString();
    const bool redirect = x.status >= 300 && x.status < 400;
    if (x.cancelled) {
        x.error = u"cancelled"_s;
    } else if (timedOut) {
        x.error = u"no answer from %1 within %2 s"_s.arg(QUrl(url).host()).arg((deadlineMs + 999) / 1000);
    } else if (reply->error() != QNetworkReply::NoError && !redirect) {
        x.error = x.status > 0 ? daliMessage(x.body, u"HTTP %1: %2"_s.arg(x.status).arg(reply->errorString()))
                               : reply->errorString();
    }
    reply->deleteLater();
    return x;
}

NodeSodaJobResult NodeSodaClient::abortJob(const QString &jobUrl, const QString &lastPhase) const
{
    NodeSodaJobResult r;
    r.jobUrl = jobUrl;
    r.phase = lastPhase;
    // The abort is the cancellation itself: not cancellable, and the whole
    // of it (the request and the confirmation) has one time budget.
    QElapsedTimer budget;
    budget.start();
    auto left = [&]() { return static_cast<int>(kAbortBudgetMs - budget.elapsed()); };
    const Exchange abort = request("POST", jobResource(jobUrl, u"phase"_s), encodeForm({ { u"PHASE"_s, u"ABORT"_s } }),
                                   nullptr, left());
    // UWS answers 303 to the job itself; anything else that is not a plain
    // success is no confirmation (a 302 to a login page least of all).
    const bool accepted = abort.error.isEmpty()
                          && ((abort.status >= 200 && abort.status < 300)
                              || (abort.status == 303 && sameOrigin(QUrl(jobUrl), QUrl(abort.location))));
    const QString unconfirmed = u"cancelled, but the node did not confirm the abort (%1); the job may still be running there"_s;
    if (!accepted) {
        r.error = unconfirmed.arg(abort.error.isEmpty() ? u"HTTP %1"_s.arg(abort.status) : abort.error);
        return r;
    }
    // Confirmed by the job's own phase.
    QString why = u"the phase did not change"_s;
    while (left() > 0) {
        const Exchange poll = request("GET", jobUrl, {}, nullptr, left());
        if (poll.error.isEmpty()) {
            const NodeSodaJobResult view = parseJobJson(poll.body);
            if (!view.phase.isEmpty()) {
                r.phase = view.phase;
                if (ended(view.phase)) {
                    r.error = view.phase == "ABORTED"_L1 ? u"cancelled"_s
                                                          : u"cancelled, but the job had already ended as %1"_s.arg(view.phase);
                    return r;
                }
            } else {
                why = view.error;
            }
        } else {
            why = poll.error;
        }
        QThread::msleep(std::min(kMaxPollMs / 2, std::max(0, left())));
    }
    r.error = unconfirmed.arg(why);
    return r;
}

NodeSodaJobResult NodeSodaClient::run(const Params &params, std::atomic_bool *cancel,
                                      const std::function<void(double)> &progress) const
{
    NodeSodaJobResult r;
    if (m_serviceUrl.isEmpty()) {
        r.error = u"The node advertises no SODA service."_s;
        return r;
    }
    Params all = params;
    all.append({ u"PHASE"_s, u"RUN"_s });
    const Exchange created = request("POST", m_serviceUrl, encodeForm(all), cancel, kRequestDeadlineMs);
    if (created.cancelled) {
        // The request may have reached the node: a job nobody waits for
        // runs to its end there and is reclaimed by the node's own rules.
        r.error = u"cancelled before the node confirmed the job"_s;
        return r;
    }
    if (!created.error.isEmpty()) {
        r.error = created.error;
        return r;
    }
    if (created.location.isEmpty()) {
        // A service that answers the job itself instead of redirecting to it.
        const NodeSodaJobResult direct = parseJobJson(created.body);
        if (direct.phase.isEmpty()) {
            r.error = u"The node did not say where the job is (HTTP %1)."_s.arg(created.status);
            return r;
        }
        if (ended(direct.phase))
            return direct;
        r.error = u"The node answered a running job without its address."_s;
        return r;
    }
    // The token goes with every request about the job: only to the service's
    // own origin, whatever the node put in Location.
    if (!sameOrigin(QUrl(m_serviceUrl), QUrl(created.location))) {
        r.error = u"The node placed the job on another origin (%1); not followed."_s.arg(
                QUrl(created.location).host());
        return r;
    }
    const QString jobUrl = created.location;
    r.jobUrl = jobUrl;

    int pollMs = kFirstPollMs;
    int failures = 0;
    QString lastPhase;
    for (;;) {
        // Wait for the next poll, watching the cancel flag meanwhile.
        for (int waited = 0; waited < pollMs; waited += kCancelCheckMs) {
            if (cancel && cancel->load())
                return abortJob(jobUrl, lastPhase);
            QThread::msleep(kCancelCheckMs);
        }
        pollMs = std::min(kMaxPollMs, pollMs * 2);

        const Exchange poll = request("GET", jobUrl, {}, cancel, kRequestDeadlineMs);
        if (poll.cancelled)
            return abortJob(jobUrl, lastPhase);
        NodeSodaJobResult view;
        if (poll.error.isEmpty())
            view = parseJobJson(poll.body);
        if (!poll.error.isEmpty() || view.phase.isEmpty()) {
            // A poll that failed is not a job that failed: try again, within
            // bounds; past them the job is lost to us and stopped if it can be.
            if (++failures < kMaxPollFailures)
                continue;
            const QString why = poll.error.isEmpty() ? view.error : poll.error;
            NodeSodaJobResult lost = abortJob(jobUrl, lastPhase);
            lost.error = u"lost track of the job on the node (%1); %2"_s.arg(why, lost.error);
            return lost;
        }
        failures = 0;
        view.jobUrl = jobUrl;
        lastPhase = view.phase;
        if (progress) {
            const QJsonObject job = QJsonDocument::fromJson(poll.body).object();
            if (job.contains(u"progress"_s))
                progress(job.value(u"progress"_s).toDouble());
        }
        if (ended(view.phase))
            return view;
    }
}
