#include "BackendClient.h"

#include <optional>

#include <QDateTime>
#include <QThread>
#include "BackendHeaders.h"
#include "BackendTaskFollow.h"
#include "DiagnosticsManager.h"
#include "SofiaSuggest.h"
#include "TaskQuitDrain.h"

#include <QByteArray>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QSet>
#include <QThreadStorage>
#include <QDebug>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include <QFutureSynchronizer>
#include <QUrl>
#include <QRegularExpression>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>

// ── Private helpers ───────────────────────────────────────────────────────────

namespace {

// Names whose absence in a binary-endpoint response means the client/server
// contract has drifted (not merely an optional field): the geometry that sizes
// the receive buffers, and the scientific units the client displays. When one
// of these is entirely absent the client used to read it back as 0/"" silently;
// visivoHeader() now logs it so the drift is visible (see E-02 / test_contract).
bool isRequiredVisivoHeader(const char *name)
{
    static const QSet<QByteArray> kRequired = {
        QByteArray(VisivoHeaders::WIDTH),
        QByteArray(VisivoHeaders::HEIGHT),
        QByteArray(VisivoHeaders::DEPTH),
        QByteArray(VisivoHeaders::BUNIT),
        QByteArray(VisivoHeaders::SPECTRAL_AXIS_UNIT),
    };
    return kRequired.contains(QByteArray(name));
}

// Read a response header by its canonical name (use the VisivoHeaders::*
// constants so a backend rename tracked by the contract snapshot propagates
// here automatically). Warns when a *required* header is missing.
QByteArray visivoHeader(const QMap<QByteArray, QByteArray> &headers, const char *name)
{
    const QByteArray value = headers.value(QByteArray(name).toLower());
    if (value.isNull() && isRequiredVisivoHeader(name)) {
        qWarning().noquote()
                << "[BackendClient] backend response missing required header" << name
                << "- client/server contract drift? Regenerate contract snapshots.";
    }
    return value;
}

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
        // Only the local backend's token lives in the env / ~/.visivo_token;
        // another backend without a token stays without one.
        if (isLocalBackendUrl(m_baseUrl))
            m_token = BackendClient::readTokenFile();
        return;
    }

    // For LOCAL backends, ~/.visivo_token is written by the running backend's
    // auth.py at startup and is therefore the authoritative source. If the
    // explicit token from Settings is stale (e.g. the backend was restarted
    // and regenerated its token), every request would 401 and trigger the
    // retry path, polluting the diagnostics log. Prefer the file token here
    // to short-circuit that ping-pong. Every other backend keeps its explicit
    // token: ~/.visivo_token belongs to the local backend only, and a backend
    // on this machine at another port (a testbed node) has its own.
    if (isLocalBackendUrl(m_baseUrl)) {
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

namespace {
QMutex s_localUrlMutex;
QString s_localBackendUrl;  // empty = Settings' default
bool s_noLocalBackend = false;

bool isLoopbackHost(const QString &host)
{
    const QString h = host.trimmed().toLower();
    return h.isEmpty() || h == QLatin1StringView("localhost") || h == QLatin1StringView("127.0.0.1")
           || h == QLatin1StringView("::1") || h == QLatin1StringView("[::1]");
}

int effectivePort(const QUrl &url)
{
    return url.port(url.scheme().compare(QLatin1StringView("https"), Qt::CaseInsensitive) == 0 ? 443 : 80);
}
}  // namespace

void BackendClient::setLocalBackendUrl(const QString &url)
{
    QMutexLocker lock(&s_localUrlMutex);
    s_localBackendUrl = url.trimmed();
}

void BackendClient::setNoLocalBackend(bool none)
{
    QMutexLocker lock(&s_localUrlMutex);
    s_noLocalBackend = none;
}

bool BackendClient::isLocalBackendUrl(const QString &url)
{
    QString local;
    {
        QMutexLocker lock(&s_localUrlMutex);
        if (s_noLocalBackend)
            return false;
        local = s_localBackendUrl;
    }
    if (local.isEmpty())
        local = QStringLiteral("http://127.0.0.1:8000");
    if (url.trimmed().isEmpty())
        return true;  // no URL yet: the client is the local one being set up
    const QUrl candidate(url.trimmed());
    const QUrl reference(local);
    if (!isLoopbackHost(candidate.host()) || !isLoopbackHost(reference.host()))
        return false;
    return effectivePort(candidate) == effectivePort(reference);
}

QString BackendClient::readTokenFile()
{
    // Priority 1: environment variable (set by launch script or user shell).
    //
    // Trimmed BEFORE the emptiness test, not after. A VISIVO_TOKEN of " " used
    // to be accepted here and returned as an empty token, so the file was never
    // consulted — while BackendLauncher, which trims first, treated the same
    // value as absent and pinned a real token in the child. The two disagreed
    // and every request came back 401.
    const QString envToken =
            QProcessEnvironment::systemEnvironment()
                    .value(QStringLiteral("VISIVO_TOKEN")).trimmed();
    if (!envToken.isEmpty()) {
        return envToken;
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

/// Paths that may go out before this client has a backend session: the mint
/// route itself, and readiness polling (both stateless). Everything else is
/// held back while the session is unresolved — see the guard in perform*().
static bool _isSessionExemptPath(const QString &path)
{
    return path.endsWith(QStringLiteral("/v1/sessions/new"))
            || path.endsWith(QStringLiteral("/v1/health"))
            || path.endsWith(QStringLiteral("/v1/version"));
}

/// Mint outcome of the request buildRequest() built most recently on THIS
/// thread. The hand-rolled binary paths build and send on the same thread, so
/// this lets performBinaryPost() refuse to put state under an unresolved
/// session without each of its eight callers repeating the check.
static thread_local BackendClient::SessionMint t_lastMint = BackendClient::SessionMint::Ready;


QNetworkRequest BackendClient::buildRequest(const QUrl &url) const
{
    // Mint before any header is set: the mint may refresh m_token, and the auth
    // header and the session id must come from the SAME identity.
    t_lastMint = this->ensureSessionMinted();

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    // R1: Authentication header.
    if (!m_token.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("X-Visivo-Token"), m_token.toUtf8());
    }

    // R3: Session isolation header. Prefer this client's own session id; fall back
    // to the process-wide default so every request from this desktop lands in the
    // same private session (multi-desktop isolation).
    QString session = m_sessionId;
    if (session.isEmpty()) {
        QMutexLocker lock(&s_sessionMutex);
        session = s_minted.value(mintKey(m_baseUrl, resolvedToken(m_baseUrl, m_token)),
                                 s_defaultSessionId);
    }
    if (!session.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("X-Visivo-Session"), session.toUtf8());
    }

    return request;
}

QString BackendClient::s_defaultSessionId;
QHash<QString, QString> BackendClient::s_minted;
QSet<QString> BackendClient::s_mintPinnedToDefault;
QSet<QString> BackendClient::s_mintUnsupportedBase;
QHash<QString, QString> BackendClient::s_tokenAlias;
QMutex BackendClient::s_sessionMutex;
QMutex BackendClient::s_mintMutex;

/// Set while THIS thread is performing the mint request itself, so the request
/// re-entering buildRequest() does not try to mint again (and cannot deadlock on
/// s_mintMutex). Thread-local because the mint runs on whichever thread asked.
static thread_local bool t_mintInFlight = false;

/// Minted sessions are per (backend, token) — see the header.
QString BackendClient::mintKey(const QString &baseUrl, const QString &token)
{
    return baseUrl + QLatin1Char('\n') + token;
}

QString BackendClient::resolvedToken(const QString &baseUrl, const QString &token)
{
    // Alias chains are one hop in practice (old token → refreshed token); the
    // bound just keeps a pathological rotation loop from spinning here.
    QString t = token;
    for (int hops = 0; hops < 4; ++hops) {
        const auto it = s_tokenAlias.constFind(mintKey(baseUrl, t));
        if (it == s_tokenAlias.constEnd() || *it == t)
            break;
        t = *it;
    }
    return t;
}

QString BackendClient::sessionIdForUrl(const QString &baseUrl, const QString &token)
{
    QMutexLocker lock(&s_sessionMutex);
    return s_minted.value(mintKey(baseUrl, resolvedToken(baseUrl, token)), s_defaultSessionId);
}

bool BackendClient::sessionResolvedForUrl(const QString &baseUrl, const QString &token)
{
    QMutexLocker lock(&s_sessionMutex);
    const QString key = mintKey(baseUrl, resolvedToken(baseUrl, token));
    return s_minted.contains(key) || s_mintPinnedToDefault.contains(key)
            || s_mintUnsupportedBase.contains(baseUrl);
}

BackendClient::SessionMint BackendClient::ensureSessionFor(const QString &baseUrl,
                                                           const QString &token,
                                                           QString *effectiveToken,
                                                           QString *sessionId)
{
    BackendClient client(baseUrl, token);
    const SessionMint outcome = client.ensureSessionMinted();
    // The mint may have adopted a refreshed token; hand both back so a caller
    // building its own request puts the matching pair on the wire.
    // performPost() reassigns m_token when it had to fall back to a refreshed
    // one, so token() is now the identity that actually authenticated.
    if (effectiveToken)
        *effectiveToken = client.token();
    if (sessionId)
        *sessionId = sessionIdForUrl(baseUrl, client.token());
    return outcome;
}

bool BackendClient::mintInFlightOnThisThread()
{
    return t_mintInFlight;
}

BackendClient::SessionMint BackendClient::ensureSessionMinted() const
{
    if (m_baseUrl.isEmpty())
        return SessionMint::Deferred;
    if (t_mintInFlight)
        return SessionMint::Deferred;   // our own mint request needs no session

    auto cached = [this]() -> std::optional<SessionMint> {
        QMutexLocker lock(&s_sessionMutex);
        const QString eff = resolvedToken(m_baseUrl, m_token);
        if (eff != m_token) {
            // A previous mint learned our token had been rotated. Adopt the
            // effective one HERE, before any header is built, so the auth token
            // and the session id on the wire are the same identity — and so the
            // token-candidate list in perform*() starts from the live token
            // instead of burning an attempt on a 401.
            const_cast<BackendClient *>(this)->m_token = eff;
        }
        const QString key = mintKey(m_baseUrl, eff);
        if (s_minted.contains(key))
            return SessionMint::Ready;
        if (s_mintPinnedToDefault.contains(key) || s_mintUnsupportedBase.contains(m_baseUrl))
            return SessionMint::Unsupported;
        return std::nullopt;
    };
    if (const auto hit = cached())
        return *hit;

    // One mint at a time: a concurrent caller blocks here and then finds the
    // cached id below. The wait is bounded by the client's request timeout.
    QMutexLocker mintLock(&s_mintMutex);
    if (const auto hit = cached())
        return *hit;

    QString error;
    int status = 0;
    const QString startToken = m_token;   // performPost() may adopt a refreshed one
    t_mintInFlight = true;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/sessions/new")), QJsonDocument(QJsonObject{}),
            error, nullptr, &status);
    t_mintInFlight = false;

    const QString minted = error.isEmpty()
            ? QJsonDocument::fromJson(payload).object()
                      .value(QStringLiteral("session_id")).toString()
            : QString();

    QMutexLocker lock(&s_sessionMutex);
    // Record the rotation so callers still holding the old token hit the cache
    // instead of minting a spare session on every request.
    if (m_token != startToken)
        s_tokenAlias.insert(mintKey(m_baseUrl, startToken), m_token);
    if (!minted.isEmpty()) {
        // performPost() adopts a refreshed token when the first is rejected, so
        // m_token is now the identity that actually authenticated — file the
        // session under THAT key. Never REPLACE an id already in use for that
        // identity: another caller may already be storing datasets under it.
        const QString effKey = mintKey(m_baseUrl, m_token);
        if (!s_minted.contains(effKey))
            s_minted.insert(effKey, minted);
        return SessionMint::Ready;
    }
    if (status == 0) {
        // The backend never answered (still starting, network down). Nothing was
        // stored anywhere, so a later retry is safe — do NOT pin.
        return SessionMint::Deferred;
    }
    // It answered but gave us no session. Either way we pin to the locally
    // generated default id for the rest of the run: retrying could hand us a
    // session id later and split the datasets between the two. A strict backend
    // rejects the default with a clear "Session not found" rather than failing
    // silently. Scope matters, though:
    if (status == 404 || status == 405) {
        // The route does not exist — a property of the SERVER, so base-wide.
        s_mintUnsupportedBase.insert(m_baseUrl);
    } else {
        // 401 / 429 / 5xx are about this caller or this moment; pinning them
        // base-wide would disable minting for every other token on the backend.
        s_mintPinnedToDefault.insert(mintKey(m_baseUrl, m_token));
    }
    return SessionMint::Unsupported;
}

QString BackendClient::effectiveToken(const QString &baseUrl, const QString &token)
{
    QMutexLocker lock(&s_sessionMutex);
    return resolvedToken(baseUrl, token);
}

