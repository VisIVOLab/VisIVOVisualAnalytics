// SampClient.cpp – the GUI-thread face of the SAMP client
//
// Every method queues work onto the SAMP thread (SampHubWorker) and returns;
// the worker's signals come back here as queued signals and are re-emitted on
// the GUI thread. See SampClient.h for the protocol, SampHubWorker.cpp for the
// network side.

#include "SampClient.h"
#include "BackendClient.h"
#include "SampHubWorker.h"

#include <QDebug>
#include <QMetaObject>
#include <QTimer>

SampClient::SampClient(BackendClient *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_worker(new SampHubWorker)
    , m_retryTimer(new QTimer(this))
{
    qRegisterMetaType<SampMessage>();
    qRegisterMetaType<SampClientInfo>();
    qRegisterMetaType<QList<SampClientInfo>>();

    m_thread.setObjectName(QStringLiteral("SAMP"));
    m_worker->moveToThread(&m_thread);
    // The documented pattern: the worker is deleted on its own thread, as the
    // thread finishes, together with its sockets and timers.
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &SampHubWorker::connected, this,
            [this](const QString &publicId, const QString &hubUrl) {
                m_connecting = false;
                m_connected = true;
                m_reconnect.connected();
                m_retryTimer->stop();
                m_publicId = publicId;
                m_hubUrl = hubUrl;
                emit connected(publicId, hubUrl);
            });
    connect(m_worker, &SampHubWorker::disconnected, this, [this]() {
        m_connecting = false;
        m_connected = false;
        m_receiving = false;
        m_receiveProblem.clear();
        m_publicId.clear();
        m_clients.clear();
        emit disconnected();
    });
    connect(m_worker, &SampHubWorker::error, this, [this](const QString &description) {
        // A failure while connecting ends the attempt; one while connected
        // does not change the connection state.
        const bool attemptFailed = !m_connected && m_connecting;
        if (!m_connected)
            m_connecting = false;
        // While waiting for a hub, the attempts that find none are the
        // expected outcome, every few seconds: not a warning each time.
        if (m_reconnect.waiting() && !m_connected) {
            qDebug() << "[SampClient] no hub yet:" << description;
            return;
        }
        // Connect before the hub runs (TOPCAT not started yet, or a lockfile
        // left by a hub that was killed): said once, then tried again until
        // one answers. Set before error(), whose handlers show the state.
        if (attemptFailed && m_reconnect.attemptFailed()) {
            m_retryTimer->start();
            emit error(QStringLiteral("%1. VisIVO keeps trying every few seconds; "
                                      "Disconnect From Hub stops it.")
                               .arg(description));
            return;
        }
        emit error(description);
    });
    // The hub went away: try again until one answers, unless the user has
    // disconnected meanwhile (this signal is queued, and may come after).
    // Set before the disconnected() that follows, so its handlers can say so.
    connect(m_worker, &SampHubWorker::hubLost, this, [this]() {
        if (m_reconnect.hubLost())
            m_retryTimer->start();
    });
    m_retryTimer->setInterval(kRetryIntervalMs);
    connect(m_retryTimer, &QTimer::timeout, this, &SampClient::retryLostHub);
    connect(m_worker, &SampHubWorker::receivingChanged, this,
            [this](bool receiving, const QString &problem) {
                m_receiving = receiving;
                m_receiveProblem = problem;
                emit receivingChanged(receiving, problem);
            });
    connect(m_worker, &SampHubWorker::messageReceived, this, &SampClient::messageReceived);
    connect(m_worker, &SampHubWorker::clientListChanged, this,
            [this](const QList<SampClientInfo> &clients) {
                m_clients = clients;
                emit clientListChanged(clients);
            });
    connect(m_worker, &SampHubWorker::tableSent, this, &SampClient::tableSent);
    connect(m_worker, &SampHubWorker::callSent, this, &SampClient::callSent);
    connect(m_worker, &SampHubWorker::callAnswered, this, &SampClient::callAnswered);
    connect(m_worker, &SampHubWorker::subscribersAnswered, this, &SampClient::subscribersAnswered);

    m_thread.start();
}

