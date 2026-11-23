#pragma once
// TestSedBuilder.h
// QTest class for building a SED out of catalogue rows: VLKB band-merged
// groups, flat catalogues with several flux columns, and the ways both go
// wrong. Header-only logic; no GUI, no VTK, no network.

#include <QObject>

class TestSedBuilder : public QObject
{
    Q_OBJECT

private slots:
    void aBandMergedGroupBecomesOnePointPerBand();
    void bandsWithoutAUsableFluxAreReportedNotSilentlyDropped();
    void theMsxSubBandsMapToTheirRealWavelengths();
    void aFlatRowWithSeveralFluxColumnsBecomesASed();
    void fluxColumnNamesAreRecognisedInTheirCommonSpellings();
    void aColumnThatOnlyLooksLikeAFluxIsNotOne();
    void errorColumnsFollowTheirFluxColumnSpelling();
    void duplicateBandsAreKeptAsBranches();
    void abandWithTwoBranchesIsStillOneBand();
    void pointsComeBackSortedByWavelength();
    void aSourceWithOneBandIsRefusedWithAReason();
    void anEmptySelectionIsRefusedWithAReason();
    void theGroupOfASourceIsItsWholeBandMergedFamily();
    void anUngroupedSourceIsItsOwnGroup();
    void sizeComesFromTheBandFwhmAndFallsBackToTheEllipse();
    void drawnRadiiAreNeverReadAsAnAngularSize();
    void aWavelengthUnitSuffixIsConvertedNotIgnored();
    void theRepresentativeSizeIsTheMedianOfTheUsedBands();
    void wiseAndTwoMassColumnsAreReadAtTheirRealWavelengths();
    void aVlkbGroupTakesItsCounterpartFluxesFromTheirOwnBand();
    void msxErrorsArePercentOfTheFlux();
    void theDefaultFitStartsAt160Micron();
    void theDefaultFitFallsBackToEveryBandWhenTooFewAreLeft();
    void theCatalogueDistanceIsReadButInParsecs();
    void scalingTo250ReproducesTheCatalogueFluxes();
    void scalingTo250NeedsA250Size();
    void scalingTo250NeverMixesDeconvolvedAndObservedSizes();
    void observedPlusGreybodyLuminosityFollowsTheCatalogueRecipe();
};
