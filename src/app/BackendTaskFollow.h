#ifndef BACKEND_TASK_FOLLOW_H
#define BACKEND_TASK_FOLLOW_H
// BackendTaskFollow.h
// Following a backend task (POST /v1/tasks/<op>, then GET /v1/tasks/{id})
// to its end, for computations that can run for tens of minutes.
//
// Why this exists (issue #3): a moment over an SKA-scale cube takes 20-25
// minutes. The backend lets a task run with no wall-clock ceiling, but the
// synchronous route stops at 900 s with a 504. The client used to give a task
// 120 polls of 250 ms (30 s), call anything longer "timed out", and then fall
// back to the SYNCHRONOUS route: the same computation started again from
// scratch while the task was still running, and then ran into the 504.
//
// The rules here:
//   * a task is followed until it reaches a terminal state; there is no
//     client-side time limit, the user stops it with Cancel;
//   * the poll interval backs off to a few seconds, so a long task costs a
//     poll every few seconds rather than four a second;
//   * the synchronous route is used ONLY when the backend has no task route
//     at all (an older backend: 404 or 405 when creating the task). After a
//     task was created, its failure, cancellation or loss is the answer, and
//     it is never computed a second time behind the user's back;
//   * an abort asked for by the user (or by the window going away) is sent to
//     the backend as DELETE /v1/tasks/{id}, so the task stops there too.
//
// The loop takes its effects (poll, cancel, sleep, clock) as hooks, so the
// state machine is tested without a backend (tests/test_backend_task_follow.cpp).
// BackendClient's own wiring is followBackendTask(const BackendClient &, …).

#include "BackendClient.h"

#include <QCoreApplication>
#include <QString>

#include <algorithm>
#include <atomic>
#include <functional>

namespace visivo {

/// What a followed task is doing, for the progress sheet.
enum class TaskPhase : int {
    Starting = 0,   ///< creating the task
    Queued,         ///< the backend has it but has not started it ("pending")
    Running,
    Cancelling,     ///< the user asked to stop; DELETE sent, waiting for the answer
    Synchronous,    ///< older backend: one blocking request, cannot be cancelled
};

/// Shared between the UI thread (which asks for an abort and shows the phase)
/// and the worker that follows the task. Lock-free: both fields are atomics.
struct BackendTaskControl
{
    std::atomic_bool abortRequested{ false };
    std::atomic<int> phase{ static_cast<int>(TaskPhase::Starting) };
    /// 0-100 for a task whose progress measures its work (see
    /// TaskPollPolicy::progressIsMeasured); -1 for every other task.
    std::atomic<int> percent{ -1 };

    void requestAbort() { abortRequested.store(true); }
    bool aborted() const { return abortRequested.load(); }
    void setPhase(TaskPhase p) { phase.store(static_cast<int>(p)); }
    TaskPhase currentPhase() const { return static_cast<TaskPhase>(phase.load()); }
    int currentPercent() const { return percent.load(); }
};

enum class TaskOutcome {
    Completed,      ///< status "completed": the result is in status.resultObject
    Failed,         ///< status "failed" on the backend
    Cancelled,      ///< status "cancelled" (cancelled by someone else, or the backend)
    Aborted,        ///< the user aborted here; the backend was asked to cancel
    Lost,           ///< the status could not be read for too long
    CreateFailed,   ///< the backend refused to start the task
    RouteMissing,   ///< the backend has no task route: the only case for the sync route
};

struct TaskPollPolicy
{
    int firstPollMs{ 250 };
    /// Back-off ceiling. A few seconds: on a 25-minute task the result is
    /// noticed at most this late, and the backend is asked ~500 times, not 6000.
    int maxPollMs{ 3000 };
    double backoff{ 1.5 };
    /// How often an abort is checked while waiting for the next poll.
    int abortCheckMs{ 100 };
    /// Consecutive failed status reads tolerated before the task is given up
    /// (a backend restart forgets its tasks, and asking again changes nothing).
    /// Time, not a count, so it does not depend on the back-off.
    qint64 lostAfterMs{ 120'000 };
    /// The task's progress measures the work done (a decompression counts the
    /// compressed bytes read), so the sheet may show it as a percentage. Off
    /// by default: the moment and PV tasks report a ticker, and a fake
    /// percentage is worse than an honest clock.
    bool progressIsMeasured{ false };
    /// How long the DELETE of a cancellation may take (0: BackendClient's
    /// default, 15 s). Shorter where the caller waits for it on the way out.
    int cancelTimeoutMs{ 0 };
};

/// The percentage a status read's progress (0-1) stands for, 0-100.
inline int taskPercent(double progress)
{
    if (!(progress > 0.0))   // NaN too
        return 0;
    return progress >= 1.0 ? 100 : static_cast<int>(progress * 100.0);
}

struct TaskFollowResult
{
    TaskOutcome outcome{ TaskOutcome::CreateFailed };
    QString taskId;
    /// The last status read; for Completed its resultObject is the product.
    BackendTaskStatusResult status;
    QString error;
    /// For Aborted and Lost: the backend accepted the cancellation (it may
    /// still take a moment to stop the work).
    bool cancelConfirmed{ false };
    int polls{ 0 };

