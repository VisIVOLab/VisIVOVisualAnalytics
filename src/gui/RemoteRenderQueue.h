#ifndef RemoteRenderQueue_h
#define RemoteRenderQueue_h

// RemoteRenderQueue.h
// Asking the backend for a render session when its slots may all be busy
// (issue #4, docs/backend-api/render.md, "The queue").
//
// A viewer asks create_session with "queue": true. When every slot is busy the
// backend answers 202 with a place in the queue instead of refusing; the viewer
// asks again with that place every poll_after_seconds until it is answered with
// a session (200). A place that is no longer known answers 410, and the viewer
// asks again without it, at the back of the queue. A full queue, or a wait past
// the backend's timeout, answers 503: the viewer stays local.
//
// The cube and the point viewers both follow these rules. This header reads the
// answers and keeps the place, with no widgets and no network, so the rules can
// be tested; the viewers send the requests.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cmath>

namespace visivo {

/// What a create_session answer is.
enum class RenderSessionAnswerKind {
    Session,     ///< 200 with a session_id: connect to it.
    Queued,      ///< 202 queued: every slot busy, a place in the queue.
    Preparing,   ///< 202 preparing: the cube's brick pyramid is being built.
    PlaceGone,   ///< 410: the place is no longer in the queue; ask again without it.
    Refused,     ///< 503: no server-side rendering, or no slot and no place.
    Failed,      ///< Anything else: a network error, another HTTP error, no session_id.
};

struct RenderSessionAnswer
{
    RenderSessionAnswerKind kind = RenderSessionAnswerKind::Failed;
    int httpStatus = 0;
    QString sessionId;
    /// The backend's sentence: the 202's "error", or an error's "detail".
    QString message;
    /// A 503 because every slot is busy (queue full, or waited too long), not
    /// because this backend cannot render at all.
    bool capacity = false;

    // A place in the queue (Queued).
    QString queueId;
    int position = 0;
    int queueLength = 0;
    int activeSessions = 0;
    int capacitySlots = 0;
    double waitedSeconds = 0.0;
    double waitTimeoutSeconds = 0.0;
    double pollAfterSeconds = 2.0;

    // A pyramid build (Preparing).
    double progress = 0.0;
};

/// Reads a create_session answer. @p httpStatus is the reply's status code (0
/// when the request never got one), @p networkError whether Qt reported an
/// error (it does for every 4xx and 5xx), @p doc the body.
inline RenderSessionAnswer parseRenderSessionAnswer(int httpStatus, bool networkError,
                                                    const QJsonObject &doc)
{
    RenderSessionAnswer a;
    a.httpStatus = httpStatus;
    const QString detail = doc.value(QLatin1String("detail")).toString();
    if (httpStatus == 410) {
        a.kind = RenderSessionAnswerKind::PlaceGone;
        a.message = detail;
        return a;
    }
    if (httpStatus == 503) {
        a.kind = RenderSessionAnswerKind::Refused;
        a.message = detail;
        // "Server-side rendering capacity reached ..." (render.md, Errors).
        a.capacity = detail.contains(QLatin1String("capacity reached"));
        return a;
    }
    if (networkError || httpStatus < 200 || httpStatus >= 300) {
        a.kind = RenderSessionAnswerKind::Failed;
        a.message = detail;
        return a;
    }
    if (httpStatus == 202) {
        a.message = doc.value(QLatin1String("error")).toString();
        if (doc.value(QLatin1String("queued")).toBool()) {
            a.queueId = doc.value(QLatin1String("queue_id")).toString();
            // A place the backend did not name cannot be asked for again.
            a.kind = a.queueId.isEmpty() ? RenderSessionAnswerKind::Failed
                                         : RenderSessionAnswerKind::Queued;
            a.position = doc.value(QLatin1String("position")).toInt();
            a.queueLength = doc.value(QLatin1String("queue_length")).toInt();
            a.activeSessions = doc.value(QLatin1String("active_sessions")).toInt();
            a.capacitySlots = doc.value(QLatin1String("capacity")).toInt();
            a.waitedSeconds = doc.value(QLatin1String("waited_seconds")).toDouble();
            a.waitTimeoutSeconds = doc.value(QLatin1String("wait_timeout_seconds")).toDouble();
            const QJsonValue poll = doc.value(QLatin1String("poll_after_seconds"));
            if (poll.isDouble())
                a.pollAfterSeconds = poll.toDouble();
            return a;
        }
        if (doc.value(QLatin1String("preparing")).toBool()) {
            a.kind = RenderSessionAnswerKind::Preparing;
            a.progress = doc.value(QLatin1String("progress")).toDouble();
            return a;
        }
        a.kind = RenderSessionAnswerKind::Failed;
        return a;
    }
    a.sessionId = doc.value(QLatin1String("session_id")).toString();
    a.kind = a.sessionId.isEmpty() ? RenderSessionAnswerKind::Failed
                                   : RenderSessionAnswerKind::Session;
    return a;
}

/// When to ask again for a place: the backend's poll_after_seconds, kept
/// between 1 and 5 s. Never longer: a place not asked for within
/// expires_after_seconds (at least 6 s) is given to the next request.
inline int queuePollDelayMs(const RenderSessionAnswer &a)
{
    const double s = std::isfinite(a.pollAfterSeconds) ? a.pollAfterSeconds : 2.0;
    return static_cast<int>(std::lround(std::clamp(s, 1.0, 5.0) * 1000.0));
}

/// The banner's line for a place in the queue.
inline QString queueStatusText(const RenderSessionAnswer &a)
{
    if (a.position <= 0)
        return QStringLiteral("A server-side rendering slot is free: starting the view…");
    QString text = QStringLiteral("Queued for server-side rendering: %1 of %2")
                           .arg(a.position)
                           .arg(std::max(a.position, a.queueLength));
    if (a.capacitySlots > 0)
        text += QStringLiteral(" (all %1 slots busy)").arg(a.capacitySlots);
    text += QStringLiteral(". The view appears by itself when its turn comes; "
                           "going back to Local gives the place up.");
    return text;
}

/// A viewer's place in the queue, and what to do with each answer.
///
/// The viewer keeps its own request generation (bumped when it stops waiting,
/// so a late answer is known to be stale) and passes whether an answer is
/// current. A stale answer still carries what the backend made for it: a
/// session, or a place, that nobody will use and must be given back.
class RenderQueuePlace
{
public:
    enum class Step {
        Ignore,      ///< Nothing to do.
        Connect,     ///< A session: connect to it.
        AskLater,    ///< Queued: ask again after queuePollDelayMs().
        AskNow,      ///< The place is gone: ask again at once, without it.
        Preparing,   ///< A pyramid build: ask again in a moment.
        StayLocal,   ///< Refused or failed: stay local and say why.
    };

