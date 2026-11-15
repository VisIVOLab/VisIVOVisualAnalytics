#include "TestVrSpectator.h"

#include "VrSpectator.h"
#include "VrSpectatorFeed.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <cmath>

using namespace Qt::StringLiterals;

#ifndef VISIVO_REPO_ROOT
#    define VISIVO_REPO_ROOT "."
#endif

namespace {

/// Bytes that start as a JPEG does (the feed hands frames over undecoded).
QByteArray jpegOf(int w, int h, const char *tag)
{
    return QByteArray("\xff\xd8\xff\xe0", 4) + QByteArray(tag) + QByteArray::number(w) + 'x' + QByteArray::number(h);
}

struct Answer
{
    int code{ 204 };
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray type = "application/json";
};

/// A backend with the spectator routes: the watch and snapshot answers set
/// below, the frame reads from a queue (then 204 after a short wait, as the
/// backend's own wait would).
class FakeSpectator : public QObject
{
public:
    FakeSpectator()
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
                        length = lower.mid(at + 15, lower.indexOf("\r\n", at) - at - 15).trimmed().toLongLong();
                    if (buffer->size() < end + 4 + length)
                        return;
                    const QByteArray line = head.left(head.indexOf("\r\n"));
                    lines << QString::fromLatin1(line);
                    heads << head;
                    bodies << buffer->mid(end + 4, length);
                    buffer->clear();
                    Answer a{ 404, R"({"detail":"Not Found"})" };
                    bool later = false;
                    int delayMs = 50;
                    bool isOn = false;
                    if (line.startsWith("POST /v1/vr/headset/spectator ")) {
                        a = { watchStatus, watchBody };
                        isOn = QJsonDocument::fromJson(bodies.last()).object().value(u"on"_s).toBool();
                        if (!isOn && watchOnDelayMs > 0 && !onAnswered)
                            offBeforeOnAnswered = true;
                        if (isOn && watchOnDelayMs > 0) {
                            later = true;
                            delayMs = watchOnDelayMs;
                        }
                    } else if (line.startsWith("POST /v1/vr/headset/spectator/snapshot ")) {
                        a = { snapshotStatus, snapshotBody };
                    } else if (line.startsWith("GET /v1/vr/headset/spectator/frame?")) {
                        if (!frames.isEmpty())
                            a = frames.takeFirst();
                        else {
                            a = { 204, {} };
                            later = true;
                        }
                    }
                    QPointer<QTcpSocket> sock(s);
                    auto send = [this, sock, a, isOn]() {
                        if (isOn)
                            onAnswered = true;
                        if (!sock)
                            return;
                        QByteArray out = "HTTP/1.1 " + QByteArray::number(a.code) + " X\r\nContent-Type: " + a.type
                                + "\r\nContent-Length: " + QByteArray::number(a.body.size()) + "\r\n";
                        for (const auto &h : a.headers)
                            out += h.first + ": " + h.second + "\r\n";
                        out += "Connection: close\r\n\r\n" + a.body;
                        sock->write(out);
                        sock->disconnectFromHost();
                    };
                    if (later)
                        QTimer::singleShot(delayMs, this, send);
                    else
                        send();
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }
    int count(const QString &prefix) const
    {
        int n = 0;
        for (const QString &l : lines)
            n += l.startsWith(prefix) ? 1 : 0;
        return n;
    }
    QJsonObject lastBodyOf(const QString &prefix) const
    {
        for (qsizetype i = lines.size() - 1; i >= 0; --i)
            if (lines.at(i).startsWith(prefix))
                return QJsonDocument::fromJson(bodies.at(i)).object();
        return {};
    }
    static Answer frame(qint64 seq, const QByteArray &jpeg, int w, int h)
    {
        Answer a{ 200, jpeg };
        a.type = "image/jpeg";
        a.headers = { { "X-Visivo-Frame-Seq", QByteArray::number(seq) },
                      { "X-Visivo-Frame-Width", QByteArray::number(w) },
                      { "X-Visivo-Frame-Height", QByteArray::number(h) },
                      { "X-Visivo-Frame-Eye", "left" },
                      { "X-Visivo-Frame-Age-Ms", "12" },
                      { "X-Visivo-Frame-Cost-Ms", "0.350" },
                      { "X-Visivo-Instance", "inst-1" } };
        return a;
    }

