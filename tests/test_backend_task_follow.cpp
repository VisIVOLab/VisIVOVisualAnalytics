#include "TestBackendTaskFollow.h"

#include "app/BackendClient.h"
#include "app/BackendTaskFollow.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;
using visivo::BackendTaskControl;
using visivo::TaskOutcome;
using visivo::TaskPhase;

namespace {

/// A backend task on a fake clock: sleeping advances the clock, a poll answers
/// what statusAt() says for the current time, a cancel is recorded.
struct FakeBackend
{
    qint64 now{ 0 };
    int polls{ 0 };
    int longestSleep{ 0 };
    QStringList cancelled;            ///< task ids DELETE was sent for
    bool cancelAccepted{ true };
    std::function<BackendTaskStatusResult(qint64 nowMs)> statusAt;
    /// Run inside a poll, e.g. the user pressing Cancel meanwhile.
    std::function<void(int poll)> duringPoll;

    visivo::TaskFollowHooks hooks()
    {
        visivo::TaskFollowHooks h;
        h.poll = [this](const QString &) {
            ++polls;
            if (duringPoll)
                duringPoll(polls);
            return statusAt(now);
        };
        h.cancel = [this](const QString &id, QString &error) {
            cancelled << id;
            if (!cancelAccepted)
                error = u"409 already finished"_s;
            return cancelAccepted;
        };
        h.sleepMs = [this](int ms) {
            now += ms;
            longestSleep = std::max(longestSleep, ms);
        };
        h.nowMs = [this]() { return now; };
        return h;
    }
};

BackendTaskStatusResult status(const QString &state, const QString &error = {})
{
    BackendTaskStatusResult s;
    s.valid = state != u"failed"_s;
    s.status = state;
    s.error = error;
    s.taskId = u"t1"_s;
    if (state == u"completed"_s)
        s.resultObject = QJsonObject{ { u"valid"_s, true }, { u"width"_s, 7 } };
    return s;
}

BackendTaskStatusResult unreadable()
{
    BackendTaskStatusResult s;
    s.error = u"Connection refused"_s;
    return s;
}

BackendTaskCreateResult created(const QString &state = u"running"_s)
{
    BackendTaskCreateResult c;
    c.valid = true;
    c.taskId = u"t1"_s;
    c.status = state;
    c.httpStatus = 200;
    return c;
}

BackendTaskCreateResult refused(int httpStatus, const QString &error)
{
    BackendTaskCreateResult c;
    c.httpStatus = httpStatus;
    c.error = error;
    return c;
}

constexpr qint64 kMinute = 60'000;

} // namespace

void TestBackendTaskFollow::aLongTaskIsFollowedToItsEnd()
{
    // A 25-minute M2 over an SKA-scale cube: the old loop called it "timed
    // out" after 30 s and recomputed it with the sync route.
    FakeBackend backend;
    backend.statusAt = [](qint64 t) { return status(t < 25 * kMinute ? u"running"_s : u"completed"_s); };
    BackendTaskControl control;
    const auto r = visivo::followBackendTask(created(), backend.hooks(), &control);
    QCOMPARE(r.outcome, TaskOutcome::Completed);
    QVERIFY(!r.fallBackToSync());
    QCOMPARE(r.status.resultObject.value(u"width"_s).toInt(), 7);
    QVERIFY(backend.cancelled.isEmpty());
    QVERIFY(backend.now >= 25 * kMinute);
    // Noticed within one back-off ceiling of the end.
    QVERIFY(backend.now < 25 * kMinute + visivo::TaskPollPolicy{}.maxPollMs + 1);
}

void TestBackendTaskFollow::theBackOffStopsAtItsCeiling()
{
    FakeBackend backend;
    backend.statusAt = [](qint64 t) { return status(t < 25 * kMinute ? u"running"_s : u"completed"_s); };
    visivo::TaskPollPolicy policy;
    const auto r = visivo::followBackendTask(created(), backend.hooks(), nullptr, policy);
    QCOMPARE(r.outcome, TaskOutcome::Completed);
    // No sleep is longer than the abort check, so Cancel answers within it ...
    QVERIFY(backend.longestSleep <= policy.abortCheckMs);
    // ... and the backend is asked every few seconds, not four times a second:
    // 25 min at a 3 s ceiling is ~500 polls (the old 250 ms loop: 6000).
    QVERIFY2(backend.polls < 25 * kMinute / policy.maxPollMs + 20,
             qPrintable(QString::number(backend.polls)));
    QCOMPARE(r.polls, backend.polls);
}

