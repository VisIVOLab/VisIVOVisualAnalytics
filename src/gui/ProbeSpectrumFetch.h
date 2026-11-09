#ifndef PROBE_SPECTRUM_FETCH_H
#define PROBE_SPECTRUM_FETCH_H
// ProbeSpectrumFetch.h
// The live spectrum under the cursor, when the window does not hold the cube.
//
// On a cube above the full-resolution threshold the window shows a strided
// preview, and the live spectrum used to be read out of it at the slice's
// FULL-resolution pixel: outside the preview's (smaller) extent there was
// nothing, so the pane stayed empty and Pin Spectrum stayed greyed, and in its
// lower-left corner the curve came from the wrong line of sight. One line of
// sight is small, so it is asked of the backend instead (POST
// /v1/spectral/probe), as the slice itself is.
//
// The cursor moves faster than a request comes back. ProbeFetchGate keeps at
// most one request in flight per window and remembers only the LATEST pixel
// asked for meanwhile: when the reply comes back it is shown only if the probe
// is still on its pixel, and the latest pixel, if different, is asked next.
// Intermediate pixels are never fetched, a stale reply is never shown, and the
// GUI thread never waits.
//
// Header-only and widget-free, so it is tested on its own (TestProbeSpectrumFetch).

#include <algorithm>
#include <array>
#include <optional>

namespace visivo {

/// A full-resolution pixel (x, y) of the cube.
using ProbePixel = std::array<int, 2>;

class ProbeFetchGate
{
public:
    /// The fetch to start, with the generation its reply must carry back.
    struct Start
    {
        ProbePixel pixel{ 0, 0 };
        int generation{ 0 };
    };

    /// What to do with a reply.
    struct Done
    {
        bool apply{ false };              ///< show this reply
        std::optional<Start> next;        ///< start this fetch now
    };

    /// The probe moved to @p pixel. Returns the fetch to start now, or nothing
    /// when one is in flight (the pixel is remembered and asked for next).
    std::optional<Start> request(const ProbePixel &pixel)
    {
        if (m_inFlight) {
            m_pending = pixel;
            return std::nullopt;
        }
        m_inFlight = true;
        m_pending.reset();
        return Start{ pixel, m_generation };
    }

    /// The fetch started as @p started has returned. @p wanted is the pixel
    /// the probe is on now (nothing when the probe is gone).
    Done finished(const Start &started, const std::optional<ProbePixel> &wanted)
    {
        Done done;
        m_inFlight = false;
        const bool current = started.generation == m_generation;
        done.apply = current && wanted && *wanted == started.pixel;
        // The latest pixel asked for meanwhile, unless this reply already is it.
        std::optional<ProbePixel> next = m_pending;
        m_pending.reset();
        if (next && done.apply && *next == started.pixel)
            next.reset();
        if (next) {
            m_inFlight = true;
            done.next = Start{ *next, m_generation };
        }
        return done;
    }

    /// Forget everything asked for (the probe was switched off, the window is
    /// showing another cube): a reply still in flight is dropped when it lands.
    void reset()
    {
        ++m_generation;
        m_pending.reset();
        // m_inFlight stays: that reply still comes back, and finished() must
        // see it to start whatever is asked for after the reset.
    }

    bool inFlight() const { return m_inFlight; }

private:
    bool m_inFlight{ false };
    std::optional<ProbePixel> m_pending;
    int m_generation{ 0 };
};

/// The full-resolution index of voxel @p index of a preview that samples the
/// cube with @p stride along an axis of @p fullCount voxels: the backend takes
/// every stride-th voxel from the first (worker_cube_preview), so preview voxel
/// i is the cube's i × stride.
inline int previewToFullIndex(int index, int stride, int fullCount)
{
    if (fullCount <= 1)
        return 0;
    const long long full = static_cast<long long>(index) * std::max(1, stride);
    return static_cast<int>(std::clamp<long long>(full, 0, fullCount - 1));
}

}   // namespace visivo

#endif   // PROBE_SPECTRUM_FETCH_H
