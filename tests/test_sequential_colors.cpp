#include "TestSequentialColors.h"

#include "SequentialColorTables.h"
#include "WindowLevelDrag.h"

#include <QtTest>

#include <cmath>

namespace {

using SequentialColorTables::kEntries;
using SequentialColorTables::kTables;

// Rec. 709 luma of an entry: what the eye reads as "how light".
double lightness(const float *rgb)
{
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
}

} // namespace

void TestSequentialColors::everyMapStartsLightAndEndsDark()
{
    for (const auto &table : kTables) {
        const QString name = QString::fromUtf8(table.name.data(), static_cast<int>(table.name.size()));
        // White, or very near it: the point of a sequential map for paper.
        QVERIFY2(lightness(table.rgb[0]) > 0.95, qPrintable(name));
        QVERIFY2(lightness(table.rgb[kEntries - 1]) < 0.35, qPrintable(name));
        // The hue is the map's: Greys has none, the others do at the dark end.
        const float *end = table.rgb[kEntries - 1];
        if (name != QStringLiteral("Greys"))
            QVERIFY2(std::fabs(end[0] - end[2]) > 0.05f || std::fabs(end[0] - end[1]) > 0.05f,
                     qPrintable(name));
        for (std::size_t i = 0; i < kEntries; ++i)
            for (int ch = 0; ch < 3; ++ch) {
                QVERIFY(table.rgb[i][ch] >= 0.f && table.rgb[i][ch] <= 1.f);
            }
    }
}

void TestSequentialColors::lightnessNeverRisesAlongAMap()
{
    // One direction, so the map reads as "more is darker" and a reversed one as
    // "more is lighter". A small tolerance for the 6-digit rounding of the table.
    for (const auto &table : kTables) {
        const QString name = QString::fromUtf8(table.name.data(), static_cast<int>(table.name.size()));
        for (std::size_t i = 1; i < kEntries; ++i)
            QVERIFY2(lightness(table.rgb[i]) <= lightness(table.rgb[i - 1]) + 2e-3,
                     qPrintable(QStringLiteral("%1 entry %2").arg(name).arg(i)));
    }
}

void TestSequentialColors::aReversedMapReadsTheTableFromTheTop()
{
    for (const auto &table : kTables) {
        float forward[3], reversed[3];
        SequentialColorTables::entry(table, false, 0, forward);
        SequentialColorTables::entry(table, true, kEntries - 1, reversed);
        QCOMPARE(forward[0], reversed[0]);
        QCOMPARE(forward[1], reversed[1]);
        QCOMPARE(forward[2], reversed[2]);
        // The reversed map is dark at its low end and light at its high end.
        float low[3], high[3];
        SequentialColorTables::entry(table, true, 0, low);
        SequentialColorTables::entry(table, true, kEntries - 1, high);
        QVERIFY(lightness(low) < lightness(high));
    }
}

void TestSequentialColors::theSixMapsAreNamedAsTheRegistryNamesThem()
{
    // ColorMaps.cpp adds each name and its "_r" by table position, and the
    // backend hands the same names to matplotlib: the order and spelling are a
    // contract between the two.
    const char *expected[] = { "Purples", "Blues", "Greens", "Reds", "Greys", "Oranges" };
    QCOMPARE(std::size(kTables), std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i)
        QCOMPARE(QString::fromUtf8(kTables[i].name.data(), static_cast<int>(kTables[i].name.size())),
                 QString::fromLatin1(expected[i]));
}

void TestSequentialColors::aRightDragWidensTheWindowAndALeftDragNarrowsIt()
{
    const auto wider = WindowLevelDrag::rangeAfterDrag(0., 10., 100., 0., 400., 300.);
    const auto narrower = WindowLevelDrag::rangeAfterDrag(0., 10., -100., 0., 400., 300.);
    QVERIFY(wider[1] - wider[0] > 10.);
    QVERIFY(narrower[1] - narrower[0] < 10.);
    QVERIFY(narrower[1] - narrower[0] > 0.);
    // The level (centre) does not move on a horizontal drag.
    QCOMPARE(0.5 * (wider[0] + wider[1]), 5.0);
    QCOMPARE(0.5 * (narrower[0] + narrower[1]), 5.0);
}

void TestSequentialColors::aVerticalDragMovesTheLevelAndKeepsTheWindow()
{
    // Up (positive dy in VTK display coordinates) lowers the range, so the image
    // brightens, as the drag of the cube's panes does; the width is unchanged.
    const auto up = WindowLevelDrag::rangeAfterDrag(2., 6., 0., 75., 400., 300.);
    QCOMPARE(up[1] - up[0], 4.0);
    QVERIFY(up[0] < 2.);
    const auto down = WindowLevelDrag::rangeAfterDrag(2., 6., 0., -75., 400., 300.);
    QCOMPARE(down[1] - down[0], 4.0);
    QVERIFY(down[0] > 2.);
}

void TestSequentialColors::aDragBackUndoesADragForward()
{
    // Each drag is measured from where it began, so going out and coming back
    // lands on the range it started from.
    const auto out = WindowLevelDrag::rangeAfterDrag(1., 9., 120., 40., 500., 400.);
    QVERIFY(out[1] > out[0]);
    const auto home = WindowLevelDrag::rangeAfterDrag(1., 9., 0., 0., 500., 400.);
    QCOMPARE(home[0], 1.0);
    QCOMPARE(home[1], 9.0);
}

void TestSequentialColors::anEmptyStartRangeStillGivesAFiniteRange()
{
    for (const auto &start : { std::array<double, 2>{ 3., 3. }, std::array<double, 2>{ 5., 1. },
                               std::array<double, 2>{ NAN, 2. } }) {
        const auto r = WindowLevelDrag::rangeAfterDrag(start[0], start[1], 30., -20., 300., 200.);
        QVERIFY(std::isfinite(r[0]) && std::isfinite(r[1]));
        QVERIFY(r[1] > r[0]);
    }
    // A view of zero size (a window not yet laid out) must not divide by zero.
    const auto r = WindowLevelDrag::rangeAfterDrag(0., 1., 10., 10., 0., 0.);
    QVERIFY(std::isfinite(r[0]) && std::isfinite(r[1]));
}
