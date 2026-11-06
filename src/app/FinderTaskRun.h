#ifndef FINDERTASKRUN_H
#define FINDERTASKRUN_H

// FinderTaskRun — a source finder run as a task of a VisIVO backend
// (POST /v1/tasks/sourcefind), followed to its end on a worker thread:
// polled, cancellable, and given up when the backend stops answering. One
// loop for the two dialogs that run finders (the cube viewer's SoFiA-2 dialog
// and the SKAVA "source finder next to the data").

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>

class BackendClient;

namespace visivo {

struct FinderTaskOutcome
{
    bool valid{ false };
    /// "cancelled" when the user cancelled; otherwise the reason in words.
    QString error;
    bool cancelled{ false };
    /// HTTP status of the create request when the backend refused it (503:
    /// finder not installed, 422: wrong dataset kind, 429: busy); 0 otherwise.
    int refusedStatus{ 0 };
    QString taskId;
    /// The completed task's `result` (n_sources, sources, mask_path,
    /// catalog_path, log_path, summary for SoFiA-2).
    QJsonObject result;
};

/// Start @p finder on @p datasetId and follow the task. Runs on a worker:
/// @p log receives lines for the user, @p progress the task's 0..1 progress
/// (either may be empty); setting @p cancel cancels the task on the backend.
FinderTaskOutcome runFinderTask(const BackendClient &client, const QString &datasetId,
                                const QString &finder, const QJsonObject &params,
                                std::atomic_bool *cancel,
                                const std::function<void(const QString &)> &log,
                                const std::function<void(double)> &progress = {});

} // namespace visivo

#endif // FINDERTASKRUN_H
