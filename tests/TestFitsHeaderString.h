#pragma once
// TestFitsHeaderString.h
// QTest class for the cards -> header-string conversion libwcs parses.
// Header-only pure logic; no libwcs, no GUI link.

#include <QObject>

class TestFitsHeaderString : public QObject
{
    Q_OBJECT

private slots:
    void everyCardIsPaddedToEightyColumns();
    void anOverlongCardIsTruncatedRatherThanShiftingTheRest();
    void theBufferAlwaysEndsWithAPaddedEnd();
    void noCardsStillYieldsAParseableHeader();
};
