// test_cube_lod_assembly.cpp
// Unit tests for CubeLodAssembly, the volumes of a refined 3-D view (issue
// #321): coarse pieces cut from the preview around a box of fine blocks. What
// they guard is what would show on screen: a voxel drawn twice or not at all,
// a block that extrapolates at its face, a block shifted against the
// preview, two neighbours whose faces do not meet, or a far block drawn over
// a near one. Also the status sentences and the pyramid_status parsing.

#include "TestCubeLodAssembly.h"
#include "BackendClient.h"
#include "CubeLodAssembly.h"
#include "CubeLodStatus.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace CubeLodAssembly;
using namespace Qt::StringLiterals;
using CubeLodGeometry::Piece;

namespace {

/// A refined view: the fine nodes of @p range at @p level, and the coarse
/// pieces around them, cut from a preview of @p previewSampling.
std::vector<Block> assemble(const Dims &cube, const Sampling &preview, int level, int nodeSize,
                            const Box &range, const Sampling &fine)
{
    const Box fineBox = CubeLodGeometry::cubeBoxOfRange(range, cube, level, nodeSize);
    std::vector<Block> blocks = coarsePieces(cube, preview, fineBox);
    for (const Node &n : CubeLodGeometry::nodesIn(range, level)) {
        Block b = fineBlock(cube, nodeSize, n, 1);
        b.sampling = fine;
        blocks.push_back(b);
    }
    return blocks;
}

struct Scene
{
    Dims cube;
    Sampling preview;
    int level;
    int nodeSize;
    Box range;
    Sampling fine;
};

std::vector<Scene> scenes()
{
    return {
        // 2048^3 PLUTO from a pyramid: preview level 3, detail level 0.
        { { 2048, 2048, 2048 }, Sampling::blockMean(8), 0, 256, { { 2, 3, 4 }, { 4, 4, 6 } },
          Sampling::pyramidLevel(0) },
        // Odd shape, strided preview with a stride that is not a power of
        // two, strided level 1, a box touching the cube's high corner.
        { { 701, 333, 158 }, Sampling::strided(6), 1, 128, { { 2, 1, 0 }, { 3, 2, 1 } },
          Sampling::stridedLevel(1) },
        // Pyramid level 1 under a level-3 preview, box at the low corner.
        { { 1000, 900, 520 }, Sampling::blockMean(8), 1, 128, { { 0, 0, 0 }, { 2, 2, 2 } },
          Sampling::pyramidLevel(1) },
    };
}

}   // namespace

void TestCubeLodAssembly::blocks_ownTheCubeExactlyOnce()
{
    for (const Scene &s : scenes()) {
        const std::vector<Block> blocks = assemble(s.cube, s.preview, s.level, s.nodeSize, s.range, s.fine);
        // Every voxel next to a block face, where an off-by-one would show,
        // plus a coarse stride through the rest: a full count of a 2048^3
        // cube would take too long for a unit test.
        auto axis = [&blocks](int n, int a) {
            std::vector<int> v;
            for (int i = 0; i < n; i += std::max(1, n / 37))
                v.push_back(i);
            for (const Block &b : blocks)
                for (int c : { b.owned.lo[a] - 1, b.owned.lo[a], b.owned.hi[a] - 1, b.owned.hi[a] })
                    if (c >= 0 && c < n)
                        v.push_back(c);
            v.push_back(n - 1);
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            return v;
        };
        int hits = 0, misses = 0, doubles = 0;
        for (int z : axis(s.cube[2], 2))
            for (int y : axis(s.cube[1], 1))
                for (int x : axis(s.cube[0], 0)) {
                    int owners = 0;
                    for (const Block &b : blocks)
                        owners += (x >= b.owned.lo[0] && x < b.owned.hi[0] && y >= b.owned.lo[1]
                                   && y < b.owned.hi[1] && z >= b.owned.lo[2] && z < b.owned.hi[2]);
                    hits += owners == 1;
                    misses += owners == 0;
                    doubles += owners > 1;
                }
        QCOMPARE(misses, 0);
        QCOMPARE(doubles, 0);
        QVERIFY(hits > 0);
    }
}

void TestCubeLodAssembly::blocks_reachTheFacesTheyDraw()
{
    for (const Scene &s : scenes()) {
        for (const Block &b : assemble(s.cube, s.preview, s.level, s.nodeSize, s.range, s.fine)) {
            QVERIFY2(covers(b), qPrintable(u"block %1 does not bracket its region"_s.arg(int(b.side))));
            // A coarse piece is cut from the preview: its samples are the
            // preview's indices, at most one ghost outside the preview.
            if (!b.fine) {
                for (int i = 0; i < 3; ++i) {
                    QVERIFY(b.samples.lo[i] >= -1);
                    const int previewN = int(std::ceil(double(s.cube[i]) / s.preview.scale));
                    QVERIFY(b.samples.hi[i] <= previewN + 1);
                }
            }
        }
    }
}

