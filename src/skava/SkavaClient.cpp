#include "SkavaClient.h"
#include "VoTable.h"

#include <QDebug>
#include <QEventLoop>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThreadStorage>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace {

/// Per-thread QNetworkAccessManager. ``QNetworkAccessManager`` is not
/// thread-safe and must live in the thread that calls ``get()``; the
/// VisIVO backend client uses the same trick so we follow suit.
static QNetworkAccessManager *threadLocalNam()
{
    static QThreadStorage<QNetworkAccessManager *> storage;
    if (!storage.hasLocalData()) {
        storage.setLocalData(new QNetworkAccessManager());
    }
    return storage.localData();
}

/// Format a double for inclusion in URL query parameters. ``QString::number``
/// with the default precision keeps trailing precision useful for floats.
static QString fmtParam(double v)
{
    return QString::number(v, 'g', 12);
}

static QString fmtIntParam(int v)
{
    return QString::number(v);
}

static QString getStringField(const QJsonObject &obj, const QString &key)
{
    return obj.value(key).toString();
}

static double getDoubleField(const QJsonObject &obj, const QString &key,
                              double fallback = std::numeric_limits<double>::quiet_NaN())
{
    if (!obj.contains(key) || obj.value(key).isNull()) return fallback;
    return obj.value(key).toDouble(fallback);
}

}  // namespace

// ── Construction ────────────────────────────────────────────────────────────

SkavaClient::SkavaClient(const QString &baseUrl, const QString &bearerToken)
    : m_baseUrl(baseUrl),
      m_bearerToken(bearerToken)
{
    while (m_baseUrl.endsWith(u'/')) {
        m_baseUrl.chop(1);
    }
}

SkavaClient::~SkavaClient() = default;

void SkavaClient::setBaseUrl(const QString &url)
{
    m_baseUrl = url;
    while (m_baseUrl.endsWith(u'/')) {
        m_baseUrl.chop(1);
    }
}

void SkavaClient::setBearerToken(const QString &token)
{
    m_bearerToken = token;
}

namespace {
/// How long a single SKAVA request may take before it is abandoned. Generous
/// enough for a real ObsCore query over a slow link, short enough that an
/// unreachable node is reported rather than waited on.
constexpr int kRequestTimeoutMs  = 30'000;
/// Inactivity timeout: a transfer that stalls mid-stream.
constexpr int kTransferTimeoutMs = 15'000;
/// How often the blocking request checks whether the user pressed Cancel.
constexpr int kCancelPollMs      = 200;
}   // namespace

// ── HTTP helper ─────────────────────────────────────────────────────────────

void SkavaClient::requestCancel()
{
    m_cancelRequested.store(true);
}

void SkavaClient::clearCancel()
{
    m_cancelRequested.store(false);
}

void SkavaClient::stampOrigin(SkavaDatalinkResponse &r) const
{
    r.catalogueUrl = !m_baseUrl.isEmpty() ? m_baseUrl : (m_protocol == Protocol::Vo ? m_datalinkUrl : QString());
    r.protocol = m_protocol == Protocol::Vo ? u"vo"_s : u"skava"_s;
    r.tapUrl = m_protocol == Protocol::Vo ? m_tapUrl : QString();
    r.datalinkUrl = m_protocol == Protocol::Vo ? m_datalinkUrl : QString();
}

QByteArray SkavaClient::performGet(const QUrl &url, QString &error, const QByteArray &accept,
                                   SkavaAuthChallenge *challenge, bool anonymous) const
{
    QNetworkRequest req(url);
    req.setRawHeader("Accept", accept);
    if (!m_bearerToken.isEmpty() && !anonymous) {
        req.setRawHeader("Authorization",
                          QByteArray("Bearer ") + m_bearerToken.toUtf8());
    }
    // Two deadlines, because they fail differently and both were missing.
    //
    // setTransferTimeout covers a connection that goes quiet mid-transfer.
    // The timer covers the rest: a host that accepts the connection and never
    // answers, a proxy that holds it open, a VPN that dropped. Without either,
    // this QEventLoop waits forever — the worker thread never returns, the
    // watcher never fires, and the panel stays "Searching…" with the wait
    // cursor up for the rest of the session. That is exactly what a user saw
    // against an unreachable SKAVA node.
    req.setTransferTimeout(kTransferTimeoutMs);

    auto *nam = threadLocalNam();
    QNetworkReply *reply = nam->get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished,
                     &loop, &QEventLoop::quit);
    bool timedOut = false;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();          // → finished() → loop.quit()
    });
    deadline.start(kRequestTimeoutMs);

    // Cancellation is polled rather than signalled: requestCancel() runs on the
    // UI thread, and a QNetworkReply may only be touched from the thread that
    // created it. The poll timer lives in this nested event loop, so the abort
    // happens on the right thread, at the cost of up to one poll of latency.
    bool cancelled = false;
    QTimer cancelPoll;
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
        if (!m_cancelRequested.load())
            return;
        cancelled = true;
        reply->abort();          // → finished() → loop.quit()
    });
    cancelPoll.start(kCancelPollMs);

    loop.exec();
    deadline.stop();
    cancelPoll.stop();

    if (cancelled || m_cancelRequested.load()) {
        error = u"Request cancelled."_s;
        reply->deleteLater();
        return {};
    }
    if (timedOut) {
        error = u"No answer from %1 after %2 s. The node may be unreachable from "
                u"this network (some SKAVA deployments need a VPN), or down."_s
                        .arg(url.host())
                        .arg(kRequestTimeoutMs / 1000);
        reply->deleteLater();
        return {};
    }
    if (reply->error() != QNetworkReply::NoError) {
        // SKAVA explains a 4xx/5xx in its error envelope (dataset_not_found,
        // no_available_nodes, …); Qt's errorString only says "Not Found".
        const QString detail = parseErrorEnvelope(reply->readAll());
        // A 401 says what token it wants: the caller can sign in and retry.
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        error = detail.isEmpty() ? reply->errorString() : explainHttpError(status, detail);
        if (status == 401) {
            const QString header = QString::fromUtf8(reply->rawHeader("WWW-Authenticate"));
            if (challenge) {
                *challenge = parseAuthChallenge(header);
                challenge->required = true;
            }
            error = m_bearerToken.isEmpty()
                    ? u"%1 requires a sign-in (%2)."_s.arg(url.host(), error)
                    : u"%1 refused the token (%2)."_s.arg(url.host(), error);
        }
        reply->deleteLater();
        return {};
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    return body;
}

