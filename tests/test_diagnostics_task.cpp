#include "TestDiagnosticsTask.h"

#include "app/BackendClient.h"
#include "app/BackendTaskFollow.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtConcurrent>
#include <QtTest>

#include <functional>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

struct Request {
    QByteArray method;
    QByteArray path;
    QJsonObject body;
};

struct Reply {
    int status = 200;
    QJsonObject body;
};

/// A backend on the test's thread: each request is recorded and answered by
/// @p route; the client runs on a worker so that both can block.
class MockBackend : public QObject
{
public:
    explicit MockBackend(std::function<Reply(const Request &)> route) : route(std::move(route))
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buffer]() {
                    *buffer += s->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const QByteArray head = buffer->left(end);
                    qsizetype length = 0;
                    for (const QByteArray &line : head.split('\n')) {
                        const QByteArray l = line.trimmed();
                        if (l.toLower().startsWith("content-length:"))
                            length = l.mid(15).trimmed().toLongLong();
                    }
                    if (buffer->size() < end + 4 + length)
                        return;
                    const QList<QByteArray> first = head.left(head.indexOf('\r')).split(' ');
                    Request request;
                    request.method = first.value(0);
                    request.path = first.value(1);
                    request.body = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
                    buffer->clear();
                    requests.append(request);
                    const Reply reply = this->route(request);
                    const QByteArray payload = QJsonDocument(reply.body).toJson(QJsonDocument::Compact);
                    s->write("HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\n"
                             "Content-Type: application/json\r\nContent-Length: "
                             + QByteArray::number(payload.size())
                             + "\r\nConnection: close\r\n\r\n" + payload);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }
    int count(const QByteArray &method, const QByteArray &path) const
    {
        int n = 0;
        for (const Request &r : requests)
            n += r.method == method && r.path == path;
        return n;
    }

    QTcpServer server;
    std::function<Reply(const Request &)> route;
    QList<Request> requests;
};

QJsonObject diagnosticsResult()
{
    return QJsonObject{ { u"valid"_s, true },
                        { u"error"_s, u""_s },
                        { u"field"_s, u"rho"_s },
                        { u"field_unit"_s, u"1e10 Msun/h / (kpc/h)^3"_s },
                        { u"weight_field"_s, u"mass"_s },
                        { u"weight_unit"_s, u"1e10 Msun/h"_s },
                        { u"weighting"_s, u"weighted by mass"_s },
                        { u"selection"_s, QJsonArray{ u"x < 500"_s } },
                        { u"selected_rows"_s, 1234567 },
                        { u"finite_rows"_s, 1234560 },
                        { u"minimum"_s, 0.5 },
                        { u"maximum"_s, 42.0 },
                        { u"mean"_s, 3.25 },
                        { u"stddev"_s, 1.5 },
                        { u"weighted_mean"_s, 4.0 },
                        { u"weight_sum"_s, 99.0 },
                        { u"bin_edges"_s, QJsonArray{ 0.5, 21.25, 42.0 } },
                        { u"counts"_s, QJsonArray{ 1000000, 234560 } } };
}

BackendCatalogueDiagnosticsRequest request()
{
    BackendCatalogueDiagnosticsRequest req;
    req.datasetId = u"d1"_s;
    req.field = u"rho"_s;
    req.weightField = u"mass"_s;
    req.bins = 2;
    return req;
}

QJsonObject taskStatus(const QString &status, double progress, const QJsonObject &result = {})
{
    QJsonObject o{ { u"valid"_s, true },     { u"error"_s, u""_s },
                   { u"task_id"_s, u"t1"_s }, { u"operation"_s, u"catalogue_diagnostics"_s },
                   { u"status"_s, status },   { u"progress"_s, progress } };
    if (!result.isEmpty())
        o.insert(u"result"_s, result);
    return o;
}

const QJsonObject kCreated{ { u"valid"_s, true }, { u"error"_s, u""_s },
                            { u"task_id"_s, u"t1"_s }, { u"status"_s, u"running"_s } };

