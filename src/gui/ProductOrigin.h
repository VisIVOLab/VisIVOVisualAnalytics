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

#include <cmath>

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

/// The path a PV diagram was cut along. Its own kind rather than a "polygon":
/// a PV path is open and carries a width, and restoring one into a region's
/// polygon would draw a closed shape that was never there.
inline QVariantMap encodePvPathOrigin(const std::vector<std::array<int, 2>> &vertices,
                                      int widthPixels)
{
    if (vertices.size() < 2)
        return {};
    QVariantList points;
    for (const std::array<int, 2> &v : vertices)
        points.append(QVariant(QVariantList{ v[0], v[1] }));
    return { { QStringLiteral("kind"), QStringLiteral("pvpath") },
             { QStringLiteral("points"), points },
             { QStringLiteral("width"), widthPixels } };
}

inline int originWidth(const QVariantMap &origin)
{
    return origin.value(QStringLiteral("width")).toInt();
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

// ── The voxel frame a geometry was recorded in ───────────────────────────────
// Region geometry is stored in DISPLAYED-voxel indices, and on a decimated LOD
// image that index space is coarser than the full-resolution one (spacing 2,
// say). Replaying the raw indices after the viewer has swapped to another LOD
// puts the overlay at the wrong place and size - the numbers would be right and
// the box would be somewhere else. Stamping the mapping voxel -> world at
// record time lets the restore convert into whatever grid is displayed then.
//
//   world = pos + voxel * scale        (pos = origin - extent0 * spacing)

inline QVariantMap withVoxelFrame(QVariantMap origin, double scaleX, double scaleY,
                                  double posX, double posY)
{
    origin.insert(QStringLiteral("frame"),
                  QVariantList{ scaleX, scaleY, posX, posY });
    return origin;
}

/// Read the recorded frame. False when the product predates the frame stamp -
/// callers then have to assume the geometry is already in the current grid.
inline bool originFrame(const QVariantMap &origin, double *scale, double *pos)
{
    const QVariantList f = origin.value(QStringLiteral("frame")).toList();
    if (f.size() != 4 || !scale || !pos)
        return false;
    for (const QVariant &v : f) {
        if (!v.canConvert<double>())
            return false;
    }
    scale[0] = f.at(0).toDouble();
    scale[1] = f.at(1).toDouble();
    pos[0]   = f.at(2).toDouble();
    pos[1]   = f.at(3).toDouble();
    return scale[0] != 0.0 && scale[1] != 0.0;
}

/// Convert one recorded voxel into the grid described by `scale`/`pos`, using
/// the frame stamped on `origin`. Returns the input unchanged when there is no
/// stamp or the two grids match, so this is safe to call unconditionally.
inline void remapOriginVoxel(const QVariantMap &origin, const double scale[2],
                             const double pos[2], int voxel[2])
{
    double oldScale[2] = { 0., 0. }, oldPos[2] = { 0., 0. };
    if (!originFrame(origin, oldScale, oldPos))
        return;
    if (oldScale[0] == scale[0] && oldScale[1] == scale[1] && oldPos[0] == pos[0]
        && oldPos[1] == pos[1])
        return;
    for (int i = 0; i < 2; ++i) {
        const double world = oldPos[i] + voxel[i] * oldScale[i];
        voxel[i] = static_cast<int>(std::lround((world - pos[i]) / scale[i]));
    }
}

// ── Which surface the 2-D measurement was taken on ───────────────────────────
// A cube shows several 2-D surfaces in the same place: channel N of the cube, or
// a moment map. Region statistics computed on one of them are meaningless next
// to another, and the geometry alone cannot tell them apart - reopening a
// channel-10 measurement while channel 50 is displayed put the overlay back with
// nothing saying the numbers came from elsewhere.

inline QVariantMap withMeasuredSurface(QVariantMap origin, const QString &surface, int channel)
{
    if (!surface.isEmpty())
        origin.insert(QStringLiteral("surface"), surface);   // "slice" | "moment"
    if (channel >= 0)
        origin.insert(QStringLiteral("channel"), channel);   // 0-based
    return origin;
}

inline QString originSurface(const QVariantMap &origin)
{
    return origin.value(QStringLiteral("surface")).toString();
}

/// 0-based channel the measurement was taken on, or -1 when it was not a
/// channel (a moment map) or the product predates the stamp.
inline int originChannel(const QVariantMap &origin)
{
    const QVariant v = origin.value(QStringLiteral("channel"));
    return v.canConvert<int>() ? v.toInt() : -1;
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
    if (kind == QStringLiteral("pvpath")) {
        const int n = int(originPolygon(origin).size());
        const int w = originWidth(origin);
        return QStringLiteral("a %1-point PV path, %2 px wide").arg(n).arg(w);
    }
    if (kind != QStringLiteral("region"))
        return QString();
    const QString shape = originShape(origin);
    if (shape == QStringLiteral("polygon"))
        return QStringLiteral("a polygon of %1 points%2")
                .arg(originPolygon(origin).size())
                .arg(originSurface(origin) == QStringLiteral("moment")
                             ? QStringLiteral(" on the moment map")
                     : originChannel(origin) >= 0
                             ? QStringLiteral(" on channel %1").arg(originChannel(origin) + 1)
                             : QString());
    // Where it was measured, when the product says so.
    QString where;
    const QString surface = originSurface(origin);
    if (surface == QStringLiteral("moment"))
        where = QStringLiteral(" on the moment map");
    else if (originChannel(origin) >= 0)
        where = QStringLiteral(" on channel %1").arg(originChannel(origin) + 1);
    else if (surface == QStringLiteral("slice"))
        where = QStringLiteral(" on the displayed slice");

    int a[2] = { 0, 0 };
    if (!originPoint(origin, QStringLiteral("anchor"), a, 2))
        return QStringLiteral("a %1 region%2").arg(shape, where);
    return QStringLiteral("a %1 region at (%2, %3)%4")
            .arg(shape).arg(a[0]).arg(a[1]).arg(where);
}

}   // namespace visivo

#endif   // PRODUCT_ORIGIN_H
