#pragma once
// TestLteCoRequest.h
// QTest class for the desktop side of the LTE analysis of CO isotopologues
// (issue #416, app/LteCoRequest.h): the request, the answer, the CSV, the
// guesses that fill the dialog, and BackendClient::lteCo against a canned
// backend. No GUI.

#include <QObject>

class TestLteCoRequest : public QObject
{
    Q_OBJECT

private slots:
    void unsetFieldsAreLeftToTheBackend();
    void aCubeNeedsItsVelocityWindow();
    void outOfRangeInputsAreRefusedBeforeSending();
    void typedNumbersReadInEitherLocale();
    void theAnswerIsParsedWithNullsAsUnknown();
    void theEtaTheBackendAskedForIsFound();
    void theAssumptionsSayWhereEachNumberCameFrom();
    void theTableRowCarriesUnitsAndErrors();
    void theCsvCarriesProvenanceAndFullPrecision();
    void namesAndRestFrequenciesGuessTheLine();
    void velocitiesAreConvertedToKmPerSecond();
    void onlyAreaRegionsAreMeasured();
    void theClientPostsTheRequestAndReadsTheAnswer();
    void theClientReportsARefusal();
};
