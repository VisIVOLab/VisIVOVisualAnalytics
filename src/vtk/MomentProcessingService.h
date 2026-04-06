#ifndef MomentProcessingService_h
#define MomentProcessingService_h

#include <vtkSmartPointer.h>

#include <QString>

#include <array>

class vtkImageData;

struct MomentRequest
{
    QString datasetPath;   // Client-local desktop path, only for error context / future staging flows.
    QString datasetId;     // Required for backend-authoritative computation.
    QString backendUrl;
    QString sessionId;
    QString backendToken;
    int order;
    int channelStart{ 0 };
    int channelEnd{ 0 };
    bool maskEnabled{ false };
    double thresholdValue{ 0.0 };
};

struct MomentResult
{
    vtkSmartPointer<vtkImageData> image;
    std::array<double, 2> imageRange{ 0., 0. };
    bool valid{ false };
    QString error;
    // Scientific metadata from the server-side WCS pipeline.
    QString momentUnit;       // e.g. "Jy/beam Hz", "km/s", "km^2/s^2"
    QString bunit;            // raw BUNIT from the FITS header
    QString spectralAxisType; // CTYPE3 value
    QString spectralAxisUnit; // CUNIT3 value
    QString wcsStatus{ QStringLiteral("ok") }; // "ok", "sanitized", or "degraded"
    QString wcsWarningMessage;                  // human-readable WCS warning, empty when ok
};

class MomentProcessingService
{
public:
    MomentProcessingService() = default;

    // Dispatches moment computation. The backend-facing implementation is authoritative and
    // requires a backend-visible dataset identifier.
    MomentResult computeMoment(const MomentRequest &request) const;

    // Authoritative backend/API path. It operates only on backend-visible datasets
    // identified by datasetId; client-local desktop paths are not considered
    // backend-openable in the normal flow.
    MomentResult computeMomentRemote(const MomentRequest &request) const;
};

#endif