// ── Search ──────────────────────────────────────────────────────────────────

void SkavaClient::setTapUrl(const QString &url)
{
    m_tapUrl = url.trimmed();
    while (m_tapUrl.endsWith(u'/'))
        m_tapUrl.chop(1);
}

void SkavaClient::setDatalinkUrl(const QString &url)
{
    m_datalinkUrl = url.trimmed();
}

QString SkavaClient::tapUrl() const
{
    return m_tapUrl.isEmpty() ? m_baseUrl + u"/tap"_s : m_tapUrl;
}

SkavaDiscoveryResponse SkavaClient::searchDiscovery(
        const SkavaSearchFilter &filter) const
{
    if (m_protocol == Protocol::Vo)
        return searchTap(filter);
    SkavaDiscoveryResponse result;
    if (m_baseUrl.isEmpty()) {
        result.error = u"SKAVA base URL is not configured."_s;
        return result;
    }

    QUrl url(m_baseUrl + u"/discovery/search"_s);
    QUrlQuery q;

    // Per discovery_queries.md POS is "CIRCLE ra dec radius" — values
    // separated by spaces, the whole string is the value of the POS param.
    if (std::isfinite(filter.posRa) && std::isfinite(filter.posDec)
        && std::isfinite(filter.posRadius)) {
        const QString posStr = u"CIRCLE %1 %2 %3"_s
                .arg(fmtParam(filter.posRa))
                .arg(fmtParam(filter.posDec))
                .arg(fmtParam(filter.posRadius));
        q.addQueryItem(u"POS"_s, posStr);
    }
    // BAND values are in METRES (wavelengths) per D3.1 §6.3.
    if (std::isfinite(filter.bandMinMetres) && std::isfinite(filter.bandMaxMetres)) {
        q.addQueryItem(u"BAND"_s,
                       u"%1 %2"_s.arg(fmtParam(filter.bandMinMetres))
                                  .arg(fmtParam(filter.bandMaxMetres)));
    }
    if (std::isfinite(filter.timeMinMjd) && std::isfinite(filter.timeMaxMjd)) {
        q.addQueryItem(u"TIME"_s,
                       u"%1 %2"_s.arg(fmtParam(filter.timeMinMjd))
                                  .arg(fmtParam(filter.timeMaxMjd)));
    }
    if (!filter.collection.isEmpty())       q.addQueryItem(u"COLLECTION"_s, filter.collection);
    if (!filter.dataproductType.isEmpty())  q.addQueryItem(u"DPTYPE"_s,     filter.dataproductType);
    if (!filter.obsId.isEmpty())            q.addQueryItem(u"obs_id"_s,     filter.obsId);
    if (!filter.spatialCell.isEmpty())      q.addQueryItem(u"SPATIAL_CELL"_s, filter.spatialCell);
    q.addQueryItem(u"limit"_s,  fmtIntParam(filter.limit));
    q.addQueryItem(u"offset"_s, fmtIntParam(filter.offset));
    url.setQuery(q);

    QString error;
    const QByteArray raw = performGet(url, error, "application/json", &result.authChallenge);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        result.error = u"Unexpected SKAVA response (not a JSON object)."_s;
        return result;
    }
    return parseDiscoveryResponse(doc.object());
}

// ── DataLink ────────────────────────────────────────────────────────────────

SkavaDatalinkResponse SkavaClient::getDatalink(const QString &obsId) const
{
    if (m_protocol == Protocol::Vo)
        return getDatalinkVo(obsId);
    SkavaDatalinkResponse result;
    if (m_baseUrl.isEmpty()) {
        result.error = u"SKAVA base URL is not configured."_s;
        return result;
    }
    if (obsId.trimmed().isEmpty()) {
        result.error = u"obs_id is required."_s;
        return result;
    }

    // Percent-encode the obs_id in case it contains '+', spaces, etc.
    const QString encoded = QString::fromUtf8(
            QUrl::toPercentEncoding(obsId.trimmed()));
    QUrl url(m_baseUrl + u"/datalink/"_s + encoded);

    QString error;
    const QByteArray raw = performGet(url, error, "application/json", &result.authChallenge);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        result.error = u"Unexpected SKAVA response (not a JSON object)."_s;
        return result;
    }
    result = parseDatalinkResponse(doc.object());
    stampOrigin(result);
    return result;
}