void TestBackendTaskFollow::aFailedTaskDoesNotFallBackToSync()
{
    FakeBackend backend;
    backend.statusAt = [](qint64 t) {
        return t < 5000 ? status(u"running"_s) : status(u"failed"_s, u"MemoryError"_s);
    };
    const auto r = visivo::followBackendTask(created(), backend.hooks(), nullptr);
    QCOMPARE(r.outcome, TaskOutcome::Failed);
    QCOMPARE(r.error, u"MemoryError"_s);
    QVERIFY(!r.fallBackToSync());
    QVERIFY(!r.stoppedByUser());
    QVERIFY(backend.cancelled.isEmpty());
}

void TestBackendTaskFollow::aCancelledTaskDoesNotFallBackToSync()
{
    FakeBackend backend;
    backend.statusAt = [](qint64) { return status(u"cancelled"_s); };
    const auto r = visivo::followBackendTask(created(), backend.hooks(), nullptr);
    QCOMPARE(r.outcome, TaskOutcome::Cancelled);
    QVERIFY(!r.error.isEmpty());
    QVERIFY(!r.fallBackToSync());
    QVERIFY(!r.stoppedByUser());   // not by THIS user: reported as an error
}

void TestBackendTaskFollow::aLostTaskIsCancelledAndDoesNotFallBackToSync()
{
    // The backend restarted: the status is unreadable from minute 3 on.
    FakeBackend backend;
    backend.statusAt = [](qint64 t) { return t < 3 * kMinute ? status(u"running"_s) : unreadable(); };
    visivo::TaskPollPolicy policy;
    const auto r = visivo::followBackendTask(created(), backend.hooks(), nullptr, policy);
    QCOMPARE(r.outcome, TaskOutcome::Lost);
    QVERIFY(!r.fallBackToSync());
    QCOMPARE(backend.cancelled, QStringList{ u"t1"_s });
    QVERIFY(r.error.contains(u"Connection refused"_s));
    // Given up after the outage budget, not at the first failed read.
    QVERIFY(backend.now >= 3 * kMinute + policy.lostAfterMs);
    QVERIFY(backend.now < 3 * kMinute + policy.lostAfterMs + 2 * policy.maxPollMs);
}

void TestBackendTaskFollow::aShortOutageIsRidden()
{
    // A minute of unreadable status (tunnel hiccup) in the middle of the run.
    FakeBackend backend;
    backend.statusAt = [](qint64 t) {
        if (t >= 2 * kMinute && t < 3 * kMinute)
            return unreadable();
        return status(t < 6 * kMinute ? u"running"_s : u"completed"_s);
    };
    const auto r = visivo::followBackendTask(created(), backend.hooks(), nullptr);
    QCOMPARE(r.outcome, TaskOutcome::Completed);
    QVERIFY(backend.cancelled.isEmpty());
}

void TestBackendTaskFollow::anAbortSendsOneDeleteAndStopsPolling()
{
    FakeBackend backend;
    backend.statusAt = [](qint64) { return status(u"running"_s); };
    BackendTaskControl control;
    // The user presses Cancel while the fifth status request is out.
    backend.duringPoll = [&control](int poll) {
        if (poll == 5)
            control.requestAbort();
    };
    const auto r = visivo::followBackendTask(created(), backend.hooks(), &control);
    QCOMPARE(r.outcome, TaskOutcome::Aborted);
    QVERIFY(r.stoppedByUser());
    QVERIFY(r.cancelConfirmed);
    QVERIFY(!r.fallBackToSync());
    QCOMPARE(backend.cancelled, QStringList{ u"t1"_s });
    QCOMPARE(backend.polls, 5);   // no status read after the abort
    QCOMPARE(control.currentPhase(), TaskPhase::Cancelling);
}

