#ifndef WORKSPACECHROME_H
#define WORKSPACECHROME_H

// WorkspaceChrome — shared "chrome" wiring for the viewer windows (Design 1d,
// tappa 1). One instance per viewer window (cube / image). It:
//   • wires the command-bar widgets built by VisivoTheme::makeViewerToolbar
//     (⌘K palette, recent-dataset switcher, theme toggle, live remote chip);
//   • installs an *additive* bottom status rail with four fixed slots
//     (health · queue · cache · session) plus a ⌥J "Jobs panel" shortcut;
//   • polls BackendClient::health() on a timer (off the GUI thread) and feeds
//     both the command-bar chip and the status-rail slots.
//
// It keeps the per-window setup identical and avoids duplicating this logic in
// vtkWindowCube and vtkWindowImage.

#include "theme/VisivoTheme.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QFutureWatcher>

class QMainWindow;
class QLabel;
class QToolButton;
class QDockWidget;
class MainWindow;
class SessionDataTree;
class InspectorPanel;

class WorkspaceChrome : public QObject
{
    Q_OBJECT
public:
    WorkspaceChrome(QMainWindow *window,
                    QString backendUrl,
                    QString backendToken,
                    QString sessionId,
                    QObject *parent = nullptr);

    /// Wire the interactive command-bar widgets (may hold nulls; each is
    /// wired only if present). Also registers the live remote/health chip.
    void attachCommandBar(const VisivoTheme::ViewerToolbarWidgets &widgets);

    /// Build and install the additive status rail on the window's status bar
    /// (four fixed slots + a clickable ⌥J "Jobs panel" affordance).
    void installStatusRail();

    /// Create the left "Session Data" dock (product tree) and seed it with the
    /// current dataset. @p datasetKey is the stable FITS filepath (parent key).
    void installSessionDock(const QString &datasetKey, const QString &datasetLabel,
                            const QString &datasetKind);

    /// Create the right "Inspector" dock (Properties / Analysis / Provenance +
    /// cloud chip). Returns the panel so the window can populate its tabs and
    /// wire the pane-layout / Linked controls. Cloud chip is fed by the poll.
    InspectorPanel *installInspectorDock();

    /// Begin polling backend health (safe no-op if no backend URL).
    void start();

    /// Snapshot returned by the health worker (backend health + latency).
    struct HealthSnapshot;

private:
    MainWindow *mainWindow() const;
    void poll();
    void onPollFinished();
    /// Re-apply theme-token styles to the rail widgets after a dark⇄light flip
    /// (they bake token colours at build time and don't follow the global QSS).
    void restyleForTheme();

    QPointer<QMainWindow> m_window;
    QString m_url;
    QString m_token;
    QString m_session;

    // Command-bar chip (live "host · N running").
    QPointer<QLabel> m_remoteChip;
    // Status-rail fixed slots.
    QPointer<QLabel> m_healthSlot;
    QPointer<QLabel> m_queueSlot;
    QPointer<QLabel> m_cacheSlot;
    QPointer<QLabel> m_sessionSlot;
    QPointer<QToolButton> m_jobsBtn;

    QPointer<QDockWidget> m_sessionDock;
    QPointer<SessionDataTree> m_sessionTree;
    QPointer<QDockWidget> m_inspectorDock;
    QPointer<InspectorPanel> m_inspector;
    QPointer<QLabel> m_cloudChip;   // inspector cloud chip, fed by the poll

    QTimer m_timer;
    QFutureWatcher<HealthSnapshot> *m_watcher = nullptr;
    mutable QPointer<MainWindow> m_mainWindow;   // cached lookup
};

#endif // WORKSPACECHROME_H
