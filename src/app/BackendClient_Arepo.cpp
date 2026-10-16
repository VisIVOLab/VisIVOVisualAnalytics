// BackendClient_Arepo.cpp
// AREPO snapshot transport: browse one, cost an extraction, run it.
//
// An AREPO snapshot is a moving-mesh simulation — an unstructured set of cells
// that no viewer can display. The backend resamples a window of it onto a
// uniform grid with yt (app/compute/arepo.py) and writes FITS; the client only
// chooses the window. The legacy did the same work by spawning
// `python arepo-extractor.py` through QProcess against an interpreter path kept
// in QSettings, which meant it worked on the machine it had been set up on.

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

using BackendJson::toDoubleVector;
using BackendJson::toIntVector;

/// An HDF5 attribute as one line of text. The backend has already reduced it to
/// a JSON scalar, list or summary string; this is only about display.
QString attributeText(const QJsonValue &value)
{
    if (value.isArray()) {
        QStringList parts;
        const QJsonArray array = value.toArray();
        for (const QJsonValue &item : array)
            parts.append(attributeText(item));
        return QStringLiteral("[%1]").arg(parts.join(QStringLiteral(", ")));
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (qFuzzyCompare(number, std::round(number)) && std::abs(number) < 1e15)
            return QString::number(static_cast<long long>(std::llround(number)));
        return QString::number(number, 'g', 8);
    }
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isNull())
        return QStringLiteral("—");
    return value.toString();
}

BackendArepoNode parseNode(const QJsonObject &object)
{
    BackendArepoNode node;
    node.name = object.value(QStringLiteral("name")).toString();
    node.kind = object.value(QStringLiteral("kind")).toString();
    node.shape = toIntVector(object.value(QStringLiteral("shape")).toArray());
    node.dtype = object.value(QStringLiteral("dtype")).toString();

    const QJsonObject attributes = object.value(QStringLiteral("attributes")).toObject();
    for (auto it = attributes.constBegin(); it != attributes.constEnd(); ++it)
        node.attributes.append({ it.key(), attributeText(it.value()) });

    const QJsonArray children = object.value(QStringLiteral("children")).toArray();
    for (const QJsonValue &child : children)
        node.children.append(parseNode(child.toObject()));
    return node;
}

BackendArepoPlan parsePlan(const QJsonObject &object)
{
    BackendArepoPlan plan;
    plan.shape = toIntVector(object.value(QStringLiteral("shape")).toArray());
    plan.cellSizePc = toDoubleVector(object.value(QStringLiteral("cell_size_pc")).toArray());
    plan.windowKpc = toDoubleVector(object.value(QStringLiteral("window_kpc")).toArray());
    plan.extentKpc = toDoubleVector(object.value(QStringLiteral("extent_kpc")).toArray());
    plan.offsetKpc = toDoubleVector(object.value(QStringLiteral("offset_kpc")).toArray());
    plan.domainWidthKpc =
            toDoubleVector(object.value(QStringLiteral("domain_width_kpc")).toArray());
    plan.level = object.value(QStringLiteral("level")).toInt();
    plan.cells = static_cast<qint64>(object.value(QStringLiteral("cells")).toDouble());
    plan.bytesEstimate =
            static_cast<qint64>(object.value(QStringLiteral("bytes_estimate")).toDouble());
    plan.valid = !plan.shape.isEmpty();
    return plan;
}

/// The backend's own message for a failed call. FastAPI reports a 4xx as
/// {"detail": "…"}; showing the raw body would put JSON in front of the user,
/// and these messages are the ones that say *why* (no yt, grid too large).
QString describeFailure(const QByteArray &payload, const QString &transportError,
                        const QString &fallback)
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
    return transportError.isEmpty() ? fallback : transportError;
}

}   // namespace

