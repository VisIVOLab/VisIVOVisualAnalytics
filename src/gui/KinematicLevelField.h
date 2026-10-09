#ifndef KinematicLevelField_h
#define KinematicLevelField_h

#include <QByteArray>

#include <algorithm>
#include <array>
#include <limits>

/// Thresholding of the Kinematic Lasso's geodesic LEVEL FIELD.
///
/// The two-cube lasso validation found that the useful ``reach`` tracks a
/// source's surface brightness, cannot be derived from header metadata, and so
/// has to stay a control the user drags (validation/README.md). The backend
/// therefore serves ``/v1/cube/kinematic_levels_bin``: one geodesic grow up to a
/// maximum budget, returned as two uint8 fields, from which EVERY smaller budget
/// is recovered by a threshold — no round trip per slider step.
///
/// This lives apart from the viewer so it can be unit-tested: the selection at a
/// budget is pure index arithmetic over two byte arrays, which is exactly the
/// kind of code where an axis-order or bbox slip is silent, and the viewer that
/// calls it needs VTK and QtWidgets that the headless suite does not link.
namespace KinematicLevelField {

/// Level meaning "not reachable within the field's budget" (see the backend's
/// ``_LEVEL_UNREACHABLE``). Any threshold excludes it, since budgets are < 255.
inline constexpr int kUnreachable = 255;

/// Largest ROI side accepted. Dimensions arrive from response headers, so they
/// are bounded individually BEFORE being multiplied: three values near INT_MAX
/// overflow even a 64-bit product, and the size check would then pass on
/// nonsense.
inline constexpr int kMaxRoiSide = 1 << 20;

struct Input
{
    QByteArray addLevels;   ///< uint8, w*h*d, x-fastest
    QByteArray subLevels;   ///< uint8, same geometry; all-255 when nothing is carved
    int width = 0, height = 0, depth = 0;
    int originX = 0, originY = 0, originZ = 0;   ///< ROI origin in full-cube voxels
    /// Full cube shape. Supplied so the ROI can be checked to LIE INSIDE the cube:
    /// the bbox is reported in cube voxels, so a stale or malformed response with a
    /// negative origin — or one whose ROI runs off the end — would otherwise yield
    /// coordinates outside the data that the caller then hands to Apply.
    int fullWidth = 0, fullHeight = 0, fullDepth = 0;
};

struct Result
{
    bool valid = false;         ///< false = malformed input, NOT "empty selection"
    int count = 0;              ///< selected voxels; 0 is a legitimate answer
    QByteArray mask;            ///< little-endian float32 0/1, x-fastest (ROI-sized)
    std::array<int, 6> bbox{ 0, 0, 0, 0, 0, 0 };   ///< x0,x1,y0,y1,z0,z1, FULL-cube voxels
};

/// True when the geometry is self-consistent, sits inside the cube, and is small
/// enough to index with an int-sized byte array.
inline bool isWellFormed(const Input &in)
{
    if (in.width <= 0 || in.height <= 0 || in.depth <= 0)
        return false;
    if (in.width > kMaxRoiSide || in.height > kMaxRoiSide || in.depth > kMaxRoiSide)
        return false;
    // Origin: never negative, and the ROI must end within the cube. Checked with
    // 64-bit arithmetic so an extreme origin cannot overflow its way back into
    // range. A zero full shape means the caller did not supply one — accept the
    // ROI on its own terms rather than rejecting every input.
    if (in.originX < 0 || in.originY < 0 || in.originZ < 0)
        return false;
    const auto fits = [](int origin, int extent, int full) {
        return full <= 0 || qint64(origin) + qint64(extent) <= qint64(full);
    };
    if (!fits(in.originX, in.width, in.fullWidth)
        || !fits(in.originY, in.height, in.fullHeight)
        || !fits(in.originZ, in.depth, in.fullDepth))
        return false;
    const qsizetype n = qsizetype(in.width) * in.height * in.depth;
    if (in.addLevels.size() != n || in.subLevels.size() != n)
        return false;
    // The mask below is built with an int length.
    return n <= qsizetype(std::numeric_limits<int>::max()) / qsizetype(sizeof(float));
}

/// Selection at an integer budget: ``add <= reach`` minus ``sub <= reach``.
///
/// Integer budgets only, and that is what makes this exact rather than an
/// approximation of a per-step grow: the backend stores ``ceil(distance)``, and
/// for integer r, ``ceil(d) <= r`` iff ``d <= r``. A negative budget selects
/// nothing; the seed sits at level 0, so budget 0 already selects it.
inline Result thresholdAt(const Input &in, int reach)
{
    Result out;
    if (!isWellFormed(in))
        return out;
    out.valid = true;

    const qsizetype n = qsizetype(in.width) * in.height * in.depth;
    out.mask = QByteArray(int(n) * int(sizeof(float)), Qt::Uninitialized);
    auto *dst = reinterpret_cast<float *>(out.mask.data());
    const auto *add = reinterpret_cast<const quint8 *>(in.addLevels.constData());
    const auto *sub = reinterpret_cast<const quint8 *>(in.subLevels.constData());

    if (reach < 0) {   // nothing is <= a negative budget; keep the mask all-zero
        std::fill(dst, dst + n, 0.0f);
        return out;
    }
    const quint8 thr = quint8(std::clamp(reach, 0, 254));

    int x0 = in.width, x1 = -1, y0 = in.height, y1 = -1, z0 = in.depth, z1 = -1;
    for (qsizetype i = 0; i < n; ++i) {
        const bool on = (add[i] <= thr) && !(sub[i] <= thr);
        dst[i] = on ? 1.0f : 0.0f;
        if (!on)
            continue;
        ++out.count;
        const int x = int(i % in.width);
        const int y = int((i / in.width) % in.height);
        const int z = int(i / (qsizetype(in.width) * in.height));
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
        z0 = std::min(z0, z); z1 = std::max(z1, z);
    }
    if (out.count > 0) {
        // Report in FULL-cube voxels: Apply and the 2-D overlay read the bbox the
        // same way whichever path produced the selection.
        out.bbox = { in.originX + x0, in.originX + x1,
                     in.originY + y0, in.originY + y1,
                     in.originZ + z0, in.originZ + z1 };
    }
    return out;
}

}  // namespace KinematicLevelField

#endif  // KinematicLevelField_h