    QTcpServer server;
    QStringList lines;
    QList<QByteArray> heads;
    QList<QByteArray> bodies;
    int watchStatus{ 200 };
    QByteArray watchBody = R"({"client":"page-1","on":true,"fps":15,"width":512,"lease_s":10,"instance":"inst-1"})";
    int snapshotStatus{ 200 };
    QByteArray snapshotBody = R"({"valid":true,"workspace_filename":"headset_quest_3.png","output_path":"/ws/headset_quest_3.png","width":512,"height":540,"seq":3})";
    QList<Answer> frames;
    int watchOnDelayMs{ 0 };            ///< an "on" answered this late
    bool onAnswered{ false };
    bool offBeforeOnAnswered{ false };  ///< an "off" came while an "on" was unanswered
};

QHash<QByteArray, QByteArray> frameHeaders(qint64 seq)
{
    return { { "x-visivo-frame-seq", QByteArray::number(seq) }, { "x-visivo-frame-width", "512" },
             { "x-visivo-frame-height", "540" }, { "x-visivo-frame-eye", "left" },
             { "x-visivo-frame-age-ms", "18" }, { "x-visivo-frame-cost-ms", "0.420" } };
}

QString backendSource(const QString &needle)
{
    QDirIterator it(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/backend/app"_s, { u"*.py"_s }, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile f(it.next());
        if (f.fileName().contains(u"/static/"_s) || !f.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(f.readAll());
        if (text.contains(needle))
            return text;
    }
    return {};
}

/// The field names of the pydantic model @p name in @p source.
QSet<QString> modelFields(const QString &source, const QString &name)
{
    QSet<QString> fields;
    const qsizetype start = source.indexOf(u"class %1(BaseModel):"_s.arg(name));
    if (start < 0)
        return fields;
    static const QRegularExpression nextTop(u"^\\S"_s, QRegularExpression::MultilineOption);
    const auto stop = nextTop.match(source, source.indexOf(u'\n', start) + 1);
    const QString body = source.mid(start, stop.hasMatch() ? stop.capturedStart() - start : -1);
    static const QRegularExpression field(u"^    (\\w+)\\s*:"_s, QRegularExpression::MultilineOption);
    for (auto f = field.globalMatch(body); f.hasNext();)
        fields.insert(f.next().captured(1));
    return fields;
}

} // namespace

void TestVrSpectator::watchBodyIsBoundedAndCarriesNoToken()
{
    const QJsonObject on = VrSpectator::watchBody(u"page-1"_s, true, 99, 5000);
    QCOMPARE(on.value(u"client"_s).toString(), u"page-1"_s);
    QCOMPARE(on.value(u"on"_s).toBool(), true);
    QCOMPARE(on.value(u"fps"_s).toInt(), VrSpectator::kMaxFps);
    QCOMPARE(on.value(u"width"_s).toInt(), VrSpectator::kMaxWidth);
    QCOMPARE(VrSpectator::watchBody({}, true, 0, 10).value(u"width"_s).toInt(), VrSpectator::kMinWidth);
    const QJsonObject off = VrSpectator::watchBody(u"page-1"_s, false);
    QCOMPARE(off.keys(), QStringList({ u"client"_s, u"on"_s }));
    QVERIFY(!VrSpectator::watchBody({}, false).contains(u"client"_s));
    for (const QJsonObject &o : { on, off })
        QVERIFY(!QJsonDocument(o).toJson().contains("token"));
}

