// BackendClient_Archives.cpp
// Transport for the two archive services that answer with a file: hips2fits
// (a FITS cut out of any HiPS survey) and the VO Simple Cone Search.
//
// Both were client-side in the legacy — hips2fits through QNetworkAccessManager
// with the survey list hard-coded in the dialog, cone search through a bundled
// Python script run with QProcess — and both are backend routes now, so the
// URL validation, the SSRF guard and the saving happen in one place.

#include "BackendClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

BackendHips2FitsOptions BackendClient::hips2fitsOptions() const
{
    BackendHips2FitsOptions result;
    QString error;
    const QByteArray payload =
            performGet(QUrl(m_baseUrl + QStringLiteral("/v1/hips/hips2fits/surveys")), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    const auto toList = [&o](const char *key) {
        QStringList out;
        for (const QJsonValue &v : o.value(QLatin1String(key)).toArray())
            out << v.toString();
        return out;
    };
    result.surveys = toList("surveys");
    result.projections = toList("projections");
    result.stretches = toList("stretches");
    result.endpoint = o.value(QStringLiteral("endpoint")).toString();
    result.valid = !result.surveys.isEmpty() || !result.projections.isEmpty();
    if (!result.valid)
        result.error = QStringLiteral("The backend returned no hips2fits options.");
    return result;
}

BackendHips2FitsResult BackendClient::hips2fits(const QVariantMap &parameters) const
{
    BackendHips2FitsResult result;
    QString error;
    const QJsonDocument body(QJsonObject::fromVariantMap(parameters));
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/hips/hips2fits")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    result.valid             = o.value(QStringLiteral("valid")).toBool(false);
    result.error             = o.value(QStringLiteral("error")).toString();
    result.outputPath        = o.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = o.value(QStringLiteral("workspace_filename")).toString();
    result.bytes             = o.value(QStringLiteral("bytes")).toInteger();
    result.isFits            = o.value(QStringLiteral("is_fits")).toBool(false);
    result.url               = o.value(QStringLiteral("url")).toString();
    return result;
}

QList<BackendConeSearchService> BackendClient::coneSearchServices() const
{
    QList<BackendConeSearchService> services;
    QString error;
    const QByteArray payload = performGet(
            QUrl(m_baseUrl + QStringLiteral("/v1/resolve/cone_search/services")), error);
    if (!error.isEmpty())
        return services;
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    for (const QJsonValue &v : o.value(QStringLiteral("services")).toArray()) {
        const QJsonObject entry = v.toObject();
        services.append({ entry.value(QStringLiteral("name")).toString(),
                          entry.value(QStringLiteral("url")).toString() });
    }
    return services;
}

BackendConeSearchResult BackendClient::coneSearch(const QString &serviceUrl, double ra, double dec,
                                                  double radiusDeg, int verbosity,
                                                  const QString &outputBasename) const
{
    BackendConeSearchResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("url"),             serviceUrl },
            { QStringLiteral("ra"),              ra },
            { QStringLiteral("dec"),             dec },
            { QStringLiteral("radius"),          radiusDeg },
            { QStringLiteral("verbosity"),       verbosity },
            { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/resolve/cone_search")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    result.valid  = o.value(QStringLiteral("valid")).toBool(false);
    result.error  = o.value(QStringLiteral("error")).toString();
    result.url    = o.value(QStringLiteral("url")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("columns")).toArray())
        result.columns << v.toString();
    for (const QJsonValue &rowValue : o.value(QStringLiteral("rows")).toArray()) {
        QStringList row;
        for (const QJsonValue &cell : rowValue.toArray()) {
            // The table shows text; a null is an absent measurement, which is
            // not the same as the number zero and must not print as one.
            if (cell.isNull())
                row << QString();
            else if (cell.isString())
                row << cell.toString();
            else if (cell.isBool())
                row << (cell.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
            else
                row << QString::number(cell.toDouble(), 'g', 10);
        }
        result.rows << row;
    }
    result.totalRows        = o.value(QStringLiteral("total_rows")).toInt();
    result.truncated        = o.value(QStringLiteral("truncated")).toBool(false);
    result.outputPath       = o.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = o.value(QStringLiteral("workspace_filename")).toString();
    const QJsonObject position = o.value(QStringLiteral("position_columns")).toObject();
    result.raColumn  = position.value(QStringLiteral("ra")).toString();
    result.decColumn = position.value(QStringLiteral("dec")).toString();
    return result;
}

BackendVlkbPresetsResult BackendClient::vlkbTapPresets() const
{
    BackendVlkbPresetsResult result;
    QString error;
    const QByteArray payload = performGet(
            QUrl(m_baseUrl + QStringLiteral("/v1/vlkb/tap/presets")), error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    for (const QJsonValue &v : o.value(QStringLiteral("presets")).toArray()) {
        const QJsonObject entry = v.toObject();
        result.presets.append({ entry.value(QStringLiteral("id")).toString(),
                                entry.value(QStringLiteral("label")).toString(),
                                entry.value(QStringLiteral("table")).toString() });
    }
    for (const QJsonValue &v : o.value(QStringLiteral("bands")).toArray())
        result.bands << v.toString();
    result.serviceUrl = o.value(QStringLiteral("service_url")).toString();
    result.valid = !result.presets.isEmpty();
    if (!result.valid)
        result.error = QStringLiteral("The backend returned no VLKB query presets.");
    return result;
}

BackendVlkbTapResult BackendClient::vlkbTapQuery(const QString &preset, const QString &query,
                                                 double glonMin, double glonMax, double glatMin,
                                                 double glatMax, const QString &band, int maxRows,
                                                 const QString &outputBasename) const
{
    BackendVlkbTapResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("preset"),          preset },
            { QStringLiteral("query"),           query },
            { QStringLiteral("glon_min"),        glonMin },
            { QStringLiteral("glon_max"),        glonMax },
            { QStringLiteral("glat_min"),        glatMin },
            { QStringLiteral("glat_max"),        glatMax },
            { QStringLiteral("band"),            band },
            { QStringLiteral("max_rows"),        maxRows },
            { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/vlkb/tap")), body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    result.valid = o.value(QStringLiteral("valid")).toBool(false);
    result.error = o.value(QStringLiteral("error")).toString();
    result.url   = o.value(QStringLiteral("url")).toString();
    result.query = o.value(QStringLiteral("query")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("columns")).toArray())
        result.columns << v.toString();
    for (const QJsonValue &rowValue : o.value(QStringLiteral("rows")).toArray()) {
        QStringList row;
        for (const QJsonValue &cell : rowValue.toArray())
            row << cell.toString();
        result.rows << row;
    }
    result.totalRows         = o.value(QStringLiteral("total_rows")).toInt();
    result.truncated         = o.value(QStringLiteral("truncated")).toBool(false);
    result.outputPath        = o.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = o.value(QStringLiteral("workspace_filename")).toString();
    result.hitRowLimit       = o.value(QStringLiteral("hit_row_limit")).toBool(false);
    return result;
}
