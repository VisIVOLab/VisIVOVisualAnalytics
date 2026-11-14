#include <algorithm>
#include "TestVrHeadsetHub.h"

#include "BackendClient.h"
#include "HeadsetHubLink.h"
#include "VrHeadsetHub.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace Qt::StringLiterals;

#ifndef VISIVO_REPO_ROOT
#    define VISIVO_REPO_ROOT "."
#endif

namespace {

/// A backend that answers the hub's three routes from the fields below, and
/// FastAPI's bare 404 to anything else (or to everything, with oldBackend).
class FakeHub : public QObject
{
public:
    FakeHub()
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
                    if (oldBackend) {
                        // as it is
                    } else if (line.startsWith("GET /v1/vr/headset ")) {
                        code = 200;
                        payload = headsets;
                    } else if (line.startsWith("POST /v1/vr/headset/command ")) {
                        code = commandStatus;
                        payload = commandBody;
                    } else if (line.startsWith("GET /v1/vr/activity?")) {
                        code = 200;
                        const bool first = line.contains("after=0&");
                        payload = first ? activity
                                        : QByteArray(R"({"events":[],"next":)") + activityNext
                                        + R"(,"instance":")" + instance + "\"}";
                    }
                    s->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                             + "Content-Type: application/json\r\nContent-Length: "
                             + QByteArray::number(payload.size())
                             + "\r\nConnection: close\r\n\r\n" + payload);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }
    int find(const QString &prefix) const
    {
        for (int i = 0; i < lines.size(); ++i)
            if (lines.at(i).startsWith(prefix))
                return i;
        return -1;
    }

    QTcpServer server;
    QStringList lines;
    QList<QByteArray> heads;
    QList<QByteArray> bodies;
    bool oldBackend{ false };
    QByteArray headsets = R"({"headsets":[]})";
    int commandStatus{ 200 };
    QByteArray commandBody = R"({"seq":1,"client":"page-1"})";
    QByteArray activity = R"({"events":[],"next":0})";
    QByteArray activityNext = "0";
    QByteArray instance = "inst-1";
};

VrHub::Event hubEvent(qint64 seq, const QString &kind, const QString &cube, int epoch = 0)
{
    VrHub::Event e;
    e.seq = seq;
    e.epoch = epoch;
    e.kind = kind;
    e.cubePath = cube;
    e.client = u"page-1"_s;
    return e;
}

VrHub::Event productEvent(const char *payloadJson)
{
    VrHub::Event e = hubEvent(7, u"product"_s, u"/d/m33.fits"_s);
    e.summary = u"Moment 0"_s;
    e.payload = QJsonDocument::fromJson(QByteArray(payloadJson)).object();
    return e;
}

QString backendFileWith(const QString &needle)
{
    QDirIterator it(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/backend/app"_s, { u"*.py"_s },
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.contains(u"/static/"_s))
            continue;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).contains(needle))
            return path;
    }
    return {};
}

/// The fields of every `class X(BaseModel):` in @p source.
QList<QSet<QString>> modelFields(const QString &source)
{
    QList<QSet<QString>> models;
    static const QRegularExpression klass(u"^class (\\w+)\\(BaseModel\\):"_s,
                                          QRegularExpression::MultilineOption);
    static const QRegularExpression field(u"^    (\\w+)\\s*:"_s, QRegularExpression::MultilineOption);
    for (auto it = klass.globalMatch(source); it.hasNext();) {
        const auto m = it.next();
        const qsizetype start = m.capturedEnd();
        static const QRegularExpression nextTop(u"^\\S"_s, QRegularExpression::MultilineOption);
        const auto stop = nextTop.match(source, start + 1);
        const QString body = source.mid(start, stop.hasMatch() ? stop.capturedStart() - start : -1);
        QSet<QString> fields;
        for (auto f = field.globalMatch(body); f.hasNext();)
            fields.insert(f.next().captured(1));
        models.append(fields);
    }
    return models;
}

QSet<QString> keysOf(const QJsonObject &o)
{
    QSet<QString> keys;
    for (auto it = o.begin(); it != o.end(); ++it)
        keys.insert(it.key());
    return keys;
}

} // namespace

void TestVrHeadsetHub::presenceIsParsedNewestFirst()
{
    const QByteArray body = R"({"headsets":[
        {"client":"p2","transport":"pair","label":"Quest 3","since":1763110000,"last_seen":"2026-11-14T10:00:05Z",
         "in_vr":true,"cube":{"dataset_id":"ds1","path":"/d/m33.fits","kind":"cube","box":null},"staged_cube":null},
        {"client":"p1","transport":"adb","in_vr":false,"cube":null},
        {"transport":"lan"}]})";
    const VrHub::Presence p = VrHub::parsePresence(200, body);
    QVERIFY(p.ok);
    QCOMPARE(p.headsets.size(), 2);   // the one without a client is skipped
    const VrHub::Headset &h = p.headsets.first();
    QCOMPARE(h.client, u"p2"_s);
    QVERIFY(h.inVr);
    QCOMPARE(h.cube.datasetId, u"ds1"_s);
    QCOMPARE(h.sinceMs, qint64(1763110000) * 1000);
    QCOMPARE(h.lastSeenMs, QDateTime(QDate(2026, 11, 14), QTime(10, 0, 5), QTimeZone::UTC).toMSecsSinceEpoch());
    QVERIFY(!p.headsets.at(1).cube.isSet());
    const VrHub::Presence boxed = VrHub::parsePresence(200, R"({"headsets":[{"client":"p","cube":
        {"dataset_id":"d","path":"/c.fits","kind":"cube","box":[[0,9],[0,9],[2,5]]}}]})");
    QCOMPARE(boxed.headsets.first().cube.box, u"[[0,9],[0,9],[2,5]]"_s);
    QVERIFY(VrHub::showsCube(h, u"ds1"_s, QString()));
    QVERIFY(VrHub::showsCube(h, u"other"_s, u"/d/m33.fits"_s));
    QVERIFY(!VrHub::showsCube(p.headsets.at(1), u"ds1"_s, u"/d/m33.fits"_s));
}

