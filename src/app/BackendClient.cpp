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

namespace {

QByteArray decodeCompressedPayload(const QByteArray &encoded, const QString &compression,
                                   QString &error)
{
    const QByteArray raw = QByteArray::fromBase64(encoded);
    if (compression.isEmpty() || compression == QStringLiteral("none")) {
        return raw;
    }
    if (compression == QStringLiteral("qt-zlib")) {
        const QByteArray uncompressed = qUncompress(raw);
        if (uncompressed.isEmpty()) {
            error = QStringLiteral("Failed to decompress backend payload.");
        }
        return uncompressed;
    }
    error = QStringLiteral("Unsupported backend compression: %1").arg(compression);
    return {};
}

} // namespace

namespace {
    QThreadStorage<QNetworkAccessManager*> s_namStorage;

    QNetworkAccessManager* getThreadLocalNam() {
        if (!s_namStorage.hasLocalData()) {
            s_namStorage.setLocalData(new QNetworkAccessManager());
        }
        return s_namStorage.localData();
    }
} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

BackendClient::BackendClient(QString baseUrl, QString token)
    : m_baseUrl(std::move(baseUrl)), m_token(std::move(token))
{
    // R1: Auto-resolve token from env / file if not provided explicitly.
    if (m_token.isEmpty()) {
        m_token = BackendClient::readTokenFile();
        return;
    }

    // For LOCAL backends, ~/.visivo_token is written by the running backend's
    // auth.py at startup and is therefore the authoritative source. If the
    // explicit token from Settings is stale (e.g. the backend was restarted
    // and regenerated its token), every request would 401 and trigger the
    // retry path, polluting the diagnostics log. Prefer the file token here
    // to short-circuit that ping-pong. Remote backends keep using the
    // explicit token because ~/.visivo_token belongs to the local machine.
    const QUrl parsed(m_baseUrl);
    const QString host = parsed.host().trimmed().toLower();
    const bool isLocal = host.isEmpty()
                         || host == QLatin1StringView("localhost")
                         || host == QLatin1StringView("127.0.0.1")
                         || host == QLatin1StringView("::1");
    if (isLocal) {
        const QString fileToken = BackendClient::readTokenFile();
        if (!fileToken.isEmpty() && fileToken != m_token) {
            m_token = fileToken;
        }
    }
}

// ── Accessors ─────────────────────────────────────────────────────────────────

QString BackendClient::baseUrl() const
{
    return m_baseUrl;
}

void BackendClient::setBaseUrl(const QString &url)
{
    m_baseUrl = url;
}

QString BackendClient::token() const
{
    return m_token;
}

void BackendClient::setToken(const QString &token)
{
    m_token = token;
}

QString BackendClient::sessionId() const
{
    return m_sessionId;
}

void BackendClient::setSessionId(const QString &sessionId)
{
    m_sessionId = sessionId;
}

// ── Static helpers ────────────────────────────────────────────────────────────

QString BackendClient::readTokenFile()
{
    // Priority 1: environment variable (set by launch script or user shell).
    const QString envToken =
            QProcessEnvironment::systemEnvironment().value(QStringLiteral("VISIVO_TOKEN"));
    if (!envToken.isEmpty()) {
        return envToken.trimmed();
    }

    // Priority 2: ~/.visivo_token written by the Python backend at startup.
    const QString tokenPath =
            QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                    .absoluteFilePath(QStringLiteral(".visivo_token"));
    QFile tokenFile(tokenPath);
    if (tokenFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString token = QString::fromUtf8(tokenFile.readAll()).trimmed();
        tokenFile.close();
        return token;
    }

    return {};
}

// ── Request builder ───────────────────────────────────────────────────────────

QNetworkRequest BackendClient::buildRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    // R1: Authentication header.
    if (!m_token.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("X-Visivo-Token"), m_token.toUtf8());
    }

    // R3: Session isolation header.
    if (!m_sessionId.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("X-Visivo-Session"), m_sessionId.toUtf8());
    }

    return request;
}

// ── API calls ─────────────────────────────────────────────────────────────────

