#include "BackendClient.h"
#include "DiagnosticsManager.h"

#include <QByteArray>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QThreadStorage>
#include <QDebug>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include <QFutureSynchronizer>
#include <QUrl>
#include <QUrlQuery>

#include <limits>

// ── Private helpers ───────────────────────────────────────────────────────────

// ── BackendClient: RegionRepro domain (extracted verbatim) ──

namespace {
/// The reason in the body of a failed request: FastAPI's `detail` is a text for
/// an error the route raised and a list of {loc, msg} for a body it could not
/// read. @p status (Qt's text for the HTTP status) is the fallback.
QString backendErrorDetail(const QJsonObject &body, const QString &status)
{
    const QJsonValue detail = body.value(QStringLiteral("detail"));
    if (detail.isString() && !detail.toString().isEmpty())
        return detail.toString();
    if (detail.isArray()) {
        QStringList parts;
        for (const QJsonValue &v : detail.toArray()) {
            const QJsonObject item = v.toObject();
            QStringList where;
            for (const QJsonValue &w : item.value(QStringLiteral("loc")).toArray())
                if (w.toString() != QStringLiteral("body"))
                    where << (w.isString() ? w.toString() : QString::number(w.toInt()));
            parts << (where.join(QLatin1Char('.')) + QStringLiteral(": ")
                      + item.value(QStringLiteral("msg")).toString());
            if (parts.size() == 3)
                break;
        }
        if (!parts.isEmpty())
            return parts.join(QStringLiteral("; "));
    }
    return status;
}

// _parseSnapshotList moved here (RegionRepro-only).
static BackendSnapshotListResult _parseSnapshotList(const QByteArray &raw)
{
    BackendSnapshotListResult result;
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid    = obj.value(QStringLiteral("valid")).toBool();
    result.error    = obj.value(QStringLiteral("error")).toString();
    result.nEntries = obj.value(QStringLiteral("n_entries")).toInt();
    for (const QJsonValue &v : obj.value(QStringLiteral("history")).toArray()) {
        const QJsonObject e = v.toObject();
        BackendSnapshotEntry entry;
        entry.operation = e.value(QStringLiteral("operation")).toString();
        entry.datasetId = e.value(QStringLiteral("dataset_id")).toString();
        entry.timestamp = e.value(QStringLiteral("timestamp")).toString();
        const QJsonObject paramsObj = e.value(QStringLiteral("params")).toObject();
        for (auto it = paramsObj.constBegin(); it != paramsObj.constEnd(); ++it)
            entry.params.insert(it.key(), it.value().toVariant());
        result.history.append(entry);
    }
    return result;
}
}  // namespace


BackendRegionParseResult BackendClient::parseRegions(const QString &text,
                                                     const QString &format,
                                                     const QString &datasetId,
                                                     const QString &toFrame) const
{
    BackendRegionParseResult result;
    QJsonObject bodyObj{
        { QStringLiteral("text"), text },
        { QStringLiteral("format"), format },
    };
    if (!datasetId.isEmpty()) bodyObj[QStringLiteral("dataset_id")] = datasetId;
    if (!toFrame.isEmpty())   bodyObj[QStringLiteral("to_frame")]   = toFrame;
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/regions/parse")), QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid   = obj.value(QStringLiteral("valid")).toBool();
    result.error   = obj.value(QStringLiteral("error")).toString();
    result.format  = obj.value(QStringLiteral("format")).toString();
    result.regions = obj.value(QStringLiteral("regions")).toArray();
    result.diagnostics = obj.value(QStringLiteral("diagnostics")).toArray();
    result.skipped = obj.value(QStringLiteral("n_skipped")).toInt(result.diagnostics.size());
    result.diagnosticsTruncated = obj.value(QStringLiteral("diagnostics_truncated")).toBool();
    return result;
}

BackendRegionExportResult BackendClient::exportRegions(const QJsonArray &regions,
                                                       const QString &format,
                                                       const QString &datasetId,
                                                       const QString &toFrame) const
{
    BackendRegionExportResult result;
    QJsonObject bodyObj{
        { QStringLiteral("regions"), regions },
        { QStringLiteral("format"), format },
    };
    if (!datasetId.isEmpty()) bodyObj[QStringLiteral("dataset_id")] = datasetId;
    if (!toFrame.isEmpty())   bodyObj[QStringLiteral("to_frame")]   = toFrame;
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/regions/export")), QJsonDocument(bodyObj), error);
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    if (!error.isEmpty()) {
        // On an HTTP error the body says what was wrong; Qt's own text only
        // gives the status ("Unprocessable Entity").
        result.error = backendErrorDetail(obj, error);
        return result;
    }
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    result.text  = obj.value(QStringLiteral("text")).toString();
    return result;
}

