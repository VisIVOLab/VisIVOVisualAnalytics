#include "FinderTaskRun.h"

#include "BackendClient.h"
#include "TaskQuitDrain.h"

#include <QThread>

using namespace Qt::StringLiterals;

namespace visivo {

namespace {

constexpr int kTaskPollMs = 500;
constexpr int kTaskCancelCheckMs = 100;
/// Consecutive status requests that may fail before the task is given up
/// (a backend restarted, the task evicted: asking again changes nothing).
constexpr int kMaxTaskPollFailures = 10;

} // namespace

FinderTaskOutcome runFinderTask(const BackendClient &client, const QString &datasetId,
                                const QString &finder, const QJsonObject &params,
                                std::atomic_bool *cancel,
                                const std::function<void(const QString &)> &log,
                                const std::function<void(double)> &progress)
{
    const auto say = [&log](const QString &line) {
        if (log)
            log(line);
    };
    FinderTaskOutcome o;
    const BackendTaskCreateResult created = client.createSourceFindTask(datasetId, finder, params);
    if (!created.valid) {
        o.refusedStatus = created.httpStatus >= 400 ? created.httpStatus : 0;
        o.error = created.error.isEmpty() ? u"The backend did not start the finder."_s : created.error;
        return o;
    }
    o.taskId = created.taskId;
    say(u"task %1 started on the backend"_s.arg(created.taskId));
    // Under the quit drain, as followBackendTask is (TaskQuitDrain.h): quitting
    // aborts this run, which cancels the task with the quit's short DELETE,
    // while the application still exists. Without it the quit only dropped the
    // status reads, and the DELETE came after the "lost track" polls, with the
    // ordinary 15 s bound: past the drain's ceiling.
    BackendTaskControl quitControl;
    const TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), &quitControl);
    const auto stopAsked = [&]() { return (cancel && cancel->load()) || quitControl.aborted(); };
    const auto cancelTimeoutMs = []() {
        return TaskQuitDrain::instance().quitting() ? kQuitCancelTimeoutMs : 0;
    };
    double lastProgress = -1;
    int failures = 0;
    auto cancelled = [&]() {
        QString cancelError;
        const bool confirmed = client.cancelTask(created.taskId, cancelError, cancelTimeoutMs());
        o.cancelled = true;
        o.error = confirmed ? u"cancelled"_s
                            : u"cancelled, but the backend did not confirm it%1; the task may still be running"_s.arg(
                                      cancelError.isEmpty() ? QString() : u" ("_s + cancelError + u")"_s);
        return o;
    };
    for (;;) {
        for (int waited = 0; waited < kTaskPollMs; waited += kTaskCancelCheckMs) {
            if (stopAsked())
                return cancelled();
            QThread::msleep(kTaskCancelCheckMs);
        }
        const BackendTaskStatusResult status =
                client.requestTaskStatus(created.taskId, &quitControl.abortRequested);
        if (stopAsked())
            return cancelled();   // the read was dropped for it
        if (!status.valid && status.status.isEmpty()) {
            // A poll that failed is not a task that failed, within bounds;
            // past them the task is lost to us and stopped if it can be.
            if (++failures >= kMaxTaskPollFailures) {
                QString cancelError;
                const bool stopped =
                        client.cancelTask(created.taskId, cancelError, cancelTimeoutMs());
                o.error = u"lost track of task %1 on the backend (%2); %3"_s.arg(
                        created.taskId, status.error,
                        stopped ? u"the backend accepted its cancellation"_s
                                : u"it could not be cancelled and may still be running%1"_s.arg(
                                          cancelError.isEmpty() ? QString() : u" ("_s + cancelError + u")"_s));
                return o;
            }
            say(u"(poll) "_s + status.error);
            continue;
        }
        failures = 0;
        if (status.progress != lastProgress && status.status == "running"_L1) {
            lastProgress = status.progress;
            say(u"running… %1%"_s.arg(static_cast<int>(status.progress * 100)));
            if (progress)
                progress(status.progress);
        }
        if (status.status == "completed"_L1) {
            o.valid = true;
            o.result = status.resultObject;
            return o;
        }
        if (status.status == "failed"_L1 || status.status == "cancelled"_L1) {
            o.cancelled = status.status == "cancelled"_L1;
            o.error = status.error.isEmpty() ? u"the task %1"_s.arg(status.status) : status.error;
            return o;
        }
    }
}

} // namespace visivo
