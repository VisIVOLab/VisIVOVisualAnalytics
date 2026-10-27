#include "TestSourceTable.h"

#include "vlkb/SourceTable.h"

#include <QTest>

using namespace Qt::StringLiterals;
namespace vk = visivo::vlkb;

namespace {
// The shape of a VLKB SIAv2 answer, reduced to the fields the parser reads.
QByteArray votable(const QList<QStringList> &rows)
{
    QByteArray x = R"(<?xml version="1.0"?><VOTABLE><RESOURCE><TABLE>
<FIELD name="dataproduct_type"/><FIELD name="obs_collection"/><FIELD name="obs_title"/>
<FIELD name="obs_publisher_did"/><FIELD name="overlapSky"/><DATA><TABLEDATA>)";
    for (const QStringList &r : rows) {
        x += "<TR>";
        for (const QString &c : r)
            x += "<TD>" + c.toUtf8() + "</TD>";
        x += "</TR>";
    }
    x += "</TABLEDATA></DATA></TABLE></RESOURCE></VOTABLE>";
    return x;
}
}   // namespace

void TestSourceTable::parse_readsTheLegacyExample()
{
    const vk::SourceTable t = vk::parseSourceTable("id\tglon\tglat\n1\t30\t0.2\n2\t330\t-0.2\n");
    QVERIFY(t.error.isEmpty());
    QCOMPARE(t.columns, (QStringList{ u"id"_s, u"glon"_s, u"glat"_s }));
    QCOMPARE(t.rows.size(), 2);
    const QList<vk::UserSource> s = vk::pickSources(t, 0, 1, 2);
    QCOMPARE(s.size(), 2);
    QCOMPARE(s[0].id, u"1"_s);
    QCOMPARE(s[1].glon, 330.0);
    QCOMPARE(s[1].glat, -0.2);
}

void TestSourceTable::parse_toleratesCrlfBlankLinesAndRaggedRows()
{
    const vk::SourceTable t =
            vk::parseSourceTable("name\tl\tb\r\n\r\nA\t10\t1\r\nB\t20\r\nC\t30\t3\textra\r\n");
    QVERIFY(t.error.isEmpty());
    QCOMPARE(t.rows.size(), 3);
    QCOMPARE(t.rows[1].size(), 3);          // padded
    QVERIFY(t.rows[1][2].isEmpty());
    QCOMPARE(t.rows[2].size(), 3);          // truncated, and reported
    QCOMPARE(t.warnings.size(), 1);
}

void TestSourceTable::parse_refusesFilesThatAreNotSourceTables()
{
    QVERIFY(!vk::parseSourceTable("").error.isEmpty());
    QVERIFY(!vk::parseSourceTable("# only a comment\n").error.isEmpty());
    QVERIFY(!vk::parseSourceTable("id glon\n1 30\n").error.isEmpty());          // two columns
    QVERIFY(!vk::parseSourceTable("id\tglon\tglat\n").error.isEmpty());          // no rows
}

void TestSourceTable::parse_detectsTheSeparator()
{
    const vk::SourceTable csv = vk::parseSourceTable("# from a catalogue\nid,glon,glat\n\"A 1\",30,0.2\n");
    QVERIFY(csv.error.isEmpty());
    QCOMPARE(csv.delimiter, u"comma"_s);
    QCOMPARE(csv.rows[0][0], u"A 1"_s);
    const vk::SourceTable sp = vk::parseSourceTable("id   glon  glat\nB    330   -0.2\n");
    QCOMPARE(sp.delimiter, u"spaces"_s);
    QCOMPARE(sp.rows[0], (QStringList{ u"B"_s, u"330"_s, u"-0.2"_s }));
    const vk::SourceTable sc = vk::parseSourceTable("id;glon;glat\nC;10;1\n");
    QCOMPARE(sc.delimiter, u"semicolon"_s);
    // Quotes group a cell: separators and doubled quotes inside it are text.
    const vk::SourceTable q =
            vk::parseSourceTable("\"name, full\",glon,glat\n\"Src, \"\"A\"\"\",30,1\n");
    QCOMPARE(q.columns.size(), 3);
    QCOMPARE(q.rows[0], (QStringList{ u"Src, \"A\""_s, u"30"_s, u"1"_s }));
    QCOMPARE(vk::parseSourceTable("id glon glat\n\"Source A\" 30 1\n").rows[0][0], u"Source A"_s);
    // A tab-separated header wins even if a cell contains a comma.
    QCOMPARE(vk::parseSourceTable("id\tglon\tglat\nD,1\t10\t1\n").rows[0][0], u"D,1"_s);
}

