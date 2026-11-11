#include "SharedSelection.h"

#include "BackendHeaders.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

namespace visivo {

namespace {

bool int3(const QJsonValue &v, std::array<int, 3> *out)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        // Integers only (as the page's entryReadable): 2.5 voxels is no box.
        if (!a[i].isDouble() || a[i].toDouble() != std::floor(a[i].toDouble())) return false;
        (*out)[i] = a[i].toInt();
    }
    return true;
}

bool seeds(const QJsonValue &v, QVector<std::array<int, 3>> *out)
{
    out->clear();
    if (v.isUndefined() || v.isNull()) return true;
    if (!v.isArray()) return false;
    for (const QJsonValue &s : v.toArray()) {
        std::array<int, 3> t{};
        if (!int3(s, &t)) return false;
        out->append(t);
    }
    return true;
}

QString fmtNumber(double v)
{
    const double a = std::fabs(v);
    if (a != 0.0 && (a < 1e-3 || a >= 1e5)) return QString::number(v, 'e', 3);
    return QString::number(v, 'g', 4);
}

} // namespace

bool lassoGrowRequestFromJson(const QJsonObject &o, LassoGrowRequest *out)
{
    LassoGrowRequest g;
    for (const char *k : { "seed_z", "seed_y", "seed_x", "reach", "lambda_v", "nsigma" })
        if (!o.value(QLatin1String(k)).isDouble()) return false;
    g.seed = { o.value("seed_z"_L1).toInt(), o.value("seed_y"_L1).toInt(), o.value("seed_x"_L1).toInt() };
    if (!seeds(o.value("add_seeds"_L1), &g.addSeeds) || !seeds(o.value("sub_seeds"_L1), &g.subSeeds))
        return false;
    g.reach = o.value("reach"_L1).toDouble();
    g.lambdaV = o.value("lambda_v"_L1).toDouble();
    g.sigma = std::max(0.0, o.value("sigma"_L1).toDouble(0.0));
    g.nsigma = o.value("nsigma"_L1).toDouble();
    g.roiRadius = o.value("roi_radius"_L1).toInt(0);
    // A grow sent without a level-field budget sized its shell by its reach.
    g.reachMax = o.value("reach_max"_L1).toDouble(g.reach);
    g.smoothV = o.value("smooth_v"_L1).toDouble(0.0);
    g.smoothXy = o.value("smooth_xy"_L1).toDouble(0.0);
    *out = g;
    return true;
}

SharedSelectionInfo parseSharedSelectionInfo(const QByteArray &json)
{
    SharedSelectionInfo r;
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = u"not a JSON object: %1"_s.arg(pe.errorString());
        return r;
    }
    const QJsonObject o = doc.object();
    // The seq and the instance even of an entry that cannot be read: the
    // link moves its cursor past it instead of asking for it again.
    r.instance = o.value("instance"_L1).toString();
    if (o.value("seq"_L1).isDouble()) r.seq = qint64(o.value("seq"_L1).toDouble());
    if (!o.value("seq"_L1).isDouble() || r.seq != o.value("seq"_L1).toDouble() || !int3(o.value("origin"_L1), &r.origin)
        || !int3(o.value("dims"_L1), &r.dims) || !int3(o.value("full_shape"_L1), &r.fullShape)) {
        r.error = u"a shared selection needs seq, origin, dims and full_shape"_s;
        return r;
    }
    r.seq = qint64(o.value("seq"_L1).toDouble());
    r.kind = o.value("kind"_L1).toString();
    r.source = o.value("source"_L1).toString();
    r.client = o.value("client"_L1).toString();
    r.datasetId = o.value("dataset_id"_L1).toString();
    r.count = qint64(o.value("count"_L1).toDouble());
    if (r.kind != "lasso"_L1 && r.kind != "box"_L1) {
        r.error = u"unknown selection kind '%1'"_s.arg(r.kind);
        return r;
    }
    const QJsonObject s = o.value("stats"_L1).toObject();
    r.statsValid = s.value("valid"_L1).toBool(false);
    r.statsError = s.value("error"_L1).toString();
    r.hasFlux = s.value("flux_jy_kms"_L1).isDouble();
    r.flux = s.value("flux_jy_kms"_L1).toDouble();
    r.hasSum = s.value("sum"_L1).isDouble();
    r.sum = s.value("sum"_L1).toDouble();
    r.bunit = s.value("bunit"_L1).toString();
    r.channelMin = s.value("channel_min"_L1).toInt(-1);
    r.channelMax = s.value("channel_max"_L1).toInt(-1);
    r.hasVelocity = s.value("velocity_min_kms"_L1).isDouble() && s.value("velocity_max_kms"_L1).isDouble();
    r.velocityMin = s.value("velocity_min_kms"_L1).toDouble();
    r.velocityMax = s.value("velocity_max_kms"_L1).toDouble();
    r.meta = o.value("meta"_L1).toObject();
    r.nonce = r.meta.value("nonce"_L1).toString();
    if (r.meta.value("grow"_L1).isObject())
        r.hasGrow = lassoGrowRequestFromJson(r.meta.value("grow"_L1).toObject(), &r.grow);
    if (r.kind == "box"_L1) {
        // The box IS its dims at its origin: what was measured and what the
        // mask route returns. The record's own `box` is not trusted.
        for (int i = 0; i < 3; ++i) {
            r.box[2 * i] = r.origin[i];
            r.box[2 * i + 1] = r.origin[i] + r.dims[i] - 1;
        }
        r.hasBox = true;
    }
    r.trimBox = r.meta.value("trim_box"_L1).toInt(0);
    r.valid = true;
    return r;
}

