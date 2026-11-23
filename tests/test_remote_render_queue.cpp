// test_remote_render_queue.cpp
// See TestRemoteRenderQueue.h. The answers are those of
// docs/technical/backend-api/render.md ("The queue").

#include "TestRemoteRenderQueue.h"

#include "RemoteRenderQueue.h"
#include "RemoteSessionRecovery.h"

#include <QJsonDocument>
#include <QTest>
#include <QThread>

using visivo::parseRenderSessionAnswer;
using visivo::RenderQueuePlace;
using visivo::RenderSessionAnswer;
using Kind = visivo::RenderSessionAnswerKind;
using Step = visivo::RenderQueuePlace::Step;

namespace {

QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

RenderSessionAnswer session(const char *sid = "9f1c")
{
    return parseRenderSessionAnswer(
            200, false,
            json(QByteArray(R"({"valid": true, "error": "", "session_id": ")") + sid
                 + R"(", "width": 1920, "height": 1080})"));
}

RenderSessionAnswer queued(const char *id, int position = 2, int length = 3)
{
    const QByteArray body = QByteArray(R"({"valid": false, "queued": true, "queue_id": ")") + id
            + R"(", "position": )" + QByteArray::number(position)
            + R"(, "queue_length": )" + QByteArray::number(length)
            + R"(, "active_sessions": 8, "capacity": 8, "waited_seconds": 12.4,
               "wait_timeout_seconds": 600.0, "poll_after_seconds": 2.0,
               "expires_after_seconds": 15.0,
               "error": "All 8 server-side rendering slots are busy: number 2 of 3 in the queue."})";
    return parseRenderSessionAnswer(202, false, json(body.constData()));
}

RenderSessionAnswer gone()
{
    return parseRenderSessionAnswer(
            410, true,
            json(R"({"detail": "This request is no longer in the rendering queue. Ask again without queue_id."})"));
}

} // namespace

void TestRemoteRenderQueue::aSessionIsRead()
{
    const RenderSessionAnswer a = session("abc");
    QCOMPARE(a.kind, Kind::Session);
    QCOMPARE(a.sessionId, QStringLiteral("abc"));
}

void TestRemoteRenderQueue::aPlaceInTheQueueIsNotAPyramidBuild()
{
    const RenderSessionAnswer a = queued("5be1");
    QCOMPARE(a.kind, Kind::Queued);
    QCOMPARE(a.queueId, QStringLiteral("5be1"));
    QCOMPARE(a.position, 2);
    QCOMPARE(a.queueLength, 3);
    QCOMPARE(a.activeSessions, 8);
    QCOMPARE(a.capacitySlots, 8);
    QCOMPARE(a.pollAfterSeconds, 2.0);
    QCOMPARE(a.waitTimeoutSeconds, 600.0);
    QVERIFY(a.message.startsWith(QStringLiteral("All 8")));
}

void TestRemoteRenderQueue::aPyramidBuildIsNotAPlace()
{
    const RenderSessionAnswer a = parseRenderSessionAnswer(
            202, false,
            json(R"({"valid": false, "preparing": true, "progress": 0.4,
                     "error": "Preparing the cube for full resolution"})"));
    QCOMPARE(a.kind, Kind::Preparing);
    QCOMPARE(a.progress, 0.4);
    QVERIFY(a.queueId.isEmpty());
}

void TestRemoteRenderQueue::aPlaceWithoutIdIsAFailure()
{
    // Nothing to ask again with: not a wait that could ever end.
    QCOMPARE(parseRenderSessionAnswer(202, false, json(R"({"queued": true, "position": 1})")).kind,
             Kind::Failed);
    // A 200 without a session, and a 202 that is neither.
    QCOMPARE(parseRenderSessionAnswer(200, false, json(R"({"valid": true})")).kind, Kind::Failed);
    QCOMPARE(parseRenderSessionAnswer(202, false, json(R"({"valid": false})")).kind, Kind::Failed);
    // No answer at all (the tunnel is down).
    QCOMPARE(parseRenderSessionAnswer(0, true, QJsonObject()).kind, Kind::Failed);
}

