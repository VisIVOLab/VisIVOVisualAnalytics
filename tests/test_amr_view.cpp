// test_amr_view.cpp — see TestAmrView.h.

#include "TestAmrView.h"
#include "app/AmrView.h"

#include <QJsonDocument>
#include <QTest>

using namespace visivo::amrview;

namespace {

QJsonObject reply(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

const char *kPlan = R"({
    "valid": true, "path": "/runs/output_00185", "code": "ramses",
    "plan": {"field": "density", "level": 8, "shape": [256, 128, 64],
             "voxels": 2097152, "bytes_estimate": 8388608,
             "peak_memory_bytes_estimate": 300000000,
             "voxel_size_physical": 0.5, "lower_physical": [0, 0, 0],
             "upper_physical": [128, 64, 32], "length_unit": "kpc", "value_unit": "g/cm3",
             "fits": true, "max_voxels": 67108864, "max_level_within_limit": 10,
             "finest_level_in_region": 9, "levels": [6, 7, 8, 9, 10],
             "fields": ["density", "pressure", "mass", "level"]}})";

}   // namespace

void TestAmrView::parsePlan_readsTheBackendReply()
{
    const Plan plan = parsePlan(reply(kPlan));
    QVERIFY(plan.valid);
    QCOMPARE(plan.code, QStringLiteral("ramses"));
    QCOMPARE(plan.level, 8);
    QCOMPARE(plan.shape, (QVector<int>{ 256, 128, 64 }));
    QCOMPARE(plan.voxels, qint64(2097152));
    QCOMPARE(plan.peakMemoryBytes, qint64(300000000));
    QCOMPARE(plan.upperPhysical, (QVector<double>{ 128, 64, 32 }));
    QCOMPARE(plan.lengthUnit, QStringLiteral("kpc"));
    QVERIFY(plan.fits);
    QCOMPARE(plan.maxLevelWithinLimit, 10);
    QCOMPARE(plan.finestLevelInRegion, 9);
    QCOMPARE(plan.levels, (QVector<int>{ 6, 7, 8, 9, 10 }));
    QCOMPARE(plan.fields.size(), 4);
}

void TestAmrView::parsePlan_refusesAReplyWithoutAGrid()
{
    QVERIFY(!parsePlan(reply(R"({"valid": true})")).valid);
    QVERIFY(!parsePlan(reply(R"({"plan": {"shape": [10, 10], "voxels": 100}})")).valid);
}

void TestAmrView::parsePlan_unknownFinestLevelIsMinusOne()
{
    const Plan plan = parsePlan(reply(R"({"plan": {"shape": [2, 2, 2], "voxels": 8,
                                         "finest_level_in_region": null}})"));
    QVERIFY(plan.valid);
    QCOMPARE(plan.finestLevelInRegion, -1);
}

void TestAmrView::summary_saysLevelMemoryAndWhyItDoesNotFit()
{
    Plan plan = parsePlan(reply(kPlan));
    QString text = summary(plan);
    QVERIFY(text.contains(QStringLiteral("Level 8")));
    QVERIFY(text.contains(QStringLiteral("256 × 128 × 64")));
    QVERIFY(text.contains(QStringLiteral("Peak memory")));
    QVERIFY(text.contains(QStringLiteral("Finest level that fits the limit:</b> 10")));
    QVERIFY(text.contains(QStringLiteral("Finest level in the region:</b> 9")));
    QVERIFY(!text.contains(QStringLiteral("Too large")));

    plan.fits = false;
    plan.reason = QStringLiteral("level 7 fits <b>");
    text = summary(plan);
    QVERIFY(text.contains(QStringLiteral("Too large")));
    QVERIFY(text.contains(QStringLiteral("&lt;b&gt;")));   // the reason is escaped
}

void TestAmrView::requestBody_sendsALevelOnlyWhenChosen()
{
    QJsonObject body = requestBody(QStringLiteral("/runs/o"), QStringLiteral("density"), -1,
                                   Region{}, voxelBudget(64));
    QVERIFY(!body.contains(QStringLiteral("level")));
    QVERIFY(!body.contains(QStringLiteral("lo")));
    QCOMPARE(body.value(QStringLiteral("field")).toString(), QStringLiteral("density"));
    QCOMPARE(body.value(QStringLiteral("max_voxels")).toDouble(), 64e6);

    body = requestBody(QStringLiteral("/runs/o"), QStringLiteral("mass"), 0, Region{}, 1);
    QCOMPARE(body.value(QStringLiteral("level")).toInt(), 0);   // level 0 is a choice, not none
}

