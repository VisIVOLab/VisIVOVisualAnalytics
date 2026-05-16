#ifndef CubeOpenPreviewTask_h
#define CubeOpenPreviewTask_h

#include <vtkSmartPointer.h>

#include <QString>

#include <array>

class vtkImageData;

struct CubeOpenStageResult
{
    bool valid{ false };
    QString errorMessage;
    vtkSmartPointer<vtkImageData> cubeImageData;
    vtkSmartPointer<vtkImageData> momentImageData;
    std::array<double, 2> cubeRange{ 0., 0. };
    std::array<double, 2> momentRange{ 0., 0. };
    std::array<int, 6> dataExtent{ 0, -1, 0, -1, 0, -1 };
    double cubeMean{ 0. };
    double cubeRms{ 0. };
    // When true, cubeImageData has already been sanitized off-thread:
    // NaN voxels were replaced with invisibleSentinel, cubeRange holds the
    // finite-only visible range. The GUI thread can skip its own pass.
    bool preSanitized{ false };
    double invisibleSentinel{ -1.0 };
    // Fraction of NaN/inf voxels found during sanitization (0..1). Cached so
    // the sanity panel does not need to rescan the volume on the GUI thread.
    double blankFraction{ 0. };
};

CubeOpenStageResult loadCubeOpenPreview(const QString &filepath);
CubeOpenStageResult loadCubeOpenFull(const QString &filepath);

#endif
