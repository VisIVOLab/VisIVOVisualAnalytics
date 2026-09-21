#pragma once
// TestFitsHeaderCard.h
// QTest class for splitting a raw 80-column FITS card into keyword, value and
// comment — the parsing behind the header editor. Header-only; no FITS library.

#include <QObject>

class TestFitsHeaderCard : public QObject
{
    Q_OBJECT

private slots:
    void aNumericCardSplitsIntoKeyValueComment();
    void aQuotedStringKeepsItsSlash();
    void anEscapedQuoteInsideAStringSurvives();
    void aCommentContainingASlashIsKeptWhole();
    void aCardWithoutACommentHasNone();
    void historyAndCommentAreCommentary();
    void aCardWithoutAnEqualsIsCommentary();
    void theKeywordIsUpperCasedAndTrimmed();
    void valuesThatWouldBeReadBackAsNumbersNeedQuoting();
};
