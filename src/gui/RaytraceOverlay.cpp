#include "RaytraceOverlay.h"

#include <QFont>
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

QString label(double v)
{
    const double a = std::abs(v);
    if (v == 0.0)
        return u"0"_s;
    if (a >= 1e4 || a < 1e-2)
        return QString::number(v, 'e', 1);
    return QString::number(v, 'g', 3);
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

void paintBar(QPainter &p, const QRectF &target, const RaytraceOverlay &o)
{
    if (o.lut.empty() || !(o.hi > o.lo))
        return;
    const double h = target.height();
    const QRectF bar(target.right() - 0.075 * h, target.top() + 0.22 * h, 0.022 * h, 0.56 * h);
    QFont font = p.font();
    font.setPixelSize(std::max(9, int(std::lround(0.024 * h))));
    p.setFont(font);
    // A dark plate under it, as the 3-D views' colour bar has.
    const QRectF plate(bar.left() - 0.012 * h, bar.top() - 0.07 * h, 0.17 * h, bar.height() + 0.1 * h);
    QPainterPath back;
    back.addRoundedRect(plate.translated(-0.1 * h, 0), 0.01 * h, 0.01 * h);
    p.fillPath(back, QColor(0, 0, 0, 150));
    QLinearGradient ramp(bar.bottomLeft(), bar.topLeft());
    const int n = int(o.lut.size());
    for (int i = 0; i < n; ++i) {
        const QVector3D &c = o.lut[size_t(i)];
        ramp.setColorAt(i / std::max(1.0, n - 1.0),
                        QColor::fromRgbF(std::clamp(c.x(), 0.0f, 1.0f),
                                         std::clamp(c.y(), 0.0f, 1.0f),
                                         std::clamp(c.z(), 0.0f, 1.0f)));
    }
    p.fillRect(bar, ramp);
    p.setPen(QColor(220, 225, 235));
    p.drawRect(bar);
    const QFontMetricsF fm(font);
    for (int k = 0; k <= 4; ++k) {
        const double f = k / 4.0;
        const double v = o.log && o.lo > 0 ? o.lo * std::pow(o.hi / o.lo, f) : o.lo + (o.hi - o.lo) * f;
        const double y = bar.bottom() - f * bar.height();
        p.drawLine(QPointF(bar.left() - 0.006 * h, y), QPointF(bar.left(), y));
        const QString text = label(v);
        p.drawText(QPointF(bar.left() - 0.01 * h - fm.horizontalAdvance(text), y + fm.ascent() / 2.5),
                   text);
    }
    if (!o.title.isEmpty()) {
        const QString title = fm.elidedText(o.title, Qt::ElideRight, 0.2 * h);
        p.drawText(QPointF(bar.right() - fm.horizontalAdvance(title), bar.top() - 0.025 * h), title);
    }
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
