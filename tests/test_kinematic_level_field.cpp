#include "TestKinematicLevelField.h"

#include "KinematicLevelField.h"

#include <QTest>

using namespace KinematicLevelField;

namespace {

/// Build a field from explicit per-voxel levels, x-fastest — the same order the
/// backend serialises and the same order the mask comes back in.
Input makeField(int w, int h, int d, const QVector<int> &add, const QVector<int> &sub,
                int ox = 0, int oy = 0, int oz = 0)
{
    Input in;
    in.width = w; in.height = h; in.depth = d;
    in.originX = ox; in.originY = oy; in.originZ = oz;
    in.addLevels.resize(add.size());
    in.subLevels.resize(sub.size());
    for (int i = 0; i < add.size(); ++i)
        in.addLevels[i] = char(quint8(add[i]));
    for (int i = 0; i < sub.size(); ++i)
        in.subLevels[i] = char(quint8(sub[i]));
    return in;
}

QVector<int> filled(int n, int value)
{
    return QVector<int>(n, value);
}

bool maskAt(const Result &r, int w, int h, int x, int y, int z)
{
    const auto *m = reinterpret_cast<const float *>(r.mask.constData());
    return m[(qsizetype(z) * h + y) * w + x] != 0.0f;
}

}  // namespace

// ── the contract the slider depends on ──────────────────────────────────────

void TestKinematicLevelField::thresholdSelectsLevelsUpToTheBudget()
{
    // A 1-D ramp of levels 0..4: budget r must select exactly the first r+1.
    const Input in = makeField(5, 1, 1, { 0, 1, 2, 3, 4 }, filled(5, kUnreachable));
    for (int reach = 0; reach <= 4; ++reach) {
        const Result r = thresholdAt(in, reach);
        QVERIFY(r.valid);
        QCOMPARE(r.count, reach + 1);
        for (int x = 0; x <= 4; ++x)
            QCOMPARE(maskAt(r, 5, 1, x, 0, 0), x <= reach);
    }
}

void TestKinematicLevelField::selectionGrowsMonotonicallyWithTheBudget()
{
    // Raising the slider may only ADD voxels: the UI relies on that to feel like
    // one control rather than a recomputation with its own mind.
    const Input in = makeField(4, 3, 2,
                               { 0, 1, 2, 3,  1, 2, 3, 4,  2, 3, 4, 5,
                                 3, 4, 5, 6,  4, 5, 6, 7,  5, 6, 7, 8 },
                               filled(24, kUnreachable));
    Result prev = thresholdAt(in, 0);
    QVERIFY(prev.valid);
    for (int reach = 1; reach <= 9; ++reach) {
        const Result cur = thresholdAt(in, reach);
        QVERIFY(cur.valid);
        QVERIFY2(cur.count >= prev.count, "a larger budget dropped voxels");
        const auto *p = reinterpret_cast<const float *>(prev.mask.constData());
        const auto *c = reinterpret_cast<const float *>(cur.mask.constData());
        for (int i = 0; i < 24; ++i) {
            if (p[i] != 0.0f)
                QVERIFY2(c[i] != 0.0f, "a voxel selected at a smaller budget was dropped");
        }
        prev = cur;
    }
}

void TestKinematicLevelField::seedLevelZeroIsSelectedAtBudgetZero()
{
    // The clicked voxel is level 0, so even the smallest budget keeps it — a
    // slider at its minimum must never show an empty selection on a good click.
    const Input in = makeField(3, 1, 1, { 5, 0, 5 }, filled(3, kUnreachable));
    const Result r = thresholdAt(in, 0);
    QVERIFY(r.valid);
    QCOMPARE(r.count, 1);
    QVERIFY(maskAt(r, 3, 1, 1, 0, 0));
}

void TestKinematicLevelField::unreachableIsNeverSelected()
{
    // 255 is the sentinel; budgets are capped below it, so it must stay out even
    // at the largest budget the field can carry.
    const Input in = makeField(2, 1, 1, { 0, kUnreachable }, filled(2, kUnreachable));
    const Result r = thresholdAt(in, 254);
    QVERIFY(r.valid);
    QCOMPARE(r.count, 1);
    QVERIFY(!maskAt(r, 2, 1, 1, 0, 0));
}

void TestKinematicLevelField::negativeBudgetSelectsNothing()
{
    const Input in = makeField(2, 1, 1, { 0, 1 }, filled(2, kUnreachable));
    const Result r = thresholdAt(in, -1);
    QVERIFY2(r.valid, "a negative budget is empty, not malformed");
    QCOMPARE(r.count, 0);
}

