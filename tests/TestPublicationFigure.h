#pragma once
// TestPublicationFigure.h
// QTest class for what the Publication Figure dialog asks of the backend
// (issue #41, gui/PublicationFigureSpec.h): the one-click defaults, the request
// body, the overlays in the canonical region form, the formats and colour maps,
// and BackendClient::publicationFigure against a canned backend. No GUI.

#include <QObject>

class TestPublicationFigure : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAreTheOneClickFigure();
    void everyOptionReachesTheBody();
    void theWindowsRangeIsSentOnlyWhenUsed();
    void aPreviewHasNoFileAndAFigureHasItsFormat();
    void overlaysAreSentOnlyWhenChosen();
    void annotationsAreCanonicalTextAndVectors();
    void hiddenRegionsStayOut();
    void contourLinesKeepTheirColourWidthAndDash();
    void contourLinesAreJoinedOnceAndKept();
    void colourMapsOfTheWindowMapToTheBackendsOrInferno();
    void theDialogOpensWithTheWholeRegistryBeforeTheBackendAnswers();
    void theBackendsAnswerNarrowsTheListAndFlagsStandIns();
    void aListThatReadsTheSameIsNotRebuilt();
    void theWindowsTableIsNotOfferedWhereTheNameIsExactOrThereIsNone();
    void theWindowsTableAndTheScaleBarRuleReachTheBody();
    void theColourMapLookupIsNotAHeavyJob();
    void theClientReadsTheColourMapAnswer();
    void anOlderBackendGivesNoColourMapAnswer();
    void theWindowsFrameIsTheFiguresFrame();
    void formatsAreThoseTheBackendWrites();
    void theClientPostsTheBodyAndReadsThePreview();
    void theClientReportsARefusal();
};
