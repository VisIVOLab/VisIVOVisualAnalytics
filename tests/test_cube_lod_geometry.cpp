// test_cube_lod_geometry.cpp
// Unit tests for CubeLodGeometry, the coordinates behind the 3-D view's level
// of detail (issue #321): the pyramid's level shapes, where the samples of the
// preview and of every level sit, nodes and ghosts, the cropping planes, and the
// kd-split around the fine box with its back-to-front order.
//
// Two functions are ports of backend code and are checked against what the
// backend itself returns, recorded in tests/data/cube_lod_golden.json. After a
// change to brick_pyramid.level_shapes or render_session/_ooc.py
// _vtk_crop_planes, regenerate it from the repository root:
//
//     backend/.venv/bin/python tests/data/cube_lod_golden.py > tests/data/cube_lod_golden.json
//
// The drawing orders are checked against rays, not against a second reading of
// the rule: for many rays from the eye, the boxes a ray crosses must be drawn
// farthest first.

#include "TestCubeLodGeometry.h"
#include "vtk/CubeLodGeometry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <map>
#include <random>

using namespace CubeLodGeometry;

namespace {

QJsonObject golden()
{
    QFile f(QStringLiteral(VISIVO_TEST_DATA_DIR "/cube_lod_golden.json"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

Dims dimsOf(const QJsonValue &v)
{
    const QJsonArray a = v.toArray();
    return { a.at(0).toInt(), a.at(1).toInt(), a.at(2).toInt() };
}

/// Where a ray from @p eye along @p dir enters the region a box of cube voxels
/// owns ([lo - 0.5, hi - 0.5]); false when it misses. 0 when the eye is inside.
bool entry(const Box &b, const std::array<double, 3> &eye, const std::array<double, 3> &dir, double *t)
{
    double t0 = 0.0;
    double t1 = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 3; ++i) {
        const double lo = b.lo[i] - 0.5;
        const double hi = b.hi[i] - 0.5;
        if (std::fabs(dir[i]) < 1e-15) {
            if (eye[i] < lo || eye[i] > hi)
                return false;
            continue;
        }
        double a = (lo - eye[i]) / dir[i];
        double c = (hi - eye[i]) / dir[i];
        if (a > c)
            std::swap(a, c);
        t0 = std::max(t0, a);
        t1 = std::min(t1, c);
    }
    if (!(t1 > t0 + 1e-9))
        return false;
    *t = t0;
    return true;
}

/// True when, along every one of @p rays random rays from @p eye, the boxes the
/// ray crosses come in @p drawn (first drawn first) farthest first. On failure
/// @p why says which pair.
bool backToFrontAlongRays(const std::vector<Box> &drawn, const std::array<double, 3> &eye, int rays,
                          QString *why)
{
    std::mt19937 rng(7);
    std::normal_distribution<double> g(0.0, 1.0);
    for (int r = 0; r < rays; ++r) {
        std::array<double, 3> dir{ g(rng), g(rng), g(rng) };
        std::vector<std::pair<double, int>> hits;   // (entry, draw position)
        for (int k = 0; k < int(drawn.size()); ++k) {
            double t;
            if (entry(drawn[size_t(k)], eye, dir, &t))
                hits.emplace_back(t, k);
        }
        std::sort(hits.begin(), hits.end());
        for (size_t i = 1; i < hits.size(); ++i) {
            if (hits[i].first - hits[i - 1].first < 1e-7)
                continue;   // the ray grazes a shared edge: no order to keep
            // hits[i-1] is nearer, so it must be drawn later.
            if (hits[i - 1].second < hits[i].second) {
                *why = QStringLiteral("box %1 (nearer) drawn before box %2").arg(hits[i - 1].second).arg(hits[i].second);
                return false;
            }
        }
    }
    return true;
}

Box pieceBox(Side s, const Box &inner, const std::vector<Piece> &pieces)
{
    if (s == Side::Inner)
        return inner;
    for (const Piece &p : pieces)
        if (p.side == s)
            return p.box;
    return {};
}

}   // namespace

void TestCubeLodGeometry::levelShapes_matchTheBackendPyramid()
{
    const QJsonArray cases = golden().value(u"level_shapes").toArray();
    QVERIFY2(cases.size() >= 6, "tests/data/cube_lod_golden.json is missing or empty");
    for (const QJsonValue &c : cases) {
        const QJsonObject o = c.toObject();
        const std::vector<Dims> got = levelShapes(dimsOf(o.value(u"shape")), o.value(u"brick").toInt());
        const QJsonArray want = o.value(u"levels").toArray();
        QCOMPARE(int(got.size()), int(want.size()));
        for (int l = 0; l < want.size(); ++l) {
            QCOMPARE(got[size_t(l)], dimsOf(want.at(l)));
            QCOMPARE(levelShape(dimsOf(o.value(u"shape")), l), dimsOf(want.at(l)));
        }
    }
}

void TestCubeLodGeometry::levelShape_halvesRoundingUp()
{
    QCOMPARE(levelShape({ 7, 13, 1000 }, 0), (Dims{ 7, 13, 1000 }));
    QCOMPARE(levelShape({ 7, 13, 1000 }, 1), (Dims{ 4, 7, 500 }));
    QCOMPARE(levelShape({ 7, 13, 1000 }, 3), (Dims{ 1, 2, 125 }));
    QCOMPARE(levelShape({ 1, 1, 1 }, 5), (Dims{ 1, 1, 1 }));
    // Every level covers the cube: n_l * 2^l >= n.
    for (int n : { 1, 2, 3, 255, 257, 1000, 2049 })
        for (int l = 0; l < 12; ++l)
            QVERIFY(std::int64_t(levelShape({ n, n, n }, l)[0]) * levelScale(l) >= n);
    QCOMPARE(levelShapes({ 300, 10, 10 }, 0).size(), size_t(1));   // no brick: level 0 only
}

void TestCubeLodGeometry::sampling_previewOffsets()
{
    // A strided preview takes every s-th voxel: sample j is cube voxel j*s.
    const Sampling strided = Sampling::strided(4);
    QCOMPARE(strided.centre(3), 12.0);
    QCOMPARE(toWorld(12.0, strided), 3.0);
    // A block-mean preview (#320): sample j averages voxels j*s .. j*s+s-1,
    // so its centre is j*s + (s-1)/2; the world frame stays sample j at j.
    const Sampling mean = Sampling::blockMean(4);
    QCOMPARE(mean.centre(3), 13.5);
    QCOMPARE(toWorld(13.5, mean), 3.0);
    QCOMPARE(toWorld(12.0, mean), 2.625);
    for (double c : { -0.5, 0.0, 7.25, 1023.0 })
        QCOMPARE(toCube(toWorld(c, mean), mean), c);
    // Pyramid levels are block means; a strided read of a level is not.
    QCOMPARE(Sampling::pyramidLevel(0).offset, 0.0);
    QCOMPARE(Sampling::pyramidLevel(3).scale, 8.0);
    QCOMPARE(Sampling::pyramidLevel(3).offset, 3.5);
    QCOMPARE(Sampling::stridedLevel(3).offset, 0.0);
    // Stride 1 (or a nonsense stride) is the cube itself.
    QCOMPARE(Sampling::blockMean(1).offset, 0.0);
    QCOMPARE(Sampling::strided(0).scale, 1.0);
}

void TestCubeLodGeometry::placement_landsOnTheAveragedVoxels()
{
    // A block of level l whose sample 0 is level sample k0: its sample i must
    // sit, in the preview's world, where the voxels it averages have their mean
    // position, for either kind of preview.
    for (const Sampling preview : { Sampling::blockMean(8), Sampling::strided(8), Sampling::blockMean(3) }) {
        for (int level = 0; level < 4; ++level) {
            const Dims k0{ -1, 5, 12 };
            const Placement p = placeBlock(k0, Sampling::pyramidLevel(level), preview);
            QCOMPARE(p.spacing, double(levelScale(level)) / preview.scale);
            for (int i = 0; i < 4; ++i) {
                for (int axis = 0; axis < 3; ++axis) {
                    const std::int64_t first = (k0[axis] + i) * levelScale(level);
                    double sum = 0.0;
                    for (std::int64_t v = first; v < first + levelScale(level); ++v)
                        sum += double(v);
                    const double mean = sum / double(levelScale(level));
                    QVERIFY(qAbs(p.origin[axis] + i * p.spacing - toWorld(mean, preview)) < 1e-12);
                }
            }
        }
        // The preview placed in its own frame is the frame: origin 0, spacing 1.
        const Placement self = placeBlock({ 0, 0, 0 }, preview, preview);
        QCOMPARE(self.origin[0], 0.0);
        QCOMPARE(self.spacing, 1.0);
    }
    // A strided level read (no pyramid) puts sample k on cube voxel k*2^l.
    const Placement p = placeBlock({ 2, 0, 0 }, Sampling::stridedLevel(2), Sampling::strided(8));
    QCOMPARE(p.origin[0], 1.0);   // voxel 8, preview sample 1
    QCOMPARE(p.spacing, 0.5);
}

void TestCubeLodGeometry::nodes_gridBoxesAndSnapping()
{
    const Dims cube{ 1000, 700, 300 };
    // Level 2 is 250 x 175 x 75; nodes of 64 level voxels (256 cube voxels).
    QCOMPARE(nodeGrid(cube, 2, 64), (Dims{ 4, 3, 2 }));
    const Node last{ 2, 3, 2, 1 };
    QCOMPARE(nodeLevelBox(cube, 64, last), (Box{ { 192, 128, 64 }, { 250, 175, 75 } }));
    QCOMPARE(nodeCubeBox(cube, 64, last), (Box{ { 768, 512, 256 }, { 1000, 700, 300 } }));
    // The nodes of a level tile the cube: every cube voxel in exactly one node.
    std::int64_t covered = 0;
    for (const Node &n : nodesIn(wholeBox(nodeGrid(cube, 2, 64)), 2))
        covered += nodeCubeBox(cube, 64, n).count();
    QCOMPARE(covered, wholeBox(cube).count());

    // Snapping is outward: [300, 301) along x needs node 1 (256..512).
    const Box one{ { 300, 0, 299 }, { 301, 1, 300 } };
    const Box r = nodeRangeCovering(one, cube, 2, 64);
    QCOMPARE(r, (Box{ { 1, 0, 1 }, { 2, 1, 2 } }));
    QCOMPARE(cubeBoxOfRange(r, cube, 2, 64), (Box{ { 256, 0, 256 }, { 512, 256, 300 } }));
    // A box on node boundaries is not grown.
    QCOMPARE(nodeRangeCovering(Box{ { 256, 256, 0 }, { 512, 512, 256 } }, cube, 2, 64),
             (Box{ { 1, 1, 0 }, { 2, 2, 1 } }));
    // A box reaching past the cube is clipped first; one outside it is empty.
    QCOMPARE(nodeRangeCovering(Box{ { -50, -50, -50 }, { 5000, 5000, 5000 } }, cube, 2, 64),
             wholeBox(nodeGrid(cube, 2, 64)));
    QVERIFY(nodeRangeCovering(Box{ { 1000, 0, 0 }, { 1200, 10, 10 } }, cube, 2, 64).empty());
    QCOMPARE(nodesIn(Box{ { 0, 0, 0 }, { 2, 1, 1 } }, 3).size(), size_t(2));
    QCOMPARE(nodesIn(Box{ { 0, 0, 0 }, { 2, 1, 1 } }, 3)[1], (Node{ 3, 1, 0, 0 }));
}

void TestCubeLodGeometry::ghosted_clampsTheReadAtTheCube()
{
    // At the cube's low and high border: the block keeps its ghosts (the edge
    // is repeated), the read stays inside the level.
    const Ghosted edge = ghosted(Box{ { 0, 4, 60 }, { 10, 8, 64 } }, { 10, 20, 64 }, 1);
    QCOMPARE(edge.sampleLo, (Dims{ -1, 3, 59 }));
    QCOMPARE(edge.dims, (Dims{ 12, 6, 6 }));
    QCOMPARE(edge.read, (Box{ { 0, 3, 59 }, { 10, 9, 64 } }));
    // Without ghosts the block is the box.
    const Ghosted none = ghosted(Box{ { 2, 2, 2 }, { 5, 6, 7 } }, { 10, 10, 10 }, 0);
    QCOMPARE(none.dims, (Dims{ 3, 4, 5 }));
    QCOMPARE(none.read, (Box{ { 2, 2, 2 }, { 5, 6, 7 } }));
}

void TestCubeLodGeometry::bracketingSamples_isTheGhostedLevelRange()
{
    // For a box on a level's blocks, the samples that bracket the region it
    // owns are the level's own samples plus one ghost per side: what the
    // backend's out-of-core renderer reads (_ooc._load).
    for (int level = 0; level < 5; ++level) {
        const std::int64_t s = levelScale(level);
        const Box box{ { int(3 * s), int(0 * s), int(7 * s) }, { int(9 * s), int(2 * s), int(8 * s) } };
        const Box samples = bracketingSamples(box, Sampling::pyramidLevel(level));
        QCOMPARE(samples, (Box{ { 2, -1, 6 }, { 10, 3, 9 } }));
    }
    // For any box and any sampling it is the tightest bracket.
    std::mt19937 rng(11);
    for (int trial = 0; trial < 500; ++trial) {
        const int stride = 1 + int(rng() % 9);
        const Sampling sampling = (trial % 2) ? Sampling::blockMean(stride) : Sampling::strided(stride);
        const int a = int(rng() % 300);
        const int b = a + 1 + int(rng() % 50);
        const Box box{ { a, a, a }, { b, b, b } };
        const Box r = bracketingSamples(box, sampling);
        QVERIFY(sampling.centre(r.lo[0]) <= a - 0.5);
        QVERIFY(sampling.centre(r.lo[0] + 1) > a - 0.5);
        QVERIFY(sampling.centre(r.hi[0] - 1) >= b - 0.5);
        QVERIFY(sampling.centre(r.hi[0] - 2) < b - 0.5);
    }
}

void TestCubeLodGeometry::cropPlanes_matchTheBackend()
{
    const QJsonArray cases = golden().value(u"crop_planes").toArray();
    QVERIFY2(cases.size() >= 100, "tests/data/cube_lod_golden.json is missing or short");
    for (int k = 0; k < cases.size(); ++k) {
        const QJsonObject o = cases.at(k).toObject();
        const QJsonArray c = o.value(u"crop").toArray();
        const QJsonArray org = o.value(u"origin").toArray();
        const QJsonArray want = o.value(u"planes").toArray();
        Crop crop;
        for (int i = 0; i < 6; ++i)
            crop[size_t(i)] = c.at(i).toDouble();
        const Crop got = vtkCropPlanes(crop, { org.at(0).toDouble(), org.at(1).toDouble(), org.at(2).toDouble() },
                                       o.value(u"spacing").toDouble(), dimsOf(o.value(u"dims")));
        for (int i = 0; i < 6; ++i) {
            // Exact: both are float32 values, rounded the same way.
            if (got[size_t(i)] != want.at(i).toDouble())
                QFAIL(qPrintable(QStringLiteral("case %1 plane %2: %3 != %4")
                                         .arg(k).arg(i)
                                         .arg(got[size_t(i)], 0, 'g', 17)
                                         .arg(want.at(i).toDouble(), 0, 'g', 17)));
        }
    }
    // Every plane is a float32 value, the low one at or above, the high one
    // strictly below the exact inverse.
    const Crop crop{ 2.0e7, 2.0e7 + 64.0, 0.0, 10.0, -3.0, 4.0 };
    const Crop got = vtkCropPlanes(crop, { 2.0e7 - 1.0, -1.0, -4.0 }, 1.0, { 66, 12, 9 });
    for (int i = 0; i < 6; ++i)
        QCOMPARE(double(float(got[size_t(i)])), got[size_t(i)]);
    QVERIFY(got[0] >= cropPlaneFor(crop[0], 2.0e7 - 1.0, 1.0, 66));
    QVERIFY(got[1] < cropPlaneFor(crop[1] - kFaceEps, 2.0e7 - 1.0, 1.0, 66));
}

void TestCubeLodGeometry::cropPlaneFor_drawsTheFaceAskedFor()
{
    // The mapper draws a plane p at d = o - s/2 + (p - o) n / (n - 1).
    for (const double spacing : { 1.0, 0.25, 4.0 })
        for (const double d : { -0.5, 1.5, 4.25, 9.5 }) {
            const double p = cropPlaneFor(d * spacing, 3.0, spacing, 10);
            QVERIFY(qAbs((3.0 - spacing / 2 + (p - 3.0) * 10.0 / 9.0) - d * spacing) < 1e-12);
        }
    QCOMPARE(cropPlaneFor(5.0, 2.0, 1.0, 1), 2.0);   // one sample: the origin
    // The owned region of a box: half a voxel out, clipped to the samples.
    const Crop own = ownedCrop(Box{ { 0, 3, 5 }, { 4, 10, 6 } }, { 4, 20, 6 });
    QCOMPARE(own, (Crop{ 0.0, 3.0, 2.5, 9.5, 4.5, 5.0 }));
    const Crop w = cropToWorld(own, Sampling::blockMean(4));
    QCOMPARE(w[2], (2.5 - 1.5) / 4.0);
}

void TestCubeLodGeometry::kdSplit_coversTheCubeExactlyOnce()
{
    std::mt19937 rng(321);
    for (int trial = 0; trial < 400; ++trial) {
        const Dims cube{ 1 + int(rng() % 17), 1 + int(rng() % 13), 1 + int(rng() % 11) };
        Box inner;
        for (int i = 0; i < 3; ++i) {
            // Boxes touching the border, the whole axis, or reaching past it.
            const int a = int(rng() % (cube[i] + 2)) - 1;
            const int b = a + 1 + int(rng() % (cube[i] + 1));
            inner.lo[i] = a;
            inner.hi[i] = b;
        }
        const std::vector<Piece> pieces = kdSplit(cube, inner);
        QVERIFY(pieces.size() <= 6);
        const Box clipped = intersect(inner, wholeBox(cube));
        if (clipped.empty()) {
            QVERIFY(pieces.empty());
            continue;
        }
        std::vector<int> hits(size_t(wholeBox(cube).count()), 0);
        auto mark = [&](const Box &b) {
            QVERIFY(!b.empty());
            for (int z = b.lo[2]; z < b.hi[2]; ++z)
                for (int y = b.lo[1]; y < b.hi[1]; ++y)
                    for (int x = b.lo[0]; x < b.hi[0]; ++x) {
                        QVERIFY(x >= 0 && y >= 0 && z >= 0 && x < cube[0] && y < cube[1] && z < cube[2]);
                        ++hits[size_t((z * cube[1] + y) * cube[0] + x)];
                    }
        };
        mark(clipped);
        for (const Piece &p : pieces)
            mark(p.box);
        QVERIFY2(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }),
                 qPrintable(QStringLiteral("trial %1: a voxel is covered twice or not at all").arg(trial)));
    }
    // The whole cube as the box: nothing around it.
    QVERIFY(kdSplit({ 8, 8, 8 }, wholeBox({ 8, 8, 8 })).empty());
}

