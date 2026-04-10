#ifndef PvTypes_h
#define PvTypes_h

// ---------------------------------------------------------------------------
// PvTypes.h
//
// Shared result type for the backend PV (position-velocity) extraction.
// Kept in a separate header so both vtkWindowCube and PvController can
// include it without creating a circular dependency.
//
// RemotePvFetchResult carries the fully decoded, double-precision data
// produced by fetchRemotePv() — binary float32 payloads have already been
// converted to QVector<double> before this struct is returned.
// ---------------------------------------------------------------------------

#include <QVector>
#include <QString>

#include <array>
#include <vector>

struct RemotePvFetchResult
{
    bool valid{ false };
    QString errorMessage;
    QVector<double> positions;        // pixel-distance offset along path
    QVector<double> positionsArcsec;  // angular offset in arcseconds (empty if unavailable)
    QVector<double> values;           // flattened [numSamples × depth] intensity grid
    int numSamples{ 0 };
    int depth{ 0 };
    QString computedOn;
    int widthPixels{ 1 };
    int vertexCount{ 0 };
    double totalLength{ 0. };
    int validSamples{ 0 };
    QString spatialUnit;              // "arcsec" when physical coords are available
    double pixelScaleArcsecPerPixel{ 0. };
    QString spectralAxisType;
    QString spectralAxisUnit;

    // Vertices captured at dispatch time; passed back to the callback so
    // vtkWindowCube can build provenance strings without relying on shared
    // mutable state (pvPolylineVertices).
    std::vector<std::array<int, 2>> capturedVertices;
    int capturedWidthPixels{ 1 };
};

#endif // PvTypes_h
