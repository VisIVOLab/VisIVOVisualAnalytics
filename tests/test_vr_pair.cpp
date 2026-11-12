#include "TestVrPair.h"

#include "app/BackendClient.h"
#include "app/VrPair.h"
#include "app/VrPairQr.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace Qt::StringLiterals;
using VrPair::State;

namespace {

constexpr qint64 kNow = 1'800'000'000'000;   // a fixed "now", epoch ms

VrPair::Code codeFor(const QString &code, qint64 ttlMs = 300'000)
{
    VrPair::Code c;
    c.code = code;
    c.pageUrl = u"https://pair.example/vr/#pair="_s + code;
    c.expiresAtMs = kNow + ttlMs;
    return c;
}

VrPair::Tracker waitingTracker(const QString &dataset = u"ds1"_s)
{
    VrPair::Tracker t(dataset);
    t.requesting();
    VrPair::CreateResult r;
    r.ok = true;
    r.code = codeFor(u"K7Q4MX"_s);
    t.created(r, kNow);
    return t;
}

VrPair::Status status(bool enabled, const QStringList &pending,
                      const QList<VrPair::Connection> &connected = {})
{
    VrPair::Status s;
    s.valid = true;
    s.enabled = enabled;
    for (const QString &p : pending)
        s.pending.append({ p });
    s.connected = connected;
    return s;
}

VrPair::Connection headset(const QString &id, const QString &dataset = u"ds1"_s)
{
    VrPair::Connection c;
    c.id = id;
    c.dataset = dataset;
    c.transport = u"webrtc"_s;
    return c;
}

/// A fake backend: answers each route with what the test set, and records
/// every request line, header block and body.
class FakeBackend : public QObject
{
public:
    FakeBackend()
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buffer = new QByteArray;
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QObject::destroyed, s, [buffer]() { delete buffer; });
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buffer]() {
                    *buffer += s->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const QByteArray head = buffer->left(end);
                    const QByteArray lower = head.toLower();
                    qsizetype length = 0;
                    const qsizetype at = lower.indexOf("content-length:");
                    if (at >= 0)
                        length = lower.mid(at + 15, lower.indexOf("\r\n", at) - at - 15)
                                         .trimmed()
                                         .toLongLong();
                    if (buffer->size() < end + 4 + length)
                        return;
                    const QByteArray line = head.left(head.indexOf("\r\n"));
                    lines << QString::fromLatin1(line);
                    heads << head;
                    bodies << buffer->mid(end + 4, length);
                    buffer->clear();
                    int code = 404;
                    QByteArray payload = R"({"detail":"Not Found"})";
                    QByteArray extra;
                    if (line.startsWith("POST /v1/vr/pair ")) {
                        code = postStatus;
                        payload = postBody;
                        extra = "Date: Thu, 12 Nov 2026 10:00:00 GMT\r\n";
                    } else if (line.startsWith("GET /v1/vr/pair ")) {
                        code = 200;
                        payload = getBody;
                    } else if (line.startsWith("DELETE /v1/vr/pair/")) {
                        code = 200;
                        payload = R"({"revoked":true})";
                    }
                    s->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\n" + extra
                             + "Content-Type: application/json\r\nContent-Length: "
                             + QByteArray::number(payload.size())
                             + "\r\nConnection: close\r\n\r\n" + payload);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }
    /// The request whose line starts with @p prefix, -1 when none.
    int find(const QString &prefix) const
    {
        for (int i = 0; i < lines.size(); ++i)
            if (lines.at(i).startsWith(prefix))
                return i;
        return -1;
    }

    QTcpServer server;
    int postStatus{ 200 };
    QByteArray postBody;
    QByteArray getBody;
    QStringList lines;
    QList<QByteArray> heads;
    QList<QByteArray> bodies;
};

}   // namespace