void BackendClient::applySessionHeaders(QNetworkRequest &request, const QString &baseUrl,
                                        const QString &token, const QString &sessionOverride)
{
    QString identity = token;
    {
        QMutexLocker lock(&s_sessionMutex);
        identity = resolvedToken(baseUrl, token);
    }
    if (!identity.isEmpty())
        request.setRawHeader(QByteArrayLiteral("X-Visivo-Token"), identity.toUtf8());
    // Non-blocking on purpose: these callers run on the GUI thread. By the time a
    // viewer opens, the dataset load has already minted the session; if it has
    // not, we send the process-wide default exactly as buildRequest() would.
    const QString session = !sessionOverride.isEmpty()
            ? sessionOverride
            : sessionIdForUrl(baseUrl, identity);
    if (!session.isEmpty())
        request.setRawHeader(QByteArrayLiteral("X-Visivo-Session"), session.toUtf8());
}

void BackendClient::setDefaultSessionId(const QString &sessionId)
{
    s_defaultSessionId = sessionId;
}

QString BackendClient::defaultSessionId()
{
    return s_defaultSessionId;
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
    result.queuedTasks = object.value(QStringLiteral("queued_tasks")).toInt(0);
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

BackendClassifyResult BackendClient::classifyFile(const QString &path) const
{
    BackendClassifyResult result;
    QString error;
    QJsonObject body;
    body.insert(QStringLiteral("path"), path);
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/files/classify")), QJsonDocument(body), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.path = object.value(QStringLiteral("path")).toString();
    result.openPath = object.value(QStringLiteral("open_path")).toString();
    result.kind = object.value(QStringLiteral("kind")).toString();
    result.opener = object.value(QStringLiteral("opener")).toString();
    result.confident = object.value(QStringLiteral("confident")).toBool(false);
    result.detail = object.value(QStringLiteral("detail")).toString();
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

BackendAssistantResult BackendClient::assistantQuery(const QString &question,
                                                     const QString &datasetId,
                                                     const QJsonArray &history)
{
    BackendAssistantResult result;
    QString error;
    QJsonObject bodyObj{ { QStringLiteral("question"), question } };
    if (!datasetId.isEmpty())
        bodyObj.insert(QStringLiteral("dataset_id"), datasetId);
    if (!history.isEmpty())
        bodyObj.insert(QStringLiteral("history"), history);
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/assistant/query")), QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseAssistantResult(QJsonDocument::fromJson(payload).object());
}

BackendAssistantResult BackendClient::parseAssistantResult(const QJsonObject &o)
{
    BackendAssistantResult result;
    result.answer = o.value(QStringLiteral("answer")).toString();
    result.provider = o.value(QStringLiteral("provider")).toString();
    result.configured = o.value(QStringLiteral("configured")).toBool(false);
    result.toolCallsRecorded = o.value(QStringLiteral("tool_calls_recorded")).toBool(false);
    result.reproducibilityScript = o.value(QStringLiteral("reproducibility_script")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("tool_calls")).toArray()) {
        const QJsonObject c = v.toObject();
        BackendAssistantToolCall call;
        call.name = c.value(QStringLiteral("name")).toString();
        call.argumentsJson = QString::fromUtf8(
                QJsonDocument(c.value(QStringLiteral("arguments")).toObject()).toJson(QJsonDocument::Compact));
        if (c.value(QStringLiteral("result")).isObject())
            call.resultJson = QString::fromUtf8(
                    QJsonDocument(c.value(QStringLiteral("result")).toObject()).toJson(QJsonDocument::Compact));
        call.error = c.value(QStringLiteral("error")).toString();
        result.toolCalls.push_back(call);
    }
    for (const QJsonValue &v : o.value(QStringLiteral("actions")).toArray()) {
        const QJsonObject a = v.toObject();
        if (!a.value(QStringLiteral("type")).toString().isEmpty())
            result.actions.push_back(a);
    }
    result.valid = !result.answer.isEmpty();
    return result;
}

BackendAssistantConfigResult BackendClient::setAssistantConfig(const QJsonObject &body)
{
    BackendAssistantConfigResult result;
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/assistant/config")), QJsonDocument(body), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseAssistantConfig(QJsonDocument::fromJson(payload).object());
}

namespace {
// The model questions go to: the backend's top-level "model" (which includes
// one it picked by itself), else, from a backend older than that field, the
// configured one. "" when neither is known.
QString assistantModelInUse(const QJsonObject &o)
{
    const QJsonValue inUse = o.value(QStringLiteral("model"));
    if (inUse.isString())
        return inUse.toString();
    // config.model is null when none is set
    return o.value(QStringLiteral("config")).toObject().value(QStringLiteral("model")).toString();
}
}  // namespace

BackendAssistantConfigResult BackendClient::parseAssistantConfig(const QJsonObject &o)
{
    BackendAssistantConfigResult result;
    result.valid = o.value(QStringLiteral("valid")).toBool(false);
    result.configured = o.value(QStringLiteral("configured")).toBool(false);
    result.provider = o.value(QStringLiteral("provider")).toString();
    // Absent on a backend older than the field: no problem is then known.
    result.problem = o.value(QStringLiteral("problem")).toString().trimmed();
    result.model = assistantModelInUse(o);
    return result;
}

BackendAssistantStatusResult BackendClient::assistantStatus()
{
    BackendAssistantStatusResult result;
    QString error;
    const QByteArray payload = performGet(
            QUrl(m_baseUrl + QStringLiteral("/v1/assistant/status")), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseAssistantStatus(QJsonDocument::fromJson(payload).object());
}

BackendAssistantStatusResult BackendClient::parseAssistantStatus(const QJsonObject &o)
{
    BackendAssistantStatusResult result;
    // The route has no "valid" flag: a body naming the provider is a status.
    if (!o.contains(QStringLiteral("provider"))) {
        result.error = QStringLiteral("The backend sent no copilot status.");
        return result;
    }
    result.valid = true;
    result.configured = o.value(QStringLiteral("configured")).toBool(false);
    result.provider = o.value(QStringLiteral("provider")).toString();
    result.problem = o.value(QStringLiteral("problem")).toString().trimmed();
    result.model = assistantModelInUse(o);
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

visivo::FilePreview BackendClient::filePreview(const QString &path, int maxBytes,
                                              int maxLines) const
{
    QString error;
    int status = 0;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("path"), path },
                                          { QStringLiteral("max_bytes"), maxBytes },
                                          { QStringLiteral("max_lines"), maxLines } });
    const QByteArray payload = performPost(QUrl(m_baseUrl + QStringLiteral("/v1/files/preview")),
                                           body, error, nullptr, &status);
    if (!error.isEmpty()) {
        visivo::FilePreview failed;
        failed.path = path;
        // On an HTTP error performPost returns the reply's body, and the error
        // text is Qt's ("server replied: Not Found"): the body says why.
        failed.routeMissing = visivo::isMissingRouteReply(status, payload);
        const QString why = status >= 400 ? visivo::replyErrorText(payload) : QString();
        failed.error = why.isEmpty() ? error : why;
        return failed;
    }
    return visivo::parseFilePreviewReply(payload);
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
    // null (absent header card) → 0.0, which every consumer reads as "unknown".
    result.restFreqHz = object.value(QStringLiteral("rest_freq_hz")).toDouble(0.0);
    // ABSENT means "a backend older than this field", and that one may well be
    // able to render: default to capable so it keeps the option. Present-and-
    // false is an explicit no.
    {
        const QJsonValue ssr = object.value(QStringLiteral("server_side_rendering_available"));
        result.serverSideRenderingAvailable =
                (ssr.isUndefined() || ssr.isNull()) ? true : ssr.toBool(false);
    }
    result.bunit = object.value(QStringLiteral("bunit")).toString();

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
    result.fieldName = object.value(QStringLiteral("field_name")).toString();
    for (const QJsonValue &f : object.value(QStringLiteral("fields")).toArray()) {
        const QJsonObject field = f.toObject();
        result.fields.append({ field.value(QStringLiteral("name")).toString(),
                               field.value(QStringLiteral("path")).toString() });
    }
    result.byteOrder = object.value(QStringLiteral("byte_order")).toString();
    result.byteOrderDeclared = object.value(QStringLiteral("byte_order_declared")).toString();
    result.byteOrderWarning =
            visivo::parseVbtByteOrderWarning(object.value(QStringLiteral("byte_order_warning")));
    return result;
}

} // namespace

BackendOpenDatasetResult BackendClient::openDataset(const QString &path)
{
    return openDataset(path, false);
}

BackendOpenDatasetResult BackendClient::openDataset(const QString &path, bool alreadyDecompressed)
{
    BackendOpenDatasetResult result;
    QString error;
    // A compressed FITS is decompressed first, as a task: a large .fits.gz
    // takes minutes, far beyond the open request's timeout. The open that
    // follows finds the decompressed copy in the backend's cache (issue #57).
    if (!alreadyDecompressed && isCompressedFitsName(path)) {
        const QString refusal = decompressForOpen(path);
        if (!refusal.isEmpty()) {
            result.error = refusal;
            return result;
        }
    }
    QJsonObject bodyObj{ { QStringLiteral("path"), path } };
    // A VBT the user chose to read against its header's byte order (#165).
    const QString byteOrder = m_useRememberedVbtByteOrder
            ? visivo::rememberedVbtByteOrder(m_baseUrl, path) : QString();
    if (!byteOrder.isEmpty())
        bodyObj.insert(QStringLiteral("byte_order"), byteOrder);
    const QJsonDocument body(bodyObj);
    int status = 0;
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/datasets/open")), body, error,
                        nullptr, &status);
    if (!error.isEmpty()) {
        // A refusal's reason is in the body (a truncated FITS answers 422 with
        // it, issue #118); performPost's error is only Qt's HTTP phrase.
        result.error = visivo::refusalText(status, payload, error);
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    QString sessionId;
    result = parseOpenDatasetObject(object, sessionId);

    // R3: Persist the session_id so subsequent requests use the same session.
    if (result.valid && !sessionId.isEmpty()) {
        m_sessionId = sessionId;
    }
    if (result.valid)
        visivo::noteVbtReadOrder(result.sessionId, path, result.byteOrder,
                                 result.byteOrderDeclared);
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
    int status = 0;
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/datasets/open_url")),
                        body, error, nullptr, &status);
    if (!error.isEmpty()) {
        result.error = visivo::refusalText(status, payload, error);   // #118: the 422's detail
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
    int status = 0;
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/datasets/open_skava")),
                        body, error, nullptr, &status);
    if (!error.isEmpty()) {
        result.error = visivo::refusalText(status, payload, error);   // #118: the 422's detail
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
    // null (absent header card) → 0.0, which every consumer reads as "unknown".
    result.restFreqHz = object.value(QStringLiteral("rest_freq_hz")).toDouble(0.0);
    // ABSENT means "a backend older than this field", and that one may well be
    // able to render: default to capable so it keeps the option. Present-and-
    // false is an explicit no.
    {
        const QJsonValue ssr = object.value(QStringLiteral("server_side_rendering_available"));
        result.serverSideRenderingAvailable =
                (ssr.isUndefined() || ssr.isNull()) ? true : ssr.toBool(false);
    }
    result.bunit = object.value(QStringLiteral("bunit")).toString();

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
    return openCatalogue(QStringList{ path }, format);
}

BackendCatalogueInfo BackendClient::openCatalogue(const QStringList &paths, const QString &format)
{
    BackendCatalogueInfo result;
    QString error;
    QJsonArray pathsArr;
    for (const QString &p : paths)
        pathsArr.append(p);
    QJsonObject bodyObj{ { QStringLiteral("path"), paths.value(0) },
                         { QStringLiteral("format"), format },
                         { QStringLiteral("paths"), pathsArr } };
    // A VBT the user chose to read against its header's byte order (#165).
    const QString byteOrder = format == QLatin1String("vbt") && m_useRememberedVbtByteOrder
            ? visivo::rememberedVbtByteOrder(m_baseUrl, paths.value(0)) : QString();
    if (!byteOrder.isEmpty())
        bodyObj.insert(QStringLiteral("byte_order"), byteOrder);
    const QJsonDocument body(bodyObj);
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
    result.xField = coord.value(QStringLiteral("x")).toString();
    result.yField = coord.value(QStringLiteral("y")).toString();
    result.zField = coord.value(QStringLiteral("z")).toString();
    result.coordSystem = coord.value(QStringLiteral("system")).toString(QStringLiteral("sky"));
    result.serverSideRenderingAvailable =
            object.value(QStringLiteral("server_side_rendering_available")).toBool(false);
    result.byteOrder = object.value(QStringLiteral("byte_order")).toString();
    result.byteOrderDeclared = object.value(QStringLiteral("byte_order_declared")).toString();
    result.byteOrderWarning =
            visivo::parseVbtByteOrderWarning(object.value(QStringLiteral("byte_order_warning")));
    if (result.valid)
        visivo::noteVbtReadOrder(result.sessionId, paths.value(0), result.byteOrder,
                                 result.byteOrderDeclared);
    const QJsonArray fields = object.value(QStringLiteral("fields")).toArray();
    result.fields.reserve(fields.size());
    for (const QJsonValue &fieldValue : fields) {
        const QJsonObject field = fieldValue.toObject();
        result.fields.push_back({ field.value(QStringLiteral("name")).toString(),
                                  field.value(QStringLiteral("type")).toString() });
    }
    return result;
}