BackendArepoInspectResult BackendClient::inspectArepoSnapshot(const QString &path) const
{
    BackendArepoInspectResult result;
    const QJsonObject body{ { QStringLiteral("path"), path } };

    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/simulations/arepo/inspect")),
            QJsonDocument(body), transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        result.error = describeFailure(payload, transportError,
                                       QStringLiteral("Could not read the snapshot."));
        return result;
    }

    result.path = object.value(QStringLiteral("path")).toString();
    result.tree = parseNode(object.value(QStringLiteral("tree")).toObject());
    const QJsonValue box = object.value(QStringLiteral("box_size_kpc"));
    result.boxSizeKpc = box.isDouble() ? box.toDouble()
                                       : std::numeric_limits<double>::quiet_NaN();
    const QJsonValue unit = object.value(QStringLiteral("unit_length_cm"));
    result.unitLengthCm = unit.isDouble() ? unit.toDouble()
                                          : std::numeric_limits<double>::quiet_NaN();
    const QJsonObject particles = object.value(QStringLiteral("particle_types")).toObject();
    for (auto it = particles.constBegin(); it != particles.constEnd(); ++it) {
        QStringList fields;
        const QJsonArray names = it.value().toArray();
        for (const QJsonValue &name : names)
            fields.append(name.toString());
        result.particleTypes.insert(it.key(), fields);
    }
    result.ytAvailable = object.value(QStringLiteral("yt_available")).toBool();
    result.valid = true;
    return result;
}

BackendArepoPlan BackendClient::planArepoExtraction(const QString &path, int level,
                                                    const QVector<double> &windowKpc,
                                                    const QVector<double> &offsetKpc) const
{
    BackendArepoPlan plan;
    const QJsonObject body{
        { QStringLiteral("path"), path },
        { QStringLiteral("level"), level },
        { QStringLiteral("window_kpc"), toJsonArray(windowKpc) },
        { QStringLiteral("offset_kpc"), toJsonArray(offsetKpc) },
    };

    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/simulations/arepo/plan")),
            QJsonDocument(body), transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        plan.error = describeFailure(payload, transportError,
                                     QStringLiteral("Could not plan the extraction."));
        return plan;
    }
    plan = parsePlan(object);
    return plan;
}

BackendArepoExtractResult BackendClient::extractArepoCubes(
        const QString &path, int level, const QString &particle, const QStringList &fields,
        const QVector<double> &windowKpc, const QVector<double> &offsetKpc) const
{
    BackendArepoExtractResult result;
    QJsonArray fieldArray;
    for (const QString &field : fields)
        fieldArray.append(field);

    const QJsonObject body{
        { QStringLiteral("path"), path },
        { QStringLiteral("level"), level },
        { QStringLiteral("particle"), particle },
        { QStringLiteral("fields"), fieldArray },
        { QStringLiteral("window_kpc"), toJsonArray(windowKpc) },
        { QStringLiteral("offset_kpc"), toJsonArray(offsetKpc) },
    };

    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/simulations/arepo/extract")),
            QJsonDocument(body), transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        result.error = describeFailure(payload, transportError,
                                       QStringLiteral("The extraction failed."));
        return result;
    }

    result.plan = parsePlan(object.value(QStringLiteral("plan")).toObject());
    const QJsonArray outputs = object.value(QStringLiteral("outputs")).toArray();
    for (const QJsonValue &value : outputs) {
        const QJsonObject entry = value.toObject();
        BackendArepoCube cube;
        cube.field = entry.value(QStringLiteral("field")).toString();
        cube.particle = entry.value(QStringLiteral("particle")).toString();
        cube.path = entry.value(QStringLiteral("path")).toString();
        cube.bunit = entry.value(QStringLiteral("bunit")).toString();
        cube.shape = toIntVector(entry.value(QStringLiteral("shape")).toArray());
        result.outputs.append(cube);
    }
    if (result.outputs.isEmpty()) {
        result.error = QStringLiteral("The backend produced no cube.");
        return result;
    }
    result.valid = true;
    return result;
}

// ── VLKB multi-cutout ─────────────────────────────────────────────────────────
//
// The archive's asynchronous batch API: submit a list of datasets as one job,
// poll a phase, collect one archive. The protocol lives on the backend
// (app/vlkb_mcutout.py) — including the SSRF guard, the auth token and the
// unpacking — so this is only the transport.

namespace {

QJsonObject mcutoutJobBody(const QString &jobId, const QString &serviceUrl,
                           const QString &bearer)
{
    return QJsonObject{
        { QStringLiteral("job_id"), jobId },
        { QStringLiteral("service_url"), serviceUrl },
        { QStringLiteral("vlkb_bearer"), bearer },
    };
}

}   // namespace

