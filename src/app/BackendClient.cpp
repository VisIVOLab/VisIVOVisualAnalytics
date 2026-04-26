#include "BackendClient.h"
#include "DiagnosticsManager.h"

#include <QByteArray>
#include <QDataStream>
#include <QDir>
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
    return result;
}

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

BackendCubeSubvolumeResult BackendClient::requestSubvolume(const QString &datasetId, int x0,
                                                           int x1, int y0, int y1, int z0,
                                                           int z1) const
{
    BackendCubeSubvolumeResult result;
    QString error;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("x0"), x0 },
                                          { QStringLiteral("x1"), x1 },
                                          { QStringLiteral("y0"), y0 },
                                          { QStringLiteral("y1"), y1 },
                                          { QStringLiteral("z0"), z0 },
                                          { QStringLiteral("z1"), z1 } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cube/subvolume")), body, error);
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
    result.data = decodePayload(object, QStringLiteral("data_base64"),
                                QStringLiteral("compression"), result.error);
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
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

BackendLinewidthResult BackendClient::fetchLinewidthBinary(const QString &datasetId,
                                                            int channelStart, int channelEnd,
                                                            bool maskEnabled, float threshold,
                                                            double restFreqHz)
{
    BackendLinewidthResult result;
    QString error;
    QJsonObject bodyObj{
            { QStringLiteral("dataset_id"),      datasetId },
            { QStringLiteral("channel_start"),   channelStart },
            { QStringLiteral("channel_end"),     channelEnd },
            { QStringLiteral("mask_enabled"),    maskEnabled },
            { QStringLiteral("threshold_value"), static_cast<double>(threshold) } };
    // Only include rest_freq_hz when the caller provides a meaningful value.
    // Velocity-axis cubes must omit it; frequency/wavelength axes supply it.
    if (restFreqHz > 0.0) {
        bodyObj.insert(QStringLiteral("rest_freq_hz"), restFreqHz);
    }
    const QJsonDocument body(bodyObj);
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/spectral/linewidth/binary")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    // Parse the two concatenated binary frames (FWHM then EW).
    int offset = 0;
    BinaryArrayResult fwhm, ew;
    if (!parseBinaryFrame(payload, offset, fwhm)) {
        result.error = fwhm.error;
        return result;
    }
    if (!parseBinaryFrame(payload, offset, ew)) {
        result.error = ew.error;
        return result;
    }
    result.valid  = true;
    result.width  = fwhm.cols;
    result.height = fwhm.rows;
    result.binaryPayload = payload;
    return result;
}

// ── S-03: Baseline subtraction ────────────────────────────────────────────────

BackendBaselineResult BackendClient::requestBaseline(const QString &datasetId,
                                                      int channelStart, int channelEnd,
                                                      int polyOrder,
                                                      const QString &method,
                                                      const QVector<QPair<int,int>> &lineFreeRanges)
{
    BackendBaselineResult result;
    QString error;
    // Encode line-free ranges as [[start,end],...] pairs
    QJsonArray lfcArray;
    for (const auto &range : lineFreeRanges) {
        QJsonArray pair;
        pair.append(range.first);
        pair.append(range.second);
        lfcArray.append(pair);
    }
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("session_id"),         m_sessionId },
            { QStringLiteral("dataset_id"),         datasetId },
            { QStringLiteral("channel_start"),      channelStart },
            { QStringLiteral("channel_end"),        channelEnd },
            { QStringLiteral("poly_order"),         polyOrder },
            { QStringLiteral("method"),             method },
            { QStringLiteral("line_free_channels"), lfcArray } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl
                             + QStringLiteral("/v1/spectral/baseline/")
                             + m_sessionId + QStringLiteral("/") + datasetId),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid         = obj.value(QStringLiteral("valid")).toBool();
    result.error         = obj.value(QStringLiteral("error")).toString();
    result.newDatasetId  = obj.value(QStringLiteral("new_dataset_id")).toString();
    result.width         = obj.value(QStringLiteral("width")).toInt();
    result.height        = obj.value(QStringLiteral("height")).toInt();
    result.depth         = obj.value(QStringLiteral("depth")).toInt();
    result.rangeMin      = obj.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax      = obj.value(QStringLiteral("range_max")).toDouble();
    result.polyOrder     = obj.value(QStringLiteral("poly_order")).toInt();
    result.fitChannels   = obj.value(QStringLiteral("fit_channels")).toInt();
    result.outputPath    = obj.value(QStringLiteral("output_path")).toString();
    result.rmsBefore     = obj.value(QStringLiteral("rms_before")).toDouble();
    result.rmsAfter      = obj.value(QStringLiteral("rms_after")).toDouble();
    result.method        = obj.value(QStringLiteral("method")).toString(method);
    return result;
}

