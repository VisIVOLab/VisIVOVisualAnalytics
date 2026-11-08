#ifndef TOOL_PROGRESS_SHEET_H
#define TOOL_PROGRESS_SHEET_H
// ToolProgressSheet.h
// One progress affordance for every long-running analysis tool.
//
// The tools grew three different answers to "this is going to take a while":
// Stack and Source Finding put up a window-modal QProgressDialog (on macOS a
// sheet that drops out of the title bar, which is the one users actually
// noticed); Line-width and Baseline wrote into their own dialog's status bar;
// Compute Moment closed its dialog first and then reported into the CUBE
// window's status bar, bottom-left, where a user looking at the tool they just
// launched does not look. Same operation, three places to find out whether
// anything is happening.
//
// So the sheet is the house style and this is the only way to make one. It is
// deliberately indeterminate: none of the backend computes report per-chunk
// progress yet, and a fake percentage is worse than an honest spinner.
//
// Cancellation is NOT offered by showToolProgressSheet. A synchronous compute
// that is already running on the backend keeps running whether or not the
// client stops watching, so a Cancel button that only hides the sheet would be
// a lie; the dialogs handle an attempted close through
// visivo::confirmAbandonRunningCompute (ToolDialogClose.h) instead, which says
// plainly that the result will be discarded.
//
// A backend TASK can be cancelled (DELETE /v1/tasks/{id}), so
// showTaskProgressSheet below does offer Cancel, and shows the task's state
// and elapsed time: those tasks are the ones that run for tens of minutes.
// The one task whose progress is a real measure (decompressing a .fits.gz
// counts its bytes) also gets a percentage and a determinate bar.

#include "app/BackendTaskFollow.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QProgressDialog>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <memory>