/// Runs the client's call on a worker and waits on this thread, serving.
BackendCatalogueDiagnosticsResponse follow(const MockBackend &backend,
                                           visivo::BackendTaskControl *control,
                                           std::function<void()> whileWaiting = {})
{
    QFutureWatcher<BackendCatalogueDiagnosticsResponse> watcher;
    QSignalSpy done(&watcher, &QFutureWatcherBase::finished);
    const QString url = backend.url();
    watcher.setFuture(QtConcurrent::run([url, control]() {
        const BackendClient client(url);
        return client.catalogueDiagnosticsFollowingTask(request(), control);
    }));
    QElapsedTimer clock;
    clock.start();
    while (!watcher.isFinished() && clock.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (whileWaiting)
            whileWaiting();
    }
    if (!watcher.isFinished())
        done.wait(5000);
    // Never block on result() here: this thread serves the mock.
    if (!watcher.isFinished()) {
        BackendCatalogueDiagnosticsResponse timedOut;
        timedOut.error = u"the test timed out"_s;
        return timedOut;
    }
    return watcher.result();
}

void compareResults(const BackendCatalogueDiagnosticsResponse &a,
                    const BackendCatalogueDiagnosticsResponse &b)
{
    QCOMPARE(a.valid, b.valid);
    QCOMPARE(a.field, b.field);
    QCOMPARE(a.fieldUnit, b.fieldUnit);
    QCOMPARE(a.weightField, b.weightField);
    QCOMPARE(a.weightUnit, b.weightUnit);
    QCOMPARE(a.weighting, b.weighting);
    QCOMPARE(a.selection, b.selection);
    QCOMPARE(a.selectedRows, b.selectedRows);
    QCOMPARE(a.finiteRows, b.finiteRows);
    QCOMPARE(a.minimum, b.minimum);
    QCOMPARE(a.maximum, b.maximum);
    QCOMPARE(a.mean, b.mean);
    QCOMPARE(a.stddev, b.stddev);
    QCOMPARE(a.weightedMean, b.weightedMean);
    QCOMPARE(a.weightSum, b.weightSum);
    QCOMPARE(a.binEdges, b.binEdges);
    QCOMPARE(a.counts, b.counts);
}

} // namespace

void TestDiagnosticsTask::theTaskIsCreatedPolledAndGivesTheSameResult()
{
    int polls = 0;
    MockBackend backend([&polls](const Request &r) -> Reply {
        if (r.method == "POST" && r.path == "/v1/tasks/catalogue_diagnostics")
            return { 200, kCreated };
        if (r.method == "GET" && r.path == "/v1/tasks/t1")
            return { 200, ++polls < 3 ? taskStatus(u"running"_s, 0.3 * polls)
                                      : taskStatus(u"completed"_s, 1.0, diagnosticsResult()) };
        if (r.method == "POST" && r.path == "/v1/catalogue/diagnostics")
            return { 200, diagnosticsResult() };
        return { 404, QJsonObject{ { u"detail"_s, u"Not Found"_s } } };
    });
    QVERIFY(backend.server.isListening());
    visivo::BackendTaskControl control;
    const BackendCatalogueDiagnosticsResponse viaTask = follow(backend, &control);
    QVERIFY2(viaTask.valid, qPrintable(viaTask.error));
    QVERIFY(!viaTask.cancelled);
    QCOMPARE(backend.count("POST", "/v1/tasks/catalogue_diagnostics"), 1);
    QCOMPARE(backend.count("GET", "/v1/tasks/t1"), 3);
    QCOMPARE(backend.count("DELETE", "/v1/tasks/t1"), 0);
    QCOMPARE(backend.count("POST", "/v1/catalogue/diagnostics"), 0);
    // The task gets the synchronous route's body.
    QJsonObject body;
    QByteArrayList seen;
    for (const Request &r : backend.requests) {
        seen << r.method + ' ' + r.path;
        if (r.method == "POST" && r.path == "/v1/tasks/catalogue_diagnostics")
            body = r.body;
    }
    QVERIFY2(!body.isEmpty(), seen.join(", ").constData());
    QCOMPARE(body.value(u"dataset_id"_s).toString(), u"d1"_s);
    QCOMPARE(body.value(u"field"_s).toString(), u"rho"_s);
    QCOMPARE(body.value(u"weight_field"_s).toString(), u"mass"_s);
    QCOMPARE(body.value(u"bins"_s).toInt(), 4);   // clamped as before
    QVERIFY(!body.contains(u"limit"_s) && !body.contains(u"offset"_s));

    // The same answer as the synchronous route's.
    BackendCatalogueDiagnosticsResponse viaRoute;
    {
        QFutureWatcher<BackendCatalogueDiagnosticsResponse> watcher;
        const QString url = backend.url();
        watcher.setFuture(QtConcurrent::run([url]() {
            return BackendClient(url).catalogueDiagnostics(request());
        }));
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(), 10000);
        viaRoute = watcher.result();
    }
    compareResults(viaTask, viaRoute);
    QCOMPARE(viaTask.counts, (QVector<double>{ 1000000, 234560 }));
    QCOMPARE(viaTask.weightedMean.value_or(-1.0), 4.0);
}

