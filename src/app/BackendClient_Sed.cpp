// BackendClient_Sed.cpp
// SED fitting transport: greybody fits and VLKB theoretical-model fits.
//
// The fit itself lives on the backend (app/compute/sed.py) so it is
// reproducible from the API and testable without a GUI. The legacy client ran
// it locally by spawning `python sedfit_main.py` with eval()-ed argv strings,
// which meant the result depended on whatever Python and packages the user's
// machine happened to have.

#include "BackendClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

#include <limits>

namespace {

QJsonArray toJsonArray(const QVector<double> &values)
{
    QJsonArray out;
    for (double value : values)
        out.append(value);
    return out;
}

QVector<double> toDoubleVector(const QJsonArray &array)
{
    QVector<double> out;
    out.reserve(array.size());
    for (const QJsonValue &value : array)
        out.append(value.toDouble());
    return out;
}

QStringList toStringList(const QJsonArray &array)
{
    QStringList out;
    for (const QJsonValue &value : array)
        out.append(value.toString());
    return out;
}

QMap<QString, double> toDoubleMap(const QJsonObject &object)
{
    QMap<QString, double> out;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        out.insert(it.key(), it.value().toDouble());
    return out;
}

/// A range is only sent when the caller filled it in; otherwise the backend's
/// own default applies, and the two ends never disagree about what it is.
void addRange(QJsonObject &body, const QString &key, const QVector<double> &range)
{
    if (range.size() == 3)
        body.insert(key, toJsonArray(range));
}

BackendSedFit parseFit(const QJsonObject &object)
{
    BackendSedFit fit;
    fit.model = object.value(QStringLiteral("model")).toString();
    fit.parameters = toDoubleMap(object.value(QStringLiteral("parameters")).toObject());
    fit.uncertainties = toDoubleMap(object.value(QStringLiteral("uncertainties")).toObject());
    fit.unbounded = toStringList(object.value(QStringLiteral("unbounded")).toArray());
    fit.atBoundary = toStringList(object.value(QStringLiteral("at_boundary")).toArray());
    fit.warnings = toStringList(object.value(QStringLiteral("warnings")).toArray());
    fit.chi2 = object.value(QStringLiteral("chi2")).toDouble();
    // dof ≤ 0 means there IS no reduced χ², and the backend sends null. Reading
    // that as 0 would show the most under-determined fit as the best one.
    const QJsonValue reduced = object.value(QStringLiteral("chi2_reduced"));
    fit.chi2Reduced = reduced.isDouble() ? reduced.toDouble()
                                         : std::numeric_limits<double>::quiet_NaN();
    fit.dof = object.value(QStringLiteral("dof")).toInt();
    fit.iterations = object.value(QStringLiteral("iterations")).toInt();
    fit.converged = object.value(QStringLiteral("converged")).toBool();
    fit.luminosityLsun = object.value(QStringLiteral("luminosity_lsun")).toDouble();
    fit.bandsUsed = object.value(QStringLiteral("bands_used")).toInt();
    fit.bandsUpperLimit = object.value(QStringLiteral("bands_upper_limit")).toInt();
    fit.modelWavelengthUm =
            toDoubleVector(object.value(QStringLiteral("model_wavelength_um")).toArray());
    fit.modelFluxJy = toDoubleVector(object.value(QStringLiteral("model_flux_jy")).toArray());
    fit.bandWavelengthUm =
            toDoubleVector(object.value(QStringLiteral("band_wavelength_um")).toArray());
    fit.fittedFluxJy = toDoubleVector(object.value(QStringLiteral("fitted_flux_jy")).toArray());
    return fit;
}

/// Pull the backend's error message out of a failed response. FastAPI reports
/// a 4xx as {"detail": "…"}; showing the raw body instead would put JSON in
/// front of the user.
QString describeFailure(const QByteArray &payload, const QString &transportError)
{
    if (!payload.isEmpty()) {
        const QJsonObject object = QJsonDocument::fromJson(payload).object();
        const QJsonValue detail = object.value(QStringLiteral("detail"));
        if (detail.isString() && !detail.toString().isEmpty())
            return detail.toString();
        const QString error = object.value(QStringLiteral("error")).toString();
        if (!error.isEmpty())
            return error;
    }
    return transportError;
}

}   // namespace

