// SampHubWorker.cpp – SAMP hub I/O, the callback endpoint and the backend
// bridge, all on the SAMP thread (see SampHubWorker.h)
//
// Hub methods used (SAMP 1.3 Standard Profile): samp.hub.register,
// setXmlrpcCallback, declareMetadata, declareSubscriptions, notify, notifyAll,
// callAll, reply, getRegisteredClients, getMetadata, getSubscriptions,
// getSubscribedClients, ping, unregister.
// Client methods served: samp.client.receiveNotification, receiveCall,
// receiveResponse.

#include "SampHubWorker.h"
#include "BackendClient.h"
#include "SampCallbackServer.h"

#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {

/// A request to the VisIVO backend (auth + session headers). The session
/// header matters: SAMP hand-offs address datasets by id, and under multi-user
/// mode a request without it resolves to a session that holds none of them.
/// The base URL is reconstructed from *url* so it matches the key
/// BackendClient minted the session under.
QNetworkRequest buildBackendRequest(const QUrl &url, const QString &token)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    // Strip only the API path, keeping any reverse-proxy prefix (a backend may
    // live under /visivo): scheme://authority alone would miss the mint key.
    QString baseUrl = url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
    if (const int api = baseUrl.indexOf(QStringLiteral("/v1/")); api > 0)
        baseUrl.truncate(api);
    BackendClient::applySessionHeaders(req, baseUrl, token);
    return req;
}

/// Wait for @p reply with a timeout, on this (the SAMP) thread only.
/// @p httpStatus (optional): the HTTP status, 0 when none came back.
QByteArray waitForReply(QNetworkReply *reply, int timeoutMs, QString &errorOut,
                        int *httpStatus = nullptr)
{
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    if (!reply->isFinished())
        loop.exec();
    QByteArray data;
    if (httpStatus)
        *httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!reply->isFinished()) {
        reply->abort();
        errorOut = QStringLiteral("timed out");
    } else if (reply->error() != QNetworkReply::NoError) {
        errorOut = reply->errorString();
    } else {
        data = reply->readAll();
    }
    reply->deleteLater();
    return data;
}

QByteArray backendGet(QNetworkAccessManager *nam, const QUrl &url, const QString &token,
                      QString &errorOut)
{
    return waitForReply(nam->get(buildBackendRequest(url, token)), 5000, errorOut);
}

QByteArray backendPost(QNetworkAccessManager *nam, const QUrl &url, const QString &token,
                       const QByteArray &body, QString &errorOut, int *httpStatus = nullptr)
{
    return waitForReply(nam->post(buildBackendRequest(url, token), body), 5000, errorOut,
                        httpStatus);
}

}  // namespace

