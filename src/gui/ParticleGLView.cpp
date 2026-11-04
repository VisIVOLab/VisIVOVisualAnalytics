#include "ParticleGLView.h"

#include <QJsonArray>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QDebug>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QSurfaceFormat>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace {

// Each particle an instanced quad about it, in the view's plane, reaching
// 2.5 kernel widths (backend/app/particle_raytrace.py: _CUTOFF). A kernel
// narrower than a pixel is widened to one with the same light
// (_MIN_RADIUS_PX), as the backend does.
const char *kSplatVertex = R"(#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in float a_r;
layout(location = 2) in float a_e;
layout(location = 3) in vec3 a_col;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform int u_parallel;
uniform float u_pxPerUnit;   // parallel: pixels per unit; perspective: the focal length in pixels
uniform int u_projected;
uniform float u_slab;    // a map's half thickness about the focal plane; 0: everything
uniform float u_focal;   // the focal plane's depth
out vec2 v_xy;
out vec3 v_emit;
const float CUT = 2.5;
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0, (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    vec4 c = u_view * vec4(a_pos, 1.0);
    float depth = -c.z;
    // Behind the camera, or outside a map's slab, as the backend leaves it out.
    if (depth <= 0.0 || (u_slab > 0.0 && abs(depth - u_focal) > u_slab)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_xy = vec2(9.0);
        v_emit = vec3(0.0);
        return;
    }
    float rpx = a_r * (u_parallel == 1 ? u_pxPerUnit : u_pxPerUnit / depth);
    float grown = max(rpx, 1.0);
    float w = rpx / grown;
    w *= w;
    float reach = CUT * a_r * grown / max(rpx, 1e-30);
    gl_Position = u_proj * (c + vec4(corner * reach, 0.0, 0.0));
    v_xy = corner * CUT;
    v_emit = u_projected == 1 ? vec3(a_e * w, a_e * w * a_col.x, 0.0) : a_col * (a_e * w);
}
)";

const char *kSplatFragment = R"(#version 410 core
in vec2 v_xy;
in vec3 v_emit;
out vec4 o;
void main() {
    float d2 = dot(v_xy, v_xy);
    if (d2 > 6.25)
        discard;
    o = vec4(v_emit * exp(-d2), 0.0);
}
)";

