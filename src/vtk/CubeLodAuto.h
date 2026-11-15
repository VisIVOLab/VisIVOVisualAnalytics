#ifndef CubeLodAuto_h
#define CubeLodAuto_h

#include "CubeLodPlan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// CubeLodAuto — the parts of the camera-driven level of detail (issue #321,
// phase 4) that are decisions rather than drawing: what the camera shows, in
// the planner's terms; whether a new plan is worth a fetch; and the cache of
// blocks already read. Pure, like CubeLodPlan, so it is unit-tested
// (tests/test_cube_lod_auto.cpp); the window measures the camera and acts.
// ---------------------------------------------------------------------------
namespace CubeLodAuto {

using CubeLodGeometry::Box;
using CubeLodGeometry::Dims;
using CubeLodGeometry::Node;
using CubeLodGeometry::Sampling;

// ── What the camera shows ───────────────────────────────────────────────────

/// The cube voxels a box of the view's world covers: @p worldBounds is
/// (x0, x1, y0, y1, z0, z1) in the world of a preview sampling the cube as
/// @p preview. Every voxel the box touches is in, so a sliver of a voxel at
/// the frustum's edge still counts; the result is clamped to the cube and
/// empty when the box misses it or is not finite.
inline Box visibleCubeBox(const std::array<double, 6> &worldBounds, const Sampling &preview,
                          const Dims &cubeShape)
{
    Box out;
    for (int i = 0; i < 3; ++i) {
        const double a = CubeLodGeometry::toCube(worldBounds[2 * i], preview);
        const double b = CubeLodGeometry::toCube(worldBounds[2 * i + 1], preview);
        if (!std::isfinite(a) || !std::isfinite(b) || cubeShape[i] < 1)
            return Box{};
        // Voxel v spans [v - 0.5, v + 0.5): the voxels under [lo, hi].
        const double lo = std::floor(std::min(a, b) + 0.5);
        const double hi = std::floor(std::max(a, b) + 0.5) + 1.0;
        out.lo[i] = int(std::clamp(lo, 0.0, double(cubeShape[i])));
        out.hi[i] = int(std::clamp(hi, 0.0, double(cubeShape[i])));
    }
    return out.empty() ? Box{} : out;
}

/// The world box the cube's voxels fill, faces at the outer edges of its end
/// voxels, in the world of @p preview: what the frustum is intersected with.
inline std::array<double, 6> cubeWorldBounds(const Dims &cubeShape, const Sampling &preview)
{
    std::array<double, 6> w{};
    for (int i = 0; i < 3; ++i) {
        w[2 * i] = CubeLodGeometry::toWorld(-0.5, preview);
        w[2 * i + 1] = CubeLodGeometry::toWorld(cubeShape[i] - 0.5, preview);
    }
    return w;
}

/// The camera, as far as the level of detail needs it.
struct Camera
{
    bool parallel{ false };
    double parallelScale{ 1.0 };    ///< world units, half the view's height
    double viewAngleDeg{ 30.0 };    ///< vertical, perspective only
    double focalDistance{ 1.0 };    ///< eye to focal point, world units
    int viewportHeightPx{ 0 };      ///< device pixels
};

/// Cube voxels one screen pixel covers at the focal depth, for a preview
/// sampling the cube as @p preview (one world unit = preview.scale cube
/// voxels). Infinite when the camera says nothing usable, which the planner
/// reads as "very coarse": no refinement.
inline double voxelsPerPixel(const Camera &c, const Sampling &preview)
{
    if (c.viewportHeightPx < 1 || !(preview.scale > 0.0))
        return std::numeric_limits<double>::infinity();
    double worldHeight = 0.0;
    if (c.parallel) {
        worldHeight = 2.0 * c.parallelScale;
    } else {
        const double half = c.viewAngleDeg * 3.14159265358979323846 / 360.0;
        worldHeight = 2.0 * c.focalDistance * std::tan(half);
    }
    if (!(worldHeight > 0.0) || !std::isfinite(worldHeight))
        return std::numeric_limits<double>::infinity();
    return worldHeight / double(c.viewportHeightPx) * preview.scale;
}

// ── Whether to fetch ────────────────────────────────────────────────────────

/// What a fine set is: its level and nodes. Two plans with the same signature
/// draw the same blocks, so the second needs nothing.
struct Signature
{
    int level{ -1 };
    int nodeSize{ 0 };
    std::vector<Node> nodes;   ///< sorted, so the order a plan lists them in does not matter