void TestRemoteRenderQueue::goneAndRefusedAreTold()
{
    QCOMPARE(gone().kind, Kind::PlaceGone);

    const RenderSessionAnswer full = parseRenderSessionAnswer(
            503, true,
            json(R"({"detail": "Server-side rendering capacity reached (8 sessions active, 16 requests waiting)."})"));
    QCOMPARE(full.kind, Kind::Refused);
    QVERIFY(full.capacity);
    QVERIFY(full.message.contains(QStringLiteral("16 requests waiting")));

    const RenderSessionAnswer timedOut = parseRenderSessionAnswer(
            503, true,
            json(R"({"detail": "Server-side rendering capacity reached: waited 600 s in the queue without a free slot."})"));
    QVERIFY(timedOut.capacity);

    // The queue turned off (VISIVO_RENDER_QUEUE_MAX=0): the short form.
    const RenderSessionAnswer queueOff = parseRenderSessionAnswer(
            503, true,
            json(R"({"detail": "Server-side rendering capacity reached (2 sessions active)."})"));
    QCOMPARE(queueOff.kind, Kind::Refused);
    QVERIFY(queueOff.capacity);

    // No EGL on this backend: not a matter of waiting.
    const RenderSessionAnswer noGpu = parseRenderSessionAnswer(
            503, true,
            json(R"({"detail": "Server-side rendering is not available on this backend."})"));
    QCOMPARE(noGpu.kind, Kind::Refused);
    QVERIFY(!noGpu.capacity);

    const RenderSessionAnswer other =
            parseRenderSessionAnswer(500, true, json(R"({"detail": "Preparing the cube for full resolution failed: x"})"));
    QCOMPARE(other.kind, Kind::Failed);
    QVERIFY(other.message.startsWith(QStringLiteral("Preparing the cube")));
}

void TestRemoteRenderQueue::theFirstRequestAsksToWaitWithoutAPlace()
{
    const RenderQueuePlace place;
    const QJsonObject body = place.requestBody(QStringLiteral("c0ffee"));
    QCOMPARE(body.value(QStringLiteral("dataset_id")).toString(), QStringLiteral("c0ffee"));
    QCOMPARE(body.value(QStringLiteral("queue")).toBool(), true);
    QVERIFY(!body.contains(QStringLiteral("queue_id")));
    QVERIFY(!place.waiting());
}

void TestRemoteRenderQueue::thePlaceGoesBackUntilASlotIsFree()
{
    RenderQueuePlace place;
    QCOMPARE(place.decide(true, queued("q1", 2, 3)).step, Step::AskLater);
    QVERIFY(place.waiting());
    QCOMPARE(place.requestBody(QStringLiteral("d")).value(QStringLiteral("queue_id")).toString(),
             QStringLiteral("q1"));
    // A position can grow by one (a user with no view goes first): still waiting.
    QCOMPARE(place.decide(true, queued("q1", 3, 4)).step, Step::AskLater);
    QCOMPARE(place.queueId(), QStringLiteral("q1"));

    const RenderQueuePlace::Decision d = place.decide(true, session("s1"));
    QCOMPARE(d.step, Step::Connect);
    QVERIFY(d.sessionToDelete.isEmpty());
    QVERIFY(d.placeToCancel.isEmpty());
    // Used: the next request starts afresh.
    QVERIFY(!place.waiting());
    QVERIFY(!place.requestBody(QStringLiteral("d")).contains(QStringLiteral("queue_id")));
}

void TestRemoteRenderQueue::aGonePlaceAsksAgainAtTheBack()
{
    RenderQueuePlace place;
    place.decide(true, queued("q1"));
    const RenderQueuePlace::Decision d = place.decide(true, gone());
    QCOMPARE(d.step, Step::AskNow);
    QVERIFY(d.placeToCancel.isEmpty());   // the backend no longer has it
    QVERIFY(!place.requestBody(QStringLiteral("d")).contains(QStringLiteral("queue_id")));
}

void TestRemoteRenderQueue::aRefusalDropsThePlace()
{
    RenderQueuePlace place;
    place.decide(true, queued("q1"));
    const RenderQueuePlace::Decision d = place.decide(
            true, parseRenderSessionAnswer(
                          503, true,
                          json(R"({"detail": "Server-side rendering capacity reached: waited 600 s in the queue without a free slot."})")));
    QCOMPARE(d.step, Step::StayLocal);
    QVERIFY(d.placeToCancel.isEmpty());   // dropped by the 503 itself
    QVERIFY(!place.waiting());
}

