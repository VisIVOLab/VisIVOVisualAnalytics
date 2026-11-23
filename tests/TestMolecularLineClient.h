#pragma once
// TestMolecularLineClient.h
// QTest class for the desktop side of issue #416: the Gaussian fit window and
// its warnings (app/LineFitRequest.h), and the typed beam and inferred
// intensity unit (app/DatasetBeam.h). Header-only logic; no GUI, no network.

#include <QObject>

class TestMolecularLineClient : public QObject
{
    Q_OBJECT

private slots:
    void aZoomedPlotSendsItsRangeAsTheFitWindow();
    void aPlotShowingEverythingSendsNoWindow();
    void aWindowOutsideTheDataLeavesNoChannelsToFit();
    void theFitWarningIsParsed();
    void aNegativePeakOnAnEmissionSpectrumIsFlagged();
    void aNegativePeakOnAnAbsorptionSpectrumIsNot();
    void theBeamRequestCarriesArcsecAndClear();
    void theBeamResponseIsParsedInDegrees();
    void aBeamCanBeTypedOnlyWhenTheHeaderHasNone();
    void theBeamSummarySaysWhereItCameFrom();
    void theInferredUnitIsPreferredAndSaidOnTheAxis();
};
