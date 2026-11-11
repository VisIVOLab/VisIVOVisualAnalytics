#ifndef RemoteSessionRecovery_h
#define RemoteSessionRecovery_h

// RemoteSessionRecovery.h
// What a remote view (a cube's, a point table's) does when its stream drops
// while it was live.
//
// A render session can end under a connected client: its render process
// crashed or was killed, or the backend closed it. Over the WebSocket the
// backend then closes with the reason "session ended"
// (docs/backend-api/render.md); WebRTC has no close reason, so a viewer asks
// the backend whether the session is still there (GET .../levels answers 404
// for one that is gone) and decides as if that reason had come. Reconnecting
// to an ended session is refused, so the view used to sit on "Remote
// rendering - Disconnected" with its last frame rate until the user switched
// to Local and back.
//
// Now the view asks for a new session by itself, once. Once means once per
// stable stream: a replacement that drops again before it has been live for
// kStableLiveMs (a render process that dies at its first full-resolution
// frame, say) is not replaced again, or the view would cycle create, live,
// crash for ever; the banner offers Retry instead. A stream that dropped for
// another reason (the backend went away, the network) offers Retry at once:
// its session may still be there.
//
// Header-only, no widgets (TestRemoteRenderQueue).

#include <QElapsedTimer>
#include <QString>

namespace visivo {

/// The close reason the backend gives when the session itself has ended.
inline const QString &sessionEndedCloseReason()
{
    static const QString reason = QStringLiteral("session ended");
    return reason;
}

/// How long a stream must have been live for its drop to count as a new
/// failure, not as the same one again.
inline constexpr qint64 kStableLiveMs = 10'000;

enum class RemoteDropAction {
    Ignore,       ///< Not a live remote view: a negotiation's own failure path handles it.
    NewSession,   ///< Ask the backend for a new session, now, without asking the user.
    OfferRetry,   ///< Show Retry: the user decides when to try again.
};

struct RemoteDrop
{
    bool showingRemote = false;   ///< The view draws remotely (not Local, not switching).
    bool wasLive = false;         ///< The stream had frames on screen when it dropped.
    QString closeReason;          ///< The close reason, empty without one (always, on WebRTC).
    qint64 liveMs = 0;            ///< How long the dropped stream had been live.
};

inline bool sessionEnded(const RemoteDrop &d)
{
    return d.closeReason == sessionEndedCloseReason();
}

/// A view's replacements: whether a drop gets a new session or Retry.
class RemoteRecovery
{
public:
    RemoteDropAction onDrop(const RemoteDrop &d)
    {
        if (!d.showingRemote || !d.wasLive)
            return RemoteDropAction::Ignore;
        if (d.liveMs >= kStableLiveMs)
            m_replacements = 0;   // what came before worked for a while
        if (sessionEnded(d) && m_replacements == 0) {
            ++m_replacements;
            return RemoteDropAction::NewSession;
        }
        return RemoteDropAction::OfferRetry;
    }

    /// The user clicked Retry: that is this failure's replacement, so a quick
    /// drop of it offers Retry again rather than a session of its own.
    void userRetried() { m_replacements = 1; }

    int replacements() const { return m_replacements; }

private:
    int m_replacements = 0;
};

/// Whether the create, or the connection after it, now under way replaces a
/// stream that dropped (by itself or by Retry). Only then does its failure
/// keep the view in Remote with Retry; a switch the user starts later fails
/// as any switch does. Every way out of the replacement ends it: Live, a
/// failure, the user going back to Local, the window releasing its session.
class RemoteReplacement
{
public:
    /// A replacement may start only while the user's choice is not Local (a
    /// drop check answered after ✕ must not bring Remote back) and no other
    /// switch is under way.
    static bool mayStart(bool userChoseLocal, bool switchInProgress)
    {
        return !userChoseLocal && !switchInProgress;
    }

    void started() { m_active = true; }
    /// Live, a cancel (Local, release, abort), or a switch that did not start.
    void ended() { m_active = false; }
    /// The create or its connection failed: true when it was a replacement's
    /// (show Retry), and the replacement is over either way.
    bool failed()
    {
        const bool was = m_active;
        m_active = false;
        return was;
    }
    bool active() const { return m_active; }

private:
    bool m_active = false;
};

/// A drop check (GET .../levels, for a stream without a close reason) whose
/// answer comes back: acted on only if the view is still the remote one it
/// was asked for.
inline bool dropCheckStillWanted(bool showingRemote, bool sameSession, bool switchInProgress)
{
    return showingRemote && sameSession && !switchInProgress;
}

/// Why a replacement session could not be made, for the Retry banner: the
/// backend out of reach (no HTTP status), or its answer. A failed replacement
/// keeps the view in Remote with Retry; it never says the backend cannot
/// render, which a failed first switch with an HTTP error does.
inline QString replacementFailure(int httpStatus, const QString &message)
{
    if (httpStatus == 0)
        return QStringLiteral("the backend could not be reached");
    if (message.isEmpty())
        return QStringLiteral("the backend could not start a new view (HTTP %1)").arg(httpStatus);
    return QStringLiteral("the backend could not start a new view: ") + message;
}

/// What the widget knows when a stream drops, for either transport. Reset at
/// each connection and at a deliberate teardown, so a later drop never reads
/// an earlier one's facts.
class StreamCloseRecorder
{
public:
    void connecting()
    {
        m_wasLive = false;
        m_reason.clear();
        m_liveMs = 0;
        m_liveSince.invalidate();
    }

    /// The stream has frames on screen (the first one, or WebRTC's Live).
    void markLive()
    {
        if (!m_liveSince.isValid())
            m_liveSince.start();
    }

    /// The stream ended; @p reason is the WebSocket's close reason.
    void closed(bool wasLive, const QString &reason)
    {
        m_wasLive = wasLive;
        m_reason = reason;
        m_liveMs = m_liveSince.isValid() ? m_liveSince.elapsed() : 0;
        m_liveSince.invalidate();
    }

    /// WebRTC reports its end as a state ("Disconnected", "Error: ...");
    /// it carries no close reason.
    void webrtcState(const QString &state, bool wasLive)
    {
        if (state == QLatin1String("Live"))
            markLive();
        else if (isTerminal(state))
            closed(wasLive, QString());
    }

    static bool isTerminal(const QString &state)
    {
        return state == QLatin1String("Disconnected") || state == QLatin1String("Failed")
                || state == QLatin1String("Closed") || state.startsWith(QLatin1String("Error"));
    }

    bool wasLive() const { return m_wasLive; }
    QString reason() const { return m_reason; }
    qint64 liveMs() const { return m_liveMs; }

private:
    bool m_wasLive = false;
    QString m_reason;
    qint64 m_liveMs = 0;
    QElapsedTimer m_liveSince;
};

}   // namespace visivo

#endif   // RemoteSessionRecovery_h