BackendHealthResult BackendClient::health() const
{
    BackendHealthResult result;
    QString error;
    // R8: /v1/ prefix on all routes.
    const QByteArray payload = performGet(QUrl(m_baseUrl + QStringLiteral("/v1/health")), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.ok = object.value(QStringLiteral("ok")).toBool(false);
    result.workers = object.value(QStringLiteral("workers")).toInt();
    result.activeSessions = object.value(QStringLiteral("active_sessions")).toInt();
    result.productCacheEntries = object.value(QStringLiteral("product_cache_entries")).toInt();
    result.productCacheCapacity = object.value(QStringLiteral("product_cache_capacity")).toInt();
    result.taskRegistryEntries = object.value(QStringLiteral("task_registry_entries")).toInt();
    result.taskTtlEnabled = object.value(QStringLiteral("task_ttl_enabled")).toBool(false);
    result.taskTtlSeconds = object.value(QStringLiteral("task_ttl_seconds")).toInt();

    // Live activity: running counter + last-finished trail.
    result.runningTasks = object.value(QStringLiteral("running_tasks")).toInt();
    const QJsonArray recentArr = object.value(QStringLiteral("recent_tasks")).toArray();
    result.recentTasks.clear();
    result.recentTasks.reserve(static_cast<std::size_t>(recentArr.size()));
    for (const QJsonValue &v : recentArr) {
        const QJsonObject t = v.toObject();
        BackendRecentTask task;
        task.operation       = t.value(QStringLiteral("operation")).toString();
        task.status          = t.value(QStringLiteral("status")).toString();
        task.durationSeconds = t.value(QStringLiteral("duration_seconds")).toDouble(0.0);
        task.ageSeconds      = t.value(QStringLiteral("age_seconds")).toDouble(0.0);
        task.cacheHit        = t.value(QStringLiteral("cache_hit")).toBool(false);
        result.recentTasks.push_back(std::move(task));
    }

    if (!result.ok) {
        result.error = QStringLiteral("Backend health check failed.");
    }
    return result;
}

BackendListFilesResult BackendClient::listFiles(const QString &path) const
{
    BackendListFilesResult result;
    QString error;
    QUrl url(m_baseUrl + QStringLiteral("/v1/files/list"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("path"), path);
    url.setQuery(query);
    const QByteArray payload = performGet(url, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.currentPath = object.value(QStringLiteral("current_path")).toString();
    const QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    result.entries.reserve(static_cast<std::size_t>(entries.size()));
    for (const QJsonValue &value : entries) {
        const QJsonObject e = value.toObject();
        result.entries.push_back({ e.value(QStringLiteral("name")).toString(),
                                   e.value(QStringLiteral("path")).toString(),
                                   e.value(QStringLiteral("type")).toString(),
                                   e.value(QStringLiteral("size")).toInteger(),
                                   e.value(QStringLiteral("modified_time")).toString(),
                                   e.value(QStringLiteral("is_fits")).toBool(false) });
    }
    return result;
}

BackendSessionDatasetsResult BackendClient::listSessionDatasets(const QString &sessionId) const
{
    BackendSessionDatasetsResult result;
    result.sessionId = sessionId;
    if (sessionId.isEmpty()) {
        result.error = QStringLiteral("listSessionDatasets() requires a non-empty session id.");
        return result;
    }
    QString error;
    QUrl url(m_baseUrl + QStringLiteral("/v1/sessions/") + sessionId
             + QStringLiteral("/datasets"));
    const QByteArray payload = performGet(url, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    // The endpoint omits a `valid` field on the happy path; presence of
    // `session_id` is the success marker. Honour `error`/`detail` if set
    // (e.g. 404 on stale session) so the UI can show a meaningful message.
    if (object.contains(QStringLiteral("error"))) {
        result.error = object.value(QStringLiteral("error")).toString();
        return result;
    }
    if (object.contains(QStringLiteral("detail")) && !object.contains(QStringLiteral("datasets"))) {
        result.error = object.value(QStringLiteral("detail")).toString();
        return result;
    }
    result.sessionId = object.value(QStringLiteral("session_id")).toString(sessionId);
    const QJsonArray entries = object.value(QStringLiteral("datasets")).toArray();
    result.datasets.reserve(static_cast<std::size_t>(entries.size()));
    for (const QJsonValue &v : entries) {
        const QJsonObject e = v.toObject();
        BackendSessionDatasetEntry d;
        d.datasetId = e.value(QStringLiteral("dataset_id")).toString();
        d.kind      = e.value(QStringLiteral("kind")).toString();
        d.path      = e.value(QStringLiteral("path")).toString();
        d.width     = e.value(QStringLiteral("width")).toInt(0);
        d.height    = e.value(QStringLiteral("height")).toInt(0);
        d.depth     = e.value(QStringLiteral("depth")).toInt(0);
        result.datasets.push_back(d);
    }
    result.valid = true;
    return result;
}

BackendDiagnosticsEventsResult BackendClient::pollBackendDiagnostics(qint64 sinceId,
                                                                     int limit) const
{
    BackendDiagnosticsEventsResult result;
    QString error;
    QUrl url(m_baseUrl + QStringLiteral("/v1/diagnostics/events"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("since"), QString::number(std::max<qint64>(0, sinceId)));
    q.addQueryItem(QStringLiteral("limit"), QString::number(std::clamp(limit, 1, 2000)));
    url.setQuery(q);
    const QByteArray payload = performGet(url, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    if (obj.contains(QStringLiteral("error"))
        && !obj.contains(QStringLiteral("events"))) {
        result.error = obj.value(QStringLiteral("error")).toString();
        return result;
    }
    result.valid  = obj.value(QStringLiteral("valid")).toBool(true);
    result.lastId = static_cast<qint64>(
            obj.value(QStringLiteral("last_id")).toDouble(0.0));
    const QJsonArray arr = obj.value(QStringLiteral("events")).toArray();
    result.events.reserve(static_cast<std::size_t>(arr.size()));
    for (const QJsonValue &v : arr) {
        const QJsonObject e = v.toObject();
        BackendDiagnosticsEvent ev;
        ev.id       = static_cast<qint64>(e.value(QStringLiteral("id")).toDouble(0.0));
        ev.ts       = e.value(QStringLiteral("ts")).toDouble(0.0);
        ev.level    = e.value(QStringLiteral("level")).toString();
        ev.category = e.value(QStringLiteral("category")).toString();
        ev.source   = e.value(QStringLiteral("source")).toString();
        ev.message  = e.value(QStringLiteral("message")).toString();
        ev.datasetId    = e.value(QStringLiteral("dataset_id")).toString();
        ev.sessionId    = e.value(QStringLiteral("session_id")).toString();
        ev.operationTag = e.value(QStringLiteral("operation_tag")).toString();
        result.events.push_back(std::move(ev));
    }
    return result;
}

BackendFileHeaderResult BackendClient::fileHeader(const QString &path) const
{
    BackendFileHeaderResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("path"), path } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/files/header")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    const QJsonArray cards = object.value(QStringLiteral("cards")).toArray();
    result.cards.reserve(cards.size());
    for (const QJsonValue &v : cards) {
        result.cards.push_back(v.toString());
    }
    return result;
}

namespace {

/// Shared parser for the OpenDatasetResponse JSON shape produced by both
/// /v1/datasets/open and /v1/datasets/open_url. Factored out so the two
/// endpoints don't drift apart.
static BackendOpenDatasetResult parseOpenDatasetObject(const QJsonObject &object,
                                                        QString &outSessionId)
{
    BackendOpenDatasetResult result;
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.datasetId = object.value(QStringLiteral("dataset_id")).toString();
    result.sessionId = object.value(QStringLiteral("session_id")).toString();
    outSessionId = result.sessionId;
    result.kind = object.value(QStringLiteral("kind")).toString();
    result.activeAxes = object.value(QStringLiteral("active_axes")).toInt();
    result.width = object.value(QStringLiteral("width")).toInt();
    result.height = object.value(QStringLiteral("height")).toInt();
    result.depth = object.value(QStringLiteral("depth")).toInt();
    result.degenerateAxesSummary =
            object.value(QStringLiteral("degenerate_axes_summary")).toString();
    result.wcsStatus = object.value(QStringLiteral("wcs_status")).toString(QStringLiteral("ok"));
    result.wcsWarningMessage = object.value(QStringLiteral("wcs_warning_message")).toString();
    const QJsonArray wcsSanitizedAxes = object.value(QStringLiteral("wcs_sanitized_axes")).toArray();
    result.wcsSanitizedAxes.clear();
    result.wcsSanitizedAxes.reserve(wcsSanitizedAxes.size());
    for (const QJsonValue &value : wcsSanitizedAxes) {
        result.wcsSanitizedAxes.push_back(value.toInt());
    }
    result.spectralAxisType = object.value(QStringLiteral("spectral_axis_type")).toString();
    result.spectralAxisUnit = object.value(QStringLiteral("spectral_axis_unit")).toString();

    const QJsonArray spacing = object.value(QStringLiteral("spacing")).toArray();
    const QJsonArray origin = object.value(QStringLiteral("origin")).toArray();
    const QJsonArray ctype = object.value(QStringLiteral("ctype")).toArray();
    const QJsonArray cunit = object.value(QStringLiteral("cunit")).toArray();
    const QJsonArray crval = object.value(QStringLiteral("crval")).toArray();
    const QJsonArray crpix = object.value(QStringLiteral("crpix")).toArray();
    const QJsonArray cdelt = object.value(QStringLiteral("cdelt")).toArray();
    for (int i = 0; i < 3; ++i) {
        if (i < spacing.size())
            result.spacing[static_cast<std::size_t>(i)] = spacing.at(i).toDouble(1.0);
        if (i < origin.size())
            result.origin[static_cast<std::size_t>(i)] = origin.at(i).toDouble(0.0);
        if (i < ctype.size())
            result.ctype[static_cast<std::size_t>(i)] = ctype.at(i).toString();
        if (i < cunit.size())
            result.cunit[static_cast<std::size_t>(i)] = cunit.at(i).toString();
        if (i < crval.size())
            result.crval[static_cast<std::size_t>(i)] = crval.at(i).toDouble(0.0);
        if (i < crpix.size())
            result.crpix[static_cast<std::size_t>(i)] = crpix.at(i).toDouble(1.0);
        if (i < cdelt.size())
            result.cdelt[static_cast<std::size_t>(i)] = cdelt.at(i).toDouble(1.0);
    }
    result.pixelScaleArcsec =
            object.value(QStringLiteral("pixel_scale_arcsec")).toDouble(0.0);
    const QJsonValue effCdelt = object.value(QStringLiteral("effective_cdelt"));
    if (effCdelt.isArray()) {
        const QJsonArray ea = effCdelt.toArray();
        if (ea.size() >= 2) {
            result.effectiveCdelt[0] = ea.at(0).toDouble(1.0);
            result.effectiveCdelt[1] = ea.at(1).toDouble(1.0);
            result.effectiveCdelt[2] = ea.size() >= 3 ? ea.at(2).toDouble(1.0) : 1.0;
            result.effectiveCdeltValid = true;
        }
    }
    const QJsonValue ref00 = object.value(QStringLiteral("wcs_ref_sky_00"));
    if (ref00.isArray() && ref00.toArray().size() >= 2) {
        result.wcsRefSky00[0] = ref00.toArray().at(0).toDouble();
        result.wcsRefSky00[1] = ref00.toArray().at(1).toDouble();
        result.wcsRefSkyValid = true;
    }
    const QJsonValue ref0100 = object.value(QStringLiteral("wcs_ref_sky_0100"));
    if (ref0100.isArray() && ref0100.toArray().size() >= 2) {
        result.wcsRefSky0100[0] = ref0100.toArray().at(0).toDouble();
        result.wcsRefSky0100[1] = ref0100.toArray().at(1).toDouble();
    }
    const QJsonValue bmaj = object.value(QStringLiteral("beam_major"));
    const QJsonValue bmin = object.value(QStringLiteral("beam_minor"));
    const QJsonValue bpa  = object.value(QStringLiteral("beam_pa"));
    if (!bmaj.isNull()) result.beamMajorDeg = bmaj.toDouble();
    if (!bmin.isNull()) result.beamMinorDeg = bmin.toDouble();
    if (!bpa.isNull())  result.beamPaDeg    = bpa.toDouble();

    // Size-aware loading policy (Pattern B). Defaults preserve legacy
    // auto-full behaviour for backends that don't return these fields.
    const QJsonValue fileSize = object.value(QStringLiteral("file_size_bytes"));
    if (!fileSize.isUndefined() && !fileSize.isNull()) {
        result.fileSizeBytes = static_cast<qint64>(fileSize.toDouble(0.0));
    }
    const QJsonValue thresh = object.value(QStringLiteral("full_res_threshold_bytes"));
    if (!thresh.isUndefined() && !thresh.isNull()) {
        result.fullResThresholdBytes =
                static_cast<qint64>(thresh.toDouble(double(qint64(4) << 30)));
    }
    const QJsonValue mode = object.value(QStringLiteral("recommended_load_mode"));
    if (mode.isString()) {
        const QString s = mode.toString();
        if (s == QLatin1String("preview_only") || s == QLatin1String("auto_full")) {
            result.recommendedLoadMode = s;
        }
    }
    return result;
}

} // namespace

BackendOpenDatasetResult BackendClient::openDataset(const QString &path)
{
    BackendOpenDatasetResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("path"), path } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/datasets/open")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    QString sessionId;
    result = parseOpenDatasetObject(object, sessionId);

    // R3: Persist the session_id so subsequent requests use the same session.
    if (result.valid && !sessionId.isEmpty()) {
        m_sessionId = sessionId;
    }
    return result;
}

BackendOpenDatasetResult BackendClient::openDatasetFromUrl(
        const QString &url,
        const QString &obsId,
        const QString &remoteBearerToken)
{
    BackendOpenDatasetResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("url"), url },
            { QStringLiteral("obs_id"), obsId },
            { QStringLiteral("bearer_token"), remoteBearerToken } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/datasets/open_url")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    QString sessionId;
    result = parseOpenDatasetObject(object, sessionId);
    if (result.valid && !sessionId.isEmpty()) {
        m_sessionId = sessionId;
    }
    return result;
}

BackendOpenDatasetResult BackendClient::openSkavaDataset(const QString &obsId,
                                                            const QString &accessUrl)
{
    BackendOpenDatasetResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("obs_id"), obsId },
            { QStringLiteral("access_url"), accessUrl } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/datasets/open_skava")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    QString sessionId;
    result = parseOpenDatasetObject(object, sessionId);
    if (result.valid && !sessionId.isEmpty()) {
        m_sessionId = sessionId;
    }
    return result;
}

#if 0  // ── legacy inline parsing replaced by parseOpenDatasetObject ─────────
{
    BackendOpenDatasetResult result;
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.datasetId = object.value(QStringLiteral("dataset_id")).toString();
    result.sessionId = object.value(QStringLiteral("session_id")).toString();

    // R3: Persist the session_id so subsequent requests use the same session.
    if (result.valid && !result.sessionId.isEmpty()) {
        m_sessionId = result.sessionId;
    }

    result.kind = object.value(QStringLiteral("kind")).toString();
    result.activeAxes = object.value(QStringLiteral("active_axes")).toInt();
    result.width = object.value(QStringLiteral("width")).toInt();
    result.height = object.value(QStringLiteral("height")).toInt();
    result.depth = object.value(QStringLiteral("depth")).toInt();
    result.degenerateAxesSummary =
            object.value(QStringLiteral("degenerate_axes_summary")).toString();
    result.wcsStatus = object.value(QStringLiteral("wcs_status")).toString(QStringLiteral("ok"));
    result.wcsWarningMessage = object.value(QStringLiteral("wcs_warning_message")).toString();
    const QJsonArray wcsSanitizedAxes = object.value(QStringLiteral("wcs_sanitized_axes")).toArray();
    result.wcsSanitizedAxes.clear();
    result.wcsSanitizedAxes.reserve(wcsSanitizedAxes.size());
    for (const QJsonValue &value : wcsSanitizedAxes) {
        result.wcsSanitizedAxes.push_back(value.toInt());
    }
    result.spectralAxisType = object.value(QStringLiteral("spectral_axis_type")).toString();
    result.spectralAxisUnit = object.value(QStringLiteral("spectral_axis_unit")).toString();

    const QJsonArray spacing = object.value(QStringLiteral("spacing")).toArray();
    const QJsonArray origin = object.value(QStringLiteral("origin")).toArray();
    const QJsonArray ctype = object.value(QStringLiteral("ctype")).toArray();
    const QJsonArray cunit = object.value(QStringLiteral("cunit")).toArray();
    const QJsonArray crval = object.value(QStringLiteral("crval")).toArray();
    const QJsonArray crpix = object.value(QStringLiteral("crpix")).toArray();
    const QJsonArray cdelt = object.value(QStringLiteral("cdelt")).toArray();
    for (int i = 0; i < 3; ++i) {
        if (i < spacing.size()) {
            result.spacing[static_cast<std::size_t>(i)] = spacing.at(i).toDouble(1.0);
        }
        if (i < origin.size()) {
            result.origin[static_cast<std::size_t>(i)] = origin.at(i).toDouble(0.0);
        }
        if (i < ctype.size()) {
            result.ctype[static_cast<std::size_t>(i)] = ctype.at(i).toString();
        }
        if (i < cunit.size()) {
            result.cunit[static_cast<std::size_t>(i)] = cunit.at(i).toString();
        }
        if (i < crval.size()) {
            result.crval[static_cast<std::size_t>(i)] = crval.at(i).toDouble(0.0);
        }
        if (i < crpix.size()) {
            result.crpix[static_cast<std::size_t>(i)] = crpix.at(i).toDouble(1.0);
        }
        if (i < cdelt.size()) {
            result.cdelt[static_cast<std::size_t>(i)] = cdelt.at(i).toDouble(1.0);
        }
    }
    result.pixelScaleArcsec =
            object.value(QStringLiteral("pixel_scale_arcsec")).toDouble(0.0);
    const QJsonValue effCdelt = object.value(QStringLiteral("effective_cdelt"));
    if (effCdelt.isArray()) {
        const QJsonArray ea = effCdelt.toArray();
        if (ea.size() >= 2) {
            result.effectiveCdelt[0] = ea.at(0).toDouble(1.0);
            result.effectiveCdelt[1] = ea.at(1).toDouble(1.0);
            result.effectiveCdelt[2] = ea.size() >= 3 ? ea.at(2).toDouble(1.0) : 1.0;
            result.effectiveCdeltValid = true;
        }
    }
    const QJsonValue ref00   = object.value(QStringLiteral("wcs_ref_sky_00"));
    const QJsonValue ref0100 = object.value(QStringLiteral("wcs_ref_sky_0100"));
    if (ref00.isArray() && ref0100.isArray()) {
        const QJsonArray a00   = ref00.toArray();
        const QJsonArray a0100 = ref0100.toArray();
        if (a00.size() >= 2 && a0100.size() >= 2) {
            result.wcsRefSky00[0]   = a00.at(0).toDouble();
            result.wcsRefSky00[1]   = a00.at(1).toDouble();
            result.wcsRefSky0100[0] = a0100.at(0).toDouble();
            result.wcsRefSky0100[1] = a0100.at(1).toDouble();
            result.wcsRefSkyValid   = true;
        }
    }
    // Beam (BMAJ/BMIN in degrees, BPA in degrees E of N). Absent → NaN.
    const QJsonValue bmaj = object.value(QStringLiteral("beam_major"));
    const QJsonValue bmin = object.value(QStringLiteral("beam_minor"));
    const QJsonValue bpa  = object.value(QStringLiteral("beam_pa"));
    if (!bmaj.isNull()) result.beamMajorDeg = bmaj.toDouble();
    if (!bmin.isNull()) result.beamMinorDeg = bmin.toDouble();
    if (!bpa.isNull())  result.beamPaDeg    = bpa.toDouble();

    // Size-aware loading policy (Pattern B). Defaults preserve legacy
    // auto-full behaviour for backends that don't return these fields.
    const QJsonValue fileSize = object.value(QStringLiteral("file_size_bytes"));
    if (!fileSize.isUndefined() && !fileSize.isNull()) {
        result.fileSizeBytes = static_cast<qint64>(fileSize.toDouble(0.0));
    }
    const QJsonValue thresh = object.value(QStringLiteral("full_res_threshold_bytes"));
    if (!thresh.isUndefined() && !thresh.isNull()) {
        result.fullResThresholdBytes =
                static_cast<qint64>(thresh.toDouble(double(qint64(4) << 30)));
    }
    const QJsonValue mode = object.value(QStringLiteral("recommended_load_mode"));
    if (mode.isString()) {
        const QString s = mode.toString();
        if (s == QLatin1String("preview_only") || s == QLatin1String("auto_full")) {
            result.recommendedLoadMode = s;
        }
    }
    return result;
}
#endif  // ── end legacy inline parsing block ─────────────────────────────

BackendCatalogueInfo BackendClient::openCatalogue(const QString &path, const QString &format)
{
    BackendCatalogueInfo result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("path"), path },
                                          { QStringLiteral("format"), format } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/catalogue/open")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.datasetId = object.value(QStringLiteral("dataset_id")).toString();
    result.sessionId = object.value(QStringLiteral("session_id")).toString();
    result.numRows = object.value(QStringLiteral("num_rows")).toInt();
    if (!result.sessionId.isEmpty()) {
        this->setSessionId(result.sessionId);
    }
    const QJsonObject coord = object.value(QStringLiteral("coordinate_fields")).toObject();
    result.raField = coord.value(QStringLiteral("ra")).toString();
    result.decField = coord.value(QStringLiteral("dec")).toString();
    const QJsonArray fields = object.value(QStringLiteral("fields")).toArray();
    result.fields.reserve(fields.size());
    for (const QJsonValue &fieldValue : fields) {
        const QJsonObject field = fieldValue.toObject();
        result.fields.push_back({ field.value(QStringLiteral("name")).toString(),
                                  field.value(QStringLiteral("type")).toString() });
    }
    return result;
}

BackendSampSendResult BackendClient::sendFitsToSamp(const QString &path,
                                                    const QString &filename) const
{
    BackendSampSendResult result;
    QString error;
    QJsonObject bodyObj{ { QStringLiteral("path"), path } };
    if (!filename.isEmpty()) {
        bodyObj.insert(QStringLiteral("filename"), filename);
    }
    const QJsonDocument body(bodyObj);
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/samp/send-fits")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.jobId = object.value(QStringLiteral("job_id")).toString();
    result.sentDirect = object.value(QStringLiteral("sent_direct")).toBool(false);
    return result;
}

BackendSampSendResult BackendClient::sendCatalogueToSamp(const QString &path,
                                                         const QString &format,
                                                         const QString &filename) const
{
    BackendSampSendResult result;
    QString error;
    QJsonObject bodyObj{
        { QStringLiteral("path"), path },
        { QStringLiteral("format"), format },
    };
    if (!filename.isEmpty()) {
        bodyObj.insert(QStringLiteral("filename"), filename);
    }
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/samp/send-catalogue")),
                        QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.jobId = object.value(QStringLiteral("job_id")).toString();
    result.sentDirect = object.value(QStringLiteral("sent_direct")).toBool(false);
    return result;
}

BackendSampImportResult BackendClient::importSampUrl(const QString &url,
                                                     const QString &filename) const
{
    BackendSampImportResult result;
    QString error;
    QJsonObject bodyObj{ { QStringLiteral("url"), url } };
    if (!filename.isEmpty()) {
        bodyObj.insert(QStringLiteral("filename"), filename);
    }
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/samp/import-url")),
                        QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.token = object.value(QStringLiteral("token")).toString();
    result.backendPath = object.value(QStringLiteral("backend_path")).toString();
    result.filename = object.value(QStringLiteral("filename")).toString();
    return result;
}

BackendSampImportResult BackendClient::uploadFileToSampStorage(const QString &localPath,
                                                               const QString &filename) const
{
    BackendSampImportResult result;
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Failed to read local file: %1").arg(localPath);
        return result;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QString error;
    const QString resolvedName =
            filename.isEmpty() ? QFileInfo(localPath).fileName() : filename;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("filename"), resolvedName },
            { QStringLiteral("content_base64"), QString::fromUtf8(raw.toBase64()) } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/samp/upload-file")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.token = object.value(QStringLiteral("token")).toString();
    result.backendPath = object.value(QStringLiteral("backend_path")).toString();
    result.filename = object.value(QStringLiteral("filename")).toString();
    return result;
}

