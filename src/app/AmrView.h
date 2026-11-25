#pragma once

// AmrView.h
// The views of an AMR output (RAMSES and the other AmrHierarchy sources,
// issue #550) as pure logic: what the backend's plan says and how it is
// worded, the request bodies, the refinement-level map of a slice and how its
// rectangles land on a pixel grid, how a box of cube voxels becomes the region
// of a request, and how an AMR* header card tells a cube was made this way.
// The resampling is the backend's (app/compute/amr_views.py, POST
// /v1/simulations/amr/{plan,uniform,level-map}); the cube it writes opens like
// any volume. No widgets, no network: tests/test_amr_view.cpp.

#include "ByteSize.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace visivo::amrview {

/// A region of the run: two corners, in the unit the request names.
struct Region
{
    bool set{ false };
    std::array<double, 3> lo{ 0, 0, 0 };
    std::array<double, 3> hi{ 1, 1, 1 };
    QString unit{ QStringLiteral("box") };   ///< "box" (fraction of the box) or "physical"
};

/// What the backend would make (the reply of /plan, and the plan inside that of
/// /uniform).
struct Plan
{
    bool valid{ false };
    QString error;
    QString code;                  ///< the code that wrote the output ("ramses"...)
    QString field;
    int level{ 0 };
    QVector<int> shape;            ///< voxels along x, y, z
    qint64 voxels{ 0 };
    qint64 bytes{ 0 };             ///< of one field's cube
    qint64 peakMemoryBytes{ 0 };   ///< what the run holds at once
    double voxelSizePhysical{ 0 };
    QVector<double> lowerPhysical, upperPhysical;
    QString lengthUnit;
    QString valueUnit;
    bool fits{ true };
    QString reason;                ///< why it does not fit, "" when it does
    qint64 maxVoxels{ 0 };
    int maxLevelWithinLimit{ 0 };
    int finestLevelInRegion{ -1 }; ///< -1: unknown
    QVector<int> levels;           ///< the mesh's leaf levels
    QStringList fields;
};

struct Cube
{
    QString field;
    QString path;                  ///< on the BACKEND's filesystem
    QVector<int> shape;
};

struct Result
{
    bool valid{ false };
    QString error;
    Plan plan;
    QVector<Cube> outputs;
    int finestLevel{ -1 };
};

namespace detail {

inline QVector<double> doubles(const QJsonValue &value)
{
    QVector<double> out;
    for (const QJsonValue &v : value.toArray())
        out.append(v.toDouble());
    return out;
}

inline QVector<int> ints(const QJsonValue &value)
{
    QVector<int> out;
    for (const QJsonValue &v : value.toArray())
        out.append(v.toInt());
    return out;
}

inline QString g(double v)
{
    return QString::number(v, 'g', 4);
}

}   // namespace detail

/// The plan of a reply: /plan holds it under "plan", /uniform likewise.
inline Plan parsePlan(const QJsonObject &reply)
{
    Plan plan;
    const QJsonObject p = reply.value(QStringLiteral("plan")).toObject();
    plan.code = reply.value(QStringLiteral("code")).toString();
    plan.field = p.value(QStringLiteral("field")).toString();
    plan.level = p.value(QStringLiteral("level")).toInt();
    plan.shape = detail::ints(p.value(QStringLiteral("shape")));
    plan.voxels = static_cast<qint64>(p.value(QStringLiteral("voxels")).toDouble());
    plan.bytes = static_cast<qint64>(p.value(QStringLiteral("bytes_estimate")).toDouble());
    plan.peakMemoryBytes =
            static_cast<qint64>(p.value(QStringLiteral("peak_memory_bytes_estimate")).toDouble());
    plan.voxelSizePhysical = p.value(QStringLiteral("voxel_size_physical")).toDouble();
    plan.lowerPhysical = detail::doubles(p.value(QStringLiteral("lower_physical")));
    plan.upperPhysical = detail::doubles(p.value(QStringLiteral("upper_physical")));
    plan.lengthUnit = p.value(QStringLiteral("length_unit")).toString();
    plan.valueUnit = p.value(QStringLiteral("value_unit")).toString();
    plan.fits = p.value(QStringLiteral("fits")).toBool(true);
    plan.reason = p.value(QStringLiteral("reason")).toString();
    plan.maxVoxels = static_cast<qint64>(p.value(QStringLiteral("max_voxels")).toDouble());
    plan.maxLevelWithinLimit = p.value(QStringLiteral("max_level_within_limit")).toInt();
    const QJsonValue finest = p.value(QStringLiteral("finest_level_in_region"));
    plan.finestLevelInRegion = finest.isDouble() ? finest.toInt() : -1;
    plan.levels = detail::ints(p.value(QStringLiteral("levels")));
    for (const QJsonValue &v : p.value(QStringLiteral("fields")).toArray())
        plan.fields.append(v.toString());
    plan.valid = plan.shape.size() == 3 && plan.voxels > 0;
    return plan;
}

