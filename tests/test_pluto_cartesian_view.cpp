// test_pluto_cartesian_view.cpp — see TestPlutoCartesianView.h.

#include "TestPlutoCartesianView.h"
#include "app/PlutoCartesianView.h"

#include <QJsonDocument>
#include <QTest>

using namespace visivo::plutoview;

namespace {

QJsonObject reply(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

const char *kMeridional = R"({
    "valid": true, "path": "/runs/sph/data.0003.flt::pluto-var=rho",
    "index_reason": "spherical geometry",
    "plan": {"geometry": "SPHERICAL", "mode": "meridional", "axes": ["X", "Z"],
             "lower": [0.0, -2.0], "upper": [2.0, 2.0], "shape": [200, 400],
             "cell": [0.01, 0.01], "mirror": false, "voxels": 80000, "bytes_estimate": 320000}})";

}   // namespace

void TestPlutoCartesianView::parsePlan_readsTheBackendReply()
{
    const Plan plan = parsePlan(reply(kMeridional));
    QVERIFY(plan.valid);
    QCOMPARE(plan.geometry, QStringLiteral("SPHERICAL"));
    QCOMPARE(plan.mode, QStringLiteral("meridional"));
    QCOMPARE(plan.axes, (QStringList{ "X", "Z" }));
    QCOMPARE(plan.shape, (QVector<int>{ 200, 400 }));
    QCOMPARE(plan.upper, (QVector<double>{ 2.0, 2.0 }));
    QCOMPARE(plan.voxels, qint64(80000));
    QCOMPARE(plan.indexReason, QStringLiteral("spherical geometry"));
    QVERIFY(plan.path.endsWith(QStringLiteral("::pluto-var=rho")));
}

void TestPlutoCartesianView::parsePlan_refusesAReplyWithoutAGrid()
{
    QVERIFY(!parsePlan(reply(R"({"valid": true})")).valid);
    // Axes and voxel counts that do not match are not a plan.
    QVERIFY(!parsePlan(reply(R"({"plan": {"axes": ["X", "Y"], "shape": [10]}})")).valid);
}

void TestPlutoCartesianView::parseCubes_listsTheOutputs()
{
    const QVector<Cube> cubes = parseCubes(reply(R"({"outputs": [
        {"field": "rho", "path": "/x/rho.pluto-cartesian.fits", "shape": [8, 8, 8]},
        {"field": "prs", "path": "/x/prs.pluto-cartesian.fits", "shape": [8, 8, 8]}]})"));
    QCOMPARE(cubes.size(), 2);
    QCOMPARE(cubes[1].field, QStringLiteral("prs"));
    QVERIFY(cubes[0].path.endsWith(QStringLiteral(".pluto-cartesian.fits")));
    QCOMPARE(cubes[0].shape, (QVector<int>{ 8, 8, 8 }));
    QVERIFY(parseCubes(reply("{}")).isEmpty());
}

void TestPlutoCartesianView::mirror_isOnlyForAMeridionalImage()
{
    QVERIFY(canMirror(parsePlan(reply(kMeridional))));
    Plan cube;
    cube.mode = QStringLiteral("cube");
    QVERIFY(!canMirror(cube));
    cube.mode = QStringLiteral("plane");
    QVERIFY(!canMirror(cube));
}

void TestPlutoCartesianView::summary_namesTheViewTheGridAndTheBox()
{
    const QString text = summary(parsePlan(reply(kMeridional)));
    QVERIFY(text.contains(QStringLiteral("meridional x-z image")));
    QVERIFY(text.contains(QStringLiteral("spherical")));
    QVERIFY(text.contains(QStringLiteral("200 × 400")));
    QVERIFY(text.contains(QStringLiteral("z -2 to 2")));
    QVERIFY(text.contains(QStringLiteral("0.3 MiB")));
    QCOMPARE(modeLabel(QStringLiteral("cube")), QStringLiteral("Cartesian cube"));
    QCOMPARE(modeLabel(QStringLiteral("grid")), QStringLiteral("uniform grid"));
}

void TestPlutoCartesianView::requestBody_carriesTheBudgetAndOnlyWhatIsAsked()
{
    QJsonObject body = requestBody(QStringLiteral("/r/dbl.out"), 64000000, false, false);
    QCOMPARE(body.value("path").toString(), QStringLiteral("/r/dbl.out"));
    QCOMPARE(body.value("max_voxels").toDouble(), 64000000.0);
    QCOMPARE(body.value("mirror").toBool(), false);
    QVERIFY(!body.contains("fields"));
    body = requestBody(QStringLiteral("/r/dbl.out"), 1000000, true, true);
    QCOMPARE(body.value("mirror").toBool(), true);
    QCOMPARE(body.value("fields").toArray().first().toString(), QStringLiteral("*"));
}

void TestPlutoCartesianView::voxelBudget_isInMillions()
{
    QCOMPARE(voxelBudget(64), qint64(64000000));
    QCOMPARE(voxelBudget(0), qint64(1000000));
    QCOMPARE(voxelBudget(1000), qint64(1000000000));
}

void TestPlutoCartesianView::isCartesianViewPath_knowsTheBackendsFileName()
{
    // Cells outside the run's domain are NaN there: the sanity check must not
    // call that damage.
    QVERIFY(isCartesianViewPath(QStringLiteral("/exports/rho.pluto-cartesian.fits")));
    QVERIFY(!isCartesianViewPath(QStringLiteral("/data/rho.fits")));
}
