#include "TestTaskQuitDrain.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/FinderTaskRun.h"
#include "app/TaskQuitDrain.h"

#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QFuture>
#include <QtConcurrentRun>
#include <QThreadPool>
#include <QtTest>

#include <atomic>
#include <thread>

using namespace Qt::Literals::StringLiterals;
using visivo::BackendTaskControl;
using visivo::TaskQuitDrain;

namespace {

/// A backend on its own thread (the test thread blocks in drain()): a task
/// that keeps running, a session to mint, and a count of every request.
class FakeTaskBackend : public QThread
{
public:
    std::atomic<int> requests{ 0 };
    std::atomic<int> creates{ 0 };
    std::atomic<int> deletes{ 0 };
    std::atomic<quint16> port{ 0 };
    std::atomic_bool failed{ false };
    /// How long a task create waits before answering, once recorded.
    std::atomic<int> createDelayMs{ 0 };
    /// How long a task status read, and a DELETE, wait before answering.
    std::atomic<int> statusDelayMs{ 0 };
    std::atomic<int> deleteDelayMs{ 0 };
    std::atomic<int> statusReads{ 0 };

    /// Started and listening (false: could not listen, or no answer in 5 s).
    bool startAndWait()
    {
        start();
        QElapsedTimer t;
        t.start();
        while (port == 0 && !failed && t.elapsed() < 5000)
            QThread::msleep(1);
        return port != 0;
    }
    ~FakeTaskBackend() override
    {
        quit();
        wait();
    }

protected:
    void run() override
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost)) {
            failed = true;
            return;
        }
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this, &server]() {
            QTcpSocket *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                const QByteArray head = socket->readAll();
                const QList<QByteArray> line = head.left(head.indexOf('\r')).split(' ');
                const QByteArray method = line.value(0), path = line.value(1);
                ++requests;
                QByteArray body = R"({"valid":true})";
                int delayMs = 0;
                if (path.contains("/v1/sessions/new")) {
                    body = R"({"valid":true,"session_id":"s1"})";
                } else if (method == "DELETE" && path.contains("/v1/tasks/")) {
                    ++deletes;
                    body = R"({"valid":true,"status":"cancelled"})";
                    delayMs = deleteDelayMs;
                } else if (method == "POST" && path.contains("/v1/tasks/")) {
                    ++creates;   // the task exists on the backend from here
                    body = R"({"valid":true,"task_id":"t9","status":"running"})";
                    delayMs = createDelayMs;
                } else if (path.contains("/v1/tasks/")) {
                    ++statusReads;
                    body = R"({"valid":true,"task_id":"t1","status":"running","progress":0.1})";
                    delayMs = statusDelayMs;
                }
                QTimer::singleShot(delayMs, socket, [socket, body]() {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Connection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            });
        });
        port = server.serverPort();
        exec();
    }
};

}   // namespace

void TestTaskQuitDrain::cleanup()
{
    TaskQuitDrain::instance().resetForTest();
}

void TestTaskQuitDrain::drainWaitsForTheWorkersCancellation()
{
    // A worker following a task: it notices the abort, then "sends the DELETE"
    // (120 ms), and only then returns. The drain must not return before that:
    // after it, the application object goes away.
    BackendTaskControl control;
    std::atomic_bool started{ false };
    std::atomic_bool cancelSent{ false };
    std::thread worker([&]() {
        TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), &control);
        started = true;
        while (!control.aborted())
            QThread::msleep(5);
        QThread::msleep(120);
        cancelSent = true;
    });
    while (!started)
        QThread::msleep(1);
    QCOMPARE(TaskQuitDrain::instance().active(), 1);
    QVERIFY(!control.aborted());
    QVERIFY(TaskQuitDrain::instance().drain(5000));
    QVERIFY(control.aborted());
    QVERIFY(cancelSent);   // the ordering this exists for
    QVERIFY(TaskQuitDrain::instance().quitting());
    worker.join();
    QCOMPARE(TaskQuitDrain::instance().active(), 0);
}

