#ifndef SHAREDSELECTIONLINK_H
#define SHAREDSELECTIONLINK_H

// SharedSelectionLink — the cube viewer's end of the selection hub (issue
// #188), on the GUI thread's event loop (no worker thread waits).
//
// One invariant, as on the VR page (share.js): what this window shows
// converges to the hub's latest entry for its cube. Every publish carries a
// fresh nonce that the hub echoes; the link remembers the nonce of what is on
// screen. reconcile() reads the hub's latest entry (wait=0) and acts by the
// pure rule in SharedSelection.h (reconcileDecision, confirmShareDraw); it runs
// after every long-poll answer (a new entry, or the 20 s timeout: the safety
// net), after every publish settles (kept, refused, failed or timed out) and
// before anything received is drawn. The long poll never waits on a publish.
// The requests carry the window's token and session
// (BackendClient::applySessionHeaders).

#include "SharedSelection.h"

#include <QElapsedTimer>
#include <QObject>
#include <QUrl>
#include <QPointer>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

class SharedSelectionLink : public QObject
{
    Q_OBJECT

public:
    explicit SharedSelectionLink(QObject *parent = nullptr);
    ~SharedSelectionLink() override;

    /// Follows the hub for @p datasetId of the cube @p fullW x @p fullH x @p fullD.
    void start(const QString &baseUrl, const QString &token, const QString &session,
               const QString &datasetId, int fullW, int fullH, int fullD);
    void stop();
    bool running() const { return m_running; }
    QString clientId() const { return m_view.client; }
    const visivo::ShareViewState &viewState() const { return m_view; }

    /// Publishes the selection on screen (it becomes what this window shows):
    /// a lasso's float32 mask over its box, or a box (an empty @p mask with
    /// kind "box"). A fresh nonce is added to @p meta.
    void publish(const QString &kind, const QByteArray &mask, const std::array<int, 3> &origin,
                 const std::array<int, 3> &dims, const QJsonObject &meta);

    /// A selection made here and not published (or a clear) replaced what was
    /// on screen: the entries read so far stay off the screen.
    void noteLocalSelection();

    /// Reads the hub and converges (also run by the link itself).
    void reconcile();

    /// How long a publish may wait for the hub's answer before it counts as
    /// settled with an unknown outcome (ms).
    static constexpr int kPublishTimeoutMs = 30000;
    /// The wait before the one retry of a publish answered 429 (ms).
    static constexpr int kRetry429Ms = 1000;

signals:
    /// A selection to show: another client's (@p own false) or this window's
    /// own drawn back from the hub (@p own true), with its mask.
    void received(const visivo::SharedSelectionInfo &info, const QByteArray &mask, qint64 elapsedMs, bool own);
    /// A publish settled: @p seq > 0 when the hub kept it, else @p error says why.
    void published(qint64 seq, qint64 count, const QString &error, qint64 elapsedMs);
    /// The link stopped for good (an older backend without the hub).
    void stopped(const QString &why);

private:
    void poll();
    void onPollFinished(QNetworkReply *reply);
    void retryLater();
    QNetworkReply *getLatest();
    void reconcileStep1(QNetworkReply *reply, quint64 gen);
    void reconcileDone();
    bool noteInstance(const QString &instance);   ///< true: the backend restarted (state reset)
    void reject(qint64 seq);
    void post(const QUrl &url, const QByteArray &mask, const QString &nonce, QElapsedTimer t, bool retried);

    QNetworkAccessManager *m_nam{ nullptr };
    QString m_baseUrl, m_token, m_session, m_datasetId;
    QString m_instance;      ///< the backend instance the seqs belong to
    visivo::ShareViewState m_view;
    qint64 m_lastSeq{ 0 };   ///< the long poll's cursor
    qint64 m_seenSeq{ 0 };   ///< the latest seq read from the hub, handled or not
    bool m_primed{ false };
    bool m_running{ false };
    bool m_reconciling{ false };
    bool m_reconcileAgain{ false };
    int m_fails{ 0 };
    quint64 m_generation{ 0 };
    QPointer<QNetworkReply> m_poll;
    QPointer<QNetworkReply> m_rec;
    QTimer m_retry;
};

#endif // SHAREDSELECTIONLINK_H
