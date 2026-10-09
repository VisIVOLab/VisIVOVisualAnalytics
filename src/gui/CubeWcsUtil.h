#ifndef CubeWcsUtil_h
#define CubeWcsUtil_h

// Shared WCS-axis / celestial-coordinate formatting helpers, identical between
// the cube and image viewers (deduplicated). Free functions, no class state.

#include <QString>
#include <array>

class vtkAxisActor2D;
class vtkTextActor;
class vtkTextProperty;

namespace CubeWcs {

void configureAxisActor(vtkAxisActor2D *axis, double x1, double y1, double x2, double y2);
void configureTickLabelActor(vtkTextActor *actor, bool rightAligned);
void configureVerticalAxisTitle(vtkAxisActor2D *axis);
/// Point a VTK text property at the app's bundled Lato (extracted from resources to
/// a cache file) so the 2-D overlays share the UI font, not VTK's default (§3.0c #8).
void applyOverlayFont(vtkTextProperty *tp);
QString formatCelestialCoordinate(int frame, int axis, double value);
QString upperCtype(const QString &value);
int inferCelestialFrameFromCtypePair(const std::array<QString, 3> &ctype);
bool axisHasAnyMetadata(const QString &ctype, const QString &cunit, double crval, double crpix, double cdelt);
bool axisHasLinearWcs(const QString &ctype, double crval, double crpix, double cdelt);
bool isCelestialLikeAxis(const QString &ctypeRaw);

}  // namespace CubeWcs

#endif  // CubeWcsUtil_h
