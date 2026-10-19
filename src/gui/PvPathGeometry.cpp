#include "PvPathGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pvpath {

namespace {
double dist(const Vertex &a, const Vertex &b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}
}  // namespace

int hitVertex(const Path &points, const Vertex &p, double tolerance)
{
    int best = -1;
    double bestDistance = tolerance;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double d = dist(points[i], p);
        if (d <= bestDistance) {
            bestDistance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

SegmentHit hitSegment(const Path &points, const Vertex &p, double tolerance)
{
    SegmentHit best;
    best.distance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Vertex &a = points[i];
        const Vertex &b = points[i + 1];
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double len2 = dx * dx + dy * dy;
        double t = 0.0;
        if (len2 > 0.0)
            t = std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / len2, 0.0, 1.0);
        const double d = dist({ a[0] + t * dx, a[1] + t * dy }, p);
        if (d <= tolerance && d < best.distance) {
            best.segment = static_cast<int>(i);
            best.t = t;
            best.distance = d;
        }
    }
    if (best.segment < 0)
        best.distance = 0.0;
    return best;
}

Vertex centroid(const Path &path)
{
    Vertex c{ 0.0, 0.0 };
    if (path.empty())
        return c;
    for (const Vertex &v : path) {
        c[0] += v[0];
        c[1] += v[1];
    }
    c[0] /= static_cast<double>(path.size());
    c[1] /= static_cast<double>(path.size());
    return c;
}

Path translated(const Path &path, double dx, double dy)
{
    Path out = path;
    for (Vertex &v : out) {
        v[0] += dx;
        v[1] += dy;
    }
    return out;
}

Path rotated(const Path &path, const Vertex &center, double radians)
{
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Path out = path;
    for (Vertex &v : out) {
        const double x = v[0] - center[0];
        const double y = v[1] - center[1];
        v = { center[0] + c * x - s * y, center[1] + s * x + c * y };
    }
    return out;
}

double angleAround(const Vertex &center, const Vertex &p)
{
    return std::atan2(p[1] - center[1], p[0] - center[0]);
}

Path withVertexInserted(const Path &path, int segment, const Vertex &p)
{
    if (segment < 0 || segment + 1 >= static_cast<int>(path.size()))
        return path;
    Path out = path;
    out.insert(out.begin() + segment + 1, p);
    return out;
}

Path withVertexRemoved(const Path &path, int index)
{
    if (index < 0 || index >= static_cast<int>(path.size()) || path.size() <= 2)
        return path;
    Path out = path;
    out.erase(out.begin() + index);
    return out;
}