void TestRemoteRenderQueue::aFailureGivesThePlaceBack()
{
    // A poll lost on the network: the place may still be on the backend,
    // holding the next free slot for nobody until it expires.
    RenderQueuePlace place;
    place.decide(true, queued("q1"));
    const RenderQueuePlace::Decision d = place.decide(true, parseRenderSessionAnswer(0, true, {}));
    QCOMPARE(d.step, Step::StayLocal);
    QCOMPARE(d.placeToCancel, QStringLiteral("q1"));
    QVERIFY(!place.waiting());
}

void TestRemoteRenderQueue::aStaleSessionIsDeleted()
{
    // The user went back to Local (or closed the window) while the request
    // that was finally admitted was in flight: its session is nobody's.
    RenderQueuePlace place;
    const RenderQueuePlace::Decision d = place.decide(false, session("late"));
    QCOMPARE(d.step, Step::Ignore);
    QCOMPARE(d.sessionToDelete, QStringLiteral("late"));
    QVERIFY(d.placeToCancel.isEmpty());
}

void TestRemoteRenderQueue::aStalePlaceIsCancelled()
{
    RenderQueuePlace place;
    const RenderQueuePlace::Decision d = place.decide(false, queued("late-place"));
    QCOMPARE(d.step, Step::Ignore);
    QCOMPARE(d.placeToCancel, QStringLiteral("late-place"));
    QVERIFY(d.sessionToDelete.isEmpty());
    // Not kept: nobody will ask with it.
    QVERIFY(!place.waiting());

    // Anything else stale leaves nothing behind.
    QCOMPARE(place.decide(false, gone()).step, Step::Ignore);
    QVERIFY(place.decide(false, gone()).placeToCancel.isEmpty());
}

void TestRemoteRenderQueue::stoppingGivesThePlaceUpOnce()
{
    RenderQueuePlace place;
    place.decide(true, queued("q1"));
    QCOMPARE(place.take(), QStringLiteral("q1"));
    QVERIFY(place.take().isEmpty());
    QVERIFY(!place.waiting());
}

void TestRemoteRenderQueue::pollDelayStaysWithinTheExpiry()
{
    RenderSessionAnswer a = queued("q");
    QCOMPARE(visivo::queuePollDelayMs(a), 2000);
    a.pollAfterSeconds = 0.1;
    QCOMPARE(visivo::queuePollDelayMs(a), 1000);
    a.pollAfterSeconds = 60.0;   // never past a place's expiry (at least 6 s)
    QCOMPARE(visivo::queuePollDelayMs(a), 5000);
    // An answer without the field: the documented 2 s.
    const RenderSessionAnswer bare = parseRenderSessionAnswer(
            202, false, json(R"({"queued": true, "queue_id": "q", "position": 1})"));
    QCOMPARE(visivo::queuePollDelayMs(bare), 2000);
}

void TestRemoteRenderQueue::theBannerSaysWhereTheViewIs()
{
    const QString text = visivo::queueStatusText(queued("q", 2, 3));
    QVERIFY2(text.startsWith(QStringLiteral("Queued for server-side rendering: 2 of 3")),
             qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("all 8 slots busy")));
    // A position past the length (it can grow by one): never "4 of 3".
    QVERIFY(visivo::queueStatusText(queued("q", 4, 3)).contains(QStringLiteral("4 of 4")));
    QVERIFY(visivo::queueStatusText(queued("q", 0, 0)).contains(QStringLiteral("slot is free")));
}

namespace {

using RStep = visivo::ReconnectStep;
const QString kEnded = QStringLiteral("session ended");

visivo::RemoteDrop liveDrop(const QString &reason, qint64 liveMs)
{
    visivo::RemoteDrop d;
    d.showingRemote = true;
    d.wasLive = true;
    d.closeReason = reason;
    d.liveMs = liveMs;
    return d;
}

}   // namespace