void TestTaskQuitDrain::aFollowStartedWhileQuittingIsAbortedAtOnce()
{
    QVERIFY(TaskQuitDrain::instance().drain(100));
    BackendTaskControl control;
    {
        TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), &control);
        QVERIFY(control.aborted());
    }
    QCOMPARE(TaskQuitDrain::instance().active(), 0);
}

void TestTaskQuitDrain::drainIsBoundedByItsTimeout()
{
    // A worker that never ends must not hold the exit for ever.
    BackendTaskControl control;
    std::atomic_bool release{ false };
    std::atomic_bool started{ false };
    std::thread worker([&]() {
        TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), &control);
        started = true;
        while (!release)
            QThread::msleep(5);
    });
    while (!started)
        QThread::msleep(1);
    QElapsedTimer t;
    t.start();
    QVERIFY(!TaskQuitDrain::instance().drain(150));
    QVERIFY(t.elapsed() < 1000);
    QVERIFY(control.aborted());
    release = true;
    worker.join();
}

void TestTaskQuitDrain::aSecondDrainWaitsOnlyWhatIsLeft()
{
    // exitApplication drains, then exit() brings aboutToQuit's drain: with a
    // worker that never ends, the two together wait one ceiling, not two.
    BackendTaskControl control;
    std::atomic_bool release{ false };
    std::atomic_bool started{ false };
    std::thread worker([&]() {
        TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), &control);
        started = true;
        while (!release)
            QThread::msleep(5);
    });
    while (!started)
        QThread::msleep(1);
    QElapsedTimer t;
    t.start();
    QVERIFY(!TaskQuitDrain::instance().drain(300));
    QVERIFY(!TaskQuitDrain::instance().drain(300));
    QVERIFY(t.elapsed() < 450);   // two full ceilings would be 600 ms
    release = true;
    worker.join();
}

void TestTaskQuitDrain::nothingToDrainReturnsAtOnce()
{
    QElapsedTimer t;
    t.start();
    QVERIFY(TaskQuitDrain::instance().drain(5000));
    QVERIFY(t.elapsed() < 100);
}

void TestTaskQuitDrain::aWorkerStillCreatingItsTaskIsWaitedFor()
{
    // Quit lands while the worker's POST /v1/tasks/... is out: it holds no
    // Follow yet. The drain waits for the pool, the worker follows once the
    // create returns, is aborted at once, cancels, and only then the drain ends.
    auto control = std::make_shared<BackendTaskControl>();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    std::atomic_bool started{ false };
    QThreadPool::globalInstance()->start([control, cancelled, &started]() {
        started = true;
        QThread::msleep(150);   // the create request
        TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), control.get());
        while (!control->aborted())
            QThread::msleep(5);
        QThread::msleep(50);    // the DELETE
        *cancelled = true;
    });
    while (!started)
        QThread::msleep(1);
    QCOMPARE(TaskQuitDrain::instance().active(), 0);   // nothing registered yet
    QVERIFY(TaskQuitDrain::instance().drain(5000));
    QVERIFY(control->aborted());
    QVERIFY(cancelled->load());
}

void TestTaskQuitDrain::queuedWorkRunsAndIsWaitedFor()
{
    // One pool thread busy following a task, more work queued behind it. It
    // is not dropped (a dropped QtConcurrent job never finishes its future):
    // it runs, and the drain returns only after it has.
    QThreadPool *pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    auto control = std::make_shared<BackendTaskControl>();
    auto queuedRan = std::make_shared<std::atomic<int>>(0);
    std::atomic_bool started{ false };
    pool->start([control, &started]() {
        TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), control.get());
        started = true;
        while (!control->aborted())
            QThread::msleep(5);
        QThread::msleep(30);   // its DELETE
    });
    for (int i = 0; i < 4; ++i)
        pool->start([queuedRan]() { ++*queuedRan; });
    while (!started)
        QThread::msleep(1);
    QVERIFY(TaskQuitDrain::instance().drain(5000));
    QCOMPARE(queuedRan->load(), 4);
    pool->setMaxThreadCount(threads);
}

