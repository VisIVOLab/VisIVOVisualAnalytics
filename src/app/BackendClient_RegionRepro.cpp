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
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    result.text  = obj.value(QStringLiteral("text")).toString();
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
    result.validCount   = obj.value(QStringLiteral("valid_count")).toInt();
    result.blankedCount = obj.value(QStringLiteral("blanked_count")).toInt();
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
