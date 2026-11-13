#include "HeadsetHubLink.h"

#include "BackendClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <iterator>

using namespace Qt::StringLiterals;

namespace {
constexpr int kTransferTimeoutMs = (HeadsetHubLink::kWaitS + 10) * 1000;
constexpr int kPresenceTimeoutMs = 8000;
constexpr int kBackoffMs[] = { 1000, 2000, 5000, 10000, 30000 };

int httpStatus(QNetworkReply *r)
{
    return r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

/// What is connected, as text: presence is announced only when it changes.
QString presenceKey(const QList<VrHub::Headset> &headsets)
{
    QStringList parts;
    for (const VrHub::Headset &h : headsets)
        parts << u"%1|%2|%3|%4|%5|%6|%7"_s.arg(h.client, h.transport, h.label)
                         .arg(h.inVr ? 1 : 0)
                         .arg(h.cube.datasetId, h.cube.path, h.staged.path);
    return parts.join(u'\n');
}
} // namespace

QHash<QString, QPointer<HeadsetHubLink>> &HeadsetHubLink::registry()
{
    static QHash<QString, QPointer<HeadsetHubLink>> links;
    return links;
}

QString HeadsetHubLink::keyOf(const QString &baseUrl, const QString &session)
{
    QString url = baseUrl;
    while (url.endsWith(u'/'))
        url.chop(1);
    return url + u'\n' + session;
}

HeadsetHubLink *HeadsetHubLink::forSession(const QString &baseUrl, const QString &token,
                                           const QString &session)
{
    if (baseUrl.isEmpty())
        return nullptr;
    const QString key = keyOf(baseUrl, session);
    QPointer<HeadsetHubLink> &slot = registry()[key];
    if (!slot)
        slot = new HeadsetHubLink(baseUrl, token, session);
    else if (!token.isEmpty() && token != slot->m_token)
        slot->m_token = token;   // a refreshed token: the next requests carry it
    return slot;
}

HeadsetHubLink::HeadsetHubLink(const QString &baseUrl, const QString &token, const QString &session)
    : QObject(nullptr), m_nam(new QNetworkAccessManager(this)), m_baseUrl(baseUrl), m_token(token),
      m_session(session)
{
    m_activityRetry.setSingleShot(true);
    connect(&m_activityRetry, &QTimer::timeout, this, &HeadsetHubLink::pollActivity);
    m_presenceTimer.setInterval(kPresenceIntervalMs);
    connect(&m_presenceTimer, &QTimer::timeout, this, &HeadsetHubLink::refreshPresence);
}

HeadsetHubLink::~HeadsetHubLink()
{
    stop();
    const QString key = keyOf(m_baseUrl, m_session);
    if (registry().value(key) == this || !registry().value(key))
        registry().remove(key);
}

void HeadsetHubLink::addUser(QObject *user)
{
    if (!user || m_users.contains(user))
        return;
    m_users.insert(user);
    connect(user, &QObject::destroyed, this, [this, user]() { removeUser(user); });
    if (!m_running && !m_unsupported)
        start();
}

void HeadsetHubLink::removeUser(QObject *user)
{
    if (!m_users.remove(user))
        return;
    disconnect(user, &QObject::destroyed, this, nullptr);
    if (m_users.isEmpty()) {
        stop();
        // Gone from the registry now, so a window opened meanwhile gets a
        // fresh link rather than this one on its way out.
        registry().remove(keyOf(m_baseUrl, m_session));
        deleteLater();
    }
}

void HeadsetHubLink::start()
{
    m_running = true;
    m_fails = 0;
    m_presenceTimer.start();
    refreshPresence();
    pollActivity();
}

void HeadsetHubLink::stop()
{
    m_running = false;
    ++m_generation;
    m_activityRetry.stop();
    m_presenceTimer.stop();
    for (QPointer<QNetworkReply> *p : { &m_activityReply, &m_presenceReply }) {
        if (*p) {
            QNetworkReply *r = *p;
            *p = nullptr;
            r->abort();
        }
    }
}

QNetworkReply *HeadsetHubLink::get(const QUrl &url, int transferTimeoutMs)
{
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_session);
    req.setTransferTimeout(transferTimeoutMs);
    return m_nam->get(req);
}

void HeadsetHubLink::markUnsupported()
{
    if (m_unsupported)
        return;
    m_unsupported = true;
    stop();
    const bool hadHeadsets = !m_headsets.isEmpty();
    m_headsets.clear();
    m_presenceKey.clear();
    if (hadHeadsets)
        emit headsetsChanged();
    emit stopped(u"this backend has no headset hub"_s);
}

// ── Presence ────────────────────────────────────────────────────────────────

void HeadsetHubLink::refreshPresence()
{
    if (!m_running || m_presenceReply)
        return;
    QNetworkReply *reply = get(QUrl(m_baseUrl + u"/v1/vr/headset"_s), kPresenceTimeoutMs);
    m_presenceReply = reply;
    const quint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
        reply->deleteLater();
        if (gen != m_generation)
            return;
        m_presenceReply = nullptr;
        onPresence(reply);
    });
}

void HeadsetHubLink::onPresence(QNetworkReply *reply)
{
    const VrHub::Presence p = VrHub::parsePresence(httpStatus(reply), reply->readAll(),
                                                   reply->errorString());
    if (p.unsupported) {
        markUnsupported();
        return;
    }
    // A failed read changes nothing: a headset is not taken for gone because
    // one poll was lost (the hub itself forgets a page after 45 s).
    if (!p.ok)
        return;
    const QString key = presenceKey(p.headsets);
    m_headsets = p.headsets;
    if (key != m_presenceKey) {
        m_presenceKey = key;
        emit headsetsChanged();
    }
}

// ── Activity ────────────────────────────────────────────────────────────────

void HeadsetHubLink::pollActivity()
{
    if (!m_running || m_activityReply)
        return;
    QUrl url(m_baseUrl + u"/v1/vr/activity"_s);
    QUrlQuery q;
    q.addQueryItem(u"after"_s, QString::number(m_after));
    q.addQueryItem(u"wait"_s, QString::number(kWaitS));
    url.setQuery(q);
    QNetworkReply *reply = get(url, kTransferTimeoutMs);
    m_activityReply = reply;
    const quint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
        reply->deleteLater();
        if (gen != m_generation)
            return;
        m_activityReply = nullptr;
        onActivity(reply);
    });
}

void HeadsetHubLink::retryActivityLater()
{
    ++m_fails;
    const int n = int(std::size(kBackoffMs));
    m_activityRetry.start(kBackoffMs[std::min(m_fails, n) - 1]);
}

void HeadsetHubLink::onActivity(QNetworkReply *reply)
{
    if (!m_running)
        return;
    VrHub::ActivityPage page =
            VrHub::parseActivity(httpStatus(reply), reply->readAll(), reply->errorString());
    if (page.unsupported) {
        markUnsupported();
        return;
    }
    if (!page.ok) {
        retryActivityLater();
        return;
    }
    m_fails = 0;
    if (page.next < m_after) {
        // The backend restarted: its seqs began again. What was listed stays,
        // as an older epoch; the new ring is read from its start.
        ++m_epoch;
        m_after = 0;
        pollActivity();
        return;
    }
    for (VrHub::Event &e : page.events)
        e.epoch = m_epoch;
    m_after = std::max(m_after, page.next);
    if (m_log.add(page.events))
        emit activityChanged();
    pollActivity();
}
