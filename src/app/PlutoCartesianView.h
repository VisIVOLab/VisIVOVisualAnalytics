#pragma once

// PlutoCartesianView.h
// The Cartesian view of a PLUTO output on polar, cylindrical, spherical or
// stretched cells (issue #527), as pure logic: what the backend's plan says,
// how it is worded for the user, and the body of the request that makes it.
// The resampling itself is the backend's (app/compute/pluto_cartesian.py,
// POST /v1/simulations/pluto/cartesian); the cube it writes opens like any
// other volume, in the grid's units. No widgets, no network:
// tests/test_pluto_cartesian_view.cpp.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace visivo::plutoview {

/// What the backend would make: the output axes, their box, the voxel counts.
struct Plan
{
    bool valid{ false };
    QString error;
    QString path;          ///< the PLUTO variable reference the backend resolved
    QString geometry;      ///< "SPHERICAL"...
    QString mode;          ///< "cube" | "plane" | "meridional" | "grid"
    QString indexReason;   ///< why the grid is shown on cell indices, "" if it is not
    QStringList axes;      ///< "X", "Y", "Z" in FITS order
    QVector<int> shape;
    QVector<double> lower, upper, cell;
    qint64 voxels{ 0 };
    qint64 bytes{ 0 };
    bool mirror{ false };
};

struct Cube
{
    QString field;
    QString path;          ///< on the BACKEND's filesystem
    QVector<int> shape;
};

struct Result
{
    bool valid{ false };
    QString error;
    Plan plan;
    QVector<Cube> outputs;
};

namespace detail {

inline QVector<double> doubles(const QJsonValue &value)
{
    QVector<double> out;
    for (const QJsonValue &v : value.toArray())
        out.append(v.toDouble());
    return out;
}

inline QStringList strings(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &v : value.toArray())
        out.append(v.toString());
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

/// The plan object of a reply (the reply of /cartesian/plan holds it under
/// "plan" with the reference beside it; that of /cartesian holds it likewise).
inline Plan parsePlan(const QJsonObject &reply)
{
    Plan plan;
    const QJsonObject p = reply.value(QStringLiteral("plan")).toObject();
    plan.path = reply.value(QStringLiteral("path")).toString();
    plan.indexReason = reply.value(QStringLiteral("index_reason")).toString();
    plan.geometry = p.value(QStringLiteral("geometry")).toString();
    plan.mode = p.value(QStringLiteral("mode")).toString();
    plan.axes = detail::strings(p.value(QStringLiteral("axes")));
    plan.shape = detail::ints(p.value(QStringLiteral("shape")));
    plan.lower = detail::doubles(p.value(QStringLiteral("lower")));
    plan.upper = detail::doubles(p.value(QStringLiteral("upper")));
    plan.cell = detail::doubles(p.value(QStringLiteral("cell")));
    plan.voxels = static_cast<qint64>(p.value(QStringLiteral("voxels")).toDouble());
    plan.bytes = static_cast<qint64>(p.value(QStringLiteral("bytes_estimate")).toDouble());
    plan.mirror = p.value(QStringLiteral("mirror")).toBool();
    plan.valid = !plan.shape.isEmpty() && plan.shape.size() == plan.axes.size();
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

/// What the view is, in words: "cube" is a Cartesian cube, "plane" the x-y
/// image, "meridional" the x-z image of an axisymmetric run, "grid" the same
/// axes made uniform.
inline QString modeLabel(const QString &mode)
{
    if (mode == QLatin1String("cube"))
        return QStringLiteral("Cartesian cube");
    if (mode == QLatin1String("plane"))
        return QStringLiteral("x-y image");
    if (mode == QLatin1String("meridional"))
        return QStringLiteral("meridional x-z image (x is the cylindrical radius)");
    return QStringLiteral("uniform grid");
}

/// Only an axisymmetric image has a negative-x half to mirror into.
inline bool canMirror(const Plan &plan)
{
    return plan.mode == QLatin1String("meridional");
}

/// Voxels the dialog asks for: a count in millions, at least one million.
inline qint64 voxelBudget(int millions)
{
    return static_cast<qint64>(std::max(1, millions)) * 1000000;
}

inline QString byteSize(qint64 bytes)
{
    constexpr double kMiB = 1024.0 * 1024.0;
    return bytes >= 1024.0 * kMiB ? QStringLiteral("%1 GiB").arg(bytes / (1024.0 * kMiB), 0, 'f', 2)
                                  : QStringLiteral("%1 MiB").arg(bytes / kMiB, 0, 'f', 1);
}

/// The plan as a short rich-text summary for the dialog.
inline QString summary(const Plan &plan)
{
    QStringList dims, cells, boxes;
    for (int n : plan.shape)
        dims.append(QString::number(n));
    for (double c : plan.cell)
        cells.append(detail::g(c));
    for (int i = 0; i < plan.axes.size() && i < plan.lower.size() && i < plan.upper.size(); ++i)
        boxes.append(QStringLiteral("%1 %2 to %3")
                             .arg(plan.axes[i].toLower(), detail::g(plan.lower[i]),
                                  detail::g(plan.upper[i])));
    return QStringLiteral("<b>%1</b> of a %2 grid<br><b>Grid:</b> %3 voxels &nbsp; "
                          "<b>Voxel:</b> %4 &nbsp; <b>Per variable:</b> %5<br><b>Box:</b> %6<br>"
                          "<i>Coordinates are in the run's length unit; cells outside the "
                          "domain are empty.</i>")
            .arg(modeLabel(plan.mode), plan.geometry.toLower(), dims.join(QStringLiteral(" × ")),
                 cells.join(QStringLiteral(" × ")), byteSize(plan.bytes),
                 boxes.join(QStringLiteral(", ")));
}

/// The body of POST /v1/simulations/pluto/cartesian[/plan].
inline QJsonObject requestBody(const QString &path, qint64 maxVoxels, bool mirror,
                               bool allVariables)
{
    QJsonObject body{ { QStringLiteral("path"), path },
                      { QStringLiteral("max_voxels"), static_cast<double>(maxVoxels) },
                      { QStringLiteral("mirror"), mirror } };
    if (allVariables)
        body.insert(QStringLiteral("fields"), QJsonArray{ QStringLiteral("*") });
    return body;
}

}   // namespace visivo::plutoview