void TestVrHeadsetHub::anOldBackendIsNotAMissingHeadset()
{
    const VrHub::Presence old = VrHub::parsePresence(404, R"({"detail":"Not Found"})");
    QVERIFY(old.unsupported);
    QVERIFY(!old.ok);
    const VrHub::Presence down = VrHub::parsePresence(0, {}, u"Connection refused"_s);
    QVERIFY(!down.unsupported);
    QCOMPARE(down.error, u"Connection refused"_s);
    QVERIFY(!VrHub::parsePresence(200, "not json").ok);
}

void TestVrHeadsetHub::statusTextSaysWhatIsOnShow()
{
    QVERIFY(VrHub::statusText({}).isEmpty());
    VrHub::Headset h;
    h.client = u"p"_s;
    h.transport = u"pair"_s;
    QCOMPARE(VrHub::statusText({ h }), u"Headset: connected"_s);
    h.inVr = true;
    h.cube.path = u"/d/m33.fits"_s;
    QCOMPARE(VrHub::statusText({ h }), u"Headset: in VR · m33.fits"_s);
    QCOMPARE(VrHub::statusText({ h, h }), u"Headset: in VR · m33.fits (+1 more)"_s);
    QCOMPARE(VrHub::headsetLine(h), u"Headset (paired by code), in VR, showing m33.fits"_s);
    h.cube = {};
    h.staged.path = u"/d/n.fits"_s;
    h.inVr = false;
    h.label = u"Quest 3"_s;
    h.transport = u"adb"_s;
    QCOMPARE(VrHub::headsetLine(h), u"Quest 3 (USB cable), not in VR, loading n.fits"_s);
}

void TestVrHeadsetHub::longNamesGetBreakPoints()
{
    const QChar zwsp(0x200B);
    // After each separator; the text reads the same once the breaks go.
    const QString name = u"WALLABY_J100903-290239_Hydra_TR2.fits"_s;
    const QString w = VrHub::wrappable(name);
    QCOMPARE(QString(w).remove(zwsp), name);
    QVERIFY(w.contains(u"WALLABY_"_s + zwsp));
    QVERIFY(w.contains(u"TR2."_s + zwsp));
    // A stem with no separators still breaks: no run of 16 unbroken characters.
    const QString stem = QString(150, u'a') + u".fits"_s;
    const QString ws = VrHub::wrappable(stem);
    QCOMPARE(QString(ws).remove(zwsp), stem);
    int run = 0, longest = 0;
    for (const QChar c : ws) {
        run = (c == zwsp || c.isSpace()) ? 0 : run + 1;
        longest = std::max(longest, run);
    }
    QVERIFY2(longest <= 16, qPrintable(QString::number(longest)));
    // Ordinary words are left alone; a surrogate pair is never split.
    QCOMPARE(VrHub::wrappable(u"No headset is connected"_s), u"No headset is connected"_s);
    const QString emoji = QString(15, u'b') + QString::fromUtf8("\xF0\x9F\x94\xAD") + u"cc"_s;
    const QString we = VrHub::wrappable(emoji);
    for (qsizetype k = 0; k + 1 < we.size(); ++k)
        QVERIFY(!(we[k].isHighSurrogate() && we[k + 1] == zwsp));
}

void TestVrHeadsetHub::showBodyCarriesTheCubeButNoToken()
{
    VrLink::Inputs in;
    in.token = u"secret-token"_s;
    in.sessionId = u"sess"_s;
    in.datasetId = u"ds1"_s;
    in.path = u"/d/m33.fits"_s;
    QJsonObject body = VrHub::showCommandBody(in);
    QCOMPARE(keysOf(body), (QSet<QString>{ u"type"_s, u"dataset_id"_s, u"path"_s }));
    QCOMPARE(body.value(u"type"_s).toString(), u"show"_s);
    QVERIFY(!QJsonDocument(body).toJson().contains("secret-token"));
    in.kind = u"velocity"_s;
    in.boxMpc = u"150"_s;
    body = VrHub::showCommandBody(in, u"page-1"_s);
    QCOMPARE(body.value(u"client"_s).toString(), u"page-1"_s);
    QCOMPARE(body.value(u"kind"_s).toString(), u"velocity"_s);
    // The hub's box is a voxel box: a velocity field's box in Mpc is not sent.
    QVERIFY(!body.contains(u"box"_s));
    in.kind = u"cube"_s;
    QVERIFY(!VrHub::showCommandBody(in).contains(u"kind"_s));
}

void TestVrHeadsetHub::commandAnswersAreUnderstood()
{
    using O = VrHub::CommandResult::Outcome;
    const auto sent = VrHub::interpretCommand(200, R"({"seq":4,"client":"p2"})", {});
    QCOMPARE(sent.outcome, O::Sent);
    QCOMPARE(sent.seq, qint64(4));
    QCOMPARE(sent.client, u"p2"_s);
    QCOMPARE(VrHub::interpretCommand(404, R"({"detail":"No headset page is connected."})", {}).outcome,
             O::NoHeadset);
    QCOMPARE(VrHub::interpretCommand(404, R"({"detail":"Not Found"})", {}).outcome, O::Unsupported);
    const auto busy = VrHub::interpretCommand(429, R"({"detail":"too many"})", {});
    QCOMPARE(busy.outcome, O::Failed);
    QVERIFY(busy.message.contains(u"429"_s));
    QVERIFY(busy.message.contains(u"too many"_s));
    QVERIFY(VrHub::interpretCommand(0, {}, u"timeout"_s).message.contains(u"timeout"_s));
}