namespace {
void readSampSendResult(const QJsonObject &object, BackendSampSendResult &result)
{
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.jobId = object.value(QStringLiteral("job_id")).toString();
    result.sentDirect = object.value(QStringLiteral("sent_direct")).toBool(false);
    const QJsonObject message = object.value(QStringLiteral("message")).toObject();
    result.mtype = message.value(QStringLiteral("mtype")).toString();
    result.params = message.value(QStringLiteral("params")).toObject().toVariantMap();
    result.rows = static_cast<qint64>(object.value(QStringLiteral("rows")).toDouble(-1));
    result.sizeBytes = static_cast<qint64>(object.value(QStringLiteral("size_bytes")).toDouble(0));
    result.subset = object.value(QStringLiteral("subset")).toBool(false);
}
}  // namespace

BackendSampSendResult BackendClient::sendFitsToSamp(const QString &path,
                                                    const QString &filename,
                                                    bool returnMessage) const
{
    BackendSampSendResult result;
    QString error;
    QJsonObject bodyObj{ { QStringLiteral("path"), path } };
    if (!filename.isEmpty()) {
        bodyObj.insert(QStringLiteral("filename"), filename);
    }
    if (returnMessage) {
        bodyObj.insert(QStringLiteral("deliver"), QStringLiteral("return"));
    }
    const QJsonDocument body(bodyObj);
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/samp/send-fits")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    readSampSendResult(QJsonDocument::fromJson(payload).object(), result);
    return result;
}

BackendSampSendResult BackendClient::sendCatalogueToSamp(const QString &path,
                                                         const QString &format,
                                                         const QString &filename,
                                                         const QString &tableId,
                                                         bool returnMessage,
                                                         const QList<qint64> &rowIndices) const
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
    if (!tableId.isEmpty()) {
        bodyObj.insert(QStringLiteral("table_id"), tableId);
    }
    if (!rowIndices.isEmpty()) {
        QJsonArray rows;
        for (const qint64 r : rowIndices)
            rows.append(r);
        bodyObj.insert(QStringLiteral("row_indices"), rows);
    }
    if (returnMessage) {
        bodyObj.insert(QStringLiteral("deliver"), QStringLiteral("return"));
    }
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/samp/send-catalogue")),
                        QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    readSampSendResult(QJsonDocument::fromJson(payload).object(), result);
    return result;
}