inline QVector<Cube> parseCubes(const QJsonObject &reply)
{
    QVector<Cube> cubes;
    for (const QJsonValue &v : reply.value(QStringLiteral("outputs")).toArray()) {
        const QJsonObject o = v.toObject();
        cubes.append({ o.value(QStringLiteral("field")).toString(),
                       o.value(QStringLiteral("path")).toString(),
                       detail::ints(o.value(QStringLiteral("shape"))) });
    }
    return cubes;
}

/// The body of POST /v1/simulations/amr/plan and /uniform. @p level < 0 sends
/// none: the backend takes the finest level the region holds that fits.
inline QJsonObject requestBody(const QString &path, const QString &field, int level,
                               const Region &region, qint64 maxVoxels)
{
    QJsonObject body{ { QStringLiteral("path"), path },
                      { QStringLiteral("field"), field },
                      { QStringLiteral("max_voxels"), static_cast<double>(maxVoxels) } };
    if (level >= 0)
        body.insert(QStringLiteral("level"), level);
    if (region.set) {
        body.insert(QStringLiteral("lo"), QJsonArray{ region.lo[0], region.lo[1], region.lo[2] });
        body.insert(QStringLiteral("hi"), QJsonArray{ region.hi[0], region.hi[1], region.hi[2] });
        body.insert(QStringLiteral("region_unit"), region.unit);
    }
    return body;
}

/// Voxels the dialog asks for: a count in millions, at least one million.
inline qint64 voxelBudget(int millions)
{
    return static_cast<qint64>(std::max(1, millions)) * 1000000;
}

/// The plan as a short rich-text summary for the dialog.
inline QString summary(const Plan &plan)
{
    QStringList dims;
    for (int n : plan.shape)
        dims.append(QString::number(n));
    QStringList boxes;
    const char axes[] = { 'x', 'y', 'z' };
    for (int i = 0; i < 3 && i < plan.lowerPhysical.size() && i < plan.upperPhysical.size(); ++i)
        boxes.append(QStringLiteral("%1 %2 to %3")
                             .arg(QLatin1Char(axes[i]), detail::g(plan.lowerPhysical[i]),
                                  detail::g(plan.upperPhysical[i])));
    QString text = QStringLiteral(
                           "<b>Level %1</b>: %2 voxels (%3), voxel %4 %5<br>"
                           "<b>Per field:</b> %6 &nbsp; <b>Peak memory:</b> %7<br>"
                           "<b>Box:</b> %8 %9")
                           .arg(QString::number(plan.level), dims.join(QStringLiteral(" × ")),
                                QLocale().toString(plan.voxels),
                                detail::g(plan.voxelSizePhysical), plan.lengthUnit,
                                visivo::formatBytes(plan.bytes),
                                visivo::formatBytes(plan.peakMemoryBytes),
                                boxes.join(QStringLiteral(", ")), plan.lengthUnit);
    text += QStringLiteral("<br><b>Finest level that fits the limit:</b> %1")
                    .arg(plan.maxLevelWithinLimit);
    if (plan.finestLevelInRegion >= 0)
        text += QStringLiteral(" &nbsp; <b>Finest level in the region:</b> %1")
                        .arg(plan.finestLevelInRegion);
    if (!plan.fits)
        text += QStringLiteral("<br><b>Too large:</b> %1")
                        .arg(plan.reason.toHtmlEscaped());
    return text;
}

// ── A box of cube voxels as the region of a request ─────────────────────────

/// One grid axis of a cube: the native coordinate of voxel 0 and the step
/// between voxels (negative for a descending axis).
struct AxisScale
{
    double origin{ 0.0 };
    double step{ 1.0 };

    double world(double voxel) const { return origin + voxel * step; }
    /// Fractional voxel under @p coordinate (voxel centres are integers).
    double voxel(double coordinate) const { return (coordinate - origin) / step; }
};