BackendCubePreviewResult BackendClient::requestPreview(const QString &datasetId,
                                                       int downsample) const
{
    BackendCubePreviewResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("downsample"), downsample } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/preview")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.width = object.value(QStringLiteral("width")).toInt();
    result.height = object.value(QStringLiteral("height")).toInt();
    result.depth = object.value(QStringLiteral("depth")).toInt();
    result.scalarType = object.value(QStringLiteral("scalar_type")).toString();
    result.rangeMin = object.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax = object.value(QStringLiteral("range_max")).toDouble();
    result.data = decodePayload(object, QStringLiteral("data_base64"),
                                QStringLiteral("compression"), result.error);
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

BackendCubeSliceResult BackendClient::requestSlice(const QString &datasetId, const QString &axis,
                                                   int index) const
{
    BackendCubeSliceResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("axis"), axis },
                                          { QStringLiteral("index"), index } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/slice")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.width = object.value(QStringLiteral("width")).toInt();
    result.height = object.value(QStringLiteral("height")).toInt();
    result.scalarType = object.value(QStringLiteral("scalar_type")).toString();
    result.rangeMin = object.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax = object.value(QStringLiteral("range_max")).toDouble();
    result.data = decodePayload(object, QStringLiteral("data_base64"),
                                QStringLiteral("compression"), result.error);
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

// Synchronous POST that captures status code + response headers + raw body.
// Mirrors performPost's token retry but exposes more of the reply because
// the binary subvolume endpoint puts metadata in X-Visivo-* response
// headers instead of a JSON envelope.
// Returns the body; ``statusOut`` is the HTTP status code (0 on transport
// error), ``headersOut`` is a snapshot of the response headers (lowercased
// names → raw values), ``error`` is set on transport/timeout failures.
static QByteArray performBinaryPost(QNetworkAccessManager *nam,
                                    const QNetworkRequest &reqTemplate,
                                    const QByteArray &bodyBytes,
                                    int timeoutMs,
                                    int &statusOut,
                                    QMap<QByteArray, QByteArray> &headersOut,
                                    QString &error)
{
    statusOut = 0;
    headersOut.clear();
    QNetworkRequest request = reqTemplate;
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QEventLoop loop;
    QNetworkReply *reply = nam->post(request, bodyBytes);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();
        loop.quit();
    });
    timeoutTimer.start(timeoutMs);
    loop.exec();
    timeoutTimer.stop();

    const QByteArray body = reply->readAll();
    statusOut = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    for (const auto &header : reply->rawHeaderPairs()) {
        headersOut.insert(header.first.toLower(), header.second);
    }
    if (timedOut) {
        error = QStringLiteral("Backend binary request timed out after %1 ms").arg(timeoutMs);
    } else if (reply->error() != QNetworkReply::NoError && statusOut < 400) {
        error = reply->errorString();
    }
    reply->deleteLater();
    return body;
}


