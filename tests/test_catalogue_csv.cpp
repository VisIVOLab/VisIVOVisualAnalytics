// test_catalogue_csv.cpp
// Catalogues arrive as files someone else wrote, and the failure mode of a CSV
// reader is not a crash: it is a source silently placed at the wrong
// coordinates because a quoted comma shifted every column after it. These cases
// pin the reader against files that are quoted, wrapped, or simply truncated.

#include "TestCatalogueCsv.h"

#include "CatalogueOverlayUtils.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

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

// ── Delimited text by content (issue #155) ──────────────────────────────────
// The overlays used to send `.csv` to the table reader and everything else to
// the DS9 reader, so a `.tsv` catalogue was read as regions and found none.
// These pin the client to the backend's rule (backend/app/text_table.py).

namespace {
QString extraValue(const CatalogueOverlayEntry &e, const QString &key)
{
    for (const auto &kv : e.extra)
        if (kv.first == key)
            return kv.second;
    return QString();
}
}   // namespace

void TestCatalogueCsv::sniffTriesCommaThenTabThenSemicolon()
{
    using DelimitedText::sniffSeparator;
    QCOMPARE(sniffSeparator(u"ra,dec\n1,2\n"_s), QChar(u','));
    QCOMPARE(sniffSeparator(u"ra\tdec\n1\t2\n"_s), QChar(u'\t'));
    QCOMPARE(sniffSeparator(u"ra;dec\n1;2\n"_s), QChar(u';'));
    // Comma first: a semicolon table whose numbers have decimal commas would
    // also split on commas, but not into the same count on every line.
    QCOMPARE(sniffSeparator(u"ra;dec;name\n10,5;-20,25;a\n11,5;-21;b\n"_s), QChar(u';'));
    // One line is a sentence, not a table; nor is prose with stray commas.
    QVERIFY(sniffSeparator(u"ra,dec\n"_s).isNull());
    QVERIFY(sniffSeparator(u"Hello, world.\nNo table here.\n"_s).isNull());
    // A start of a longer file: its cut last line is not judged.
    QCOMPARE(sniffSeparator(u"a,b\n1,2\n3,4\n5"_s, true), QChar(u','));
    QVERIFY(sniffSeparator(u"a,b\n1,2\n3,4\n5"_s, false).isNull());
}

void TestCatalogueCsv::quotedFieldsMayHoldTheOtherSeparators()
{
    using DelimitedText::sniffSeparator;
    // A quoted tab or semicolon inside a comma table, a quoted comma inside a
    // tab table: the field count stays the same, so the separator is still
    // found, and the field is whole.
    QCOMPARE(sniffSeparator(u"ra,dec,note\n1,2,\"a\tb;c\"\n3,4,x\n"_s), QChar(u','));
    QCOMPARE(sniffSeparator(u"ra\tdec\tnote\n1\t2\t\"a, b\"\n3\t4\t\"c, d, e\"\n"_s),
             QChar(u'\t'));
    QCOMPARE(sniffSeparator(u"ra;dec;note\n1;2;\"x; y\"\n3;4;z\n"_s), QChar(u';'));

    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("quoted.tsv"),
                                  QStringLiteral("RA\tDEC\tnote\tflux\n"
                                                 "10.5\t-20.25\t\"bright\tblended, faint\"\t3.5\n"
                                                 "11\t-21\tplain\t4\n"));
    const auto r = CatalogueOverlayUtils::parseFile(path);
    QVERIFY2(r.valid, qPrintable(r.errorMessage));
    QCOMPARE(r.entries.size(), std::size_t(2));
    QCOMPARE(extraValue(r.entries[0], QStringLiteral("note")),
             QStringLiteral("bright\tblended, faint"));
    QCOMPARE(extraValue(r.entries[0], QStringLiteral("flux")), QStringLiteral("3.5"));
}

