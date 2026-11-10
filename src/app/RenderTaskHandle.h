#ifndef RENDER_TASK_HANDLE_H
#define RENDER_TASK_HANDLE_H
// RenderTaskHandle.h
// The ray-traced render task a view holds (RaytraceOutput, issues #101, #106),
// shared by the view, the worker that creates it and the quit drain (#149).
//
// The view follows its render with a timer on the GUI thread, and keeps the
// task once it has completed (its frames, its regrade). Nothing of that runs
// once the application quits: the event loop has ended, and a DELETE sent
// from the view's destructor never leaves. So the task's id lives here, and
// whoever takes it sends its DELETE, exactly once:
//   - the view, when it forgets the render (a new one, Cancel, closed);
//   - the worker that created it, when the view forgot it meanwhile, or the
//     application is quitting by the time the create answers;
//   - the quit drain (cancelForQuit), from a thread of its own, bounded.
// The create runs on a worker (create()), not on the GUI thread's manager: a
// create still answering at quit is then waited for by the drain (the global
// pool), and its task cancelled, instead of being lost with the event loop.
//
// Widget-free (TestTaskQuitDrain, VisIVOQuitExitTest).

#include "BackendClient.h"
#include "TaskQuitDrain.h"

#include <QDebug>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <mutex>

namespace visivo {

class RenderTaskHandle : public QuitCanceller
{
public:
    explicit RenderTaskHandle(BackendClient client) : m_client(std::move(client)) {}

    /// The render held now: the view's routes (frames, tone, light) use it.
    QString taskId() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_taskId;
    }

    /// The view no longer wants its render: the task held, if any, is
    /// returned for the caller to delete, and a create still answering is
    /// deleted by its worker. The returned generation is the next create's.
    quint64 forget(QString *taken)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_generation;
        if (taken)
            *taken = m_taskId;
        m_taskId.clear();
        return m_generation;
    }

    /// On a worker: create the render of @p generation (blocking). Kept when
    /// the view still wants it and the application is not quitting; else
    /// deleted here (bounded while quitting) and returned without its id.
    BackendTaskCreateResult create(quint64 generation, const QString &taskPath,
                                   const QJsonObject &request)
    {
        BackendTaskCreateResult created = client().createRenderTask(taskPath, request);
        if (!created.valid)
            return created;
        {
            // Under the lock with the quit's check: cancelForQuit runs only
            // once quitting() is true, so either it finds the id stored here,
            // or this sees the quit and deletes it; never both, never neither.
            std::lock_guard<std::mutex> lock(m_mutex);
            if (generation == m_generation && !TaskQuitDrain::instance().quitting()) {
                m_taskId = created.taskId;
                return created;
            }
        }
        const int timeoutMs = TaskQuitDrain::instance().quitting() ? kQuitCancelTimeoutMs : 0;
        QString error;
        client().deleteRender(created.taskId, error, timeoutMs);
        created.valid = false;
        created.taskId.clear();
        created.error = QStringLiteral("The render was no longer wanted when it started.");
        return created;
    }

    /// Delete @p taskId (forget()'s), on this thread, bounded.
    bool remove(const QString &taskId, QString &error, int timeoutMs = 0) const
    {
        return client().deleteRender(taskId, error, timeoutMs);
    }

    void cancelForQuit() override
    {
        QString taken;
        forget(&taken);
        if (taken.isEmpty())
            return;
        QString error;
        if (!client().deleteRender(taken, error, kQuitCancelTimeoutMs))
            qWarning().noquote() << QStringLiteral("[quit] render %1 not cancelled: %2")
                                            .arg(taken, error);
    }

private:
    /// A copy per request: a request may adopt another token into its client
    /// (a 401 retry), and the view's, the worker's and the quit's run at once.
    BackendClient client() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_client;
    }

    BackendClient m_client;    ///< under m_mutex
    mutable std::mutex m_mutex;
    QString m_taskId;          ///< under m_mutex
    quint64 m_generation{ 0 }; ///< under m_mutex
};

}   // namespace visivo

#endif   // RENDER_TASK_HANDLE_H