BackendRegionConvertResult BackendClient::convertRegions(const QJsonArray &regions,
                                                         const QString &toFrame,
                                                         const QString &datasetId,
                                                         int origin,
                                                         const QString &unit) const
{
    BackendRegionConvertResult result;
    QJsonObject bodyObj{
        { QStringLiteral("regions"), regions },
        { QStringLiteral("to_frame"), toFrame },
        { QStringLiteral("origin"), origin },
    };
    if (!datasetId.isEmpty()) bodyObj[QStringLiteral("dataset_id")] = datasetId;
    if (!unit.isEmpty())      bodyObj[QStringLiteral("unit")]       = unit;
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/regions/convert")), QJsonDocument(bodyObj), error);
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    if (!error.isEmpty()) {
        // On an HTTP error the body is the reason ("The region lies outside the
        // dataset's sky projection."); Qt's own text only says the status.
        result.error = backendErrorDetail(obj, error);
        return result;
    }
    result.valid       = obj.value(QStringLiteral("valid")).toBool();
    result.error       = obj.value(QStringLiteral("error")).toString();
    result.frame       = obj.value(QStringLiteral("frame")).toString();
    result.nativeFrame = obj.value(QStringLiteral("native_frame")).toString();
    result.regions     = obj.value(QStringLiteral("regions")).toArray();
    return result;
}

BackendRegionStatsResult BackendClient::regionStats(const QString &datasetId,
                                                    const QJsonObject &region,
                                                    int channel) const
{
    BackendRegionStatsResult result;
    QJsonObject bodyObj{{ QStringLiteral("region"), region }};
    if (channel >= 0) bodyObj[QStringLiteral("channel")] = channel;
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/regions/stats/") + datasetId),
            QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid        = obj.value(QStringLiteral("valid")).toBool();
    result.error        = obj.value(QStringLiteral("error")).toString();
    result.totalCount   = obj.value(QStringLiteral("total_count")).toInteger();
    result.validCount   = obj.value(QStringLiteral("valid_count")).toInteger();
    result.blankedCount = obj.value(QStringLiteral("blanked_count")).toInteger();
    result.mean         = obj.value(QStringLiteral("mean")).toDouble();
    result.rms          = obj.value(QStringLiteral("rms")).toDouble();
    result.sum          = obj.value(QStringLiteral("sum")).toDouble();
    result.minValue     = obj.value(QStringLiteral("min")).toDouble();
    result.maxValue     = obj.value(QStringLiteral("max")).toDouble();
    result.median       = obj.value(QStringLiteral("median")).toDouble();
    result.madSigma     = obj.value(QStringLiteral("mad_sigma")).toDouble();
    result.bunit        = obj.value(QStringLiteral("bunit")).toString();
    const QJsonValue flux = obj.value(QStringLiteral("flux_jy"));
    result.hasFlux      = !flux.isNull() && !flux.isUndefined();
    result.fluxJy       = flux.toDouble();
    // A backend before issue #503 sends only "rms" (the standard deviation).
    result.stddev = obj.contains(QStringLiteral("stddev"))
            ? obj.value(QStringLiteral("stddev")).toDouble() : result.rms;
    const QJsonValue rmsRaw = obj.value(QStringLiteral("rms_unweighted"));
    result.hasRmsUnweighted = rmsRaw.isDouble();
    result.rmsUnweighted    = rmsRaw.toDouble();
    const QJsonArray peak = obj.value(QStringLiteral("peak_pixel")).toArray();
    result.hasPeak = peak.size() == 2;
    if (result.hasPeak) {
        result.peakX = peak.at(0).toDouble();
        result.peakY = peak.at(1).toDouble();
    }
    return result;
}