void TestCatalogueCsv::aTabTableIsReadAsATable()
{
    QTemporaryDir dir;
    // Named .tsv and named .txt: the content decides, not the name.
    for (const QString &name : { QStringLiteral("cat.tsv"), QStringLiteral("cat.txt") }) {
        const QString path = writeCsv(dir, name,
                                      QStringLiteral("NAME\tRA\tDEC\tflux\n"
                                                     "src1\t10.5\t-20.25\t3.5\n"
                                                     "src2\t11.0\t-21.0\t\n"));
        const auto r = CatalogueOverlayUtils::parseFile(path);
        QVERIFY2(r.valid, qPrintable(name + u": "_s + r.errorMessage));
        QCOMPARE(r.entries.size(), std::size_t(2));
        QCOMPARE(r.entries[0].label, QStringLiteral("src1"));
        QCOMPARE(r.entries[0].raDeg, 10.5);
        QCOMPARE(r.entries[1].decDeg, -21.0);
        QCOMPARE(r.extraFields, QStringList{ QStringLiteral("flux") });
        // A trailing empty field is an empty value, not a shifted row.
        QCOMPARE(r.entries[1].extra.size(), 0);
        QCOMPARE(r.skippedEntries, 0);
    }
}

void TestCatalogueCsv::aSemicolonTableWithDecimalCommasReadsNumbers()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("eu.txt"),
                                  QStringLiteral("ID;RA;DEC;flux\n"
                                                 "a;10,5;-20,25;3,75\n"
                                                 "b;11;-21,5;4\n"));
    const auto r = CatalogueOverlayUtils::parseFile(path);
    QVERIFY2(r.valid, qPrintable(r.errorMessage));
    QCOMPARE(r.entries.size(), std::size_t(2));
    QCOMPARE(r.entries[0].raDeg, 10.5);
    QCOMPARE(r.entries[0].decDeg, -20.25);
    QCOMPARE(r.entries[1].decDeg, -21.5);
    // The extra fields too: a SED fit reads its fluxes from them.
    QCOMPARE(extraValue(r.entries[0], QStringLiteral("flux")), QStringLiteral("3.75"));

    // A semicolon table written with decimal POINTS is not a decimal-comma
    // table at all, so nothing in it is rewritten (the per-column case, a text
    // column in a decimal-comma table, is decimalCommasAreJudgedPerColumn).
    const QString points = writeCsv(dir, QStringLiteral("pts.csv"),
                                    QStringLiteral("ID;RA;DEC;note\n"
                                                   "a;10.5;-20.25;1,2\n"
                                                   "b;11.5;-21.0;x\n"));
    const auto p = CatalogueOverlayUtils::parseFile(points);
    QVERIFY2(p.valid, qPrintable(p.errorMessage));
    QCOMPARE(p.entries[0].raDeg, 10.5);
    QCOMPARE(extraValue(p.entries[0], QStringLiteral("note")), QStringLiteral("1,2"));
}

void TestCatalogueCsv::commentsAndBlankLinesAreNotRows()
{
    QTemporaryDir dir;
    const QString path = writeCsv(dir, QStringLiteral("commented.csv"),
                                  QStringLiteral("# exported by a source finder\n"
                                                 "# the \"flux\" column is in Jy\n"
                                                 "\n"
                                                 "RA,DEC,flux\n"
                                                 "1,2,7\n"
                                                 "\n"
                                                 "# a note between rows\n"
                                                 "3,4,8\n"));
    const auto r = CatalogueOverlayUtils::parseFile(path);
    QVERIFY2(r.valid, qPrintable(r.errorMessage));
    QCOMPARE(r.entries.size(), std::size_t(2));
    QCOMPARE(r.entries[1].raDeg, 3.0);
    QCOMPARE(r.skippedEntries, 0);   // the comment is not a bad row
    // The quote in the second comment did not swallow the header.
    QCOMPARE(r.extraFields, QStringList{ QStringLiteral("flux") });
}

void TestCatalogueCsv::ds9FilesAreStillRegions()
{
    QTemporaryDir dir;
    // Bare points split on their commas into two fields per line, the shape of
    // a two-column table: they are regions all the same, whatever the name.
    const QString bare = QStringLiteral("fk5;point(10.1,20.2) # text={a}\n"
                                        "fk5;point(10.3,20.4)\n"
                                        "fk5;point(10.5,20.6)\n");
    for (const QString &name : { QStringLiteral("p.reg"), QStringLiteral("p.txt") }) {
        const auto r = CatalogueOverlayUtils::parseFile(writeCsv(dir, name, bare));
        QVERIFY2(r.valid, qPrintable(name + u": "_s + r.errorMessage));
        QCOMPARE(r.frameLabel, QStringLiteral("DS9 regions"));
        QCOMPARE(r.entries.size(), std::size_t(3));
        QCOMPARE(r.entries[0].label, QStringLiteral("a"));
    }
    // The usual ds9 export: format comment, global line, frame on its own.
    const QString exported = QStringLiteral(
            "# Region file format: DS9 version 4.1\n"
            "global color=green dashlist=8 3 width=1 font=\"helvetica 10 normal roman\"\n"
            "fk5\n"
            "circle(10.1,20.2,3\")\n"
            "circle(10.3,20.4,3\")\n");
    const auto r = CatalogueOverlayUtils::parseFile(writeCsv(dir, QStringLiteral("e.txt"),
                                                             exported));
    QVERIFY2(r.valid, qPrintable(r.errorMessage));
    QCOMPARE(r.frameLabel, QStringLiteral("DS9 regions"));
    QCOMPARE(r.entries.size(), std::size_t(2));
    QVERIFY(CatalogueOverlayUtils::looksLikeDs9Regions(exported));
    QVERIFY(!CatalogueOverlayUtils::looksLikeDs9Regions(u"image;ra;dec\na.fits;1;2\n"_s));
}