void TestBackendTaskFollow::anAbortTheBackendRefusesIsReported()
{
    FakeBackend backend;
    backend.cancelAccepted = false;
    backend.statusAt = [](qint64) { return status(u"running"_s); };
    BackendTaskControl control;
    control.requestAbort();   // before the first poll
    const auto r = visivo::followBackendTask(created(), backend.hooks(), &control);
    QCOMPARE(r.outcome, TaskOutcome::Aborted);
    QVERIFY(!r.cancelConfirmed);
    QCOMPARE(backend.polls, 0);
    QCOMPARE(backend.cancelled, QStringList{ u"t1"_s });
    QVERIFY(r.error.contains(u"may still be running"_s));
    QVERIFY(r.error.contains(u"409"_s));
}

void TestBackendTaskFollow::anAbortThatCutsAReadShortIsNotAnOutage()
{
    // A long outage is already under way when the user cancels; the read the
    // abort interrupts comes back unreadable. That is an abort, not a loss.
    FakeBackend backend;
    BackendTaskControl control;
    visivo::TaskPollPolicy policy;
    backend.statusAt = [&](qint64 t) {
        if (t < kMinute)
            return status(u"running"_s);
        if (t >= kMinute + policy.lostAfterMs)   // the read that would say "lost"
            control.requestAbort();   // the read is abandoned
        return unreadable();
    };
    const auto r = visivo::followBackendTask(created(), backend.hooks(), &control, policy);
    QCOMPARE(r.outcome, TaskOutcome::Aborted);
    QVERIFY(r.stoppedByUser());
    QCOMPARE(backend.cancelled, QStringList{ u"t1"_s });
}

void TestBackendTaskFollow::anAbortRacingACompletedAnswerWins()
{
    // The user aborts while the read that finds the task completed is out:
    // the result is not used, and a finished task gets no DELETE.
    FakeBackend backend;
    BackendTaskControl control;
    backend.statusAt = [](qint64) { return status(u"completed"_s); };
    backend.duringPoll = [&control](int) { control.requestAbort(); };
    const auto r = visivo::followBackendTask(created(), backend.hooks(), &control);
    QCOMPARE(r.outcome, TaskOutcome::Aborted);
    QVERIFY(r.stoppedByUser());
    QVERIFY(backend.cancelled.isEmpty());
    QCOMPARE(backend.polls, 1);
}

void TestBackendTaskFollow::taskCreationKeepsTheHttpStatusForTheFallback()
{
    // The fallback to the synchronous route hangs on the HTTP status of the
    // create request: each create call must record it, or an older backend
    // with no task route reads as a refusal and nothing is computed.
    for (const int code : { 404, 405 }) {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, code]() {
            QTcpSocket *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, code]() {
                socket->readAll();
                const QByteArray body = R"({"detail":"Not Found"})";
                socket->write("HTTP/1.1 " + QByteArray::number(code) + " Nope\r\n"
                              "Content-Type: application/json\r\nConnection: close\r\n"
                              "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
                              + body);
                socket->disconnectFromHost();
            });
        });
        const BackendClient client(u"http://127.0.0.1:%1"_s.arg(server.serverPort()));
        const BackendTaskCreateResult pv = client.createPvTask(u"d1"_s, { { 0., 0. }, { 5., 5. } }, 1);
        QCOMPARE(pv.httpStatus, code);
        QVERIFY(visivo::isMissingTaskRoute(pv));
        const BackendTaskCreateResult moment =
                client.createMomentTask(u"d1"_s, 0, 0, 1, false, 0., false);
        QCOMPARE(moment.httpStatus, code);
        QVERIFY(visivo::isMissingTaskRoute(moment));
    }
}

