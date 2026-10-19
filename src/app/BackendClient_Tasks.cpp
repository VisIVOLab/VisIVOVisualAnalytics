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

// ── BackendClient: Tasks domain (extracted verbatim) ──

namespace {
// getThreadLocalNam + its thread-local storage duplicated here (also in main).
QThreadStorage<QNetworkAccessManager*> s_namStorage;

QNetworkAccessManager* getThreadLocalNam() {
        if (!s_namStorage.hasLocalData()) {
            s_namStorage.setLocalData(new QNetworkAccessManager());
        }
        return s_namStorage.localData();
    }
}  // namespace


BackendSaveSubregionResult BackendClient::saveCubeSubregion(
        const QString &datasetId,
        int x0, int x1, int y0, int y1, int z0, int z1,
        const QString &outputBasename)
{
    BackendSaveSubregionResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("x0"), x0 }, { QStringLiteral("x1"), x1 },
            { QStringLiteral("y0"), y0 }, { QStringLiteral("y1"), y1 },
            { QStringLiteral("z0"), z0 }, { QStringLiteral("z1"), z1 },
            { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/save_subregion")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseSaveSubregionObject(QJsonDocument::fromJson(raw).object());
}

BackendSaveSubregionResult BackendClient::maskCubeRegion(
        const QString &datasetId,
        int x0, int x1, int y0, int y1, int z0, int z1,
        const QString &mode, const QString &outputBasename)
{
    BackendSaveSubregionResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("x0"), x0 }, { QStringLiteral("x1"), x1 },
            { QStringLiteral("y0"), y0 }, { QStringLiteral("y1"), y1 },
            { QStringLiteral("z0"), z0 }, { QStringLiteral("z1"), z1 },
            { QStringLiteral("mode"), mode },
            { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/mask")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseSaveSubregionObject(QJsonDocument::fromJson(raw).object());
}

