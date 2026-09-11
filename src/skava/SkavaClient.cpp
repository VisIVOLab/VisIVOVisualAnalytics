#include "SkavaClient.h"

#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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

SkavaClient::SkavaClient(const QString &baseUrl,
                          const QString &bearerToken,
                          QObject *parent)
    : QObject(parent),
      m_baseUrl(baseUrl),
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

// ── HTTP helper ─────────────────────────────────────────────────────────────

QByteArray SkavaClient::performGet(const QUrl &url, QString &error) const
{
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/json");
    if (!m_bearerToken.isEmpty()) {
        req.setRawHeader("Authorization",
                          QByteArray("Bearer ") + m_bearerToken.toUtf8());
    }
    auto *nam = threadLocalNam();
    QNetworkReply *reply = nam->get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished,
                     &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        error = reply->errorString();
        reply->deleteLater();
        return {};
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    return body;
}

// ── Search ──────────────────────────────────────────────────────────────────

SkavaDiscoveryResponse SkavaClient::searchDiscovery(
        const SkavaSearchFilter &filter) const
{
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
    const QByteArray raw = performGet(url, error);
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
    const QByteArray raw = performGet(url, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        result.error = u"Unexpected SKAVA response (not a JSON object)."_s;
        return result;
    }
    return parseDatalinkResponse(doc.object());
}

// ── Parsers (JSON-only, unit-testable) ──────────────────────────────────────

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
    r.latencyScore    = getDoubleField(obj, u"latency_score"_s, 0.0);
    r.loadScore       = getDoubleField(obj, u"load_score"_s, 0.0);
    r.capabilityScore = getDoubleField(obj, u"capability_score"_s, 0.0);
    r.accessEndpoint  = getStringField(obj, u"access_endpoint"_s);
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
        r.error = u"%1 -%2"_s.arg(errObj.value(u"code"_s).toString(),
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
        r.error = u"%1 -%2"_s.arg(errObj.value(u"code"_s).toString(),
                                    errObj.value(u"message"_s).toString());
        return r;
    }
    // The DataLink JSON envelope per docs/access_datalink.md.
    const QJsonObject primary = obj.value(u"primary_access"_s).toObject();
    r.primaryAccessUrl    = getStringField(primary, u"access_url"_s);
    r.primaryAccessFormat = getStringField(primary, u"access_format"_s);
    r.primaryAccessEstSize =
            static_cast<qint64>(primary.value(u"access_estsize"_s).toDouble(0.0));
    const QJsonObject routing = obj.value(u"routing"_s).toObject();
    r.bestNode = getStringField(routing, u"best_node"_s);
    for (const auto &repVal : routing.value(u"replicas"_s).toArray()) {
        r.replicas.append(parseReplica(repVal.toObject()));
    }
    r.dataset = parseDatasetMetadata(obj.value(u"dataset"_s).toObject());
    // ── Service descriptors → extract visivo-backend entries ───────────
    // These tell the desktop "compute next to data" is available: the
    // SRC node hosts a VisIVO backend that can open this dataset
    // directly without a download. Unknown service_type values are
    // ignored — the schema is intentionally extensible per
    // docs/access_datalink.md.
    for (const auto &sdVal : obj.value(u"service_descriptors"_s).toArray()) {
        const QJsonObject sdObj = sdVal.toObject();
        const QString type = sdObj.value(u"service_type"_s).toString();
        if (type != "visivo-backend"_L1) continue;
        if (!sdObj.value(u"enabled"_s).toBool(true)) continue;
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
