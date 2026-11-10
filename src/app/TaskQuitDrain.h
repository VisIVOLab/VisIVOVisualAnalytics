#ifndef TASK_QUIT_DRAIN_H
#define TASK_QUIT_DRAIN_H
// TaskQuitDrain.h
// Cancel every followed backend task, and wait for its worker, before the
// application object goes away.
//
// A task is followed on a pool thread (followBackendTask), whose status reads
// and DELETE run their own local event loops. Those need a QCoreApplication.
// Quitting during a moment used to request the abort on aboutToQuit and move
// on: exec() returned, ~QApplication waited for the pool, and the worker,
// noticing the abort only then, started the DELETE's event loop after the
// application was gone ("QEventLoop: Cannot be used without
// QCoreApplication"). It never ended, the process hung with its windows on
// screen, no DELETE reached the backend, and the moment ran to its end (#133).
//
// So every worker that follows a task holds a Follow for that time. drain()
// (on aboutToQuit, and from MainWindow::exitApplication before it exits)
// asks each of them to abort, makes their DELETE short, and waits until all
// have returned and the thread pool is idle, with a bound, while the
// application still exists. A task followed after the drain began is aborted
// at once.
//
// Work still queued on the global pool when the drain begins still runs: no
// task is CREATED once quitting (BackendClient's create calls refuse), and a
// follower of a task created before is aborted as it registers.
//
// Every BackendClient request (performGet / performPost / the binary POST,
// the session mint among them) is dropped once the drain has begun, and the
// 429 and second-token retries stop: no worker waits out a request timeout of
// minutes. Two requests run on: the cancellation DELETE, bounded to
// kQuitCancelTimeoutMs, and a task CREATE already sent, within its own timeout,
// since only its answer carries the id the DELETE needs. What is left for the
// bound is CPU work already under way (decoding a reply), which ends.
//
// A task followed with no worker of its own (a ray-traced render, polled by a
// timer on the GUI thread, #149) registers a QuitCanceller instead: drain()
// calls each one, every one from a thread of its own and all at once, and
// waits for them. Each sends its task's DELETE, bounded, and returns.
//
// Header-only and widget-free (TestTaskQuitDrain).

#include "BackendTaskFollow.h"

#include <QElapsedTimer>
#include <QThread>
#include <QString>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace visivo {

/// A task the quit must cancel that no worker follows: its follower is driven
/// by the GUI thread's event loop, which stops at the quit (RenderTaskHandle).
class QuitCanceller
{
public:
    virtual ~QuitCanceller() = default;
    /// Send the cancellation (bounded by kQuitCancelTimeoutMs) and return.
    /// Called from a thread of the drain's (never the GUI thread: an event
    /// loop there returns at once after exit()), maybe once per drain: a
    /// second call finds nothing left to cancel.
    virtual void cancelForQuit() = 0;
};

class TaskQuitDrain
{
public:
    static TaskQuitDrain &instance()
    {
        static TaskQuitDrain drain;
        return drain;
    }

    /// Held by a worker for as long as it follows a task with @p control.
    class Follow
    {
    public:
        Follow(TaskQuitDrain &drain, BackendTaskControl *control)
            : m_drain(drain), m_control(control)
        {
            m_drain.add(m_control);
        }
        ~Follow() { m_drain.remove(m_control); }
        Follow(const Follow &) = delete;
        Follow &operator=(const Follow &) = delete;

    private:
        TaskQuitDrain &m_drain;
        BackendTaskControl *m_control;
    };

    /// Held for as long as @p canceller may have a task to cancel at quit.
    class Registration
    {
    public:
        Registration(TaskQuitDrain &drain, std::shared_ptr<QuitCanceller> canceller)
            : m_drain(drain), m_canceller(std::move(canceller))
        {
            m_drain.addCanceller(m_canceller);
        }
        ~Registration() { m_drain.removeCanceller(m_canceller.get()); }
        Registration(const Registration &) = delete;
        Registration &operator=(const Registration &) = delete;

    private:
        TaskQuitDrain &m_drain;
        std::shared_ptr<QuitCanceller> m_canceller;
    };

    /// Whether a drain has begun: a cancellation's DELETE is then kept short.
    bool quitting() const { return m_quitting.load(); }

