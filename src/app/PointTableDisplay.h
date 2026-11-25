#ifndef POINT_TABLE_DISPLAY_H
#define POINT_TABLE_DISPLAY_H
// PointTableDisplay.h
// What the 3-D points window says about a table of positions (issue #563):
// the unit its axes are in, which column it opens coloured by, and the range
// of a colour bar over a column that does not vary. The window drew "X (Mpc)"
// over a RAMSES output whose ticks ran over the box (0..1), coloured by a mass
// every dark-matter particle shares, with a bar from -0.5 to 0.5.
//
// The positions are drawn as stored: this window has no physical-units switch
// (the cube's Inspector has), so an axis names the unit the file's column is
// stored in, as the backend's `component.units` says it. No widgets, no VTK:
// tests/test_point_table_display.cpp.

#include <QString>
#include <QtGlobal>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <optional>

namespace visivo::pointtable {

/// A stored unit as it reads on an axis: the note after it left out, so the
/// RAMSES "box units (fraction of the box)" is "box units" and a Gadget
/// "kpc/h comoving" stays whole.
inline QString shortUnit(const QString &stored)
{
    QString unit = stored.trimmed();
    const qsizetype note = unit.indexOf(QStringLiteral(" ("));
    if (note > 0 && unit.endsWith(u')'))
        unit.truncate(note);
    return unit.trimmed();
}

/// The title of an axis: "X (kpc/h comoving)" over a column whose unit the
/// table states; the table's own unit always wins. A position the window
/// computes (a sky catalogue's distance, a table that states nothing, a VBT)
/// is in Mpc, as it always was: @p undeclared. A column mapped onto an axis
/// that states no unit has none to name: a bare letter.
inline QString axisTitle(const QString &letter, const QString &storedUnit,
                         const QString &undeclared)
{
    const QString unit = shortUnit(storedUnit);
    if (!unit.isEmpty())
        return QStringLiteral("%1 (%2)").arg(letter, unit);
    return undeclared.isEmpty() ? letter : QStringLiteral("%1 (%2)").arg(letter, undeclared);
}

// ── The default colour column ────────────────────────────────────────────────

/// What the rows loaded say of a column: no value yet (a table's other columns
/// arrive after the first paint), one value throughout, or more than one.
enum class Spread { Unknown, Constant, Varies };

/// Identifiers and flags: a colour by them says nothing, so a default is never
/// one of them (the user can still choose them).
inline bool isLabelColumn(const QString &name)
{
    static const QStringList labels{ QStringLiteral("id"), QStringLiteral("tag"),
                                     QStringLiteral("family") };
    return labels.contains(name.trimmed(), Qt::CaseInsensitive);
}

struct ColourChoice
{
    int index{ -1 };         ///< into the candidates; -1: colour by nothing
    bool pending{ false };   ///< nothing known to vary yet: ask again when the other columns arrive
};

/// The column a table of points opens coloured by. @p candidates are the
/// column names it may use (positions and velocity components left out), in
/// table order; @p preferred names the quantities worth showing first, best
/// first; @p spread answers a candidate's Spread by its index, and is asked
/// only when needed. The first of those, or the table's first column, is kept
/// as it always was; only when it is constant (a mass every dark-matter
/// particle shares) is the next quantity that varies taken, else none, which
/// is asked again once the other columns are loaded when some are unknown.
template <typename SpreadOf>
inline ColourChoice chooseDefaultColour(const QStringList &candidates,
                                        const QStringList &preferred, SpreadOf spread)
{
    int first = -1, rank = static_cast<int>(preferred.size());
    for (int i = 0; i < candidates.size(); ++i) {
        for (int p = 0; p < rank; ++p) {
            if (candidates.at(i).compare(preferred.at(p), Qt::CaseInsensitive) == 0) {
                first = i;
                rank = p;
                break;
            }
        }
    }
    if (first < 0 && !candidates.isEmpty())
        first = 0;
    if (first < 0)
        return {};
    if (spread(first) != Spread::Constant)
        return { first, false };

    // The kept one does not vary: the preferred quantities that remain, then
    // the others in table order.
    QList<int> order;
    for (const QString &name : preferred)
        for (int i = 0; i < candidates.size(); ++i)
            if (candidates.at(i).compare(name, Qt::CaseInsensitive) == 0 && !order.contains(i))
                order << i;
    for (int i = 0; i < candidates.size(); ++i)
        if (!order.contains(i))
            order << i;
    ColourChoice choice;
    for (const int i : order) {
        if (i == first || isLabelColumn(candidates.at(i)))
            continue;
        const Spread s = spread(i);
        if (s == Spread::Varies)
            return { i, false };
        choice.pending = choice.pending || s == Spread::Unknown;
    }
    return choice;
}

// ── A colour range over a column that does not vary ──────────────────────────

/// The range a colour scale is drawn over, and whether it is a made-up one.
struct ColourSpan
{
    double lo{ 0.0 };
    double hi{ 1.0 };
    std::optional<double> constant;   ///< the one value of a column that does not vary
};

/// The span of a column from its smallest and largest value. One that does not
/// vary has a range made around its value so a colour scale exists (the value
/// takes the middle colour, and a Log scale stays positive for a positive
/// one), but it is reported as constant: the bar shows the one value, not the
/// made-up ends.
inline ColourSpan colourSpan(double lo, double hi)
{
    if (lo < hi)
        return { lo, hi, std::nullopt };
    const double pad = lo != 0.0 ? std::abs(lo) * 0.5 : 0.5;
    return { lo - pad, hi + pad, lo };
}

/// The title of a bar over a column that does not vary, in the 3-D view and
/// over a ray-traced picture alike.
inline QString constantBarTitle(const QString &column)
{
    return column + QStringLiteral(" (constant)");
}

/// Where along a colour table over [lo, hi] a value falls (0..1), on a log
/// scale when @p log and the range is positive: the colour a constant column's
/// points have, which the bar of such a column shows alone.
inline double fractionAlong(double value, double lo, double hi, bool log)
{
    if (!(hi > lo))
        return 0.5;
    const double f = log && lo > 0.0 && value > 0.0
            ? std::log(value / lo) / std::log(hi / lo)
            : (value - lo) / (hi - lo);
    return std::clamp(f, 0.0, 1.0);
}

// ── Where the points are drawn (issue #570) ──────────────────────────────────

/// View ▸ Rendering.
enum class RenderChoice { Auto, Local, Remote };

struct RenderFacts
{
    RenderChoice choice{ RenderChoice::Auto };
    bool backendRenders{ false };     ///< the open said it can draw points server-side
    bool backendIsLocal{ false };     ///< this machine's own backend: its GPU is this one
    qint64 tableRows{ 0 };            ///< rows of the table, filters applied
    qint64 loadedRows{ 0 };           ///< rows this window holds
    bool fitsHere{ true };            ///< every row would fit in this window's memory
};

struct RenderPlan
{
    bool remote{ false };             ///< streamed from the backend
    bool loadAll{ false };            ///< Local was asked for: bring the rest of the rows here
    QString note;                     ///< why the choice was not followed, "" when it was
};

/// What each choice does for a table of points. Local draws in the client,
/// every row of it: the rows still on the backend are loaded first (loadAll),
/// as "Load all" does. When they do not fit, a backend that can draw them
/// keeps the view (and says why), and this machine's own backend draws the rows
/// loaded. Remote and Auto never leave a machine's own backend: it is drawn
/// here, never mixed.
inline RenderPlan planRender(const RenderFacts &f)
{
    RenderPlan plan;
    const bool partial = f.tableRows > f.loadedRows;
    const bool canRemote = f.backendRenders && !f.backendIsLocal;
    switch (f.choice) {
    case RenderChoice::Remote:
        plan.remote = canRemote;
        if (!canRemote)
            plan.note = !f.backendRenders
                    ? QStringLiteral("Only a backend that renders server-side (EGL) can draw "
                                     "the points: showing the loaded rows.")
                    : QStringLiteral("This is the local backend: its points are drawn on this "
                                     "machine.");
        break;
    case RenderChoice::Auto:
        plan.remote = canRemote && partial;
        break;
    case RenderChoice::Local:
        if (!partial)
            break;
        if (f.fitsHere) {
            plan.loadAll = true;
        } else if (canRemote) {
            plan.remote = true;
            plan.note = QStringLiteral(
                    "All %L1 rows are too many to hold in this window: they stay drawn on "
                    "the backend.").arg(f.tableRows);
        } else {
            plan.note = QStringLiteral(
                    "All %L1 rows are too many to hold in this window: drawing the %L2 "
                    "loaded.").arg(f.tableRows).arg(f.loadedRows);
        }
        break;
    }
    return plan;
}

}   // namespace visivo::pointtable

#endif   // POINT_TABLE_DISPLAY_H
