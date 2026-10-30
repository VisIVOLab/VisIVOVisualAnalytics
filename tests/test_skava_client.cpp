#include "TestSkavaClient.h"

#include "skava/BackendRouting.h"
#include "skava/BrokerClient.h"
#include "skava/NodeSoda.h"
#include "skava/SkavaClient.h"
#include "skava/VoTable.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;

namespace {

QByteArray fixture(const char *name)
{
    QFile f(QStringLiteral(VISIVO_TEST_DATA_DIR "/skava/") + QString::fromLatin1(name));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing SKAVA fixture %s", name);
    return f.readAll();
}

QJsonObject fixtureObject(const char *name)
{
    return QJsonDocument::fromJson(fixture(name)).object();
}

}  // namespace

void TestSkavaClient::parsesDiscoverySearch()
{
    const auto r = SkavaClient::parseDiscoveryResponse(fixtureObject("discovery_search.json"));
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.returned, 2);
    QVERIFY(r.total >= r.returned);
    QCOMPARE(r.results.size(), 2);
    const auto &first = r.results.first();
    QVERIFY(!first.metadata.obsId.isEmpty());
    QCOMPARE(first.metadata.dataproductType, u"cube"_s);
    QVERIFY(std::isfinite(first.metadata.sRa) && std::isfinite(first.metadata.sDec));
    QVERIFY(!first.replicas.isEmpty());
    QVERIFY(!first.bestNode.isEmpty());
}

void TestSkavaClient::parsesDatalink()
{
    const auto r = SkavaClient::parseDatalinkResponse(fixtureObject("datalink.json"));
    QVERIFY2(r.valid, qPrintable(r.error));
    QVERIFY(!r.primaryAccessUrl.isEmpty());
    QVERIFY(!r.bestNode.isEmpty());
    QCOMPARE(r.dataset.dataproductType, u"cube"_s);
    // The descriptor list carries five service types; only visivo-backend is kept.
    QCOMPARE(r.visivoBackends.size(), 1);
    const auto &vb = r.visivoBackends.first();
    QVERIFY(!vb.endpoint.isEmpty());
    QCOMPARE(vb.nodeCode, r.bestNode);
    QVERIFY(vb.supportedKinds.contains(u"cube"_s));
}

void TestSkavaClient::nullScoresStayNaN()
{
    // latency_score etc. are nullable ranking scores: absent must not read as 0.
    const QJsonObject rep = QJsonDocument::fromJson(
            R"({"code":"N","name":"n","is_enabled":true,"is_available":true,
                "latency_score":null,"access_endpoint":"file:///x"})").object();
    const auto r = SkavaClient::parseReplica(rep);
    QVERIFY(std::isnan(r.latencyScore));
    QVERIFY(std::isnan(r.loadScore));
    QCOMPARE(r.accessEndpoint, u"file:///x"_s);
}

void TestSkavaClient::parsesErrorEnvelope()
{
    const QString msg = SkavaClient::parseErrorEnvelope(fixture("error_envelope.json"));
    QVERIFY(msg.startsWith(u"dataset_not_found - "_s));
    QVERIFY(msg.contains(u"does-not-exist"_s));
    QVERIFY(SkavaClient::parseErrorEnvelope("<html>502 Bad Gateway</html>").isEmpty());
    QVERIFY(SkavaClient::parseErrorEnvelope(R"({"detail":"x"})").isEmpty());
}

void TestSkavaClient::explainsAnUnreachableNode()
{
    const QString detail = u"no_available_nodes - No available nodes for dataset 'x'"_s;
    const QString text = SkavaClient::explainHttpError(503, detail);
    QVERIFY(text.startsWith(detail));
    QVERIFY(text.contains(u"cannot reach any node"_s));
    QVERIFY(!text.contains(u"sign"_s, Qt::CaseInsensitive));
    QCOMPARE(SkavaClient::explainHttpError(404, u"dataset_not_found - x"_s), u"dataset_not_found - x"_s);
    QCOMPARE(SkavaClient::explainHttpError(503, u"overloaded"_s), u"overloaded"_s);
}

void TestSkavaClient::accessUrlForNode()
{
    auto r = SkavaClient::parseDatalinkResponse(fixtureObject("datalink.json"));
    QCOMPARE(r.accessUrlForNode(r.bestNode), r.primaryAccessUrl);
    QCOMPARE(r.accessUrlForNode(QString()), r.primaryAccessUrl);
    QVERIFY(r.accessUrlForNode(u"NOT-A-NODE"_s).isEmpty());
    SkavaReplica other;
    other.code = u"OTHER"_s;
    other.accessEndpoint = u"file:///elsewhere/cube.fits"_s;
    r.replicas.append(other);
    QCOMPARE(r.accessUrlForNode(u"OTHER"_s), u"file:///elsewhere/cube.fits"_s);
}

// ── VO protocol ─────────────────────────────────────────────────────────────

void TestSkavaClient::adqlFromFilter()
{
    SkavaSearchFilter f;
    f.posRa = 200.0;
    f.posDec = 42.0;
    f.posRadius = 0.5;
    f.bandMinMetres = 0.2;
    f.bandMaxMetres = 0.18;          // reversed on purpose
    f.dataproductType = u"cube"_s;
    f.limit = 10;
    f.offset = 20;
    const QString q = SkavaClient::buildObscoreAdql(f);
    QVERIFY(q.startsWith(u"SELECT TOP 30 * FROM ivoa.obscore WHERE "_s));
    QVERIFY(q.contains(u"1 = CONTAINS(POINT('ICRS', s_ra, s_dec), CIRCLE('ICRS', 200, 42, 0.5))"_s));
    QVERIFY(q.contains(u"em_min <= 0.2"_s) && q.contains(u"em_max >= 0.18"_s));
    QVERIFY(q.contains(u"dataproduct_type = 'cube'"_s));
    QVERIFY(!q.contains(u"t_min"_s));
    QCOMPARE(SkavaClient::buildObscoreAdql(SkavaSearchFilter{}),
             u"SELECT TOP 100 * FROM ivoa.obscore ORDER BY obs_publisher_did"_s);
}

