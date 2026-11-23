#pragma once

// CatalogueSelectionState.h
// Two pieces of a 3-D catalogue window's selection bookkeeping (issue #448):
//
// - Named 3-D regions: each gets its own default name ("Region 1",
//   "Region 2", ...) and a name already in use is refused, so a region can be
//   told apart in the filter list, the diagnostics and the backend's labels.
// - Applied filters: the filter list is edited freely, but the rows on
//   screen, the next page and the diagnostics follow what was last applied.
//   Apply makes the edited list the applied one; a failed query puts the
//   previous one back.
//
// Header-only, no widgets, so it can be tested.

#include "app/BackendClient.h"

#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace visivo::selection {

/// Names compare trimmed and without case: "Halo A" and " halo a" are one.
inline QString nameKey(const QString &name)
{
    return name.trimmed().toCaseFolded();
}

/// The names of the 3-D regions in @p filters, skipping @p exceptIndex (the
/// region being edited, whose own name is not a clash).
inline QStringList regionNames(const std::vector<BackendTabularFilter> &filters,
                               int exceptIndex = -1)
{
    QStringList names;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (static_cast<int>(i) == exceptIndex || !filters[i].isSpatial())
            continue;
        if (!filters[i].name.trimmed().isEmpty())
            names << filters[i].name;
    }
    return names;
}

/// Whether @p name is already one of @p taken. An empty name is never a
/// clash: the list then shows the shape ("Box selection").
inline bool isTakenName(const QString &name, const QStringList &taken)
{
    const QString key = nameKey(name);
    if (key.isEmpty())
        return false;
    for (const QString &other : taken)
        if (nameKey(other) == key)
            return true;
    return false;
}

/// The first "Region N" (N from 1) that is not in @p taken.
inline QString nextRegionName(const QStringList &taken)
{
    for (int n = 1;; ++n) {
        const QString candidate = QStringLiteral("Region %1").arg(n);
        if (!isTakenName(candidate, taken))
            return candidate;
    }
}

inline bool sameFilter(const BackendTabularFilter &a, const BackendTabularFilter &b)
{
    return a.field == b.field && a.op == b.op && a.value == b.value && a.shape == b.shape
            && a.name == b.name && a.fields == b.fields && a.minimum == b.minimum
            && a.maximum == b.maximum && a.centre == b.centre && a.radius == b.radius;
}

inline bool sameFilters(const std::vector<BackendTabularFilter> &a,
                        const std::vector<BackendTabularFilter> &b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!sameFilter(a[i], b[i]))
            return false;
    return true;
}

/// @p filters with ``field op value`` as the only condition on that column:
/// its other conditions (type = 4 with type = 0) would leave nothing, and are
/// replaced. Spatial selections are kept, in their order.
inline std::vector<BackendTabularFilter> withOnlyCondition(
        const std::vector<BackendTabularFilter> &filters, const QString &field, const QString &op,
        const QString &value)
{
    std::vector<BackendTabularFilter> out;
    out.reserve(filters.size() + 1);
    for (const BackendTabularFilter &f : filters)
        if (f.isSpatial() || f.field != field)
            out.push_back(f);
    BackendTabularFilter condition;
    condition.field = field;
    condition.op = op;
    condition.value = value;
    out.push_back(condition);
    return out;
}

/// The filters the rows on screen were queried with.
class AppliedFilters
{
public:
    const std::vector<BackendTabularFilter> &filters() const { return m_applied; }

    /// Apply was pressed with @p edited: it is the applied list from now on
    /// (the Remote view is sent it at once). True when it differs from the
    /// list applied before. The window refuses Apply while a query is out,
    /// so there is never one pending here. A query of the applied list
    /// itself (Load all) does not begin: succeed and fail are then no-ops.
    bool begin(const std::vector<BackendTabularFilter> &edited)
    {
        m_previous = m_applied;
        m_applied = edited;
        m_pending = true;
        return !sameFilters(m_previous, m_applied);
    }

    /// The query answered with rows: nothing to undo any more. True when the
    /// rows are now filtered differently from before (what a view of the
    /// population, such as Selection Diagnostics, must follow).
    bool succeed()
    {
        if (!m_pending)
            return false;
        m_pending = false;
        const bool changed = !sameFilters(m_previous, m_applied);
        m_previous.clear();
        return changed;
    }

    /// The query failed: the rows on screen are still the previous list's.
    /// True when that changed the applied list back.
    bool fail()
    {
        if (!m_pending)
            return false;
        m_pending = false;
        const bool changed = !sameFilters(m_previous, m_applied);
        m_applied = std::move(m_previous);
        m_previous.clear();
        return changed;
    }

    /// The list the rows on screen were actually queried with: while an
    /// Apply is out, still the one before it (it may yet fail).
    const std::vector<BackendTabularFilter> &settled() const
    {
        return m_pending ? m_previous : m_applied;
    }

    /// A window opened on another's rows (Compare Quantity) starts from its
    /// settled list, the one its copied rows were queried with.
    void adopt(const std::vector<BackendTabularFilter> &applied)
    {
        m_applied = applied;
        m_previous.clear();
        m_pending = false;
    }

private:
    std::vector<BackendTabularFilter> m_applied;
    std::vector<BackendTabularFilter> m_previous;
    bool m_pending = false;
};

} // namespace visivo::selection
