#pragma once
// TestCatalogueCsv.h
// QTest class for the catalogue CSV reader: quoting, multi-line records and
// truncated files. Header-only parsing; no GUI, no VTK.

#include <QObject>

class TestCatalogueCsv : public QObject
{
    Q_OBJECT

private slots:
    void quotedCommaDoesNotShiftTheColumns();
    void aValueSpanningTwoLinesStaysOneRow();
    void aHeaderSpanningTwoLinesStaysOneHeader();
    void escapedQuotesAndEmptyFieldsSurvive();
    void crlfAndAMissingFinalNewlineAreRead();
    void aFileWithOnlyAHeaderIsRejectedCleanly();
    void anUnterminatedQuoteInTheHeaderIsAnError();
    void aTruncatedLastRowIsSkippedAndReported();
    void duplicateFieldNamesBothSurvive();
};
