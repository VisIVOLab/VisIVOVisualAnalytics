// test_snapshot_timeline_steps.cpp
// Unit tests for SnapshotTimelineSteps (issue #446): what a timeline step
// opens, what each replacement view takes back, and in what order.

#include "TestSnapshotTimelineSteps.h"
#include "CopilotActionRouter.h"
#include "SnapshotTimelineSteps.h"

#include <QJsonObject>
#include <QList>
#include <QScopedValueRollback>
#include <QTest>

#include <memory>

using namespace Qt::StringLiterals;
namespace Steps = SnapshotTimelineSteps;

namespace {

// A comparison view as far as the representation link goes: its shape, its
// flags, and the same send/receive rules and guard as the window's
// broadcastComparisonRepresentation / applyComparisonRepresentation.
struct FakeView
{
    QString geometry;
    bool inGroup{ true };
    bool linked{ true };
    bool representationLinked{ true };
    bool applying{ false };
    QList<FakeView *> *group{ nullptr };

    Steps::LinkFlags flags() const
    {
        return { inGroup, linked, representationLinked, applying };
    }
    // The combo's change signal: the window sends its representation.
    void setGeometry(const QString &shape)
    {
        geometry = shape;
        if (!Steps::representationSends(flags()))
            return;
        for (FakeView *peer : *group) {
            if (peer != this && Steps::representationReceives(peer->flags()))
                peer->applyRepresentation(shape);
        }
    }
    void applyRepresentation(const QString &shape)
    {
        const QScopedValueRollback<bool> guard(applying, true);
        setGeometry(shape);
    }
};

} // namespace

void TestSnapshotTimelineSteps::multiPieceCatalogue_hasNoTimeline()
{
    QVERIFY(Steps::timelineApplies(0));
    QVERIFY(Steps::timelineApplies(1));
    // snap_z0_1_1_1.vbt ... _8 opened together: pieces of one output.
    QVERIFY(!Steps::timelineApplies(8));
}

void TestSnapshotTimelineSteps::step_skipsOutputsThatCannotOpen()
{
    // Outputs 0 and 2 have no stars (or do not open).
    const QVector<bool> available{ false, true, false, true, true };
    QCOMPARE(Steps::stepTarget(available, 5, 1, 1), 3);
    QCOMPARE(Steps::stepTarget(available, 5, 3, -1), 1);
    QCOMPARE(Steps::stepTarget(available, 5, 1, -1), -1);   // nothing before
    QVERIFY(!Steps::outputAvailable(available, 2));
    QVERIFY(!Steps::outputAvailable(available, -1));
}

void TestSnapshotTimelineSteps::step_withoutMarks_takesTheNextOutput()
{
    // An older backend marks nothing: every output can be opened.
    QCOMPARE(Steps::stepTarget({}, 3, 0, 1), 1);
    QCOMPARE(Steps::stepTarget({}, 3, 2, -1), 1);
    QCOMPARE(Steps::stepTarget({}, 3, 1, 0), -1);
}

void TestSnapshotTimelineSteps::step_pastTheLastOpenable_isNone()
{
    // Play stops at the last output that opens, not in front of a gap.
    const QVector<bool> available{ true, true, false, false };
    QCOMPARE(Steps::stepTarget(available, 4, 1, 1), -1);
    QCOMPARE(Steps::stepTarget(available, 4, 3, 1), -1);
}

void TestSnapshotTimelineSteps::slider_landsOnTheNearestOpenable()
{
    const QVector<bool> available{ true, false, false, true, false };
    QCOMPARE(Steps::sliderTarget(available, 5, 0, 3), 3);   // openable: itself
    QCOMPARE(Steps::sliderTarget(available, 5, 0, 1), 3);   // onward, past the gap
    QCOMPARE(Steps::sliderTarget(available, 5, 0, 4), 3);   // nothing onward: back
    QCOMPARE(Steps::sliderTarget(available, 5, 3, 2), 0);   // dragged back
    // Only the output on show is left that way: nothing to open.
    QCOMPARE(Steps::sliderTarget({ true, false }, 2, 0, 1), -1);
    QCOMPARE(Steps::sliderTarget(available, 5, 3, 3), -1);
    QCOMPARE(Steps::sliderTarget(available, 5, 0, 7), -1);
}

