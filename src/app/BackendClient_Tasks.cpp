#include "BackendClient.h"
#include "BackendTaskFollow.h"
#include "DiagnosticsManager.h"
#include "SofiaSuggest.h"
#include "TaskQuitDrain.h"

#include <QByteArray>
#include <QCoreApplication>

#include <atomic>
#include <thread>
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
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <limits>

namespace {
/// How long a task cancellation may wait for the backend's answer.
constexpr int kCancelTaskTimeoutMs = 15'000;
}  // namespace

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

BackendTaskCreateResult BackendClient::createMomentFitsTask(const QString &datasetId, int order,
                                                            int channelStart, int channelEnd,
                                                            bool maskEnabled,
                                                            double thresholdValue,
                                                            const QString &outputBasename,
                                                            bool thresholdAuto) const
{
    BackendTaskCreateResult result;
    // Not while quitting: the quit would have to cancel it (TaskQuitDrain.h).
    if (visivo::refuseTaskWhileQuitting(result.error))
        return result;
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
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/moment_fits")), body, error,
                        nullptr, &result.httpStatus);
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

BackendSaveMomentFitsResult BackendClient::exportMomentFitsFollowingTask(
        const QString &datasetId, int order, int channelStart, int channelEnd, bool maskEnabled,
        double thresholdValue, const QString &outputBasename, bool thresholdAuto,
        visivo::BackendTaskControl *control)
{
    const BackendTaskCreateResult created =
            createMomentFitsTask(datasetId, order, channelStart, channelEnd, maskEnabled,
                                 thresholdValue, outputBasename, thresholdAuto);
    const visivo::TaskFollowResult followed = visivo::followBackendTask(
            *this, created, QStringLiteral("[moment_fits][task]"), control);
    if (followed.outcome == visivo::TaskOutcome::Completed)
        return parseSaveMomentFitsObject(followed.status.resultObject);
    if (followed.fallBackToSync()) {
        // An older backend: the synchronous route, bounded by its request
        // timeout and not cancellable. Said so on the progress sheet.
        if (control)
            control->setPhase(visivo::TaskPhase::Synchronous);
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Task,
                QStringLiteral("BackendClient"),
                QStringLiteral("This backend has no /v1/tasks/moment_fits route; exporting with "
                               "the synchronous request (no cancel, limited by the backend's "
                               "request timeout)."),
                datasetId, m_sessionId, QStringLiteral("moment"));
        return saveMomentFits(datasetId, order, channelStart, channelEnd, maskEnabled,
                              thresholdValue, outputBasename, thresholdAuto);
    }
    BackendSaveMomentFitsResult result;
    result.error = followed.error;
    result.cancelled = followed.stoppedByUser();
    return result;
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
    // Not past the start of a quit (TaskQuitDrain.h): the loop cannot run
    // once the application is gone.
    QTimer quitWatch;
    QObject::connect(&quitWatch, &QTimer::timeout, &loop, [reply, &loop]() {
        if (visivo::TaskQuitDrain::instance().quitting()) {
            reply->abort();
            loop.quit();
        }
    });
    quitWatch.start(50);
    if (!visivo::TaskQuitDrain::instance().quitting())
        loop.exec();
    else
        reply->abort();
    quitWatch.stop();
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
    // Not while quitting: the quit would have to cancel it (TaskQuitDrain.h).
    if (visivo::refuseTaskWhileQuitting(result.error))
        return result;
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
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/moment")), body, error,
                        nullptr, &result.httpStatus);
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