QString sharedSelectionRefusal(const SharedSelectionInfo &info, const QString &ownClient,
                               qint64 lastSeq, int fullW, int fullH, int fullD)
{
    if (!info.valid) return u"malformed: %1"_s.arg(info.error);
    if (info.seq <= lastSeq) return u"old"_s;
    if (!ownClient.isEmpty() && info.client == ownClient) return u"own"_s;
    // A shape not known yet (any axis 0) is not compared; the box must then
    // fit the entry's own cube (as the page's entryFits).
    const bool known = fullW > 0 && fullH > 0 && fullD > 0;
    const std::array<int, 3> full = known ? std::array<int, 3>{ fullW, fullH, fullD } : info.fullShape;
    for (int i = 0; i < 3; ++i) {
        if (known && info.fullShape[i] != full[i]) return u"another cube's shape"_s;
        if (info.dims[i] <= 0 || info.origin[i] < 0 || info.origin[i] + info.dims[i] > full[i])
            return u"outside the cube"_s;
    }
    return QString();
}

QJsonObject desktopLassoMeta(const LassoGrowRequest &grow, bool hasGrow, const QVariantMap &parameters,
                             bool handEdited, int trimBox)
{
    QJsonObject m;
    if (hasGrow) m.insert(u"grow"_s, QJsonObject::fromVariantMap(lassoGrowRequestMap(grow)));
    m.insert(u"parameters"_s, QJsonObject::fromVariantMap(parameters));
    m.insert(u"hand_edited"_s, handEdited);
    m.insert(u"trim_box"_s, trimBox);
    m.insert(u"made_in"_s, u"desktop"_s);
    return m;
}

QUrlQuery sharePublishQuery(const QString &datasetId, const QString &kind, const QString &client,
                            const std::array<int, 3> &origin, const std::array<int, 3> &dims,
                            const QJsonObject &meta)
{
    QUrlQuery q;
    q.addQueryItem(u"dataset_id"_s, datasetId);
    q.addQueryItem(u"kind"_s, kind);
    q.addQueryItem(u"source"_s, u"desktop"_s);
    q.addQueryItem(u"client"_s, client);
    q.addQueryItem(u"origin_x"_s, QString::number(origin[0]));
    q.addQueryItem(u"origin_y"_s, QString::number(origin[1]));
    q.addQueryItem(u"origin_z"_s, QString::number(origin[2]));
    q.addQueryItem(u"width"_s, QString::number(dims[0]));
    q.addQueryItem(u"height"_s, QString::number(dims[1]));
    q.addQueryItem(u"depth"_s, QString::number(dims[2]));
    // Every character but the unreserved ones escaped, so a '+' or '&' in a
    // Provenance label survives the query.
    q.addQueryItem(u"meta"_s, QString::fromLatin1(
            QJsonDocument(meta).toJson(QJsonDocument::Compact).toPercentEncoding()));
    return q;
}

bool shareRouteUnsupported(int status, const QByteArray &body)
{
    if (status == 405) return true;
    if (status != 404) return false;
    const QJsonDocument d = QJsonDocument::fromJson(body);
    return d.isObject() && d.object().value("detail"_L1).toString() == "Not Found"_L1;
}