namespace visivo {

/// How long work may run before its sheet shows: a compute that finishes
/// sooner never flashes one at the user.
inline constexpr int kProgressSheetDelayMs = 400;

/// Create and show the standard progress sheet for @p parent.
///
/// @param parent the tool dialog (or the viewer window, for a tool that closes
///        its dialog before computing) the sheet belongs to. Never null.
/// @param what   present participle of the work, e.g. "Computing the line-width
///        map". Rendered as "<what>…", so pass it without the ellipsis.
///
/// The caller owns the returned dialog and should close it through
/// closeToolProgressSheet(). A compute that finishes inside 400 ms never
/// flashes a sheet at the user.
inline QProgressDialog *showToolProgressSheet(QWidget *parent, const QString &what)
{
    auto *sheet = new QProgressDialog(what + QStringLiteral("…"),
                                      /*cancelButtonText=*/QString(),
                                      /*minimum=*/0, /*maximum=*/0, parent);
    sheet->setWindowModality(Qt::WindowModal);
    sheet->setMinimumDuration(kProgressSheetDelayMs);
    // The compute owns when this closes, not the progress value: without these
    // an indeterminate dialog can reset and re-show itself.
    sheet->setAutoClose(false);
    sheet->setAutoReset(false);
    sheet->setWindowTitle(QCoreApplication::translate("ToolProgressSheet", "Working"));
    // setValue(), NOT show(): show() puts the sheet up immediately, which
    // defeats the minimumDuration and flashes a sheet at every fast compute
    // (confirmed with an offscreen Qt probe: visible after 50 ms).
    // setValue() arms QProgressDialog's OWN delayed-show timer, so the sheet
    // appears only if the work is still running 400 ms later — and because the
    // timer belongs to the dialog, closing it cancels the pending show. A
    // hand-rolled QTimer::singleShot(400, …, show) does not: measured, it
    // re-showed a sheet that the completion handler had already closed.
    sheet->setValue(0);
    return sheet;
}

/// Turns the window's own close button into Cancel for a task sheet.
///
/// QProgressDialog emits canceled() on a close and then hides: the task would
/// be cancelling with nothing on screen to say so. Only a close the user asked
/// for (a spontaneous event, from the title bar) is turned into Cancel: the
/// sheet's own closeToolProgressSheet(), and the closing of every window when
/// the application quits, close it as usual, so Quit is never held up.
class TaskSheetCloseGuard final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Close && event->spontaneous()) {
            if (auto *sheet = qobject_cast<QProgressDialog *>(watched)) {
                event->ignore();   // a filtered close still closes unless refused
                Q_EMIT sheet->canceled();
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

/// The progress sheet for a backend task followed by visivo::followBackendTask:
/// the task's phase and the elapsed time, refreshed every second, and a Cancel
/// button that asks the worker to abort, which cancels the task on the backend.
///
/// The sheet stays up after Cancel (saying it is cancelling) until the worker
/// returns and the caller closes it through closeToolProgressSheet(): Cancel is
/// a request the backend answers, not the end of the work.
///
/// @p requestedAt, when valid, is when the user asked for the work (the click
/// on Open, say), which can be a second or more before the task was created.
/// The elapsed time counts from it, and the sheet shows kProgressSheetDelayMs
/// after it rather than after this call. Invalid: both count from now.
inline QProgressDialog *showTaskProgressSheet(QWidget *parent, const QString &what,
                                              std::shared_ptr<BackendTaskControl> control,
                                              const QElapsedTimer &requestedAt = QElapsedTimer())
{
    QProgressDialog *sheet = showToolProgressSheet(parent, what);
    // While the value is still the minimum, this re-arms QProgressDialog's own
    // delayed show with what is left of the delay.
    if (requestedAt.isValid())
        sheet->setMinimumDuration(
                progressShowDelayMs(requestedAt.elapsed(), kProgressSheetDelayMs));
    sheet->setCancelButtonText(QCoreApplication::translate("ToolProgressSheet", "Cancel"));
    // QProgressDialog connects canceled() to its own cancel(), which resets
    // and HIDES the sheet. Hidden, it would tell the user nothing while the
    // backend is asked to stop. Qt makes that connection string-based, so it
    // is undone the same way.
    QObject::disconnect(sheet, SIGNAL(canceled()), sheet, SLOT(cancel()));
    sheet->installEventFilter(new TaskSheetCloseGuard(sheet));

    auto clock = std::make_shared<QElapsedTimer>(requestedAt);
    if (!clock->isValid())
        clock->start();
    QPointer<QProgressDialog> guard(sheet);
    auto refresh = [guard, what, control, clock]() {
        if (!guard)
            return;
        // A task that measures its work (TaskPollPolicy::progressIsMeasured)
        // turns the spinner into a bar; every other one keeps the spinner.
        const int percent = control->currentPercent();
        const TaskPhase phase = control->currentPhase();
        guard->setLabelText(describeTaskProgress(what, phase, clock->elapsed(), percent));
        // Not before the sheet is due: setValue() on a hidden QProgressDialog
        // estimates the time left and may show it at once, so a task done in
        // 300 ms flashed a window. Its own timer shows it when due.
        if (!guard->isVisible() && clock->elapsed() < kProgressSheetDelayMs)
            return;
        if (percent >= 0 && phase == TaskPhase::Cancelling) {
            // Stopping is not progress: back to the spinner, and no stale
            // percentage beside a bar that lost its Cancel button's room.
            if (guard->maximum() != 0)
                guard->setRange(0, 0);
        } else if (percent >= 0) {
            if (guard->maximum() != 100)
                guard->setMaximum(100);
            // Only on a change: setValue() on a modal dialog processes events.
            if (guard->value() != percent)
                guard->setValue(percent);
        }
    };
    QObject::connect(sheet, &QProgressDialog::canceled, sheet, [guard, control, refresh]() {
        control->requestAbort();
        // The worker sets Cancelling itself when it acts on the request; say it
        // at once, and take the button away so it is not pressed twice.
        control->setPhase(TaskPhase::Cancelling);
        refresh();
        if (guard)
            guard->setCancelButtonText(QString());
    });
    auto *timer = new QTimer(sheet);
    QObject::connect(timer, &QTimer::timeout, sheet, refresh);
    // Four times a second: the label shows whole seconds, and refreshed once a
    // second from an arbitrary phase it ran up to a second behind.
    timer->start(250);
    refresh();
    return sheet;
}

/// Close and delete the sheet, and clear the caller's pointer. Safe with a
/// null or already-destroyed sheet, so it can be called from a completion
/// handler that may run after the dialog was torn down.
inline void closeToolProgressSheet(QPointer<QProgressDialog> &sheet)
{
    if (!sheet)
        return;
    sheet->close();
    sheet->deleteLater();
    sheet = nullptr;
}

}   // namespace visivo

#endif   // TOOL_PROGRESS_SHEET_H
