#ifndef SHAREDSELECTION_H
#define SHAREDSELECTION_H

// SharedSelection — a selection shared between the VR headset's page and the
// cube viewer (issue #188), as the backend's selection hub describes it
// (GET /v1/share/selection): which side made it, its box, the statistics the
// backend measured on the full cube when it was shared, and the creator's
// record (the Kinematic Lasso's grow request under the API's names, or the
// box's range). The side that receives a selection presents it with that
// record attached and records no step of its own; an Apply of it records the
// creator's grow (or the box) as the replay. QtCore only, for the tests.

#include "LassoReplay.h"
#include "ProductRegistry.h"

#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QUrlQuery>
#include <QVariantMap>

#include <array>

namespace visivo {

/// Where the selection on screen came from, kept beside its mask.
struct SharedSelectionOrigin
{
    bool received{ false };          ///< made on the other side, not here
    bool own{ false };               ///< this window's own selection, drawn back from the hub
    QString source;                  ///< "headset" | "desktop"
    QString kind;                    ///< "lasso" | "box"
    bool hasGrow{ false };           ///< a received lasso came with the request it was grown with
    std::array<int, 6> box{ 0, 0, 0, 0, 0, 0 };   ///< a box: x0, x1, y0, y1, z0, z1 (inclusive)
    QVariantMap parameters;          ///< the creator's Provenance labels
    qint64 seq{ 0 };
};

struct SharedSelectionInfo
{
    bool valid{ false };
    QString error;
    qint64 seq{ 0 };
    QString kind, source, client, datasetId;
    std::array<int, 3> origin{ 0, 0, 0 };    ///< x, y, z
    std::array<int, 3> dims{ 0, 0, 0 };      ///< width, height, depth
    std::array<int, 3> fullShape{ 0, 0, 0 };
    qint64 count{ 0 };
    // The statistics measured on the full cube when it was shared.
    bool statsValid{ false };
    QString statsError;
    double flux{ 0.0 };
    bool hasFlux{ false };
    double sum{ 0.0 };
    bool hasSum{ false };
    QString bunit;
    int channelMin{ -1 }, channelMax{ -1 };
    double velocityMin{ 0.0 }, velocityMax{ 0.0 };
    bool hasVelocity{ false };
    QJsonObject meta;
    QString nonce;                   ///< meta.nonce: the publish's own id, '' when it sent none
    QString instance;                ///< the backend instance (seqs restart with it)
    bool hasGrow{ false };
    LassoGrowRequest grow;           ///< meta.grow, when the creator grew it
    bool hasBox{ false };            ///< kind "box": its range, always from origin and dims
    std::array<int, 6> box{ 0, 0, 0, 0, 0, 0 };
    int trimBox{ 0 };
};

/// GET /v1/share/selection's JSON. A box's range is its origin and dims
/// (never the record's own `box`, which the backend checks against them).
SharedSelectionInfo parseSharedSelectionInfo(const QByteArray &json);

/// The grow request from its API map (the inverse of lassoGrowRequestMap).
/// False when a required field is missing or not a number.
bool lassoGrowRequestFromJson(const QJsonObject &o, LassoGrowRequest *out);

/// Whether a selection read from the hub is to be presented here: not this
/// client's own (its echo), newer than the last one handled, of this cube's
/// shape (not compared while the shape is unknown: an axis 0), with a box
/// inside it (the cube's, else the entry's own). Empty when it is; else why not.
QString sharedSelectionRefusal(const SharedSelectionInfo &info, const QString &ownClient,
                               qint64 lastSeq, int fullW, int fullH, int fullD);

/// What the desktop tells the headset about the lasso on screen: the grow
/// under the API's names (absent when there is no grow behind the mask), the
/// Provenance labels, whether it was edited by hand.
QJsonObject desktopLassoMeta(const LassoGrowRequest &grow, bool hasGrow, const QVariantMap &parameters,
                             bool handEdited, int trimBox);

/// The query of POST /v1/share/selection.
QUrlQuery sharePublishQuery(const QString &datasetId, const QString &kind, const QString &client,
                            const std::array<int, 3> &origin, const std::array<int, 3> &dims,
                            const QJsonObject &meta);

/// The one rule both clients follow (as the page's share.js
/// reconcileDecision): what is shown converges to the hub's latest entry for
/// this cube. The state is what this client shows: the nonce of what is on
/// screen (own or received), the nonce of its latest own selection, and the
/// floor (entries at or below it were read and then replaced by a local
/// selection, or were there before this client followed the hub).
struct ShareViewState
{
    QString client;
    QString shownNonce;
    QString localNonce;
    qint64 floorSeq{ 0 };
    int full[3]{ 0, 0, 0 };
};
enum class ReconcileAction
{
    None,    ///< on screen already, dismissed, or not for this cube: nothing
    Skip,    ///< an older own selection: a newer local one is about to replace it
    Fetch,   ///< another client's: fetch its mask, confirm it is still the latest, draw it
    Own,     ///< this client's latest own selection, not on screen: draw it back
};
ReconcileAction reconcileDecision(const SharedSelectionInfo *latest, const ShareViewState &s);

/// A selection made here and not published (or a Clear) replaced what was on
/// screen: nothing shown or own any more, and the entries read so far stay
/// off the screen (a newer one from another client still replaces it).
ShareViewState noteLocal(ShareViewState s, qint64 seenSeq);

/// What a publish's HTTP status means (as the page's publishOutcomeOf): kept
/// (200); stale (409, a later one is the hub's); retry (429: once, after a
/// short delay); refused (another 4xx: the hub will never hold it, e.g. over
/// the voxel cap: it stays local and is no longer this client's latest own);
/// unknown (5xx, no answer, a timeout: the hub may have kept it).
enum class PublishOutcome { Kept, Stale, Retry, Refused, Unknown, Cleared };
/// @p detail is the answer's detail: a 409 "cleared: …" (the cube's selection
/// was cleared after this publish arrived) is Cleared, refused like a 4xx.
PublishOutcome publishOutcomeOf(int status, const QString &detail = QString());

/// The hub's backend instance against the one this client knows: Reset when
/// it changed (a restart: seqs began again at 1, so the cursor, the floor and
/// the nonces of the old one mean nothing).
enum class InstanceVerdict { Adopt, Same, Reset };
InstanceVerdict instanceVerdict(const QString &known, const QString &seen);

/// After the mask was fetched: draw only if the entry is still the hub's
/// latest and nothing local happened meanwhile (no new local selection, no
/// dismissal).
bool confirmShareDraw(const SharedSelectionInfo *latestNow, qint64 fetchedSeq,
                      const QString &shownBefore, qint64 floorBefore,
                      const QString &shownNow, qint64 floorNow);

/// Whether an answer of the hub's routes says the backend has no hub at all
/// (an older backend: 405, or FastAPI's bare {"detail":"Not Found"} for an
/// unknown route), rather than an unknown session or cube, which is retried.
bool shareRouteUnsupported(int status, const QByteArray &body);

/// Voxels set in a float32 0/1 mask (a value > 0.5, as the backend reads a
/// mask; NaN is not), -1 when the
/// byte count is not @p voxels floats.
qint64 sharedMaskCount(const QByteArray &mask, qint64 voxels);

/// The non-modal note of a selection received, e.g. "Kinematic Lasso from the
/// headset: 13216 voxels, flux 6.061 Jy km/s".
QString sharedSelectionNote(const SharedSelectionInfo &info);

/// The record of a box made in the headset and applied here (the box's own
/// call: a mask of ones over it, posted to mask_apply_bin), its Provenance
/// labels and its FITS HISTORY items. The box's step is the one apply that
/// records its ROI (origin and dims): for a box the ROI is the selection and
/// there is no grow response to take it from.
ReplayStep sharedBoxApplyStep(const std::array<int, 6> &box, const QString &mode);
QVariantMap sharedBoxDisplayParams(const std::array<int, 6> &box, const QVariantMap &creatorParams);
QStringList sharedBoxHistoryItems(const std::array<int, 6> &box);

/// Whether Isolate / Remove may act on the selection on screen: a mask, made
/// here from a seed or received from the other side (which has no seed here:
/// the controls must not regrow it).
enum class LassoApplyGate { Ok, NoSelection, NoSeed };
LassoApplyGate lassoApplyGate(bool hasSeed, bool maskEmpty, const SharedSelectionOrigin &from);

/// What an Isolate / Remove of the selection on screen records: the
/// Provenance labels, the FITS HISTORY items, the replay steps and the
/// replay caveat. Made here: the grow, then the apply. Received: a box as the
/// box; a lasso with its creator's grow as that grow (labelled with where it
/// was made); a lasso without one as the mask alone, with a caveat (never a
/// default request, which would replay another selection). No step records an
/// ROI of its own.
struct LassoApplyRecord
{
    QVariantMap params;
    QStringList history;
    QList<ReplayStep> replay;
    QString caveat;
};
LassoApplyRecord lassoApplyRecord(const SharedSelectionOrigin &from, const LassoGrowRequest &grow,
                                  const QString &noiseWhere, bool handEdited, int trimBox,
                                  const QString &mode);

} // namespace visivo

Q_DECLARE_METATYPE(visivo::SharedSelectionInfo)

#endif // SHAREDSELECTION_H
