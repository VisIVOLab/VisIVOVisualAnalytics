// test_quit_exit_main.cpp
// A process of its own (QCoreApplication::exit() would end the event loops of
// every later test in VisIVOTests): a task created on the APPLICATION thread
// while the application quits is still cancelled on the backend (#133).
//
// The decompression of a .fits.gz is created from the GUI thread
// (DecompressBeforeOpen). Quit arrives after the backend has created the task
// and before its answer: exit() ends the create's local event loop, and the
// task id, the one thing its DELETE needs, used to be lost. Exit code 0 when
// exactly one DELETE reached the backend; 77 (skipped) without loopback.
//
// With --gui-follower, the task is FOLLOWED on the application thread as well
// (BackendClient::openDataset of a .fits.gz: decompressForOpen, polling in
// local event loops), and the quit arrives while it polls: its DELETE must
// still go out, from a thread of its own, after exit() has ended this
// thread's event loops.
//
// With --render, a ray-traced render (RaytraceOutput, #149): created on a
// worker, then polled by a timer on the application thread, as the view does.
// The quit arrives while it polls; the drain must send exactly one DELETE
// /v1/particle_render/{id} before it returns, and the view's teardown after
// exec() none. With --render-about-to-quit, the same with the quit as ⌘Q
// and the Dock's Quit reach it when nothing drained first: exit(), then the
// drain from aboutToQuit, where an event loop on this thread returns at once,
// so the DELETE must go from a thread of the drain's. With --render-quitting,
// a render asked for once the quit has begun is refused and sends nothing.

#include "app/BackendClient.h"
#include "app/BackendTaskFollow.h"
#include "app/DiagnosticsManager.h"
#include "app/RenderTaskHandle.h"
#include "app/TaskQuitDrain.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QtConcurrentRun>

#include <atomic>
#include <cstdio>
#include <optional>

using namespace Qt::Literals::StringLiterals;

namespace {

/// The backend, on its own thread: records a create, answers it late.
class SlowCreateBackend : public QThread
{
public:
    std::atomic<int> creates{ 0 };
    std::atomic<int> deletes{ 0 };
    std::atomic<int> statusReads{ 0 };
    std::atomic<int> renderDeletes{ 0 };
    std::atomic<int> requests{ 0 };
    std::atomic<int> createDelayMs{ 600 };
    std::atomic<quint16> port{ 0 };
    std::atomic_bool failed{ false };