BackendCubeSubvolumeResult BackendClient::requestSubvolume(const QString &datasetId, int x0,
                                                           int x1, int y0, int y1, int z0,
                                                           int z1) const
{
    BackendCubeSubvolumeResult result;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("x0"), x0 },
                                          { QStringLiteral("x1"), x1 },
                                          { QStringLiteral("y0"), y0 },
                                          { QStringLiteral("y1"), y1 },
                                          { QStringLiteral("z0"), z0 },
                                          { QStringLiteral("z1"), z1 } });
    const QByteArray bodyBytes = body.toJson(QJsonDocument::Compact);

    // ── 1) Binary endpoint with parallel Z-slab fetches ────────────────
    // /v1/cube/subvolume_bin returns raw little-endian float32 with
    // metadata in X-Visivo-* headers. To saturate the SSH tunnel we
    // split the Z range into N=4 slabs and fire 4 POSTs concurrently
    // through 4 distinct thread-local QNetworkAccessManagers — each
    // POST is its own TCP stream multiplexed over the tunnel, so
    // throughput stacks roughly linearly until the tunnel's outer TCP
    // saturates.
    //
    // Fallback to the JSON endpoint on:
    //   - 404 on any slab (older backend without the binary route)
    //   - any transport error  — keep the legacy path as a safety net.
    {
        struct SlabResult {
            bool ok{ false };
            int status{ 0 };
            int width{ 0 }, height{ 0 }, depth{ 0 };
            double rangeMin{ 0.0 }, rangeMax{ 0.0 };
            QString scalarType, spectralAxisType, spectralAxisUnit, bunit;
            QByteArray data;
            QString error;
        };

        const QUrl binUrl(m_baseUrl + QStringLiteral("/v1/cube/subvolume_bin"));
        const int timeoutMs =
                static_cast<int>(this->requestTimeoutFor(binUrl).count());
        const QString token = this->m_token;
        const QString sessionId = this->m_sessionId;
        const QString baseUrl = m_baseUrl;

        // Split [z0, z1] in N slabs. Sub-Z-axis chunks are concatenated
        // back along the spectral axis. We use 4 by default — empirically
        // saturates the SSH tunnel without thrashing it; raise via env
        // ``VISIVO_SUBVOLUME_PARALLEL`` for hosts with more headroom.
        int nSlabs = 4;
        if (const QByteArray ev = qgetenv("VISIVO_SUBVOLUME_PARALLEL"); !ev.isEmpty()) {
            const int n = ev.toInt();
            if (n >= 1 && n <= 16) nSlabs = n;
        }
        const int zSize = z1 - z0 + 1;
        if (zSize < nSlabs) {
            // Tiny Z range — fall through to single fetch by setting nSlabs=1.
            nSlabs = qMax(1, zSize);
        }

        // Build slab ranges along Z. Distribute remainder so all slabs
        // differ by at most one channel.
        QList<QPair<int, int>> slabRanges;
        slabRanges.reserve(nSlabs);
        const int baseSize = zSize / nSlabs;
        const int remainder = zSize % nSlabs;
        int cur = z0;
        for (int i = 0; i < nSlabs; ++i) {
            const int sz = baseSize + (i < remainder ? 1 : 0);
            slabRanges.append({ cur, cur + sz - 1 });
            cur += sz;
        }

        // Worker lambda: fetch a single slab through its own thread-local
        // QNAM. Captures by value the small POD-ish state it needs so the
        // lambda is safely runnable from a worker thread.
        auto fetchSlab = [baseUrl, token, sessionId, timeoutMs, datasetId,
                          x0, x1, y0, y1](int sz0, int sz1) -> SlabResult {
            SlabResult r;
            const QJsonDocument body(QJsonObject{
                    { QStringLiteral("dataset_id"), datasetId },
                    { QStringLiteral("x0"), x0 }, { QStringLiteral("x1"), x1 },
                    { QStringLiteral("y0"), y0 }, { QStringLiteral("y1"), y1 },
                    { QStringLiteral("z0"), sz0 }, { QStringLiteral("z1"), sz1 } });
            const QByteArray bytes = body.toJson(QJsonDocument::Compact);

            // Inline buildRequest() — we can't call the member from a
            // worker thread without capturing ``this``, and capturing
            // ``this`` would tie the worker's lifetime to the
            // BackendClient instance for no benefit (the synchronous
            // QFuture::waitForFinished() guarantees the instance
            // outlives every dispatched thread).
            QNetworkAccessManager *nam = getThreadLocalNam();
            const QUrl url(baseUrl + QStringLiteral("/v1/cube/subvolume_bin"));
            QNetworkRequest req(url);
            req.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
            if (!token.isEmpty()) {
                req.setRawHeader(QByteArrayLiteral("X-Visivo-Token"), token.toUtf8());
            }
            if (!sessionId.isEmpty()) {
                req.setRawHeader(QByteArrayLiteral("X-Visivo-Session"), sessionId.toUtf8());
            }
            QMap<QByteArray, QByteArray> headers;
            QString err;
            const QByteArray slabBody =
                    performBinaryPost(nam, req, bytes, timeoutMs, r.status, headers, err);
            if (r.status != 200) {
                r.error = err.isEmpty()
                                ? QStringLiteral("HTTP %1 on subvolume_bin").arg(r.status)
                                : err;
                return r;
            }
            const auto hget = [&](const char *name) -> QByteArray {
                return headers.value(QByteArray(name).toLower());
            };
            r.width  = hget("X-Visivo-Width").toInt();
            r.height = hget("X-Visivo-Height").toInt();
            r.depth  = hget("X-Visivo-Depth").toInt();
            r.scalarType        = QString::fromUtf8(hget("X-Visivo-Scalar-Type"));
            r.rangeMin          = hget("X-Visivo-Range-Min").toDouble();
            r.rangeMax          = hget("X-Visivo-Range-Max").toDouble();
            r.spectralAxisType  = QString::fromUtf8(hget("X-Visivo-Spectral-Axis-Type"));
            r.spectralAxisUnit  = QString::fromUtf8(hget("X-Visivo-Spectral-Axis-Unit"));
            r.bunit             = QString::fromUtf8(hget("X-Visivo-Bunit"));
            const qint64 expected =
                    qint64(r.width) * qint64(r.height) * qint64(r.depth)
                    * qint64(sizeof(float));
            if (r.width <= 0 || r.height <= 0 || r.depth <= 0
                || expected != slabBody.size()) {
                r.error = QStringLiteral(
                                  "subvolume_bin slab size mismatch: got %1, expected %2 "
                                  "(w=%3 h=%4 d=%5)")
                                  .arg(slabBody.size())
                                  .arg(expected)
                                  .arg(r.width).arg(r.height).arg(r.depth);
                return r;
            }
            r.data = slabBody;
            r.ok = true;
            return r;
        };

        // Fire all slabs concurrently.
        QElapsedTimer subvolTimer;
        subvolTimer.start();
        QList<QFuture<SlabResult>> futures;
        futures.reserve(slabRanges.size());
        for (const auto &range : slabRanges) {
            futures.append(QtConcurrent::run(fetchSlab, range.first, range.second));
        }

        // Collect, in submission order so the Z concatenation is stable.
        QList<SlabResult> slabs;
        slabs.reserve(futures.size());
        bool anyFailed = false;
        QString firstError;
        for (auto &f : futures) {
            f.waitForFinished();
            const SlabResult sr = f.result();
            if (!sr.ok && !anyFailed) {
                firstError = sr.error;
            }
            if (!sr.ok) {
                anyFailed = true;
            }
            slabs.append(sr);
        }

        const qint64 fetchElapsedMs = subvolTimer.elapsed();
        if (!anyFailed && !slabs.isEmpty()) {
            // Stitch the float32 buffers in Z order. Width/height match
            // across slabs by construction.
            qint64 totalBytes = 0;
            int totalZ = 0;
            for (const auto &s : slabs) {
                totalBytes += s.data.size();
                totalZ += s.depth;
            }
            QElapsedTimer stitchTimer;
            stitchTimer.start();
            QByteArray stitched;
            stitched.reserve(int(totalBytes));
            for (const auto &s : slabs) {
                stitched.append(s.data);
            }
            const qint64 stitchElapsedMs = stitchTimer.elapsed();
            qDebug().noquote() << QStringLiteral(
                "[subvolume-client-timing] N=%1 totalMB=%2 "
                "fetch_all=%3ms stitch=%4ms")
                    .arg(slabs.size())
                    .arg(double(totalBytes) / 1e6, 0, 'f', 1)
                    .arg(fetchElapsedMs)
                    .arg(stitchElapsedMs);
            double globalMin = std::numeric_limits<double>::infinity();
            double globalMax = -std::numeric_limits<double>::infinity();
            for (const auto &s : slabs) {
                if (std::isfinite(s.rangeMin) && s.rangeMin < globalMin)
                    globalMin = s.rangeMin;
                if (std::isfinite(s.rangeMax) && s.rangeMax > globalMax)
                    globalMax = s.rangeMax;
            }
            if (!std::isfinite(globalMin)) globalMin = 0.0;
            if (!std::isfinite(globalMax)) globalMax = 0.0;
            result.valid             = true;
            result.error             = QString();
            result.width             = slabs[0].width;
            result.height            = slabs[0].height;
            result.depth             = totalZ;
            result.scalarType        = slabs[0].scalarType;
            result.rangeMin          = globalMin;
            result.rangeMax          = globalMax;
            result.spectralAxisType  = slabs[0].spectralAxisType;
            result.spectralAxisUnit  = slabs[0].spectralAxisUnit;
            result.bunit             = slabs[0].bunit;
            result.data              = stitched;
            return result;
        }

        if (!firstError.isEmpty()) {
            qWarning().noquote()
                    << QStringLiteral("subvolume_bin parallel fetch failed: %1. "
                                      "Falling back to JSON endpoint.")
                               .arg(firstError);
        }
        // Reset partial state before fallback.
        result = BackendCubeSubvolumeResult{};
    }

    // ── 2) JSON fallback (legacy path) ─────────────────────────────────
    QString error;
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/subvolume")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid             = object.value(QStringLiteral("valid")).toBool(false);
    result.error             = object.value(QStringLiteral("error")).toString();
    result.width             = object.value(QStringLiteral("width")).toInt();
    result.height            = object.value(QStringLiteral("height")).toInt();
    result.depth             = object.value(QStringLiteral("depth")).toInt();
    result.scalarType        = object.value(QStringLiteral("scalar_type")).toString();
    result.rangeMin          = object.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax          = object.value(QStringLiteral("range_max")).toDouble();
    result.spectralAxisType  = object.value(QStringLiteral("spectral_axis_type")).toString();
    result.spectralAxisUnit  = object.value(QStringLiteral("spectral_axis_unit")).toString();
    result.bunit             = object.value(QStringLiteral("bunit")).toString();
    result.data = decodePayload(object, QStringLiteral("data_base64"),
                                QStringLiteral("compression"), result.error);
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

// ── Velocity (cosmic-flow) field — F1 ─────────────────────────────────────────