void TestSkavaClient::adqlQuotesStrings()
{
    SkavaSearchFilter f;
    f.obsId = u"it's'; DROP"_s;
    QVERIFY(SkavaClient::buildObscoreAdql(f).contains(u"obs_id = 'it''s''; DROP'"_s));
}

void TestSkavaClient::parsesObscoreVotable()
{
    const auto r = SkavaClient::parseObscoreVotable(fixture("tap_obscore.vot"));
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.results.size(), 2);
    const auto &first = r.results.first();
    QCOMPARE(first.metadata.obsId, u"d22-apertif-dr1-HI"_s);
    QCOMPARE(first.metadata.dataproductType, u"cube"_s);
    QVERIFY(std::isfinite(first.metadata.sRa) && std::isfinite(first.metadata.emMax));
    QCOMPARE(first.accessFormat, SkavaClient::datalinkContentType());
    QVERIFY(first.accessUrl.contains(u"/datalink/d22-apertif-dr1-HI"_s));
}

void TestSkavaClient::parsesDatalinkVotable()
{
    const auto r = SkavaClient::parseDatalinkVotable(fixture("datalink.vot"), u"d22-apertif-dr1-HI"_s);
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.dataset.obsId, u"d22-apertif-dr1-HI"_s);
    QCOMPARE(r.primaryAccessUrl, u"file:///data/apertif/Apertif_DR1_field191_HI.fits"_s);
    QCOMPARE(r.bestNode, u"VIS-LOCAL"_s);
    QCOMPARE(r.primaryAccessEstSize, qint64(2000000000));
    QCOMPARE(r.replicas.size(), 1);
    QCOMPARE(r.visivoBackends.size(), 1);
    QCOMPARE(r.visivoBackends.first().endpoint, u"http://host.docker.internal:8001"_s);
    QCOMPARE(r.visivoBackends.first().nodeCode, u"VIS-LOCAL"_s);
    QVERIFY(!r.visivoBackends.first().requiresAuth);
    QVERIFY(r.sodaServiceUrl.endsWith(u"/soda/execute"_s));
    QCOMPARE(r.sodaStandardId, u"ivo://ivoa.net/std/SODA#sync-1.0"_s);
}

void TestSkavaClient::datalinkVotableRoutesLikeJson()
{
    // The VOTable and the JSON DataLink of the same dataset must route the
    // same way: same node, same replica.
    auto vo = SkavaClient::parseDatalinkVotable(fixture("datalink.vot"), u"d22-apertif-dr1-HI"_s);
    vo.dataset.dataproductType = u"cube"_s;     // as the panel merges it from the TAP row
    BackendNode local;
    local.id = u"local"_s;
    local.url = u"http://127.0.0.1:8001"_s;
    local.isLocal = true;
    const SkavaRoute route = routeSkavaDataset({ local }, vo);
    QVERIFY(route.valid());
    QCOMPARE(route.accessUrl, u"file:///data/apertif/Apertif_DR1_field191_HI.fits"_s);
    QCOMPARE(route.nodeCode, u"VIS-LOCAL"_s);
}

void TestSkavaClient::votableErrorsAreReported()
{
    const QByteArray err = R"(<?xml version="1.0"?><VOTABLE xmlns="http://www.ivoa.net/xml/VOTable/v1.4">
      <RESOURCE type="results"><INFO name="QUERY_STATUS" value="ERROR">Unsupported geometry</INFO></RESOURCE></VOTABLE>)";
    const auto r = SkavaClient::parseObscoreVotable(err);
    QVERIFY(!r.valid);
    QCOMPARE(r.error, u"Unsupported geometry"_s);
    const QByteArray bin = R"(<VOTABLE><RESOURCE type="results"><TABLE><FIELD name="obs_id"/>
      <DATA><BINARY2><STREAM encoding="base64">AAAA</STREAM></BINARY2></DATA></TABLE></RESOURCE></VOTABLE>)";
    QVERIFY(SkavaClient::parseObscoreVotable(bin).error.contains(u"votable/td"_s));
    QVERIFY(!SkavaClient::parseObscoreVotable("{\"not\": \"xml\"}").valid);
}

void TestSkavaClient::votableSkipsMetaResource()
{
    // A descriptive "meta" RESOURCE ahead of the results must not be taken for them.
    const QByteArray xml = R"(<VOTABLE>
      <RESOURCE type="meta"><TABLE><FIELD name="description"/><DATA><TABLEDATA><TR><TD>x</TD></TR></TABLEDATA></DATA></TABLE></RESOURCE>
      <RESOURCE type="results"><TABLE><FIELD name="obs_id"/><FIELD name="access_url"/>
        <DATA><TABLEDATA><TR><TD>a</TD><TD>http://h/a.fits</TD></TR></TABLEDATA></DATA></TABLE></RESOURCE></VOTABLE>)";
    const auto r = SkavaClient::parseObscoreVotable(xml);
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.results.size(), 1);
    QCOMPARE(r.results.first().metadata.obsId, u"a"_s);
}