void TestVrHeadsetHub::activityIsParsed()
{
    const QByteArray body = R"({"events":[
        {"seq":3,"client":"p","time":1763110000.5,"kind":"product","cube_path":"/d/m33.fits",
         "summary":"Moment 0","payload":{"product":"moment","path":"/w/m33_m0.fits"}},
        {"seq":0,"kind":"look"},
        {"seq":2,"client":"p","time":"2026-11-14T10:00:00Z","kind":"look","payload":{"colormap":"magma"}}],
        "next":3,"lost":true,"instance":"abc"})";
    const VrHub::ActivityPage page = VrHub::parseActivity(200, body);
    QVERIFY(page.ok);
    QCOMPARE(page.next, qint64(3));
    QVERIFY(page.lost);
    QCOMPARE(page.instance, u"abc"_s);
    QCOMPARE(page.events.size(), 2);   // seq 0 is not an event
    QCOMPARE(page.events.first().timeMs, qint64(1763110000500));
    QCOMPARE(page.events.first().payload.value(u"product"_s).toString(), u"moment"_s);
    QVERIFY(VrHub::parseActivity(404, R"({"detail":"Not Found"})").unsupported);
    QVERIFY(!VrHub::parseActivity(500, "{}").ok);
    QVERIFY(VrHub::eventRowText(page.events.first()).endsWith(u"  Moment 0"_s));
    QVERIFY(VrHub::eventRowText(hubEvent(1, u"clip"_s, {})).endsWith(u"Clip box changed"_s));
}

void TestVrHeadsetHub::lookAndClipAreCoalescedPerCube()
{
    VrHub::ActivityLog log;
    QVERIFY(log.add({ hubEvent(1, u"look"_s, u"/a"_s), hubEvent(2, u"look"_s, u"/b"_s),
                      hubEvent(3, u"product"_s, u"/a"_s), hubEvent(4, u"clip"_s, u"/a"_s) }));
    QCOMPARE(log.events().size(), 4);
    // A newer look of /a replaces the older; /b's stays.
    QVERIFY(log.add({ hubEvent(5, u"look"_s, u"/a"_s) }));
    QCOMPARE(log.events().size(), 4);
    QCOMPARE(log.latest(u"look"_s, u"/a"_s)->seq, qint64(5));
    QCOMPARE(log.latest(u"look"_s, u"/b"_s)->seq, qint64(2));
    // An older one arriving late does not come back; a repeat changes nothing.
    QVERIFY(!log.add({ hubEvent(1, u"look"_s, u"/a"_s) }));
    QVERIFY(!log.add({ hubEvent(3, u"product"_s, u"/a"_s) }));
    // Products are never coalesced.
    QVERIFY(log.add({ hubEvent(6, u"product"_s, u"/a"_s) }));
    QCOMPARE(log.events().first().seq, qint64(6));
    QVERIFY(!log.latest(u"look"_s, u"/c"_s));

    // Without a cube_path, the payload's dataset id names the cube; a look
    // naming no cube at all is never coalesced away.
    VrHub::ActivityLog byId;
    VrHub::Event a = hubEvent(1, u"look"_s, QString());
    a.payload.insert(u"dataset_id"_s, u"ds1"_s);
    VrHub::Event b = hubEvent(2, u"look"_s, QString());
    b.payload.insert(u"dataset_id"_s, u"ds2"_s);
    VrHub::Event c = hubEvent(3, u"look"_s, QString());
    c.payload.insert(u"dataset_id"_s, u"ds1"_s);
    QVERIFY(byId.add({ a, b, hubEvent(4, u"look"_s, QString()), hubEvent(5, u"look"_s, QString()) }));
    QCOMPARE(byId.events().size(), 4);
    QVERIFY(byId.add({ c }));
    QCOMPARE(byId.events().size(), 4);   // ds1's older look went, ds2's stayed
    QCOMPARE(byId.latest(u"look"_s, u"dataset:ds1"_s)->seq, qint64(3));
    QCOMPARE(byId.latest(u"look"_s, u"dataset:ds2"_s)->seq, qint64(2));
}

void TestVrHeadsetHub::theListIsCappedNewestFirst()
{
    VrHub::ActivityLog log;
    QList<VrHub::Event> many;
    for (int i = 1; i <= VrHub::kActivityCapacity + 10; ++i)
        many.append(hubEvent(i, u"product"_s, u"/a"_s));
    QVERIFY(log.add(many));
    QCOMPARE(log.events().size(), VrHub::kActivityCapacity);
    QCOMPARE(log.events().first().seq, qint64(VrHub::kActivityCapacity + 10));
    QCOMPARE(log.events().last().seq, qint64(11));
}

void TestVrHeadsetHub::aRestartedBackendsEventsComeFirst()
{
    VrHub::ActivityLog log;
    log.add({ hubEvent(40, u"product"_s, u"/a"_s, 0) });
    log.add({ hubEvent(1, u"product"_s, u"/a"_s, 1) });
    QCOMPARE(log.events().size(), 2);
    QCOMPARE(log.events().first().epoch, 1);
    // The same seq in another epoch is another event.
    QVERIFY(log.add({ hubEvent(40, u"product"_s, u"/a"_s, 1) }));
}