BackendVelocityOpenResult BackendClient::openVelocityField(const QString &path, double boxMpc)
{
    BackendVelocityOpenResult result;
    QJsonObject bodyObj{ { QStringLiteral("path"), path } };
    if (boxMpc > 0.0) {
        bodyObj.insert(QStringLiteral("box_mpc"), boxMpc);
    }
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/velocity/open")), QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid     = obj.value(QStringLiteral("valid")).toBool();
    result.error     = obj.value(QStringLiteral("error")).toString();
    result.datasetId = obj.value(QStringLiteral("dataset_id")).toString();
    result.sessionId = obj.value(QStringLiteral("session_id")).toString();
    const QJsonObject s = obj.value(QStringLiteral("summary")).toObject();
    const QJsonArray shape = s.value(QStringLiteral("shape")).toArray();  // [nz, ny, nx]
    if (shape.size() == 3) {
        result.nz = shape.at(0).toInt();
        result.ny = shape.at(1).toInt();
        result.nx = shape.at(2).toInt();
    }
    result.speedMin = s.value(QStringLiteral("speed_min")).toDouble();
    result.speedMax = s.value(QStringLiteral("speed_max")).toDouble();
    result.unit     = s.value(QStringLiteral("unit")).toString();
    result.frame    = s.value(QStringLiteral("frame")).toString();
    result.boxKnown = s.value(QStringLiteral("box_known")).toBool();
    // Persist the session so the follow-up grid fetch hits the same backend
    // session that just registered the dataset.
    if (result.valid && !result.sessionId.isEmpty()) {
        m_sessionId = result.sessionId;
    }
    return result;
}

BackendVelocityGridResult BackendClient::fetchVelocityGrid(const QString &datasetId,
                                                           int maxDim) const
{
    BackendVelocityGridResult r;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("max_dim"), maxDim } });
    QNetworkRequest req = buildRequest(QUrl(m_baseUrl + QStringLiteral("/v1/velocity/grid_bin")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray data = performBinaryPost(
            nam, req, body.toJson(QJsonDocument::Compact), 120000, status, headers, err);
    if (status != 200) {
        r.error = err.isEmpty() ? QStringLiteral("HTTP %1 on velocity/grid_bin").arg(status) : err;
        return r;
    }
    const auto hget = [&](const char *name) -> QByteArray {
        return headers.value(QByteArray(name).toLower());
    };
    const auto parse3 = [](const QByteArray &b, double &a, double &c, double &d) {
        const QStringList p = QString::fromUtf8(b).split(QLatin1Char(','));
        if (p.size() == 3) { a = p[0].toDouble(); c = p[1].toDouble(); d = p[2].toDouble(); }
    };
    r.nx = hget("X-Visivo-Width").toInt();
    r.ny = hget("X-Visivo-Height").toInt();
    r.nz = hget("X-Visivo-Depth").toInt();
    parse3(hget("X-Visivo-Spacing"), r.sx, r.sy, r.sz);
    parse3(hget("X-Visivo-Origin"), r.ox, r.oy, r.oz);
    r.speedMin = hget("X-Visivo-Speed-Min").toDouble();
    r.speedMax = hget("X-Visivo-Speed-Max").toDouble();
    {
        const QByteArray fmin = hget("X-Visivo-Full-Speed-Min");
        const QByteArray fmax = hget("X-Visivo-Full-Speed-Max");
        r.fullSpeedMin = fmin.isEmpty() ? r.speedMin : fmin.toDouble();
        r.fullSpeedMax = fmax.isEmpty() ? r.speedMax : fmax.toDouble();
    }
    r.unit  = QString::fromUtf8(hget("X-Visivo-Unit"));
    r.frame = QString::fromUtf8(hget("X-Visivo-Frame"));
    r.downsampleFactor = std::max(1, hget("X-Visivo-Downsample-Factor").toInt());
    {
        const QStringList fs = QString::fromUtf8(hget("X-Visivo-Full-Shape")).split(QLatin1Char(','));
        if (fs.size() == 3) { r.fullNz = fs[0].toInt(); r.fullNy = fs[1].toInt(); r.fullNx = fs[2].toInt(); }
    }
    const qint64 expected =
            qint64(r.nx) * qint64(r.ny) * qint64(r.nz) * 3 * qint64(sizeof(float));
    if (r.nx <= 0 || r.ny <= 0 || r.nz <= 0 || expected != data.size()) {
        r.error = QStringLiteral("bad velocity grid payload: got %1 bytes, expected %2")
                          .arg(data.size()).arg(expected);
        return r;
    }
    r.vectors = data;
    r.valid = true;
    return r;
}

BackendVelocityGridResult BackendClient::fetchVelocitySubgrid(const QString &datasetId,
                                                              int x0, int x1, int y0, int y1,
                                                              int z0, int z1) const
{
    BackendVelocityGridResult r;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("x0"), x0 }, { QStringLiteral("x1"), x1 },
            { QStringLiteral("y0"), y0 }, { QStringLiteral("y1"), y1 },
            { QStringLiteral("z0"), z0 }, { QStringLiteral("z1"), z1 } });
    QNetworkRequest req = buildRequest(QUrl(m_baseUrl + QStringLiteral("/v1/velocity/subgrid_bin")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray data = performBinaryPost(
            nam, req, body.toJson(QJsonDocument::Compact), 120000, status, headers, err);
    if (status != 200) {
        r.error = err.isEmpty() ? QStringLiteral("HTTP %1 on velocity/subgrid_bin").arg(status) : err;
        return r;
    }
    const auto hget = [&](const char *name) -> QByteArray {
        return headers.value(QByteArray(name).toLower());
    };
    r.nx = hget("X-Visivo-Width").toInt();
    r.ny = hget("X-Visivo-Height").toInt();
    r.nz = hget("X-Visivo-Depth").toInt();
    r.roiX0 = hget("X-Visivo-Roi-X0").toInt();
    r.roiY0 = hget("X-Visivo-Roi-Y0").toInt();
    r.roiZ0 = hget("X-Visivo-Roi-Z0").toInt();
    r.speedMin = hget("X-Visivo-Speed-Min").toDouble();
    r.speedMax = hget("X-Visivo-Speed-Max").toDouble();
    r.downsampleFactor = 1;
    const qint64 expected =
            qint64(r.nx) * qint64(r.ny) * qint64(r.nz) * 3 * qint64(sizeof(float));
    if (r.nx <= 0 || r.ny <= 0 || r.nz <= 0 || expected != data.size()) {
        r.error = QStringLiteral("bad velocity subgrid payload: got %1 bytes, expected %2")
                          .arg(data.size()).arg(expected);
        return r;
    }
    r.vectors = data;
    r.valid = true;
    return r;
}

BackendVelocityBasinsResult BackendClient::fetchVelocityBasins(const QString &datasetId,
                                                               int basinMaxDim, int direction) const
{
    BackendVelocityBasinsResult r;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("basin_max_dim"), basinMaxDim },
            { QStringLiteral("direction"), direction } });
    QNetworkRequest req = buildRequest(QUrl(m_baseUrl + QStringLiteral("/v1/velocity/basins_bin")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray data = performBinaryPost(
            nam, req, body.toJson(QJsonDocument::Compact), 300000, status, headers, err);
    if (status != 200) {
        r.error = err.isEmpty() ? QStringLiteral("HTTP %1 on velocity/basins_bin").arg(status) : err;
        return r;
    }
    const auto hget = [&](const char *name) -> QByteArray {
        return headers.value(QByteArray(name).toLower());
    };
    r.nx = hget("X-Visivo-Width").toInt();
    r.ny = hget("X-Visivo-Height").toInt();
    r.nz = hget("X-Visivo-Depth").toInt();
    r.nBasins = hget("X-Visivo-N-Basins").toInt();
    const qint64 expected = qint64(r.nx) * qint64(r.ny) * qint64(r.nz) * qint64(sizeof(qint32));
    if (r.nx <= 0 || r.ny <= 0 || r.nz <= 0 || expected != data.size()) {
        r.error = QStringLiteral("bad basins payload: got %1 bytes, expected %2")
                          .arg(data.size()).arg(expected);
        return r;
    }
    r.labels = data;
    r.valid = true;
    return r;
}

BackendCubePvResult BackendClient::requestPv(const QString &datasetId,
                                             const std::vector<std::array<int, 2>> &vertices,
                                             int widthPixels) const
{
    QString error;
    QJsonArray vertexArray;
    for (const auto &v : vertices) {
        vertexArray.append(QJsonArray{ v[0], v[1] });
    }
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("vertices"), vertexArray },
                                          { QStringLiteral("width_pixels"), widthPixels } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/pv")), body, error);
    if (!error.isEmpty()) {
        BackendCubePvResult result;
        result.error = error;
        return result;
    }
    return parsePvResultObject(QJsonDocument::fromJson(payload).object());
}

BackendCubeNoiseResult BackendClient::requestNoise(const QString &datasetId, int x0, int x1,
                                                   int y0, int y1, int channelStart,
                                                   int channelEnd) const
{
    BackendCubeNoiseResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("x0"), x0 },
                                          { QStringLiteral("x1"), x1 },
                                          { QStringLiteral("y0"), y0 },
                                          { QStringLiteral("y1"), y1 },
                                          { QStringLiteral("channel_start"), channelStart },
                                          { QStringLiteral("channel_end"), channelEnd } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/noise")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }

    return parseNoiseResultObject(QJsonDocument::fromJson(payload).object());
}

BackendCatalogueSubset BackendClient::requestCatalogueSubset(const QString &datasetId,
                                                             int maxRows) const
{
    BackendCatalogueSubset result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("max_rows"), maxRows } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/catalogue/subset")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    const QJsonArray rows = object.value(QStringLiteral("rows")).toArray();
    for (const QJsonValue &rowValue : rows) {
        result.rows.append(rowValue.toObject().toVariantMap());
    }
    return result;
}

BackendVbtOpenResult BackendClient::openVbt(const QString &path, const QString &format)
{
    BackendVbtOpenResult result;
    QString error;
    QJsonObject bodyObj{ { QStringLiteral("path"), path } };
    if (!format.isEmpty()) {
        bodyObj.insert(QStringLiteral("format"), format);
    }
    const QJsonDocument body(bodyObj);
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/vbt/open")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.datasetId = object.value(QStringLiteral("dataset_id")).toString();
    result.sessionId = object.value(QStringLiteral("session_id")).toString();
    result.kind = object.value(QStringLiteral("kind")).toString();
    result.numRows = object.value(QStringLiteral("num_rows")).toInt();
    result.scalarType = object.value(QStringLiteral("scalar_type")).toString();
    if (!result.sessionId.isEmpty()) {
        this->setSessionId(result.sessionId);
    }
    const QJsonArray fields = object.value(QStringLiteral("fields")).toArray();
    result.fields.reserve(static_cast<std::size_t>(fields.size()));
    for (const QJsonValue &v : fields) {
        const QJsonObject f = v.toObject();
        result.fields.push_back({ f.value(QStringLiteral("name")).toString(),
                                  f.value(QStringLiteral("type")).toString() });
    }
    const QJsonArray dims = object.value(QStringLiteral("dimensions")).toArray();
    if (dims.size() == 3) {
        result.dimensions = { dims[0].toInt(), dims[1].toInt(), dims[2].toInt() };
    }
    const QJsonArray spacing = object.value(QStringLiteral("spacing")).toArray();
    if (spacing.size() == 3) {
        result.spacing = { spacing[0].toDouble(), spacing[1].toDouble(), spacing[2].toDouble() };
    }
    return result;
}

