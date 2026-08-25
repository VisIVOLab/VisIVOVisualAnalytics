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

// ── BackendClient: Spectral domain (extracted verbatim) ──

namespace {
static BackendSpectrumResult parseSpectrum(const QByteArray &raw, const QString &error)
{
    BackendSpectrumResult r;
    if (!error.isEmpty()) { r.error = error; return r; }
    const QJsonObject o = QJsonDocument::fromJson(raw).object();
    r.valid        = o.value(QStringLiteral("valid")).toBool();
    r.error        = o.value(QStringLiteral("error")).toString();
    r.spectralUnit = o.value(QStringLiteral("spectral_unit")).toString();
    r.spectralType = o.value(QStringLiteral("spectral_type")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("spectral_axis")).toArray())
        r.spectralAxis.append(v.toDouble());
    for (const QJsonValue &v : o.value(QStringLiteral("values")).toArray())
        r.values.append(v.isNull() ? std::numeric_limits<double>::quiet_NaN() : v.toDouble());
    return r;
}
}  // namespace

BackendLinewidthResult BackendClient::fetchLinewidthBinary(const QString &datasetId,
                                                            int channelStart, int channelEnd,
                                                            bool maskEnabled, float threshold,
                                                            double restFreqHz)
{
    BackendLinewidthResult result;
    QString error;
    QJsonObject bodyObj{
            { QStringLiteral("dataset_id"),      datasetId },
            { QStringLiteral("channel_start"),   channelStart },
            { QStringLiteral("channel_end"),     channelEnd },
            { QStringLiteral("mask_enabled"),    maskEnabled },
            { QStringLiteral("threshold_value"), static_cast<double>(threshold) } };
    // Only include rest_freq_hz when the caller provides a meaningful value.
    // Velocity-axis cubes must omit it; frequency/wavelength axes supply it.
    if (restFreqHz > 0.0) {
        bodyObj.insert(QStringLiteral("rest_freq_hz"), restFreqHz);
    }
    const QJsonDocument body(bodyObj);
    QMap<QString, QString> headers;
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/spectral/linewidth/binary")),
                        body, error, &headers);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    // The binary frames carry no in-band metadata; the backend surfaces the map units as
    // response headers (X-Fwhm-Unit / X-Ew-Unit) so the FWHM/EW products can be labelled.
    result.fwhmUnit = headers.value(QStringLiteral("x-fwhm-unit"));
    result.ewUnit   = headers.value(QStringLiteral("x-ew-unit"));
    // Parse the two concatenated binary frames (FWHM then EW).
    int offset = 0;
    BinaryArrayResult fwhm, ew;
    if (!parseBinaryFrame(payload, offset, fwhm)) {
        result.error = fwhm.error;
        return result;
    }
    if (!parseBinaryFrame(payload, offset, ew)) {
        result.error = ew.error;
        return result;
    }
    result.valid  = true;
    result.width  = fwhm.cols;
    result.height = fwhm.rows;
    result.binaryPayload = payload;
    return result;
}

BackendCubeStatsResult BackendClient::fetchCubeStats(const QString &datasetId)
{
    BackendCubeStatsResult result;
    QString error;
    const QUrl url(m_baseUrl + QStringLiteral("/v1/analysis/cube-stats/") + datasetId);
    const QByteArray raw = performGet(url, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    if (!obj.value(QStringLiteral("valid")).toBool(true)) {
        result.error = obj.value(QStringLiteral("error")).toString();
        return result;
    }
    result.valid        = true;
    result.validCount   = static_cast<long>(obj.value(QStringLiteral("valid_count")).toDouble());
    result.blankedCount = static_cast<long>(obj.value(QStringLiteral("blanked_count")).toDouble());
    result.min          = obj.value(QStringLiteral("min")).toDouble();
    result.max          = obj.value(QStringLiteral("max")).toDouble();
    result.mean         = obj.value(QStringLiteral("mean")).toDouble();
    result.median       = obj.value(QStringLiteral("median")).toDouble();
    result.std          = obj.value(QStringLiteral("std")).toDouble();
    result.rms          = obj.value(QStringLiteral("rms")).toDouble();
    result.sum          = obj.value(QStringLiteral("sum")).toDouble();
    result.medianExact  = obj.value(QStringLiteral("median_exact")).toBool();
    result.bunit        = obj.value(QStringLiteral("bunit")).toString();
    return result;
}

BackendBaselineResult BackendClient::requestBaseline(const QString &datasetId,
                                                      int channelStart, int channelEnd,
                                                      int polyOrder,
                                                      const QString &method,
                                                      const QVector<QPair<int,int>> &lineFreeRanges)
{
    BackendBaselineResult result;
    QString error;
    // Encode line-free ranges as [[start,end],...] pairs
    QJsonArray lfcArray;
    for (const auto &range : lineFreeRanges) {
        QJsonArray pair;
        pair.append(range.first);
        pair.append(range.second);
        lfcArray.append(pair);
    }
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("session_id"),         m_sessionId },
            { QStringLiteral("dataset_id"),         datasetId },
            { QStringLiteral("channel_start"),      channelStart },
            { QStringLiteral("channel_end"),        channelEnd },
            { QStringLiteral("poly_order"),         polyOrder },
            { QStringLiteral("method"),             method },
            { QStringLiteral("line_free_channels"), lfcArray } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl
                             + QStringLiteral("/v1/spectral/baseline/")
                             + m_sessionId + QStringLiteral("/") + datasetId),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid         = obj.value(QStringLiteral("valid")).toBool();
    result.error         = obj.value(QStringLiteral("error")).toString();
    result.newDatasetId  = obj.value(QStringLiteral("new_dataset_id")).toString();
    result.width         = obj.value(QStringLiteral("width")).toInt();
    result.height        = obj.value(QStringLiteral("height")).toInt();
    result.depth         = obj.value(QStringLiteral("depth")).toInt();
    result.rangeMin      = obj.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax      = obj.value(QStringLiteral("range_max")).toDouble();
    result.polyOrder     = obj.value(QStringLiteral("poly_order")).toInt();
    result.fitChannels   = obj.value(QStringLiteral("fit_channels")).toInt();
    result.outputPath    = obj.value(QStringLiteral("output_path")).toString();
    result.rmsBefore     = obj.value(QStringLiteral("rms_before")).toDouble();
    result.rmsAfter      = obj.value(QStringLiteral("rms_after")).toDouble();
    result.method        = obj.value(QStringLiteral("method")).toString(method);
    return result;
}

