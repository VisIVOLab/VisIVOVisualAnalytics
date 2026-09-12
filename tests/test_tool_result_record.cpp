// test_tool_result_record.cpp
// The record is the one description a result window is built from — shown once
// by the tool, stored on the product, replayed when the user opens its Session
// Data row. These tests pin the shape, because the shape is what can be wrong
// without failing to compile: it was, and the symptom was a window with a title,
// a subtitle and an empty body.

#include "TestToolResultRecord.h"

#include "ToolResultRecord.h"

#include <QTest>

using visivo::ResultRow;
using visivo::ResultSection;

void TestToolResultRecord::roundTrip_keepsEveryRowAsALabelValuePair()
{
    const QList<ResultSection> sections{
        { QStringLiteral("Sigma"),
          { { QStringLiteral("Mean"), QStringLiteral("0.0034779168") },
            { QStringLiteral("Median"), QStringLiteral("0.0034984828") } } },
        { QStringLiteral("MAD"),
          { { QStringLiteral("Mean"), QStringLiteral("0.0023458227") } } },
    };
    const QVariantMap record =
            visivo::encodeResultRecord(QStringLiteral("Noise Estimation"),
                                       QStringLiteral("Channels 0..397"), sections);
    const QList<ResultSection> back = visivo::decodeResultSections(record);

    QCOMPARE(back.size(), 2);
    QCOMPARE(back.at(0).first, QStringLiteral("Sigma"));
    QCOMPARE(back.at(0).second.size(), 2);
    QCOMPARE(back.at(0).second.at(0).first, QStringLiteral("Mean"));
    QCOMPARE(back.at(0).second.at(0).second, QStringLiteral("0.0034779168"));
    QCOMPARE(back.at(1).first, QStringLiteral("MAD"));
    QCOMPARE(back.at(1).second.size(), 1);
}

void TestToolResultRecord::roundTrip_keepsSectionAndRowOrder()
{
    // A QVariantMap would sort these; "max, mean, median, min" is not how a
    // summary reads, which is why rows travel as an ordered list.
    const QList<ResultSection> sections{
        { QStringLiteral("Zebra"), { { QStringLiteral("Mean"), QStringLiteral("1") },
                                     { QStringLiteral("Median"), QStringLiteral("2") },
                                     { QStringLiteral("Min"), QStringLiteral("3") },
                                     { QStringLiteral("Max"), QStringLiteral("4") } } },
        { QStringLiteral("Alpha"), { { QStringLiteral("Only"), QStringLiteral("5") } } },
    };
    const QList<ResultSection> back = visivo::decodeResultSections(
            visivo::encodeResultRecord(QStringLiteral("t"), QString(), sections));

    QCOMPARE(back.at(0).first, QStringLiteral("Zebra"));   // not alphabetised
    QCOMPARE(back.at(1).first, QStringLiteral("Alpha"));
    QStringList labels;
    for (const ResultRow &r : back.at(0).second)
        labels << r.first;
    QCOMPARE(labels, (QStringList{ QStringLiteral("Mean"), QStringLiteral("Median"),
                                   QStringLiteral("Min"), QStringLiteral("Max") }));
}

void TestToolResultRecord::rowsAreNotFlattened_theBugThatEmptiedTheWindow()
{
    // The encoder appended each row as a bare QVariantList, and
    // QList::append(const QList<T> &) CONCATENATES: every row became two loose
    // strings, the decoder found no pairs, and each section came back with no
    // rows. Assert the encoded structure directly, not just the round trip —
    // a round trip through a matching bug on both sides would pass.
    const QVariantMap record = visivo::encodeResultRecord(
            QStringLiteral("t"), QString(),
            { { QStringLiteral("S"), { { QStringLiteral("a"), QStringLiteral("1") },
                                       { QStringLiteral("b"), QStringLiteral("2") } } } });
    const QVariantList sections = record.value(QStringLiteral("sections")).toList();
    QCOMPARE(sections.size(), 1);
    const QVariantList rows = sections.at(0).toMap().value(QStringLiteral("rows")).toList();
    QCOMPARE(rows.size(), 2);                    // two ROWS, not four strings
    QCOMPARE(rows.at(0).toList().size(), 2);     // each one still a pair
    QCOMPARE(rows.at(0).toList().at(0).toString(), QStringLiteral("a"));
    QCOMPARE(rows.at(1).toList().at(1).toString(), QStringLiteral("2"));
}

void TestToolResultRecord::emptyRecord_decodesToNothingRatherThanCrashing()
{
    QVERIFY(visivo::decodeResultSections(QVariantMap{}).isEmpty());
    QVERIFY(visivo::recordTitle(QVariantMap{}).isEmpty());
    // A section with no rows is legal: the window shows its heading.
    const QVariantMap record = visivo::encodeResultRecord(
            QStringLiteral("t"), QString(), { { QStringLiteral("Empty"), {} } });
    const QList<ResultSection> back = visivo::decodeResultSections(record);
    QCOMPARE(back.size(), 1);
    QVERIFY(back.at(0).second.isEmpty());
}

void TestToolResultRecord::malformedRows_areSkippedNotGuessedAt()
{
    // A record can come from a product registered by an older build.
    QVariantMap record;
    record.insert(QStringLiteral("sections"),
                  QVariantList{ QVariantMap{
                          { QStringLiteral("title"), QStringLiteral("S") },
                          { QStringLiteral("rows"),
                            QVariantList{ QVariant(QVariantList{ QStringLiteral("ok"),
                                                                 QStringLiteral("1") }),
                                          QVariant(QStringLiteral("loose string")),
                                          QVariant(QVariantList{ QStringLiteral("three"),
                                                                 QStringLiteral("too"),
                                                                 QStringLiteral("many") }) } } } });
    const QList<ResultSection> back = visivo::decodeResultSections(record);
    QCOMPARE(back.size(), 1);
    QCOMPARE(back.at(0).second.size(), 1);
    QCOMPARE(back.at(0).second.at(0).first, QStringLiteral("ok"));
}

void TestToolResultRecord::titleAndSubtitle_surviveTheRoundTrip()
{
    const QVariantMap record = visivo::encodeResultRecord(
            QStringLiteral("Region Analysis"),
            QStringLiteral("Box · computed on the currently loaded data block"), {});
    QCOMPARE(visivo::recordTitle(record), QStringLiteral("Region Analysis"));
    QCOMPARE(visivo::recordSubtitle(record),
             QStringLiteral("Box · computed on the currently loaded data block"));
}