void TestCubeLodAssembly::cutSamples_repeatsTheEdge()
{
    // A 3 x 2 x 2 grid holding its own linear index.
    std::vector<float> grid(12);
    for (int i = 0; i < 12; ++i)
        grid[size_t(i)] = float(i);
    const Dims shape{ 3, 2, 2 };
    const std::vector<float> out = cutSamples(grid.data(), shape, Box{ { -1, 1, 1 }, { 4, 3, 2 } });
    QCOMPARE(out.size(), size_t(5 * 2 * 1));
    // z = 1, y = 1 then y = 2 (repeats y = 1); x = -1 repeats x = 0, x = 3 repeats x = 2.
    const std::vector<float> expected{ 9, 9, 10, 11, 11, 9, 9, 10, 11, 11 };
    QCOMPARE(out, expected);
    QVERIFY(cutSamples(nullptr, shape, Box{ { 0, 0, 0 }, { 1, 1, 1 } }).empty());
}

void TestCubeLodAssembly::placement_putsSamplesWhereThePreviewShowsThem()
{
    const Dims cube{ 2048, 2048, 2048 };
    const Sampling preview = Sampling::blockMean(8);
    // Fine level 0 block whose sample 0 (ghost) is cube voxel 511.
    Block fine = fineBlock(cube, 256, Node{ 0, 2, 0, 0 }, 1);
    fine.sampling = Sampling::pyramidLevel(0);
    const Geometry g = geometryOf(fine, cube, preview);
    QCOMPARE(g.dims, (Dims{ 258, 258, 258 }));
    QCOMPARE(g.placement.spacing, 1.0 / 8.0);
    // Its sample k is cube voxel 511 + k; the preview shows cube voxel c at
    // world (c - 3.5) / 8.
    QCOMPARE(g.placement.origin[0], (511.0 - 3.5) / 8.0);
    QCOMPARE(g.placement.origin[1], (-1.0 - 3.5) / 8.0);
    // Level 1 block means: sample k stands for cube voxel 2k + 0.5.
    Block l1 = fineBlock(cube, 128, Node{ 1, 1, 0, 0 }, 1);
    l1.sampling = Sampling::pyramidLevel(1);
    const Geometry g1 = geometryOf(l1, cube, preview);
    QCOMPARE(g1.placement.spacing, 2.0 / 8.0);
    QCOMPARE(g1.placement.origin[0], (2.0 * 127 + 0.5 - 3.5) / 8.0);
    // A coarse piece sits on the preview's own samples: integer origin, spacing 1.
    for (const Block &b : coarsePieces(cube, preview, fine.owned)) {
        const Geometry gc = geometryOf(b, cube, preview);
        QCOMPARE(gc.placement.spacing, 1.0);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(gc.placement.origin[i], double(b.samples.lo[i]));
    }
}

void TestCubeLodAssembly::neighbours_meetOnOneWorldFace()
{
    for (const Scene &s : scenes()) {
        const std::vector<Block> blocks = assemble(s.cube, s.preview, s.level, s.nodeSize, s.range, s.fine);
        for (size_t i = 0; i < blocks.size(); ++i) {
            for (size_t j = 0; j < blocks.size(); ++j) {
                for (int a = 0; a < 3; ++a) {
                    if (blocks[i].owned.hi[a] != blocks[j].owned.lo[a])
                        continue;
                    // i's high face and j's low face are the same cube plane:
                    // they must be the same world plane, whatever the samplings.
                    const Geometry gi = geometryOf(blocks[i], s.cube, s.preview);
                    const Geometry gj = geometryOf(blocks[j], s.cube, s.preview);
                    QCOMPARE(gi.drawn[2 * a + 1], gj.drawn[2 * a]);
                    // And every block draws within its own samples' span.
                    for (const Geometry &g : { gi, gj }) {
                        const double lo = g.placement.origin[a];
                        const double hi = lo + (g.dims[a] - 1) * g.placement.spacing;
                        QVERIFY(g.drawn[2 * a] >= lo - 1e-9);
                        QVERIFY(g.drawn[2 * a + 1] <= hi + 1e-9);
                    }
                }
            }
        }
    }
}

void TestCubeLodAssembly::userCrop_hidesBlocksOutsideIt()
{
    const Dims cube{ 512, 512, 512 };
    const Sampling preview = Sampling::strided(4);
    const Box fineBox{ { 128, 128, 128 }, { 256, 256, 256 } };
    const std::vector<Block> pieces = coarsePieces(cube, preview, fineBox);
    // A clip box inside the fine box: every coarse piece is outside it.
    const Crop clip = CubeLodGeometry::cropToWorld({ 140, 200, 140, 200, 140, 200 }, preview);
    for (const Block &b : pieces)
        QVERIFY(!clipped(geometryOf(b, cube, preview), clip).has_value());
    // Without a clip a piece draws its whole region.
    const Geometry g = geometryOf(pieces.front(), cube, preview);
    QCOMPARE(*clipped(g, std::nullopt), g.drawn);
}