void TestVrHeadsetHub::productsOpenWhereTheyBelong()
{
    using VrHub::OpenIn;
    const auto openOf = [](const char *json) { return VrHub::openIn(VrHub::parseProduct(productEvent(json))); };
    QCOMPARE(openOf(R"({"product":"moment","path":"/w/m0.fits"})"), OpenIn::ImageViewer);
    QCOMPARE(openOf(R"({"product":"moment"})"), OpenIn::Nothing);
    QCOMPARE(openOf(R"({"product":"pv","request":{"dataset_id":"ds1","vertices":[[1,2],[30,40]],"width_pixels":3}})"),
             OpenIn::PvViewer);
    QCOMPARE(openOf(R"({"product":"pv","output_path":"/w/pv.fits"})"), OpenIn::PvViewer);
    QCOMPARE(openOf(R"({"product":"spectrum","path":"/w/s.csv"})"), OpenIn::SpectrumPlot);
    QCOMPARE(openOf(R"({"product":"spectrum","values":[1,2,3],"axis":[0,1,2]})"), OpenIn::SpectrumPlot);
    QCOMPARE(openOf(R"({"product":"isolate","path":"/w/iso.fits"})"), OpenIn::CubeWindow);
    QCOMPARE(openOf(R"({"type":"remove","path":"/w/rem.fits"})"), OpenIn::CubeWindow);
    QCOMPARE(openOf(R"({"product":"catalogue","path":"/w/cat.csv"})"), OpenIn::File);
    QCOMPARE(openOf(R"({"product":"teleport"})"), OpenIn::Nothing);

    const VrHub::Product pv = VrHub::parseProduct(
            productEvent(R"({"product":"pv","request":{"vertices":[[1,2],[30,40.5]],"width_pixels":3}})"));
    const auto v = VrHub::pvVertices(pv);
    QCOMPARE(int(v.size()), 2);
    QCOMPARE(v.at(1)[1], 40.5);
    QCOMPARE(VrHub::pvWidth(pv), 3);
    QCOMPARE(VrHub::parseProduct(productEvent(R"({"product":"moment","path":"/w/x/m0.fits"})")).workspaceFilename,
             u"m0.fits"_s);
    // As the hub sends it: the page's file name, and the path the backend added.
    const VrHub::Product saved = VrHub::parseProduct(
            productEvent(R"({"product":"moment","file":"m33_m0.fits","output_path":"/srv/exports/m33_m0.fits"})"));
    QCOMPARE(saved.path, u"/srv/exports/m33_m0.fits"_s);
    QCOMPARE(saved.workspaceFilename, u"m33_m0.fits"_s);
    QCOMPARE(VrHub::openIn(saved), OpenIn::ImageViewer);
}

void TestVrHeadsetHub::productRecipeBecomesProvenance()
{
    const VrHub::Product p = VrHub::parseProduct(productEvent(R"({"product":"pv","path":"/w/pv.fits",
        "recipe":{"schema":"visivo-recipe/1","product":{"id":"vr-pv-2","kind":"pv","label":"PV diagram",
          "replay":[{"route":"/v1/cube/pv","request":{"vertices":[[1,2],[3,4]],"width_pixels":1}},{"request":{}}],
          "replay_caveat":"none"},
          "parameters":{"Width":"1 px"},"origin":{"kind":"pv"}}})"));
    QCOMPARE(p.label, u"PV diagram"_s);
    QCOMPARE(p.recipeId, u"vr-pv-2"_s);
    QCOMPARE(p.replay.size(), 1);   // a step without a route is dropped
    QCOMPARE(p.replay.first().route, u"/v1/cube/pv"_s);
    QCOMPARE(p.replay.first().request.value(u"width_pixels"_s).toInt(), 1);
    QCOMPARE(p.caveat, u"none"_s);
    QCOMPARE(p.parameters.value(u"Width"_s).toString(), u"1 px"_s);
    QCOMPARE(p.origin.value(u"kind"_s).toString(), u"pv"_s);
    // No label anywhere: the event's summary.
    QCOMPARE(VrHub::parseProduct(productEvent(R"({"product":"moment"})")).label, u"Moment 0"_s);
}

void TestVrHeadsetHub::productIdIsStableAcrossOpens()
{
    const VrHub::Event e = productEvent(R"({"product":"pv","recipe":{"product":{"id":"vr-pv-2"}}})");
    const VrHub::Product p = VrHub::parseProduct(e);
    QCOMPARE(VrHub::productId(e, p), u"headset-page-1-vr-pv-2"_s);
    QCOMPARE(VrHub::productId(e, p), VrHub::productId(e, VrHub::parseProduct(e)));
    const VrHub::Event bare = productEvent(R"({"product":"moment"})");
    QCOMPARE(VrHub::productId(bare, VrHub::parseProduct(bare)), u"headset-page-1-e0-7"_s);
}