namespace {
// A DataLink VOTable with the given {links} rows and service RESOURCEs.
QByteArray datalinkDoc(const QString &rows, const QString &services = {})
{
    return (u"<VOTABLE><RESOURCE type='results'><TABLE>"
            "<FIELD name='access_url'/><FIELD name='service_def'/><FIELD name='semantics'/>"
            "<FIELD name='link_auth'/><FIELD name='node_id'/><DATA><TABLEDATA>"
            "<TR><TD>file:///cube.fits</TD><TD/><TD>#this</TD><TD/><TD>A</TD></TR>"_s
            + rows + u"</TABLEDATA></DATA></TABLE></RESOURCE>"_s + services + u"</VOTABLE>"_s)
            .toUtf8();
}

QString linkRow(const QString &url, const QString &serviceDef, const QString &semantics,
                const QString &node, const QString &auth = {})
{
    return u"<TR><TD>%1</TD><TD>%2</TD><TD>%3</TD><TD>%4</TD><TD>%5</TD></TR>"_s
            .arg(url.toHtmlEscaped(), serviceDef, semantics, auth, node);
}

QString serviceResource(const QString &id, const QString &standardId, const QString &url)
{
    return u"<RESOURCE type='meta' utype='adhoc:service' ID='%1'>"
           "<PARAM name='standardID' datatype='char' arraysize='*' value='%2'/>"
           "<PARAM name='accessURL' datatype='char' arraysize='*' value='%3'/></RESOURCE>"_s
            .arg(id, standardId, url);
}
}  // namespace

void TestSkavaClient::votableStatusRowsMatchOnPath()
{
    // The backend is named by the status document's PATH: a trailing slash or
    // a query string on it changes nothing, and a status path hidden in some
    // other URL's query names no backend. "optional" means a token is in play.
    const QString rows = linkRow(u"https://node/v1/node/status/"_s, {}, u"#auxiliary"_s, u"A"_s, u"false"_s)
            + linkRow(u"https://far:8000/v1/node/status?detail=1"_s, {}, u"#auxiliary"_s, u"B"_s, u"optional"_s)
            + linkRow(u"https://x/other?redirect=/v1/node/status"_s, {}, u"#auxiliary"_s, u"C"_s)
            + linkRow(u"http://old/v1/health"_s, {}, u"#auxiliary"_s, u"D"_s, u"true"_s)
            + linkRow(u"https://proxy/tenant%2Fname/v1/node/status"_s, {}, u"#auxiliary"_s, u"E"_s);
    const auto r = SkavaClient::parseDatalinkVotable(datalinkDoc(rows), u"cube"_s);
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.visivoBackends.size(), 4);
    // A percent-encoded segment of the backend's path is kept as written.
    QCOMPARE(r.visivoBackends[3].endpoint, u"https://proxy/tenant%2Fname"_s);
    QCOMPARE(r.visivoBackends[0].endpoint, u"https://node"_s);
    QVERIFY(!r.visivoBackends[0].requiresAuth);
    QCOMPARE(r.visivoBackends[1].endpoint, u"https://far:8000"_s);
    QCOMPARE(r.visivoBackends[1].nodeCode, u"B"_s);
    QVERIFY(r.visivoBackends[1].requiresAuth);
    QCOMPARE(r.visivoBackends[2].endpoint, u"http://old"_s);   // older SKAVA still understood
    QVERIFY(r.visivoBackends[2].requiresAuth);
}

void TestSkavaClient::votablePrefersFirstSyncCutout()
{
    // Rows come best node first. An asynchronous cutout only stands in until
    // a synchronous one appears, and a later synchronous one (another node)
    // does not replace the first.
    const QString rows = linkRow({}, u"a"_s, u"#cutout"_s, u"A"_s)
            + linkRow({}, u"b"_s, u"#cutout"_s, u"A"_s)
            + linkRow({}, u"c"_s, u"#cutout"_s, u"B"_s)
            + linkRow({}, u"m"_s, u"#proc"_s, u"A"_s);
    const QString services = serviceResource(u"a"_s, u"ivo://ivoa.net/std/SODA#async-1.0"_s, u"https://best/soda/async"_s)
            + serviceResource(u"b"_s, u"ivo://ivoa.net/std/SODA#sync-1.0"_s, u"https://best/soda/sync"_s)
            + serviceResource(u"c"_s, u"ivo://ivoa.net/std/SODA#sync-1.0"_s, u"https://other/soda/sync"_s)
            + serviceResource(u"m"_s, u"ivo://ivoa.net/std/SODA#async-1.0"_s, u"https://best/soda/async"_s);
    const auto r = SkavaClient::parseDatalinkVotable(datalinkDoc(rows, services), u"cube"_s);
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.sodaServiceUrl, u"https://best/soda/sync"_s);
    QCOMPARE(r.sodaStandardId, u"ivo://ivoa.net/std/SODA#sync-1.0"_s);

    // Only asynchronous services: the first one is kept.
    const auto onlyAsync = SkavaClient::parseDatalinkVotable(
            datalinkDoc(linkRow({}, u"a"_s, u"#cutout"_s, u"A"_s) + linkRow({}, u"m"_s, u"#cutout"_s, u"B"_s),
                        serviceResource(u"a"_s, u"ivo://ivoa.net/std/SODA#async-1.0"_s, u"https://best/soda/async"_s)
                        + serviceResource(u"m"_s, u"ivo://ivoa.net/std/SODA#async-1.0"_s, u"https://other/soda/async"_s)),
            u"cube"_s);
    QCOMPARE(onlyAsync.sodaServiceUrl, u"https://best/soda/async"_s);
}

void TestSkavaClient::adqlOrdersWhenPaging()
{
    SkavaSearchFilter f;
    f.limit = 10;
    QVERIFY(SkavaClient::buildObscoreAdql(f).endsWith(u" ORDER BY obs_publisher_did"_s));
    f.offset = 10;
    QVERIFY(SkavaClient::buildObscoreAdql(f).endsWith(u" ORDER BY obs_publisher_did"_s));
}

// ── The routing decision and the node services ─────────────────────────────
// datalink_testbed.{json,vot} and discovery_testbed.json were produced by the
// SKAVA testbed (SKAVA/testbed, four nodes, resource-aware policy) for the
// synthetic cube tb-cube-l.

