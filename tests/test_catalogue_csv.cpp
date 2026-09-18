// test_catalogue_csv.cpp
// Catalogues arrive as files someone else wrote, and the failure mode of a CSV
// reader is not a crash: it is a source silently placed at the wrong
// coordinates because a quoted comma shifted every column after it. These cases
// pin the reader against files that are quoted, wrapped, or simply truncated.

#include "TestCatalogueCsv.h"

#include "CatalogueOverlayUtils.h"

#include <QTemporaryDir>
#include <QTest>

namespace {
QString writeCsv(const QTemporaryDir &dir, const QString &name, const QString &content)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    f.write(content.toUtf8());
    f.close();
    return path;
}
}   // namespace

void TestCatalogueCsv::quotedCommaDoesNotShiftTheColumns()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("q.csv"),
                                  QStringLiteral("RA,DEC,note,flux\n"
                                                 "10.5,-20.25,\"bright, blended\",3.5\n"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    QVERIFY(r.valid);
    QCOMPARE(r.entries.size(), std::size_t(1));
    QCOMPARE(r.entries[0].raDeg, 10.5);
    QCOMPARE(r.entries[0].decDeg, -20.25);
    // …and the flux is still the flux, not half a note.
    bool sawFlux = false;
    for (const auto &kv : r.entries[0].extra)
        if (kv.first == QStringLiteral("flux")) {
            QCOMPARE(kv.second, QStringLiteral("3.5"));
            sawFlux = true;
        }
    QVERIFY(sawFlux);
}

void TestCatalogueCsv::aValueSpanningTwoLinesStaysOneRow()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("multi.csv"),
                                  QStringLiteral("RA,DEC,note,flux\n"
                                                 "1,2,\"first line\nsecond line\",9\n"
                                                 "3,4,plain,8\n"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    QVERIFY(r.valid);
    QCOMPARE(r.entries.size(), std::size_t(2));   // two sources, not three
    QCOMPARE(r.entries[1].raDeg, 3.0);
    // The assertions above pass even when the two physical lines are read
    // separately (the fragment is dropped as invalid, and the count still comes
    // out at two). These do not: the value has to be WHOLE, the column after it
    // has to be in the right place, and nothing may be skipped.
    QCOMPARE(r.skippedEntries, 0);
    auto field = [](const CatalogueOverlayEntry &e, const QString &key) {
        for (const auto &kv : e.extra)
            if (kv.first == key)
                return kv.second;
        return QString();
    };
    QCOMPARE(field(r.entries[0], QStringLiteral("note")),
             QStringLiteral("first line\nsecond line"));
    QCOMPARE(field(r.entries[0], QStringLiteral("flux")), QStringLiteral("9"));
}

void TestCatalogueCsv::aHeaderSpanningTwoLinesStaysOneHeader()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("hdr.csv"),
                                  QStringLiteral("RA,DEC,\"flux\ndensity\"\n"
                                                 "1,2,7\n"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    QVERIFY(r.valid);
    QCOMPARE(r.entries.size(), std::size_t(1));
    QCOMPARE(r.extraFields.size(), 1);
    // The WHOLE column name, and its value attached to it: reading the header
    // line by line also yields a field called "flux" and a row of three numbers,
    // so only the full name and the value tell the two parsers apart.
    QCOMPARE(r.extraFields.first(), QStringLiteral("flux\ndensity"));
    QCOMPARE(r.entries[0].extra.size(), 1);
    QCOMPARE(r.entries[0].extra[0].first, QStringLiteral("flux\ndensity"));
    QCOMPARE(r.entries[0].extra[0].second, QStringLiteral("7"));
    QCOMPARE(r.skippedEntries, 0);
}

void TestCatalogueCsv::escapedQuotesAndEmptyFieldsSurvive()
{
    QTemporaryDir dir;
    // "" is a literal quote inside a quoted field; "" alone is an empty field
    // that must still hold its column position.
    const QString path = writeCsv(dir, QStringLiteral("esc.csv"),
                                  QStringLiteral("RA,DEC,note,flag,flux\n"
                                                 "1,2,\"say \"\"hello\"\"\",\"\",5\n"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    QVERIFY(r.valid);
    QCOMPARE(r.entries.size(), std::size_t(1));
    auto field = [](const CatalogueOverlayEntry &e, const QString &key) {
        for (const auto &kv : e.extra)
            if (kv.first == key)
                return kv.second;
        return QString();
    };
    QCOMPARE(field(r.entries[0], QStringLiteral("note")), QStringLiteral("say \"hello\""));
    QCOMPARE(field(r.entries[0], QStringLiteral("flux")), QStringLiteral("5"));   // not shifted
}

void TestCatalogueCsv::crlfAndAMissingFinalNewlineAreRead()
{
    QTemporaryDir dir;
    // Windows line endings, and no newline after the last row.
    const QString path = writeCsv(dir, QStringLiteral("crlf.csv"),
                                  QStringLiteral("RA,DEC,flux\r\n1,2,7\r\n3,4,8"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    QVERIFY(r.valid);
    QCOMPARE(r.entries.size(), std::size_t(2));
    QCOMPARE(r.entries[1].decDeg, 4.0);
}

void TestCatalogueCsv::aFileWithOnlyAHeaderIsRejectedCleanly()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("hdronly.csv"),
                                  QStringLiteral("RA,DEC,flux\n"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    QVERIFY(!r.valid);                  // nothing to show
    QVERIFY(!r.errorMessage.isEmpty()); // and it says so
}

void TestCatalogueCsv::anUnterminatedQuoteInTheHeaderIsAnError()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("badhdr.csv"),
                                  QStringLiteral("RA,DEC,\"flux\n"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    QVERIFY(!r.valid);
    QVERIFY(!r.errorMessage.isEmpty());
}

void TestCatalogueCsv::aTruncatedLastRowIsSkippedAndReported()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("trunc.csv"),
                                  QStringLiteral("RA,DEC,note\n"
                                                 "1,2,ok\n"
                                                 "3,4,\"never closed\n"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    // What could be read is loaded…
    QVERIFY(r.valid);
    QCOMPARE(r.entries.size(), std::size_t(1));
    QCOMPARE(r.skippedEntries, 1);
    // …and the file is not passed off as whole.
    QVERIFY(!r.errorMessage.isEmpty());
}

void TestCatalogueCsv::duplicateFieldNamesBothSurvive()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("dup.csv"),
                                  QStringLiteral("RA,DEC,flux,flux\n"
                                                 "1,2,10,20\n"));
    const auto r = CatalogueOverlayUtils::parseCsv(path);
    QVERIFY(r.valid);
    QCOMPARE(r.extraFields.size(), 2);            // two columns, not one
    QCOMPARE(r.entries[0].extra.size(), 2);
    QCOMPARE(r.entries[0].extra[0].second, QStringLiteral("10"));
    QCOMPARE(r.entries[0].extra[1].second, QStringLiteral("20"));
}