    bool empty() const { return nodes.empty(); }
    bool operator==(const Signature &o) const
    {
        return level == o.level && nodeSize == o.nodeSize && nodes == o.nodes;
    }
    bool operator!=(const Signature &o) const { return !(*this == o); }
};

inline bool nodeLess(const Node &a, const Node &b)
{
    return std::tie(a.level, a.iz, a.iy, a.ix) < std::tie(b.level, b.iz, b.iy, b.ix);
}

inline Signature signatureOf(const CubeLodPlan::Plan &plan)
{
    Signature s;
    if (!plan.refine)
        return s;
    s.level = plan.level;
    s.nodeSize = plan.nodeSize;
    s.nodes = plan.nodes;
    std::sort(s.nodes.begin(), s.nodes.end(), nodeLess);
    return s;
}

/// What the view holds when the camera settles.
struct State
{
    /// A box chosen with Refine Selected Box is shown or on its way: it stays
    /// until it is cleared, whatever the camera does.
    bool explicitHeld{ false };
    Signature shown;     ///< the camera's set on screen (empty: none)
    Signature pending;   ///< the camera's set being read (empty: none)
};

enum class Action {
    Keep,     ///< nothing to do
    Fetch,    ///< read the plan's blocks; swap them in once all have arrived
    Cancel,   ///< drop the set being read: the view came back to what is shown
    Clear,    ///< back to the plain preview: the view needs no finer level
};

/// What to do with @p plan, made for the settled camera, given @p state.
///
/// A plan that refines nothing (zoomed out, the cube out of view, a budget
/// too small) clears what the camera had asked for: blocks the view cannot
/// resolve only cost frames. A plan equal to the one being read, or to the
/// one shown with nothing else on its way, needs nothing.
inline Action decide(const CubeLodPlan::Plan &plan, const State &state)
{
    if (state.explicitHeld)
        return Action::Keep;
    const Signature wanted = signatureOf(plan);
    if (wanted.empty())
        return (state.shown.empty() && state.pending.empty()) ? Action::Keep : Action::Clear;
    if (!state.pending.empty() && wanted == state.pending)
        return Action::Keep;
    if (wanted == state.shown)
        return state.pending.empty() ? Action::Keep : Action::Cancel;
    return Action::Fetch;
}

// ── Blocks already read ─────────────────────────────────────────────────────

/// Least-recently-used cache of values that each take a known number of
/// bytes, held under a byte capacity. A value larger than the capacity is
/// not kept. Values are copied in and out, so hold them by a shared handle
/// (a smart pointer) when they are large.
template <typename Key, typename Value>
class LruCache
{
public:
    explicit LruCache(std::int64_t capacityBytes = 0)
        : m_capacity(std::max<std::int64_t>(0, capacityBytes))
    {
    }

    std::int64_t capacity() const { return m_capacity; }
    std::int64_t bytes() const { return m_bytes; }
    std::size_t size() const { return m_index.size(); }

    /// A new capacity; the least recently used values go until the rest fits.
    void setCapacity(std::int64_t capacityBytes)
    {
        m_capacity = std::max<std::int64_t>(0, capacityBytes);
        evict();
    }

    /// The value under @p key, now the most recently used; null when absent.
    const Value *find(const Key &key)
    {
        auto it = m_index.find(key);
        if (it == m_index.end())
            return nullptr;
        m_order.splice(m_order.begin(), m_order, it->second);
        return &it->second->value;
    }

    bool contains(const Key &key) const { return m_index.count(key) != 0; }

    /// Keep @p value under @p key (replacing what was there), as the most
    /// recently used; older values go while the total exceeds the capacity.
    void insert(const Key &key, Value value, std::int64_t bytes)
    {
        remove(key);
        if (bytes < 0 || bytes > m_capacity)
            return;
        m_order.push_front(Entry{ key, std::move(value), bytes });
        m_index.emplace(key, m_order.begin());
        m_bytes += bytes;
        evict();
    }

    void remove(const Key &key)
    {
        auto it = m_index.find(key);
        if (it == m_index.end())
            return;
        m_bytes -= it->second->bytes;
        m_order.erase(it->second);
        m_index.erase(it);
    }

    void clear()
    {
        m_order.clear();
        m_index.clear();
        m_bytes = 0;
    }

    /// Keys from the most to the least recently used (for tests and logs).
    std::vector<Key> keys() const
    {
        std::vector<Key> out;
        for (const Entry &e : m_order)
            out.push_back(e.key);
        return out;
    }

private:
    struct Entry
    {
        Key key;
        Value value;
        std::int64_t bytes{ 0 };
    };
    void evict()
    {
        while (m_bytes > m_capacity && !m_order.empty()) {
            m_bytes -= m_order.back().bytes;
            m_index.erase(m_order.back().key);
            m_order.pop_back();
        }
    }

    std::int64_t m_capacity{ 0 };
    std::int64_t m_bytes{ 0 };
    std::list<Entry> m_order;   ///< most recently used first
    std::map<Key, typename std::list<Entry>::iterator> m_index;
};

/// The key of a fine block in the cache: the dataset, the level and node side
/// it was read at, and its node.
using BlockKey = std::tuple<std::string, int, int, int, int, int>;

inline BlockKey blockKey(const std::string &dataset, int nodeSize, const Node &n)
{
    return { dataset, n.level, nodeSize, n.ix, n.iy, n.iz };
}

}   // namespace CubeLodAuto

#endif   // CubeLodAuto_h