BackendPixelSkyResult BackendClient::pixelToSky(const QString &datasetId, double x, double y,
                                                double z) const
{
    BackendPixelSkyResult result;
    QString error;
    int status = 0;
    const QJsonDocument body(QJsonObject{ { QStringLiteral("dataset_id"), datasetId },
                                          { QStringLiteral("x"), x },
                                          { QStringLiteral("y"), y },
                                          { QStringLiteral("z"), z } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/datasets/pixel_to_sky")), body,
                        error, nullptr, &status);
    if (!error.isEmpty()) {
        result.routeMissing = visivo::isMissingRouteReply(status, payload);
        const QString why = status >= 400 ? visivo::replyErrorText(payload) : QString();
        result.error = why.isEmpty() ? error : why;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.raDeg = object.value(QStringLiteral("ra_deg")).toDouble();
    result.decDeg = object.value(QStringLiteral("dec_deg")).toDouble();
    result.frame = object.value(QStringLiteral("frame")).toString();
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
    return uploadDataToSampStorage(
            raw, filename.isEmpty() ? QFileInfo(localPath).fileName() : filename);
}

BackendSampImportResult BackendClient::uploadDataToSampStorage(const QByteArray &raw,
                                                               const QString &resolvedName) const
{
    BackendSampImportResult result;
    QString error;
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
    const QJsonValue threshold = object.value(QStringLiteral("default_threshold"));
    result.hasDefaultThreshold = threshold.isDouble();
    result.defaultThreshold = threshold.toDouble();
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
/// How long a request already out may still take once the application quits.
/// Nothing, but for a task's CREATE (POST …/v1/tasks/…, also behind a proxy
/// path prefix): the backend may have started the task already, and only the
/// answer carries the id its DELETE needs. That one keeps its own timeout
/// (well inside the drain's ceiling), and the follower, aborted as it
/// registers, then cancels the task.
static int quitGraceMsFor(const QNetworkReply *reply)
{
    const bool create = reply->operation() == QNetworkAccessManager::PostOperation
            && reply->url().path().contains(QStringLiteral("/v1/tasks/"));
    return create ? std::numeric_limits<int>::max() : 0;
}

/// Drop @p reply and end @p loop once the application is quitting
/// (TaskQuitDrain::drain) and the request's grace (quitGraceMsFor) is over,
/// setting @p abandoned. A worker's request must end while the application
/// still exists: its loop cannot run after, and the application's destructor
/// waits for the worker (#133). Any request, the session mint included; the
/// cancellation DELETE (cancelTask) is let through, bounded on its own.
static void abandonWhenQuitting(QTimer &watch, QEventLoop &loop, QNetworkReply *reply,
                                bool &abandoned)
{
    const int graceMs = quitGraceMsFor(reply);
    auto quitSeen = std::make_shared<QElapsedTimer>();
    QObject::connect(&watch, &QTimer::timeout, &loop, [&loop, reply, &abandoned, graceMs, quitSeen]() {
        if (!visivo::TaskQuitDrain::instance().quitting())
            return;
        if (!quitSeen->isValid())
            quitSeen->start();
        if (quitSeen->elapsed() < graceMs)
            return;
        abandoned = true;
        reply->abort();
        loop.quit();
    });
    watch.start(50);
}

/// POST a binary body and return the raw reply. Refuses to send when the
/// session is still unresolved because the backend never answered — putting
/// state under the fallback id would let a later successful mint split this
/// client's datasets across two sessions (see performPost()).
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
    if (t_lastMint == BackendClient::SessionMint::Deferred
        && !_isSessionExemptPath(reqTemplate.url().path())) {
        error = QStringLiteral("Backend session not established yet: %1 is unreachable.")
                        .arg(reqTemplate.url().host());
        return {};
    }
    QNetworkRequest request = reqTemplate;
    // Default to JSON, but preserve a content type the caller already set (e.g.
    // application/octet-stream for a raw binary body like the mask-apply payload).
    if (!request.hasRawHeader("Content-Type"))
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
    QTimer quitWatch;
    bool abandoned = false;
    abandonWhenQuitting(quitWatch, loop, reply, abandoned);
    if (visivo::TaskQuitDrain::instance().quitting())   // dropped before this loop sends it
        abandoned = true;
    else
        loop.exec();
    quitWatch.stop();
    timeoutTimer.stop();
    if (visivo::TaskQuitDrain::instance().quitting() && !reply->isFinished())
        abandoned = true;   // exit() ended the loop before the watch fired
    if (abandoned && !reply->isFinished())
        reply->abort();

    const QByteArray body = reply->readAll();
    statusOut = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    for (const auto &header : reply->rawHeaderPairs()) {
        headersOut.insert(header.first.toLower(), header.second);
    }
    if (abandoned) {
        error = QStringLiteral("Request abandoned: the application is quitting.");
    } else if (timedOut) {
        error = QStringLiteral("Backend binary request timed out after %1 ms").arg(timeoutMs);
    } else if (reply->error() != QNetworkReply::NoError && statusOut < 400) {
        error = reply->errorString();
    }
    reply->deleteLater();
    return body;
}


BackendCubeSubvolumeResult BackendClient::requestSubvolume(const QString &datasetId, int x0,
                                                           int x1, int y0, int y1, int z0,
                                                           int z1, bool keepSlabs) const
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
    // Fallback to the JSON endpoint ONLY on a 404 (an older backend without
    // the binary route). It used to fall back on any transport error too, and
    // that was the hazard: a timed-out or failed slab of a multi-GB cube sent
    // the whole volume again as base64 JSON, which the client then held as the
    // reply buffer, its copy, the parsed document and the decoded bytes — four
    // times the cube on top of everything else, while the backend kept serving
    // the aborted slabs. A failed slab now fails the load, and says so.
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
        // Resolve exactly like buildRequest(): own id → this backend's minted id
        // → the process-wide default. Sending nothing here would put these slab
        // requests in a different session than the rest of the client's traffic.
        // The mint may adopt a refreshed token, so capture the token AFTER it:
        // the auth header and the session id must come from the same identity.
        const SessionMint mint = this->ensureSessionMinted();
        const QString token = this->m_token;
        const QString sessionId = !this->m_sessionId.isEmpty()
                ? this->m_sessionId
                : BackendClient::sessionIdForUrl(this->m_baseUrl, token);
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
        auto fetchSlab = [baseUrl, token, sessionId, timeoutMs, datasetId, mint,
                          x0, x1, y0, y1](int sz0, int sz1) -> SlabResult {
            SlabResult r;
            // performBinaryPost() reads t_lastMint, which buildRequest() would
            // have set — but we build the request by hand on a worker thread,
            // where that thread_local is still at its default. Carry the
            // parent's verdict across so the guard applies here too.
            t_lastMint = mint;
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
                return visivoHeader(headers, name);
            };
            r.width  = hget(VisivoHeaders::WIDTH).toInt();
            r.height = hget(VisivoHeaders::HEIGHT).toInt();
            r.depth  = hget(VisivoHeaders::DEPTH).toInt();
            r.scalarType        = QString::fromUtf8(hget(VisivoHeaders::SCALAR_TYPE));
            r.rangeMin          = hget(VisivoHeaders::RANGE_MIN).toDouble();
            r.rangeMax          = hget(VisivoHeaders::RANGE_MAX).toDouble();
            r.spectralAxisType  = QString::fromUtf8(hget(VisivoHeaders::SPECTRAL_AXIS_TYPE));
            r.spectralAxisUnit  = QString::fromUtf8(hget(VisivoHeaders::SPECTRAL_AXIS_UNIT));
            r.bunit             = QString::fromUtf8(hget(VisivoHeaders::BUNIT));
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
        bool routeMissing = false;
        QString firstError;
        for (auto &f : futures) {
            f.waitForFinished();
            SlabResult sr = f.result();
            if (!sr.ok && !anyFailed) {
                firstError = sr.error;
            }
            if (!sr.ok) {
                anyFailed = true;
                routeMissing = routeMissing || sr.status == 404;
            }
            slabs.append(std::move(sr));
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
            QList<QByteArray> kept;
            if (keepSlabs) {
                kept.reserve(slabs.size());
                for (auto &s : slabs) {
                    kept.append(std::move(s.data));
                }
            } else {
                stitched.reserve(int(totalBytes));
                for (const auto &s : slabs) {
                    stitched.append(s.data);
                }
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
            result.slabs             = kept;
            return result;
        }

        if (!routeMissing) {
            result = BackendCubeSubvolumeResult{};
            result.error = firstError.isEmpty()
                    ? QStringLiteral("subvolume_bin parallel fetch failed")
                    : firstError;
            qWarning().noquote()
                    << QStringLiteral("subvolume_bin parallel fetch failed: %1").arg(result.error);
            return result;
        }
        qWarning().noquote()
                << QStringLiteral("subvolume_bin is not served by this backend (404): "
                                  "falling back to the JSON endpoint.");
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

namespace {

// "a,b,c" → three finite doubles; false (and `out` untouched) otherwise.
bool parseVelocityTriple(const QByteArray &value, double out[3])
{
    const QStringList p = QString::fromUtf8(value).split(QLatin1Char(','));
    if (p.size() != 3)
        return false;
    double v[3];
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        v[i] = p[i].trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(v[i]))
            return false;
    }
    std::copy(v, v + 3, out);
    return true;
}

// X-Visivo-Downsample-Factors, "fx,fy,fz": three integers >= 1, else false.
bool parseVelocityFactors(const QByteArray &value, int out[3])
{
    const QStringList p = QString::fromUtf8(value).split(QLatin1Char(','));
    if (p.size() != 3)
        return false;
    int v[3];
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        v[i] = p[i].trimmed().toInt(&ok);
        if (!ok || v[i] < 1)
            return false;
    }
    std::copy(v, v + 3, out);
    return true;
}

} // namespace

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
        return visivoHeader(headers, name);
    };
    const auto parse3 = [](const QByteArray &b, double &a, double &c, double &d) {
        const QStringList p = QString::fromUtf8(b).split(QLatin1Char(','));
        if (p.size() == 3) { a = p[0].toDouble(); c = p[1].toDouble(); d = p[2].toDouble(); }
    };
    r.nx = hget(VisivoHeaders::WIDTH).toInt();
    r.ny = hget(VisivoHeaders::HEIGHT).toInt();
    r.nz = hget(VisivoHeaders::DEPTH).toInt();
    parse3(hget(VisivoHeaders::SPACING), r.sx, r.sy, r.sz);
    parse3(hget(VisivoHeaders::ORIGIN), r.ox, r.oy, r.oz);
    r.speedMin = hget(VisivoHeaders::SPEED_MIN).toDouble();
    r.speedMax = hget(VisivoHeaders::SPEED_MAX).toDouble();
    {
        const QByteArray fmin = hget(VisivoHeaders::FULL_SPEED_MIN);
        const QByteArray fmax = hget(VisivoHeaders::FULL_SPEED_MAX);
        r.fullSpeedMin = fmin.isEmpty() ? r.speedMin : fmin.toDouble();
        r.fullSpeedMax = fmax.isEmpty() ? r.speedMax : fmax.toDouble();
    }
    r.unit  = QString::fromUtf8(hget(VisivoHeaders::UNIT));
    r.frame = QString::fromUtf8(hget(VisivoHeaders::FRAME));
    r.downsampleFactor = std::max(1, hget(VisivoHeaders::DOWNSAMPLE_FACTOR).toInt());
    {
        const QStringList fs = QString::fromUtf8(hget(VisivoHeaders::FULL_SHAPE)).split(QLatin1Char(','));
        if (fs.size() == 3) { r.fullNz = fs[0].toInt(); r.fullNy = fs[1].toInt(); r.fullNx = fs[2].toInt(); }
    }
    {
        // x,y,z order, unlike Full-Shape.
        int f[3];
        if (parseVelocityFactors(hget(VisivoHeaders::DOWNSAMPLE_FACTORS), f)) {
            r.factorX = f[0]; r.factorY = f[1]; r.factorZ = f[2];
            r.hasAxisFactors = true;
        }
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

BackendKinematicModelResult BackendClient::requestKinematicModel(
        const QString &datasetId, double x0, double y0, double paDeg, double inclDeg,
        double vsys, double vrot, double rmaxPix, double sigma,
        const QVector<std::array<double, 4>> &rings) const
{
    BackendKinematicModelResult r;
    QJsonObject bodyObj{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("x0"), x0 }, { QStringLiteral("y0"), y0 },
            { QStringLiteral("pa_deg"), paDeg }, { QStringLiteral("incl_deg"), inclDeg },
            { QStringLiteral("vsys"), vsys }, { QStringLiteral("vrot"), vrot },
            { QStringLiteral("rmax_pix"), rmaxPix }, { QStringLiteral("sigma"), sigma } };
    if (!rings.isEmpty()) {
        QJsonArray ringArr;
        for (const auto &ring : rings) {
            ringArr.append(QJsonObject{
                    { QStringLiteral("r_pix"), ring[0] }, { QStringLiteral("pa_deg"), ring[1] },
                    { QStringLiteral("incl_deg"), ring[2] }, { QStringLiteral("vrot"), ring[3] } });
        }
        bodyObj.insert(QStringLiteral("rings"), ringArr);
    }
    const QJsonDocument body(bodyObj);
    QNetworkRequest req =
            buildRequest(QUrl(m_baseUrl + QStringLiteral("/v1/cube/kinematic_model_bin")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray data = performBinaryPost(
            nam, req, body.toJson(QJsonDocument::Compact), 120000, status, headers, err);
    if (status != 200) {
        // Body carries a plain-text reason for 413 (too large) / 500.
        const QString reason = !data.isEmpty() ? QString::fromUtf8(data)
                             : (err.isEmpty() ? QString() : err);
        r.error = reason.isEmpty()
                          ? QStringLiteral("HTTP %1 on kinematic_model_bin").arg(status)
                          : QStringLiteral("HTTP %1: %2").arg(status).arg(reason);
        return r;
    }
    const auto hget = [&](const char *name) -> QByteArray {
        return visivoHeader(headers, name);
    };
    r.width  = hget(VisivoHeaders::WIDTH).toInt();
    r.height = hget(VisivoHeaders::HEIGHT).toInt();
    r.depth  = hget(VisivoHeaders::DEPTH).toInt();
    r.peak   = hget(VisivoHeaders::PEAK).toDouble();
    r.spectralAxisUnit = QString::fromUtf8(hget(VisivoHeaders::SPECTRAL_AXIS_UNIT));
    r.spectralMode     = QString::fromUtf8(hget(VisivoHeaders::SPECTRAL_MODE));
    const qint64 expected =
            qint64(r.width) * qint64(r.height) * qint64(r.depth) * qint64(sizeof(float));
    if (r.width <= 0 || r.height <= 0 || r.depth <= 0 || expected != data.size()) {
        r.error = QStringLiteral("bad kinematic model payload: got %1 bytes, expected %2")
                          .arg(data.size()).arg(expected);
        return r;
    }
    r.data = data;
    r.valid = true;
    return r;
}

// Build a JSON array of [z,y,x] seed triples.
static QJsonArray seedsToJson(const QVector<std::array<int, 3>> &seeds)
{
    QJsonArray arr;
    for (const auto &s : seeds) {
        arr.append(QJsonArray{ s[0], s[1], s[2] });
    }
    return arr;
}

BackendKinematicLevelsResult BackendClient::kinematicLevels(
        const QString &datasetId, int seedZ, int seedY, int seedX,
        double reachMax, double lambdaV, double sigma, double nsigma, int roiRadius,
        const QVector<std::array<int, 3>> &addSeeds,
        const QVector<std::array<int, 3>> &subSeeds, double smoothV, double smoothXy) const
{
    BackendKinematicLevelsResult r;
    QJsonObject bodyObj{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("seed_z"), seedZ }, { QStringLiteral("seed_y"), seedY },
            { QStringLiteral("seed_x"), seedX },
            { QStringLiteral("reach"), reachMax }, { QStringLiteral("reach_max"), reachMax },
            { QStringLiteral("lambda_v"), lambdaV },
            { QStringLiteral("sigma"), sigma }, { QStringLiteral("nsigma"), nsigma },
            { QStringLiteral("roi_radius"), roiRadius } };
    if (!addSeeds.isEmpty()) bodyObj.insert(QStringLiteral("add_seeds"), seedsToJson(addSeeds));
    if (!subSeeds.isEmpty()) bodyObj.insert(QStringLiteral("sub_seeds"), seedsToJson(subSeeds));
    if (smoothV > 0.0 || smoothXy > 0.0) {
        bodyObj.insert(QStringLiteral("smooth_v"), smoothV);
        bodyObj.insert(QStringLiteral("smooth_xy"), smoothXy);
    }
    const QJsonDocument body(bodyObj);
    QNetworkRequest req =
            buildRequest(QUrl(m_baseUrl + QStringLiteral("/v1/cube/kinematic_levels_bin")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray data = performBinaryPost(
            nam, req, body.toJson(QJsonDocument::Compact), 120000, status, headers, err);
    if (status != 200) {
        const QString reason = !data.isEmpty() ? QString::fromUtf8(data)
                             : (err.isEmpty() ? QString() : err);
        r.error = reason.isEmpty()
                          ? QStringLiteral("HTTP %1 on kinematic_levels_bin").arg(status)
                          : QStringLiteral("HTTP %1: %2").arg(status).arg(reason);
        return r;
    }
    const auto hget = [&](const char *name) -> QByteArray {
        return visivoHeader(headers, name);
    };
    r.roiWidth  = hget(VisivoHeaders::WIDTH).toInt();
    r.roiHeight = hget(VisivoHeaders::HEIGHT).toInt();
    r.roiDepth  = hget(VisivoHeaders::DEPTH).toInt();
    r.reachMax  = hget(VisivoHeaders::RANGE_MAX).toInt();
    r.trimBox   = hget(VisivoHeaders::TRIM_BOX).toInt();
    const int sentinel = hget(VisivoHeaders::PEAK).toInt();
    if (sentinel > 0 && sentinel <= 255) r.unreachable = sentinel;
    const auto origin = hget(VisivoHeaders::ORIGIN).split(',');
    if (origin.size() == 3) {
        r.originX = origin[0].toInt();
        r.originY = origin[1].toInt();
        r.originZ = origin[2].toInt();
    }
    const auto full = hget(VisivoHeaders::FULL_SHAPE).split(',');
    if (full.size() == 3) {
        r.fullWidth  = full[0].toInt();
        r.fullHeight = full[1].toInt();
        r.fullDepth  = full[2].toInt();
    }
    // Two uint8 fields of identical geometry, add first — reject anything else
    // rather than indexing past the end while thresholding.
    // Dimensions come from response headers, so bound each side BEFORE the
    // multiply: three values near INT_MAX overflow the qint64 product and the
    // size check below would then pass on a nonsense payload.
    constexpr int kMaxRoiSide = 1 << 20;
    if (r.roiWidth <= 0 || r.roiHeight <= 0 || r.roiDepth <= 0
        || r.roiWidth > kMaxRoiSide || r.roiHeight > kMaxRoiSide || r.roiDepth > kMaxRoiSide) {
        r.error = QStringLiteral("kinematic level ROI dimension out of range: %1x%2x%3")
                          .arg(r.roiWidth).arg(r.roiHeight).arg(r.roiDepth);
        return r;
    }
    const qint64 one = qint64(r.roiWidth) * qint64(r.roiHeight) * qint64(r.roiDepth);
    if (data.size() != 2 * one) {
        r.error = QStringLiteral("bad kinematic level payload: got %1 bytes, expected %2")
                          .arg(data.size()).arg(2 * one);
        return r;
    }
    // The split below and the float32 mask the caller builds from these fields are
    // both int-indexed; refuse an oversized ROI here rather than truncating it into
    // a short buffer downstream. A single field this large is >2 GB — a backend
    // bug or a hostile response, not a real selection.
    if (one > qint64(std::numeric_limits<int>::max()) / qint64(sizeof(float))) {
        r.error = QStringLiteral("kinematic level ROI too large: %1 voxels").arg(one);
        return r;
    }
    r.addLevels = data.left(qsizetype(one));
    r.subLevels = data.mid(qsizetype(one));
    r.valid = true;
    return r;
}

BackendKinematicSelectResult BackendClient::kinematicSelect(
        const QString &datasetId, int seedZ, int seedY, int seedX,
        double reach, double lambdaV, double sigma, double nsigma, int roiRadius,
        const QVector<std::array<int, 3>> &addSeeds,
        const QVector<std::array<int, 3>> &subSeeds, double smoothV, double smoothXy) const
{
    BackendKinematicSelectResult r;
    QJsonObject bodyObj{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("seed_z"), seedZ }, { QStringLiteral("seed_y"), seedY },
            { QStringLiteral("seed_x"), seedX },
            { QStringLiteral("reach"), reach }, { QStringLiteral("lambda_v"), lambdaV },
            { QStringLiteral("sigma"), sigma }, { QStringLiteral("nsigma"), nsigma },
            { QStringLiteral("roi_radius"), roiRadius } };
    if (!addSeeds.isEmpty()) bodyObj.insert(QStringLiteral("add_seeds"), seedsToJson(addSeeds));
    if (!subSeeds.isEmpty()) bodyObj.insert(QStringLiteral("sub_seeds"), seedsToJson(subSeeds));
    if (smoothV > 0.0 || smoothXy > 0.0) {
        bodyObj.insert(QStringLiteral("smooth_v"), smoothV);
        bodyObj.insert(QStringLiteral("smooth_xy"), smoothXy);
    }
    const QJsonDocument body(bodyObj);
    QNetworkRequest req =
            buildRequest(QUrl(m_baseUrl + QStringLiteral("/v1/cube/kinematic_select_bin")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray data = performBinaryPost(
            nam, req, body.toJson(QJsonDocument::Compact), 120000, status, headers, err);
    if (status != 200) {
        const QString reason = !data.isEmpty() ? QString::fromUtf8(data)
                             : (err.isEmpty() ? QString() : err);
        r.error = reason.isEmpty()
                          ? QStringLiteral("HTTP %1 on kinematic_select_bin").arg(status)
                          : QStringLiteral("HTTP %1: %2").arg(status).arg(reason);
        return r;
    }
    const auto hget = [&](const char *name) -> QByteArray {
        return visivoHeader(headers, name);
    };
    r.roiWidth  = hget(VisivoHeaders::WIDTH).toInt();
    r.roiHeight = hget(VisivoHeaders::HEIGHT).toInt();
    r.roiDepth  = hget(VisivoHeaders::DEPTH).toInt();
    r.count     = hget(VisivoHeaders::SELECTED_COUNT).toInt();
    r.trimBox   = hget(VisivoHeaders::TRIM_BOX).toInt();
    const auto origin = hget(VisivoHeaders::ORIGIN).split(',');
    if (origin.size() == 3) {
        r.originX = origin[0].toInt();
        r.originY = origin[1].toInt();
        r.originZ = origin[2].toInt();
    }
    const auto full = hget(VisivoHeaders::FULL_SHAPE).split(',');
    if (full.size() == 3) {
        r.fullWidth  = full[0].toInt();
        r.fullHeight = full[1].toInt();
        r.fullDepth  = full[2].toInt();
    }
    const auto bb = hget(VisivoHeaders::BBOX).split(',');
    for (int i = 0; i < 6 && i < bb.size(); ++i) {
        r.bbox[i] = bb[i].toInt();
    }
    const qint64 expected =
            qint64(r.roiWidth) * qint64(r.roiHeight) * qint64(r.roiDepth) * qint64(sizeof(float));
    if (r.roiWidth <= 0 || r.roiHeight <= 0 || r.roiDepth <= 0 || expected != data.size()) {
        r.error = QStringLiteral("bad kinematic selection payload: got %1 bytes, expected %2")
                          .arg(data.size()).arg(expected);
        return r;
    }
    r.data = data;
    r.valid = true;
    return r;
}

BackendSaveSubregionResult BackendClient::maskCubeSelection(
        const QString &datasetId, int seedZ, int seedY, int seedX,
        const QVector<std::array<int, 3>> &addSeeds, const QVector<std::array<int, 3>> &subSeeds,
        double reach, double lambdaV, const QString &mode,
        double sigma, double nsigma, int roiRadius, const QString &outputBasename) const
{
    BackendSaveSubregionResult result;
    QJsonObject bodyObj{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("seed_z"), seedZ }, { QStringLiteral("seed_y"), seedY },
            { QStringLiteral("seed_x"), seedX },
            { QStringLiteral("reach"), reach }, { QStringLiteral("lambda_v"), lambdaV },
            { QStringLiteral("sigma"), sigma }, { QStringLiteral("nsigma"), nsigma },
            { QStringLiteral("roi_radius"), roiRadius },
            { QStringLiteral("mode"), mode },
            { QStringLiteral("output_basename"), outputBasename } };
    if (!addSeeds.isEmpty()) bodyObj.insert(QStringLiteral("add_seeds"), seedsToJson(addSeeds));
    if (!subSeeds.isEmpty()) bodyObj.insert(QStringLiteral("sub_seeds"), seedsToJson(subSeeds));
    QString error;
    const QByteArray raw = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/cube/mask_selection")), QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return parseSaveSubregionObject(QJsonDocument::fromJson(raw).object());
}

BackendSaveSubregionResult BackendClient::applyCubeMask(
        const QString &datasetId, const QByteArray &maskBytes,
        int originX, int originY, int originZ,
        int width, int height, int depth,
        const QString &mode, const QString &outputBasename, const QString &recipe) const
{
    BackendSaveSubregionResult result;
    QUrl url(m_baseUrl + QStringLiteral("/v1/cube/mask_apply_bin"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("dataset_id"), datasetId);
    q.addQueryItem(QStringLiteral("origin_x"), QString::number(originX));
    q.addQueryItem(QStringLiteral("origin_y"), QString::number(originY));
    q.addQueryItem(QStringLiteral("origin_z"), QString::number(originZ));
    q.addQueryItem(QStringLiteral("width"), QString::number(width));
    q.addQueryItem(QStringLiteral("height"), QString::number(height));
    q.addQueryItem(QStringLiteral("depth"), QString::number(depth));
    q.addQueryItem(QStringLiteral("mode"), mode);
    if (!outputBasename.isEmpty())
        q.addQueryItem(QStringLiteral("output_basename"), outputBasename);
    if (!recipe.isEmpty())   // addQueryItem takes ENCODED input
        q.addQueryItem(QStringLiteral("recipe"), QString::fromUtf8(QUrl::toPercentEncoding(recipe)));
    url.setQuery(q.query(QUrl::FullyEncoded), QUrl::StrictMode);

    QNetworkRequest req = buildRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray raw =
            performBinaryPost(nam, req, maskBytes, 120000, status, headers, err);
    if (status != 200) {
        const QString reason = !raw.isEmpty() ? QString::fromUtf8(raw)
                             : (err.isEmpty() ? QString() : err);
        result.error = reason.isEmpty()
                ? QStringLiteral("HTTP %1 on mask_apply_bin").arg(status)
                : QStringLiteral("HTTP %1: %2").arg(status).arg(reason);
        return result;
    }
    return parseSaveSubregionObject(QJsonDocument::fromJson(raw).object());
}

BackendKinematicFitResult BackendClient::kinematicFit(
        const QString &datasetId, const QByteArray &maskBytes,
        int originX, int originY, int originZ, int width, int height, int depth,
        double sigma, int minRingPix) const
{
    BackendKinematicFitResult result;
    QUrl url(m_baseUrl + QStringLiteral("/v1/cube/kinematic_fit_bin"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("dataset_id"), datasetId);
    q.addQueryItem(QStringLiteral("origin_x"), QString::number(originX));
    q.addQueryItem(QStringLiteral("origin_y"), QString::number(originY));
    q.addQueryItem(QStringLiteral("origin_z"), QString::number(originZ));
    q.addQueryItem(QStringLiteral("width"), QString::number(width));
    q.addQueryItem(QStringLiteral("height"), QString::number(height));
    q.addQueryItem(QStringLiteral("depth"), QString::number(depth));
    if (sigma > 0.0)
        q.addQueryItem(QStringLiteral("sigma"), QString::number(sigma, 'g', 17));
    q.addQueryItem(QStringLiteral("min_ring_pix"), QString::number(minRingPix));
    url.setQuery(q.query(QUrl::FullyEncoded), QUrl::StrictMode);

    QNetworkRequest req = buildRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    // A per-ring fit scans angles over every annulus: allow it minutes on a big cube.
    const QByteArray raw = performBinaryPost(nam, req, maskBytes, 600000, status, headers, err);
    if (status != 200) {
        QString reason = QString::fromUtf8(raw);
        const QJsonObject eo = QJsonDocument::fromJson(raw).object();
        if (eo.contains(QStringLiteral("detail")))
            reason = eo.value(QStringLiteral("detail")).toString();
        if (reason.isEmpty())
            reason = err;
        result.error = reason.isEmpty() ? QStringLiteral("HTTP %1 on kinematic_fit_bin").arg(status)
                                        : QStringLiteral("HTTP %1: %2").arg(status).arg(reason);
        return result;
    }
    return parseKinematicFitObject(QJsonDocument::fromJson(raw).object());
}

BackendKinematicResidualResult BackendClient::kinematicResidual(
        const QString &datasetId, const QByteArray &maskBytes,
        int originX, int originY, int originZ, int width, int height, int depth,
        double x0, double y0, double vsys, double lineSigma,
        double pa, double incl, double vrot, double rmax,
        const QVector<std::array<double, 4>> &rings, double sigma) const
{
    BackendKinematicResidualResult result;
    QUrl url(m_baseUrl + QStringLiteral("/v1/cube/kinematic_residual_bin"));
    QUrlQuery q;
    const auto num = [](double v) { return QString::number(v, 'g', 17); };
    q.addQueryItem(QStringLiteral("dataset_id"), datasetId);
    q.addQueryItem(QStringLiteral("origin_x"), QString::number(originX));
    q.addQueryItem(QStringLiteral("origin_y"), QString::number(originY));
    q.addQueryItem(QStringLiteral("origin_z"), QString::number(originZ));
    q.addQueryItem(QStringLiteral("width"), QString::number(width));
    q.addQueryItem(QStringLiteral("height"), QString::number(height));
    q.addQueryItem(QStringLiteral("depth"), QString::number(depth));
    q.addQueryItem(QStringLiteral("x0"), num(x0));
    q.addQueryItem(QStringLiteral("y0"), num(y0));
    q.addQueryItem(QStringLiteral("vsys"), num(vsys));
    q.addQueryItem(QStringLiteral("line_sigma"), num(lineSigma));
    if (rings.isEmpty()) {
        q.addQueryItem(QStringLiteral("pa"), num(pa));
        q.addQueryItem(QStringLiteral("incl"), num(incl));
        q.addQueryItem(QStringLiteral("vrot"), num(vrot));
        q.addQueryItem(QStringLiteral("rmax"), num(rmax));
    } else {
        QStringList items;
        for (const auto &ring : rings)
            items << QStringLiteral("%1:%2:%3:%4").arg(num(ring[0]), num(ring[1]), num(ring[2]),
                                                        num(ring[3]));
        q.addQueryItem(QStringLiteral("rings"),
                       QString::fromUtf8(QUrl::toPercentEncoding(items.join(QLatin1Char(',')))));
    }
    if (sigma > 0.0)
        q.addQueryItem(QStringLiteral("sigma"), num(sigma));
    url.setQuery(q.query(QUrl::FullyEncoded), QUrl::StrictMode);

    QNetworkRequest req = buildRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray raw = performBinaryPost(nam, req, maskBytes, 300000, status, headers, err);
    if (status != 200) {
        QString reason = QString::fromUtf8(raw);
        const QJsonObject eo = QJsonDocument::fromJson(raw).object();
        if (eo.contains(QStringLiteral("detail")))
            reason = eo.value(QStringLiteral("detail")).toString();
        if (reason.isEmpty())
            reason = err;
        result.error = reason.isEmpty() ? QStringLiteral("HTTP %1 on kinematic_residual_bin").arg(status)
                                        : QStringLiteral("HTTP %1: %2").arg(status).arg(reason);
        return result;
    }
    return parseKinematicResidualObject(QJsonDocument::fromJson(raw).object());
}

BackendKinematicResidualResult BackendClient::parseKinematicResidualObject(const QJsonObject &o)
{
    BackendKinematicResidualResult r;
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    const auto num = [nan](const QJsonObject &obj, const char *key) {
        const QJsonValue v = obj.value(QLatin1String(key));
        return v.isDouble() ? v.toDouble() : nan;
    };
    r.width = o.value(QStringLiteral("width")).toInt();
    r.height = o.value(QStringLiteral("height")).toInt();
    r.sigma = num(o, "sigma");
    r.sigmaSource = o.value(QStringLiteral("sigma_source")).toString();
    r.velocityConvention = o.value(QStringLiteral("velocity_convention")).toString();
    const QJsonObject ro = o.value(QStringLiteral("residual")).toObject();
    r.residual.valid = !ro.isEmpty();
    r.residual.nPix = ro.value(QStringLiteral("n_pix")).toInt();
    r.residual.median = num(ro, "median");
    r.residual.p90 = num(ro, "p90");
    r.residual.withinChannel = num(ro, "within_channel");
    r.model = QByteArray::fromBase64(o.value(QStringLiteral("model_base64")).toString().toLatin1());
    r.velocityField = QByteArray::fromBase64(
            o.value(QStringLiteral("velocity_field_base64")).toString().toLatin1());
    const qsizetype plane = qsizetype(r.width) * r.height * qsizetype(sizeof(float));
    r.valid = r.width > 0 && r.height > 0 && r.residual.valid && r.model.size() == plane
              && r.velocityField.size() == plane;
    if (!r.valid)
        r.error = QStringLiteral("The kinematic residual returned an unexpected payload.");
    return r;
}

BackendKinematicFitResult BackendClient::parseKinematicFitObject(const QJsonObject &o)
{
    BackendKinematicFitResult r;
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    // JSON null (a measure that could not be made) reads as NaN, never as 0.
    const auto num = [nan](const QJsonObject &obj, const char *key) {
        const QJsonValue v = obj.value(QLatin1String(key));
        return v.isDouble() ? v.toDouble() : nan;
    };
    const auto b64 = [&o](const char *key) {
        return QByteArray::fromBase64(o.value(QLatin1String(key)).toString().toLatin1());
    };
    r.width = o.value(QStringLiteral("width")).toInt();
    r.height = o.value(QStringLiteral("height")).toInt();
    r.velocityUnit = o.value(QStringLiteral("velocity_unit")).toString();
    r.velocityConvention = o.value(QStringLiteral("velocity_convention")).toString();
    r.sigma = num(o, "sigma");
    r.sigmaSource = o.value(QStringLiteral("sigma_source")).toString();
    r.beamAreaPx = num(o, "beam_area_px");
    const QJsonObject m = o.value(QStringLiteral("measures")).toObject();
    r.flux = num(m, "flux");
    r.fluxIsJy = m.value(QStringLiteral("flux_is_jy")).toBool();
    r.w20 = num(m, "w20");
    r.w50 = num(m, "w50");
    r.vsysW20 = num(m, "vsys_w20");
    r.nVelocityField = m.value(QStringLiteral("n_velocity_field")).toInt();
    const QJsonObject g = o.value(QStringLiteral("geometry")).toObject();
    r.x0 = num(g, "x0"); r.y0 = num(g, "y0");
    r.paMorph = num(g, "pa_morph"); r.axisRatio = num(g, "axis_ratio");
    r.incl = num(g, "incl"); r.rmax = num(g, "rmax");
    const QJsonObject f = o.value(QStringLiteral("flat")).toObject();
    r.pa = num(f, "pa"); r.vsys = num(f, "vsys"); r.vrot = num(f, "vrot");
    r.vrotSini = num(f, "vrot_sini"); r.fitRms = num(f, "rms");
    for (const QJsonValue &v : o.value(QStringLiteral("rings")).toArray()) {
        const QJsonObject ro = v.toObject();
        r.rings.append({ num(ro, "r"), num(ro, "pa"), num(ro, "incl"), num(ro, "vrot"),
                         ro.value(QStringLiteral("n_pix")).toInt() });
    }
    r.lineSigma = num(o, "line_sigma");
    r.channelWidth = num(o, "channel_width");
    const auto residual = [&num](const QJsonValue &v) {
        BackendKinematicResidual res;
        if (!v.isObject())
            return res;   // null: no warped model
        const QJsonObject ro = v.toObject();
        res.valid = true;
        res.nPix = ro.value(QStringLiteral("n_pix")).toInt();
        res.median = num(ro, "median");
        res.p90 = num(ro, "p90");
        res.withinChannel = num(ro, "within_channel");
        return res;
    };
    r.residualFlat = residual(o.value(QStringLiteral("residual_flat")));
    r.residualWarp = residual(o.value(QStringLiteral("residual_warp")));
    for (const QJsonValue &v : o.value(QStringLiteral("spectrum")).toArray())
        r.spectrum.append(v.isDouble() ? v.toDouble() : nan);
    for (const QJsonValue &v : o.value(QStringLiteral("spectrum_velocity")).toArray())
        r.spectrumVelocity.append(v.isDouble() ? v.toDouble() : nan);
    r.velocityField = b64("velocity_field_base64");
    r.modelFlat = b64("model_flat_base64");
    r.modelWarp = b64("model_warp_base64");
    const qsizetype plane = qsizetype(r.width) * r.height * qsizetype(sizeof(float));
    r.valid = r.width > 0 && r.height > 0 && r.velocityField.size() == plane
              && r.modelFlat.size() == plane
              && (r.modelWarp.isEmpty() || r.modelWarp.size() == plane);
    if (!r.valid)
        r.error = QStringLiteral("The kinematic fit returned maps of an unexpected size.");
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
        return visivoHeader(headers, name);
    };
    r.nx = hget(VisivoHeaders::WIDTH).toInt();
    r.ny = hget(VisivoHeaders::HEIGHT).toInt();
    r.nz = hget(VisivoHeaders::DEPTH).toInt();
    r.roiX0 = hget(VisivoHeaders::ROI_X0).toInt();
    r.roiY0 = hget(VisivoHeaders::ROI_Y0).toInt();
    r.roiZ0 = hget(VisivoHeaders::ROI_Z0).toInt();
    r.speedMin = hget(VisivoHeaders::SPEED_MIN).toDouble();
    r.speedMax = hget(VisivoHeaders::SPEED_MAX).toDouble();
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
                                                               int basinMaxDim, int direction,
                                                               double boxMpc) const
{
    BackendVelocityBasinsResult r;
    QJsonObject bodyObj{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("basin_max_dim"), basinMaxDim },
            { QStringLiteral("direction"), direction },
            // Always, 0 included: an omitted box means the dataset's own box to
            // the backend, which may not be the one the viewer draws in.
            { QStringLiteral("box_mpc"), std::max(0.0, boxMpc) } };
    const QJsonDocument body(bodyObj);
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
        return visivoHeader(headers, name);
    };
    r.nx = hget(VisivoHeaders::WIDTH).toInt();
    r.ny = hget(VisivoHeaders::HEIGHT).toInt();
    r.nz = hget(VisivoHeaders::DEPTH).toInt();
    r.nBasins = hget(VisivoHeaders::N_BASINS).toInt();
    // All three or none: half a geometry would place the labels inconsistently.
    double spacing[3], origin[3];
    int factors[3];
    if (parseVelocityTriple(hget(VisivoHeaders::SPACING), spacing)
        && parseVelocityTriple(hget(VisivoHeaders::ORIGIN), origin)
        && parseVelocityFactors(hget(VisivoHeaders::DOWNSAMPLE_FACTORS), factors)) {
        std::copy(spacing, spacing + 3, r.spacing);
        std::copy(origin, origin + 3, r.origin);
        std::copy(factors, factors + 3, r.factors);
        r.hasGeometry = true;
    }
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

BackendVelocityDensityResult BackendClient::fetchVelocityDensity(const QString &datasetId,
                                                                 int maxDim,
                                                                 double boxMpc) const
{
    BackendVelocityDensityResult r;
    // box_mpc always, 0 included (see fetchVelocityBasins).
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("max_dim"), maxDim },
            { QStringLiteral("box_mpc"), std::max(0.0, boxMpc) } });
    QNetworkRequest req = buildRequest(QUrl(m_baseUrl + QStringLiteral("/v1/velocity/density_bin")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkAccessManager *nam = getThreadLocalNam();
    QMap<QByteArray, QByteArray> headers;
    QString err;
    int status = 0;
    const QByteArray data = performBinaryPost(
            nam, req, body.toJson(QJsonDocument::Compact), 300000, status, headers, err);
    if (status != 200) {
        r.error = err.isEmpty() ? QStringLiteral("HTTP %1 on velocity/density_bin").arg(status) : err;
        return r;
    }
    const auto hget = [&](const char *name) -> QByteArray {
        return visivoHeader(headers, name);
    };
    r.nx = hget(VisivoHeaders::WIDTH).toInt();
    r.ny = hget(VisivoHeaders::HEIGHT).toInt();
    r.nz = hget(VisivoHeaders::DEPTH).toInt();
    r.divMin = hget(VisivoHeaders::DIV_MIN).toDouble();
    r.divMax = hget(VisivoHeaders::DIV_MAX).toDouble();
    r.unit = QString::fromUtf8(hget(VisivoHeaders::UNIT));
    const auto parse3 = [&](const char *name, double out[3]) {
        const QList<QByteArray> parts = hget(name).split(',');
        for (int i = 0; i < 3 && i < parts.size(); ++i)
            out[i] = parts[i].trimmed().toDouble();
    };
    parse3(VisivoHeaders::SPACING, r.spacing);
    parse3(VisivoHeaders::ORIGIN, r.origin);
    const qint64 expected = qint64(r.nx) * qint64(r.ny) * qint64(r.nz) * qint64(sizeof(float));
    if (r.nx <= 0 || r.ny <= 0 || r.nz <= 0 || expected != data.size()) {
        r.error = QStringLiteral("bad density payload: got %1 bytes, expected %2")
                          .arg(data.size()).arg(expected);
        return r;
    }
    r.divergence = data;
    r.valid = true;
    return r;
}

BackendCubePvResult BackendClient::requestPv(const QString &datasetId,
                                             const std::vector<std::array<double, 2>> &vertices,
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
            performPostRetryingBackpressure(QUrl(m_baseUrl + QStringLiteral("/v1/cube/pv")), body, error);
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
            performPostRetryingBackpressure(QUrl(m_baseUrl + QStringLiteral("/v1/cube/noise")),
                                            body, error);
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
    const QString byteOrder = m_useRememberedVbtByteOrder   // #165
            ? visivo::rememberedVbtByteOrder(m_baseUrl, path) : QString();
    if (!byteOrder.isEmpty())
        bodyObj.insert(QStringLiteral("byte_order"), byteOrder);
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
    result.byteOrder = object.value(QStringLiteral("byte_order")).toString();
    result.byteOrderDeclared = object.value(QStringLiteral("byte_order_declared")).toString();
    result.byteOrderWarning =
            visivo::parseVbtByteOrderWarning(object.value(QStringLiteral("byte_order_warning")));
    if (result.valid)
        visivo::noteVbtReadOrder(result.sessionId, path, result.byteOrder,
                                 result.byteOrderDeclared);
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
    if (req.randomSample)
        body.insert(QStringLiteral("random_sample"), true);
    if (req.evenSample || req.randomSample) {
        body.insert(QStringLiteral("even_sample"), req.evenSample);
        body.insert(QStringLiteral("phase"), req.phase);
    }
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
    result.step = std::max(1, object.value(QStringLiteral("step")).toInt(1));
    const QJsonArray rows = object.value(QStringLiteral("rows")).toArray();
    for (const QJsonValue &rowValue : rows) {
        result.rows.append(rowValue.toObject().toVariantMap());
    }
    // One position per row, or none: a partial list could not be matched up.
    const QJsonArray rowIndex = object.value(QStringLiteral("row_index")).toArray();
    if (rowIndex.size() == rows.size()) {
        result.rowIndex.reserve(rowIndex.size());
        for (const QJsonValue &v : rowIndex)
            result.rowIndex.append(static_cast<qint64>(v.toDouble(-1)));
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

BackendImageTileResult BackendClient::requestImageTile(const QString &datasetId, int level,
                                                       int tileX, int tileY, int tileSize) const
{
    BackendImageTileResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"), datasetId },
            { QStringLiteral("level"), level },
            { QStringLiteral("tile_x"), tileX },
            { QStringLiteral("tile_y"), tileY },
            { QStringLiteral("tile_size"), tileSize } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/image/tile")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    result = parseImageTileObject(object);
    if (result.tileSize <= 0) {
        result.tileSize = tileSize;
    }
    result.data = decodePayload(object, QStringLiteral("data_base64"),
                                QStringLiteral("compression"), result.error);
    if (!result.error.isEmpty()) {
        result.valid = false;
    }
    return result;
}

BackendImageTileResult BackendClient::parseImageTileObject(const QJsonObject &object)
{
    BackendImageTileResult result;
    result.valid = object.value(QStringLiteral("valid")).toBool(false);
    result.error = object.value(QStringLiteral("error")).toString();
    result.width = object.value(QStringLiteral("width")).toInt();
    result.height = object.value(QStringLiteral("height")).toInt();
    result.fullWidth = object.value(QStringLiteral("full_width")).toInt(result.width);
    result.fullHeight = object.value(QStringLiteral("full_height")).toInt(result.height);
    result.level = object.value(QStringLiteral("level")).toInt();
    result.numLevels = object.value(QStringLiteral("num_levels")).toInt(1);
    result.tileX = object.value(QStringLiteral("tile_x")).toInt();
    result.tileY = object.value(QStringLiteral("tile_y")).toInt();
    result.tileSize = object.value(QStringLiteral("tile_size")).toInt();
    result.scalarType = object.value(QStringLiteral("scalar_type")).toString();
    result.rangeMin = object.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax = object.value(QStringLiteral("range_max")).toDouble();
    result.averaged = object.value(QStringLiteral("averaged")).toBool(false);
    result.pyramidState = object.value(QStringLiteral("pyramid")).toString();
    result.pyramidProgress = object.value(QStringLiteral("pyramid_progress")).toDouble();
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
            performPostRetryingBackpressure(
                    QUrl(m_baseUrl + QStringLiteral("/v1/products/isosurface")), body, error);
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
                                                 bool maskEnabled, double thresholdValue,
                                                 bool thresholdAuto) const
{
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
            performPostRetryingBackpressure(
                    QUrl(m_baseUrl + QStringLiteral("/v1/products/moment")), body, error);
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
                                                    const QString &sessionId,
                                                    bool thresholdAuto)
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
                         { QStringLiteral("threshold_value"), static_cast<double>(threshold) },
                         { QStringLiteral("threshold_auto"), thresholdAuto } });
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

namespace {
/// Endpoints that go through the backend's heavy-task machinery. One list, two
/// consumers: the long client timeout (see requestTimeoutFor) and the in-flight
/// counter the status bar shows.
const QStringList &heavyComputePaths()
{
    static const QStringList paths{
        QStringLiteral("/spectral/linewidth"), QStringLiteral("/spectral/stack"),
        QStringLiteral("/spectral/baseline"),  QStringLiteral("/products/isosurface"),
        QStringLiteral("/products/moment"),    QStringLiteral("/products/map_math"),
        QStringLiteral("/products/mosaic"),    QStringLiteral("/products/publication_figure"),
        QStringLiteral("/products/image_quality"), QStringLiteral("/cube/pv"),
        QStringLiteral("/cube/noise"),         QStringLiteral("/cube/mask"),
        QStringLiteral("/velocity/"),          QStringLiteral("/kinematic"),
        // Resampling an AREPO window is minutes of yt, not seconds: on the
        // interactive timeout the client called a running job failed.
        QStringLiteral("/simulations/arepo/extract"),
        // Downloading and unpacking a batch of cutouts is minutes of
        // archive time, not an interactive request.
        QStringLiteral("/vlkb/mcutout/fetch"),
        // A second on a laptop, but it runs under _run_heavy_with_limit: on
        // slow storage the server's ceiling, not 120 s, is the one to honour.
        QStringLiteral("/sofia2/suggest"),
    };
    return paths;
}

bool isHeavyComputePath(const QString &path)
{
    for (const QString &fragment : heavyComputePaths())
        if (path.contains(fragment))
            return true;
    return false;
}

/// Atomic: requests run from worker threads (each with its own QNAM).
std::atomic<int> g_heavyInFlight{ 0 };

/// RAII counter, so an early return or an exception cannot leak a count.
struct HeavyRequestScope
{
    const bool counted;
    explicit HeavyRequestScope(const QString &path) : counted(isHeavyComputePath(path))
    {
        if (counted)
            ++g_heavyInFlight;
    }
    ~HeavyRequestScope()
    {
        if (counted)
            --g_heavyInFlight;
    }
};
}   // namespace

int BackendClient::heavyRequestsInFlight()
{
    return g_heavyInFlight.load();
}

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
    if (path.contains(QStringLiteral("/tasks/")))
        return std::chrono::milliseconds(visivo::taskRequestTimeoutMs(path));
    if (path.endsWith(QStringLiteral("/cube/slice")) || path.endsWith(QStringLiteral("/image/preview"))) {
        return std::chrono::milliseconds(30000);
    }
    // Server-side downloads: both of these stream a remote FITS through an
    // urlopen(timeout=600) on the backend, so 300 s here had the client abort
    // while the server was still allowed five more minutes — the same mismatch
    // that broke the line-width map, in two more places (audit).
    if (path.endsWith(QStringLiteral("/vlkb/fetch_cutout"))
        || path.endsWith(QStringLiteral("/datasets/open_url"))) {
        return std::chrono::milliseconds(660000); // 11 min > server 600 s
    }
    // Source finding runs SoFiA-2 as a child process on the backend; on a
    // multi-GB cube that is minutes. It fell through to the generic 120 s below,
    // and the server had NO ceiling at all until this audit gave it one
    // (VISIVO_SOFIA_TIMEOUT_S, 900 s) — so the client stays above that.
    if (path.startsWith(QStringLiteral("/v1/sourcefind/"))) {
        return std::chrono::milliseconds(960000); // 16 min > server 900 s
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
        || path.endsWith(QStringLiteral("/cube/subvolume_bin"))
        // Whole-cube stats stream the FULL-res cube once out-of-core server-side; on an
        // SKA-scale cube that single pass can take minutes on slow storage / a tunnel.
        || path.contains(QStringLiteral("/analysis/cube-stats"))
        // A region's statistics at full resolution read the region from the
        // file, several times for its median on a large one (issue #95).
        || path.contains(QStringLiteral("/regions/stats"))) {
        return std::chrono::milliseconds(600000); // 10 min
    }
    // Copilot query: the server enforces its own wall-clock budget
    // (VISIVO_ASSISTANT_TIMEOUT_S, default 120 s) and returns a graceful 504.
    // Keep the client timeout comfortably ABOVE it so the server's helpful
    // message wins the race instead of a raw client abort.
    if (path.contains(QStringLiteral("/assistant/query"))) {
        return std::chrono::milliseconds(180000); // 3 min > server 120 s budget
    }
    // ── Heavy scientific compute ─────────────────────────────────────────────
    // Same rule as the copilot endpoint above, which was the only place that
    // followed it: the client timeout must sit ABOVE the server's own ceiling,
    // so the server's answer (a result, or its graceful 504) wins the race.
    //
    // These endpoints go through the backend's `_run_heavy*`, bounded by
    // VISIVO_HEAVY_TIMEOUT_S — 900 s by default. The generic 120 s below meant
    // the client aborted a line-width map on a 1.5 GB cube after two minutes
    // while the backend went on computing for up to fifteen, holding a session
    // compute slot the whole time (which is then one of the three that answer
    // 429 to the next tool). Measured on a 1046×952×398 cube: the client gave
    // up, the work did not.
    if (isHeavyComputePath(path))
        return std::chrono::milliseconds(960000);   // 16 min > server 900 s
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
    {
        // float64 little-endian, uncompressed: a float32 frequency in Hz would
        // lose the channel width.
        const QByteArray raw = QByteArray::fromBase64(
                object.value(QStringLiteral("spectral_coordinates_base64")).toString().toLatin1());
        if (!raw.isEmpty() && raw.size() == result.depth * qsizetype(sizeof(double))) {
            result.spectralCoordinates.resize(result.depth);
            for (int i = 0; i < result.depth; ++i)
                result.spectralCoordinates[i] = qFromLittleEndian<double>(raw.constData() + i * sizeof(double));
        }
    }
    {
        const QJsonArray m = object.value(QStringLiteral("sky_matrix_arcsec")).toArray();
        if (m.size() == 4)
            for (const QJsonValue &v : m)
                result.skyMatrixArcsec.push_back(v.toDouble());
    }
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
/// Short operation tag for the Diagnostics "Operation" column, derived from the
/// path rather than from a table: "/v1/spectral/linewidth/binary" →
/// "spectral/linewidth". Until now the column was filled ONLY by the async task
/// poller, so it was blank for every synchronous compute — which is most of them
/// (user report).
QString BackendClient::operationTagForPath(const QString &path)
{
    QString p = path;
    if (p.startsWith(QStringLiteral("/v1/")))
        p = p.mid(4);
    // Transport suffixes are not operations.
    for (const QString &suffix : { QStringLiteral("/binary"), QStringLiteral("/bin"),
                                   QStringLiteral("_bin") })
        if (p.endsWith(suffix))
            p.chop(suffix.size());
    const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    // Two segments read best ("products/isosurface"); a trailing id is noise, so
    // a segment that is not a word is dropped.
    QStringList keep;
    for (const QString &seg : parts) {
        static const QRegularExpression word(QStringLiteral("^[a-z][a-z0-9_\\-]*$"));
        if (!word.match(seg).hasMatch())
            break;
        keep << seg;
        if (keep.size() == 2)
            break;
    }
    return keep.join(QLatin1Char('/'));
}

/// The dataset a request is about, for the "Context" column: from the JSON body
/// of a POST, or the query string of a GET. Every call site passed an empty
/// datasetId, so Context showed nothing but the session id.
static QString _datasetIdForRequest(const QJsonDocument &body, const QUrl &url)
{
    if (body.isObject()) {
        const QString id = body.object().value(QStringLiteral("dataset_id")).toString();
        if (!id.isEmpty())
            return id;
    }
    const QUrlQuery query(url);
    return query.queryItemValue(QStringLiteral("dataset_id"));
}

static bool _isDiagnosticsQuietPath(const QString &path)
{
    // Anything the UI POLLS on a timer. One line per poll per window drowns the
    // Diagnostics log in traffic nobody asked about: with the Data Hub open,
    // /v1/exports/list alone is every 4 s, and during a multi-minute compute
    // that was the only thing on screen — the compute's own two lines scrolled
    // out of sight (user report). The requests still happen and still show up
    // in the qWarning stream on failure; they just stop narrating success.
    return path.startsWith(QStringLiteral("/v1/diagnostics/"))
        || path == QStringLiteral("/v1/health")
        || path == QStringLiteral("/v1/exports/list")
        // The headset pairing dialog polls it every few seconds.
        || path.endsWith(QStringLiteral("/v1/vr/pair"))
        // The headset hub: presence polled, activity read after each long poll.
        || path.endsWith(QStringLiteral("/v1/vr/headset"))
        || path.endsWith(QStringLiteral("/v1/vr/activity"));
}

// A route polled every few seconds by a window left open (the headset pairing
// status): with the backend gone, one Error line per poll would bury the log.
// Its failures are reported once per outage: the first failure, then nothing
// until the route has answered again.
static bool _isOncePerOutagePath(const QString &path)
{
    return path.endsWith(QStringLiteral("/v1/vr/pair"))
        || path.endsWith(QStringLiteral("/v1/vr/headset"))
        || path.endsWith(QStringLiteral("/v1/vr/activity"));
}

static QMutex s_outageMutex;
static QSet<QString> s_outages;   ///< base URL + path of the routes failing now

/// True for the first failure since @p key last answered.
static bool _firstFailureOfOutage(const QString &key)
{
    QMutexLocker lock(&s_outageMutex);
    if (s_outages.contains(key))
        return false;
    s_outages.insert(key);
    return true;
}

static bool _inOutage(const QString &key)
{
    QMutexLocker lock(&s_outageMutex);
    return s_outages.contains(key);
}

static void _outageOver(const QString &key)
{
    QMutexLocker lock(&s_outageMutex);
    s_outages.remove(key);
}

QByteArray BackendClient::performGet(const QUrl &url, QString &error,
                                     const std::atomic_bool *abort, int *statusOut) const
{
    if (statusOut)
        *statusOut = 0;
    // Counted while it runs (RAII), so the status bar can say "N running" for a
    // synchronous compute — and announced when it STARTS, not only when it ends:
    // the log used to be silent for the whole of a multi-minute job (user report).
    const HeavyRequestScope heavyScope(url.path());
    if (heavyScope.counted) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"),
                QStringLiteral("%1 %2 started - this can take minutes on a large cube")
                        .arg(QStringLiteral("GET"), url.path()),
                _datasetIdForRequest(QJsonDocument(), url),
                this->m_sessionId, BackendClient::operationTagForPath(url.path()));
    }

    // R3 consistency: when the backend never answered the mint we have no
    // session, and sending state-bearing traffic under the fallback id would let
    // a later successful mint split this client's datasets across two sessions.
    // Such a request would almost certainly fail anyway (the backend is
    // unreachable), so refuse it here and let the caller retry once minting
    // works. Readiness polling stays exempt so callers can wait for the backend.
    if (!m_baseUrl.isEmpty() && !_isSessionExemptPath(url.path())
        && ensureSessionMinted() == SessionMint::Deferred) {
        error = QStringLiteral("Backend session not established yet: %1 is unreachable.")
                        .arg(m_baseUrl);
        return {};
    }
    QElapsedTimer perfTimer;
    perfTimer.start();
    QNetworkAccessManager *nam = getThreadLocalNam();
    // The 401 retry with the local token file: for the local backend only.
    const QString fallbackToken =
            isLocalBackendUrl(m_baseUrl) ? BackendClient::readTokenFile().trimmed() : QString();
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
            // A once-per-outage route reports it below, with the outage.
            if (!_isOncePerOutagePath(url.path())) {
                qWarning().noquote() << attemptError;
                DiagnosticsManager::instance()->publish(
                        DiagnosticsLevel::Error, DiagnosticsCategory::Backend,
                        QStringLiteral("BackendClient"), attemptError,
                        _datasetIdForRequest(QJsonDocument(), url), this->m_sessionId,
                        BackendClient::operationTagForPath(url.path()));
            }
            reply->abort();
            loop.quit();
        });
        timeoutTimer.start(static_cast<int>(this->requestTimeoutFor(url).count()));
        // A caller that may be cancelled (a task followed for tens of minutes)
        // must not wait out a long read: watch its flag and drop the reply.
        QTimer abortWatch;
        if (abort) {
            QObject::connect(&abortWatch, &QTimer::timeout, &loop, [&]() {
                if (!abort->load())
                    return;
                attemptError = QStringLiteral("Request abandoned: cancelled by the user.");
                reply->abort();
                loop.quit();
            });
            abortWatch.start(100);
        }
        QTimer quitWatch;
        bool abandoned = false;
        abandonWhenQuitting(quitWatch, loop, reply, abandoned);
        // Quitting already: the reply is dropped before this loop serves it
        // (the GET itself may already be on its way; nobody reads the answer).
        if (visivo::TaskQuitDrain::instance().quitting())
            abandoned = true;
        else if (!(abort && abort->load()))
            loop.exec();
        else
            attemptError = QStringLiteral("Request abandoned: cancelled by the user.");
        quitWatch.stop();
        abortWatch.stop();
        timeoutTimer.stop();
        // On the application thread, exit() ends this loop at once, before the
        // watch has fired: an unanswered reply is then no empty success.
        if (visivo::TaskQuitDrain::instance().quitting() && !reply->isFinished())
            abandoned = true;
        if (abandoned) {
            attemptError = QStringLiteral("Request abandoned: the application is quitting.");
            if (!reply->isFinished())
                reply->abort();
        }
        if (!attemptError.isEmpty() && abort && abort->load() && !reply->isFinished())
            reply->abort();

        const QByteArray data = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusOut)
            *statusOut = status;   // 0 = transport failure
        if (attemptError.isEmpty() && reply->error() != QNetworkReply::NoError) {
            attemptError = reply->errorString();
        } else if (attemptError.isEmpty() && status >= 400) {
            attemptError = QString::fromUtf8(data);
        }
        reply->deleteLater();

        // No second token while quitting: the exit waits for this worker.
        if (status == 401 && attempt + 1 < tokenCandidates.size()
            && !visivo::TaskQuitDrain::instance().quitting()) {
            // Not again and again for a polled route already failing.
            if (!(_isOncePerOutagePath(url.path()) && _inOutage(m_baseUrl + url.path())))
                DiagnosticsManager::instance()->publish(
                        DiagnosticsLevel::Warning, DiagnosticsCategory::Backend,
                        QStringLiteral("BackendClient"),
                        QStringLiteral(
                                "Backend GET for %1 returned 401. Retrying with refreshed token.")
                                .arg(url.path()),
                        {}, this->m_sessionId);
            continue;
        }

        if (attemptError.isEmpty()) {
            if (token != this->m_token) {
                const_cast<BackendClient *>(this)->m_token = token;
            }
            if (_isOncePerOutagePath(url.path()))
                _outageOver(m_baseUrl + url.path());
            if (!_isDiagnosticsQuietPath(url.path())) {
                DiagnosticsManager::instance()->publish(
                        DiagnosticsLevel::Info, DiagnosticsCategory::Backend,
                        QStringLiteral("BackendClient"),
                        QStringLiteral("GET %1 → %2 (%3 ms, %4 bytes)")
                                .arg(url.path())
                                .arg(status > 0 ? status : 200)
                                .arg(perfTimer.elapsed())
                                .arg(data.size()),
                        _datasetIdForRequest(QJsonDocument(), url), this->m_sessionId,
                        BackendClient::operationTagForPath(url.path()));
            }
            return data;
        }

        finalData = data;
        lastError = attemptError;
        lastStatus = status;
        break;
    }

    error = lastError;
    // A polled route's repeated failure is not news: the first one was logged.
    if (!error.isEmpty() && _isOncePerOutagePath(url.path())
        && !_firstFailureOfOutage(m_baseUrl + url.path()))
        return finalData;
    if (!error.isEmpty() && _isOncePerOutagePath(url.path()))
        qWarning().noquote() << "Backend GET failed for" << url.path()
                             << "(reported once until it answers again)";
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

