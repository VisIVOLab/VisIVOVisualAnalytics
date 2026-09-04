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

// ── BackendClient: Discovery domain (extracted verbatim) ──

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
    // The backend sends JSON null for a redshift it could not use (negative or
    // non-finite). toDouble() would turn that into 0.0 — a real distance, and one
    // the catalogue viewer already reads as "no override" — so it becomes NaN and
    // the caller can tell the three cases apart.
    for (const QJsonValue &v : obj.value(QStringLiteral("distances_Mpc")).toArray())
        result.distancesMpc.append(v.isDouble() ? v.toDouble()
                                                : std::numeric_limits<double>::quiet_NaN());
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

BackendSodaCutoutResult BackendClient::sodaCutout(const QString &obsId,
                                                    const QString &accessUrl,
                                                    const QString &pos,
                                                    const QString &band,
                                                    const QString &time) const
{
    BackendSodaCutoutResult result;
    QJsonObject bodyObj{
        { QStringLiteral("obs_id"),            obsId },
        { QStringLiteral("access_url"),        accessUrl },
        // Keep the cutout on the backend and open it in place — the desktop
        // never downloads the result (compute next to data).
        { QStringLiteral("save_to_workspace"), true },
    };
    if (!pos.isEmpty())  bodyObj[QStringLiteral("pos")]  = pos;
    if (!band.isEmpty()) bodyObj[QStringLiteral("band")] = band;
    if (!time.isEmpty()) bodyObj[QStringLiteral("time")] = time;

    QString error;
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/datasets/cutout")),
                        QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid   = obj.value(QStringLiteral("valid")).toBool();
    result.error   = obj.value(QStringLiteral("error")).toString();
    result.path    = obj.value(QStringLiteral("path")).toString();
    result.applied = obj.value(QStringLiteral("applied")).toString();
    return result;
}
