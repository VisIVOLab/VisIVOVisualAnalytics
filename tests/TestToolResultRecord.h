#pragma once
// TestToolResultRecord.h
// QTest class for ToolResultRecord — the serialised shape of a tool's result
// window. Header-only pure logic; no GUI link.

#include <QObject>

class TestToolResultRecord : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip_keepsEveryRowAsALabelValuePair();
    void roundTrip_keepsSectionAndRowOrder();
    void rowsAreNotFlattened_theBugThatEmptiedTheWindow();
    void emptyRecord_decodesToNothingRatherThanCrashing();
    void malformedRows_areSkippedNotGuessedAt();
    void titleAndSubtitle_surviveTheRoundTrip();
};