QByteArray BackendClient::performPostRetryingBackpressure(const QUrl &url,
                                                          const QJsonDocument &body,
                                                          QString &error,
                                                          QMap<QString, QString> *responseHeaders,
                                                          int attempts, int firstDelayMs) const
{
    int delay = std::max(50, firstDelayMs);
    for (int attempt = 1;; ++attempt) {
        int status = 0;
        error.clear();
        const QByteArray data = performPost(url, body, error, responseHeaders, &status,
                                            /*announceStart=*/attempt == 1);
        if (status != 429 || visivo::TaskQuitDrain::instance().quitting())
            return data;   // success, a failure that retrying cannot fix, or quitting
        if (attempt >= std::max(1, attempts)) {
            // Out of attempts: say what happened in words, not as the server's
            // JSON body — this is what the user reads.
            error = QStringLiteral(
                            "The backend is busy: every compute slot for this session is in "
                            "use, and %1 was still refused after %2 attempts. Wait for the "
                            "running computation to finish and try again.")
                            .arg(url.path())
                            .arg(attempt);
            qWarning().noquote() << QStringLiteral("[backpressure] %1 gave up after %2 attempts")
                                            .arg(url.path())
                                            .arg(attempt);
            return data;
        }
        qWarning().noquote()
                << QStringLiteral("[backpressure] %1 → 429, retry %2/%3 in %4 ms")
                           .arg(url.path())
                           .arg(attempt + 1)
                           .arg(attempts)
                           .arg(delay);
        QThread::msleep(static_cast<unsigned long>(delay));
        delay = std::min(delay * 2, 4000);
    }
}

