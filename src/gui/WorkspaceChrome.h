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

#include <functional>

class QMainWindow;
class QLabel;
class QToolButton;
class QDockWidget;
class QVBoxLayout;
class QStackedWidget;
class QWidget;
class MainWindow;
class SessionDataTree;
class InspectorPanel;
class DockRail;

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

    /// Add a widget to the left column's LAYERS block, below the product tree
    /// (design #1d: the view/rendering controls live here, not in the Inspector).
    void addSessionLayers(QWidget *w);

    /// Create the right "Inspector" dock (Properties / Analysis / Provenance +
    /// cloud chip). Returns the panel so the window can populate its tabs and
    /// wire the pane-layout / Linked controls. Cloud chip is fed by the poll.
    InspectorPanel *installInspectorDock();

    /// The inspector dock (for wiring a show/hide toggle). Null until installed.
    QDockWidget *inspectorDock() const { return m_inspectorDock; }

    // ── Collapsible rails (§4.1) ──────────────────────────────────────────────
    // Each dock is two-state: full content (272 / 320 px) or a 36 px rail. Never
    // 0, never hidden. Only the user's click, focus mode, or a restored setting
    // collapses — never auto-collapse.
    void setSessionCollapsed(bool collapsed);
    void setInspectorCollapsed(bool collapsed);
    bool sessionCollapsed() const { return m_sessionCollapsed; }
    bool inspectorCollapsed() const { return m_inspectorCollapsed; }
    /// Expand the Inspector + select a tab. Called by the rail entries and by the
    /// auto-expand triggers (tool launch → Analysis, product → Provenance).
    void revealInspectorTab(int index);
    /// Left-rail product count (fed by the window from ProductRegistry).
    void setSessionRailCount(int count);

    /// Select a product row in the Session tree → drives Inspector › Provenance.
    void selectSessionProduct(const QString &productId);

    /// Focus mode thins the status rail to health + queue (the two that change);
    /// hides the cache + session slots. @p compact = true while focus is active.
    void setStatusRailCompact(bool compact);

    /// Begin polling backend health (safe no-op if no backend URL).
    void start();

    /// Snapshot returned by the health worker (backend health + latency).
    struct HealthSnapshot;

signals:
    void dockCollapseChanged();   ///< so the window can re-fit the 2-D panes

private:
    MainWindow *mainWindow() const;
    void poll();
    void onPollFinished();
    /// Custom dock title bar (label + collapse glyph). Lets the header hide cleanly.
    QWidget *makeDockTitleBar(const QString &title, const QString &glyph,
                              const std::function<void()> &onToggle);
    /// Swap the stack page + pin the dock width to rail (36) or open width. Also
    /// relaxes page 0's pinned min/max while hidden (QStackedWidget::minimumSizeHint
    /// is the max over ALL pages, so a pinned page-0 would keep the rail wide).
    void applyDockWidth(QDockWidget *dock, QStackedWidget *stack, bool collapsed, int openWidth);
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
    QVBoxLayout *m_sessionLayersLayout = nullptr;   ///< view/rendering controls below the tree
    QPointer<QDockWidget> m_inspectorDock;
    QPointer<InspectorPanel> m_inspector;
    QPointer<QLabel> m_cloudChip;   // inspector cloud chip, fed by the poll

    // Collapsible-rail state (§4.1).
    QPointer<QStackedWidget> m_sessionStack;    ///< page 0 content, page 1 rail
    QPointer<QStackedWidget> m_inspectorStack;
    QPointer<DockRail> m_sessionRail;
    QPointer<DockRail> m_inspectorRail;
    bool m_sessionCollapsed = false;
    bool m_inspectorCollapsed = false;

    QTimer m_timer;
    QFutureWatcher<HealthSnapshot> *m_watcher = nullptr;
    mutable QPointer<MainWindow> m_mainWindow;   // cached lookup
};

#endif // WORKSPACECHROME_H
