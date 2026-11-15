#include "VrSpectatorFeed.h"

#include "BackendClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

using namespace Qt::StringLiterals;

namespace {
constexpr int kReadTimeoutMs = (VrSpectator::kWaitS + 10) * 1000;
constexpr int kPostTimeoutMs = 10000;
constexpr int kSnapshotTimeoutMs = 30000;
/// A watch that keeps running out (409) is started again at most this often in a row.
constexpr int kMaxRestarts = 3;

int httpStatus(QNetworkReply *r)
{
    return r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

/// The watch's "off" on @p nam (a manager of its own, detached from the
/// feed), which goes with its reply. Copies of everything: the feed may be
/// gone by the time it is sent.
void postOff(QNetworkAccessManager *nam, const QString &baseUrl, const QString &token,
             const QString &session, const QJsonObject &body)
{
    QNetworkRequest req(QUrl(baseUrl + u"/v1/vr/headset/spectator"_s));
    BackendClient::applySessionHeaders(req, baseUrl, token, session);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    req.setTransferTimeout(kPostTimeoutMs);
    QNetworkReply *off = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(off, &QNetworkReply::finished, nam, [off, nam]() {
        off->deleteLater();
        nam->deleteLater();
    });
}

/// The Qt error of a reply that got no HTTP answer, else empty.
QString qtError(QNetworkReply *r)
{
    return httpStatus(r) == 0 ? r->errorString() : QString();
}
} // namespace

VrSpectatorFeed::VrSpectatorFeed(const QString &baseUrl, const QString &token, const QString &session,
                                 const QString &client, QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)), m_baseUrl(baseUrl), m_token(token),
      m_session(session), m_client(client)
{
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &VrSpectatorFeed::readNext);
    m_clock.start();
}

VrSpectatorFeed::~VrSpectatorFeed()
{
    stop();
}

QNetworkReply *VrSpectatorFeed::post(const QString &path, const QJsonObject &body, int timeoutMs)
{
    QNetworkRequest req(QUrl(m_baseUrl + path));
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_session);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    req.setTransferTimeout(timeoutMs);
    return m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void VrSpectatorFeed::start(int fps, int width)
{
    m_fps = fps;
    m_width = width;
    m_running = true;
    ++m_generation;
    m_fails = 0;
    m_restarts = 0;
    m_rate.clear();
    sendWatch(true);
}

void VrSpectatorFeed::sendWatch(bool on, bool thenRead)
{
    const QString path = u"/v1/vr/headset/spectator"_s;
    if (!on) {
        // Best effort (the backend's lease ends it anyway), and it must outlive
        // this feed: a window closing deletes it right after. The manager that
        // sends it leaves with its reply (and with an "on" still on its way);
        // a new one serves this feed.
        QNetworkAccessManager *nam = m_nam;
        nam->setParent(nullptr);
        m_nam = new QNetworkAccessManager(this);
        const QJsonObject body = VrSpectator::watchBody(m_client, false);
        const QString baseUrl = m_baseUrl, token = m_token, session = m_session;
        // Sent only once an "on" still on its way has been answered: two
        // requests on two connections may be handled in either order, and an
        // "on" handled last would start the watch again. Not `this` as the
        // context: the feed may be gone by then.
        QNetworkReply *pendingOn = m_pendingOn;
        m_pendingOn = nullptr;
        if (pendingOn && pendingOn->isRunning())
            connect(pendingOn, &QNetworkReply::finished, nam,
                    [nam, baseUrl, token, session, body]() { postOff(nam, baseUrl, token, session, body); });
        else
            postOff(nam, baseUrl, token, session, body);
        return;
    }
    QNetworkReply *reply = post(path, VrSpectator::watchBody(m_client, on, m_fps, m_width), kPostTimeoutMs);
    m_pendingOn = reply;
    const quint64 gen = m_generation;
    m_lastProgressMs = m_clock.elapsed();
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, thenRead]() {
        reply->deleteLater();
        if (gen != m_generation || !m_running)
            return;
        const VrSpectator::WatchResult r =
                VrSpectator::interpretWatch(httpStatus(reply), reply->readAll(), qtError(reply));
        if (r.outcome != VrSpectator::WatchResult::Outcome::Watching) {
            finish(r.message.isEmpty() ? u"The backend did not start watching."_s : r.message);
            return;
        }
        if (!r.client.isEmpty())
            m_client = r.client;
        emit watching(r.fps, r.width);
        if (thenRead)
            readNext();
    });
}