void TestSkavaClient::parsesRoutingDecisionJson()
{
    const auto r = SkavaClient::parseDatalinkResponse(fixtureObject("datalink_testbed.json"));
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.policy, u"resource_aware"_s);
    QCOMPARE(r.bestNode, u"BIG"_s);
    QCOMPARE(r.replicas.size(), 3);
    // Ranking order, each with the policy's reason and cost.
    QCOMPARE(r.replicas[0].code, u"BIG"_s);
    QVERIFY(r.replicas[0].rankReason.startsWith(u"rtt "_s));
    QVERIFY(std::isfinite(r.replicas[0].score) && std::isfinite(r.replicas[1].score));
    QVERIFY(r.replicas[0].score < r.replicas[1].score);
    QVERIFY(std::isfinite(r.replicas[0].telemetryAgeS));
    QVERIFY(r.excluded.isEmpty());
    QVERIFY(r.replica(u"SMALL"_s) != nullptr);
    QCOMPARE(r.replica(u"SMALL"_s)->code, u"SMALL"_s);
    QVERIFY(r.replica(u"NOPE"_s) == nullptr);

    // The replicas a policy set aside come with the reason.
    QJsonObject obj = fixtureObject("datalink_testbed.json");
    QJsonObject routing = obj.value(u"routing"_s).toObject();
    routing.insert(u"excluded"_s, QJsonArray{ QJsonObject{ { u"code"_s, u"FAR"_s },
                                                           { u"reason"_s, u"node down: ConnectError"_s } } });
    obj.insert(u"routing"_s, routing);
    const auto ex = SkavaClient::parseDatalinkResponse(obj);
    QCOMPARE(ex.excluded.size(), 1);
    QCOMPARE(ex.excluded[0].code, u"FAR"_s);
    QVERIFY(ex.excluded[0].reason.startsWith(u"node down"_s));

    // The search results carry the same reasons per replica.
    const auto s = SkavaClient::parseDiscoveryResponse(fixtureObject("discovery_testbed.json"));
    QVERIFY2(s.valid, qPrintable(s.error));
    QVERIFY(!s.results.isEmpty());
    QVERIFY(s.results[0].replicas[0].rankReason.startsWith(u"rtt "_s));
}

void TestSkavaClient::parsesNodeServicesJson()
{
    const auto r = SkavaClient::parseDatalinkResponse(fixtureObject("datalink_testbed.json"));
    QVERIFY2(r.valid, qPrintable(r.error));
    // The best node's cutout (sync and as a job), moment and PV; SKAVA's own
    // soda-sync / cutout proxies are not node services.
    QCOMPARE(r.services.size(), 4);
    const SkavaNodeService *cut = r.service(u"cutout-sync"_s);
    QVERIFY(cut != nullptr);
    QCOMPARE(cut->nodeCode, u"BIG"_s);
    QCOMPARE(cut->accessUrl, u"http://node-big:8000/soda/sync"_s);
    QCOMPARE(cut->standardId, u"ivo://ivoa.net/std/SODA#sync-1.0"_s);
    QVERIFY(cut->requiresAuth);
    QCOMPARE(cut->inputParams.value(u"ID"_s), u"file:///data/cubes/tb-cube-l.fits"_s);
    QVERIFY(r.service(u"cutout-async"_s) != nullptr);
    const SkavaNodeService *moment = r.service(u"moment"_s);
    QVERIFY(moment != nullptr);
    QCOMPARE(moment->accessUrl, u"http://node-big:8000/soda/async"_s);
    QCOMPARE(moment->inputParams.value(u"OPERATION"_s), u"MOMENT"_s);
    QCOMPARE(r.service(u"pv"_s)->inputParams.value(u"OPERATION"_s), u"PV"_s);
    // On a node: the best one has them, the others do not.
    QVERIFY(r.service(u"moment"_s, u"BIG"_s) != nullptr);
    QVERIFY(r.service(u"moment"_s, u"SMALL"_s) == nullptr);
    // The legacy fields name the synchronous cutout.
    QCOMPARE(r.sodaServiceUrl, u"http://node-big:8000/soda/sync"_s);
    QCOMPARE(r.sodaStandardId, u"ivo://ivoa.net/std/SODA#sync-1.0"_s);
}

void TestSkavaClient::parsesNodeServicesVotable()
{
    const auto r = SkavaClient::parseDatalinkVotable(fixture("datalink_testbed.vot"), u"tb-cube-l"_s);
    QVERIFY2(r.valid, qPrintable(r.error));
    const auto json = SkavaClient::parseDatalinkResponse(fixtureObject("datalink_testbed.json"));
    // The VOTable and the JSON of the same dataset agree on the decision...
    QCOMPARE(r.bestNode, json.bestNode);
    QCOMPARE(r.replicas.size(), json.replicas.size());
    for (int i = 0; i < r.replicas.size(); ++i) {
        QCOMPARE(r.replicas[i].code, json.replicas[i].code);
        QVERIFY(r.replicas[i].rankReason.startsWith(u"rtt "_s));
        QVERIFY(std::isfinite(r.replicas[i].score));
    }
    QVERIFY(r.replicas[0].score < r.replicas[2].score);
    // ...and on where each copy lies on its node, although the VOTable's
    // #this links lead a client to the node's SODA instead.
    QCOMPARE(r.primaryAccessUrl, json.primaryAccessUrl);
    for (int i = 0; i < r.replicas.size(); ++i)
        QCOMPARE(r.replicas[i].accessEndpoint, json.accessUrlForNode(json.replicas[i].code));
    QVERIFY(r.primaryAccessUrl.startsWith(u"file:///"_s));
    // Sizes in bytes on both paths (the JSON's access_estsize is kilobytes).
    QCOMPARE(r.primaryAccessEstSize, json.primaryAccessEstSize);
    QCOMPARE(r.primaryAccessEstSize, qint64(32771) * 1024);
    // ...and on the services, with the inputs the node fixes.
    QCOMPARE(r.services.size(), json.services.size());
    for (int i = 0; i < r.services.size(); ++i) {
        QCOMPARE(r.services[i].kind, json.services[i].kind);
        QCOMPARE(r.services[i].accessUrl, json.services[i].accessUrl);
        QCOMPARE(r.services[i].nodeCode, json.services[i].nodeCode);
        QCOMPARE(r.services[i].requiresAuth, json.services[i].requiresAuth);
        QCOMPARE(r.services[i].inputParams, json.services[i].inputParams);
    }
    QCOMPARE(r.service(u"pv"_s)->inputParams.value(u"ID"_s), u"file:///data/cubes/tb-cube-l.fits"_s);
    QCOMPARE(r.sodaServiceUrl, json.sodaServiceUrl);
    // A VOTable without the decision columns (older SKAVA) still parses.
    const auto old = SkavaClient::parseDatalinkVotable(fixture("datalink.vot"), u"d22-apertif-dr1-HI"_s);
    QVERIFY2(old.valid, qPrintable(old.error));
    QVERIFY(old.replicas[0].rankReason.isEmpty() && std::isnan(old.replicas[0].score));
}

