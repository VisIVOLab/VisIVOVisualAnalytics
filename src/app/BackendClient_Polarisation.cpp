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

// ── BackendClient: Polarisation domain (extracted verbatim) ──

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