namespace {

/// RFC 9110 §11.2: a token character.
bool isTokenChar(QChar ch)
{
    static const QString extra = u"!#$%&'*+-.^_`|~"_s;
    return ch.isLetterOrNumber() && ch.unicode() < 128 ? true : extra.contains(ch);
}

/// One item of a WWW-Authenticate header, read at ``pos``: a scheme (a
/// token followed by whitespace, a comma or the end), a token68
/// ("abc/def==", a token followed by '=' padding and nothing else), or an
/// auth-param ("name=value", the value a token or a quoted-string with
/// backslash escapes). Returns false at the end of the header.
struct AuthItem
{
    enum Kind { Scheme, Token68, Param } kind{ Scheme };
    QString name;
    QString value;
};

bool readAuthItem(const QString &h, qsizetype &pos, AuthItem &item, bool &afterComma)
{
    // Separators, and any stray character that is none of the grammar's.
    // A run is read over the token68 alphabet (the token's plus '/'), the
    // widest of the three items'. ``afterComma`` says a comma was crossed:
    // a bare token right after a scheme without one is that scheme's
    // token68 payload, not another scheme.
    afterComma = false;
    qsizetype start = pos;
    for (;;) {
        while (pos < h.size() && (h[pos] == u',' || h[pos].isSpace())) {
            afterComma = afterComma || h[pos] == u',';
            ++pos;
        }
        if (pos >= h.size())
            return false;
        start = pos;
        while (pos < h.size() && (isTokenChar(h[pos]) || h[pos] == u'/'))
            ++pos;
        if (pos > start)
            break;
        ++pos;              // a stray character: skip it
    }
    const QString run = h.mid(start, pos - start);
    item.value.clear();
    // token68 ("abc/def==", "/x"): a run with a '/' anywhere, or one with
    // '=' padding followed by a separator, is one, whole; a scheme or a
    // parameter name has neither.
    qsizetype pad = pos;
    while (pad < h.size() && h[pad] == u'=')
        ++pad;
    const bool endsItem = pad >= h.size() || h[pad] == u',' || h[pad].isSpace();
    if (run.contains(u'/') || (pad > pos && endsItem)) {
        item.kind = AuthItem::Token68;
        pos = pad;
        return true;
    }
    item.name = run;
    qsizetype eq = pos;
    while (eq < h.size() && h[eq].isSpace())
        ++eq;
    if (eq < h.size() && h[eq] == u'=') {
        item.kind = AuthItem::Param;
        pos = eq + 1;
        while (pos < h.size() && h[pos].isSpace())
            ++pos;
        if (pos < h.size() && h[pos] == u'"') {
            ++pos;
            while (pos < h.size() && h[pos] != u'"') {
                if (h[pos] == u'\\' && pos + 1 < h.size())
                    ++pos;          // quoted-pair: the next character as is
                item.value += h[pos];
                ++pos;
            }
            if (pos < h.size())
                ++pos;              // the closing quote
        } else {
            // A token by the grammar; read leniently to the next separator,
            // for a server that leaves a URL unquoted.
            const qsizetype vstart = pos;
            while (pos < h.size() && h[pos] != u',' && !h[pos].isSpace())
                ++pos;
            item.value = h.mid(vstart, pos - vstart);
        }
        return true;
    }
    item.kind = AuthItem::Scheme;
    return true;
}

}  // namespace

SkavaAuthChallenge SkavaClient::parseAuthChallenge(const QString &wwwAuthenticate)
{
    SkavaAuthChallenge c;
    const QString header = wwwAuthenticate.trimmed();
    if (header.isEmpty())
        return c;
    c.required = true;
    // RFC 9110 §11.6.1: challenges separated by commas, each a scheme with
    // either a token68 or comma-separated auth-params; a scheme's params
    // run until the next scheme.
    QString scheme;
    qsizetype pos = 0;
    AuthItem item;
    bool afterComma = false;
    bool lastWasScheme = false;
    while (readAuthItem(header, pos, item, afterComma)) {
        // "Negotiate abc": a bare token right after a scheme, with no comma
        // between, is its token68 (schemes are comma-separated).
        if (item.kind == AuthItem::Scheme && lastWasScheme && !afterComma)
            item.kind = AuthItem::Token68;
        lastWasScheme = item.kind == AuthItem::Scheme;
        if (item.kind == AuthItem::Scheme) {
            scheme = item.name.toLower();
            if (scheme == "ivoa_bearer"_L1 || c.scheme.isEmpty())
                c.scheme = scheme;
        } else if (item.kind == AuthItem::Param) {
            if (scheme == "ivoa_bearer"_L1 && item.name.compare(u"access_url"_s, Qt::CaseInsensitive) == 0)
                c.wellKnownUrl = item.value;
            else if (item.name.compare(u"realm"_s, Qt::CaseInsensitive) == 0 && c.realm.isEmpty())
                c.realm = item.value;
        }
        // A token68 carries nothing the client reads.
    }
    return c;
}

SkavaClient::OidcEndpoints SkavaClient::parseOidcDocument(const QByteArray &body)
{
    OidcEndpoints e;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        e.error = u"The discovery document is not JSON."_s;
        return e;
    }
    e.authorizationUrl = doc.object().value(u"authorization_endpoint"_s).toString();
    e.tokenUrl = doc.object().value(u"token_endpoint"_s).toString();
    if (!e.valid())
        e.error = u"The discovery document names no authorization and token endpoints."_s;
    return e;
}

SkavaClient::OidcEndpoints SkavaClient::fetchOidcEndpoints(const QString &wellKnownUrl) const
{
    OidcEndpoints e;
    const QUrl url(wellKnownUrl.trimmed());
    if (!url.isValid() || url.scheme().isEmpty()) {
        e.error = u"The IAM's discovery document address is not a URL: %1"_s.arg(wellKnownUrl);
        return e;
    }
    // The document is public: no token goes with the request. Made by this
    // client, so a cancel asked of it ends this too.
    QString error;
    const QByteArray body = performGet(url, error, "application/json", nullptr, /*anonymous=*/true);
    if (!error.isEmpty()) {
        e.error = error;
        return e;
    }
    return parseOidcDocument(body);
}

// ── Parsers (JSON-only, unit-testable) ──────────────────────────────────────

