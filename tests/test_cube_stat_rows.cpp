#include "TestCubeStatRows.h"

#include "gui/CubeStatRows.h"

#include <QtTest>

namespace {

QString fmt(double v)
{
    return QString::number(v, 'g', 6) + QStringLiteral(" JY/BEAM");
}

visivo::CubeGridStats preview()
{
    visivo::CubeGridStats g;
    g.range = { -0.0012, 0.0092 };
    return g;
}

}   // namespace

void TestCubeStatRows::thePreviewShowsNoMeanOrRmsUntilTheBackendAnswers()
{
    // HCG 44 as a preview: the strided grid has a range but no mean/RMS of its
    // own. The rows used to read "0 JY/BEAM" while Std was already non-zero.
    const auto t = visivo::cubeStatTexts(std::nullopt, preview(), /*backendFailed=*/false, fmt);
    QCOMPARE(t.min, QStringLiteral("-0.0012 JY/BEAM"));
    QCOMPARE(t.max, QStringLiteral("0.0092 JY/BEAM"));
    QCOMPARE(t.mean, QStringLiteral("-"));
    QCOMPARE(t.rms, QStringLiteral("-"));
    QVERIFY(t.toolTip.contains(QStringLiteral("Computing")));
}

void TestCubeStatRows::theBackendScanWinsOverAnyLoadedGrid()
{
    // The whole cube's numbers, whatever the loaded grid (a preview refresh, a
    // sub-cube) measured.
    const std::array<double, 4> full{ -0.00124432, 0.00925545, 1.13352e-06, 0.000228249 };
    visivo::CubeGridStats roi;
    roi.range = { 0., 1. };
    roi.meanRms = std::array<double, 2>{ 5., 6. };
    roi.wholeCube = false;
    const auto t = visivo::cubeStatTexts(full, roi, /*backendFailed=*/false, fmt);
    QCOMPARE(t.min, QStringLiteral("-0.00124432 JY/BEAM"));
    QCOMPARE(t.max, QStringLiteral("0.00925545 JY/BEAM"));
    QCOMPARE(t.mean, QStringLiteral("1.13352e-06 JY/BEAM"));
    QCOMPARE(t.rms, QStringLiteral("0.000228249 JY/BEAM"));
    QVERIFY(t.toolTip.isEmpty());
}

void TestCubeStatRows::aFullResolutionGridShowsItsOwnNumbers()
{
    // The whole cube loaded at full resolution measured its own mean and RMS:
    // shown until the backend's scan lands, and kept when the scan fails.
    visivo::CubeGridStats whole;
    whole.range = { -1., 2. };
    whole.meanRms = std::array<double, 2>{ 0.5, 0.75 };
    const auto t = visivo::cubeStatTexts(std::nullopt, whole, /*backendFailed=*/true, fmt);
    QCOMPARE(t.mean, QStringLiteral("0.5 JY/BEAM"));
    QCOMPARE(t.rms, QStringLiteral("0.75 JY/BEAM"));
    QVERIFY(t.toolTip.isEmpty());
}

void TestCubeStatRows::aCameraRoiIsNotTheWholeCube()
{
    // A full-resolution load of a camera ROI measured only part of the cube:
    // its numbers are not shown as the whole cube's.
    visivo::CubeGridStats roi;
    roi.range = { -1., 2. };
    roi.meanRms = std::array<double, 2>{ 0.5, 0.75 };
    roi.wholeCube = false;
    const auto t = visivo::cubeStatTexts(std::nullopt, roi, /*backendFailed=*/true, fmt);
    QCOMPARE(t.min, QStringLiteral("-"));
    QCOMPARE(t.mean, QStringLiteral("-"));
    QVERIFY(t.toolTip.contains(QStringLiteral("only part of the cube")));
}

void TestCubeStatRows::aFailedScanSaysWhy()
{
    // Preview: the reason, and how to get Mean and RMS.
    const auto t = visivo::cubeStatTexts(std::nullopt, preview(), /*backendFailed=*/true, fmt);
    QCOMPARE(t.mean, QStringLiteral("-"));
    QVERIFY(t.toolTip.contains(QStringLiteral("could not compute")));
    QVERIFY(t.toolTip.contains(QStringLiteral("Load full resolution")));
    // No grid shown yet (a preview held back while the backend renders): the
    // rows are not left blank.
    const auto none = visivo::cubeStatTexts(std::nullopt, std::nullopt, /*backendFailed=*/true, fmt);
    QCOMPARE(none.min, QStringLiteral("-"));
    QCOMPARE(none.rms, QStringLiteral("-"));
    QVERIFY(none.toolTip.contains(QStringLiteral("could not compute")));
}