BackendDynspecDedisperseResult BackendClient::dedisperseDynspec(
        const QString &datasetId,
        double dmPcCm3,
        double refFreqMhz,
        const QString &outputBasename)
{
    BackendDynspecDedisperseResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"),      datasetId },
            { QStringLiteral("dm_pc_cm3"),       dmPcCm3 },
            { QStringLiteral("ref_freq_mhz"),    refFreqMhz },
            { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/dynspec/dedisperse")),
                        body, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid             = obj.value(QStringLiteral("valid")).toBool(false);
    result.error             = obj.value(QStringLiteral("error")).toString();
    result.newDatasetId      = obj.value(QStringLiteral("new_dataset_id")).toString();
    result.outputPath        = obj.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = obj.value(QStringLiteral("workspace_filename")).toString();
    result.width             = obj.value(QStringLiteral("width")).toInt();
    result.height            = obj.value(QStringLiteral("height")).toInt();
    result.rangeMin          = obj.value(QStringLiteral("range_min")).toDouble();
    result.rangeMax          = obj.value(QStringLiteral("range_max")).toDouble();
    result.dmPcCm3           = obj.value(QStringLiteral("dm_pc_cm3")).toDouble();
    result.refFreqMhz        = obj.value(QStringLiteral("ref_freq_mhz")).toDouble();
    result.maxShiftSamples   = obj.value(QStringLiteral("max_shift_samples")).toInt();
    return result;
}

BackendDynspecPulseProfileResult BackendClient::pulseProfile(
        const QString &datasetId,
        double periodS,
        int nBins,
        double dmPcCm3,
        double freqStartMhz,
        double freqEndMhz)
{
    BackendDynspecPulseProfileResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"),     datasetId },
            { QStringLiteral("period_s"),       periodS },
            { QStringLiteral("n_bins"),         nBins },
            { QStringLiteral("dm_pc_cm3"),      dmPcCm3 },
            { QStringLiteral("freq_start_mhz"), freqStartMhz },
            { QStringLiteral("freq_end_mhz"),   freqEndMhz } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/dynspec/pulse_profile")),
                        body, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid    = obj.value(QStringLiteral("valid")).toBool(false);
    result.error    = obj.value(QStringLiteral("error")).toString();
    const QJsonArray phaseArr = obj.value(QStringLiteral("phase")).toArray();
    const QJsonArray profArr  = obj.value(QStringLiteral("profile")).toArray();
    const QJsonArray cntArr   = obj.value(QStringLiteral("counts")).toArray();
    result.phase.reserve(phaseArr.size());
    result.profile.reserve(profArr.size());
    result.counts.reserve(cntArr.size());
    for (const auto &v : phaseArr) result.phase.append(v.toDouble());
    for (const auto &v : profArr)  result.profile.append(v.toDouble());
    for (const auto &v : cntArr)   result.counts.append(v.toInt());
    result.peak      = obj.value(QStringLiteral("peak")).toDouble();
    result.sigmaMad  = obj.value(QStringLiteral("sigma_mad")).toDouble();
    result.snrPeak   = obj.value(QStringLiteral("snr_peak")).toDouble();
    result.periodS   = obj.value(QStringLiteral("period_s")).toDouble();
    result.dmPcCm3   = obj.value(QStringLiteral("dm_pc_cm3")).toDouble();
    result.nBins     = obj.value(QStringLiteral("n_bins")).toInt();
    result.nSamples  = obj.value(QStringLiteral("n_samples")).toInt();
    return result;
}

BackendDynspecRfiMaskResult BackendClient::rfiMask(
        const QString &datasetId,
        double nSigma,
        const QString &axis,
        const QString &outputBasename)
{
    BackendDynspecRfiMaskResult result;
    QString error;
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_id"),      datasetId },
            { QStringLiteral("n_sigma"),         nSigma },
            { QStringLiteral("axis"),            axis },
            { QStringLiteral("output_basename"), outputBasename } });
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/dynspec/rfi_mask")),
                        body, error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    result.valid             = obj.value(QStringLiteral("valid")).toBool(false);
    result.error             = obj.value(QStringLiteral("error")).toString();
    result.newDatasetId      = obj.value(QStringLiteral("new_dataset_id")).toString();
    result.outputPath        = obj.value(QStringLiteral("output_path")).toString();
    result.workspaceFilename = obj.value(QStringLiteral("workspace_filename")).toString();
    result.width             = obj.value(QStringLiteral("width")).toInt();
    result.height            = obj.value(QStringLiteral("height")).toInt();
    result.flaggedFraction   = obj.value(QStringLiteral("flagged_fraction")).toDouble();
    result.nSigma            = obj.value(QStringLiteral("n_sigma")).toDouble();
    result.axis              = obj.value(QStringLiteral("axis")).toString();
    return result;
}

