#ifndef CubeNoiseTypes_h
#define CubeNoiseTypes_h

// ---------------------------------------------------------------------------
// CubeNoiseEstimateResult
//
// Plain result struct for the backend noise estimation operation.
// Kept in a separate header so both vtkWindowCube and NoiseController can
// include it without creating a circular dependency.
// ---------------------------------------------------------------------------

#include <QString>
#include <QVector>

struct CubeNoiseEstimateResult
{
    bool valid{ false };
    QString errorMessage;
    int channelStart{ 0 };
    int channelEnd{ 0 };
    int numChannels{ 0 };
    QVector<double> mad;
    QVector<double> sigma;
    int x0{ 0 };
    int x1{ 0 };
    int y0{ 0 };
    int y1{ 0 };
    // Fraction (0..1) of finite pixels in the region that were exactly 0.
    // Surfaced as a warning in the result dialog when high — see the
    // "zero-as-blank" rationale in backend compute.worker_noise_estimate.
    double blankFraction{ 0.0 };
};

#endif // CubeNoiseTypes_h