BackendSaveChannel2dResult BackendClient::saveCubeChannelAs2dFits(
        const QString &datasetId,
        int channel,
        const QString &outputBasename)
{
    BackendSaveChannel2dResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"),      datasetId },
            { QStringLiteral("channel"),         channel },
            { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/save_channel_2d")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseSaveChannel2dObject(QJsonDocument::fromJson(raw).object());
}

BackendSaveChannel2dResult BackendClient::saveMap2dToWorkspace(
        const QString &sourceDatasetId,
        const QByteArray &pixels, int width, int height,
        const QString &outputBasename,
        const QString &bunit,
        const QStringList &history,
        const QPoint &regionOrigin)
{
    BackendSaveChannel2dResult result;
    // Checked here rather than server-side alone: a mismatch means the caller
    // paired the wrong buffer with the wrong shape, and uploading megabytes to
    // be told so is a slow way to find out.
    const qsizetype expected =
            static_cast<qsizetype>(width) * height * static_cast<qsizetype>(sizeof(float));
    if (width <= 0 || height <= 0 || pixels.size() != expected) {
        result.error = QStringLiteral(
                "The map is %1 x %2 but the buffer holds %3 bytes, not %4.")
                               .arg(width).arg(height).arg(pixels.size()).arg(expected);
        return result;
    }

    QJsonObject body{
        { QStringLiteral("dataset_id"),      sourceDatasetId },
        { QStringLiteral("output_basename"), outputBasename },
        { QStringLiteral("width"),           width },
        { QStringLiteral("height"),          height },
        { QStringLiteral("pixels_base64"),
          QString::fromLatin1(pixels.toBase64()) },
        { QStringLiteral("bunit"),           bunit },
    };
    if (!history.isEmpty()) {
        QJsonArray lines;
        for (const QString &line : history)
            lines.append(line);
        body.insert(QStringLiteral("history"), lines);
    }
    if (!regionOrigin.isNull()) {
        body.insert(QStringLiteral("region_origin"),
                    QJsonArray{ regionOrigin.x(), regionOrigin.y() });
    }

    QString error;
    const QByteArray raw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/exports/save_map_2d")),
            QJsonDocument(body), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseSaveChannel2dObject(QJsonDocument::fromJson(raw).object());
}

BackendSavePvFitsResult BackendClient::savePvFits(const QString &datasetId,
                                                  const std::vector<std::array<double, 2>> &vertices,
                                                  int widthPixels,
                                                  const QString &outputBasename)
{
    BackendSavePvFitsResult result;
    QString error;
    QJsonArray vertexArray;
    for (const auto &v : vertices)
        vertexArray.append(QJsonArray{ v[0], v[1] });
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("vertices"), vertexArray },
                                          { QStringLiteral("width_pixels"), widthPixels },
                                          { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/products/pv_fits")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject o = QJsonDocument::fromJson(raw).object();
    result.valid = o.value(QStringLiteral("valid")).toBool(false);
    result.error = o.value(QStringLiteral("error")).toString();
    result.outputPath = o.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = o.value(QStringLiteral("workspace_filename")).toString();
    result.width = o.value(QStringLiteral("width")).toInt();
    result.height = o.value(QStringLiteral("height")).toInt();
    result.rangeMin = o.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax = o.value(QStringLiteral("range_max")).toDouble();
    result.bunit = o.value(QStringLiteral("bunit")).toString();
    if (!result.valid && result.error.isEmpty())
        result.error = QStringLiteral("The backend could not write the PV FITS file.");
    return result;
}

BackendSaveMomentFitsResult BackendClient::saveMomentFits(
        const QString &datasetId,
        int order,
        int channelStart, int channelEnd,
        bool maskEnabled,
        double thresholdValue,
        const QString &outputBasename,
        bool thresholdAuto)
{
    BackendSaveMomentFitsResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"),       datasetId },
            { QStringLiteral("moment_order"),     order },
            { QStringLiteral("channel_start"),    channelStart },
            { QStringLiteral("channel_end"),      channelEnd },
            { QStringLiteral("mask_enabled"),     maskEnabled },
            { QStringLiteral("threshold_value"),  thresholdValue },
            { QStringLiteral("threshold_auto"),   thresholdAuto },
            { QStringLiteral("output_basename"),  outputBasename } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/products/moment_fits")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseSaveMomentFitsObject(QJsonDocument::fromJson(raw).object());
}

BackendHeaderPatchResult BackendClient::patchFitsHeader(
        const QString &path, const QList<BackendHeaderCardEdit> &cards,
        const QStringList &remove, const QString &outputBasename)
{
    BackendHeaderPatchResult result;
    QString error;
    QJsonArray cardArray;
    for (const BackendHeaderCardEdit &card : cards) {
        QJsonObject entry{ { QStringLiteral("key"), card.key },
                           { QStringLiteral("value"), card.value } };
        // Absent means "leave the comment alone"; present-and-empty means
        // "clear it". Sending "" unconditionally would clear every comment.
        if (card.commentProvided)
            entry.insert(QStringLiteral("comment"), card.comment);
        cardArray.append(entry);
    }
    QJsonArray removeArray;
    for (const QString &key : remove)
        removeArray.append(key);

    const QJsonDocument body(QJsonObject{
            { QStringLiteral("path"),            path },
            { QStringLiteral("cards"),           cardArray },
            { QStringLiteral("remove"),          removeArray },
            { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray raw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/files/header/patch")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject o = QJsonDocument::fromJson(raw).object();
    result.valid             = o.value(QStringLiteral("valid")).toBool(false);
    result.error             = o.value(QStringLiteral("error")).toString();
    result.outputPath        = o.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = o.value(QStringLiteral("workspace_filename")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("changes")).toArray())
        result.changes << v.toString();
    result.wcsStatus    = o.value(QStringLiteral("wcs_status")).toString();
    result.wcsMessage   = o.value(QStringLiteral("wcs_message")).toString();
    result.wcsCelestial = o.value(QStringLiteral("wcs_celestial")).toBool(false);
    return result;
}

BackendFilterFitsResult BackendClient::filterFits(const QString &path,
                                                 double smoothFwhmPixels, int regridFactor,
                                                 const QString &combine,
                                                 const QString &outputBasename)
{
    BackendFilterFitsResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("path"),               path },
            { QStringLiteral("smooth_fwhm_pixels"), smoothFwhmPixels },
            { QStringLiteral("regrid_factor"),      regridFactor },
            { QStringLiteral("combine"),            combine },
            { QStringLiteral("output_basename"),    outputBasename } });
    const QByteArray raw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/image/filter")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject o = QJsonDocument::fromJson(raw).object();
    result.valid             = o.value(QStringLiteral("valid")).toBool(false);
    result.error             = o.value(QStringLiteral("error")).toString();
    result.outputPath        = o.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = o.value(QStringLiteral("workspace_filename")).toString();
    result.width             = o.value(QStringLiteral("width")).toInt();
    result.height            = o.value(QStringLiteral("height")).toInt();
    result.depth             = o.value(QStringLiteral("depth")).toInt(1);
    result.rangeMin          = o.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax          = o.value(QStringLiteral("range_max")).toDouble();
    result.bunit             = o.value(QStringLiteral("bunit")).toString();
    result.combine           = o.value(QStringLiteral("combine")).toString();
    result.smoothFwhmPixels  = o.value(QStringLiteral("smooth_fwhm_pixels")).toDouble();
    result.smoothFwhmArcsec  = o.value(QStringLiteral("smooth_fwhm_arcsec")).toDouble();
    result.regridFactor      = o.value(QStringLiteral("regrid_factor")).toInt(1);
    result.fluxScale         = o.value(QStringLiteral("flux_scale")).toDouble(1.0);
    result.beamMissing       = o.value(QStringLiteral("beam_missing")).toBool(false);
    return result;
}