BackendMcutoutSubmitResult BackendClient::submitVlkbMcutout(
        const QVector<BackendMcutoutItem> &items, const QString &serviceUrl,
        const QString &bearer) const
{
    BackendMcutoutSubmitResult result;
    if (items.isEmpty()) {
        result.error = QStringLiteral("Select at least one dataset.");
        return result;
    }

    QJsonArray array;
    for (const BackendMcutoutItem &item : items) {
        array.append(QJsonObject{
            { QStringLiteral("id"), item.id },
            { QStringLiteral("pos"), item.pos },
            { QStringLiteral("possys"), QStringLiteral("GALACTIC") },
        });
    }
    const QJsonObject body{
        { QStringLiteral("items"), array },
        { QStringLiteral("service_url"), serviceUrl },
        { QStringLiteral("vlkb_bearer"), bearer },
    };

    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/vlkb/mcutout/submit")), QJsonDocument(body),
            transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        result.error = describeFailure(payload, transportError,
                                       QStringLiteral("The batch could not be submitted."));
        return result;
    }
    result.jobId = object.value(QStringLiteral("job_id")).toString();
    result.phase = object.value(QStringLiteral("phase")).toString();
    result.count = object.value(QStringLiteral("count")).toInt();
    result.valid = !result.jobId.isEmpty();
    if (!result.valid)
        result.error = QStringLiteral("The archive did not name a job.");
    return result;
}

BackendMcutoutPhaseResult BackendClient::vlkbMcutoutPhase(const QString &jobId,
                                                          const QString &serviceUrl,
                                                          const QString &bearer) const
{
    BackendMcutoutPhaseResult result;
    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/vlkb/mcutout/phase")),
            QJsonDocument(mcutoutJobBody(jobId, serviceUrl, bearer)), transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        result.error = describeFailure(payload, transportError,
                                       QStringLiteral("The job's phase is unknown."));
        return result;
    }
    result.phase = object.value(QStringLiteral("phase")).toString();
    result.terminal = object.value(QStringLiteral("terminal")).toBool();
    result.valid = true;
    return result;
}

BackendMcutoutReportResult BackendClient::vlkbMcutoutReport(const QString &jobId,
                                                            const QString &serviceUrl,
                                                            const QString &bearer) const
{
    BackendMcutoutReportResult result;
    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/vlkb/mcutout/report")),
            QJsonDocument(mcutoutJobBody(jobId, serviceUrl, bearer)), transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        result.error = describeFailure(payload, transportError,
                                       QStringLiteral("The job report is unavailable."));
        return result;
    }
    const QJsonArray entries = object.value(QStringLiteral("results")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        BackendMcutoutOutcome outcome;
        outcome.index = entry.value(QStringLiteral("index")).toInt(-1);
        outcome.ok = entry.value(QStringLiteral("ok")).toBool();
        outcome.filename = entry.value(QStringLiteral("filename")).toString();
        outcome.detail = entry.value(QStringLiteral("detail")).toString();
        result.results.append(outcome);
    }
    result.valid = true;
    return result;
}

BackendMcutoutFetchResult BackendClient::vlkbMcutoutFetch(const QString &jobId,
                                                          const QString &serviceUrl,
                                                          const QString &bearer) const
{
    BackendMcutoutFetchResult result;
    QString transportError;
    const QByteArray payload = performPostRetryingBackpressure(
            QUrl(m_baseUrl + QStringLiteral("/v1/vlkb/mcutout/fetch")),
            QJsonDocument(mcutoutJobBody(jobId, serviceUrl, bearer)), transportError);
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    if (!transportError.isEmpty() || !object.value(QStringLiteral("valid")).toBool()) {
        result.error = describeFailure(payload, transportError,
                                       QStringLiteral("The results could not be collected."));
        return result;
    }
    result.archivePath = object.value(QStringLiteral("archive")).toString();
    const QJsonArray files = object.value(QStringLiteral("files")).toArray();
    for (const QJsonValue &value : files)
        result.files.append(value.toString());
    result.bytes = static_cast<qint64>(object.value(QStringLiteral("bytes")).toDouble());
    result.valid = true;
    return result;
}
