#pragma once
// TestCubeToolGate.h
// QTest class for CubeToolGate — the cube viewer's tool-availability table.
// Header-only pure logic; no GUI/VTK link.

#include <QObject>

class TestCubeToolGate : public QObject
{
    Q_OBJECT

private slots:
    void slice2dTools_followTheSliceViewBeingOnScreen();
    void view3dTools_followThe3dViewBeingOnScreen();
    void families_areIndependentNotExclusive();
    void slice2dTools_alsoOnAMomentPane();
    void noViews_disablesBothFamilies();

    void pinSpectrum_followsTheProbedSpectrumOnly();
    void exportRegion_followsRegionExistence();
    void sendSlice_followsAnOpenImageViewer();
    void linkViews_followsLinkTargets();
    void stateGates_ignoreTheLayout();
};
