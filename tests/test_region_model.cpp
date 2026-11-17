// test_region_model.cpp
// See TestRegionModel.h.

#include "TestRegionModel.h"

#include "CubeRegionGeom.h"
#include "RegionFrameText.h"
#include "RegionCubeSampling.h"
#include "RegionFullRes.h"
#include "RegionImportResult.h"
#include "RegionModel.h"
#include "RegionPanePlacement.h"
#include "RegionPlaneDrawing.h"
#include "RegionTreeRows.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace visivo::regions;

namespace {

Region box(double cx, double cy, double w, double h, double angle = 0.0)
{
    Region r;
    r.shape = Shape::Box;
    r.center = { cx, cy };
    r.size = { w, h };
    r.angleDeg = angle;
    return r;
}

Region circle(double cx, double cy, double rad)
{
    Region r;
    r.shape = Shape::Circle;
    r.center = { cx, cy };
    r.radius = rad;
    return r;
}

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) <= tol; }

/// The legacy int-voxel region, read back as a model region exactly the way the
/// viewer turns a drawn draft into one (through the backend's canonical form).
Region fromDraft(const QString &shape, std::array<int, 2> a, std::array<int, 2> c,
                 const std::vector<std::array<int, 2>> &poly = {}, double inner = 0.0)
{
    Region r;
    const QJsonObject o = visivo::regionInSourcePixels(shape, visivo::DisplayGrid{}, a, c, poly, inner);
    QString why;
    if (!fromCanonical(o, 0.0, &r, &why))
        qWarning() << "fromDraft:" << why;
    return r;
}

}   // namespace

void TestRegionModel::containsFollowsTheBackendRules()
{
    QVERIFY(contains(circle(10, 10, 3), 13, 10));     // the rim is in
    QVERIFY(!contains(circle(10, 10, 3), 13, 11));
    Region e;
    e.shape = Shape::Ellipse;
    e.center = { 0, 0 };
    e.semiAxes = { 4, 2 };
    e.angleDeg = 90;   // the major axis is now vertical
    QVERIFY(contains(e, 0, 4));
    QVERIFY(!contains(e, 4, 0));
    Region an;
    an.shape = Shape::Annulus;
    an.center = { 0, 0 };
    an.radii = { 2, 4 };
    QVERIFY(contains(an, 2, 0));    // both edges included
    QVERIFY(contains(an, 4, 0));
    QVERIFY(!contains(an, 1, 0));
    Region pt;
    pt.shape = Shape::Point;
    pt.center = { 5.2, 5.4 };
    QVERIFY(contains(pt, 5, 5));
    QVERIFY(!contains(pt, 6, 5));
    const Region rb = box(0, 0, 10, 2, 45);
    QVERIFY(contains(rb, 3, 3));
    QVERIFY(!contains(rb, 3, -3));
}

void TestRegionModel::aBoxFromCornersHoldsTheSamePixelsAsBefore()
{
    // The viewers counted pixels with CubeRegionGeom on int voxels; the model
    // must count the same ones, or every statistic would change under people.
    const std::array<int, 2> a{ 12, 30 }, c{ 4, 21 };
    const Region b = fromDraft(u"box"_qs, a, c);
    const Region ci = fromDraft(u"circle"_qs, a, c);
    const Region an = fromDraft(u"annulus"_qs, a, c, {}, 3.5);
    const std::vector<std::array<int, 2>> poly{ { 2, 2 }, { 20, 5 }, { 11, 25 }, { 6, 14 } };
    const Region pg = fromDraft(u"polygon"_qs, a, c, poly);
    QCOMPARE(b.shape, Shape::Box);
    QCOMPARE(an.shape, Shape::Annulus);
    for (int y = -5; y < 45; ++y) {
        for (int x = -5; x < 45; ++x) {
            QCOMPARE(contains(b, x, y), CubeRegionGeom::pointInBox(a, c, x, y));
            QCOMPARE(contains(ci, x, y), CubeRegionGeom::pointInCircle(a, c, x, y));
            QCOMPARE(contains(an, x, y), CubeRegionGeom::pointInAnnulus(a, c, 3.5, x, y));
            QCOMPARE(contains(pg, x, y), CubeRegionGeom::pointInPolygon(poly, x, y));
        }
    }
}

void TestRegionModel::displayedBoundsAreTheVoxelsInside()
{
    // Full resolution: a box from pixel 4 to 10 visits 4..10, not 3..11.
    const visivo::DisplayGrid full;
    const Region b = fromDraft(u"box"_qs, { 4, 20 }, { 10, 30 });
    const auto fb = visivo::displayedVoxelBounds(b, full);
    QCOMPARE(fb[0], 4);
    QCOMPARE(fb[1], 10);
    QCOMPARE(fb[2], 20);
    QCOMPARE(fb[3], 30);
    const auto cb = visivo::displayedVoxelBounds(circle(50, 50, 10), full);
    QCOMPARE(cb[0], 40);
    QCOMPARE(cb[1], 60);
    // A decimated level: voxels 2..4 at spacing 8 come back as 2..4.
    const visivo::DisplayGrid g{ 3.5, 3.5, 8.0, 8.0, 0, 0 };
    const auto r = visivo::regionFromDisplayed(u"box"_qs, g, { 2, 1 }, { 4, 3 }, {}, 0.0);
    QVERIFY(r.has_value());
    const auto db = visivo::displayedVoxelBounds(*r, g);
    QCOMPARE(db[0], 2);
    QCOMPARE(db[1], 4);
    QCOMPARE(db[2], 1);
    QCOMPARE(db[3], 3);
    // Every voxel inside lies within the bounds.
    for (int y = -2; y < 8; ++y)
        for (int x = -2; x < 8; ++x)
            if (visivo::displayedVoxelIn(*r, g, x, y))
                QVERIFY(x >= db[0] && x <= db[1] && y >= db[2] && y <= db[3]);
}

void TestRegionModel::boundsCoverRotatedShapes()
{
    const auto b = bounds(box(0, 0, 2, 2, 45));
    QVERIFY(near(b[1], std::sqrt(2.0)));
    QVERIFY(near(b[2], -std::sqrt(2.0)));
    Region e;
    e.shape = Shape::Ellipse;
    e.semiAxes = { 5, 1 };
    e.angleDeg = 90;
    const auto eb = bounds(e);
    QVERIFY(near(eb[1], 1.0));
    QVERIFY(near(eb[3], 5.0));
    // Every pixel inside lies within the bounds.
    for (int y = -6; y <= 6; ++y)
        for (int x = -6; x <= 6; ++x)
            if (contains(e, x, y))
                QVERIFY(x >= eb[0] && x <= eb[1] && y >= eb[2] && y <= eb[3]);
}