void TestCatalogueCsv::aCsvOrTsvThatIsNoTableSaysSo()
{
    QTemporaryDir dir;
    for (const QString &name : { QStringLiteral("prose.csv"), QStringLiteral("prose.tsv") }) {
        const auto r = CatalogueOverlayUtils::parseFile(
                writeCsv(dir, name, QStringLiteral("Hello, world.\nNo table here\n")));
        QVERIFY(!r.valid);
        // Not the DS9 reader's "no supported entries": the table reader's own words.
        QVERIFY2(r.errorMessage.contains(QStringLiteral("not a delimited text table")),
                 qPrintable(r.errorMessage));
    }
    const auto empty = CatalogueOverlayUtils::parseFile(
            writeCsv(dir, QStringLiteral("empty.tsv"), QString()));
    QVERIFY(!empty.valid);
    QVERIFY2(empty.errorMessage.contains(QStringLiteral("empty")), qPrintable(empty.errorMessage));
}

// ── `#` lines, decimal commas and the comma fallback (the backend's final rule) ─

void TestCatalogueCsv::aHashLineNamesTheColumnsOfANumericFirstRow()
{
    QTemporaryDir dir;
    // `#ID,RA,DEC`, `# ID, RA, DEC` with a blank line before the data, and the
    // same with tabs: the `#` line is the header, and the first line is data.
    const QStringList spellings = {
        QStringLiteral("#ID,RA,DEC\n1,10,20\n2,11,21\n"),
        QStringLiteral("# ID, RA, DEC\n\n1,10,20\n2,11,21\n"),
        QStringLiteral("## ID\tRA\tDEC\n1\t10\t20\n2\t11\t21\n"),
    };
    int n = 0;
    for (const QString &text : spellings) {
        const QString name = u"hash%1.txt"_s.arg(++n);
        const auto r = CatalogueOverlayUtils::parseFile(writeCsv(dir, name, text));
        QVERIFY2(r.valid, qPrintable(name + u": "_s + r.errorMessage));
        QCOMPARE(r.entries.size(), std::size_t(2));   // the first line is a source
        QCOMPARE(r.entries[0].label, QStringLiteral("1"));
        QCOMPARE(r.entries[0].raDeg, 10.0);
        QCOMPARE(r.entries[1].decDeg, 21.0);
    }
}

void TestCatalogueCsv::aDescriptiveHashLineAboveARealHeaderIsAComment()
{
    QTemporaryDir dir;
    // A title with commas, and a commented-out row of numbers: neither names the
    // columns, because the line below them is the header (no number in it) or
    // they are numbers themselves.
    for (const QString &above : { QStringLiteral("# Sources, v2, J2000"),
                                  QStringLiteral("# 9,99,999") }) {
        const auto r = CatalogueOverlayUtils::parseFile(writeCsv(
                dir, QStringLiteral("desc.csv"), above + u"\nID,RA,DEC\n1,10,20\n"_s));
        QVERIFY2(r.valid, qPrintable(above + u": "_s + r.errorMessage));
        QCOMPARE(r.entries.size(), std::size_t(1));
        QCOMPARE(r.entries[0].label, QStringLiteral("1"));
        QCOMPARE(r.entries[0].raDeg, 10.0);
    }
    // The rule itself: names must be non-empty, not numbers, as many as the
    // first row's fields, and the first row must hold a number.
    using DelimitedText::headerFromComment;
    const QStringList numeric = { u"1"_s, u"10"_s, u"20"_s };
    QCOMPARE(headerFromComment(u"# id, ra, dec", numeric, u','),
             (QStringList{ u"id"_s, u"ra"_s, u"dec"_s }));
    QVERIFY(headerFromComment(u"# id, ra", numeric, u',').isEmpty());
    QVERIFY(headerFromComment(u"# id,,dec", numeric, u',').isEmpty());
    QVERIFY(headerFromComment(u"# id,ra,dec", { u"a"_s, u"b"_s, u"c"_s }, u',').isEmpty());
}

