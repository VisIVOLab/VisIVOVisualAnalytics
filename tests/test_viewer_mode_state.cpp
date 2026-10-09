// test_viewer_mode_state.cpp
// See TestViewerModeState.h.

#include "TestViewerModeState.h"

#include "ViewerModeState.h"

#include <QTest>

using visivo::ViewerModeKind;
using visivo::viewerModeFor;

void TestViewerModeState::nothingArmedIsPan()
{
    QCOMPARE(viewerModeFor(false, false, false, false), ViewerModeKind::Pan);
}

void TestViewerModeState::eachModeIsReportedOnItsOwn()
{
    QCOMPARE(viewerModeFor(true, false, false, false), ViewerModeKind::Region);
    QCOMPARE(viewerModeFor(false, true, false, false), ViewerModeKind::Ruler);
    QCOMPARE(viewerModeFor(false, false, true, false), ViewerModeKind::Annotate);
    QCOMPARE(viewerModeFor(false, false, false, true), ViewerModeKind::Probe);
}

void TestViewerModeState::theModeHoldingTheMouseWins()
{
    // The four flags are cleared by four different code paths, reachable from
    // the menu, the palette and each other's cleanup. If two ever say "on", the
    // strip must name the one that will consume the next click — showing the
    // other is worse than showing nothing, because the user acts on it.
    //
    // The order is toggleProbeFreeze()'s own dispatch order, which is where a
    // click really goes: annotation placement, then measurement, then region.
    QCOMPARE(viewerModeFor(true, true, false, false), ViewerModeKind::Ruler);
    QCOMPARE(viewerModeFor(true, false, true, false), ViewerModeKind::Annotate);
    QCOMPARE(viewerModeFor(false, true, true, false), ViewerModeKind::Annotate);
    QCOMPARE(viewerModeFor(true, true, true, true), ViewerModeKind::Annotate);
}

void TestViewerModeState::probeYieldsToEveryTool()
{
    // Probe is passive: it follows the cursor and consumes no click, so any
    // armed tool is the mode the user is in.
    QCOMPARE(viewerModeFor(true, false, false, true), ViewerModeKind::Region);
    QCOMPARE(viewerModeFor(false, true, false, true), ViewerModeKind::Ruler);
    QCOMPARE(viewerModeFor(false, false, true, true), ViewerModeKind::Annotate);
}