void VrSpectatorFeed::readNext()
{
    if (!m_running || m_read)
        return;
    QNetworkRequest req(VrSpectator::frameUrl(m_baseUrl, m_client, m_after));
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_session);
    req.setTransferTimeout(kReadTimeoutMs);
    QNetworkReply *reply = m_nam->get(req);
    m_read = reply;
    const quint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
        reply->deleteLater();
        if (gen != m_generation)
            return;
        m_read = nullptr;
        onFrame(reply);
    });
}

void VrSpectatorFeed::onFrame(QNetworkReply *reply)
{
    if (!m_running)
        return;
    QHash<QByteArray, QByteArray> headers;
    for (const auto &h : reply->rawHeaderPairs())
        headers.insert(h.first.toLower(), h.second);
    VrSpectator::FrameReply r =
            VrSpectator::parseFrameReply(httpStatus(reply), headers, reply->readAll(), qtError(reply));
    // A restarted backend numbers its frames from 1 again.
    const QString instance = QString::fromLatin1(headers.value(QByteArrayLiteral("x-visivo-instance")));
    if (!instance.isEmpty() && !m_instance.isEmpty() && instance != m_instance)
        m_after = 0;
    if (!instance.isEmpty())
        m_instance = instance;
    using Kind = VrSpectator::FrameReply::Kind;
    switch (r.kind) {
    case Kind::Frame: {
        m_fails = 0;
        m_restarts = 0;
        m_after = r.seq;
        m_lastProgressMs = m_clock.elapsed();
        m_info = r;
        ++m_frames;
        m_rate.add(m_clock.elapsed());
        emit frameReady(m_info);
        break;
    }
    case Kind::None:
        m_fails = 0;
        // Watched, yet nothing comes: the page may have paused on a bad link or
        // missed its command (a reload); telling it again costs nothing.
        if (m_clock.elapsed() - m_lastProgressMs > m_rewatchMs)
            sendWatch(true, /*thenRead=*/false);
        break;
    case Kind::NotWatched:
        // The lease ran out (no read for 10 s: a sleeping laptop, a stalled
        // network): start again, a few times in a row at most.
        if (++m_restarts > kMaxRestarts) {
            finish(u"The backend keeps ending the watch."_s);
            return;
        }
        sendWatch(true);
        return;
    case Kind::Gone:
    case Kind::Unsupported:
        finish(r.message);
        return;
    case Kind::Failed:
        ++m_fails;
        emit trouble(r.message);
        m_retry.start(VrSpectator::retryDelayMs(m_fails));
        return;
    }
    readNext();
}

void VrSpectatorFeed::finish(const QString &why)
{
    if (!m_running)
        return;
    m_running = false;
    ++m_generation;
    m_retry.stop();
    if (m_read) {
        QNetworkReply *r = m_read;
        m_read = nullptr;
        r->abort();
    }
    emit ended(why);
}

void VrSpectatorFeed::stop()
{
    if (!m_running)
        return;
    m_running = false;
    ++m_generation;
    m_retry.stop();
    if (m_read) {
        QNetworkReply *r = m_read;
        m_read = nullptr;
        r->abort();
    }
    sendWatch(false);
}

double VrSpectatorFeed::fps() const
{
    return m_rate.fps(m_clock.elapsed());
}

void VrSpectatorFeed::snapshot()
{
    QNetworkReply *reply = post(u"/v1/vr/headset/spectator/snapshot"_s, VrSpectator::snapshotBody(m_client),
                                kSnapshotTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const VrSpectator::SnapshotResult r =
                VrSpectator::interpretSnapshot(httpStatus(reply), reply->readAll(), qtError(reply));
        if (r.ok)
            emit snapshotSaved(r);
        else
            emit snapshotFailed(r.message);
    });
}