void TestCatalogueCsv::aHashRowWithTheTablesFieldCountIsData()
{
    QTemporaryDir dir;
    const auto r = CatalogueOverlayUtils::parseFile(writeCsv(
            dir, QStringLiteral("hashrow.csv"),
            QStringLiteral("ID,RA,DEC\n#S5,10,20\n# a comment\n2,11,21\n")));
    QVERIFY2(r.valid, qPrintable(r.errorMessage));
    QCOMPARE(r.entries.size(), std::size_t(2));
    QCOMPARE(r.entries[0].label, QStringLiteral("#S5"));   // a source, not a comment
    QCOMPARE(r.entries[1].raDeg, 11.0);
    QCOMPARE(r.skippedEntries, 0);                          // the comment is not a row
}

void TestCatalogueCsv::aHashLineInsideAQuotedFieldIsPartOfIt()
{
    QTemporaryDir dir;
    const auto r = CatalogueOverlayUtils::parseFile(writeCsv(
            dir, QStringLiteral("mlq.csv"),
            QStringLiteral("RA,DEC,note\n1,2,\"first\n# not a comment\"\n3,4,x\n")));
    QVERIFY2(r.valid, qPrintable(r.errorMessage));
    QCOMPARE(r.entries.size(), std::size_t(2));
    QCOMPARE(extraValue(r.entries[0], QStringLiteral("note")),
             QStringLiteral("first\n# not a comment"));
    QCOMPARE(r.skippedEntries, 0);
}

void TestCatalogueCsv::oneCommaListColumnIsNotADecimalComma()
{
    // Two columns of decimal-comma numbers say "decimal comma"; one column of
    // "1,2" alone could be a list of identifiers, and stays text.
    using DelimitedText::usesDecimalComma;
    QVERIFY(usesDecimalComma({ u"a;10,5;20,25"_s, u"b;11;21,5"_s }));
    QVERIFY(!usesDecimalComma({ u"a;1,2;20"_s, u"b;3,4;21"_s }));
    QVERIFY(!usesDecimalComma({ u"a;10,5;20,25"_s, u"b;11.5;21"_s }));   // a decimal point

    QTemporaryDir dir;
    const auto r = CatalogueOverlayUtils::parseFile(writeCsv(
            dir, QStringLiteral("members.txt"),
            QStringLiteral("ID;RA;DEC;members\na;10;20;1,2\nb;11;21;3,4\n")));
    QVERIFY2(r.valid, qPrintable(r.errorMessage));
    QCOMPARE(extraValue(r.entries[0], QStringLiteral("members")), QStringLiteral("1,2"));
}