void TestCubeLodGeometry::kdOrder_isBackToFrontFromEveryOctant()
{
    const Dims cube{ 40, 30, 20 };
    for (const Box &inner : { Box{ { 10, 8, 5 }, { 25, 20, 12 } }, Box{ { 0, 8, 5 }, { 25, 30, 12 } },
                              Box{ { 39, 0, 0 }, { 40, 1, 20 } } }) {
        const std::vector<Piece> pieces = kdSplit(cube, inner);
        // Every combination of below / inside / above the box on each axis
        // (the 26 octants, edges and faces around it, and the box itself),
        // near the box and far outside the cube.
        for (const double far : { 1.0, 0.0 }) {
            for (int code = 0; code < 27; ++code) {
                std::array<double, 3> eye;
                for (int i = 0, c = code; i < 3; ++i, c /= 3) {
                    const int where = c % 3;
                    const double reach = far ? 2.0 * cube[i] : 3.3;
                    eye[size_t(i)] = where == 0 ? inner.lo[i] - 0.5 - reach
                                   : where == 1 ? (inner.lo[i] + inner.hi[i]) / 2.0 - 0.13
                                                : inner.hi[i] - 0.5 + reach;
                }
                const std::vector<Side> order = kdBackToFront(inner, eye, pieces);
                QCOMPARE(order.size(), pieces.size() + 1);
                std::vector<Box> drawn;
                for (Side s : order)
                    drawn.push_back(pieceBox(s, inner, pieces));
                QString why;
                QVERIFY2(backToFrontAlongRays(drawn, eye, 400, &why),
                         qPrintable(QStringLiteral("eye code %1 far %2: %3").arg(code).arg(far).arg(why)));
            }
        }
    }
    // The check itself catches a wrong order: front to back, from outside.
    const Box inner{ { 10, 8, 5 }, { 25, 20, 12 } };
    const std::vector<Piece> pieces = kdSplit(cube, inner);
    const std::array<double, 3> eye{ -20.3, -15.1, -9.7 };
    std::vector<Side> order = kdBackToFront(inner, eye, pieces);
    std::reverse(order.begin(), order.end());
    std::vector<Box> drawn;
    for (Side s : order)
        drawn.push_back(pieceBox(s, inner, pieces));
    QString why;
    QVERIFY(!backToFrontAlongRays(drawn, eye, 400, &why));
}