void TestRegionModel::hitTestPrefersSelectedHandlesThenTopmostBody()
{
    RegionList list;
    const int low = list.add(box(10, 10, 20, 20));
    const int top = list.add(circle(15, 15, 4));
    Hit hit;
    // Inside both: the topmost (last drawn) wins.
    QCOMPARE(list.regionAt({ 15, 15 }, 1.0, 5.0, &hit), top);
    QCOMPARE(hit.kind, Hit::Body);
    // Only inside the box.
    QCOMPARE(list.regionAt({ 2, 2 }, 1.0, 5.0, &hit), low);
    // Nowhere.
    QCOMPARE(list.regionAt({ 50, 50 }, 1.0, 5.0, &hit), -1);
    QCOMPARE(hit.kind, Hit::None);
    // The selected box's corner handle beats the circle drawn over it.
    list.select(low);
    QCOMPARE(list.regionAt({ 20.4, 20.3 }, 1.0, 5.0, &hit), low);
    QCOMPARE(hit.kind, Hit::OnHandle);
    QCOMPARE(hit.handle.kind, HandleKind::Corner);
    QCOMPARE(hit.handle.index, 2);
    // A small region drawn BEFORE a large one around it stays reachable.
    RegionList nested;
    const int small = nested.add(circle(50, 50, 3));
    const int large = nested.add(box(50, 50, 60, 60));
    QCOMPARE(nested.regionAt({ 50, 50 }, 1.0, 5.0, &hit), small);
    QCOMPARE(nested.regionAt({ 70, 70 }, 1.0, 5.0, &hit), large);
    QCOMPARE(nested.regionAt({ 79.6, 50 }, 1.0, 5.0, &hit), large);   // its outline
    // Near the outline of a thin box counts as the body.
    Region thin = box(0, 0, 10, 0.2);
    QCOMPARE(hitTest(thin, { 2, 0.9 }, 1.0, 5.0, false).kind, Hit::Body);
    QCOMPARE(hitTest(thin, { 2, 1.5 }, 1.0, 5.0, false).kind, Hit::None);
}

void TestRegionModel::movingShiftsEveryPosition()
{
    Region p;
    p.shape = Shape::Polygon;
    p.points = { { 0, 0 }, { 4, 0 }, { 0, 4 } };
    const Region m = moved(p, 1.5, -2);
    QCOMPARE(m.points[1][0], 5.5);
    QCOMPARE(m.points[2][1], 2.0);
    const Region c = moved(circle(1, 1, 2), 3, 4);
    QCOMPARE(c.center[0], 4.0);
    QCOMPARE(c.radius, 2.0);
}

void TestRegionModel::boxCornerDragKeepsTheOppositeCorner()
{
    const Region b = box(10, 10, 4, 2, 30);
    const auto before = boxCorners(b);
    Handle corner{ HandleKind::Corner, 2, before[2] };
    const Region d = withHandleAt(b, corner, { before[2][0] + 3, before[2][1] + 1 });
    const auto after = boxCorners(d);
    QVERIFY(near(after[0][0], before[0][0], 1e-9));
    QVERIFY(near(after[0][1], before[0][1], 1e-9));
    QCOMPARE(d.angleDeg, 30.0);
    // The dragged corner lands under the cursor.
    QVERIFY(near(after[2][0], before[2][0] + 3, 1e-9));
    QVERIFY(near(after[2][1], before[2][1] + 1, 1e-9));
    // Dragged across the opposite corner, the box flips instead of going negative.
    const Region f = withHandleAt(b, corner, { before[0][0] - 2, before[0][1] - 2 });
    QVERIFY(f.size[0] > 0 && f.size[1] > 0);
}

void TestRegionModel::radiiAxesAndRotationFollowTheCursor()
{
    const Region c = withHandleAt(circle(0, 0, 2), { HandleKind::Radius, 0, { 2, 0 } }, { 0, 7 });
    QCOMPARE(c.radius, 7.0);

    Region an;
    an.shape = Shape::Annulus;
    an.radii = { 2, 5 };
    QCOMPARE(withHandleAt(an, { HandleKind::InnerRadius, 0, {} }, { 3, 0 }).radii[0], 3.0);
    QCOMPARE(withHandleAt(an, { HandleKind::Radius, 0, {} }, { 0, 9 }).radii[1], 9.0);
    // The inner ring cannot pass the outer one, nor the outer the inner.
    QVERIFY(withHandleAt(an, { HandleKind::InnerRadius, 0, {} }, { 8, 0 }).radii[0] < 5.0);
    QVERIFY(withHandleAt(an, { HandleKind::Radius, 0, {} }, { 1, 0 }).radii[1] > 2.0);

    Region e;
    e.shape = Shape::Ellipse;
    e.semiAxes = { 4, 2 };
    e.angleDeg = 90;
    const Region major = withHandleAt(e, { HandleKind::MajorAxis, 0, {} }, { 0.3, 6 });
    QVERIFY(near(major.semiAxes[0], 6.0));
    QCOMPARE(major.semiAxes[1], 2.0);

    // The knob sits on +y: dragging it to +x turns the shape by -90 (= 270).
    const auto knob = handles(box(0, 0, 4, 2), 3.0).back();
    QCOMPARE(knob.kind, HandleKind::Rotate);
    QVERIFY(near(knob.pos[1], 4.0));
    const Region turned = withHandleAt(box(0, 0, 4, 2), knob, { 5, 0 });
    QVERIFY(near(turned.angleDeg, 270.0));
}

void TestRegionModel::polygonVertexDrag()
{
    Region p;
    p.shape = Shape::Polygon;
    p.points = { { 0, 0 }, { 4, 0 }, { 0, 4 } };
    const auto hs = handles(p, 0);
    QCOMPARE(int(hs.size()), 3);
    const Region d = withHandleAt(p, hs[1], { 6.5, 1 });
    QCOMPARE(d.points[1][0], 6.5);
    QCOMPARE(d.points[0][0], 0.0);
}

void TestRegionModel::sizesNeverCollapse()
{
    const Region c = withHandleAt(circle(0, 0, 2), { HandleKind::Radius, 0, {} }, { 0, 0 });
    QVERIFY(c.radius > 0);
    QVERIFY(isUsable(c));
    const Region b = box(0, 0, 4, 2);
    const Region z = withHandleAt(b, { HandleKind::Corner, 2, boxCorners(b)[2] }, boxCorners(b)[0]);
    QVERIFY(isUsable(z));
}

void TestRegionModel::listAddsNamesAndSelects()
{
    RegionList list;
    const int a = list.add(box(0, 0, 1, 1));
    const int b = list.add(circle(0, 0, 1));
    const int c = list.add(box(0, 0, 1, 1));
    QVERIFY(a != b && b != c);
    QCOMPARE(list.find(a)->name, u"Box 1"_qs);
    QCOMPARE(list.find(b)->name, u"Circle 1"_qs);
    QCOMPARE(list.find(c)->name, u"Box 2"_qs);
    QCOMPARE(list.selectedId(), c);
    QVERIFY(list.select(-1));
    QVERIFY(!list.select(999));
    QVERIFY(list.remove(b));
    QVERIFY(!list.find(b));
    QCOMPARE(list.count(), 2);
}

void TestRegionModel::undoAndRedoRestoreTheList()
{
    RegionList list;
    QVERIFY(!list.canUndo());
    const int a = list.add(box(0, 0, 1, 1));
    const int b = list.add(circle(0, 0, 1));
    QVERIFY(list.remove(a));
    QVERIFY(list.undo());               // the box is back, the circle still selected
    QVERIFY(list.find(a));
    QCOMPARE(list.selectedId(), b);
    QVERIFY(list.undo());               // before the circle
    QVERIFY(!list.find(b));
    QCOMPARE(list.selectedId(), a);
    QVERIFY(list.redo());
    QVERIFY(list.find(b));
    // A new change forgets what could be redone.
    list.add(box(1, 1, 1, 1));
    QVERIFY(!list.canRedo());
    // Bounded.
    for (int i = 0; i < RegionList::kUndoDepth + 20; ++i)
        list.add(circle(i, i, 1));
    int steps = 0;
    while (list.undo())
        ++steps;
    QCOMPARE(steps, RegionList::kUndoDepth);
}