void TestCatalogueCsv::raggedTrailingCommaAndOneColumnFilesReadAsComma()
{
    QTemporaryDir dir;
    // Ragged rows: no separator is consistent, but the comma reading has
    // numbers, so the file is a table (short rows padded).
    const auto ragged = CatalogueOverlayUtils::parseFile(writeCsv(
            dir, QStringLiteral("ragged.csv"), QStringLiteral("RA,DEC,flux\n1,2\n3,4,5\n")));
    QVERIFY2(ragged.valid, qPrintable(ragged.errorMessage));
    QCOMPARE(ragged.entries.size(), std::size_t(2));
    QCOMPARE(ragged.entries[0].extra.size(), 0);
    QCOMPARE(extraValue(ragged.entries[1], QStringLiteral("flux")), QStringLiteral("5"));

    // A trailing comma on the rows only: the empty last field is dropped, and
    // the columns stay where the header puts them.
    const auto trailing = CatalogueOverlayUtils::parseFile(writeCsv(
            dir, QStringLiteral("trailing.csv"), QStringLiteral("RA,DEC\n1,2,\n3,4,\n")));
    QVERIFY2(trailing.valid, qPrintable(trailing.errorMessage));
    QCOMPARE(trailing.entries.size(), std::size_t(2));
    QCOMPARE(trailing.entries[0].raDeg, 1.0);
    QCOMPARE(trailing.entries[0].decDeg, 2.0);
    // The overlay ignores fields past the header anyway; the reader itself must
    // drop them, as pandas does, for every other user of the rows.
    {
        const QString path = dir.filePath(QStringLiteral("trailing.csv"));
        const auto layout = DelimitedText::detect(path).layout;
        QVERIFY(layout.commaFallback);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QTextStream stream(&file);
        DelimitedText::RecordReader reader(stream, layout);
        QStringList fields;
        bool unterminated = false;
        QVERIFY(reader.header(&fields, &unterminated));
        QVERIFY(reader.next(&fields, &unterminated));
        QCOMPARE(fields, (QStringList{ u"1"_s, u"2"_s }));
    }

    // One column of numbers is a table too (with no DEC, the overlay says so,
    // rather than calling it no table at all).
    const QString one = writeCsv(dir, QStringLiteral("one.csv"), QStringLiteral("RA\n1\n2\n"));
    const auto detection = DelimitedText::detect(one);
    QCOMPARE(detection.layout.separator, QChar(u','));
    QVERIFY(detection.layout.commaFallback);
    const auto r = CatalogueOverlayUtils::parseFile(one);
    QVERIFY(!r.valid);
    QVERIFY2(r.errorMessage.contains(QStringLiteral("requires RA and DEC")),
             qPrintable(r.errorMessage));
}

void TestCatalogueCsv::aByteOrderMarkIsNotPartOfTheFirstLine()
{
    QTemporaryDir dir;
    // A BOM before a comment (the comment is still a comment), and before the
    // header (the first column is still called RA).
    for (const QString &text : { u"﻿# exported\nRA,DEC\n1,2\n"_s,
                                 u"﻿RA,DEC\n1,2\n"_s }) {
        const auto r = CatalogueOverlayUtils::parseFile(
                writeCsv(dir, QStringLiteral("bom.csv"), text));
        QVERIFY2(r.valid, qPrintable(r.errorMessage));
        QCOMPARE(r.entries.size(), std::size_t(1));
        QCOMPARE(r.entries[0].raDeg, 1.0);
    }
    QVERIFY(!DelimitedText::isSignificantLine(u"﻿# note"));
}

// ── Review fixes: region words, csv quoting, speed, per-column commas, line ends ─

void TestCatalogueCsv::columnsNamedLikeRegionWordsAreStillTables()
{
    QTemporaryDir dir;
    // Each of these loaded before the overlays learnt to spot DS9 by content,
    // and must still load: the table is sniffed first, and the region words
    // count only as whole statements (`global ` or `box(`).
    const QList<QPair<QString, QString>> files = {
        { u"gid.csv"_s, u"global_id,RA,DEC\n1,10,20\n2,11,21\n"_s },
        { u"gname.csv"_s, u"name,RA,DEC\nGlobal_1,10,20\nb,11,21\n"_s },
        { u"box.csv"_s, u"Box (arcsec),RA,DEC\n1,10,20\n2,11,21\n"_s },
        { u"line.tsv"_s, u"Line (GHz)\tRA\tDEC\n1.4\t10\t20\n1.5\t11\t21\n"_s },
        { u"word.csv"_s, u"# Region file format notes: none\nRA,DEC\n10,20\n11,21\n"_s },
    };
    for (const auto &[name, text] : files) {
        const auto r = CatalogueOverlayUtils::parseFile(writeCsv(dir, name, text));
        QVERIFY2(r.valid, qPrintable(name + u": "_s + r.errorMessage));
        QCOMPARE(r.frameLabel, QStringLiteral("FK5/ICRS (RA/DEC degrees)"));
        QCOMPARE(r.entries.size(), std::size_t(2));
    }
    using CatalogueOverlayUtils::isDs9Statement;
    QVERIFY(isDs9Statement(u"circle(10,20,3\")"));
    QVERIFY(isDs9Statement(u"fk5;point(10.1,20.2) # text={a}"));
    QVERIFY(isDs9Statement(u"-box(1,2,3,4,0)"));
    QVERIFY(isDs9Statement(u"global color=green"));
    QVERIFY(isDs9Statement(u"fk5"));
    QVERIFY(!isDs9Statement(u"global_id,RA,DEC"));
    QVERIFY(!isDs9Statement(u"Box (arcsec),RA,DEC"));
    QVERIFY(!isDs9Statement(u"Line (GHz)\tRA\tDEC"));
    QVERIFY(!isDs9Statement(u"physical;ra;dec"));
}

