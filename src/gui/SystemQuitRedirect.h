#ifndef SYSTEM_QUIT_REDIRECT_H
#define SYSTEM_QUIT_REDIRECT_H
// SystemQuitRedirect.h
// The system's quit (the Dock's Quit on macOS, a log-out) goes the way the
// Quit menu does: "Do you want to exit?", then MainWindow::exitApplication,
// which cancels the backend work and waits for it (TaskQuitDrain.h).
//
// Left to Qt, a system quit arrives as a spontaneous QEvent::Quit, and
// QApplication answers it by sending a close event to every window first. A
// cube window computing a moment refuses its close (it hides itself and closes
// when the work ends, vtkWindowCube::closeEvent), and one refusal cancels the
// whole quit: the cube vanished, its moment was cancelled, no question was
// asked, and the application went on running with the Data Hub alone (#133).
// The Quit menu never had the problem: it closes the main window, whose
// closeEvent asks and then exits without closing anything.
//
// So the spontaneous Quit is taken here before any window hears of it,
// ignored (the system's request is declined), and the Quit menu's path is
// started instead. QCoreApplication::exit() sends no Quit event and passes.

#include <QCoreApplication>
#include <QEvent>
#include <QObject>

#include <functional>

namespace visivo {

/// Whether an event sent to the application object is the system's quit,
/// to be redirected: a Quit the platform sent (spontaneous). One a program
/// sends itself (QCoreApplication::quit) keeps Qt's own handling.
inline bool redirectsSystemQuit(QEvent::Type type, bool spontaneous)
{
    return type == QEvent::Quit && spontaneous;
}

/// Installed on the application object: runs @p quitLikeTheMenu, queued,
/// instead of Qt's close-every-window quit.
class SystemQuitRedirect final : public QObject
{
public:
    explicit SystemQuitRedirect(std::function<void()> quitLikeTheMenu, QObject *parent = nullptr)
        : QObject(parent), m_quit(std::move(quitLikeTheMenu))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == QCoreApplication::instance()
            && redirectsSystemQuit(event->type(), event->spontaneous())) {
            // Declined: the platform does not terminate the process itself,
            // and Qt closes no window. The question comes next, from the
            // event loop (not inside the platform's termination callback).
            event->ignore();
            if (!m_pending && m_quit) {
                m_pending = true;
                QMetaObject::invokeMethod(
                        this,
                        [this]() {
                            m_quit();   // asks; a quit asked meanwhile finds it pending
                            m_pending = false;
                        },
                        Qt::QueuedConnection);
            }
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_quit;
    bool m_pending{ false };   ///< a second Dock Quit while the first asks: one question
};

}   // namespace visivo

#endif   // SYSTEM_QUIT_REDIRECT_H