void TestBackendTaskFollow::aMissingTaskRouteFallsBackToSync()
{
    // An older backend: FastAPI's answers for an unknown route.
    for (const int code : { 404, 405 }) {
        FakeBackend backend;
        backend.statusAt = [](qint64) { return status(u"running"_s); };
        BackendTaskControl control;
        const auto r = visivo::followBackendTask(refused(code, u"{\"detail\":\"Not Found\"}"_s),
                                                 backend.hooks(), &control);
        QCOMPARE(r.outcome, TaskOutcome::RouteMissing);
        QVERIFY(r.fallBackToSync());
        QCOMPARE(backend.polls, 0);
        QVERIFY(backend.cancelled.isEmpty());
    }
}

void TestBackendTaskFollow::otherCreationFailuresDoNotFallBack()
{
    // 422 not a cube, 429 busy, 500 crash, 0 unreachable, and a 200 that
    // says valid=false: none of them means "this backend has no task route".
    for (const int code : { 422, 429, 500, 0, 200 }) {
        FakeBackend backend;
        backend.statusAt = [](qint64) { return status(u"running"_s); };
        const auto r = visivo::followBackendTask(refused(code, u"nope"_s), backend.hooks(), nullptr);
        QCOMPARE(r.outcome, TaskOutcome::CreateFailed);
        QVERIFY2(!r.fallBackToSync(), qPrintable(QString::number(code)));
        QCOMPARE(r.error, u"nope"_s);
        QCOMPARE(backend.polls, 0);
    }
}

void TestBackendTaskFollow::aCancelDuringCreationOnAnOldBackendStartsNothing()
{
    FakeBackend backend;
    backend.statusAt = [](qint64) { return status(u"running"_s); };
    BackendTaskControl control;
    control.requestAbort();
    const auto r = visivo::followBackendTask(refused(404, u"Not Found"_s), backend.hooks(), &control);
    QCOMPARE(r.outcome, TaskOutcome::Aborted);
    QVERIFY(!r.fallBackToSync());
    QVERIFY(r.stoppedByUser());
    QVERIFY(backend.cancelled.isEmpty());   // there is no task to cancel
}

void TestBackendTaskFollow::aPendingTaskReportsQueued()
{
    FakeBackend backend;
    BackendTaskControl control;
    TaskPhase seenWhilePending = TaskPhase::Starting;
    TaskPhase seenWhileRunning = TaskPhase::Starting;
    backend.statusAt = [&](qint64 t) {
        if (t < 10'000) {
            seenWhilePending = control.currentPhase();
            return status(u"pending"_s);
        }
        if (t < 20'000) {
            seenWhileRunning = control.currentPhase();
            return status(u"running"_s);
        }
        return status(u"completed"_s);
    };
    const auto r = visivo::followBackendTask(created(u"pending"_s), backend.hooks(), &control);
    QCOMPARE(r.outcome, TaskOutcome::Completed);
    QCOMPARE(seenWhilePending, TaskPhase::Queued);
    // The phase seen at a poll is the one the previous answer set.
    QVERIFY(seenWhileRunning == TaskPhase::Queued || seenWhileRunning == TaskPhase::Running);
    QCOMPARE(control.currentPhase(), TaskPhase::Running);
}

