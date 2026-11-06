#ifndef COSMOLOGYDISTANCETABLE_H
#define COSMOLOGYDISTANCETABLE_H
// CosmologyDistanceTable.h
// Comoving distances for a whole catalogue under a CUSTOM cosmology.
//
// The backend's preset route takes 100 000 redshifts in one call, but the only
// route that takes a custom H0 / Ωm / ΩΛ (GET /v1/cosmology/distances) answers
// one redshift per request. A catalogue with a few distinct redshifts asks for
// each of them; a larger one asks for a fixed set of nodes spanning its range
// and reads every source off them. The distances are still the backend's: the
// client only interpolates between values it was given.
//
// The interpolant is D_C(z)/z, linear in ln(1 + z). D_C itself is linear in z
// near the observer and bends at high z, so a straight line in ln(1 + z)
// through D_C has an error that does NOT shrink with more nodes near z = 0
// (measured: 1.5e-3 at 128 nodes up to z = 1). D_C/z is smooth and finite at
// both ends, and 128 nodes keep the worst error under 1e-4 for flat, open and
// matter-only models from z = 1e-4 to 20 (astropy, measured; pinned in
// tests/test_cosmology_tools.cpp against a stand-in integrator).

#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace visivo::cosmology {

/// Nodes per table, and the most distinct redshifts asked for one by one.
inline constexpr int kTableNodes = 128;

/// The distinct redshifts of @p redshifts that are usable (finite, > 0),
/// sorted. A catalogue z of 0 or below is "not measured" here, as in the
/// catalogue parser.
inline QVector<double> distinctUsable(const QVector<double> &redshifts)
{
    QVector<double> out;
    out.reserve(redshifts.size());
    for (double z : redshifts)
        if (std::isfinite(z) && z > 0.0)
            out.append(z);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// @p n nodes uniform in ln(1 + z) from @p zMin to @p zMax, both included.
/// Requires 0 < zMin < zMax and n ≥ 2.
inline QVector<double> tableNodes(double zMin, double zMax, int n = kTableNodes)
{
    QVector<double> nodes;
    if (!(zMin > 0.0) || !(zMax > zMin) || n < 2)
        return nodes;
    const double u0 = std::log1p(zMin);
    const double u1 = std::log1p(zMax);
    nodes.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double u = u0 + (u1 - u0) * static_cast<double>(i) / (n - 1);
        nodes.append(std::expm1(u));
    }
    nodes.first() = zMin;   // exact ends: no round trip through log/exp
    nodes.last() = zMax;
    return nodes;
}

/// The comoving distance at @p z from a table of (node, distance) pairs, the
/// nodes ascending and every distance finite. NaN outside the table's range
/// or for a z that is not a measurement, so no source is extrapolated.
inline double interpolate(const QVector<double> &nodes, const QVector<double> &distances, double z)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (!(std::isfinite(z) && z > 0.0) || nodes.size() != distances.size() || nodes.isEmpty())
        return nan;
    if (nodes.size() == 1)
        return z == nodes.first() ? distances.first() : nan;
    if (z < nodes.first() || z > nodes.last())
        return nan;
    auto hi = std::lower_bound(nodes.begin(), nodes.end(), z);
    if (hi == nodes.begin())
        return distances.first();
    if (*hi == z)
        return distances[static_cast<int>(hi - nodes.begin())];
    const int j = static_cast<int>(hi - nodes.begin());
    const int i = j - 1;
    const double u = std::log1p(z);
    const double ui = std::log1p(nodes[i]);
    const double uj = std::log1p(nodes[j]);
    const double fi = distances[i] / nodes[i];
    const double fj = distances[j] / nodes[j];
    const double t = (u - ui) / (uj - ui);
    return z * (fi + (fj - fi) * t);
}

}   // namespace visivo::cosmology

#endif   // COSMOLOGYDISTANCETABLE_H
