#include "TestUpdateCheck.h"

#include "UpdateCheck.h"

#include <QTest>

using namespace Qt::StringLiterals;
namespace upd = visivo::update;

void TestUpdateCheck::parse_readsTagAndReleasePage()
{
    const QByteArray json = R"({"tag_name": "v1.0.5", "draft": false, "prerelease": false,
        "html_url": "https://github.com/VisIVOLab/VisIVOVisualAnalytics/releases/tag/1.0.5"})";
    const upd::LatestRelease r = upd::parseLatestRelease(json);
    QVERIFY(r.valid);
    QCOMPARE(r.version, u"1.0.5"_s);
    QCOMPARE(r.pageUrl.toString(),
             u"https://github.com/VisIVOLab/VisIVOVisualAnalytics/releases/tag/1.0.5"_s);
}

void TestUpdateCheck::parse_refusesDraftsPrereleasesAndNonVersionTags()
{
    QVERIFY(!upd::parseLatestRelease(R"({"tag_name": "1.0.6", "draft": true})").valid);
    QVERIFY(!upd::parseLatestRelease(R"({"tag_name": "1.0.6", "prerelease": true})").valid);
    QVERIFY(!upd::parseLatestRelease(R"({"tag_name": "nightly"})").valid);
    QVERIFY(!upd::parseLatestRelease(R"({"tag_name": "1.0.6-rc1"})").valid);
    QVERIFY(!upd::parseLatestRelease(R"({"message": "Not Found"})").valid);
}

void TestUpdateCheck::parse_refusesBrokenJson()
{
    const upd::LatestRelease r = upd::parseLatestRelease("<html>rate limited</html>");
    QVERIFY(!r.valid);
    QVERIFY(!r.error.isEmpty());
}

void TestUpdateCheck::parse_neverFollowsAForeignPageUrl()
{
    for (const char *url : { "https://example.org/VisIVOLab/VisIVOVisualAnalytics/releases/x",
                             "http://github.com/VisIVOLab/VisIVOVisualAnalytics/releases/x",
                             "https://github.com/someone/else/releases/x",
                             "https://github.com/VisIVOLab/VisIVOVisualAnalytics/../../attacker/other/releases/tag/1.0.5",
                             "https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/1",
                             "https://github.com:8443/VisIVOLab/VisIVOVisualAnalytics/releases/tag/1.0.5",
                             "" }) {
        const QByteArray json = QByteArray(R"({"tag_name": "1.0.5", "html_url": ")") + url + "\"}";
        const upd::LatestRelease r = upd::parseLatestRelease(json);
        QVERIFY(r.valid);
        QCOMPARE(r.pageUrl, upd::releasesPageUrl());
    }
}

void TestUpdateCheck::isNewer_comparesNumericallyNotAlphabetically()
{
    QVERIFY(upd::isNewer(u"1.0.5"_s, u"1.0.4"_s));
    QVERIFY(upd::isNewer(u"1.0.10"_s, u"1.0.9"_s));
    QVERIFY(upd::isNewer(u"1.1"_s, u"1.0.9"_s));
    QVERIFY(upd::isNewer(u"v2.0.0"_s, u"1.9.9"_s));
    QVERIFY(!upd::isNewer(u"1.0.4"_s, u"1.0.4"_s));
    QVERIFY(!upd::isNewer(u"1.0.4"_s, u"1.0.5"_s));
    QVERIFY(!upd::isNewer(u"1.0"_s, u"1.0.0"_s));
}

void TestUpdateCheck::isNewer_staysQuietForBuildsItCannotPlace()
{
    QVERIFY(!upd::isNewer(u"1.0.5"_s, u"1.0.5-dev"_s));
    QVERIFY(!upd::isNewer(u"1.0.5"_s, QString()));
    QVERIFY(!upd::isNewer(u"latest"_s, u"1.0.4"_s));
    QVERIFY(upd::isComparableVersion(u"v1.0.4"_s));
    QVERIFY(!upd::isComparableVersion(u"1.0.4-dev"_s));
}

void TestUpdateCheck::isCheckDue_runsAtMostOncePerInterval()
{
    const QDateTime now = QDateTime(QDate(2026, 10, 26), QTime(12, 0), QTimeZone::UTC);
    QVERIFY(upd::isCheckDue(QDateTime(), now));
    QVERIFY(!upd::isCheckDue(now.addSecs(-3600), now));
    QVERIFY(upd::isCheckDue(now.addSecs(-24 * 3600), now));
    QVERIFY(upd::isCheckDue(now.addSecs(3600), now));   // clock moved back
}
