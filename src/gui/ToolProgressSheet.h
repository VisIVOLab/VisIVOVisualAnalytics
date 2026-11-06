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

#include "app/BackendTaskFollow.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProgressDialog>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <memory>

namespace visivo {

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
    sheet->setMinimumDuration(400);
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

/// The progress sheet for a backend task followed by visivo::followBackendTask:
/// the task's phase and the elapsed time, refreshed every second, and a Cancel
/// button that asks the worker to abort, which cancels the task on the backend.
///
/// The sheet stays up after Cancel (saying it is cancelling) until the worker
/// returns and the caller closes it through closeToolProgressSheet(): Cancel is
/// a request the backend answers, not the end of the work.
inline QProgressDialog *showTaskProgressSheet(QWidget *parent, const QString &what,
                                              std::shared_ptr<BackendTaskControl> control)
{
    QProgressDialog *sheet = showToolProgressSheet(parent, what);
    sheet->setCancelButtonText(QCoreApplication::translate("ToolProgressSheet", "Cancel"));
    // QProgressDialog connects canceled() to its own cancel(), which resets
    // and HIDES the sheet. Hidden, it would tell the user nothing while the
    // backend is asked to stop. Qt makes that connection string-based, so it
    // is undone the same way.
    QObject::disconnect(sheet, SIGNAL(canceled()), sheet, SLOT(cancel()));

    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QPointer<QProgressDialog> guard(sheet);
    auto refresh = [guard, what, control, clock]() {
        if (guard)
            guard->setLabelText(describeTaskProgress(what, control->currentPhase(),
                                                     clock->elapsed()));
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
    timer->start(1000);
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