void TestDiagnosticsTask::progressReachesTheControl()
{
    int polls = 0;
    MockBackend backend([&polls](const Request &r) -> Reply {
        if (r.method == "POST")
            return { 200, kCreated };
        return { 200, ++polls < 4 ? taskStatus(u"running"_s, 0.45)
                                  : taskStatus(u"completed"_s, 1.0, diagnosticsResult()) };
    });
    visivo::BackendTaskControl control;
    int seen = -1;
    const auto result = follow(backend, &control, [&]() {
        seen = std::max(seen, control.currentPercent());
    });
    QVERIFY(result.valid);
    QCOMPARE(seen, 45);
}

void TestDiagnosticsTask::cancelSendsDeleteToTheTask()
{
    MockBackend backend([](const Request &r) -> Reply {
        if (r.method == "POST")
            return { 200, kCreated };
        if (r.method == "DELETE")
            return { 200, QJsonObject{ { u"valid"_s, true }, { u"cancelled"_s, true } } };
        return { 200, taskStatus(u"running"_s, 0.2) };
    });
    visivo::BackendTaskControl control;
    const auto result = follow(backend, &control, [&]() {
        if (backend.count("GET", "/v1/tasks/t1") >= 1)
            control.requestAbort();
    });
    QVERIFY(result.cancelled);
    QVERIFY(!result.valid);
    QCOMPARE(backend.count("DELETE", "/v1/tasks/t1"), 1);
    QCOMPARE(backend.count("POST", "/v1/catalogue/diagnostics"), 0);
}

void TestDiagnosticsTask::aRefusedRequestComesBackAsAnError()
{
    // The task runs and its result says why (an unknown field): shown as is.
    MockBackend backend([](const Request &r) -> Reply {
        if (r.method == "POST")
            return { 200, kCreated };
        return { 200, taskStatus(u"completed"_s, 1.0,
                                 QJsonObject{ { u"valid"_s, false },
                                              { u"error"_s, u"Unknown field: 'rho'"_s } }) };
    });
    visivo::BackendTaskControl control;
    const auto result = follow(backend, &control);
    QVERIFY(!result.valid);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.error, u"Unknown field: 'rho'"_s);
}

void TestDiagnosticsTask::aTaskThatFailsIsAnErrorNotARetry()
{
    // A busy backend refuses the task (429), and a task that fails: neither
    // is computed again through the synchronous route.
    for (const bool refuse : { true, false }) {
        MockBackend backend([refuse](const Request &r) -> Reply {
            if (r.method == "POST" && r.path == "/v1/tasks/catalogue_diagnostics")
                return refuse ? Reply{ 429, QJsonObject{ { u"detail"_s, u"Too many tasks"_s } } }
                              : Reply{ 200, kCreated };
            if (r.method == "POST")
                return { 200, diagnosticsResult() };
            QJsonObject failed = taskStatus(u"failed"_s, 0.5);
            failed.insert(u"error"_s, u"disk read error"_s);
            return { 200, failed };
        });
        visivo::BackendTaskControl control;
        const auto result = follow(backend, &control);
        QVERIFY(!result.valid);
        QVERIFY(!result.cancelled);
        QVERIFY(!result.error.isEmpty());
        if (!refuse)
            QCOMPARE(result.error, u"disk read error"_s);
        QCOMPARE(backend.count("POST", "/v1/catalogue/diagnostics"), 0);
    }
}

void TestDiagnosticsTask::anOlderBackendFallsBackToTheSynchronousRoute()
{
    MockBackend backend([](const Request &r) -> Reply {
        if (r.path == "/v1/catalogue/diagnostics")
            return { 200, diagnosticsResult() };
        return { 404, QJsonObject{ { u"detail"_s, u"Not Found"_s } } };
    });
    visivo::BackendTaskControl control;
    const auto result = follow(backend, &control);
    QVERIFY(result.valid);
    QCOMPARE(result.selectedRows, 1234567);
    QCOMPARE(backend.count("POST", "/v1/catalogue/diagnostics"), 1);
    QCOMPARE(control.currentPhase(), visivo::TaskPhase::Synchronous);
}
