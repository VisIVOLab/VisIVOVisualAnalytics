// test_region_model.cpp
// See TestRegionModel.h.

#include "TestRegionModel.h"

#include "CubeRegionGeom.h"
#include "RegionFrameText.h"
#include "RegionCubeSampling.h"
#include "RegionFullRes.h"
#include "RegionModel.h"

#include <QJsonArray>
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