void TestCubeLodGeometry::nodeOrder_isBackToFrontInsideTheBox()
{
    const Dims cube{ 1000, 700, 300 };
    const int nodeSize = 64;
    const int level = 1;   // nodes of 128 cube voxels
    const Box range{ { 1, 0, 0 }, { 6, 4, 3 } };
    const std::vector<Node> nodes = nodesIn(range, level);
    const Box box = cubeBoxOfRange(range, cube, level, nodeSize);
    std::mt19937 rng(5);
    for (int trial = 0; trial < 60; ++trial) {
        // Eyes all around and inside the box (and inside nodes).
        std::array<double, 3> eye;
        for (int i = 0; i < 3; ++i) {
            std::uniform_real_distribution<double> u(box.lo[i] - 400.0, box.hi[i] + 400.0);
            eye[size_t(i)] = u(rng);
        }
        for (const int top : { level, 5 }) {
            const std::vector<Node> order = backToFront(nodes, eye, nodeSize, top);
            QCOMPARE(order.size(), nodes.size());
            std::vector<Box> drawn;
            for (const Node &n : order)
                drawn.push_back(nodeCubeBox(cube, nodeSize, n));
            QString why;
            QVERIFY2(backToFrontAlongRays(drawn, eye, 300, &why), qPrintable(why));
        }
    }
    // The check itself catches a wrong order: from beyond the box's high
    // corner, the nodes in reverse index order are drawn nearest first.
    const std::array<double, 3> eye{ box.hi[0] + 300.3, box.hi[1] + 200.7, box.hi[2] + 100.1 };
    std::vector<Box> drawn;
    for (const Node &n : nodes)
        drawn.insert(drawn.begin(), nodeCubeBox(cube, nodeSize, n));
    QString why;
    QVERIFY(!backToFrontAlongRays(drawn, eye, 300, &why));
}