void TestVrHeadsetHub::aMomentOfASelectionNamesItsMask()
{
    // The page's moment of a lasso selection: grow, isolate, moment, with
    // "Mask: none" (no threshold) in its parameters (issue #249).
    const VrHub::Product sel = VrHub::parseProduct(productEvent(R"({"product":"moment",
        "path":"/w/c_vr_m0.fits","file":"c_vr_m0.fits",
        "recipe":{"product":{"id":"vr-m0","kind":"moment","replay":[
          {"route":"/v1/cube/kinematic_select_bin","request":{"seeds":[[1,2,3]]}},
          {"route":"/v1/cube/mask_apply_bin","request":{"mode":"blank_outside"}},
          {"route":"/v1/products/moment","request":{"dataset_id":"{new_dataset_id}"}}]},
          "parameters":{"Order":"Moment 0","Mask":"none"}}})"));
    QCOMPARE(VrHub::selectionMeasured(sel), u"the Kinematic Lasso selection"_s);
    const QVariantMap params = VrHub::provenanceParams(sel);
    QCOMPARE(params.value(u"Mask"_s).toString(),
             u"the Kinematic Lasso selection (outside blanked); no threshold"_s);
    QCOMPARE(params.value(u"Made in"_s).toString(), u"the VR headset"_s);
    QCOMPARE(params.value(u"File"_s).toString(), u"c_vr_m0.fits"_s);
    QCOMPARE(params.value(u"Order"_s).toString(), u"Moment 0"_s);

    // A selection received as a mask only: still a selection, not a lasso grow.
    const VrHub::Product maskOnly = VrHub::parseProduct(productEvent(R"({"product":"moment",
        "recipe":{"product":{"replay":[{"route":"/v1/cube/mask_apply_bin","request":{"mode":"blank_outside"}},
          {"route":"/v1/products/moment","request":{}}]},"parameters":{"Mask":"none"}}})"));
    QCOMPARE(VrHub::provenanceParams(maskOnly).value(u"Mask"_s).toString(),
             u"the selection (outside blanked); no threshold"_s);

    // The whole cube, or a removed selection: the record's own words stay.
    const VrHub::Product whole = VrHub::parseProduct(productEvent(R"({"product":"moment",
        "recipe":{"product":{"replay":[{"route":"/v1/products/moment","request":{}}]},
          "parameters":{"Mask":"none"}}})"));
    QVERIFY(VrHub::selectionMeasured(whole).isEmpty());
    QCOMPARE(VrHub::provenanceParams(whole).value(u"Mask"_s).toString(), u"none"_s);
    QVERIFY(!VrHub::provenanceParams(whole).contains(u"File"_s));
    const VrHub::Product removed = VrHub::parseProduct(productEvent(R"({"product":"moment",
        "recipe":{"product":{"replay":[{"route":"/v1/cube/mask_apply_bin","request":{"mode":"blank_inside"}},
          {"route":"/v1/products/moment","request":{}}]},"parameters":{"Mask":"none"}}})"));
    QCOMPARE(VrHub::provenanceParams(removed).value(u"Mask"_s).toString(), u"none"_s);
    // A threshold the page did set is not overwritten.
    const VrHub::Product thresholded = VrHub::parseProduct(productEvent(R"({"product":"moment",
        "recipe":{"product":{"replay":[{"route":"/v1/cube/mask_apply_bin","request":{"mode":"blank_outside"}}]},
          "parameters":{"Mask":">= 0.5"}}})"));
    QCOMPARE(VrHub::provenanceParams(thresholded).value(u"Mask"_s).toString(), u">= 0.5"_s);
}

void TestVrHeadsetHub::spectrumFileIsRead()
{
    QVector<double> x, y;
    QString xl, yl;
    QVERIFY(VrHub::parseSpectrumCsv("# made in the headset\nvelocity_kms,flux_jy\n-10,0.5\n0,nan\n10,1.5\n",
                                    x, y, &xl, &yl));
    QCOMPARE(x, (QVector<double>{ -10, 0, 10 }));
    QCOMPARE(y.size(), 3);
    QVERIFY(std::isnan(y.at(1)));
    QCOMPARE(xl, u"velocity_kms"_s);
    QCOMPARE(yl, u"flux_jy"_s);
    QVERIFY(VrHub::parseSpectrumCsv("1 2\n3 4\n", x, y));
    QVERIFY(!VrHub::parseSpectrumCsv("only,a,header\n", x, y));
}