void TestCatalogueCsv::aQuoteOpensAFieldOnlyAtItsStart()
{
    using DelimitedText::splitLine;
    // Python's csv rule, which the backend reads with: a quote in the middle of
    // a field is a character; "" escapes inside a quoted field only.
    QCOMPARE(splitLine(u"1,2,3\"", u','), (QStringList{ u"1"_s, u"2"_s, u"3\""_s }));
    QCOMPARE(splitLine(u"1,ab\"c\"d", u','), (QStringList{ u"1"_s, u"ab\"c\"d"_s }));
    QCOMPARE(splitLine(u"\"a\"\"b\",c", u','), (QStringList{ u"a\"b"_s, u"c"_s }));
    QCOMPARE(splitLine(u"# \"a,b\",c", u','), (QStringList{ u"# \"a"_s, u"b\""_s, u"c"_s }));

    QTemporaryDir dir;
    // 3" is three arcseconds, not the start of a field that swallows the file.
    const auto inch = CatalogueOverlayUtils::parseFile(writeCsv(
            dir, QStringLiteral("inch.csv"), QStringLiteral("RA,DEC,size\n1,2,3\"\n4,5,6\n7,8,9\n")));
    QVERIFY2(inch.valid, qPrintable(inch.errorMessage));
    QCOMPARE(inch.entries.size(), std::size_t(3));
    QCOMPARE(extraValue(inch.entries[0], QStringLiteral("size")), QStringLiteral("3\""));
    QVERIFY(inch.errorMessage.isEmpty());   // no "truncated" warning either

    // `# "a,b",c` splits into the table's three fields, so it is a row (as the
    // backend reads it), not a comment; its RA is not a number, so it is skipped.
    const auto hashRow = CatalogueOverlayUtils::parseFile(writeCsv(
            dir, QStringLiteral("hq.csv"), QStringLiteral("ID,RA,DEC\n# \"a,b\",c\n1,2,3\n")));
    QVERIFY2(hashRow.valid, qPrintable(hashRow.errorMessage));
    QCOMPARE(hashRow.entries.size(), std::size_t(1));
    QCOMPARE(hashRow.skippedEntries, 1);
}

void TestCatalogueCsv::aLongFileIsReadInLinearTime()
{
    // The quote state used to be recounted over the whole record at every
    // line: 80 000 rows after a stray quote took seconds. Carried incrementally,
    // 100 000 rows, and one quoted field spanning 50 000 lines, take a fraction
    // of a second; the bound is loose so a slow machine does not fail it.
    QTemporaryDir dir;
    QString rows = u"RA,DEC,size\n1,2,3\"\n"_s;
    for (int i = 0; i < 100000; ++i)
        rows += u"%1,%2,x\n"_s.arg(i % 360).arg(i % 90);
    QString spanning = u"RA,DEC,note\n1,2,\""_s;
    for (int i = 0; i < 50000; ++i)
        spanning += u"line %1\n"_s.arg(i);
    spanning += u"end\"\n3,4,x\n"_s;

    QElapsedTimer timer;
    timer.start();
    const auto r = CatalogueOverlayUtils::parseFile(writeCsv(dir, u"long.csv"_s, rows));
    const auto s = CatalogueOverlayUtils::parseFile(writeCsv(dir, u"span.csv"_s, spanning));
    const qint64 ms = timer.elapsed();
    QVERIFY2(r.valid, qPrintable(r.errorMessage));
    QCOMPARE(r.entries.size(), std::size_t(100001));
    QVERIFY2(s.valid, qPrintable(s.errorMessage));
    QCOMPARE(s.entries.size(), std::size_t(2));
    QVERIFY2(ms < 5000, qPrintable(u"took %1 ms"_s.arg(ms)));
}