BackendStackResult BackendClient::fetchStackBinary(const QStringList &datasetIds,
                                                    const QString &method,
                                                    const QVector<double> &weights,
                                                    const QString &weightBy)
{
    BackendStackResult result;
    QString error;
    QJsonArray idsArray;
    for (const QString &id : datasetIds)
        idsArray.append(id);
    QJsonArray wArray;
    for (double w : weights)
        wArray.append(w);
    const QJsonDocument body(QJsonObject{
            { QStringLiteral("dataset_ids"), idsArray },
            { QStringLiteral("method"),      method },
            { QStringLiteral("weights"),     wArray },
            { QStringLiteral("weight_by"),   weightBy } });
    const QByteArray payload =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/spectral/stack/binary")),
                        body, error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    int offset = 0;
    BinaryArrayResult frame;
    if (!parseBinaryFrame(payload, offset, frame)) {
        result.error = frame.error;
        return result;
    }
    result.valid    = true;
    result.spectrum = frame;
    result.method   = method;
    result.nCubes   = datasetIds.size();
    return result;
}

BackendSpectrumResult BackendClient::spectralProbe(const QString &datasetId,
                                                     double x, double y) const
{
    QJsonObject body{
        { QStringLiteral("dataset_id"), datasetId },
        { QStringLiteral("x"), x },
        { QStringLiteral("y"), y },
    };
    QString error;
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/spectral/probe")),
                        QJsonDocument(body), error);
    return parseSpectrum(raw, error);
}

BackendSpectrumResult BackendClient::spectralAperture(const QString &datasetId,
                                                        double x, double y,
                                                        double radiusPixels) const
{
    QJsonObject body{
        { QStringLiteral("dataset_id"), datasetId },
        { QStringLiteral("x"), x },
        { QStringLiteral("y"), y },
        { QStringLiteral("radius"), radiusPixels },
    };
    QString error;
    const QByteArray raw =
            performPost(QUrl(m_baseUrl + QStringLiteral("/v1/spectral/aperture")),
                        QJsonDocument(body), error);
    return parseSpectrum(raw, error);
}

BackendLineFitResult BackendClient::fitSpectralLine(const QVector<double> &values,
                                                    const QVector<double> &axis,
                                                    bool withBaseline) const
{
    BackendLineFitResult result;
    QJsonArray valArr, axArr;
    for (double v : values) valArr.append(v);
    for (double a : axis)   axArr.append(a);
    const QJsonObject bodyObj{
        { QStringLiteral("values"), valArr },
        { QStringLiteral("axis"), axArr },
        { QStringLiteral("with_baseline"), withBaseline },
    };
    QString error;
    const QByteArray payload = performPost(
            QUrl(m_baseUrl + QStringLiteral("/v1/spectral/linefit")), QJsonDocument(bodyObj), error);
    if (!error.isEmpty()) { result.error = error; return result; }
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    result.valid          = obj.value(QStringLiteral("valid")).toBool();
    result.error          = obj.value(QStringLiteral("error")).toString();
    result.peak           = obj.value(QStringLiteral("peak")).toDouble();
    result.center         = obj.value(QStringLiteral("center")).toDouble();
    result.sigma          = obj.value(QStringLiteral("sigma")).toDouble();
    result.fwhm           = obj.value(QStringLiteral("fwhm")).toDouble();
    result.integratedFlux = obj.value(QStringLiteral("integrated_flux")).toDouble();
    result.continuum      = obj.value(QStringLiteral("continuum")).toDouble();
    result.rms            = obj.value(QStringLiteral("rms")).toDouble();
    const QJsonArray baseline = obj.value(QStringLiteral("baseline")).toArray();
    if (baseline.size() >= 2) {
        result.baselineIntercept = baseline.at(0).toDouble();
        result.baselineSlope     = baseline.at(1).toDouble();
    }
    return result;
}