// ── S-04: Spectral stacking ───────────────────────────────────────────────────

BackendStackResult BackendClient::fetchStackBinary(const QStringList &datasetIds,
                                                    const QString &method,
                                                    const QVector<double> &weights,
                                                    const QString &weightBy)
{
    BackendStackResult result;
    QString error;
    QJsonArray idsArray;
    for (const QString &id : datasetIds)
        idsArray.append(id);
    QJsonArray wArray;
    for (double w : weights)
        wArray.append(w);
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_ids"), idsArray },
            { QStringLiteral("method"),      method },
            { QStringLiteral("weights"),     wArray },
            { QStringLiteral("weight_by"),   weightBy } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/spectral/stack/binary")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    int offset = 0;
    BinaryArrayResult frame;
    if (!parseBinaryFrame(payload, offset, frame)) {
        result.error = frame.error;
        return result;
    }
    result.valid    = true;
    result.spectrum = frame;
    result.method   = method;
    result.nCubes   = datasetIds.size();
    return result;
}

// ── S-05: SoFiA-2 source finding ──────────────────────────────────────────────

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
                                                        double thresholdValue) const
{
    BackendTaskCreateResult result;
    QString error;
    const QJsonDocument body(
            QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                         { QStringLiteral("moment_order"), order },
                         { QStringLiteral("channel_start"), channelStart },
                         { QStringLiteral("channel_end"), channelEnd },
                         { QStringLiteral("mask_enabled"), maskEnabled },
                         { QStringLiteral("threshold_value"), thresholdValue } });
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
                                                    const std::vector<std::array<int, 2>> &vertices,
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
    return result;
}

QByteArray BackendClient::performGet(const QUrl &url, QString &error) const
{
    QNetworkAccessManager *nam = getThreadLocalNam();
    QNetworkRequest request = buildRequest(url);

    QEventLoop loop;
    QNetworkReply *reply = nam->get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("Backend request timed out after %1 ms: %2")
                        .arg(this->requestTimeoutFor(url).count())
                        .arg(url.path());
        qWarning().noquote() << error;
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"), error, {}, this->m_sessionId);
        reply->abort();
        loop.quit();
    });
    timeoutTimer.start(static_cast<int>(this->requestTimeoutFor(url).count()));
    loop.exec();
    timeoutTimer.stop();

    const QByteArray data = reply->readAll();
    if (error.isEmpty() && reply->error() != QNetworkReply::NoError) {
        error = reply->errorString();
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"),
                QStringLiteral("Backend GET failed for %1: %2").arg(url.path(), error), {},
                this->m_sessionId);
    } else if (error.isEmpty()) {
        const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400) {
            error = QString::fromUtf8(data);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                    QStringLiteral("BackendClient"),
                    QStringLiteral("Backend GET returned HTTP %1 for %2: %3")
                            .arg(status)
                            .arg(url.path(), error),
                    {}, this->m_sessionId);
        }
    }
    reply->deleteLater();
    return data;
}

QByteArray BackendClient::performPost(const QUrl &url, const QJsonDocument &body,
                                      QString &error) const
{
    QNetworkAccessManager *nam = getThreadLocalNam();
    QNetworkRequest request = buildRequest(url);

    QEventLoop loop;
    QNetworkReply *reply = nam->post(request, body.toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("Backend request timed out after %1 ms: %2")
                        .arg(this->requestTimeoutFor(url).count())
                        .arg(url.path());
        qWarning().noquote() << error;
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"), error, {}, this->m_sessionId);
        reply->abort();
        loop.quit();
    });
    timeoutTimer.start(static_cast<int>(this->requestTimeoutFor(url).count()));
    loop.exec();
    timeoutTimer.stop();

    const QByteArray data = reply->readAll();
    if (error.isEmpty() && reply->error() != QNetworkReply::NoError) {
        error = reply->errorString();
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"),
                QStringLiteral("Backend POST failed for %1: %2").arg(url.path(), error), {},
                this->m_sessionId);
    } else if (error.isEmpty()) {
        const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400) {
            error = QString::fromUtf8(data);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                    QStringLiteral("BackendClient"),
                    QStringLiteral("Backend POST returned HTTP %1 for %2: %3")
                            .arg(status)
                            .arg(url.path(), error),
                    {}, this->m_sessionId);
        }
    }
    reply->deleteLater();
    return data;
}