bool BackendClient::isCompressedFitsName(const QString &path)
{
    // The suffixes the backend opens through its decompression cache.
    static const QStringList suffixes{ QStringLiteral(".fits.gz"), QStringLiteral(".fit.gz"),
                                       QStringLiteral(".fts.gz"), QStringLiteral(".fz") };
    for (const QString &suffix : suffixes) {
        if (path.endsWith(suffix, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

BackendTaskCreateResult BackendClient::createDecompressTask(const QString &path) const
{
    // On the application thread (DecompressBeforeOpen asks from there), the
    // request runs on a thread of its own while this one keeps painting in a
    // local loop. A quit's exit() ends that loop at once, and with it a
    // request still out on this thread: the backend may have created the
    // task, and its id, the one thing its DELETE needs, would be lost. Here the
    // answer is still waited for (join: bounded by the request's own timeout),
    // and the follower that comes next cancels the task (TaskQuitDrain.h).
    const QCoreApplication *app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread())
        return postDecompressTask(path);
    // The session first, on THIS thread: a mint already in flight lower in
    // this stack (a slow open's) holds the mint lock, which a sender thread
    // would wait on for ever while this thread waits for the sender. Here the
    // in-flight mint answers "not yet", and the request is made (and refused)
    // on this thread, as it always was.
    if (ensureSessionMinted() == SessionMint::Deferred) {
        if (mintInFlightOnThisThread())
            return postDecompressTask(path);   // refused at once, as before
        // The backend never answered the mint: say so, rather than mint again
        // inside the create (a second wait as long as the first).
        BackendTaskCreateResult refused;
        refused.error = QStringLiteral("Backend session not established yet: %1 is unreachable.")
                                .arg(m_baseUrl);
        return refused;
    }
    const BackendClient client = *this;
    BackendTaskCreateResult made;
    std::atomic_bool done{ false };
    std::thread sender([&client, &path, &made, &done]() {
        made = client.postDecompressTask(path);
        done = true;
    });
    {
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, &done]() {
            if (done)
                loop.quit();
        });
        poll.start(20);
        if (!done)
            loop.exec();
    }
    sender.join();
    // A token the sender's copy adopted (a 401 retry with the token file) is
    // this client's from now on too.
    if (client.token() != m_token)
        const_cast<BackendClient *>(this)->m_token = client.token();
    return made;
}

BackendTaskCreateResult BackendClient::postDecompressTask(const QString &path) const
{
    BackendTaskCreateResult result;
    // Not while quitting: the quit would have to cancel it (TaskQuitDrain.h).
    if (visivo::refuseTaskWhileQuitting(result.error))
        return result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("path"), path } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/decompress")), body, error,
                        nullptr, &result.httpStatus);
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

QString BackendClient::decompressForOpen(const QString &path) const
{
    const BackendTaskCreateResult created = createDecompressTask(path);
    // An older backend has no route (it refuses compressed files on open, with
    // its own message); a missing file is a 404 too, and the open says so.
    if (!created.valid && (created.httpStatus == 404 || created.httpStatus == 405))
        return {};
    // Opened before (the copy is cached) or not compressed after all: the
    // backend says so in the create response, and there is nothing to follow.
    if (decompressionAlreadyDone(created))
        return {};
    QElapsedTimer clock;
    clock.start();
    // Under the quit drain like every follower (TaskQuitDrain.h): quitting
    // aborts it, and it cancels the decompression instead of retrying status
    // reads the quit abandons until the task counts as lost.
    visivo::BackendTaskControl control;
    const visivo::TaskQuitDrain::Follow follow(visivo::TaskQuitDrain::instance(), &control);
    visivo::TaskFollowHooks hooks;
    hooks.poll = [this, &control](const QString &id) {
        return requestTaskStatus(id, &control.abortRequested);
    };
    hooks.cancel = [this](const QString &id, QString &error) {
        const int timeoutMs =
                visivo::TaskQuitDrain::instance().quitting() ? visivo::kQuitCancelTimeoutMs : 0;
        if (!visivo::TaskQuitDrain::instance().quitting())
            return cancelTask(id, error, timeoutMs);
        // Quitting: this may be the GUI thread, under an exit() already
        // asked for, where an event loop returns at once and the DELETE would
        // be dropped. It is sent from a thread of its own, joined here (not a
        // pool job: waiting on that could run it right here).
        const BackendClient client = *this;
        bool ok = false;
        QString threadError;
        std::thread sender([&client, &id, &ok, &threadError, timeoutMs]() {
            ok = client.cancelTask(id, threadError, timeoutMs);
        });
        sender.join();
        error = threadError;
        return ok;
    };
    // A local event loop rather than a sleep: openDataset() runs on the GUI
    // thread too, which must keep painting while a large file decompresses.
    hooks.sleepMs = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    hooks.nowMs = [&clock]() { return clock.elapsed(); };
    visivo::TaskPollPolicy policy;
    policy.maxPollMs = 1000;
    const visivo::TaskFollowResult r = visivo::followBackendTask(created, hooks, &control, policy);
    if (r.outcome == visivo::TaskOutcome::Completed)
        return {};
    qWarning().noquote() << QStringLiteral("[decompress] %1: %2").arg(path, r.error);
    return r.error.isEmpty() ? QStringLiteral("The backend could not decompress %1.").arg(path)
                             : r.error;
}

