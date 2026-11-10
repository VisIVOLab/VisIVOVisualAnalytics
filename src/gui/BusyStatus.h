#pragma once

// BusyStatus.h
// The one busy indicator of a window (issue #159): for an operation that can
// take more than a moment and reports no progress of its own (a progress
// window with Cancel is for those that do, ToolProgressSheet.h). While an
// operation runs the window's status bar shows a moving bar and what is going
// on, and the window has a busy cursor; its outcome is said in the status bar
// when it comes. Operations are kept apart and steps of one operation join it
// (BusyTracker.h has the rules).
//
//   visivo::BusyStatus busy(this, visivo::openingTexts(name));
//   ...
//   busy.finish(visivo::BusyOutcome::Failed);   // before the error box
//
// The cursor is the window's, not an application override; and while a modal
// box is up inside the operation (a chooser, a viewer's own error box), a
// BusyPause takes every indicator down, so the box is what is read.

#include "BusyTracker.h"

#include <QPointer>

class QMainWindow;

namespace visivo {

class BusyStatus
{
public:
    /// How the scope relates to others on the window.
    enum class Scope {
        Joined,     ///< a step of the operation running at this event-loop level, if any
        Detached,   ///< an operation of its own, held past its function by a callback
    };

    BusyStatus(QMainWindow *window, const BusyTexts &texts, Scope scope = Scope::Joined);
    ~BusyStatus();
    BusyStatus(const BusyStatus &) = delete;
    BusyStatus &operator=(const BusyStatus &) = delete;

    /// Says how the operation ended and, once no other runs, takes the
    /// indicator down (only its first outcome counts). @p detail follows the
    /// outcome's sentence.
    void finish(BusyOutcome outcome, const QString &detail = {});

private:
    QPointer<QMainWindow> m_window;
    int m_token{ 0 };
};

/// While alive, no busy indicator shows: put it around a modal box that comes
/// up in the middle of a busy operation.
class BusyPause
{
public:
    BusyPause();
    ~BusyPause();
    BusyPause(const BusyPause &) = delete;
    BusyPause &operator=(const BusyPause &) = delete;
};

/// Where a busy indicator for a request made from a viewer goes: the active
/// window, when it is a main window (the viewer that asked, which is what the
/// user is looking at), otherwise @p fallback.
QMainWindow *busyWindowFor(QMainWindow *fallback);

}  // namespace visivo