void TestBackendTaskFollow::theProgressTextSaysStateAndTime()
{
    const QString what = u"Computing the moment map"_s;
    const QString running = visivo::describeTaskProgress(what, TaskPhase::Running, 1'392'000);
    QVERIFY(running.startsWith(what + u"…"_s));
    QVERIFY2(running.contains(u"running on the backend"_s), qPrintable(running));
    QVERIFY2(running.contains(u"23 min 12 s elapsed"_s), qPrintable(running));
    QVERIFY(!running.contains(u'%'));   // no fake percentage

    QVERIFY(visivo::describeTaskProgress(what, TaskPhase::Running, 7'500).contains(u"7 s elapsed"_s));
    QVERIFY(visivo::describeTaskProgress(what, TaskPhase::Running, 61'000).contains(u"1 min 01 s"_s));
    QVERIFY(visivo::describeTaskProgress(what, TaskPhase::Queued, 0).contains(u"queued"_s));
    QVERIFY(visivo::describeTaskProgress(what, TaskPhase::Cancelling, 0).contains(u"cancelling"_s));
    QVERIFY(visivo::describeTaskProgress(what, TaskPhase::Synchronous, 0)
                    .contains(u"cannot be cancelled"_s));
}

void TestBackendTaskFollow::aMeasuredProgressIsShownAsAPercentage()
{
    // A decompression counts the compressed bytes it has read: its progress
    // is a measure, and the sheet shows it.
    FakeBackend backend;
    QList<int> seen;
    BackendTaskControl control;
    backend.statusAt = [&](qint64 t) {
        seen << control.currentPercent();
        if (t >= 5'000)
            return status(u"completed"_s);
        BackendTaskStatusResult s = status(u"running"_s);
        s.progress = t / 5'000.0;
        return s;
    };
    visivo::TaskPollPolicy policy;
    policy.progressIsMeasured = true;
    const auto r = visivo::followBackendTask(created(), backend.hooks(), &control, policy);
    QCOMPARE(r.outcome, TaskOutcome::Completed);
    QCOMPARE(seen.first(), -1);   // nothing measured before the first answer
    QVERIFY(std::is_sorted(seen.begin(), seen.end()));
    QVERIFY(seen.last() > 0 && seen.last() < 100);

    QCOMPARE(visivo::taskPercent(0.421), 42);
    QCOMPARE(visivo::taskPercent(1.7), 100);
    QCOMPARE(visivo::taskPercent(-0.2), 0);
    QCOMPARE(visivo::taskPercent(std::nan("")), 0);

    const QString what = u"Decompressing cube%20x.fits.gz"_s;   // a % in the name stays
    const QString text = visivo::describeTaskProgress(what, TaskPhase::Running, 9'000, 42);
    QVERIFY2(text.startsWith(what + u"… 42 %"_s), qPrintable(text));
    QVERIFY2(text.contains(u"9 s elapsed"_s), qPrintable(text));
    // Cancelling is not "42 % done".
    QVERIFY(!visivo::describeTaskProgress(what, TaskPhase::Cancelling, 0, 42).contains(u"42 %"_s));
}

void TestBackendTaskFollow::aTickerProgressIsNeverShownAsAPercentage()
{
    // The moment and PV tasks report a ticker: no percentage unless asked for.
    FakeBackend backend;
    backend.statusAt = [](qint64 t) {
        BackendTaskStatusResult s = status(t < 5'000 ? u"running"_s : u"completed"_s);
        s.progress = 0.5;
        return s;
    };
    BackendTaskControl control;
    QCOMPARE(visivo::followBackendTask(created(), backend.hooks(), &control).outcome,
             TaskOutcome::Completed);
    QCOMPARE(control.currentPercent(), -1);
}

void TestBackendTaskFollow::aCachedDecompressionNeedsNoFollowing()
{
    // A file opened before is answered "completed" by the create request
    // itself: the reopen must not poll (that cost ~1.5 s per reopen).
    BackendTaskCreateResult hit = created(u"completed"_s);
    hit.cacheHit = true;
    QVERIFY(BackendClient::decompressionAlreadyDone(hit));
    QVERIFY(!BackendClient::decompressionAlreadyDone(created(u"running"_s)));
    QVERIFY(!BackendClient::decompressionAlreadyDone(created(u"pending"_s)));
    QVERIFY(!BackendClient::decompressionAlreadyDone(refused(404, u"File not found."_s)));
}

void TestBackendTaskFollow::theSheetShowsOnTimeCountedFromTheRequest()
{
    // The open's health check, classification and create request come before
    // the sheet exists: what they took is taken off its delay, not added.
    QCOMPARE(visivo::progressShowDelayMs(0, 400), 400);
    QCOMPARE(visivo::progressShowDelayMs(150, 400), 250);
    QCOMPARE(visivo::progressShowDelayMs(400, 400), 0);
    QCOMPARE(visivo::progressShowDelayMs(2'000, 400), 0);   // late already: at once
    QCOMPARE(visivo::progressShowDelayMs(-50, 400), 400);   // never more than the delay
    QCOMPARE(visivo::progressShowDelayMs(100, -1), 0);
}