void TestRegionModel::aDragIsOneUndoStep()
{
    RegionList list;
    const int id = list.add(circle(0, 0, 1));
    list.beginGesture();
    Region r = *list.find(id);
    for (int i = 1; i <= 10; ++i) {
        r = moved(r, 1, 0);
        QVERIFY(list.replace(r));
    }
    QVERIFY(list.endGesture());
    QCOMPARE(list.find(id)->center[0], 10.0);
    QVERIFY(list.undo());
    QCOMPARE(list.find(id)->center[0], 0.0);
    // A click without a move leaves nothing to undo.
    list.beginGesture();
    QVERIFY(!list.endGesture());
}

void TestRegionModel::canonicalRoundTripEveryShape()
{
    Region e;
    e.shape = Shape::Ellipse;
    e.center = { 3.25, 4.5 };
    e.semiAxes = { 5, 2 };
    e.angleDeg = 33;
    e.name = u"Source A"_qs;
    e.color = u"#ff8800"_qs;
    Region an;
    an.shape = Shape::Annulus;
    an.center = { 1, 2 };
    an.radii = { 0, 3 };
    Region pg;
    pg.shape = Shape::Polygon;
    pg.points = { { 0, 0 }, { 4.5, 0 }, { 0, 4 } };
    Region pt;
    pt.shape = Shape::Point;
    pt.center = { 7, 8 };
    for (const Region &r : { e, an, pg, pt, box(1, 2, 3, 4, 10), circle(5, 6, 7) }) {
        for (double origin : { 0.0, 1.0 }) {
            const QJsonObject o = toCanonical(r, origin);
            QCOMPARE(o[u"frame"_qs].toString(), u"image"_qs);
            Region back;
            QString why;
            QVERIFY2(fromCanonical(o, origin, &back, &why), qPrintable(why));
            QVERIFY(sameGeometry(back, r));
            QCOMPARE(back.name, r.name);
            QCOMPARE(back.color, r.color);
        }
    }
    // The 1-based form is what ds9 shows: pixel 0 is 1.
    QCOMPARE(toCanonical(circle(0, 9, 1), 1.0)[u"center"_qs].toArray()[0].toDouble(), 1.0);
}

void TestRegionModel::canonicalRejectsSkyAndDegenerateRegions()
{
    Region out;
    QString why;
    QJsonObject sky{ { u"shape"_qs, u"circle"_qs }, { u"frame"_qs, u"icrs"_qs },
                     { u"center"_qs, QJsonArray{ 10, 20 } }, { u"radius"_qs, 2 },
                     { u"unit"_qs, u"arcsec"_qs } };
    QVERIFY(!fromCanonical(sky, 0, &out, &why));
    QVERIFY(why.contains(u"icrs"_qs));
    QJsonObject flat{ { u"shape"_qs, u"box"_qs }, { u"frame"_qs, u"image"_qs },
                      { u"center"_qs, QJsonArray{ 10, 20 } }, { u"size"_qs, QJsonArray{ 0, 4 } } };
    QVERIFY(!fromCanonical(flat, 0, &out, &why));
    QJsonObject twoPoints{ { u"shape"_qs, u"polygon"_qs },
                           { u"points"_qs, QJsonArray{ QJsonArray{ 0, 0 }, QJsonArray{ 1, 1 } } } };
    QVERIFY(!fromCanonical(twoPoints, 0, &out, &why));
    QJsonObject line{ { u"shape"_qs, u"line"_qs }, { u"center"_qs, QJsonArray{ 1, 1 } } };
    QVERIFY(!fromCanonical(line, 0, &out, &why));
    QVERIFY(why.contains(u"line"_qs));
}

void TestRegionModel::sexagesimalFormatting()
{
    // M51: 13h29m52.698s +47d11m42.93s.
    const double ra = (13 + 29 / 60.0 + 52.698 / 3600.0) * 15.0;
    const double dec = 47 + 11 / 60.0 + 42.93 / 3600.0;
    QCOMPARE(formatSexagesimal(ra, true, 4), u"13:29:52.6980"_qs);
    QCOMPARE(formatSexagesimal(dec, false, 3), u"+47:11:42.930"_qs);
    QCOMPARE(formatSexagesimal(-0.5, false, 2), u"-00:30:00.00"_qs);
    // Rounding carries: 59.99996 s of time is the next minute, never ":60".
    QCOMPARE(formatSexagesimal((1 + 59.99996 / 3600.0) * 15.0, true, 4), u"01:01:00.0000"_qs);
    QCOMPARE(formatSexagesimal(359.9999999999, true, 4), u"00:00:00.0000"_qs);
}

void TestRegionModel::coordinateParsing()
{
    const double ra = (13 + 29 / 60.0 + 52.7 / 3600.0) * 15.0;
    QVERIFY(near(*parseCoordinate(u"13:29:52.7"_qs, true, true), ra, 1e-9));
    QVERIFY(near(*parseCoordinate(u"13h29m52.7s"_qs, true, true), ra, 1e-9));
    QVERIFY(near(*parseCoordinate(u"13 29 52.7"_qs, true, true), ra, 1e-9));
    QVERIFY(near(*parseCoordinate(u"202.4695"_qs, true, true), 202.4695, 1e-12));   // decimal = degrees
    QVERIFY(near(*parseCoordinate(u"202d28m10.2s"_qs, true, true), 202 + 28 / 60.0 + 10.2 / 3600.0, 1e-9));
    QVERIFY(near(*parseCoordinate(u"-5:23:10"_qs, false, true), -(5 + 23 / 60.0 + 10 / 3600.0), 1e-12));
    QVERIFY(near(*parseCoordinate(u"-5°23'10\""_qs, false, true), -(5 + 23 / 60.0 + 10 / 3600.0), 1e-12));
    QVERIFY(near(*parseCoordinate(u"-0:30:00"_qs, false, true), -0.5, 1e-12));   // the sign of a zero degree
    // Galactic longitudes are degrees even when written sexagesimal.
    QVERIFY(near(*parseCoordinate(u"10:30:00"_qs, true, false), 10.5, 1e-12));
    QVERIFY(near(*parseCoordinate(u"-10"_qs, true, false), 350.0, 1e-12));
    QVERIFY(!parseCoordinate(u"95"_qs, false, true));
    QVERIFY(!parseCoordinate(u"12:61:00"_qs, true, true));
    QVERIFY(!parseCoordinate(u"10h00m00s"_qs, false, true));   // a declination in hours
    QVERIFY(!parseCoordinate(u"abc"_qs, true, true));
    QVERIFY(!parseCoordinate(QString(), true, true));
    // What is printed reads back.
    QVERIFY(near(*parseCoordinate(formatSexagesimal(ra, true, 4), true, true), ra, 1e-6));
}

void TestRegionModel::framesOfferedFollowTheWcs()
{
    QCOMPARE(int(availableFrames(false).size()), 1);
    QCOMPARE(availableFrames(false).front(), FrameChoice::Pixels);
    QCOMPARE(int(availableFrames(true).size()), 6);
    QCOMPARE(frameChoiceFor(u"fk5"_qs), FrameChoice::Fk5Sexagesimal);
    QCOMPARE(frameChoiceFor(QString()), FrameChoice::Pixels);
    QCOMPARE(frameInfo(FrameChoice::GalacticDegrees).backendFrame, u"galactic"_qs);
    QCOMPARE(sizeUnit(FrameChoice::IcrsDegrees), u"arcsec"_qs);
    QCOMPARE(sizeUnit(FrameChoice::Pixels), u"pix"_qs);
}