/// The region, in the cube's own axes (box units: an AMR cube is written in the
/// run's code units, its axes a fraction of the box), of the voxels @p box
/// ({x0, x1, y0, y1, z0, z1}, inclusive, 0-based): the outer faces of its end
/// voxels, so the plan covers every voxel the box shows. Unset when an axis has
/// no usable step.
inline Region regionFromVoxelBox(const std::array<int, 6> &box, const std::array<AxisScale, 3> &axes)
{
    Region region;
    for (int i = 0; i < 3; ++i) {
        if (!(std::isfinite(axes[std::size_t(i)].step) && axes[std::size_t(i)].step != 0.0))
            return Region{};
        const double a = axes[std::size_t(i)].world(box[std::size_t(2 * i)] - 0.5);
        const double b = axes[std::size_t(i)].world(box[std::size_t(2 * i + 1)] + 0.5);
        region.lo[std::size_t(i)] = std::min(a, b);
        region.hi[std::size_t(i)] = std::max(a, b);
    }
    region.unit = QStringLiteral("box");
    region.set = true;
    return region;
}

// ── The refinement-level map of a slice ─────────────────────────────────────

/// The body of POST /v1/simulations/amr/level-map for the plane of @p axis
/// ("x", "y" or "z") at @p coordinate, in the cube's own axes (box units); the
/// rectangles come back in the same unit.
inline QJsonObject levelMapBody(const QString &path, const QString &axis, double coordinate,
                                int maxRects)
{
    return QJsonObject{ { QStringLiteral("path"), path },
                        { QStringLiteral("axis"), axis },
                        { QStringLiteral("coordinate"), coordinate },
                        { QStringLiteral("unit"), QStringLiteral("box") },
                        { QStringLiteral("max_rects"), maxRects } };
}

using Rect = std::array<double, 4>;   ///< u0, v0, u1, v1

struct LevelMap
{
    bool valid{ false };
    QString error;
    QMap<int, QVector<Rect>> levels;   ///< rectangles by level, coarsest first
    QMap<int, qint64> counts;          ///< every cell of the plane, whatever was cut
    bool truncated{ false };
    qint64 cells{ 0 };
    QString lengthUnit;
};

inline LevelMap parseLevelMap(const QJsonObject &reply)
{
    LevelMap map;
    const QJsonObject levels = reply.value(QStringLiteral("levels")).toObject();
    for (auto it = levels.begin(); it != levels.end(); ++it) {
        bool ok = false;
        const int level = it.key().toInt(&ok);
        if (!ok)
            continue;
        QVector<Rect> rects;
        for (const QJsonValue &r : it.value().toArray()) {
            const QJsonArray a = r.toArray();
            if (a.size() == 4)
                rects.append({ a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble() });
        }
        map.levels.insert(level, rects);
    }
    const QJsonObject counts = reply.value(QStringLiteral("counts")).toObject();
    for (auto it = counts.begin(); it != counts.end(); ++it)
        map.counts.insert(it.key().toInt(), static_cast<qint64>(it.value().toDouble()));
    map.truncated = reply.value(QStringLiteral("truncated")).toBool();
    map.cells = static_cast<qint64>(reply.value(QStringLiteral("cells")).toDouble());
    map.lengthUnit = reply.value(QStringLiteral("length_unit")).toString();
    map.valid = !map.levels.isEmpty();
    return map;
}

/// A rectangle of the plane (u, v in the cube's axes) as the pixel box of the
/// two axes it is drawn on: {x0, y0, x1, y1}, edges at half pixels.
inline Rect rectToVoxels(const Rect &r, const AxisScale &u, const AxisScale &v)
{
    return { u.voxel(r[0]), v.voxel(r[1]), u.voxel(r[2]), v.voxel(r[3]) };
}

/// Which cube axis the plane normal to @p axis shows horizontally and
/// vertically, as the backend orders them (x, y, z without the normal).
inline std::array<int, 2> planeAxes(int normalAxis)
{
    return normalAxis == 0 ? std::array<int, 2>{ 1, 2 }
            : (normalAxis == 1 ? std::array<int, 2>{ 0, 2 } : std::array<int, 2>{ 0, 1 });
}

