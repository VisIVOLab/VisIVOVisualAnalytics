#ifndef CUBE_LOD_STATUS_H
#define CUBE_LOD_STATUS_H
// CubeLodStatus.h
// The sentences the cube viewer shows about what its 3-D view is drawn from
// (issues #320 and #321): which preview it holds, and where a refined box
// adds finer detail. Pure, so the wording is unit-tested
// (tests/test_cube_lod_assembly.cpp) and stays the same in the status line,
// the data-state badge and the documentation.

#include "CubeLodGeometry.h"

#include <QString>

namespace CubeLodStatus {

/// "1st", "2nd", "3rd", "4th", "11th", "21st": every N-th voxel.
inline QString ordinal(int n)
{
    using namespace Qt::StringLiterals;
    const int mod100 = n % 100;
    const int mod10 = n % 10;
    QString suffix = u"th"_s;
    if (mod100 < 11 || mod100 > 13) {
        if (mod10 == 1)
            suffix = u"st"_s;
        else if (mod10 == 2)
            suffix = u"nd"_s;
        else if (mod10 == 3)
            suffix = u"rd"_s;
    }
    return QString::number(n) + suffix;
}

/// How a grid of samples was made, as a short phrase: "block means, pyramid
/// level 3" for a pyramid level, "every 8th voxel" for a strided read, "every
/// voxel" for the cube itself. @p source is the backend's word (pyramid,
/// strided, cube), @p level the pyramid level, @p stride cube voxels per sample.
inline QString samplingPhrase(const QString &source, int level, int stride)
{
    using namespace Qt::StringLiterals;
    if (stride <= 1 || source == u"cube"_s)
        return u"every voxel"_s;
    if (source == u"pyramid"_s)
        return level >= 0 ? u"block means, pyramid level %1"_s.arg(level) : u"block means"_s;
    return u"every %1 voxel"_s.arg(ordinal(stride));
}

/// The status line's word on the preview: "Preview: block means, pyramid
/// level 3" or "Preview: every 8th voxel".
inline QString previewText(const QString &source, int level, int stride)
{
    using namespace Qt::StringLiterals;
    return u"Preview: "_s + samplingPhrase(source, level, stride);
}

/// "x 512..767, y 0..255, z 1024..1279": a half-open box of cube voxels, as
/// the inclusive ranges a user reads elsewhere in the viewer.
inline QString boxText(const CubeLodGeometry::Box &box)
{
    using namespace Qt::StringLiterals;
    return u"x %1..%2, y %3..%4, z %5..%6"_s.arg(box.lo[0])
            .arg(box.hi[0] - 1)
            .arg(box.lo[1])
            .arg(box.hi[1] - 1)
            .arg(box.lo[2])
            .arg(box.hi[2] - 1);
}

/// The status line while a refined box is shown: "Detail: level 0 in x ..,
/// y .., z ..; elsewhere block means, pyramid level 3".
inline QString detailText(int fineLevel, const QString &fineSource, const CubeLodGeometry::Box &box,
                          const QString &previewSource, int previewLevel, int previewStride)
{
    using namespace Qt::StringLiterals;
    QString fine = u"level %1"_s.arg(fineLevel);
    if (fineLevel > 0 && fineSource == u"strided"_s)
        fine += u" (every %1 voxel)"_s.arg(ordinal(int(CubeLodGeometry::levelScale(fineLevel))));
    return u"Detail: %1 in %2; elsewhere %3"_s.arg(
            fine, boxText(box), samplingPhrase(previewSource, previewLevel, previewStride));
}

}   // namespace CubeLodStatus

#endif   // CUBE_LOD_STATUS_H
