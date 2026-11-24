#include "TestIsoLevels.h"

#include "BackendClient.h"
#include "gui/IsoLevels.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include <cmath>

using namespace visivo::isolevels;
using namespace Qt::Literals::StringLiterals;

namespace {
void compareValues(const QVector<double> &got, const QVector<double> &want)
{
    QCOMPARE(got.size(), want.size());
    for (int i = 0; i < want.size(); ++i)
        QVERIFY2(std::abs(got[i] - want[i]) <= 1e-12 * std::max(1.0, std::abs(want[i])),
                 qPrintable(u"level %1: %2 != %3"_s.arg(i).arg(got[i]).arg(want[i])));
}
}

void TestIsoLevels::linearLevelsIncludeBothEnds()
{
    compareValues(spaced(5, 1.0, 9.0, Spacing::Linear), { 1, 3, 5, 7, 9 });
}

void TestIsoLevels::logLevelsAreEvenInTheirLogarithm()
{
    compareValues(spaced(4, 1e-3, 1.0, Spacing::Log), { 1e-3, 1e-2, 1e-1, 1.0 });
}

void TestIsoLevels::oneLevelIsTheMiddleOfTheRange()
{
    compareValues(spaced(1, 2.0, 8.0, Spacing::Linear), { 5.0 });
    compareValues(spaced(1, 1.0, 100.0, Spacing::Log), { 10.0 });
}

void TestIsoLevels::aReversedRangeIsReadTheRightWayRound()
{
    compareValues(spaced(3, 9.0, 1.0, Spacing::Linear), { 1, 5, 9 });
}

void TestIsoLevels::logNeedsARangeAboveZero()
{
    QString error;
    QVERIFY(spaced(3, 0.0, 1.0, Spacing::Log, &error).isEmpty());
    QVERIFY(error.contains(u"above zero"_s));
    QVERIFY(spaced(3, -1.0, 1.0, Spacing::Log).isEmpty());
    // Linear spacing may cross zero (a velocity field).
    QCOMPARE(spaced(3, -1.0, 1.0, Spacing::Linear).size(), 3);
}

void TestIsoLevels::moreThanTheMaximumIsRefused()
{
    QVERIFY(spaced(kMaxLevels + 1, 1.0, 2.0, Spacing::Linear).isEmpty());
    QVERIFY(spaced(0, 1.0, 2.0, Spacing::Linear).isEmpty());
    QCOMPARE(spaced(kMaxLevels, 1.0, 2.0, Spacing::Linear).size(), kMaxLevels);
}

void TestIsoLevels::aTypedListIsSortedWithoutDuplicates()
{
    compareValues(parseList(u"1, 0.01; 0.1  1"_s), { 0.01, 0.1, 1.0 });
    compareValues(parseList(u"2e-3,5E-4"_s), { 5e-4, 2e-3 });
}

void TestIsoLevels::aTypedListRefusesWhatItCannotRead()
{
    QString error;
    QVERIFY(parseList(u"1, two, 3"_s, &error).isEmpty());
    QVERIFY(error.contains(u"two"_s));
    QVERIFY(parseList(u"  "_s, &error).isEmpty());
    QVERIFY(parseList(u"nan"_s, &error).isEmpty());
    QVERIFY(parseList(u"1 2 3 4 5 6 7 8 9"_s, &error).isEmpty());
    QVERIFY(error.contains(QString::number(kMaxLevels)));
}

void TestIsoLevels::opacityFallsOutwardFromAnOpaqueInnerSurface()
{
    // Rank 0 is the lowest value, the outermost surface of an emission peak.
    QCOMPARE(defaultOpacity(0, 1), 1.0);
    QCOMPARE(defaultOpacity(0, 4), kOuterOpacity);
    QCOMPARE(defaultOpacity(3, 4), 1.0);
    for (int i = 1; i < 4; ++i)
        QVERIFY(defaultOpacity(i, 4) > defaultOpacity(i - 1, 4));
}

void TestIsoLevels::colourComesFromTheMapAtEachValue()
{
    const auto levels = makeLevels({ 1.0, 2.0, 3.0 }, [](double v) {
        return QColor(static_cast<int>(v * 10), 0, 0);
    });
    QCOMPARE(levels.size(), 3);
    QCOMPARE(levels[1].colour, QColor(20, 0, 0));
    QCOMPARE(levels[2].opacity, 1.0);
    QCOMPARE(levels[0].opacity, kOuterOpacity);
    compareValues(values(levels), { 1.0, 2.0, 3.0 });
}

