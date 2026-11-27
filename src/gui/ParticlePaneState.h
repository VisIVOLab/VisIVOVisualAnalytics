#ifndef PARTICLEPANESTATE_H
#define PARTICLEPANESTATE_H

// The state of the particle window's comparison panes that does not need a widget:
// which quantity the next pane takes, and the layout / quantity-per-pane / link flags
// that a session keeps. Header-only, QtCore only, so it is unit-tested without the
// window (the pane layout rules themselves are in PaneLayoutModel.h).

#include "PaneLayoutModel.h"

#include <array>

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace visivo::particlepanes {

/// The quantities a comparison proposes first: the three of the MVP (density,
/// temperature, entropy), then other physical scalars.
inline QStringList preferredQuantities()
{
    return { QStringLiteral("rho"),      QStringLiteral("density"),  QStringLiteral("temperature"),
             QStringLiteral("temp"),     QStringLiteral("entropy"),  QStringLiteral("k"),
             QStringLiteral("u"),        QStringLiteral("pressure"), QStringLiteral("metallicity"),
             QStringLiteral("mass"),     QStringLiteral("sfr"),      QStringLiteral("age") };
}

/// A streamed (Remote) pane that is not on screen gives its backend session back after
/// this long, so a layout change that only moves the view for a moment costs nothing.
constexpr int kPauseGraceMs = 2000;

/// Whether a streamed pane should give its session back: its view is not on screen
/// while the window that holds it is (a pane the layout hides), and it is a pane of a
/// grid at all (a window of its own that was minimised or closed keeps its stream).
constexpr bool pauseStream(bool inGrid, bool viewVisible, bool hostVisible)
{
    return inGrid && !viewVisible && hostVisible;
}

/// One colour choice of the window's "colour by" list.
struct QuantityChoice
{
    QString key;    ///< "raw:<column>" for a data column; others are built-in modes
    QString name;   ///< the column's name (raw columns only)
};

/// The item a new pane colours by: the first preferred quantity not already shown, else
/// the first other column. Positions and velocity components are not quantities to
/// compare. -1 when no column is left.
inline int nextQuantity(const QList<QuantityChoice> &choices, const QSet<QString> &usedKeys,
                        const QStringList &notQuantities,
                        const QStringList &preferred = preferredQuantities())
{
    const auto usable = [&](int i) {
        const QuantityChoice &c = choices.at(i);
        return c.key.startsWith(QStringLiteral("raw:")) && !usedKeys.contains(c.key)
                && !notQuantities.contains(c.name, Qt::CaseInsensitive);
    };
    for (const QString &wanted : preferred)
        for (int i = 0; i < choices.size(); ++i)
            if (usable(i) && choices.at(i).name.compare(wanted, Qt::CaseInsensitive) == 0)
                return i;
    for (int i = 0; i < choices.size(); ++i)
        if (usable(i))
            return i;
    return -1;
}

/// What a pane's filter answer needs to know about another pane of the grid.
struct PaneFilterLink
{
    bool present{ false };     ///< the slot holds a pane
    bool linked{ false };      ///< its camera, selection and filters follow the group
    bool sameTable{ false };   ///< it looks at the same backend dataset
};

/// The slots that take the rows a filter answered in @p source: the other linked panes of
/// the same table, whichever pane the filter was edited in. An unlinked source keeps its
/// filters to itself, and an unlinked pane keeps its own.
inline QList<int> filterTargets(int source, const std::array<PaneFilterLink, visivo::panes::kSlots> &panes)
{
    QList<int> out;
    if (source < 0 || source >= visivo::panes::kSlots || !panes[source].present
        || !panes[source].linked)
        return out;
    for (int i = 0; i < visivo::panes::kSlots; ++i)
        if (i != source && panes[i].present && panes[i].linked && panes[i].sameTable)
            out << i;
    return out;
}

/// What a session keeps of the panes: the layout, each pane's quantity (by column name,
/// which outlives the column order), whether it is linked to the group, and which pane
/// was active. Camera and filters are the group's, so they are not stored per pane.
struct PaneSession
{
    struct Pane
    {
        QString quantity;       ///< the column's name; empty: the pane's own default
        bool linked{ true };
    };
    int count{ 1 };
    int active{ 0 };
    QList<Pane> panes;          ///< one per slot in use, in slot order

    bool operator==(const PaneSession &o) const
    {
        if (count != o.count || active != o.active || panes.size() != o.panes.size())
            return false;
        for (int i = 0; i < panes.size(); ++i)
            if (panes[i].quantity != o.panes[i].quantity || panes[i].linked != o.panes[i].linked)
                return false;
        return true;
    }
};

/// One slot of the grid as a session sees it.
struct PaneSlotState
{
    bool present{ false };
    QString quantity;
    bool linked{ true };
};

/// The session of a grid: the HOST's pane first (a session is restored from the host's
/// window, whichever slot it sat in), then the others in slot order. @p active is a slot;
/// the session names it by its place in that order.
inline PaneSession makeSession(int count, int activeSlot, int hostSlot,
                               const std::array<PaneSlotState, visivo::panes::kSlots> &cells)
{
    PaneSession s;
    s.count = visivo::panes::normalisedCount(count);
    const auto add = [&](int slot) {
        if (slot == activeSlot)
            s.active = s.panes.size();
        s.panes.append({ cells[slot].quantity, cells[slot].linked });
    };
    if (hostSlot >= 0 && hostSlot < visivo::panes::kSlots && cells[hostSlot].present)
        add(hostSlot);
    for (int i = 0; i < visivo::panes::kSlots; ++i)
        if (cells[i].present && i != hostSlot)
            add(i);
    return s;
}

inline QJsonObject toJson(const PaneSession &s)
{
    QJsonArray panes;
    for (const PaneSession::Pane &p : s.panes)
        panes.append(QJsonObject{ { QStringLiteral("quantity"), p.quantity },
                                  { QStringLiteral("linked"), p.linked } });
    return { { QStringLiteral("version"), 1 },
             { QStringLiteral("count"), s.count },
             { QStringLiteral("active"), s.active },
             { QStringLiteral("panes"), panes } };
}

/// Tolerant: a malformed or foreign object reads as one pane with no quantity.
inline PaneSession fromJson(const QJsonObject &o)
{
    PaneSession s;
    const QJsonArray panes = o.value(QStringLiteral("panes")).toArray();
    for (const QJsonValue &v : panes) {
        if (s.panes.size() >= visivo::panes::kSlots)
            break;
        const QJsonObject p = v.toObject();
        s.panes.append({ p.value(QStringLiteral("quantity")).toString(),
                         p.value(QStringLiteral("linked")).toBool(true) });
    }
    // A layout holds at least as many panes as the list says and is 1, 2 or 4.
    int count = visivo::panes::normalisedCount(o.value(QStringLiteral("count")).toInt(1));
    while (count < s.panes.size() && visivo::panes::canGrow(count))
        count = visivo::panes::grownCount(count);
    s.count = count;
    while (s.panes.size() > s.count)
        s.panes.removeLast();
    s.active = qBound(0, o.value(QStringLiteral("active")).toInt(0), qMax(0, s.panes.size() - 1));
    return s;
}

} // namespace visivo::particlepanes

#endif // PARTICLEPANESTATE_H