// ── carve seeds ─────────────────────────────────────────────────────────────

void TestKinematicLevelField::subLevelsCarveAtTheSameBudget()
{
    // Carving is evaluated at the SAME budget as the grow, so a voxel the carve
    // seed cannot reach yet stays selected until it can.
    const Input in = makeField(3, 1, 1, { 0, 0, 0 }, { kUnreachable, 2, kUnreachable });
    const Result atOne = thresholdAt(in, 1);
    QCOMPARE(atOne.count, 3);                 // carve has not reached the middle
    const Result atTwo = thresholdAt(in, 2);
    QCOMPARE(atTwo.count, 2);                 // now it has
    QVERIFY(!maskAt(atTwo, 3, 1, 1, 0, 0));
}

void TestKinematicLevelField::carveCanEmptyTheSelectionWithoutBeingAnError()
{
    // An empty result is a real answer, not a malformed field: the caller has to
    // be able to tell the two apart or it will leave a stale surface on screen.
    const Input in = makeField(2, 1, 1, { 0, 1 }, { 0, 1 });
    const Result r = thresholdAt(in, 4);
    QVERIFY(r.valid);
    QCOMPARE(r.count, 0);
}

void TestKinematicLevelField::allSentinelSubFieldCarvesNothing()
{
    // What the backend sends when there are no carve seeds at all.
    const Input in = makeField(4, 1, 1, { 0, 1, 2, 3 }, filled(4, kUnreachable));
    const Result r = thresholdAt(in, 3);
    QCOMPARE(r.count, 4);
}

// ── geometry ────────────────────────────────────────────────────────────────

void TestKinematicLevelField::maskIsXFastestMatchingTheBackendLayout()
{
    // Round-trip the x-fastest convention: place ONE voxel at a linear index and
    // require the reported bbox to name the (x, y, z) that convention implies.
    //
    // Asserting the mask instead would prove nothing — it is copied by linear
    // index, so it cannot disagree with itself. The decomposition in the bbox is
    // the only place an axis can actually be swapped, and the asymmetric extents
    // below (3 x 4 x 2, with x != y != z) are what make a swap observable.
    const int w = 3, h = 4, d = 2;
    struct Pos { int x, y, z; };
    for (const Pos p : { Pos{ 1, 2, 1 }, Pos{ 2, 0, 0 }, Pos{ 0, 3, 1 } }) {
        QVector<int> add = filled(w * h * d, kUnreachable);
        add[(qsizetype(p.z) * h + p.y) * w + p.x] = 0;
        const Input in = makeField(w, h, d, add, filled(w * h * d, kUnreachable));
        const Result r = thresholdAt(in, 0);
        QCOMPARE(r.count, 1);
        QVERIFY(maskAt(r, w, h, p.x, p.y, p.z));
        const std::array<int, 6> expected{ p.x, p.x, p.y, p.y, p.z, p.z };
        QCOMPARE(r.bbox, expected);
    }
}

void TestKinematicLevelField::bboxIsReportedInFullCubeVoxels()
{
    // The ROI is a window into the cube; Apply and the 2-D overlay both read the
    // bbox as cube coordinates, so the origin has to be added in.
    const int w = 3, h = 1, d = 1;
    const Input in = makeField(w, h, d, { kUnreachable, 0, kUnreachable },
                               filled(w * h * d, kUnreachable),
                               /*ox=*/100, /*oy=*/200, /*oz=*/300);
    const Result r = thresholdAt(in, 0);
    QCOMPARE(r.count, 1);
    const std::array<int, 6> expected{ 101, 101, 200, 200, 300, 300 };
    QCOMPARE(r.bbox, expected);
}

void TestKinematicLevelField::bboxCoversExactlyTheSelectedVoxels()
{
    // Two corners selected in a 4x4x1 field: the bbox must span them and nothing
    // wider (a stale initial value would show up as a full-ROI box).
    const int w = 4, h = 4, d = 1;
    QVector<int> add = filled(w * h * d, kUnreachable);
    add[(0 * h + 1) * w + 1] = 0;             // (x=1, y=1)
    add[(0 * h + 3) * w + 2] = 0;             // (x=2, y=3)
    const Input in = makeField(w, h, d, add, filled(w * h * d, kUnreachable));
    const Result r = thresholdAt(in, 0);
    QCOMPARE(r.count, 2);
    const std::array<int, 6> expected{ 1, 2, 1, 3, 0, 0 };
    QCOMPARE(r.bbox, expected);
}