QString SkavaClient::parseErrorEnvelope(const QByteArray &body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return {};
    const QJsonValue err = doc.object().value(u"error"_s);
    if (!err.isObject())
        return {};
    const QJsonObject e = err.toObject();
    const QString code = e.value(u"code"_s).toString();
    const QString message = e.value(u"message"_s).toString();
    if (code.isEmpty() && message.isEmpty())
        return {};
    return code.isEmpty() ? message : (message.isEmpty() ? code : u"%1 - %2"_s.arg(code, message));
}

QString SkavaClient::explainHttpError(int status, const QString &detail)
{
    // The catalogue knows the dataset but reaches none of the nodes holding
    // it: nothing the user can fix here, and nothing a sign-in would change.
    if (status == 503 && detail.startsWith(u"no_available_nodes"_s))
        return u"%1\n\nThe catalogue cannot reach any node that holds this dataset right now: "
               u"the node's backend may be stopped, or the tunnel to it closed. The catalogue "
               u"probes its nodes every few seconds, so a node that comes back is found again."_s
                .arg(detail);
    return detail;
}

QString SkavaDatalinkResponse::accessUrlForNode(const QString &nodeCode) const
{
    if (nodeCode.isEmpty() || nodeCode == bestNode)
        return primaryAccessUrl;
    for (const auto &rep : replicas) {
        if (rep.code == nodeCode)
            return rep.accessEndpoint;
    }
    return {};
}

const SkavaReplica *SkavaDatalinkResponse::replica(const QString &nodeCode) const
{
    for (const auto &rep : replicas) {
        if (rep.code == nodeCode)
            return &rep;
    }
    return nullptr;
}

const SkavaNodeService *SkavaDatalinkResponse::service(const QString &kind, const QString &nodeCode) const
{
    for (const auto &svc : services) {
        if (svc.kind == kind && (nodeCode.isEmpty() || svc.nodeCode == nodeCode))
            return &svc;
    }
    return nullptr;
}

namespace {

/// The SODA the legacy fields name: a node may offer its cutout synchronously
/// and as a job, and the services come best node first, so the first
/// synchronous one is the one to call; an asynchronous one only stands in
/// until a synchronous one is seen.
void pickLegacySoda(SkavaDatalinkResponse &r)
{
    bool haveSync = false;
    for (const auto &svc : r.services) {
        if (svc.kind != "cutout-sync"_L1 && svc.kind != "cutout-async"_L1)
            continue;
        const bool sync = svc.kind == "cutout-sync"_L1;
        if (r.sodaServiceUrl.isEmpty() || (sync && !haveSync)) {
            r.sodaServiceUrl = svc.accessUrl;
            r.sodaStandardId = svc.standardId;
            haveSync = sync;
        }
    }
}

constexpr QLatin1StringView kSodaSync("ivo://ivoa.net/std/SODA#sync-1.0");
constexpr QLatin1StringView kSodaAsync("ivo://ivoa.net/std/SODA#async-1.0");

}  // namespace

QString SkavaClient::nodeServiceKind(const QString &semantics, const QString &standardId,
                                     const QString &operation)
{
    // Only the SODA standard the client speaks; a service of another kind
    // behind a #cutout row is not one to call with SODA parameters. A
    // #cutout without a standardID (older documents) is taken as synchronous.
    const QString id = standardId.trimmed();
    const bool sync = id.compare(kSodaSync, Qt::CaseInsensitive) == 0;
    const bool async = id.compare(kSodaAsync, Qt::CaseInsensitive) == 0;
    if (semantics == "#cutout"_L1) {
        if (async)
            return u"cutout-async"_s;
        return sync || id.isEmpty() ? u"cutout-sync"_s : QString();
    }
    if (semantics != "#proc"_L1 || !async)
        return {};
    const QString op = operation.trimmed().toUpper();
    if (op == "MOMENT"_L1)
        return u"moment"_s;
    if (op == "PV"_L1)
        return u"pv"_s;
    return {};
}

SkavaDatasetMetadata SkavaClient::parseDatasetMetadata(const QJsonObject &obj)
{
    SkavaDatasetMetadata m;
    m.obsId           = getStringField(obj, u"obs_id"_s);
    m.obsCollection   = getStringField(obj, u"obs_collection"_s);
    m.obsTitle        = getStringField(obj, u"obs_title"_s);
    m.dataproductType = getStringField(obj, u"dataproduct_type"_s);
    m.calibLevel      = obj.value(u"calib_level"_s).toInt(0);
    m.targetName      = getStringField(obj, u"target_name"_s);
    m.facilityName    = getStringField(obj, u"facility_name"_s);
    m.instrumentName  = getStringField(obj, u"instrument_name"_s);
    m.sRa             = getDoubleField(obj, u"s_ra"_s);
    m.sDec            = getDoubleField(obj, u"s_dec"_s);
    m.sFov            = getDoubleField(obj, u"s_fov"_s);
    m.tMin            = getDoubleField(obj, u"t_min"_s);
    m.tMax            = getDoubleField(obj, u"t_max"_s);
    m.emMin           = getDoubleField(obj, u"em_min"_s);
    m.emMax           = getDoubleField(obj, u"em_max"_s);
    m.spatialIndexCell = getStringField(obj, u"spatial_index_cell"_s);
    m.doi             = getStringField(obj, u"doi"_s);
    m.pid             = getStringField(obj, u"pid"_s);
    m.citation        = getStringField(obj, u"citation"_s);
    m.license         = getStringField(obj, u"license"_s);
    return m;
}