const char *kToneVertex = R"(#version 410 core
out vec2 v_uv;
void main() {
    vec2 p = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0, (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// The backend's tone_map (a 3-D render) and _projected_image (a map).
const char *kToneFragment = R"(#version 410 core
in vec2 v_uv;
out vec4 o;
uniform sampler2D u_accum;
uniform sampler2D u_lut;
uniform int u_projected;
uniform float u_exposure;
uniform int u_curve;
uniform float u_stretch;
uniform float u_gamma;
uniform float u_sat;
uniform float u_lo;
uniform float u_hi;
uniform int u_hue;
uniform int u_hueLog;
uniform float u_hueLo;
uniform float u_hueHi;
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
vec3 lut(float t) {
    return texture(u_lut, vec2(clamp(t, 0.0, 1.0) * (255.0 / 256.0) + 0.5 / 256.0, 0.5)).rgb;
}
void main() {
    vec4 a = texture(u_accum, v_uv);
    if (u_projected == 1) {
        float d = a.r;
        if (d <= 0.0) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
        float b = clamp((log(d) / log(10.0) - u_lo) / (u_hi - u_lo), 0.0, 1.0);
        vec3 rgb;
        if (u_hue == 0) {
            rgb = b > 0.0 ? lut(b) : vec3(0.0);
        } else {
            float m = a.g / d;
            float x = u_hueLog == 1 ? log(max(m, 1e-30)) / log(10.0) : m;
            float f = clamp((x - u_hueLo) / max(u_hueHi - u_hueLo, 1e-30), 0.0, 1.0);
            rgb = lut(u_hue == 1 ? 0.3 + 0.7 * f : f) * b;
        }
        o = vec4(clamp(rgb, 0.0, 1.0), 1.0);
        return;
    }
    vec3 rgb = a.rgb;
    float lum = dot(rgb, LUMA);
    float x = lum * u_exposure;
    float m;
    if (u_curve == 0) m = asinh(x * u_stretch) / asinh(u_stretch);
    else if (u_curve == 1) m = log(1.0 + x * u_stretch) / log(1.0 + u_stretch);
    else if (u_curve == 2) m = 1.0 - exp(-x);
    else m = x;
    vec3 c = lum > 0.0 ? rgb * (m / lum) : vec3(0.0);
    if (u_sat != 1.0) {
        float g = dot(c, LUMA);
        c = g + (c - g) * u_sat;
    }
    float peak = max(c.r, max(c.g, c.b));
    if (peak > 1.0)
        c /= peak;
    c = clamp(c, 0.0, 1.0);
    if (u_gamma != 1.0)
        c = pow(c, vec3(1.0 / u_gamma));
    o = vec4(c, 1.0);
}
)";

QVector3D vec(const QJsonObject &o, const QString &key)
{
    const QJsonArray a = o.value(key).toArray();
    return { float(a.at(0).toDouble()), float(a.at(1).toDouble()), float(a.at(2).toDouble()) };
}

double percentile(std::vector<float> &v, double q)
{
    if (v.empty())
        return 0.0;
    const size_t k = std::min(v.size() - 1, size_t(q / 100.0 * double(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
    return v[k];
}

} // namespace

ParticleGLView::ParticleGLView(QWidget *parent) : QOpenGLWidget(parent)
{
    QSurfaceFormat fmt = format();
    fmt.setVersion(4, 1);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSamples(0);
    setFormat(fmt);
}

ParticleGLView::~ParticleGLView()
{
    makeCurrent();
    m_splat.reset();
    m_tonePass.reset();
    m_vao.reset();
    m_quadVao.reset();
    m_vbo.reset();
    m_accum.reset();
    m_lutTexture.reset();
    doneCurrent();
}

void ParticleGLView::setParticles(const QByteArray &rows, int count, Mode mode)
{
    m_rows = rows;
    m_count = count;
    m_mode = mode;
    // The box the particles fill (the first three fields of every row).
    const int fields = mode == Mode::Projected ? 6 : 8;
    const float *f = reinterpret_cast<const float *>(rows.constData());
    m_hasBounds = count > 0;
    m_bounds = { 1e300, -1e300, 1e300, -1e300, 1e300, -1e300 };
    for (int i = 0; i < count; ++i, f += fields)
        for (int a = 0; a < 3; ++a) {
            if (!std::isfinite(f[a]))
                continue;
            m_bounds[2 * a] = std::min(m_bounds[2 * a], double(f[a]));
            m_bounds[2 * a + 1] = std::max(m_bounds[2 * a + 1], double(f[a]));
        }
    m_upload = true;
    m_stats = false;
    update();
}

void ParticleGLView::setCamera(const QJsonObject &camera)
{
    m_camera = camera;
    update();
}

void ParticleGLView::setTone(const Tone &tone)
{
    const bool hueChanged = tone.hue != m_tone.hue || tone.hueLog != m_tone.hueLog
            || tone.slab != m_tone.slab;
    m_tone = tone;
    m_lutDirty = true;
    if (hueChanged)
        m_stats = false;
    update();
}

void ParticleGLView::setOverlayShown(bool box, bool colourbar)
{
    m_showBox = box;
    m_showBar = colourbar;
    update();
}

void ParticleGLView::densityWindow(double &lo, double &hi) const
{
    const double stop = m_tone.ev * std::log10(2.0);
    hi = m_logHi - stop;
    lo = m_tone.decades > 0 ? hi - m_tone.decades : m_logLo - stop;
    if (!(hi > lo))
        lo = hi - 1.0;   // a flat map, as the backend's density_brightness
}

visivo::RaytraceOverlay ParticleGLView::overlay() const
{
    visivo::RaytraceOverlay o;
    o.box = m_showBox;
    o.colourbar = m_showBar;
    o.hasBounds = m_hasBounds;
    o.bounds = m_bounds;
    o.parallel = m_mode == Mode::Projected;
    o.title = m_tone.barTitle;
    if (m_mode == Mode::Projected) {
        if (!m_stats)
            return o;
        if (m_tone.hue == Hue::Density) {
            double lo, hi;
            densityWindow(lo, hi);
            o.lut = m_tone.lut;
            o.lo = std::pow(10.0, lo);
            o.hi = std::pow(10.0, hi);
            o.log = true;
            if (o.title.isEmpty())
                o.title = u"Column (relative)"_s;
        } else if (m_tone.hue == Hue::NamedMap) {
            // The part of the map the hue takes (from 30 % up).
            const size_t n = m_tone.lut.size();
            o.lut.assign(m_tone.lut.begin() + std::ptrdiff_t(0.3 * double(n)), m_tone.lut.end());
            o.lo = m_tone.hueLog ? std::pow(10.0, m_hueLo) : m_hueLo;
            o.hi = m_tone.hueLog ? std::pow(10.0, m_hueHi) : m_hueHi;
            o.log = m_tone.hueLog;
        } else {
            o.lut = m_tone.lut;
            o.lo = m_tone.hueMin;
            o.hi = m_tone.hueMax;
            o.log = m_tone.hueLog;
        }
        o.hasBar = !o.lut.empty() && o.hi > o.lo;
        return o;
    }
    // A 3-D render: the view's colours, when it has a colour column.
    o.lut = m_tone.lut;
    o.lo = m_tone.hueMin;
    o.hi = m_tone.hueMax;
    o.log = m_tone.hueLog;
    o.hasBar = !o.lut.empty() && o.hi > o.lo;
    return o;
}

void ParticleGLView::setImageAspect(double aspect)
{
    if (aspect > 0 && aspect != m_aspect) {
        m_aspect = aspect;
        m_stats = false;
        update();
    }
}

QRect ParticleGLView::imageRect() const
{
    const double dpr = devicePixelRatioF();
    const int W = int(width() * dpr), H = int(height() * dpr);
    int w = W, h = int(W / m_aspect);
    if (h > H) {
        h = H;
        w = int(H * m_aspect);
    }
    return { (W - w) / 2, (H - h) / 2, std::max(1, w), std::max(1, h) };
}

void ParticleGLView::initializeGL()
{
    initializeOpenGLFunctions();
    m_splat = std::make_unique<QOpenGLShaderProgram>();
    m_tonePass = std::make_unique<QOpenGLShaderProgram>();
    if (!m_splat->addShaderFromSourceCode(QOpenGLShader::Vertex, kSplatVertex)
        || !m_splat->addShaderFromSourceCode(QOpenGLShader::Fragment, kSplatFragment)
        || !m_splat->link()
        || !m_tonePass->addShaderFromSourceCode(QOpenGLShader::Vertex, kToneVertex)
        || !m_tonePass->addShaderFromSourceCode(QOpenGLShader::Fragment, kToneFragment)
        || !m_tonePass->link()) {
        emit failed(u"The GPU preview's shaders did not build: "_s + m_splat->log()
                    + m_tonePass->log());
        return;
    }
    m_vao = std::make_unique<QOpenGLVertexArrayObject>();
    m_vao->create();
    m_quadVao = std::make_unique<QOpenGLVertexArrayObject>();
    m_quadVao->create();
    m_vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
    m_vbo->create();
    m_ready = true;
}

void ParticleGLView::resizeGL(int, int)
{
    m_stats = false;   // another size: other statistics
}

void ParticleGLView::ensureTarget(const QSize &size)
{
    if (m_accum && m_accum->size() == size)
        return;
    QOpenGLFramebufferObjectFormat fmt;
    fmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fmt.setInternalTextureFormat(GL_RGBA32F);
    m_accum = std::make_unique<QOpenGLFramebufferObject>(size, fmt);
    m_stats = false;
}

void ParticleGLView::accumulate(const QSize &size)
{
    if (m_upload) {
        const int fields = m_mode == Mode::Projected ? 6 : 8;
        m_vao->bind();
        m_vbo->bind();
        m_vbo->allocate(m_rows.constData(), int(m_rows.size()));
        const int stride = fields * int(sizeof(float));
        for (int loc = 0; loc < 4; ++loc)
            glEnableVertexAttribArray(GLuint(loc));
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(0));
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(12));
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(16));
        glVertexAttribPointer(3, m_mode == Mode::Projected ? 1 : 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void *>(20));
        for (int loc = 0; loc < 4; ++loc)
            glVertexAttribDivisor(GLuint(loc), 1);
        m_vbo->release();
        m_vao->release();
        m_rows.clear();   // on the GPU now
        m_upload = false;
    }

    // The camera, as the backend projects (particle_raytrace.project): a
    // projected map always without perspective (parallel_camera).
    const QVector3D pos = vec(m_camera, u"position"_s), fp = vec(m_camera, u"focal_point"_s);
    const QVector3D up = vec(m_camera, u"view_up"_s);
    const double angle = m_camera.value(u"view_angle"_s).toDouble(30.0);
    const double dist = std::max(1e-30, double((fp - pos).length()));
    const bool parallel = m_mode == Mode::Projected || m_camera.value(u"parallel"_s).toBool();
    const double half = m_camera.value(u"parallel"_s).toBool()
            ? m_camera.value(u"parallel_scale"_s).toDouble(1.0)
            : dist * std::tan(angle * M_PI / 360.0);
    QMatrix4x4 view;
    view.lookAt(pos, fp, up);
    QMatrix4x4 proj;
    const double aspect = double(size.width()) / size.height();
    const double far = 1e6 * dist;
    if (parallel)
        proj.ortho(float(-half * aspect), float(half * aspect), float(-half), float(half), 0.0f,
                   float(far));
    else
        proj.perspective(float(angle), float(aspect), float(1e-4 * dist), float(far));
    const double pxPerUnit = parallel ? (size.height() / 2.0) / half
                                      : (size.height() / 2.0) / std::tan(angle * M_PI / 360.0);

    m_accum->bind();
    glViewport(0, 0, size.width(), size.height());
    // The overlay's QPainter may leave these on.
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);   // emission adds up, in any order
    m_splat->bind();
    m_splat->setUniformValue("u_view", view);
    m_splat->setUniformValue("u_proj", proj);
    m_splat->setUniformValue("u_parallel", parallel ? 1 : 0);
    m_splat->setUniformValue("u_pxPerUnit", float(pxPerUnit));
    m_splat->setUniformValue("u_projected", m_mode == Mode::Projected ? 1 : 0);
    m_splat->setUniformValue("u_slab", float(m_mode == Mode::Projected ? m_tone.slab / 2.0 : 0.0));
    m_splat->setUniformValue("u_focal", float(dist));
    m_vao->bind();
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, m_count);
    m_vao->release();
    m_splat->release();
    glDisable(GL_BLEND);
    m_accum->release();
}

