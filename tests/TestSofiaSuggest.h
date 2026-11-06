#pragma once
// TestSofiaSuggest.h
// QTest class for SofiaSuggest — the SoFiA-2 parameter suggestion (issue #64)
// as the dialog reads it, edits it and records it. Core-only; reads the
// service's request models from the repository.

#include <QObject>

class TestSofiaSuggest : public QObject
{
    Q_OBJECT

private slots:
    void parse_readsTheDocumentedAnswer();
    void parse_realAnswerRunsAsTheServiceMapsIt();
    void parse_handlesMissingDefaultsAndOptions();
    void parse_noParametersIsNotASuggestion();
    void measurementLine_isCompactAndSkipsWhatIsMissing();
    void edits_surviveANewSuggestion();
    void edits_typingTheSuggestionBackClearsTheEdit();
    void edits_keysTheSuggestionDropsGoUnlessTheUserSetThem();
    void reset_putsEveryEditBack();
    void region_isSentOnlyAfterAYes();
    void status_saysHowEachValueCameToBe();
    void request_usesTheServiceRequestModelsFieldNames();
    void replay_usesTheTaskRouteRequestModelsFieldNames();
    void ask_failedRequestGoesBackToTheAnsweredOne();
    void errorDetail_readsFastApiBodies();
    void refusal_readsTheBodyNotQtsErrorString();
    void display_maskColumnsAndRunLabels();
    void emptyResult_isAResultNotAFailure();
    void addedParams_areTheUsersAndSurvive();
    void addedParams_takeTheBackendsDefaultsAndNames();
    void failureText_saysWhetherSofiaRan();
    void runSummary_readsSofiasCounts();
};