BackendRegionProfileResult BackendClient::regionProfile(const QString &datasetId,
                                                        const QJsonObject &line,
                                                        int channel, int widthPix) const
{
    BackendRegionProfileResult result;
    QJsonObject bodyObj{{ QStringLiteral("region"), line },
                        { QStringLiteral("width"), widthPix }};
    if (channel >= 0) bodyObj[QStringLiteral("channel")] = channel;
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/regions/profile/") + datasetId),
            QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    const auto column = [&obj](const char *key, bool nullIsNan) {
        QVector<double> out;
        const QJsonArray a = obj.value(QLatin1String(key)).toArray();
        out.reserve(a.size());
        for (const QJsonValue &v : a)
            out.append(v.isNull() && nullIsNan ? std::numeric_limits<double>::quiet_NaN()
                                               : v.toDouble());
        return out;
    };
    result.offsets    = column("offsets", false);
    result.values     = column("values", true);     // null: a blank or off-image sample
    result.x          = column("x", false);
    result.y          = column("y", false);
    result.offsetUnit = obj.value(QStringLiteral("offset_unit")).toString();
    result.bunit      = obj.value(QStringLiteral("bunit")).toString();
    result.widthPix   = obj.value(QStringLiteral("width_pix")).toInt(1);
    result.validSamples = obj.value(QStringLiteral("n_valid")).toInt();
    result.lengthPix  = obj.value(QStringLiteral("length_pix")).toDouble();
    return result;
}

BackendImageNoiseResult BackendClient::imageNoise(const QString &datasetId,
                                                  const QJsonArray &regions,
                                                  int channel, double clipSigma) const
{
    BackendImageNoiseResult result;
    QJsonObject bodyObj{{ QStringLiteral("regions"), regions },
                        { QStringLiteral("clip_sigma"), clipSigma }};
    if (channel >= 0) bodyObj[QStringLiteral("channel")] = channel;
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/regions/noise/") + datasetId),
            QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    result.mode = obj.value(QStringLiteral("mode")).toString();
    result.rms = obj.value(QStringLiteral("rms")).toDouble();
    result.madSigma = obj.value(QStringLiteral("mad_sigma")).toDouble();
    const QJsonValue scatter = obj.value(QStringLiteral("rms_scatter"));
    if (scatter.isDouble())
        result.rmsScatter = scatter.toDouble();
    result.validCount = obj.value(QStringLiteral("valid_count")).toInteger();
    result.usedCount = obj.value(QStringLiteral("used_count")).toInteger();
    result.clipSigma = obj.value(QStringLiteral("clip_sigma")).toDouble();
    result.exact = obj.value(QStringLiteral("exact")).toBool();
    result.sampleCount = obj.value(QStringLiteral("sample_count")).toInteger();
    result.bunit = obj.value(QStringLiteral("bunit")).toString();
    // null when the image has no beam in Jy/beam: the conversion is not shown.
    result.beamAreaPix = obj.value(QStringLiteral("beam_area_pix")).toDouble();
    result.rmsPerPixel = obj.value(QStringLiteral("rms_per_pixel")).toDouble();
    result.madSigmaPerPixel = obj.value(QStringLiteral("mad_sigma_per_pixel")).toDouble();
    for (const QJsonValue &v : obj.value(QStringLiteral("regions")).toArray()) {
        const QJsonObject r = v.toObject();
        BackendImageNoiseResult::PerRegion p;
        p.name = r.value(QStringLiteral("name")).toString();
        p.validCount = r.value(QStringLiteral("valid_count")).toInteger();
        p.measured = r.value(QStringLiteral("rms")).isDouble();
        p.rms = r.value(QStringLiteral("rms")).toDouble();
        p.madSigma = r.value(QStringLiteral("mad_sigma")).toDouble();
        result.regions.append(p);
    }
    return result;
}

BackendProvJsonResult BackendClient::requestProvJson()
{
    BackendProvJsonResult result;
    QString error;
    const QByteArray raw = performGet(
            QUrl(m_baseUrl + QStringLiteral("/v1/reproducibility/provenance")), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    result.prov  = obj.value(QStringLiteral("prov")).toObject();
    return result;
}

BackendSnapshotListResult BackendClient::listSnapshots()
{
    BackendSnapshotListResult result;
    QString error;
    const QByteArray raw = performGet(
            QUrl(m_baseUrl + QStringLiteral("/v1/reproducibility/snapshots")), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    return _parseSnapshotList(raw);
}

BackendSnapshotListResult BackendClient::recordSnapshot(
        const QString &operation,
        const QString &datasetId,
        const QVariantMap &params)
{
    BackendSnapshotListResult result;
    QString error;
    QJsonObject paramsObj;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        paramsObj.insert(it.key(), QJsonValue::fromVariant(it.value()));
    const QJsonDocument body(QJsonObject{
        { QStringLiteral("operation"),  operation },
        { QStringLiteral("dataset_id"), datasetId },
        { QStringLiteral("params"),     paramsObj },
    });
    const QByteArray raw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/reproducibility/snapshots")),
            body, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    return _parseSnapshotList(raw);
}
