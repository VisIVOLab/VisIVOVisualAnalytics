// test_render_session_calls.cpp
// See TestRenderSessionCalls.h. The cube viewer's close (releaseRemoteRenderSession
// → cancelRemoteQueuePlace) and RemotePointView::stop() both come down to
// RenderQueuePlace::take() handed to cancelRenderQueuePlace(): this is that path,
// without the window.

#include "TestRenderSessionCalls.h"

#include "RemoteRenderQueue.h"
#include "RenderSessionCalls.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace Qt::Literals::StringLiterals;

namespace {

/// Records the request line of every request and answers 200.
class OneLineServer : public QObject
{
public:
    OneLineServer()
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buffer = new QByteArray;
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buffer]() {
                    *buffer += s->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    lines << buffer->left(buffer->indexOf("\r\n"));
                    headers << buffer->left(end);
                    delete buffer;
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: 2\r\nConnection: close\r\n\r\n{}");
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }

    QTcpServer server;
    QList<QByteArray> lines;
    QList<QByteArray> headers;
};

const QString kToken = u"test-token"_s;
const QString kSession = u"0123456789abcdef0123456789abcdef"_s;

visivo::RenderSessionAnswer queuedAnswer(const char *id)
{
    return visivo::parseRenderSessionAnswer(
            202, false,
            QJsonDocument::fromJson(QByteArray(R"({"valid": false, "queued": true, "queue_id": ")")
                                    + id + R"(", "position": 1, "queue_length": 1})")
                    .object());
}

} // namespace

void TestRenderSessionCalls::closingWhileQueuedGivesThePlaceBack()
{
    OneLineServer backend;
    QVERIFY(backend.server.isListening());
    visivo::RenderQueuePlace place;
    QCOMPARE(place.decide(true, queuedAnswer("q1")).step, visivo::RenderQueuePlace::Step::AskLater);

    // The window closes while it waits: the place goes back, once.
    visivo::cancelRenderQueuePlace(backend.url(), kToken, kSession, place.take());
    visivo::cancelRenderQueuePlace(backend.url(), kToken, kSession, place.take());

    QTRY_COMPARE(backend.lines.size(), 1);
    QCOMPARE(backend.lines.first(), QByteArray("DELETE /v1/render/queue/q1 HTTP/1.1"));
    QVERIFY(backend.headers.first().toLower().contains("x-visivo-token:"));
    QVERIFY(backend.headers.first().contains(kSession.toLatin1()));
    QTest::qWait(100);
    QCOMPARE(backend.lines.size(), 1);
}

void TestRenderSessionCalls::aLateSessionIsDeleted()
{
    OneLineServer backend;
    // A request admitted after the viewer stopped waiting.
    const visivo::RenderQueuePlace::Decision d = visivo::RenderQueuePlace().decide(
            false, visivo::parseRenderSessionAnswer(
                           200, false,
                           QJsonDocument::fromJson(R"({"valid": true, "session_id": "late"})")
                                   .object()));
    visivo::deleteRenderSession(backend.url(), kToken, kSession, d.sessionToDelete);
    visivo::cancelRenderQueuePlace(backend.url(), kToken, kSession, d.placeToCancel);
    QTRY_COMPARE(backend.lines.size(), 1);
    QCOMPARE(backend.lines.first(), QByteArray("DELETE /v1/render/late HTTP/1.1"));
}

void TestRenderSessionCalls::nothingToGiveBackSendsNothing()
{
    OneLineServer backend;
    visivo::RenderQueuePlace place;   // never queued
    visivo::cancelRenderQueuePlace(backend.url(), kToken, kSession, place.take());
    visivo::deleteRenderSession(backend.url(), kToken, kSession, QString());
    QTest::qWait(200);
    QVERIFY(backend.lines.isEmpty());
}

void TestRenderSessionCalls::theRequestsOutliveTheViewer()
{
    // The manager belongs to the application, not to a viewer: a window deleted
    // right after its close does not take the DELETE with it.
    QNetworkAccessManager *nam = visivo::renderSessionManager();
    QVERIFY(nam);
    QCOMPARE(nam->parent(), QCoreApplication::instance());
    QCOMPARE(visivo::renderSessionManager(), nam);
}