    /// The synchronous route is a fallback for backends WITHOUT task routes,
    /// never for a task that ran and ended badly.
    bool fallBackToSync() const { return outcome == TaskOutcome::RouteMissing; }
    /// Stopped rather than failed: the caller reports it quietly.
    bool stoppedByUser() const { return outcome == TaskOutcome::Aborted; }
};

struct TaskFollowHooks
{
    /// Should return early (status empty) once an abort is requested, so a
    /// slow read does not hold up the DELETE.
    std::function<BackendTaskStatusResult(const QString &taskId)> poll;
    std::function<bool(const QString &taskId, QString &error)> cancel;
    std::function<void(int ms)> sleepMs;
    std::function<qint64()> nowMs;
    /// Optional: told each status read, for logging.
    std::function<void(const BackendTaskStatusResult &)> observed;
};

/// An older backend has no /v1/tasks/<op> route: FastAPI answers 404 for an
/// unknown path and 405 for a path that exists with other methods. A 404 for
/// an unknown dataset is caught here too; the sync route then answers the same
/// 404 at once, so the fallback costs nothing in that case.
inline bool isMissingTaskRoute(const BackendTaskCreateResult &created)
{
    return !created.valid && created.taskId.isEmpty()
           && (created.httpStatus == 404 || created.httpStatus == 405);
}

/// Follow @p created to a terminal state. @p control may be null (no abort,
/// no phase reporting).
inline TaskFollowResult followBackendTask(const BackendTaskCreateResult &created,
                                          const TaskFollowHooks &hooks,
                                          BackendTaskControl *control,
                                          const TaskPollPolicy &policy = {})
{
    TaskFollowResult r;
    r.taskId = created.taskId;
    if (!created.valid || created.taskId.isEmpty()) {
        r.outcome = isMissingTaskRoute(created) ? TaskOutcome::RouteMissing
                                                : TaskOutcome::CreateFailed;
        r.error = created.error.isEmpty()
                ? QCoreApplication::translate("BackendTaskFollow",
                                              "The backend did not start the task.")
                : created.error;
        // Cancel pressed while the create request was out, on a backend with
        // no task route: nothing runs yet, so the sync route must not start.
        if (r.outcome == TaskOutcome::RouteMissing && control && control->aborted()) {
            r.outcome = TaskOutcome::Aborted;
            r.error = QCoreApplication::translate("BackendTaskFollow",
                                                  "Cancelled before the computation started.");
        }
        return r;
    }

    // Once an abort is asked for, the sheet says "cancelling" and a poll that
    // was already under way must not turn it back into "running".
    auto setPhase = [control](TaskPhase p) {
        if (control && (p == TaskPhase::Cancelling || !control->aborted()))
            control->setPhase(p);
    };
    auto abortRequested = [control]() { return control && control->aborted(); };
    // Ask the backend to stop the task, and say how that went. Used for the
    // user's abort and for a task whose status can no longer be read.
    auto cancelOnBackend = [&](QString &detail) {
        if (!hooks.cancel)
            return false;
        return hooks.cancel(created.taskId, detail);
    };

    setPhase(created.status == QLatin1String("pending") ? TaskPhase::Queued : TaskPhase::Running);

    int delay = std::max(1, policy.firstPollMs);
    qint64 failingSince = -1;
    for (;;) {
        for (int waited = 0; waited < delay;) {
            if (abortRequested())
                break;
            const int step = std::max(1, std::min(policy.abortCheckMs, delay - waited));
            hooks.sleepMs(step);
            waited += step;
        }
        if (abortRequested()) {
            setPhase(TaskPhase::Cancelling);
            QString detail;
            r.cancelConfirmed = cancelOnBackend(detail);
            r.outcome = TaskOutcome::Aborted;
            r.error = r.cancelConfirmed
                    ? QCoreApplication::translate("BackendTaskFollow",
                                                  "Cancelled: the backend accepted the "
                                                  "cancellation of task %1 and is stopping it.")
                              .arg(created.taskId)
                    : QCoreApplication::translate(
                              "BackendTaskFollow",
                              "Cancelled here, but the backend did not confirm stopping task "
                              "%1%2; it may still be running.")
                              .arg(created.taskId,
                                   detail.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(detail));
            return r;
        }

        const BackendTaskStatusResult s = hooks.poll(created.taskId);
        ++r.polls;
        if (hooks.observed)
            hooks.observed(s);
        delay = std::min(policy.maxPollMs,
                         std::max(delay + 1, static_cast<int>(delay * policy.backoff)));

        // A read cut short by the abort is not an outage: go straight to the
        // DELETE at the top of the loop.
        if (s.status.isEmpty() && abortRequested())
            continue;
        if (s.status.isEmpty()) {
            // The status could not be read (transport error, a 404 after a
            // backend restart, a garbled body). A failed read is not a failed
            // task, within bounds; past them the task is lost to us and is
            // stopped if the backend can still be reached.
            const qint64 now = hooks.nowMs();
            if (failingSince < 0)
                failingSince = now;
            if (now - failingSince >= policy.lostAfterMs) {
                QString detail;
                r.cancelConfirmed = cancelOnBackend(detail);
                r.outcome = TaskOutcome::Lost;
                r.error = QCoreApplication::translate(
                                  "BackendTaskFollow",
                                  "Lost track of task %1: its status could not be read for %2 s "
                                  "(%3). %4")
                                  .arg(created.taskId)
                                  .arg(policy.lostAfterMs / 1000)
                                  .arg(s.error.isEmpty() ? QStringLiteral("no answer") : s.error)
                                  .arg(r.cancelConfirmed
                                               ? QCoreApplication::translate(
                                                         "BackendTaskFollow",
                                                         "The backend accepted its cancellation.")
                                               : QCoreApplication::translate(
                                                         "BackendTaskFollow",
                                                         "It could not be cancelled and may still "
                                                         "be running."));
                return r;
            }
            continue;
        }
        failingSince = -1;
        r.status = s;

        // The answer came after the user aborted: the abort wins, whatever the
        // task did meanwhile. A result nobody wants any more is not shown, and
        // a task that already ended needs no DELETE.
        if (abortRequested() && (s.status == QLatin1String("completed")
                                 || s.status == QLatin1String("failed")
                                 || s.status == QLatin1String("cancelled"))) {
            setPhase(TaskPhase::Cancelling);
            r.outcome = TaskOutcome::Aborted;
            r.error = QCoreApplication::translate("BackendTaskFollow",
                                                  "Cancelled: task %1 had already ended on "
                                                  "the backend; its result is not used.")
                              .arg(created.taskId);
            return r;
        }
        if (abortRequested())
            continue;   // still running: the DELETE at the top of the loop

        if (s.status == QLatin1String("completed")) {
            r.outcome = TaskOutcome::Completed;
            return r;
        }
        if (s.status == QLatin1String("failed")) {
            r.outcome = TaskOutcome::Failed;
            r.error = s.error.isEmpty()
                    ? QCoreApplication::translate("BackendTaskFollow",
                                                  "Task %1 failed on the backend.")
                              .arg(created.taskId)
                    : s.error;
            return r;
        }
        if (s.status == QLatin1String("cancelled")) {
            r.outcome = TaskOutcome::Cancelled;
            r.error = s.error.isEmpty()
                    ? QCoreApplication::translate("BackendTaskFollow",
                                                  "Task %1 was cancelled on the backend.")
                              .arg(created.taskId)
                    : s.error;
            return r;
        }
        // "pending", "running", or a state this client does not know yet:
        // keep following; the user can always cancel.
        setPhase(s.status == QLatin1String("pending") ? TaskPhase::Queued : TaskPhase::Running);
        if (policy.progressIsMeasured && control && s.status == QLatin1String("running"))
            control->percent.store(taskPercent(s.progress));
    }
}

/// How much longer a progress sheet should wait before it shows, so that it
/// appears @p targetMs after the user asked for the work. The sheet is made
/// only once the task exists, which on a file open comes after a health check,
/// a classification and the create request: counted from there, the sheet
/// appeared late and its clock ran behind the click.
inline int progressShowDelayMs(qint64 sinceRequestMs, int targetMs = 500)
{
    return static_cast<int>(std::clamp<qint64>(targetMs - std::max<qint64>(0, sinceRequestMs), 0,
                                               std::max(0, targetMs)));
}

/// The progress sheet's text for a followed task: what is happening and for
/// how long. A percentage only when @p percent is one (0-100) and the task is
/// running: most tasks report a ticker, not a measure of the work, and a fake
/// percentage is worse than an honest clock; a decompression counts its bytes.
inline QString describeTaskProgress(const QString &what, TaskPhase phase, qint64 elapsedMs,
                                    int percent = -1)
{
    const qint64 seconds = std::max<qint64>(0, elapsedMs / 1000);
    const QString clock = seconds < 60
            ? QStringLiteral("%1 s").arg(seconds)
            : QStringLiteral("%1 min %2 s").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    QString state;
    switch (phase) {
    case TaskPhase::Starting:
        state = QCoreApplication::translate("BackendTaskFollow", "starting the task on the backend");
        break;
    case TaskPhase::Queued:
        state = QCoreApplication::translate("BackendTaskFollow",
                                            "queued on the backend, waiting for a compute slot");
        break;
    case TaskPhase::Running:
        state = QCoreApplication::translate("BackendTaskFollow", "running on the backend");
        break;
    case TaskPhase::Cancelling:
        state = QCoreApplication::translate("BackendTaskFollow",
                                            "cancelling the task on the backend");
        break;
    case TaskPhase::Synchronous:
        state = QCoreApplication::translate(
                "BackendTaskFollow",
                "running as one request (this backend has no task routes, so it cannot be "
                "cancelled)");
        break;
    }
    if (percent >= 0 && phase == TaskPhase::Running)
        return QCoreApplication::translate("BackendTaskFollow", "%1… %2 %\n%3, %4 elapsed.")
                .arg(what, QString::number(std::min(100, percent)), state, clock);
    return QCoreApplication::translate("BackendTaskFollow", "%1…\n%2, %3 elapsed.")
            .arg(what, state, clock);
}

/// The same loop wired to @p client: GET /v1/tasks/{id}, DELETE /v1/tasks/{id},
/// a sleeping worker thread, and Diagnostics for the transitions. Call it on a
/// worker thread, never on the GUI thread: it blocks until the task ends.
TaskFollowResult followBackendTask(const BackendClient &client,
                                   const BackendTaskCreateResult &created,
                                   const QString &logTag, BackendTaskControl *control,
                                   const TaskPollPolicy &policy = {});

}   // namespace visivo

#endif   // BACKEND_TASK_FOLLOW_H