void TestVrPair::requestBodyCarriesTheLinkValuesButTheToken()
{
    VrLink::Inputs in;
    in.baseUrl = u"http://127.0.0.1:8000"_s;
    in.token = u"secret-token"_s;
    in.sessionId = u"sess-1"_s;
    in.datasetId = u"ds1"_s;
    in.path = u"/data/cube.fits"_s;
    QJsonObject body = VrPair::requestBody(in);
    QCOMPARE(body.value(u"session"_s).toString(), u"sess-1"_s);
    QCOMPARE(body.value(u"dataset"_s).toString(), u"ds1"_s);
    QCOMPARE(body.value(u"path"_s).toString(), u"/data/cube.fits"_s);
    // A cube: no kind and no box, as in the link.
    QVERIFY(!body.contains(u"kind"_s));
    QVERIFY(!body.contains(u"box"_s));
    QVERIFY(!QJsonDocument(body).toJson().contains("secret-token"));

    in.kind = u"cube"_s;
    QVERIFY(!VrPair::requestBody(in).contains(u"kind"_s));
    in.kind = u"velocity"_s;
    in.boxMpc = u"250"_s;
    body = VrPair::requestBody(in);
    QCOMPARE(body.value(u"kind"_s).toString(), u"velocity"_s);
    QCOMPARE(body.value(u"box"_s).toString(), u"250"_s);   // a string, as in the fragment
    in.kind = u"points"_s;
    in.boxMpc.clear();
    body = VrPair::requestBody(in);
    QCOMPARE(body.value(u"kind"_s).toString(), u"points"_s);
    QVERIFY(!body.contains(u"box"_s));
}

void TestVrPair::expiryIsReadAsEpochOrIso()
{
    QCOMPARE(VrPair::parseTime(QJsonValue(1800000300.0)), qint64(1800000300000));
    QCOMPARE(VrPair::parseTime(QJsonValue(1800000300.25)), qint64(1800000300250));
    const qint64 utc = QDateTime(QDate(2026, 11, 12), QTime(10, 5), QTimeZone::UTC)
                               .toMSecsSinceEpoch();
    QCOMPARE(VrPair::parseTime(QJsonValue(u"2026-11-12T10:05:00Z"_s)), utc);
    QCOMPARE(VrPair::parseTime(QJsonValue(u"2026-11-12T10:05:00+00:00"_s)), utc);
    QCOMPARE(VrPair::parseTime(QJsonValue(u"2026-11-12T11:05:00+01:00"_s)), utc);
    QCOMPARE(VrPair::parseTime(QJsonValue(u"2026-11-12T05:05:00-05:00"_s)), utc);
    // No offset written: UTC, not this computer's zone.
    QCOMPARE(VrPair::parseTime(QJsonValue(u"2026-11-12T10:05:00"_s)), utc);
    QCOMPARE(VrPair::parseTime(QJsonValue(u"2026-11-12T10:05:00.500Z"_s)), utc + 500);
    QCOMPARE(VrPair::parseTime(QJsonValue(u"soon"_s)), qint64(-1));
    QCOMPARE(VrPair::parseTime(QJsonValue()), qint64(-1));
    QCOMPARE(VrPair::parseTime(QJsonValue(-5.0)), qint64(-1));
}