SampHubWorker::SampHubWorker(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_callback(new SampCallbackServer(this))
    , m_backendPendingTimer(new QTimer(this))
    , m_hubPingTimer(new QTimer(this))
    , m_clientsRefreshTimer(new QTimer(this))
{
    m_backendPendingTimer->setInterval(kBackendPollIntervalMs);
    connect(m_backendPendingTimer, &QTimer::timeout,
            this, &SampHubWorker::drainBackendPendingQueue);
    m_hubPingTimer->setInterval(kHubPingIntervalMs);
    connect(m_hubPingTimer, &QTimer::timeout, this, &SampHubWorker::pingHub);
    m_clientsRefreshTimer->setSingleShot(true);
    m_clientsRefreshTimer->setInterval(300);
    connect(m_clientsRefreshTimer, &QTimer::timeout, this, &SampHubWorker::refreshClients);
    m_callback->setHandler([this](const SampXmlRpc::Call &call) { return handleCallback(call); });
}

SampHubWorker::~SampHubWorker() = default;

void SampHubWorker::setBackend(const QString &baseUrl, const QString &token)
{
    // Another backend has not been told the status yet.
    if (baseUrl != m_backendUrl)
        m_lastReportedKey.clear();
    m_backendUrl = baseUrl;
    m_backendToken = token;
}

// ── Connection lifecycle ──────────────────────────────────────────────────────

void SampHubWorker::start()
{
    if (m_connected) {
        // Every connectToHub() gets an outcome, even a redundant one.
        emit connected(m_publicId, m_hubUrl);
        return;
    }
    if (m_starting) {
        // A start cancelled by a stop() is still waiting on the network
        // further down this stack: connect again once it has unwound.
        m_restartRequested = true;
        return;
    }
    m_starting = true;
    m_restartRequested = false;
    const auto notStarting = qScopeGuard([this]() {
        m_starting = false;
        if (m_restartRequested && !m_connected) {
            m_restartRequested = false;
            // A stop() between now and then cancels this restart too.
            const quint64 generation = m_generation;
            QMetaObject::invokeMethod(
                    this,
                    [this, generation]() {
                        if (generation == m_generation)
                            start();
                    },
                    Qt::QueuedConnection);
        }
    });
    // Every step below waits on the network, and the wait runs this thread's
    // event loop: a stop() or a hub disconnect can be handled in between.
    // Then the generation has moved on, and this start gives up.
    const quint64 generation = ++m_generation;
    const auto cancelled = [this, generation]() { return generation != m_generation; };

    QString err;
    if (!readLockFile(m_hubUrl, m_sampSecret, err)) {
        qWarning() << "[SampClient] Hub lockfile not found:" << err;
        emit error(QStringLiteral("SAMP Hub not found: ") + err);
        return;
    }
    if (cancelled())
        return;
    qDebug() << "[SampClient] Hub URL:" << m_hubUrl;

    if (!registerWithHub(generation, err)) {
        if (cancelled())
            return;
        qWarning() << "[SampClient] Registration failed:" << err;
        emit error(QStringLiteral("SAMP registration failed: ") + err);
        return;
    }
    if (cancelled())
        return;

    // Callable before subscribing: a hub refuses subscriptions from a client
    // it cannot call ("Client is not callable").
    QString receiveProblem;
    if (!startCallback(err))
        receiveProblem = QStringLiteral("the hub cannot call VisIVO back (%1)").arg(err);
    if (cancelled())
        return;

    declareMetadata();
    if (cancelled())
        return;

    if (receiveProblem.isEmpty() && !declareSubscriptions(err))
        receiveProblem = QStringLiteral("the hub refused the subscriptions (%1)").arg(err);
    if (cancelled())
        return;

    m_connected = true;
    m_backendPendingTimer->start();
    m_hubPingTimer->start();
    // Signals first, network last: a teardown inside the status report's
    // wait then emits disconnected() after connected(), never before it.
    emit connected(m_publicId, m_hubUrl);
    emit receivingChanged(receiveProblem.isEmpty(), receiveProblem);
    // After connected(): the warning is what must stay on screen.
    if (!receiveProblem.isEmpty()) {
        qWarning() << "[SampClient]" << receiveProblem;
        emit error(QStringLiteral("SAMP: connected, but VisIVO cannot receive messages: %1")
                           .arg(receiveProblem));
    }
    qDebug() << "[SampClient] Connected as" << m_publicId << "callback" << m_callback->url();
    refreshClients();
}

void SampHubWorker::refreshClients()
{
    if (!m_connected)
        return;
    // One refresh at a time: events arriving during its network waits ask
    // for one more, run after it, so the last list emitted is the newest.
    if (m_refreshingClients) {
        m_refreshClientsAgain = true;
        return;
    }
    m_refreshingClients = true;
    reportStatusToBackend();
    m_refreshingClients = false;
    if (m_refreshClientsAgain && m_connected) {
        m_refreshClientsAgain = false;
        m_clientsRefreshTimer->start();
    }
}

void SampHubWorker::stop()
{
    teardown(true);
}

void SampHubWorker::teardown(bool unregister, bool lost)
{
    // First, before any network wait: whatever is suspended in one (a start,
    // a drain) must find the connection gone when it resumes.
    ++m_generation;
    const bool wasConnected = m_connected;
    const bool wasStarting = m_starting;
    const QString privateKey = m_privateKey;
    m_connected = false;
    m_privateKey.clear();
    m_publicId.clear();
    m_hubId.clear();
    m_inbox.clear();
    m_outbox.clear();
    // Those were not sent: the backend hands them out again (leases).
    m_backendAcks.clearInFlight();
    m_pendingCalls.clear();
    m_outstandingCalls.clear();
    m_outstandingOrder.clear();
    m_restartRequested = false;   // the latest request is this stop
    m_backendPendingTimer->stop();
    m_hubPingTimer->stop();
    m_clientsRefreshTimer->stop();
    m_refreshClientsAgain = false;
    m_callback->close();
    if (unregister && !privateKey.isEmpty()) {
        QString err;
        xmlRpcCall(QStringLiteral("samp.hub.unregister"), { privateKey }, err);
    }
    if (!wasConnected) {
        // A connection cancelled half-way: the GUI is still waiting for an outcome.
        if (wasStarting)
            emit disconnected();
        return;
    }
    reportStatusToBackend();
    emit receivingChanged(false, QString());
    if (lost)
        emit hubLost();
    emit disconnected();
    qDebug() << "[SampClient] Disconnected from Hub.";
}

bool SampHubWorker::isHubGone(const QString &error)
{
    return error.contains(QStringLiteral("refused"), Qt::CaseInsensitive)
           || error.contains(QStringLiteral("closed"), Qt::CaseInsensitive)
           || error.contains(QStringLiteral("not found"), Qt::CaseInsensitive);
}

// ── Sending ───────────────────────────────────────────────────────────────────

void SampHubWorker::notify(const QString &recipientId, const QString &mtype,
                           const QVariantMap &params)
{
    if (!m_connected) {
        qWarning() << "[SampClient] notify called while not connected:" << mtype;
        return;
    }
    m_outbox.append({ recipientId, mtype, params, QString(), QString(), QString(), QString(),
                      QString() });
    processOutbox();
}

void SampHubWorker::callAll(const QString &tag, const QString &mtype, const QVariantMap &params,
                            const QString &recipientId)
{
    if (!m_connected) {
        emit callSent(tag, {}, QStringLiteral("not connected to a SAMP hub"));
        return;
    }
    QVariantMap expanded;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        expanded.insert(it.key(), it.value().typeId() == QMetaType::QString
                                          ? QVariant(expandBackendUrl(it.value().toString(),
                                                                      m_backendUrl))
                                          : it.value());
    }
    m_outbox.append({ recipientId, mtype, expanded, QString(), QString(), QString(), tag,
                      QString() });
    processOutbox();
}