BackendTaskCreateResult BackendClient::createPvTask(const QString &datasetId,
                                                    const std::vector<std::array<double, 2>> &vertices,
                                                    int widthPixels) const
{
    BackendTaskCreateResult result;
    // Not while quitting: the quit would have to cancel it (TaskQuitDrain.h).
    if (visivo::refuseTaskWhileQuitting(result.error))
        return result;
    QString error;
    QJsonArray vertexArray;
    for (const auto &v : vertices) {
        vertexArray.append(QJsonArray{ v[0], v[1] });
    }
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("vertices"), vertexArray },
                                          { QStringLiteral("width_pixels"), widthPixels } });
    // The status tells an older backend with no task route (404/405) from a
    // refusal: only the first may fall back to the synchronous route.
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/pv")), body, error,
                        nullptr, &result.httpStatus);
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

BackendFindersResult BackendClient::listFinders() const
{
    BackendFindersResult result;
    QString error;
    const QByteArray payload = performGet(QUrl(m_baseUrl + QStringLiteral("/v1/finders")), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!object.contains(QStringLiteral("finders"))) {
        result.error = QStringLiteral("The backend lists no finders (older backend?).");
        return result;
    }
    for (const auto &value : object.value(QStringLiteral("finders")).toArray()) {
        const QJsonObject f = value.toObject();
        BackendFinderInfo info;
        info.name = f.value(QStringLiteral("name")).toString();
        for (const auto &k : f.value(QStringLiteral("kinds")).toArray())
            info.kinds << k.toString();
        info.description = f.value(QStringLiteral("description")).toString();
        const QJsonObject params = f.value(QStringLiteral("parameters")).toObject();
        for (auto it = params.begin(); it != params.end(); ++it)
            info.parameters.insert(it.key(), it.value().toString());
        info.available = f.value(QStringLiteral("available")).toBool(false);
        info.reason = f.value(QStringLiteral("reason")).toString();
        if (!info.name.isEmpty())
            result.finders.append(info);
    }
    result.valid = true;
    return result;
}

BackendTaskCreateResult BackendClient::createSourceFindTask(const QString &datasetId,
                                                            const QString &finder,
                                                            const QJsonObject &params) const
{
    BackendTaskCreateResult result;
    // Not while quitting: the quit would have to cancel it (TaskQuitDrain.h).
    if (visivo::refuseTaskWhileQuitting(result.error))
        return result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("finder"), finder },
                                          { QStringLiteral("params"), params } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/sourcefind")), body, error,
                        nullptr, &result.httpStatus);
    if (!error.isEmpty()) {
        // A refusal carries its reason in the body as FastAPI's {"detail": …};
        // `error` is Qt's "server replied: …". The words are what the user reads.
        result.error = visivo::refusalText(result.httpStatus, payload, error);
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.taskId = object.value(QStringLiteral("task_id")).toString();
    result.status = object.value(QStringLiteral("status")).toString();
    return result;
}

BackendSofiaSuggestResult BackendClient::suggestSofia2Parameters(const QString &datasetId,
                                                                const QJsonObject &request) const
{
    BackendSofiaSuggestResult result;
    QString error;
    const QString safeSeg = QString::fromUtf8(QUrl::toPercentEncoding(datasetId));
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/sofia2/suggest/") + safeSeg),
                        QJsonDocument(request), error, nullptr, &result.httpStatus);
    if (!error.isEmpty()) {
        result.error = visivo::refusalText(result.httpStatus, payload, error);
        return result;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        result.error = QStringLiteral("The backend's suggestion is not JSON.");
        return result;
    }
    result.body = doc.object();
    return result;
}

bool BackendClient::sendBoundedDelete(const QString &path, int timeoutMs, QByteArray &raw,
                                      QString &error) const
{
    const int timeout = timeoutMs > 0 ? timeoutMs : kCancelTaskTimeoutMs;
    QNetworkRequest req(QUrl(m_baseUrl + path));
    // Hand-built, as deleteExport: QNAM has no DELETE helper that takes the
    // session headers.
    BackendClient::applySessionHeaders(req, m_baseUrl, m_token, m_sessionId);
    // Bounded: a cancellation is asked for from a worker the user is waiting
    // on, and a backend that does not answer must not hold it.
    req.setTransferTimeout(timeout);
    auto *nam = getThreadLocalNam();
    auto *reply = nam->sendCustomRequest(req, "DELETE");
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer deadline;
    deadline.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();          // → finished() → loop.quit()
    });
    deadline.start(timeout);
    loop.exec();
    deadline.stop();
    // The loop can end without the reply: on the application thread after
    // QCoreApplication::exit(), a new event loop returns at once. The DELETE
    // was then never answered, and is no success.
    const bool answered = reply->isFinished();
    if (!answered)
        reply->abort();
    raw = reply->readAll();
    const bool ok = answered && reply->error() == QNetworkReply::NoError;
    if (!answered) {
        error = QStringLiteral("the cancellation was not sent: the event loop had ended");
    } else if (timedOut) {
        error = QStringLiteral("the backend did not confirm the cancellation within %1 s")
                        .arg(qMax(1, timeout / 1000));
    } else if (!ok) {
        const QString detail = QJsonDocument::fromJson(raw).object().value(QStringLiteral("detail")).toString();
        error = detail.isEmpty() ? reply->errorString() : detail;
    }
    reply->deleteLater();
    return ok && !timedOut;
}

