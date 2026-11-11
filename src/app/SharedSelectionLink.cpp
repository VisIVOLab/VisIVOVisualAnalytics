#include "SharedSelectionLink.h"

#include "BackendClient.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <iterator>

using namespace Qt::StringLiterals;

namespace {
/// The longest the hub is asked to wait (seconds; it caps at 25), and the
/// transfer timeout above it.
constexpr int kWaitS = 20;
constexpr int kTransferTimeoutMs = (kWaitS + 10) * 1000;
constexpr int kBackoffMs[] = { 1000, 2000, 5000, 10000, 30000 };

QString newId(int n) { return QUuid::createUuid().toString(QUuid::Id128).left(n); }

int httpStatus(QNetworkReply *r)
{
    return r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}
} // namespace

SharedSelectionLink::SharedSelectionLink(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this))
{
    m_view.client = u"desktop-"_s + newId(12);
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &SharedSelectionLink::poll);
}

SharedSelectionLink::~SharedSelectionLink()
{
    stop();
}

void SharedSelectionLink::start(const QString &baseUrl, const QString &token, const QString &session,
                                const QString &datasetId, int fullW, int fullH, int fullD)
{
    if (m_running && baseUrl == m_baseUrl && session == m_session && datasetId == m_datasetId)
        return;
    stop();
    m_baseUrl = baseUrl;
    m_token = token;
    m_session = session;
    m_datasetId = datasetId;
    m_view.full[0] = fullW; m_view.full[1] = fullH; m_view.full[2] = fullD;
    m_view.shownNonce.clear();
    m_view.localNonce.clear();
    m_view.floorSeq = 0;
    m_lastSeq = m_seenSeq = 0;
    m_instance.clear();
    m_primed = false;
    m_fails = 0;
    m_running = !datasetId.isEmpty() && !baseUrl.isEmpty();
    // The first reconcile primes: the hub's current entry becomes the cursor
    // and the floor, never drawn (an entry made before this window followed
    // the hub must not replace what is on screen). Then the long poll.
    if (m_running) reconcile();
}

void SharedSelectionLink::stop()
{
    m_running = false;
    ++m_generation;
    m_reconciling = m_reconcileAgain = false;
    m_retry.stop();
    for (QPointer<QNetworkReply> *p : { &m_poll, &m_rec }) {
        if (*p) {
            QNetworkReply *r = *p;
            *p = nullptr;
            r->abort();
        }
    }
}

void SharedSelectionLink::noteLocalSelection()
{
    m_view = visivo::noteLocal(m_view, m_seenSeq);
}

bool SharedSelectionLink::noteInstance(const QString &instance)
{
    const visivo::InstanceVerdict v = visivo::instanceVerdict(m_instance, instance);
    if (v == visivo::InstanceVerdict::Reset) {
        // The backend restarted: its seqs began again at 1.
        m_lastSeq = m_seenSeq = 0;
        m_view.floorSeq = 0;
        m_view.shownNonce.clear();
        m_view.localNonce.clear();
    }
    if (v != visivo::InstanceVerdict::Same) m_instance = instance;
    return v == visivo::InstanceVerdict::Reset;
}

void SharedSelectionLink::reject(qint64 seq)
{
    // An entry this window cannot use: never asked for again.
    m_lastSeq = std::max(m_lastSeq, seq);
    m_view.floorSeq = std::max(m_view.floorSeq, seq);
}

void SharedSelectionLink::poll()
{
    if (!m_running || m_poll) return;
    QUrl url(m_baseUrl + u"/v1/share/selection"_s);
    QUrlQuery q;
    q.addQueryItem(u"dataset_id"_s, m_datasetId);
    q.addQueryItem(u"after"_s, QString::number(m_lastSeq));
    q.addQueryItem(u"wait"_s, QString::number(kWaitS));
    url.setQuery(q);
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_session);
    req.setTransferTimeout(kTransferTimeoutMs);
    QNetworkReply *reply = m_nam->get(req);
    m_poll = reply;
    const quint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
        reply->deleteLater();
        if (gen != m_generation) return;     // stopped or restarted meanwhile
        m_poll = nullptr;
        onPollFinished(reply);
    });
}

void SharedSelectionLink::retryLater()
{
    ++m_fails;
    const int n = int(std::size(kBackoffMs));
    m_retry.start(kBackoffMs[std::min(m_fails, n) - 1]);
}