void SampHubWorker::askSubscribers(const QString &tag, const QString &mtype)
{
    if (!m_connected) {
        emit subscribersAnswered(tag, {}, QStringLiteral("not connected to a SAMP hub"));
        return;
    }
    const quint64 generation = m_generation;
    QString err;
    const QVariant result = xmlRpcCall(QStringLiteral("samp.hub.getSubscribedClients"),
                                       { m_privateKey, mtype }, err);
    if (generation != m_generation) {
        emit subscribersAnswered(tag, {}, QStringLiteral("the hub connection was lost"));
        return;
    }
    // {client id: annotations}. A hub may list the caller too: VisIVO does
    // not count as a tool that takes what it sends.
    QStringList ids;
    if (err.isEmpty()) {
        const QVariantMap clients = result.toMap();
        for (auto it = clients.constBegin(); it != clients.constEnd(); ++it) {
            if (it.key() != m_publicId)
                ids.append(it.key());
        }
    }
    emit subscribersAnswered(tag, ids, err);
}

void SampHubWorker::forgetCall(const QString &msgTag)
{
    m_outstandingCalls.remove(msgTag);
    m_outstandingOrder.removeAll(msgTag);
}

void SampHubWorker::processOutbox()
{
    // A send waits on the network, and the wait runs this thread's event
    // loop, where the next queued notify() lands: without the guard each
    // would nest inside the previous one, as deep as a burst of row clicks.
    if (m_processingOutbox)
        return;
    m_processingOutbox = true;
    const auto done = qScopeGuard([this]() { m_processingOutbox = false; });
    while (!m_outbox.isEmpty()) {
        const Outgoing next = m_outbox.takeFirst();
        if (!next.replyMsgId.isEmpty()) {
            reply(next.replyMsgId, Samp::response(next.replyError));   // checks the key
            continue;
        }
        const bool sendable =
                m_connected && (next.fromBackend.isEmpty() || next.fromBackend == m_backendUrl);
        const bool delivered =
                sendable && sendNow(next.recipientId, next.mtype, next.params, next.callTag);
        // Not delivered: left unacknowledged, the backend hands it out again
        // once its lease runs out.
        m_backendAcks.finished(next.fromBackend, next.backendJobId, delivered);
    }
}

bool SampHubWorker::sendNow(const QString &recipientId, const QString &mtype,
                            const QVariantMap &params, const QString &callTag)
{
    QVariantMap message;
    message[QStringLiteral("samp.mtype")] = mtype;
    // Standard Profile: every scalar travels as a string (SAMP 1.3 §3.3), and
    // the backend's queue hands numbers through as JSON numbers.
    message[QStringLiteral("samp.params")] = Samp::toSampValue(params);

    const quint64 generation = m_generation;
    QString err;
    bool transportFailed = false;
    QVariant callResult;
    QString msgTag;
    if (!callTag.isEmpty()) {
        // The hub's msg-tag is ours to choose; registered before the call, as
        // an answer may come back while the call's own wait is still running.
        msgTag = QStringLiteral("visivo-%1").arg(++m_callCounter);
        m_outstandingCalls.insert(msgTag, OutstandingCall{ callTag, false, {}, {} });
        m_outstandingOrder.append(msgTag);
        // A tool that never answers must not keep its call for ever.
        while (m_outstandingOrder.size() > kMaxOutstandingCalls)
            forgetCall(m_outstandingOrder.first());
        callResult = recipientId.isEmpty()
                ? xmlRpcCall(QStringLiteral("samp.hub.callAll"), { m_privateKey, msgTag, message },
                             err, &transportFailed)
                : xmlRpcCall(QStringLiteral("samp.hub.call"),
                             { m_privateKey, recipientId, msgTag, message }, err,
                             &transportFailed);
    } else if (recipientId.isEmpty())
        xmlRpcCall(QStringLiteral("samp.hub.notifyAll"), { m_privateKey, message }, err,
                   &transportFailed);
    else
        xmlRpcCall(QStringLiteral("samp.hub.notify"), { m_privateKey, recipientId, message }, err,
                   &transportFailed);
    if (generation != m_generation)
        return false;   // the connection this was sent on is gone; its outcome is moot
    if (!callTag.isEmpty()) {
        // callAll answers with {recipient id: msg-id}: who was asked; call,
        // with the msg-id alone, asked the one recipient.
        QStringList recipients;
        if (err.isEmpty())
            recipients = recipientId.isEmpty() ? callResult.toMap().keys()
                                               : QStringList{ recipientId };
        const auto it = m_outstandingCalls.find(msgTag);
        if (it != m_outstandingCalls.end()) {
            it->recipientsKnown = true;
            for (const QString &id : recipients) {
                if (!it->answeredEarly.contains(id))
                    it->waiting.insert(id);
            }
            if (it->waiting.isEmpty())
                forgetCall(msgTag);   // failed, nobody asked, or all answered already
        }
        emit callSent(callTag, recipients, err);
    }
    if (!err.isEmpty()) {
        qWarning() << "[SampClient] notify error:" << mtype << err;
        // A fault (no such recipient, …) fails this send only; an unreachable
        // hub ends the connection.
        if (transportFailed && isHubGone(err)) {
            teardown(false, /*lost=*/true);
            return false;
        }
        emit error(QStringLiteral("SAMP send of %1 failed: %2").arg(mtype, err));
        // An answer from the hub (no such recipient, …): trying again would
        // get the same one.
        return !transportFailed;
    }
    if (mtype.startsWith(QStringLiteral("table.load."))) {
        const Samp::TableRef ref = Samp::tableRefFrom(params);
        if (!ref.tableId.isEmpty() && !ref.url.isEmpty())
            emit tableSent(ref.tableId, ref.url);
    }
    return true;
}