void TestSkavaClient::nodeServiceKinds()
{
    const QString sync = u"ivo://ivoa.net/std/SODA#sync-1.0"_s;
    const QString async = u"ivo://ivoa.net/std/SODA#async-1.0"_s;
    QCOMPARE(SkavaClient::nodeServiceKind(u"#cutout"_s, sync, {}), u"cutout-sync"_s);
    QCOMPARE(SkavaClient::nodeServiceKind(u"#cutout"_s, async, {}), u"cutout-async"_s);
    QCOMPARE(SkavaClient::nodeServiceKind(u"#cutout"_s, {}, {}), u"cutout-sync"_s);
    QCOMPARE(SkavaClient::nodeServiceKind(u"#proc"_s, async, u"moment"_s), u"moment"_s);
    QCOMPARE(SkavaClient::nodeServiceKind(u"#proc"_s, async, u"PV"_s), u"pv"_s);
    // A product is a job; an unknown operation is not a service VisIVO runs.
    QVERIFY(SkavaClient::nodeServiceKind(u"#proc"_s, sync, u"MOMENT"_s).isEmpty());
    QVERIFY(SkavaClient::nodeServiceKind(u"#proc"_s, async, u"SMOOTH"_s).isEmpty());
    QVERIFY(SkavaClient::nodeServiceKind(u"#this"_s, async, u"PV"_s).isEmpty());
    // A #cutout behind a service of another standard is not a SODA to call.
    QVERIFY(SkavaClient::nodeServiceKind(u"#cutout"_s, u"ivo://other/service#async-1.0"_s, {}).isEmpty());
    QVERIFY(SkavaClient::nodeServiceKind(u"#cutout"_s, u"ivo://ivoa.net/std/SIA#query-2.0"_s, {}).isEmpty());
    QCOMPARE(SkavaClient::nodeServiceKind(u"#cutout"_s, u"IVO://IVOA.NET/STD/SODA#SYNC-1.0"_s, {}), u"cutout-sync"_s);

    // Without standardIDs both cutouts count as synchronous: the FIRST one
    // is kept, the second does not replace it.
    const QByteArray xml = R"(<VOTABLE><RESOURCE><TABLE><FIELD name="semantics"/><FIELD name="access_url"/><FIELD name="service_def"/>
      <DATA><TABLEDATA><TR><TD>#this</TD><TD>file:///a.fits</TD><TD/></TR><TR><TD>#cutout</TD><TD/><TD>first</TD></TR>
      <TR><TD>#cutout</TD><TD/><TD>second</TD></TR></TABLEDATA></DATA></TABLE></RESOURCE>
      <RESOURCE type="meta" utype="adhoc:service" ID="first"><PARAM name="accessURL" value="https://first/soda/sync"/></RESOURCE>
      <RESOURCE type="meta" utype="adhoc:service" ID="second"><PARAM name="accessURL" value="https://second/soda/sync"/></RESOURCE></VOTABLE>)";
    const auto r = SkavaClient::parseDatalinkVotable(xml, u"x"_s);
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.services.size(), 2);
    QCOMPARE(r.sodaServiceUrl, u"https://first/soda/sync"_s);
}

namespace {
// A dataset with replicas on A, B and C, a backend on each, ranked A, B, C.
SkavaDatalinkResponse threeNodes()
{
    SkavaDatalinkResponse dl;
    dl.valid = true;
    dl.dataset.obsId = u"x"_s;
    dl.dataset.dataproductType = u"cube"_s;
    dl.bestNode = u"A"_s;
    dl.primaryAccessUrl = u"file:///a/x.fits"_s;
    dl.policy = u"resource_aware"_s;
    for (const QString &code : { u"A"_s, u"B"_s, u"C"_s }) {
        SkavaReplica rep;
        rep.code = code;
        rep.accessEndpoint = u"file:///%1/x.fits"_s.arg(code.toLower());
        rep.rankReason = u"reason %1"_s.arg(code);
        rep.score = code == u"A"_s ? 1.0 : code == u"B"_s ? 2.0 : 3.0;
        dl.replicas.append(rep);
        SkavaVisivoBackendDescriptor desc;
        desc.endpoint = u"http://%1:8000"_s.arg(code.toLower());
        desc.nodeCode = code;
        desc.requiresAuth = code == u"A"_s;
        dl.visivoBackends.append(desc);
    }
    return dl;
}

QList<BackendNode> registryFor(const QStringList &codes)
{
    QList<BackendNode> registry;
    for (const QString &code : codes) {
        BackendNode node;
        node.id = u"src-"_s + code.toLower();
        node.url = u"http://%1:8000"_s.arg(code.toLower());
        node.srcCode = code;
        registry.append(node);
    }
    return registry;
}

QStringList codesOf(const SkavaRouting &routing)
{
    QStringList out;
    for (const auto &route : routing.candidates)
        out << route.nodeCode;
    return out;
}
}  // namespace

