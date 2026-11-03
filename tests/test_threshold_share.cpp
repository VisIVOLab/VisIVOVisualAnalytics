#include "TestThresholdShare.h"

#include "gui/ThresholdShare.h"

#include <QtTest>

#include <limits>
#include <vector>

void TestThresholdShare::theShareCountsOnlyRealVoxels()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Masked voxels carry the sentinel (-1) or are NaN: neither counts.
    const std::vector<float> v{ -1.f, nan, 0.f, 1.f, 2.f, 3.f };
    QCOMPARE(*visivo::shareAbove(v.data(), v.size(), 1.5, -1.0), 0.5);
    QCOMPARE(*visivo::shareAbove(v.data(), v.size(), 5.0, -1.0), 0.0);
    const std::vector<float> masked{ -1.f, nan };
    QVERIFY(!visivo::shareAbove(masked.data(), masked.size(), 0.0, -1.0));
    QVERIFY(!visivo::shareAbove(nullptr, 0, 0.0, -1.0));
}

void TestThresholdShare::aLargeCubeIsSampled()
{
    std::vector<float> v(1000);
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<float>(i % 100);   // 1 % of the values at each level
    const auto share = visivo::shareAbove(v.data(), v.size(), 89.5, -1.0, 100);
    QVERIFY(share);
    QVERIFY(std::abs(*share - 0.10) < 0.03);
}

void TestThresholdShare::theTextShowsSmallShares()
{
    QCOMPARE(visivo::shareAboveText(0.0), QStringLiteral("0 % of voxels above"));
    QCOMPARE(visivo::shareAboveText(0.0, true), QStringLiteral("< 0.01 % of voxels above"));
    QCOMPARE(visivo::shareAboveText(0.9996), QStringLiteral("> 99.9 % of voxels above"));
    QCOMPARE(visivo::shareAboveText(0.00005), QStringLiteral("< 0.01 % of voxels above"));
    QCOMPARE(visivo::shareAboveText(0.00042), QStringLiteral("0.042 % of voxels above"));
    QCOMPARE(visivo::shareAboveText(0.0042), QStringLiteral("0.42 % of voxels above"));
    QCOMPARE(visivo::shareAboveText(0.01), QStringLiteral("1.0 % of voxels above"));
    QCOMPARE(visivo::shareAboveText(0.35), QStringLiteral("35 % of voxels above"));
}
