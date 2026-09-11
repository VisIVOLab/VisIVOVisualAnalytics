#ifndef MomentMapController_h
#define MomentMapController_h

// ---------------------------------------------------------------------------
// MomentMapController
//
// Owns the full moment-map computation flow extracted from vtkWindowCube:
//   1. Open the "Moment Map Settings" dialog (config selection)
//   2. Dispatch async computeMomentMap() via QFutureWatcher
//   3. On completion: call onRunFinished() so the cube window can
//      re-enable moment actions, then call onCompleted() with the result
//      and pre-built provenance strings for the cube window to apply
//
// Dependencies injected at construction time:
//   - dataset/backend context (filepath, backendUrl, datasetId, …)
//   - cube spectral depth (for channel-range pre-fill in the dialog)
//   - initial threshold value (used as default mask threshold)
//   - parent widget (dialog parent + status-bar access)
//   - onRunFinished callback: called both immediately after dispatch starts
//     (to refresh action availability) and when the watcher finishes
//   - onCompleted callback: called on success with the result, the config
//     that was used, and pre-built provenance summary/details strings;
//     vtkWindowCube uses these to apply the rendering and update the label
//
// vtkWindowCube retains:
//   - applyMomentMapResult()   (VTK rendering – too entangled to move)
//   - updateMomentProvenancePanel()  (reads a QLabel pointer)
//   - setMomentActionsEnabled() / updateBackendDerivedActionsAvailability()
//   - showPersistentStatusMessage() / clearPersistentStatusMessage()
//   - setMomentOrder() as a thin wrapper (adds the cross-system isBusy guard)
// ---------------------------------------------------------------------------

#include "MomentMapComputeTask.h"

#include <QObject>
#include <QPointer>

#include <functional>

class QWidget;
class QProgressDialog;

class MomentMapController : public QObject
{
    Q_OBJECT

public:
    // Mirrors the old MomentGenerationConfig; lives here now so the
    // callback signature doesn't pull in the full vtkWindowCube header.
    struct Config
    {
        int order{ 0 };
        int channelStart{ 0 };
        int channelEnd{ 0 };
        // Mask ON with an auto (~3·σ) threshold by default: M1/M2 need masking
        // to be physically clean, and CASA/SoFiA mask by default too.
        bool maskEnabled{ true };
        double thresholdValue{ 0.0 };
        bool thresholdAuto{ true };
    };

    // onRunFinished – called when the watcher transitions busy state:
    //   • once just after dispatch begins  (so actions are disabled immediately)
    //   • once when the watcher finishes   (so actions are re-enabled)
    //
    // onCompleted – called on successful completion with:
    //   result           – the full MomentMapComputeResult from the worker
    //   config           – the Config that was passed to the worker
    //   provenanceSummary – short one-liner for the status-bar label
    //   provenanceDetails – full multi-line tooltip string
    //   (note: vtkWindowCube appends spectralAxisTitle/wcsFrameLabel to details)
    explicit MomentMapController(
            const QString &filepath,
            const QString &backendUrl,
            const QString &datasetId,
            const QString &sessionId,
            const QString &backendToken,
            int depth,
            double initialThreshold,
            QWidget *parentWindow,
            std::function<void()> onRunFinished,
            std::function<void(const MomentMapComputeResult &result,
                               const Config &config,
                               const QString &provenanceSummary,
                               const QString &provenanceDetails)> onCompleted,
            QObject *parent = nullptr);

    // True while an async moment computation is in progress.
    // Queried by vtkWindowCube::isBusy() and updateBackendDerivedActionsAvailability().
    bool isBusy() const;

    // Returns the configuration that was accepted in the last successful dialog.
    // Valid only after startComputation() returns with isBusy() == true.
    const Config &lastAcceptedConfig() const { return m_currentConfig; }

    // Error message from the most recent failed computation.
    // Empty if the last computation succeeded or none has run yet.
    const QString &lastError() const { return m_lastError; }

    // Stateless provenance-string formatters (used by the Inspector › Provenance
    // params too, so exposed publicly — §3.0b).
    static QString describeMomentOrder(int order);
    static QString describeMomentScope();

public slots:
    // Opens the dialog with defaultOrder pre-selected; if accepted, dispatches
    // the async computation.  Re-entrancy-safe: no-op if already running.
    void startComputation(int defaultOrder);

    // Compute + display a moment map WITHOUT the dialog, from an explicit Config
    // (the scientific copilot's compute_moment action). Channels are clamped to
    // the cube depth; an empty range means the full cube. Same worker + product
    // registration path as the interactive flow.
    void startComputationHeadless(Config config);

private:
    void dispatch(const Config &config);   ///< async compute for a resolved Config (shared)
    bool configureMomentDialog(int defaultOrder, Config &config, bool restoredFromLastSession);
    static QString formatMomentChannelRange(const Config &config);

    const QString m_filepath;
    const QString m_backendUrl;
    const QString m_datasetId;
    const QString m_sessionId;
    const QString m_backendToken;
    const int m_depth;
    QWidget *m_parentWindow;
    std::function<void()> m_onRunFinished;
    std::function<void(const MomentMapComputeResult &,
                       const Config &,
                       const QString &,
                       const QString &)> m_onCompleted;

    Config m_currentConfig;
    int m_currentRequestId{ 0 };
    bool m_busy{ false };
    /// The window-modal progress sheet for the INTERACTIVE path only (the
    /// copilot's headless dispatch leaves it null). See ToolProgressSheet.h.
    QPointer<QProgressDialog> m_progressSheet;
    QString m_lastError;
};

#endif // MomentMapController_h
