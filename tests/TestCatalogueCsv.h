#pragma once
// TestCatalogueCsv.h
// QTest class for the catalogue CSV reader: quoting, multi-line records,
// truncated files, and tab/semicolon tables told from DS9 regions by content.
// Header-only parsing; no GUI, no VTK.

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
    // Delimited text by content (issue #155).
    void sniffTriesCommaThenTabThenSemicolon();
    void quotedFieldsMayHoldTheOtherSeparators();
    void aTabTableIsReadAsATable();
    void aSemicolonTableWithDecimalCommasReadsNumbers();
    void commentsAndBlankLinesAreNotRows();
    void ds9FilesAreStillRegions();
    void aCsvOrTsvThatIsNoTableSaysSo();
    void aHashLineNamesTheColumnsOfANumericFirstRow();
    void aDescriptiveHashLineAboveARealHeaderIsAComment();
    void aHashRowWithTheTablesFieldCountIsData();
    void aHashLineInsideAQuotedFieldIsPartOfIt();
    void oneCommaListColumnIsNotADecimalComma();
    void raggedTrailingCommaAndOneColumnFilesReadAsComma();
    void aByteOrderMarkIsNotPartOfTheFirstLine();
    void columnsNamedLikeRegionWordsAreStillTables();
    void aQuoteOpensAFieldOnlyAtItsStart();
    void aLongFileIsReadInLinearTime();
    void decimalCommasAreJudgedPerColumn();
    void theCommaFallbackTakesNaNAndInfinityAsNumbers();
    void aLoneCarriageReturnEndsALine();
};
