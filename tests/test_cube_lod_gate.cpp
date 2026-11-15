// test_cube_lod_gate.cpp
// Unit tests for CubeLodGate, the decision table behind "may the 3-D view add
// finer detail over its preview" (issue #321). Every condition that fails
// turns the level of detail off with a reason of its own, so the status line
// can say why the view stays coarse.

#include "TestCubeLodGate.h"
#include "CubeLodGate.h"

#include <QTest>

using namespace CubeLodGate;

namespace {
/// A remote cube's preview, rendered locally in Composite: the LOD applies.
Inputs allowed()
{
    Inputs in;
    in.settingEnabled = true;
    in.remoteDataset = true;
    in.showingPreview = true;
    in.previewStride = 8;
    in.localRender = true;
    in.compositeBlend = true;
    in.isosurfaceShown = false;
    in.localVrActive = false;
    return in;
}
}   // namespace

void TestCubeLodGate::active_whenEveryConditionHolds()
{
    const Decision d = evaluate(allowed());
    QVERIFY(d.active);
    QVERIFY(d.reason.isEmpty());
}

void TestCubeLodGate::eachCondition_turnsItOffWithItsReason()
{
    struct Case
    {
        void (*change)(Inputs &);
        const char *reasonPart;
    };
    const Case cases[] = {
        { [](Inputs &in) { in.settingEnabled = false; }, "off in Settings" },
        { [](Inputs &in) { in.remoteDataset = false; }, "through the backend" },
        { [](Inputs &in) { in.showingPreview = false; }, "Full-resolution" },
        { [](Inputs &in) { in.previewStride = 1; }, "every voxel" },
        { [](Inputs &in) { in.previewStride = 0; }, "every voxel" },
        { [](Inputs &in) { in.localRender = false; }, "backend renders" },
        { [](Inputs &in) { in.compositeBlend = false; }, "MIP and MinIP" },
        { [](Inputs &in) { in.isosurfaceShown = true; }, "isosurface" },
        { [](Inputs &in) { in.localVrActive = true; }, "headset" },
    };
    QStringList seen;
    for (const Case &c : cases) {
        Inputs in = allowed();
        c.change(in);
        const Decision d = evaluate(in);
        QVERIFY(!d.active);
        QVERIFY2(d.reason.contains(QLatin1String(c.reasonPart)), qPrintable(d.reason));
        if (!seen.contains(d.reason))
            seen << d.reason;
    }
    // One reason per condition (the two strides share theirs).
    QCOMPARE(seen.size(), 8);
}

void TestCubeLodGate::settingOff_isTheReasonWhateverElse()
{
    Inputs in;   // everything wrong
    in.settingEnabled = false;
    QVERIFY(evaluate(in).reason.contains(u"off in Settings"));
    // With the setting on, the data comes before the view.
    in.settingEnabled = true;
    QVERIFY(evaluate(in).reason.contains(u"through the backend"));
}