void TestSourceTable::guessColumn_findsCommonNames()
{
    const QStringList cols{ u"Designation"_s, u"RA"_s, u"GLON"_s, u"GLAT"_s };
    QCOMPARE(vk::guessColumn(cols, u"id"_s), 0);
    QCOMPARE(vk::guessColumn(cols, u"glon"_s), 2);
    QCOMPARE(vk::guessColumn(cols, u"glat"_s), 3);
    QCOMPARE(vk::guessColumn({ u"x"_s, u"l"_s, u"b"_s }, u"glon"_s), 1);
    QCOMPARE(vk::guessColumn({ u"x"_s }, u"glat"_s), -1);
}

void TestSourceTable::pickSources_skipsBadCoordinatesAndNamesAnonymousRows()
{
    const vk::SourceTable t =
            vk::parseSourceTable("id\tglon\tglat\n\t10\t1\nbad\tabc\t1\npole\t10\t95\nok\t-2\t0\n");
    QStringList problems;
    const QList<vk::UserSource> s = vk::pickSources(t, 0, 1, 2, &problems);
    QCOMPARE(s.size(), 2);
    QCOMPARE(s[0].id, u"row 1"_s);
    QCOMPARE(s[1].id, u"ok"_s);
    QCOMPARE(s[1].row, 3);
    QCOMPARE(problems.size(), 2);
}

void TestSourceTable::parseInventory_keepsCuttableDatasetsOnly()
{
    const QList<vk::InventoryEntry> e = vk::parseInventory(votable({
            { u"image"_s, u"Hi-GAL Tiles"_s, u"Continuum | 500 um"_s, u"ivo://a"_s, u"3"_s },
            { u"cube"_s, u"CHaMP"_s, u"12CO | 1-0"_s, u"ivo://b"_s, u"2"_s },
            { u"image"_s, u"Hi-GAL Tiles"_s, u"Continuum | 500 um"_s, u"ivo://merge"_s, u"0"_s },
            { u"image"_s, u"Hi-GAL Tiles"_s, u"Continuum | 160 um"_s, QString(), u"3"_s },
    }));
    QCOMPARE(e.size(), 2);
    QCOMPARE(e[0].survey, u"Hi-GAL Tiles"_s);
    QCOMPARE(e[0].species, u"Continuum"_s);
    QCOMPARE(e[0].transition, u"500 um"_s);
    QCOMPARE(e[1].type, u"cube"_s);
    QCOMPARE(e[1].overlap, 2);
}

void TestSourceTable::bestEntry_prefersFullOverlap()
{
    const QList<vk::InventoryEntry> e = vk::parseInventory(votable({
            { u"image"_s, u"S"_s, u"Continuum | 70 um"_s, u"ivo://partial"_s, u"2"_s },
            { u"image"_s, u"S"_s, u"Continuum | 70 um"_s, u"ivo://full"_s, u"3"_s },
            { u"image"_s, u"S"_s, u"Continuum | 70 um"_s, u"ivo://later"_s, u"2"_s },
    }));
    vk::InventoryEntry best;
    QVERIFY(vk::bestEntry(e, vk::keyOf(e[0]), &best));
    QCOMPARE(best.ivoID, u"ivo://full"_s);
    QVERIFY(!vk::bestEntry(e, vk::ProductKey{ u"image"_s, u"S"_s, u"Continuum"_s, u"500 um"_s },
                           &best));
}

void TestSourceTable::coverage_comparesWithWhatTheTableFound()
{
    const auto a = vk::parseInventory(votable({
            { u"image"_s, u"S"_s, u"Continuum | 70 um"_s, u"ivo://1"_s, u"3"_s },
            { u"image"_s, u"S"_s, u"Continuum | 500 um"_s, u"ivo://2"_s, u"3"_s },
    }));
    const auto b = vk::parseInventory(votable({
            { u"image"_s, u"S"_s, u"Continuum | 70 um"_s, u"ivo://3"_s, u"3"_s },
    }));
    const QList<vk::ProductKey> all = vk::productsFound({ a, b, {} });
    QCOMPARE(all.size(), 2);
    QString detail;
    QCOMPARE(vk::coverageOf(a, u"image"_s, u"S"_s, all), vk::Coverage::Full);
    QCOMPARE(vk::coverageOf(b, u"image"_s, u"S"_s, all, &detail), vk::Coverage::Partial);
    QVERIFY(detail.contains(u"500 um: no"_s));
    QCOMPARE(vk::coverageOf({}, u"image"_s, u"S"_s, all), vk::Coverage::None);
    QCOMPARE(vk::coverageOf(a, u"cube"_s, u"S"_s, all), vk::Coverage::None);
}