BackendVbtSubsetResult BackendClient::requestVbtSubset(const QString &datasetId, int maxRows,
                                                       const QString &fieldName) const
{
    BackendVbtSubsetResult result;
    QString error;
    QJsonObject bodyObj{ { QStringLiteral("dataset_id"), datasetId } };
    if (maxRows > 0) {
        bodyObj.insert(QStringLiteral("max_rows"), maxRows);
    }
    if (!fieldName.isEmpty()) {
        bodyObj.insert(QStringLiteral("field_name"), fieldName);
    }
    const QJsonDocument body(bodyObj);
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/vbt/subset")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.kind = object.value(QStringLiteral("kind")).toString();
    result.numRows = object.value(QStringLiteral("num_rows")).toInt();
    const QJsonArray fieldNamesArray = object.value(QStringLiteral("fields")).toArray();
    for (const QJsonValue &v : fieldNamesArray) {
        result.fieldNames.append(v.toString());
    }
    const QJsonArray dims = object.value(QStringLiteral("dimensions")).toArray();
    if (dims.size() == 3) {
        result.dimensions = { dims[0].toInt(), dims[1].toInt(), dims[2].toInt() };
    }
    const QJsonArray spacingArr = object.value(QStringLiteral("spacing")).toArray();
    if (spacingArr.size() == 3) {
        result.spacing = { spacingArr[0].toDouble(), spacingArr[1].toDouble(),
                           spacingArr[2].toDouble() };
    }
    if (result.kind == QStringLiteral("point")) {
        const QJsonObject columns = object.value(QStringLiteral("columns")).toObject();
        for (const QString &fname : result.fieldNames) {
            const QByteArray raw =
                    QByteArray::fromBase64(columns.value(fname).toString().toLatin1());
            const int n = raw.size() / static_cast<int>(sizeof(double));
            std::vector<double> col(static_cast<std::size_t>(n));
            memcpy(col.data(), raw.constData(), static_cast<std::size_t>(n) * sizeof(double));
            result.columns[fname] = std::move(col);
        }
    } else {
        result.fieldName = object.value(QStringLiteral("field_name")).toString();
        result.block =
                QByteArray::fromBase64(object.value(QStringLiteral("block")).toString().toLatin1());
    }
    return result;
}

static QJsonObject buildTabularQueryBody(const BackendTabularQueryRequest &req)
{
    QJsonObject body{
        { QStringLiteral("dataset_id"), req.datasetId },
        { QStringLiteral("limit"), req.limit },
        { QStringLiteral("offset"), req.offset },
    };
    QJsonArray filters;
    for (const auto &f : req.filters) {
        filters.append(QJsonObject{
            { QStringLiteral("field"), f.field },
            { QStringLiteral("op"), f.op },
            { QStringLiteral("value"), f.value },
        });
    }
    body.insert(QStringLiteral("filters"), filters);
    if (!req.sortField.isEmpty()) {
        body.insert(QStringLiteral("sort"),
                    QJsonObject{
                        { QStringLiteral("field"), req.sortField },
                        { QStringLiteral("direction"),
                          req.sortDirection.isEmpty() ? QStringLiteral("asc") : req.sortDirection },
                    });
    }
    return body;
}

BackendTabularQueryResponse BackendClient::queryTabularCatalogue(
        const BackendTabularQueryRequest &req) const
{
    BackendTabularQueryResponse result;
    QString error;
    const QJsonDocument body(buildTabularQueryBody(req));
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/catalogue/query")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.totalRows = object.value(QStringLiteral("total_rows")).toInt();
    result.returnedRows = object.value(QStringLiteral("returned_rows")).toInt();
    const QJsonArray rows = object.value(QStringLiteral("rows")).toArray();
    for (const QJsonValue &rowValue : rows) {
        result.rows.append(rowValue.toObject().toVariantMap());
    }
    return result;
}

BackendTabularQueryResponse BackendClient::queryTabularVbt(
        const BackendTabularQueryRequest &req) const
{
    BackendTabularQueryResponse result;
    QString error;
    const QJsonDocument body(buildTabularQueryBody(req));
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/vbt/query")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.totalRows = object.value(QStringLiteral("total_rows")).toInt();
    result.returnedRows = object.value(QStringLiteral("returned_rows")).toInt();
    result.numRows = object.value(QStringLiteral("num_rows")).toInt();
    const QJsonArray fieldNamesArray = object.value(QStringLiteral("fields")).toArray();
    for (const QJsonValue &v : fieldNamesArray) {
        result.fieldNames.append(v.toString());
    }
    const QJsonObject columns = object.value(QStringLiteral("columns")).toObject();
    for (const QString &fname : result.fieldNames) {
        const QByteArray raw =
                QByteArray::fromBase64(columns.value(fname).toString().toLatin1());
        const int n = raw.size() / static_cast<int>(sizeof(double));
        std::vector<double> col(static_cast<std::size_t>(n));
        memcpy(col.data(), raw.constData(), static_cast<std::size_t>(n) * sizeof(double));
        result.columns[fname] = std::move(col);
    }
    return result;
}

