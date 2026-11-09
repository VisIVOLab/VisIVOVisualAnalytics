#include "TestOpenJobMerge.h"

#include "gui/OpenJobMerge.h"

#include <QtTest>

using namespace Qt::Literals::StringLiterals;
using visivo::ClientOpen;
using visivo::HubJobRow;

namespace {

constexpr qint64 kNow = 1'000'000'000;   // when the backend's answer arrived (ms)

BackendRecentTask job(const QString &op, const QString &status, double seconds, double ageSeconds)
{
    BackendRecentTask t;
    t.operation = op;
    t.status = status;
    t.durationSeconds = seconds;
    t.ageSeconds = ageSeconds;
    return t;
}

ClientOpen open(const QString &path, const QString &status, qint64 requested, qint64 finished,
                bool reached)
{
    return { path, status, requested, finished, reached };
}

}   // namespace

void TestOpenJobMerge::anOpenTakesTheClientsTime()
{
    // The user asked 2.4 s before the backend's refusal (1.6 ms), shown 0.1 s later.
    const auto rows = visivo::mergeOpenJobs(
            { job(u"datasets/open"_s, u"failed"_s, 0.0016, 3.0) },
            { open(u"/d/trunc.fits"_s, u"failed"_s, kNow - 5400, kNow - 2900, true) }, kNow);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].operation, u"datasets/open"_s);
    QVERIFY(rows[0].clientMeasured);
    QCOMPARE(rows[0].durationSeconds, 2.5);
    QCOMPARE(rows[0].backendSeconds, 0.0016);
    QCOMPARE(rows[0].status, u"failed"_s);
    QCOMPARE(rows[0].path, u"/d/trunc.fits"_s);
}

void TestOpenJobMerge::aRefusalBeforeTheBackendIsListed()
{
    const auto rows = visivo::mergeOpenJobs(
            { job(u"products/moment"_s, u"done"_s, 4.0, 30.0) },
            { open(u"/d/missing.fits"_s, u"failed"_s, kNow - 1200, kNow - 1000, false) }, kNow);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].operation, u"open missing.fits"_s);   // newest first
    QCOMPARE(rows[0].status, u"failed"_s);
    QCOMPARE(rows[0].durationSeconds, 0.2);
    QCOMPARE(rows[0].ageSeconds, 1.0);
    QVERIFY(rows[0].clientMeasured);
    QCOMPARE(rows[0].backendSeconds, -1.0);
    QCOMPARE(rows[1].operation, u"products/moment"_s);
    QVERIFY(!rows[1].clientMeasured);
}

void TestOpenJobMerge::theOpenRouteIsPreferredOverTheDecompression()
{
    // A .fits.gz: the decompression task, then the open of the copy.
    const auto rows = visivo::mergeOpenJobs(
            { job(u"datasets/open"_s, u"done"_s, 0.2, 1.0),
              job(u"decompress"_s, u"completed"_s, 8.0, 1.3) },
            { open(u"/d/big.fits.gz"_s, u"done"_s, kNow - 10'000, kNow - 800, true) }, kNow);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].operation, u"datasets/open"_s);
    QVERIFY(rows[0].clientMeasured);
    QCOMPARE(rows[0].durationSeconds, 9.2);
    QCOMPARE(rows[1].operation, u"decompress"_s);   // a job of its own, as the backend has it
    QVERIFY(!rows[1].clientMeasured);
    QCOMPARE(rows[1].durationSeconds, 8.0);
}

void TestOpenJobMerge::aCancelledDecompressionMatchesItsTask()
{
    const auto rows = visivo::mergeOpenJobs(
            { job(u"decompress"_s, u"cancelled"_s, 3.0, 2.0) },
            { open(u"/d/big.fits.gz"_s, u"cancelled"_s, kNow - 6000, kNow - 1900, true) }, kNow);
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows[0].clientMeasured);
    QCOMPARE(rows[0].status, u"cancelled"_s);
    QCOMPARE(rows[0].durationSeconds, 4.1);
}

void TestOpenJobMerge::twoOpensClaimTheirOwnJobs()
{
    // Back to back, the second asked 0.5 s after the first ended: inside the
    // first one's slack, so each must take the job closest to its own end.
    const auto rows = visivo::mergeOpenJobs(
            { job(u"datasets/open"_s, u"failed"_s, 0.002, 2.0),
              job(u"datasets/open"_s, u"failed"_s, 0.002, 1.0) },
            { open(u"/d/a.fits"_s, u"failed"_s, kNow - 3000, kNow - 1950, true),
              open(u"/d/b.fits"_s, u"failed"_s, kNow - 1450, kNow - 950, true) },
            kNow);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].path, u"/d/b.fits"_s);
    QCOMPARE(rows[0].durationSeconds, 0.5);
    QCOMPARE(rows[1].path, u"/d/a.fits"_s);
    QCOMPARE(rows[1].durationSeconds, 1.05);
}