void TestSkavaClient::routesCandidatesInOrder()
{
    const SkavaDatalinkResponse dl = threeNodes();
    // A wants a token the registry has not got: B and C remain, in SKAVA's order.
    const SkavaRouting routing = routeSkavaCandidates(registryFor({ u"A"_s, u"B"_s, u"C"_s }), dl);
    QVERIFY(routing.valid());
    QCOMPARE(codesOf(routing), (QStringList{ u"B"_s, u"C"_s }));
    QCOMPARE(routing.skipped.size(), 1);
    QVERIFY(routing.skipped[0].startsWith(u"A: the backend requires a token"_s));
    // Each candidate carries its own replica and SKAVA's reason for the node.
    QCOMPARE(routing.candidates[0].accessUrl, u"file:///b/x.fits"_s);
    QCOMPARE(routing.candidates[0].rankReason, u"reason B"_s);
    QCOMPARE(routing.candidates[0].score, 2.0);
    QCOMPARE(routing.candidates[1].node.id, u"src-c"_s);
    // The single-route form is the first candidate, with every skip reason.
    const SkavaRoute first = routeSkavaDataset(registryFor({ u"A"_s, u"B"_s, u"C"_s }), dl);
    QCOMPARE(first.nodeCode, u"B"_s);
    QCOMPARE(first.skipped, routing.skipped);
    // The automatic token is the local backend's: it does not make the
    // remote A usable.
    QCOMPARE(codesOf(routeSkavaCandidates(registryFor({ u"A"_s, u"B"_s, u"C"_s }), dl, true)),
             (QStringList{ u"B"_s, u"C"_s }));
    // Nothing registered: no candidate, every descriptor explained.
    const SkavaRouting none = routeSkavaCandidates(QList<BackendNode>{}, dl);
    QVERIFY(!none.valid());
    QCOMPARE(none.skipped.size(), 3);
    QVERIFY(!routeSkavaDataset(QList<BackendNode>{}, dl).valid());
}

void TestSkavaClient::preferredNodeGoesFirst()
{
    const SkavaDatalinkResponse dl = threeNodes();
    const auto registry = registryFor({ u"A"_s, u"B"_s, u"C"_s });
    // The node asked for leads; the others keep SKAVA's order behind it.
    auto withTokens = registry;
    withTokens[0].token = u"a-token"_s;
    QCOMPARE(codesOf(routeSkavaCandidates(withTokens, dl, false, u"C"_s)),
             (QStringList{ u"C"_s, u"A"_s, u"B"_s }));
    // Asking for a node that cannot serve changes nothing: it stays explained.
    const SkavaRouting unusable = routeSkavaCandidates(registry, dl, false, u"A"_s);
    QCOMPARE(codesOf(unusable), (QStringList{ u"B"_s, u"C"_s }));
    QCOMPARE(unusable.skipped.size(), 1);
    // An unknown node changes nothing either.
    QCOMPARE(codesOf(routeSkavaCandidates(withTokens, dl, false, u"Z"_s)),
             (QStringList{ u"A"_s, u"B"_s, u"C"_s }));
}

// ── The node's SODA job and the broker's job ───────────────────────────────

void TestSkavaClient::sodaJobJson()
{
    // The JSON view of a job on the node (Accept: application/json), done:
    // the product's path on the node is what the desktop opens.
    const QByteArray done = R"({"job_id":"task_1","phase":"COMPLETED","operation":"MOMENT",
        "parameters":{"ID":"file:///data/x.fits","MOMENT":"0"},"progress":1.0,
        "results":[{"id":"result","href":"http://node/soda/async/task_1/results/result"}],
        "output_path":"/root/.visivo/exports/soda/task_1/x_moment0_ab12cd34.fits","applied":"","error":null})";
    const NodeSodaJobResult r = NodeSodaClient::parseJobJson(done);
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.phase, u"COMPLETED"_s);
    QCOMPARE(r.outputPath, u"/root/.visivo/exports/soda/task_1/x_moment0_ab12cd34.fits"_s);
    QCOMPARE(r.resultUrl, u"http://node/soda/async/task_1/results/result"_s);
    // Still running: no error, not valid yet.
    const NodeSodaJobResult running = NodeSodaClient::parseJobJson(R"({"job_id":"t","phase":"EXECUTING","results":[],"error":null})");
    QVERIFY(!running.valid && running.error.isEmpty());
    QCOMPARE(running.phase, u"EXECUTING"_s);
    // Failed: the node's message.
    const NodeSodaJobResult failed = NodeSodaClient::parseJobJson(R"({"job_id":"t","phase":"ERROR","results":[],"error":"UsageError: BAND outside the cube"})");
    QVERIFY(!failed.valid);
    QCOMPARE(failed.error, u"UsageError: BAND outside the cube"_s);
    // An empty selection completes with a word, not a product.
    const NodeSodaJobResult empty = NodeSodaClient::parseJobJson(R"({"job_id":"t","phase":"COMPLETED","results":[{"id":"result","href":"h"}],"no_data":"The selection is empty.","error":null})");
    QVERIFY(empty.valid && empty.outputPath.isEmpty());
    QCOMPARE(empty.noData, u"The selection is empty."_s);
    // Not a job at all, or not a phase UWS has: nothing to wait for.
    QVERIFY(!NodeSodaClient::parseJobJson("<uws:job/>").valid);
    QVERIFY(!NodeSodaClient::parseJobJson(R"({"detail":"Not Found"})").valid);
    const NodeSodaJobResult bogus = NodeSodaClient::parseJobJson(R"({"job_id":"t","phase":"bogus","results":[]})");
    QVERIFY(!bogus.valid && bogus.phase.isEmpty() && bogus.error.contains(u"bogus"_s));

    // The form the job is created with: SODA parameters, URL-encoded.
    const QByteArray form = NodeSodaClient::encodeForm({ { u"ID"_s, u"file:///data/x y.fits"_s },
                                                         { u"POS"_s, u"CIRCLE 10 -5 0.5"_s },
                                                         { u"BAND"_s, u"0.21 0.22"_s } });
    QCOMPARE(form, QByteArray("ID=file%3A%2F%2F%2Fdata%2Fx%20y.fits&POS=CIRCLE%2010%20-5%200.5&BAND=0.21%200.22"));
}

