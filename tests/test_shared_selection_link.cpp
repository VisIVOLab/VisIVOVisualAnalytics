// test_shared_selection_link.cpp
// Two SharedSelectionLinks in one session of a live backend: one publishes a
// mask with a record, the other (the cube viewer of the same session) receives
// it within a second, with the same voxels and the record intact; the
// publisher does not receive its own echo.

#include "TestSharedSelectionLink.h"

#include "SharedSelectionLink.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrlQuery>

#include <cstring>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

QJsonObject postJson(QNetworkAccessManager &nam, const QString &url, const QString &token,
                     const QString &session, const QJsonObject &body)
{
    QNetworkRequest req{ QUrl(url) };
    req.setRawHeader("X-Visivo-Token", token.toUtf8());
    if (!session.isEmpty()) req.setRawHeader("X-Visivo-Session", session.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    QNetworkReply *r = nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
    r->deleteLater();
    return o;
}

// A hub of one session: the first publish is answered 429 (the session's slots
// are busy), the next ones are kept with the next seq; GET ?wait=0 answers the
// latest entry; the long poll is held open; no mask is asked for here.
class FakeHub : public QObject
{
public:
    FakeHub()
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                auto received = std::make_shared<QByteArray>();
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, received]() {
                    *received += s->readAll();
                    const qsizetype end = received->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    qsizetype length = 0;
                    for (const QByteArray &line : received->left(end).split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toLongLong();
                    }
                    if (received->size() < end + 4 + length) return;
                    const QByteArray requestLine = received->left(received->indexOf("\r\n"));
                    received->clear();
                    answer(s, requestLine);
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }

    QTcpServer server;
    int posts = 0;
    qint64 seq = 0;
    QString latestNonce;
    QJsonObject latest;

private:
    static void reply(QTcpSocket *s, const QByteArray &status, const QByteArray &body)
    {
        s->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
    }

    void answer(QTcpSocket *s, const QByteArray &requestLine)
    {
        const QList<QByteArray> parts = requestLine.split(' ');
        const QUrl u(QString::fromUtf8(parts.value(1)));
        const QUrlQuery q(u);
        if (parts.value(0) == "POST") {
            if (++posts == 1) {
                reply(s, "429 Too Many Requests", R"({"detail":"busy"})");
                return;
            }
            const QJsonObject meta = QJsonDocument::fromJson(
                    q.queryItemValue(u"meta"_s, QUrl::FullyDecoded).toUtf8()).object();
            latestNonce = meta.value(u"nonce"_s).toString();
            latest = { { u"seq"_s, ++seq }, { u"kind"_s, u"lasso"_s }, { u"source"_s, u"desktop"_s },
                       { u"client"_s, q.queryItemValue(u"client"_s, QUrl::FullyDecoded) },
                       { u"dataset_id"_s, u"ds_a"_s }, { u"origin"_s, QJsonArray{ 0, 0, 0 } },
                       { u"dims"_s, QJsonArray{ 1, 1, 1 } }, { u"full_shape"_s, QJsonArray{ 10, 12, 8 } },
                       { u"count"_s, 1 }, { u"meta"_s, meta }, { u"instance"_s, u"i1"_s } };
            const QJsonObject kept{ { u"valid"_s, true }, { u"seq"_s, seq }, { u"count"_s, 1 },
                                    { u"instance"_s, u"i1"_s } };
            reply(s, "200 OK", QJsonDocument(kept).toJson(QJsonDocument::Compact));
            return;
        }
        if (q.queryItemValue(u"wait"_s) == u"0"_s) {
            if (latest.isEmpty()) reply(s, "204 No Content", {});
            else reply(s, "200 OK", QJsonDocument(latest).toJson(QJsonDocument::Compact));
            return;
        }
        // The long poll: held open until the link stops.
    }
};

} // namespace

void TestSharedSelectionLink::theDelayedRetryOfAnOlderSelectionIsDropped()
{
    // Publish A is answered 429; B, made while A waits for its retry, is kept.
    // A's late retry would take a fresh seq and replace B in the hub: the
    // other clients would show A while this window, which skips its own older
    // entry, keeps B. It must be dropped.
    FakeHub hub;
    SharedSelectionLink link;
    link.start(hub.url(), u"t"_s, u"s"_s, u"ds_a"_s, 10, 12, 8);
    QSignalSpy pub(&link, &SharedSelectionLink::published);
    QByteArray one(4, '\0');
    const float v = 1.0f;
    std::memcpy(one.data(), &v, 4);
    link.publish(u"lasso"_s, one, { 0, 0, 0 }, { 1, 1, 1 }, {});
    QTRY_COMPARE_WITH_TIMEOUT(hub.posts, 1, 5000);
    link.publish(u"lasso"_s, one, { 0, 0, 0 }, { 1, 1, 1 }, {});
    QTRY_COMPARE_WITH_TIMEOUT(pub.size(), 1, 5000);
    QVERIFY(pub.first().at(0).toLongLong() > 0);
    const QString b = link.viewState().localNonce;
    QCOMPARE(hub.latestNonce, b);

    // Past A's retry delay: nothing more was posted, and the hub still holds B.
    QTest::qWait(SharedSelectionLink::kRetry429Ms + 700);
    QCOMPARE(hub.posts, 2);
    QCOMPARE(hub.latestNonce, b);
    QCOMPARE(link.viewState().localNonce, b);
    QCOMPARE(link.viewState().shownNonce, b);
    QCOMPARE(pub.size(), 1);   // B's word stays the last one said
    link.stop();
}

