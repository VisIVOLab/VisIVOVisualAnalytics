#ifndef PvController_h
#define PvController_h

// ---------------------------------------------------------------------------
// PvController
//
// Owns the backend fetch + data decoding for a single PV (position-velocity)
// diagram extraction request:
//   1. Receive a polyline (vertices + width) from vtkWindowCube
//   2. Dispatch fetchRemotePv() off the UI thread via QFutureWatcher
//   3. On completion: call onRunFinished() so the cube window can re-enable
//      the PV action, then call onCompleted() with the RemotePvFetchResult
//      (success) or log + show errors directly (failure)
//
// Dependencies injected at construction time:
//   - backend context (backendUrl, datasetId, sessionId, backendToken)
//   - parent widget (for QMessageBox parent + status-bar access)
//   - onRunFinished: called immediately after dispatch starts (to disable
//     action) and again when the watcher finishes (to re-enable it)
//   - onCompleted: called only on success; vtkWindowCube uses the result to
//     build spectral axis values, provenance strings, and populate
//     PvDiagramWidget
//
// vtkWindowCube retains:
//   - all VTK overlay actors and interaction state (pvPolylineVertices,
//     pvWidthPixels, pvModeActive, pvDragging, pvValid, pvCursorValid)
//   - setPvModeActive(), finalizePvInteraction(), updatePvFromDisplayPosition(),
//     refreshPvOverlay(), clearPv()
//   - pvSampledPath(), pvLocalNormalForSample()   (used by refreshPvOverlay)
//   - PvDiagramWidget ownership and population
//   - extractCurrentPvDiagram() as a thin wrapper that validates state then
//     calls startExtraction()
// ---------------------------------------------------------------------------

#include "PvTypes.h"

#include <QObject>

#include <array>
#include <functional>
#include <vector>

class QWidget;

class PvController : public QObject
{
    Q_OBJECT

public:
    // onRunFinished – called when the watcher transitions busy state:
    //   • once just after dispatch begins  (so the PV action is disabled)
    //   • once when the watcher finishes   (so it can be re-enabled)
    //
    // onCompleted – called only on successful extraction with the fully
    //   decoded RemotePvFetchResult (including capturedVertices /
    //   capturedWidthPixels so provenance strings can be built without
    //   depending on shared mutable state in vtkWindowCube).
    explicit PvController(
            const QString &backendUrl,
            const QString &datasetId,
            const QString &sessionId,
            const QString &backendToken,
            QWidget *parentWindow,
            std::function<void()> onRunFinished,
            std::function<void(const RemotePvFetchResult &)> onCompleted,
            QObject *parent = nullptr);

    // True while an async PV extraction is in progress.
    // Queried by vtkWindowCube::isBusy() and
    // updateBackendDerivedActionsAvailability().
    bool isBusy() const;

public slots:
    // Dispatches the async fetch. While one runs, a new request is QUEUED and
    // replaces any request already queued: the latest path wins, so editing a
    // path live never piles up computations nor loses the final shape.
    // @p interactive: an error goes to the status bar instead of a message box
    // (a live edit must not throw a dialog at every intermediate shape).
    void startExtraction(const std::vector<std::array<double, 2>> &vertices,
                         int widthPixels, bool interactive = false);

private:
    const QString m_backendUrl;
    const QString m_datasetId;
    const QString m_sessionId;
    const QString m_backendToken;
    QWidget *m_parentWindow;
    std::function<void()> m_onRunFinished;
    std::function<void(const RemotePvFetchResult &)> m_onCompleted;

    bool m_busy{ false };
    struct Pending
    {
        std::vector<std::array<double, 2>> vertices;
        int widthPixels{ 1 };
        bool interactive{ false };
    };
    bool m_hasPending{ false };
    Pending m_pending;
};

#endif // PvController_h
