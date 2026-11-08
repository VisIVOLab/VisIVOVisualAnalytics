// SampMessageHandler.h – MType handlers for inbound SAMP messages in VisIVO Visual Analytics
//
// SampMessageHandler connects to SampClient::messageReceived() (delivered on
// the GUI thread) and turns each SAMP message into a request the main window
// acts on.
//
// Supported inbound MTypes:
//   image.load.fits      → imageLoadRequested(url, name, replyToken)
//   table.load.votable   → tableLoadRequested(url, tableId, name, mtype, replyToken)
//   table.load.fits      → tableLoadRequested(url, tableId, name, mtype, replyToken)
//   coord.pointAt.sky    → skyCoordReceived(ra, dec)
//   table.highlight.row  → rowHighlightRequested(tableId, url, row, sender)
//   table.select.rowList → rowSelectionRequested(tableId, url, rows, sender)
//   samp.hub.event.*     → internal Hub lifecycle (handled by SampClient itself)
//
// SampMessageHandler only routes; staging a file on the backend and finding
// the viewer a row belongs to is the main window's job.

#ifndef SAMPMESSAGEHANDLER_H
#define SAMPMESSAGEHANDLER_H

#include "SampClient.h"

#include <QList>
#include <QObject>
#include <QString>

class BackendClient;

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
     * image.load.fits: the image at @p url should open. @p replyToken is set for a
     * call: answer it with SampClient::replyToCall() once it opened or failed.
     */
    void imageLoadRequested(const QString &url, const QString &name,
                            const QString &replyToken);

    /**
     * table.load.votable / table.load.fits: the table at @p url should open.
     * @p tableId is the sender's table-id (may be empty); keep it with the
     * opened catalogue so row messages naming it can be matched.
     */
    void tableLoadRequested(const QString &url, const QString &tableId, const QString &name,
                            const QString &mtype, const QString &replyToken);

    /**
     * Emitted for coord.pointAt.sky messages.
     * The main window / HiPS viewport should connect to this to re-centre.
     */
    void skyCoordReceived(double raDeg, double decDeg);

    /** table.highlight.row: @p row is 0-based in the original table. */
    void rowHighlightRequested(const QString &tableId, const QString &url, qint64 row,
                               const QString &senderId);

    /** table.select.rowList: rows are 0-based in the original table. */
    void rowSelectionRequested(const QString &tableId, const QString &url,
                               const QList<qint64> &rows, const QString &senderId);

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
    BackendClient *m_backend{ nullptr };
    SampClient    *m_samp{ nullptr };
};

#endif // SAMPMESSAGEHANDLER_H