qint64 sharedMaskCount(const QByteArray &mask, qint64 voxels)
{
    if (voxels <= 0 || mask.size() != voxels * qint64(sizeof(float))) return -1;
    qint64 n = 0;
    const char *p = mask.constData();
    for (qint64 i = 0; i < voxels; ++i) {
        float v;
        std::memcpy(&v, p + i * 4, 4);   // the wire is little-endian, as is every host we build for
        if (std::isfinite(v) && v != 0.0f) ++n;
    }
    return n;
}

QString sharedSelectionNote(const SharedSelectionInfo &info)
{
    const QString who = info.source == "headset"_L1 ? u"the headset"_s : u"another desktop"_s;
    const QString what = info.kind == "box"_L1 ? u"Box"_s : u"Kinematic Lasso"_s;
    QString t = u"%1 from %2: %3 voxels"_s.arg(what, who).arg(info.count);
    if (info.hasFlux) t += u", flux %1 Jy km/s"_s.arg(fmtNumber(info.flux));
    else if (info.hasSum) t += u", sum %1 %2"_s.arg(fmtNumber(info.sum), info.bunit).trimmed();
    if (info.hasVelocity)
        t += u", %1 to %2 km/s"_s.arg(info.velocityMin, 0, 'f', 1).arg(info.velocityMax, 0, 'f', 1);
    return t;
}

ReplayStep sharedBoxApplyStep(const std::array<int, 6> &box, const QString &mode)
{
    // The one replay step that records an ROI of its own, deliberately: for a
    // box the ROI IS the selection (every voxel of it), and there is no grow
    // response to take it from. Every other apply takes its ROI from a
    // response (the grow's, or the mask route's), as the recipe rule says.
    QVariantMap req;
    req.insert(u"origin_x"_s, box[0]);
    req.insert(u"origin_y"_s, box[2]);
    req.insert(u"origin_z"_s, box[4]);
    req.insert(u"width"_s, box[1] - box[0] + 1);
    req.insert(u"height"_s, box[3] - box[2] + 1);
    req.insert(u"depth"_s, box[5] - box[4] + 1);
    req.insert(u"mode"_s, mode);
    return ReplayStep{ u"/v1/cube/mask_apply_bin"_s, req,
                       u"a little-endian float32 mask of ones over the box (width x height x "
                       u"depth values, x fastest): every voxel of the box"_s };
}

QVariantMap sharedBoxDisplayParams(const std::array<int, 6> &box, const QVariantMap &creatorParams)
{
    QVariantMap p;
    p.insert(u"Region"_s, u"box"_s);
    p.insert(u"Pixels"_s, u"x %1..%2, y %3..%4"_s.arg(box[0]).arg(box[1]).arg(box[2]).arg(box[3]));
    p.insert(u"Channels"_s, u"%1..%2"_s.arg(box[4]).arg(box[5]));
    if (creatorParams.contains(u"Channels from"_s))
        p.insert(u"Channels from"_s, creatorParams.value(u"Channels from"_s));
    p.insert(u"Made in"_s, u"VR headset"_s);
    return p;
}

QStringList sharedBoxHistoryItems(const std::array<int, 6> &box)
{
    return { u"selection=box"_s,
             u"box_x=%1..%2"_s.arg(box[0]).arg(box[1]),
             u"box_y=%1..%2"_s.arg(box[2]).arg(box[3]),
             u"box_z=%1..%2"_s.arg(box[4]).arg(box[5]),
             u"made_in=VR headset"_s };
}

ReconcileAction reconcileDecision(const SharedSelectionInfo *latest, const ShareViewState &s)
{
    if (!latest || !latest->valid) return ReconcileAction::None;
    if (!latest->nonce.isEmpty() && latest->nonce == s.shownNonce) return ReconcileAction::None;
    if (latest->client == s.client)
        return !latest->nonce.isEmpty() && latest->nonce == s.localNonce ? ReconcileAction::Own
                                                                           : ReconcileAction::Skip;
    if (latest->seq <= s.floorSeq) return ReconcileAction::None;
    // Of this cube's shape, its box inside it (the client and order checks
    // are the ones above, so they are passed as never refusing).
    if (!sharedSelectionRefusal(*latest, QString(), 0, s.full[0], s.full[1], s.full[2]).isEmpty())
        return ReconcileAction::None;
    return ReconcileAction::Fetch;
}

ShareViewState noteLocal(ShareViewState s, qint64 seenSeq)
{
    s.shownNonce.clear();
    s.localNonce.clear();
    s.floorSeq = std::max(s.floorSeq, seenSeq);
    return s;
}