void TestVrSpectator::watchAnswersAreUnderstood()
{
    using O = VrSpectator::WatchResult::Outcome;
    auto r = VrSpectator::interpretWatch(200, R"({"client":"p2","on":true,"fps":24,"width":640})", {});
    QCOMPARE(r.outcome, O::Watching);
    QCOMPARE(r.client, u"p2"_s);
    QCOMPARE(r.fps, 24);
    QCOMPARE(r.width, 640);
    QCOMPARE(VrSpectator::interpretWatch(200, R"({"client":"p2","on":false})", {}).outcome, O::Stopped);
    r = VrSpectator::interpretWatch(404, R"({"detail":"No headset page is connected in this session."})", {});
    QCOMPARE(r.outcome, O::NoHeadset);
    QVERIFY(!r.message.isEmpty());
    // An older backend: no such route, not "no headset".
    QCOMPARE(VrSpectator::interpretWatch(404, R"({"detail":"Not Found"})", {}).outcome, O::Unsupported);
    r = VrSpectator::interpretWatch(422, R"({"detail":"fps"})", {});
    QCOMPARE(r.outcome, O::Failed);
    QVERIFY(r.message.contains(u"422"_s));
    QVERIFY(VrSpectator::interpretWatch(0, {}, u"Connection refused"_s).message.contains(u"Connection refused"_s));
}

void TestVrSpectator::frameRepliesAreParsed()
{
    using K = VrSpectator::FrameReply::Kind;
    const QByteArray jpg = jpegOf(32, 24, "red");
    auto r = VrSpectator::parseFrameReply(200, frameHeaders(7), jpg, {});
    QCOMPARE(r.kind, K::Frame);
    QCOMPARE(r.seq, qint64(7));
    QCOMPARE(r.width, 512);
    QCOMPARE(r.height, 540);
    QCOMPARE(r.eye, u"left"_s);
    QCOMPARE(r.ageMs, 18.0);
    QCOMPARE(r.costMs, 0.42);
    QCOMPARE(r.jpeg, jpg);
    // Without the cost (an older page): not a number, not 0.
    auto h = frameHeaders(8);
    h.remove("x-visivo-frame-cost-ms");
    QVERIFY(std::isnan(VrSpectator::parseFrameReply(200, h, jpg, {}).costMs));
    // Not a JPEG, or no seq: no frame.
    QCOMPARE(VrSpectator::parseFrameReply(200, frameHeaders(9), "{\"x\":1}", {}).kind, K::Failed);
    QCOMPARE(VrSpectator::parseFrameReply(200, {}, jpg, {}).kind, K::Failed);
    QCOMPARE(VrSpectator::parseFrameReply(204, {}, {}, {}).kind, K::None);
    QCOMPARE(VrSpectator::parseFrameReply(409, {}, R"({"detail":"not watched"})", {}).kind, K::NotWatched);
    QCOMPARE(VrSpectator::parseFrameReply(404, {}, R"({"detail":"Unknown headset client"})", {}).kind, K::Gone);
    QCOMPARE(VrSpectator::parseFrameReply(404, {}, R"({"detail":"Not Found"})", {}).kind, K::Unsupported);
    QCOMPARE(VrSpectator::parseFrameReply(500, {}, {}, {}).kind, K::Failed);
    QCOMPARE(VrSpectator::parseFrameReply(0, {}, {}, u"timeout"_s).kind, K::Failed);
}

void TestVrSpectator::frameUrlAndRetryDelays()
{
    const QUrl u = VrSpectator::frameUrl(u"http://h:8000"_s, u"vr-a b"_s, -3, 99);
    QCOMPARE(u.path(), u"/v1/vr/headset/spectator/frame"_s);
    const QUrlQuery q(u);
    QCOMPARE(q.queryItemValue(u"client"_s, QUrl::FullyDecoded), u"vr-a b"_s);
    QCOMPARE(q.queryItemValue(u"after"_s), u"0"_s);
    QCOMPARE(q.queryItemValue(u"wait"_s), u"5"_s);
    QCOMPARE(VrSpectator::retryDelayMs(1), 500);
    QCOMPARE(VrSpectator::retryDelayMs(3), 2000);
    QCOMPARE(VrSpectator::retryDelayMs(50), 5000);
}

