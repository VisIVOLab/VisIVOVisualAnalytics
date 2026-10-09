#ifndef CatalogueWcsUtil_h
#define CatalogueWcsUtil_h

// Shared WCS coordinate-conversion helpers for the 3-D catalogue windows.
// Previously the wcscon() declaration + WCS system constants were copy-pasted
// verbatim into five vtkWindowCatalogue3D*.cpp files (E-04 dedup); this is the
// single source. Zero behaviour change — the same libwcs symbol, same constants.

#include <algorithm>
#include <cmath>

// wcscon() converts between WCS coordinate systems (FK5/J2000, Galactic, …).
// Only the declaration is needed here; the function is linked from libwcs.
extern "C" {
void wcscon(int sys1, int sys2, double eq1, double eq2,
            double *dtheta, double *dphi, double epoch);
}

// libwcs system codes.
inline constexpr int kWCS_J2000    = 1;
inline constexpr int kWCS_GALACTIC = 3;

// Galactic (l, b) → Supergalactic (SGL, SGB), both in degrees. de Vaucouleurs
// frame: SG north pole at galactic (l=47.37°, b=+6.32°); SGL=0 at galactic
// (l=137.37°, b=0°). wcstools' wcscon has no supergalactic code, so we apply the
// rotation directly by projecting the galactic unit vector onto the SG basis.
// R-13 — the natural frame for cosmic-flow figures.
inline void galacticToSupergalactic(double lDeg, double bDeg,
                                    double &sglDeg, double &sgbDeg)
{
    constexpr double deg = 3.14159265358979323846 / 180.0;
    auto unit = [](double lon, double lat, double v[3]) {
        v[0] = std::cos(lat) * std::cos(lon);
        v[1] = std::cos(lat) * std::sin(lon);
        v[2] = std::sin(lat);
    };
    double ez[3], ex[3];
    unit(47.37 * deg, 6.32 * deg, ez);    // SGZ pole (galactic)
    unit(137.37 * deg, 0.0, ex);          // SGX direction (galactic)
    // Re-orthogonalise ex against ez (Gram–Schmidt) then ey = ez × ex.
    const double dot = ex[0] * ez[0] + ex[1] * ez[1] + ex[2] * ez[2];
    for (int i = 0; i < 3; ++i)
        ex[i] -= dot * ez[i];
    const double nx = std::sqrt(ex[0] * ex[0] + ex[1] * ex[1] + ex[2] * ex[2]);
    for (int i = 0; i < 3; ++i)
        ex[i] /= nx;
    const double ey[3] = { ez[1] * ex[2] - ez[2] * ex[1],
                           ez[2] * ex[0] - ez[0] * ex[2],
                           ez[0] * ex[1] - ez[1] * ex[0] };
    double g[3];
    unit(lDeg * deg, bDeg * deg, g);
    const double sx = g[0] * ex[0] + g[1] * ex[1] + g[2] * ex[2];
    const double sy = g[0] * ey[0] + g[1] * ey[1] + g[2] * ey[2];
    const double sz = g[0] * ez[0] + g[1] * ez[1] + g[2] * ez[2];
    double sgl = std::atan2(sy, sx) / deg;
    if (sgl < 0.0)
        sgl += 360.0;
    sglDeg = sgl;
    sgbDeg = std::asin(std::max(-1.0, std::min(1.0, sz))) / deg;
}

#endif  // CatalogueWcsUtil_h