bool BackendClient::cancelTask(const QString &taskId, QString &error, int timeoutMs) const
{
    const QString safeSeg = QString::fromUtf8(QUrl::toPercentEncoding(taskId, "", "/"));
    QByteArray raw;
    return sendBoundedDelete(QStringLiteral("/v1/tasks/") + safeSeg, timeoutMs, raw, error)
           && QJsonDocument::fromJson(raw).object().value(QStringLiteral("cancelled")).toBool(true);
}

BackendTaskCreateResult BackendClient::createRenderTask(const QString &taskPath,
                                                        const QJsonObject &request) const
{
    BackendTaskCreateResult result;
    // Not while quitting: the quit would have to cancel it (TaskQuitDrain.h).
    if (visivo::refuseTaskWhileQuitting(result.error))
        return result;
    QString error;
    const QByteArray payload = performPost(QUrl(m_baseUrl + taskPath), QJsonDocument(request),
                                           error, nullptr, &result.httpStatus);
    if (!error.isEmpty()) {
        // On an HTTP error performPost returns the reply's body: the reason
        // ("detail"), or FastAPI's bare "Not Found" of an older backend.
        result.error = error;
        result.errorBody = payload;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.taskId = object.value(QStringLiteral("task_id")).toString();
    result.valid = object.value(QStringLiteral("valid")).toBool(false) && !result.taskId.isEmpty();
    result.error = object.value(QStringLiteral("error")).toString();
    result.status = object.value(QStringLiteral("status")).toString();
    return result;
}

bool BackendClient::deleteRender(const QString &taskId, QString &error, int timeoutMs) const
{
    const QString safeSeg = QString::fromUtf8(QUrl::toPercentEncoding(taskId, "", "/"));
    QByteArray raw;
    return sendBoundedDelete(QStringLiteral("/v1/particle_render/") + safeSeg, timeoutMs, raw,
                             error);
}

bool BackendClient::closeRenderSession(const QString &renderSessionId, QString &error,
                                       int timeoutMs) const
{
    const QString safeSeg = QString::fromUtf8(QUrl::toPercentEncoding(renderSessionId, "", "/"));
    QByteArray raw;
    return sendBoundedDelete(QStringLiteral("/v1/render/") + safeSeg, timeoutMs, raw, error);
}

bool BackendClient::closeDataset(const QString &datasetId, QString &error) const
{
    if (visivo::TaskQuitDrain::instance().quitting()) {
        error = QStringLiteral("not sent: the application is quitting");
        return false;
    }
    const QString safeSeg = QString::fromUtf8(QUrl::toPercentEncoding(datasetId, "", "/"));
    QByteArray raw;
    return sendBoundedDelete(QStringLiteral("/v1/datasets/") + safeSeg, 5000, raw, error);
}

BackendTaskStatusResult BackendClient::requestTaskStatus(const QString &taskId,
                                                         const std::atomic_bool *abort) const
{
    BackendTaskStatusResult result;
    QString error;
    const QByteArray payload =
            performGet(QUrl(m_baseUrl + QStringLiteral("/v1/tasks/") + taskId), error, abort);
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

// ── Following a long task (issue #3) ─────────────────────────────────────────

visivo::TaskFollowResult visivo::followBackendTask(const BackendClient &client,
                                                   const BackendTaskCreateResult &created,
                                                   const QString &logTag,
                                                   BackendTaskControl *control,
                                                   const TaskPollPolicy &policy)
{
    // The Diagnostics "Operation" column.
    const QString operation = logTag.contains(QStringLiteral("moment"), Qt::CaseInsensitive)
            ? QStringLiteral("moment")
            : logTag.contains(QStringLiteral("pv"), Qt::CaseInsensitive) ? QStringLiteral("pv")
                                                                         : QString();
    const QString sessionId = client.sessionId();
    auto publish = [&](DiagnosticsLevel level, const QString &message) {
        DiagnosticsManager::instance()->publish(level, DiagnosticsCategory::Task,
                                                QStringLiteral("BackendClient"), message, {},
                                                sessionId, operation);
    };

    if (created.valid && !created.taskId.isEmpty()) {
        qDebug().noquote() << QStringLiteral("%1 created task_id=%2 status=%3")
                                      .arg(logTag, created.taskId, created.status);
        publish(DiagnosticsLevel::Info,
                QStringLiteral("Task created (%1); following it until it ends.")
                        .arg(created.taskId));
    }

    // Followed under the quit drain (TaskQuitDrain.h): quitting aborts it and
    // waits for this worker while the application still exists. A follow
    // without a control gets one, so that it can be aborted too.
    BackendTaskControl ownControl;
    if (!control)
        control = &ownControl;
    const TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), control);

    QElapsedTimer clock;
    clock.start();
    QString lastStatus;
    TaskFollowHooks hooks;
    // The status read watches the abort flag: the poll that finds the task
    // completed may take minutes (the product rides in it), and Cancel must
    // not wait for it before sending the DELETE.
    hooks.poll = [&client, control](const QString &id) {
        return client.requestTaskStatus(id, control ? &control->abortRequested : nullptr);
    };
    hooks.cancel = [&client, &policy](const QString &id, QString &error) {
        // While quitting, the DELETE is bounded short: the exit waits for it.
        int timeoutMs = policy.cancelTimeoutMs;
        if (TaskQuitDrain::instance().quitting())
            timeoutMs = timeoutMs > 0 ? std::min(timeoutMs, kQuitCancelTimeoutMs)
                                      : kQuitCancelTimeoutMs;
        return client.cancelTask(id, error, timeoutMs);
    };
    // A plain sleep: this runs on a pool thread, which has no event loop of its
    // own to serve, and the network calls run their own local loops.
    hooks.sleepMs = [](int ms) { QThread::msleep(static_cast<unsigned long>(ms)); };
    hooks.nowMs = [&clock]() { return clock.elapsed(); };
    hooks.observed = [&](const BackendTaskStatusResult &s) {
        // Log a change of state, not every poll: a 25-minute task polls ~500 times.
        const QString now = s.status.isEmpty() ? QStringLiteral("(unreadable)") : s.status;
        if (now != lastStatus) {
            qDebug().noquote() << QStringLiteral("%1 task_id=%2 status=%3 after %4 s %5")
                                          .arg(logTag, created.taskId, now)
                                          .arg(clock.elapsed() / 1000)
                                          .arg(s.error);
            lastStatus = now;
        }
    };

    const TaskFollowResult r = followBackendTask(created, hooks, control, policy);
    const qint64 seconds = clock.elapsed() / 1000;
    switch (r.outcome) {
    case TaskOutcome::Completed:
        publish(DiagnosticsLevel::Info,
                QStringLiteral("Task completed (%1) after %2 s%3.")
                        .arg(r.taskId)
                        .arg(seconds)
                        .arg(r.status.cacheHit ? QStringLiteral(", cache hit") : QString()));
        break;
    case TaskOutcome::Failed:
        publish(DiagnosticsLevel::Error,
                QStringLiteral("Task failed (%1): %2").arg(r.taskId, r.error));
        break;
    case TaskOutcome::Cancelled:
    case TaskOutcome::Aborted:
        publish(DiagnosticsLevel::Warning, r.error);
        break;
    case TaskOutcome::Lost:
        publish(DiagnosticsLevel::Error, r.error);
        break;
    case TaskOutcome::CreateFailed:
        publish(DiagnosticsLevel::Error,
                QStringLiteral("Task creation failed: %1").arg(r.error));
        break;
    case TaskOutcome::RouteMissing:
        publish(DiagnosticsLevel::Warning,
                QStringLiteral("The backend has no task route (HTTP %1); using the synchronous "
                               "request.")
                        .arg(created.httpStatus));
        break;
    }
    qDebug().noquote() << QStringLiteral("%1 task_id=%2 outcome=%3 polls=%4 after %5 s")
                                  .arg(logTag, r.taskId)
                                  .arg(static_cast<int>(r.outcome))
                                  .arg(r.polls)
                                  .arg(seconds);
    return r;
}