QByteArray BackendClient::decodePayload(const QJsonObject &object, const QString &base64Field,
                                        const QString &compressionField, QString &error)
{
    return decodeCompressedPayload(object.value(base64Field).toString().toUtf8(),
                                   object.value(compressionField).toString(), error);
}

// ── HiPS ──────────────────────────────────────────────────────────────────────

BackendHiPSSurveyInfo BackendClient::openHiPS(const QString &url)
{
    BackendHiPSSurveyInfo result;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("url"), url } });
    QString error;
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/hips/open")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid     = obj.value(QStringLiteral("valid")).toBool(false);
    result.error     = obj.value(QStringLiteral("error")).toString();
    result.surveyId  = obj.value(QStringLiteral("survey_id")).toString();
    result.sessionId = obj.value(QStringLiteral("session_id")).toString();
    result.baseUrl   = obj.value(QStringLiteral("base_url")).toString();
    result.title     = obj.value(QStringLiteral("title")).toString();
    result.frame     = obj.value(QStringLiteral("frame")).toString();
    result.maxOrder  = obj.value(QStringLiteral("max_order")).toInt(9);
    result.minOrder  = obj.value(QStringLiteral("min_order")).toInt(0);
    result.tileWidth = obj.value(QStringLiteral("tile_width")).toInt(512);
    for (const QJsonValue &v : obj.value(QStringLiteral("tile_formats")).toArray()) {
        result.tileFormats.append(v.toString());
    }
    if (result.valid) {
        // Echo the session id for subsequent calls on this client instance.
        m_sessionId = result.sessionId;
    }
    return result;
}

QByteArray BackendClient::requestHiPSAllsky(const QString &surveyId, int order) const
{
    QUrl url(m_baseUrl + QStringLiteral("/v1/hips/") + surveyId
             + QStringLiteral("/allsky"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("order"), QString::number(order));
    url.setQuery(q);
    QString error;
    const QByteArray data = performGet(url, error);
    // Returns raw image bytes; caller checks for empty/error.
    return data;
}

QByteArray BackendClient::requestHiPSTile(const QString &surveyId, int order, int pix) const
{
    const QUrl url(m_baseUrl + QStringLiteral("/v1/hips/") + surveyId
                   + QStringLiteral("/tile/") + QString::number(order)
                   + QStringLiteral("/") + QString::number(pix));
    QString error;
    return performGet(url, error);
}

BackendHiPSViewResponse BackendClient::requestHiPSTilesForView(const QString &surveyId,
                                                               double raDeg, double decDeg,
                                                               double fovDeg, int order) const
{
    BackendHiPSViewResponse result;
    const QUrl url(m_baseUrl + QStringLiteral("/v1/hips/") + surveyId
                   + QStringLiteral("/query_tiles"));
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("survey_id"), surveyId },
            { QStringLiteral("ra_center"),  raDeg   },
            { QStringLiteral("dec_center"), decDeg  },
            { QStringLiteral("fov_deg"),    fovDeg  },
            { QStringLiteral("order"),      order   },
    });
    QString error;
    const QByteArray payload = performPost(url, body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid = obj.value(QStringLiteral("valid")).toBool(false);
    result.error = obj.value(QStringLiteral("error")).toString();
    for (const QJsonValue &v : obj.value(QStringLiteral("tiles")).toArray()) {
        const QJsonObject t = v.toObject();
        BackendHiPSTileInfo info;
        info.pix          = t.value(QStringLiteral("pix")).toInt();
        info.order        = t.value(QStringLiteral("order")).toInt();
        info.raCenterDeg  = t.value(QStringLiteral("ra_center")).toDouble();
        info.decCenterDeg = t.value(QStringLiteral("dec_center")).toDouble();
        const QJsonArray cra  = t.value(QStringLiteral("corners_ra")).toArray();
        const QJsonArray cdec = t.value(QStringLiteral("corners_dec")).toArray();
        for (int i = 0; i < 4; ++i) {
            info.cornersRaDeg[i]  = cra.at(i).toDouble();
            info.cornersDecDeg[i] = cdec.at(i).toDouble();
        }
        result.tiles.push_back(std::move(info));
    }
    return result;
}

// ── Milestone 2 ───────────────────────────────────────────────────────────────

BackendCosmologyBatchResult BackendClient::requestCosmologyDistanceBatch(
        const QVector<double> &redshifts, const QString &model) const
{
    BackendCosmologyBatchResult result;
    const QUrl url(m_baseUrl + QStringLiteral("/v1/cosmology/distance/batch"));
    QJsonArray zArr;
    for (double z : redshifts)
        zArr.append(z);
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("redshifts"), zArr   },
            { QStringLiteral("model"),     model  },
    });
    QString error;
    const QByteArray payload = performPost(url, body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    result.model = obj.value(QStringLiteral("model")).toString();
    for (const QJsonValue &v : obj.value(QStringLiteral("distances_Mpc")).toArray())
        result.distancesMpc.append(v.toDouble());
    return result;
}