BackendImageResult BackendClient::requestImage(const QString &datasetId) const
{
    BackendImageResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/image/full")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.width = object.value(QStringLiteral("width")).toInt();
    result.height = object.value(QStringLiteral("height")).toInt();
    result.fullWidth = object.value(QStringLiteral("full_width")).toInt(result.width);
    result.fullHeight = object.value(QStringLiteral("full_height")).toInt(result.height);
    result.scalarType = object.value(QStringLiteral("scalar_type")).toString();
    result.rangeMin = object.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax = object.value(QStringLiteral("range_max")).toDouble();
    result.isPreview = object.value(QStringLiteral("is_preview")).toBool(false);
    result.previewScaleFactor =
            object.value(QStringLiteral("preview_scale_factor")).toDouble(1.0);
    result.data = decodePayload(object, QStringLiteral("data_base64"),
                                QStringLiteral("compression"), result.error);
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

BackendImageResult BackendClient::requestImagePreview(const QString &datasetId,
                                                      int maxLongestSide) const
{
    BackendImageResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("max_longest_side"), maxLongestSide } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/image/preview")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.width = object.value(QStringLiteral("width")).toInt();
    result.height = object.value(QStringLiteral("height")).toInt();
    result.fullWidth = object.value(QStringLiteral("full_width")).toInt(result.width);
    result.fullHeight = object.value(QStringLiteral("full_height")).toInt(result.height);
    result.scalarType = object.value(QStringLiteral("scalar_type")).toString();
    result.rangeMin = object.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax = object.value(QStringLiteral("range_max")).toDouble();
    result.isPreview = object.value(QStringLiteral("is_preview")).toBool(false);
    result.previewScaleFactor =
            object.value(QStringLiteral("preview_scale_factor")).toDouble(1.0);
    result.data = decodePayload(object, QStringLiteral("data_base64"),
                                QStringLiteral("compression"), result.error);
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

BackendIsosurfaceResult BackendClient::requestIsosurface(const QString &datasetId,
                                                         double threshold) const
{
    BackendIsosurfaceResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("threshold"), threshold } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/products/isosurface")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.numPoints = object.value(QStringLiteral("num_points")).toInt();
    result.numPolys = object.value(QStringLiteral("num_polys")).toInt();
    result.pointsData = decodePayload(object, QStringLiteral("points_base64"),
                                      QStringLiteral("compression"), result.error);
    if (result.error.isEmpty()) {
        result.polysData = decodePayload(object, QStringLiteral("polys_base64"),
                                         QStringLiteral("compression"), result.error);
    }
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

BackendMomentResult BackendClient::requestMoment(const QString &datasetId, int order,
                                                 int channelStart, int channelEnd,
                                                 bool maskEnabled, double thresholdValue) const
{
    QString error;
    const QJsonDocument body(
            QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                         { QStringLiteral("moment_order"), order },
                         { QStringLiteral("channel_start"), channelStart },
                         { QStringLiteral("channel_end"), channelEnd },
                         { QStringLiteral("mask_enabled"), maskEnabled },
                         { QStringLiteral("threshold_value"), thresholdValue } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/products/moment")), body, error);
    if (!error.isEmpty()) {
        BackendMomentResult result;
        result.error = error;
        return result;
    }
    return parseMomentResultObject(QJsonDocument::fromJson(payload).object());
}

BinaryArrayResult BackendClient::fetchMomentBinary(const QString &datasetId,
                                                    int order,
                                                    int channelStart, int channelEnd,
                                                    bool maskEnabled, float threshold,
                                                    const QString &sessionId)
{
    BinaryArrayResult result;
    const QString prevSession = m_sessionId;
    if (!sessionId.isEmpty())
        m_sessionId = sessionId;

    QString error;
    const QJsonDocument body(
            QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                         { QStringLiteral("moment_order"), order },
                         { QStringLiteral("channel_start"), channelStart },
                         { QStringLiteral("channel_end"), channelEnd },
                         { QStringLiteral("mask_enabled"), maskEnabled },
                         { QStringLiteral("threshold_value"), static_cast<double>(threshold) } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/moment/result/binary")), body, error);
    m_sessionId = prevSession;

    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    if (payload.size() < 12) {
        result.error = QStringLiteral("Binary response too short (%1 bytes).").arg(payload.size());
        return result;
    }
    QDataStream ds(payload);
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 rows = 0, cols = 0, flags = 0;
    ds >> rows >> cols >> flags;
    result.rows = static_cast<int>(rows);
    result.cols = static_cast<int>(cols);
    result.data = payload.mid(12);  // raw float32 body
    return result;
}

BinaryArrayResult BackendClient::fetchPvBinary(const QString &datasetId,
                                                const QVector<QPointF> &path,
                                                int slitWidth,
                                                const QString &sessionId)
{
    BinaryArrayResult result;
    const QString prevSession = m_sessionId;
    if (!sessionId.isEmpty())
        m_sessionId = sessionId;

    QString error;
    QJsonArray vertexArray;
    for (const QPointF &pt : path)
        vertexArray.append(QJsonArray{ static_cast<int>(qRound(pt.x())),
                                       static_cast<int>(qRound(pt.y())) });
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("vertices"), vertexArray },
                                          { QStringLiteral("width_pixels"), slitWidth } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/pv/result/binary")), body, error);
    m_sessionId = prevSession;

    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    if (payload.size() < 12) {
        result.error = QStringLiteral("Binary response too short (%1 bytes).").arg(payload.size());
        return result;
    }
    QDataStream ds(payload);
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 rows = 0, cols = 0, flags = 0;
    ds >> rows >> cols >> flags;
    result.rows = static_cast<int>(rows);
    result.cols = static_cast<int>(cols);
    result.data = payload.mid(12);
    return result;
}

// ── Binary frame parser ───────────────────────────────────────────────────────

bool BackendClient::parseBinaryFrame(const QByteArray &payload, int &offset,
                                      BinaryArrayResult &result)
{
    if (payload.size() - offset < 12) {
        result.error = QStringLiteral("Binary frame header truncated at offset %1.").arg(offset);
        return false;
    }
    QDataStream ds(payload.mid(offset, 12));
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 rows = 0, cols = 0, flags = 0;
    ds >> rows >> cols >> flags;
    const int bodySize = static_cast<int>(rows) * static_cast<int>(cols) * 4;
    if (payload.size() - offset - 12 < bodySize) {
        result.error = QStringLiteral("Binary frame body truncated (need %1 bytes).").arg(bodySize);
        return false;
    }
    result.rows = static_cast<int>(rows);
    result.cols = static_cast<int>(cols);
    result.data = payload.mid(offset + 12, bodySize);
    offset += 12 + bodySize;
    return true;
}

// ── S-02: Line-width maps ─────────────────────────────────────────────────────


// ── S-03: Baseline subtraction ────────────────────────────────────────────────


// ── Sub-cube extraction (crop ROI → Workspace Exports) ─────────────────────


// ── Cube channel → 2-D FITS export ─────────────────────────────────────────


// ── Dynamic-spectrum analysis ──────────────────────────────────────────────




// ── Moment map → Workspace Exports ─────────────────────────────────────────


// ── Workspace Exports management ───────────────────────────────────────────





// ── S-04: Spectral stacking ───────────────────────────────────────────────────


// ── S-05: SoFiA-2 source finding ──────────────────────────────────────────────









// ── Low-level HTTP ────────────────────────────────────────────────────────────

std::chrono::milliseconds BackendClient::requestTimeoutFor(const QUrl &url) const
{
    const QString path = url.path();
    if (path.endsWith(QStringLiteral("/health"))
        || path.endsWith(QStringLiteral("/files/list"))
        || path.endsWith(QStringLiteral("/files/header"))) {
        return std::chrono::milliseconds(5000);
    }
    if (path.endsWith(QStringLiteral("/datasets/open"))) {
        return std::chrono::milliseconds(10000);
    }
    if (path.endsWith(QStringLiteral("/tasks/moment")) || path.contains(QStringLiteral("/tasks/"))) {
        return std::chrono::milliseconds(10000);
    }
    if (path.endsWith(QStringLiteral("/cube/slice")) || path.endsWith(QStringLiteral("/image/preview"))) {
        return std::chrono::milliseconds(30000);
    }
    if (path.endsWith(QStringLiteral("/vlkb/fetch_cutout"))) {
        return std::chrono::milliseconds(300000); // FITS downloads can be large
    }
    // Cube preview + subvolume can transfer multi-GB float32 payloads on
    // SKA-scale datasets (10+ GB cubes, ROI crops up to a few GB). Even
    // with backend-side zlib compression a 1.5 GB subvolume through an
    // SSH tunnel can take 60-90 s steady-state. Bump these endpoints to
    // 10 min so the operator doesn't see a spurious timeout while the
    // tunnel is still streaming. Real performance work happens server-
    // side (chunked dask reads, binary streaming) — this timeout is a
    // safety net, not the optimisation.
    if (path.endsWith(QStringLiteral("/cube/preview"))
        || path.endsWith(QStringLiteral("/cube/subvolume"))
        || path.endsWith(QStringLiteral("/cube/subvolume_bin"))) {
        return std::chrono::milliseconds(600000); // 10 min
    }
    return std::chrono::milliseconds(120000);
}

BackendMomentResult BackendClient::parseMomentResultObject(const QJsonObject &object)
{
    BackendMomentResult result;
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.width = object.value(QStringLiteral("width")).toInt();
    result.height = object.value(QStringLiteral("height")).toInt();
    result.scalarType = object.value(QStringLiteral("scalar_type")).toString();
    result.rangeMin = object.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax = object.value(QStringLiteral("range_max")).toDouble();
    result.spectralAxisType = object.value(QStringLiteral("spectral_axis_type")).toString();
    result.spectralAxisUnit = object.value(QStringLiteral("spectral_axis_unit")).toString();
    result.momentUnit = object.value(QStringLiteral("moment_unit")).toString();
    result.bunit = object.value(QStringLiteral("bunit")).toString();
    result.wcsStatus = object.value(QStringLiteral("wcs_status")).toString(QStringLiteral("ok"));
    result.wcsWarningMessage = object.value(QStringLiteral("wcs_warning_message")).toString();
    result.data = decodePayload(object, QStringLiteral("data_base64"),
                                QStringLiteral("compression"), result.error);
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

BackendCubePvResult BackendClient::parsePvResultObject(const QJsonObject &object)
{
    BackendCubePvResult result;
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.numSamples = object.value(QStringLiteral("num_samples")).toInt();
    result.depth = object.value(QStringLiteral("depth")).toInt();
    result.scalarType = object.value(QStringLiteral("scalar_type")).toString();
    result.computedOn = object.value(QStringLiteral("computed_on")).toString();
    result.widthPixels = object.value(QStringLiteral("width_pixels")).toInt(1);
    result.vertexCount = object.value(QStringLiteral("vertex_count")).toInt();
    result.totalLength = object.value(QStringLiteral("total_length")).toDouble();
    result.validSamples = object.value(QStringLiteral("valid_samples")).toInt();
    result.spectralAxisType = object.value(QStringLiteral("spectral_axis_type")).toString();
    result.spectralAxisUnit = object.value(QStringLiteral("spectral_axis_unit")).toString();
    result.bunit = object.value(QStringLiteral("bunit")).toString();
    result.beamMajor = object.value(QStringLiteral("beam_major")).toDouble();
    result.beamMinor = object.value(QStringLiteral("beam_minor")).toDouble();
    result.beamPa = object.value(QStringLiteral("beam_pa")).toDouble();
    result.positionsArcsec = decodePayload(object, QStringLiteral("positions_arcsec_base64"),
                                           QStringLiteral("compression"), result.error);
    result.error.clear(); // missing arcsec positions is non-fatal
    result.pixelScaleArcsecPerPixel =
            object.value(QStringLiteral("pixel_scale_arcsec_per_pixel")).toDouble(0.);
    result.spatialUnit = object.value(QStringLiteral("spatial_unit")).toString(QStringLiteral("pixel"));
    result.positions = decodePayload(object, QStringLiteral("positions_base64"),
                                     QStringLiteral("compression"), result.error);
    if (result.error.isEmpty()) {
        result.data = decodePayload(object, QStringLiteral("data_base64"),
                                    QStringLiteral("compression"), result.error);
    }
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

BackendCubeNoiseResult BackendClient::parseNoiseResultObject(const QJsonObject &object)
{
    BackendCubeNoiseResult result;
    result.valid        = object.value(QStringLiteral("valid")).toBool(false);
    result.error        = object.value(QStringLiteral("error")).toString();
    result.channelStart = object.value(QStringLiteral("channel_start")).toInt();
    result.channelEnd   = object.value(QStringLiteral("channel_end")).toInt();
    result.numChannels  = object.value(QStringLiteral("num_channels")).toInt();

    const QJsonArray madArray = object.value(QStringLiteral("mad")).toArray();
    result.mad.reserve(madArray.size());
    for (const QJsonValue &v : madArray)
        result.mad.push_back(v.toDouble(std::numeric_limits<double>::quiet_NaN()));

    const QJsonArray sigmaArray = object.value(QStringLiteral("sigma")).toArray();
    result.sigma.reserve(sigmaArray.size());
    for (const QJsonValue &v : sigmaArray)
        result.sigma.push_back(v.toDouble(std::numeric_limits<double>::quiet_NaN()));

    const QJsonObject region = object.value(QStringLiteral("region")).toObject();
    result.x0 = region.value(QStringLiteral("x0")).toInt();
    result.x1 = region.value(QStringLiteral("x1")).toInt();
    result.y0 = region.value(QStringLiteral("y0")).toInt();
    result.y1 = region.value(QStringLiteral("y1")).toInt();
    result.blankFraction = object.value(QStringLiteral("blank_fraction")).toDouble(0.0);
    return result;
}

// ── Workspace Exports: JSON-only parsers (unit-testable without HTTP) ──────

BackendSaveSubregionResult BackendClient::parseSaveSubregionObject(const QJsonObject &obj)
{
    BackendSaveSubregionResult result;
    result.valid             = obj.value(QStringLiteral("valid")).toBool(false);
    result.error             = obj.value(QStringLiteral("error")).toString();
    result.newDatasetId      = obj.value(QStringLiteral("new_dataset_id")).toString();
    result.outputPath        = obj.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = obj.value(QStringLiteral("workspace_filename")).toString();
    result.width             = obj.value(QStringLiteral("width")).toInt();
    result.height            = obj.value(QStringLiteral("height")).toInt();
    result.depth             = obj.value(QStringLiteral("depth")).toInt();
    result.rangeMin          = obj.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax          = obj.value(QStringLiteral("range_max")).toDouble();
    return result;
}

BackendSaveChannel2dResult BackendClient::parseSaveChannel2dObject(const QJsonObject &obj)
{
    BackendSaveChannel2dResult result;
    result.valid             = obj.value(QStringLiteral("valid")).toBool(false);
    result.error             = obj.value(QStringLiteral("error")).toString();
    result.newDatasetId      = obj.value(QStringLiteral("new_dataset_id")).toString();
    result.outputPath        = obj.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = obj.value(QStringLiteral("workspace_filename")).toString();
    result.width             = obj.value(QStringLiteral("width")).toInt();
    result.height            = obj.value(QStringLiteral("height")).toInt();
    result.channel           = obj.value(QStringLiteral("channel")).toInt();
    result.rangeMin          = obj.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax          = obj.value(QStringLiteral("range_max")).toDouble();
    return result;
}

BackendSaveMomentFitsResult BackendClient::parseSaveMomentFitsObject(const QJsonObject &obj)
{
    BackendSaveMomentFitsResult result;
    result.valid             = obj.value(QStringLiteral("valid")).toBool(false);
    result.error             = obj.value(QStringLiteral("error")).toString();
    result.outputPath        = obj.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = obj.value(QStringLiteral("workspace_filename")).toString();
    result.width             = obj.value(QStringLiteral("width")).toInt();
    result.height            = obj.value(QStringLiteral("height")).toInt();
    result.rangeMin          = obj.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax          = obj.value(QStringLiteral("range_max")).toDouble();
    result.bunit             = obj.value(QStringLiteral("bunit")).toString();
    result.momentUnit        = obj.value(QStringLiteral("moment_unit")).toString();
    return result;
}

BackendWorkspaceExportsList BackendClient::parseExportsListObject(const QJsonObject &obj)
{
    BackendWorkspaceExportsList result;
    result.valid        = obj.value(QStringLiteral("valid")).toBool(false);
    result.error        = obj.value(QStringLiteral("error")).toString();
    result.workspaceDir = obj.value(QStringLiteral("workspace_dir")).toString();
    const QJsonArray arr = obj.value(QStringLiteral("entries")).toArray();
    result.entries.reserve(arr.size());
    for (const auto &v : arr) {
        const QJsonObject e = v.toObject();
        BackendWorkspaceExportEntry entry;
        entry.filename     = e.value(QStringLiteral("filename")).toString();
        entry.size         = e.value(QStringLiteral("size")).toVariant().toLongLong();
        entry.modifiedTime = e.value(QStringLiteral("modified_time")).toString();
        entry.absolutePath = e.value(QStringLiteral("absolute_path")).toString();
        result.entries.append(entry);
    }
    return result;
}

BackendWorkspaceDeleteResult BackendClient::parseExportsDeleteObject(const QJsonObject &obj)
{
    BackendWorkspaceDeleteResult result;
    result.valid           = obj.value(QStringLiteral("valid")).toBool(false);
    result.error           = obj.value(QStringLiteral("error")).toString();
    result.deletedFilename = obj.value(QStringLiteral("deleted_filename")).toString();
    return result;
}

// Suppress publish-on-success for paths we don't want to flood the diagnostics
// table with: the diagnostics poll itself (would loop), and the DataHub
// health heartbeat (every few seconds). Failures are always logged.
static bool _isDiagnosticsQuietPath(const QString &path)
{
    return path.startsWith(QStringLiteral("/v1/diagnostics/"))
        || path == QStringLiteral("/v1/health");
}

QByteArray BackendClient::performGet(const QUrl &url, QString &error) const
{
    QElapsedTimer perfTimer;
    perfTimer.start();
    QNetworkAccessManager *nam = getThreadLocalNam();
    const QString fallbackToken = BackendClient::readTokenFile().trimmed();
    const QStringList tokenCandidates = [&]() {
        QStringList tokens;
        tokens << this->m_token.trimmed();
        if (!fallbackToken.isEmpty() && fallbackToken != this->m_token.trimmed()) {
            tokens << fallbackToken;
        }
        return tokens;
    }();

    QByteArray finalData;
    QString lastError;
    int lastStatus = 0;

    for (int attempt = 0; attempt < tokenCandidates.size(); ++attempt) {
        QString attemptError;
        const QString token = tokenCandidates.at(attempt);
        QNetworkRequest request = buildRequest(url);
        if (!token.isEmpty()) {
            request.setRawHeader(QByteArrayLiteral("X-Visivo-Token"), token.toUtf8());
        }

        QEventLoop loop;
        QNetworkReply *reply = nam->get(request);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
            attemptError = QStringLiteral("Backend request timed out after %1 ms: %2")
                                   .arg(this->requestTimeoutFor(url).count())
                                   .arg(url.path());
            qWarning().noquote() << attemptError;
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                    QStringLiteral("BackendClient"), attemptError, {}, this->m_sessionId);
            reply->abort();
            loop.quit();
        });
        timeoutTimer.start(static_cast<int>(this->requestTimeoutFor(url).count()));
        loop.exec();
        timeoutTimer.stop();

        const QByteArray data = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (attemptError.isEmpty() && reply->error() != QNetworkReply::NoError) {
            attemptError = reply->errorString();
        } else if (attemptError.isEmpty() && status >= 400) {
            attemptError = QString::fromUtf8(data);
        }
        reply->deleteLater();

        if (status == 401 && attempt + 1 < tokenCandidates.size()) {
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Backend,
                    QStringLiteral("BackendClient"),
                    QStringLiteral("Backend GET for %1 returned 401. Retrying with refreshed token.")
                            .arg(url.path()),
                    {}, this->m_sessionId);
            continue;
        }

        if (attemptError.isEmpty()) {
            if (token != this->m_token) {
                const_cast<BackendClient *>(this)->m_token = token;
            }
            if (!_isDiagnosticsQuietPath(url.path())) {
                DiagnosticsManager::instance()->publish(
                        DiagnosticsLevel::Info, DiagnosticsCategory::Backend,
                        QStringLiteral("BackendClient"),
                        QStringLiteral("GET %1 → %2 (%3 ms, %4 bytes)")
                                .arg(url.path())
                                .arg(status > 0 ? status : 200)
                                .arg(perfTimer.elapsed())
                                .arg(data.size()),
                        {}, this->m_sessionId);
            }
            return data;
        }

        finalData = data;
        lastError = attemptError;
        lastStatus = status;
        break;
    }

    error = lastError;
    if (lastStatus >= 400) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"),
                QStringLiteral("Backend GET returned HTTP %1 for %2: %3")
                        .arg(lastStatus)
                        .arg(url.path(), error),
                {}, this->m_sessionId);
    } else if (!error.isEmpty()) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"),
                QStringLiteral("Backend GET failed for %1: %2").arg(url.path(), error), {},
                this->m_sessionId);
    }
    return finalData;
}