QByteArray BackendClient::performPost(const QUrl &url, const QJsonDocument &body,
                                      QString &error,
                                      QMap<QString, QString> *responseHeaders,
                                      int *statusOut, bool announceStart) const
{
    // Counted while it runs (RAII), so the status bar can say "N running" for a
    // synchronous compute — and announced when it STARTS, not only when it ends:
    // the log used to be silent for the whole of a multi-minute job (user report).
    const HeavyRequestScope heavyScope(url.path());
    if (heavyScope.counted && announceStart) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Backend,
                QStringLiteral("BackendClient"),
                QStringLiteral("%1 %2 started - this can take minutes on a large cube")
                        .arg(QStringLiteral("POST"), url.path()),
                _datasetIdForRequest(body, url),
                this->m_sessionId, BackendClient::operationTagForPath(url.path()));
    }

    if (statusOut)
        *statusOut = 0;
    // R3 consistency: when the backend never answered the mint we have no
    // session, and sending state-bearing traffic under the fallback id would let
    // a later successful mint split this client's datasets across two sessions.
    // Such a request would almost certainly fail anyway (the backend is
    // unreachable), so refuse it here and let the caller retry once minting
    // works. Readiness polling stays exempt so callers can wait for the backend.
    if (!m_baseUrl.isEmpty() && !_isSessionExemptPath(url.path())
        && ensureSessionMinted() == SessionMint::Deferred) {
        error = QStringLiteral("Backend session not established yet: %1 is unreachable.")
                        .arg(m_baseUrl);
        return {};
    }
    QElapsedTimer perfTimer;
    perfTimer.start();
    QNetworkAccessManager *nam = getThreadLocalNam();
    const QByteArray bodyBytes = body.toJson(QJsonDocument::Compact);
    // The 401 retry with the local token file: for the local backend only.
    const QString fallbackToken =
            isLocalBackendUrl(m_baseUrl) ? BackendClient::readTokenFile().trimmed() : QString();
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
        const bool wantHeaders = (responseHeaders != nullptr);
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
                    QStringLiteral("BackendClient"), attemptError,
                    _datasetIdForRequest(body, url), this->m_sessionId,
                    BackendClient::operationTagForPath(url.path()));
            reply->abort();
            loop.quit();
        });
        timeoutTimer.start(static_cast<int>(this->requestTimeoutFor(url).count()));
        QTimer quitWatch;
        bool abandoned = false;
        abandonWhenQuitting(quitWatch, loop, reply, abandoned);
        // Quitting already: dropped before this loop sends it, a task create
        // included. Only a create already out when the quit came keeps its
        // grace (the watch).
        if (visivo::TaskQuitDrain::instance().quitting())
            abandoned = true;
        else
            loop.exec();
        quitWatch.stop();
        timeoutTimer.stop();
        // On the application thread, exit() ends this loop at once, before the
        // watch has fired: an unanswered reply is then no empty success (and
        // no token is adopted from it).
        if (visivo::TaskQuitDrain::instance().quitting() && !reply->isFinished())
            abandoned = true;
        if (abandoned) {
            attemptError = QStringLiteral("Request abandoned: the application is quitting.");
            if (!reply->isFinished())
                reply->abort();
        }

        const QByteArray data = reply->readAll();
        // Capture response headers (lower-cased keys) before the reply is deleted, so a caller
        // that asked for them (line-width binary path reading X-*-Unit) can read them on success.
        QMap<QString, QString> capturedHeaders;
        if (wantHeaders) {
            const auto pairs = reply->rawHeaderPairs();
            for (const auto &hp : pairs)
                capturedHeaders.insert(QString::fromLatin1(hp.first).toLower(),
                                       QString::fromLatin1(hp.second));
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusOut)
            *statusOut = status;   // 0 = transport failure
        if (attemptError.isEmpty() && reply->error() != QNetworkReply::NoError) {
            attemptError = reply->errorString();
        } else if (attemptError.isEmpty() && status >= 400) {
            attemptError = QString::fromUtf8(data);
        }
        reply->deleteLater();

        // No second token while quitting: the exit waits for this worker.
        if (status == 401 && attempt + 1 < tokenCandidates.size()
            && !visivo::TaskQuitDrain::instance().quitting()) {
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
                        _datasetIdForRequest(body, url), this->m_sessionId,
                        BackendClient::operationTagForPath(url.path()));
            }
            if (responseHeaders)
                *responseHeaders = capturedHeaders;
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