void TestAmrView::requestBody_carriesTheRegionAndItsUnit()
{
    Region region;
    region.set = true;
    region.lo = { 10, 20, 30 };
    region.hi = { 40, 50, 60 };
    region.unit = QStringLiteral("physical");
    const QJsonObject body = requestBody(QStringLiteral("/o"), QStringLiteral("level"), -1, region,
                                         1000);
    QCOMPARE(body.value(QStringLiteral("region_unit")).toString(), QStringLiteral("physical"));
    QCOMPARE(body.value(QStringLiteral("lo")).toArray().at(2).toDouble(), 30.0);
    QCOMPARE(body.value(QStringLiteral("hi")).toArray().at(0).toDouble(), 40.0);
}

void TestAmrView::parseCubes_listsTheOutput()
{
    const QVector<Cube> cubes = parseCubes(reply(R"({"outputs": [
        {"field": "density", "path": "/x/amr-uniform/density-L8.fits", "shape": [8, 8, 8]}]})"));
    QCOMPARE(cubes.size(), 1);
    QCOMPARE(cubes[0].field, QStringLiteral("density"));
    QCOMPARE(cubes[0].shape, (QVector<int>{ 8, 8, 8 }));
    QVERIFY(parseCubes(reply("{}")).isEmpty());
}

void TestAmrView::regionFromVoxelBox_isTheOuterFacesOfTheEndVoxels()
{
    // Voxel 0 is centred at 1 kpc and voxels are 2 kpc: its faces are at 0 and 2.
    const std::array<AxisScale, 3> axes{ AxisScale{ 1.0, 2.0 }, AxisScale{ 1.0, 2.0 },
                                         AxisScale{ 11.0, 2.0 } };
    const Region region = regionFromVoxelBox({ 0, 3, 1, 1, 2, 4 }, axes);
    QVERIFY(region.set);
    QCOMPARE(region.unit, QStringLiteral("box"));
    QCOMPARE(region.lo[0], 0.0);
    QCOMPARE(region.hi[0], 8.0);
    QCOMPARE(region.lo[1], 2.0);   // a one-voxel box is one voxel wide
    QCOMPARE(region.hi[1], 4.0);
    QCOMPARE(region.lo[2], 14.0);
    QCOMPARE(region.hi[2], 20.0);
}

void TestAmrView::regionFromVoxelBox_handlesADescendingAxis()
{
    const std::array<AxisScale, 3> axes{ AxisScale{ 10.0, -1.0 }, AxisScale{ 0.0, 1.0 },
                                         AxisScale{ 0.0, 1.0 } };
    const Region region = regionFromVoxelBox({ 2, 4, 0, 0, 0, 0 }, axes);
    QVERIFY(region.set);
    QVERIFY(region.lo[0] < region.hi[0]);
    QCOMPARE(region.lo[0], 5.5);
    QCOMPARE(region.hi[0], 8.5);
}

void TestAmrView::regionFromVoxelBox_refusesAnAxisWithoutAStep()
{
    const std::array<AxisScale, 3> axes{ AxisScale{ 0.0, 1.0 }, AxisScale{ 0.0, 0.0 },
                                         AxisScale{ 0.0, 1.0 } };
    QVERIFY(!regionFromVoxelBox({ 0, 1, 0, 1, 0, 1 }, axes).set);
}

void TestAmrView::parseLevelMap_groupsRectanglesByLevel()
{
    const LevelMap map = parseLevelMap(reply(R"({
        "valid": true, "plane_axes": ["x", "y"], "length_unit": "kpc",
        "levels": {"6": [[0, 0, 4, 4]], "8": [[0, 0, 1, 1], [1, 0, 2, 1]]},
        "counts": {"6": 1, "8": 5}, "cells": 6, "truncated": true})"));
    QVERIFY(map.valid);
    QCOMPARE(map.levels.keys(), (QList<int>{ 6, 8 }));
    QCOMPARE(map.levels.value(8).size(), 2);
    QCOMPARE(map.levels.value(6).first(), (Rect{ 0, 0, 4, 4 }));
    QCOMPARE(map.counts.value(8), qint64(5));   // all the cells, though two are drawn
    QVERIFY(map.truncated);
    QCOMPARE(map.cells, qint64(6));
    QVERIFY(!parseLevelMap(reply(R"({"valid": true, "levels": {}})")).valid);
}