QByteArray BackendClient::performPost(const QUrl &url, const QJsonDocument &body,
                                      QString &error) const
{
    QElapsedTimer perfTimer;
    perfTimer.start();
    QNetworkAccessManager *nam = getThreadLocalNam();
    const QByteArray bodyBytes = body.toJson(QJsonDocument::Compact);
    const QString fallbackToken = BackendClient::readTokenFile().trimmed();
    const QStringList tokenCandidates = [&]() {
        QStringList tokens;
        tokens << this->m_token.trimmed();
        if (!fallbackToken.isEmpty() && fallbackToken != this->m_token.trimmed()) {
            tokens << fallbackToken;
        }
        return tokens;
    }();

    QByteArray finalData;
    QString lastError;
    int lastStatus = 0;

    for (int attempt = 0; attempt < tokenCandidates.size(); ++attempt) {
        QString attemptError;
        const QString token = tokenCandidates.at(attempt);
        QNetworkRequest request = buildRequest(url);
        if (!token.isEmpty()) {
            request.setRawHeader(QByteArrayLiteral("X-Visivo-Token"), token.toUtf8());
        }

        QEventLoop loop;
        QNetworkReply *reply = nam->post(request, bodyBytes);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
            attemptError = QStringLiteral("Backend request timed out after %1 ms: %2")
                                   .arg(this->requestTimeoutFor(url).count())
                                   .arg(url.path());
            qWarning().noquote() << attemptError;
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                    QStringLiteral("BackendClient"), attemptError, {}, this->m_sessionId);
            reply->abort();
            loop.quit();
        });
        timeoutTimer.start(static_cast<int>(this->requestTimeoutFor(url).count()));
        loop.exec();
        timeoutTimer.stop();

        const QByteArray data = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (attemptError.isEmpty() && reply->error() != QNetworkReply::NoError) {
            attemptError = reply->errorString();
        } else if (attemptError.isEmpty() && status >= 400) {
            attemptError = QString::fromUtf8(data);
        }
        reply->deleteLater();

        if (status == 401 && attempt + 1 < tokenCandidates.size()) {
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Backend,
                    QStringLiteral("BackendClient"),
                    QStringLiteral("Backend POST for %1 returned 401. Retrying with refreshed token.")
                            .arg(url.path()),
                    {}, this->m_sessionId);
            continue;
        }

        if (attemptError.isEmpty()) {
            if (token != this->m_token) {
                const_cast<BackendClient *>(this)->m_token = token;
            }
            if (!_isDiagnosticsQuietPath(url.path())) {
                DiagnosticsManager::instance()->publish(
                        DiagnosticsLevel::Info, DiagnosticsCategory::Backend,
                        QStringLiteral("BackendClient"),
                        QStringLiteral("POST %1 → %2 (%3 ms, %4 bytes)")
                                .arg(url.path())
                                .arg(status > 0 ? status : 200)
                                .arg(perfTimer.elapsed())
                                .arg(data.size()),
                        {}, this->m_sessionId);
            }
            return data;
        }

        finalData = data;
        lastError = attemptError;
        lastStatus = status;
        break;
    }

    error = lastError;
    if (lastStatus >= 400) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"),
                QStringLiteral("Backend POST returned HTTP %1 for %2: %3")
                        .arg(lastStatus)
                        .arg(url.path(), error),
                {}, this->m_sessionId);
    } else if (!error.isEmpty()) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"),
                QStringLiteral("Backend POST failed for %1: %2").arg(url.path(), error), {},
                this->m_sessionId);
    }
    return finalData;
}

QByteArray BackendClient::decodePayload(const QJsonObject &object, const QString &base64Field,
                                        const QString &compressionField, QString &error)
{
    return decodeCompressedPayload(object.value(base64Field).toString().toUtf8(),
                                   object.value(compressionField).toString(), error);
}

// ── HiPS ──────────────────────────────────────────────────────────────────────





// ── Milestone 2 ───────────────────────────────────────────────────────────────




// ── VLKB ──────────────────────────────────────────────────────────────────────


// ── Region interchange (CRTF / DS9) ───────────────────────────────────────────






// ── Advanced 2-D products (UC 3.2 / 3.3 / 1.3 / 4.2) ───────────────────────────






// ── A-01: Aperture photometry ─────────────────────────────────────────────────


// ── A-02: 2-D Gaussian source fitting ────────────────────────────────────────


// ── A-03: WCS reprojection ────────────────────────────────────────────────────


// ── A-04: Catalogue cross-match ───────────────────────────────────────────────


// ── P-01: Stokes plane extraction ─────────────────────────────────────────────


// ── P-02: Polarised intensity and angle ───────────────────────────────────────


// ── P-03: Rotation Measure synthesis ──────────────────────────────────────────


// ── C-01: Angular power spectrum ───────────────────────────────────────────────


// ── C-03: Skeleton / void detection ────────────────────────────────────────────


// ── R-01: CARTA script export — RETIRED (feature removed; no backend route). ──

// ── R-02: PROV-JSON provenance ─────────────────────────────────────────────────


// ── R-03: Parameter snapshots ──────────────────────────────────────────────────