void TestRegionModel::fieldsRoundTripThroughCanonical()
{
    // Pixel frame: what the editor shows is 1-based, and comes back 0-based.
    const Region b = box(9, 19, 4, 2, 15);
    const RegionFields f = fieldsFromCanonical(toCanonical(b, 1.0), FrameChoice::Pixels);
    QCOMPARE(f.center[0], u"10.000"_qs);
    QCOMPARE(f.center[1], u"20.000"_qs);
    QCOMPARE(int(f.sizes.size()), 2);
    QCOMPARE(f.angleDeg, 15.0);
    QJsonObject o;
    QString why;
    QVERIFY2(canonicalFromFields(f, FrameChoice::Pixels, &o, &why), qPrintable(why));
    Region back;
    QVERIFY2(fromCanonical(o, 1.0, &back, &why), qPrintable(why));
    QVERIFY(sameGeometry(back, b));

    // A sky region typed in sexagesimal becomes degrees in the canonical form.
    QJsonObject sky{ { u"shape"_qs, u"circle"_qs }, { u"frame"_qs, u"icrs"_qs },
                     { u"center"_qs, QJsonArray{ 202.469575, 47.195258 } }, { u"radius"_qs, 30.0 },
                     { u"unit"_qs, u"arcsec"_qs } };
    const RegionFields sf = fieldsFromCanonical(sky, FrameChoice::IcrsSexagesimal);
    QCOMPARE(sf.center[0], u"13:29:52.6980"_qs);
    QJsonObject typed;
    QVERIFY(canonicalFromFields(sf, FrameChoice::IcrsSexagesimal, &typed, &why));
    QCOMPARE(typed[u"frame"_qs].toString(), u"icrs"_qs);
    QCOMPARE(typed[u"unit"_qs].toString(), u"arcsec"_qs);
    QVERIFY(near(typed[u"center"_qs].toArray()[0].toDouble(), 202.469575, 5e-7));   // 0.1 ms of time
    QVERIFY(near(typed[u"center"_qs].toArray()[1].toDouble(), 47.195258, 5e-7));    // 1 mas

    // Polygons carry their vertices.
    Region pg;
    pg.shape = Shape::Polygon;
    pg.points = { { 0, 0 }, { 4.5, 0 }, { 0, 4 } };
    const RegionFields pf = fieldsFromCanonical(toCanonical(pg, 1.0), FrameChoice::Pixels);
    QCOMPARE(int(pf.vertices.size()), 3);
    QVERIFY(canonicalFromFields(pf, FrameChoice::Pixels, &o, &why));
    QVERIFY(fromCanonical(o, 1.0, &back, &why));
    QVERIFY(sameGeometry(back, pg));
}

void TestRegionModel::fieldsReportWhatIsWrong()
{
    RegionFields f;
    f.shape = Shape::Annulus;
    f.center = { u"13:29:52"_qs, u"+47:11:43"_qs };
    f.sizes = { 30, 10 };
    QJsonObject o;
    QString why;
    QVERIFY(!canonicalFromFields(f, FrameChoice::IcrsSexagesimal, &o, &why));
    QVERIFY(why.contains(u"inner radius"_qs));
    f.sizes = { 0, 10 };   // an annulus may have no hole
    QVERIFY(canonicalFromFields(f, FrameChoice::IcrsSexagesimal, &o, &why));
    f.center[1] = u"+97:00:00"_qs;
    QVERIFY(!canonicalFromFields(f, FrameChoice::IcrsSexagesimal, &o, &why));
    QVERIFY(why.startsWith(u"Centre"_qs));
    f.shape = Shape::Circle;
    f.center[1] = u"47"_qs;
    f.sizes = { 0 };
    QVERIFY(!canonicalFromFields(f, FrameChoice::IcrsSexagesimal, &o, &why));
    QVERIFY(why.contains(u"Radius"_qs));
}

// ── The 3-D view's samples of a region (RegionCubeSampling.h) ────────────────

void TestRegionModel::cubeSamplesFollowTheDisplaySampling()
{
    using CubeLodGeometry::Sampling;
    // A box over cube pixels 8..15 in x and 4..11 in y.
    const Region b = box(11.5, 7.5, 7.0, 7.0);
    // Full resolution (or a refined box at level 0): every pixel, 8 x 8.
    const AxisGrid full{ 0, 63, 0.0, 1.0 };
    QCOMPARE(int(samplesInRegion(b, full, full, Sampling{}).size()), 64);
    // A strided preview (stride 4, offset 0): samples at cube 0, 4, 8, 12, ...
    // The box holds cube 8 and 12 along x, and 4 and 8 along y.
    const AxisGrid preview{ 0, 15, 0.0, 1.0 };
    auto s = samplesInRegion(b, preview, preview, Sampling::strided(4));
    QCOMPARE(int(s.size()), 4);
    QCOMPARE(s.front()[0], 2);   // cube 8
    QCOMPARE(s.front()[1], 1);   // cube 4
    // A block-mean preview: sample k is the mean of 4k..4k+3, centred at
    // 4k + 1.5, so the box holds cube 9.5 and 13.5 along x, 5.5 and 9.5 along y.
    s = samplesInRegion(b, preview, preview, Sampling::blockMean(4));
    QCOMPARE(int(s.size()), 4);
    QCOMPARE(s.front()[0], 2);
    QCOMPARE(s.back()[0], 3);
    QVERIFY(near(sampleToCube(preview, Sampling::blockMean(4), 2), 9.5));
    QVERIFY(near(cubeToSample(preview, Sampling::blockMean(4), 9.5), 2.0));
    // A box read at full resolution with its origin at its corner (40, 30):
    // sample k is cube 40 + k.
    const AxisGrid roiX{ 0, 23, 40.0, 1.0 }, roiY{ 0, 23, 30.0, 1.0 };
    s = samplesInRegion(box(45.0, 32.0, 2.0, 0.0), roiX, roiY, Sampling{});
    QCOMPARE(int(s.size()), 3);
    QCOMPARE(s.front()[0], 4);   // cube 44
    QCOMPARE(s.front()[1], 2);   // cube 32
    // Smaller than the preview's spacing: the sample nearest its centre.
    s = samplesInRegion(circle(10.2, 6.1, 0.5), preview, preview, Sampling::strided(4));
    QCOMPARE(int(s.size()), 1);
    QCOMPARE(s.front()[0], 3);   // cube 12, nearest 10.2 (10.2 / 4 = 2.55)
    QCOMPARE(s.front()[1], 2);   // cube 8, nearest 6.1 (1.525 rounds to 2)
    // Off the displayed cube: nothing.
    QVERIFY(samplesInRegion(circle(500.0, 500.0, 2.0), preview, preview, Sampling::strided(4)).empty());
}

void TestRegionModel::addAllIsOneUndoStep()
{
    RegionList list;
    list.add(circle(1.0, 1.0, 1.0));
    const auto ids = list.addAll({ box(5.0, 5.0, 2.0, 2.0), circle(9.0, 9.0, 3.0) });
    QCOMPARE(int(ids.size()), 2);
    QCOMPARE(list.count(), 3);
    QCOMPARE(list.selectedId(), ids.front());
    QCOMPARE(list.find(ids.back())->name, QStringLiteral("Circle 2"));
    QVERIFY(list.undo());
    QCOMPARE(list.count(), 1);   // the whole import goes back at once
    QVERIFY(list.canRedo());
    QVERIFY(list.addAll({}).empty());
    QVERIFY(list.canRedo());     // nothing added: no undo step, redo kept
}

// ── Shapes read from region files (issue #342) ───────────────────────────────