SkavaReplica SkavaClient::parseReplica(const QJsonObject &obj)
{
    SkavaReplica r;
    r.code            = getStringField(obj, u"code"_s);
    r.name            = getStringField(obj, u"name"_s);
    r.isEnabled       = obj.value(u"is_enabled"_s).toBool(true);
    r.isAvailable     = obj.value(u"is_available"_s).toBool(true);
    r.latencyScore    = getDoubleField(obj, u"latency_score"_s);
    r.loadScore       = getDoubleField(obj, u"load_score"_s);
    r.capabilityScore = getDoubleField(obj, u"capability_score"_s);
    r.accessEndpoint  = getStringField(obj, u"access_endpoint"_s);
    r.rankReason      = getStringField(obj, u"rank_reason"_s);
    r.score           = getDoubleField(obj, u"score"_s);
    r.telemetryAgeS   = getDoubleField(obj, u"telemetry_age_s"_s);
    return r;
}

SkavaDiscoveryResult SkavaClient::parseDiscoveryResult(const QJsonObject &obj)
{
    SkavaDiscoveryResult r;
    r.metadata = parseDatasetMetadata(obj.value(u"metadata"_s).toObject());
    for (const auto &repVal : obj.value(u"replicas"_s).toArray()) {
        r.replicas.append(parseReplica(repVal.toObject()));
    }
    r.bestNode  = getStringField(obj, u"best_node"_s);
    r.accessUrl = getStringField(obj, u"access_url"_s);
    return r;
}

SkavaDiscoveryResponse SkavaClient::parseDiscoveryResponse(const QJsonObject &obj)
{
    SkavaDiscoveryResponse r;
    // SKAVA's standard error envelope per discovery_queries.md.
    if (obj.contains(u"error"_s) && obj.value(u"error"_s).isObject()) {
        const QJsonObject errObj = obj.value(u"error"_s).toObject();
        r.error = u"%1 - %2"_s.arg(errObj.value(u"code"_s).toString(),
                                    errObj.value(u"message"_s).toString());
        return r;
    }
    if (!obj.contains(u"results"_s)) {
        r.error = u"Malformed response: missing 'results' array."_s;
        return r;
    }
    for (const auto &item : obj.value(u"results"_s).toArray()) {
        r.results.append(parseDiscoveryResult(item.toObject()));
    }
    r.total    = obj.value(u"total"_s).toInt(r.results.size());
    r.limit    = obj.value(u"limit"_s).toInt(0);
    r.offset   = obj.value(u"offset"_s).toInt(0);
    r.returned = obj.value(u"returned"_s).toInt(r.results.size());
    r.valid = true;
    return r;
}

SkavaDatalinkResponse SkavaClient::parseDatalinkResponse(const QJsonObject &obj)
{
    SkavaDatalinkResponse r;
    if (obj.contains(u"error"_s) && obj.value(u"error"_s).isObject()) {
        const QJsonObject errObj = obj.value(u"error"_s).toObject();
        r.error = u"%1 - %2"_s.arg(errObj.value(u"code"_s).toString(),
                                    errObj.value(u"message"_s).toString());
        return r;
    }
    // The DataLink JSON envelope per docs/access_datalink.md.
    const QJsonObject primary = obj.value(u"primary_access"_s).toObject();
    r.primaryAccessUrl    = getStringField(primary, u"access_url"_s);
    r.primaryAccessFormat = getStringField(primary, u"access_format"_s);
    // Bytes: SKAVA's content_length when it gives one, else ObsCore's
    // access_estsize, which is in kilobytes.
    r.primaryAccessEstSize = primary.contains(u"content_length"_s)
            ? static_cast<qint64>(primary.value(u"content_length"_s).toDouble(0.0))
            : static_cast<qint64>(primary.value(u"access_estsize"_s).toDouble(0.0) * 1024.0);
    const QJsonObject routing = obj.value(u"routing"_s).toObject();
    r.bestNode = getStringField(routing, u"best_node"_s);
    for (const auto &repVal : routing.value(u"replicas"_s).toArray()) {
        r.replicas.append(parseReplica(repVal.toObject()));
    }
    r.policy = getStringField(routing, u"policy"_s);
    for (const auto &exVal : routing.value(u"excluded"_s).toArray()) {
        const QJsonObject ex = exVal.toObject();
        r.excluded.append({ getStringField(ex, u"code"_s), getStringField(ex, u"reason"_s) });
    }
    r.dataset = parseDatasetMetadata(obj.value(u"dataset"_s).toObject());
    // ── Service descriptors ────────────────────────────────────────────
    // visivo-backend entries tell the desktop "compute next to data" is
    // available: the SRC node hosts a VisIVO backend that can open this
    // dataset directly without a download. The node's SODA and product
    // services (node-soda-*, visivo-moment, visivo-pv) say what it can
    // compute in place. Unknown service_type values are ignored — the
    // schema is intentionally extensible per docs/access_datalink.md.
    static const QHash<QString, QString> kNodeServiceKinds{
        { u"node-soda-sync"_s, u"cutout-sync"_s }, { u"node-soda-async"_s, u"cutout-async"_s },
        { u"visivo-moment"_s, u"moment"_s },       { u"visivo-pv"_s, u"pv"_s } };
    for (const auto &sdVal : obj.value(u"service_descriptors"_s).toArray()) {
        const QJsonObject sdObj = sdVal.toObject();
        const QString type = sdObj.value(u"service_type"_s).toString();
        if (!sdObj.value(u"enabled"_s).toBool(true)) continue;
        if (kNodeServiceKinds.contains(type)) {
            SkavaNodeService svc;
            svc.kind = kNodeServiceKinds.value(type);
            svc.id = sdObj.value(u"service_id"_s).toString();
            svc.standardId = sdObj.value(u"standard_id"_s).toString();
            svc.accessUrl = sdObj.value(u"access_url"_s).toString();
            svc.nodeCode = sdObj.value(u"node_code"_s).toString();
            svc.requiresAuth = sdObj.value(u"requires_auth"_s).toBool(false);
            svc.description = sdObj.value(u"description"_s).toString();
            const QJsonObject inputs = sdObj.value(u"input_params"_s).toObject();
            for (auto it = inputs.begin(); it != inputs.end(); ++it)
                svc.inputParams.insert(it.key(), it.value().toString());
            if (!svc.accessUrl.isEmpty())
                r.services.append(svc);
            continue;
        }
        if (type != "visivo-backend"_L1) continue;
        SkavaVisivoBackendDescriptor desc;
        desc.endpoint     = sdObj.value(u"endpoint"_s).toString();
        desc.nodeCode     = sdObj.value(u"node_code"_s).toString();
        desc.requiresAuth = sdObj.value(u"requires_auth"_s).toBool(false);
        for (const auto &k : sdObj.value(u"supports_kinds"_s).toArray()) {
            desc.supportedKinds.append(k.toString());
        }
        if (!desc.endpoint.isEmpty()) {
            r.visivoBackends.append(desc);
        }
    }
    pickLegacySoda(r);
    // Fall back to legacy compatibility fields if structured ones are missing.
    if (r.primaryAccessUrl.isEmpty()) {
        r.primaryAccessUrl = getStringField(obj, u"access_url"_s);
    }
    if (r.bestNode.isEmpty()) {
        r.bestNode = getStringField(obj, u"best_node"_s);
    }
    r.valid = !r.primaryAccessUrl.isEmpty();
    if (!r.valid && r.error.isEmpty()) {
        r.error = u"DataLink response did not contain a primary access URL."_s;
    }
    return r;
}

