#ifndef ContourLayer_h
#define ContourLayer_h

// ContourLayer.h
// One set of contours on a VTK renderer: the image viewer's own contours, those
// drawn from another FITS file, and the cube's slice contours are all one of
// these, so they take the same settings (ContourSettings.h): levels by count,
// sigma, logarithm or list, an optional Gaussian smoothing first, one colour or
// a colour map or a colour per level, a line width and dashed negative levels.
//
// The pipeline is  input -> [smoothing] -> flying edges -> strips -> two
// filters that colour the lines and keep the solid or the dashed ones -> two
// actors  (the dash is a property of an actor, so negative levels need their
// own). Because it is a pipeline, a live input (the cube's slice) is contoured
// again whenever it changes.

#include "ContourSettings.h"

#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <array>
#include <functional>
#include <vector>

class QString;
class vtkActor;
class vtkAlgorithmOutput;
class vtkFlyingEdges2D;
class vtkImageData;
class vtkPolyDataMapper;
class vtkRenderer;
class vtkStripper;
class vtkTrivialProducer;
class vtkTransform;
class vtkVisivoContourSmooth;
class vtkVisivoContourColour;

class ContourLayer
{
public:
    ContourLayer();
    ~ContourLayer();
    ContourLayer(const ContourLayer &) = delete;
    ContourLayer &operator=(const ContourLayer &) = delete;

    /// Add the layer's actors to @p renderer (and take them from the previous
    /// one); nullptr takes them off.
    void attach(vtkRenderer *renderer);

    /// Contour a fixed image, or whatever a pipeline outputs.
    void setInputData(vtkImageData *image);
    void setInputConnection(vtkAlgorithmOutput *port);
    bool hasInput() const { return m_hasInput; }

    /// Turn the lines, for an image placed with its own rotation.
    void setUserTransform(vtkTransform *transform);

    /// Pixels to world units (1 when the world is the source pixels), and the
    /// sky size of a pixel in arcsec (0 when not known): what the smoothing's
    /// sigma is converted with.
    void setPixelScale(double worldPerPixel, double arcsecPerPixel);

    /// Apply the settings. Returns why nothing can be drawn (no levels, an
    /// unknown pixel size for a smoothing in arcsec), or an empty string.
    QString setSettings(const visivo::contours::Settings &settings);
    const visivo::contours::Settings &settings() const { return m_settings; }
    /// The levels in effect, ascending.
    const std::vector<double> &levels() const { return m_levels; }

    void setVisible(bool visible);
    bool visible() const { return m_visible; }

    /// The noise of an image: 1.4826 x the median absolute deviation of its
    /// pixels (a sample of them for a large one); 0 when it cannot be told.
    static double noiseOf(vtkImageData *image);

    /// The lines as they are drawn now, in source pixels (0-based): each level's
    /// polylines, closed where they close, with their colour and dash.
    /// @p worldToPixel maps a point of the layer's world (after its transform).
    std::vector<visivo::contours::Line> lines(
            const std::function<std::array<double, 2>(double, double)> &worldToPixel) const;

private:
    void rebuildInput();
    void applyStyle();

    vtkSmartPointer<vtkTrivialProducer> m_source;
    vtkSmartPointer<vtkVisivoContourSmooth> m_smooth;
    vtkSmartPointer<vtkFlyingEdges2D> m_edges;
    vtkSmartPointer<vtkStripper> m_strips;
    vtkSmartPointer<vtkVisivoContourColour> m_solid;
    vtkSmartPointer<vtkVisivoContourColour> m_dashed;
    vtkSmartPointer<vtkPolyDataMapper> m_solidMapper;
    vtkSmartPointer<vtkPolyDataMapper> m_dashedMapper;
    vtkSmartPointer<vtkActor> m_solidActor;
    vtkSmartPointer<vtkActor> m_dashedActor;
    vtkSmartPointer<vtkTransform> m_transform;
    vtkWeakPointer<vtkRenderer> m_renderer;
    vtkAlgorithmOutput *m_inputPort{ nullptr };
    bool m_hasInput{ false };
    bool m_visible{ false };
    bool m_smoothing{ false };
    double m_worldPerPixel{ 1.0 };
    double m_arcsecPerPixel{ 0.0 };
    visivo::contours::Settings m_settings;
    std::vector<double> m_levels;
    /// The colour of each level (0-255 RGB), as it is drawn.
    std::vector<std::array<unsigned char, 3>> m_colours;
};

#endif   // ContourLayer_h