namespace {
Region readCanonical(const char *json)
{
    Region r;
    QString why;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (!fromCanonical(o, 1.0, &r, &why))
        qWarning() << "readCanonical:" << why;
    return r;
}
}   // namespace

void TestRegionModel::newShapesRoundTripAndGeometry()
{
    // Each new shape as the backend returns it (image frame, 1-based).
    const Region line = readCanonical(R"j({"shape":"line","frame":"image","unit":"pix",
        "points":[[11,11],[21,11]],"source_line":3})j");
    QCOMPARE(line.shape, Shape::Line);
    QCOMPARE(line.sourceLine, 3);
    QVERIFY(!contains(line, 15.0, 10.0));   // no area
    QVERIFY(near(distanceToOutline(line, { 15.0, 12.0 }), 2.0));
    QCOMPARE(int(openPaths(line).size()), 1);
    QVERIFY(outlines(line).empty());

    const Region vec = readCanonical(R"j({"shape":"vector","frame":"image","unit":"pix",
        "center":[11,11],"length":10,"angle_deg":90,"props":{"arrow":true}})j");
    QCOMPARE(vec.shape, Shape::Vector);
    QVERIFY(near(vectorEnd(vec)[0], 10.0, 1e-9) && near(vectorEnd(vec)[1], 20.0, 1e-9));
    QCOMPARE(int(openPaths(vec).size()), 2);   // shaft and head
    QVERIFY(vec.props.value(QStringLiteral("arrow")).toBool());
    const Region turned = withHandleAt(vec, handles(vec, 5.0).front(), { 20.0, 10.0 });
    QVERIFY(near(turned.length, 10.0) && near(turned.angleDeg, 0.0));

    const Region text = readCanonical(R"j({"shape":"text","frame":"image","unit":"pix",
        "center":[5,6],"text":"NGC 1"})j");
    QCOMPARE(text.text, QStringLiteral("NGC 1"));
    QVERIFY(!hasArea(text.shape));

    const Region ea = readCanonical(R"j({"shape":"ellipse_annulus","frame":"image","unit":"pix",
        "center":[51,51],"inner":[4,2],"outer":[10,5],"angle_deg":0})j");
    QVERIFY(contains(ea, 57.0, 50.0));    // between the ellipses
    QVERIFY(!contains(ea, 52.0, 50.0));   // in the hole
    QVERIFY(!contains(ea, 50.0, 56.0));   // outside
    QCOMPARE(int(outlines(ea).size()), 2);
    const Region ba = readCanonical(R"j({"shape":"box_annulus","frame":"image","unit":"pix",
        "center":[51,51],"inner":[4,4],"outer":[10,6],"angle_deg":90})j");
    // Turned by 90 degrees: the outer box is 6 wide and 10 high on the grid.
    QVERIFY(contains(ba, 50.0, 54.0));
    QVERIFY(!contains(ba, 54.0, 50.0));
    QVERIFY(!contains(ba, 51.0, 50.0));   // in the hole
    const auto b = bounds(ba);
    QVERIFY(near(b[1] - b[0], 6.0, 1e-9) && near(b[3] - b[2], 10.0, 1e-9));

    // Back to canonical: sizes, the string, the properties and the file line.
    for (const Region &r : { line, vec, text, ea, ba }) {
        Region again;
        QVERIFY(fromCanonical(toCanonical(r, 1.0), 1.0, &again));
        QVERIFY(sameGeometry(r, again));
        QCOMPARE(again.text, r.text);
        QCOMPARE(again.sourceLine, r.sourceLine);
    }
    QVERIFY(toCanonical(vec, 1.0).value(QStringLiteral("props")).toObject()
                    .value(QStringLiteral("arrow")).toBool());

    // An annulus's outer edge never crosses the inner one under a drag.
    const Handle major = handles(ea, 5.0).front();
    QCOMPARE(major.kind, HandleKind::MajorAxis);
    QVERIFY(withHandleAt(ea, major, { 51.0, 50.0 }).outer[0] > ea.inner[0]);
    // A line keeps two vertices.
    Region bad;
    QVERIFY(!fromCanonical(QJsonDocument::fromJson(
                                   R"j({"shape":"line","frame":"image","points":[[1,1]]})j")
                                   .object(),
                           1.0, &bad));
}

void TestRegionModel::importKeepsEveryRegionAndSaysWhy()
{
    // A parse response: two rings of one annulus statement, a labelled circle,
    // a region the list cannot hold, and the backend's own diagnostic.
    const QString file = QStringLiteral("# Region file format: DS9\n"
                                        "image\n"
                                        "annulus(50,50,2,4,6)\n"
                                        "circle(10,10,3) # text={src} color=red\n"
                                        "panda(10,10,0,360,4,1,2,2)\n"
                                        "fk5;circle(0,0,0\")\n");
    const QJsonArray regions = QJsonDocument::fromJson(R"j([
        {"shape":"annulus","frame":"image","unit":"pix","center":[50,50],"radii":[2,4],"source_line":3},
        {"shape":"annulus","frame":"image","unit":"pix","center":[50,50],"radii":[4,6],"source_line":3},
        {"shape":"circle","frame":"image","unit":"pix","center":[10,10],"radius":3,"source_line":4,
         "props":{"text":"src","color":"red"}},
        {"shape":"circle","frame":"image","unit":"pix","center":[10,10],"radius":0,"source_line":6}
    ])j").array();
    const QJsonArray diags = QJsonDocument::fromJson(R"j([
        {"line":5,"text":"panda(10,10,0,360,4,1,2,2)","reason":"unsupported shape 'panda'"}
    ])j").array();
    const ImportResult r = importFromParse(regions, diags, false, 1, file);
    QCOMPARE(int(r.regions.size()), 3);
    QCOMPARE(r.regions[0].name, QStringLiteral("Annulus, line 3, ring 1/2"));
    QCOMPARE(r.regions[1].name, QStringLiteral("Annulus, line 3, ring 2/2"));
    QCOMPARE(r.regions[2].name, QStringLiteral("src"));
    QCOMPARE(r.regions[2].color, QStringLiteral("red"));
    // The backend's line and the one the list refused, in file order, each
    // with its text and reason.
    QCOMPARE(int(r.diagnostics.size()), 2);
    QCOMPARE(r.diagnostics[0].line, 5);
    QCOMPARE(r.diagnostics[1].line, 6);
    QCOMPARE(r.diagnostics[1].text, QStringLiteral("fk5;circle(0,0,0\")"));
    QVERIFY(r.diagnostics[1].reason.contains(QStringLiteral("no size")));
    QVERIFY(importSummary(r).contains(QStringLiteral("2 not read")));

    // The two rings move together; the circle alone.
    RegionList list;
    const auto ids = list.addAll(r.regions);
    QCOMPARE(int(list.ringGroup(ids[0]).size()), 2);
    QCOMPARE(int(list.ringGroup(ids[2]).size()), 1);
    QCOMPARE(r.notRead(), 2);   // one by the backend, one by the list
    // The same file imported again: its rings are a group of their own.
    const auto again = list.addAll(r.regions);
    QCOMPARE(int(list.ringGroup(again[0]).size()), 2);
    QCOMPARE(int(list.ringGroup(ids[0]).size()), 2);
}

// ── Regions on the cube's cutting plane (issue #343) ─────────────────────────