void TestSkavaClient::sodaServiceUrlOnBackend()
{
    // SKAVA names the node's service as the catalogue sees it; the desktop
    // calls it on the backend it registered for that node.
    QCOMPARE(NodeSodaClient::serviceUrlOn(u"http://localhost:18101"_s, u"http://node-big:8000/soda/async"_s,
                                          u"http://node-big:8000"_s),
             u"http://localhost:18101/soda/async"_s);
    QCOMPARE(NodeSodaClient::serviceUrlOn(u"https://src.example/visivo/"_s, u"http://node-big:8000/soda/sync"_s),
             u"https://src.example/visivo/soda/sync"_s);
    QCOMPARE(NodeSodaClient::serviceUrlOn(QString(), u"http://node-big:8000/soda/async"_s),
             u"http://node-big:8000/soda/async"_s);
    // A backend behind a prefix: the service's path under ITS backend goes
    // under the registered one, with the query; the prefix is not doubled.
    QCOMPARE(NodeSodaClient::serviceUrlOn(u"https://host/api"_s, u"https://host/api/soda/async?route=x"_s,
                                          u"https://host/api/"_s),
             u"https://host/api/soda/async?route=x"_s);
    QCOMPARE(NodeSodaClient::serviceUrlOn(u"https://desk.example/node"_s, u"https://host/api/soda/async?route=x"_s,
                                          u"https://host/api"_s),
             u"https://desk.example/node/soda/async?route=x"_s);
    // Not under the base it claims: the whole path is kept.
    QCOMPARE(NodeSodaClient::serviceUrlOn(u"https://desk.example"_s, u"https://other/v2/soda/async"_s,
                                          u"https://host/api"_s),
             u"https://desk.example/v2/soda/async"_s);
}

void TestSkavaClient::brokerJobJson()
{
    // broker_job.json: a finished job of the testbed's Computing Broker mock.
    const BrokerJob job = BrokerClient::parseJob(fixture("broker_job.json"));
    QVERIFY2(job.valid, qPrintable(job.error));
    QCOMPARE(job.status, u"COMPLETE"_s);
    QVERIFY(job.ended());
    QCOMPARE(job.node, u"BIG"_s);
    QCOMPARE(job.backend, u"http://node-big:8000"_s);
    QVERIFY(job.rankReason.startsWith(u"rtt "_s));
    QCOMPARE(job.finder, u"sofia2"_s);
    QCOMPARE(job.nSources, 5);
    QVERIFY(job.maskPath.endsWith(u"/sofia_out_mask.fits"_s));
    QVERIFY(job.catalogPath.endsWith(u"/sofia_out_cat.xml"_s));
    QVERIFY(!job.taskId.isEmpty());
    QVERIFY(job.error.isEmpty());
    // A failed job carries the broker's error; a pending one has no products.
    const BrokerJob failed = BrokerClient::parseJob(R"({"id":"job-1","status":"FAILED","error":"dataset 'nope' is not in the catalogue","placement":{},"result":null})");
    QVERIFY(failed.valid && failed.ended());
    QVERIFY(failed.error.startsWith(u"dataset 'nope'"_s));
    QCOMPARE(failed.nSources, -1);
    const BrokerJob pending = BrokerClient::parseJob(R"({"id":"job-2","status":"PENDING","request":{"parameters":{"finder":"photutils"}}})");
    QVERIFY(pending.valid && !pending.ended());
    QCOMPARE(pending.finder, u"photutils"_s);
    QVERIFY(!BrokerClient::parseJob(R"({"detail":"Not Found"})").valid);
    QVERIFY(!BrokerClient::parseJob("not json").valid);
    // A job with no status, or a status the broker's API has not got, is not one to wait for.
    QVERIFY(!BrokerClient::parseJob(R"({"id":"job-3","status":null})").valid);
    QVERIFY(!BrokerClient::parseJob(R"({"id":"job-4","status":"bogus"})").valid);
}

// ── Sign-in ────────────────────────────────────────────────────────────────