void SharedSelectionLink::onPollFinished(QNetworkReply *reply)
{
    if (!m_running) return;
    const int status = httpStatus(reply);
    const QByteArray body = reply->readAll();
    if (visivo::shareRouteUnsupported(status, body)) {
        m_running = false;
        emit stopped(u"this backend does not share selections"_s);
        return;
    }
    if (status != 200 && status != 204) {
        // Among them a 404 for a session or cube the backend does not know
        // (a restart): retried, a new pairing recovers it.
        retryLater();
        return;
    }
    m_fails = 0;
    // Anything (a new entry, or the timeout: the safety net) -> reconcile,
    // which polls again when it is done.
    reconcile();
}

QNetworkReply *SharedSelectionLink::getLatest()
{
    QUrl url(m_baseUrl + u"/v1/share/selection"_s);
    QUrlQuery q;
    q.addQueryItem(u"dataset_id"_s, m_datasetId);
    q.addQueryItem(u"after"_s, u"0"_s);
    q.addQueryItem(u"wait"_s, u"0"_s);
    url.setQuery(q);
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_session);
    req.setTransferTimeout(kTransferTimeoutMs);
    return m_nam->get(req);
}

void SharedSelectionLink::reconcile()
{
    if (!m_running) return;
    if (m_reconciling) {
        m_reconcileAgain = true;
        return;
    }
    m_reconciling = true;
    m_reconcileAgain = false;
    QNetworkReply *reply = getLatest();
    m_rec = reply;
    const quint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
        reply->deleteLater();
        if (gen != m_generation) return;
        m_rec = nullptr;
        reconcileStep1(reply, gen);
    });
}

void SharedSelectionLink::reconcileStep1(QNetworkReply *reply, quint64 gen)
{
    const int status = httpStatus(reply);
    const QByteArray body = reply->readAll();
    if (visivo::shareRouteUnsupported(status, body)) {
        m_running = false;
        m_reconciling = false;
        emit stopped(u"this backend does not share selections"_s);
        return;
    }
    if (status != 200 && status != 204) {
        m_reconciling = false;
        retryLater();
        return;
    }
    visivo::SharedSelectionInfo latest;
    const bool has = status == 200;
    if (has) {
        latest = visivo::parseSharedSelectionInfo(body);
        noteInstance(latest.instance);
        m_seenSeq = std::max(m_seenSeq, latest.seq);
    }
    if (!m_primed) {
        if (has) {
            m_lastSeq = std::max(m_lastSeq, latest.seq);
            m_view.floorSeq = std::max(m_view.floorSeq, latest.seq);
        }
        m_primed = true;
        reconcileDone();
        return;
    }
    if (has && !latest.valid) {
        reject(latest.seq);   // unreadable: the long poll must not spin on it
        reconcileDone();
        return;
    }
    const visivo::ReconcileAction action = visivo::reconcileDecision(has ? &latest : nullptr, m_view);
    if (action == visivo::ReconcileAction::None || action == visivo::ReconcileAction::Skip) {
        if (has && latest.valid) m_lastSeq = std::max(m_lastSeq, latest.seq);
        reconcileDone();
        return;
    }
    // Fetch its mask, confirm it is still the latest, draw it.
    const bool own = action == visivo::ReconcileAction::Own;
    const QString shownBefore = m_view.shownNonce;
    const qint64 floorBefore = m_view.floorSeq;
    QUrl url(m_baseUrl + u"/v1/share/selection/mask"_s);
    QUrlQuery q;
    q.addQueryItem(u"dataset_id"_s, m_datasetId);
    q.addQueryItem(u"seq"_s, QString::number(latest.seq));
    url.setQuery(q);
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_session);
    req.setTransferTimeout(120000);
    QElapsedTimer t;
    t.start();
    QNetworkReply *mr = m_nam->get(req);
    m_rec = mr;
    connect(mr, &QNetworkReply::finished, this, [this, mr, gen, latest, t, own, shownBefore, floorBefore]() {
        mr->deleteLater();
        if (gen != m_generation) return;
        m_rec = nullptr;
        const int ms = httpStatus(mr);
        const QByteArray mask = mr->readAll();
        if (ms == 404) {                     // superseded meanwhile: read again
            m_reconcileAgain = true;
            reconcileDone();
            return;
        }
        const qint64 voxels = qint64(latest.dims[0]) * latest.dims[1] * latest.dims[2];
        if (ms != 200 || visivo::sharedMaskCount(mask, voxels) != latest.count) {
            if (ms != 200) {
                m_reconciling = false;
                retryLater();
                return;
            }
            reject(latest.seq);   // not the mask announced: skip it for good
            reconcileDone();
            return;
        }
        // Still the latest, and nothing local meanwhile?
        QNetworkReply *cr = getLatest();
        m_rec = cr;
        connect(cr, &QNetworkReply::finished, this, [this, cr, gen, latest, mask, t, own, shownBefore, floorBefore]() {
            cr->deleteLater();
            if (gen != m_generation) return;
            m_rec = nullptr;
            const int cs = httpStatus(cr);
            const QByteArray cbody = cr->readAll();
            visivo::SharedSelectionInfo now;
            if (cs == 200) {
                now = visivo::parseSharedSelectionInfo(cbody);
                noteInstance(now.instance);
                m_seenSeq = std::max(m_seenSeq, now.seq);
            }
            if (!visivo::confirmShareDraw(cs == 200 ? &now : nullptr, latest.seq, shownBefore,
                                          floorBefore, m_view.shownNonce, m_view.floorSeq)) {
                m_reconcileAgain = true;
                reconcileDone();
                return;
            }
            m_view.shownNonce = latest.nonce;
            m_lastSeq = std::max(m_lastSeq, latest.seq);
            emit received(latest, mask, t.elapsed(), own);
            if (gen == m_generation) reconcileDone();
        });
    });
}