BackendSedFitResult BackendClient::requestSedFit(const BackendSedFitRequest &request) const
{
    BackendSedFitResult result;

    QJsonArray wavelengths;
    QJsonArray fluxes;
    QJsonArray errors;
    QJsonArray upperLimits;
    bool anyError = false;
    bool anyUpperLimit = false;
    for (const BackendSedPoint &point : request.points) {
        wavelengths.append(point.wavelengthUm);
        fluxes.append(point.fluxJy);
        errors.append(point.errorJy);
        upperLimits.append(point.upperLimit);
        anyError = anyError || point.errorJy > 0.0;
        anyUpperLimit = anyUpperLimit || point.upperLimit;
    }
    if (wavelengths.size() < 2) {
        result.error = QStringLiteral("A SED fit needs at least 2 photometric points.");
        return result;
    }

    QJsonObject body{
        { QStringLiteral("model"), request.model },
        { QStringLiteral("wavelengths_um"), wavelengths },
        { QStringLiteral("fluxes_jy"), fluxes },
        { QStringLiteral("distance_pc"), request.distancePc },
        { QStringLiteral("kappa_ref"), request.kappaRef },
        { QStringLiteral("lambda_ref_um"), request.lambdaRefUm },
        { QStringLiteral("colour_correction"), request.colourCorrection },
        { QStringLiteral("size_arcsec"), request.sizeArcsec },
        { QStringLiteral("source_label"), request.sourceLabel },
    };
    // All-zero errors mean "no errors", not "zero uncertainty"; sending them
    // would have the backend treat every band as infinitely precise.
    if (anyError)
        body.insert(QStringLiteral("errors_jy"), errors);
    if (anyUpperLimit)
        body.insert(QStringLiteral("upper_limits"), upperLimits);
    addRange(body, QStringLiteral("mass_range"), request.massRange);
    addRange(body, QStringLiteral("temp_range"), request.tempRange);
    addRange(body, QStringLiteral("beta_range"), request.betaRange);
    addRange(body, QStringLiteral("lambda0_range"), request.lambda0Range);
    addRange(body, QStringLiteral("scale_range"), request.scaleRange);

    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/sed/fit")), QJsonDocument(body), transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        result.error = describeFailure(payload, transportError.isEmpty()
                                                ? QStringLiteral("The SED fit failed.")
                                                : transportError);
        return result;
    }

    const QJsonArray fits = object.value(QStringLiteral("fits")).toArray();
    for (const QJsonValue &value : fits)
        result.fits.append(parseFit(value.toObject()));
    if (result.fits.isEmpty()) {
        result.error = QStringLiteral("The backend returned no fitted model.");
        return result;
    }
    result.best = object.value(QStringLiteral("best")).toString();
    result.valid = true;
    return result;
}