    /// Abort every followed task and wait, at most @p timeoutMs, until its
    /// worker has returned and the global thread pool is idle. The pool too: a
    /// worker still CREATING its task (the POST) holds no Follow yet; it takes
    /// one when the create returns, is aborted at once (quitting), sends its
    /// DELETE and ends, all while this waits. True when nothing is left.
    /// Safe to call twice: the second finds nothing to wait for.
    bool drain(int timeoutMs)
    {
        std::vector<std::shared_ptr<QuitCanceller>> cancellers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cancellers = m_cancellers;
            m_quitting.store(true);
            for (const Entry &entry : m_controls)
                entry.control->requestAbort();
            // One deadline for the whole quit: the first call sets it, a
            // later one (exitApplication, then aboutToQuit) waits only what is
            // left of it, never a second full ceiling.
            if (!m_deadlineSet) {
                m_deadline.start();
                m_deadlineMs = timeoutMs;
                m_deadlineSet = true;
            }
            timeoutMs = static_cast<int>(std::clamp<qint64>(
                    m_deadlineMs - m_deadline.elapsed(), 0, timeoutMs));
        }
        // Work queued behind the running workers is NOT dropped: a queued
        // QtConcurrent job would leave its future unfinished, and a worker
        // waiting on it stuck; and a queued follower may hold a task already
        // created (a decompression's). It runs: a new task is refused while
        // quitting (refuseTaskWhileQuitting), a follower is aborted as it
        // registers, and the wait below covers both.
        QElapsedTimer waited;
        waited.start();
        // The followers without a worker: their DELETEs, side by side, each
        // bounded on its own (kQuitCancelTimeoutMs), so not cut by the
        // deadline; they go out while the workers below cancel theirs.
        std::vector<std::thread> senders;
        senders.reserve(cancellers.size());
        for (const std::shared_ptr<QuitCanceller> &canceller : cancellers)
            senders.emplace_back([canceller]() { canceller->cancelForQuit(); });
        for (std::thread &sender : senders)
            sender.join();
        // Not for a follower on THIS thread: it sits further down the stack
        // (a decompression followed in a local event loop on the GUI thread,
        // which the quit interrupted) and can only go on once the drain
        // returns. It is aborted, and sends its DELETE as the stack unwinds,
        // the application still alive; from a thread of its own, since an
        // event loop on this one returns at once after exit()
        // (BackendClient::decompressForOpen).
        while (activeElsewhere() > 0 && waited.elapsed() < timeoutMs)
            QThread::msleep(10);
        const qint64 left = std::max<qint64>(0, timeoutMs - waited.elapsed());
        const bool poolIdle = QThreadPool::globalInstance()->waitForDone(static_cast<int>(left));
        return activeElsewhere() == 0 && poolIdle;
    }

    int active() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_controls.size());
    }

    /// The followers on another thread than the caller's.
    int activeElsewhere() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const QThread *self = QThread::currentThread();
        return static_cast<int>(std::count_if(m_controls.begin(), m_controls.end(),
                                              [self](const Entry &e) { return e.thread != self; }));
    }

    /// For tests: forget a drain, as a new process would.
    void resetForTest()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quitting.store(false);
        m_deadlineSet = false;
    }

private:
    struct Entry
    {
        BackendTaskControl *control{ nullptr };
        const QThread *thread{ nullptr };
    };

    void add(BackendTaskControl *control)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_controls.push_back({ control, QThread::currentThread() });
        if (m_quitting.load())
            control->requestAbort();   // asked for while the application quits
    }
    void addCanceller(std::shared_ptr<QuitCanceller> canceller)
    {
        // Added after a drain began: called by the next one, if any. Its
        // owner starts no task by then (refuseTaskWhileQuitting), and one it
        // was creating sees the quit itself.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancellers.push_back(std::move(canceller));
    }
    void removeCanceller(const QuitCanceller *canceller)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancellers.erase(std::remove_if(m_cancellers.begin(), m_cancellers.end(),
                                          [canceller](const std::shared_ptr<QuitCanceller> &c) {
                                              return c.get() == canceller;
                                          }),
                           m_cancellers.end());
    }
    void remove(BackendTaskControl *control)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const QThread *self = QThread::currentThread();
        const auto it = std::find_if(m_controls.begin(), m_controls.end(), [&](const Entry &e) {
            return e.control == control && e.thread == self;
        });
        if (it != m_controls.end())
            m_controls.erase(it);   // this Follow's entry, not another with the same control
    }

    mutable std::mutex m_mutex;
    std::vector<Entry> m_controls;
    std::vector<std::shared_ptr<QuitCanceller>> m_cancellers;
    std::atomic_bool m_quitting{ false };
    bool m_deadlineSet{ false };   ///< under m_mutex
    QElapsedTimer m_deadline;
    qint64 m_deadlineMs{ 0 };
};

/// For the task-create calls: true (and @p error set) when the application is
/// quitting, so that queued work starts no task its quit would then have to
/// cancel.
inline bool refuseTaskWhileQuitting(QString &error)
{
    if (!TaskQuitDrain::instance().quitting())
        return false;
    error = QStringLiteral("The application is quitting: no new backend task is started.");
    return true;
}

/// How long a cancellation's DELETE may take while the application quits:
/// long enough for a reachable backend, short enough not to hold the exit.
inline constexpr int kQuitCancelTimeoutMs = 3000;
/// How long the drain waits for the workers. An abort is seen within one check
/// (100 ms) and its DELETE is bounded above; but a Cancel pressed before the
/// quit may have a DELETE under way with the ordinary bound (15 s), and it too
/// needs the application alive to end. The wait ends as soon as the workers
/// have: this is a ceiling for an unreachable backend, not a delay.
inline constexpr int kQuitDrainTimeoutMs = 15'000 + 2000;

}   // namespace visivo

#endif   // TASK_QUIT_DRAIN_H
