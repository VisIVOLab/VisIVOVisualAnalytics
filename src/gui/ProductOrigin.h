#ifndef PRODUCT_ORIGIN_H
#define PRODUCT_ORIGIN_H
// ProductOrigin.h
// Where a product came from, as pure QtCore data.
//
// A product records what a tool MEASURED. On its own that is half a provenance:
// "Box region stats, mean 1204.7" does not say which box, and a pinned spectrum
// does not say which pixel it was probed at (user report). So a product also
// carries the geometry that produced it, and opening its Session Data row puts
// that geometry back on screen — the overlay is redrawn, not just described.
//
// The shape is shared by the cube and image viewers because both draw the same
// four region kinds, and it is separate from the widgets because, like
// ToolResultRecord, it is the part that can be wrong without failing to compile.
//
//   { "kind": "region", "shape": "annulus",
//     "anchor": [x, y], "current": [x, y], "innerRadius": 12.0 }
//   { "kind": "region", "shape": "polygon", "points": [[x, y], …] }
//   { "kind": "probe",  "voxel": [x, y, z] }

#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <array>
#include <vector>

namespace visivo {

/// The region shapes both viewers can draw. Kept as strings in the record so it
/// survives a change to either window's own enum ordering.
inline QString regionShapeName(int mode)
{
    switch (mode) {
    case 1:  return QStringLiteral("box");
    case 2:  return QStringLiteral("circle");
    case 3:  return QStringLiteral("polygon");
    case 4:  return QStringLiteral("annulus");
    default: return QString();
    }
}

/// A drawn region: anchor + current voxel for box/circle/annulus, the vertex
/// list for a polygon, plus the annulus inner radius.
inline QVariantMap encodeRegionOrigin(int mode,
                                      const std::array<int, 2> &anchor,
                                      const std::array<int, 2> &current,
                                      const std::vector<std::array<int, 2>> &polygon,
                                      double annulusInnerRadius)
{
    const QString shape = regionShapeName(mode);
    if (shape.isEmpty())
        return {};
    QVariantMap m{ { QStringLiteral("kind"), QStringLiteral("region") },
                   { QStringLiteral("shape"), shape } };
    if (shape == QStringLiteral("polygon")) {
        QVariantList points;
        for (const std::array<int, 2> &p : polygon)
            points.append(QVariant(QVariantList{ p[0], p[1] }));
        m.insert(QStringLiteral("points"), points);
        return m;
    }
    m.insert(QStringLiteral("anchor"), QVariant(QVariantList{ anchor[0], anchor[1] }));
    m.insert(QStringLiteral("current"), QVariant(QVariantList{ current[0], current[1] }));
    if (shape == QStringLiteral("annulus"))
        m.insert(QStringLiteral("innerRadius"), annulusInnerRadius);
    return m;
}

/// The pixel a spectrum was probed at.
inline QVariantMap encodeProbeOrigin(const std::array<int, 3> &voxel)
{
    return { { QStringLiteral("kind"), QStringLiteral("probe") },
             { QStringLiteral("voxel"),
               QVariant(QVariantList{ voxel[0], voxel[1], voxel[2] }) } };
}

inline QString originKind(const QVariantMap &origin)
{
    return origin.value(QStringLiteral("kind")).toString();
}
inline QString originShape(const QVariantMap &origin)
{
    return origin.value(QStringLiteral("shape")).toString();
}

/// Read a 2- or 3-element integer point. Returns false when the record does not
/// carry one, so a caller never restores half a geometry.
inline bool originPoint(const QVariantMap &origin, const QString &key,
                        int *out, int count)
{
    const QVariantList v = origin.value(key).toList();
    if (v.size() < count)
        return false;
    for (int i = 0; i < count; ++i) {
        bool ok = false;
        out[i] = v.at(i).toInt(&ok);
        if (!ok)
            return false;
    }
    return true;
}

inline std::vector<std::array<int, 2>> originPolygon(const QVariantMap &origin)
{
    std::vector<std::array<int, 2>> out;
    const QVariantList points = origin.value(QStringLiteral("points")).toList();
    for (const QVariant &pv : points) {
        const QVariantList p = pv.toList();
        if (p.size() >= 2)
            out.push_back({ p.at(0).toInt(), p.at(1).toInt() });
    }
    return out;
}

inline double originInnerRadius(const QVariantMap &origin)
{
    return origin.value(QStringLiteral("innerRadius")).toDouble();
}

/// A short human phrase for the row's tooltip and the status bar.
inline QString describeOrigin(const QVariantMap &origin)
{
    const QString kind = originKind(origin);
    if (kind == QStringLiteral("probe")) {
        int v[3] = { 0, 0, 0 };
        if (originPoint(origin, QStringLiteral("voxel"), v, 3))
            return QStringLiteral("probed at pixel (%1, %2), channel %3")
                    .arg(v[0]).arg(v[1]).arg(v[2] + 1);
        return QStringLiteral("a probed pixel");
    }
    if (kind != QStringLiteral("region"))
        return QString();
    const QString shape = originShape(origin);
    if (shape == QStringLiteral("polygon"))
        return QStringLiteral("a polygon of %1 points").arg(originPolygon(origin).size());
    int a[2] = { 0, 0 };
    if (!originPoint(origin, QStringLiteral("anchor"), a, 2))
        return QStringLiteral("a %1 region").arg(shape);
    return QStringLiteral("a %1 region at (%2, %3)").arg(shape).arg(a[0]).arg(a[1]);
}

}   // namespace visivo

#endif   // PRODUCT_ORIGIN_H