void TestOpenJobMerge::anOpenWithoutItsBackendJobIsNotInvented()
{
    // Its job fell out of the backend's five: nothing is added in its place.
    const auto rows = visivo::mergeOpenJobs(
            { job(u"products/moment"_s, u"done"_s, 1.0, 5.0) },
            { open(u"/d/old.fits"_s, u"done"_s, kNow - 120'000, kNow - 119'000, true) }, kNow);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].operation, u"products/moment"_s);
}

void TestOpenJobMerge::otherJobsAreLeftAlone()
{
    // A moment computed while a file was opening is not the open.
    const auto rows = visivo::mergeOpenJobs(
            { job(u"products/moment"_s, u"done"_s, 3.0, 1.5) },
            { open(u"/d/x.fits"_s, u"failed"_s, kNow - 4000, kNow - 1000, true) }, kNow);
    QCOMPARE(rows.size(), 1);
    QVERIFY(!rows[0].clientMeasured);
    QCOMPARE(rows[0].durationSeconds, 3.0);
}

void TestOpenJobMerge::timeOrderAndCap()
{
    std::vector<BackendRecentTask> backend;
    for (int i = 0; i < 5; ++i)
        backend.push_back(job(u"products/moment"_s, u"done"_s, 1.0, 10.0 + 10.0 * i));
    const auto rows = visivo::mergeOpenJobs(
            backend,
            { open(u"/d/new.fits"_s, u"failed"_s, kNow - 600, kNow - 500, false),
              open(u"/d/mid.fits"_s, u"failed"_s, kNow - 25'100, kNow - 25'000, false) },
            kNow);
    QCOMPARE(rows.size(), 5);
    QCOMPARE(rows[0].operation, u"open new.fits"_s);
    QCOMPARE(rows[1].ageSeconds, 10.0);
    QCOMPARE(rows[2].ageSeconds, 20.0);
    QCOMPARE(rows[3].operation, u"open mid.fits"_s);
    QCOMPARE(rows[4].ageSeconds, 30.0);
}

void TestOpenJobMerge::anOpenAfterTheAnswerWaitsForTheNext()
{
    // Recorded after the health request left: the backend's answer cannot hold it yet.
    const auto rows = visivo::mergeOpenJobs(
            {}, { open(u"/d/late.fits"_s, u"failed"_s, kNow + 1000, kNow + 3000, true) }, kNow);
    QVERIFY(rows.isEmpty());
}

void TestOpenJobMerge::anEarlierOpenDoesNotTakeTheNextOnesJob()
{
    // A ended 2 s ago and its job is gone from the list; B ran from -1 s to
    // -0.5 s and its job finished at -0.6 s, inside A's slack too.
    const auto rows = visivo::mergeOpenJobs(
            { job(u"datasets/open"_s, u"done"_s, 0.1, 0.6) },
            { open(u"/d/a.fits"_s, u"failed"_s, kNow - 3000, kNow - 2000, true),
              open(u"/d/b.fits"_s, u"done"_s, kNow - 1000, kNow - 500, true) },
            kNow);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].path, u"/d/b.fits"_s);
    QCOMPARE(rows[0].status, u"done"_s);
    QCOMPARE(rows[0].durationSeconds, 0.5);
}

void TestOpenJobMerge::aMatchedOpenIsOrderedByItsOwnEnd()
{
    // The backend answered 20 s ago; the viewer came up 1 s ago (a long load
    // after the open): the open is newer than a moment finished 5 s ago.
    const auto rows = visivo::mergeOpenJobs(
            { job(u"datasets/open"_s, u"done"_s, 0.1, 20.0),
              job(u"products/moment"_s, u"done"_s, 3.0, 5.0) },
            { open(u"/d/recent.fits"_s, u"done"_s, kNow - 25'000, kNow - 1000, true) }, kNow);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].path, u"/d/recent.fits"_s);
    QCOMPARE(rows[0].ageSeconds, 1.0);
    QCOMPARE(rows[1].operation, u"products/moment"_s);
}