// ── Cosmology (issue #29) ─────────────────────────────────────────────────────
// The parsers and the request shapes live here, not beside the requests in
// BackendClient_Discovery.cpp / _Polarisation.cpp, because this file is the one
// the unit-test binary links: the field names the routes use are pinned there.

namespace {
/// A number the answer may leave out: NaN, never 0, when it is absent or null.
double optionalNumber(const QJsonObject &object, const QString &key)
{
    const QJsonValue v = object.value(key);
    return v.isDouble() ? v.toDouble() : std::numeric_limits<double>::quiet_NaN();
}
}   // namespace

QString BackendClient::cosmologyDistancesQuery(double z, const BackendCosmologyParams *custom)
{
    QUrlQuery query;
    // 17 significant digits: the redshift and parameters go out as typed, not
    // rounded to the six figures QString::number gives by default.
    query.addQueryItem(QStringLiteral("z"), QString::number(z, 'g', 17));
    if (custom) {
        query.addQueryItem(QStringLiteral("H0"), QString::number(custom->h0, 'g', 17));
        query.addQueryItem(QStringLiteral("Om0"), QString::number(custom->om0, 'g', 17));
        query.addQueryItem(QStringLiteral("Ode0"), QString::number(custom->ode0, 'g', 17));
    }
    return query.toString(QUrl::FullyEncoded);
}