void TestRegionModel::planeAndRegionsShareOnePlacement()
{
    using CubeLodGeometry::Sampling;
    const int width = 101, height = 61, channel = 37;
    for (const Sampling &s : { Sampling{}, Sampling::strided(4), Sampling::blockMean(4),
                               Sampling::pyramidLevel(2) }) {
        const PlaneRect rect = cuttingPlane(width, height, channel, s);
        // Texel i of the full-resolution slice is centred at u = (i + 0.5) / W
        // on the plane: that is exactly where a region puts cube pixel i.
        for (int i : { 0, 7, 50, width - 1 }) {
            const double u = (i + 0.5) / width;
            const double texelX = rect.x0 + u * (rect.x1 - rect.x0);
            QVERIFY(near(texelX, planePoint({ double(i), 0.0 }, channel, s)[0], 1e-9));
            QCOMPARE(int(std::lround(planeToCube(texelX, s))), i);   // the pick goes back
        }
        for (int j : { 0, 30, height - 1 }) {
            const double v = (j + 0.5) / height;
            const double texelY = rect.y0 + v * (rect.y1 - rect.y0);
            QVERIFY(near(texelY, planePoint({ 0.0, double(j) }, channel, s)[1], 1e-9));
        }
        // Plane and regions at one height: the channel's.
        QVERIFY(near(rect.z, planePoint({ 3.0, 4.0 }, channel, s)[2], 1e-12));
        // And where the volume shows that channel's samples.
        QVERIFY(near(rect.z, CubeLodGeometry::toWorld(channel, s), 1e-12));
    }
    // Strided by 4: preview sample k shows cube voxel 4k, at world k. A region
    // corner on cube pixel 40 sits on preview sample 10, and channel 36 on
    // sample 9 (the old plane, at a fraction of the preview's depth, did not).
    QVERIFY(near(planePoint({ 40.0, 20.0 }, 36, Sampling::strided(4))[0], 10.0));
    QVERIFY(near(planePoint({ 40.0, 20.0 }, 36, Sampling::strided(4))[2], 9.0));
    // Block means of 4: sample k is the mean of 4k..4k+3, centred at 4k + 1.5.
    QVERIFY(near(planePoint({ 41.5, 0.0 }, 0, Sampling::blockMean(4))[0], 10.0));
    // Full resolution: the identity.
    QVERIFY(near(planePoint({ 12.25, 7.5 }, 3, Sampling{})[0], 12.25));
}

// ── Regions on the maps of the Product panes (issue #355) ────────────────────

void TestRegionModel::momentPaneMatchesTheSlice()
{
    using CubeLodGeometry::Sampling;
    // A 120 x 90 cube: the slice and a moment map are both served at full
    // resolution, origin 0, spacing 1, whatever the 3-D view shows (a strided
    // or block-mean preview of the same cube).
    const std::array<int, 2> cube{ 120, 90 };
    const MapPlacement slice = worldIsCube({ 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 });
    const auto moment = skyMapPlacement(cube, { 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 }, cube, {});
    QVERIFY(moment.has_value());
    for (const Vec2 &p : { Vec2{ 0.0, 0.0 }, Vec2{ 17.25, 33.5 }, Vec2{ 119.0, 89.0 } }) {
        const auto a = slice.toWorld(p), b = moment->toWorld(p);
        QVERIFY(near(a[0], b[0], 1e-12) && near(a[1], b[1], 1e-12));
        const Vec2 back = moment->toCube(b[0], b[1]);
        QVERIFY(near(back[0], p[0], 1e-12) && near(back[1], p[1], 1e-12));
    }
    // A box drawn on either view from the same voxels is the same region.
    const auto onSlice = visivo::regionFromDisplayed(QStringLiteral("box"), slice.displayGrid(), { 10, 20 },
                                                     { 30, 40 }, {}, 0.0);
    const auto onMoment = visivo::regionFromDisplayed(QStringLiteral("box"), moment->displayGrid(), { 10, 20 },
                                                      { 30, 40 }, {}, 0.0);
    QVERIFY(onSlice && onMoment);
    QVERIFY(visivo::regions::sameGeometry(*onSlice, *onMoment));
    // The 3-D preview does not enter the 2-D placement: under every display
    // sampling the region lands on the plane where the slice puts it.
    for (const Sampling &s : { Sampling{}, Sampling::strided(4), Sampling::blockMean(4) }) {
        const auto a = planePoint(onSlice->center, 5, s), b = planePoint(onMoment->center, 5, s);
        QVERIFY(near(a[0], b[0], 1e-12) && near(a[1], b[1], 1e-12));
    }
    // A reduced slice (a 2 x 2 block mean, world still in cube pixels: origin
    // 0.5, spacing 2) keeps the list where it is, and measures in blocks.
    const MapPlacement reduced = worldIsCube({ 0.5, 0.5 }, { 2.0, 2.0 }, { 0, 0 });
    const auto w = reduced.toWorld({ 41.0, 7.0 });
    QVERIFY(near(w[0], 41.0, 1e-12) && near(w[1], 7.0, 1e-12));
    QVERIFY(near(reduced.displayGrid().sourceX(3), 6.5, 1e-12));
    QVERIFY(near(reduced.cubePerWorld(), 1.0, 1e-12));
}

void TestRegionModel::croppedAndForeignMaps()
{
    const std::array<int, 2> cube{ 120, 90 };
    // RM synthesis over a box from cube pixel (40, 30), 25 x 20: map pixel k
    // is cube pixel 40 + k.
    const auto crop = skyMapPlacement({ 25, 20 }, { 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 }, cube,
                                      std::array<int, 2>{ 40, 30 });
    QVERIFY(crop.has_value());
    const auto w = crop->toWorld({ 45.0, 33.0 });
    QVERIFY(near(w[0], 5.0, 1e-12) && near(w[1], 3.0, 1e-12));
    const visivo::DisplayGrid g = crop->displayGrid();
    QVERIFY(near(g.sourceX(5), 45.0, 1e-12) && near(g.sourceY(3), 33.0, 1e-12));
    // A box over cube pixels 44..46 x 32..34 holds map pixels 4..6 x 2..4.
    Region box;
    box.shape = Shape::Box;
    box.center = { 45.0, 33.0 };
    box.size = { 3.0, 3.0 };
    const auto b = visivo::displayedVoxelBounds(box, g);
    QCOMPARE(b[0], 4);
    QCOMPARE(b[1], 6);
    QCOMPARE(b[2], 2);
    QCOMPARE(b[3], 4);
    QVERIFY(visivo::displayedVoxelIn(box, g, 4, 2));
    QVERIFY(!visivo::displayedVoxelIn(box, g, 7, 2));
    // A map whose VTK extent starts elsewhere still counts from its first voxel.
    const auto shifted = skyMapPlacement({ 25, 20 }, { 10.0, 10.0 }, { 1.0, 1.0 }, { 3, 3 }, cube,
                                         std::array<int, 2>{ 40, 30 });
    QVERIFY(shifted.has_value());
    QVERIFY(near(shifted->displayGrid().sourceX(3), 40.0, 1e-12));
    QVERIFY(near(shifted->toWorld({ 40.0, 30.0 })[0], 10.0, 1e-12));
    // Not on the sky grid: a crop running off the cube, a map of another size
    // with no crop (a fit over a selection's box), an empty map.
    QVERIFY(!skyMapPlacement({ 25, 20 }, { 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 }, cube,
                             std::array<int, 2>{ 100, 30 }));
    QVERIFY(!skyMapPlacement({ 60, 45 }, { 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 }, cube, {}));
    QVERIFY(!skyMapPlacement({ 0, 0 }, { 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 }, cube, {}));
}

