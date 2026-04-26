// SampClient.h – Qt SAMP client for VisIVO-next
//
// This class implements a lightweight SAMP Standard Profile client using
// Qt's own networking stack (QNetworkAccessManager + QDomDocument for XML-RPC).
// No external XML-RPC library is required.
//
// Architecture
// ------------
// The SAMP Hub runs on the user's desktop (started by TOPCAT, Aladin, or
// `samp_hub`).  SampClient connects to it via XML-RPC and:
//
//  1. Registers VisIVO as a SAMP client (obtains a private key + public ID).
//  2. Declares its metadata (name, description, icon, supported MTypes).
//  3. Subscribes to the MTypes it can handle (image.load.fits, etc.).
//  4. Polls the Hub for pending notifications (Standard Profile requires client
//     polling because the Hub cannot push to a client that has no callback URL;
//     alternatively we start a minimal HTTP callback server on a random local
//     port—see SAMP_USE_CALLBACK_SERVER define).
//  5. Forwards inbound messages to the backend via BackendClient.
//  6. Picks up outbound messages from the backend (GET /v1/samp/pending) and
//     sends them to the Hub.
//
// Thread-safety
// -------------
// All Hub XML-RPC calls are made from a dedicated QThread to avoid blocking
// the Qt main event loop.  Qt signals are used to deliver results back to the
// main thread.

#ifndef SAMPCLIENT_H
#define SAMPCLIENT_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class BackendClient;
class QDomElement;

// ── Data types ────────────────────────────────────────────────────────────────

/// A single SAMP client registered in the Hub.
struct SampClientInfo
{
    QString id;           ///< Hub-assigned public ID (e.g. "c1")
    QString name;         ///< samp.name metadata string
    QString description;
    QString iconUrl;
    QStringList subscriptions;  ///< List of subscribed MTypes
};

/// An inbound SAMP message.
struct SampMessage
{
    QString mtype;
    QVariantMap params;
    QString senderId;
    QString msgId;
};

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
    /// MType string constants.
    static constexpr const char *MTYPE_IMAGE_FITS    = "image.load.fits";
    static constexpr const char *MTYPE_TABLE_VOTABLE = "table.load.votable";
    static constexpr const char *MTYPE_TABLE_FITS    = "table.load.fits";
    static constexpr const char *MTYPE_COORD_SKY     = "coord.pointAt.sky";
    static constexpr const char *MTYPE_HUB_SHUTDOWN  = "samp.hub.event.shutdown";

    explicit SampClient(BackendClient *backend, QObject *parent = nullptr);
    ~SampClient() override;

    // ── Connection lifecycle ──────────────────────────────────────────────────

    /** Find the Hub lockfile and attempt registration.  Emits connected() on success. */
    void connectToHub();

    /** Unregister from the Hub and stop polling. */
    void disconnectFromHub();

    /** @return true if currently registered with the Hub. */
    bool isConnected() const { return m_connected; }

    /** Public SAMP ID assigned by the Hub (empty when not connected). */
    QString publicId() const { return m_publicId; }

    /** URL of the Hub XML-RPC endpoint (from lockfile). */
    QString hubUrl() const { return m_hubUrl; }

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

    // ── Hub client list ───────────────────────────────────────────────────────

    /** Synchronously fetch registered clients from the Hub. */
    QList<SampClientInfo> registeredClients() const;

signals:
    /** Emitted when registration with the Hub succeeds. */
    void connected(const QString &publicId, const QString &hubUrl);

    /** Emitted when disconnected (graceful or Hub shutdown). */
    void disconnected();

    /** Emitted when a SAMP notification/call is received from another tool. */
    void messageReceived(const SampMessage &message);

    /** Emitted when the client list in the Hub changes. */
    void clientListChanged(const QList<SampClientInfo> &clients);

    /** Emitted on any Hub communication error. */
    void error(const QString &description);

private slots:
    void onPollTimer();
    void onBackendPendingTimer();

private:
    // ── XML-RPC helpers ───────────────────────────────────────────────────────

    /**
     * Perform a synchronous XML-RPC call to the Hub.
     * Returns the parsed response value, or an invalid QVariant on failure.
     * Sets @p errorOut on failure.
     */
    QVariant xmlRpcCall(const QString &method,
                        const QVariantList &params,
                        QString &errorOut) const;

    /** Build a minimal XML-RPC request body. */
    static QByteArray buildXmlRpcRequest(const QString &method, const QVariantList &params);

    /** Parse an XML-RPC response and return the <value> as a QVariant. */
    static QVariant parseXmlRpcResponse(const QByteArray &data, QString &errorOut);

    /** Convert a QVariant to an XML-RPC <value> element string. */
    static QString variantToXmlRpcValue(const QVariant &v);

    /** Parse an XML-RPC <value> element into a QVariant. */
    static QVariant xmlRpcValueToVariant(const QDomElement &el);

    // ── Hub lockfile ──────────────────────────────────────────────────────────

    /** Locate ~/.samp lockfile and parse hub.xmlrpc.url + samp.secret. */
    static bool readLockFile(QString &hubUrl, QString &secret, QString &errorOut);

    // ── Registration helpers ──────────────────────────────────────────────────

    bool registerWithHub(QString &errorOut);
    void declareMetadata();
    void declareSubscriptions();
    void reportStatusToBackend();

    // ── Polling / inbound ─────────────────────────────────────────────────────

    /** Poll Hub for pending notifications (samp.hub.getCallbacks / getNotifications). */
    void pollHub();

    /** Forward a received message to the backend inbox. */
    void forwardToBackend(const SampMessage &msg);

    // ── Backend outbound queue ────────────────────────────────────────────────

    /** Fetch pending outbound messages from the backend and dispatch to Hub. */
    void drainBackendPendingQueue();

    /** Replace the __BACKEND_BASE_URL__ placeholder with the real backend URL. */
    QString expandBackendUrl(const QString &urlTemplate) const;

    // ── State ─────────────────────────────────────────────────────────────────

    BackendClient   *m_backend{ nullptr };
    QNetworkAccessManager *m_nam{ nullptr };

    QString  m_hubUrl;
    QString  m_sampSecret;      ///< samp.secret from lockfile (used for registration)
    QString  m_privateKey;      ///< Private key returned by samp.hub.register
    QString  m_publicId;        ///< Our public client ID in the Hub

    bool m_connected{ false };

    /// Poll Hub every 500 ms for inbound notifications.
    QTimer *m_pollTimer{ nullptr };

    /// Poll backend every 600 ms for outbound messages to forward to Hub.
    QTimer *m_backendPendingTimer{ nullptr };

    static constexpr int kPollIntervalMs        = 500;
    static constexpr int kBackendPollIntervalMs = 600;
};

#endif // SAMPCLIENT_H