namespace {

// Written by the backend's own worker_spectrum_table (format=fits) on a
// 6-channel VRAD cube (CRVAL3 1000 m/s, CDELT3 5000 m/s, BUNIT Jy/beam) at
// pixel (1, 2), whose channel 3 is blank there: radius 0 and radius 1.5.
QByteArray spectrumFixture(const char *name)
{
    QFile f(QStringLiteral(VISIVO_TEST_DATA_DIR "/vr/") + QString::fromLatin1(name));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

}   // namespace

void TestVrHeadsetHub::spectrumFitsTableIsReadLikeTheCsv()
{
    const QByteArray fits = spectrumFixture("spectrum_table_pixel.fits");
    QVERIFY(!fits.isEmpty());
    QVector<double> x, y;
    QString xl, yl, error;
    QVERIFY2(VrHub::parseSpectrumFile(fits, x, y, &xl, &yl, &error), qPrintable(error));
    QCOMPARE(x, (QVector<double>{ 1000, 6000, 11000, 16000, 21000, 26000 }));
    QCOMPARE(y.size(), 6);
    QVERIFY(std::isnan(y.at(3)));
    QCOMPARE(y.at(0), 5.5);
    QCOMPARE(y.at(5), 55.5);

    // The same spectrum in the CSV form the same worker writes: one product.
    const QByteArray csv = "# VisIVO spectrum export\n# dataset: tiny_cube.fits\n# pixel:   (1, 2)\n"
                           "# samples: 6\nVRAD (m / s),Intensity (Jy/beam)\n1000,5.5\n6000,15.5\n"
                           "11000,25.5\n16000,nan\n21000,45.5\n26000,55.5\n";
    QVector<double> cx, cy;
    QString cxl, cyl;
    QVERIFY(VrHub::parseSpectrumFile(csv, cx, cy, &cxl, &cyl));
    QCOMPARE(xl, cxl);
    QCOMPARE(yl, cyl);
    QCOMPARE(x, cx);
    QCOMPARE(y.size(), cy.size());
    for (int i = 0; i < y.size(); ++i)
        QVERIFY(std::isnan(y.at(i)) ? std::isnan(cy.at(i)) : y.at(i) == cy.at(i));
}

void TestVrHeadsetHub::apertureFitsTableIsAMean()
{
    QVector<double> x, y;
    QString xl, yl;
    QVERIFY(VrHub::parseSpectrumFile(spectrumFixture("spectrum_table_aperture.fits"), x, y, &xl, &yl));
    QCOMPARE(xl, u"VRAD (m / s)"_s);
    QCOMPARE(yl, u"Mean intensity (Jy/beam)"_s);
    QCOMPARE(y.size(), 6);
    QCOMPARE(y.at(3), 35.5);   // the blank pixel is left out of the mean
}

void TestVrHeadsetHub::brokenFitsTablesAreRefused()
{
    const QByteArray fits = spectrumFixture("spectrum_table_pixel.fits");
    QVERIFY(!fits.isEmpty());
    QVector<double> x, y;
    QString error;
    // Cut inside the table's data.
    QVERIFY(!VrHub::parseSpectrumFile(fits.left(2 * 2880 + 40), x, y, nullptr, nullptr, &error));
    QVERIFY(y.isEmpty());
    QVERIFY(!error.isEmpty());
    // The primary header alone: no table.
    error.clear();
    QVERIFY(!VrHub::parseSpectrumFile(fits.left(2880), x, y, nullptr, nullptr, &error));
    QVERIFY(!error.isEmpty());
    // No END card anywhere.
    QVERIFY(!VrHub::parseSpectrumFile(QByteArray("SIMPLE  =                    T").leftJustified(2880, ' '),
                                      x, y));
    // A text column is never read as numbers: without SPECTRAL the table is
    // read by its first two numeric columns, and labelled by their names.
    QByteArray text = fits;
    const int at = text.indexOf("TFORM2  = 'D       '");
    QVERIFY(at > 0);
    text.replace(at, 20, "TFORM2  = '8A      '");
    QString xl, yl;
    QVERIFY(VrHub::parseSpectrumFile(text, x, y, &xl, &yl));
    QCOMPARE(x, (QVector<double>{ 0, 1, 2, 3, 4, 5 }));
    QCOMPARE(xl, u"CHANNEL"_s);
    QCOMPARE(yl, u"VALUE (Jy/beam)"_s);
    // With VALUE a text column too, nothing is left to plot.
    const int at3 = text.indexOf("TFORM3  = 'D       '");
    QVERIFY(at3 > 0);
    text.replace(at3, 20, "TFORM3  = '8A      '");
    QVERIFY(!VrHub::parseSpectrumFile(text, x, y));

    // Sizes a header makes up are refused before they are used: none of them
    // may overflow into a read outside the file.
    const auto withCard = [](QByteArray f, int from, const char *key, const QByteArray &value) {
        for (int p = from; p + 80 <= f.size(); p += 80)
            if (f.mid(p, 8).trimmed() == key) {
                f.replace(p, 80, (QByteArray(key).leftJustified(8, ' ') + "= " + value).leftJustified(80, ' '));
                break;
            }
        return f;
    };
    QVERIFY(!VrHub::parseSpectrumFile(withCard(fits, 0, "BITPIX", "9223372036854775807"), x, y));
    QVERIFY(!VrHub::parseSpectrumFile(withCard(fits, 2880, "TFORM1", "'9223372036854775807A'"), x, y));
    QVERIFY(!VrHub::parseSpectrumFile(withCard(fits, 2880, "TFORM1", "'99999999999999999999A'"), x, y));
    QByteArray huge = withCard(fits, 2880, "NAXIS", "0");
    huge = withCard(huge, 2880, "NAXIS1", "4611686018427387904");
    huge = withCard(huge, 2880, "NAXIS2", "4");
    QVERIFY(!VrHub::parseSpectrumFile(huge, x, y));
    QVERIFY(!VrHub::parseSpectrumFile(withCard(fits, 2880, "NAXIS2", "4611686018427387904"), x, y));
    QVERIFY(y.isEmpty());
}

void TestVrHeadsetHub::lookAndClipAreRead()
{
    const VrHub::Look l = VrHub::parseLook(QJsonDocument::fromJson(
            R"({"colormap":"magma","threshold":0.012,"color_max":0.4,"opacity":0.8,"gamma":1.2,"mode":1,"stretch":"log"})")
                                                   .object());
    QCOMPARE(l.colormap, u"magma"_s);
    QCOMPARE(*l.threshold, 0.012);
    QCOMPARE(*l.colourMax, 0.4);
    QCOMPARE(l.mode, u"1"_s);
    QCOMPARE(l.stretch, u"log"_s);
    QVERIFY(!VrHub::parseLook(QJsonObject{ { u"threshold"_s, u"high"_s } }).threshold);

    const auto clipOf = [](const char *json) {
        return VrHub::parseClip(QJsonDocument::fromJson(QByteArray(json)).object());
    };
    VrHub::Clip c = clipOf(R"({"box":{"min":[1,2,3],"max":[10,20,30]}})");
    QVERIFY(c.set);
    QCOMPARE(c.max[2], 30.0);
    c = clipOf(R"({"box":[10,1,2,20,3,30]})");   // ends in either order
    QVERIFY(c.set);
    QCOMPARE(c.min[0], 1.0);
    QCOMPARE(c.max[0], 10.0);
    c = clipOf(R"({"box":[[1,10],[2,20],[3,30]]})");
    QVERIFY(c.set);
    QCOMPARE(c.min[1], 2.0);
    QVERIFY(clipOf(R"({"min":[0,0,0],"max":[5,5,5]})").set);
    QVERIFY(!clipOf(R"({"box":null})").set);
    QVERIFY(!clipOf(R"({"box":[1,2,3]})").set);
    QVERIFY(!clipOf(R"({"box":{"min":[1,2],"max":[1,2,3]}})").set);

    // The render session's set_crop box: fractions with the faces at the
    // outer pixel edges, null for no clip or the whole cube.
    const VrHub::Clip half = clipOf(R"({"box":[[0,4],[5,9],[0,19]]})");
    const QVariantMap crop = VrHub::cropCommandBox(half, { 10, 10, 20 }).toMap();
    QCOMPARE(crop.value(u"min"_s).toList().at(1).toDouble(), 0.5);
    QCOMPARE(crop.value(u"max"_s).toList().at(0).toDouble(), 0.5);
    QCOMPARE(crop.value(u"max"_s).toList().at(2).toDouble(), 1.0);
    QVERIFY(VrHub::cropCommandBox(clipOf(R"({"box":[[0,9],[0,9],[0,19]]})"), { 10, 10, 20 }).isNull());
    QVERIFY(VrHub::cropCommandBox(VrHub::Clip{}, { 10, 10, 20 }).isNull());
    // The mapper's plane for a face drawn at d: the render session's inverse
    // (d = o - s/2 + (p - o) n / (n - 1)), so drawing it gives d back.
    for (const double d : { -0.5, 1.5, 4.5, 9.5 }) {
        const double p = VrHub::gpuCropPlane(d, 0.0, 1.0, 10);
        QVERIFY(qAbs((0.0 - 0.5 + (p - 0.0) * 10.0 / 9.0) - d) < 1e-12);
    }
    QCOMPARE(VrHub::gpuCropPlane(-0.5, 0.0, 1.0, 10), 0.0);
    QCOMPARE(VrHub::gpuCropPlane(9.5, 0.0, 1.0, 10), 9.0);
    QCOMPARE(VrHub::gpuCropPlane(3.0, 2.0, 1.0, 1), 2.0);

    VrHub::Event e = hubEvent(1, u"look"_s, u"/d/m33.fits"_s);
    QVERIFY(VrHub::aboutCube(e, u"/d/m33.fits"_s, u"x"_s));
    QVERIFY(!VrHub::aboutCube(e, u"/d/other.fits"_s, u"x"_s));
    e.cubePath.clear();
    e.payload.insert(u"dataset_id"_s, u"ds1"_s);
    QVERIFY(VrHub::aboutCube(e, u"/d/other.fits"_s, u"ds1"_s));
}

void TestVrHeadsetHub::colormapsMapToTheDesktops()
{
    const QStringList desktop{ u"Inferno"_s, u"Viridis"_s, u"Magma"_s, u"Gray"_s };
    QCOMPARE(VrHub::desktopColormap(u"viridis"_s, desktop), u"Viridis"_s);
    QCOMPARE(VrHub::desktopColormap(u"grey"_s, desktop), u"Gray"_s);
    QVERIFY(VrHub::desktopColormap(u"hot"_s, desktop).isEmpty());
    QVERIFY(VrHub::desktopColormap(QString(), desktop).isEmpty());
}

void TestVrHeadsetHub::clientCallsTheHubRoutes()
{
    FakeHub fake;
    QVERIFY(fake.server.isListening());
    fake.headsets = R"({"headsets":[{"client":"p1","transport":"pair","in_vr":true}]})";
    BackendClient client(fake.url(), u"tok-xyz"_s);
    client.setSessionId(u"sess-1"_s);

    const BackendVrHubReply presence = client.vrHeadsets();
    QCOMPARE(presence.httpStatus, 200);
    QCOMPARE(VrHub::parsePresence(presence.httpStatus, presence.body).headsets.size(), 1);
    const int get = fake.find(u"GET /v1/vr/headset "_s);
    QVERIFY(get >= 0);
    QVERIFY(fake.heads.at(get).toLower().contains("x-visivo-token: tok-xyz"));
    QVERIFY(fake.heads.at(get).toLower().contains("x-visivo-session: sess-1"));

    VrLink::Inputs in;
    in.token = u"tok-xyz"_s;
    in.datasetId = u"ds1"_s;
    in.path = u"/d/m33.fits"_s;
    const BackendVrHubReply sent = client.sendVrHeadsetCommand(VrHub::showCommandBody(in, u"p1"_s));
    QCOMPARE(VrHub::interpretCommand(sent.httpStatus, sent.body, sent.error).outcome,
             VrHub::CommandResult::Outcome::Sent);
    const int post = fake.find(u"POST /v1/vr/headset/command "_s);
    QVERIFY(post >= 0);
    QVERIFY(!fake.bodies.at(post).contains("tok-xyz"));
    QCOMPARE(QJsonDocument::fromJson(fake.bodies.at(post)).object().value(u"client"_s).toString(), u"p1"_s);

    fake.commandStatus = 404;
    fake.commandBody = R"({"detail":"No headset page is connected."})";
    const BackendVrHubReply none = client.sendVrHeadsetCommand(VrHub::showCommandBody(in));
    QCOMPARE(VrHub::interpretCommand(none.httpStatus, none.body, none.error).outcome,
             VrHub::CommandResult::Outcome::NoHeadset);

    fake.activity = R"({"events":[{"seq":1,"kind":"cube","summary":"Switched"}],"next":1})";
    const BackendVrHubReply act = client.vrActivity(0, 0);
    QCOMPARE(VrHub::parseActivity(act.httpStatus, act.body).events.size(), 1);
    QVERIFY(fake.find(u"GET /v1/vr/activity?after=0&wait=0 "_s) >= 0);
}

void TestVrHeadsetHub::linkFollowsPresenceAndActivity()
{
    FakeHub fake;
    QVERIFY(fake.server.isListening());
    fake.headsets = R"({"headsets":[{"client":"p1","transport":"pair","in_vr":false}]})";
    fake.activity = R"({"events":[{"seq":1,"client":"p1","kind":"product","summary":"Moment 0",
                       "payload":{"product":"moment","path":"/w/m0.fits"}},
                      {"seq":2,"client":"p1","kind":"look","cube_path":"/d/c.fits","payload":{"colormap":"magma"}}],
                      "next":2,"instance":"inst-1"})";
    fake.activityNext = "2";
    QObject userA, userB;
    HeadsetHubLink *link = HeadsetHubLink::forSession(fake.url(), u"tok"_s, u"sess-1"_s);
    QVERIFY(link);
    // One link per session, whichever window asks.
    QCOMPARE(HeadsetHubLink::forSession(fake.url() + u"/"_s, u"tok"_s, u"sess-1"_s), link);
    QVERIFY(HeadsetHubLink::forSession(fake.url(), u"tok"_s, u"sess-2"_s) != link);
    QSignalSpy presence(link, &HeadsetHubLink::headsetsChanged);
    QSignalSpy activity(link, &HeadsetHubLink::activityChanged);
    link->addUser(&userA);
    link->addUser(&userB);
    QTRY_COMPARE(link->headsets().size(), 1);
    QTRY_COMPARE(link->activity().events().size(), 2);
    QCOMPARE(presence.count(), 1);
    QVERIFY(activity.count() >= 1);
    QCOMPARE(link->activity().events().first().seq, qint64(2));
    // The backend restarts (another instance): its ring is read again from
    // the start, as a newer epoch, and listed above the old events.
    fake.activity = R"({"events":[{"seq":1,"client":"p1","kind":"cube","summary":"Switched"}],
                       "next":1,"instance":"inst-2"})";
    fake.activityNext = "1";
    fake.instance = "inst-2";
    QTRY_COMPARE(link->activity().events().size(), 3);
    QCOMPARE(link->activity().events().first().epoch, 1);
    QCOMPARE(link->activity().events().first().kind, u"cube"_s);
    // Repeated presence answers that change nothing are not announced again.
    link->refreshPresence();
    QTest::qWait(200);
    QCOMPARE(presence.count(), 1);
    QPointer<HeadsetHubLink> watch(link);
    link->removeUser(&userA);
    QVERIFY(watch->running());
    link->removeUser(&userB);
    QTRY_VERIFY(!watch);
    // The leftover link of sess-2 had no user: drop it by giving it one and taking it away.
    QObject tmp;
    HeadsetHubLink *other = HeadsetHubLink::forSession(fake.url(), u"tok"_s, u"sess-2"_s);
    QPointer<HeadsetHubLink> otherWatch(other);
    other->addUser(&tmp);
    other->removeUser(&tmp);
    QTRY_VERIFY(!otherWatch);
}

