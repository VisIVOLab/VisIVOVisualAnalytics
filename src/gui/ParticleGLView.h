#ifndef PARTICLEGLVIEW_H
#define PARTICLEGLVIEW_H

// The ray-traced render's preview on this machine's GPU (issue #101): the
// particles the backend prepared (/v1/particle_render/particles), each a
// Gaussian kernel summed into a floating-point buffer, then toned as the
// backend tones its frames (backend/app/particle_raytrace.py: tone_map, and
// routers/particle_render.py: _projected_image). Turning the view redraws it
// here at the screen's pace; nothing goes back to the backend.
//
// Or a grid (issue #106): a cube's emissivity and colour index, as the backend
// prepared them (/v1/volume_render/grid), ray-marched in a fragment shader into
// the same buffer, front to back with absorption, as backend/app/
// volume_raytrace.py marches them; then toned the same way.
//
// Particles: emission only. A 3-D render's absorption composites the particles
// in depth order, which the backend's still does. The statistics a picture is graded
// by (the automatic exposure, the density window, the hue's range) are read
// back from the buffer when the view settles, and kept while it turns, as the
// backend's drafts keep the last sharp frame's exposure.

#include "RaytraceOverlay.h"

#include <QJsonObject>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>
#include <QVector3D>

#include <array>
#include <memory>
#include <vector>

class QOpenGLBuffer;
class QOpenGLFramebufferObject;
class QOpenGLShaderProgram;
class QOpenGLTexture;
class QOpenGLVertexArrayObject;

class ParticleGLView : public QOpenGLWidget, protected QOpenGLExtraFunctions
{
    Q_OBJECT

public:
    enum class Mode { Light, Projected };
    /// A map's hue: the density, a named map over the mean quantity's spread,
    /// the view's colours over their range, or (a grid) the mean colour index
    /// itself, the table spanning [hueMin, hueMax] of the colour's quantity.
    enum class Hue { Density, NamedMap, ViewColours, Index };

    struct Tone
    {
        // A 3-D render.
        QString curve = QStringLiteral("asinh");
        double stretch = 10.0;
        double gamma = 2.2;
        double saturation = 1.0;
        // Both: stops over the automatic exposure (a map: over its window).
        double ev = 0.0;
        // A projected map: decades of density shown (0: automatic), its hue.
        double decades = 0.0;
        Hue hue = Hue::Density;
        bool hueLog = false;
        double hueMin = 0.0, hueMax = 1.0;   ///< the view's colours' range (ViewColours)
        std::vector<QVector3D> lut;          ///< 256 colours
        double slab = 0.0;   ///< a map's thickness about the focal plane (0: everything)
        /// A grid: the optical depth where the light is brightest (0: none),
        /// and the samples per voxel along a ray.
        double absorption = 0.0;
        double quality = 2.0;
        /// The colour bar's title (the colour column, or the map's quantity).
        /// A 3-D render's bar is the view's colours: ``lut`` over
        /// [hueMin, hueMax], on a log scale with ``hueLog``; none without a lut.
        QString barTitle;
    };

    explicit ParticleGLView(QWidget *parent = nullptr);
    ~ParticleGLView() override;

    /// Rows of little-endian float32: x y z r e red green blue (Light) or
    /// x y z r w v (Projected), as the backend sends them.
    void setParticles(const QByteArray &rows, int count, Mode mode);

    /// A grid, as the backend sends it: the emissivity as float32 and the
    /// colour index as float16, nz·ny·nx each, x fastest; the world box its
    /// points span (x0, x1, y0, y1, z0, z1).
    struct Grid
    {
        QByteArray emission;
        QByteArray index;
        int nx = 0, ny = 0, nz = 0;
        std::array<double, 6> box{};
    };
    void setGrid(Grid grid, Mode mode);
    /// Something to draw: particles or a grid.
    bool hasParticles() const { return m_count > 0 || m_hasGrid; }
    void setCamera(const QJsonObject &camera);
    void setTone(const Tone &tone);
    /// The picture's shape, letterboxed in the widget.
    void setImageAspect(double aspect);
    /// Reads the buffer back and grades the picture by it (call when the view
    /// settles). The automatic exposure found: the backend's for a 3-D render.
    void settle();
    double autoExposure() const { return m_autoExposure; }
    void setOverlayShown(bool box, bool colourbar);
    /// The box and colour bar of the picture as graded now (the stills and
    /// movie frames get the same).
    visivo::RaytraceOverlay overlay() const;

signals:
    /// GL could not start or draw (an old driver): the caller falls back.
    void failed(const QString &reason);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    QRect imageRect() const;
    /// The map's brightness window, log10 of the density, as the tone pass
    /// uses it (the backend's density_brightness).
    void densityWindow(double &lo, double &hi) const;
    void ensureTarget(const QSize &size);
    void ensureLut();
    void accumulate(const QSize &size);
    /// A grid's rays into the bound framebuffer: light with absorption
    /// ``kappa``, a map's two moments, or (``grey``) the emission alone.
    void march(const QSize &size, double kappa, bool grey);
    /// The grid's absorption coefficient for the view: as the backend's,
    /// from the brightest column of an emission-only pass at a quarter size.
    double gridKappa(const QSize &size);
    void toneTo(GLuint fbo, const QSize &size);
    void toneWith(GLuint fbo, double lo, double hi, int curve, double hueLo, double hueHi);

    bool m_ready{ false };
    Mode m_mode{ Mode::Light };
    int m_count{ 0 };
    QByteArray m_rows;
    bool m_upload{ false };
    QJsonObject m_camera;
    Tone m_tone;
    bool m_lutDirty{ true };
    double m_aspect{ 16.0 / 9.0 };

    // Graded by the last settled buffer.
    double m_autoExposure{ 1.0 };
    double m_logLo{ 0.0 }, m_logHi{ 1.0 };      ///< log10 density window, before ev
    double m_hueLo{ 0.0 }, m_hueHi{ 1.0 };      ///< hue range (in log10 when hueLog)
    bool m_stats{ false };
    std::array<double, 6> m_bounds{};
    bool m_hasBounds{ false };
    bool m_showBox{ true };
    bool m_showBar{ true };

    std::unique_ptr<QOpenGLShaderProgram> m_splat;
    std::unique_ptr<QOpenGLShaderProgram> m_tonePass;
    std::unique_ptr<QOpenGLVertexArrayObject> m_vao;
    std::unique_ptr<QOpenGLVertexArrayObject> m_quadVao;
    std::unique_ptr<QOpenGLBuffer> m_vbo;
    std::unique_ptr<QOpenGLFramebufferObject> m_accum;
    std::unique_ptr<QOpenGLTexture> m_lutTexture;

    // A grid.
    bool m_hasGrid{ false };
    Grid m_grid;
    bool m_gridUpload{ false };
    double m_kappa{ 0.0 };
    bool m_kappaStale{ true };
    std::unique_ptr<QOpenGLShaderProgram> m_march;
    std::unique_ptr<QOpenGLTexture> m_emissionTexture;
    std::unique_ptr<QOpenGLTexture> m_indexTexture;
    std::unique_ptr<QOpenGLFramebufferObject> m_quarter;
};

#endif // PARTICLEGLVIEW_H