BackendTargetResolveResult BackendClient::resolveTarget(const QString &name) const
{
    BackendTargetResolveResult result;
    const QUrl url(m_baseUrl + QStringLiteral("/v1/resolve/target"));
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("name"), name },
    });
    QString error;
    const QByteArray payload = performPost(url, body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid        = obj.value(QStringLiteral("valid")).toBool();
    result.error        = obj.value(QStringLiteral("error")).toString();
    result.raDeg        = obj.value(QStringLiteral("ra_deg")).toDouble();
    result.decDeg       = obj.value(QStringLiteral("dec_deg")).toDouble();
    result.resolvedName = obj.value(QStringLiteral("resolved_name")).toString();
    result.source       = obj.value(QStringLiteral("source")).toString();
    return result;
}

BackendHiPSCatalogueOverlayResult BackendClient::requestHiPSCatalogueOverlay(
        const QString &cataloguePath, double raDeg, double decDeg, double fovDeg,
        const QString &raField, const QString &decField, int maxSources) const
{
    BackendHiPSCatalogueOverlayResult result;
    const QUrl url(m_baseUrl + QStringLiteral("/v1/hips/catalogue_overlay"));
    QJsonObject bodyObj{
            { QStringLiteral("catalogue_path"), cataloguePath },
            { QStringLiteral("ra_center"),      raDeg         },
            { QStringLiteral("dec_center"),     decDeg        },
            { QStringLiteral("fov_deg"),        fovDeg        },
            { QStringLiteral("max_sources"),    maxSources    },
    };
    if (!raField.isEmpty())  bodyObj[QStringLiteral("ra_field")]  = raField;
    if (!decField.isEmpty()) bodyObj[QStringLiteral("dec_field")] = decField;
    QString error;
    const QByteArray payload = performPost(url, QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid       = obj.value(QStringLiteral("valid")).toBool();
    result.error       = obj.value(QStringLiteral("error")).toString();
    result.totalInView = obj.value(QStringLiteral("total_in_view")).toInt();
    result.truncated   = obj.value(QStringLiteral("truncated")).toBool();
    for (const QJsonValue &v : obj.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject s = v.toObject();
        BackendHiPSCatalogueSource src;
        src.raDeg  = s.value(QStringLiteral("ra")).toDouble();
        src.decDeg = s.value(QStringLiteral("dec")).toDouble();
        result.sources.append(src);
    }
    return result;
}

// ── VLKB ──────────────────────────────────────────────────────────────────────

BackendVlkbFetchResult BackendClient::vlkbFetchCutout(const QString &sodaUrl,
                                                        const QString &vlkbBearer) const
{
    BackendVlkbFetchResult result;
    const QUrl url(m_baseUrl + QStringLiteral("/v1/vlkb/fetch_cutout"));
    QJsonObject bodyObj{{ QStringLiteral("soda_url"), sodaUrl }};
    if (!vlkbBearer.isEmpty())
        bodyObj[QStringLiteral("vlkb_bearer")] = vlkbBearer;
    QString error;
    const QByteArray payload = performPost(url, QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    result.path  = obj.value(QStringLiteral("path")).toString();
    return result;
}

// ── A-01: Aperture photometry ─────────────────────────────────────────────────

BackendAperturePhotometryResult BackendClient::requestAperturePhotometry(
        const QString &datasetId,
        double raDeg, double decDeg,
        double apertureArcsec,
        double annulusInnerArcsec,
        double annulusOuterArcsec,
        int channel)
{
    BackendAperturePhotometryResult result;
    QString error;
    QJsonObject bodyObj{
        { QStringLiteral("dataset_id"),           datasetId },
        { QStringLiteral("ra_deg"),               raDeg },
        { QStringLiteral("dec_deg"),              decDeg },
        { QStringLiteral("aperture_arcsec"),      apertureArcsec },
        { QStringLiteral("annulus_inner_arcsec"), annulusInnerArcsec },
        { QStringLiteral("annulus_outer_arcsec"), annulusOuterArcsec },
    };
    if (channel >= 0)
        bodyObj[QStringLiteral("channel")] = channel;
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/photometry/aperture/") + datasetId),
                        QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid               = obj.value(QStringLiteral("valid")).toBool();
    result.error               = obj.value(QStringLiteral("error")).toString();
    result.fluxJy              = obj.value(QStringLiteral("flux_jy")).toDouble();
    result.fluxErrJy           = obj.value(QStringLiteral("flux_err_jy")).toDouble();
    result.backgroundJyPerPix  = obj.value(QStringLiteral("background_jy_per_pix")).toDouble();
    result.nPixAperture        = obj.value(QStringLiteral("n_pix_aperture")).toInt();
    result.channel             = obj.value(QStringLiteral("channel")).toInt(-1);
    result.pixX                = obj.value(QStringLiteral("pix_x")).toDouble();
    result.pixY                = obj.value(QStringLiteral("pix_y")).toDouble();
    result.pixelScaleArcsec    = obj.value(QStringLiteral("pixel_scale_arcsec")).toDouble();
    return result;
}

// ── A-02: 2-D Gaussian source fitting ────────────────────────────────────────

BackendGaussFit2DResult BackendClient::requestGaussFit2D(
        const QString &datasetId,
        double raDeg, double decDeg,
        double boxArcsec,
        int channel)
{
    BackendGaussFit2DResult result;
    QString error;
    QJsonObject bodyObj{
        { QStringLiteral("dataset_id"), datasetId },
        { QStringLiteral("ra_deg"),     raDeg },
        { QStringLiteral("dec_deg"),    decDeg },
        { QStringLiteral("box_arcsec"), boxArcsec },
    };
    if (channel >= 0)
        bodyObj[QStringLiteral("channel")] = channel;
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/photometry/gaussfit/") + datasetId),
                        QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid               = obj.value(QStringLiteral("valid")).toBool();
    result.error               = obj.value(QStringLiteral("error")).toString();
    result.peakJyBeam          = obj.value(QStringLiteral("peak_jy_beam")).toDouble();
    result.integratedFluxJy    = obj.value(QStringLiteral("integrated_flux_jy")).toDouble();
    result.majorArcsec         = obj.value(QStringLiteral("major_arcsec")).toDouble();
    result.minorArcsec         = obj.value(QStringLiteral("minor_arcsec")).toDouble();
    result.paDeg               = obj.value(QStringLiteral("pa_deg")).toDouble();
    result.fitRms              = obj.value(QStringLiteral("fit_rms")).toDouble();
    result.centerPixX          = obj.value(QStringLiteral("center_pix_x")).toDouble();
    result.centerPixY          = obj.value(QStringLiteral("center_pix_y")).toDouble();
    result.background          = obj.value(QStringLiteral("background")).toDouble();
    const QJsonValue dmaj = obj.value(QStringLiteral("deconvolved_major_arcsec"));
    const QJsonValue dmin = obj.value(QStringLiteral("deconvolved_minor_arcsec"));
    result.deconvolvedMajorArcsec = dmaj.isNull() ? std::numeric_limits<double>::quiet_NaN()
                                                   : dmaj.toDouble();
    result.deconvolvedMinorArcsec = dmin.isNull() ? std::numeric_limits<double>::quiet_NaN()
                                                   : dmin.toDouble();
    return result;
}

// ── A-03: WCS reprojection ────────────────────────────────────────────────────

BackendReprojectResult BackendClient::requestReproject(
        const QString &datasetId,
        const QVariantMap &targetWcs,
        int targetNy, int targetNx,
        int order)
{
    BackendReprojectResult result;
    QString error;
    QJsonObject wcsObj;
    for (auto it = targetWcs.constBegin(); it != targetWcs.constEnd(); ++it)
        wcsObj.insert(it.key(), QJsonValue::fromVariant(it.value()));
    const QJsonDocument body(QJsonObject{
        { QStringLiteral("dataset_id"),   datasetId },
        { QStringLiteral("target_wcs"),   wcsObj },
        { QStringLiteral("target_shape"), QJsonArray{ targetNy, targetNx } },
        { QStringLiteral("order"),        order },
    });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/astrometry/reproject/") + datasetId),
                        body, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid         = obj.value(QStringLiteral("valid")).toBool();
    result.error         = obj.value(QStringLiteral("error")).toString();
    result.newDatasetId  = obj.value(QStringLiteral("new_dataset_id")).toString();
    result.width         = obj.value(QStringLiteral("width")).toInt();
    result.height        = obj.value(QStringLiteral("height")).toInt();
    result.rangeMin      = obj.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax      = obj.value(QStringLiteral("range_max")).toDouble();
    result.outputPath    = obj.value(QStringLiteral("output_path")).toString();
    result.order         = obj.value(QStringLiteral("order")).toInt();
    return result;
}

// ── A-04: Catalogue cross-match ───────────────────────────────────────────────

BackendCrossmatchResult BackendClient::requestCrossmatch(
        const QString &datasetId,
        double radiusArcsec,
        const QString &catalogue)
{
    BackendCrossmatchResult result;
    QString error;
    const QUrl url(m_baseUrl
                   + QStringLiteral("/v1/astrometry/crossmatch/") + datasetId
                   + QStringLiteral("?radius_arcsec=") + QString::number(radiusArcsec)
                   + QStringLiteral("&catalogue=") + catalogue);
    const QByteArray raw = performGet(url, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid           = obj.value(QStringLiteral("valid")).toBool();
    result.error           = obj.value(QStringLiteral("error")).toString();
    result.nSources        = obj.value(QStringLiteral("n_sources")).toInt();
    result.truncated       = obj.value(QStringLiteral("truncated")).toBool();
    result.centreRaDeg     = obj.value(QStringLiteral("centre_ra_deg")).toDouble();
    result.centreDecDeg    = obj.value(QStringLiteral("centre_dec_deg")).toDouble();
    result.searchRadiusDeg = obj.value(QStringLiteral("search_radius_deg")).toDouble();
    const QJsonArray srcArr = obj.value(QStringLiteral("sources")).toArray();
    result.sources.reserve(srcArr.size());
    for (const QJsonValue &v : srcArr) {
        const QJsonObject so = v.toObject();
        BackendCrossmatchSource src;
        src.raDeg            = so.value(QStringLiteral("ra_deg")).toDouble();
        src.decDeg           = so.value(QStringLiteral("dec_deg")).toDouble();
        src.name             = so.value(QStringLiteral("name")).toString();
        src.catalogueId      = so.value(QStringLiteral("catalogue_id")).toString();
        src.separationArcsec = so.value(QStringLiteral("separation_arcsec")).toDouble();
        result.sources.append(src);
    }
    return result;
}

// ── P-01: Stokes plane extraction ─────────────────────────────────────────────

BackendStokesPlaneResult BackendClient::requestStokesPlane(
        const QString &datasetId,
        const QString &plane,
        int channel)
{
    BackendStokesPlaneResult result;
    QString error;
    const QUrl url(m_baseUrl
                   + QStringLiteral("/v1/stokes/") + datasetId
                   + QStringLiteral("/binary?plane=") + plane
                   + QStringLiteral("&channel=") + QString::number(channel));
    const QByteArray raw = performGet(url, error);
    if (!error.isEmpty()) { result.error = error; return result; }

    BinaryArrayResult frame;
    int offset = 0;
    if (!parseBinaryFrame(raw, offset, frame)) {
        result.error = QStringLiteral("Failed to parse Stokes binary frame");
        return result;
    }
    result.valid   = true;
    result.plane   = plane;
    result.channel = channel;
    result.width   = frame.cols;
    result.height  = frame.rows;
    result.data    = frame.data;   // QByteArray of raw float32 LE bytes
    // Compute range from float32 payload
    if (!frame.data.isEmpty()) {
        const int nFloats = frame.data.size() / static_cast<int>(sizeof(float));
        const float *ptr = reinterpret_cast<const float *>(frame.data.constData());
        result.rangeMin = *std::min_element(ptr, ptr + nFloats);
        result.rangeMax = *std::max_element(ptr, ptr + nFloats);
    }
    return result;
}

// ── P-02: Polarised intensity and angle ───────────────────────────────────────

BackendPolarisationPiResult BackendClient::requestPolarisationPi(
        const QString &datasetId,
        int channel,
        const QVector<int> &lineFreeChannels)
{
    BackendPolarisationPiResult result;
    QString error;

    // Step 1: register PI+PA maps via JSON endpoint to get dataset IDs
    QJsonArray lfcArr;
    for (int ch : lineFreeChannels) lfcArr.append(ch);
    const QJsonDocument regBody(QJsonObject{
        { QStringLiteral("dataset_id"),       datasetId },
        { QStringLiteral("channel"),          channel },
        { QStringLiteral("line_free_channels"), lfcArr },
    });
    const QByteArray regRaw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/polarisation/pi/") + datasetId),
            regBody, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject regObj = QJsonDocument::fromJson(regRaw).object();
    if (!regObj.value(QStringLiteral("valid")).toBool()) {
        result.error = regObj.value(QStringLiteral("error")).toString();
        return result;
    }
    result.width        = regObj.value(QStringLiteral("width")).toInt();
    result.height       = regObj.value(QStringLiteral("height")).toInt();
    result.channel      = regObj.value(QStringLiteral("channel")).toInt();
    result.sigmaQu2     = regObj.value(QStringLiteral("sigma_qu2")).toDouble();
    result.piRangeMin   = static_cast<float>(regObj.value(QStringLiteral("pi_range_min")).toDouble());
    result.piRangeMax   = static_cast<float>(regObj.value(QStringLiteral("pi_range_max")).toDouble());
    result.paRangeMin   = static_cast<float>(regObj.value(QStringLiteral("pa_range_min")).toDouble());
    result.paRangeMax   = static_cast<float>(regObj.value(QStringLiteral("pa_range_max")).toDouble());
    result.piDatasetId  = regObj.value(QStringLiteral("pi_dataset_id")).toString();
    result.paDatasetId  = regObj.value(QStringLiteral("pa_dataset_id")).toString();

    // Step 2: fetch binary data (two concatenated frames: PI then PA)
    const QJsonDocument binBody(QJsonObject{
        { QStringLiteral("dataset_id"),       datasetId },
        { QStringLiteral("channel"),          channel },
        { QStringLiteral("line_free_channels"), lfcArr },
    });
    const QByteArray binRaw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/polarisation/pi/result/binary")),
            binBody, error);
    if (!error.isEmpty()) { result.error = error; return result; }

    int offset = 0;
    BinaryArrayResult piFrame, paFrame;
    if (!parseBinaryFrame(binRaw, offset, piFrame)) {
        result.error = QStringLiteral("Failed to parse PI binary frame");
        return result;
    }
    if (!parseBinaryFrame(binRaw, offset, paFrame)) {
        result.error = QStringLiteral("Failed to parse PA binary frame");
        return result;
    }
    result.piData = QByteArray(reinterpret_cast<const char *>(piFrame.data.data()),
                               static_cast<qsizetype>(piFrame.data.size() * sizeof(float)));
    result.paData = QByteArray(reinterpret_cast<const char *>(paFrame.data.data()),
                               static_cast<qsizetype>(paFrame.data.size() * sizeof(float)));
    result.valid = true;
    return result;
}

// ── P-03: Rotation Measure synthesis ──────────────────────────────────────────

BackendRmSynthesisResult BackendClient::requestRmSynthesis(
        const QString &datasetId,
        double phiMin,
        double phiMax,
        double dPhi)
{
    BackendRmSynthesisResult result;
    QString error;

    // Step 1: get metadata (phi_grid, rmsf, shape) via JSON endpoint
    const QJsonDocument metaBody(QJsonObject{
        { QStringLiteral("dataset_id"),      datasetId },
        { QStringLiteral("phi_range_rad_m2"), QJsonArray{ phiMin, phiMax } },
        { QStringLiteral("dphi_rad_m2"),     dPhi },
    });
    const QByteArray metaRaw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/polarisation/rmsynthesis/") + datasetId),
            metaBody, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject metaObj = QJsonDocument::fromJson(metaRaw).object();
    if (!metaObj.value(QStringLiteral("valid")).toBool()) {
        result.error = metaObj.value(QStringLiteral("error")).toString();
        return result;
    }
    result.nPhi   = metaObj.value(QStringLiteral("n_phi")).toInt();
    result.nFreq  = metaObj.value(QStringLiteral("n_freq")).toInt();
    result.width  = metaObj.value(QStringLiteral("width")).toInt();
    result.height = metaObj.value(QStringLiteral("height")).toInt();
    result.phiMin = metaObj.value(QStringLiteral("phi_min")).toDouble();
    result.phiMax = metaObj.value(QStringLiteral("phi_max")).toDouble();
    result.dPhi   = metaObj.value(QStringLiteral("d_phi")).toDouble();
    result.rangeMin = static_cast<float>(metaObj.value(QStringLiteral("range_min")).toDouble());
    result.rangeMax = static_cast<float>(metaObj.value(QStringLiteral("range_max")).toDouble());
    for (const QJsonValue &v : metaObj.value(QStringLiteral("phi_grid")).toArray())
        result.phiGrid.append(v.toDouble());
    for (const QJsonValue &v : metaObj.value(QStringLiteral("rmsf")).toArray())
        result.rmsf.append(v.toDouble());

    // Step 2: fetch binary FDF cube (nPhi*height rows × width cols float32)
    const QJsonDocument binBody(QJsonObject{
        { QStringLiteral("dataset_id"),      datasetId },
        { QStringLiteral("phi_range_rad_m2"), QJsonArray{ phiMin, phiMax } },
        { QStringLiteral("dphi_rad_m2"),     dPhi },
    });
    const QByteArray binRaw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/polarisation/rmsynthesis/") + datasetId
                 + QStringLiteral("/binary")),
            binBody, error);
    if (!error.isEmpty()) { result.error = error; return result; }

    int offset = 0;
    BinaryArrayResult fdfFrame;
    if (!parseBinaryFrame(binRaw, offset, fdfFrame)) {
        result.error = QStringLiteral("Failed to parse FDF binary frame");
        return result;
    }
    result.fdfData = QByteArray(reinterpret_cast<const char *>(fdfFrame.data.data()),
                                static_cast<qsizetype>(fdfFrame.data.size() * sizeof(float)));
    result.valid = true;
    return result;
}

