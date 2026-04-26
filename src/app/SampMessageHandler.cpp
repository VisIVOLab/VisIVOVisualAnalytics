// SampMessageHandler.cpp – MType handler implementations

#include "SampMessageHandler.h"
#include "BackendClient.h"

#include <QDebug>
#include <QUrl>
#include <cmath>

// ── Constructor ───────────────────────────────────────────────────────────────

SampMessageHandler::SampMessageHandler(SampClient *sampClient,
                                       BackendClient *backend,
                                       QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_samp(sampClient)
{
    // Wire SampClient → handler
    connect(sampClient, &SampClient::messageReceived,
            this, &SampMessageHandler::onMessageReceived);

    // Forward lifecycle signals for UI connection indicators
    connect(sampClient, &SampClient::connected,
            this, &SampMessageHandler::sampConnected);
    connect(sampClient, &SampClient::disconnected,
            this, &SampMessageHandler::sampDisconnected);
    connect(sampClient, &SampClient::error,
            this, &SampMessageHandler::sampError);
}

// ── Main dispatcher ───────────────────────────────────────────────────────────

void SampMessageHandler::onMessageReceived(const SampMessage &message)
{
    qDebug() << "[SampMessageHandler] mtype=" << message.mtype
             << "sender=" << message.senderId;

    if (message.mtype == QStringLiteral(SampClient::MTYPE_IMAGE_FITS)) {
        handleImageLoadFits(message);
    } else if (message.mtype == QStringLiteral(SampClient::MTYPE_TABLE_VOTABLE) ||
               message.mtype == QStringLiteral(SampClient::MTYPE_TABLE_FITS)) {
        handleTableLoad(message);
    } else if (message.mtype == QStringLiteral(SampClient::MTYPE_COORD_SKY)) {
        handleCoordPointAtSky(message);
    } else {
        // Hub lifecycle events (shutdown, register, unregister) are handled
        // internally by SampClient; anything else is considered unhandled.
        emit unhandledMessage(message);
    }
}

// ── Handlers ─────────────────────────────────────────────────────────────────

void SampMessageHandler::handleImageLoadFits(const SampMessage &msg)
{
    const QString url = msg.params.value(QStringLiteral("url")).toString();
    if (url.isEmpty()) {
        qWarning() << "[SampMessageHandler] image.load.fits: missing 'url' param";
        return;
    }

    const QString path = resolveUrl(url);
    qDebug() << "[SampMessageHandler] image.load.fits resolved to:" << path;

    // Emit for the main window to handle dataset open
    emit fitsDatasetOpened(path, msg.mtype);
}

void SampMessageHandler::handleTableLoad(const SampMessage &msg)
{
    const QString url = msg.params.value(QStringLiteral("url")).toString();
    if (url.isEmpty()) {
        qWarning() << "[SampMessageHandler] table.load: missing 'url' param";
        return;
    }

    const QString path = resolveUrl(url);
    qDebug() << "[SampMessageHandler] table.load resolved to:" << path;

    emit votableLoaded(url, path);
}

void SampMessageHandler::handleCoordPointAtSky(const SampMessage &msg)
{
    bool raOk  = false;
    bool decOk = false;

    const double ra  = msg.params.value(QStringLiteral("ra")).toString().toDouble(&raOk);
    const double dec = msg.params.value(QStringLiteral("dec")).toString().toDouble(&decOk);

    if (!raOk || !decOk) {
        qWarning() << "[SampMessageHandler] coord.pointAt.sky: invalid ra/dec params:"
                   << msg.params;
        return;
    }

    if (!std::isfinite(ra) || !std::isfinite(dec) ||
        ra < 0.0 || ra > 360.0 || dec < -90.0 || dec > 90.0) {
        qWarning() << "[SampMessageHandler] coord.pointAt.sky: out-of-range ra="
                   << ra << "dec=" << dec;
        return;
    }

    qDebug() << "[SampMessageHandler] coord.pointAt.sky ra=" << ra << "dec=" << dec;
    emit skyCoordReceived(ra, dec);
}

// ── URL resolver ──────────────────────────────────────────────────────────────

QString SampMessageHandler::resolveUrl(const QString &url)
{
    const QUrl qurl(url);
    if (qurl.isLocalFile())
        return qurl.toLocalFile();   // file:///path → /path
    // http/https: return as-is; the backend will fetch it
    return url;
}
