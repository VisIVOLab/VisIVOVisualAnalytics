// SampHubWorker.h – everything SAMP that touches the network, on its own thread
//
// SampClient (the object the GUI talks to) owns a QThread and one of these
// living on it. The worker does the blocking work: the XML-RPC calls to the
// hub, the callback server the hub calls back on, the backend's outbound
// queue and inbox. None of it can stall the GUI, and a callback is answered
// even while the GUI thread is busy. Messages reach the GUI as queued signals,
// so viewer code only ever runs on the GUI thread, from its own event loop.
//
// Not used directly: go through SampClient.

#ifndef SAMPHUBWORKER_H
#define SAMPHUBWORKER_H

#include "SampProtocol.h"

#include <QList>
#include <QObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class QNetworkAccessManager;
class QTimer;
class SampCallbackServer;

class SampHubWorker : public QObject
{
    Q_OBJECT

public:
    explicit SampHubWorker(QObject *parent = nullptr);
    ~SampHubWorker() override;

public slots:
    /// The backend the outbound queue, the inbox and the icon URL live on.
    void setBackend(const QString &baseUrl, const QString &token);
    /// Register, become callable, declare metadata and subscriptions.
    void start();
    /// Unregister (when the hub is still there) and stop listening.
    void stop();
    /// Notify one client (@p recipientId) or every subscriber (empty id).
    void notify(const QString &recipientId, const QString &mtype, const QVariantMap &params);
    /// Answer a call whose reply the GUI owns (a load: samp.ok once the data
    /// opened, samp.error with @p error when it did not).
    void replyToCall(const QString &replyToken, const QString &error);
    /// samp.hub.callAll: every subscriber is asked, and each answers through
    /// receiveResponse (callAnswered); with @p recipientId, samp.hub.call asks
    /// that one only. @p tag names the call for the GUI.
    /// "__BACKEND_BASE_URL__" in a string parameter becomes the backend URL.
    void callAll(const QString &tag, const QString &mtype, const QVariantMap &params,
                 const QString &recipientId);
    /// samp.hub.getSubscribedClients: which other clients take @p mtype
    /// (subscribersAnswered, under @p tag). Asked before a file is staged,
    /// so that nobody waits for a send no tool can take.
    void askSubscribers(const QString &tag, const QString &mtype);

signals:
    void connected(const QString &publicId, const QString &hubUrl);
    void disconnected();
    /// A failure the user must see (status bar and Diagnostics).
    void error(const QString &description);
    /// Whether the hub can call VisIVO back; @p problem says why not.
    void receivingChanged(bool receiving, const QString &problem);
    void messageReceived(const SampMessage &message);
    void clientListChanged(const QList<SampClientInfo> &clients);
    /// A table.load.* message left with this table-id and (expanded) url.
    void tableSent(const QString &tableId, const QString &url);
    /// callAll(@p tag) reached the hub, which asked @p recipients (their ids;
    /// empty: no tool subscribes to that MType). @p error: the hub refused it.
    void callSent(const QString &tag, const QStringList &recipients, const QString &error);
    /// One recipient's answer to callAll(@p tag): @p error empty for samp.ok.
    void callAnswered(const QString &tag, const QString &responderId, const QString &error);
    /// The answer to askSubscribers(@p tag): the other clients' ids, or
    /// @p error when the hub could not be asked.
    void subscribersAnswered(const QString &tag, const QStringList &clientIds,
                             const QString &error);
    /// The connection ended because the hub went away (it shut down, or
    /// stopped answering), not because VisIVO or the hub chose to end it:
    /// emitted just before disconnected().
    void hubLost();

private:
    /// @p transportFailed (optional): set when the hub could not be reached at
    /// all, as opposed to an XML-RPC fault it answered with.
    QVariant xmlRpcCall(const QString &method, const QVariantList &params, QString &errorOut,
                        bool *transportFailed = nullptr);
    bool readLockFile(QString &hubUrl, QString &secret, QString &errorOut);
    /// Register; the keys are kept only if @p generation is still current
    /// when the hub answers (else the registration is undone at once).
    bool registerWithHub(quint64 generation, QString &errorOut);
    bool startCallback(QString &errorOut);
    void declareMetadata();
    bool declareSubscriptions(QString &errorOut);
    QList<SampClientInfo> registeredClients();
    void reportStatusToBackend();
    /// The peer list and the backend's status, serialised (see the .cpp).
    void refreshClients();

