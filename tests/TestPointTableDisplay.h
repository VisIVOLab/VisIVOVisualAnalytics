#pragma once
// TestPointTableDisplay.h
// QTest class for what the 3-D points window says about a table of positions
// (issue #563): the unit on an axis title, the name of the table's format in
// Session Data, the colour a table opens with, and a colour bar over a column
// that does not vary. Header-only logic; no window.

#include <QObject>

class TestPointTableDisplay : public QObject
{
    Q_OBJECT

private slots:
    void anAxisNamesTheUnitTheTableStatesForIt();
    void aNoteAfterTheUnitIsLeftOffTheTitle();
    void aTableThatStatesNothingKeepsMpcOnItsComputedPositions();
    void aMappedColumnWithoutAUnitHasNoneToName();
    void aRamsesOutputIsNamedRamsesNotVbt();
    void aVbtAndAnOlderBackendStayVbt();
    void thePreferredColumnIsKeptWhenItVaries();
    void theFirstColumnIsKeptWhenNoneIsPreferred();
    void aConstantMassGivesWayToAColumnThatVaries();
    void noColumnIsChosenWhenEveryoneIsConstant();
    void theChoiceWaitsForColumnsNotLoadedYet();
    void anIdentifierIsNeverTheDefault();
    void aConstantColumnHasOneValueNotAMadeUpRange();
    void aColumnThatVariesKeepsItsRange();
    void aConstantBarIsOneValueAndItsOwnColour();
};