// ── C-01: Angular power spectrum ───────────────────────────────────────────────

BackendAngularPowerResult BackendClient::requestAngularPower(
        const QString &datasetId,
        int channel,
        int nBins)
{
    BackendAngularPowerResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
        { QStringLiteral("channel"), channel },
        { QStringLiteral("n_bins"),  nBins },
    });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/cosmology/angular_power/") + datasetId),
                        body, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid      = obj.value(QStringLiteral("valid")).toBool();
    result.error      = obj.value(QStringLiteral("error")).toString();
    result.channel    = obj.value(QStringLiteral("channel")).toInt();
    result.width      = obj.value(QStringLiteral("width")).toInt();
    result.height     = obj.value(QStringLiteral("height")).toInt();
    result.nBins      = obj.value(QStringLiteral("n_bins")).toInt();
    result.kUnit      = obj.value(QStringLiteral("k_unit")).toString();
    result.powerUnit  = obj.value(QStringLiteral("power_unit")).toString();
    for (const QJsonValue &v : obj.value(QStringLiteral("k_bins")).toArray())
        result.kBins.append(v.toDouble());
    for (const QJsonValue &v : obj.value(QStringLiteral("power")).toArray())
        result.power.append(v.toDouble());
    return result;
}

// ── C-03: Skeleton / void detection ────────────────────────────────────────────

