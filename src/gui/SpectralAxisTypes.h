#ifndef SPECTRAL_AXIS_TYPES_H
#define SPECTRAL_AXIS_TYPES_H

#include <QString>

// Spectral-axis classification types, lifted verbatim out of vtkWindowCube
// (E-04 god-object decomposition) into a standalone, VTK-free header so the
// CTYPE→kind inference that uses them can be unit-tested headless. vtkWindowCube
// keeps `vtkWindowCube::SpectralAxisKind` / `::SpectralAxisDescriptor` working via
// `using` aliases, so no call site changes.

/// The physical nature of a cube's third (spectral) axis, inferred from its FITS
/// CTYPE. Channel = no usable physical coordinate (raw index).
enum class SpectralAxisKind
{
    Channel,
    Frequency,
    RadioVelocity,
    OpticalVelocity,
    GenericVelocity,
    GenericSpectral,
};

/// A resolved description of the spectral axis: its kind, unit, display labels,
/// and confidence flags (trusted = matched a standard CTYPE; inferred = guessed;
/// physical = carries a real world coordinate rather than a bare channel index).
struct SpectralAxisDescriptor
{
    SpectralAxisKind kind{ SpectralAxisKind::Channel };
    QString unit;
    QString label;
    QString sourceLabel;
    bool trusted{ false };
    bool inferred{ false };
    bool physical{ false };
};

#endif  // SPECTRAL_AXIS_TYPES_H