/// A colour for a level, coarse to fine over the levels the plane holds: a
/// blue to red ramp, so the finest cells are the ones that stand out.
inline std::array<unsigned char, 3> levelColour(int level, int lowest, int highest)
{
    static constexpr double stops[][3] = { { 0.12, 0.35, 0.95 }, { 0.10, 0.80, 0.85 },
                                           { 0.30, 0.85, 0.30 }, { 0.98, 0.85, 0.10 },
                                           { 0.98, 0.45, 0.10 }, { 0.90, 0.10, 0.15 } };
    constexpr int n = int(sizeof(stops) / sizeof(stops[0]));
    const double t = highest > lowest ? double(level - lowest) / double(highest - lowest) : 0.0;
    const double x = std::clamp(t, 0.0, 1.0) * (n - 1);
    const int i = std::min(int(x), n - 2);
    const double f = x - i;
    std::array<unsigned char, 3> out{};
    for (int c = 0; c < 3; ++c)
        out[std::size_t(c)] = static_cast<unsigned char>(
                std::lround(255.0 * (stops[i][c] * (1.0 - f) + stops[i + 1][c] * f)));
    return out;
}

// ── A cube made from an AMR output, told by its header ──────────────────────

/// What the AMR* cards of a cube say of where it came from.
struct Provenance
{
    QString source;     ///< AMRSRC: the output on the backend
    QString field;      ///< AMRFLD
    QString code;       ///< AMRCODE
    int level{ -1 };    ///< AMRLEV
    int maxLevel{ -1 }; ///< AMRLMAX
};

namespace detail {

/// The records (80 characters each) of a header's cards: the backend sends one
/// string per card, and a long value comes as that card with its CONTINUE
/// records after it.
inline QStringList records(const QStringList &cards)
{
    QStringList out;
    for (const QString &card : cards) {
        if (card.size() <= 80) {
            out.append(card);
            continue;
        }
        for (int at = 0; at < card.size(); at += 80)
            out.append(card.mid(at, 80));
    }
    return out;
}

/// The quoted string that starts at @p from in @p text, '' read as one quote;
/// @p end is set to the index after the closing quote.
inline QString quoted(const QString &text, int from, int *end)
{
    QString out;
    int i = from;
    if (i >= text.size() || text[i] != QLatin1Char('\''))
        return out;
    for (++i; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('\'')) {
            if (i + 1 < text.size() && text[i + 1] == QLatin1Char('\'')) {
                out.append(QLatin1Char('\''));
                ++i;
                continue;
            }
            ++i;
            break;
        }
        out.append(text[i]);
    }
    if (end)
        *end = i;
    return out;
}

}   // namespace detail

/// The value of keyword @p key as text: a string card without its quotes (a
/// long one joined across its CONTINUE records), a number card up to its
/// comment. Empty when the keyword is not there.
inline QString cardValue(const QStringList &cards, const QString &key)
{
    const QStringList recs = detail::records(cards);
    const QString head = key.leftJustified(8) + QStringLiteral("= ");
    for (int r = 0; r < recs.size(); ++r) {
        if (!recs[r].startsWith(head))
            continue;
        const QString body = recs[r].mid(10).trimmed();
        if (!body.startsWith(QLatin1Char('\''))) {
            const int slash = body.indexOf(QLatin1Char('/'));
            return (slash < 0 ? body : body.left(slash)).trimmed();
        }
        int end = 0;
        QString value = detail::quoted(recs[r], 10 + recs[r].mid(10).indexOf(QLatin1Char('\'')),
                                       &end);
        // "&" at the end of the string: it goes on in the next CONTINUE record.
        while (value.endsWith(QLatin1Char('&')) && r + 1 < recs.size()
               && recs[r + 1].startsWith(QStringLiteral("CONTINUE"))) {
            value.chop(1);
            ++r;
            const int q = recs[r].indexOf(QLatin1Char('\''));
            if (q < 0)
                break;
            value += detail::quoted(recs[r], q, &end);
        }
        return value.trimmed();
    }
    return QString();
}

/// The provenance of a cube the backend made from an AMR output (its AMRSRC
/// card names the output); nothing for any other cube.
inline std::optional<Provenance> provenanceFromCards(const QStringList &cards)
{
    Provenance p;
    p.source = cardValue(cards, QStringLiteral("AMRSRC"));
    if (p.source.isEmpty())
        return std::nullopt;
    p.field = cardValue(cards, QStringLiteral("AMRFLD"));
    p.code = cardValue(cards, QStringLiteral("AMRCODE"));
    bool ok = false;
    const int level = cardValue(cards, QStringLiteral("AMRLEV")).toInt(&ok);
    p.level = ok ? level : -1;
    const int top = cardValue(cards, QStringLiteral("AMRLMAX")).toInt(&ok);
    p.maxLevel = ok ? top : -1;
    return p;
}

}   // namespace visivo::amrview
