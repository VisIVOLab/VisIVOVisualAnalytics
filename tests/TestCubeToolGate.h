#pragma once
// TestCubeToolGate.h
// QTest class for CubeToolGate — the cube viewer's tool-availability table.
// Header-only pure logic; no GUI/VTK link.

#include <QObject>

class TestCubeToolGate : public QObject
{
    Q_OBJECT

private slots:
    void slice2dTools_availableOnTheSlicePane();
    void slice2dTools_gatedOn3dWithReason();
    void slice2dTools_gatedOnMomentWithItsOwnReason();
    void slice2dTools_gatedOnSpectrumAndPvPanes();
    void slice2dTools_availableOnOtherProductAndNoPane();

    void extractSpectrum_staysArmedOnItsOwnLivePreview();

    void view3dTools_onlyOnThe3dPane();
    void view3dTools_mirrorTheSliceGate();

    void pinSpectrum_followsTheProbedSpectrumNotThePane();
    void exportRegion_followsRegionExistence();
    void sendSlice_followsAnOpenImageViewer();
    void linkViews_followsLinkTargets();
};
