#include "TestImageLod.h"

#include "vtk/ImageLod.h"

#include <QTest>

using ImageLod::TileGrid;

// The 227MP mosaic used in manual testing: 15979×14204, 512px tiles → 6 levels,
// 32×28 tiles at level 0 (matches the backend + the app log).
static TileGrid bigGrid()
{
    return TileGrid{ 15979, 14204, 512, 6 };
}

void TestImageLod::chooseLevel_fullResWhenZoomedIn()
{
    // One source pixel (or fewer) per screen pixel → full resolution.
    QCOMPARE(ImageLod::chooseLevel(1000.0, 1000.0, 6), 0);
    QCOMPARE(ImageLod::chooseLevel(500.0, 1000.0, 6), 0);
}

void TestImageLod::chooseLevel_picksByLog2Ratio()
{
    // 4 source px per screen px → level 2; 8 → level 3.
    QCOMPARE(ImageLod::chooseLevel(4000.0, 1000.0, 6), 2);
    QCOMPARE(ImageLod::chooseLevel(8000.0, 1000.0, 6), 3);
}

void TestImageLod::chooseLevel_clampsToNumLevels()
{
    // Fully zoomed out on a huge image → coarsest available level, never beyond.
    QCOMPARE(ImageLod::chooseLevel(1.0e9, 1000.0, 6), 5);
    QCOMPARE(ImageLod::chooseLevel(1.0e9, 1000.0, 1), 0);
}

void TestImageLod::chooseLevel_guardsDegenerateViewport()
{
    QCOMPARE(ImageLod::chooseLevel(4000.0, 0.0, 6), 0);
    QCOMPARE(ImageLod::chooseLevel(0.0, 1000.0, 6), 0);
}

void TestImageLod::tileCounts_matchBackendGrid()
{
    const auto g = bigGrid();
    QCOMPARE(ImageLod::tileCounts(0, g), qMakePair(32, 28));   // 512px tiles
    QCOMPARE(ImageLod::tileCounts(1, g), qMakePair(16, 14));   // 1024px tiles
    QCOMPARE(ImageLod::tileCounts(2, g), qMakePair(8, 7));     // 2048px tiles
}

void TestImageLod::chooseAssemblyLevel_fullResWhenItFits()
{
    // A 2000×2000 = 4MP image easily fits a 64MP budget → full res (level 0).
    QCOMPARE(ImageLod::chooseAssemblyLevel(2000, 2000, 4, 64'000'000), 0);
}

void TestImageLod::chooseAssemblyLevel_capsHugeMosaic()
{
    // 15979×14204 = 227MP > 64MP; level 1 = 7990×7102 = 56.7MP ≤ 64MP.
    QCOMPARE(ImageLod::chooseAssemblyLevel(15979, 14204, 6, 64'000'000), 1);
    // A tiny budget forces the coarsest available level.
    QCOMPARE(ImageLod::chooseAssemblyLevel(15979, 14204, 6, 1), 5);
}

void TestImageLod::chooseAssemblyLevel_clampsAndGuards()
{
    QCOMPARE(ImageLod::chooseAssemblyLevel(15979, 14204, 1, 1), 0);      // numLevels=1
    QCOMPARE(ImageLod::chooseAssemblyLevel(0, 0, 6, 64'000'000), 5);     // degenerate dims
    QCOMPARE(ImageLod::chooseAssemblyLevel(2000, 2000, 4, 0), 3);        // maxPixels<=0 → coarsest
}

void TestImageLod::visibleTiles_singleTile()
{
    const TileGrid g{ 2000, 2000, 512, 4 };
    // A point inside tile (1,1) with no margin → exactly that tile.
    const auto t = ImageLod::visibleTiles(600, 600, 610, 610, 0, g, /*margin=*/0);
    QCOMPARE(t.size(), 1);
    QCOMPARE(t.first(), qMakePair(1, 1));
}

void TestImageLod::visibleTiles_marginExpands()
{
    const TileGrid g{ 2000, 2000, 512, 4 };
    // margin=1 around tile (1,1) → the 3×3 neighbourhood (tx,ty ∈ [0,2]).
    const auto t = ImageLod::visibleTiles(600, 600, 610, 610, 0, g, /*margin=*/1);
    QCOMPARE(t.size(), 9);
    QVERIFY(t.contains(qMakePair(0, 0)));
    QVERIFY(t.contains(qMakePair(2, 2)));
    QVERIFY(!t.contains(qMakePair(3, 3)));  // clamped out of the 4×4 grid
}

void TestImageLod::visibleTiles_cornerOrderTolerant()
{
    const TileGrid g{ 2000, 2000, 512, 4 };
    const auto a = ImageLod::visibleTiles(100, 200, 1500, 1600, 0, g, 0);
    const auto b = ImageLod::visibleTiles(1500, 1600, 100, 200, 0, g, 0);  // swapped
    QCOMPARE(a, b);
}

void TestImageLod::visibleTiles_clampsToImage()
{
    const TileGrid g{ 2000, 2000, 512, 4 };
    // Rectangle extends far past the image → tiles stay within [0,cols-1].
    const auto t = ImageLod::visibleTiles(-500, -500, 999999, 999999, 0, g, 2);
    for (const auto &xy : t) {
        QVERIFY(xy.first >= 0 && xy.first < 4);
        QVERIFY(xy.second >= 0 && xy.second < 4);
    }
    QCOMPARE(t.size(), 16);  // whole 4×4 grid
}