void TestSnapshotTimelineSteps::seriesReply_invalidNeverHidesAValidSeries()
{
    QVERIFY(!Steps::acceptsSeriesReply(true, false));
    QVERIFY(Steps::acceptsSeriesReply(true, true));
    QVERIFY(Steps::acceptsSeriesReply(false, false));
    QVERIFY(Steps::acceptsSeriesReply(false, true));
}

void TestSnapshotTimelineSteps::seriesReply_keepsTheOutputsThatFailedHere()
{
    const QStringList held{ u"/r/snap_0"_s, u"/r/snap_1"_s, u"/r/snap_2"_s };
    // snap_1 failed to open here; the backend still lists it as openable,
    // and now lists one more output.
    const QStringList reply{ u"/r/snap_0"_s, u"/r/snap_1"_s, u"/r/snap_2"_s, u"/r/snap_3"_s };
    QCOMPARE(Steps::mergedAvailability(held, { true, false, true }, reply,
                                       { true, true, false, true }),
             QVector<bool>({ true, false, false, true }));
    // An older backend sends no marks; the failed one stays marked.
    QCOMPARE(Steps::mergedAvailability(held, { true, false, true }, reply, {}),
             QVector<bool>({ true, false, true, true }));
}

void TestSnapshotTimelineSteps::quantity_isRestoredByName()
{
    // Temperature was column 4; the next output lists one column more first.
    const QStringList next{ u"x"_s, u"y"_s, u"z"_s, u"mass"_s, u"rho"_s, u"temperature"_s };
    QCOMPARE(Steps::quantityKey(u"raw:4"_s, u"temperature"_s, next), u"raw:5"_s);
    // Case may differ between writers.
    QCOMPARE(Steps::quantityKey(u"raw:4"_s, u"Temperature"_s, next), u"raw:5"_s);
}

void TestSnapshotTimelineSteps::quantity_missingInTheOutput_isEmpty()
{
    // raw:4 would have been rho: another quantity, silently. Not any more.
    const QStringList early{ u"x"_s, u"y"_s, u"z"_s, u"mass"_s, u"rho"_s };
    QVERIFY(Steps::quantityKey(u"raw:4"_s, u"temperature"_s, early).isEmpty());
    QVERIFY(Steps::quantityKey(u"raw:4"_s, QString(), early).isEmpty());
}

void TestSnapshotTimelineSteps::quantity_builtInKey_isKept()
{
    QCOMPARE(Steps::quantityKey(u"speed"_s, QString(), { u"x"_s }), u"speed"_s);
}

void TestSnapshotTimelineSteps::rebuild_framesFirstThenKeepsTheStepCamera()
{
    using Steps::Rebuild;
    // The step's new window: its first build frames the data ...
    QVERIFY(!Steps::rebuildKeepsCamera(false, Rebuild::NewRows, false));
    QVERIFY(!Steps::rebuildKeepsCamera(false, Rebuild::SameRows, true));
    // ... then the step gives back the camera, and the rows' other columns
    // arrive: that rebuild keeps it, linked or not.
    QVERIFY(Steps::rebuildKeepsCamera(true, Rebuild::SameRows, false));
    QVERIFY(Steps::rebuildKeepsCamera(true, Rebuild::SameRows, true));
    // A filter or Load more in a linked view keeps the group's camera.
    QVERIFY(Steps::rebuildKeepsCamera(true, Rebuild::NewRows, true));
    // Alone (or the last of a group), a filter frames what it kept.
    QVERIFY(!Steps::rebuildKeepsCamera(true, Rebuild::NewRows, false));
    // Another frame or cosmology moves every point: framed again.
    QVERIFY(!Steps::rebuildKeepsCamera(true, Rebuild::Placement, true));
}

void TestSnapshotTimelineSteps::representation_receiversNeedEveryFlag()
{
    Steps::LinkFlags f{ true, true, true, false };
    QVERIFY(Steps::representationSends(f));
    QVERIFY(Steps::representationReceives(f));
    f.applying = true;
    QVERIFY(!Steps::representationSends(f));   // no echo
    QVERIFY(Steps::representationReceives(f));
    QVERIFY(!Steps::representationSends({ false, true, true, false }));
    QVERIFY(!Steps::representationReceives({ true, false, true, false }));
    QVERIFY(!Steps::representationReceives({ true, true, false, false }));
}

