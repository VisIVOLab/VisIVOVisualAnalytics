#ifndef LinkedViewFrame_h
#define LinkedViewFrame_h

// LinkedViewFrame.h
// What a "height" means on each side of a Link Views between an image window and
// a cube window (issue #522), and the conversion between them. Pure, so it is
// unit-tested without a render window (tests/test_astro_match.cpp).
//
// An image window has no margins: its camera frames the whole viewport, so the
// view it shares is the viewport's centre and height, in the pixels of its
// image. A cube window frames its 2-D views inside a gutter (axes, colour bar)
// and shares them gutter-independently (CubeGeom::readNormalizedCamera): the
// world point at the centre of the DATA AREA and the world height of the data
// area, which is a fraction of the viewport. Handing one number to the other
// as if they were the same shows more or less sky than the other window does;
// across two pixel scales the angular heights then differ by that fraction.
// The cube window brings its side to the viewport's frame here, before the sky
// matcher (AstroUtils::matchView) scales the height, and back after it.

#include "AstroUtils.h"

namespace visivo::linkview {

/// The viewport of a 2-D view and the data area inside it, in device pixels
/// (y up, as VTK's display coordinates are).
struct Frame
{
    double width{ 0.0 };
    double height{ 0.0 };
    double areaX{ 0.0 };
    double areaY{ 0.0 };
    double areaWidth{ 0.0 };
    double areaHeight{ 0.0 };

    bool valid() const { return width > 0.0 && height > 0.0 && areaWidth > 0.0 && areaHeight > 0.0; }
};

/// A gutter-independent view as a cube window shares it (the data area's
/// centre and the world height of the data area), in the world of its view.
struct Normalized
{
    double cx{ 0.0 };
    double cy{ 0.0 };
    double height{ 0.0 };
};

/// The centre and height of the whole viewport for a view @p n framed in @p f.
inline AstroUtils::View viewportOf(const Frame &f, const Normalized &n)
{
    const double perPixel = n.height / f.areaHeight;   // world per device pixel
    return { n.cx - (f.areaX + 0.5 * f.areaWidth - 0.5 * f.width) * perPixel,
             n.cy - (f.areaY + 0.5 * f.areaHeight - 0.5 * f.height) * perPixel,
             f.height * perPixel };
}

/// The inverse of viewportOf(): the view whose viewport is @p v, as a window
/// with frame @p f shares it.
inline Normalized normalizedOf(const Frame &f, const AstroUtils::View &v)
{
    const double perPixel = v.height / f.height;
    return { v.cx + (f.areaX + 0.5 * f.areaWidth - 0.5 * f.width) * perPixel,
             v.cy + (f.areaY + 0.5 * f.areaHeight - 0.5 * f.height) * perPixel,
             f.areaHeight * perPixel };
}

}   // namespace visivo::linkview

#endif   // LinkedViewFrame_h