void TestTaskQuitDrain::aWorkerWaitingOnAQueuedFutureFinishes()
{
    // The subvolume loader's pattern: a pool worker waits on futures of jobs
    // queued behind it. With the queue dropped, the future never finished and
    // the worker (and the pool's destructor) waited for ever.
    QThreadPool *pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(2);
    auto release = std::make_shared<std::atomic_bool>(false);
    auto waiterDone = std::make_shared<std::atomic_bool>(false);
    // Occupy both threads: one blocker, one waiter on a job queued after them.
    pool->start([release]() {
        while (!*release)
            QThread::msleep(5);
    });
    // The waiter is queued first, then the job it waits on: both behind the
    // blocker. (A dropped QtConcurrent job, unlike a QPromise, never finishes
    // its future: that is the case this pins.)
    auto futureSlot = std::make_shared<QFuture<int>>();
    auto futureReady = std::make_shared<std::atomic_bool>(false);
    auto jobRan = std::make_shared<std::atomic_bool>(false);
    // The waiter waits on the job only once the drain has begun: waiting
    // earlier lets waitForFinished() run the queued job itself, and the job
    // would be gone from the queue before the drain could drop it.
    pool->start([futureSlot, futureReady, waiterDone]() {
        while (!*futureReady || !TaskQuitDrain::instance().quitting())
            QThread::msleep(1);
        futureSlot->waitForFinished();
        *waiterDone = true;
    });
    QFuture<int> future = QtConcurrent::run(pool, [jobRan]() {
        *jobRan = true;
        return 42;
    });
    *futureSlot = future;
    *futureReady = true;
    QThread::msleep(30);
    QVERIFY(!jobRan->load());   // still queued behind the blocker and the waiter
    // The blocker ends only once the drain has begun, so the job is still
    // queued when the drain looks at the pool (dropping it there would leave
    // its future unfinished and the waiter stuck: drain() would then fail).
    std::thread releaser([release]() {
        while (!TaskQuitDrain::instance().quitting())
            QThread::msleep(1);
        QThread::msleep(20);
        *release = true;
    });
    QVERIFY(TaskQuitDrain::instance().drain(5000));
    releaser.join();
    QVERIFY(waiterDone->load());
    QVERIFY(future.isFinished());
    pool->setMaxThreadCount(threads);
}

void TestTaskQuitDrain::aQueuedFollowerOfACreatedTaskIsAborted()
{
    // A decompression: its task created on the GUI thread, its follower queued
    // behind a busy pool. It must still run, be aborted as it registers, and
    // cancel the task (here: count the DELETE) before the drain returns.
    QThreadPool *pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    auto busy = std::make_shared<BackendTaskControl>();
    std::atomic_bool started{ false };
    pool->start([busy, &started]() {
        TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), busy.get());
        started = true;
        while (!busy->aborted())
            QThread::msleep(5);
    });
    auto control = std::make_shared<BackendTaskControl>();
    auto deleteSent = std::make_shared<std::atomic_bool>(false);
    pool->start([control, deleteSent]() {
        TaskQuitDrain::Follow follow(TaskQuitDrain::instance(), control.get());
        if (control->aborted()) {
            QThread::msleep(20);   // the DELETE
            *deleteSent = true;
        }
    });
    while (!started)
        QThread::msleep(1);
    QVERIFY(TaskQuitDrain::instance().drain(5000));
    QVERIFY(deleteSent->load());
    pool->setMaxThreadCount(threads);
}

