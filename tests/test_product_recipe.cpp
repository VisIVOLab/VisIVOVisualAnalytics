// test_product_recipe.cpp
// The exported recipe is the one record a reader has of how a product was
// made. These tests pin its shape: the replay is a list of calls, each with
// its route and its request as typed values under the API's field names, so a
// script sends them back instead of parsing "(69, 83, 80)" out of a label.

#include "TestProductRecipe.h"

#include "ProductRecipe.h"
#include "ProductRegistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

AnalysisProduct lassoProduct()
{
    AnalysisProduct p;
    p.productId = u"lasso-ds_1"_s;
    p.kind = u"lasso"_s;
    p.label = u"Lasso: isolated cube"_s;
    p.parentLabel = u"cube.fits"_s;
    p.parentKey = u"/data/cube.fits"_s;
    p.params.insert(u"Seed (z, y, x)"_s, u"(69, 83, 80)"_s);
    p.params.insert(u"Reach"_s, u"8"_s);
    ReplayStep grow;
    grow.route = u"/v1/cube/kinematic_select_bin"_s;
    grow.request.insert(u"seed_z"_s, 69);
    grow.request.insert(u"seed_y"_s, 83);
    grow.request.insert(u"seed_x"_s, 80);
    grow.request.insert(u"reach"_s, 8.0);
    grow.request.insert(u"lambda_v"_s, 1.0);
    grow.request.insert(u"sigma"_s, 0.0012345678901234567);
    grow.request.insert(u"reach_max"_s, 64.0);
    grow.request.insert(u"add_seeds"_s, QVariantList{ QVariantList{ 70, 90, 91 } });
    ReplayStep apply;
    apply.route = u"/v1/cube/mask_apply_bin"_s;
    apply.request.insert(u"mode"_s, u"blank_outside"_s);
    apply.request.insert(u"origin_z"_s, 40);
    apply.body = u"the float32 mask the first call returned"_s;
    p.replay = { grow, apply };
    return p;
}

} // namespace

void TestProductRecipe::replaySteps_carryTheRouteAndTheTypedRequestInOrder()
{
    const QJsonObject r = visivo::productRecipe(lassoProduct(), u"VisIVO 1.0.2"_s,
                                                u"2026-10-22T10:00:00Z"_s);
    QCOMPARE(r.value(u"schema"_s).toString(), u"visivo-recipe/1"_s);
    QCOMPARE(r.value(u"application"_s).toString(), u"VisIVO 1.0.2"_s);
    QCOMPARE(r.value(u"exported"_s).toString(), u"2026-10-22T10:00:00Z"_s);

    const QJsonArray steps = r.value(u"product"_s).toObject().value(u"replay"_s).toArray();
    QCOMPARE(steps.size(), 2);
    const QJsonObject grow = steps.at(0).toObject();
    QCOMPARE(grow.value(u"route"_s).toString(), u"/v1/cube/kinematic_select_bin"_s);
    const QJsonObject req = grow.value(u"request"_s).toObject();
    // Numbers are numbers, not the display strings of `parameters`.
    QVERIFY(req.value(u"seed_z"_s).isDouble());
    QCOMPARE(req.value(u"seed_z"_s).toInt(), 69);
    QCOMPARE(req.value(u"reach"_s).toDouble(), 8.0);
    QCOMPARE(req.value(u"reach_max"_s).toDouble(), 64.0);
    // Seeds are [z, y, x] arrays, as the route takes them.
    const QJsonArray adds = req.value(u"add_seeds"_s).toArray();
    QCOMPARE(adds.size(), 1);
    QCOMPARE(adds.at(0).toArray().at(2).toInt(), 91);
    // The noise estimate survives the text round trip at full precision.
    const QJsonObject back = QJsonDocument::fromJson(QJsonDocument(r).toJson()).object();
    const double sigma = back.value(u"product"_s).toObject().value(u"replay"_s).toArray()
                                 .at(0).toObject().value(u"request"_s).toObject()
                                 .value(u"sigma"_s).toDouble();
    QCOMPARE(sigma, 0.0012345678901234567);
    QVERIFY(!grow.contains(u"body"_s));

    const QJsonObject apply = steps.at(1).toObject();
    QCOMPARE(apply.value(u"route"_s).toString(), u"/v1/cube/mask_apply_bin"_s);
    QCOMPARE(apply.value(u"request"_s).toObject().value(u"mode"_s).toString(),
             u"blank_outside"_s);
    QCOMPARE(apply.value(u"body"_s).toString(), u"the float32 mask the first call returned"_s);

    // The display parameters stay, for the reader.
    QCOMPARE(r.value(u"parameters"_s).toObject().value(u"Seed (z, y, x)"_s).toString(),
             u"(69, 83, 80)"_s);
    QCOMPARE(r.value(u"dataset"_s).toObject().value(u"path"_s).toString(),
             u"/data/cube.fits"_s);
}

void TestProductRecipe::aStepWithoutARequest_isJustItsRoute()
{
    AnalysisProduct p;
    p.replay = { ReplayStep{ u"/v1/cube/kinematic_fit_bin"_s, {}, {} } };
    p.replayCaveat = u"needs the mask"_s;
    const QJsonObject product =
            visivo::productRecipe(p, u"app"_s, u"now"_s).value(u"product"_s).toObject();
    const QJsonObject step = product.value(u"replay"_s).toArray().at(0).toObject();
    QCOMPARE(step.value(u"route"_s).toString(), u"/v1/cube/kinematic_fit_bin"_s);
    QVERIFY(!step.contains(u"request"_s));
    QVERIFY(!step.contains(u"body"_s));
    QCOMPARE(product.value(u"replay_caveat"_s).toString(), u"needs the mask"_s);
}

void TestProductRecipe::aProductWithoutReplay_hasNoReplayKey()
{
    AnalysisProduct p;
    p.kind = u"noise"_s;
    const QJsonObject product =
            visivo::productRecipe(p, u"app"_s, u"now"_s).value(u"product"_s).toObject();
    QVERIFY(!product.contains(u"replay"_s));
    QVERIFY(!product.contains(u"replay_caveat"_s));
}

void TestProductRecipe::volatileIds_areWrittenOnlyWhenKnown()
{
    AnalysisProduct p;
    QJsonObject ds = visivo::productRecipe(p, u"app"_s, u"now"_s).value(u"dataset"_s).toObject();
    QVERIFY(!ds.contains(u"backend_dataset_id"_s));
    QVERIFY(!ds.contains(u"backend_session_id"_s));
    p.sourceDatasetId = u"ds_1"_s;
    p.sessionId = u"s_1"_s;
    ds = visivo::productRecipe(p, u"app"_s, u"now"_s).value(u"dataset"_s).toObject();
    QCOMPARE(ds.value(u"backend_dataset_id"_s).toString(), u"ds_1"_s);
    QCOMPARE(ds.value(u"backend_session_id"_s).toString(), u"s_1"_s);
}
