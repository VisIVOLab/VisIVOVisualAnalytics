#ifndef BACKEND_PROCESS_TREE_H
#define BACKEND_PROCESS_TREE_H
// BackendProcessTree.h
// The local backend and every process it starts, held as one unit so that
// stopping the backend stops all of them (#176).
//
// The backend is not one process: uvicorn runs a compute pool, render-session
// children, cube readers and pyramid builders, and Python's multiprocessing
// adds its resource tracker. None of them dies with its parent. Ending only
// the process QProcess started left the rest running with parent PID 1, each
// keeping its memory, for days (five of them found nine days later).
//
//   Unix     The backend is started in a session (and so a process group) of
//            its own, whose id is its pid. Every child inherits the group, so
//            one signal to the group reaches the whole family. stop() sends
//            SIGTERM, waits for the GROUP to empty — not only for the process
//            QProcess started, whose workers can outlive it by seconds or
//            forever — and then sends SIGKILL to whatever is left.
//   Windows  The backend is created suspended, put in a Job object that kills
//            every process in it when its last handle closes
//            (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE), and only then resumed.
//            The handle is held by this object, and the system closes it when
//            the client dies, so the tree ends even after a crash. Children
//            join the job by default.
//
// A client crash on Unix runs none of this: the backend notices by itself that
// the client is gone (VISIVO_EXIT_WITH_PID, backend/app/parent_watch.py) and
// shuts its own children down. It is started directly, not through a shell, so
// its parent is the client.
//
// Header-only, QtCore only (TestBackendProcessTree).

#include <QDeadlineTimer>
#include <QPointer>
#include <QProcess>
#include <QThread>

#include <memory>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace visivo {

class BackendProcessTree
{
public:
    BackendProcessTree() = default;
    BackendProcessTree(const BackendProcessTree &) = delete;
    BackendProcessTree &operator=(const BackendProcessTree &) = delete;

    ~BackendProcessTree()
    {
#if defined(Q_OS_WIN)
        // Closing the last handle ends every process still in the job.
        if (m_job)
            ::CloseHandle(m_job);
#endif
    }

    /// Before @p process starts: arrange for it to lead a tree of its own.
    /// adopt() must follow every successful start (on Windows the process is
    /// created suspended and only adopt() lets it run).
    void prepare(QProcess &process)
    {
#if defined(Q_OS_UNIX)
        // setsid() rather than setpgid(0, 0): a new session also has no
        // controlling terminal, so a Ctrl-C in the terminal that launched the
        // client reaches the client, which then stops the backend in order,
        // instead of hitting the backend's workers mid-write.
        process.setChildProcessModifier([] { ::setsid(); });
#elif defined(Q_OS_WIN)
        // Created suspended, so that it joins the job before it runs a single
        // instruction. Joining after the start was not enough: a venv's
        // Scripts\python.exe is a redirector that starts the real interpreter
        // at once, and that one would have been born outside the job.
        // QProcess hands the modifier its own PROCESS_INFORMATION, which it
        // fills in and keeps (with the thread handle) while the process runs;
        // adopt() reads the handles from it right after start().
        m_startInfo = nullptr;
        process.setCreateProcessArgumentsModifier(
                [this](QProcess::CreateProcessArguments *args) {
                    args->flags |= CREATE_SUSPENDED;
                    m_startInfo = reinterpret_cast<PROCESS_INFORMATION *>(
                            args->processInformation);
                });
#else
        Q_UNUSED(process);
#endif
    }

    /// Once @p process has started (right after start(), before the event loop
    /// runs again): remember its tree, and on Windows let it run. False when
    /// it could not be put in one (Windows: the job could not be made or
    /// joined); stopping then reaches only the process itself.
    bool adopt(QProcess &process)
    {
#if defined(Q_OS_UNIX)
        const qint64 pid = process.processId();
        if (pid <= 0)
            return false;
        m_group = static_cast<pid_t>(pid);
        return true;
#elif defined(Q_OS_WIN)
        PROCESS_INFORMATION *info = m_startInfo;
        m_startInfo = nullptr;
        // The modifier stays installed for nobody: a later start() of the same
        // QProcess must not be suspended with no adopt() to resume it.
        process.setCreateProcessArgumentsModifier({});
        if (!info || process.state() == QProcess::NotRunning)
            return false;
        const bool joined = ensureJob() && ::AssignProcessToJobObject(m_job, info->hProcess) != 0;
        m_joined = m_joined || joined;
        // Whatever happened above, the backend must run.
        if (info->hThread)
            ::ResumeThread(info->hThread);
        return joined;
#else
        Q_UNUSED(process);
        return false;
#endif
    }

