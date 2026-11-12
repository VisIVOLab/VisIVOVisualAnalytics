// test_shared_selection.cpp
// A selection made in the VR headset reaches the cube viewer with the record
// it was made with (issue #188): the grow request survives the trip under the
// API's names, so an Apply here records the headset's grow; the hub's answer
// is read as the backend writes it (backend/app/selection_hub.py info()).

#include "TestSharedSelection.h"

#include "SharedSelection.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <QUrl>

#include <cstring>
#include <limits>

using namespace Qt::StringLiterals;
using visivo::LassoGrowRequest;

namespace {

LassoGrowRequest fullRequest()
{
    LassoGrowRequest g;
    g.seed = { 69, 83, 80 };
    g.addSeeds = { { 70, 90, 91 } };
    g.subSeeds = { { 60, 70, 71 }, { 61, 72, 73 } };
    g.reach = 8.0;
    g.lambdaV = 1.5;
    g.sigma = 0.0012345678901234567;
    g.nsigma = 2.5;
    g.roiRadius = 3;
    g.reachMax = 64.0;
    g.smoothV = 2.0;
    g.smoothXy = 6.0;
    return g;
}

QByteArray hubAnswer(const QString &kind = u"lasso"_s, const QJsonObject &meta = {})
{
    // The JSON SharedSelection.info() writes.
    QJsonObject stats{ { u"valid"_s, true }, { u"error"_s, QString() },
                       { u"selected_voxels"_s, 13216 }, { u"sum"_s, 812.5 },
                       { u"bunit"_s, u"Jy/beam"_s }, { u"flux_jy_kms"_s, 6.0614 },
                       { u"channel_min"_s, 40 }, { u"channel_max"_s, 71 },
                       { u"velocity_min_kms"_s, 1240.4 }, { u"velocity_max_kms"_s, 1500.9 } };
    QJsonObject o{ { u"seq"_s, 12 }, { u"kind"_s, kind }, { u"source"_s, u"headset"_s },
                   { u"client"_s, u"vr-abc"_s }, { u"dataset_id"_s, u"ds_1"_s },
                   { u"origin"_s, QJsonArray{ 10, 20, 30 } }, { u"dims"_s, QJsonArray{ 4, 5, 6 } },
                   { u"full_shape"_s, QJsonArray{ 100, 110, 120 } }, { u"count"_s, 13216 },
                   { u"age_s"_s, 0.2 }, { u"stats"_s, stats }, { u"meta"_s, meta } };
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

} // namespace

void TestSharedSelection::growRequest_roundTripsThroughTheApiMap()
{
    const LassoGrowRequest g = fullRequest();
    const QJsonObject o = QJsonObject::fromVariantMap(visivo::lassoGrowRequestMap(g));
    LassoGrowRequest back;
    QVERIFY(visivo::lassoGrowRequestFromJson(o, &back));
    QCOMPARE(back.seed, g.seed);
    QCOMPARE(back.addSeeds, g.addSeeds);
    QCOMPARE(back.subSeeds, g.subSeeds);
    QCOMPARE(back.reach, g.reach);
    QCOMPARE(back.lambdaV, g.lambdaV);
    QCOMPARE(back.sigma, g.sigma);
    QCOMPARE(back.nsigma, g.nsigma);
    QCOMPARE(back.roiRadius, g.roiRadius);
    QCOMPARE(back.reachMax, g.reachMax);
    QCOMPARE(back.smoothV, g.smoothV);
    QCOMPARE(back.smoothXy, g.smoothXy);
    // And the map it records is the same map the headset sent.
    QCOMPARE(visivo::lassoGrowRequestMap(back), visivo::lassoGrowRequestMap(g));
}

void TestSharedSelection::growRequest_readsTheVrPagesDefaultRequest()
{
    // recipe.js lassoGrowRequest([z, y, x]) with LASSO_DEFAULTS.
    const QJsonObject page = QJsonDocument::fromJson(
            R"({"seed_z":57,"seed_y":250,"seed_x":309,"reach":8,"lambda_v":1,"sigma":0,)"
            R"("nsigma":2.5,"roi_radius":0,"reach_max":64})").object();
    LassoGrowRequest g;
    QVERIFY(visivo::lassoGrowRequestFromJson(page, &g));
    QCOMPARE(g.seed, (std::array<int, 3>{ 57, 250, 309 }));
    QVERIFY(g.addSeeds.isEmpty() && g.subSeeds.isEmpty());
    QCOMPARE(g.reachMax, 64.0);
    QCOMPARE(g.smoothV, 0.0);
    // The desktop records exactly the request the page sent.
    QCOMPARE(QJsonObject::fromVariantMap(visivo::lassoGrowRequestMap(g)), page);
    // A grow without its reach is not a grow.
    QJsonObject bad = page;
    bad.remove(u"reach"_s);
    QVERIFY(!visivo::lassoGrowRequestFromJson(bad, &g));
    bad = page;
    bad.insert(u"add_seeds"_s, QJsonArray{ QJsonArray{ 1, 2 } });
    QVERIFY(!visivo::lassoGrowRequestFromJson(bad, &g));
}

void TestSharedSelection::info_parsesTheHubsAnswer()
{
    const QJsonObject grow = QJsonObject::fromVariantMap(visivo::lassoGrowRequestMap(fullRequest()));
    const QJsonObject meta{ { u"grow"_s, grow }, { u"trim_box"_s, 40 },
                            { u"parameters"_s, QJsonObject{ { u"Reach"_s, u"8"_s } } } };
    const auto i = visivo::parseSharedSelectionInfo(hubAnswer(u"lasso"_s, meta));
    QVERIFY2(i.valid, qPrintable(i.error));
    QCOMPARE(i.seq, 12);
    QCOMPARE(i.kind, u"lasso"_s);
    QCOMPARE(i.source, u"headset"_s);
    QCOMPARE(i.client, u"vr-abc"_s);
    QCOMPARE(i.origin, (std::array<int, 3>{ 10, 20, 30 }));
    QCOMPARE(i.dims, (std::array<int, 3>{ 4, 5, 6 }));
    QCOMPARE(i.fullShape, (std::array<int, 3>{ 100, 110, 120 }));
    QCOMPARE(i.count, 13216);
    QVERIFY(i.statsValid && i.hasFlux && i.hasVelocity);
    QCOMPARE(i.flux, 6.0614);
    QCOMPARE(i.channelMin, 40);
    QCOMPARE(i.channelMax, 71);
    QVERIFY(i.hasGrow);
    QCOMPARE(i.grow.seed, fullRequest().seed);
    QCOMPARE(i.grow.subSeeds, fullRequest().subSeeds);
    QCOMPARE(i.trimBox, 40);
    QVERIFY(!i.hasBox);
}

void TestSharedSelection::info_aBoxTakesItsRangeFromTheRecordOrItsBox()
{
    // The box is its origin and dims; a record naming another box is not trusted.
    const QJsonObject meta{ { u"box"_s, QJsonArray{ QJsonArray{ 0, 99 }, QJsonArray{ 0, 99 },
                                                     QJsonArray{ 0, 99 } } } };
    auto i = visivo::parseSharedSelectionInfo(hubAnswer(u"box"_s, meta));
    QVERIFY(i.valid && i.hasBox && !i.hasGrow);
    QCOMPARE(i.box, (std::array<int, 6>{ 10, 13, 20, 24, 30, 35 }));
    i = visivo::parseSharedSelectionInfo(hubAnswer(u"box"_s));
    QVERIFY(i.valid && i.hasBox);
    QCOMPARE(i.box, (std::array<int, 6>{ 10, 13, 20, 24, 30, 35 }));
    // A lasso has no box, whatever its record says.
    QVERIFY(!visivo::parseSharedSelectionInfo(hubAnswer(u"lasso"_s, meta)).hasBox);
}

void TestSharedSelection::info_refusesMalformedAnswers()
{
    QVERIFY(!visivo::parseSharedSelectionInfo("not json").valid);
    QVERIFY(!visivo::parseSharedSelectionInfo("[1,2]").valid);
    QVERIFY(!visivo::parseSharedSelectionInfo(R"({"seq":1,"origin":[0,0],"dims":[1,1,1],"full_shape":[1,1,1]})").valid);
    QVERIFY(!visivo::parseSharedSelectionInfo(hubAnswer(u"sphere"_s)).valid);
}

void TestSharedSelection::refusal_skipsOwnOldAndOtherCubes()
{
    const auto i = visivo::parseSharedSelectionInfo(hubAnswer());
    QCOMPARE(visivo::sharedSelectionRefusal(i, u"desktop-1"_s, 11, 100, 110, 120), QString());
    QCOMPARE(visivo::sharedSelectionRefusal(i, u"desktop-1"_s, 12, 100, 110, 120), u"old"_s);
    QCOMPARE(visivo::sharedSelectionRefusal(i, u"vr-abc"_s, 0, 100, 110, 120), u"own"_s);
    QCOMPARE(visivo::sharedSelectionRefusal(i, u"desktop-1"_s, 0, 100, 110, 121), u"another cube's shape"_s);
    auto out = i;
    out.origin = { 98, 20, 30 };
    QCOMPARE(visivo::sharedSelectionRefusal(out, u"desktop-1"_s, 0, 100, 110, 120), u"outside the cube"_s);
    QVERIFY(visivo::sharedSelectionRefusal(visivo::SharedSelectionInfo{}, u"d"_s, 0, 1, 1, 1).startsWith(u"malformed"_s));
}

void TestSharedSelection::publishQuery_keepsTheRecordIntactOnTheWire()
{
    QVariantMap params{ { u"Threshold"_s, u"2.5σ"_s }, { u"Note"_s, u"a+b & c=d %"_s } };
    const QJsonObject meta = visivo::desktopLassoMeta(fullRequest(), true, params, false, 0);
    QUrl url(u"http://127.0.0.1:8000/v1/share/selection"_s);
    url.setQuery(visivo::sharePublishQuery(u"ds_1"_s, u"lasso"_s, u"desktop-x"_s, { 1, 2, 3 }, { 4, 5, 6 }, meta));
    const QByteArray wire = url.toEncoded();
    // The server splits on '&' and '=', then turns '+' into a space and
    // decodes %XX once (application/x-www-form-urlencoded).
    QMap<QString, QString> got;
    for (const QByteArray &item : wire.mid(wire.indexOf('?') + 1).split('&')) {
        const qsizetype eq = item.indexOf('=');
        QByteArray v = item.mid(eq + 1);
        v.replace('+', ' ');
        got.insert(QString::fromUtf8(item.left(eq)), QString::fromUtf8(QByteArray::fromPercentEncoding(v)));
    }
    QCOMPARE(got.value(u"dataset_id"_s), u"ds_1"_s);
    QCOMPARE(got.value(u"source"_s), u"desktop"_s);
    QCOMPARE(got.value(u"client"_s), u"desktop-x"_s);
    QCOMPARE(got.value(u"origin_z"_s), u"3"_s);
    QCOMPARE(got.value(u"depth"_s), u"6"_s);
    const QJsonObject back = QJsonDocument::fromJson(got.value(u"meta"_s).toUtf8()).object();
    QCOMPARE(back, meta);
    QCOMPARE(back.value(u"parameters"_s).toObject().value(u"Note"_s).toString(), u"a+b & c=d %"_s);
    LassoGrowRequest g;
    QVERIFY(visivo::lassoGrowRequestFromJson(back.value(u"grow"_s).toObject(), &g));
    QCOMPARE(g.seed, fullRequest().seed);
    // Without a grow (a box received from the headset) the record has none.
    QVERIFY(!visivo::desktopLassoMeta(fullRequest(), false, {}, false, 0).contains(u"grow"_s));
}

void TestSharedSelection::maskCount_countsSetVoxelsAndChecksTheSize()
{
    // The backend's rule (> 0.5): 0.5 and 0.25 are not selected, nor is NaN.
    const float v[] = { 1.f, 0.f, 1.f, std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.25f };
    QByteArray m(reinterpret_cast<const char *>(v), sizeof(v));
    QCOMPARE(visivo::sharedMaskCount(m, 6), 2);
    QCOMPARE(visivo::sharedMaskCount(m, 5), -1);
    QCOMPARE(visivo::sharedMaskCount(QByteArray(), 0), -1);
}

void TestSharedSelection::note_saysWhoVoxelsAndFlux()
{
    const auto i = visivo::parseSharedSelectionInfo(hubAnswer());
    QCOMPARE(visivo::sharedSelectionNote(i),
             u"Kinematic Lasso from the headset: 13216 voxels, flux 6.061 Jy km/s, 1240.4 to 1500.9 km/s"_s);
}

void TestSharedSelection::boxRecord_isTheBoxsOwnCall()
{
    const std::array<int, 6> box{ 10, 13, 20, 24, 30, 35 };
    const ReplayStep s = visivo::sharedBoxApplyStep(box, u"blank_outside"_s);
    QCOMPARE(s.route, u"/v1/cube/mask_apply_bin"_s);
    QCOMPARE(s.request.value(u"origin_x"_s).toInt(), 10);
    QCOMPARE(s.request.value(u"origin_y"_s).toInt(), 20);
    QCOMPARE(s.request.value(u"origin_z"_s).toInt(), 30);
    QCOMPARE(s.request.value(u"width"_s).toInt(), 4);
    QCOMPARE(s.request.value(u"height"_s).toInt(), 5);
    QCOMPARE(s.request.value(u"depth"_s).toInt(), 6);
    QCOMPARE(s.request.value(u"mode"_s).toString(), u"blank_outside"_s);
    QVERIFY(s.body.contains(u"ones over the box"_s));
    const QVariantMap p = visivo::sharedBoxDisplayParams(box, { { u"Channels from"_s, u"the two corners"_s } });
    QCOMPARE(p.value(u"Pixels"_s).toString(), u"x 10..13, y 20..24"_s);
    QCOMPARE(p.value(u"Channels"_s).toString(), u"30..35"_s);
    QCOMPARE(p.value(u"Channels from"_s).toString(), u"the two corners"_s);
    QCOMPARE(p.value(u"Made in"_s).toString(), u"VR headset"_s);
    QCOMPARE(visivo::sharedBoxHistoryItems(box).first(), u"selection=box"_s);
}

namespace {
visivo::SharedSelectionOrigin received(const QString &kind, bool hasGrow)
{
    visivo::SharedSelectionOrigin o;
    o.received = true;
    o.source = u"headset"_s;
    o.kind = kind;
    o.hasGrow = hasGrow;
    o.box = { 10, 13, 20, 24, 30, 35 };
    o.parameters = { { u"Channels from"_s, u"the two corners"_s }, { u"Reach"_s, u"8"_s } };
    return o;
}

bool anyRoi(const QList<ReplayStep> &steps)
{
    // An apply never records an ROI of its own: only a box's own call names one.
    for (const ReplayStep &s : steps)
        if (s.route == u"/v1/cube/mask_apply_bin"_s && s.body.contains(u"mask"_s)
            && !s.body.contains(u"ones over the box"_s) && s.request.contains(u"origin_x"_s))
            return true;
    return false;
}
} // namespace

void TestSharedSelection::applyGate_acceptsAReceivedSelectionWithoutASeed()
{
    using G = visivo::LassoApplyGate;
    const visivo::SharedSelectionOrigin here;
    QCOMPARE(visivo::lassoApplyGate(true, false, here), G::Ok);
    QCOMPARE(visivo::lassoApplyGate(false, false, here), G::NoSeed);
    QCOMPARE(visivo::lassoApplyGate(true, true, here), G::NoSelection);
    // Received: no seed here (the sliders must not regrow it), applied anyway.
    QCOMPARE(visivo::lassoApplyGate(false, false, received(u"lasso"_s, true)), G::Ok);
    QCOMPARE(visivo::lassoApplyGate(false, false, received(u"box"_s, false)), G::Ok);
    QCOMPARE(visivo::lassoApplyGate(false, true, received(u"box"_s, false)), G::NoSelection);
}

void TestSharedSelection::applyRecord_ofASelectionMadeHereIsItsGrow()
{
    const auto r = visivo::lassoApplyRecord({}, fullRequest(), u"noise box"_s, false, 0, u"blank_outside"_s);
    QCOMPARE(r.replay.size(), 2);
    QCOMPARE(r.replay[0].route, u"/v1/cube/kinematic_select_bin"_s);
    QCOMPARE(r.replay[0].request, visivo::lassoGrowRequestMap(fullRequest()));
    QCOMPARE(r.replay[1].request.value(u"mode"_s).toString(), u"blank_outside"_s);
    QVERIFY(!r.params.contains(u"Made in"_s));
    QVERIFY(r.caveat.isEmpty());
    QCOMPARE(r.history, visivo::lassoHistoryItems(fullRequest(), false, 0));
    QVERIFY(!anyRoi(r.replay));
    QVERIFY(!visivo::lassoApplyRecord({}, fullRequest(), {}, true, 0, u"blank_inside"_s).caveat.isEmpty());
}

void TestSharedSelection::applyRecord_ofAReceivedLassoIsItsCreatorsGrow()
{
    const auto r = visivo::lassoApplyRecord(received(u"lasso"_s, true), fullRequest(),
                                            u"set on the VR page"_s, false, 0, u"blank_outside"_s);
    QCOMPARE(r.replay.size(), 2);
    QCOMPARE(r.replay[0].request, visivo::lassoGrowRequestMap(fullRequest()));
    QCOMPARE(r.params.value(u"Made in"_s).toString(), u"VR headset"_s);
    QCOMPARE(r.params.value(u"Seed (z, y, x)"_s).toString(), u"(69, 83, 80)"_s);
    QVERIFY(r.history.contains(u"made_in=VR headset"_s));
    QVERIFY(r.caveat.isEmpty());
    QVERIFY(!anyRoi(r.replay));
}

void TestSharedSelection::applyRecord_ofAReceivedLassoWithoutAGrowIsTheMaskOnly()
{
    // A default request would replay another selection: never recorded.
    const auto r = visivo::lassoApplyRecord(received(u"lasso"_s, false), visivo::LassoGrowRequest{},
                                            {}, false, 0, u"blank_inside"_s);
    QCOMPARE(r.replay.size(), 1);
    QCOMPARE(r.replay[0].route, u"/v1/cube/mask_apply_bin"_s);
    QCOMPARE(r.replay[0].request, (QVariantMap{ { u"mode"_s, u"blank_inside"_s } }));
    QVERIFY(r.replay[0].body.contains(u"/v1/share/selection/mask"_s));
    // Its ROI comes from the mask route's headers, never a recorded number.
    QVERIFY(r.replay[0].body.contains(u"X-Visivo-Origin"_s));
    QVERIFY(r.replay[0].body.contains(u"X-Visivo-Depth"_s));
    QVERIFY(!r.replay[0].request.contains(u"origin_x"_s));
    for (const ReplayStep &s : r.replay) QVERIFY(s.route != u"/v1/cube/kinematic_select_bin"_s);
    QVERIFY(!r.params.contains(u"Seed (z, y, x)"_s));
    QCOMPARE(r.params.value(u"Reach"_s).toString(), u"8"_s);   // the creator's own labels
    QVERIFY(r.params.value(u"Recorded as"_s).toString().startsWith(u"the mask only"_s));
    QVERIFY(!r.caveat.isEmpty());
    QCOMPARE(r.history.first(), u"selection=mask"_s);
}

void TestSharedSelection::applyRecord_ofAReceivedBoxIsTheBox()
{
    auto r = visivo::lassoApplyRecord(received(u"box"_s, false), visivo::LassoGrowRequest{}, {}, false, 0,
                                      u"blank_outside"_s);
    QCOMPARE(r.replay.size(), 1);
    QCOMPARE(r.replay[0].route, u"/v1/cube/mask_apply_bin"_s);
    QCOMPARE(r.replay[0].request.value(u"width"_s).toInt(), 4);
    QCOMPARE(r.params.value(u"Pixels"_s).toString(), u"x 10..13, y 20..24"_s);
    QCOMPARE(r.params.value(u"Made in"_s).toString(), u"VR headset"_s);
    QCOMPARE(r.history.first(), u"selection=box"_s);
    QVERIFY(r.caveat.isEmpty());
    r = visivo::lassoApplyRecord(received(u"box"_s, false), {}, {}, true, 0, u"blank_outside"_s);
    QVERIFY(r.caveat.contains(u"box gives only its unedited part"_s));
}

void TestSharedSelection::unsupported_onlyForABackendWithoutTheHub()
{
    QVERIFY(visivo::shareRouteUnsupported(404, R"({"detail":"Not Found"})"));
    QVERIFY(visivo::shareRouteUnsupported(405, ""));
    QVERIFY(!visivo::shareRouteUnsupported(404, R"({"detail":"Session not found."})"));
    QVERIFY(!visivo::shareRouteUnsupported(404, R"({"detail":"Unknown dataset_id 'ds'."})"));
    QVERIFY(!visivo::shareRouteUnsupported(500, R"({"detail":"Not Found"})"));
}

namespace {
visivo::SharedSelectionInfo entry(qint64 seq, const QString &client, const QString &nonce)
{
    auto i = visivo::parseSharedSelectionInfo(hubAnswer(u"lasso"_s, QJsonObject{ { u"nonce"_s, nonce } }));
    i.seq = seq;
    i.client = client;
    return i;
}
visivo::ShareViewState view(const QString &shown, const QString &local, qint64 floor)
{
    visivo::ShareViewState s;
    s.client = u"desktop-me"_s;
    s.shownNonce = shown;
    s.localNonce = local;
    s.floorSeq = floor;
    s.full[0] = 100; s.full[1] = 110; s.full[2] = 120;
    return s;
}
} // namespace

void TestSharedSelection::reconcile_convergesOnTheHubsLatest()
{
    using A = visivo::ReconcileAction;
    const auto s = view(u"n-shown"_s, u"n-local"_s, 4);
    QCOMPARE(entry(5, u"vr-abc"_s, u"n-x"_s).nonce, u"n-x"_s);
    QCOMPARE(visivo::reconcileDecision(nullptr, s), A::None);
    auto e = entry(5, u"vr-abc"_s, u"n-x"_s);
    QCOMPARE(visivo::reconcileDecision(&e, s), A::Fetch);                         // another client's, newer
    e = entry(5, u"vr-abc"_s, u"n-shown"_s);
    QCOMPARE(visivo::reconcileDecision(&e, s), A::None);                          // on screen already
    e = entry(4, u"vr-abc"_s, u"n-x"_s);
    QCOMPARE(visivo::reconcileDecision(&e, s), A::None);                          // read, then replaced here
    e = entry(9, u"desktop-me"_s, u"n-old"_s);
    QCOMPARE(visivo::reconcileDecision(&e, s), A::Skip);                          // an older own one
    e = entry(9, u"desktop-me"_s, u"n-local"_s);
    QCOMPARE(visivo::reconcileDecision(&e, s), A::Own);                           // ours, not on screen
    e = entry(9, u"vr-abc"_s, u"n-x"_s);
    e.fullShape = { 100, 110, 121 };
    QCOMPARE(visivo::reconcileDecision(&e, s), A::None);                          // another cube
    // Confirm: still the latest, and nothing local meanwhile.
    e = entry(5, u"vr-abc"_s, u"n-x"_s);
    QVERIFY(visivo::confirmShareDraw(&e, 5, u"a"_s, 4, u"a"_s, 4));
    QVERIFY(!visivo::confirmShareDraw(&e, 4, u"a"_s, 4, u"a"_s, 4));
    QVERIFY(!visivo::confirmShareDraw(nullptr, 5, u"a"_s, 4, u"a"_s, 4));
    QVERIFY(!visivo::confirmShareDraw(&e, 5, u"a"_s, 4, u"b"_s, 4));                // a local publish
    QVERIFY(!visivo::confirmShareDraw(&e, 5, u"a"_s, 4, u""_s, 6));                 // a local lasso or clear
}

void TestSharedSelection::reconcile_theReviewCasesEndOnOneSelection()
{
    using A = visivo::ReconcileAction;
    // The desktop holds a received B (seq 6, being fetched); the user sends a
    // local lasso A meanwhile: publish() sets shown = local = A, floor = the
    // seq read so far (6). B's confirm fails (shown changed), and B is never
    // drawn again: at or below the floor.
    auto b = entry(6, u"vr-abc"_s, u"n-b"_s);
    QVERIFY(!visivo::confirmShareDraw(&b, 6, QString(), 0, u"n-a"_s, 6));
    QCOMPARE(visivo::reconcileDecision(&b, view(u"n-a"_s, u"n-a"_s, 6)), A::None);
    // A kept by the hub although its answer was lost (seq 7): on screen, nothing to do.
    auto a = entry(7, u"desktop-me"_s, u"n-a"_s);
    QCOMPARE(visivo::reconcileDecision(&a, view(u"n-a"_s, u"n-a"_s, 6)), A::None);
    // B was drawn before A reached the hub (shown = B): when A lands it is
    // drawn back, the hub's latest and ours.
    QCOMPARE(visivo::reconcileDecision(&a, view(u"n-b"_s, u"n-a"_s, 5)), A::Own);
    // A refused as stale (409): the hub's later C is drawn.
    auto c = entry(8, u"vr-abc"_s, u"n-c"_s);
    QCOMPARE(visivo::reconcileDecision(&c, view(u"n-a"_s, u"n-a"_s, 6)), A::Fetch);
    // A local edit after A was sent: A in the hub is an older own one, skipped.
    QCOMPARE(visivo::reconcileDecision(&a, view(QString(), QString(), 7)), A::Skip);
}

namespace {
QJsonObject sharedCases()
{
#ifndef VISIVO_REPO_ROOT
#    define VISIVO_REPO_ROOT "."
#endif
    QFile f(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/backend/tests/vr/share_rule_cases.json"_s);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

visivo::ShareViewState stateFrom(const QJsonObject &base, const QJsonObject &over)
{
    QJsonObject s = base;
    for (auto it = over.begin(); it != over.end(); ++it) s.insert(it.key(), it.value());
    visivo::ShareViewState v;
    v.client = s.value(u"client"_s).toString();
    v.shownNonce = s.value(u"shown"_s).toString();
    v.localNonce = s.value(u"local"_s).toString();
    v.floorSeq = s.value(u"floor"_s).toInteger();
    const QJsonArray full = s.value(u"full"_s).toArray();   // null: not known (0, 0, 0)
    for (int i = 0; i < 3 && full.size() == 3; ++i) v.full[i] = full[i].toInt();
    return v;
}
} // namespace

void TestSharedSelection::sharedCases_agreeWithThePage()
{
    // The same cases the page's share.js is tested on (share.test.mjs).
    const QJsonObject c = sharedCases();
    QVERIFY2(!c.isEmpty(), "backend/tests/vr/share_rule_cases.json not found");
    const QJsonObject base = c.value(u"base"_s).toObject();
    const QJsonObject state = c.value(u"state"_s).toObject();
    const QHash<QString, visivo::ReconcileAction> names{
        { u"none"_s, visivo::ReconcileAction::None }, { u"skip"_s, visivo::ReconcileAction::Skip },
        { u"fetch"_s, visivo::ReconcileAction::Fetch }, { u"own"_s, visivo::ReconcileAction::Own } };
    int n = 0;
    for (const QJsonValue &v : c.value(u"decision"_s).toArray()) {
        const QJsonObject k = v.toObject();
        const QString name = k.value(u"name"_s).toString();
        visivo::SharedSelectionInfo info;
        const bool none = k.value(u"latest"_s).isNull();
        if (!none) {
            QJsonObject e = base;
            const QJsonObject over = k.value(u"latest"_s).toObject();
            for (auto it = over.begin(); it != over.end(); ++it) e.insert(it.key(), it.value());
            info = visivo::parseSharedSelectionInfo(QJsonDocument(e).toJson(QJsonDocument::Compact));
        }
        const auto got = visivo::reconcileDecision(none ? nullptr : &info,
                                                   stateFrom(state, k.value(u"state"_s).toObject()));
        QVERIFY2(got == names.value(k.value(u"expect"_s).toString()), qPrintable(name));
        ++n;
    }
    QVERIFY(n >= 20);
    for (const QJsonValue &v : c.value(u"confirm"_s).toArray()) {
        const QJsonObject k = v.toObject();
        visivo::SharedSelectionInfo now;
        const bool none = k.value(u"now"_s).isNull();
        if (!none) {
            QJsonObject e = base;
            e.insert(u"seq"_s, k.value(u"now"_s).toObject().value(u"seq"_s));
            now = visivo::parseSharedSelectionInfo(QJsonDocument(e).toJson(QJsonDocument::Compact));
        }
        const QJsonObject b = k.value(u"before"_s).toObject(), a = k.value(u"after"_s).toObject();
        QVERIFY2(visivo::confirmShareDraw(none ? nullptr : &now, k.value(u"fetched"_s).toInteger(),
                                          b.value(u"shown"_s).toString(), b.value(u"floor"_s).toInteger(),
                                          a.value(u"shown"_s).toString(), a.value(u"floor"_s).toInteger())
                         == k.value(u"expect"_s).toBool(),
                 qPrintable(k.value(u"name"_s).toString()));
    }
    for (const QJsonValue &v : c.value(u"note_local"_s).toArray()) {
        const QJsonObject k = v.toObject();
        const QJsonObject s = k.value(u"state"_s).toObject(), e = k.value(u"expect"_s).toObject();
        visivo::ShareViewState in;
        in.shownNonce = s.value(u"shown"_s).toString();
        in.localNonce = s.value(u"local"_s).toString();
        in.floorSeq = s.value(u"floor"_s).toInteger();
        const auto out = visivo::noteLocal(in, k.value(u"seen"_s).toInteger());
        QCOMPARE(out.shownNonce, e.value(u"shown"_s).toString());
        QCOMPARE(out.localNonce, e.value(u"local"_s).toString());
        QCOMPARE(out.floorSeq, e.value(u"floor"_s).toInteger());
    }
    const QHash<QString, visivo::PublishOutcome> outcomes{
        { u"kept"_s, visivo::PublishOutcome::Kept }, { u"stale"_s, visivo::PublishOutcome::Stale },
        { u"retry"_s, visivo::PublishOutcome::Retry }, { u"refused"_s, visivo::PublishOutcome::Refused },
        { u"unknown"_s, visivo::PublishOutcome::Unknown }, { u"cleared"_s, visivo::PublishOutcome::Cleared } };
    for (const QJsonValue &v : c.value(u"publish_outcome"_s).toArray()) {
        const QJsonObject k = v.toObject();
        QVERIFY2(visivo::publishOutcomeOf(k.value(u"status"_s).toInt(), k.value(u"detail"_s).toString())
                         == outcomes.value(k.value(u"expect"_s).toString()),
                 qPrintable(QString::number(k.value(u"status"_s).toInt())));
    }
    const QHash<QString, visivo::InstanceVerdict> verdicts{
        { u"adopt"_s, visivo::InstanceVerdict::Adopt }, { u"same"_s, visivo::InstanceVerdict::Same },
        { u"reset"_s, visivo::InstanceVerdict::Reset } };
    for (const QJsonValue &v : c.value(u"instance"_s).toArray()) {
        const QJsonObject k = v.toObject();
        QVERIFY(visivo::instanceVerdict(k.value(u"known"_s).toString(), k.value(u"seen"_s).toString())
                == verdicts.value(k.value(u"expect"_s).toString()));
    }
}