void TestVrSpectator::snapshotAnswersAreUnderstood()
{
    auto r = VrSpectator::interpretSnapshot(
            200, R"({"valid":true,"workspace_filename":"headset_quest_3_1.png","output_path":"/ws/x.png","width":512,"height":540,"seq":4})",
            {});
    QVERIFY(r.ok);
    QCOMPARE(r.filename, u"headset_quest_3_1.png"_s);
    QCOMPARE(r.width, 512);
    r = VrSpectator::interpretSnapshot(409, R"({"detail":"No frame"})", {});
    QVERIFY(!r.ok);
    QCOMPARE(r.message, u"No frame from the headset yet."_s);
    QVERIFY(!VrSpectator::interpretSnapshot(404, R"({"detail":"Not Found"})", {}).ok);
    QVERIFY(VrSpectator::interpretSnapshot(500, R"({"detail":"disk full"})", {}).message.contains(u"disk full"_s));
    QCOMPARE(VrSpectator::snapshotBody(u"p1"_s).keys(), QStringList({ u"client"_s }));
}

void TestVrSpectator::rateAndStatusLine()
{
    VrSpectator::RateMeter m;
    QCOMPARE(m.fps(0), 0.0);
    for (int i = 0; i <= 10; ++i)
        m.add(i * 100);
    QCOMPARE(m.fps(1000), 10.0);
    QCOMPARE(m.fps(5000), 0.0);   // nothing in the last two seconds
    QCOMPARE(VrSpectator::statusLine(14.96, 512, 540, 23.4, 0.416),
             u"15.0 fps, 512×540, 23 ms old, headset capture 0.42 ms/frame"_s);
    QCOMPARE(VrSpectator::statusLine(0, 10, 20, std::nan(""), std::nan("")), u"0.0 fps, 10×20"_s);
}

void TestVrSpectator::feedShowsFramesAndSaysGoodbye()
{
    FakeSpectator fake;
    fake.frames << FakeSpectator::frame(1, jpegOf(64, 48, "red"), 64, 48)
                << FakeSpectator::frame(2, jpegOf(64, 48, "blue"), 64, 48);
    auto *feed = new VrSpectatorFeed(fake.url(), u"tok"_s, u"sess-1"_s, u"page-1"_s);
    QSignalSpy watching(feed, &VrSpectatorFeed::watching);
    QSignalSpy frames(feed, &VrSpectatorFeed::frameReady);
    feed->start(20, 400);
    QTRY_COMPARE(frames.size(), 2);
    QCOMPARE(watching.size(), 1);
    const QJsonObject on = fake.lastBodyOf(u"POST /v1/vr/headset/spectator "_s);
    QCOMPARE(on.value(u"on"_s).toBool(), true);
    QCOMPARE(on.value(u"fps"_s).toInt(), 20);
    QCOMPARE(on.value(u"width"_s).toInt(), 400);
    // The token and the session in the headers of every request, never in a body.
    for (const QByteArray &h : fake.heads) {
        QVERIFY(h.toLower().contains("x-visivo-token: tok"));
        QVERIFY(h.toLower().contains("x-visivo-session: sess-1"));
    }
    for (const QByteArray &b : fake.bodies)
        QVERIFY(!b.contains("tok\""));
    QCOMPARE(feed->latest().jpeg, jpegOf(64, 48, "blue"));
    QCOMPARE(feed->latest().seq, qint64(2));
    QCOMPARE(feed->latest().width, 64);
    QCOMPARE(feed->latest().costMs, 0.35);
    QCOMPARE(frames.last().first().value<VrSpectator::FrameReply>().seq, qint64(2));
    // The next read asks for frames above the last one, one read at a time.
    QTRY_VERIFY(fake.lines.last().contains("after=2"));
    // Destroyed (its window closed): the backend is told the watch is over.
    const int before = fake.count(u"POST /v1/vr/headset/spectator "_s);
    delete feed;
    QTRY_COMPARE(fake.count(u"POST /v1/vr/headset/spectator "_s), before + 1);
    QCOMPARE(fake.lastBodyOf(u"POST /v1/vr/headset/spectator "_s).value(u"on"_s).toBool(), false);
}

