#pragma once
// LayerBlendLut.h
// The step of LayerBlend.h that needs a VTK lookup table: a layer's values to
// RGBA through its table. Kept apart from LayerBlend.h so that stays free of
// VTK and unit-tested; the image viewer's Additive blend and the cube window's
// field composite both map through this, so they cannot drift.

#include "LayerBlend.h"

#include <vtkLookupTable.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace visivo::blend {

/// \a values (\a n floats) through \a lut as RGBA bytes into \a rgba (4 bytes
/// a pixel): clamped to the table's range first, and transparent where the
/// value is NaN. \a scratch is the clamped copy, reused between calls.
inline void mapThroughTable(vtkLookupTable *lut, const float *values, std::size_t n,
                            std::uint8_t *rgba, std::vector<float> &scratch)
{
    scratch.resize(n);
    const double *range = lut->GetRange();
    clampToRange(values, n, range[0], range[1], scratch.data());
    lut->MapScalarsThroughTable(scratch.data(), rgba, VTK_FLOAT, static_cast<int>(n), 1, VTK_RGBA);
    clearNoData(values, n, rgba);
}

}   // namespace visivo::blend
