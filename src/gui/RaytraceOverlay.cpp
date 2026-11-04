#include "RaytraceOverlay.h"

#include "VolumeColorbar.h"

#include <QFont>
#include <QFontMetrics>
#include <QJsonArray>
#include <QLinearGradient>
#include <QMatrix4x4>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace visivo {

namespace {

QVector3D vec(const QJsonObject &o, const QString &key)
{
    const QJsonArray a = o.value(key).toArray();
    return { float(a.at(0).toDouble()), float(a.at(1).toDouble()), float(a.at(2).toDouble()) };
}

/// The box's twelve edges, projected as the renderer projects (its camera:
/// particle_raytrace.project, ParticleGLView).
void paintBox(QPainter &p, const QRectF &target, const QJsonObject &camera,
              const RaytraceOverlay &o)
{
    const QVector3D pos = vec(camera, u"position"_s), fp = vec(camera, u"focal_point"_s);
    const double angle = camera.value(u"view_angle"_s).toDouble(30.0);
    const double dist = std::max(1e-30, double((fp - pos).length()));
    const bool parallel = o.parallel || camera.value(u"parallel"_s).toBool();
    const double half = camera.value(u"parallel"_s).toBool()
            ? camera.value(u"parallel_scale"_s).toDouble(1.0)
            : dist * std::tan(angle * M_PI / 360.0);
    QMatrix4x4 view;
    view.lookAt(pos, fp, vec(camera, u"view_up"_s));
    QMatrix4x4 proj;
    const double aspect = target.width() / std::max(1.0, target.height());
    if (parallel)
        proj.ortho(float(-half * aspect), float(half * aspect), float(-half), float(half), 0.0f,
                   float(1e6 * dist));
    else
        proj.perspective(float(angle), float(aspect), float(1e-4 * dist), float(1e6 * dist));
    const QMatrix4x4 m = proj * view;
    const auto &b = o.bounds;
    std::array<QVector4D, 8> corners;
    for (int i = 0; i < 8; ++i)
        corners[i] = m * QVector4D(float(b[(i & 1) ? 1 : 0]), float(b[(i & 2) ? 3 : 2]),
                                   float(b[(i & 4) ? 5 : 4]), 1.0f);
    const auto toPixel = [&](const QVector4D &c) {
        return QPointF(target.left() + (c.x() / c.w() + 1.0) / 2.0 * target.width(),
                       target.top() + (1.0 - c.y() / c.w()) / 2.0 * target.height());
    };
    QPen pen(QColor(200, 210, 225, 170));
    pen.setWidthF(std::max(1.0, target.height() / 900.0));
    p.setPen(pen);
    for (int i = 0; i < 8; ++i)
        for (int bit : { 1, 2, 4 }) {
            const int j = i | bit;
            if (j == i)
                continue;
            // An edge reaching behind the camera is left out.
            if (corners[i].w() <= 0 || corners[j].w() <= 0)
                continue;
            p.drawLine(toPixel(corners[i]), toPixel(corners[j]));
        }
}

/// The bar as the 3-D view's own (VolumeColorbar): the same place, size and
/// type, so it does not change with the style; a still larger than the view
/// draws it larger by the same factor.
void paintBar(QPainter &p, const QRectF &target, const RaytraceOverlay &o)
{
    if (o.lut.empty() || !(o.hi > o.lo))
        return;
    VolumeColorbar::Look look;
    look.colours.reserve(int(o.lut.size()));
    for (const QVector3D &c : o.lut)
        look.colours << QColor::fromRgbF(std::clamp(c.x(), 0.0f, 1.0f),
                                         std::clamp(c.y(), 0.0f, 1.0f),
                                         std::clamp(c.z(), 0.0f, 1.0f));
    const bool log = o.log && o.lo > 0;
    const auto value = [&](double f) {
        return log ? o.lo * std::pow(o.hi / o.lo, f) : o.lo + (o.hi - o.lo) * f;
    };
    bool exponent = false;
    for (const double f : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
        look.ticks << qMakePair(f, QString::number(value(f), 'g', 3));
        exponent = exponent || look.ticks.back().second.contains(u'e');
    }
    // One notation down the bar, as the 3-D view's.
    if (exponent)
        for (auto &tick : look.ticks)
            tick.second = QString::number(value(tick.first), 'e', 2);
    look.title = o.title;
    const double s = std::max(0.1, o.scale);
    const QFont &font = o.font;
    p.save();
    p.setFont(font);
    p.translate(target.topLeft());
    p.scale(s, s);
    const QFontMetrics fm(font, p.device());
    const QSize host(int(std::lround(target.width() / s)), int(std::lround(target.height() / s)));
    VolumeColorbar::paint(p, VolumeColorbar::placement(host, fm, look), fm, look);
    p.restore();
}

} // namespace

void paintRaytraceOverlay(QPainter &painter, const QRectF &target, const QJsonObject &camera,
                          const RaytraceOverlay &overlay)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setClipRect(target);
    if (overlay.box && overlay.hasBounds && !camera.isEmpty())
        paintBox(painter, target, camera, overlay);
    if (overlay.colourbar && overlay.hasBar)
        paintBar(painter, target, overlay);
    painter.restore();
}

} // namespace visivo