void TestCubeLodAssembly::drawOrder_isBackToFrontForEveryEye()
{
    for (const Scene &s : scenes()) {
        const std::vector<Block> blocks = assemble(s.cube, s.preview, s.level, s.nodeSize, s.range, s.fine);
        const Box fineBox = CubeLodGeometry::cubeBoxOfRange(s.range, s.cube, s.level, s.nodeSize);
        // Eyes in every octant around the box, inside it, and far away.
        std::vector<std::array<double, 3>> eyes;
        for (double fx : { -0.5, 0.2, 0.5, 0.8, 1.5 })
            for (double fy : { -0.5, 0.5, 1.5 })
                for (double fz : { -2.0, 0.3, 3.0 })
                    eyes.push_back({ fx * s.cube[0], fy * s.cube[1], fz * s.cube[2] });
        for (const auto &eye : eyes) {
            const std::vector<int> order = drawOrder(blocks, fineBox, eye, s.nodeSize);
            QCOMPARE(order.size(), blocks.size());
            std::vector<int> position(blocks.size(), -1);
            for (size_t k = 0; k < order.size(); ++k) {
                QVERIFY(position[size_t(order[k])] < 0);   // a permutation
                position[size_t(order[k])] = int(k);
            }
            // Block A may hide B only if a plane between them has the eye on
            // A's side. When A can hide B and B cannot hide A, B goes first.
            auto canHide = [&eye](const Block &a, const Block &b) {
                for (int i = 0; i < 3; ++i) {
                    if (a.owned.hi[i] <= b.owned.lo[i] && eye[i] < b.owned.lo[i] - 0.5)
                        return true;
                    if (b.owned.hi[i] <= a.owned.lo[i] && eye[i] > a.owned.lo[i] - 0.5)
                        return true;
                }
                return false;
            };
            for (size_t a = 0; a < blocks.size(); ++a)
                for (size_t b = 0; b < blocks.size(); ++b)
                    if (a != b && canHide(blocks[a], blocks[b]) && !canHide(blocks[b], blocks[a]))
                        QVERIFY2(position[b] < position[a], "a far block is drawn over a near one");
        }
    }
}

void TestCubeLodAssembly::statusText_namesThePreviewAndTheDetail()
{
    using namespace CubeLodStatus;
    QCOMPARE(ordinal(1), u"1st"_s);
    QCOMPARE(ordinal(2), u"2nd"_s);
    QCOMPARE(ordinal(3), u"3rd"_s);
    QCOMPARE(ordinal(8), u"8th"_s);
    QCOMPARE(ordinal(11), u"11th"_s);
    QCOMPARE(ordinal(12), u"12th"_s);
    QCOMPARE(ordinal(21), u"21st"_s);
    QCOMPARE(previewText(u"pyramid"_s, 3, 8), u"Preview: block means, pyramid level 3"_s);
    QCOMPARE(previewText(u"strided"_s, -1, 8), u"Preview: every 8th voxel"_s);
    // An older backend says nothing: a strided preview.
    QCOMPARE(previewText(QString(), -1, 6), u"Preview: every 6th voxel"_s);
    QCOMPARE(previewText(u"strided"_s, -1, 1), u"Preview: every voxel"_s);
    const Box box{ { 512, 0, 1024 }, { 768, 256, 1280 } };
    QCOMPARE(detailText(0, u"cube"_s, box, u"pyramid"_s, 3, 8),
             u"Detail: level 0 in x 512..767, y 0..255, z 1024..1279; elsewhere block means, "
             u"pyramid level 3"_s);
    QCOMPARE(detailText(1, u"strided"_s, box, u"strided"_s, -1, 8),
             u"Detail: level 1 (every 2nd voxel) in x 512..767, y 0..255, z 1024..1279; elsewhere "
             u"every 8th voxel"_s);
}

void TestCubeLodAssembly::pyramidStatus_isParsedAsXyz()
{
    const QJsonObject answer{ { u"state"_s, u"ready"_s },
                              { u"progress"_s, 1.0 },
                              { u"levels"_s, 3 },
                              { u"brick"_s, 64 },
                              { u"shapes"_s, QJsonArray{ QJsonArray{ 520, 900, 1000 },
                                                          QJsonArray{ 260, 450, 500 },
                                                          QJsonArray{ 130, 225, 250 } } },
                              { u"bytes_estimate"_s, 4.5e9 } };
    const BackendPyramidStatus s = BackendClient::parsePyramidStatus(answer);
    QVERIFY(s.ready());
    QCOMPARE(s.levels, 3);
    QCOMPARE(s.brick, 64);
    QCOMPARE(s.shapes.size(), 3);
    QCOMPARE(s.shapes[0], (std::array<int, 3>{ 1000, 900, 520 }));
    QCOMPARE(s.shapes[2], (std::array<int, 3>{ 250, 225, 130 }));
    QCOMPARE(s.bytesEstimate, qint64(4500000000LL));
    // No state: not an answer the client can act on.
    QVERIFY(!BackendClient::parsePyramidStatus(QJsonObject{}).valid);
    QVERIFY(!BackendClient::parsePyramidStatus(QJsonObject{ { u"state"_s, u"building"_s } }).ready());
}
