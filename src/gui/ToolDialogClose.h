#ifndef TOOL_DIALOG_CLOSE_H
#define TOOL_DIALOG_CLOSE_H
// ToolDialogClose.h
// One place for what happens when a tool window is closed with a computation
// still running.
//
// Every compute dialog in the app had the same closeEvent: `event->accept()`,
// with a comment explaining that the worker completes on the thread pool and the
// finished handler discards the result because the QPointer to the dialog is
// null. All true — and invisible to the user, who was asked nothing and told
// nothing (user question: "what happens if I close the popup? does it keep
// computing? how would I know? is it actually cancelled, or are there zombie
// processes?").
//
// The honest answers, which this dialog now gives:
//
//   * the computation KEEPS RUNNING — on the backend to completion, holding one
//     of the session's compute slots, and its result is thrown away;
//   * it CANNOT be cancelled: these endpoints are synchronous POSTs with no
//     cancel route, and the backend's process-pool children cannot be killed
//     from Python (see the note in dependencies.py). Nothing leaks as an OS
//     zombie — the work simply finishes unobserved;
//   * while it runs it is visible in the status bar's "N running" count and in
//     Diagnostics (the request announces itself when it starts).
//
// So the choice is the user's, stated plainly, rather than a silent discard.

#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QWidget>

namespace visivo {

/// Ask before abandoning an in-flight computation. True = go ahead and close.
inline bool confirmAbandonRunningCompute(QWidget *dialog, const QString &toolName,
                                         const QString &what)
{
    QMessageBox box(dialog);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(toolName);
    box.setText(QStringLiteral("%1 is still running on the backend.").arg(what));
    box.setInformativeText(QStringLiteral(
            "Closing this window discards the result. The computation itself cannot be "
            "cancelled - it will run to completion on the backend and keep one of this "
            "session's compute slots busy until it does.\n\n"
            "You can watch it in the status bar (\"N running\") and in Diagnostics."));
    QPushButton *closeBtn = box.addButton(QStringLiteral("Close and Discard"),
                                          QMessageBox::DestructiveRole);
    QPushButton *keepBtn = box.addButton(QStringLiteral("Keep Waiting"),
                                         QMessageBox::RejectRole);
    box.setDefaultButton(keepBtn);
    box.exec();
    return box.clickedButton() == closeBtn;
}

}   // namespace visivo

#endif   // TOOL_DIALOG_CLOSE_H
