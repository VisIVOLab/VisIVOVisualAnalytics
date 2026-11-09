// test_samp.cpp — implementations for TestSamp (issue #130)

#include "TestSamp.h"

#include "app/DatasetName.h"
#include "app/SampCallbackServer.h"
#include "app/ToolTipText.h"
#include "app/SampProtocol.h"
#include "app/SampXmlRpc.h"
#include "gui/Catalogue3DParser.h"

#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

/// A receiveNotification body as JSAMP writes it: explicit <string> values.
QByteArray notificationBody(const QString &key, const QString &mtype, const QVariantMap &params)
{
    QVariantMap message;
    message.insert(u"samp.mtype"_s, mtype);
    message.insert(u"samp.params"_s, params);
    return SampXmlRpc::methodCall(u"samp.client.receiveNotification"_s,
                                  { key, u"c7"_s, message });
}

}  // namespace

// ── XML-RPC ───────────────────────────────────────────────────────────────────

void TestSamp::xmlRpc_callRoundTripsNestedValuesAndEscaping()
{
    QVariantMap params;
    params.insert(u"url"_s, u"http://127.0.0.1:2525/dynamic/4/t0?a=1&b=<2>"_s);
    params.insert(u"row-list"_s, QVariantList{ u"0"_s, u"7"_s, u"42"_s });
    QVariantMap message;
    message.insert(u"samp.mtype"_s, u"table.select.rowList"_s);
    message.insert(u"samp.params"_s, params);

    const QByteArray body = SampXmlRpc::methodCall(u"samp.hub.notifyAll"_s,
                                                   { u"key&<>\"'"_s, message, 12, true, 1.5 });
    const SampXmlRpc::Call call = SampXmlRpc::parseMethodCall(body);
    QVERIFY2(call.isValid(), qPrintable(call.error));
    QCOMPARE(call.method, u"samp.hub.notifyAll"_s);
    QCOMPARE(call.params.size(), 5);
    QCOMPARE(call.params.at(0).toString(), u"key&<>\"'"_s);
    QCOMPARE(call.params.at(1).toMap(), message);
    QCOMPARE(call.params.at(2).toLongLong(), 12LL);
    QCOMPARE(call.params.at(3).toBool(), true);
    QCOMPARE(call.params.at(4).toDouble(), 1.5);

    // An integer wider than XML-RPC's 32 bits travels as a string.
    const SampXmlRpc::Call wide =
            SampXmlRpc::parseMethodCall(SampXmlRpc::methodCall(u"m"_s, { qint64(1) << 40 }));
    QCOMPARE(wide.params.at(0).toString(), QString::number(qint64(1) << 40));
}

void TestSamp::xmlRpc_parsesImplicitStringsAndWhitespace()
{
    // A <value> with no type is a string (XML-RPC spec); indentation between
    // elements must not become part of any value.
    const QByteArray body = R"(<?xml version="1.0"?>
<methodCall>
  <methodName> samp.client.receiveCall </methodName>
  <params>
    <param><value>sk</value></param>
    <param><value><string>hub</string></value></param>
    <param><value>msg-1</value></param>
    <param>
      <value>
        <struct>
          <member><name>samp.mtype</name><value>coord.pointAt.sky</value></member>
          <member>
            <name>samp.params</name>
            <value><struct>
              <member><name>ra</name><value><string>83.8</string></value></member>
              <member><name>dec</name><value>-5.4</value></member>
            </struct></value>
          </member>
        </struct>
      </value>
    </param>
  </params>
</methodCall>)";
    const SampXmlRpc::Call call = SampXmlRpc::parseMethodCall(body);
    QVERIFY2(call.isValid(), qPrintable(call.error));
    QCOMPARE(call.method, u"samp.client.receiveCall"_s);
    QCOMPARE(call.params.size(), 4);
    QCOMPARE(call.params.at(0).toString(), u"sk"_s);
    QCOMPARE(call.params.at(2).toString(), u"msg-1"_s);
    const QVariantMap message = call.params.at(3).toMap();
    QCOMPARE(message.value(u"samp.mtype"_s).toString(), u"coord.pointAt.sky"_s);
    const QVariantMap p = message.value(u"samp.params"_s).toMap();
    QCOMPARE(p.value(u"ra"_s).toString(), u"83.8"_s);
    QCOMPARE(p.value(u"dec"_s).toString(), u"-5.4"_s);

    // An empty array and an empty string survive.
    const SampXmlRpc::Call empty = SampXmlRpc::parseMethodCall(
            "<methodCall><methodName>m</methodName><params>"
            "<param><value><array><data></data></array></value></param>"
            "<param><value></value></param></params></methodCall>");
    QVERIFY2(empty.isValid(), qPrintable(empty.error));
    QCOMPARE(empty.params.at(0).toList().size(), 0);
    QCOMPARE(empty.params.at(1).toString(), QString());
}