BackendSedModelsResult BackendClient::requestSedModels(
        const QVector<BackendSedPoint> &points, double distancePc, double prefilter,
        double weightMidIr, double weightFarIr, double weightSubmm, double deltaChi2,
        const QString &serviceUrl, const QString &sourceLabel) const
{
    BackendSedModelsResult result;

    QJsonArray wavelengths;
    QJsonArray fluxes;
    QJsonArray errors;
    QJsonArray flags;
    for (const BackendSedPoint &point : points) {
        wavelengths.append(point.wavelengthUm);
        fluxes.append(point.fluxJy);
        errors.append(point.errorJy);
        // The model service's flag column marks a band as a detection (1) or an
        // upper limit (0), which is the opposite polarity to our own flag.
        flags.append(point.upperLimit ? 0 : 1);
    }
    if (wavelengths.size() < 2) {
        result.error = QStringLiteral("A model fit needs at least 2 photometric points.");
        return result;
    }

    QJsonObject body{
        { QStringLiteral("wavelengths_um"), wavelengths },
        { QStringLiteral("fluxes_jy"), fluxes },
        { QStringLiteral("errors_jy"), errors },
        { QStringLiteral("flags"), flags },
        { QStringLiteral("distance_pc"), distancePc },
        { QStringLiteral("prefilter"), prefilter },
        { QStringLiteral("weight_mid_ir"), weightMidIr },
        { QStringLiteral("weight_far_ir"), weightFarIr },
        { QStringLiteral("weight_submm"), weightSubmm },
        { QStringLiteral("delta_chi2"), deltaChi2 },
        { QStringLiteral("service_url"), serviceUrl },
        { QStringLiteral("source_label"), sourceLabel },
    };

    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/sed/models")), QJsonDocument(body),
            transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        result.error = describeFailure(payload, transportError.isEmpty()
                                                ? QStringLiteral("The model fit failed.")
                                                : transportError);
        return result;
    }

    result.columns = toStringList(object.value(QStringLiteral("columns")).toArray());
    const QJsonArray models = object.value(QStringLiteral("models")).toArray();
    for (const QJsonValue &value : models) {
        const QJsonObject entry = value.toObject();
        BackendSedModel model;
        for (auto it = entry.constBegin(); it != entry.constEnd(); ++it) {
            if (it.key() == QLatin1String("model_wavelength_um")
                || it.key() == QLatin1String("model_flux_jy"))
                continue;
            model.fields.insert(it.key(), it.value().toVariant());
        }
        model.chi2 = entry.value(QStringLiteral("chi2")).toDouble();
        model.wavelengthUm =
                toDoubleVector(entry.value(QStringLiteral("model_wavelength_um")).toArray());
        model.fluxJy = toDoubleVector(entry.value(QStringLiteral("model_flux_jy")).toArray());
        result.models.append(model);
    }
    result.totalModels = object.value(QStringLiteral("total_models")).toInt();
    result.skippedModels = object.value(QStringLiteral("skipped_models")).toInt();
    result.truncated = object.value(QStringLiteral("truncated")).toBool();
    result.valid = true;
    return result;
}

BackendSedDefaults BackendClient::requestSedDefaults() const
{
    BackendSedDefaults defaults;
    QString error;
    const QByteArray payload =
            performGet(QUrl(m_baseUrl + QStringLiteral("/v1/sed/defaults")), error);
    if (!error.isEmpty()) {
        defaults.error = error;
        return defaults;
    }
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!object.value(QStringLiteral("valid")).toBool()) {
        defaults.error = describeFailure(payload, QStringLiteral("No SED defaults available."));
        return defaults;
    }
    defaults.massRange = toDoubleVector(object.value(QStringLiteral("mass_range")).toArray());
    defaults.tempRange = toDoubleVector(object.value(QStringLiteral("temp_range")).toArray());
    defaults.betaRange = toDoubleVector(object.value(QStringLiteral("beta_range")).toArray());
    defaults.lambda0Range = toDoubleVector(object.value(QStringLiteral("lambda0_range")).toArray());
    defaults.scaleRange = toDoubleVector(object.value(QStringLiteral("scale_range")).toArray());
    defaults.kappaRef = object.value(QStringLiteral("kappa_ref")).toDouble(0.1);
    defaults.lambdaRefUm = object.value(QStringLiteral("lambda_ref_um")).toDouble(300.0);
    defaults.distancePc = object.value(QStringLiteral("distance_pc")).toDouble(1000.0);
    defaults.modelsServiceUrl = object.value(QStringLiteral("models_service_url")).toString();
    defaults.valid = true;
    return defaults;
}