    ~SlowCreateBackend() override
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
                QByteArray body = R"({"valid":true})";
                int delayMs = 0;
                ++requests;
                if (path.contains("/v1/sessions/new")) {
                    body = R"({"valid":true,"session_id":"s1"})";
                } else if (method == "DELETE" && path.contains("/v1/particle_render/")) {
                    ++renderDeletes;
                    body = R"({"deleted":true})";
                } else if (method == "DELETE" && path.contains("/v1/tasks/")) {
                    ++deletes;
                    body = R"({"valid":true,"status":"cancelled"})";
                } else if (method == "POST" && path.contains("/v1/tasks/")) {
                    ++creates;   // the task exists from here
                    body = R"({"valid":true,"task_id":"t9","status":"running"})";
                    delayMs = createDelayMs;
                } else if (path.contains("/v1/tasks/")) {
                    ++statusReads;
                    body = R"({"valid":true,"task_id":"t9","status":"running"})";
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

namespace {

/// --gui-follower: the whole decompression on the application thread.
int guiFollower(QCoreApplication &app, SlowCreateBackend &backend)
{
    backend.createDelayMs = 0;
    BackendClient client(u"http://127.0.0.1:%1"_s.arg(backend.port.load()), u"tok"_s);
    QTimer quitWhilePolling;
    QObject::connect(&quitWhilePolling, &QTimer::timeout, &app, [&]() {
        if (backend.statusReads == 0)
            return;   // not following yet
        quitWhilePolling.stop();
        visivo::TaskQuitDrain::instance().drain(visivo::kQuitDrainTimeoutMs);
        QCoreApplication::exit(0);
    });
    QString error;
    QTimer::singleShot(0, &app, [&]() {
        quitWhilePolling.start(5);
        error = client.openDataset(u"/data/cube.fits.gz"_s).error;   // compressed: decompressForOpen
    });
    app.exec();
    std::printf("gui follower: creates=%d status reads=%d deletes=%d error=%s\n",
                backend.creates.load(), backend.statusReads.load(), backend.deletes.load(),
                qPrintable(error));
    const bool ok = backend.creates == 1 && backend.statusReads >= 1 && backend.deletes == 1;
    std::puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

enum class RenderQuit { DrainThenExit, ExitThenAboutToQuit, AskWhileQuitting };

/// --render*: a ray-traced render as RaytraceOutput follows it, a handle
/// registered with the drain, polled on the application thread.
int render(QCoreApplication &app, SlowCreateBackend &backend, RenderQuit how)
{
    const bool askWhileQuitting = how == RenderQuit::AskWhileQuitting;
    backend.createDelayMs = 0;
    const QString url = u"http://127.0.0.1:%1"_s.arg(backend.port.load());
    auto task = std::make_shared<visivo::RenderTaskHandle>(BackendClient(url, u"tok"_s));
    std::optional<visivo::TaskQuitDrain::Registration> registration;
    registration.emplace(visivo::TaskQuitDrain::instance(), task);
    QNetworkAccessManager nam;
    QTimer poll;
    poll.setInterval(20);
    QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
        nam.get(QNetworkRequest(QUrl(url + u"/v1/tasks/"_s + task->taskId())));
    });
    QFuture<BackendTaskCreateResult> asked;
    bool askedOnce = false;   // a default QFuture has no result to read
    auto ask = [&]() {
        askedOnce = true;
        const quint64 generation = task->forget(nullptr);
        auto *watcher = new QFutureWatcher<BackendTaskCreateResult>(&app);
        QObject::connect(watcher, &QFutureWatcher<BackendTaskCreateResult>::finished, &app,
                         [&, watcher]() {
                             watcher->deleteLater();
                             if (watcher->result().valid)
                                 poll.start();
                         });
        asked = QtConcurrent::run([task, generation]() {
            return task->create(generation, u"/v1/tasks/particle_render"_s, QJsonObject{});
        });
        watcher->setFuture(asked);
    };
    // main.cpp's handler: the drain, once exec()'s loop has ended.
    bool drainedOnAboutToQuit = false;
    if (how == RenderQuit::ExitThenAboutToQuit)
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&]() {
            drainedOnAboutToQuit = true;
            visivo::TaskQuitDrain::instance().drain(visivo::kQuitDrainTimeoutMs);
        });
    QTimer quitWhilePolling;
    QObject::connect(&quitWhilePolling, &QTimer::timeout, &app, [&]() {
        if (!askWhileQuitting && backend.statusReads == 0)
            return;   // not polling yet
        quitWhilePolling.stop();
        if (how != RenderQuit::ExitThenAboutToQuit)
            visivo::TaskQuitDrain::instance().drain(visivo::kQuitDrainTimeoutMs);
        if (askWhileQuitting) {
            ask();   // after the drain began: refused, nothing sent
            QThreadPool::globalInstance()->waitForDone(5000);
        }
        QCoreApplication::exit(0);
    });
    QTimer::singleShot(0, &app, [&]() {
        quitWhilePolling.start(5);
        if (!askWhileQuitting)
            ask();
    });
    const int deletesAtDrain = [&]() {
        app.exec();
        return backend.renderDeletes.load();
    }();
    // The view's teardown, after exec(): it deletes what it still holds.
    QString left;
    task->forget(&left);
    registration.reset();
    std::printf("render: creates=%d status reads=%d render deletes=%d (at the drain %d) "
                "requests=%d left=%s\n",
                backend.creates.load(), backend.statusReads.load(), backend.renderDeletes.load(),
                deletesAtDrain, backend.requests.load(), qPrintable(left));
    // Asked while quitting: the create ran, and refused, so nothing was sent.
    const bool askDone = askedOnce && asked.isFinished();
    const bool refused = askDone && !asked.result().valid
            && asked.result().httpStatus == 0
            && asked.result().error.contains(u"quitting"_s);
    if (askWhileQuitting)
        std::printf("render asked while quitting: %s\n",
                    askDone ? qPrintable(asked.result().error) : "(not run)");
    const bool ok = askWhileQuitting
            ? refused && backend.requests == 0 && left.isEmpty()
            : backend.creates == 1 && backend.statusReads >= 1 && deletesAtDrain == 1
                    && backend.renderDeletes == 1 && backend.deletes == 0 && left.isEmpty()
                    && (how != RenderQuit::ExitThenAboutToQuit || drainedOnAboutToQuit);
    std::puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}   // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    DiagnosticsManager::instance();
    SlowCreateBackend backend;
    backend.start();
    QElapsedTimer started;
    started.start();
    while (backend.port == 0 && !backend.failed && started.elapsed() < 5000)
        QThread::msleep(1);
    if (backend.port == 0) {
        std::puts("SKIP: no loopback listener");
        return 77;
    }
    if (app.arguments().contains(u"--gui-follower"_s))
        return guiFollower(app, backend);
    if (app.arguments().contains(u"--render"_s))
        return render(app, backend, RenderQuit::DrainThenExit);
    if (app.arguments().contains(u"--render-about-to-quit"_s))
        return render(app, backend, RenderQuit::ExitThenAboutToQuit);
    if (app.arguments().contains(u"--render-quitting"_s))
        return render(app, backend, RenderQuit::AskWhileQuitting);
    const BackendClient client(u"http://127.0.0.1:%1"_s.arg(backend.port.load()), u"tok"_s);

    // The quit, as MainWindow::exitApplication does it, once the backend has
    // the task and before its answer: drain, then exit().
    QTimer quitWhenCreated;
    QObject::connect(&quitWhenCreated, &QTimer::timeout, &app, [&]() {
        if (backend.creates == 0)
            return;
        quitWhenCreated.stop();
        visivo::TaskQuitDrain::instance().drain(visivo::kQuitDrainTimeoutMs);
        QCoreApplication::exit(0);
    });

    BackendTaskCreateResult created;
    bool followed = false;
    QTimer::singleShot(0, &app, [&]() {
        quitWhenCreated.start(5);
        created = client.createDecompressTask(u"/data/cube.fits.gz"_s);
        // As DecompressBeforeOpen goes on: the follower on the pool, waited
        // for without an event loop (the quit has ended them on this thread).
        QFuture<visivo::TaskFollowResult> follow = QtConcurrent::run([client, created]() {
            return visivo::followBackendTask(client, created, u"[test][decompress][task]"_s,
                                             nullptr);
        });
        QElapsedTimer waited;
        waited.start();
        while (!follow.isFinished() && waited.elapsed() < 10'000)
            QThread::msleep(20);
        followed = follow.isFinished();
    });
    app.exec();

    std::printf("created=%s task_id=%s followed=%d creates=%d deletes=%d\n",
                created.valid ? "yes" : "no", qPrintable(created.taskId), followed,
                backend.creates.load(), backend.deletes.load());
    const bool ok = created.valid && created.taskId == u"t9"_s && followed
            && backend.creates == 1 && backend.deletes == 1;
    std::puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
