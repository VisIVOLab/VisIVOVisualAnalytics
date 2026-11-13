#include "ParticleFrame.h"

#include <QJsonArray>
#include <QList>

#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace visivo {

ParticleFrame ParticleFrame::fromHeaders(const QByteArray &origin, const QByteArray &scale)
{
    ParticleFrame f;
    const QList<QByteArray> parts = origin.split(',');
    if (parts.size() != 3)
        return f;
    ParticleFrame out;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const double v = parts[i].trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(v))
            return f;
        out.origin[size_t(i)] = v;
    }
    bool ok = false;
    const double s = scale.trimmed().toDouble(&ok);
    if (!scale.isEmpty() && (!ok || !std::isfinite(s) || !(s > 0.0)))
        return f;
    out.scale = scale.isEmpty() ? 1.0 : s;
    return out;
}

ParticleFrame ParticleFrame::centredOn(const std::array<double, 6> &box)
{
    ParticleFrame f;
    for (int i = 0; i < 3; ++i) {
        const double c = 0.5 * (box[size_t(2 * i)] + box[size_t(2 * i + 1)]);
        f.origin[size_t(i)] = std::isfinite(c) ? c : 0.0;
    }
    return f;
}

std::array<double, 3> ParticleFrame::toTable(const float *local) const
{
    return { origin[0] + scale * double(local[0]), origin[1] + scale * double(local[1]),
             origin[2] + scale * double(local[2]) };
}

std::array<double, 6> ParticleFrame::toLocal(const std::array<double, 6> &box) const
{
    std::array<double, 6> out{};
    for (size_t i = 0; i < 6; ++i)
        out[i] = (box[i] - origin[i / 2]) / scale;
    return out;
}

QJsonObject ParticleFrame::localCamera(const QJsonObject &camera) const
{
    if (isIdentity())
        return camera;
    QJsonObject out = camera;
    for (const QString &key : { u"position"_s, u"focal_point"_s }) {
        const QJsonArray a = camera.value(key).toArray();
        if (a.size() != 3)
            continue;
        out.insert(key, QJsonArray{ (a.at(0).toDouble() - origin[0]) / scale,
                                    (a.at(1).toDouble() - origin[1]) / scale,
                                    (a.at(2).toDouble() - origin[2]) / scale });
    }
    if (camera.contains(u"parallel_scale"_s))
        out.insert(u"parallel_scale"_s, camera.value(u"parallel_scale"_s).toDouble(1.0) / scale);
    return out;
}

} // namespace visivo