void ParticleGLView::settle()
{
    m_stats = false;
    update();
}

void ParticleGLView::toneTo(GLuint fbo, const QSize &size)
{
    if (!m_stats) {
        // The statistics the backend grades by, from this buffer.
        std::vector<float> rgba(size_t(size.width()) * size.height() * 4);
        m_accum->bind();
        glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, rgba.data());
        m_accum->release();
        std::vector<float> a, b;
        a.reserve(rgba.size() / 4);
        if (m_mode == Mode::Light) {
            for (size_t i = 0; i < rgba.size(); i += 4) {
                const float lum = 0.2126f * rgba[i] + 0.7152f * rgba[i + 1] + 0.0722f * rgba[i + 2];
                if (lum > 0)
                    a.push_back(lum);
            }
            const double ref = percentile(a, 99.5);
            m_autoExposure = ref > 0 ? 1.0 / ref : 1.0;
        } else {
            for (size_t i = 0; i < rgba.size(); i += 4) {
                if (rgba[i] <= 0)
                    continue;
                a.push_back(std::log10(rgba[i]));
                const float mean = rgba[i + 1] / rgba[i];
                if (!m_tone.hueLog)
                    b.push_back(mean);
                else if (mean > 0)
                    b.push_back(std::log10(mean));
            }
            m_logLo = percentile(a, 1.0);
            m_logHi = percentile(a, 99.9);
            m_hueLo = percentile(b, 1.0);
            m_hueHi = percentile(b, 99.0);
        }
        m_stats = true;
    }
    if (m_lutDirty) {
        std::vector<float> rgb;
        for (const QVector3D &c : m_tone.lut)
            rgb.insert(rgb.end(), { c.x(), c.y(), c.z() });
        if (rgb.empty())
            rgb = { 0, 0, 0, 1, 1, 1 };
        m_lutTexture = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
        m_lutTexture->setSize(int(rgb.size() / 3), 1);
        m_lutTexture->setFormat(QOpenGLTexture::RGB32F);
        m_lutTexture->allocateStorage(QOpenGLTexture::RGB, QOpenGLTexture::Float32);
        m_lutTexture->setData(QOpenGLTexture::RGB, QOpenGLTexture::Float32, rgb.data());
        m_lutTexture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
        m_lutTexture->setWrapMode(QOpenGLTexture::ClampToEdge);
        m_lutDirty = false;
    }
    double lo, hi;
    densityWindow(lo, hi);
    const int curve = m_tone.curve == u"log"_s ? 1 : m_tone.curve == u"exponential"_s ? 2
                    : m_tone.curve == u"linear"_s ? 3 : 0;
    double hueLo = m_hueLo, hueHi = m_hueHi;
    if (m_tone.hue == Hue::ViewColours) {   // the view's own range
        hueLo = m_tone.hueLog ? std::log10(std::max(m_tone.hueMin, 1e-300)) : m_tone.hueMin;
        hueHi = m_tone.hueLog ? std::log10(std::max(m_tone.hueMax, 1e-300)) : m_tone.hueMax;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    m_tonePass->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_accum->texture());
    glActiveTexture(GL_TEXTURE1);
    m_lutTexture->bind(1);
    m_tonePass->setUniformValue("u_accum", 0);
    m_tonePass->setUniformValue("u_lut", 1);
    m_tonePass->setUniformValue("u_projected", m_mode == Mode::Projected ? 1 : 0);
    m_tonePass->setUniformValue("u_exposure", float(m_autoExposure * std::pow(2.0, m_tone.ev)));
    m_tonePass->setUniformValue("u_curve", curve);
    m_tonePass->setUniformValue("u_stretch", float(m_tone.stretch));
    m_tonePass->setUniformValue("u_gamma", float(m_tone.gamma));
    m_tonePass->setUniformValue("u_sat", float(m_tone.saturation));
    m_tonePass->setUniformValue("u_lo", float(lo));
    m_tonePass->setUniformValue("u_hi", float(hi));
    m_tonePass->setUniformValue("u_hue", m_tone.hue == Hue::Density ? 0
                                       : m_tone.hue == Hue::NamedMap ? 1 : 2);
    m_tonePass->setUniformValue("u_hueLog", m_tone.hueLog ? 1 : 0);
    m_tonePass->setUniformValue("u_hueLo", float(hueLo));
    m_tonePass->setUniformValue("u_hueHi", float(hueHi));
    m_quadVao->bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_quadVao->release();
    m_lutTexture->release(1);
    glActiveTexture(GL_TEXTURE0);
    m_tonePass->release();
}

