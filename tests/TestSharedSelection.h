#pragma once
// TestSharedSelection.h
// QTest class for SharedSelection — a selection shared between the VR
// headset's page and the cube viewer (issue #188). Core-only.

#include <QObject>

class TestSharedSelection : public QObject
{
    Q_OBJECT

private slots:
    void growRequest_roundTripsThroughTheApiMap();
    void growRequest_readsTheVrPagesDefaultRequest();
    void info_parsesTheHubsAnswer();
    void info_aBoxTakesItsRangeFromTheRecordOrItsBox();
    void info_refusesMalformedAnswers();
    void refusal_skipsOwnOldAndOtherCubes();
    void publishQuery_keepsTheRecordIntactOnTheWire();
    void maskCount_countsSetVoxelsAndChecksTheSize();
    void note_saysWhoVoxelsAndFlux();
    void boxRecord_isTheBoxsOwnCall();
    void applyGate_acceptsAReceivedSelectionWithoutASeed();
    void applyRecord_ofASelectionMadeHereIsItsGrow();
    void applyRecord_ofAReceivedLassoIsItsCreatorsGrow();
    void applyRecord_ofAReceivedLassoWithoutAGrowIsTheMaskOnly();
    void applyRecord_ofAReceivedBoxIsTheBox();
    void unsupported_onlyForABackendWithoutTheHub();
    void reconcile_convergesOnTheHubsLatest();
    void sharedCases_agreeWithThePage();
    void reconcile_theReviewCasesEndOnOneSelection();
};
