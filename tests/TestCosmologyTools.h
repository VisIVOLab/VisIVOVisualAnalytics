#pragma once
// TestCosmologyTools.h
// QTest class for the client side of issue #29 (cosmology tools in the GUI):
//
//   * CosmologyModel.h — custom-model validation and naming (a non-flat model
//     must say so), redshift-list parsing, the CSV / clipboard table;
//   * CosmologyDistanceTable.h — the node table the catalogue viewer reads a
//     custom model's distances from, against a stand-in integrator;
//   * PowerSpectrumSlope.h — the P(ℓ) slope fit;
//   * the new BackendClient bindings and parsers, and CosmologyReplay.h, whose
//     field names are checked against the backend's own request models.

#include <QObject>

class TestCosmologyTools : public QObject
{
    Q_OBJECT

private slots:
    // CosmologyModel.h
    void presetsAreTheBackendsFour();
    void customModelIsValidatedLikeTheBackend();
    void describeSaysWhenACustomModelIsNotFlat();
    void redshiftListAcceptsCommasSpacesAndLines();
    void redshiftListReportsWhatItRefuses();
    void scaleIsAngularDiameterDistancePerArcsecond();
    void tableNamesTheModelAndLeavesMissingCellsEmpty();
    void tableQuotesCellsThatHoldTheSeparator();
    void customModelRoundTripsThroughSettings();

    // CosmologyDistanceTable.h
    void distinctUsableDropsUnmeasuredAndRepeats();
    void tableNodesSpanTheRangeExactly();
    void interpolationIsWithinOnePartIn1e4();
    void interpolationReturnsNodeValuesExactly();
    void interpolationNeverExtrapolates();

    // PowerSpectrumSlope.h
    void slopeOfAnExactPowerLaw();
    void slopeHonoursRangeAndModeCount();
    void slopeNeedsThreeBins();
    void slopeSkipsNonPositivePower();
    void slopeIsWeightedByTheBinErrors();

    // BackendClient bindings
    void distancesQueryUsesTheRoutesParameterNames();
    void distancesQueryOmitsParametersForPlanck18();
    void parseDistancesReadsEveryField();
    void parseDistancesTurnsAMissingFieldIntoNaN();
    void parseDistancesReadsARefusalsDetail();
    void parsePresetDistanceReadsTheScale();
    void parseAngularPowerReadsTheFullAnswer();
    void parseAngularPowerFallsBackOnLegacyFields();
    void skeletonBodySendsNullForAutoAndNoPruning();
    void parseSkeletonDecodesTheMask();
    void parseSkeletonRefusesAMaskOfTheWrongSize();

    // CosmologyReplay.h against the backend's request models
    void angularPowerReplayUsesTheRoutesFieldNames();
    void skeletonReplayUsesTheRoutesFieldNames();
    void momentReplayUsesTheTaskRoutesFieldNames();
    void skeletonDisplayParamsStateTheAppliedThreshold();
    void runLabelsPutWhatDiffersFirst();
    void momentExportNameIsStablePerRecipe();
};
