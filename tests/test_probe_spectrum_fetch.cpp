#include "TestProbeSpectrumFetch.h"

#include "gui/ProbeSpectrumFetch.h"
#include "CubeLodGeometry.h"

#include <QtTest>

using visivo::ProbeFetchGate;
using visivo::ProbePixel;

void TestProbeSpectrumFetch::aStillCursorIsFetchedOnce()
{
    ProbeFetchGate gate;
    const auto start = gate.request({ 10, 20 });
    QVERIFY(start.has_value());
    QCOMPARE(start->pixel, (ProbePixel{ 10, 20 }));
    QVERIFY(gate.inFlight());
    const auto done = gate.finished(*start, ProbePixel{ 10, 20 });
    QVERIFY(done.apply);
    QVERIFY(!done.next.has_value());
    QVERIFY(!gate.inFlight());
}

void TestProbeSpectrumFetch::movesWhileInFlightCollapseToTheLatest()
{
    ProbeFetchGate gate;
    const auto first = gate.request({ 1, 1 });
    QVERIFY(first.has_value());
    // The cursor crosses three pixels before the first reply: none is fetched now.
    QVERIFY(!gate.request({ 2, 1 }).has_value());
    QVERIFY(!gate.request({ 3, 1 }).has_value());
    QVERIFY(!gate.request({ 4, 1 }).has_value());
    const auto done = gate.finished(*first, ProbePixel{ 4, 1 });
    QVERIFY(!done.apply);   // the probe is no longer on (1, 1)
    QVERIFY(done.next.has_value());
    QCOMPARE(done.next->pixel, (ProbePixel{ 4, 1 }));   // only the latest
    QVERIFY(gate.inFlight());
    const auto last = gate.finished(*done.next, ProbePixel{ 4, 1 });
    QVERIFY(last.apply);
    QVERIFY(!last.next.has_value());
}

void TestProbeSpectrumFetch::aReplyForAPixelLeftBehindIsNotShown()
{
    ProbeFetchGate gate;
    const auto start = gate.request({ 5, 5 });
    // The probe moved, but to a pixel the window reads on its own (no request).
    const auto done = gate.finished(*start, ProbePixel{ 6, 6 });
    QVERIFY(!done.apply);
    QVERIFY(!done.next.has_value());
    QVERIFY(!gate.inFlight());
}

void TestProbeSpectrumFetch::backToTheSamePixelIsNotFetchedAgain()
{
    ProbeFetchGate gate;
    const auto start = gate.request({ 7, 7 });
    QVERIFY(!gate.request({ 8, 7 }).has_value());
    QVERIFY(!gate.request({ 7, 7 }).has_value());   // and back
    const auto done = gate.finished(*start, ProbePixel{ 7, 7 });
    QVERIFY(done.apply);
    QVERIFY(!done.next.has_value());
}

void TestProbeSpectrumFetch::aResetDropsTheReplyInFlight()
{
    ProbeFetchGate gate;
    const auto start = gate.request({ 1, 2 });
    gate.reset();   // e.g. the full cube arrived and the curve was read locally
    // Still one in flight: a new pixel waits for it rather than doubling up.
    QVERIFY(!gate.request({ 3, 4 }).has_value());
    const auto done = gate.finished(*start, ProbePixel{ 1, 2 });
    QVERIFY(!done.apply);   // asked before the reset
    QVERIFY(done.next.has_value());
    QCOMPARE(done.next->pixel, (ProbePixel{ 3, 4 }));
    QVERIFY(gate.finished(*done.next, ProbePixel{ 3, 4 }).apply);
}

void TestProbeSpectrumFetch::aProbeSwitchedOffShowsNothing()
{
    ProbeFetchGate gate;
    const auto start = gate.request({ 1, 1 });
    const auto done = gate.finished(*start, std::nullopt);
    QVERIFY(!done.apply);
    QVERIFY(!done.next.has_value());
}

void TestProbeSpectrumFetch::previewVoxelsAreStridedCubeVoxels()
{
    // The 3-D pick turns a hit on the preview into a cube voxel with the
    // preview's sampling (vtkWindowCube::displayToCube). A strided preview
    // takes data[::s, ::s, ::s]: a 700-pixel axis at stride 4 has 175 preview
    // voxels, the last being the cube's 696, not 699.
    using CubeLodGeometry::Sampling;
    QCOMPARE(CubeLodGeometry::toCube(0, Sampling::strided(4)), 0.0);
    QCOMPARE(CubeLodGeometry::toCube(1, Sampling::strided(4)), 4.0);
    QCOMPARE(CubeLodGeometry::toCube(174, Sampling::strided(4)), 696.0);
    // A block-mean preview (#320): voxel i is the mean of cube voxels
    // 8i .. 8i + 7, so it stands for their centre, 8i + 3.5.
    QCOMPARE(CubeLodGeometry::toCube(0, Sampling::blockMean(8)), 3.5);
    QCOMPARE(CubeLodGeometry::toCube(10, Sampling::blockMean(8)), 83.5);
    // The pick's marker goes back to where it was drawn.
    for (const Sampling &s : { Sampling::strided(6), Sampling::blockMean(8), Sampling{} })
        QCOMPARE(CubeLodGeometry::toWorld(CubeLodGeometry::toCube(12.25, s), s), 12.25);
    // Stride 1 is the cube itself.
    QCOMPARE(CubeLodGeometry::toCube(123, Sampling{}), 123.0);
}
