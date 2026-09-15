// test_fits_header_string.cpp
// A remote dataset's WCS is parsed out of the cards the backend sends, and
// libwcs reads them as one flat buffer of fixed-width cards. Every rule here
// fails silently when broken - a short card shifts every keyword after it, a
// missing END makes the parser run off the end - so they are pinned.

#include "TestFitsHeaderString.h"

#include "FitsHeaderString.h"

#include <QTest>

namespace {
// astropy hands over card images; a trailing-space-trimmed one is what arrives.
const std::vector<std::string> kCards = {
    "SIMPLE  =                    T / conforms to FITS standard",
    "BITPIX  =                  -32",
    "NAXIS   =                    2",
    "CTYPE1  = 'RA---TAN'",
    "CRVAL1  =                 10.5",
};
}   // namespace

void TestFitsHeaderString::everyCardIsPaddedToEightyColumns()
{
    const std::string header = visivo::buildFitsHeaderString(kCards);
    // cards + END, each exactly 80 columns
    QCOMPARE(header.size(), (kCards.size() + 1) * visivo::kFitsCardLength);
    for (std::size_t i = 0; i < kCards.size(); ++i) {
        const std::string card = header.substr(i * visivo::kFitsCardLength,
                                               visivo::kFitsCardLength);
        QCOMPARE(card.size(), visivo::kFitsCardLength);
        QVERIFY2(card.rfind(kCards[i], 0) == 0,
                 qPrintable(QString::fromStdString("card " + std::to_string(i) + " = '" + card
                                                   + "'")));
        QCOMPARE(card.back(), ' ');
    }
    // The keyword of card N really starts at column 80·N - which is the whole
    // point of the padding.
    QCOMPARE(header.substr(3 * visivo::kFitsCardLength, 6), std::string("CTYPE1"));
}

void TestFitsHeaderString::aLongValueKeepsItsContinuationCards()
{
    // astropy returns the WHOLE physical sequence for a long string value - the
    // card and its CONTINUE images - as one entry. Truncating it to 80 columns
    // threw the continuations away and left a value ending in '&' that points at
    // nothing; the chunks must all survive, still 80 columns each.
    auto card80 = [](std::string text) {
        text.resize(visivo::kFitsCardLength, ' ');
        return text;
    };
    const std::string first = card80("LONGKEY = 'first half of the value, continued&'");
    const std::string cont = card80("CONTINUE  'the second half'");
    const std::string header = visivo::buildFitsHeaderString({ first + cont, "NAXIS   =  2" });
    QCOMPARE(header.size(), 4 * visivo::kFitsCardLength);
    QCOMPARE(header.substr(0, 7), std::string("LONGKEY"));
    QCOMPARE(header.substr(visivo::kFitsCardLength, 8), std::string("CONTINUE"));
    // …and the card after it still starts on its own 80-column boundary.
    QCOMPARE(header.substr(2 * visivo::kFitsCardLength, 5), std::string("NAXIS"));

    // A ragged over-long entry is padded chunk by chunk, never left short.
    std::string ragged = "COMMENT ";
    ragged.append(100, 'x');
    const std::string h2 = visivo::buildFitsHeaderString({ ragged });
    QCOMPARE(h2.size(), 3 * visivo::kFitsCardLength);   // 2 chunks + END
}

void TestFitsHeaderString::theBufferAlwaysEndsWithAPaddedEnd()
{
    const std::string header = visivo::buildFitsHeaderString(kCards);
    const std::string last = header.substr(header.size() - visivo::kFitsCardLength);
    QCOMPARE(last.substr(0, 3), std::string("END"));
    QCOMPARE(last.size(), visivo::kFitsCardLength);
    QCOMPARE(last.find_first_not_of(' ', 3), std::string::npos);
}

void TestFitsHeaderString::noCardsStillYieldsAParseableHeader()
{
    // Not a crash and not an empty buffer: libwcs is handed something it can
    // terminate on, and reports "no WCS" rather than reading past the end.
    const std::string header = visivo::buildFitsHeaderString({});
    QCOMPARE(header.size(), visivo::kFitsCardLength);
    QCOMPARE(header.substr(0, 3), std::string("END"));
}
