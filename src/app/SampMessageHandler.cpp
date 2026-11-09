// SampMessageHandler.cpp – MType handler implementations

#include "SampMessageHandler.h"
#include "BackendClient.h"

#include <QDebug>

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

    // The SAMP thread has already refused what Samp::validateMessage() rejects,
    // so the parameters below are present and well-formed.
    const QString &mt = message.mtype;
    const QVariantMap &p = message.params;
    if (mt == QLatin1String(Samp::kImageLoadFits)) {
        emit imageLoadRequested(p.value(QStringLiteral("url")).toString().trimmed(),
                                p.value(QStringLiteral("name")).toString(), message.replyToken);
    } else if (mt == QLatin1String(Samp::kTableLoadVotable)
               || mt == QLatin1String(Samp::kTableLoadFits)) {
        emit tableLoadRequested(p.value(QStringLiteral("url")).toString().trimmed(),
                                p.value(QStringLiteral("table-id")).toString().trimmed(),
                                p.value(QStringLiteral("name")).toString(), mt, message.replyToken);
    } else if (mt == QLatin1String(Samp::kCoordPointAtSky)) {
        emit skyCoordReceived(p.value(QStringLiteral("ra")).toString().toDouble(),
                              p.value(QStringLiteral("dec")).toString().toDouble());
    } else if (mt == QLatin1String(Samp::kTableHighlightRow)) {
        const Samp::TableRef ref = Samp::tableRefFrom(p);
        if (const auto row = Samp::parseRowIndex(p.value(QStringLiteral("row"))))
            emit rowHighlightRequested(ref.tableId, ref.url, *row, message.senderId);
    } else if (mt == QLatin1String(Samp::kTableSelectRowList)) {
        const Samp::TableRef ref = Samp::tableRefFrom(p);
        if (const auto rows = Samp::parseRowList(p.value(QStringLiteral("row-list"))))
            emit rowSelectionRequested(ref.tableId, ref.url, *rows, message.senderId);
    } else {
        // Hub lifecycle events (shutdown, register, unregister) are handled
        // internally by SampClient; anything else is considered unhandled.
        emit unhandledMessage(message);
    }
}
