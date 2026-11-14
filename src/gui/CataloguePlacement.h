#ifndef CataloguePlacement_h
#define CataloguePlacement_h

// CataloguePlacement.h
// What the 3-D catalogue window says about where its sources are (issue #265):
// the status line that counts the placed, the unplaced (no distance) and the
// uncertain ones (placed from cz < 1000 km/s), the rows of the "Unplaced
// sources" table, and the pick readout's "Distance …, from …" line.
//
// Widget-free and header-only: unit-tested in tests/test_catalogue_parser.cpp.

#include "Catalogue3DParser.h"

#include <QString>
#include <QStringList>

#include <cmath>
#include <vector>

namespace CataloguePlacement {

struct Counts
{
    qsizetype placed{ 0 };
    qsizetype unplaced{ 0 };
    qsizetype uncertain{ 0 };
};

/// The counts of a window's rows: @p entries placed, @p unplaced kept apart.
/// The uncertain flag depends on where each distance came from, so it is
/// worked out with the schema's current distance mode.
inline Counts count(const std::vector<Catalogue3DEntry> &entries,
                    const std::vector<Catalogue3DEntry> &unplaced,
                    const Catalogue3DSchema &schema)
{
    Counts c;
    c.placed = static_cast<qsizetype>(entries.size());
    c.unplaced = static_cast<qsizetype>(unplaced.size());
    if (schema.isCartesian())
        return c;
    const Catalogue3DParser::detail::DistanceFields fields(schema);
    for (const auto &e : entries)
        if (Catalogue3DParser::detail::entryDistance(e, schema, fields).uncertain())
            ++c.uncertain;
    return c;
}

/// "N placed · M unplaced (no distance) · K with cz < 1000 km/s (distance
/// uncertain: peculiar velocities)".
inline QString statusText(const Counts &c)
{
    return QStringLiteral("%L1 placed · %L2 unplaced (no distance) · %L3 with cz < 1000 km/s "
                          "(distance uncertain: peculiar velocities)")
            .arg(c.placed)
            .arg(c.unplaced)
            .arg(c.uncertain);
}

/// Where a distance came from, for the pick readout: "from z (comoving,
/// Planck18)", "from z (redshift space, H0 = 69.32)", "from column DIST".
/// @p model is the applied cosmology's name (a custom one described), used for
/// a distance the selector computed; one integrated locally is Planck18's.
inline QString originText(const Catalogue3DParser::detail::EntryDistance &d,
                          const QString &model, double h0)
{
    using Catalogue3DParser::detail::DistanceOrigin;
    switch (d.origin) {
    case DistanceOrigin::RedshiftSpace:
        return QStringLiteral("from z (redshift space, cz/H0 with H0 = %1)").arg(h0, 0, 'f', 2);
    case DistanceOrigin::Column:
        return QStringLiteral("from column %1").arg(d.column);
    case DistanceOrigin::Redshift:
        return QStringLiteral("from z (comoving, %1)")
                .arg(d.fromModel && !model.isEmpty() ? model : QStringLiteral("Planck18"));
    case DistanceOrigin::Cartesian:
        return QStringLiteral("from the catalogue's X/Y/Z");
    case DistanceOrigin::None:
        break;
    }
    return QStringLiteral("no distance");
}

/// The pick readout's lines: "Distance 12.3 Mpc, from …", and "cz < 1000 km/s:
/// uncertain" when that applies.
inline QStringList pickLines(const Catalogue3DParser::detail::EntryDistance &d,
                             const QString &model, double h0)
{
    QStringList out;
    if (!d.placed()) {
        out << QStringLiteral("No distance (%1)")
                        .arg(Catalogue3DParser::detail::unplacedReasonText(d.reason));
        return out;
    }
    out << QStringLiteral("Distance %1 Mpc, %2")
                    .arg(d.mpc, 0, 'f', 1)
                    .arg(originText(d, model, h0));
    if (d.uncertain())
        out << QStringLiteral("cz < 1000 km/s: uncertain");
    return out;
}

/// One row of the "Unplaced sources" table: Name, RA, Dec, cz [km/s], Reason.
struct UnplacedRow
{
    QString name;
    double raDeg{ 0.0 };
    double decDeg{ 0.0 };
    double czKms{ std::nan("") };   ///< NaN: no redshift
    QString reason;
};

inline std::vector<UnplacedRow> unplacedRows(const std::vector<Catalogue3DEntry> &unplaced,
                                             const Catalogue3DSchema &schema)
{
    std::vector<UnplacedRow> rows;
    rows.reserve(unplaced.size());
    const Catalogue3DParser::detail::DistanceFields fields(schema);
    for (const auto &e : unplaced) {
        const auto d = Catalogue3DParser::detail::entryDistance(e, schema, fields);
        UnplacedRow r;
        r.name = Catalogue3DParser::detail::entryDisplayName(e, schema);
        r.raDeg = e.raDeg;
        r.decDeg = e.decDeg;
        if (std::isfinite(d.redshift))
            r.czKms = Catalogue3DParser::detail::kSpeedOfLightKms * d.redshift;
        r.reason = Catalogue3DParser::detail::unplacedReasonText(d.reason);
        rows.push_back(r);
    }
    return rows;
}

}   // namespace CataloguePlacement

#endif // CataloguePlacement_h
