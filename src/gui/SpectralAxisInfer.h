#ifndef SPECTRAL_AXIS_INFER_H
#define SPECTRAL_AXIS_INFER_H

#include "SpectralAxisTypes.h"

#include <QString>

// CTYPE→spectral-axis inference, lifted verbatim from the anonymous namespace of
// vtkWindowCube_Wcs.cpp (E-04) into a testable, VTK-free header. Encodes the FITS
// spectral-axis convention knowledge (FREQ / VRAD / VOPT / FELO / VELO / CHAN →
// Frequency / Radio- / Optical- / Generic-velocity / Channel / GenericSpectral)
// with its subtle branch ordering, which had no unit tests where it lived.
//
// The trivial helpers upperCtype (trimmed().toUpper()) and cleanAxisUnit
// (trimmed()) are inlined here so the header pulls in no VTK / libwcs.
namespace spectralaxis {

/// Human-readable label for a spectral-axis kind (fallback when a descriptor has
/// no CTYPE-derived source label).
inline QString spectralAxisKindLabel(SpectralAxisKind kind)
{
    switch (kind) {
    case SpectralAxisKind::Frequency:
        return QStringLiteral("Frequency");
    case SpectralAxisKind::RadioVelocity:
        return QStringLiteral("Radio Velocity");
    case SpectralAxisKind::OpticalVelocity:
        return QStringLiteral("Optical Velocity");
    case SpectralAxisKind::GenericVelocity:
        return QStringLiteral("Velocity");
    case SpectralAxisKind::GenericSpectral:
        return QStringLiteral("Spectral Axis");
    case SpectralAxisKind::Channel:
    default:
        return QStringLiteral("Channel");
    }
}

/// Infer the spectral axis from its FITS CTYPE + unit. @p hasPhysicalCoordinates
/// is false when the axis carries no WCS (→ raw Channel). Branch order matters:
/// VOPT/FELO are tested before the generic VELO fallback, and an explicit CHANnel
/// CTYPE demotes the axis to non-physical.
inline SpectralAxisDescriptor inferSpectralAxisDescriptor(const QString &ctypeRaw,
                                                          const QString &unitRaw,
                                                          bool hasPhysicalCoordinates)
{
    SpectralAxisDescriptor descriptor;
    const QString ctype = ctypeRaw.trimmed().toUpper();   // was CubeWcs::upperCtype
    descriptor.unit = unitRaw.trimmed();                  // was cleanAxisUnit
    descriptor.sourceLabel = ctypeRaw.trimmed();

    if (!hasPhysicalCoordinates) {
        descriptor.kind = SpectralAxisKind::Channel;
        descriptor.label = QStringLiteral("Channel");
        descriptor.sourceLabel = QStringLiteral("Channel");
        return descriptor;
    }

    descriptor.physical = true;
    if (ctype.startsWith(QStringLiteral("FREQ"))) {
        descriptor.kind = SpectralAxisKind::Frequency;
        descriptor.label = QStringLiteral("Frequency");
        descriptor.trusted = true;
    } else if (ctype.startsWith(QStringLiteral("VRAD"))) {
        descriptor.kind = SpectralAxisKind::RadioVelocity;
        descriptor.label = QStringLiteral("Radio Velocity");
        descriptor.trusted = true;
    } else if (ctype.startsWith(QStringLiteral("VOPT")) || ctype.startsWith(QStringLiteral("FELO"))) {
        descriptor.kind = SpectralAxisKind::OpticalVelocity;
        descriptor.label = QStringLiteral("Optical Velocity");
        descriptor.trusted = true;
    } else if (ctype.startsWith(QStringLiteral("VELO")) || ctype.contains(QStringLiteral("VELO"))) {
        descriptor.kind = SpectralAxisKind::GenericVelocity;
        descriptor.label = QStringLiteral("Velocity");
        descriptor.inferred = true;
    } else if (ctype.contains(QStringLiteral("CHAN")) || ctype == QStringLiteral("CHANNEL")) {
        descriptor.kind = SpectralAxisKind::Channel;
        descriptor.label = QStringLiteral("Channel");
        descriptor.sourceLabel = QStringLiteral("Channel");
        descriptor.physical = false;
    } else {
        descriptor.kind = SpectralAxisKind::GenericSpectral;
        descriptor.label = ctypeRaw.trimmed().isEmpty() ? QStringLiteral("Spectral Axis")
                                                        : ctypeRaw.trimmed();
        descriptor.inferred = !ctypeRaw.trimmed().isEmpty();
    }

    if (descriptor.sourceLabel.isEmpty()) {
        descriptor.sourceLabel = spectralAxisKindLabel(descriptor.kind);
    }
    return descriptor;
}

}  // namespace spectralaxis

#endif  // SPECTRAL_AXIS_INFER_H
