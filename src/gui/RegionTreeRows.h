#ifndef RegionTreeRows_h
#define RegionTreeRows_h

// RegionTreeRows.h
// What the Regions node of Session Data shows for a window's region list
// (issue #388). The list is the one source of truth; the tree is rebuilt from
// this description after every change, and every gesture in the tree goes
// back to the list as an edit (RegionSessionController).
//
// Regions read from one file sit under a group named after it ("hcg44.reg"),
// in the order of the list; regions drawn or typed in are rows of their own.
// A group's check box is ticked when all its regions are shown, cleared when
// none are, and partly ticked otherwise.
//
// Pure QtCore and header-only so it is unit-tested without a window
// (tests/test_region_model.cpp).

#include "RegionModel.h"

#include <QString>

#include <vector>

namespace visivo::regions {

enum class TreeCheck { Unchecked, Partial, Checked };

struct RegionTreeRow
{
    int id{ 0 };
    QString text;      ///< "src A · Circle", or "Box 1" when the name already says the shape
    QString tooltip;
    QString color;     ///< as in the region; empty: the viewer's default cyan
    bool visible{ true };
};

/// One top-level entry under Regions: a region of its own, or the group of
/// an import (`batch` > 0).
struct RegionTreeEntry
{
    int batch{ 0 };          ///< 0: a single region (rows has one element)
    QString title;           ///< a group's title: "hcg44.reg (12)", "hcg44.reg (12, 3 hidden)"
    std::vector<RegionTreeRow> rows;
    TreeCheck check{ TreeCheck::Checked };
};

struct RegionTreeView
{
    std::vector<RegionTreeEntry> entries;
    int selectedId{ -1 };
    int total{ 0 };
    int hidden{ 0 };
    TreeCheck check{ TreeCheck::Checked };   ///< of the Regions node itself

    bool isEmpty() const { return total == 0; }
    QString title() const
    {
        return hidden > 0 ? QStringLiteral("Regions (%1, %2 hidden)").arg(total).arg(hidden)
                          : QStringLiteral("Regions (%1)").arg(total);
    }
    /// The ids and groups in order: two views with the same signature have
    /// the same rows, so the tree can be updated in place (keeping what the
    /// user expanded and scrolled to) rather than rebuilt.
    QString signature() const
    {
        QString s;
        for (const RegionTreeEntry &e : entries) {
            s += e.batch > 0 ? QStringLiteral("g%1[").arg(e.batch) : QStringLiteral("[");
            for (const RegionTreeRow &r : e.rows)
                s += QString::number(r.id) + QLatin1Char(',');
            s += QLatin1Char(']');
        }
        return s;
    }
    /// The ids of the regions in group @p batch, or of every region when
    /// @p batch is negative (the Regions node).
    std::vector<int> idsOf(int batch) const
    {
        std::vector<int> ids;
        if (batch == 0)
            return ids;   // batch 0 is never a group
        for (const RegionTreeEntry &e : entries) {
            if (batch > 0 && e.batch != batch)
                continue;
            for (const RegionTreeRow &r : e.rows)
                ids.push_back(r.id);
        }
        return ids;
    }
};

inline TreeCheck checkOf(int shown, int count)
{
    return shown == 0 ? TreeCheck::Unchecked
                      : shown == count ? TreeCheck::Checked : TreeCheck::Partial;
}

/// The row of one region: its name, its shape when the name does not say it
/// already, and a tooltip with where it came from.
inline RegionTreeRow regionTreeRow(const Region &r)
{
    RegionTreeRow row;
    row.id = r.id;
    row.color = r.color;
    row.visible = r.visible;
    const QString shape = shapeLabel(r.shape);
    const QString name = r.name.isEmpty() ? shape : r.name;
    row.text = name.startsWith(shape, Qt::CaseInsensitive) ? name
                                                           : QStringLiteral("%1 · %2").arg(name, shape);
    QString tip = QStringLiteral("%1, %2").arg(name, shape.toLower());
    if (!r.sourceFile.isEmpty())
        tip += r.sourceLine > 0 ? QStringLiteral(", %1 line %2").arg(r.sourceFile).arg(r.sourceLine)
                                : QStringLiteral(", from %1").arg(r.sourceFile);
    if (!r.visible)
        tip += QStringLiteral(" (hidden)");
    row.tooltip = tip + QStringLiteral(". Double-click for its properties, right-click for more.");
    return row;
}

/// The Regions node of Session Data for @p list.
inline RegionTreeView buildRegionTree(const RegionList &list)
{
    RegionTreeView view;
    view.selectedId = list.selectedId();
    for (const Region &r : list.regions()) {
        ++view.total;
        if (!r.visible)
            ++view.hidden;
        // An import's regions join its group wherever they stand in the list;
        // the group stands where its first region does.
        if (r.batch > 0 && !r.sourceFile.isEmpty()) {
            auto it = std::find_if(view.entries.begin(), view.entries.end(),
                                   [&r](const RegionTreeEntry &e) { return e.batch == r.batch; });
            if (it == view.entries.end()) {
                RegionTreeEntry group;
                group.batch = r.batch;
                group.title = r.sourceFile;
                view.entries.push_back(group);
                it = view.entries.end() - 1;
            }
            it->rows.push_back(regionTreeRow(r));
            continue;
        }
        RegionTreeEntry single;
        single.rows.push_back(regionTreeRow(r));
        single.check = r.visible ? TreeCheck::Checked : TreeCheck::Unchecked;
        view.entries.push_back(single);
    }
    for (RegionTreeEntry &e : view.entries) {
        if (e.batch == 0)
            continue;
        const int shown = static_cast<int>(std::count_if(e.rows.begin(), e.rows.end(),
                                                         [](const RegionTreeRow &r) { return r.visible; }));
        e.check = checkOf(shown, static_cast<int>(e.rows.size()));
        const int hiddenHere = static_cast<int>(e.rows.size()) - shown;
        e.title = hiddenHere > 0
                ? QStringLiteral("%1 (%2, %3 hidden)").arg(e.title).arg(e.rows.size()).arg(hiddenHere)
                : QStringLiteral("%1 (%2)").arg(e.title).arg(e.rows.size());
    }
    view.check = checkOf(view.total - view.hidden, view.total);
    return view;
}

}   // namespace visivo::regions

#endif   // RegionTreeRows_h