void TestRemoteRenderQueue::aKilledSessionIsReplacedThenRetryIsOffered()
{
    visivo::RemoteReconnect r;
    // Live for a minute, then its render process is killed: a new session.
    QCOMPARE(r.onDrop(liveDrop(kEnded, 60'000)), RStep::NewSession);
    r.onLive();
    QVERIFY(!r.active());
    // The new one goes live and dies at once (its first full-resolution
    // frame, say): not replaced again, or it would cycle for ever.
    QCOMPARE(r.onDrop(liveDrop(kEnded, 1'500)), RStep::GiveUp);
    QVERIFY(!r.active());
    // The user clicks Reconnect: the session is gone (404), one new session.
    QCOMPARE(r.userReconnect(), RStep::CheckSession);
    QCOMPARE(r.onCheck(404), RStep::NewSession);
    r.onLive();
    // That one dies quickly too: Reconnect again, no loop.
    QCOMPARE(r.onDrop(liveDrop(kEnded, 3'000)), RStep::GiveUp);
    // A reconnect that then streams for a while is a success: its own later
    // death is replaced again.
    QCOMPARE(r.userReconnect(), RStep::CheckSession);
    QCOMPARE(r.onCheck(404), RStep::NewSession);
    r.onLive();
    QCOMPARE(r.onDrop(liveDrop(kEnded, visivo::kStableLiveMs)), RStep::NewSession);
}

void TestRemoteRenderQueue::anotherDropOffersRetry()
{
    // Issue #411: a drop without "session ended" (a laptop that slept, a
    // tunnel that died, a stream that went silent) asks the backend first.
    visivo::RemoteReconnect r;
    QCOMPARE(r.onDrop(liveDrop(QString(), 60'000)), RStep::CheckSession);
    QVERIFY(r.active());
    // The session is still there: its stream is opened again.
    QCOMPARE(r.onCheck(200), RStep::SameSession);
    r.onLive();
    QVERIFY(!r.active());

    // Next time it is gone (closed for idling while the laptop slept): a new one.
    QCOMPARE(r.onDrop(liveDrop(QStringLiteral("server error"), 60'000)), RStep::CheckSession);
    QCOMPARE(r.onCheck(404), RStep::NewSession);
    r.onLive();
}

void TestRemoteRenderQueue::aDropThatWasNotLiveIsLeftToTheSwitch()
{
    visivo::RemoteReconnect r;
    visivo::RemoteDrop negotiating = liveDrop(kEnded, 0);
    negotiating.wasLive = false;   // a connection that never had a frame
    QCOMPARE(r.onDrop(negotiating), RStep::Ignore);
    visivo::RemoteDrop local = liveDrop(kEnded, 60'000);
    local.showingRemote = false;   // the user went to Local, or a switch is under way
    QCOMPARE(r.onDrop(local), RStep::Ignore);
    QVERIFY(!r.active());
    // Nothing under way: answers that come late change nothing.
    QCOMPARE(r.onCheck(404), RStep::Ignore);
    QCOMPARE(r.onAttemptFailed(), RStep::Ignore);
    QCOMPARE(r.onWaited(), RStep::Ignore);
}

void TestRemoteRenderQueue::anUnreachableBackendIsAskedAgainWithGrowingWaits()
{
    visivo::RemoteReconnect r;
    QCOMPARE(r.onDrop(liveDrop(QString(), 60'000)), RStep::CheckSession);
    QCOMPARE(r.attempt(), 1);
    // The backend does not answer (a tunnel being reopened): wait, longer each time.
    QList<int> waits;
    for (int i = 1; i < visivo::RemoteReconnect::maxAttempts(); ++i) {
        QCOMPARE(r.onCheck(0), RStep::Wait);
        waits << r.delayMs();
        QCOMPARE(r.attempt(), i + 1);
        QCOMPARE(r.onWaited(), RStep::CheckSession);
    }
    for (int i = 1; i < waits.size(); ++i)
        QVERIFY2(waits[i] >= waits[i - 1], "the waits never shrink");
    QCOMPARE(waits.first(), visivo::kReconnectDelaysMs.front());
    QCOMPARE(waits.last(), visivo::kReconnectDelaysMs.back());
    // Bounded: the last attempt fails too, and the view offers Reconnect.
    QCOMPARE(r.onCheck(503), RStep::GiveUp);
    QVERIFY(!r.active());
    QCOMPARE(r.onWaited(), RStep::Ignore);

    // The backend back meanwhile: Reconnect starts a fresh budget.
    QCOMPARE(r.userReconnect(), RStep::CheckSession);
    QCOMPARE(r.attempt(), 1);
    QCOMPARE(r.onCheck(200), RStep::SameSession);
    r.onLive();
}

void TestRemoteRenderQueue::aFailedAttemptWaitsThenAsksAgain()
{
    visivo::RemoteReconnect r;
    QCOMPARE(r.onDrop(liveDrop(QString(), 60'000)), RStep::CheckSession);
    // There, but its stream does not come back (refused, or no Live in time).
    QCOMPARE(r.onCheck(200), RStep::SameSession);
    QCOMPARE(r.onAttemptFailed(), RStep::Wait);
    QCOMPARE(r.delayMs(), visivo::kReconnectDelaysMs[0]);
    // Asked again: it was closed meanwhile; the new session cannot be made
    // (the backend unreachable) and is asked for again after the next wait.
    QCOMPARE(r.onWaited(), RStep::CheckSession);
    QCOMPARE(r.onCheck(404), RStep::NewSession);
    QCOMPARE(r.onAttemptFailed(), RStep::Wait);
    QCOMPARE(r.delayMs(), visivo::kReconnectDelaysMs[1]);
    QCOMPARE(r.onWaited(), RStep::CheckSession);
    // A failed create is not a replacement that went live: another may be asked.
    QCOMPARE(r.onCheck(404), RStep::NewSession);
    r.onLive();
    QVERIFY(!r.active());
}

void TestRemoteRenderQueue::aLinkThatComesAndGoesIsNotReconnectedForEver()
{
    visivo::RemoteReconnect r;
    QCOMPARE(r.onDrop(liveDrop(QString(), 60'000)), RStep::CheckSession);
    QCOMPARE(r.onCheck(200), RStep::SameSession);
    r.onLive();
    // Back, then gone again within seconds, over and over: each is a failed
    // attempt with its wait, and the budget runs out.
    int drops = 0;
    RStep step = RStep::Wait;
    while (step == RStep::Wait && drops < 100) {
        ++drops;
        step = r.onDrop(liveDrop(QString(), 2'000));
        if (step == RStep::Wait) {
            QCOMPARE(r.onWaited(), RStep::CheckSession);
            QCOMPARE(r.onCheck(200), RStep::SameSession);
            r.onLive();
        }
    }
    QCOMPARE(step, RStep::GiveUp);
    QCOMPARE(drops, visivo::RemoteReconnect::maxAttempts());
    // A stream that then stays up a while starts the count again.
    QCOMPARE(r.userReconnect(), RStep::CheckSession);
    QCOMPARE(r.onCheck(200), RStep::SameSession);
    r.onLive();
    QCOMPARE(r.onDrop(liveDrop(QString(), visivo::kStableLiveMs)), RStep::CheckSession);
}

void TestRemoteRenderQueue::cancellingEndsTheReconnection()
{
    visivo::RemoteReconnect r;
    QCOMPARE(r.onDrop(liveDrop(QString(), 60'000)), RStep::CheckSession);
    r.cancel();   // ✕ (Local) while the check is under way
    QVERIFY(!r.active());
    QCOMPARE(r.onCheck(404), RStep::Ignore);
    QCOMPARE(r.onWaited(), RStep::Ignore);
}

void TestRemoteRenderQueue::theReconnectionSaysWhereItIs()
{
    const QString now = visivo::reconnectStatus(1, 9, 0);
    QVERIFY2(now.contains(QStringLiteral("attempt 1 of 9")), qPrintable(now));
    const QString later = visivo::reconnectStatus(3, 9, 3'500);
    QVERIFY2(later.contains(QStringLiteral("in 4 s")), qPrintable(later));
    QVERIFY2(later.contains(QStringLiteral("attempt 3 of 9")), qPrintable(later));
}

void TestRemoteRenderQueue::aSilentStreamIsTakenForLost()
{
    using V = visivo::StreamWatchdog::Verdict;
    constexpr qint64 kCheck = visivo::StreamWatchdog::kCheckMs;
    constexpr qint64 kSilence = 20'000;
    visivo::StreamWatchdog w;
    QCOMPARE(w.check(0, kSilence), V::Off);
    w.start(0);
    qint64 t = 0;
    // Traffic keeps it fine.
    for (int i = 0; i < 20; ++i) {
        t += kCheck;
        w.traffic(t - 100);
        QCOMPARE(w.check(t, kSilence), V::Fine);
    }
    // Then silence: a stall once it is longer than the limit, checked on time.
    const qint64 lastTraffic = t - 100;
    V v = V::Fine;
    while (v == V::Fine) {
        t += kCheck;
        v = w.check(t, kSilence);
    }
    QCOMPARE(v, V::Stalled);
    QVERIFY(t - lastTraffic > kSilence);
    QVERIFY(t - lastTraffic <= kSilence + kCheck);
    // Said once: the next stream starts it again.
    QVERIFY(!w.running());
    QCOMPARE(w.check(t + kCheck, kSilence), V::Off);
}

void TestRemoteRenderQueue::aLateCheckIsNotSilence()
{
    using V = visivo::StreamWatchdog::Verdict;
    constexpr qint64 kCheck = visivo::StreamWatchdog::kCheckMs;
    constexpr qint64 kSilence = 20'000;
    visivo::StreamWatchdog w;
    w.start(0);
    QCOMPARE(w.check(kCheck, kSilence), V::Fine);
    // Ten minutes without a check (App Nap, a sleep): the time away is not
    // silence, and the stream gets its full limit from now on.
    qint64 t = kCheck + 600'000;
    QCOMPARE(w.check(t, kSilence), V::Fine);
    t += kCheck;
    QCOMPARE(w.check(t, kSilence), V::Fine);
    // Still nothing after the limit, checked on time: lost.
    while (w.check(t += kCheck, kSilence) == V::Fine) {}
    QVERIFY(!w.running());
}

void TestRemoteRenderQueue::bothTransportsSayHowTheStreamEnded()
{
    visivo::StreamCloseRecorder ws;
    // WebSocket: live at the first frame, the server's reason at the close.
    ws.connecting();
    ws.markLive();
    ws.closed(true, kEnded);
    QVERIFY(ws.wasLive());
    QCOMPARE(ws.reason(), kEnded);
    QVERIFY(ws.liveMs() >= 0);
    // The next connection starts clean: an earlier drop's facts never decide
    // for a later one.
    ws.connecting();
    QVERIFY(!ws.wasLive());
    QVERIFY(ws.reason().isEmpty());
    QCOMPARE(ws.liveMs(), 0);

    // WebRTC: its states, no close reason.
    visivo::StreamCloseRecorder rtc;
    rtc.connecting();
    rtc.webrtcState(QStringLiteral("Connecting…"), false);
    rtc.webrtcState(QStringLiteral("Live"), false);
    rtc.webrtcState(QStringLiteral("Disconnected"), true);
    QVERIFY(rtc.wasLive());
    QVERIFY(rtc.reason().isEmpty());
    rtc.connecting();
    rtc.webrtcState(QStringLiteral("Error: ICE failed"), false);   // never live
    QVERIFY(!rtc.wasLive());
    QVERIFY(visivo::StreamCloseRecorder::isTerminal(QStringLiteral("Failed")));
    QVERIFY(!visivo::StreamCloseRecorder::isTerminal(QStringLiteral("Live")));
}

void TestRemoteRenderQueue::aFailedReplacementSaysWhyWithoutBlamingTheBackend()
{
    // Retry while the backend is down: no HTTP status at all.
    const RenderSessionAnswer down = parseRenderSessionAnswer(0, true, QJsonObject());
    QCOMPARE(down.kind, Kind::Failed);
    QCOMPARE(visivo::replacementFailure(down.httpStatus, down.message),
             QStringLiteral("the backend could not be reached"));
    // The replacement's own render process ended while it started (a 503).
    const RenderSessionAnswer ended = parseRenderSessionAnswer(
            503, true, json(R"({"detail": "render process ended unexpectedly"})"));
    QCOMPARE(ended.kind, Kind::Refused);
    QVERIFY(!ended.capacity);
    const QString why = visivo::replacementFailure(ended.httpStatus, ended.message);
    QVERIFY2(why.contains(QStringLiteral("render process ended unexpectedly")), qPrintable(why));
    QVERIFY(!why.contains(QStringLiteral("unavailable")));
    QCOMPARE(visivo::replacementFailure(500, QString()),
             QStringLiteral("the backend could not start a new view (HTTP 500)"));
}

void TestRemoteRenderQueue::onlyAReplacementsFailureOffersRetry()
{
    visivo::RemoteReplacement r;
    // The session ends, a replacement starts, and the user presses ✕ while
    // it is still being made: the replacement is over.
    QVERIFY(visivo::RemoteReplacement::mayStart(false, false));
    r.started();
    QVERIFY(r.active());
    r.ended();   // ✕ (Local), or the window releasing its session
    // Later the user asks for Remote and that first create fails: a failed
    // switch (the backend may not render), not a failed replacement.
    QVERIFY(!r.failed());

    // A replacement created whose stream fails before its first frame:
    // Retry, once; whatever fails next is the next switch's.
    r.started();
    QVERIFY(r.failed());
    QVERIFY(!r.failed());

    // One that goes live is over too: a later failure is not its.
    r.started();
    r.ended();   // Live
    QVERIFY(!r.failed());
}

void TestRemoteRenderQueue::aDropCheckAfterLocalChangesNothing()
{
    // A WebRTC stream dropped and GET .../levels is under way; the user
    // presses ✕ (Local) before its 404: no new session behind their back.
    QVERIFY(!visivo::dropCheckStillWanted(/*showingRemote=*/false, true, false));
    QVERIFY(!visivo::RemoteReplacement::mayStart(/*userChoseLocal=*/true, false));
    // Another switch under way, or another session by now: ignored too.
    QVERIFY(!visivo::dropCheckStillWanted(true, true, true));
    QVERIFY(!visivo::dropCheckStillWanted(true, false, false));
    QVERIFY(!visivo::RemoteReplacement::mayStart(false, true));
    // Still the same remote view: acted on.
    QVERIFY(visivo::dropCheckStillWanted(true, true, false));
}

void TestRemoteRenderQueue::aSilentDropIsLiveOnlyUpToItsLastSignOfLife()
{
    // A half-dead link: frames stop at once, the drop is noticed only after
    // the watchdog's silence. Its live time ends at the last frame, so it is
    // not a stable stream that resets the reconnection's budget.
    visivo::StreamCloseRecorder rec;
    rec.connecting();
    rec.markLive();
    QThread::msleep(20);
    rec.markTraffic();
    QThread::msleep(200);   // silence, then the watchdog's drop
    rec.closed(true, QString());
    QVERIFY2(rec.liveMs() < 150, qPrintable(QString::number(rec.liveMs())));
    QVERIFY(rec.liveMs() >= 15);
    // Without any traffic seen, up to the close as before.
    rec.connecting();
    rec.markLive();
    QThread::msleep(60);
    rec.closed(true, QString());
    QVERIFY(rec.liveMs() >= 50);
    // Traffic before Live does not count.
    rec.connecting();
    rec.markTraffic();
    rec.markLive();
    QThread::msleep(30);
    rec.closed(true, QString());
    QVERIFY(rec.liveMs() >= 25);
}

void TestRemoteRenderQueue::aLiveStreamWithoutFramesIsCaughtAfterItsGrace()
{
    using V = visivo::StreamWatchdog::Verdict;
    constexpr qint64 kCheck = visivo::StreamWatchdog::kCheckMs;
    constexpr qint64 kSilence = 20'000, kGrace = 60'000;
    visivo::StreamWatchdog w;
    w.start(0, kGrace);   // WebRTC Live: no frame yet
    qint64 t = 0;
    while (t < kGrace + kSilence - kCheck) {
        t += kCheck;
        QCOMPARE(w.check(t, kSilence), V::Fine);
    }
    // Still no frame: caught, not left Live for good.
    V v = V::Fine;
    while (v == V::Fine && t < kGrace + kSilence + 3 * kCheck) {
        t += kCheck;
        v = w.check(t, kSilence);
    }
    QCOMPARE(v, V::Stalled);
    // A late check during the grace does not cut it short.
    w.start(0, kGrace);
    QCOMPARE(w.check(30'000, kSilence), V::Fine);
    QCOMPARE(w.check(30'000 + kCheck, kSilence), V::Fine);
}

void TestRemoteRenderQueue::aRefusedTokenStopsAtOnce()
{
    visivo::RemoteReconnect r;
    QCOMPARE(r.onDrop(liveDrop(QString(), 60'000)), RStep::CheckSession);
    QVERIFY(!r.refusedAccess());
    QCOMPARE(r.onCheck(401), RStep::GiveUp);
    QVERIFY(r.refusedAccess());
    QVERIFY(!r.active());
    QCOMPARE(r.userReconnect(), RStep::CheckSession);
    QVERIFY(!r.refusedAccess());
    QCOMPARE(r.onCheck(403), RStep::GiveUp);
}