// ── VO protocol: TAP on ObsCore + DataLink VOTable ─────────────────────────

namespace {

QString adqlString(const QString &v)
{
    QString escaped = v;
    escaped.replace(u"'"_s, u"''"_s);
    return u"'"_s + escaped + u"'"_s;
}

double cellDouble(const VoTable &t, int row, const QString &name)
{
    bool ok = false;
    const double v = t.cell(row, name).trimmed().toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

// What a backend's #auxiliary row points at: its public node status (now), or
// its health document (SKAVA before node telemetry). Either names the backend.
constexpr QLatin1StringView kStatusSuffixes[]{ QLatin1StringView("/v1/node/status"),
                                               QLatin1StringView("/v1/health") };

}  // namespace

QString SkavaClient::datalinkContentType()
{
    return u"application/x-votable+xml;content=datalink"_s;
}

QString SkavaClient::buildObscoreAdql(const SkavaSearchFilter &filter)
{
    QStringList where;
    if (std::isfinite(filter.posRa) && std::isfinite(filter.posDec) && std::isfinite(filter.posRadius)) {
        where << u"1 = CONTAINS(POINT('ICRS', s_ra, s_dec), CIRCLE('ICRS', %1, %2, %3))"_s.arg(
                fmtParam(filter.posRa), fmtParam(filter.posDec), fmtParam(filter.posRadius));
    }
    // Interval overlap: the dataset's range meets the requested one.
    if (std::isfinite(filter.bandMinMetres) && std::isfinite(filter.bandMaxMetres)) {
        where << u"em_min <= %1"_s.arg(fmtParam(std::max(filter.bandMinMetres, filter.bandMaxMetres)))
              << u"em_max >= %1"_s.arg(fmtParam(std::min(filter.bandMinMetres, filter.bandMaxMetres)));
    }
    if (std::isfinite(filter.timeMinMjd) && std::isfinite(filter.timeMaxMjd)) {
        where << u"t_min <= %1"_s.arg(fmtParam(std::max(filter.timeMinMjd, filter.timeMaxMjd)))
              << u"t_max >= %1"_s.arg(fmtParam(std::min(filter.timeMinMjd, filter.timeMaxMjd)));
    }
    if (!filter.collection.isEmpty())
        where << u"obs_collection = "_s + adqlString(filter.collection);
    if (!filter.dataproductType.isEmpty())
        where << u"dataproduct_type = "_s + adqlString(filter.dataproductType);
    if (!filter.obsId.isEmpty())
        where << u"obs_id = "_s + adqlString(filter.obsId);
    // ADQL 2.0 has no OFFSET: ask for limit+offset rows and drop the first ones.
    const int top = std::max(1, filter.limit + std::max(0, filter.offset));
    QString q = u"SELECT TOP %1 * FROM ivoa.obscore"_s.arg(top);
    if (!where.isEmpty())
        q += u" WHERE "_s + where.join(u" AND "_s);
    // Pages are separate queries: without a stable order a service may return the rows in a
    // different order each time, and dropping the first `offset` would skip or repeat products.
    // obs_publisher_did is the ObsCore product identifier (mandatory, unique). Every page,
    // the first included, must use the same order.
    q += u" ORDER BY obs_publisher_did"_s;
    return q;
}

SkavaDiscoveryResponse SkavaClient::parseObscoreVotable(const QByteArray &votable)
{
    SkavaDiscoveryResponse r;
    const VoTable t = VoTable::parse(votable);
    if (!t.valid) {
        r.error = t.error;
        return r;
    }
    if (!t.fields.contains(u"obs_id"_s)) {
        r.error = u"The TAP result has no obs_id column: not an ObsCore table."_s;
        return r;
    }
    for (int i = 0; i < t.rows.size(); ++i) {
        SkavaDiscoveryResult res;
        SkavaDatasetMetadata &m = res.metadata;
        m.obsId           = t.cell(i, u"obs_id"_s);
        m.obsCollection   = t.cell(i, u"obs_collection"_s);
        m.obsTitle        = t.cell(i, u"obs_title"_s);
        m.dataproductType = t.cell(i, u"dataproduct_type"_s);
        m.calibLevel      = t.cell(i, u"calib_level"_s).toInt();
        m.targetName      = t.cell(i, u"target_name"_s);
        m.facilityName    = t.cell(i, u"facility_name"_s);
        m.instrumentName  = t.cell(i, u"instrument_name"_s);
        m.sRa   = cellDouble(t, i, u"s_ra"_s);
        m.sDec  = cellDouble(t, i, u"s_dec"_s);
        m.sFov  = cellDouble(t, i, u"s_fov"_s);
        m.tMin  = cellDouble(t, i, u"t_min"_s);
        m.tMax  = cellDouble(t, i, u"t_max"_s);
        m.emMin = cellDouble(t, i, u"em_min"_s);
        m.emMax = cellDouble(t, i, u"em_max"_s);
        res.accessUrl    = t.cell(i, u"access_url"_s);
        res.accessFormat = t.cell(i, u"access_format"_s);
        r.results.append(res);
    }
    r.total = r.returned = r.results.size();
    r.valid = true;
    return r;
}

SkavaDatalinkResponse SkavaClient::parseDatalinkVotable(const QByteArray &votable, const QString &obsId)
{
    SkavaDatalinkResponse r;
    const VoTable t = VoTable::parse(votable);
    if (!t.valid) {
        r.error = t.error;
        return r;
    }
    QString firstError;
    for (int i = 0; i < t.rows.size(); ++i) {
        const QString sem = t.cell(i, u"semantics"_s);
        const QString url = t.cell(i, u"access_url"_s);
        const QString node = t.cell(i, u"node_id"_s);
        const QString err = t.cell(i, u"error_message"_s);
        if (!err.isEmpty()) {
            if (firstError.isEmpty())
                firstError = err;
            continue;
        }
        if (sem == "#this"_L1 && !url.isEmpty()) {
            // A copy local to its node is linked through the node's SODA (a
            // URL any client can follow); where it lies on the node's storage,
            // which the node's backend opens in place, is local_access_url.
            // An older SKAVA put that file:// location in access_url itself.
            const QString local = t.cell(i, u"local_access_url"_s);
            const QString endpoint = local.isEmpty() ? url : local;
            SkavaReplica rep;
            rep.code = node;
            rep.name = node;
            rep.accessEndpoint = endpoint;
            rep.rankReason = t.cell(i, u"rank_reason"_s);
            rep.score = cellDouble(t, i, u"score"_s);
            if (r.primaryAccessUrl.isEmpty()) {    // rows come best node first
                r.primaryAccessUrl = endpoint;
                r.primaryAccessFormat = t.cell(i, u"content_type"_s);
                r.primaryAccessEstSize = t.cell(i, u"content_length"_s).toLongLong();
                r.bestNode = node;
            }
            r.replicas.append(rep);
        } else if (sem == "#auxiliary"_L1) {
            // Matched on the URL's path: a query string or a trailing slash
            // does not change which backend the row names.
            // The path is handled in its encoded form, so a percent-encoded
            // segment of the backend's own path survives the round trip.
            const QUrl statusUrl(url);
            QString path = statusUrl.path(QUrl::FullyEncoded);
            while (path.size() > 1 && path.endsWith(u'/'))
                path.chop(1);
            for (QLatin1StringView suffix : kStatusSuffixes) {
                if (!path.endsWith(suffix))
                    continue;
                QUrl endpoint = statusUrl;
                endpoint.setPath(path.chopped(suffix.size()), QUrl::StrictMode);
                endpoint.setQuery(QString());
                endpoint.setFragment(QString());
                SkavaVisivoBackendDescriptor desc;
                desc.endpoint = endpoint.toString();
                desc.nodeCode = node;
                // The status document itself is public; "optional" is how
                // SKAVA says the backend's API takes a token.
                const QString auth = t.cell(i, u"link_auth"_s);
                desc.requiresAuth = auth == "true"_L1 || auth == "optional"_L1;
                r.visivoBackends.append(desc);
                break;
            }
        } else if (sem == "#cutout"_L1 || sem == "#proc"_L1) {
            // A node service: its descriptor RESOURCE says what it is (a SODA
            // cutout, or a product by its fixed OPERATION) and, from SKAVA,
            // fixes the dataset's ID on that node (another DataLink may leave
            // ID to the caller: the replica's URL). The rows come best node first.
            const VoTableService *svc = t.service(t.cell(i, u"service_def"_s));
            if (!svc || svc->accessUrl.isEmpty())
                continue;
            const QString kind = nodeServiceKind(sem, svc->standardId, svc->inputValues.value(u"OPERATION"_s));
            if (kind.isEmpty())
                continue;
            SkavaNodeService service;
            service.kind = kind;
            service.id = svc->id;
            service.standardId = svc->standardId;
            service.accessUrl = svc->accessUrl;
            service.nodeCode = node;
            service.requiresAuth = t.cell(i, u"link_auth"_s) == "true"_L1;
            service.inputParams = svc->inputValues;
            service.description = t.cell(i, u"description"_s);
            r.services.append(service);
        }
    }
    pickLegacySoda(r);
    r.dataset.obsId = t.params.value(u"ID"_s, obsId);
    if (r.dataset.obsId.isEmpty() && !t.rows.isEmpty())
        r.dataset.obsId = t.cell(0, u"ID"_s);
    r.valid = !r.primaryAccessUrl.isEmpty();
    if (!r.valid)
        r.error = firstError.isEmpty() ? u"The DataLink document lists no access to the dataset itself."_s
                                       : firstError;
    return r;
}

SkavaDiscoveryResponse SkavaClient::searchTap(const SkavaSearchFilter &filter) const
{
    SkavaDiscoveryResponse result;
    if (m_baseUrl.isEmpty() && m_tapUrl.isEmpty()) {
        result.error = u"The TAP service URL is not configured."_s;
        return result;
    }
    QUrl url(tapUrl() + u"/sync"_s);
    QUrlQuery q;
    q.addQueryItem(u"REQUEST"_s, u"doQuery"_s);
    q.addQueryItem(u"LANG"_s, u"ADQL"_s);
    q.addQueryItem(u"RESPONSEFORMAT"_s, u"votable/td"_s);
    q.addQueryItem(u"QUERY"_s, QString::fromUtf8(QUrl::toPercentEncoding(buildObscoreAdql(filter))));
    url.setQuery(q.query(QUrl::FullyEncoded), QUrl::StrictMode);
    QString error;
    const QByteArray raw = performGet(url, error, "application/x-votable+xml, text/xml;q=0.9, */*;q=0.1",
                                      &result.authChallenge);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    result = parseObscoreVotable(raw);
    if (!result.valid)
        return result;
    // Emulated OFFSET (see buildObscoreAdql).
    const int skip = std::clamp(filter.offset, 0, static_cast<int>(result.results.size()));
    result.results.remove(0, skip);
    result.offset = filter.offset;
    result.limit = filter.limit;
    result.returned = result.results.size();
    return result;
}

namespace {
// application/x-votable+xml;content=datalink — media type and parameter names are
// case-insensitive and the value may be quoted (RFC 2045 §5.1).
bool isDatalinkFormat(const QString &accessFormat)
{
    const QStringList parts = accessFormat.split(u';');
    if (parts.first().trimmed().compare(u"application/x-votable+xml"_s, Qt::CaseInsensitive) != 0)
        return false;
    for (qsizetype i = 1; i < parts.size(); ++i) {
        const QString param = parts[i].trimmed();
        const qsizetype eq = param.indexOf(u'=');
        if (eq < 0 || param.left(eq).trimmed().compare(u"content"_s, Qt::CaseInsensitive) != 0)
            continue;
        QString value = param.mid(eq + 1).trimmed();
        if (value.size() >= 2 && value.startsWith(u'"') && value.endsWith(u'"'))
            value = value.mid(1, value.size() - 2);
        if (value.compare(u"datalink"_s, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}
}  // namespace

SkavaClient::Protocol SkavaClient::protocolFromString(const QString &name)
{
    return name.trimmed().compare(u"vo"_s, Qt::CaseInsensitive) == 0 ? Protocol::Vo : Protocol::SkavaJson;
}

SkavaDatalinkResponse SkavaClient::resolveAccess(const SkavaDiscoveryResult &row) const
{
    const bool viaDatalink = m_protocol != Protocol::Vo || row.accessUrl.isEmpty()
                             || isDatalinkFormat(row.accessFormat);
    if (viaDatalink) {
        SkavaDatalinkResponse r = (m_protocol == Protocol::Vo && !row.accessUrl.isEmpty())
                                          ? fetchDatalinkVo(QUrl(row.accessUrl), row.metadata.obsId)
                                          : getDatalink(row.metadata.obsId);
        // DataLink describes the dataset only by id, collection and type; the selected row has
        // the full ObsCore metadata (position, field, band) for the SODA prefill. Take it from
        // THIS row: obs_id alone may match several products.
        if (r.valid) {
            const QString type = r.dataset.dataproductType;
            r.dataset = row.metadata;
            if (!type.isEmpty())
                r.dataset.dataproductType = type;
        }
        return r;
    }
    // A plain ObsCore row: access_url is the file itself. No DataLink, so no replicas or
    // co-located backends — routing then falls back to downloading it.
    SkavaDatalinkResponse r;
    r.dataset = row.metadata;
    r.primaryAccessUrl = row.accessUrl;
    r.primaryAccessFormat = row.accessFormat;
    r.valid = true;
    stampOrigin(r);
    return r;
}

SkavaDatalinkResponse SkavaClient::getDatalinkVo(const QString &obsId) const
{
    SkavaDatalinkResponse result;
    const QString id = obsId.trimmed();
    if (id.isEmpty()) {
        result.error = u"obs_id is required."_s;
        return result;
    }
    QUrl url;
    if (!m_datalinkUrl.isEmpty()) {
        url = QUrl(m_datalinkUrl);                     // SRCNet DataLink: <url>?ID=<id>
        QUrlQuery q(url);
        // addQueryItem takes ENCODED input: a raw '+' would read back as a space
        // and a literal "%2F" as '/'.
        q.addQueryItem(u"ID"_s, QString::fromUtf8(QUrl::toPercentEncoding(id)));
        url.setQuery(q.query(QUrl::FullyEncoded), QUrl::StrictMode);
    } else if (!m_baseUrl.isEmpty()) {
        url = QUrl(m_baseUrl + u"/datalink/"_s + QString::fromUtf8(QUrl::toPercentEncoding(id)));
        url.setQuery(u"RESPONSEFORMAT=votable"_s);
    } else {
        result.error = u"The DataLink service URL is not configured."_s;
        return result;
    }
    return fetchDatalinkVo(url, id);
}

SkavaDatalinkResponse SkavaClient::fetchDatalinkVo(const QUrl &url, const QString &obsId) const
{
    SkavaDatalinkResponse result;
    QString error;
    const QByteArray raw = performGet(url, error, "application/x-votable+xml, text/xml;q=0.9, */*;q=0.1",
                                      &result.authChallenge);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    result = parseDatalinkVotable(raw, obsId);
    stampOrigin(result);
    return result;
}