void TestTaskQuitDrain::aFollowedTaskIsCancelledOnTheBackendBeforeTheDrainEnds()
{
    // The real follower (followBackendTask with a BackendClient), on the
    // pool, following a task that keeps running: the drain must see its
    // DELETE reach the backend, and the worker return, before it returns.
    // Made here, as the application makes it on its GUI thread: the follower
    // publishes to it, and created first on a pool thread it would be
    // parented across threads.
    DiagnosticsManager::instance();
    FakeTaskBackend backend;   // stopped and joined on every way out
    if (!backend.startAndWait())
        QSKIP("No loopback listener here.");
    const BackendClient client(u"http://127.0.0.1:%1"_s.arg(backend.port.load()), u"tok"_s);
    BackendTaskCreateResult created;
    created.valid = true;
    created.taskId = u"t1"_s;
    created.status = u"running"_s;
    QFuture<visivo::TaskFollowResult> followed = QtConcurrent::run([client, created]() {
        return visivo::followBackendTask(client, created, u"[test][task]"_s, nullptr);
    });
    QElapsedTimer t;
    t.start();
    while (TaskQuitDrain::instance().active() == 0 && t.elapsed() < 5000)
        QThread::msleep(5);
    QCOMPARE(TaskQuitDrain::instance().active(), 1);   // the production Follow
    QVERIFY(TaskQuitDrain::instance().drain(10'000));
    QVERIFY(followed.isFinished());
    QCOMPARE(backend.deletes.load(), 1);
    QCOMPARE(followed.result().outcome, visivo::TaskOutcome::Aborted);
}

void TestTaskQuitDrain::aTaskWhoseCreateIsStillAnsweringIsCancelled_data()
{
    QTest::addColumn<int>("delayMs");
    QTest::addColumn<QString>("prefix");
    QTest::newRow("short answer") << 400 << QString();
    // Beyond the cancellation's 3 s, inside the create's own timeout.
    QTest::newRow("slow answer") << 4000 << QString();
    // A backend behind a proxy path: the create is still recognised.
    QTest::newRow("proxy prefix") << 400 << u"/visivo"_s;
}

void TestTaskQuitDrain::aTaskWhoseCreateIsStillAnsweringIsCancelled()
{
    // The quit lands after the backend started the task but before its
    // answer (the id) arrived: the create is let finish, and the follower
    // cancels the task. Aborted at once, nothing would.
    QFETCH(int, delayMs);
    QFETCH(QString, prefix);
    DiagnosticsManager::instance();
    FakeTaskBackend backend;
    if (!backend.startAndWait())
        QSKIP("No loopback listener here.");
    backend.createDelayMs = delayMs;
    const BackendClient client(u"http://127.0.0.1:%1%2"_s.arg(backend.port.load()).arg(prefix),
                               u"tok"_s);
    QFuture<visivo::TaskFollowResult> followed = QtConcurrent::run([client]() {
        return visivo::followBackendTask(
                client, client.createMomentTask(u"d1"_s, 0, 0, 1, false, 0., false),
                u"[test][moment][task]"_s, nullptr);
    });
    QElapsedTimer t;
    t.start();
    while (backend.creates == 0 && t.elapsed() < 5000)
        QThread::msleep(2);
    QCOMPARE(backend.creates.load(), 1);
    QVERIFY(TaskQuitDrain::instance().drain(10'000));
    QVERIFY(followed.isFinished());
    QCOMPARE(backend.deletes.load(), 1);
}

void TestTaskQuitDrain::aSlowRequestIsDroppedOnQuit()
{
    // A plain status read with no abort flag of its own, answered only after
    // 8 s: the quit drops it (BackendClient's quit watch), the worker returns,
    // and the drain ends long before the answer would have come.
    DiagnosticsManager::instance();
    FakeTaskBackend backend;
    if (!backend.startAndWait())
        QSKIP("No loopback listener here.");
    backend.statusDelayMs = 8000;
    const BackendClient client(u"http://127.0.0.1:%1"_s.arg(backend.port.load()), u"tok"_s);
    QFuture<BackendTaskStatusResult> read =
            QtConcurrent::run([client]() { return client.requestTaskStatus(u"t1"_s); });
    QElapsedTimer t;
    t.start();
    while (backend.statusReads == 0 && t.elapsed() < 5000)
        QThread::msleep(2);
    QCOMPARE(backend.statusReads.load(), 1);
    t.restart();
    QVERIFY(TaskQuitDrain::instance().drain(10'000));
    QVERIFY(t.elapsed() < 2000);
    QVERIFY(read.isFinished());
    QVERIFY(!read.result().valid);
}

void TestTaskQuitDrain::theQuitsDeleteIsBounded()
{
    // The backend takes 8 s to answer the DELETE: the quit's cancellation
    // waits at most kQuitCancelTimeoutMs for it, not the ordinary 15 s.
    DiagnosticsManager::instance();
    FakeTaskBackend backend;
    if (!backend.startAndWait())
        QSKIP("No loopback listener here.");
    backend.deleteDelayMs = 8000;
    const BackendClient client(u"http://127.0.0.1:%1"_s.arg(backend.port.load()), u"tok"_s);
    BackendTaskCreateResult created;
    created.valid = true;
    created.taskId = u"t1"_s;
    created.status = u"running"_s;
    QFuture<visivo::TaskFollowResult> followed = QtConcurrent::run([client, created]() {
        return visivo::followBackendTask(client, created, u"[test][task]"_s, nullptr);
    });
    QElapsedTimer t;
    t.start();
    while (TaskQuitDrain::instance().active() == 0 && t.elapsed() < 5000)
        QThread::msleep(5);
    t.restart();
    QVERIFY(TaskQuitDrain::instance().drain(10'000));
    QVERIFY(t.elapsed() < visivo::kQuitCancelTimeoutMs + 1500);
    QVERIFY(followed.isFinished());
    QCOMPARE(backend.deletes.load(), 1);   // sent, though not confirmed in time
}

void TestTaskQuitDrain::aSourceFinderRunIsCancelledOnQuit()
{
    // runFinderTask follows its task with a loop of its own: it is under the
    // drain too, and cancels its task with the quit's short DELETE.
    DiagnosticsManager::instance();
    FakeTaskBackend backend;
    if (!backend.startAndWait())
        QSKIP("No loopback listener here.");
    const BackendClient client(u"http://127.0.0.1:%1"_s.arg(backend.port.load()), u"tok"_s);
    QFuture<visivo::FinderTaskOutcome> run = QtConcurrent::run([client]() {
        return visivo::runFinderTask(client, u"d1"_s, u"sofia"_s, QJsonObject{}, nullptr, {});
    });
    QElapsedTimer t;
    t.start();
    while (TaskQuitDrain::instance().active() == 0 && t.elapsed() < 5000)
        QThread::msleep(5);
    QCOMPARE(TaskQuitDrain::instance().active(), 1);   // its Follow
    t.restart();
    QVERIFY(TaskQuitDrain::instance().drain(10'000));
    QVERIFY(t.elapsed() < 3000);
    QVERIFY(run.isFinished());
    QVERIFY(run.result().cancelled);
    QCOMPARE(backend.deletes.load(), 1);
}

void TestTaskQuitDrain::theCreateCallsSendNothingWhileQuitting()
{
    FakeTaskBackend backend;   // stopped and joined on every way out
    if (!backend.startAndWait())
        QSKIP("No loopback listener here.");
    const BackendClient client(u"http://127.0.0.1:%1"_s.arg(backend.port.load()), u"tok"_s);
    QVERIFY(TaskQuitDrain::instance().drain(100));
    const BackendTaskCreateResult made[] = {
        client.createMomentTask(u"d1"_s, 0, 0, 1, false, 0., false),
        client.createMomentFitsTask(u"d1"_s, 0, 0, 1, false, 0., u"m.fits"_s, false),
        client.createPvTask(u"d1"_s, { { 0., 0. }, { 5., 5. } }, 1),
        client.createDecompressTask(u"/data/c.fits.gz"_s),
        client.createSourceFindTask(u"d1"_s, u"sofia"_s, QJsonObject{}),
    };
    for (const BackendTaskCreateResult &r : made) {
        QVERIFY(!r.valid);
        QCOMPARE(r.httpStatus, 0);
        QVERIFY(!visivo::isMissingTaskRoute(r));   // so no synchronous fallback
        QVERIFY(!r.error.isEmpty());
    }
    QCOMPARE(backend.requests.load(), 0);
}

void TestTaskQuitDrain::noTaskIsCreatedWhileQuitting()
{
    QString error;
    QVERIFY(!visivo::refuseTaskWhileQuitting(error));
    QVERIFY(error.isEmpty());
    QVERIFY(TaskQuitDrain::instance().drain(100));
    QVERIFY(visivo::refuseTaskWhileQuitting(error));
    QVERIFY(!error.isEmpty());
}