void TestCubeLodGeometry::nodeOrder_mixedLevelsFollowTheOctree()
{
    // A hierarchical partition, as a graded octree gives: the eight level-2
    // nodes of a 256^3 cube, one split into level-1 nodes, one of those into
    // level-0 nodes.
    const Dims cube{ 256, 256, 256 };
    const int nodeSize = 32;
    std::vector<Node> nodes;
    for (const Node &n : nodesIn(wholeBox(nodeGrid(cube, 2, nodeSize)), 2)) {
        if (!(n == Node{ 2, 0, 1, 0 })) {
            nodes.push_back(n);
            continue;
        }
        for (const Node &c : nodesIn(Box{ { 0, 2, 0 }, { 2, 4, 2 } }, 1)) {
            if (!(c == Node{ 1, 1, 2, 1 })) {
                nodes.push_back(c);
                continue;
            }
            for (const Node &g : nodesIn(Box{ { 2, 4, 2 }, { 4, 6, 4 } }, 0))
                nodes.push_back(g);
        }
    }
    std::int64_t covered = 0;
    for (const Node &n : nodes)
        covered += nodeCubeBox(cube, nodeSize, n).count();
    QCOMPARE(covered, wholeBox(cube).count());
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> u(-200.0, 456.0);
    for (int trial = 0; trial < 40; ++trial) {
        const std::array<double, 3> eye{ u(rng), u(rng), u(rng) };
        const std::vector<Node> order = backToFront(nodes, eye, nodeSize, 2);
        std::vector<Box> drawn;
        for (const Node &n : order)
            drawn.push_back(nodeCubeBox(cube, nodeSize, n));
        QString why;
        QVERIFY2(backToFrontAlongRays(drawn, eye, 300, &why), qPrintable(why));
    }
}
