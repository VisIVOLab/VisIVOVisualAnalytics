#include "TestSkavaClient.h"

#include "skava/SkavaClient.h"

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