BackendSkeletonResult BackendClient::requestSkeleton(
        const QString &datasetId,
        int channel,
        double *threshold,
        const QString &mode)
{
    BackendSkeletonResult result;
    QString error;
    QJsonObject reqObj;
    reqObj.insert(QStringLiteral("channel"), channel);
    reqObj.insert(QStringLiteral("mode"), mode);
    if (threshold)
        reqObj.insert(QStringLiteral("threshold"), *threshold);
    else
        reqObj.insert(QStringLiteral("threshold"), QJsonValue::Null);
    const QJsonDocument body(reqObj);
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/analysis/skeleton/") + datasetId
                             + QStringLiteral("/binary")),
                        body, error);
    if (!error.isEmpty()) { result.error = error; return result; }

    // Try binary parse; if it fails parse as JSON (503 error response)
    if (raw.size() > 12) {
        int offset = 0;
        BinaryArrayResult frame;
        if (parseBinaryFrame(raw, offset, frame)) {
            result.valid          = true;
            result.channel        = channel;
            result.width          = frame.cols;
            result.height         = frame.rows;
            result.mode           = mode;
            {
                const int nFloats = frame.data.size() / static_cast<int>(sizeof(float));
                const float *ptr = reinterpret_cast<const float *>(frame.data.constData());
                result.nSkeletonPixels = static_cast<int>(
                    std::count_if(ptr, ptr + nFloats, [](float v) { return v > 0.f; }));
            }
            result.data = frame.data;   // QByteArray of raw float32 LE bytes
            return result;
        }
    }
    // Fall back to JSON error
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.error   = obj.value(QStringLiteral("error")).toString();
    result.install = obj.value(QStringLiteral("install")).toString();
    result.feature = obj.value(QStringLiteral("feature")).toString();
    return result;
}

// ── R-01: CARTA script export ──────────────────────────────────────────────────

BackendCartaExportResult BackendClient::requestCartaExport(const QStringList &datasetIds)
{
    BackendCartaExportResult result;
    QString error;
    QJsonArray idsArr;
    for (const QString &id : datasetIds) idsArr.append(id);
    const QJsonDocument body(QJsonObject{
        { QStringLiteral("dataset_ids"), idsArr },
    });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/reproducibility/carta_script")),
                        body, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid      = obj.value(QStringLiteral("valid")).toBool();
    result.error      = obj.value(QStringLiteral("error")).toString();
    result.script     = obj.value(QStringLiteral("script")).toString();
    result.nDatasets  = obj.value(QStringLiteral("n_datasets")).toInt();
    return result;
}

// ── R-02: PROV-JSON provenance ─────────────────────────────────────────────────

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

// ── R-03: Parameter snapshots ──────────────────────────────────────────────────

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