// ── Receiving ─────────────────────────────────────────────────────────────────

QByteArray SampHubWorker::handleCallback(const SampXmlRpc::Call &call)
{
    const Samp::Inbound in = Samp::decodeClientCall(call, m_privateKey);
    if (!in.error.isEmpty()) {
        qWarning() << "[SampClient] refused callback" << call.method << ":" << in.error;
        return SampXmlRpc::faultResponse(1, in.error);
    }
    if (in.kind == Samp::Inbound::Kind::Response) {
        // An answer to one of VisIVO's callAll()s; anything else is stray.
        qDebug() << "[SampClient] response from" << in.responderId << "tag" << in.msgTag
                 << in.response.value(QStringLiteral("samp.status")).toString();
        const auto it = m_outstandingCalls.find(in.msgTag);
        if (it != m_outstandingCalls.end()) {
            const QString tag = it->tag;
            if (!it->recipientsKnown) {
                it->answeredEarly.insert(in.responderId);   // callAll is still returning
            } else {
                it->waiting.remove(in.responderId);
                if (it->waiting.isEmpty())
                    forgetCall(in.msgTag);
            }
            emit callAnswered(tag, in.responderId, Samp::responseError(in.response));
        }
        return SampXmlRpc::methodResponse(QString());
    }
    // Answer the hub first, act afterwards: the hub waits on this HTTP
    // exchange, and a call is replied to separately (samp.hub.reply).
    m_inbox.append(in.message);
    QMetaObject::invokeMethod(this, &SampHubWorker::processInbox, Qt::QueuedConnection);
    return SampXmlRpc::methodResponse(QString());
}

void SampHubWorker::processInbox()
{
    if (m_processingInbox)
        return;   // the outer call, suspended in a network wait, will get to it
    m_processingInbox = true;
    const auto done = qScopeGuard([this]() { m_processingInbox = false; });
    while (!m_inbox.isEmpty())
        handleMessage(m_inbox.takeFirst());
}

void SampHubWorker::reply(const QString &msgId, const QVariantMap &response)
{
    if (msgId.isEmpty() || m_privateKey.isEmpty())
        return;
    QString err;
    xmlRpcCall(QStringLiteral("samp.hub.reply"), { m_privateKey, msgId, response }, err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] reply to" << msgId << "failed:" << err;
}

void SampHubWorker::replyToCall(const QString &replyToken, const QString &error)
{
    // Only a call of THIS registration, and only once: a load that finishes
    // after a reconnect belongs to a registration the hub has forgotten, even
    // if the new hub handed out the same msg-id. (A call can arrive while
    // start() is still subscribing, so this does not wait for m_connected.)
    const std::optional<QString> pending = m_pendingCalls.take(replyToken, m_generation);
    if (!pending)
        return;
    const QString msgId = *pending;
    // Through the outbox, so that a burst of replies is sent one after the
    // other rather than each inside the previous one's network wait.
    m_outbox.append({ QString(), QString(), QVariantMap(), msgId, error, QString(), QString() });
    processOutbox();
}