void TestVrPair::expiryFollowsTheServersClock()
{
    // The server's clock is 10 minutes ahead: its expiry 5 minutes from ITS
    // now is 5 minutes from ours.
    const qint64 serverNow = kNow + 600'000;
    QCOMPARE(VrPair::localExpiry(serverNow + 300'000, serverNow, kNow), kNow + 300'000);
    // No Date: taken as it is when plausible…
    QCOMPARE(VrPair::localExpiry(kNow + 120'000, -1, kNow), kNow + 120'000);
    // …else the contract's default life.
    QCOMPARE(VrPair::localExpiry(kNow - 1000, -1, kNow), kNow + VrPair::kDefaultTtlMs);
    QCOMPARE(VrPair::localExpiry(kNow + 48LL * 3600 * 1000, -1, kNow),
             kNow + VrPair::kDefaultTtlMs);
    QCOMPARE(VrPair::localExpiry(-1, serverNow, kNow), kNow + VrPair::kDefaultTtlMs);
    QCOMPARE(VrPair::parseHttpDate(u"Thu, 12 Nov 2026 10:00:00 GMT"_s),
             QDateTime(QDate(2026, 11, 12), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch());
    QCOMPARE(VrPair::parseHttpDate(u"Thu, 12 Nov 2026 11:00:00 +0100"_s),
             QDateTime(QDate(2026, 11, 12), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch());
    QCOMPARE(VrPair::parseHttpDate(QString()), qint64(-1));
    QCOMPARE(VrPair::parseHttpDate(u"yesterday"_s), qint64(-1));
}

void TestVrPair::createAnswerIsUnderstood()
{
    const QString date = u"Thu, 12 Nov 2026 10:00:00 GMT"_s;
    const QByteArray body = R"({"code":"K7Q4MX","expires_at":"2026-11-12T10:05:00Z",)"
                            R"("page_url":"https://pair.example/vr/#pair=K7Q4MX"})";
    const VrPair::CreateResult r = VrPair::interpretCreate(200, body, QString(), date, kNow);
    QVERIFY(r.ok);
    QCOMPARE(r.code.code, u"K7Q4MX"_s);
    QCOMPARE(r.code.pageUrl, u"https://pair.example/vr/#pair=K7Q4MX"_s);
    QCOMPARE(r.code.expiresAtMs, kNow + 300'000);
    QCOMPARE(r.unavailable, VrPair::Unavailable::None);

    // 201 is a success too; a body without a code is not.
    QVERIFY(VrPair::interpretCreate(201, body, QString(), date, kNow).ok);
    const VrPair::CreateResult empty =
            VrPair::interpretCreate(200, R"({"page_url":"x"})", QString(), date, kNow);
    QVERIFY(!empty.ok);
    QVERIFY(!empty.message.isEmpty());
    QVERIFY(!VrPair::interpretCreate(200, "<html>", QString(), date, kNow).ok);
}

void TestVrPair::conflictReasonsOpenTheUsbWay_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("why");
    QTest::newRow("not-configured")
            << 409 << QByteArray(R"({"detail":"VISIVO_VR_PAIR_URL is not set","reason":"not-configured"})")
            << int(VrPair::Unavailable::NotConfigured);
    QTest::newRow("unreachable")
            << 409 << QByteArray(R"({"detail":"connection refused","reason":"unreachable"})")
            << int(VrPair::Unavailable::Unreachable);
    QTest::newRow("old backend") << 404 << QByteArray(R"({"detail":"Not Found"})")
                                 << int(VrPair::Unavailable::OldBackend);
    QTest::newRow("old backend, proxy") << 404 << QByteArray("<html>404</html>")
                                        << int(VrPair::Unavailable::OldBackend);
    QTest::newRow("old backend, 405") << 405 << QByteArray(R"({"detail":"Method Not Allowed"})")
                                      << int(VrPair::Unavailable::OldBackend);
}

void TestVrPair::conflictReasonsOpenTheUsbWay()
{
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(int, why);
    const VrPair::CreateResult r = VrPair::interpretCreate(status, body, QString(), QString(), kNow);
    QVERIFY(!r.ok);
    QCOMPARE(int(r.unavailable), why);
    QVERIFY(r.message.contains(u"USB cable"_s));
    VrPair::Tracker t(u"ds1"_s);
    t.created(r, kNow);
    QCOMPARE(t.state(), State::Unavailable);
    QVERIFY(!t.polling());
}

void TestVrPair::otherRefusalsAreFailures_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QString>("says");
    QTest::newRow("409 other reason")
            << 409 << QByteArray(R"({"detail":"too many codes","reason":"rate-limited"})")
            << u"too many codes"_s;
    QTest::newRow("route's own 404") << 404 << QByteArray(R"({"detail":"Unknown dataset ds1"})")
                                     << u"Unknown dataset ds1"_s;
    QTest::newRow("token refused") << 401 << QByteArray(R"({"detail":"Unauthorized"})")
                                   << u"token"_s;
    QTest::newRow("server error") << 500 << QByteArray() << u"HTTP 500"_s;
    QTest::newRow("no answer") << 0 << QByteArray() << u"did not answer"_s;
}

void TestVrPair::otherRefusalsAreFailures()
{
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(QString, says);
    const VrPair::CreateResult r =
            VrPair::interpretCreate(status, body, u"Connection refused"_s, QString(), kNow);
    QVERIFY(!r.ok);
    QCOMPARE(r.unavailable, VrPair::Unavailable::None);
    QVERIFY2(r.message.contains(says), qPrintable(r.message));
    VrPair::Tracker t(u"ds1"_s);
    t.created(r, kNow);
    QCOMPARE(t.state(), State::Failed);
}

void TestVrPair::statusAnswerIsParsed()
{
    const QByteArray body =
            R"({"enabled":true,"pending":[{"code":"K7Q4MX","expires_at":"2026-11-12T10:05:00Z"}],)"
            R"("connected":[{"id":"h1","since":"2026-11-12T10:01:00Z","dataset":"ds1",)"
            R"("transport":"webrtc"},{"since":"x"}]})";
    const VrPair::Status s = VrPair::parseStatus(200, body);
    QVERIFY(s.valid);
    QVERIFY(s.enabled);
    QCOMPARE(s.pending.size(), 1);
    QCOMPARE(s.pending.first().code, u"K7Q4MX"_s);
    QCOMPARE(s.connected.size(), 1);   // the entry without an id is dropped
    QCOMPARE(s.connected.first().id, u"h1"_s);
    QCOMPARE(s.connected.first().dataset, u"ds1"_s);
    QCOMPARE(s.connected.first().transport, u"webrtc"_s);

    QVERIFY(VrPair::parseStatus(200, R"({"enabled":false})").valid);
    QVERIFY(!VrPair::parseStatus(200, R"({"enabled":false})").enabled);
    QVERIFY(!VrPair::parseStatus(500, body).valid);
    QVERIFY(!VrPair::parseStatus(0, QByteArray()).valid);
    QVERIFY(!VrPair::parseStatus(200, "not json").valid);
    QVERIFY(!VrPair::parseStatus(200, R"({"pending":[]})").valid);
}

void TestVrPair::codeIsGroupedForReading()
{
    QCOMPARE(VrPair::displayCode(u"K7Q4MX"_s), u"K7Q-4MX"_s);
    QCOMPARE(VrPair::displayCode(u"k7q4mx"_s), u"K7Q-4MX"_s);
    QCOMPARE(VrPair::displayCode(u"K7Q-4MX"_s), u"K7Q-4MX"_s);
    QCOMPARE(VrPair::displayCode(u"ABCDEFGH"_s), u"ABCDEFGH"_s);
}

void TestVrPair::countdownRoundsUp()
{
    QCOMPARE(VrPair::countdown(300'000), u"5:00"_s);
    QCOMPARE(VrPair::countdown(299'001), u"5:00"_s);
    QCOMPARE(VrPair::countdown(299'000), u"4:59"_s);
    QCOMPARE(VrPair::countdown(65'000), u"1:05"_s);
    QCOMPARE(VrPair::countdown(1), u"0:01"_s);
    QCOMPARE(VrPair::countdown(0), u"0:00"_s);
    QCOMPARE(VrPair::countdown(-5000), u"0:00"_s);
}

void TestVrPair::qrPayloadIsThePageWithThisCode()
{
    VrPair::Code c = codeFor(u"K7Q4MX"_s);
    QCOMPARE(VrPair::qrPayload(c), u"https://pair.example/vr/#pair=K7Q4MX"_s);
    // Grouped either side, or another case, is still the same code.
    c.pageUrl = u"https://pair.example/vr/#pair=k7q-4mx"_s;
    QCOMPARE(VrPair::qrPayload(c), c.pageUrl);
    // A page with other fragment parameters next to it.
    c.pageUrl = u"https://pair.example/vr/?lang=en#v=1&pair=K7Q4MX"_s;
    QCOMPARE(VrPair::qrPayload(c), c.pageUrl);
    // A local loader in development is http on localhost.
    c.pageUrl = u"http://localhost:8443/#pair=K7Q4MX"_s;
    QCOMPARE(VrPair::qrPayload(c), c.pageUrl);

    // Nothing for another code, no code, a token, or no web address.
    c.pageUrl = u"https://pair.example/vr/#pair=ZZZZZZ"_s;
    QVERIFY(VrPair::qrPayload(c).isEmpty());
    c.pageUrl = u"https://pair.example/vr/"_s;
    QVERIFY(VrPair::qrPayload(c).isEmpty());
    c.pageUrl = u"https://pair.example/vr/#pair=K7Q4MX&token=abc"_s;
    QVERIFY(VrPair::qrPayload(c).isEmpty());
    c.pageUrl = u"https://pair.example/vr/?token=abc#pair=K7Q4MX"_s;
    QVERIFY(VrPair::qrPayload(c).isEmpty());
    c.pageUrl = u"javascript:alert(1)#pair=K7Q4MX"_s;
    QVERIFY(VrPair::qrPayload(c).isEmpty());
    c.pageUrl.clear();
    QVERIFY(VrPair::qrPayload(c).isEmpty());
}

void TestVrPair::qrEncodesThePayload()
{
    const QString url = u"https://pair.example/vr/#pair=K7Q4MX"_s;
    const VrPairQr::Matrix m = VrPairQr::encode(url);
    // Versions are 21 + 4k modules a side; this fits in a small one.
    QVERIFY(m.size >= 21);
    QVERIFY(m.size <= 41);
    QCOMPARE((m.size - 21) % 4, 0);
    QCOMPARE(m.dark.size(), m.size * m.size);
    // The finder patterns: a dark 7×7 square ring in three corners.
    for (int i = 0; i < 7; ++i) {
        QVERIFY(m.at(i, 0));
        QVERIFY(m.at(0, i));
        QVERIFY(m.at(m.size - 1 - i, 0));
        QVERIFY(m.at(i, m.size - 1));
    }
    QVERIFY(!m.at(1, 1));
    QVERIFY(m.at(3, 3));
    // Deterministic, and different text is a different code.
    QCOMPARE(VrPairQr::encode(url).dark, m.dark);
    QVERIFY(VrPairQr::encode(u"https://pair.example/vr/#pair=ZZZZZZ"_s).dark != m.dark);
    QCOMPARE(VrPairQr::encode(QString()).size, 0);
    QCOMPARE(VrPairQr::encode(QString(5000, u'x')).size, 0);   // too long for any QR code
}

void TestVrPair::waitingThenConnectedThenDisconnected()
{
    VrPair::Tracker t = waitingTracker();
    QCOMPARE(t.state(), State::Waiting);
    QVERIFY(t.polling());
    t.polled(status(true, { u"K7Q4MX"_s }), kNow + 2000);
    QCOMPARE(t.state(), State::Waiting);
    t.polled(status(true, {}, { headset(u"h1"_s) }), kNow + 4000);
    QCOMPARE(t.state(), State::Connected);
    QCOMPARE(t.connections().size(), 1);
    QVERIFY(t.polling());   // to notice the headset leaving
    t.polled(status(true, {}), kNow + 6000);
    QCOMPARE(t.state(), State::Disconnected);
    QVERIFY(!t.polling());

    // Disconnect pressed: what it acts on is gone at once.
    VrPair::Tracker u = waitingTracker();
    u.polled(status(true, {}, { headset(u"h1"_s), headset(u"h2"_s) }), kNow + 1000);
    QCOMPARE(u.connections().size(), 2);
    u.disconnecting();
    QCOMPARE(u.state(), State::Disconnected);
    QVERIFY(u.connections().isEmpty());
}

void TestVrPair::unusedCodeExpires()
{
    VrPair::Tracker t = waitingTracker();
    t.tick(kNow + 299'000);
    QCOMPARE(t.state(), State::Waiting);
    QVERIFY(t.codeOutstanding(kNow + 299'000));
    // Run out: no longer outstanding, but still polled for a little while.
    t.tick(kNow + 300'000);
    QCOMPARE(t.state(), State::Waiting);
    QVERIFY(!t.codeOutstanding(kNow + 300'000));
    QVERIFY(t.polling());
    t.tick(kNow + 300'000 + VrPair::kLateUseGraceMs);
    QCOMPARE(t.state(), State::Expired);
    QVERIFY(!t.polling());

    // The backend dropping the code from the list at its expiry: checked for
    // a late use, then expired (not "lost").
    VrPair::Tracker u = waitingTracker();
    u.polled(status(true, {}), kNow + 300'500);
    QCOMPARE(u.state(), State::Connecting);
    QVERIFY(u.lateUse());
    u.tick(kNow + 300'500 + VrPair::kLateUseGraceMs + 1);
    QCOMPARE(u.state(), State::Expired);

    // Polls missed for long: expired at once.
    VrPair::Tracker v = waitingTracker();
    v.polled(status(true, {}), kNow + 300'000 + VrPair::kLateUseGraceMs);
    QCOMPARE(v.state(), State::Expired);
}

void TestVrPair::codeUsedJustBeforeExpiryStillConnects()
{
    // Last poll at T-1 s saw the code; the headset used it right then; the
    // next poll, after the expiry, finds it gone and then the headset.
    VrPair::Tracker t = waitingTracker();
    t.polled(status(true, { u"K7Q4MX"_s }), kNow + 299'000);
    t.tick(kNow + 300'200);
    QCOMPARE(t.state(), State::Waiting);
    t.polled(status(true, {}), kNow + 301'000);
    QCOMPARE(t.state(), State::Connecting);
    t.tick(kNow + 305'000);
    QCOMPARE(t.state(), State::Connecting);
    t.polled(status(true, {}, { headset(u"h1"_s) }), kNow + 307'000);
    QCOMPARE(t.state(), State::Connected);
}

void TestVrPair::codeStillWaitingBesideAnEarlierHeadset()
{
    // A headset paired from this window before is still connected when the
    // dialog shows a new code: the code stays shown, and revoked on close.
    VrPair::Tracker t = waitingTracker();
    t.polled(status(true, { u"K7Q4MX"_s }, { headset(u"old"_s) }), kNow + 2000);
    QCOMPARE(t.state(), State::Waiting);
    QCOMPARE(t.connections().size(), 1);
    QVERIFY(t.codeOutstanding(kNow + 2000));
    QVERIFY(VrPair::statusText(t, kNow + 2000).contains(u"still connected"_s));
    // The code used and a second headset there: connected, both listed.
    t.polled(status(true, {}, { headset(u"old"_s), headset(u"new"_s) }), kNow + 4000);
    QCOMPARE(t.state(), State::Connected);
    QCOMPARE(t.connections().size(), 2);
    // Disconnect while waiting lets the earlier headset go, the code stays.
    VrPair::Tracker u = waitingTracker();
    u.polled(status(true, { u"K7Q4MX"_s }, { headset(u"old"_s) }), kNow + 2000);
    u.disconnecting();
    QCOMPARE(u.state(), State::Waiting);
    QVERIFY(u.connections().isEmpty());
}

void TestVrPair::usedCodeWithoutHeadsetIsLost()
{
    VrPair::Tracker t = waitingTracker();
    t.polled(status(true, {}), kNow + 10'000);   // gone from pending, nobody connected
    QCOMPARE(t.state(), State::Connecting);
    QVERIFY(t.polling());
    QVERIFY(!t.codeOutstanding(kNow + 10'000));   // used: closing must not revoke it
    t.polled(status(true, {}), kNow + 12'000);
    QCOMPARE(t.state(), State::Connecting);
    t.tick(kNow + 10'000 + VrPair::kConnectGraceMs);
    QCOMPARE(t.state(), State::Connecting);
    t.tick(kNow + 10'000 + VrPair::kConnectGraceMs + 1);
    QCOMPARE(t.state(), State::Lost);
    QVERIFY(!t.polling());
}

void TestVrPair::usedCodeThenHeadsetConnects()
{
    VrPair::Tracker t = waitingTracker();
    t.polled(status(true, {}), kNow + 10'000);
    QCOMPARE(t.state(), State::Connecting);
    // The code's countdown no longer applies once it is used: the grace does.
    t.tick(kNow + 30'000);
    QCOMPARE(t.state(), State::Connecting);
    t.polled(status(true, {}, { headset(u"h1"_s) }), kNow + 20'000);
    QCOMPARE(t.state(), State::Connected);
}

void TestVrPair::otherDatasetsHeadsetIsNotOurs()
{
    // A headset paired from another window of this session, on another cube.
    VrPair::Tracker t = waitingTracker(u"ds1"_s);
    t.polled(status(true, { u"K7Q4MX"_s }, { headset(u"h9"_s, u"ds2"_s) }), kNow + 2000);
    QCOMPARE(t.state(), State::Waiting);
    QVERIFY(t.connections().isEmpty());
    // One that says no dataset is counted (it cannot be told apart).
    t.polled(status(true, {}, { headset(u"h1"_s, QString()) }), kNow + 4000);
    QCOMPARE(t.state(), State::Connected);
}

void TestVrPair::droppedPollChangesNothing()
{
    VrPair::Tracker t = waitingTracker();
    t.polled(VrPair::Status(), kNow + 2000);
    QCOMPARE(t.state(), State::Waiting);
    t.polled(status(true, {}, { headset(u"h1"_s) }), kNow + 4000);
    t.polled(VrPair::Status(), kNow + 6000);
    QCOMPARE(t.state(), State::Connected);
    // A poll after the state stopped changing is ignored as well.
    VrPair::Tracker u = waitingTracker();
    u.tick(kNow + 400'000);
    u.polled(status(true, {}, { headset(u"h1"_s) }), kNow + 400'001);
    QCOMPARE(u.state(), State::Expired);
}

void TestVrPair::pairingSwitchedOffOpensTheUsbWay()
{
    VrPair::Tracker t = waitingTracker();
    t.polled(status(false, {}), kNow + 2000);
    QCOMPARE(t.state(), State::Unavailable);
    QCOMPARE(t.unavailable(), VrPair::Unavailable::NotConfigured);
    QVERIFY(t.message().contains(u"USB cable"_s));
}

void TestVrPair::closingRevokesOnlyAnUnusedCode()
{
    VrPair::Tracker t = waitingTracker();
    QVERIFY(t.codeOutstanding(kNow + 1000));
    QVERIFY(!t.codeOutstanding(kNow + 300'000));   // expired by itself
    t.polled(status(true, {}, { headset(u"h1"_s) }), kNow + 2000);
    QVERIFY(!t.codeOutstanding(kNow + 2000));
    VrPair::Tracker r(u"ds1"_s);
    r.requesting();
    QVERIFY(!r.codeOutstanding(kNow));
}

void TestVrPair::statusTextFollowsTheState()
{
    VrPair::Tracker t(u"ds1"_s);
    t.requesting();
    QVERIFY(VrPair::statusText(t, kNow).contains(u"Asking"_s));
    t = waitingTracker();
    QVERIFY(VrPair::statusText(t, kNow + 1000).contains(u"4:59"_s));
    // The code itself is never in the status line (it is copied for support).
    QVERIFY(!VrPair::statusText(t, kNow).contains(u"K7Q"_s));
    t.polled(status(true, {}, { headset(u"h1"_s) }), kNow + 2000);
    QCOMPARE(VrPair::statusText(t, kNow), u"Headset connected."_s);
    t.polled(status(true, {}, { headset(u"h1"_s), headset(u"h2"_s) }), kNow + 4000);
    QCOMPARE(VrPair::statusText(t, kNow), u"2 headsets connected."_s);
    t.polled(status(true, {}), kNow + 6000);
    QVERIFY(VrPair::statusText(t, kNow).contains(u"New Code"_s));
    VrPair::Tracker e = waitingTracker();
    QVERIFY(VrPair::statusText(e, kNow + 300'000).contains(u"expiring"_s));
    e.tick(kNow + 300'000 + VrPair::kLateUseGraceMs);
    QVERIFY(VrPair::statusText(e, kNow).contains(u"expired"_s));
    VrPair::Tracker late = waitingTracker();
    late.polled(status(true, {}), kNow + 300'500);
    QVERIFY(VrPair::statusText(late, kNow).contains(u"ran out"_s));
}

void TestVrPair::serviceSettingsReachTheBackendUnlessTheEnvironmentSetsThem()
{
    QProcessEnvironment env;
    VrPair::applyServiceSettings(env, u" wss://pair.example/ "_s, u"https://pair.example/vr/"_s);
    QCOMPARE(env.value(u"VISIVO_VR_PAIR_URL"_s), u"wss://pair.example/"_s);
    QCOMPARE(env.value(u"VISIVO_VR_PAGE_URL"_s), u"https://pair.example/vr/"_s);

    // The environment wins, variable by variable.
    QProcessEnvironment set;
    set.insert(u"VISIVO_VR_PAIR_URL"_s, u"wss://other.example/"_s);
    VrPair::applyServiceSettings(set, u"wss://pair.example/"_s, u"https://pair.example/vr/"_s);
    QCOMPARE(set.value(u"VISIVO_VR_PAIR_URL"_s), u"wss://other.example/"_s);
    QCOMPARE(set.value(u"VISIVO_VR_PAGE_URL"_s), u"https://pair.example/vr/"_s);

    // Nothing built in: empty settings pass nothing at all.
    QProcessEnvironment none;
    VrPair::applyServiceSettings(none, QString(), u"  "_s);
    QVERIFY(!none.contains(u"VISIVO_VR_PAIR_URL"_s));
    QVERIFY(!none.contains(u"VISIVO_VR_PAGE_URL"_s));
}

void TestVrPair::clientCallsTheThreeRoutes()
{
    FakeBackend fake;
    QVERIFY(fake.server.isListening());
    fake.postBody = R"({"code":"K7Q4MX","expires_at":"2026-11-12T10:05:00Z",)"
                    R"("page_url":"https://pair.example/vr/#pair=K7Q4MX"})";
    fake.getBody = R"({"enabled":true,"pending":[{"code":"K7Q4MX","expires_at":1}],"connected":[]})";

    BackendClient client(fake.url(), u"tok-xyz"_s);
    client.setSessionId(u"sess-1"_s);
    VrLink::Inputs in;
    in.sessionId = u"sess-1"_s;
    in.datasetId = u"ds1"_s;
    in.path = u"/d/c.fits"_s;
    const BackendVrPairReply created = client.createVrPairCode(VrPair::requestBody(in));
    QCOMPARE(created.httpStatus, 200);
    QCOMPARE(created.date, u"Thu, 12 Nov 2026 10:00:00 GMT"_s);
    const VrPair::CreateResult r = VrPair::interpretCreate(
            created.httpStatus, created.body, created.error, created.date, kNow);
    QVERIFY(r.ok);
    QCOMPARE(r.code.expiresAtMs, kNow + 300'000);

    const int post = fake.find(u"POST /v1/vr/pair "_s);
    QVERIFY(post >= 0);
    const QByteArray head = fake.heads.at(post).toLower();
    QVERIFY(head.contains("x-visivo-token: tok-xyz"));
    QVERIFY(head.contains("x-visivo-session: sess-1"));
    const QJsonObject sent = QJsonDocument::fromJson(fake.bodies.at(post)).object();
    QCOMPARE(sent.value(u"dataset"_s).toString(), u"ds1"_s);
    QCOMPARE(sent.value(u"session"_s).toString(), u"sess-1"_s);
    QVERIFY(!fake.bodies.at(post).contains("tok-xyz"));

    const BackendVrPairReply polled = client.vrPairStatus();
    QCOMPARE(polled.httpStatus, 200);
    QCOMPARE(VrPair::parseStatus(polled.httpStatus, polled.body).pending.size(), 1);

    QString error;
    QVERIFY(client.revokeVrPair(u"K7Q-4MX"_s, error));
    QVERIFY(fake.find(u"DELETE /v1/vr/pair/K7Q-4MX "_s) >= 0);
    // An id with characters a path cannot hold is encoded, never split.
    QVERIFY(client.revokeVrPair(u"a/b c"_s, error));
    QVERIFY(fake.find(u"DELETE /v1/vr/pair/a%2Fb%20c "_s) >= 0);
    QVERIFY(!client.revokeVrPair(QString(), error));

    // 409: the status and the reason come through.
    fake.postStatus = 409;
    fake.postBody = R"({"detail":"pairing is off","reason":"not-configured"})";
    const BackendVrPairReply refused = client.createVrPairCode(VrPair::requestBody(in));
    QCOMPARE(refused.httpStatus, 409);
    QCOMPARE(VrPair::interpretCreate(refused.httpStatus, refused.body, refused.error,
                                     refused.date, kNow)
                     .unavailable,
             VrPair::Unavailable::NotConfigured);
}
