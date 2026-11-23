// test_catalogue_selection.cpp
// See TestCatalogueSelection.h.

#include "TestCatalogueSelection.h"

#include "CatalogueSelectionState.h"
#include "SubfindTargetFraming.h"

#include <QTest>

#include <cmath>

using namespace visivo;

namespace {

BackendTabularFilter region(const QString &name)
{
    BackendTabularFilter f;
    f.shape = QStringLiteral("sphere");
    f.op = QStringLiteral("inside");
    f.name = name;
    f.fields = { QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z") };
    f.radius = 1.0;
    return f;
}

BackendTabularFilter predicate(const QString &field, const QString &value)
{
    BackendTabularFilter f;
    f.field = field;
    f.op = QStringLiteral(">");
    f.value = value;
    return f;
}

double distance(const framing::Camera &c)
{
    const double dx = c.position[0] - c.focal[0];
    const double dy = c.position[1] - c.focal[1];
    const double dz = c.position[2] - c.focal[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

void TestCatalogueSelection::regionNamesAreOfferedInTurn()
{
    QCOMPARE(selection::nextRegionName({}), QStringLiteral("Region 1"));
    const std::vector<BackendTabularFilter> filters{ region(QStringLiteral("Region 1")),
                                                     predicate(QStringLiteral("mass"),
                                                               QStringLiteral("1e10")),
                                                     region(QStringLiteral("Region 3")) };
    // The gap is filled first; field predicates have no name to take.
    QCOMPARE(selection::nextRegionName(selection::regionNames(filters)),
             QStringLiteral("Region 2"));
    QCOMPARE(selection::nextRegionName({ QStringLiteral("Region 1"), QStringLiteral("region 2") }),
             QStringLiteral("Region 3"));
}

void TestCatalogueSelection::aRegionNameInUseIsRefused()
{
    const QStringList taken{ QStringLiteral("Northern outflow"), QStringLiteral("Region 1") };
    QVERIFY(selection::isTakenName(QStringLiteral("Region 1"), taken));
    // Trimmed and without case: these read as the same name in the list.
    QVERIFY(selection::isTakenName(QStringLiteral("  northern OUTFLOW "), taken));
    QVERIFY(!selection::isTakenName(QStringLiteral("Region 2"), taken));
    // An empty name shows the shape instead and never clashes.
    QVERIFY(!selection::isTakenName(QStringLiteral("   "), { QString() }));
}

void TestCatalogueSelection::editingARegionKeepsItsOwnName()
{
    const std::vector<BackendTabularFilter> filters{ region(QStringLiteral("Halo A")),
                                                     region(QStringLiteral("Halo B")) };
    const QStringList others = selection::regionNames(filters, 0);
    QVERIFY(!selection::isTakenName(QStringLiteral("Halo A"), others));
    QVERIFY(selection::isTakenName(QStringLiteral("Halo B"), others));
}

void TestCatalogueSelection::diagnosticsFollowTheAppliedFilters()
{
    selection::AppliedFilters applied;
    std::vector<BackendTabularFilter> edited{ predicate(QStringLiteral("mass"),
                                                        QStringLiteral("1e10")) };
    QVERIFY(applied.filters().empty());

    // Applied: the snapshot is a copy; editing the list afterwards does not
    // reach it.
    QVERIFY(applied.begin(edited));
    edited.push_back(region(QStringLiteral("Region 1")));
    QCOMPARE(applied.filters().size(), std::size_t(1));
    QVERIFY(applied.succeed());   // the population changed: diagnostics recompute

    // The same list applied again changes nothing to recompute.
    QVERIFY(!applied.begin(applied.filters()));
    QVERIFY(!applied.succeed());

    // A different value of the same predicate is a different population.
    std::vector<BackendTabularFilter> stricter{ predicate(QStringLiteral("mass"),
                                                          QStringLiteral("1e11")) };
    QVERIFY(applied.begin(stricter));
    QVERIFY(applied.succeed());
    QCOMPARE(applied.filters().front().value, QStringLiteral("1e11"));
    // Nothing pending: succeed and fail are no-ops.
    QVERIFY(!applied.succeed());
    QVERIFY(!applied.fail());
}

void TestCatalogueSelection::aFailedQueryPutsThePreviousFiltersBack()
{
    selection::AppliedFilters applied;
    applied.begin({ region(QStringLiteral("Region 1")) });
    applied.succeed();
    applied.begin({ region(QStringLiteral("Region 1")), region(QStringLiteral("Region 2")) });
    QCOMPARE(applied.filters().size(), std::size_t(2));   // the Remote view's, meanwhile
    QVERIFY(applied.fail());
    QCOMPARE(applied.filters().size(), std::size_t(1));
    QCOMPARE(applied.filters().front().name, QStringLiteral("Region 1"));

    // A window opened on another's rows starts from its applied list.
    selection::AppliedFilters peer;
    peer.adopt(applied.filters());
    QVERIFY(selection::sameFilters(peer.filters(), applied.filters()));
    QVERIFY(!peer.fail());
}

void TestCatalogueSelection::loadAllKeepsTheAppliedFilters()
{
    // Applied: region 1. Edited since, not applied: region 1 and 2.
    selection::AppliedFilters applied;
    applied.begin({ region(QStringLiteral("Region 1")) });
    applied.succeed();
    const std::vector<BackendTabularFilter> edited{ region(QStringLiteral("Region 1")),
                                                    region(QStringLiteral("Region 2")) };
    // Load all queries the applied list again without beginning an Apply:
    // whatever the query does, the applied list is not the edited one.
    QVERIFY(!applied.succeed());
    QVERIFY(!applied.fail());
    QCOMPARE(applied.filters().size(), std::size_t(1));
    QVERIFY(!selection::sameFilters(applied.filters(), edited));
}

void TestCatalogueSelection::aPeerOpenedMidApplyTakesTheSettledFilters()
{
    selection::AppliedFilters source;
    source.begin({ region(QStringLiteral("Region 1")) });
    source.succeed();
    // An Apply is out: the rows a Compare window copies are still Region 1's.
    source.begin({ region(QStringLiteral("Region 2")) });
    QCOMPARE(source.settled().front().name, QStringLiteral("Region 1"));
    selection::AppliedFilters peer;
    peer.adopt(source.settled());
    source.fail();
    QVERIFY(selection::sameFilters(peer.filters(), source.filters()));
    // Settled once the query answers.
    source.begin({ region(QStringLiteral("Region 3")) });
    source.succeed();
    QCOMPARE(source.settled().front().name, QStringLiteral("Region 3"));
}

void TestCatalogueSelection::theFramingDistanceGrowsWithTheRadius()
{
    // 30 degrees: 1.25 r / tan(15 deg).
    const double d = framing::framingDistance(2.0, 30.0);
    QVERIFY(std::abs(d - 2.0 * 1.25 / std::tan(15.0 * M_PI / 180.0)) < 1e-12);
    QVERIFY(std::abs(framing::framingDistance(4.0, 30.0) - 2.0 * d) < 1e-12);
    // A very wide view never puts the camera inside the halo.
    QCOMPARE(framing::framingDistance(1.0, 170.0), 1.5);
    // No radius, no distance; a nonsense angle falls back to 30 degrees.
    QCOMPARE(framing::framingDistance(0.0, 30.0), 0.0);
    QCOMPARE(framing::framingDistance(-1.0, 30.0), 0.0);
    QCOMPARE(framing::framingDistance(2.0, 0.0), d);
}

void TestCatalogueSelection::framingKeepsTheViewDirection()
{
    framing::Camera cam;
    cam.position = { 0.0, 0.0, 100.0 };
    cam.focal = { 0.0, 0.0, 0.0 };
    const framing::Camera framed = framing::frameTarget(cam, { 10.0, 20.0, 30.0 }, 2.0, 3.0);
    QCOMPARE(framed.focal, (std::array<double, 3>{ 10.0, 20.0, 30.0 }));
    // Looked at from +z still, at the radius' distance (the zoom is ignored).
    QCOMPARE(framed.position[0], 10.0);
    QCOMPARE(framed.position[1], 20.0);
    QVERIFY(std::abs(distance(framed) - framing::framingDistance(2.0, 30.0)) < 1e-9);
    QCOMPARE(framed.viewUp, cam.viewUp);

    // A parallel view is framed by its scale.
    cam.parallel = true;
    QCOMPARE(framing::frameTarget(cam, { 0.0, 0.0, 0.0 }, 2.0, 1.0).parallelScale, 2.5);

    // A camera on its focal point has no direction to keep: from the front.
    framing::Camera degenerate;
    degenerate.position = degenerate.focal = { 1.0, 1.0, 1.0 };
    const framing::Camera front = framing::frameTarget(degenerate, { 0.0, 0.0, 0.0 }, 1.0, 1.0);
    QCOMPARE(front.position[0], 0.0);
    QVERIFY(front.position[2] > 0.0);
}

void TestCatalogueSelection::aRecentreKeepsTheDistance()
{
    framing::Camera cam;
    cam.position = { 3.0, 4.0, 0.0 };
    cam.focal = { 0.0, 0.0, 0.0 };
    // No radius: the distance divided by the zoom factor.
    QVERIFY(std::abs(distance(framing::frameTarget(cam, { 1.0, 1.0, 1.0 }, 0.0, 1.0)) - 5.0)
            < 1e-12);
    QVERIFY(std::abs(distance(framing::frameTarget(cam, { 1.0, 1.0, 1.0 }, 0.0, 2.5)) - 2.0)
            < 1e-12);
}

void TestCatalogueSelection::anUnmovedViewIsTheSameView()
{
    framing::Camera cam;
    cam.position = { 0.0, 0.0, framing::framingDistance(2.0, 30.0) };
    cam.focal = { 0.0, 0.0, 0.0 };
    // Already framed: the status line must not claim it moved.
    QVERIFY(framing::sameView(cam, framing::frameTarget(cam, { 0.0, 0.0, 0.0 }, 2.0, 3.0)));
    QVERIFY(!framing::sameView(cam, framing::frameTarget(cam, { 0.0, 0.1, 0.0 }, 2.0, 3.0)));
    QVERIFY(!framing::sameView(cam, framing::frameTarget(cam, { 0.0, 0.0, 0.0 }, 4.0, 3.0)));
}

void TestCatalogueSelection::theSizedRadiusIsPreferred()
{
    QCOMPARE(framing::radiusFieldOrder().first(), QStringLiteral("rvir"));
    QCOMPARE(framing::radiusFieldOrder(QStringLiteral("r500")).first(), QStringLiteral("r500"));
    QCOMPARE(framing::radiusFieldOrder(QStringLiteral("r500")).size(), 5);
    // A size column that is not a radius changes nothing.
    QCOMPARE(framing::radiusFieldOrder(QStringLiteral("mvir")), framing::radiusFieldOrder());
}

void TestCatalogueSelection::radiiSayWhatTheyAreDefinedAgainst()
{
    QCOMPARE(framing::radiusWithDefinition(QStringLiteral("rvir"),
                                           QStringLiteral("mean background density")),
             QStringLiteral("rvir (relative to the mean background density)"));
    QCOMPARE(framing::radiusWithDefinition(QStringLiteral("r500"),
                                           QStringLiteral("500 times mean background density")),
             QStringLiteral("r500 (500 times the mean background density)"));
    QCOMPARE(framing::radiusWithDefinition(QStringLiteral("mass"), QString()),
             QStringLiteral("mass"));
    // The status line names the target, so it replaces the selection line.
    QCOMPARE(framing::framingStatus(QStringLiteral("Row 1"), QStringLiteral("rvir"),
                                    QStringLiteral("mean background density"), 250.0),
             QStringLiteral("Framed Row 1 at 1.25 × rvir (relative to the mean background "
                            "density) = 250."));
    QCOMPARE(framing::framingStatus(QString(), QStringLiteral("rmax"), QString(), 1.5),
             QStringLiteral("Framed target at 1.25 × rmax = 1.5."));
}

void TestCatalogueSelection::cameraJsonRoundTrips()
{
    framing::Camera cam;
    cam.position = { 1.0, 2.0, 3.0 };
    cam.focal = { 4.0, 5.0, 6.0 };
    cam.viewUp = { 0.0, 0.0, 1.0 };
    cam.viewAngle = 45.0;
    cam.parallel = true;
    cam.parallelScale = 7.0;
    const auto back = framing::cameraFromJson(framing::cameraToJson(cam));
    QVERIFY(back.has_value());
    QCOMPARE(back->position, cam.position);
    QCOMPARE(back->focal, cam.focal);
    QCOMPARE(back->viewUp, cam.viewUp);
    QCOMPARE(back->viewAngle, 45.0);
    QVERIFY(back->parallel);
    QCOMPARE(back->parallelScale, 7.0);
    // No camera (a view not drawn yet, a failed request): nothing to frame.
    QVERIFY(!framing::cameraFromJson(QJsonObject()).has_value());
    QJsonObject partial = framing::cameraToJson(cam);
    partial.insert(QStringLiteral("view_up"), QJsonArray{ 0.0, 1.0 });
    QVERIFY(!framing::cameraFromJson(partial).has_value());
}

using namespace Qt::StringLiterals;

void TestCatalogueSelection::aOneClickConditionJoinsTheAppliedFiltersOnly()
{
    // Applied: a region and a type of 4. Edited but not applied: a cut on rho.
    visivo::selection::AppliedFilters applied;
    applied.begin({ region(u"Halo"_s), predicate(u"type"_s, u"4"_s) });
    QVERIFY(applied.succeed());   // filtered now, unfiltered before
    const std::vector<BackendTabularFilter> edits{ region(u"Halo"_s),
                                                   predicate(u"type"_s, u"4"_s),
                                                   predicate(u"rho"_s, u"2"_s) };
    // What the view now queries: the applied list, type 4 replaced by 0.
    const auto query = visivo::selection::withOnlyCondition(applied.filters(), u"type"_s,
                                                            u"="_s, u"0"_s);
    QCOMPARE(int(query.size()), 2);
    QCOMPARE(query[0].shape, u"sphere"_s);
    QCOMPARE(query[1].field, u"type"_s);
    QCOMPARE(query[1].value, u"0"_s);
    for (const BackendTabularFilter &f : query)
        QVERIFY(f.field != u"rho"_s);   // the pending edit is not applied
    applied.begin(query);
    QVERIFY(applied.succeed());
    QVERIFY(visivo::selection::sameFilters(applied.filters(), query));
    // The list keeps the pending edit, with the condition, for its own Apply.
    const auto list = visivo::selection::withOnlyCondition(edits, u"type"_s, u"="_s, u"0"_s);
    QCOMPARE(int(list.size()), 3);
    QCOMPARE(list[1].field, u"rho"_s);
    QCOMPARE(list[2].field, u"type"_s);
    QCOMPARE(list[2].value, u"0"_s);
    // A second click changes nothing.
    QVERIFY(visivo::selection::sameFilters(
            visivo::selection::withOnlyCondition(query, u"type"_s, u"="_s, u"0"_s), query));
}
