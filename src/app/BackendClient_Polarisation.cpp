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
        int channel,
        const QString &companionDatasetId)
{
    BackendStokesPlaneResult result;
    QString error;
    QString query = QStringLiteral("/binary?plane=") + plane
                    + QStringLiteral("&channel=") + QString::number(channel);
    // The file that actually holds this plane, when the dataset the user opened
    // does not (one FITS per Stokes plane).
    if (!companionDatasetId.isEmpty())
        query += QStringLiteral("&companion_dataset_id=") + companionDatasetId;
    const QUrl url(m_baseUrl + QStringLiteral("/v1/stokes/") + datasetId + query);
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

namespace {

/// The "companions" object the polarisation endpoints expect, or an absent
/// value when nothing is loaded. Built once so the metadata request and the
/// binary request that follows it cannot disagree about which files to read —
/// they would then describe one computation and return another.
QJsonObject stokesCompanionsJson(const BackendStokesCompanionIds &ids)
{
    QJsonObject obj;
    if (!ids.i.isEmpty()) obj.insert(QStringLiteral("i_dataset_id"), ids.i);
    if (!ids.q.isEmpty()) obj.insert(QStringLiteral("q_dataset_id"), ids.q);
    if (!ids.u.isEmpty()) obj.insert(QStringLiteral("u_dataset_id"), ids.u);
    if (!ids.v.isEmpty()) obj.insert(QStringLiteral("v_dataset_id"), ids.v);
    return obj;
}

}   // namespace

BackendPolarisationPiResult BackendClient::requestPolarisationPi(
        const QString &datasetId,
        int channel,
        const QVector<int> &lineFreeChannels,
        const BackendStokesCompanionIds &companions)
{
    BackendPolarisationPiResult result;
    QString error;
    const QJsonObject companionsObj = stokesCompanionsJson(companions);

    // Step 1: register PI+PA maps via JSON endpoint to get dataset IDs
    QJsonArray lfcArr;
    for (int ch : lineFreeChannels) lfcArr.append(ch);
    QJsonObject piBody{
        { QStringLiteral("dataset_id"),       datasetId },
        { QStringLiteral("channel"),          channel },
        { QStringLiteral("line_free_channels"), lfcArr },
    };
    if (!companionsObj.isEmpty())
        piBody.insert(QStringLiteral("companions"), companionsObj);
    const QJsonDocument regBody(piBody);
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
    const QJsonDocument binBody(piBody);   // same request, so the same computation
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
    // Straight assignment: BinaryArrayResult::data is already the raw float32
    // bytes. The earlier form re-wrapped it as if it were a float ARRAY and
    // multiplied the count by sizeof(float), reading four times the allocation
    // — harmless only for as long as nothing called these endpoints.
    result.piData = piFrame.data;
    result.paData = paFrame.data;
    result.valid = true;
    return result;
}

BackendRmSynthesisResult BackendClient::requestRmSynthesis(
        const QString &datasetId,
        double phiMin,
        double phiMax,
        double dPhi,
        const BackendStokesCompanionIds &companions,
        bool peakOnly,
        const QRect &region)
{
    BackendRmSynthesisResult result;
    result.peakMode = peakOnly;
    QString error;

    QJsonObject body{
        { QStringLiteral("dataset_id"),       datasetId },
        { QStringLiteral("phi_range_rad_m2"), QJsonArray{ phiMin, phiMax } },
        { QStringLiteral("dphi_rad_m2"),      dPhi },
        { QStringLiteral("reduce"), peakOnly ? QStringLiteral("peak") : QStringLiteral("cube") },
    };
    const QJsonObject companionsObj = stokesCompanionsJson(companions);
    if (!companionsObj.isEmpty())
        body.insert(QStringLiteral("companions"), companionsObj);
    if (region.isValid()) {
        body.insert(QStringLiteral("region"),
                    QJsonArray{ region.x(), region.y(), region.width(), region.height() });
    }

    // Step 1: get metadata (phi_grid, rmsf, shape) via JSON endpoint
    const QJsonDocument metaBody(body);
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
    result.phiGrid       = BackendJson::toDoubleVector(metaObj.value(QStringLiteral("phi_grid")).toArray());
    result.rmsf          = BackendJson::toDoubleVector(metaObj.value(QStringLiteral("rmsf")).toArray());
    result.droppedPlanes = BackendJson::toIntVector(metaObj.value(QStringLiteral("dropped_planes")).toArray());
    result.droppedFreqHz = BackendJson::toDoubleVector(metaObj.value(QStringLiteral("dropped_freq_hz")).toArray());
    result.phiPeakMin = static_cast<float>(metaObj.value(QStringLiteral("phi_peak_min")).toDouble());
    result.phiPeakMax = static_cast<float>(metaObj.value(QStringLiteral("phi_peak_max")).toDouble());
    if (const QJsonValue ps = metaObj.value(QStringLiteral("peak_sigma")); ps.isDouble())
        result.peakSigma = ps.toDouble();

    // Step 2: fetch binary FDF cube (nPhi*height rows × width cols float32)
    const QJsonDocument binBody(body);   // same request, so the same computation
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
    result.fdfData = fdfFrame.data;   // already raw float32 bytes
    // Peak mode sends a second frame: the φ at which each pixel peaks, i.e. the
    // RM map. Its absence is a protocol error, not an empty result.
    if (peakOnly) {
        BinaryArrayResult phiFrame;
        if (!parseBinaryFrame(binRaw, offset, phiFrame)) {
            result.error = QStringLiteral("Failed to parse the peak-φ binary frame");
            return result;
        }
        result.phiPeakData = phiFrame.data;
    }
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