    struct Decision
    {
        Step step = Step::Ignore;
        /// A session made for a request nobody waits for: DELETE it.
        QString sessionToDelete;
        /// A place nobody waits in: DELETE /v1/render/queue/{id}.
        QString placeToCancel;
    };

    /// The place to send back as queue_id; empty for a first request.
    const QString &queueId() const { return m_queueId; }
    bool waiting() const { return !m_queueId.isEmpty(); }

    /// The create_session body: always willing to wait, with the place if any.
    QJsonObject requestBody(const QString &datasetId) const
    {
        QJsonObject body{ { QStringLiteral("dataset_id"), datasetId },
                          { QStringLiteral("queue"), true } };
        if (!m_queueId.isEmpty())
            body.insert(QStringLiteral("queue_id"), m_queueId);
        return body;
    }

    Decision decide(bool current, const RenderSessionAnswer &a)
    {
        Decision d;
        if (!current) {
            // The viewer stopped waiting (Local, closed, timed out) while this
            // was in flight: what the backend made for it is nobody's.
            if (a.kind == RenderSessionAnswerKind::Session)
                d.sessionToDelete = a.sessionId;
            else if (a.kind == RenderSessionAnswerKind::Queued)
                d.placeToCancel = a.queueId;
            return d;
        }
        switch (a.kind) {
        case RenderSessionAnswerKind::Session:
            m_queueId.clear();   // used: the backend has dropped it
            d.step = Step::Connect;
            break;
        case RenderSessionAnswerKind::Queued:
            // A different place than the one asked with (a request that lost
            // its own): the old one is the backend's to drop, keep the new.
            m_queueId = a.queueId;
            d.step = Step::AskLater;
            break;
        case RenderSessionAnswerKind::PlaceGone:
            m_queueId.clear();
            d.step = Step::AskNow;
            break;
        case RenderSessionAnswerKind::Preparing:
            d.step = Step::Preparing;
            break;
        case RenderSessionAnswerKind::Refused:
            // A 503 drops the place on the backend (full, or waited too long).
            m_queueId.clear();
            d.step = Step::StayLocal;
            break;
        case RenderSessionAnswerKind::Failed:
            // A network error may have left the place on the backend: give it
            // back rather than hold a slot reserved for nobody until it expires.
            d.placeToCancel = take();
            d.step = Step::StayLocal;
            break;
        }
        return d;
    }

    /// Stop waiting: the place to give up (empty when there is none).
    QString take()
    {
        QString id = m_queueId;
        m_queueId.clear();
        return id;
    }

private:
    QString m_queueId;
};

} // namespace visivo

#endif // RemoteRenderQueue_h