void ParticleGLView::paintGL()
{
    // Frames per second while it is being turned, for the diagnostics log.
    static QElapsedTimer clock;
    static int frames = 0;
    static double busyMs = 0.0;
    QElapsedTimer busy;
    busy.start();
    const auto count = qScopeGuard([&]() {
        busyMs += busy.nsecsElapsed() / 1.0e6;
        if (!clock.isValid())
            clock.start();
        if (++frames, clock.elapsed() >= 1000) {
            qDebug().noquote() << u"[gpu-preview] %1 frames in %2 ms, %3 ms each to submit"_s
                                          .arg(frames).arg(clock.elapsed())
                                          .arg(busyMs / frames, 0, 'f', 1);
            frames = 0;
            busyMs = 0.0;
            clock.restart();
        }
    });
    const GLuint screen = defaultFramebufferObject();
    glBindFramebuffer(GL_FRAMEBUFFER, screen);
    glDisable(GL_SCISSOR_TEST);   // the last frame's overlay painter may have left it on
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_ready || m_count <= 0 || m_camera.isEmpty())
        return;
    const QRect rect = imageRect();
    ensureTarget(rect.size());
    accumulate(rect.size());
    glBindFramebuffer(GL_FRAMEBUFFER, screen);
    glViewport(rect.x(), rect.y(), rect.width(), rect.height());
    toneTo(screen, rect.size());
    // The box and colour bar, in the widget's own coordinates.
    const double dpr = devicePixelRatioF();
    QPainter painter(this);
    visivo::paintRaytraceOverlay(painter,
                                 QRectF(rect.x() / dpr, rect.y() / dpr, rect.width() / dpr,
                                        rect.height() / dpr),
                                 m_camera, overlay());
}
