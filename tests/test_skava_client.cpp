#include "TestSkavaClient.h"

#include "skava/BackendRouting.h"
#include "skava/SkavaClient.h"
#include "skava/VoTable.h"

#include <QFile>
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

void TestSkavaClient::adqlOrdersWhenPaging()
{
    SkavaSearchFilter f;
    f.limit = 10;
    QVERIFY(SkavaClient::buildObscoreAdql(f).endsWith(u" ORDER BY obs_publisher_did"_s));
    f.offset = 10;
    QVERIFY(SkavaClient::buildObscoreAdql(f).endsWith(u" ORDER BY obs_publisher_did"_s));
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
