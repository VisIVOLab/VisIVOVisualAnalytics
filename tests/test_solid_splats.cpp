#include "TestSolidSplats.h"

#include "gui/SolidSplats.h"

#include <QtTest/QtTest>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {

// The calls SolidSplats makes on vtkDoubleArray and vtkLookupTable.
struct FakeArray {
    std::vector<double> v;
    int modified = 0;
    long long GetNumberOfTuples() const { return static_cast<long long>(v.size()); }
    void SetValue(long long i, double x) { v[static_cast<std::size_t>(i)] = x; }
    void Modified() { ++modified; }
};

struct FakeLut {
    std::vector<std::array<double, 4>> table;
    std::array<double, 2> range{ 0.0, 0.0 };
    std::array<double, 4> nan{ 0.5, 0.0, 0.0, 1.0 };   // VTK's default: dark red
    void SetNumberOfTableValues(int n) { table.assign(static_cast<std::size_t>(n), { 0, 0, 0, 0 }); }
    void SetTableRange(double a, double b) { range = { a, b }; }
    void SetTableValue(int i, double r, double g, double b, double a)
    {
        table[static_cast<std::size_t>(i)] = { r, g, b, a };
    }
    void SetNanColor(double r, double g, double b, double a) { nan = { r, g, b, a }; }
};

} // namespace

void TestSolidSplats::staleNaNScalarsAreClearedWhenGoingSolid()
{
    // A column with NaN coloured the splats; then "none": the scalars the
    // mapper keeps must not hold NaN (undefined opacity, NaN colour).
    FakeArray scalars;
    scalars.v = { 1.0, std::numeric_limits<double>::quiet_NaN(), 3.5, -2.0,
                  std::numeric_limits<double>::infinity() };
    SolidSplats::clearScalars(&scalars);
    for (double x : scalars.v)
        QCOMPARE(x, 0.0);
    QCOMPARE(scalars.modified, 1);
}

void TestSolidSplats::theTableMapsEverythingNaNIncludedToTheSolidColour()
{
    FakeLut lut;
    SolidSplats::configureTable(&lut);
    QCOMPARE(int(lut.table.size()), 2);
    for (const auto &c : lut.table) {
        QCOMPARE(c[0], SolidSplats::kRed);
        QCOMPARE(c[1], SolidSplats::kGreen);
        QCOMPARE(c[2], SolidSplats::kBlue);
        QCOMPARE(c[3], 1.0);
    }
    QCOMPARE(lut.nan[0], SolidSplats::kRed);
    QCOMPARE(lut.nan[1], SolidSplats::kGreen);
    QCOMPARE(lut.nan[2], SolidSplats::kBlue);
    QVERIFY(lut.range[0] <= -1.0e15 && lut.range[1] >= 1.0e15);
}
