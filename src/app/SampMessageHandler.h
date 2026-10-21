// SampMessageHandler.h – MType handlers for inbound SAMP messages in VisIVO Visual Analytics
//
// SampMessageHandler connects to SampClient::messageReceived() and dispatches
// each SAMP message to the appropriate VisIVO action.
//
// Supported inbound MTypes:
//   image.load.fits      → open FITS image/cube via BackendClient
//   table.load.votable   → open VOTable as catalogue
//   table.load.fits      → open FITS table as catalogue
//   coord.pointAt.sky    → emit skyCoordReceived(ra, dec)
//   samp.hub.event.*     → internal Hub lifecycle (handled by SampClient itself)
//
// Note: actual file loading is delegated to DatasetOpenService and the existing
// BackendClient API.  SampMessageHandler only orchestrates the routing.

#ifndef SAMPMESSAGEHANDLER_H
#define SAMPMESSAGEHANDLER_H

#include "SampClient.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

class BackendClient;
class DatasetOpenService;

// ── SampMessageHandler ────────────────────────────────────────────────────────

class SampMessageHandler : public QObject
{
    Q_OBJECT

public:
    explicit SampMessageHandler(SampClient *sampClient,
                                BackendClient *backend,
                                QObject *parent = nullptr);

signals:
    /**
     * Emitted when a FITS dataset has been opened as a result of an inbound
     * image.load.fits / table.load.fits message.
     * Connect this to your DatasetOpenService or main window.
     */
    void fitsDatasetOpened(const QString &backendPath, const QString &mtype);

    /**
     * Emitted when a VOTable has been loaded (table.load.votable).
     * @param url         The original URL from the SAMP message
     * @param localPath   Path to a locally-cached copy, or empty if the file
     *                    is already accessible by the backend via its URL.
     */
    void votableLoaded(const QString &url, const QString &localPath);

    /**
     * Emitted for coord.pointAt.sky messages.
     * The main window / HiPS viewport should connect to this to re-centre.
     */
    void skyCoordReceived(double raDeg, double decDeg);

    /**
     * Generic signal for any message that was received but not specifically
     * handled.  Useful for logging or plugin hooks.
     */
    void unhandledMessage(const SampMessage &message);

    /** Forwarded from SampClient for UI connection-state indicators. */
    void sampConnected(const QString &publicId, const QString &hubUrl);
    void sampDisconnected();
    void sampError(const QString &description);

public slots:
    /** Main dispatcher — connected to SampClient::messageReceived(). */
    void onMessageReceived(const SampMessage &message);

private:
    void handleImageLoadFits(const SampMessage &msg);
    void handleTableLoad(const SampMessage &msg);
    void handleCoordPointAtSky(const SampMessage &msg);

    /**
     * Resolve a SAMP URL to a backend-accessible path.
     *
     * - file:// URLs: strip the scheme, return the local client path.
     * - http:// / https:// URLs: return the URL as-is.
     *
     * MainWindow is responsible for staging the file on the backend when the
     * backend cannot access the resolved location directly.
     */
    static QString resolveUrl(const QString &url);

    BackendClient *m_backend{ nullptr };
    SampClient    *m_samp{ nullptr };
};

#endif // SAMPMESSAGEHANDLER_H
