#pragma once
// TestCubeLodGate.h
// QTest class for CubeLodGate: when the cube viewer may add finer detail over
// the 3-D preview (issue #321). Header-only pure logic; no GUI/VTK link.

#include <QObject>

class TestCubeLodGate : public QObject
{
    Q_OBJECT

private slots:
    void active_whenEveryConditionHolds();
    void eachCondition_turnsItOffWithItsReason();
    void settingOff_isTheReasonWhateverElse();
};