void TestVrHeadsetHub::linkStopsQuietlyOnAnOldBackend()
{
    FakeHub fake;
    QVERIFY(fake.server.isListening());
    fake.oldBackend = true;
    QObject user;
    HeadsetHubLink *link = HeadsetHubLink::forSession(fake.url(), u"tok"_s, u"sess-old"_s);
    QSignalSpy stopped(link, &HeadsetHubLink::stopped);
    link->addUser(&user);
    QTRY_COMPARE(stopped.count(), 1);
    QVERIFY(!link->supported());
    QVERIFY(!link->running());
    QVERIFY(link->headsets().isEmpty());
    const int requests = fake.lines.size();
    QTest::qWait(300);
    QVERIFY(fake.lines.size() <= requests + 1);   // nothing more is asked
    QPointer<HeadsetHubLink> watch(link);
    link->removeUser(&user);
    QTRY_VERIFY(!watch);
}

void TestVrHeadsetHub::bodiesMatchTheBackendsRequestModels()
{
    // The hub is #236's: until it is in this tree there is nothing to compare.
    const QString commandFile = backendFileWith(u"/v1/vr/headset/command"_s);
    if (commandFile.isEmpty())
        QSKIP("the backend's headset hub (#236) is not in this tree");
    QFile f(commandFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(f.readAll());

    VrLink::Inputs in;
    in.datasetId = u"ds1"_s;
    in.path = u"/d/v.vbt"_s;
    in.kind = u"velocity"_s;
    in.boxMpc = u"150"_s;
    const QSet<QString> sent = keysOf(VrHub::showCommandBody(in, u"p1"_s));
    bool found = false;
    for (const QSet<QString> &fields : modelFields(source))
        if (fields.contains(u"type"_s) && fields.contains(u"dataset_id"_s) && fields.contains(sent))
            found = true;
    QVERIFY2(found, qPrintable(u"no request model in %1 takes every field of the show command: "_s
                               .arg(commandFile)
                               + QStringList(sent.begin(), sent.end()).join(u", "_s)));
    // The routes and query names the desktop reads.
    const QString activityFile = backendFileWith(u"/v1/vr/activity"_s);
    QVERIFY(!activityFile.isEmpty());
    QFile a(activityFile);
    QVERIFY(a.open(QIODevice::ReadOnly));
    const QString activity = QString::fromUtf8(a.readAll());
    for (const QString &name : { u"after"_s, u"wait"_s, u"events"_s, u"next"_s })
        QVERIFY2(activity.contains(name), qPrintable(name));
    QVERIFY(source.contains(u"headsets"_s) || backendFileWith(u"\"headsets\""_s).size() > 0);
}
