// SampClient.h – Qt SAMP client for VisIVO Visual Analytics
//
// A SAMP Standard Profile (SAMP 1.3) client built on Qt's own networking
// stack; no external XML-RPC library is required.
//
// Architecture
// ------------
// The SAMP Hub runs on the user's desktop (started by TOPCAT, Aladin, or
// `samp_hub`). VisIVO is a *callable* client:
//
//  1. It registers with the hub (private key + public id).
//  2. It runs a small XML-RPC callback server on 127.0.0.1 and an ephemeral
//     port, and hands its URL to samp.hub.setXmlrpcCallback.
//  3. It declares its metadata and subscriptions (table and image loads,
//     coord.pointAt.sky, table.highlight.row, table.select.rowList, hub
//     events). TOPCAT and other tools then list it as a target.
//  4. The hub calls samp.client.receiveNotification / receiveCall on the
//     callback server; calls are answered through samp.hub.reply.
//  5. Outbound messages the backend queues (GET /v1/samp/pending) are sent on
//     to the hub, with the backend URL placeholder expanded.
//
// Threads
// -------
// All of that network work runs on a dedicated thread (SampHubWorker). This
// object lives on the GUI thread: its methods only queue work, and it
// re-emits the worker's signals there, so the GUI never waits on the hub and
// every handler of messageReceived() runs on the GUI thread, from its event
// loop.

#ifndef SAMPCLIENT_H
#define SAMPCLIENT_H

#include "SampProtocol.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantMap>

class BackendClient;
class QTimer;
class SampHubWorker;

// ── SampClient ────────────────────────────────────────────────────────────────

/**
 * Lightweight SAMP Standard Profile client.
 *
 * Usage:
 * @code
 *   auto samp = new SampClient(backendClient, this);
 *   connect(samp, &SampClient::connected, this, &MyWindow::onSampConnected);
 *   connect(samp, &SampClient::messageReceived, this, &MyWindow::onSampMessage);
 *   samp->connectToHub();
 * @endcode
 */
class SampClient : public QObject
{
    Q_OBJECT

public:
    /// MType string constants (kept for existing callers; see Samp::k*).
    static constexpr const char *MTYPE_IMAGE_FITS    = Samp::kImageLoadFits;
    static constexpr const char *MTYPE_TABLE_VOTABLE = Samp::kTableLoadVotable;
    static constexpr const char *MTYPE_TABLE_FITS    = Samp::kTableLoadFits;
    static constexpr const char *MTYPE_COORD_SKY     = Samp::kCoordPointAtSky;
    static constexpr const char *MTYPE_HUB_SHUTDOWN  = Samp::kHubShutdown;

    explicit SampClient(BackendClient *backend, QObject *parent = nullptr);
    ~SampClient() override;

    // ── Connection lifecycle ──────────────────────────────────────────────────

    /** Find the Hub lockfile and register. Asynchronous: emits connected() or error(). */
    void connectToHub();

    /** Unregister from the Hub. Asynchronous: emits disconnected(). It also
     *  stops waiting for a lost hub to come back. */
    void disconnectFromHub();

    /** No hub to talk to, and VisIVO is trying every few seconds, quietly,
     *  until one answers: the hub went away (shut down, stopped answering),
     *  or none was running when connectToHub() was called. Not after
     *  disconnectFromHub(), nor when the hub disconnected VisIVO itself. */
    bool isWaitingForHub() const { return m_reconnect.waiting(); }

    /** @return true if currently registered with the Hub. */
    bool isConnected() const { return m_connected; }

    /** True between connectToHub() and its outcome. */
    bool isConnecting() const { return m_connecting; }

    /** True when the hub can call VisIVO back (it receives messages). */
    bool isReceiving() const { return m_receiving; }

    /** Why VisIVO cannot receive, when connected but not receiving. */
    QString receiveProblem() const { return m_receiveProblem; }

    /** Public SAMP ID assigned by the Hub (empty when not connected). */
    QString publicId() const { return m_publicId; }

    /** URL of the Hub XML-RPC endpoint (from lockfile). */
    QString hubUrl() const { return m_hubUrl; }

    /** Push the backend URL and token (from the BackendClient) to the SAMP thread. */
    void refreshBackendEndpoint();

    // ── Sending messages ──────────────────────────────────────────────────────

    /**
     * Broadcast a message to all subscribed Hub clients.
     * @param mtype  SAMP MType, e.g. "image.load.fits"
     * @param params Parameter map (values must be strings per SAMP spec)
     */
    void notifyAll(const QString &mtype, const QVariantMap &params);