void TestVrSpectator::feedStartsAgainWhenTheWatchLapsedAndEndsWhenThePageGoes()
{
    FakeSpectator fake;
    fake.frames << Answer{ 409, R"({"detail":"not watched"})" }
                << FakeSpectator::frame(1, jpegOf(16, 16, "green"), 16, 16)
                << Answer{ 404, R"({"detail":"Unknown headset client in this session"})" };
    VrSpectatorFeed feed(fake.url(), u"tok"_s, u"sess-1"_s, u"page-1"_s);
    QSignalSpy frames(&feed, &VrSpectatorFeed::frameReady);
    QSignalSpy ended(&feed, &VrSpectatorFeed::ended);
    feed.start();
    QTRY_COMPARE(ended.size(), 1);
    QCOMPARE(fake.count(u"POST /v1/vr/headset/spectator "_s), 2);   // started again after the 409
    QCOMPARE(frames.size(), 1);
    QVERIFY(!feed.running());
    QVERIFY(ended.first().first().toString().contains(u"no longer connected"_s));

    // A backend without the route: it ends at once, saying so.
    FakeSpectator old;
    old.watchStatus = 404;
    old.watchBody = R"({"detail":"Not Found"})";
    VrSpectatorFeed feed2(old.url(), u"tok"_s, u"sess-1"_s, u"page-1"_s);
    QSignalSpy ended2(&feed2, &VrSpectatorFeed::ended);
    feed2.start();
    QTRY_COMPARE(ended2.size(), 1);
    QVERIFY(ended2.first().first().toString().contains(u"older"_s));
    QCOMPARE(old.count(u"GET "_s), 0);
}

void TestVrSpectator::feedSavesASnapshot()
{
    FakeSpectator fake;
    VrSpectatorFeed feed(fake.url(), u"tok"_s, u"sess-1"_s, u"page-1"_s);
    QSignalSpy saved(&feed, &VrSpectatorFeed::snapshotSaved);
    QSignalSpy failed(&feed, &VrSpectatorFeed::snapshotFailed);
    feed.snapshot();
    QTRY_COMPARE(saved.size(), 1);
    QCOMPARE(saved.first().first().value<VrSpectator::SnapshotResult>().filename, u"headset_quest_3.png"_s);
    QCOMPARE(fake.lastBodyOf(u"POST /v1/vr/headset/spectator/snapshot "_s).value(u"client"_s).toString(), u"page-1"_s);
    fake.snapshotStatus = 409;
    fake.snapshotBody = R"({"detail":"No frame of this headset page to save: watch it first."})";
    feed.snapshot();
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed.first().first().toString(), u"No frame from the headset yet."_s);
}

void TestVrSpectator::feedSendsTheWatchAgainWhenNothingComes()
{
    // Only 204s (the page paused on a bad link): after the rewatch time the
    // watch goes again, while the reads go on, one at a time.
    FakeSpectator fake;
    VrSpectatorFeed feed(fake.url(), u"tok"_s, u"sess-1"_s, u"page-1"_s);
    feed.setRewatchMs(300);
    QSignalSpy ended(&feed, &VrSpectatorFeed::ended);
    feed.start();
    QTRY_VERIFY_WITH_TIMEOUT(fake.count(u"POST /v1/vr/headset/spectator "_s) >= 2, 5000);
    QVERIFY(feed.running());
    QCOMPARE(ended.size(), 0);
    QCOMPARE(fake.lastBodyOf(u"POST /v1/vr/headset/spectator "_s).value(u"on"_s).toBool(), true);
    // A frame resets the wait: no rewatch right after it.
    fake.frames << FakeSpectator::frame(1, jpegOf(8, 8, "x"), 8, 8);
    QSignalSpy frames(&feed, &VrSpectatorFeed::frameReady);
    QTRY_COMPARE(frames.size(), 1);
    const int posts = fake.count(u"POST /v1/vr/headset/spectator "_s);
    QTest::qWait(150);
    QCOMPARE(fake.count(u"POST /v1/vr/headset/spectator "_s), posts);
}