void TestSkavaClient::authChallenge()
{
    // SKAVA's (and SRCNet's) 401: a Bearer challenge plus the IVOA one that
    // names the IAM's discovery document.
    const auto c = SkavaClient::parseAuthChallenge(
            u"Bearer realm=\"SKAVA\", ivoa_bearer standard_id=\"ivo://ivoa.net/sso#BearerToken\", "
            u"access_url=\"https://iam.example/.well-known/openid-configuration\""_s);
    QVERIFY(c.required);
    QCOMPARE(c.scheme, u"ivoa_bearer"_s);
    QCOMPARE(c.realm, u"SKAVA"_s);
    QCOMPARE(c.wellKnownUrl, u"https://iam.example/.well-known/openid-configuration"_s);
    // A plain Bearer challenge: a token is wanted, no IAM is named.
    const auto bearer = SkavaClient::parseAuthChallenge(u"Bearer realm=api, error=\"invalid_token\""_s);
    QVERIFY(bearer.required);
    QCOMPARE(bearer.scheme, u"bearer"_s);
    QCOMPARE(bearer.realm, u"api"_s);
    QVERIFY(bearer.wellKnownUrl.isEmpty());
    // The IVOA challenge alone, and in the other order.
    const auto ivoa = SkavaClient::parseAuthChallenge(u"ivoa_bearer access_url=\"https://iam/wk\", Bearer"_s);
    QCOMPARE(ivoa.scheme, u"ivoa_bearer"_s);
    QCOMPARE(ivoa.wellKnownUrl, u"https://iam/wk"_s);
    // A quoted value with an escaped quote and a comma, and a token68
    // challenge (Negotiate) ahead of the IVOA one: neither hides the IAM.
    const auto escaped = SkavaClient::parseAuthChallenge(
            u"Basic realm=\"SRC \\\"Science\\\", EU\", ivoa_bearer access_url=\"https://iam/wk?a=1,b=2\""_s);
    QCOMPARE(escaped.realm, u"SRC \"Science\", EU"_s);
    QCOMPARE(escaped.wellKnownUrl, u"https://iam/wk?a=1,b=2"_s);
    const auto negotiate = SkavaClient::parseAuthChallenge(u"Negotiate abc/def==, ivoa_bearer access_url=\"https://iam/wk\""_s);
    QCOMPARE(negotiate.scheme, u"ivoa_bearer"_s);
    QCOMPARE(negotiate.wellKnownUrl, u"https://iam/wk"_s);
    // Case does not matter, and a scheme's parameters stop at the next scheme.
    const auto mixed = SkavaClient::parseAuthChallenge(u"IVOA_BEARER ACCESS_URL=https://iam/wk, Bearer realm=x"_s);
    QCOMPARE(mixed.wellKnownUrl, u"https://iam/wk"_s);
    QCOMPARE(mixed.realm, u"x"_s);
    // Garbage ahead of a challenge, however long, is skipped (no recursion).
    const auto garbage = SkavaClient::parseAuthChallenge(QString(50'000, u'(') + u", ivoa_bearer access_url=\"https://iam/wk\""_s);
    QCOMPARE(garbage.wellKnownUrl, u"https://iam/wk"_s);
    // A token68 is consumed whole: a scheme name inside it is not a scheme.
    const auto inside = SkavaClient::parseAuthChallenge(u"Negotiate abc/ivoa_bearer==, Bearer realm=x"_s);
    QCOMPARE(inside.scheme, u"negotiate"_s);   // the first scheme; no ivoa_bearer in sight
    QVERIFY(inside.wellKnownUrl.isEmpty());
    QCOMPARE(inside.realm, u"x"_s);
    const auto unpadded = SkavaClient::parseAuthChallenge(u"Negotiate abc/def, ivoa_bearer access_url=\"https://iam/wk\""_s);
    QCOMPARE(unpadded.wellKnownUrl, u"https://iam/wk"_s);
    const auto leading = SkavaClient::parseAuthChallenge(u"Negotiate /ivoa_bearer, Bearer realm=x"_s);
    QCOMPARE(leading.scheme, u"negotiate"_s);
    QVERIFY(leading.wellKnownUrl.isEmpty());
    QCOMPARE(leading.realm, u"x"_s);
    // A bare token68 (no '/', no padding) right after its scheme is the
    // scheme's payload, not a scheme: schemes are separated by commas.
    const auto bare = SkavaClient::parseAuthChallenge(u"Negotiate ivoa_bearer, Bearer realm=x"_s);
    QCOMPARE(bare.scheme, u"negotiate"_s);
    QVERIFY(bare.wellKnownUrl.isEmpty());
    QCOMPARE(bare.realm, u"x"_s);
    const auto commas = SkavaClient::parseAuthChallenge(u"Negotiate, ivoa_bearer access_url=\"https://iam/wk\""_s);
    QCOMPARE(commas.scheme, u"ivoa_bearer"_s);
    QCOMPARE(commas.wellKnownUrl, u"https://iam/wk"_s);
    // No header: nothing was asked.
    QVERIFY(!SkavaClient::parseAuthChallenge(QString()).required);
}

void TestSkavaClient::oidcDocument()
{
    const auto e = SkavaClient::parseOidcDocument(
            R"({"issuer":"https://iam.example","authorization_endpoint":"https://iam.example/auth",
                "token_endpoint":"https://iam.example/token","jwks_uri":"https://iam.example/jwks"})");
    QVERIFY2(e.valid(), qPrintable(e.error));
    QCOMPARE(e.authorizationUrl, u"https://iam.example/auth"_s);
    QCOMPARE(e.tokenUrl, u"https://iam.example/token"_s);
    QVERIFY(!SkavaClient::parseOidcDocument(R"({"issuer":"x"})").valid());
    QVERIFY(!SkavaClient::parseOidcDocument("<html/>").valid());
}

void TestSkavaClient::resolveAccessUsesTheRow()
{
    // Two ObsCore products may share an obs_id: a direct-download row resolves to ITS
    // access_url, with no network round trip and no invented /datalink/<obs_id> URL.
    SkavaClient client(u"http://127.0.0.1:9"_s);
    client.setProtocol(SkavaClient::Protocol::Vo);
    SkavaDiscoveryResult row;
    row.metadata.obsId = u"shared"_s;
    row.accessUrl = u"http://archive/product2.fits"_s;
    row.accessFormat = u"application/fits"_s;
    const auto r = client.resolveAccess(row);
    QVERIFY(r.valid);
    QCOMPARE(r.primaryAccessUrl, u"http://archive/product2.fits"_s);
    QCOMPARE(r.dataset.obsId, u"shared"_s);
    QVERIFY(r.visivoBackends.isEmpty());
    // A quoted, mixed-case DataLink format is still DataLink: the row is NOT a direct file.
    // (Port 9 refuses → a network error, not a synthesised file URL.)
    row.accessUrl = u"http://127.0.0.1:9/datalink/x"_s;
    row.accessFormat = u"Application/X-VOTable+XML; Content=\"DataLink\""_s;
    const auto dl = client.resolveAccess(row);
    QVERIFY(!dl.valid);
    QVERIFY(dl.primaryAccessUrl.isEmpty());
    QCOMPARE(SkavaClient::protocolFromString(u" VO "_s), SkavaClient::Protocol::Vo);
    QCOMPARE(SkavaClient::protocolFromString(u"skava"_s), SkavaClient::Protocol::SkavaJson);
}
