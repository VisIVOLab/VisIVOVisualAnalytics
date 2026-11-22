#ifndef CopilotActionRouter_h
#define CopilotActionRouter_h

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

/// Session-level router for scientific-copilot visualisation directives (Phase 2,
/// multi-dataset). Each open viewer registers its backend dataset id with the
/// functions that apply / undo copilot actions on it. A copilot answer's
/// directives are tagged with a ``target`` dataset id (the active view by
/// default), so the same copilot can act across several open views — e.g. compute
/// a moment on a cube AND recolour a 2-D image of the same field.
///
/// Registrations are auto-removed when the owning viewer is destroyed, so a
/// stale directive can never reach a deleted window.
class CopilotActionRouter : public QObject
{
    Q_OBJECT
public:
    using ApplyFn = std::function<void(const QList<QJsonObject> &)>;
    using UndoFn = std::function<void()>;

    static CopilotActionRouter *instance();

    /// Register (or replace) the target for @p datasetId. @p owner drives the
    /// lifetime: when it is destroyed the target is dropped automatically.
    /// @p routeKey (optional) registers the view under a key of its own, for
    /// views that share a dataset id (a quantity comparison's views): its
    /// panel dispatches with that key, and a directive naming the dataset id
    /// reaches the view of that dataset registered last.
    void registerTarget(const QString &datasetId, QObject *owner,
                        ApplyFn apply, UndoFn undo, const QString &routeKey = QString());
    void unregisterTarget(const QString &datasetId);

    /// Route @p actions to their target views. Each directive's ``target`` field
    /// selects the view; a directive without one uses @p defaultTarget (the panel
    /// that produced the answer). Directives whose target isn't open are dropped.
    /// Returns the set of targets (keys as given) that actually received a directive.
    QStringList dispatch(const QList<QJsonObject> &actions, const QString &defaultTarget);

    /// Undo the most recent dispatch (every view it touched).
    void undoLast();

private:
    explicit CopilotActionRouter(QObject *parent = nullptr) : QObject(parent) {}

    struct Target
    {
        QObject *owner{ nullptr };
        QString datasetId;
        ApplyFn apply;
        UndoFn undo;
        quint64 order{ 0 };   ///< registration order: the latest wins a dataset id
    };
    /// The target @p key names: a route key, a dataset id registered under
    /// itself, else the latest view registered for that dataset id.
    QHash<QString, Target>::const_iterator find(const QString &key) const;
    QHash<QString, Target> m_targets;
    quint64 m_registrations{ 0 };
    QStringList m_lastTargets;   ///< views touched by the last dispatch (for Undo)
};

#endif  // CopilotActionRouter_h