    /**
     * Send a message to a specific Hub client.
     * @param recipientId  Target public client ID
     */
    void notify(const QString &recipientId, const QString &mtype, const QVariantMap &params);

    /**
     * Convenience: broadcast image.load.fits with the given URL.
     * @param url   Fully-qualified URL the Hub clients will fetch (http:// or file://)
     * @param name  Human-readable name shown in the receiving tool
     */
    void sendImageFits(const QString &url, const QString &name = QString(),
                       const QString &recipientId = QString());

    /**
     * Convenience: broadcast table.load.votable.
     */
    void sendTableVotable(const QString &url, const QString &name = QString(),
                          const QString &tableId = QString(),
                          const QString &recipientId = QString());

    /**
     * Convenience: broadcast coord.pointAt.sky.
     * @param raDeg   Right ascension in decimal degrees (ICRS)
     * @param decDeg  Declination in decimal degrees (ICRS)
     */
    void sendPointAtSky(double raDeg, double decDeg,
                        const QString &recipientId = QString());

    /**
     * Answer a load call (table.load.*, image.load.fits) once it is done:
     * samp.ok when @p error is empty, samp.error with it otherwise.
     * @p replyToken is SampMessage::replyToken (empty for a notification:
     * then this does nothing). The SAMP thread answers
     * every other call itself.
     */
    void replyToCall(const QString &replyToken, const QString &error = QString());

    /**
     * Ask every subscriber of @p mtype (samp.hub.callAll) and report each
     * answer: callSent() says who was asked, callAnswered() what each said.
     * Returns the tag both signals carry. "__BACKEND_BASE_URL__" in a string
     * parameter is replaced by the backend URL.
     */
    QString callAll(const QString &mtype, const QVariantMap &params);

    /**
     * Which other clients take @p mtype (samp.hub.getSubscribedClients):
     * subscribersAnswered() carries the answer under the tag returned.
     */
    QString askSubscribers(const QString &mtype);

    /** Broadcast table.highlight.row (@p row: 0-based, in the original table). */
    void sendHighlightRow(const Samp::TableRef &table, qint64 row);

    /** Broadcast table.select.rowList (rows: 0-based, in the original table). */
    void sendSelectRowList(const Samp::TableRef &table, const QList<qint64> &rows);

    // ── Hub client list ───────────────────────────────────────────────────────

    /** The other clients, as last reported (clientListChanged()). */
    QList<SampClientInfo> registeredClients() const { return m_clients; }

signals:
    /** Emitted when registration with the Hub succeeds. */
    void connected(const QString &publicId, const QString &hubUrl);

    /** Emitted when disconnected (graceful or Hub shutdown). */
    void disconnected();

    /** Emitted when a SAMP notification/call is received from another tool. */
    void messageReceived(const SampMessage &message);

    /** Emitted when the client list in the Hub changes. */
    void clientListChanged(const QList<SampClientInfo> &clients);

    /** Emitted on any Hub communication error the user should see. */
    void error(const QString &description);

    /** Whether the hub can call VisIVO back changed. */
    void receivingChanged(bool receiving, const QString &problem);

    /** A table.load.* message left with this table-id and url. */
    void tableSent(const QString &tableId, const QString &url);

    /** callAll(@p tag) went out to @p recipients (ids), or failed (@p error). */
    void callSent(const QString &tag, const QStringList &recipients, const QString &error);

    /** One recipient's answer to callAll(@p tag); @p error is empty for samp.ok. */
    void callAnswered(const QString &tag, const QString &responderId, const QString &error);

    /** askSubscribers(@p tag): the other clients' ids, or @p error. */
    void subscribersAnswered(const QString &tag, const QStringList &clientIds,
                             const QString &error);

private:
    /** One quiet attempt to register with a hub that came back. */
    void retryLostHub();

    BackendClient *m_backend{ nullptr };
    QThread m_thread;
    SampHubWorker *m_worker{ nullptr };   ///< lives on m_thread

    bool m_connected{ false };
    bool m_connecting{ false };
    bool m_receiving{ false };
    QString m_receiveProblem;
    QString m_publicId;
    QString m_hubUrl;
    QList<SampClientInfo> m_clients;
    quint64 m_callCounter{ 0 };
    SampReconnectPolicy m_reconnect;
    QTimer *m_retryTimer{ nullptr };
    static constexpr int kRetryIntervalMs = 3000;
};

#endif // SAMPCLIENT_H