    /// A message to send, or (msgId set) a reply the GUI asked for.
    struct Outgoing
    {
        QString recipientId;
        QString mtype;
        QVariantMap params;
        QString replyMsgId;
        QString replyError;
        /// Set for a message from the backend's queue: its file URLs point
        /// at that backend, so it is dropped if the backend is changed first.
        QString fromBackend;
        /// Set for a callAll(): the GUI's tag for it.
        QString callTag;
        /// Set for a message from the backend's queue: its job id there, to
        /// acknowledge once it has been sent.
        QString backendJobId;
    };
    void processOutbox();
    /// False when the message was not delivered and should be tried again
    /// later (the hub is gone, or the connection changed during the send);
    /// true when the hub took it, or refused it for good.
    bool sendNow(const QString &recipientId, const QString &mtype, const QVariantMap &params,
                 const QString &callTag = QString());

    QByteArray handleCallback(const SampXmlRpc::Call &call);
    /// Handle the queued messages one at a time, in arrival order: a reply's
    /// network wait runs the event loop, and a message handled inside it
    /// would overtake the one being handled.
    void processInbox();
    void handleMessage(const SampMessage &msg);
    void reply(const QString &msgId, const QVariantMap &response);
    /// Is the hub still there? (samp.hub.ping; an idle hub that died
    /// without a shutdown message is otherwise never noticed.)
    void pingHub();
    void forwardToBackend(const SampMessage &msg);
    void drainBackendPendingQueue();
    /// Tell the backend which of its queued messages were sent (they stay
    /// queued, and come back after a while, until it is told).
    void ackBackendMessages();
    /// Backend messages in the outbox, and sent ones not acknowledged yet.
    SampBackendAcks m_backendAcks;
    static QString expandBackendUrl(const QString &urlTemplate, const QString &backendUrl);

    /// Drop the connection; @p unregister when the hub is still listening,
    /// @p lost when it went away (hubLost() is emitted).
    void teardown(bool unregister, bool lost = false);
    /// A refused or dropped connection: the hub went away.
    static bool isHubGone(const QString &error);

    QNetworkAccessManager *m_nam{ nullptr };
    SampCallbackServer *m_callback{ nullptr };
    QTimer *m_backendPendingTimer{ nullptr };
    QTimer *m_hubPingTimer{ nullptr };
    QTimer *m_clientsRefreshTimer{ nullptr };   ///< coalesces bursts of hub events

    QString m_backendUrl;
    QString m_backendToken;
    QString m_hubUrl;
    QString m_sampSecret;
    QString m_privateKey;
    QString m_publicId;
    QString m_hubId;            ///< the hub's own client id: sender of hub events
    QString m_lastListedKey;    ///< the status last emitted to the GUI (Samp::hubStatusKey)
    QString m_lastReportedKey;  ///< the status the backend last took
    bool m_reporting{ false };
    bool m_reportAgain{ false };   ///< a report asked for while one was running
    bool m_connected{ false };
    bool m_starting{ false };
    bool m_draining{ false };
    bool m_pinging{ false };
    bool m_refreshingClients{ false };
    bool m_refreshClientsAgain{ false };
    /// Bumped by every teardown: a start() suspended in a network wait while
    /// the connection was dropped sees it changed and gives up.
    quint64 m_generation{ 0 };
    QList<SampMessage> m_inbox;
    bool m_processingInbox{ false };
    QList<Outgoing> m_outbox;
    /// Load calls of this registration the GUI has still to answer.
    SampPendingCalls m_pendingCalls;
    /// A callAll() of this registration still waiting for answers.
    struct OutstandingCall
    {
        QString tag;                ///< the GUI's
        bool recipientsKnown{ false };
        QSet<QString> waiting;      ///< asked, not answered yet
        QSet<QString> answeredEarly;   ///< answered before callAll returned
    };
    QHash<QString, OutstandingCall> m_outstandingCalls;   ///< by hub msg-tag
    QStringList m_outstandingOrder;   ///< oldest first, to bound the map
    static constexpr qsizetype kMaxOutstandingCalls = 64;
    void forgetCall(const QString &msgTag);
    quint64 m_callCounter{ 0 };
    bool m_processingOutbox{ false };
    /// connectToHub() again while a cancelled start is still unwinding.
    bool m_restartRequested{ false };

    static constexpr int kBackendPollIntervalMs = 600;
    static constexpr int kHubPingIntervalMs = 5000;
    static constexpr int kHubTimeoutMs = 4000;
    static constexpr qint64 kMaxLockFileBytes = 64 * 1024;
};

#endif  // SAMPHUBWORKER_H
