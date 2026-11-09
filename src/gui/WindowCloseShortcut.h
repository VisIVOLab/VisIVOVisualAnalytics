#ifndef WINDOW_CLOSE_SHORTCUT_H
#define WINDOW_CLOSE_SHORTCUT_H
// WindowCloseShortcut.h
// Cmd+W (Ctrl+W, Ctrl+F4 elsewhere) closes the viewer window that has focus.
//
// The viewer windows have no File menu, so nothing gave them the platform's
// Close shortcut: only the title bar's close button closed a cube window. The
// action is added to the window itself, with a window-wide context, so it
// works wherever the focus is inside it and never reaches another window.
// It closes the window the way its close button does (close(), not delete),
// so a window that asks before closing (a computation still running) still
// asks.

#include <QAction>
#include <QCoreApplication>
#include <QKeySequence>
#include <QWidget>

namespace visivo {

/// Give @p window the platform's Close Window shortcut. Returns the action
/// (owned by the window), so a window with a menu can list it there too.
inline QAction *installCloseWindowShortcut(QWidget *window)
{
    auto *action = new QAction(
            QCoreApplication::translate("WindowCloseShortcut", "Close Window"), window);
    action->setObjectName(QStringLiteral("actionCloseWindow"));
    action->setShortcuts(QKeySequence::Close);
    action->setShortcutContext(Qt::WindowShortcut);
    // Only while it is a window of its own: a viewer widget hosted in another
    // window (a tab, a dock) must not close itself out of that window.
    QObject::connect(action, &QAction::triggered, window, [window]() {
        if (window->isWindow())
            window->close();
    });
    window->addAction(action);
    return action;
}

}   // namespace visivo

#endif   // WINDOW_CLOSE_SHORTCUT_H
