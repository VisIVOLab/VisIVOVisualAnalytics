#ifndef NoiseController_h
#define NoiseController_h

// ---------------------------------------------------------------------------
// NoiseController
//
// Owns the full noise-estimation flow for a single cube viewer:
//   1. Open NoiseRegionDialog (modal, parent = cube window)
//   2. Validate dialog result
//   3. Dispatch async BackendClient::requestNoise() via QFutureWatcher
//   4. On completion: log via DiagnosticsManager, show result dialog,
//      update cube status bar, call back into vtkWindowCube for
//      action-availability refresh
//
// Dependencies injected at construction time:
//   - dataset identifiers (backendUrl, datasetId, sessionId, backendToken)
//   - cube spatial + spectral dimensions (for dialog pre-fill)
//   - parent widget (for dialog ownership and status-bar access)
//   - a callback invoked after every run completes (succeeded or failed)
//     so vtkWindowCube can refresh action availability
//
// vtkWindowCube retains:
//   - ownership (NoiseController is a member)
//   - the QAction* that triggers startEstimation()
//   - action enable/disable logic in updateBackendDerivedActionsAvailability()
//     which queries isBusy()
//
// ---------------------------------------------------------------------------

#include "CubeNoiseTypes.h"

#include <QObject>

#include <functional>

// Forward declarations
class QWidget;

// ---------------------------------------------------------------------------

class NoiseController : public QObject
{
    Q_OBJECT

public:
    // All dataset-level context is captured at construction time and never
    // changes during the lifetime of the controller.
    explicit NoiseController(
            // Backend context
            const QString &backendUrl,
            const QString &datasetId,
            const QString &sessionId,
            const QString &backendToken,
            // Cube dimensions (for dialog pre-fill)
            int width,
            int height,
            int depth,
            // UI parent (dialog parent + status bar)
            QWidget *parentWindow,
            // Called after every run (success or failure) so the cube window
            // can refresh action-availability.
            std::function<void()> onRunFinished,
            QObject *parent = nullptr);

    // Returns true while an async noise estimation is in progress.
    // vtkWindowCube queries this inside updateBackendDerivedActionsAvailability().
    bool isBusy() const;

public slots:
    // Triggered by QAction::triggered from vtkWindowCube.
    // Opens the region dialog; if accepted, launches the async estimation.
    void startEstimation();

private:
    void showResult(const CubeNoiseEstimateResult &result);

    const QString m_backendUrl;
    const QString m_datasetId;
    const QString m_sessionId;
    const QString m_backendToken;
    const int m_width;
    const int m_height;
    const int m_depth;
    QWidget *m_parentWindow;
    std::function<void()> m_onRunFinished;

    bool m_busy{ false };
};

#endif // NoiseController_h
