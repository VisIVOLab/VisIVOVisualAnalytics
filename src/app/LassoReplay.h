#ifndef LASSOREPLAY_H
#define LASSOREPLAY_H

// LassoReplay — the Kinematic Lasso's grow as a record. The request a
// selection was grown with is kept beside the mask it produced (not read back
// from the controls, which may have moved on), and takes three forms when the
// mask becomes a cube: the replay steps of the exported recipe, the Provenance
// tab's parameters and the FITS HISTORY items. QtCore only, so the field names
// can be checked against the service's request model.

#include "ProductRegistry.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <array>

namespace visivo {

/// What POST /v1/cube/kinematic_select_bin was, or would be, sent for the mask
/// on screen. `reachMax` is the budget the graph was built with — the level
/// field's reach_max when the mask is a level set of it, the reach itself for
/// a single-budget grow — which sizes the faint shell, so a replay must pass
/// it. `sigma` 0 means the service measured its own.
struct LassoGrowRequest
{
    std::array<int, 3> seed{ 0, 0, 0 };   ///< z, y, x
    QVector<std::array<int, 3>> addSeeds;
    QVector<std::array<int, 3>> subSeeds;
    double reach{ 8.0 };
    double lambdaV{ 1.0 };
    double sigma{ 0.0 };
    double nsigma{ 2.5 };
    int roiRadius{ 0 };
    double reachMax{ 0.0 };
    double smoothV{ 0.0 };
    double smoothXy{ 0.0 };
};

/// The request under the API's field names, as the client sends it: the seed
/// lists and the smoothing only when set.
QVariantMap lassoGrowRequestMap(const LassoGrowRequest &g);

/// The two calls of the exported recipe: the grow, then the apply of the mask
/// it returns. The apply's ROI is the grow's response, not a recorded number.
ReplayStep lassoGrowStep(const LassoGrowRequest &g);
ReplayStep lassoApplyStep(const QString &mode, bool handEdited);

/// The fit of the selection (POST /v1/cube/kinematic_fit_bin): the mask is the
/// body, with the ROI of the grow that made it, so the record carries the fit's
/// own parameters only. @p sigma 0 lets the service measure its own.
ReplayStep lassoFitStep(double sigma, int minRingPix);

/// The Provenance tab's label → value map for the selection. @p noiseWhere
/// says where a supplied σ came from; @p trimBox > 0 is the largest half-width
/// of a seed-centred box the service cut a grow's domain to (node cap), 0 when
/// it cut none — a line is added only when it did.
QVariantMap lassoDisplayParams(const LassoGrowRequest &g, const QString &noiseWhere,
                               bool handEdited, int trimBox = 0);

/// The "key=value" items written to the new cube's FITS HISTORY, one card each
/// (ASCII only: a card cannot carry ±).
QStringList lassoHistoryItems(const LassoGrowRequest &g, bool handEdited, int trimBox = 0);

} // namespace visivo

#endif // LASSOREPLAY_H