    /// Whether any process of the tree is still there (a zombie counts until
    /// it is reaped).
    bool alive() const
    {
#if defined(Q_OS_UNIX)
        if (m_group <= 0)
            return false;
        // EPERM: a member we may not signal, which still exists.
        return ::kill(-m_group, 0) == 0 || errno == EPERM;
#elif defined(Q_OS_WIN)
        if (!m_joined)
            return false;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!::QueryInformationJobObject(m_job, JobObjectBasicAccountingInformation, &info,
                                         sizeof(info), nullptr))
            return false;
        return info.ActiveProcesses > 0;
#else
        return false;
#endif
    }

    /// Ask every process of the tree to end (Unix: SIGTERM to the group; the
    /// backend's handler shuts its pool down). Windows has no such request for
    /// a console program: QProcess::terminate() is all there is.
    void requestStop() const
    {
#if defined(Q_OS_UNIX)
        if (m_group > 0)
            ::killpg(m_group, SIGTERM);
#endif
    }

    /// End every process of the tree now.
    void killAll() const
    {
#if defined(Q_OS_UNIX)
        if (m_group > 0)
            ::killpg(m_group, SIGKILL);
#elif defined(Q_OS_WIN)
        if (m_joined)
            ::TerminateJobObject(m_job, 1);
#endif
    }

    /// Wait until the tree is empty or @p deadline passes; true when empty.
    bool waitGone(QDeadlineTimer deadline) const
    {
        while (alive()) {
            if (deadline.hasExpired())
                return false;
            QThread::msleep(20);
        }
        return true;
    }

    /// The tree is empty: its id may now be reused by an unrelated process, so
    /// it must not be signalled again.
    void forget()
    {
#if defined(Q_OS_UNIX)
        m_group = 0;
#elif defined(Q_OS_WIN)
        m_joined = false;
#endif
    }

    /// Stop @p process (may be null or already finished) and its whole tree:
    /// ask, give them @p graceMs together, then kill what is left. Blocking;
    /// meant for the quit, after the event loop.
    void stop(QProcess *process, int graceMs)
    {
        const QDeadlineTimer grace(graceMs);
        const bool leaderRunning = process && process->state() == QProcess::Running;
        requestStop();
        if (leaderRunning) {
            process->terminate();
            // Reaps the leader, so it no longer counts as a member below.
            process->waitForFinished(qMax<qint64>(0, grace.remainingTime()));
        }
        // The leader too: a tree that could not be set up (alive() false) says
        // nothing about it, and on Windows terminate() does not end a console
        // program at all.
        const bool treeGone = waitGone(grace);
        if (!treeGone || (process && process->state() == QProcess::Running)) {
            // A worker inside a long native call, or a server still draining:
            // take them down rather than leave them behind.
            killAll();
            if (process && process->state() == QProcess::Running) {
                process->kill();
                process->waitForFinished(1000);
            }
            // Orphans are reaped by init/launchd, not by us: give it a moment.
            waitGone(QDeadlineTimer(1000));
        }
        if (!alive())
            forget();
    }

    /// The process group id (Unix), 0 when none is held. For tests and logs.
    qint64 groupId() const
    {
#if defined(Q_OS_UNIX)
        return m_group;
#else
        return 0;
#endif
    }

private:
#if defined(Q_OS_WIN)
    bool ensureJob()
    {
        if (m_job)
            return true;
        m_job = ::CreateJobObjectW(nullptr, nullptr);
        if (!m_job)
            return false;
        // No breakaway allowed: a child cannot leave the job, even one that
        // asks to (CREATE_BREAKAWAY_FROM_JOB then fails). A job of the
        // child's own, as the venv redirector makes, nests inside this one.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits,
                                       sizeof(limits))) {
            ::CloseHandle(m_job);
            m_job = nullptr;
            return false;
        }
        return true;
    }
#endif

#if defined(Q_OS_UNIX)
    pid_t m_group{ 0 };
#elif defined(Q_OS_WIN)
    HANDLE m_job{ nullptr };
    bool m_joined{ false };
    PROCESS_INFORMATION *m_startInfo{ nullptr };   ///< QProcess's, between prepare and adopt
#endif
};

/// Trees whose server is gone or is being replaced, each given its grace to
/// end by itself before what is left is killed. Each keeps its own group id
/// (or job), so a retired tree is never confused with the backend running now:
/// ending it cannot reach the new one, and the new one cannot hide it. A tree
/// is dropped the moment it is seen empty, because from then on its id may
/// belong to an unrelated process. tick() is called often (BackendLauncher
/// every 50 ms); quit() ends them all, bounded.
class RetiredProcessTrees
{
public:
    /// Take @p tree out of service: ask its processes to end, and give them
    /// @p graceMs. An empty tree is dropped at once. @p leader, the QProcess
    /// that started it, is kept (weakly) so that quit() can reap it.
    void retire(std::unique_ptr<BackendProcessTree> tree, int graceMs,
                QProcess *leader = nullptr)
    {
        if (!tree)
            return;
        if (!tree->alive()) {
            tree->forget();
            return;
        }
        tree->requestStop();
        m_trees.push_back({ std::move(tree), QDeadlineTimer(graceMs), leader });
    }

    /// Drop the trees that are empty and kill the ones past their grace.
    /// True while some remain to be looked at.
    bool tick()
    {
        for (auto it = m_trees.begin(); it != m_trees.end();) {
            const bool alive = it->tree->alive();
            if (alive && !it->deadline.hasExpired()) {
                ++it;
                continue;
            }
            // Killed or gone: either way not signalled again. The orphans of a
            // killed one are reaped by init/launchd.
            if (alive)
                it->tree->killAll();
            it->tree->forget();
            it = m_trees.erase(it);
        }
        return !m_trees.empty();
    }

    /// The client quits: end every retired tree within what is left of its
    /// grace. Blocking. The leader is passed along so that stop() reaps it:
    /// with the event loop over nothing else would, and a zombie leader
    /// still counts as a member of its group (on Linux Qt waits through a
    /// pidfd, so not even the system reaps it), which would make every quit
    /// shortly after a Retry wait out the grace and the kill.
    void quit()
    {
        for (Entry &e : m_trees)
            e.tree->stop(e.leader.data(), int(qMax<qint64>(0, e.deadline.remainingTime())));
        m_trees.clear();
    }

    bool isEmpty() const { return m_trees.empty(); }
    int size() const { return int(m_trees.size()); }

private:
    struct Entry
    {
        std::unique_ptr<BackendProcessTree> tree;
        QDeadlineTimer deadline;
        QPointer<QProcess> leader;   ///< null once deleted (deleteLater on finish)
    };
    std::vector<Entry> m_trees;
};

}   // namespace visivo

#endif // BACKEND_PROCESS_TREE_H