void TestSnapshotTimelineSteps::restore_unlinkedPointView_doesNotTurnLinkedSplatsIntoPoints()
{
    // Before the step: A linked with Gaussian splats, B unlinked with Points.
    // After it: A' and B', B' made from A' and so starting with A's flags.
    QList<FakeView *> group;
    FakeView a2, b2;
    a2.group = b2.group = &group;
    group = { &a2, &b2 };
    a2.geometry = b2.geometry = u"Ellipsoid"_s;   // a new window's default

    Steps::restoreWithOwnFlags(a2.linked, a2.representationLinked, a2.applying, true, true,
                               [&]() { a2.setGeometry(u"Gaussian splats"_s); });
    Steps::restoreWithOwnFlags(b2.linked, b2.representationLinked, b2.applying, false, true,
                               [&]() { b2.setGeometry(u"Points"_s); });

    QCOMPARE(a2.geometry, u"Gaussian splats"_s);
    QCOMPARE(b2.geometry, u"Points"_s);
    QVERIFY(!b2.linked);
    QVERIFY(!a2.applying);
    QVERIFY(!b2.applying);
    // And after the step the link works again for a user's change.
    a2.setGeometry(u"Points"_s);
    QCOMPARE(b2.geometry, u"Points"_s);   // unlinked: not reached, kept its own
    b2.linked = true;
    a2.setGeometry(u"Gaussian splats"_s);
    QCOMPARE(b2.geometry, u"Gaussian splats"_s);
}

void TestSnapshotTimelineSteps::restore_oldOrder_didReachTheLinkedView()
{
    // The order the step used before #446: controls first, flags after, no
    // guard. B' still had A's flags and sent its Points to A'.
    QList<FakeView *> group;
    FakeView a2, b2;
    a2.group = b2.group = &group;
    group = { &a2, &b2 };
    a2.setGeometry(u"Gaussian splats"_s);
    b2.setGeometry(u"Points"_s);
    b2.linked = false;
    QCOMPARE(a2.geometry, u"Points"_s);
}

void TestSnapshotTimelineSteps::copilot_routesEachComparisonViewByItsOwnKey()
{
    auto *router = CopilotActionRouter::instance();
    int toA = 0, toB = 0;
    auto a = std::make_unique<QObject>();
    auto b = std::make_unique<QObject>();
    const QString dataset = u"ds-446"_s;
    router->registerTarget(
            dataset, a.get(), [&toA](const QList<QJsonObject> &) { ++toA; }, []() {},
            u"view-a"_s);
    router->registerTarget(
            dataset, b.get(), [&toB](const QList<QJsonObject> &) { ++toB; }, []() {},
            u"view-b"_s);
    const QList<QJsonObject> reset{ QJsonObject{ { u"type"_s, u"reset_camera"_s } } };

    // View A's panel answers: A acts, not B (registered after it).
    QCOMPARE(router->dispatch(reset, u"view-a"_s), QStringList{ u"view-a"_s });
    QCOMPARE(toA, 1);
    QCOMPARE(toB, 0);
    // A directive naming the dataset: the view registered last, as before.
    const QList<QJsonObject> targeted{
        QJsonObject{ { u"type"_s, u"reset_camera"_s }, { u"target"_s, dataset } } };
    QCOMPARE(router->dispatch(targeted, QString()), QStringList{ dataset });
    QCOMPARE(toB, 1);
    // The backend tags every directive with the asking panel's dataset: from
    // view A's panel, retargeted to A's own key, it reaches A, not B.
    const auto fromA = CopilotActionRouter::retarget(targeted, dataset, u"view-a"_s);
    QCOMPARE(fromA.first().value(u"target"_s).toString(), u"view-a"_s);
    router->dispatch(fromA, u"view-a"_s);
    QCOMPARE(toA, 2);
    QCOMPARE(toB, 1);
    // Another dataset's directive is left alone.
    const QList<QJsonObject> other{
        QJsonObject{ { u"type"_s, u"reset_camera"_s }, { u"target"_s, u"ds-other"_s } } };
    QCOMPARE(CopilotActionRouter::retarget(other, dataset, u"view-a"_s).first()
                     .value(u"target"_s).toString(),
             u"ds-other"_s);
    // B closes: the dataset's directives reach A.
    b.reset();
    router->dispatch(targeted, QString());
    QCOMPARE(toA, 3);
    a.reset();
    QVERIFY(router->dispatch(targeted, QString()).isEmpty());
}
