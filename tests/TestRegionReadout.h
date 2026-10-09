#pragma once
// TestRegionReadout.h
// QTest class for the live region readout: how much of a region it is willing
// to touch per mouse move, and how it says so. No widgets, no VTK.

#include <QObject>

class TestRegionReadout : public QObject
{
    Q_OBJECT

private slots:
    void aSmallRegionIsReadWhole();
    void aLargeRegionIsSampledWithinTheBudget();
    void theStrideIsNeverZero();
    void theTextCarriesBothNumbers();
    void sampledFiguresAreMarkedAsEstimates();
    void anEmptyRegionSaysSoInsteadOfShowingZero();
};
