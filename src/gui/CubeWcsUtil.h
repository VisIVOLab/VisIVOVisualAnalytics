#ifndef CubeWcsUtil_h
#define CubeWcsUtil_h

// Shared WCS-axis / celestial-coordinate formatting helpers, identical between
// the cube and image viewers (deduplicated). Free functions, no class state.

#include <QString>
#include <array>
#include <vector>

#include <vtkSmartPointer.h>

class vtkAxisActor2D;
class vtkRenderer;
class vtkTextActor;
class vtkTextProperty;

namespace CubeWcs {

void configureAxisActor(vtkAxisActor2D *axis, double x1, double y1, double x2, double y2);
void configureTickLabelActor(vtkTextActor *actor, bool rightAligned);
void configureVerticalAxisTitle(vtkAxisActor2D *axis);
/// Draw only the x tick labels that fit side by side: measures the labels as
/// they are drawn (their text already set), picks every k-th one
/// (visivo::tickLabelStride) and hides the rest. @p axisPx is the x axis' length
/// in display pixels, @p dpr scales the gap kept between labels. @p skipFirst
/// also hides the leftmost one, where the viewers that put the bottom y label in
/// the corner leave it out. Shared by every 2-D viewer's overlay.
void thinXTickLabels(vtkRenderer *renderer,
                     const std::vector<vtkSmartPointer<vtkTextActor>> &labels, double axisPx,
                     double dpr, bool skipFirst);
/// Point a VTK text property at the app's bundled Lato (extracted from resources to
/// a cache file) so the 2-D overlays share the UI font, not VTK's default (§3.0c #8).
void applyOverlayFont(vtkTextProperty *tp);
QString formatCelestialCoordinate(int frame, int axis, double value);
QString upperCtype(const QString &value);
int inferCelestialFrameFromCtypePair(const std::array<QString, 3> &ctype);
bool axisHasAnyMetadata(const QString &ctype, const QString &cunit, double crval, double crpix, double cdelt);
bool axisHasLinearWcs(const QString &ctype, double crval, double crpix, double cdelt);
bool isCelestialLikeAxis(const QString &ctypeRaw);

/// An axis of space in length units, not of the sky and not a spectrum: the X, Y
/// and Z every simulation export writes (PLUTO runs, VBT fields). The Sanity
/// check must not take such a third axis for an ambiguous spectral one.
inline bool isLinearSpatialAxis(const QString &ctypeRaw)
{
    const QString ctype = ctypeRaw.trimmed().toUpper();
    return ctype == QLatin1String("X") || ctype == QLatin1String("Y")
            || ctype == QLatin1String("Z");
}

}  // namespace CubeWcs

#endif  // CubeWcsUtil_h