void SampHubWorker::pingHub()
{
    if (!m_connected || m_pinging)
        return;
    m_pinging = true;
    const auto done = qScopeGuard([this]() { m_pinging = false; });
    const quint64 generation = m_generation;
    QString err;
    bool transportFailed = false;
    xmlRpcCall(QStringLiteral("samp.hub.ping"), { m_privateKey }, err, &transportFailed);
    if (err.isEmpty() || generation != m_generation)
        return;
    // Gone, or restarted and no longer knows VisIVO: either way, not connected.
    // (A timeout is a busy hub, not a dead one.)
    if ((transportFailed && isHubGone(err))
        || (!transportFailed && err.contains(QStringLiteral("private key"), Qt::CaseInsensitive))) {
        qWarning() << "[SampClient] hub ping failed, disconnecting:" << err;
        emit error(QStringLiteral("SAMP hub no longer reachable: %1").arg(err));
        teardown(false, /*lost=*/true);
    }
}

void SampHubWorker::handleMessage(const SampMessage &msg)
{
    const QString &mt = msg.mtype;
    if (Samp::isHubEvent(mt) && mt != QLatin1String(Samp::kAppPing)) {
        // The private key proves the hub delivered it, not who sent it: any
        // client may address a message to VisIVO. Only the hub itself may
        // say it is shutting down or disconnecting VisIVO.
        if (m_hubId.isEmpty() || msg.senderId != m_hubId) {
            qWarning() << "[SampClient] refused" << mt << "from" << msg.senderId;
            reply(msg.msgId, Samp::response(QStringLiteral("only the hub sends %1").arg(mt)));
            return;
        }
        const quint64 generation = m_generation;
        reply(msg.msgId, Samp::response());
        if (generation != m_generation)
            return;   // reconnected during the reply: the event was for the old registration
        // A hub shutting down may be followed by another one (TOPCAT
        // restarted): worth waiting for. A hub that disconnects VisIVO has
        // chosen to, and is not asked again.
        if (mt == QLatin1String(Samp::kHubShutdown))
            teardown(false, /*lost=*/true);
        else if (mt == QLatin1String(Samp::kHubDisconnect))
            teardown(false);
        else
            m_clientsRefreshTimer->start();   // peers came, went, renamed or resubscribed
        return;
    }
    if (mt == QLatin1String(Samp::kAppPing)) {
        reply(msg.msgId, Samp::response());
        return;
    }

    const QString problem = Samp::validateMessage(msg);
    if (!problem.isEmpty()) {
        qWarning() << "[SampClient] ignored" << mt << "from" << msg.senderId << ":" << problem;
        reply(msg.msgId, Samp::response(problem));
        emit error(QStringLiteral("Ignored a SAMP message from %1: %2").arg(msg.senderId, problem));
        return;
    }
    qDebug() << "[SampClient] Received mtype=" << mt << "from" << msg.senderId;
    // A load is answered by the GUI once it opened (replyToCall); a row,
    // a selection or a position takes effect as it is delivered.
    const bool guiAnswers = Samp::isLoadMType(mt) && !msg.msgId.isEmpty();
    SampMessage delivered = msg;
    if (guiAnswers)
        delivered.replyToken = m_pendingCalls.add(m_generation, msg.msgId);
    emit messageReceived(delivered);
    if (!guiAnswers)
        reply(msg.msgId, Samp::response());
    // The backend keeps an inbox of what was loaded or pointed at; row
    // highlights come in bursts and are of no use to it.
    if (mt != QLatin1String(Samp::kTableHighlightRow)
        && mt != QLatin1String(Samp::kTableSelectRowList))
        forwardToBackend(msg);
}

void SampHubWorker::forwardToBackend(const SampMessage &msg)
{
    if (m_backendUrl.isEmpty())
        return;
    QJsonObject body;
    body[QStringLiteral("mtype")] = msg.mtype;
    body[QStringLiteral("sender_id")] = msg.senderId;
    body[QStringLiteral("msg_id")] = msg.msgId;
    QJsonObject paramsObj;
    for (auto it = msg.params.constBegin(); it != msg.params.constEnd(); ++it)
        paramsObj[it.key()] = QJsonValue::fromVariant(it.value());
    body[QStringLiteral("params")] = paramsObj;

    QString err;
    backendPost(m_nam, QUrl(m_backendUrl + QStringLiteral("/v1/samp/receive")), m_backendToken,
                QJsonDocument(body).toJson(QJsonDocument::Compact), err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] forwardToBackend error:" << err;
}

// ── Backend outbound queue ────────────────────────────────────────────────────

