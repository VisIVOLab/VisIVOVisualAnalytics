#ifndef OPEN_RENDER_SESSIONS_H
#define OPEN_RENDER_SESSIONS_H
// OpenRenderSessions.h
// The server-side render sessions this client holds (cube and point-table
// views, docs/backend-api/render.md), so that quitting closes them.
//
// A viewer gives its session back with a DELETE sent and forgotten
// (RenderSessionCalls), from its teardown. At quit that teardown comes after
// the event loop has ended, so the DELETE never left: the backend only saw
// the WebSocket go, and kept each session, its render process and (on a Mac)
// its GPU context until the idle timeout, five minutes later.
//
// So a session is listed here from its create's answer until its DELETE is
// answered, and each one listed registers a QuitCanceller: the quit drain
// (TaskQuitDrain.h) sends its DELETE, bounded by kQuitCancelTimeoutMs, from a
// thread of its own, every session at once. A session whose DELETE went out
// and was answered before the quit is no longer listed; one whose DELETE was
// still in flight is closed again, which the backend answers 404, harmless.
// One whose DELETE was refused (401, 403: a token since replaced) stays
// listed, so the quit tries again.
//
// Not covered: a session whose create is still unanswered when the quit
// begins. Its id comes only with the answer, which the ended event loop never
// reads; the backend closes it at its idle timeout.
//
// Header-only and widget-free (TestTaskQuitDrain, VisIVOQuitExitTest).

#include "BackendClient.h"
#include "TaskQuitDrain.h"

#include <QDebug>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace visivo {

class OpenRenderSessions
{
public:
    static OpenRenderSessions &instance()
    {
        static OpenRenderSessions sessions;
        return sessions;
    }

    /// The backend at @p backendUrl made @p renderSessionId for this client
    /// (@p token, backend session @p sessionId): closed at quit unless given
    /// back first.
    void opened(const QString &backendUrl, const QString &token, const QString &sessionId,
                const QString &renderSessionId)
    {
        if (renderSessionId.isEmpty())
            return;
        auto session = std::make_shared<Session>(backendUrl, token, sessionId, renderSessionId);
        std::lock_guard<std::mutex> lock(m_mutex);
        Entry &entry = m_open[key(backendUrl, renderSessionId)];
        entry.registration.reset();
        entry.session = session;
        entry.registration =
                std::make_unique<TaskQuitDrain::Registration>(TaskQuitDrain::instance(), session);
    }

    /// Its DELETE was answered (or it is known to be gone): nothing left to
    /// close at quit.
    void closed(const QString &backendUrl, const QString &renderSessionId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_open.erase(key(backendUrl, renderSessionId));
    }

    int count() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_open.size());
    }

    /// For tests: forget every session without closing it.
    void clearForTest()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_open.clear();
    }

private:
    // The drain first: statics end in reverse order of construction, and the
    // registrations this holds unregister from the drain as they go.
    OpenRenderSessions() { TaskQuitDrain::instance(); }

    class Session : public QuitCanceller
    {
    public:
        Session(const QString &backendUrl, const QString &token, const QString &sessionId,
                const QString &renderSessionId)
            : m_backendUrl(backendUrl), m_token(token), m_sessionId(sessionId),
              m_renderSessionId(renderSessionId)
        {
        }

        void cancelForQuit() override
        {
            if (m_sent.exchange(true))
                return;   // a second drain: already closed
            // A client of this thread's: built here, used here.
            BackendClient client(m_backendUrl, m_token);
            client.setSessionId(m_sessionId);
            QString error;
            if (client.closeRenderSession(m_renderSessionId, error, kQuitCancelTimeoutMs))
                qDebug().noquote() << "[quit] render session" << m_renderSessionId << "closed";
            else
                qWarning().noquote() << QStringLiteral("[quit] render session %1 not closed: %2")
                                                .arg(m_renderSessionId, error);
        }

    private:
        const QString m_backendUrl, m_token, m_sessionId, m_renderSessionId;
        std::atomic_bool m_sent{ false };
    };

    struct Entry
    {
        std::shared_ptr<Session> session;
        std::unique_ptr<TaskQuitDrain::Registration> registration;
    };

    static QString key(const QString &backendUrl, const QString &renderSessionId)
    {
        return backendUrl + QLatin1Char('\n') + renderSessionId;
    }

    mutable std::mutex m_mutex;
    std::map<QString, Entry> m_open;   ///< under m_mutex
};

}   // namespace visivo

#endif   // OPEN_RENDER_SESSIONS_H
