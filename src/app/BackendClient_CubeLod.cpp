// BackendClient_CubeLod.cpp
// Transport of the 3-D view's level of detail (issue #321): the state of a
// cube's brick pyramid, and one box of one level of the cube.
//
// The box comes back as raw float32 with its geometry in X-Visivo-* headers,
// through the same worker-safe binary POST the z-slabs of a sub-volume use
// (BackendClient::binaryPoster), so several blocks of a refined view can be
// read at once.

#include "BackendClient.h"
#include "BackendHeaders.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace {

/// "x,y,z" as three integers; false when it is not that.
bool parseTriple(const QByteArray &text, std::array<int, 3> &out)
{
    const QList<QByteArray> parts = text.split(',');
    if (parts.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        out[i] = parts[i].trimmed().toInt(&ok);
        if (!ok)
            return false;
    }
    return true;
}

}   // namespace

BackendPyramidStatus BackendClient::parsePyramidStatus(const QJsonObject &object)
{
    BackendPyramidStatus s;
    s.state = object.value(QStringLiteral("state")).toString();
    s.valid = !s.state.isEmpty();
    s.progress = object.value(QStringLiteral("progress")).toDouble(0.0);
    s.levels = object.value(QStringLiteral("levels")).toInt(0);
    s.brick = object.value(QStringLiteral("brick")).toInt(0);
    s.bytesEstimate = static_cast<qint64>(object.value(QStringLiteral("bytes_estimate")).toDouble(0.0));
    s.error = object.value(QStringLiteral("error")).toString();
    // [z, y, x] per level on the wire; everything client-side is x, y, z.
    for (const QJsonValue &v : object.value(QStringLiteral("shapes")).toArray()) {
        const QJsonArray zyx = v.toArray();
        if (zyx.size() != 3) {
            s.shapes.clear();
            break;
        }
        s.shapes.append({ zyx.at(2).toInt(), zyx.at(1).toInt(), zyx.at(0).toInt() });
    }
    if (!s.shapes.isEmpty() && s.levels != s.shapes.size())
        s.levels = static_cast<int>(s.shapes.size());
    if (!s.valid && s.error.isEmpty())
        s.error = QStringLiteral("pyramid_status answered without a state");
    return s;
}

BackendPyramidStatus BackendClient::requestPyramidStatus(const QString &datasetId, bool build) const
{
    QString error;
    int status = 0;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("build"), build } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/pyramid_status")), body, error,
                        nullptr, &status);
    // 202 is "building", with the same body: an answer, not an error.
    if (status != 200 && status != 202) {
        BackendPyramidStatus s;
        s.httpStatus = status;
        s.error = error.isEmpty() ? QStringLiteral("HTTP %1 on pyramid_status").arg(status) : error;
        return s;
    }
    BackendPyramidStatus s = parsePyramidStatus(QJsonDocument::fromJson(payload).object());
    s.httpStatus = status;
    return s;
}

BackendRegionLevelResult BackendClient::requestRegionLevel(const QString &datasetId, int level,
                                                           int x0, int x1, int y0, int y1, int z0,
                                                           int z1, int ghost,
                                                           const QString &source) const
{
    BackendRegionLevelResult result;
    const BinaryReply reply = this->binaryPoster()(
            QStringLiteral("/v1/cube/region_level_bin"),
            QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                         { QStringLiteral("level"), level },
                         { QStringLiteral("x0"), x0 },
                         { QStringLiteral("x1"), x1 },
                         { QStringLiteral("y0"), y0 },
                         { QStringLiteral("y1"), y1 },
                         { QStringLiteral("z0"), z0 },
                         { QStringLiteral("z1"), z1 },
                         { QStringLiteral("ghost"), ghost },
                         { QStringLiteral("source"), source } });
    result.httpStatus = reply.status;
    if (!reply.ok) {
        // The route's refusals (409, 413, 422) are one plain-text sentence:
        // more use to the user than the status code alone.
        const QString text = QString::fromUtf8(reply.body.left(400)).trimmed();
        result.error = (reply.status >= 400 && !text.isEmpty() && !text.startsWith(u'<'))
                ? QStringLiteral("%1 (HTTP %2)").arg(text).arg(reply.status)
                : reply.error;
        return result;
    }
    const auto header = [&reply](const char *name) {
        return reply.headers.value(QByteArray(name).toLower());
    };
    result.width = header(VisivoHeaders::WIDTH).toInt();
    result.height = header(VisivoHeaders::HEIGHT).toInt();
    result.depth = header(VisivoHeaders::DEPTH).toInt();
    result.level = header(VisivoHeaders::LEVEL).toInt();
    result.source = QString::fromUtf8(header(VisivoHeaders::SOURCE));
    result.voxelOffset = header(VisivoHeaders::VOXEL_OFFSET).toDouble();
    result.rangeMin = header(VisivoHeaders::RANGE_MIN).toDouble();
    result.rangeMax = header(VisivoHeaders::RANGE_MAX).toDouble();
    const QByteArray order = header(VisivoHeaders::BYTEORDER);
    const QByteArray scalar = header(VisivoHeaders::SCALAR_TYPE);
    const qint64 expected = qint64(result.width) * result.height * result.depth * qint64(sizeof(float));
    if (!parseTriple(header(VisivoHeaders::LEVEL_ORIGIN), result.levelOrigin)) {
        result.error = QStringLiteral("region_level_bin: no level origin in the answer");
    } else if (result.width <= 0 || result.height <= 0 || result.depth <= 0
               || reply.body.size() != expected) {
        result.error = QStringLiteral("region_level_bin: %1 bytes for %2 x %3 x %4 samples")
                               .arg(reply.body.size())
                               .arg(result.width)
                               .arg(result.height)
                               .arg(result.depth);
    } else if ((!order.isEmpty() && order != "little")
               || (!scalar.isEmpty() && scalar != "float32")) {
        // Every platform the desktop runs on is little-endian.
        result.error = QStringLiteral("region_level_bin: unsupported samples (%1, %2)")
                               .arg(QString::fromUtf8(scalar), QString::fromUtf8(order));
    } else if (result.level != level) {
        result.error = QStringLiteral("region_level_bin: asked for level %1, got %2")
                               .arg(level)
                               .arg(result.level);
    } else {
        result.data = reply.body;
        result.valid = true;
    }
    return result;
}