BackendWorkspaceDeleteResult BackendClient::copyDatasetToWorkspace(
        const QString &datasetId, const QString &basename)
{
    BackendWorkspaceDeleteResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"),      datasetId },
            { QStringLiteral("output_basename"), basename } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/exports/copy_dataset")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid           = obj.value(QStringLiteral("valid")).toBool();
    result.error           = obj.value(QStringLiteral("error")).toString();
    result.deletedFilename = obj.value(QStringLiteral("workspace_filename")).toString();
    return result;
}

BackendWorkspaceExportsList BackendClient::listExports()
{
    BackendWorkspaceExportsList result;
    QString error;
    const QByteArray raw = performGet(
            QUrl(m_baseUrl + QStringLiteral("/v1/exports/list")), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseExportsListObject(QJsonDocument::fromJson(raw).object());
}

BackendWorkspaceDeleteResult BackendClient::deleteExport(const QString &filename)
{
    BackendWorkspaceDeleteResult result;
    QString error;
    // Filename is path-segment-escaped to keep slashes out of the URL — the
    // backend rejects multi-segment filenames anyway, but pre-escaping
    // avoids accidentally hitting a different route.
    const QString safeSeg = QString::fromUtf8(
            QUrl::toPercentEncoding(filename, "", "/"));
    QNetworkRequest req(QUrl(m_baseUrl
                             + QStringLiteral("/v1/exports/") + safeSeg));
    // Hand-built (QNAM has no delete-with-body helper we can reuse), so stamp
    // the headers explicitly rather than letting it send an unnormalised token.
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_sessionId);
    auto *nam = getThreadLocalNam();
    auto *reply = nam->sendCustomRequest(req, "DELETE");
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError) {
        result.error = reply->errorString();
        reply->deleteLater();
        return result;
    }
    const QByteArray raw = reply->readAll();
    reply->deleteLater();
    return parseExportsDeleteObject(QJsonDocument::fromJson(raw).object());
}

bool BackendClient::downloadExport(const QString &filename,
                                   const QString &localPath,
                                   QString &error)
{
    QUrl url(m_baseUrl + QStringLiteral("/v1/exports/download"));
    // Manually percent-encode the filename instead of using QUrlQuery: Qt's
    // QUrlQuery leaves '+' raw because RFC 3986 considers it a sub-delim, but
    // FastAPI / Starlette decode query strings using the
    // application/x-www-form-urlencoded convention where '+' means a space.
    // That collision broke filenames like "G000.0+8.5_RM_ch0001.fits" (the
    // server received "G000.0 8.5_…" and returned 404). Force '+' (and any
    // other reserved chars) to be percent-encoded so the filename round-trips.
    const QByteArray encodedFilename = QUrl::toPercentEncoding(filename);
    url.setQuery(QStringLiteral("filename=") + QString::fromUtf8(encodedFilename));
    const QByteArray payload = performGet(url, error);
    if (!error.isEmpty()) return false;
    QFile out(localPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("Could not open '%1' for writing: %2")
                .arg(localPath, out.errorString());
        return false;
    }
    const qint64 written = out.write(payload);
    out.close();
    if (written != static_cast<qint64>(payload.size())) {
        error = QStringLiteral("Short write to '%1' (%2 / %3 bytes)")
                .arg(localPath).arg(written).arg(payload.size());
        return false;
    }
    return true;
}