PublishOutcome publishOutcomeOf(int status)
{
    if (status == 200) return PublishOutcome::Kept;
    if (status == 409) return PublishOutcome::Stale;
    if (status == 429) return PublishOutcome::Retry;
    if (status >= 400 && status < 500) return PublishOutcome::Refused;
    return PublishOutcome::Unknown;
}

InstanceVerdict instanceVerdict(const QString &known, const QString &seen)
{
    if (seen.isEmpty()) return InstanceVerdict::Same;
    if (known.isEmpty()) return InstanceVerdict::Adopt;
    return known == seen ? InstanceVerdict::Same : InstanceVerdict::Reset;
}

bool confirmShareDraw(const SharedSelectionInfo *latestNow, qint64 fetchedSeq,
                      const QString &shownBefore, qint64 floorBefore,
                      const QString &shownNow, qint64 floorNow)
{
    return latestNow && latestNow->valid && latestNow->seq == fetchedSeq
            && shownNow == shownBefore && floorNow == floorBefore;
}

LassoApplyGate lassoApplyGate(bool hasSeed, bool maskEmpty, const SharedSelectionOrigin &from)
{
    if (maskEmpty) return LassoApplyGate::NoSelection;
    if (!hasSeed && !from.received) return LassoApplyGate::NoSeed;
    return LassoApplyGate::Ok;
}

namespace {
QString madeIn(const SharedSelectionOrigin &from)
{
    if (from.own) return u"this desktop"_s;
    return from.source == "headset"_L1 ? u"VR headset"_s : u"another desktop"_s;
}

const QString kHandEditedCaveat =
        u"The mask was edited by hand after the grow: the parameters regrow only its geodesic "
        u"part. The cube itself, with its HISTORY, is the exact record."_s;
} // namespace

LassoApplyRecord lassoApplyRecord(const SharedSelectionOrigin &from, const LassoGrowRequest &grow,
                                  const QString &noiseWhere, bool handEdited, int trimBox,
                                  const QString &mode)
{
    LassoApplyRecord r;
    if (from.received && from.kind == "box"_L1) {
        r.params = sharedBoxDisplayParams(from.box, from.parameters);
        r.params.insert(u"Made in"_s, madeIn(from));
        r.history = sharedBoxHistoryItems(from.box);
        r.replay = { sharedBoxApplyStep(from.box, mode) };
        if (handEdited)
            r.caveat = u"The mask was edited by hand after the box was made: the box gives only "
                       u"its unedited part. The cube itself, with its HISTORY, is the exact record."_s;
        return r;
    }
    if (from.received && !from.hasGrow) {
        // Nothing to regrow it from: the mask is the record.
        r.params = from.parameters;
        r.params.insert(u"Made in"_s, madeIn(from));
        r.params.insert(u"Recorded as"_s, u"the mask only (received without its grow request)"_s);
        r.params.insert(u"Hand-edited"_s, handEdited ? u"yes"_s : u"no"_s);
        r.history = { u"selection=mask"_s, u"made_in=%1"_s.arg(madeIn(from)) };
        ReplayStep apply;
        apply.route = u"/v1/cube/mask_apply_bin"_s;
        apply.request.insert(u"mode"_s, mode);
        apply.body = u"the selection as a little-endian float32 mask, received through the "
                     u"session's selection hub (GET /v1/share/selection/mask) without the request "
                     u"it was made with, which only the cube itself records (see replay_caveat); "
                     u"posted with origin_x, origin_y, origin_z from that response's %1 header "
                     u"and width, height, depth from %2, %3, %4"_s
                             .arg(QLatin1String(VisivoHeaders::ORIGIN), QLatin1String(VisivoHeaders::WIDTH),
                                  QLatin1String(VisivoHeaders::HEIGHT), QLatin1String(VisivoHeaders::DEPTH));
        r.replay = { apply };
        r.caveat = u"The selection was received without the request it was made with: no call "
                   u"regenerates it. The cube itself, with its HISTORY, is the exact record."_s;
        return r;
    }
    // Grown here, or received with its creator's grow: that grow is the record.
    r.params = lassoDisplayParams(grow, noiseWhere, handEdited, trimBox);
    r.history = lassoHistoryItems(grow, handEdited, trimBox);
    if (from.received) {
        r.params.insert(u"Made in"_s, madeIn(from));
        r.history << u"made_in=%1"_s.arg(madeIn(from));
    }
    r.replay = { lassoGrowStep(grow), lassoApplyStep(mode, handEdited) };
    if (handEdited) r.caveat = kHandEditedCaveat;
    return r;
}

} // namespace visivo