void TestAmrView::rectToVoxels_putsCellEdgesOnHalfPixels()
{
    // A cube starting at 1 kpc (voxel 0's centre), 2 kpc per voxel: a cell of
    // 4 kpc spanning 0..4 covers voxels 0 and 1, from -0.5 to 1.5.
    const AxisScale u{ 1.0, 2.0 }, v{ 1.0, 2.0 };
    const Rect r = rectToVoxels({ 0, 0, 4, 4 }, u, v);
    QCOMPARE(r[0], -0.5);
    QCOMPARE(r[1], -0.5);
    QCOMPARE(r[2], 1.5);
    QCOMPARE(r[3], 1.5);
}

void TestAmrView::planeAxes_dropTheNormal()
{
    QCOMPARE(planeAxes(0), (std::array<int, 2>{ 1, 2 }));
    QCOMPARE(planeAxes(1), (std::array<int, 2>{ 0, 2 }));
    QCOMPARE(planeAxes(2), (std::array<int, 2>{ 0, 1 }));
}

void TestAmrView::levelColour_runsCoarseToFine()
{
    const auto coarse = levelColour(6, 6, 10);
    const auto fine = levelColour(10, 6, 10);
    QVERIFY(coarse[2] > coarse[0]);   // blue at the coarse end
    QVERIFY(fine[0] > fine[2]);       // red at the fine end
    QCOMPARE(levelColour(5, 6, 10), coarse);    // outside the range clamps
    QCOMPARE(levelColour(7, 7, 7), coarse);     // a single level has one colour
}

void TestAmrView::cardValue_readsStringsAndNumbers()
{
    const QStringList cards{
        QStringLiteral("AMRSRC  = '/runs/it''s/output_00185' / source output").leftJustified(80),
        QStringLiteral("AMRLEV  =                    8 / target level").leftJustified(80) };
    QCOMPARE(cardValue(cards, QStringLiteral("AMRSRC")), QStringLiteral("/runs/it's/output_00185"));
    QCOMPARE(cardValue(cards, QStringLiteral("AMRLEV")), QStringLiteral("8"));
    QVERIFY(cardValue(cards, QStringLiteral("AMRFLD")).isEmpty());
    // A keyword that only starts like another one is not it.
    QVERIFY(cardValue(cards, QStringLiteral("AMR")).isEmpty());
}

void TestAmrView::cardValue_joinsContinuedStrings()
{
    // astropy sends a long string as the card and its CONTINUE records, in one
    // string of 80-character records.
    const QString card = QStringLiteral("AMRSRC  = '/very/long/part&'").leftJustified(80)
            + QStringLiteral("CONTINUE  'one/and/part&'").leftJustified(80)
            + QStringLiteral("CONTINUE  'two'").leftJustified(80);
    QCOMPARE(cardValue({ card }, QStringLiteral("AMRSRC")),
             QStringLiteral("/very/long/partone/and/parttwo"));
}

void TestAmrView::provenanceFromCards_needsTheSourceCard()
{
    QVERIFY(!provenanceFromCards({ QStringLiteral("OBJECT  = 'density'").leftJustified(80) }));
    const QStringList cards{
        QStringLiteral("AMRSRC  = '/runs/output_00185'").leftJustified(80),
        QStringLiteral("AMRFLD  = 'density'").leftJustified(80),
        QStringLiteral("AMRCODE = 'ramses'").leftJustified(80),
        QStringLiteral("AMRLEV  =                    8").leftJustified(80),
        QStringLiteral("AMRLMAX =                   11 / finest level").leftJustified(80) };
    const auto p = provenanceFromCards(cards);
    QVERIFY(p);
    QCOMPARE(p->source, QStringLiteral("/runs/output_00185"));
    QCOMPARE(p->field, QStringLiteral("density"));
    QCOMPARE(p->code, QStringLiteral("ramses"));
    QCOMPARE(p->level, 8);
    QCOMPARE(p->maxLevel, 11);
}