void SampHubWorker::drainBackendPendingQueue()
{
    // The timer keeps firing inside this function's own network waits; one
    // drain at a time keeps the messages in the order the backend queued them.
    if (!m_connected || m_backendUrl.isEmpty() || m_draining)
        return;
    m_draining = true;
    const auto done = qScopeGuard([this]() { m_draining = false; });

    // The messages were queued for this connection: a reconnect while the
    // GET waits must not send them under the new one (client ids change).
    const quint64 generation = m_generation;
    // The files were registered on THIS backend: its URL goes into them even
    // if the backend is changed while the GET waits (then they are dropped).
    const QString backendUrl = m_backendUrl;
    // Messages sent since the last poll first: until the backend hears of
    // them it keeps them, and hands them out again when their lease ends.
    ackBackendMessages();
    if (!m_connected || generation != m_generation || backendUrl != m_backendUrl)
        return;

    QString err;
    // lease=true: the backend keeps each message until it is acknowledged, so
    // one this client fetched and then failed to send (a crash, a lost
    // answer, a hub that went away) is not lost. A backend without leases
    // ignores the parameter and drains the queue as before.
    QUrl pendingUrl(backendUrl + QStringLiteral("/v1/samp/pending"));
    pendingUrl.setQuery(QStringLiteral("lease=true"));
    const QByteArray payload = backendGet(m_nam, pendingUrl, m_backendToken, err);
    if (!err.isEmpty() || payload.isEmpty() || generation != m_generation
        || backendUrl != m_backendUrl)
        return;

    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    const QJsonArray messages = root.value(QStringLiteral("messages")).toArray();
    for (const QJsonValue &mv : messages) {
        if (!m_connected || generation != m_generation || backendUrl != m_backendUrl)
            return;   // the hub or the backend changed while sending the previous one
        const QJsonObject msg = mv.toObject();
        const QString mtype = msg.value(QStringLiteral("mtype")).toString();
        const QString targetId = msg.value(QStringLiteral("target_id")).toString();
        const QString jobId = msg.value(QStringLiteral("job_id")).toString();
        // Handed out again while the first copy still waits in the outbox
        // (a lease that ran out during a slow send): it is sent once.
        if (!m_backendAcks.fetched(jobId))
            continue;
        const QJsonObject rawParams = msg.value(QStringLiteral("params")).toObject();

        QVariantMap params;
        for (auto it = rawParams.constBegin(); it != rawParams.constEnd(); ++it) {
            // Expand the backend URL placeholder so VO tools can fetch the file.
            params[it.key()] = it.value().isString()
                    ? QVariant(expandBackendUrl(it.value().toString(), backendUrl))
                                                     : it.value().toVariant();
        }
        m_outbox.append({ targetId, mtype, params, QString(), QString(), backendUrl, QString(),
                          jobId });
        processOutbox();
    }
    ackBackendMessages();
}

void SampHubWorker::ackBackendMessages()
{
    // Called from inside the drain only (m_draining): one at a time.
    const QString backendUrl = m_backendUrl;
    const QStringList jobs = m_backendAcks.take(backendUrl);
    // Another backend's: it is not the one polled now, and is told if it is
    // chosen again (its messages come back meanwhile, and are sent then).
    if (jobs.isEmpty() || backendUrl.isEmpty())
        return;
    QJsonArray ids;
    for (const QString &id : std::as_const(jobs))
        ids.append(id);
    QString err;
    int status = 0;
    backendPost(m_nam, QUrl(backendUrl + QStringLiteral("/v1/samp/pending/ack")), m_backendToken,
                QJsonDocument(QJsonObject{ { QStringLiteral("job_ids"), ids } })
                        .toJson(QJsonDocument::Compact),
                err, &status);
    // A backend without leases (404) drained them already. Another failure
    // keeps them for the next poll, a few times (SampBackendAcks).
    m_backendAcks.posted(backendUrl, jobs, err.isEmpty(), status);
}

QString SampHubWorker::expandBackendUrl(const QString &tmpl, const QString &backendUrl)
{
    const QString placeholder = QStringLiteral("__BACKEND_BASE_URL__");
    if (!tmpl.contains(placeholder))
        return tmpl;
    return QString(tmpl).replace(placeholder, backendUrl);
}

// ── Registration ──────────────────────────────────────────────────────────────

bool SampHubWorker::registerWithHub(quint64 generation, QString &errorOut)
{
    const QVariant result = xmlRpcCall(QStringLiteral("samp.hub.register"),
                                       { m_sampSecret }, errorOut);
    if (!errorOut.isEmpty() || !result.isValid())
        return false;

    const QVariantMap map = result.toMap();
    const QString privateKey = map.value(QStringLiteral("samp.private-key")).toString();
    const QString publicId = map.value(QStringLiteral("samp.self-id")).toString();
    if (generation != m_generation) {
        // Stopped while the hub was answering: nothing may keep this
        // registration, so it is undone rather than leaked in the hub.
        if (!privateKey.isEmpty()) {
            QString ignored;
            xmlRpcCall(QStringLiteral("samp.hub.unregister"), { privateKey }, ignored);
        }
        errorOut = QStringLiteral("cancelled");
        return false;
    }
    if (privateKey.isEmpty() || publicId.isEmpty()) {
        errorOut = QStringLiteral("Hub returned empty private-key or self-id.");
        return false;
    }
    m_privateKey = privateKey;
    m_publicId = publicId;
    m_hubId = map.value(QStringLiteral("samp.hub-id")).toString();
    return true;
}