void TestImageLod::visibleTiles_fullImageCoversAll()
{
    const TileGrid g{ 2000, 2000, 512, 4 };
    const auto t = ImageLod::visibleTiles(0, 0, 1999, 1999, 0, g, 0);
    QCOMPARE(t.size(), 16);
}

void TestImageLod::viewportAssembly_fullResSubRegion()
{
    // Zoomed in: an 800px-tall source window shown on a 1000px viewport
    // (< 1 source px per screen px) → full res (level 0). margin 0.
    const auto g = bigGrid();  // 15979×14204, 512px, 6 levels
    const auto va = ImageLod::viewportAssembly(1000, 1000, 1800, 1800, 1000.0, g, /*margin=*/0);
    QVERIFY(va.valid);
    QCOMPARE(va.level, 0);
    QCOMPARE(va.spacing, 1.0);
    QCOMPARE(va.txMin, 1);   // floor(1000/512)
    QCOMPARE(va.txMax, 3);   // floor(1800/512)
    QCOMPARE(va.originX, 512.0);   // txMin * span (span=512 at level 0)
    QCOMPARE(va.originY, 512.0);
    QCOMPARE(va.width, 1536);   // 3 tiles × 512
    QCOMPARE(va.height, 1536);
}

void TestImageLod::viewportAssembly_wholeImageZoomedOut()
{
    // Fully zoomed out (whole 14204px height on a 1000px viewport → ~14×,
    // log2≈3.83 → level 4). Covers every tile; the last tile is CLIPPED to the
    // level grid, not padded (width 999, not 2×512=1024).
    const auto g = bigGrid();
    const auto va = ImageLod::viewportAssembly(0, 0, 15978, 14203, 1000.0, g, /*margin=*/1);
    QVERIFY(va.valid);
    QCOMPARE(va.level, 4);           // step 16, span 8192, tileCounts(4) = 2×2
    QCOMPARE(va.spacing, 16.0);
    QCOMPARE(va.txMin, 0);
    QCOMPARE(va.txMax, 1);
    QCOMPARE(va.originX, 0.0);
    QCOMPARE(va.width, 999);         // ceil(15979/16) clipped from 1024
    QCOMPARE(va.height, 888);        // ceil(14204/16) clipped from 1024
}

void TestImageLod::viewportAssembly_midLevelOriginAndSpacing()
{
    // 4000px source window on a 1000px viewport → 4× → level 2 (step 4, span 2048).
    const auto g = bigGrid();
    const auto va = ImageLod::viewportAssembly(4000, 4000, 8000, 8000, 1000.0, g, /*margin=*/0);
    QVERIFY(va.valid);
    QCOMPARE(va.level, 2);
    QCOMPARE(va.spacing, 4.0);
    QCOMPARE(va.txMin, 1);           // floor(4000/2048)
    QCOMPARE(va.txMax, 3);           // floor(8000/2048)
    QCOMPARE(va.originX, 2048.0);    // txMin * span
    QCOMPARE(va.width, 1536);        // 3 tiles × 512
}

void TestImageLod::viewportAssembly_coordinateMappingIsFullRes()
{
    // The whole point: a decimated voxel v maps to full-res pixel
    // originX + v*spacing, and that must equal the true source pixel of that
    // sample. voxel 0 is the union's first level pixel (tileOriginX) → full-res
    // tileOriginX*step; the last voxel maps within the image bounds.
    const auto g = bigGrid();
    const auto va = ImageLod::viewportAssembly(3000, 5000, 6000, 9000, 1000.0, g, /*margin=*/0);
    QVERIFY(va.valid);
    const double step = va.spacing;
    QCOMPARE(va.originX, static_cast<double>(va.tileOriginX) * step);   // voxel 0 → full-res
    QCOMPARE(va.originY, static_cast<double>(va.tileOriginY) * step);
    // Last voxel stays inside the full-res image.
    const double lastX = va.originX + (va.width - 1) * va.spacing;
    const double lastY = va.originY + (va.height - 1) * va.spacing;
    QVERIFY(lastX <= g.fullWidth - 1);
    QVERIFY(lastY <= g.fullHeight - 1);
}

void TestImageLod::viewportAssembly_degenerateGridInvalid()
{
    const ImageLod::TileGrid bad{ 0, 0, 512, 6 };
    const auto va = ImageLod::viewportAssembly(0, 0, 100, 100, 1000.0, bad, 1);
    QVERIFY(!va.valid);
}

void TestImageLod::viewportAssembly_nonIntersectingInvalid()
{
    // Rectangle entirely off the image (panned away) → invalid, so the caller
    // keeps its current view instead of over-fetching an edge tile.
    const auto g = bigGrid();
    const auto off = ImageLod::viewportAssembly(-5000, -5000, -1000, -1000, 1000.0, g, 1);
    QVERIFY(!off.valid);
    // Partially overlapping (top-left corner clipped in) still assembles.
    const auto partial = ImageLod::viewportAssembly(-1000, -1000, 500, 500, 1000.0, g, 0);
    QVERIFY(partial.valid);
}