BackendCosmologyDistancesResult BackendClient::parseCosmologyDistancesObject(
        const QJsonObject &object)
{
    BackendCosmologyDistancesResult r;
    r.valid = object.value(QStringLiteral("valid")).toBool();
    r.error = object.value(QStringLiteral("error")).toString();
    // A refusal (HTTP 422) carries FastAPI's `detail`, not `error`.
    if (!r.valid && r.error.isEmpty())
        r.error = object.value(QStringLiteral("detail")).toString();
    r.z = optionalNumber(object, QStringLiteral("z"));
    r.comovingMpc = optionalNumber(object, QStringLiteral("comoving_Mpc"));
    r.luminosityMpc = optionalNumber(object, QStringLiteral("luminosity_Mpc"));
    r.angularDiameterMpc = optionalNumber(object, QStringLiteral("angular_diameter_Mpc"));
    r.lookbackGyr = optionalNumber(object, QStringLiteral("lookback_time_Gyr"));
    r.ageGyr = optionalNumber(object, QStringLiteral("age_Gyr"));
    return r;
}

BackendCosmologyDistanceResult BackendClient::parseCosmologyDistanceObject(
        const QJsonObject &object)
{
    BackendCosmologyDistanceResult r;
    r.valid = object.value(QStringLiteral("valid")).toBool();
    r.error = object.value(QStringLiteral("error")).toString();
    r.model = object.value(QStringLiteral("model")).toString();
    r.z = optionalNumber(object, QStringLiteral("z"));
    r.comovingMpc = optionalNumber(object, QStringLiteral("comoving_Mpc"));
    r.luminosityMpc = optionalNumber(object, QStringLiteral("luminosity_Mpc"));
    // Proper transverse kpc per arcsecond; 0 at z = 0, where D_A is 0. The
    // backend names it scale_kpc_per_arcsec since #129; a backend from before
    // only sends the old, deprecated proper_motion_kpc_per_arcsec.
    r.kpcPerArcsec = optionalNumber(object, QStringLiteral("scale_kpc_per_arcsec"));
    if (!std::isfinite(r.kpcPerArcsec))
        r.kpcPerArcsec = optionalNumber(object, QStringLiteral("proper_motion_kpc_per_arcsec"));
    return r;
}

BackendAngularPowerResult BackendClient::parseAngularPowerObject(const QJsonObject &obj)
{
    BackendAngularPowerResult result;
    result.valid      = obj.value(QStringLiteral("valid")).toBool();
    result.error      = obj.value(QStringLiteral("error")).toString();
    result.channel    = obj.value(QStringLiteral("channel")).toInt();
    result.width      = obj.value(QStringLiteral("width")).toInt();
    result.height     = obj.value(QStringLiteral("height")).toInt();
    result.nBins      = obj.value(QStringLiteral("n_bins")).toInt();
    result.kUnit      = obj.value(QStringLiteral("k_unit")).toString();
    result.powerUnit  = obj.value(QStringLiteral("power_unit")).toString();
    result.note       = obj.value(QStringLiteral("note")).toString();
    result.calibrated = obj.value(QStringLiteral("calibrated")).toBool(false);
    result.ellIsAngular = obj.value(QStringLiteral("ell_is_angular")).toBool(false);
    result.pixelScaleArcsec = optionalNumber(obj, QStringLiteral("pixel_scale_arcsec"));
    result.modeCoupling = optionalNumber(obj, QStringLiteral("mode_coupling"));
    for (const QJsonValue &v : obj.value(QStringLiteral("k_bins")).toArray())
        result.kBins.append(v.toDouble());
    // `cl` is the current name, `power` its legacy alias: read whichever came.
    const QJsonArray cl = obj.contains(QStringLiteral("cl"))
            ? obj.value(QStringLiteral("cl")).toArray()
            : obj.value(QStringLiteral("power")).toArray();
    for (const QJsonValue &v : cl)
        result.power.append(v.toDouble());
    for (const QJsonValue &v : obj.value(QStringLiteral("ell")).toArray())
        result.ell.append(v.toDouble());
    // A bin with too few modes has a null error: NaN, so it is drawn without
    // a bar rather than with a zero-length one that claims precision.
    for (const QJsonValue &v : obj.value(QStringLiteral("cl_err")).toArray())
        result.clErr.append(v.isDouble() ? v.toDouble()
                                         : std::numeric_limits<double>::quiet_NaN());
    for (const QJsonValue &v : obj.value(QStringLiteral("n_modes")).toArray())
        result.nModes.append(v.toInt());
    // An older backend sent no ell: the wavenumber is the only axis it had.
    if (result.ell.isEmpty())
        result.ell = result.kBins;
    return result;
}

QJsonObject BackendClient::skeletonRequestBody(const BackendSkeletonRequest &request)
{
    QJsonObject body;
    body.insert(QStringLiteral("channel"), request.channel);
    body.insert(QStringLiteral("mode"), request.mode);
    body.insert(QStringLiteral("threshold"),
                request.thresholdAuto ? QJsonValue(QJsonValue::Null)
                                      : QJsonValue(request.threshold));
    body.insert(QStringLiteral("smooth_sigma_pix"), request.smoothSigmaPix);
    body.insert(QStringLiteral("persistence_threshold"),
                request.prune ? QJsonValue(request.persistenceThreshold)
                              : QJsonValue(QJsonValue::Null));
    return body;
}

BackendSkeletonResult BackendClient::parseSkeletonObject(const QJsonObject &obj)
{
    BackendSkeletonResult result;
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    if (!result.valid && result.error.isEmpty())
        result.error = obj.value(QStringLiteral("detail")).toString();
    result.install = obj.value(QStringLiteral("install")).toString();
    result.feature = obj.value(QStringLiteral("feature")).toString();
    result.channel = obj.value(QStringLiteral("channel")).toInt();
    result.width = obj.value(QStringLiteral("width")).toInt();
    result.height = obj.value(QStringLiteral("height")).toInt();
    result.threshold = obj.value(QStringLiteral("threshold")).toDouble();
    result.mode = obj.value(QStringLiteral("mode")).toString();
    result.smoothSigmaPix = obj.value(QStringLiteral("smooth_sigma_pix")).toDouble();
    result.nSkeletonPixels = obj.value(QStringLiteral("n_skeleton_pixels")).toInt();
    result.nEndpoints = static_cast<int>(obj.value(QStringLiteral("endpoints")).toArray().size());
    result.rangeMin = static_cast<float>(obj.value(QStringLiteral("range_min")).toDouble());
    result.rangeMax = static_cast<float>(obj.value(QStringLiteral("range_max")).toDouble());
    if (!result.valid)
        return result;
    result.data = QByteArray::fromBase64(
            obj.value(QStringLiteral("data_base64")).toString().toLatin1());
    const qsizetype expected = static_cast<qsizetype>(result.width) * result.height
            * static_cast<qsizetype>(sizeof(float));
    if (result.width <= 0 || result.height <= 0 || result.data.size() != expected) {
        result.valid = false;
        result.error = QStringLiteral("The skeleton mask is %1 bytes for a %2 × %3 map.")
                               .arg(result.data.size())
                               .arg(result.width)
                               .arg(result.height);
        result.data.clear();
    }
    return result;
}