void TestVrSpectator::anOffNeverOvertakesAnOnOnItsWay()
{
    // The window closed right after it opened: the "on" is still on its way
    // (answered 400 ms later). The "off" must reach the backend after it, or
    // the "on" handled last would start the watch again.
    FakeSpectator fake;
    fake.watchOnDelayMs = 400;
    auto *feed = new VrSpectatorFeed(fake.url(), u"tok"_s, u"sess-1"_s, u"page-1"_s);
    feed->start();
    QTRY_COMPARE(fake.count(u"POST /v1/vr/headset/spectator "_s), 1);
    delete feed;                          // stop(): the "off"
    QTRY_COMPARE_WITH_TIMEOUT(fake.count(u"POST /v1/vr/headset/spectator "_s), 2, 5000);
    QVERIFY(fake.onAnswered);
    QVERIFY(!fake.offBeforeOnAnswered);
    QCOMPARE(fake.lastBodyOf(u"POST /v1/vr/headset/spectator "_s).value(u"on"_s).toBool(), false);
    // Nothing on its way: the "off" goes at once.
    FakeSpectator quick;
    VrSpectatorFeed feed2(quick.url(), u"tok"_s, u"sess-1"_s, u"page-1"_s);
    QSignalSpy watching(&feed2, &VrSpectatorFeed::watching);
    feed2.start();
    QTRY_COMPARE(watching.size(), 1);
    feed2.stop();
    QTRY_COMPARE(quick.count(u"POST /v1/vr/headset/spectator "_s), 2);
}

void TestVrSpectator::bodiesMatchTheBackendsRequestModels()
{
    const QString source = backendSource(u"/v1/vr/headset/spectator/frame"_s);
    if (source.isEmpty())
        QSKIP("the backend's spectator routes (#281) are not in this tree");
    const QSet<QString> watch = modelFields(source, u"SpectatorRequest"_s);
    for (const QString &k : VrSpectator::watchBody(u"p"_s, true).keys())
        QVERIFY2(watch.contains(k), qPrintable(k));
    const QSet<QString> snap = modelFields(source, u"SpectatorSnapshotRequest"_s);
    for (const QString &k : VrSpectator::snapshotBody(u"p"_s, u"b"_s).keys())
        QVERIFY2(snap.contains(k), qPrintable(k));
    // The query names and fields the desktop reads, in the routes.
    for (const QString &name : { u"after"_s, u"wait"_s, u"workspace_filename"_s, u"RH.X_FRAME_SEQ"_s,
                                 u"RH.X_FRAME_COST_MS"_s, u"RH.X_INSTANCE"_s })
        QVERIFY2(source.contains(name), qPrintable(name));
    // The headers, declared where the backend names them (response_headers.py).
    const QString declared = backendSource(u"KNOWN_RESPONSE_HEADERS"_s);
    for (const QString &name : { u"X-Visivo-Frame-Seq"_s, u"X-Visivo-Frame-Width"_s, u"X-Visivo-Frame-Height"_s,
                                 u"X-Visivo-Frame-Eye"_s, u"X-Visivo-Frame-Age-Ms"_s, u"X-Visivo-Frame-Cost-Ms"_s,
                                 u"X-Visivo-Instance"_s })
        QVERIFY2(declared.contains(u"\"%1\""_s.arg(name)), qPrintable(name));
}