SampClient::~SampClient()
{
    // Unregister while the hub is still there: blocking here is deliberate
    // (the application is closing) and bounded by the worker's hub timeout.
    if (m_thread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, &SampHubWorker::stop, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
}

// ── Connection lifecycle ──────────────────────────────────────────────────────

void SampClient::refreshBackendEndpoint()
{
    if (!m_backend)
        return;
    QMetaObject::invokeMethod(m_worker, &SampHubWorker::setBackend, Qt::QueuedConnection,
                              m_backend->baseUrl(), m_backend->token());
}

void SampClient::connectToHub()
{
    // Asked for by the user: its outcome is reported, whatever it is.
    m_reconnect.userConnects();
    m_retryTimer->stop();
    if (m_connected || m_connecting) {
        qDebug() << "[SampClient] Already connected or connecting.";
        return;
    }
    m_connecting = true;
    refreshBackendEndpoint();
    QMetaObject::invokeMethod(m_worker, &SampHubWorker::start, Qt::QueuedConnection);
}

void SampClient::disconnectFromHub()
{
    m_reconnect.userDisconnects();
    m_retryTimer->stop();
    QMetaObject::invokeMethod(m_worker, &SampHubWorker::stop, Qt::QueuedConnection);
}

void SampClient::retryLostHub()
{
    if (!m_reconnect.waiting()) {
        m_retryTimer->stop();
        return;
    }
    if (m_connected || m_connecting)
        return;   // an attempt is still running
    m_connecting = true;
    refreshBackendEndpoint();
    QMetaObject::invokeMethod(m_worker, &SampHubWorker::start, Qt::QueuedConnection);
}

// ── Sending messages ──────────────────────────────────────────────────────────

void SampClient::notify(const QString &recipientId, const QString &mtype,
                        const QVariantMap &params)
{
    // Not while connecting either: the worker would drop it, mid-registration.
    if (!m_connected) {
        qWarning() << "[SampClient] notify called while not connected:" << mtype;
        return;
    }
    QMetaObject::invokeMethod(m_worker, &SampHubWorker::notify, Qt::QueuedConnection,
                              recipientId, mtype, params);
}

void SampClient::notifyAll(const QString &mtype, const QVariantMap &params)
{
    notify(QString(), mtype, params);
}

void SampClient::sendImageFits(const QString &url, const QString &name,
                               const QString &recipientId)
{
    QVariantMap params;
    params[QStringLiteral("url")] = url;
    params[QStringLiteral("name")] = name.isEmpty() ? url : name;
    params[QStringLiteral("samp.name")] = params[QStringLiteral("name")];
    notify(recipientId, QString::fromLatin1(Samp::kImageLoadFits), params);
}

void SampClient::sendTableVotable(const QString &url, const QString &name,
                                  const QString &tableId, const QString &recipientId)
{
    QVariantMap params;
    params[QStringLiteral("url")] = url;
    params[QStringLiteral("name")] = name.isEmpty() ? url : name;
    params[QStringLiteral("table-id")] = tableId.isEmpty() ? url : tableId;
    notify(recipientId, QString::fromLatin1(Samp::kTableLoadVotable), params);
}

void SampClient::sendPointAtSky(double raDeg, double decDeg, const QString &recipientId)
{
    const auto params = Samp::pointAtSkyParams(raDeg, decDeg);
    if (!params) {
        qWarning() << "[SampClient] not a sky position:" << raDeg << decDeg;
        return;
    }
    notify(recipientId, QString::fromLatin1(Samp::kCoordPointAtSky), *params);
}

void SampClient::replyToCall(const QString &replyToken, const QString &error)
{
    if (replyToken.isEmpty())
        return;
    QMetaObject::invokeMethod(m_worker, &SampHubWorker::replyToCall, Qt::QueuedConnection,
                              replyToken, error);
}

QString SampClient::callAll(const QString &mtype, const QVariantMap &params,
                            const QString &recipientId)
{
    const QString tag = QStringLiteral("call-%1").arg(++m_callCounter);
    QMetaObject::invokeMethod(m_worker, &SampHubWorker::callAll, Qt::QueuedConnection, tag, mtype,
                              params, recipientId);
    return tag;
}

QString SampClient::askSubscribers(const QString &mtype)
{
    const QString tag = QStringLiteral("subscribers-%1").arg(++m_callCounter);
    QMetaObject::invokeMethod(m_worker, &SampHubWorker::askSubscribers, Qt::QueuedConnection, tag,
                              mtype);
    return tag;
}

void SampClient::sendHighlightRow(const Samp::TableRef &table, qint64 row)
{
    if (table.isEmpty() || row < 0)
        return;
    notify(QString(), QString::fromLatin1(Samp::kTableHighlightRow),
           Samp::highlightRowParams(table, row));
}

void SampClient::sendSelectRowList(const Samp::TableRef &table, const QList<qint64> &rows)
{
    if (table.isEmpty())
        return;
    notify(QString(), QString::fromLatin1(Samp::kTableSelectRowList),
           Samp::selectRowListParams(table, rows));
}
