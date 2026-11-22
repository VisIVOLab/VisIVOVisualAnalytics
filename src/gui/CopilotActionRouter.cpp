#include "CopilotActionRouter.h"

using namespace Qt::StringLiterals;

CopilotActionRouter *CopilotActionRouter::instance()
{
    static CopilotActionRouter *s_instance = new CopilotActionRouter();
    return s_instance;
}

void CopilotActionRouter::registerTarget(const QString &datasetId, QObject *owner,
                                         ApplyFn apply, UndoFn undo, const QString &routeKey)
{
    if (datasetId.isEmpty() || !owner)
        return;
    const QString key = routeKey.isEmpty() ? datasetId : routeKey;
    m_targets.insert(key, Target{ owner, datasetId, std::move(apply), std::move(undo),
                                  ++m_registrations });
    // Auto-drop when the viewer is destroyed so a directive can't reach a dead
    // window. Guard against a lifetime race: only remove if THIS owner still owns
    // the key (it may have been replaced by a re-registration for the same key).
    connect(owner, &QObject::destroyed, this, [this, key, owner]() {
        auto it = m_targets.find(key);
        if (it != m_targets.end() && it->owner == owner)
            m_targets.erase(it);
        m_lastTargets.removeAll(key);
    });
}

void CopilotActionRouter::unregisterTarget(const QString &datasetId)
{
    m_targets.remove(datasetId);
    m_lastTargets.removeAll(datasetId);
}

QHash<QString, CopilotActionRouter::Target>::const_iterator
CopilotActionRouter::find(const QString &key) const
{
    auto it = m_targets.constFind(key);
    if (it != m_targets.constEnd())
        return it;
    // A directive that names a dataset id, while its views registered under
    // keys of their own: the latest of them, as before route keys existed.
    auto best = m_targets.constEnd();
    for (auto t = m_targets.constBegin(); t != m_targets.constEnd(); ++t) {
        if (t->datasetId == key && (best == m_targets.constEnd() || t->order > best->order))
            best = t;
    }
    return best;
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
    QStringList touched;   // the keys the targets resolved to, for Undo
    for (const QString &target : order) {
        auto it = find(target);
        if (it == m_targets.constEnd() || !it->apply)
            continue;   // target view isn't open — drop its directives
        const ApplyFn apply = it->apply;
        touched.append(it.key());
        apply(byTarget.value(target));
        dispatched.append(target);
    }
    m_lastTargets = touched;
    return dispatched;
}

void CopilotActionRouter::undoLast()
{
    // Exactly the views the last dispatch reached: a dataset id resolved then
    // may name another view now.
    for (const QString &key : m_lastTargets) {
        auto it = m_targets.constFind(key);
        if (it != m_targets.constEnd() && it->undo)
            it->undo();
    }
}