void TestSharedSelectionLink::aPublishedSelectionReachesTheOtherClientWithItsRecord()
{
    const QString url = qEnvironmentVariable("VISIVO_SHARE_E2E_URL");
    const QString tokenFile = qEnvironmentVariable("VISIVO_SHARE_E2E_TOKEN_FILE");
    const QString cube = qEnvironmentVariable("VISIVO_SHARE_E2E_CUBE");
    if (url.isEmpty() || tokenFile.isEmpty() || cube.isEmpty())
        QSKIP("no live backend (set VISIVO_SHARE_E2E_URL, _TOKEN_FILE, _CUBE)");
    QFile f(tokenFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString token = QString::fromUtf8(f.readAll()).trimmed();

    QNetworkAccessManager nam;
    const QString session = postJson(nam, url + u"/v1/sessions/new"_s, token, {}, {})
                                    .value(u"session_id"_s).toString();
    QVERIFY(!session.isEmpty());
    const QJsonObject open = postJson(nam, url + u"/v1/datasets/open"_s, token, session,
                                      { { u"path"_s, cube } });
    QVERIFY2(open.value(u"valid"_s).toBool(), qPrintable(open.value(u"error"_s).toString()));
    const QString ds = open.value(u"dataset_id"_s).toString();
    const int w = open.value(u"width"_s).toInt(), h = open.value(u"height"_s).toInt(),
              d = open.value(u"depth"_s).toInt();

    SharedSelectionLink viewer, sender;
    sender.start(url, token, session, ds, w, h, d);
    QSignalSpy pub(&sender, &SharedSelectionLink::published);
    // A selection already in the hub when the viewer starts following it is
    // taken as its cursor, not presented.
    {
        QByteArray old(4, '\0');
        const float one = 1.0f;
        std::memcpy(old.data(), &one, 4);
        sender.publish(u"lasso"_s, old, { 0, 0, 0 }, { 1, 1, 1 }, {});
        QTRY_COMPARE_WITH_TIMEOUT(pub.size(), 1, 5000);
        QVERIFY(pub.first().at(0).toLongLong() > 0);
        pub.clear();
    }
    viewer.start(url, token, session, ds, w, h, d);
    QSignalSpy got(&viewer, &SharedSelectionLink::received);
    QSignalSpy echo(&sender, &SharedSelectionLink::received);
    QTest::qWait(400);   // both long polls are waiting
    QCOMPARE(got.size(), 0);

    // A 6 x 5 x 4 mask with a few voxels set, in the cube's middle.
    const std::array<int, 3> origin{ w / 2, h / 2, d / 2 }, dims{ 6, 5, 4 };
    QByteArray mask(6 * 5 * 4 * 4, '\0');
    const float one = 1.0f;
    for (int i : { 0, 7, 30, 31, 59, 119 }) std::memcpy(mask.data() + i * 4, &one, 4);
    visivo::LassoGrowRequest g;
    g.seed = { origin[2], origin[1], origin[0] };
    g.reach = 8.0; g.reachMax = 64.0;
    const QJsonObject meta = visivo::desktopLassoMeta(g, true, { { u"Note"_s, u"a+b & c=d"_s } }, false, 0);
    QElapsedTimer t;
    t.start();
    sender.publish(u"lasso"_s, mask, origin, dims, meta);
    QTRY_COMPARE_WITH_TIMEOUT(got.size(), 1, 5000);
    const qint64 ms = t.elapsed();
    qInfo("publish -> received by the other client: %lld ms", ms);
    QVERIFY(ms < 1000);
    QCOMPARE(pub.size(), 1);
    QVERIFY(pub.first().at(0).toLongLong() > 0);
    QCOMPARE(pub.first().at(1).toLongLong(), 6);
    const auto info = got.first().at(0).value<visivo::SharedSelectionInfo>();
    QCOMPARE(info.count, 6);
    QCOMPARE(info.origin, origin);
    QCOMPARE(info.dims, dims);
    QCOMPARE(info.source, u"desktop"_s);
    QCOMPARE(info.client, sender.clientId());
    QVERIFY(info.hasGrow);
    QCOMPARE(info.grow.seed, g.seed);
    QCOMPARE(info.meta.value(u"parameters"_s).toObject().value(u"Note"_s).toString(), u"a+b & c=d"_s);
    QCOMPARE(got.first().at(1).toByteArray(), mask);
    QTest::qWait(300);
    QCOMPARE(echo.size(), 0);   // the publisher ignores its own echo
    viewer.stop();
    sender.stop();
}
