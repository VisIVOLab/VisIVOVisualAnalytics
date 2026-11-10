#include "TestMissingRoute.h"

#include "BackendLocality.h"
#include "app/MissingRoute.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace {

/// Answers every request with one canned status and JSON body.
class CannedServer : public QObject
{
public:
    CannedServer(int status, QByteArray body) : status(status), body(std::move(body))
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *head = new QByteArray;
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QObject::destroyed, s, [head]() { delete head; });
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, head]() {
                    // The requests here have no body: the head is all there is.
                    *head += s->readAll();
                    if (!head->contains("\r\n\r\n"))
                        return;
                    s->write("HTTP/1.1 " + QByteArray::number(this->status) + " X\r\n"
                             "Content-Type: application/json\r\nContent-Length: "
                             + QByteArray::number(this->body.size())
                             + "\r\nConnection: close\r\n\r\n" + this->body);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QTcpServer server;
    int status;
    QByteArray body;
};

/// What a ray-traced view says for the reply to GET `path`.
QString lineFor(const QString &backendUrl, quint16 port, const QString &path)
{
    QNetworkAccessManager nam;
    QNetworkReply *reply =
            nam.get(QNetworkRequest(QUrl(u"http://127.0.0.1:%1%2"_s.arg(port).arg(path))));
    QSignalSpy finished(reply, &QNetworkReply::finished);
    if (!finished.wait(5000))
        return u"(no reply)"_s;
    const QString line =
            visivo::missingRouteMessage(backendUrl, reply, reply->readAll(), u"ray-traced views"_s);
    reply->deleteLater();
    return line;
}

} // namespace

void TestMissingRoute::aRoutesOwn404IsNotAMissingRoute()
{
    // GET /v1/render/{sid}/camera on a backend that has the route, for a
    // session it does not know: the session is gone, the backend is not old.
    QVERIFY(!visivo::isMissingRouteReply(404, R"({"detail":"Unknown render session_id."})"));
    QVERIFY(!visivo::isMissingRouteReply(
            404, R"({"detail":"Unknown dataset_id 'abc'. Open the dataset first via /v1/datasets/open."})"));
    // Other statuses never are, whatever they say.
    QVERIFY(!visivo::isMissingRouteReply(500, R"({"detail":"Not Found"})"));
    QVERIFY(!visivo::isMissingRouteReply(422, QByteArray()));
    QVERIFY(!visivo::isMissingRouteReply(200, QByteArray()));
    QVERIFY(!visivo::isMissingRouteReply(0, QByteArray()));   // no answer at all
}

void TestMissingRoute::fastApisBare404IsAMissingRoute()
{
    // What PLEIADI's older backend answered for the ray-traced routes.
    QVERIFY(visivo::isMissingRouteReply(404, R"({"detail":"Not Found"})"));
    QVERIFY(visivo::isMissingRouteReply(405, R"({"detail":"Method Not Allowed"})"));
    // A proxy or tunnel in front of it: HTML, plain text or nothing.
    QVERIFY(visivo::isMissingRouteReply(404, "<html><body>404 Not Found</body></html>"));
    QVERIFY(visivo::isMissingRouteReply(404, "Not Found"));
    QVERIFY(visivo::isMissingRouteReply(404, QByteArray()));
    // JSON without a string detail is not a route's own error either.
    QVERIFY(visivo::isMissingRouteReply(404, R"({"detail":[{"msg":"x"}]})"));
}

void TestMissingRoute::messageNamesTheBackend()
{
    QCOMPARE(visivo::missingFeatureMessage(u"PLEIADI"_s, u"ray-traced views"_s),
             u"The backend on PLEIADI does not have ray-traced views (it is older than this "
             u"client); update it."_s);
    // backendDisplayName's name for the local backend reads as English.
    QCOMPARE(visivo::missingFeatureMessage(u"Local"_s, u"ray-traced views"_s),
             u"The local backend does not have ray-traced views (it is older than this "
             u"client); update it."_s);
    QVERIFY(visivo::missingFeatureMessage(QString(), u"x"_s).startsWith(u"The local backend"_s));
    QVERIFY(visivo::missingFeatureMessage(u"[::1]:8001"_s, u"x"_s)
                    .startsWith(u"The backend on [::1]:8001 does"_s));
}

void TestMissingRoute::replyOfAnOldBackendNamesIt()
{
    // The name the window gives the backend, not the loopback address the
    // test serves on (a host no registry or session knows).
    const QString name = u"http://rt-old.invalid:8123"_s;
    {
        CannedServer old(404, R"({"detail":"Not Found"})");
        QVERIFY(old.server.isListening());
        QCOMPARE(lineFor(name, old.server.serverPort(), u"/v1/render/s1/camera"_s),
                 u"The backend on rt-old.invalid:8123 does not have ray-traced views (it is "
                 u"older than this client); update it."_s);
    }
    {
        CannedServer old(405, R"({"detail":"Method Not Allowed"})");
        QVERIFY(!lineFor(name, old.server.serverPort(), u"/v1/volume_render/grid"_s).isEmpty());
    }
    {
        // A current backend that lost the session: its own words stand.
        CannedServer current(404, R"({"detail":"Unknown render session_id."})");
        QVERIFY(lineFor(name, current.server.serverPort(), u"/v1/render/s1/camera"_s).isEmpty());
    }
    {
        CannedServer current(409, R"({"detail":"This render session has no camera to report."})");
        QVERIFY(lineFor(name, current.server.serverPort(), u"/v1/render/s1/camera"_s).isEmpty());
    }
    // No reply at all: not an old backend either.
    QVERIFY(visivo::missingRouteMessage(name, nullptr, QByteArray(), u"x"_s).isEmpty());
}