void TestIsoLevels::regeneratingKeepsTheColoursNotTheOpacities()
{
    auto first = makeLevels({ 1.0, 2.0, 3.0 }, [](double) { return QColor(Qt::red); });
    first[1].colour = QColor(Qt::green);
    first[1].opacity = 0.5;
    const auto regenerated = keepEdits(
            makeLevels({ 2.0, 4.0 }, [](double) { return QColor(Qt::blue); }), first);
    QCOMPARE(regenerated[0].colour, QColor(Qt::green));   // same value: kept
    QCOMPARE(regenerated[0].opacity, kOuterOpacity);      // the new set's own gradient
    QCOMPARE(regenerated[1].colour, QColor(Qt::blue));    // new value: the map's
}

void TestIsoLevels::aSimulationRangeStartsLog()
{
    // A density over many decades: log; a radio cube's few-sigma range: linear.
    const Range density = startingRange(1e-12, 2e-5);
    QCOMPARE(density.spacing, Spacing::Log);
    QVERIFY(density.min > 0 && density.max < 2e-5);
    const Range radio = startingRange(0.002, 0.04);
    QCOMPARE(radio.spacing, Spacing::Linear);
    // A threshold at or above the maximum, or no maximum: still a usable range.
    QVERIFY(startingRange(5.0, 1.0).min < startingRange(5.0, 1.0).max);
    QVERIFY(startingRange(1.0, std::nan("")).min < startingRange(1.0, std::nan("")).max);
}

void TestIsoLevels::theAnswerKeepsOneMeshPerLevel()
{
    const auto surface = [](double value, int polys) {
        return QJsonObject{ { u"value"_s, value },
                            { u"compression"_s, u""_s },
                            { u"points_base64"_s, QString::fromLatin1(QByteArray(12, 'a').toBase64()) },
                            { u"polys_base64"_s, QString::fromLatin1(QByteArray(16, 'b').toBase64()) },
                            { u"num_points"_s, 1 },
                            { u"num_polys"_s, polys } };
    };
    const QJsonObject answer{
        { u"valid"_s, true },
        { u"error"_s, u""_s },
        { u"compression"_s, u""_s },
        { u"points_base64"_s, QString::fromLatin1(QByteArray(12, 'a').toBase64()) },
        { u"polys_base64"_s, QString::fromLatin1(QByteArray(16, 'b').toBase64()) },
        { u"num_points"_s, 1 },
        { u"num_polys"_s, 1 },
        { u"levels"_s, QJsonArray{ surface(0.5, 1), surface(2.0, 3) } },
        { u"skipped_levels"_s, QJsonArray{ 9.0 } },
        { u"stride"_s, QJsonArray{ 1, 2, 2 } },
    };
    const BackendIsosurfaceResult r = BackendClient::parseIsosurfaceResultObject(answer);
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.surfaces.size(), 2);
    QCOMPARE(r.surfaces[0].value, 0.5);
    QCOMPARE(r.surfaces[1].value, 2.0);
    QCOMPARE(r.surfaces[1].numPolys, 3);
    QCOMPARE(r.surfaces[1].pointsData.size(), 12);
    QCOMPARE(r.skippedLevels, QVector<double>{ 9.0 });
    QCOMPARE(r.stride, (QVector<int>{ 1, 2, 2 }));
}

void TestIsoLevels::anOlderAnswerHasOnlyTheFirstSurface()
{
    const QJsonObject answer{
        { u"valid"_s, true },
        { u"error"_s, u""_s },
        { u"compression"_s, u""_s },
        { u"points_base64"_s, QString::fromLatin1(QByteArray(12, 'a').toBase64()) },
        { u"polys_base64"_s, QString::fromLatin1(QByteArray(16, 'b').toBase64()) },
        { u"num_points"_s, 1 },
        { u"num_polys"_s, 1 },
    };
    const BackendIsosurfaceResult r = BackendClient::parseIsosurfaceResultObject(answer);
    QVERIFY(r.valid);
    QVERIFY(r.surfaces.isEmpty());
    QCOMPARE(r.pointsData.size(), 12);
    QVERIFY(r.skippedLevels.isEmpty());
}