void TestSamp::xmlRpc_responseAndFault()
{
    QVariantMap reg;
    reg.insert(u"samp.private-key"_s, u"k1"_s);
    reg.insert(u"samp.self-id"_s, u"c3"_s);
    QString error;
    const QVariant v = SampXmlRpc::parseMethodResponse(SampXmlRpc::methodResponse(reg), error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(v.toMap(), reg);

    error.clear();
    const QVariant f = SampXmlRpc::parseMethodResponse(
            SampXmlRpc::faultResponse(1, u"Client is not callable"_s), error);
    QVERIFY(!f.isValid());
    QCOMPARE(error, u"Client is not callable"_s);

    // A void SAMP method answers with an empty string, which is a success.
    error.clear();
    const QVariant e = SampXmlRpc::parseMethodResponse(SampXmlRpc::methodResponse(QString()), error);
    QVERIFY(error.isEmpty());
    QCOMPARE(e.toString(), QString());
}

void TestSamp::xmlRpc_refusesMalformedBodies()
{
    QVERIFY(!SampXmlRpc::parseMethodCall("not xml").isValid());
    QVERIFY(!SampXmlRpc::parseMethodCall("<methodResponse/>").isValid());
    QVERIFY(!SampXmlRpc::parseMethodCall(
                     "<methodCall><params><param><value>x</value></param></params></methodCall>")
                     .isValid());   // no method name
    QVERIFY(!SampXmlRpc::parseMethodCall(
                     "<methodCall><methodName>m</methodName><params><param><value>"
                     "<i4>twelve</i4></value></param></params></methodCall>")
                     .isValid());
    QVERIFY(!SampXmlRpc::parseMethodCall(
                     "<methodCall><methodName>m</methodName><params><param><value>"
                     "<string>a</string><string>b</string></value></param></params></methodCall>")
                     .isValid());
    QString error;
    QVERIFY(!SampXmlRpc::parseMethodResponse("<methodResponse><params/></methodResponse>", error)
                     .isValid());
    QVERIFY(!error.isEmpty());
}

void TestSamp::xmlRpc_refusesDeepNesting()
{
    // Decoded before the private key can be checked: a local process must not
    // be able to exhaust the stack with nested arrays.
    auto nested = [](int depth) {
        QByteArray v = "<value>x</value>";
        for (int i = 0; i < depth; ++i)
            v = "<value><array><data>" + v + "</data></array></value>";
        return "<methodCall><methodName>m</methodName><params><param>" + v
               + "</param></params></methodCall>";
    };
    QVERIFY(SampXmlRpc::parseMethodCall(nested(8)).isValid());
    // Nor with entities: a DTD is refused outright.
    const SampXmlRpc::Call bomb = SampXmlRpc::parseMethodCall(
            "<?xml version=\"1.0\"?><!DOCTYPE m [<!ENTITY a \"aaaaaaaaaa\">]>"
            "<methodCall><methodName>m</methodName><params><param><value>&a;&a;&a;"
            "</value></param></params></methodCall>");
    QVERIFY(!bomb.isValid());
    QVERIFY(bomb.error.contains(u"DTD"_s));
    QString responseError;
    QVERIFY(!SampXmlRpc::parseMethodResponse("<!DOCTYPE r><methodResponse/>", responseError)
                     .isValid());
    const SampXmlRpc::Call deep = SampXmlRpc::parseMethodCall(nested(10000));
    QVERIFY(!deep.isValid());
    QVERIFY(deep.error.contains(u"nested"_s));
}

// ── HTTP ──────────────────────────────────────────────────────────────────────

void TestSamp::http_parsesCompleteAndPartialRequests()
{
    const QByteArray full = "POST /xmlrpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Content-Type: text/xml\r\ncontent-length: 5\r\n\r\nhello";
    const auto done = SampHttp::parseRequest(full, 1024);
    QCOMPARE(done.state, SampHttp::Request::State::Complete);
    QCOMPARE(done.body, QByteArray("hello"));
    QCOMPARE(done.path, QByteArray("/xmlrpc"));
    QCOMPARE(done.consumed, full.size());

    // Headers only: incomplete, but known (so 100-continue can be sent).
    const QByteArray head = "POST / HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n";
    const auto partial = SampHttp::parseRequest(head + "he", 1024);
    QCOMPARE(partial.state, SampHttp::Request::State::Incomplete);
    QVERIFY(partial.headersComplete);
    QVERIFY(partial.expectContinue);

    const auto noHeaders = SampHttp::parseRequest("POST / HTTP/1.1\r\nContent-Le", 1024);
    QCOMPARE(noHeaders.state, SampHttp::Request::State::Incomplete);
    QVERIFY(!noHeaders.headersComplete);

    const QByteArray response = SampHttp::response(200, "OK", "<x/>");
    QVERIFY(response.startsWith("HTTP/1.1 200 OK\r\n"));
    QVERIFY(response.contains("Content-Length: 4\r\n"));
    QVERIFY(response.endsWith("\r\n\r\n<x/>"));
}

void TestSamp::http_refusesWhatAHubNeverSends()
{
    QCOMPARE(SampHttp::parseRequest("GET / HTTP/1.1\r\n\r\n", 1024).status, 405);
    QCOMPARE(SampHttp::parseRequest("POST / HTTP/1.1\r\n\r\n", 1024).status, 411);
    QCOMPARE(SampHttp::parseRequest(
                     "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 1024).status, 501);
    QCOMPARE(SampHttp::parseRequest("POST / HTTP/1.1\r\nContent-Length: 2048\r\n\r\n", 1024).status,
             413);
    QCOMPARE(SampHttp::parseRequest("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", 1024).state,
             SampHttp::Request::State::Bad);
    QCOMPARE(SampHttp::parseRequest("garbage\r\n\r\n", 1024).state, SampHttp::Request::State::Bad);
    const QByteArray huge(SampHttp::kMaxHeaderBytes + 10, 'a');
    QCOMPARE(SampHttp::parseRequest(huge, 1024).status, 431);
}

void TestSamp::callbackServer_answersALoopbackPost()
{
    SampCallbackServer server;
    QString listenError;
    QVERIFY2(server.listen(&listenError), qPrintable(listenError));
    QVERIFY(server.url().startsWith(u"http://127.0.0.1:"_s));

    QList<SampXmlRpc::Call> seen;
    server.setHandler([&seen](const SampXmlRpc::Call &call) {
        seen.append(call);
        return SampXmlRpc::methodResponse(QString());
    });

    QVariantMap params;
    params.insert(u"ra"_s, u"10.5"_s);
    params.insert(u"dec"_s, u"-3.25"_s);
    const QByteArray body = notificationBody(u"secret"_s, u"coord.pointAt.sky"_s, params);

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, QUrl(server.url()).port());
    QVERIFY(socket.waitForConnected(3000));
    // Sent in two pieces: the server must wait for the whole body.
    socket.write("POST /xmlrpc HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/xml\r\n"
                 "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
                 + body.left(10));
    socket.flush();
    QTest::qWait(50);
    socket.write(body.mid(10));

    QByteArray reply;
    QTRY_VERIFY_WITH_TIMEOUT((reply += socket.readAll(), reply.contains("</methodResponse>")),
                             3000);
    QVERIFY(reply.startsWith("HTTP/1.1 200 OK"));
    QCOMPARE(seen.size(), 1);
    QCOMPARE(seen.first().method, u"samp.client.receiveNotification"_s);
    const Samp::Inbound in = Samp::decodeClientCall(seen.first(), u"secret"_s);
    QVERIFY2(in.error.isEmpty(), qPrintable(in.error));
    QCOMPARE(in.message.params.value(u"ra"_s).toString(), u"10.5"_s);

    server.close();
    QVERIFY(!server.isListening());
    QVERIFY(server.url().isEmpty());
}

// ── Callback dispatch ─────────────────────────────────────────────────────────

void TestSamp::decode_notificationCallAndResponse()
{
    QVariantMap params;
    params.insert(u"table-id"_s, u"topcat-1"_s);
    params.insert(u"row"_s, u"17"_s);
    const auto note = Samp::decodeClientCall(
            SampXmlRpc::parseMethodCall(
                    notificationBody(u"pk"_s, u"table.highlight.row"_s, params)),
            u"pk"_s);
    QVERIFY(note.error.isEmpty());
    QCOMPARE(note.kind, Samp::Inbound::Kind::Notification);
    QCOMPARE(note.message.senderId, u"c7"_s);
    QCOMPARE(note.message.mtype, u"table.highlight.row"_s);
    QVERIFY(note.message.msgId.isEmpty());

    QVariantMap message;
    message.insert(u"samp.mtype"_s, u"table.load.votable"_s);
    message.insert(u"samp.params"_s, QVariantMap{ { u"url"_s, u"http://h/t.vot"_s } });
    const auto call = Samp::decodeClientCall(
            SampXmlRpc::parseMethodCall(SampXmlRpc::methodCall(
                    u"samp.client.receiveCall"_s, { u"pk"_s, u"c2"_s, u"m9"_s, message })),
            u"pk"_s);
    QVERIFY(call.error.isEmpty());
    QCOMPARE(call.kind, Samp::Inbound::Kind::Call);
    QCOMPARE(call.message.msgId, u"m9"_s);

    QVariantMap response;
    response.insert(u"samp.status"_s, u"samp.ok"_s);
    const auto resp = Samp::decodeClientCall(
            SampXmlRpc::parseMethodCall(SampXmlRpc::methodCall(
                    u"samp.client.receiveResponse"_s, { u"pk"_s, u"c2"_s, u"tag"_s, response })),
            u"pk"_s);
    QVERIFY(resp.error.isEmpty());
    QCOMPARE(resp.kind, Samp::Inbound::Kind::Response);
    QCOMPARE(resp.msgTag, u"tag"_s);
    QCOMPARE(resp.response.value(u"samp.status"_s).toString(), u"samp.ok"_s);
}

void TestSamp::decode_refusesBadKeyArityAndMethod()
{
    const auto parse = [](const QString &method, const QVariantList &params) {
        return SampXmlRpc::parseMethodCall(SampXmlRpc::methodCall(method, params));
    };
    QVariantMap msg{ { u"samp.mtype"_s, u"samp.app.ping"_s } };
    // Another local process cannot inject messages without the private key.
    QVERIFY(!Samp::decodeClientCall(parse(u"samp.client.receiveNotification"_s,
                                          { u"wrong"_s, u"c1"_s, msg }),
                                    u"pk"_s).error.isEmpty());
    // Not registered yet: there is no key to match.
    QVERIFY(!Samp::decodeClientCall(parse(u"samp.client.receiveNotification"_s,
                                          { QString(), u"c1"_s, msg }),
                                    QString()).error.isEmpty());
    QVERIFY(!Samp::decodeClientCall(parse(u"samp.client.receiveNotification"_s,
                                          { u"pk"_s, u"c1"_s }),
                                    u"pk"_s).error.isEmpty());
    QVERIFY(!Samp::decodeClientCall(parse(u"samp.client.receiveCall"_s,
                                          { u"pk"_s, u"c1"_s, QString(), msg }),
                                    u"pk"_s).error.isEmpty());
    QVERIFY(!Samp::decodeClientCall(parse(u"samp.client.receiveNotification"_s,
                                          { u"pk"_s, u"c1"_s, QVariantMap() }),
                                    u"pk"_s).error.isEmpty());   // no samp.mtype
    QVERIFY(!Samp::decodeClientCall(parse(u"samp.hub.ping"_s, { u"pk"_s }), u"pk"_s)
                     .error.isEmpty());
}

void TestSamp::validate_andResponseForCalls()
{
    auto msg = [](const QString &mtype, const QVariantMap &params) {
        SampMessage m;
        m.mtype = mtype;
        m.params = params;
        return m;
    };
    QVERIFY(Samp::validateMessage(msg(u"table.load.votable"_s, { { u"url"_s, u"http://x/t"_s } }))
                    .isEmpty());
    QVERIFY(!Samp::validateMessage(msg(u"image.load.fits"_s, {})).isEmpty());
    QVERIFY(Samp::validateMessage(msg(u"coord.pointAt.sky"_s,
                                      { { u"ra"_s, u"359.9"_s }, { u"dec"_s, u"-89"_s } }))
                    .isEmpty());
    QVERIFY(!Samp::validateMessage(msg(u"coord.pointAt.sky"_s,
                                       { { u"ra"_s, u"400"_s }, { u"dec"_s, u"0"_s } }))
                     .isEmpty());
    QVERIFY(Samp::validateMessage(msg(u"table.highlight.row"_s,
                                      { { u"url"_s, u"http://x/t"_s }, { u"row"_s, u"3"_s } }))
                    .isEmpty());
    QVERIFY(!Samp::validateMessage(msg(u"table.highlight.row"_s, { { u"row"_s, u"3"_s } }))
                     .isEmpty());   // which table?
    QVERIFY(!Samp::validateMessage(msg(u"table.highlight.row"_s,
                                       { { u"table-id"_s, u"t"_s }, { u"row"_s, u"-1"_s } }))
                     .isEmpty());
    QVERIFY(Samp::validateMessage(msg(u"table.select.rowList"_s,
                                      { { u"table-id"_s, u"t"_s },
                                        { u"row-list"_s, QVariantList{} } }))
                    .isEmpty());   // an empty selection is a selection
    QVERIFY(Samp::validateMessage(msg(u"samp.app.ping"_s, {})).isEmpty());
    QVERIFY(!Samp::validateMessage(msg(u"spectrum.load.ssa-generic"_s, {})).isEmpty());

    // What leaves for the hub is strings, lists and maps only.
    const QVariantMap sampified = Samp::toSampValue(
            QVariantMap{ { u"ra"_s, 248.81 }, { u"n"_s, 3 }, { u"ok"_s, true },
                         { u"rows"_s, QVariantList{ 1, u"2"_s } } }).toMap();
    QCOMPARE(sampified.value(u"ra"_s).typeId(), int(QMetaType::QString));
    QCOMPARE(sampified.value(u"ra"_s).toString(), u"248.81"_s);
    QCOMPARE(sampified.value(u"n"_s).toString(), u"3"_s);
    QCOMPARE(sampified.value(u"ok"_s).toString(), u"1"_s);
    QCOMPARE(sampified.value(u"rows"_s).toList(), (QVariantList{ u"1"_s, u"2"_s }));

    QVERIFY(Samp::isLoadMType(u"table.load.votable"_s));
    QVERIFY(Samp::isLoadMType(u"image.load.fits"_s));
    QVERIFY(!Samp::isLoadMType(u"table.highlight.row"_s));
    QCOMPARE(Samp::response(u"no such file"_s)
                     .value(u"samp.error"_s).toMap().value(u"samp.errortxt"_s).toString(),
             u"no such file"_s);
    QCOMPARE(Samp::response().value(u"samp.status"_s).toString(), u"samp.ok"_s);
    QVERIFY(Samp::responseError(Samp::response()).isEmpty());
    QCOMPARE(Samp::responseError(Samp::response(u"too big"_s)), u"too big"_s);
    QVERIFY(Samp::responseError({ { u"samp.status"_s, u"samp.warning"_s } }).isEmpty());
    QVERIFY(!Samp::responseError({}).isEmpty());
    // The hub's own events are subscribed to, so peers stay current.
    QVERIFY(Samp::subscribedMTypes().contains(u"samp.hub.event.metadata"_s));
    QVERIFY(Samp::subscribedMTypes().contains(u"table.select.rowList"_s));

    const QVariantMap ok = Samp::responseFor(msg(u"samp.app.ping"_s, {}));
    QCOMPARE(ok.value(u"samp.status"_s).toString(), u"samp.ok"_s);
    const QVariantMap bad = Samp::responseFor(msg(u"image.load.fits"_s, {}));
    QCOMPARE(bad.value(u"samp.status"_s).toString(), u"samp.error"_s);
    QVERIFY(!bad.value(u"samp.error"_s).toMap().value(u"samp.errortxt"_s).toString().isEmpty());
    // The response must itself be encodable as XML-RPC for samp.hub.reply.
    const SampXmlRpc::Call reply = SampXmlRpc::parseMethodCall(
            SampXmlRpc::methodCall(u"samp.hub.reply"_s, { u"pk"_s, u"m1"_s, bad }));
    QCOMPARE(reply.params.at(2).toMap(), bad);
}

void TestSamp::lockFile_parsesKeysAndIgnoresComments()
{
    const Samp::LockInfo info = Samp::parseLockFile(
            u"# SAMP Standard Profile lockfile\r\n"
            "samp.secret=abc=def\r\n"
            "\n"
            "samp.hub.xmlrpc.url = http://127.0.0.1:2525/xmlrpc\n"
            "samp.profile.version=1.3\n"
            "=orphan\n"_s);
    QCOMPARE(info.hubUrl, u"http://127.0.0.1:2525/xmlrpc"_s);
    QCOMPARE(info.secret, u"abc=def"_s);   // only the first '=' splits
    QVERIFY(Samp::parseLockFile(u"# nothing\n"_s).hubUrl.isEmpty());
}

void TestSamp::pendingCalls_reusedMsgIdAfterReconnect()
{
    // A load call is in progress (registration 1) when the hub restarts; the
    // new hub (registration 2) hands out the same msg-id for another load.
    SampPendingCalls calls;
    const QString oldToken = calls.add(1, u"msg-7"_s);
    calls.clear();   // the teardown of registration 1
    const QString newToken = calls.add(2, u"msg-7"_s);
    QVERIFY(oldToken != newToken);

    // The old load finishing must neither answer nor consume the new call...
    QVERIFY(!calls.take(oldToken, 2));
    QCOMPARE(calls.size(), 1);
    // ...which its own completion still answers, once.
    QCOMPARE(calls.take(newToken, 2).value_or(QString()), u"msg-7"_s);
    QVERIFY(!calls.take(newToken, 2));

    // Without a teardown in between, a token of an older registration is
    // refused all the same; a malformed or unknown token is ignored.
    const QString stale = calls.add(3, u"m"_s);
    QVERIFY(!calls.take(stale, 4));
    QVERIFY(!calls.take(u"nonsense"_s, 4));
    QVERIFY(!calls.take(u"4/unknown"_s, 4));
    QVERIFY(!calls.take(QString(), 4));
}

// ── Rows and tables ───────────────────────────────────────────────────────────

void TestSamp::rows_parseIndicesAndLists()
{
    QCOMPARE(Samp::parseRowIndex(u"0"_s).value_or(-1), 0);
    QCOMPARE(Samp::parseRowIndex(u" 3931545 "_s).value_or(-1), 3931545);
    QCOMPARE(Samp::parseRowIndex(QVariant(qlonglong(12))).value_or(-1), 12);
    QVERIFY(!Samp::parseRowIndex(u"-1"_s));
    QVERIFY(!Samp::parseRowIndex(u"1.5"_s));
    QVERIFY(!Samp::parseRowIndex(QString()));

    const auto rows = Samp::parseRowList(QVariantList{ u"4"_s, u"0"_s, u"9"_s });
    QVERIFY(rows);
    QCOMPARE(*rows, (QList<qint64>{ 4, 0, 9 }));
    QVERIFY(Samp::parseRowList(QVariantList{}));
    QVERIFY(!Samp::parseRowList(QVariantList{ u"4"_s, u"x"_s }));
    QVERIFY(!Samp::parseRowList(u"4,5"_s));   // not a list
}

void TestSamp::rows_buildHighlightAndSelectParams()
{
    const Samp::TableRef both{ u"visivo-1"_s, u"http://b/v1/samp/files/t"_s };
    const QVariantMap h = Samp::highlightRowParams(both, 41);
    QCOMPARE(h.value(u"row"_s).toString(), u"41"_s);
    QCOMPARE(h.value(u"table-id"_s).toString(), u"visivo-1"_s);
    QCOMPARE(h.value(u"url"_s).toString(), u"http://b/v1/samp/files/t"_s);

    // Only what is known is sent: an absent url is not an empty one.
    const QVariantMap s = Samp::selectRowListParams({ u"visivo-1"_s, QString() }, { 2, 5 });
    QVERIFY(!s.contains(u"url"_s));
    QCOMPARE(s.value(u"row-list"_s).toList(), (QVariantList{ u"2"_s, u"5"_s }));
    // And what is sent parses back as it went out.
    SampMessage m;
    m.mtype = u"table.select.rowList"_s;
    m.params = s;
    QVERIFY(Samp::validateMessage(m).isEmpty());
}

void TestSamp::registry_matchesByTableIdOrUrl()
{
    SampTableRegistry reg;
    // Sent by VisIVO: the table-id first, the URL once the client expanded it.
    reg.remember({ u"visivo-a"_s, QString() }, u"/data/a.csv"_s);
    QCOMPARE(reg.localKeyFor({ u"visivo-a"_s, QString() }), u"/data/a.csv"_s);
    QVERIFY(reg.localKeyFor({ QString(), u"http://b/f/1"_s }).isEmpty());
    reg.setUrlForTableId(u"visivo-a"_s, u"http://b/f/1"_s);
    QCOMPARE(reg.localKeyFor({ QString(), u"http://b/f/1"_s }), u"/data/a.csv"_s);
    QCOMPARE(reg.refFor(u"/data/a.csv"_s)->url, u"http://b/f/1"_s);

    // Received from TOPCAT: matched on its table-id or on its URL.
    reg.remember({ u"topcat-3"_s, u"http://127.0.0.1:2525/dynamic/3/t"_s }, u"/tmp/x_t.vot"_s);
    QCOMPARE(reg.localKeyFor({ u"topcat-3"_s, QString() }), u"/tmp/x_t.vot"_s);
    QCOMPARE(reg.localKeyFor({ QString(), u"http://127.0.0.1:2525/dynamic/3/t"_s }),
             u"/tmp/x_t.vot"_s);
    // A table-id wins over a url naming another table.
    QCOMPARE(reg.localKeyFor({ u"visivo-a"_s, u"http://127.0.0.1:2525/dynamic/3/t"_s }),
             u"/data/a.csv"_s);
    QVERIFY(reg.localKeyFor({ u"nope"_s, u"http://nope"_s }).isEmpty());
    QVERIFY(!reg.refFor(u"/never/sent"_s));

    // Sent again: both identities stay, the latest one is used outwards.
    reg.remember({ u"visivo-b"_s, QString() }, u"/data/a.csv"_s);
    QCOMPARE(reg.localKeyFor({ u"visivo-a"_s, QString() }), u"/data/a.csv"_s);
    QCOMPARE(reg.refFor(u"/data/a.csv"_s)->tableId, u"visivo-b"_s);

    // Two table-ids for one url of the same catalogue: both still resolve.
    reg.remember({ u"alias-1"_s, u"http://same"_s }, u"/data/c.vot"_s);
    reg.remember({ u"alias-2"_s, u"http://same"_s }, u"/data/c.vot"_s);
    QCOMPARE(reg.localKeyFor({ u"alias-1"_s, QString() }), u"/data/c.vot"_s);
    QCOMPARE(reg.localKeyFor({ u"alias-2"_s, QString() }), u"/data/c.vot"_s);

    // A table-id seen again now names the new catalogue.
    reg.remember({ u"topcat-3"_s, QString() }, u"/tmp/y.vot"_s);
    QCOMPARE(reg.localKeyFor({ u"topcat-3"_s, QString() }), u"/tmp/y.vot"_s);

    reg.remember({}, u"/ignored"_s);
    QVERIFY(!reg.refFor(u"/ignored"_s));
    for (int i = 0; i < SampTableRegistry::kMaxEntries + 10; ++i)
        reg.remember({ u"t%1"_s.arg(i), QString() }, u"/k%1"_s.arg(i));
    QCOMPARE(reg.size(), SampTableRegistry::kMaxEntries);
}

void TestSamp::rowMap_mapsThroughTheOriginalRowOrder()
{
    // Loaded: rows 9, 8, 7 (a descending sort), then 1, 3 (another page of an
    // even sample), and one entry of unknown origin.
    SampRowMap map;
    map.reset({ 9, 8, 7, 1, 3, -1 });
    QCOMPARE(map.entryFor(9), 0);
    QCOMPARE(map.entryFor(3), 4);
    QCOMPARE(map.entryFor(0), -1);       // filtered out or not sampled
    QCOMPARE(map.entryFor(-1), -1);      // unknown origin never matches
    QCOMPARE(map.rowFor(1), 8LL);
    QCOMPARE(map.rowFor(5), -1LL);
    QCOMPARE(map.rowFor(6), -1LL);       // past the end
    QCOMPARE(map.rowFor(-1), -1LL);

    const SampRowMap::Split split = map.entriesFor({ 7, 0, 1, 1000000 });
    QCOMPARE(split.entries, (QList<int>{ 2, 3 }));
    QCOMPARE(split.missing, 2);

    map.clear();
    QVERIFY(map.isEmpty());
    QCOMPARE(map.entryFor(9), -1);
}

void TestSamp::stagedFileName_addsTheExtensionTheMTypeImplies()
{
    const QString vot = u"table.load.votable"_s;
    const QString fitsTable = u"table.load.fits"_s;
    const QString image = u"image.load.fits"_s;
    // TOPCAT's hub URLs have no extension.
    QCOMPARE(Samp::stagedFileName(u"http://127.0.0.1:2525/dynamic/4/t0"_s, u"cat"_s, vot),
             u"t0.vot"_s);
    QCOMPARE(Samp::stagedFileName(u"http://h/x/survey.xml"_s, QString(), vot), u"survey.xml"_s);
    QCOMPARE(Samp::stagedFileName(u"http://h/x/t1"_s, QString(), fitsTable), u"t1.fits"_s);
    // A FITS table the catalogue reader would not recognise gets .fits.
    QCOMPARE(Samp::stagedFileName(u"http://h/x/t1.fts"_s, QString(), fitsTable), u"t1.fts.fits"_s);
    QVERIFY(Samp::hasFormatExtension(u"a.VOT"_s, vot));
    QVERIFY(!Samp::hasFormatExtension(u"table"_s, vot));
    QVERIFY(!Samp::hasFormatExtension(u"a.vot"_s, fitsTable));
    QVERIFY(Samp::hasFormatExtension(u"m.fits.fz"_s, image));
    QCOMPARE(Samp::stagedFileName(u"http://h/m31.fits.gz"_s, QString(), image), u"m31.fits.gz"_s);
    QCOMPARE(Samp::stagedFileName(u"file:///Users/me/My%20Cat.vot"_s, QString(), vot),
             u"My_Cat.vot"_s);
    // No usable name in the URL: the message's name, then a default.
    QCOMPARE(Samp::stagedFileName(u"http://h/"_s, u"../../etc/passwd"_s, vot),
             u"_.._etc_passwd.vot"_s);
    QCOMPARE(Samp::stagedFileName(u"http://h/"_s, QString(), vot), u"samp-table.vot"_s);
    QCOMPARE(Samp::stagedFileName(u"http://h/"_s, QString(), image), u"samp-data.fits"_s);

    // The name shown is the sender's, not the staged one.
    QCOMPARE(Samp::displayName(u"http://127.0.0.1:2525/dynamic/1/t1.vot"_s, u" NKATZ "_s),
             u"NKATZ"_s);
    QCOMPARE(Samp::displayName(u"http://127.0.0.1:2525/dynamic/1/t1.vot"_s, QString()),
             u"t1.vot"_s);
    QCOMPARE(Samp::displayName(u"file:///data/My%20Cat.vot"_s, QString()), u"My Cat.vot"_s);
    QVERIFY(Samp::displayName(u"http://h/"_s, QString()).isEmpty());
}

void TestSamp::parser_keepsSourceRowsWhenRowsAreSkipped()
{
    Catalogue3DSchema schema;
    for (const QString &name : { u"name"_s, u"ra"_s, u"dec"_s }) {
        schema.fields.push_back({ name, u"double"_s });
        schema.headers.append(name);
    }
    schema.raFieldName = u"ra"_s;
    schema.decFieldName = u"dec"_s;
    const QList<QVariantMap> rows = {
        { { u"name"_s, u"a"_s }, { u"ra"_s, 10.0 }, { u"dec"_s, 1.0 } },
        { { u"name"_s, u"b"_s }, { u"ra"_s, QVariant() }, { u"dec"_s, 2.0 } },   // skipped
        { { u"name"_s, u"c"_s }, { u"ra"_s, 30.0 }, { u"dec"_s, 3.0 } },
    };
    const auto parsed = Catalogue3DParser::parseBackendSubset(schema, rows, { 70, 12, 5 });
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.entries.size(), std::size_t(2));
    QCOMPARE(parsed.entries[0].sourceRow, 70LL);
    QCOMPARE(parsed.entries[1].sourceRow, 5LL);   // not 12: the skipped row kept its own

    // Without positions (an older backend), or with a list of the wrong
    // length, the origin is unknown rather than guessed.
    const auto unknown = Catalogue3DParser::parseBackendSubset(schema, rows, { 1, 2 });
    QCOMPARE(unknown.entries[0].sourceRow, -1LL);
    QCOMPARE(Catalogue3DParser::parseBackendSubset(schema, rows).entries[1].sourceRow, -1LL);
}

void TestSamp::registry_listsEveryIdentityOfACatalogue()
{
    // Received from TOPCAT, then sent back: TOPCAT now holds the original and
    // a copy of one catalogue, and a row picked in VisIVO is the same row of
    // both. The original comes first.
    SampTableRegistry reg;
    reg.remember({ u"topcat-1"_s, u"http://127.0.0.1:2525/dynamic/1/t1"_s }, u"/samp/ab_t1.vot"_s);
    reg.remember({ u"visivo-x"_s, u"http://backend/v1/samp/files/x"_s }, u"/samp/ab_t1.vot"_s);
    reg.remember({ u"other"_s, QString() }, u"/data/other.csv"_s);
    const QList<Samp::TableRef> refs = reg.refsFor(u"/samp/ab_t1.vot"_s);
    QCOMPARE(refs.size(), 2);
    QCOMPARE(refs.at(0).tableId, u"topcat-1"_s);
    QCOMPARE(refs.at(1).tableId, u"visivo-x"_s);
    // The latest identity is still what refFor() answers.
    QCOMPARE(reg.refFor(u"/samp/ab_t1.vot"_s)->tableId, u"visivo-x"_s);
    QVERIFY(reg.refsFor(u"/never/exchanged"_s).isEmpty());

    // A table-id reused for another catalogue leaves the first one's list.
    reg.remember({ u"topcat-1"_s, QString() }, u"/data/other.csv"_s);
    QCOMPARE(reg.refsFor(u"/samp/ab_t1.vot"_s).size(), 1);
    QCOMPARE(reg.refsFor(u"/data/other.csv"_s).size(), 2);
}

void TestSamp::catalogueLoadMType_followsTheBackendsChoice()
{
    // As /v1/samp/send-catalogue stages them: FITS stays FITS, everything
    // else goes out as a VOTable.
    QCOMPARE(Samp::catalogueLoadMType(u"fits"_s), u"table.load.fits"_s);
    QCOMPARE(Samp::catalogueLoadMType(u" FIT "_s), u"table.load.fits"_s);
    QCOMPARE(Samp::catalogueLoadMType(u"fits-table"_s), u"table.load.fits"_s);
    for (const QString &format : { u"csv"_s, u"votable"_s, u"ipac"_s, u"speck"_s, QString() })
        QCOMPARE(Samp::catalogueLoadMType(format), u"table.load.votable"_s);
}

void TestSamp::parser_namesUnnamedSourcesByTheirTableRow()
{
    Catalogue3DSchema schema;
    for (const QString &name : { u"ra"_s, u"dec"_s }) {
        schema.fields.push_back({ name, u"double"_s });
        schema.headers.append(name);
    }
    schema.raFieldName = u"ra"_s;
    schema.decFieldName = u"dec"_s;
    const QList<QVariantMap> rows = {
        { { u"ra"_s, 10.0 }, { u"dec"_s, 1.0 } },
        { { u"ra"_s, 20.0 }, { u"dec"_s, 2.0 } },
    };
    // A second page (Load more): ids restart at 0, the rows do not.
    const auto page = Catalogue3DParser::parseBackendSubset(schema, rows, { 149997, 149998 });
    QCOMPARE(Catalogue3DParser::detail::entryDisplayName(page.entries[1], schema),
             u"Row 149999"_s);
    // No rows reported (an older backend): the position in the page, as before.
    const auto unknown = Catalogue3DParser::parseBackendSubset(schema, rows);
    QCOMPARE(Catalogue3DParser::detail::entryDisplayName(unknown.entries[1], schema),
             u"Source 1"_s);
}

void TestSamp::reconnect_aLateHubLossDoesNotUndoDisconnect()
{
    SampReconnectPolicy policy;
    // TOPCAT quits: wait for a hub, until one is reached.
    policy.userConnects();
    QVERIFY(policy.hubLost());
    QVERIFY(policy.waiting());
    policy.connected();
    QVERIFY(!policy.waiting());

    // The user disconnects, and the queued hub-loss signal arrives after.
    policy.userDisconnects();
    QVERIFY(!policy.hubLost());
    QVERIFY(!policy.waiting());

    // Connecting again re-arms it.
    policy.userConnects();
    QVERIFY(policy.hubLost());
    // Disconnect while waiting stops the wait.
    policy.userDisconnects();
    QVERIFY(!policy.waiting());
}

void TestSamp::pendingTables_resolveRowMessagesBeforeTheSendReturns()
{
    SampPendingTables pending;
    pending.add(u"visivo-1"_s, u"/data/a.csv"_s);
    pending.add(QString(), u"/ignored"_s);
    // Before staging, only the table-id names it.
    QCOMPARE(pending.localKeyFor({ u"visivo-1"_s, QString() }), u"/data/a.csv"_s);
    QVERIFY(pending.localKeyFor({ QString(), u"http://b/v1/samp/files/t"_s }).isEmpty());
    // Staged: a url-only message (from a tool that names tables by url) too.
    pending.setUrl(u"visivo-1"_s, u"http://b/v1/samp/files/t"_s);
    pending.setUrl(u"unknown"_s, u"http://elsewhere"_s);
    QCOMPARE(pending.localKeyFor({ QString(), u"http://b/v1/samp/files/t"_s }), u"/data/a.csv"_s);
    QVERIFY(pending.localKeyFor({ QString(), u"http://elsewhere"_s }).isEmpty());
    QVERIFY(pending.localKeyFor({}).isEmpty());
    // The send went out: it leaves the pending list (the registry has it).
    QCOMPARE(pending.take(u"visivo-1"_s), u"/data/a.csv"_s);
    QVERIFY(pending.localKeyFor({ u"visivo-1"_s, QString() }).isEmpty());
    QVERIFY(pending.take(u"visivo-1"_s).isEmpty());
}

void TestSamp::reconnect_keepsTryingAfterAFailedConnect()
{
    // Connect before TOPCAT runs: the first failure starts the retries, the
    // retries' own failures do not start them again.
    SampReconnectPolicy policy;
    policy.userConnects();
    QVERIFY(policy.attemptFailed());
    QVERIFY(policy.waiting());
    QVERIFY(!policy.attemptFailed());
    QVERIFY(policy.waiting());
    policy.connected();
    QVERIFY(!policy.waiting());

    // After Disconnect, a failure still in flight starts nothing.
    policy.userDisconnects();
    QVERIFY(!policy.attemptFailed());
    QVERIFY(!policy.waiting());
}

void TestSamp::hubStatusKey_ignoresOrderAndSeesChanges()
{
    const SampClientInfo topcat{ u"c1"_s, u"topcat"_s, QString(), QString(),
                                 { u"table.load.votable"_s, u"coord.pointAt.sky"_s } };
    const SampClientInfo aladin{ u"c2"_s, u"Aladin"_s, QString(), QString(),
                                 { u"image.load.fits"_s } };
    const QString key = Samp::hubStatusKey(true, u"http://hub"_s, u"c3"_s, { topcat, aladin });
    // The same peers in another order, subscriptions too: the same status.
    SampClientInfo reordered = topcat;
    reordered.subscriptions = { u"coord.pointAt.sky"_s, u"table.load.votable"_s };
    QCOMPARE(Samp::hubStatusKey(true, u"http://hub"_s, u"c3"_s, { aladin, reordered }), key);
    // A peer gone, a new subscription, a new registration, a disconnect: news.
    QVERIFY(Samp::hubStatusKey(true, u"http://hub"_s, u"c3"_s, { topcat }) != key);
    SampClientInfo subscribed = aladin;
    subscribed.subscriptions << u"table.load.votable"_s;
    QVERIFY(Samp::hubStatusKey(true, u"http://hub"_s, u"c3"_s, { topcat, subscribed }) != key);
    QVERIFY(Samp::hubStatusKey(true, u"http://hub"_s, u"c4"_s, { topcat, aladin }) != key);
    QVERIFY(Samp::hubStatusKey(false, u"http://hub"_s, QString(), {}) != key);
}

void TestSamp::receivedCopies_showTheSendersName()
{
    const QString staged = u"/backend/samp/3f9a_t1.fits"_s;
    QCOMPARE(visivo::datasetDisplayName(staged), u"3f9a_t1.fits"_s);
    QVERIFY(!visivo::isReceivedCopy(staged));
    QCOMPARE(visivo::datasetPathLabel(staged), staged);

    visivo::rememberReceivedName(staged, u" M31 DSS "_s);
    QVERIFY(visivo::isReceivedCopy(staged));
    QCOMPARE(visivo::datasetDisplayName(staged), u"M31 DSS"_s);
    QVERIFY(visivo::datasetPathLabel(staged).startsWith(u"temporary copy"_s));
    QVERIFY(visivo::datasetPathLabel(staged).endsWith(staged));   // the path is still there

    // A sender that gave no name: still a temporary copy, shown by file name.
    const QString unnamed = u"/backend/samp/ab12_image.fits"_s;
    visivo::rememberReceivedName(unnamed, u"  "_s);
    QVERIFY(visivo::isReceivedCopy(unnamed));
    QCOMPARE(visivo::datasetDisplayName(unnamed), u"ab12_image.fits"_s);
    QVERIFY(visivo::datasetPathLabel(unnamed).startsWith(u"temporary copy"_s));
    visivo::receivedNames().remove(staged);
    visivo::receivedNames().remove(unnamed);
}

void TestSamp::toolTip_breaksLongValuesAndLeavesShortOnes()
{
    // Short values, exactly as they are (no markup, nothing added).
    for (const QString &v : { u"nkatz150k.tbl"_s, u"http://localhost:18900"_s,
                              u"3f2a9c4e-0b1d-4c7a-9e55-0123456789ab"_s, QString() })
        QCOMPARE(visivo::wrappedToolTip(v), v);

    // A long temp path: broken after separators, nothing lost, every line
    // within the limit, the end (…_t1.vot) on the last line.
    const QString path = u"/var/folders/xy/abcdefghijk/T/visivo_samp_files/"
                         u"8b0997a1c2d3e4f5a6b7c8d9e0f1a2b3_t1.vot"_s;
    const QString tip = visivo::wrappedToolTip(path, 40);
    QVERIFY(tip.contains(u'\n'));
    QCOMPARE(QString(tip).remove(u'\n'), path);
    for (const QString &line : tip.split(u'\n'))
        QVERIFY2(line.size() <= 40, qPrintable(line));
    QVERIFY(tip.split(u'\n').last().endsWith(u"_t1.vot"_s));
    // Broken after a separator, not inside a word.
    const QChar firstEnd = tip.split(u'\n').first().back();
    QVERIFY2(firstEnd == u'/' || firstEnd == u'_', qPrintable(tip));

    // No separator at all: hard breaks, still complete.
    const QString blob(150, u'x');
    const QString hard = visivo::wrappedToolTip(blob, 64);
    QCOMPARE(QString(hard).remove(u'\n'), blob);
    QCOMPARE(hard.split(u'\n').size(), 3);
    // Text with its own lines is left to them.
    const QString multi = u"line one\n"_s + QString(100, u'y');
    QCOMPARE(visivo::wrappedToolTip(multi), multi);
}