void SharedSelectionLink::reconcileDone()
{
    m_reconciling = false;
    if (!m_running) return;
    if (m_reconcileAgain) {
        reconcile();
        return;
    }
    if (!m_retry.isActive()) poll();
}

void SharedSelectionLink::publish(const QString &kind, const QByteArray &mask,
                                  const std::array<int, 3> &origin, const std::array<int, 3> &dims,
                                  const QJsonObject &meta)
{
    if (m_baseUrl.isEmpty() || m_datasetId.isEmpty()) {
        emit published(0, 0, u"the selection link is not started"_s, 0);
        return;
    }
    // The selection is on screen now: it is what this window shows, and every
    // entry read so far is older than it.
    const QString nonce = newId(16);
    m_view.floorSeq = std::max(m_view.floorSeq, m_seenSeq);
    m_view.shownNonce = nonce;
    m_view.localNonce = nonce;
    QJsonObject withNonce = meta;
    withNonce.insert(u"nonce"_s, nonce);
    QUrl url(m_baseUrl + u"/v1/share/selection"_s);
    url.setQuery(visivo::sharePublishQuery(m_datasetId, kind, m_view.client, origin, dims, withNonce));
    QElapsedTimer t;
    t.start();
    post(url, mask, nonce, t, /*retried=*/false);
}

void SharedSelectionLink::post(const QUrl &url, const QByteArray &mask, const QString &nonce,
                               QElapsedTimer t, bool retried)
{
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_session);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/octet-stream"_s);
    // Settled with an unknown outcome after this, like any failure.
    req.setTransferTimeout(kPublishTimeoutMs);
    QNetworkReply *reply = m_nam->post(req, mask);
    const quint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, t, gen, url, mask, nonce, retried]() {
        reply->deleteLater();
        const int status = httpStatus(reply);
        const QByteArray body = reply->readAll();
        const visivo::PublishOutcome outcome = visivo::publishOutcomeOf(status);
        if (outcome == visivo::PublishOutcome::Retry && !retried && gen == m_generation) {
            // The session's compute slots are busy: once more, shortly.
            QTimer::singleShot(kRetry429Ms, this, [this, url, mask, nonce, t, gen]() {
                if (gen == m_generation) post(url, mask, nonce, t, /*retried=*/true);
            });
            return;
        }
        if (outcome == visivo::PublishOutcome::Kept) {
            const QJsonObject o = QJsonDocument::fromJson(body).object();
            // A restart since the last read: the old seqs go, but this
            // selection, kept by the new instance, is still the one on screen.
            if (gen == m_generation && noteInstance(o.value("instance"_L1).toString()))
                m_view.shownNonce = m_view.localNonce = nonce;
            emit published(qint64(o.value("seq"_L1).toDouble()), qint64(o.value("count"_L1).toDouble()),
                           QString(), t.elapsed());
        } else {
            QString why = QJsonDocument::fromJson(body).object().value("detail"_L1).toString().left(300);
            if (why.isEmpty()) why = QString::fromUtf8(body.left(300));
            if (why.isEmpty()) why = reply->errorString();
            if ((outcome == visivo::PublishOutcome::Refused || outcome == visivo::PublishOutcome::Retry)
                && gen == m_generation && m_view.localNonce == nonce) {
                // The hub will not hold it: it stays on screen, local, and is
                // no longer this window's latest own selection.
                m_view.localNonce.clear();
            }
            emit published(0, 0, u"HTTP %1: %2"_s.arg(status).arg(why), t.elapsed());
        }
        // Settled, whatever the outcome (the hub may have kept it although the
        // answer was lost): read the hub and converge.
        if (gen == m_generation) reconcile();
    });
}
