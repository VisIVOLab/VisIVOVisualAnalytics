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

// ── BackendClient: Products domain (extracted verbatim) ──

namespace {
static BackendProductFitsResult parseProductFits(const QByteArray &payload,
                                                   const QString &error)
{
    BackendProductFitsResult r;
    if (!error.isEmpty()) { r.error = error; return r; }
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    r.valid             = o.value(QStringLiteral("valid")).toBool();
    r.error             = o.value(QStringLiteral("error")).toString();
    r.outputPath        = o.value(QStringLiteral("output_path")).toString();
    r.workspaceFilename = o.value(QStringLiteral("workspace_filename")).toString();
    r.width             = o.value(QStringLiteral("width")).toInt();
    r.height            = o.value(QStringLiteral("height")).toInt();
    r.rangeMin          = o.value(QStringLiteral("range_min")).toDouble();
    r.rangeMax          = o.value(QStringLiteral("range_max")).toDouble();
    return r;
}
}  // namespace

BackendProductFitsResult BackendClient::mapMath(const QString &datasetId, const QString &op,
                                                  const QString &datasetIdB,
                                                  double freqA, double freqB) const
{
    QJsonObject body{
        { QStringLiteral("dataset_id"), datasetId },
        { QStringLiteral("op"),         op },
    };
    if (!datasetIdB.isEmpty()) body[QStringLiteral("dataset_id_b")] = datasetIdB;
    if (freqA > 0.0)           body[QStringLiteral("freq_a")] = freqA;
    if (freqB > 0.0)           body[QStringLiteral("freq_b")] = freqB;
    QString error;
    const QByteArray payload =
            performPostRetryingBackpressure(QUrl(m_baseUrl + QStringLiteral("/v1/products/map_math")),
                        QJsonDocument(body), error);
    return parseProductFits(payload, error);
}

BackendProductFitsResult BackendClient::mosaic(const QStringList &datasetIds,
                                                 const QString &weightMode) const
{
    QJsonArray ids;
    for (const QString &id : datasetIds) ids.append(id);
    QJsonObject body{
        { QStringLiteral("dataset_ids"), ids },
        { QStringLiteral("weight_mode"), weightMode },
    };
    QString error;
    const QByteArray payload =
            performPostRetryingBackpressure(QUrl(m_baseUrl + QStringLiteral("/v1/products/mosaic")),
                        QJsonDocument(body), error);
    return parseProductFits(payload, error);
}

BackendPublicationFigureResult BackendClient::publicationFigure(
        const QString &datasetId, const QString &stretch, const QString &title,
        bool addGrid, bool addColorbar, const QString &cmap, int dpi) const
{
    QJsonObject body{
        { QStringLiteral("dataset_id"),   datasetId },
        { QStringLiteral("stretch"),      stretch },
        { QStringLiteral("title"),        title },
        { QStringLiteral("add_grid"),     addGrid },
        { QStringLiteral("add_colorbar"), addColorbar },
        { QStringLiteral("cmap"),         cmap },
        { QStringLiteral("dpi"),          dpi },
    };
    QString error;
    const QByteArray payload =
            performPostRetryingBackpressure(QUrl(m_baseUrl + QStringLiteral("/v1/products/publication_figure")),
                        QJsonDocument(body), error);
    BackendPublicationFigureResult r;
    if (!error.isEmpty()) { r.error = error; return r; }
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    r.valid      = o.value(QStringLiteral("valid")).toBool();
    r.error      = o.value(QStringLiteral("error")).toString();
    r.outputPath = o.value(QStringLiteral("output_path")).toString();
    r.width      = o.value(QStringLiteral("width")).toInt();
    r.height     = o.value(QStringLiteral("height")).toInt();
    r.hasWcs     = o.value(QStringLiteral("has_wcs")).toBool();
    return r;
}

BackendImageQualityResult BackendClient::imageQuality(const QString &datasetId) const
{
    QJsonObject body{ { QStringLiteral("dataset_id"), datasetId } };
    QString error;
    const QByteArray payload =
            performPostRetryingBackpressure(QUrl(m_baseUrl + QStringLiteral("/v1/products/image_quality")),
                        QJsonDocument(body), error);
    BackendImageQualityResult r;
    if (!error.isEmpty()) { r.error = error; return r; }
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    r.valid   = o.value(QStringLiteral("valid")).toBool();
    r.error   = o.value(QStringLiteral("error")).toString();
    r.metrics = o.value(QStringLiteral("metrics")).toObject().toVariantMap();
    return r;
}

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
    const QJsonValue dpa = obj.value(QStringLiteral("deconvolved_pa_deg"));
    result.deconvolvedPaDeg = dpa.isNull() ? std::numeric_limits<double>::quiet_NaN()
                                           : dpa.toDouble();
    return result;
}

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