void TestRegionModel::editsOnOnePaneShowOnTheOthers()
{
    // One list, three views: the slice, a full-field moment and a cropped
    // Faraday-depth map. A drag on the cropped map is read in list pixels, so
    // the slice and the moment show the region where it was dropped.
    const std::array<int, 2> cube{ 120, 90 };
    const MapPlacement slice = worldIsCube({ 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 });
    const auto moment = *skyMapPlacement(cube, { 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 }, cube, {});
    const auto crop = *skyMapPlacement({ 25, 20 }, { 0.0, 0.0 }, { 1.0, 1.0 }, { 0, 0 }, cube,
                                       std::array<int, 2>{ 40, 30 });
    RegionList list;
    Region c;
    c.shape = Shape::Circle;
    c.center = { 50.0, 40.0 };
    c.radius = 4.0;
    const int id = list.add(c);
    // Picked on the cropped map: its world position goes back to the list.
    const auto at = crop.toWorld(c.center);
    const Vec2 picked = crop.toCube(at[0], at[1]);
    Hit hit;
    QCOMPARE(list.regionAt(picked, 0.5, 1.0, &hit), id);
    // Dragged by 3 map pixels right and 2 down there: 3 and 2 cube pixels.
    const Vec2 from = crop.toCube(at[0], at[1]);
    const Vec2 to = crop.toCube(at[0] + 3.0, at[1] - 2.0);
    list.beginGesture();
    list.replace(visivo::regions::moved(*list.find(id), to[0] - from[0], to[1] - from[1]));
    list.endGesture();
    const Region &now = *list.find(id);
    QVERIFY(near(now.center[0], 53.0, 1e-12) && near(now.center[1], 38.0, 1e-12));
    const auto onSlice = slice.toWorld(now.center);
    const auto onMoment = moment.toWorld(now.center);
    const auto onCrop = crop.toWorld(now.center);
    QVERIFY(near(onSlice[0], 53.0, 1e-12) && near(onSlice[1], 38.0, 1e-12));
    QVERIFY(near(onMoment[0], onSlice[0], 1e-12) && near(onMoment[1], onSlice[1], 1e-12));
    QVERIFY(near(onCrop[0], 13.0, 1e-12) && near(onCrop[1], 8.0, 1e-12));
    // One undo step puts it back on all three.
    QVERIFY(list.undo());
    QVERIFY(near(list.find(id)->center[0], 50.0, 1e-12));
}

// ── The Remote 3-D view's plane (issue #356) ─────────────────────────────────

void TestRegionModel::remotePlaneDrawingIsTheLocalOne()
{
    RegionList list;
    Region box;
    box.shape = Shape::Box;
    box.center = { 12.0, 8.0 };
    box.size = { 8.0, 8.0 };
    box.color = QStringLiteral("#00ff00");
    box.props.insert(QStringLiteral("text"), QStringLiteral("src A"));
    const int boxId = list.add(box);
    Region point;
    point.shape = Shape::Point;
    point.center = { 3.25, 4.5 };
    list.add(point);
    Region line;
    line.shape = Shape::Line;
    line.points = { { 1.0, 1.0 }, { 2.0, 3.0 } };
    list.add(line);
    list.select(boxId);
    const auto colour = [](const Region &r) {
        return r.color.isEmpty() ? std::array<unsigned char, 3>{ 51, 230, 255 }
                                 : std::array<unsigned char, 3>{ 0, 255, 0 };
    };
    const QJsonObject cmd = planeDrawingCommand(list, colour);
    QCOMPARE(cmd.value(QStringLiteral("type")).toString(), QStringLiteral("set_regions"));
    const QJsonArray regions = cmd.value(QStringLiteral("regions")).toArray();
    QCOMPARE(regions.size(), 3);
    // The box: one closed outline through its corners, in cube pixels, as
    // the local overlay draws it; selected; its file label above its middle.
    const QJsonObject b = regions[0].toObject();
    QVERIFY(b.value(QStringLiteral("selected")).toBool());
    QCOMPARE(b.value(QStringLiteral("rgb")).toArray(), (QJsonArray{ 0, 255, 0 }));
    const QJsonArray loop = b.value(QStringLiteral("loops")).toArray().at(0).toArray();
    const auto corners = outlines(box).front();
    QCOMPARE(loop.size(), int(corners.size()));
    for (int i = 0; i < loop.size(); ++i) {
        QVERIFY(near(loop[i].toArray()[0].toDouble(), corners[std::size_t(i)][0], 1e-3));
        QVERIFY(near(loop[i].toArray()[1].toDouble(), corners[std::size_t(i)][1], 1e-3));
    }
    const QJsonObject label = b.value(QStringLiteral("labels")).toArray().at(0).toObject();
    QCOMPARE(label.value(QStringLiteral("text")).toString(), QStringLiteral("src A"));
    QCOMPARE(label.value(QStringLiteral("at")).toArray(), (QJsonArray{ 12.0, 12.0 }));
    // A point region is a marker, a line an open path.
    const QJsonObject p = regions[1].toObject();
    QVERIFY(!p.value(QStringLiteral("selected")).toBool());
    QCOMPARE(p.value(QStringLiteral("points")).toArray().at(0).toArray(), (QJsonArray{ 3.25, 4.5 }));
    QCOMPARE(p.value(QStringLiteral("rgb")).toArray(), (QJsonArray{ 51, 230, 255 }));
    QCOMPARE(regions[2].toObject().value(QStringLiteral("paths")).toArray().at(0).toArray().size(), 2);
    QVERIFY(!regions[2].toObject().contains(QStringLiteral("loops")));
    // The session's world is the cube's voxels at full resolution: the local
    // placement at scale 1 puts pixel (x, y) on channel c at (x, y, c), which
    // is where the session draws the pixel it is sent.
    const auto w = planePoint({ 12.0, 4.0 }, 37, CubeLodGeometry::Sampling{});
    QVERIFY(near(w[0], 12.0, 1e-12) && near(w[1], 4.0, 1e-12) && near(w[2], 37.0, 1e-12));
    // An empty list clears the plane.
    QVERIFY(planeDrawingCommand(RegionList(), colour).value(QStringLiteral("regions")).toArray().isEmpty());
}


// ── Session Data (#388) ──────────────────────────────────────────────────────

using namespace Qt::StringLiterals;

void TestRegionModel::hiddenRegionsAreNotPickedOrDrawn()
{
    RegionList list;
    const int big = list.add(box(10.0, 10.0, 20.0, 20.0));
    const int small = list.add(circle(10.0, 10.0, 2.0));
    // Inside both: the smaller one is picked; hidden, the press reaches the box.
    QCOMPARE(list.regionAt({ 10.0, 10.0 }, 0.5, 1.0, nullptr), small);
    QVERIFY(list.setVisible(small, false));
    QCOMPARE(list.regionAt({ 10.0, 10.0 }, 0.5, 1.0, nullptr), big);
    // On its outline too, and selected: a hidden region has no handles either.
    QVERIFY(list.select(small));
    QCOMPARE(list.regionAt({ 12.0, 10.0 }, 0.5, 1.0, nullptr), big);
    QVERIFY(list.setVisible(big, false));
    QCOMPARE(list.regionAt({ 10.0, 10.0 }, 0.5, 1.0, nullptr), -1);
    // The Remote 3-D view's plane gets the visible ones only.
    const auto colour = [](const Region &) { return std::array<unsigned char, 3>{ 0, 0, 0 }; };
    QVERIFY(planeDrawingCommand(list, colour).value(QStringLiteral("regions")).toArray().isEmpty());
    QVERIFY(list.setVisible(small, true));
    QCOMPARE(planeDrawingCommand(list, colour).value(QStringLiteral("regions")).toArray().size(), 1);
    // Not written to region files.
    QVERIFY(!toCanonical(*list.find(big), 1.0).contains(QStringLiteral("visible")));
}