bool SampHubWorker::startCallback(QString &errorOut)
{
    if (!m_callback->listen(&errorOut))
        return false;
    xmlRpcCall(QStringLiteral("samp.hub.setXmlrpcCallback"),
               { m_privateKey, m_callback->url() }, errorOut);
    if (!errorOut.isEmpty()) {
        m_callback->close();
        return false;
    }
    return true;
}

void SampHubWorker::declareMetadata()
{
    const QString displayName = QStringLiteral("VisIVO Visual Analytics");
    QVariantMap meta;
    meta[QStringLiteral("samp.name")] = displayName;
    meta[QStringLiteral("samp.description.text")] = QStringLiteral("VisIVO Visual Analytics");
    meta[QStringLiteral("name")] = displayName;
    meta[QStringLiteral("description")] = QStringLiteral("VisIVO Visual Analytics");
    if (!m_backendUrl.isEmpty())
        meta[QStringLiteral("samp.icon.url")] = m_backendUrl + QStringLiteral("/static/VisIVO_128.png");
    meta[QStringLiteral("author.name")] = QStringLiteral("INAF-OACT VisIVO Team");
    meta[QStringLiteral("home.page")] = QStringLiteral("https://visivolab.inaf.it/");

    QString err;
    xmlRpcCall(QStringLiteral("samp.hub.declareMetadata"), { m_privateKey, meta }, err);
    if (!err.isEmpty()) {
        qWarning() << "[SampClient] declareMetadata error:" << err;
        emit error(QStringLiteral("SAMP metadata declaration failed: %1").arg(err));
        return;
    }

    QString verifyErr;
    const QVariant declared = xmlRpcCall(QStringLiteral("samp.hub.getMetadata"),
                                         { m_privateKey, m_publicId }, verifyErr);
    if (!verifyErr.isEmpty()) {
        qWarning() << "[SampClient] getMetadata(self) error:" << verifyErr;
        emit error(QStringLiteral("SAMP metadata verification failed: %1").arg(verifyErr));
        return;
    }
    const QString echoedName =
            declared.toMap().value(QStringLiteral("samp.name")).toString().trimmed();
    if (echoedName != displayName) {
        const QString message =
                QStringLiteral("SAMP hub did not persist the expected client name. "
                               "Expected '%1', got '%2'.")
                        .arg(displayName,
                             echoedName.isEmpty() ? QStringLiteral("<empty>") : echoedName);
        qWarning() << "[SampClient]" << message;
        emit error(message);
    }
}

bool SampHubWorker::declareSubscriptions(QString &errorOut)
{
    // Each subscription maps an MType to an (empty) annotation map.
    QVariantMap subs;
    for (const QString &mt : Samp::subscribedMTypes())
        subs[mt] = QVariantMap();
    xmlRpcCall(QStringLiteral("samp.hub.declareSubscriptions"), { m_privateKey, subs }, errorOut);
    return errorOut.isEmpty();
}

QList<SampClientInfo> SampHubWorker::registeredClients()
{
    QList<SampClientInfo> result;
    if (!m_connected)
        return result;

    // Each call below waits on the network; a teardown in one of those waits
    // makes the rest pointless.
    const quint64 generation = m_generation;
    QString err;
    const QVariant reg = xmlRpcCall(QStringLiteral("samp.hub.getRegisteredClients"),
                                    { m_privateKey }, err);
    if (!err.isEmpty() || !reg.isValid())
        return result;

    for (const QVariant &idv : reg.toList()) {
        if (generation != m_generation)
            return {};
        const QString id = idv.toString();
        if (id == m_publicId)
            continue;
        SampClientInfo info;
        info.id = id;
        err.clear();
        const QVariant meta = xmlRpcCall(QStringLiteral("samp.hub.getMetadata"),
                                         { m_privateKey, id }, err);
        if (err.isEmpty() && meta.isValid()) {
            const QVariantMap m = meta.toMap();
            info.name = m.value(QStringLiteral("samp.name")).toString();
            info.description = m.value(QStringLiteral("samp.description.text")).toString();
            info.iconUrl = m.value(QStringLiteral("samp.icon.url")).toString();
        }
        err.clear();
        const QVariant subs = xmlRpcCall(QStringLiteral("samp.hub.getSubscriptions"),
                                         { m_privateKey, id }, err);
        if (err.isEmpty() && subs.isValid())
            info.subscriptions = subs.toMap().keys();
        result.append(info);
    }
    return result;
}

