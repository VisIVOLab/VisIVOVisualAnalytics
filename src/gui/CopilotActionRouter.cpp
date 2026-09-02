#include "CopilotActionRouter.h"

using namespace Qt::StringLiterals;

CopilotActionRouter *CopilotActionRouter::instance()
{
    static CopilotActionRouter *s_instance = new CopilotActionRouter();
    return s_instance;
}

void CopilotActionRouter::registerTarget(const QString &datasetId, QObject *owner,
                                         ApplyFn apply, UndoFn undo)
{
    if (datasetId.isEmpty() || !owner)
        return;
    m_targets.insert(datasetId, Target{ owner, std::move(apply), std::move(undo) });
    // Auto-drop when the viewer is destroyed so a directive can't reach a dead
    // window. Guard against a lifetime race: only remove if THIS owner still owns
    // the id (it may have been replaced by a re-registration for the same id).
    connect(owner, &QObject::destroyed, this, [this, datasetId, owner]() {
        auto it = m_targets.find(datasetId);
        if (it != m_targets.end() && it->owner == owner)
            m_targets.erase(it);
        m_lastTargets.removeAll(datasetId);
    });
}

void CopilotActionRouter::unregisterTarget(const QString &datasetId)
{
    m_targets.remove(datasetId);
    m_lastTargets.removeAll(datasetId);
}

QStringList CopilotActionRouter::dispatch(const QList<QJsonObject> &actions,
                                          const QString &defaultTarget)
{
    // Group directives by their target view, preserving order within each group.
    QHash<QString, QList<QJsonObject>> byTarget;
    QStringList order;   // targets in first-seen order
    for (const QJsonObject &a : actions) {
        QString target = a.value(u"target"_s).toString();
        if (target.isEmpty())
            target = defaultTarget;
        if (target.isEmpty())
            continue;
        if (!order.contains(target))
            order.append(target);
        byTarget[target].append(a);
    }

    QStringList dispatched;
    for (const QString &target : order) {
        auto it = m_targets.constFind(target);
        if (it == m_targets.constEnd() || !it->apply)
            continue;   // target view isn't open — drop its directives
        it->apply(byTarget.value(target));
        dispatched.append(target);
    }
    m_lastTargets = dispatched;
    return dispatched;
}

void CopilotActionRouter::undoLast()
{
    for (const QString &target : m_lastTargets) {
        auto it = m_targets.constFind(target);
        if (it != m_targets.constEnd() && it->undo)
            it->undo();
    }
}