void TestRegionModel::visibilityRenameAndDeleteAreUndoable()
{
    RegionList list;
    const int a = list.add(box(0, 0, 4, 4));
    const int b = list.add(circle(5, 5, 2));
    const int c = list.add(circle(9, 9, 1));
    QCOMPARE(list.selectedId(), c);

    // Hiding the selected region lets go of it; one undo step brings both back.
    QCOMPARE(list.setVisible({ b, c }, false), 2);
    QCOMPARE(list.hiddenCount(), 2);
    QCOMPARE(list.selectedId(), -1);
    QCOMPARE(list.setVisible({ b, c }, false), 0);   // nothing changes: no undo step
    QVERIFY(list.undo());
    QCOMPARE(list.hiddenCount(), 0);
    QCOMPARE(list.selectedId(), c);
    QVERIFY(list.redo());
    QCOMPARE(list.hiddenCount(), 2);
    QVERIFY(list.setVisible(b, true));
    QVERIFY(!list.setVisible(b, true));

    // Rename: trimmed, never empty, one step; a label the file drew follows.
    QVERIFY(!list.rename(a, u"   "_s));
    QVERIFY(!list.rename(999, u"x"_s));
    QVERIFY(list.rename(a, u"  core  "_s));
    QCOMPARE(list.find(a)->name, u"core"_s);
    QVERIFY(!list.rename(a, u"core"_s));
    QVERIFY(list.undo());
    QCOMPARE(list.find(a)->name, u"Box 1"_s);
    Region labelled = circle(1, 1, 1);
    labelled.name = u"NGC 3190"_s;
    labelled.props.insert(u"text"_s, u"NGC 3190"_s);
    const int l = list.add(labelled);
    QVERIFY(list.rename(l, u"NGC 3190 core"_s));
    QCOMPARE(list.find(l)->props.value(u"text"_s).toString(), u"NGC 3190 core"_s);
    QCOMPARE(toCanonical(*list.find(l), 1.0)
                     .value(u"props"_s).toObject().value(u"text"_s).toString(),
             u"NGC 3190 core"_s);
    // A drawn region has no label: renaming does not give it one on the image.
    QVERIFY(list.rename(b, u"disc"_s));
    QVERIFY(!list.find(b)->props.contains(u"text"_s));

    // Delete a group: one step, the selection goes with it.
    QVERIFY(list.select(b));
    const int before = list.count();
    QCOMPARE(list.removeAll({ a, b, 999 }), 2);
    QCOMPARE(list.count(), before - 2);
    QCOMPARE(list.selectedId(), -1);
    QCOMPARE(list.removeAll({ 999 }), 0);
    QVERIFY(list.undo());
    QCOMPARE(list.count(), before);
    QCOMPARE(list.selectedId(), b);
}

void TestRegionModel::importsGroupByFile()
{
    RegionList list;
    const int drawn = list.add(box(0, 0, 2, 2));
    const auto first = list.addAll({ circle(1, 1, 1), circle(2, 2, 1) }, u"hcg44.reg"_s);
    const auto second = list.addAll({ box(3, 3, 1, 1) }, u"hcg44.reg"_s);   // the same file again
    QCOMPARE(list.find(first.front())->sourceFile, u"hcg44.reg"_s);
    QVERIFY(list.find(drawn)->sourceFile.isEmpty());
    const RegionTreeView view = buildRegionTree(list);
    QCOMPARE(view.total, 4);
    QCOMPARE(int(view.entries.size()), 3);   // the box, then one group per import
    QCOMPARE(view.entries[0].batch, 0);
    QCOMPARE(view.entries[0].rows.front().id, drawn);
    QCOMPARE(view.entries[1].title, u"hcg44.reg (2)"_s);
    QCOMPARE(int(view.entries[1].rows.size()), 2);
    QCOMPARE(view.entries[2].title, u"hcg44.reg (1)"_s);
    QVERIFY(view.entries[1].batch != view.entries[2].batch);
    // The ids of a group, of every region; batch 0 is not a group.
    QCOMPARE(view.idsOf(view.entries[1].batch), first);
    QCOMPARE(int(view.idsOf(-1).size()), 4);
    QVERIFY(view.idsOf(0).empty());
    // Undo of the second import takes its group away.
    QVERIFY(list.undo());
    QCOMPARE(int(buildRegionTree(list).entries.size()), 2);
    Q_UNUSED(second);
}

void TestRegionModel::treeFollowsTheList()
{
    RegionList list;
    QVERIFY(buildRegionTree(list).isEmpty());   // no Regions node for an empty list
    Region named = circle(1, 1, 1);
    named.name = u"NGC 3190"_s;
    named.color = u"#00ff00"_s;
    const auto ids = list.addAll({ named, box(2, 2, 1, 1), box(3, 3, 1, 1) }, u"hcg44.reg"_s);
    RegionTreeView view = buildRegionTree(list);
    QCOMPARE(view.check, TreeCheck::Checked);
    QCOMPARE(view.entries[0].check, TreeCheck::Checked);
    // Name and shape; a generated name already says the shape.
    QCOMPARE(view.entries[0].rows[0].text, u"NGC 3190 · Circle"_s);
    QCOMPARE(view.entries[0].rows[0].color, u"#00ff00"_s);
    QCOMPARE(view.entries[0].rows[1].text, u"Box 1"_s);
    QCOMPARE(view.selectedId, ids.front());
    QCOMPARE(view.title(), u"Regions (3)"_s);
    const QString signature = view.signature();

    // Hidden in part: the group and the node are partly ticked, the row is
    // cleared, and the rows are the same (the tree is updated in place).
    QVERIFY(list.setVisible(ids[1], false));
    view = buildRegionTree(list);
    QCOMPARE(view.entries[0].check, TreeCheck::Partial);
    QCOMPARE(view.check, TreeCheck::Partial);
    QVERIFY(!view.entries[0].rows[1].visible);
    QVERIFY(view.entries[0].rows[1].tooltip.contains(u"hidden"_s));
    QCOMPARE(view.title(), u"Regions (3, 1 hidden)"_s);
    QCOMPARE(view.signature(), signature);
    // All hidden: cleared.
    QCOMPARE(list.setVisible(ids, false), 2);
    QCOMPARE(buildRegionTree(list).entries[0].check, TreeCheck::Unchecked);
    QCOMPARE(buildRegionTree(list).check, TreeCheck::Unchecked);
    // Renamed: same rows, new text. Deleted: other rows.
    QVERIFY(list.rename(ids[2], u"bar"_s));
    view = buildRegionTree(list);
    QCOMPARE(view.signature(), signature);
    QCOMPARE(view.entries[0].rows[2].text, u"bar · Box"_s);
    QVERIFY(list.remove(ids[2]));
    QVERIFY(buildRegionTree(list).signature() != signature);
    // A region drawn alone is an entry of its own, ticked or not by itself.
    const int drawn = list.add(circle(9, 9, 1));
    QVERIFY(list.setVisible(drawn, false));
    view = buildRegionTree(list);
    QCOMPARE(view.entries.back().batch, 0);
    QCOMPARE(view.entries.back().check, TreeCheck::Unchecked);
    QCOMPARE(view.selectedId, -1);   // hidden while selected: let go
}
