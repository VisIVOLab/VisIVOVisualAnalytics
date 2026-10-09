// test_layer_card_geometry.cpp
// See TestLayerCardGeometry.h.

#include "TestLayerCardGeometry.h"

#include "LayerCardGeometry.h"

#include <QTest>

using visivo::LayerCardMetrics;
using visivo::layerCheckboxRect;
using visivo::layerStripRect;
using visivo::layerVisibilityHitRect;

namespace {
constexpr LayerCardMetrics kMetrics{};
const QRect kRow(0, 0, 260, kMetrics.height);
}   // namespace

void TestLayerCardGeometry::theCheckboxSitsInsideTheRow()
{
    const QRect box = layerCheckboxRect(kRow);
    QVERIFY(kRow.contains(box));
    QCOMPARE(box.width(), kMetrics.boxSide);
    QCOMPARE(box.height(), kMetrics.boxSide);
}

void TestLayerCardGeometry::theClickTargetCoversTheWholeLeftGutter()
{
    // 14 px is a small target for a pointer, so the padding around the box
    // counts as part of it.
    const QRect hit = layerVisibilityHitRect(kRow);
    QVERIFY(hit.contains(layerCheckboxRect(kRow)));
    QCOMPARE(hit.left(), kRow.left());
    QVERIFY(hit.height() > kMetrics.boxSide);
}

void TestLayerCardGeometry::theClickTargetStopsBeforeTheName()
{
    // It must not swallow a click meant to select the layer: the name starts a
    // full gap to the right of the box.
    const QRect hit = layerVisibilityHitRect(kRow);
    const int nameLeft = layerCheckboxRect(kRow).right() + kMetrics.gap;
    QVERIFY(hit.right() < nameLeft);
}

void TestLayerCardGeometry::theStripLeavesRoomForTheDetailText()
{
    const int detail = 60;
    const QRect strip = layerStripRect(kRow, detail);
    QVERIFY(strip.right() + detail <= kRow.right());
    QCOMPARE(strip.height(), kMetrics.stripHeight);
}

void TestLayerCardGeometry::theStripNeverInvertsInANarrowDock()
{
    // Dragged narrow enough, "what is left after the text" goes negative — a
    // QPainter warning on every repaint, and nothing drawn.
    for (int width : { 200, 140, 90, 60, 30, 10 }) {
        const QRect strip = layerStripRect(QRect(0, 0, width, kMetrics.height), 80);
        QVERIFY2(strip.width() >= kMetrics.minimumStripWidth,
                 qPrintable(QStringLiteral("width %1 gave strip %2")
                                    .arg(width).arg(strip.width())));
        QVERIFY(strip.isValid());
    }
}

void TestLayerCardGeometry::theStripSitsBelowTheCheckbox()
{
    const QRect strip = layerStripRect(kRow, 60);
    QVERIFY(strip.top() > layerCheckboxRect(kRow).bottom());
    QVERIFY(strip.bottom() <= kRow.bottom());
}
