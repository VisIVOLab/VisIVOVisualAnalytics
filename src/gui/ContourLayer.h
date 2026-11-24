#ifndef ContourLayer_h
#define ContourLayer_h

// ContourLayer.h
// One set of contours on a VTK renderer: the image viewer's own contours, those
// drawn from another FITS file, and the cube's slice contours are all one of
// these, so they take the same settings (ContourSettings.h): levels by count,
// sigma, logarithm or list, an optional Gaussian smoothing first, one colour or
// a colour map or a colour per level, a line width and dashed negative levels.
//
// The contours of an image are built  [thinning] -> [smoothing] -> flying edges
// -> segments coloured by level, and the negative levels joined into lines and
// cut into dashes, and they have to stay interactive on any image:
//  - an image of more than a few megapixels is thinned (one pixel in n) before
//    it is contoured, and setSettings() says so;
//  - nothing joins the segments of the solid levels (VTK's stripper takes
//    minutes on a large image); the dashes come from a walk that is linear in
//    the number of segments (ContourSettings.h, chainSegments());
//  - the build runs on a worker thread, on a private copy of what it needs, and
//    the lines that were drawn stay on screen until the new ones are ready. A
//    live input (the cube's slice) is noticed at the next render.

#include "ContourSettings.h"

#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

class QObject;
class QString;
class vtkActor;
class vtkAlgorithmOutput;
class vtkImageData;
class vtkObject;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkRenderer;
class vtkTransform;
class vtkTrivialProducer;

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

    /// Contour a fixed image (it must not change while it is contoured), or
    /// whatever a trivial producer holds, which is looked at again at each render.
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
    /// unknown pixel size for a smoothing in arcsec) and, when the image is
    /// thinned, that it is; empty when there is nothing to say.
    QString setSettings(const visivo::contours::Settings &settings);
    const visivo::contours::Settings &settings() const { return m_settings; }
    /// The levels in effect, ascending.
    const std::vector<double> &levels() const { return m_levels; }

    /// Build the contours now, on this thread, and show them (the layer
    /// otherwise builds them on a worker thread at the next render).
    void update();
    /// A build is running or waiting for one.
    bool busy() const;

    void setVisible(bool visible);
    bool visible() const { return m_visible; }

    /// The noise of an image: 1.4826 x the median absolute deviation of its
    /// pixels (a sample of them for a large one); 0 when it cannot be told.
    static double noiseOf(vtkImageData *image);
    /// What decides the levels an image starts with (a sample of its pixels).
    static visivo::contours::DataStats statsOf(vtkImageData *image);

    /// Every n-th pixel of an image of more than this many are contoured (the
    /// default keeps the first contours well under a second).
    static constexpr long long kDefaultMaxPixels = 4'000'000;
    void setMaxPixels(long long pixels);

    /// The segments of the contours as they are drawn now, with their vertices in
    /// source pixels (0-based), for the export to join into lines. @p worldToPixel
    /// maps a point of the layer's world (after its transform). Builds them
    /// first, on this thread, if they are not ready. Empty (and @p tooMany set)
    /// when there are more than @p maxSegments.
    visivo::contours::LineSource lineSource(
            const std::function<std::array<double, 2>(double, double)> &worldToPixel,
            std::size_t maxSegments, bool *tooMany = nullptr);

    /// What a build needs and gives (ContourLayer.cpp).
    struct Job;
    struct Built;

private:
    struct Impl;

    void applyStyle();
    void changed(bool geometry);
    void schedule();
    void adopt(const Built &built);
    Job makeJob(bool needGeometry, vtkImageData *image) const;
    void followCamera();
    /// The pixels of the image to be contoured, or 0.
    long long inputPixels(int *width = nullptr, int *height = nullptr) const;
    vtkImageData *liveImage() const;
    static void onRenderStart(vtkObject *caller, unsigned long event, void *client, void *data);

    std::unique_ptr<Impl> m_impl;
    vtkSmartPointer<vtkImageData> m_image;   ///< a fixed input
    vtkAlgorithmOutput *m_inputPort{ nullptr };   ///< or a live one
    vtkSmartPointer<vtkTrivialProducer> m_solidOut;
    vtkSmartPointer<vtkTrivialProducer> m_dashedOut;
    vtkSmartPointer<vtkPolyDataMapper> m_solidMapper;
    vtkSmartPointer<vtkPolyDataMapper> m_dashedMapper;
    vtkSmartPointer<vtkActor> m_solidActor;
    vtkSmartPointer<vtkActor> m_dashedActor;
    vtkSmartPointer<vtkTransform> m_transform;
    vtkWeakPointer<vtkRenderer> m_renderer;
    unsigned long m_renderObserver{ 0 };
    double m_pixelsPerWorld{ 0.0 };   ///< the view's scale the dashes were sized for
    double m_dashOn{ 6.0 }, m_dashOff{ 4.0 };   ///< in world units
    bool m_hasInput{ false };
    bool m_visible{ false };
    bool m_smoothing{ false };
    double m_sigmaWorld{ 0.0 };
    long long m_maxPixels{ kDefaultMaxPixels };
    double m_worldPerPixel{ 1.0 };
    double m_arcsecPerPixel{ 0.0 };
    visivo::contours::Settings m_settings;
    std::vector<double> m_levels;
    /// The colour of each level (0-255 RGB), as it is drawn.
    std::vector<std::array<unsigned char, 3>> m_colours;
};

#endif   // ContourLayer_h
