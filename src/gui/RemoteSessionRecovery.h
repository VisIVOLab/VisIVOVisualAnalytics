#ifndef RemoteSessionRecovery_h
#define RemoteSessionRecovery_h

// RemoteSessionRecovery.h
// What a remote view (a cube's, a point table's) does when its stream drops
// while it was live.
//
// A render session can end under a connected client: its render process
// crashed or was killed, or the backend closed it. Over the WebSocket the
// backend then closes with the reason "session ended"
// (docs/technical/backend-api/render.md); WebRTC has no close reason, so a viewer asks
// the backend whether the session is still there (GET .../levels answers 404
// for one that is gone) and decides as if that reason had come. Reconnecting
// to an ended session is refused, so the view used to sit on "Remote
// rendering - Disconnected" with its last frame rate until the user switched
// to Local and back.
//
// Now the view reconnects by itself (issue #411: after a sleep or a screen
// lock the view stayed on a dead stream). A drop is followed by a check of the
// session: one still there gets the same session's stream again, one that is
// gone (closed for idling while the laptop slept, its process killed) a new
// session, with the view's state and camera sent to it. A backend out of reach
// (a tunnel being reopened, a backend restarting) is asked again after a
// growing wait, a bounded number of times; then the view stops and offers
// Reconnect.
//
// A new session is made once per stable stream: a replacement that drops with
// its session ended before it has been live for kStableLiveMs (a render
// process that dies at its first full-resolution frame, say) is not replaced
// again, or the view would cycle create, live, crash for ever.
//
// Header-only, no widgets (TestRemoteRenderQueue).

#include <QElapsedTimer>
#include <QString>

#include <algorithm>
#include <array>

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

/// The waits before each automatic attempt, in ms: short at first (a blip),
/// then long enough for an SSH tunnel to be reopened (ClusterConnection sees a
/// dead one within a minute and reopens it every 3 s) or a backend to restart.
/// About two minutes in all; then the view offers Reconnect.
inline constexpr std::array<int, 8> kReconnectDelaysMs{ 1'000,  2'000,  4'000,  8'000,
                                                        15'000, 30'000, 30'000, 30'000 };

/// How long the same session's stream may take to come back before the
/// attempt counts as failed (a negotiation that never reports an end).
inline constexpr int kReconnectAttemptTimeoutMs = 30'000;

enum class ReconnectStep {
    Ignore,         ///< Nothing to do: not a live remote view, or no reconnection under way.
    CheckSession,   ///< Ask the backend whether the session is still there (GET .../levels).
    SameSession,    ///< It is: connect the stream to it again.
    NewSession,     ///< It is gone: ask for a new one, then send it the view's state.
    Wait,           ///< Not now: try again after delayMs().
    GiveUp,         ///< Stop, and offer Reconnect.
};

/// A view's automatic reconnection, from a drop to Live or to giving up.
class RemoteReconnect
{
public:
    /// A stream dropped.
    ReconnectStep onDrop(const RemoteDrop &d)
    {
        if (!d.showingRemote || !d.wasLive)
            return ReconnectStep::Ignore;
        m_refused = false;
        if (d.liveMs >= kStableLiveMs) {
            // What came before worked for a while: a new outage, a new budget.
            m_failures = 0;
            m_replacedLive = false;
            m_inOutage = false;
        }
        // A stream this outage brought back that dropped again soon after: a
        // failed attempt, or a link that comes and goes would be reconnected
        // for ever without a wait.
        const bool droppedAgain = m_inOutage;
        m_inOutage = true;
        m_active = true;
        m_replacing = false;
        if (sessionEnded(d))
            return newSessionOrGiveUp();
        if (droppedAgain)
            return failed();
        return ReconnectStep::CheckSession;
    }

    /// The check answered: @p httpStatus 0 when the backend did not answer.
    ReconnectStep onCheck(int httpStatus)
    {
        if (!m_active)
            return ReconnectStep::Ignore;
        if (httpStatus == 404 || httpStatus == 410)
            return newSessionOrGiveUp();
        if (httpStatus == 401 || httpStatus == 403) {
            // The backend refuses this client's token: asking again changes
            // nothing (refusedAccess() lets the view say so).
            m_active = false;
            m_refused = true;
            return ReconnectStep::GiveUp;
        }
        if (httpStatus >= 200 && httpStatus < 300)
            return ReconnectStep::SameSession;
        return failed();   // out of reach, or an error: the session may still be there
    }

    /// The attempt (the same session's stream, or a new session) did not
    /// reach Live: the backend could not be reached, refused, or never answered.
    ReconnectStep onAttemptFailed()
    {
        if (!m_active)
            return ReconnectStep::Ignore;
        m_replacing = false;
        return failed();
    }

    /// The wait after a failure is over.
    ReconnectStep onWaited() const
    {
        return m_active ? ReconnectStep::CheckSession : ReconnectStep::Ignore;
    }

    /// The stream is live again: the reconnection is over. A new session that
    /// went live counts against the next drop that ends it quickly.
    void onLive()
    {
        if (m_replacing)
            m_replacedLive = true;
        m_replacing = false;
        m_active = false;
    }

    /// The user clicked Reconnect: a fresh budget, and one new session.
    ReconnectStep userReconnect()
    {
        m_failures = 0;
        m_replacedLive = false;
        m_replacing = false;
        m_inOutage = true;   // what it brings back is judged as any reconnection
        m_refused = false;
        m_active = true;
        return ReconnectStep::CheckSession;
    }

