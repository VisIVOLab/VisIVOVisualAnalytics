#ifndef ImageLayerLoadTask_h
#define ImageLayerLoadTask_h

#include <vtkSmartPointer.h>

#include <array>
#include <string>

class vtkImageData;

struct ImageLayerLoadRequest
{
    std::string masterFilepath;
    std::string layerFilepath;
    /// Add the layer even though it covers none of the master's sky. The sky
    /// bounds come from `wcsrange()`, which is a bounding box: for a rotated
    /// field straddling RA = 0 it can be narrower than the image really is, so
    /// the user is given a way to say "I know, add it anyway".
    bool ignoreOverlap{ false };
};

struct ImageLayerLoadResult
{
    bool valid{ false };
    std::string errorMessage;
    /// The layer covers none of the sky the master image covers. Not an error
    /// in the file — the viewer offers to open it in a window of its own.
    bool noOverlap{ false };
    std::string filepath;
    vtkSmartPointer<vtkImageData> imageData;
    std::array<double, 2> scalarRange{ 0., 0. };
    std::array<double, 3> spacing{ 1., 1., 1. };
    std::array<double, 3> origin{ 0., 0., 0. };
    double rotationDegrees{ 0. };
    bool isPreview{ false };
    int fullWidth{ 0 };
    int fullHeight{ 0 };
    int numLevels{ 1 };   ///< pyramid levels available (for viewport-driven LOD gating)
    double previewScaleFactor{ 1.0 };
    int requestGeneration{ 0 };
};

ImageLayerLoadResult loadImageLayer(const ImageLayerLoadRequest &request);

#endif