BackendSourceFindResult BackendClient::runSourceFind(const QString &datasetId,
                                                      const QVariantMap &sofiaParams)
{
    BackendSourceFindResult result;
    QString error;
    QJsonObject paramsObj;
    for (auto it = sofiaParams.constBegin(); it != sofiaParams.constEnd(); ++it)
        paramsObj.insert(it.key(), QJsonValue::fromVariant(it.value()));
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"),   datasetId },
            { QStringLiteral("sofia_params"), paramsObj } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/sourcefind/") + datasetId),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid    = obj.value(QStringLiteral("valid")).toBool();
    result.error    = obj.value(QStringLiteral("error")).toString();
    result.nSources = obj.value(QStringLiteral("n_sources")).toInt();
    result.maskPath = obj.value(QStringLiteral("mask_path")).toString();
    const QJsonArray sources = obj.value(QStringLiteral("sources")).toArray();
    for (const QJsonValue &v : sources) {
        BackendSourceFindSource src;
        const QJsonObject so = v.toObject();
        for (const QString &key : so.keys())
            src.fields.insert(key, so.value(key).toVariant());
        result.sources.append(src);
    }
    return result;
}

BackendTaskCreateResult BackendClient::createMomentTask(const QString &datasetId, int order,
                                                        int channelStart, int channelEnd,
                                                        bool maskEnabled,
                                                        double thresholdValue,
                                                        bool thresholdAuto) const
{
    BackendTaskCreateResult result;
    QString error;
    const QJsonDocument body(
            QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                         { QStringLiteral("moment_order"), order },
                         { QStringLiteral("channel_start"), channelStart },
                         { QStringLiteral("channel_end"), channelEnd },
                         { QStringLiteral("mask_enabled"), maskEnabled },
                         { QStringLiteral("threshold_value"), thresholdValue },
                         { QStringLiteral("threshold_auto"), thresholdAuto } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/moment")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.taskId = object.value(QStringLiteral("task_id")).toString();
    result.status = object.value(QStringLiteral("status")).toString();
    result.cacheHit = object.value(QStringLiteral("cache_hit")).toBool(false);
    return result;
}

BackendTaskCreateResult BackendClient::createPvTask(const QString &datasetId,
                                                    const std::vector<std::array<double, 2>> &vertices,
                                                    int widthPixels) const
{
    BackendTaskCreateResult result;
    QString error;
    QJsonArray vertexArray;
    for (const auto &v : vertices) {
        vertexArray.append(QJsonArray{ v[0], v[1] });
    }
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("vertices"), vertexArray },
                                          { QStringLiteral("width_pixels"), widthPixels } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/pv")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.taskId = object.value(QStringLiteral("task_id")).toString();
    result.status = object.value(QStringLiteral("status")).toString();
    result.cacheHit = object.value(QStringLiteral("cache_hit")).toBool(false);
    return result;
}