    /// The user went to Local, or the window let its session go.
    void cancel()
    {
        m_active = false;
        m_replacing = false;
    }

    /// Between a drop and Live (or giving up).
    bool active() const { return m_active; }
    /// The last give-up was the backend refusing the token (401/403).
    bool refusedAccess() const { return m_refused; }
    /// The attempt under way, from 1.
    int attempt() const { return m_failures + 1; }
    static constexpr int maxAttempts() { return int(kReconnectDelaysMs.size()) + 1; }
    /// The wait before the next attempt, after a failure.
    int delayMs() const
    {
        const int i = std::clamp(m_failures - 1, 0, int(kReconnectDelaysMs.size()) - 1);
        return kReconnectDelaysMs[size_t(i)];
    }

private:
    ReconnectStep failed()
    {
        ++m_failures;
        if (m_failures >= maxAttempts()) {
            m_active = false;
            return ReconnectStep::GiveUp;
        }
        return ReconnectStep::Wait;
    }

    ReconnectStep newSessionOrGiveUp()
    {
        if (m_replacedLive) {
            // A new session already went live in this outage and ended soon
            // after: one more would end the same way.
            m_active = false;
            return ReconnectStep::GiveUp;
        }
        m_replacing = true;
        return ReconnectStep::NewSession;
    }

    bool m_active = false;
    bool m_replacing = false;      ///< the attempt under way is a new session
    bool m_replacedLive = false;   ///< a new session went live in this outage
    bool m_inOutage = false;       ///< dropped, and not stable since
    bool m_refused = false;        ///< gave up on a 401/403
    int m_failures = 0;
};

/// What the view says while it reconnects: the attempt, and the wait if any.
inline QString reconnectStatus(int attempt, int maxAttempts, int waitMs)
{
    if (waitMs <= 0)
        return QStringLiteral("Reconnecting to the backend (attempt %1 of %2)…")
                .arg(attempt)
                .arg(maxAttempts);
    return QStringLiteral("Connection to the backend lost - trying again in %1 s "
                          "(attempt %2 of %3)…")
            .arg((waitMs + 999) / 1000)
            .arg(attempt)
            .arg(maxAttempts);
}

/// A stream that stopped without saying so: a WebSocket whose pings get no
/// answer, a WebRTC stream with no frame. A peer that vanished (a laptop that
/// slept, an SSH tunnel that died under it, a backend that closed the session
/// but not the peer) never reports an end, so the view would stay on its last
/// frame for good. check() runs every kCheckMs; traffic() is any sign of life.
///
/// A check that comes long after the one before means this process was not
/// running (App Nap, a sleep): the time it was away is not silence, so the
/// count starts again rather than declaring a stall that is only a late timer.
class StreamWatchdog
{
public:
    static constexpr qint64 kCheckMs = 2'000;
    static constexpr qint64 kSuspendedGapMs = 3 * kCheckMs;

    enum class Verdict { Off, Fine, Stalled };

    /// @p firstTrafficGraceMs: extra time before the first sign of life is
    /// due (a WebRTC stream is Live before its first frame, which a large
    /// cube may take a while to render).
    void start(qint64 nowMs, qint64 firstTrafficGraceMs = 0)
    {
        m_on = true;
        m_lastCheck = nowMs;
        m_lastTraffic = nowMs + std::max<qint64>(0, firstTrafficGraceMs);
    }
    void stop() { m_on = false; }
    bool running() const { return m_on; }

    void traffic(qint64 nowMs) { m_lastTraffic = nowMs; }

    /// @p silenceMs: how long without traffic is a stall. A stall stops the
    /// watchdog: the stream is over, the next one starts it again.
    Verdict check(qint64 nowMs, qint64 silenceMs)
    {
        if (!m_on)
            return Verdict::Off;
        if (nowMs - m_lastCheck > kSuspendedGapMs)
            m_lastTraffic = std::max(m_lastTraffic, nowMs);   // away, not silent
        m_lastCheck = nowMs;
        if (nowMs - m_lastTraffic > silenceMs) {
            m_on = false;
            return Verdict::Stalled;
        }
        return Verdict::Fine;
    }

private:
    bool m_on = false;
    qint64 m_lastTraffic = 0;
    qint64 m_lastCheck = 0;
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
        m_lastTrafficMs = -1;
        m_liveSince.invalidate();
    }

    /// The stream has frames on screen (the first one, or WebRTC's Live).
    void markLive()
    {
        if (!m_liveSince.isValid())
            m_liveSince.start();
    }

    /// A sign of life from the backend (a frame, a pong) while live.
    void markTraffic()
    {
        if (m_liveSince.isValid())
            m_lastTrafficMs = m_liveSince.elapsed();
    }

    /// The stream ended; @p reason is the WebSocket's close reason. It was
    /// live up to its last sign of life, not up to when its end was noticed:
    /// a stream that died at once but was taken for lost only after the
    /// watchdog's silence (or WebRTC's own checks) is not a stable one, or a
    /// half-dead link would reset the reconnection's budget at every drop.
    void closed(bool wasLive, const QString &reason)
    {
        m_wasLive = wasLive;
        m_reason = reason;
        m_liveMs = !m_liveSince.isValid() ? 0
                : m_lastTrafficMs >= 0    ? m_lastTrafficMs
                                          : m_liveSince.elapsed();
        m_lastTrafficMs = -1;
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
    qint64 m_lastTrafficMs = -1;   ///< since live; -1: none seen
    QElapsedTimer m_liveSince;
};

}   // namespace visivo

#endif   // RemoteSessionRecovery_h