void TestKinematicLevelField::countMatchesTheNonZeroVoxelsInTheMask()
{
    // Invariant across the whole budget range on a MIXED field: the reported count
    // and the mask are produced in the same loop, so a divergence would mean one
    // of the two was written on a path the other missed.
    const Input in = makeField(4, 2, 2,
                               { 0, 1, 2, 3,  1, 2, 3, 4,
                                 2, 3, 4, 5,  3, 4, 5, 6 },
                               { 9, 2, 9, 9,  9, 9, 3, 9,
                                 9, 9, 9, 4,  5, 9, 9, 9 });
    for (int reach = 0; reach <= 9; ++reach) {
        const Result r = thresholdAt(in, reach);
        QVERIFY(r.valid);
        const auto *m = reinterpret_cast<const float *>(r.mask.constData());
        int nonZero = 0;
        for (int i = 0; i < 16; ++i)
            if (m[i] != 0.0f)
                ++nonZero;
        QCOMPARE(r.count, nonZero);
    }
}

// ── malformed input ─────────────────────────────────────────────────────────

void TestKinematicLevelField::rejectsMismatchedFieldSizes()
{
    Input in = makeField(4, 1, 1, { 0, 1, 2, 3 }, filled(4, kUnreachable));
    in.subLevels.chop(1);                     // truncated payload
    const Result r = thresholdAt(in, 2);
    QVERIFY2(!r.valid, "a short field must be rejected, not indexed past its end");
}

void TestKinematicLevelField::rejectsNonPositiveDimensions()
{
    for (const auto dims : { std::array<int, 3>{ 0, 1, 1 }, std::array<int, 3>{ 1, -1, 1 },
                             std::array<int, 3>{ 1, 1, 0 } }) {
        Input in;
        in.width = dims[0]; in.height = dims[1]; in.depth = dims[2];
        QVERIFY(!thresholdAt(in, 1).valid);
    }
}

void TestKinematicLevelField::rejectsDimensionsThatWouldOverflowTheProduct()
{
    // Three values near INT_MAX overflow even a 64-bit product, so each side is
    // bounded BEFORE the multiply; without that the size check below it would
    // pass on a nonsense payload.
    Input in;
    in.width = in.height = in.depth = std::numeric_limits<int>::max();
    QVERIFY(!thresholdAt(in, 1).valid);
}

void TestKinematicLevelField::rejectsAnOriginOutsideTheCube()
{
    // The bbox is reported in CUBE voxels, so a stale or malformed response whose
    // ROI starts before the cube or runs off its end would produce coordinates
    // outside the data — which the caller would then hand to Apply.
    const QVector<int> add{ 0, 0, 0, 0 };
    const QVector<int> sub = filled(4, kUnreachable);
    const auto withOrigin = [&](int ox, int oy, int oz, int fw, int fh, int fd) {
        Input in = makeField(2, 2, 1, add, sub, ox, oy, oz);
        in.fullWidth = fw; in.fullHeight = fh; in.fullDepth = fd;
        return in;
    };
    QVERIFY(thresholdAt(withOrigin(0, 0, 0, 10, 10, 10), 0).valid);       // inside
    QVERIFY(thresholdAt(withOrigin(8, 8, 9, 10, 10, 10), 0).valid);       // flush to the end
    QVERIFY(!thresholdAt(withOrigin(-1, 0, 0, 10, 10, 10), 0).valid);     // before the start
    QVERIFY(!thresholdAt(withOrigin(9, 0, 0, 10, 10, 10), 0).valid);      // runs off in x
    QVERIFY(!thresholdAt(withOrigin(0, 0, 10, 10, 10, 10), 0).valid);     // runs off in z
    // An extreme origin must be caught by 64-bit arithmetic, not wrap back in.
    QVERIFY(!thresholdAt(withOrigin(std::numeric_limits<int>::max(), 0, 0, 10, 10, 10), 0).valid);
}

void TestKinematicLevelField::acceptsAnyOriginWhenNoFullShapeIsGiven()
{
    // A zero full shape means the caller did not supply one; the ROI is then taken
    // on its own terms rather than every input being rejected.
    Input in = makeField(2, 1, 1, { 0, 1 }, filled(2, kUnreachable), 5000, 6000, 7000);
    const Result r = thresholdAt(in, 1);
    QVERIFY(r.valid);
    QCOMPARE(r.bbox[0], 5000);
}