void SampHubWorker::reportStatusToBackend()
{
    // One report at a time. Its network waits run this thread's event loop,
    // where a disconnect can ask for another: that one runs after, from the
    // state then, so the backend never ends on an older state than the last.
    if (m_reporting) {
        m_reportAgain = true;
        return;
    }
    m_reporting = true;
    const auto done = qScopeGuard([this]() {
        m_reporting = false;
        if (m_reportAgain) {
            m_reportAgain = false;
            QMetaObject::invokeMethod(this, &SampHubWorker::reportStatusToBackend,
                                      Qt::QueuedConnection);
        }
    });
    const quint64 generation = m_generation;
    const QList<SampClientInfo> clients = m_connected ? registeredClients()
                                                      : QList<SampClientInfo>{};
    if (generation != m_generation)
        return;   // disconnected meanwhile: the teardown's report runs next (m_reportAgain)
    const QString key = Samp::hubStatusKey(m_connected, m_hubUrl, m_publicId, clients);
    if (key != m_lastListedKey) {
        m_lastListedKey = key;
        emit clientListChanged(clients);
    }
    // The backend is told again until it has taken this status: a failed
    // report must not leave /v1/samp/status stale until something changes.
    if (m_backendUrl.isEmpty() || key == m_lastReportedKey)
        return;
    const QString backendUrl = m_backendUrl;

    QJsonArray clientsArr;
    for (const SampClientInfo &c : clients) {
        QJsonObject obj;
        obj[QStringLiteral("id")] = c.id;
        obj[QStringLiteral("name")] = c.name;
        clientsArr.append(obj);
    }
    QJsonObject body;
    body[QStringLiteral("connected")] = m_connected;
    body[QStringLiteral("hub_url")] = m_hubUrl;
    body[QStringLiteral("client_id")] = m_publicId;
    body[QStringLiteral("client_name")] = QStringLiteral("VisIVO");
    body[QStringLiteral("registered_clients")] = clientsArr;

    QString err;
    backendPost(m_nam, QUrl(backendUrl + QStringLiteral("/v1/samp/hub-status")), m_backendToken,
                QJsonDocument(body).toJson(QJsonDocument::Compact), err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] reportStatusToBackend error:" << err;
    else if (backendUrl == m_backendUrl)   // not a backend set during the wait
        m_lastReportedKey = key;
}

// ── Hub lockfile ──────────────────────────────────────────────────────────────

bool SampHubWorker::readLockFile(QString &hubUrl, QString &secret, QString &errorOut)
{
    // SAMP 1.3 §3.3: $SAMP_HUB = "std-lockurl:<url>" names the lockfile, a
    // file: or an http(s): URL; otherwise it is ~/.samp.
    QUrl lockUrl = QUrl::fromLocalFile(
            QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                    .absoluteFilePath(QStringLiteral(".samp")));
    const QString sampHub = qEnvironmentVariable("SAMP_HUB").trimmed();
    if (sampHub.startsWith(QStringLiteral("std-lockurl:")))
        lockUrl = QUrl(sampHub.mid(int(qstrlen("std-lockurl:"))));

    QByteArray text;
    if (lockUrl.isLocalFile()) {
        QFile f(lockUrl.toLocalFile());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            errorOut = QStringLiteral("Cannot open %1: %2").arg(f.fileName(), f.errorString());
            return false;
        }
        text = f.read(kMaxLockFileBytes);
    } else if (lockUrl.scheme() == u"http" || lockUrl.scheme() == u"https") {
        QString err;
        text = waitForReply(m_nam->get(QNetworkRequest(lockUrl)), kHubTimeoutMs, err)
                       .left(kMaxLockFileBytes);
        if (!err.isEmpty()) {
            errorOut = QStringLiteral("Cannot read %1: %2").arg(lockUrl.toString(), err);
            return false;
        }
    } else {
        errorOut = QStringLiteral("SAMP_HUB names a lockfile VisIVO cannot read: %1").arg(sampHub);
        return false;
    }

    const Samp::LockInfo info = Samp::parseLockFile(QString::fromUtf8(text));
    hubUrl = info.hubUrl;
    secret = info.secret;
    if (hubUrl.isEmpty()) {
        errorOut = QStringLiteral("samp.hub.xmlrpc.url not found in %1")
                           .arg(lockUrl.isLocalFile() ? lockUrl.toLocalFile() : lockUrl.toString());
        return false;
    }
    return true;
}

// ── XML-RPC transport ─────────────────────────────────────────────────────────

QVariant SampHubWorker::xmlRpcCall(const QString &method, const QVariantList &params,
                                   QString &errorOut, bool *transportFailed)
{
    if (transportFailed)
        *transportFailed = false;
    const QByteArray body = SampXmlRpc::methodCall(method, params);
    QNetworkRequest req{ QUrl(m_hubUrl) };
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("text/xml"));
    // Blocking is fine here: this is the SAMP thread, and the callback server
    // on it keeps answering the hub from inside the wait.
    QString transportError;
    const QByteArray response = waitForReply(m_nam->post(req, body), kHubTimeoutMs, transportError);
    if (!transportError.isEmpty()) {
        if (transportFailed)
            *transportFailed = true;
        errorOut = transportError == QStringLiteral("timed out")
                ? QStringLiteral("XML-RPC call timed out: ") + method
                : transportError;
        return {};
    }
    return SampXmlRpc::parseMethodResponse(response, errorOut);
}