Path smoothed(const Path &control, int samplesPerSegment)
{
    if (control.size() < 3 || samplesPerSegment < 2)
        return control;
    // Centripetal parameterisation (alpha = 0.5): knot spacing ∝ sqrt(distance).
    const auto knot = [](double ti, const Vertex &a, const Vertex &b) {
        return ti + std::max(std::sqrt(dist(a, b)), 1e-9);
    };
    // Phantom end points mirror the first and last span, so the curve starts
    // and ends at the first and last control vertex along the drawn direction.
    Path p;
    p.reserve(control.size() + 2);
    p.push_back({ 2 * control[0][0] - control[1][0], 2 * control[0][1] - control[1][1] });
    p.insert(p.end(), control.begin(), control.end());
    const Vertex &l = control[control.size() - 1];
    const Vertex &m = control[control.size() - 2];
    p.push_back({ 2 * l[0] - m[0], 2 * l[1] - m[1] });

    Path out;
    out.reserve((control.size() - 1) * static_cast<std::size_t>(samplesPerSegment) + 1);
    for (std::size_t i = 1; i + 2 < p.size(); ++i) {
        const Vertex &p0 = p[i - 1], &p1 = p[i], &p2 = p[i + 1], &p3 = p[i + 2];
        const double t0 = 0.0;
        const double t1 = knot(t0, p0, p1);
        const double t2 = knot(t1, p1, p2);
        const double t3 = knot(t2, p2, p3);
        for (int k = 0; k < samplesPerSegment; ++k) {
            const double t = t1 + (t2 - t1) * k / samplesPerSegment;
            Vertex pt;
            for (int c = 0; c < 2; ++c) {
                const double a1 = (t1 - t) / (t1 - t0) * p0[c] + (t - t0) / (t1 - t0) * p1[c];
                const double a2 = (t2 - t) / (t2 - t1) * p1[c] + (t - t1) / (t2 - t1) * p2[c];
                const double a3 = (t3 - t) / (t3 - t2) * p2[c] + (t - t2) / (t3 - t2) * p3[c];
                const double b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2;
                const double b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3;
                pt[c] = (t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2;
            }
            out.push_back(pt);
        }
    }
    out.push_back(control.back());
    return out;
}

double length(const Path &path)
{
    double total = 0.0;
    for (std::size_t i = 1; i < path.size(); ++i)
        total += dist(path[i - 1], path[i]);
    return total;
}

UniformRows rowsOnUniformSpectralGrid(const std::vector<double> &values, int nx, int ny,
                                      const std::vector<double> &spectral)
{
    UniformRows out{ values, ny };
    if (nx <= 0 || ny <= 1 || values.size() != std::size_t(nx) * std::size_t(ny)
        || spectral.size() != std::size_t(ny))
        return out;
    // Channels sorted by coordinate.
    std::vector<int> order(static_cast<std::size_t>(ny));
    for (int i = 0; i < ny; ++i)
        order[std::size_t(i)] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return spectral[std::size_t(a)] < spectral[std::size_t(b)]; });
    std::vector<double> sorted(static_cast<std::size_t>(ny));
    for (int i = 0; i < ny; ++i)
        sorted[std::size_t(i)] = spectral[std::size_t(order[std::size_t(i)])];
    const double lo = sorted.front();
    const double hi = sorted.back();
    if (!(hi > lo) || !std::isfinite(lo) || !std::isfinite(hi))
        return out;
    double minStep = std::numeric_limits<double>::infinity();
    double maxStep = 0.0;
    for (int i = 1; i < ny; ++i) {
        const double d = sorted[std::size_t(i)] - sorted[std::size_t(i - 1)];
        minStep = std::min(minStep, d);
        maxStep = std::max(maxStep, d);
    }
    const bool linear = minStep > 0.0 && (maxStep - minStep) <= 1e-6 * maxStep;
    // A grid fine enough for the closest channels keeps every channel. A linear
    // axis, or one whose fine grid would be huge, gets one row per channel in
    // value order instead: exact for the former, approximate positions for the
    // latter — but no channel, no emission, is ever lost.
    const double needed = minStep > 0.0 ? std::ceil((hi - lo) / minStep) + 1.0 : 0.0;
    const bool fineGrid = !linear && minStep > 0.0 && needed <= std::max(4096.0, 8.0 * ny);
    if (!fineGrid) {
        out.rows = ny;
        out.values.assign(values.size(), 0.0);
        for (int r = 0; r < ny; ++r)
            std::copy_n(values.begin() + std::ptrdiff_t(order[std::size_t(r)]) * nx, nx,
                        out.values.begin() + std::ptrdiff_t(r) * nx);
        return out;
    }
    const int rows = int(needed);
    out.rows = rows;
    out.values.assign(std::size_t(rows) * std::size_t(nx), 0.0);
    for (int r = 0; r < rows; ++r) {
        const double y = lo + (hi - lo) * double(r) / double(rows - 1);
        // Nearest channel by value.
        auto it = std::lower_bound(sorted.begin(), sorted.end(), y);
        std::size_t k = std::size_t(it - sorted.begin());
        if (k >= sorted.size())
            k = sorted.size() - 1;
        else if (k > 0 && (y - sorted[k - 1]) <= (sorted[k] - y))
            --k;
        const int channel = order[k];
        std::copy_n(values.begin() + std::ptrdiff_t(channel) * nx, nx,
                    out.values.begin() + std::ptrdiff_t(r) * nx);
    }
    return out;
}

}  // namespace pvpath
