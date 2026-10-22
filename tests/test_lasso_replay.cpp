// test_lasso_replay.cpp
// The grow request a cube records is what a script posts back. These tests pin
// it to the service: the field names are read from the request model in
// backend/app/routers/cube.py, so a renamed field fails here before it fails a
// replay; and the apply step must not carry an ROI of its own, because the grow
// answers with the selection's bounding box, not the interactive field's.

#include "TestLassoReplay.h"

#include "BackendHeaders.h"
#include "LassoReplay.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTest>

using namespace Qt::StringLiterals;
using visivo::LassoGrowRequest;

#ifndef VISIVO_REPO_ROOT
#    define VISIVO_REPO_ROOT "."
#endif

namespace {

LassoGrowRequest fullRequest()
{
    LassoGrowRequest g;
    g.seed = { 69, 83, 80 };
    g.addSeeds = { { 70, 90, 91 } };
    g.subSeeds = { { 60, 70, 71 }, { 61, 72, 73 } };
    g.reach = 8.0;
    g.lambdaV = 1.0;
    g.sigma = 0.0012345678901234567;
    g.nsigma = 2.5;
    g.roiRadius = 0;
    g.reachMax = 64.0;
    g.smoothV = 2.0;
    g.smoothXy = 6.0;
    return g;
}

/// The fields of `class CubeKinematicSelectRequest(BaseModel)` in the router:
/// one `name: type` per line at four spaces of indent, up to the validator.
QSet<QString> serviceRequestFields()
{
    QFile f(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/backend/app/routers/cube.py"_s);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QString src = QString::fromUtf8(f.readAll());
    const qsizetype start = src.indexOf(u"class CubeKinematicSelectRequest(BaseModel):"_s);
    if (start < 0)
        return {};
    const qsizetype end = src.indexOf(u"@model_validator"_s, start);
    const QString body = src.mid(start, end > start ? end - start : -1);
    QSet<QString> fields;
    static const QRegularExpression field(u"^    (\\w+): "_s, QRegularExpression::MultilineOption);
    for (auto it = field.globalMatch(body); it.hasNext();)
        fields.insert(it.next().captured(1));
    return fields;
}

} // namespace

void TestLassoReplay::growRequest_usesTheServiceRequestModelsFieldNames()
{
    const QSet<QString> service = serviceRequestFields();
    QVERIFY2(!service.isEmpty(), "could not read CubeKinematicSelectRequest from the router");
    QSet<QString> recorded;
    for (const QString &k : visivo::lassoGrowRequestMap(fullRequest()).keys())
        recorded.insert(k);
    // dataset_id is the one the replaying session gets from opening the path.
    recorded.insert(u"dataset_id"_s);
    const QSet<QString> unknown = recorded - service;
    const QSet<QString> unsent = service - recorded;
    QVERIFY2(unknown.isEmpty(),
             qPrintable(u"recorded fields the service does not take: "_s
                        + QStringList(unknown.begin(), unknown.end()).join(u", "_s)));
    QVERIFY2(unsent.isEmpty(),
             qPrintable(u"service fields the record never sets: "_s
                        + QStringList(unsent.begin(), unsent.end()).join(u", "_s)));
}

void TestLassoReplay::growRequest_sendsOptionalFieldsOnlyWhenSet()
{
    const QVariantMap full = visivo::lassoGrowRequestMap(fullRequest());
    QCOMPARE(full.value(u"seed_z"_s).toInt(), 69);
    QCOMPARE(full.value(u"seed_x"_s).toInt(), 80);
    QCOMPARE(full.value(u"add_seeds"_s).toList().size(), 1);
    QCOMPARE(full.value(u"sub_seeds"_s).toList().size(), 2);
    QCOMPARE(full.value(u"sub_seeds"_s).toList().at(1).toList().at(2).toInt(), 73);
    QCOMPARE(full.value(u"sigma"_s).toDouble(), 0.0012345678901234567);
    QCOMPARE(full.value(u"reach_max"_s).toDouble(), 64.0);
    QCOMPARE(full.value(u"smooth_v"_s).toDouble(), 2.0);
    QCOMPARE(full.value(u"smooth_xy"_s).toDouble(), 6.0);

    // One click, no smoothing, no noise estimate: what a default grow sends.
    LassoGrowRequest plain;
    plain.seed = { 52, 85, 84 };
    plain.reachMax = 8.0;
    const QVariantMap bare = visivo::lassoGrowRequestMap(plain);
    QVERIFY(!bare.contains(u"add_seeds"_s));
    QVERIFY(!bare.contains(u"sub_seeds"_s));
    QVERIFY(!bare.contains(u"smooth_v"_s));
    QVERIFY(!bare.contains(u"smooth_xy"_s));
    QCOMPARE(bare.value(u"sigma"_s).toDouble(), 0.0);   // the service measures its own
    QCOMPARE(bare.value(u"reach"_s).toDouble(), 8.0);
    QCOMPARE(bare.value(u"lambda_v"_s).toDouble(), 1.0);
    QCOMPARE(bare.value(u"nsigma"_s).toDouble(), 2.5);

    const ReplayStep grow = visivo::lassoGrowStep(plain);
    QCOMPARE(grow.route, u"/v1/cube/kinematic_select_bin"_s);
    QCOMPARE(grow.request, bare);
    QVERIFY(grow.body.isEmpty());
}

void TestLassoReplay::applyStep_recordsTheModeAndTakesTheRoiFromTheGrowResponse()
{
    const ReplayStep apply = visivo::lassoApplyStep(u"blank_outside"_s, /*handEdited=*/false);
    QCOMPARE(apply.route, u"/v1/cube/mask_apply_bin"_s);
    QCOMPARE(apply.request.keys(), QStringList{ u"mode"_s });
    QCOMPARE(apply.request.value(u"mode"_s).toString(), u"blank_outside"_s);
    // The ROI is the grow's answer: the body names the headers that carry it.
    for (const char *h : { VisivoHeaders::ORIGIN, VisivoHeaders::WIDTH, VisivoHeaders::HEIGHT,
                           VisivoHeaders::DEPTH })
        QVERIFY2(apply.body.contains(QLatin1String(h)), h);
    QVERIFY(!apply.body.contains(u"hand-edited"_s));

    const ReplayStep edited = visivo::lassoApplyStep(u"blank_inside"_s, /*handEdited=*/true);
    QCOMPARE(edited.request.value(u"mode"_s).toString(), u"blank_inside"_s);
    QVERIFY(edited.body.contains(u"hand-edited"_s));
    QVERIFY(edited.body.contains(u"replay_caveat"_s));
}

void TestLassoReplay::fitStep_recordsItsOwnParametersAndTakesTheRoiFromTheGrowResponse()
{
    // The fit posts the mask too, so it must not carry an ROI of its own either:
    // the one the interactive field was computed on is not the grow's answer.
    const ReplayStep measured = visivo::lassoFitStep(0.0012, 150);
    QCOMPARE(measured.route, u"/v1/cube/kinematic_fit_bin"_s);
    QCOMPARE(measured.request.keys(), (QStringList{ u"min_ring_pix"_s, u"sigma"_s }));
    QCOMPARE(measured.request.value(u"sigma"_s).toDouble(), 0.0012);
    QCOMPARE(measured.request.value(u"min_ring_pix"_s).toInt(), 150);
    for (const char *h : { VisivoHeaders::ORIGIN, VisivoHeaders::WIDTH, VisivoHeaders::HEIGHT,
                           VisivoHeaders::DEPTH })
        QVERIFY2(measured.body.contains(QLatin1String(h)), h);
    QVERIFY(!measured.request.contains(u"origin_x"_s));
    QVERIFY(!measured.request.contains(u"width"_s));

    const ReplayStep robust = visivo::lassoFitStep(0.0, 150);
    QCOMPARE(robust.request.keys(), QStringList{ u"min_ring_pix"_s });   // the service measures σ
}

void TestLassoReplay::historyAndDisplay_agreeWithTheRequest()
{
    const LassoGrowRequest g = fullRequest();
    const QStringList items = visivo::lassoHistoryItems(g, /*handEdited=*/false);
    QVERIFY(items.contains(u"seed_zyx=69,83,80"_s));
    QVERIFY(items.contains(u"reach=8"_s));
    QVERIFY(items.contains(u"reach_max=64"_s));
    QVERIFY(items.contains(u"add_seeds=(70,90,91)"_s));
    QVERIFY(items.contains(u"sub_seeds=(60,70,71)|(61,72,73)"_s));
    QVERIFY(items.contains(u"smooth_zyx=2,6,6"_s));
    QVERIFY(items.contains(u"sigma=0.0012345678901234567 (noise estimate)"_s));
    QVERIFY(items.contains(u"hand_edited=no"_s));

    const QVariantMap shown = visivo::lassoDisplayParams(g, u"channels 0..397"_s, true);
    QCOMPARE(shown.value(u"Seed (z, y, x)"_s).toString(), u"(69, 83, 80)"_s);
    QCOMPARE(shown.value(u"Level-field budget (reach_max)"_s).toString(), u"64"_s);
    QCOMPARE(shown.value(u"Extra seeds carved"_s).toString(), u"(60, 70, 71); (61, 72, 73)"_s);
    QCOMPARE(shown.value(u"Smoothing (v, y, x)"_s).toString(), u"(2, 6, 6)"_s);
    QCOMPARE(shown.value(u"Noise σ"_s).toString(),
             u"0.0012345678901234567 (noise estimate, channels 0..397)"_s);
    QCOMPARE(shown.value(u"Hand-edited"_s).toString(), u"yes"_s);

    LassoGrowRequest plain;
    const QVariantMap bare = visivo::lassoDisplayParams(plain, QString(), false);
    QCOMPARE(bare.value(u"Noise σ"_s).toString(), u"robust, whole cube"_s);
    QCOMPARE(bare.value(u"Smoothing (v, y, x)"_s).toString(), u"off"_s);
    QVERIFY(!bare.contains(u"Extra seeds added"_s));
    QVERIFY(visivo::lassoHistoryItems(plain, false).contains(u"sigma=robust (whole cube)"_s));

    // The node cap's cut is recorded only when it happened.
    QVERIFY(!shown.contains(u"Domain trimmed"_s));
    QVERIFY(!visivo::lassoHistoryItems(g, false).join(u";"_s).contains(u"domain_trimmed"_s));
    const QVariantMap cut = visivo::lassoDisplayParams(g, QString(), false, /*trimBox=*/64);
    QVERIFY(cut.value(u"Domain trimmed"_s).toString().contains(u"64"_s));
    QVERIFY(visivo::lassoHistoryItems(g, false, 64)
                    .contains(u"domain_trimmed=largest box half-width 64 voxels"_s));
}
