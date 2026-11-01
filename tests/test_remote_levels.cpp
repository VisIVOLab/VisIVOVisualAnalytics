// test_remote_levels.cpp
// See TestRemoteLevels.h.

#include "TestRemoteLevels.h"

#include "RemoteLevels.h"

#include <QJsonDocument>
#include <QTest>

using visivo::parseRemoteLevels;
using visivo::ThresholdBounds;

namespace {
QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}
} // namespace

void TestRemoteLevels::createSessionAnswerIsRead()
{
    const auto l = parseRemoteLevels(json(R"({"valid": true, "session_id": "s",
        "threshold": 2.5, "data_min": -1.0, "data_max": 40.0, "levels_pending": false})"));
    QVERIFY(l.threshold && l.dataMin && l.dataMax);
    QCOMPARE(*l.threshold, 2.5);
    QCOMPARE(*l.dataMin, -1.0);
    QCOMPARE(*l.dataMax, 40.0);
    QVERIFY(!l.pending);
}

void TestRemoteLevels::levelsAnswerIsRead()
{
    const auto l = parseRemoteLevels(json(R"({"threshold": 3, "data_min": 0,
        "data_max": 9, "pending": true})"));
    QCOMPARE(*l.threshold, 3.0);
    QVERIFY(l.pending);
}

void TestRemoteLevels::unknownValuesStayUnknown()
{
    // A cube still streaming in: nothing known yet, ask again.
    auto l = parseRemoteLevels(json(R"({"threshold": null, "data_min": null,
        "data_max": null, "levels_pending": true})"));
    QVERIFY(!l.threshold && !l.dataMin && !l.dataMax);
    QVERIFY(l.pending);
    // An older backend says nothing: nothing to adopt, nothing to ask.
    l = parseRemoteLevels(json(R"({"valid": true, "session_id": "s"})"));
    QVERIFY(!l.threshold && !l.dataMin && !l.pending);
    // A flat cube has no range to put the slider on.
    l = parseRemoteLevels(json(R"({"threshold": 1, "data_min": 1, "data_max": 1})"));
    QVERIFY(l.threshold && !l.dataMin && !l.dataMax);
}

void TestRemoteLevels::theRemoteRangeWidensThePreviewsBounds()
{
    const ThresholdBounds remote = visivo::boundsFromRange(-10.0, 90.0);
    QCOMPARE(remote.lower, -7.f);
    QCOMPARE(remote.upper, 90.f);
    // The averaged preview is narrower than the full-resolution cube.
    const ThresholdBounds b = visivo::unite({ -2.f, 30.f }, remote);
    QCOMPARE(b.lower, -7.f);
    QCOMPARE(b.upper, 90.f);
}

void TestRemoteLevels::theBackendsThresholdIsNeverClamped()
{
    // p99 of full-resolution noise can lie above an averaged preview's maximum.
    const ThresholdBounds b = visivo::including({ -0.5f, 2.f }, 2.3f);
    QCOMPARE(b.lower, -0.5f);
    QCOMPARE(b.upper, 2.3f);
    QCOMPARE(visivo::including({ 0.f, 1.f }, -4.f).lower, -4.f);
}

void TestRemoteLevels::sliderPositionStaysInRange()
{
    QCOMPARE(visivo::thresholdSliderPosition({ 0.f, 10.f }, 2.5f), 25);
    QCOMPARE(visivo::thresholdSliderPosition({ 0.f, 10.f }, 20.f), 100);
    QCOMPARE(visivo::thresholdSliderPosition({ 0.f, 10.f }, -3.f), 0);
    QCOMPARE(visivo::thresholdSliderPosition({ 5.f, 5.f }, 5.f), 0);
}
