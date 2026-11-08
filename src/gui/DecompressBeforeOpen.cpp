#include "DecompressBeforeOpen.h"

#include "ToolProgressSheet.h"
#include "app/BackendClient.h"
#include "app/BackendTaskFollow.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

using namespace Qt::Literals::StringLiterals;

namespace visivo {

namespace {

/// The progress window of the decompression under way, if it is on screen:
/// a second open asked for meanwhile brings it to the front.
QPointer<QProgressDialog> &runningSheet()
{
    static QPointer<QProgressDialog> sheet;
    return sheet;
}

}   // namespace

DecompressBeforeOpenResult decompressBeforeOpen(QWidget *parent, const BackendClient &client,
                                                const QString &path,
                                                const QElapsedTimer &requestedAt,
                                                const std::function<void()> &onTaskStarted)
{
    DecompressBeforeOpenResult out;
    if (!BackendClient::isCompressedFitsName(path))
        return out;

    // One at a time. The sheet is not modal (below), so File ▸ Open stays
    // reachable while it is up, and a second decompression would nest its
    // wait inside the first one's: the first could not finish before it.
    // Taken before the create request, which waits in an event loop too.
    static bool busy = false;
    if (busy) {
        // Point at the work that is in the way. Only when it is on screen:
        // show() would bypass the window's delayed appearance.
        if (QProgressDialog *running = runningSheet(); running && running->isVisible()) {
            running->raise();
            running->activateWindow();
        }
        out.proceed = false;
        out.busy = true;
        out.error = u"Another file is being decompressed. Wait for it to open, or cancel it, "
                    u"then open %1 again."_s.arg(QFileInfo(path).fileName());
        return out;
    }
    busy = true;
    const auto release = qScopeGuard([]() { busy = false; });

    const BackendTaskCreateResult created = client.createDecompressTask(path);
    // An older backend has no route; a missing file is a 404 as well. Either
    // way the open that follows says what is wrong, in its own words.
    if (isMissingTaskRoute(created) || BackendClient::decompressionAlreadyDone(created))
        return out;
    if (!created.valid || created.taskId.isEmpty()) {
        out.proceed = false;
        out.error = created.error.isEmpty()
                ? u"The backend did not start decompressing %1."_s.arg(QFileInfo(path).fileName())
                : created.error;
        return out;
    }

    auto control = std::make_shared<BackendTaskControl>();
    QPointer<QProgressDialog> sheet = showTaskProgressSheet(
            parent, u"Decompressing %1"_s.arg(QFileInfo(path).fileName()), control, requestedAt);
    // Not modal: on macOS a modal sheet disables the application menu, so
    // Quit (and Cmd+Q, and the Dock's Quit) did nothing until the
    // decompression ended. Quitting now ends the wait below, and the task is
    // stopped as Cancel stops it.
    sheet->setWindowModality(Qt::NonModal);
    // A non-modal dialog is an ordinary window on macOS: one click on the
    // Data Hub put it behind the main window, out of reach. A tool window
    // stays above its application's windows there (and hides with the
    // application, not above other ones), and above its parent on X11 and
    // Wayland, without blocking either. Set before it is first shown.
    sheet->setWindowFlags((sheet->windowFlags() & ~Qt::WindowType_Mask) | Qt::Tool);
    runningSheet() = sheet;
    if (onTaskStarted)
        onTaskStarted();
    // The window going away takes the sheet with it: stop the work too.
    QObject::connect(sheet, &QObject::destroyed, [control]() { control->requestAbort(); });

    TaskPollPolicy policy;
    policy.maxPollMs = 1000;            // the bar moves every second, not every three
    policy.progressIsMeasured = true;   // the backend counts the bytes it has read
    // A quit waits for the DELETE (below): bounded within that wait.
    policy.cancelTimeoutMs = 2000;

    // Followed on a pool thread, as every task is (followBackendTask sleeps);
    // this thread waits in a local event loop, so the window keeps painting
    // and the sheet's Cancel button works.
    const BackendClient worker = client;
    QFutureWatcher<TaskFollowResult> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<TaskFollowResult>::finished, &loop,
                     &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([worker, created, control, policy]() {
        return followBackendTask(worker, created, u"[decompress][task]"_s, control.get(), policy);
    }));
    // Quitting the application ends this loop too, with the task still under
    // way: ask the backend to stop it rather than leave a decompression
    // nobody will open behind.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &loop,
                     [control]() { control->requestAbort(); });
    if (!watcher.isFinished())
        loop.exec();
    if (!watcher.isFinished()) {
        // The application is quitting. Stop the task as Cancel does: the
        // worker notices within 100 ms and sends the DELETE from its own
        // thread. Give it a moment (bounded: a quit must not hang on an
        // unreachable backend), so the request leaves before the process,
        // and the local backend with it, goes away.
        control->requestAbort();
        closeToolProgressSheet(sheet);
        QElapsedTimer waited;
        waited.start();
        // The worker ends within one abort check (100 ms) plus the DELETE's
        // own bound, policy.cancelTimeoutMs: the thread pool is not left
        // holding the exit on an unresponsive backend.
        while (!watcher.isFinished() && waited.elapsed() < policy.cancelTimeoutMs + 1000)
            QThread::msleep(20);
        out.proceed = false;
        out.cancelled = true;
        return out;
    }
    const TaskFollowResult followed = watcher.result();
    // Cancel may have been pressed after the worker returned and before its
    // answer reached this thread: the user's request wins. Read before the
    // sheet closes, which can itself emit canceled().
    const bool abortedHere = control->aborted();
    closeToolProgressSheet(sheet);

    if (followed.outcome == TaskOutcome::Completed && !abortedHere)
        return out;
    if (followed.outcome == TaskOutcome::Completed) {
        out.proceed = false;
        out.cancelled = true;
        return out;
    }
    out.proceed = false;
    out.cancelled = followed.stoppedByUser();
    out.error = followed.error;
    return out;
}

}   // namespace visivo