void TestCatalogueCsv::decimalCommasAreJudgedPerColumn()
{
    QTemporaryDir dir;
    // A decimal-comma table with a text column: the numbers are read as
    // numbers, the note keeps its "1,2".
    const auto notes = CatalogueOverlayUtils::parseFile(writeCsv(
            dir, QStringLiteral("notes.txt"),
            QStringLiteral("ID;RA;DEC;flux;note\n"
                           "a;10,5;-20,25;3,75;1,2\n"
                           "b;11;-21,5;4;x\n")));
    QVERIFY2(notes.valid, qPrintable(notes.errorMessage));
    QCOMPARE(notes.entries[0].raDeg, 10.5);
    QCOMPARE(extraValue(notes.entries[0], QStringLiteral("flux")), QStringLiteral("3.75"));
    QCOMPARE(extraValue(notes.entries[0], QStringLiteral("note")), QStringLiteral("1,2"));

    // Judged on the sampled rows, then column by column over the whole file
    // with either notation accepted: a later "20.5" and an empty RA do not make
    // RA or DEC text. ID is text and stays so.
    QString text = u"ID;RA;DEC\n"_s;
    for (int i = 0; i < 10; ++i)
        text += u"x%1;%2,5;%3,5\n"_s.arg(i).arg(10 + i).arg(20 + i);
    text += u"k;20.5;30\nl;;31,25\n"_s;
    const auto mixed = CatalogueOverlayUtils::parseFile(
            writeCsv(dir, QStringLiteral("mixed.txt"), text));
    QVERIFY2(mixed.valid, qPrintable(mixed.errorMessage));
    QCOMPARE(mixed.entries.size(), std::size_t(11));   // l has no RA: skipped
    QCOMPARE(mixed.skippedEntries, 1);
    QCOMPARE(mixed.entries[0].raDeg, 10.5);
    QCOMPARE(mixed.entries[0].decDeg, 20.5);
    QCOMPARE(mixed.entries[10].label, QStringLiteral("k"));
    QCOMPARE(mixed.entries[10].raDeg, 20.5);
    QCOMPARE(mixed.entries[10].decDeg, 30.0);

    const QList<QStringList> rows = { { u"a"_s, u"1,5"_s }, { u"b"_s, u"2.5"_s }, { u"c"_s, u""_s } };
    QCOMPARE(DelimitedText::decimalCommaColumns(rows, 2), (QList<bool>{ false, true }));
}

void TestCatalogueCsv::theCommaFallbackTakesNaNAndInfinityAsNumbers()
{
    QTemporaryDir dir;
    // No separator is consistent (ragged rows); the flux column is numbers to
    // pandas, missing values and infinities included, so the file is a table.
    for (const QString &value : { u"NaN"_s, u"inf"_s, u"-Infinity"_s }) {
        const QString path = writeCsv(dir, QStringLiteral("nan.csv"),
                                      u"RA,DEC,flux\nx,y\nq,r,%1\n1,2,3\n"_s.arg(value));
        const auto detection = DelimitedText::detect(path);
        QVERIFY2(detection.layout.commaFallback, qPrintable(value + u": "_s + detection.error));
        const auto r = CatalogueOverlayUtils::parseFile(path);
        QVERIFY2(r.valid, qPrintable(r.errorMessage));
        QCOMPARE(r.entries.size(), std::size_t(1));
    }
    // A column of nothing but missing values holds no number.
    QVERIFY(!DelimitedText::commaReadingHasNumbers({ u"a,b"_s, u"x"_s, u"y,NaN"_s }));
    QVERIFY(DelimitedText::commaReadingHasNumbers({ u"a,b"_s, u"x"_s, u"y,INF"_s }));
}

void TestCatalogueCsv::aLoneCarriageReturnEndsALine()
{
    QTemporaryDir dir;
    // Old Mac line ends, Windows ones, and both mixed: each line is a line.
    for (const QString &text : { u"RA,DEC,name\r1,2,a\r3,4,b\r"_s,
                                 u"RA,DEC,name\r\n1,2,a\r\n3,4,b"_s,
                                 u"RA,DEC,name\r1,2,a\n3,4,b\r\n"_s }) {
        const QString path = dir.filePath(QStringLiteral("cr.csv"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));   // binary: keep the "\r"s as written
        f.write(text.toUtf8());
        f.close();
        const auto r = CatalogueOverlayUtils::parseFile(path);
        QVERIFY2(r.valid, qPrintable(r.errorMessage));
        QCOMPARE(r.entries.size(), std::size_t(2));
        QCOMPARE(r.entries[1].raDeg, 3.0);
        QCOMPARE(r.entries[1].label, QStringLiteral("b"));   // not "b\r3,4,…"
    }
    QCOMPARE(DelimitedText::physicalLines(u"a\rb\r\nc\nd"_s, false),
             (QStringList{ u"a"_s, u"b"_s, u"c"_s, u"d"_s }));
}
