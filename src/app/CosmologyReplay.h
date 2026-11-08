#ifndef COSMOLOGYREPLAY_H
#define COSMOLOGYREPLAY_H
// CosmologyReplay.h
// The replay record of the two map tools issue #29 put in the viewers: the
// angular power spectrum and the skeleton / void detector. Each product keeps
// the service calls that regenerate it (ReplayStep), with the request under the
// route's own field names, so the exported recipe can be posted back as it is.
//
// On a 2-D image the tool reads the dataset the window opened. On a cube's
// moment map there is no such dataset: the map lives in the viewer. The tool
// first writes the moment to the Workspace Exports (the same task the cube's
// "Export Moment Map as FITS" runs), opens that file, and analyses it. The
// record lists those two steps first, because the moment's recipe is part of
// what was measured.
//
// tests/test_cosmology_tools.cpp reads the request models out of
// backend/app/routers/{cosmology,analysis}.py and checks the recorded keys
// against them, so a renamed field fails the C++ suite.

#include "BackendClient.h"
#include "ProductRegistry.h"

#include <QCryptographicHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QVariantMap>

namespace visivo::cosmology {

inline QString angularPowerRoute() { return QStringLiteral("/v1/cosmology/angular_power/{dataset_id}"); }
inline QString skeletonRoute() { return QStringLiteral("/v1/analysis/skeleton/{dataset_id}"); }

inline QVariantMap angularPowerRequestMap(int channel, int nBins)
{
    return { { QStringLiteral("channel"), channel }, { QStringLiteral("n_bins"), nBins } };
}

inline ReplayStep angularPowerStep(int channel, int nBins, const QString &datasetNote = {})
{
    return ReplayStep{ angularPowerRoute(), angularPowerRequestMap(channel, nBins), datasetNote };
}

inline QVariantMap skeletonRequestMap(const BackendSkeletonRequest &request)
{
    return BackendClient::skeletonRequestBody(request).toVariantMap();
}

inline ReplayStep skeletonStep(const BackendSkeletonRequest &request,
                               const QString &datasetNote = {})
{
    return ReplayStep{ skeletonRoute(), skeletonRequestMap(request), datasetNote };
}

/// The recipe of the moment map a cube tool analysed, as the moment export
/// takes it (POST /v1/tasks/moment_fits).
struct MomentSource
{
    QString datasetId;   ///< the cube's backend id when the tool ran
    int order{ 0 };
    int channelStart{ 0 };   ///< 0-based, inclusive (as the backend takes it)
    int channelEnd{ 0 };
    bool maskEnabled{ false };
    double thresholdValue{ 0.0 };
    bool thresholdAuto{ false };
    QString outputBasename;
};

inline QVariantMap momentFitsRequestMap(const MomentSource &m)
{
    return { { QStringLiteral("dataset_id"), m.datasetId },
             { QStringLiteral("moment_order"), m.order },
             { QStringLiteral("channel_start"), m.channelStart },
             { QStringLiteral("channel_end"), m.channelEnd },
             { QStringLiteral("mask_enabled"), m.maskEnabled },
             { QStringLiteral("threshold_value"), m.thresholdValue },
             { QStringLiteral("threshold_auto"), m.thresholdAuto },
             { QStringLiteral("output_basename"), m.outputBasename } };
}

/// The Workspace Exports name of a moment written for the map tools. It
/// carries a short digest of the cube, of the cube's revision and of the
/// moment's recipe, so the same moment of the same file always gets the same
/// name: a later run, in this session or another, finds the file and opens it
/// instead of writing `…_maptools_N.fits` again. A different moment, or the
/// same moment of a cube replaced at the same path, gets another name.
/// @p sourceFingerprint is the cube's size and modification time as the
/// backend sees them; the caller does not reuse an export when it is empty.
inline QString momentExportBasename(const QString &cubePath, const QString &stem,
                                    const MomentSource &m, const QString &sourceFingerprint)
{
    const QString canonical = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
                                      .arg(cubePath, sourceFingerprint)
                                      .arg(m.order)
                                      .arg(m.channelStart)
                                      .arg(m.channelEnd)
                                      .arg(m.maskEnabled ? 1 : 0)
                                      .arg(m.thresholdValue, 0, 'g', 17)
                                      .arg(m.thresholdAuto ? 1 : 0);
    const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return QStringLiteral("%1_m%2_%3_maptools.fits").arg(stem).arg(m.order).arg(digest);
}

/// Session Data labels: what tells two runs apart comes first (the result,
/// then the finish time), the map it was measured on last, so a truncated
/// row still differs from its neighbour. The kind is the row's badge.
inline QString powerSpectrumRunLabel(bool fitValid, double slope, const QString &finishedAt,
                                     const QString &source = {})
{
    QStringList parts{ fitValid ? QStringLiteral("n = %1").arg(slope, 0, 'f', 2)
                                : QStringLiteral("no slope") };
    parts << finishedAt;
    if (!source.isEmpty())
        parts << source;
    return parts.join(QStringLiteral(" \u00b7 "));
}

inline QString skeletonRunLabel(bool voids, int skeletonPixels, const QString &finishedAt,
                                const QString &source = {})
{
    QStringList parts{ voids ? QStringLiteral("Voids") : QStringLiteral("Filaments"),
                       QStringLiteral("%1 px").arg(skeletonPixels), finishedAt };
    if (!source.isEmpty())
        parts << source;
    return parts.join(QStringLiteral(" \u00b7 "));
}

/// The finish time a run label carries.
inline QString runFinishedAt() { return QTime::currentTime().toString(QStringLiteral("HH:mm:ss")); }

/// The two calls that turn a cube's moment into a dataset the map tools read:
/// write it as FITS, then open that file. The analysis step that follows names
/// the dataset id the open returns.
inline QList<ReplayStep> momentDatasetSteps(const MomentSource &m)
{
    ReplayStep write{ QStringLiteral("/v1/tasks/moment_fits"), momentFitsRequestMap(m),
                      QStringLiteral("A task: follow /v1/tasks/{task_id} until it completes; "
                                     "its result's output_path is the moment FITS.") };
    ReplayStep open{ QStringLiteral("/v1/datasets/open"),
                     { { QStringLiteral("path"), QStringLiteral("{output_path}") } },
                     QStringLiteral("path: the output_path of the moment_fits task; the "
                                    "answer's dataset_id is the one the next step reads.") };
    return { write, open };
}

/// The Provenance tab's lines for a skeleton run: names a reader recognises,
/// values as they were used (the threshold the server chose when on auto).
inline QVariantMap skeletonDisplayParams(const BackendSkeletonRequest &request,
                                         double appliedThreshold)
{
    QVariantMap p;
    p.insert(QStringLiteral("Mode"), request.mode == QStringLiteral("void")
                                             ? QStringLiteral("Voids (skeleton of the regions "
                                                              "below the threshold)")
                                             : QStringLiteral("Filaments (skeleton of the "
                                                              "regions above the threshold)"));
    p.insert(QStringLiteral("Threshold"),
             request.thresholdAuto
                     ? QStringLiteral("%1 (auto: mean of the map)").arg(appliedThreshold, 0, 'g', 6)
                     : QString::number(request.threshold, 'g', 6));
    p.insert(QStringLiteral("Smoothing σ (px)"),
             request.smoothSigmaPix > 0.0 ? QString::number(request.smoothSigmaPix, 'g', 4)
                                          : QStringLiteral("none"));
    p.insert(QStringLiteral("Prune below"),
             request.prune ? QString::number(request.persistenceThreshold, 'g', 6)
                           : QStringLiteral("no pruning"));
    return p;
}

}   // namespace visivo::cosmology

#endif   // COSMOLOGYREPLAY_H
