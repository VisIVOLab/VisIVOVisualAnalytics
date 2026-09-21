// test_fits_header_card.cpp
// See TestFitsHeaderCard.h.

#include "TestFitsHeaderCard.h"

#include "FitsHeaderCard.h"

#include <QTest>

using visivo::needsQuoting;
using visivo::parseFitsHeaderCard;

void TestFitsHeaderCard::aNumericCardSplitsIntoKeyValueComment()
{
    const auto card = parseFitsHeaderCard(
            QStringLiteral("CDELT1  =            -0.001000 / degrees per pixel"));
    QCOMPARE(card.key, QStringLiteral("CDELT1"));
    QCOMPARE(card.value, QStringLiteral("-0.001000"));
    QCOMPARE(card.comment, QStringLiteral("degrees per pixel"));
    QVERIFY(!card.commentary);
}

void TestFitsHeaderCard::aQuotedStringKeepsItsSlash()
{
    // The reason this parser exists: splitting on the first '/' turns a BUNIT
    // of 'Jy/beam' into 'Jy' with a comment of "beam'".
    const auto card = parseFitsHeaderCard(
            QStringLiteral("BUNIT   = 'Jy/beam '           / brightness unit"));
    QCOMPARE(card.key, QStringLiteral("BUNIT"));
    QCOMPARE(card.value, QStringLiteral("Jy/beam"));
    QCOMPARE(card.comment, QStringLiteral("brightness unit"));
}

void TestFitsHeaderCard::anEscapedQuoteInsideAStringSurvives()
{
    const auto card = parseFitsHeaderCard(
            QStringLiteral("OBJECT  = 'it''s here / there' / name"));
    QCOMPARE(card.value, QStringLiteral("it's here / there"));
    QCOMPARE(card.comment, QStringLiteral("name"));
}

void TestFitsHeaderCard::aCommentContainingASlashIsKeptWhole()
{
    // Splitting on the LAST '/' — the other obvious implementation — eats it.
    const auto card = parseFitsHeaderCard(
            QStringLiteral("TELESCOP= 'APEX    '           / m/s convention, see doc/notes"));
    QCOMPARE(card.value, QStringLiteral("APEX"));
    QCOMPARE(card.comment, QStringLiteral("m/s convention, see doc/notes"));
}

void TestFitsHeaderCard::aCardWithoutACommentHasNone()
{
    const auto card = parseFitsHeaderCard(QStringLiteral("CRPIX1  =                 16.5"));
    QCOMPARE(card.value, QStringLiteral("16.5"));
    QVERIFY(card.comment.isEmpty());
}

void TestFitsHeaderCard::historyAndCommentAreCommentary()
{
    const auto history = parseFitsHeaderCard(
            QStringLiteral("HISTORY  reduced with pipeline v2 / 2026-01-01"));
    QVERIFY(history.commentary);
    QCOMPARE(history.key, QStringLiteral("HISTORY"));
    QCOMPARE(history.value, QStringLiteral("reduced with pipeline v2 / 2026-01-01"));
    QVERIFY(parseFitsHeaderCard(QStringLiteral("COMMENT  anything at all")).commentary);
}

void TestFitsHeaderCard::aCardWithoutAnEqualsIsCommentary()
{
    QVERIFY(parseFitsHeaderCard(QStringLiteral("WEIRD    no equals sign here")).commentary);
}

void TestFitsHeaderCard::theKeywordIsUpperCasedAndTrimmed()
{
    QCOMPARE(parseFitsHeaderCard(QStringLiteral("bunit   = 'K'")).key, QStringLiteral("BUNIT"));
}

void TestFitsHeaderCard::valuesThatWouldBeReadBackAsNumbersNeedQuoting()
{
    QVERIFY(needsQuoting(QStringLiteral("12345")));
    QVERIFY(needsQuoting(QStringLiteral("-0.001")));
    QVERIFY(needsQuoting(QStringLiteral("T")));
    QVERIFY(!needsQuoting(QStringLiteral("RA---TAN")));
    QVERIFY(!needsQuoting(QString()));
}