BackendTaskStatusResult BackendClient::requestTaskStatus(const QString &taskId) const
{
    BackendTaskStatusResult result;
    QString error;
    const QByteArray payload =
            performGet(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/") + taskId), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.taskId = object.value(QStringLiteral("task_id")).toString();
    result.operation = object.value(QStringLiteral("operation")).toString();
    result.status = object.value(QStringLiteral("status")).toString();
    result.progress = object.value(QStringLiteral("progress")).toDouble();
    result.cacheHit = object.value(QStringLiteral("cache_hit")).toBool(false);
    result.resultObject = object.value(QStringLiteral("result")).toObject();
    return result;
}

BackendTaskStatusResult BackendClient::waitForTaskCompletion(const BackendTaskCreateResult &createResult,
                                                             const QString &logTag,
                                                             int maxPollAttempts,
                                                             int pollIntervalMs) const
{
    BackendTaskStatusResult terminal;
    terminal.taskId = createResult.taskId;
    QString operationTag;
    if (logTag.contains(QStringLiteral("moment"), Qt::CaseInsensitive)) {
        operationTag = QStringLiteral("moment");
    } else if (logTag.contains(QStringLiteral("pv"), Qt::CaseInsensitive)) {
        operationTag = QStringLiteral("pv");
    }
    if (!createResult.valid || createResult.taskId.isEmpty()) {
        terminal.error = createResult.error.isEmpty()
                ? QStringLiteral("Task creation failed.")
                : createResult.error;
        qWarning().noquote()
                << QStringLiteral("%1 create failed error=%2").arg(logTag, terminal.error);
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Task,
                QStringLiteral("BackendClient"),
                QStringLiteral("Task creation failed: %1").arg(terminal.error), {}, this->m_sessionId,
                operationTag);
        return terminal;
    }

    qDebug().noquote()
            << QStringLiteral("%1 created task_id=%2 status=%3 cache_hit=%4")
                       .arg(logTag, createResult.taskId, createResult.status)
                       .arg(createResult.cacheHit ? QStringLiteral("true")
                                                 : QStringLiteral("false"));
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Task, QStringLiteral("BackendClient"),
            QStringLiteral("Task created (%1)%2.")
                    .arg(createResult.taskId,
                         createResult.cacheHit ? QStringLiteral(", cache hit")
                                               : QString()),
            {}, this->m_sessionId, operationTag);
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Task, QStringLiteral("BackendClient"),
            QStringLiteral("Polling task status."), {}, this->m_sessionId, operationTag);

    for (int attempt = 0; attempt < maxPollAttempts; ++attempt) {
        // Use a local QEventLoop driven by a single-shot QTimer instead of
        // QThread::msleep so the thread can process queued Qt events while
        // waiting. This prevents the thread pool slot from being locked in an
        // unconditional sleep system call during long-running polls.
        {
            QEventLoop waitLoop;
            QTimer::singleShot(pollIntervalMs, &waitLoop, &QEventLoop::quit);
            waitLoop.exec();
        }
        const auto taskStatus = this->requestTaskStatus(createResult.taskId);
        if (!taskStatus.valid && !taskStatus.status.isEmpty()
            && taskStatus.status != QStringLiteral("failed")) {
            qWarning().noquote()
                    << QStringLiteral("%1 polling invalid task_id=%2 error=%3")
                               .arg(logTag, createResult.taskId, taskStatus.error);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Task,
                    QStringLiteral("BackendClient"),
                    QStringLiteral("Task polling failed: %1").arg(taskStatus.error), {},
                    this->m_sessionId, operationTag);
            return taskStatus;
        }
        qDebug().noquote()
                << QStringLiteral("%1 poll task_id=%2 status=%3 progress=%4")
                           .arg(logTag, createResult.taskId, taskStatus.status)
                           .arg(taskStatus.progress, 0, 'f', 2);
        if (taskStatus.status == QStringLiteral("completed")) {
            qDebug().noquote()
                    << QStringLiteral("%1 completed task_id=%2 cache_hit=%3")
                               .arg(logTag, createResult.taskId)
                               .arg(taskStatus.cacheHit ? QStringLiteral("true")
                                                        : QStringLiteral("false"));
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Info, DiagnosticsCategory::Task,
                    QStringLiteral("BackendClient"),
                    QStringLiteral("Task completed (%1)%2.")
                            .arg(createResult.taskId,
                                 taskStatus.cacheHit ? QStringLiteral(", cache hit")
                                                     : QString()),
                    {}, this->m_sessionId, operationTag);
            return taskStatus;
        }
        if (taskStatus.status == QStringLiteral("failed")) {
            qWarning().noquote()
                    << QStringLiteral("%1 failed task_id=%2 error=%3")
                               .arg(logTag, createResult.taskId, taskStatus.error);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error, DiagnosticsCategory::Task,
                    QStringLiteral("BackendClient"),
                    QStringLiteral("Task failed (%1): %2")
                            .arg(createResult.taskId, taskStatus.error),
                    {}, this->m_sessionId, operationTag);
            return taskStatus;
        }
        if (taskStatus.status == QStringLiteral("cancelled")) {
            // Cancelled is terminal too. The status route answers valid=true
            // for a cancelled task, so the invalid-response branch above does
            // not catch it: without this the client polls a task that will
            // never change again until the poll times out, and then tells the
            // user the backend stopped answering.
            BackendTaskStatusResult cancelled = taskStatus;
            if (cancelled.error.isEmpty())
                cancelled.error = QStringLiteral("Task cancelled.");
            qWarning().noquote()
                    << QStringLiteral("%1 cancelled task_id=%2").arg(logTag, createResult.taskId);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Task,
                    QStringLiteral("BackendClient"),
                    QStringLiteral("Task cancelled (%1).").arg(createResult.taskId), {},
                    this->m_sessionId, operationTag);
            return cancelled;
        }
    }

    terminal.error = QStringLiteral("Task polling timed out.");
    qWarning().noquote()
            << QStringLiteral("%1 polling timed out task_id=%2").arg(logTag, createResult.taskId);
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Warning, DiagnosticsCategory::Task, QStringLiteral("BackendClient"),
            QStringLiteral("Task polling timed out (%1).").arg(createResult.taskId), {},
            this->m_sessionId, operationTag);
    return terminal;
}
