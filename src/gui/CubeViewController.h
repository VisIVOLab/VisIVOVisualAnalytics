#ifndef CubeViewController_h
#define CubeViewController_h

#include <array>

class AstroUtils;
class vtkActor;
class vtkExtractVOI;
class vtkFlyingEdges2D;
class vtkFlyingEdges3D;
class vtkImageReslice;
class vtkLegendScaleActorWCS;
class vtkLookupTable;
class vtkPiecewiseFunction;
class vtkRenderer;
class vtkTrivialProducer;
class vtkVolume;

struct CubeViewContext
{
    vtkTrivialProducer *cubeDisplaySource;
    const AstroUtils &astro;
    vtkRenderer *cubeRenderer;
    vtkActor *isosurface;
    vtkVolume *volume;
    vtkFlyingEdges3D *isosurfaceFilter;
    vtkPiecewiseFunction *volumeOpacity;
    vtkImageReslice *slice;
    vtkExtractVOI *sliceOnCube;
    vtkLookupTable *lutSlice;
    vtkLookupTable *lutSliceOnCube;
    vtkFlyingEdges2D *contours;
    vtkActor *contoursActor;
    vtkLookupTable *lutMoment;
    vtkLegendScaleActorWCS *legendSlice;
    vtkLegendScaleActorWCS *legendMoment;
};

class CubeViewController
{
public:
    struct SliceUpdateResult
    {
        bool valid;
        double spectralValue;
        std::array<double, 2> imageRange;
    };

    explicit CubeViewController(const CubeViewContext &context);

    void updateCube(double threshold) const;
    SliceUpdateResult updateSlice(int sliceIndex) const;
    void setContoursVisible(bool visible) const;
    void updateContours(int levels, double lowerBound, double upperBound) const;
    void setLegendWcs(int wcs) const;
    void syncSlicesLut() const;
    void setCubeRenderMode(bool isosurfaceMode) const;

private:
    CubeViewContext context;
};

#endif
