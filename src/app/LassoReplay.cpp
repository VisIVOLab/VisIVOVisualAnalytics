#include "LassoReplay.h"

#include "BackendHeaders.h"

#include <QVariantList>

using namespace Qt::StringLiterals;

namespace visivo {

namespace {

QString seedList(const QVector<std::array<int, 3>> &seeds, const QString &sep,
                 const QString &fmt)
{
    QStringList out;
    for (const auto &s : seeds)
        out << fmt.arg(s[0]).arg(s[1]).arg(s[2]);
    return out.join(sep);
}

QVariantList seedArrays(const QVector<std::array<int, 3>> &seeds)
{
    QVariantList out;
    for (const auto &s : seeds)
        out << QVariant(QVariantList{ s[0], s[1], s[2] });
    return out;
}

bool smoothed(const LassoGrowRequest &g)
{
    return g.smoothV > 0.0 || g.smoothXy > 0.0;
}

// The grow answers with the selection's own bounding box, which is not the ROI
// the interactive level field was computed on: every call that posts the mask
// back takes its origin and dimensions from that response, so none are
// recorded, where a number would be the wrong one.
QString roiFromGrowResponse()
{
    return u"with origin_x, origin_y, origin_z from its %1 header and width, height, "
           u"depth from %2, %3, %4"_s.arg(QLatin1String(VisivoHeaders::ORIGIN),
                                          QLatin1String(VisivoHeaders::WIDTH),
                                          QLatin1String(VisivoHeaders::HEIGHT),
                                          QLatin1String(VisivoHeaders::DEPTH));
}

} // namespace

QVariantMap lassoGrowRequestMap(const LassoGrowRequest &g)
{
    // Mirrors BackendClient::kinematicSelect: what the route takes, under its
    // own names, with the optional fields absent rather than empty.
    QVariantMap req;
    req.insert(u"seed_z"_s, g.seed[0]);
    req.insert(u"seed_y"_s, g.seed[1]);
    req.insert(u"seed_x"_s, g.seed[2]);
    if (!g.addSeeds.isEmpty())
        req.insert(u"add_seeds"_s, seedArrays(g.addSeeds));
    if (!g.subSeeds.isEmpty())
        req.insert(u"sub_seeds"_s, seedArrays(g.subSeeds));
    req.insert(u"reach"_s, g.reach);
    req.insert(u"lambda_v"_s, g.lambdaV);
    req.insert(u"sigma"_s, g.sigma > 0.0 ? g.sigma : 0.0);   // 0: the service measures
    req.insert(u"nsigma"_s, g.nsigma);
    req.insert(u"roi_radius"_s, g.roiRadius);
    req.insert(u"reach_max"_s, g.reachMax);
    if (smoothed(g)) {
        req.insert(u"smooth_v"_s, g.smoothV);
        req.insert(u"smooth_xy"_s, g.smoothXy);
    }
    return req;
}

ReplayStep lassoGrowStep(const LassoGrowRequest &g)
{
    return ReplayStep{ u"/v1/cube/kinematic_select_bin"_s, lassoGrowRequestMap(g), QString() };
}

ReplayStep lassoApplyStep(const QString &mode, bool handEdited)
{
    ReplayStep apply;
    apply.route = u"/v1/cube/mask_apply_bin"_s;
    apply.request.insert(u"mode"_s, mode);
    apply.body = handEdited
            ? u"the hand-edited selection as a little-endian float32 mask, which only the "
              u"cube itself records (see replay_caveat); a replay of the grow alone posts "
              u"the previous call's response body %1"_s.arg(roiFromGrowResponse())
            : u"the previous call's response body, the selection as a little-endian "
              u"float32 mask, posted back %1"_s.arg(roiFromGrowResponse());
    return apply;
}

ReplayStep lassoFitStep(double sigma, int minRingPix)
{
    ReplayStep fit;
    fit.route = u"/v1/cube/kinematic_fit_bin"_s;
    if (sigma > 0.0)
        fit.request.insert(u"sigma"_s, sigma);
    fit.request.insert(u"min_ring_pix"_s, minRingPix);
    fit.body = u"the selection as a little-endian float32 mask, the response body of the "
               u"grow in its own recipe, posted %1"_s.arg(roiFromGrowResponse());
    return fit;
}

QVariantMap lassoDisplayParams(const LassoGrowRequest &g, const QString &noiseWhere,
                               bool handEdited, int trimBox)
{
    const QString shown = u"(%1, %2, %3)"_s;
    QVariantMap params;
    params.insert(u"Seed (z, y, x)"_s, shown.arg(g.seed[0]).arg(g.seed[1]).arg(g.seed[2]));
    // Extra seeds as coordinates, not counts: a count cannot be replayed.
    if (!g.addSeeds.isEmpty())
        params.insert(u"Extra seeds added"_s, seedList(g.addSeeds, u"; "_s, shown));
    if (!g.subSeeds.isEmpty())
        params.insert(u"Extra seeds carved"_s, seedList(g.subSeeds, u"; "_s, shown));
    params.insert(u"Reach"_s, QString::number(g.reach, 'g', 4));
    params.insert(u"Level-field budget (reach_max)"_s, QString::number(g.reachMax, 'g', 4));
    params.insert(u"Velocity coupling"_s, QString::number(g.lambdaV, 'f', 2));
    params.insert(u"Threshold"_s, u"%1σ"_s.arg(g.nsigma, 0, 'f', 1));
    // The estimate at the precision the grow received it, so a replay thresholds
    // the same voxels; the 4-digit display rounded it.
    params.insert(u"Noise σ"_s, g.sigma > 0.0
                          ? u"%1 (noise estimate, %2)"_s.arg(g.sigma, 0, 'g', 17).arg(noiseWhere)
                          : u"robust, whole cube"_s);
    params.insert(u"Smoothing (v, y, x)"_s,
                  smoothed(g) ? u"(%1, %2, %2)"_s.arg(g.smoothV).arg(g.smoothXy) : u"off"_s);
    params.insert(u"Hand-edited"_s, handEdited ? u"yes"_s : u"no"_s);
    // Only when it happened: a replay with the same service reproduces the cut,
    // a different node cap would not, so the record has to say it occurred.
    // With several seeds the value is the largest box any grow was cut to, so
    // the line says that rather than claim one exact domain.
    if (trimBox > 0)
        params.insert(u"Domain trimmed"_s,
                      u"yes: one or more grows exceeded the graph budget and were cut to "
                      u"a seed-centred box, largest half-width %1 voxels"_s.arg(trimBox));
    return params;
}

QStringList lassoHistoryItems(const LassoGrowRequest &g, bool handEdited, int trimBox)
{
    // The HISTORY form is compact ("(z,y,x)|(z,y,x)"): the recipe travels as a
    // query parameter and its items are split on ";".
    const QString compact = u"(%1,%2,%3)"_s;
    QStringList items;
    items << u"seed_zyx=%1,%2,%3"_s.arg(g.seed[0]).arg(g.seed[1]).arg(g.seed[2])
          << u"reach=%1"_s.arg(g.reach, 0, 'g', 4)
          << u"lambda_v=%1"_s.arg(g.lambdaV, 0, 'f', 2)
          << u"nsigma=%1"_s.arg(g.nsigma, 0, 'f', 1)
          << (g.sigma > 0.0 ? u"sigma=%1 (noise estimate)"_s.arg(g.sigma, 0, 'g', 17)
                            : u"sigma=robust (whole cube)"_s)
          << u"smooth_zyx=%1,%2,%2"_s.arg(g.smoothV).arg(g.smoothXy)
          << u"reach_max=%1"_s.arg(g.reachMax, 0, 'g', 4)
          << u"add_seeds=%1"_s.arg(g.addSeeds.isEmpty() ? u"none"_s
                                                        : seedList(g.addSeeds, u"|"_s, compact))
          << u"sub_seeds=%1"_s.arg(g.subSeeds.isEmpty() ? u"none"_s
                                                        : seedList(g.subSeeds, u"|"_s, compact))
          << u"hand_edited=%1"_s.arg(handEdited ? u"yes"_s : u"no"_s);
    if (trimBox > 0)
        items << u"domain_trimmed=largest box half-width %1 voxels"_s.arg(trimBox);
    return items;
}

} // namespace visivo
