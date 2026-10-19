#include "TestBackendRouting.h"

#include "Settings.h"
#include "skava/BackendRouting.h"
#include "skava/SkavaClient.h"

#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

// A non-local registry node.
BackendNode makeNode(const QString &id, const QString &url, const QString &srcCode)
{
    BackendNode n;
    n.id = id;
    n.name = id;
    n.url = url;
    n.token = QString();
    n.srcCode = srcCode;
    n.isLocal = false;
    n.isBuiltin = false;
    return n;
}

// A DataLink response advertising a single visivo-backend descriptor.
SkavaDatalinkResponse datalinkWith(const QString &endpoint, const QString &nodeCode)
{
    SkavaVisivoBackendDescriptor d;
    d.endpoint = endpoint;
    d.nodeCode = nodeCode;
    SkavaDatalinkResponse r;
    r.valid = true;
    // As SKAVA answers: the descriptor's node is the best one, and the
    // primary access URL is its replica.
    r.bestNode = nodeCode;
    r.primaryAccessUrl = u"file:///data/best/cube.fits"_s;
    r.visivoBackends.append(d);
    return r;
}

SkavaVisivoBackendDescriptor descriptor(const QString &endpoint, const QString &nodeCode,
                                        const QStringList &kinds = {}, bool requiresAuth = false)
{
    SkavaVisivoBackendDescriptor d;
    d.endpoint = endpoint;
    d.nodeCode = nodeCode;
    d.supportedKinds = kinds;
    d.requiresAuth = requiresAuth;
    return d;
}

SkavaReplica replica(const QString &code, const QString &accessEndpoint)
{
    SkavaReplica r;
    r.code = code;
    r.accessEndpoint = accessEndpoint;
    return r;
}

// Two nodes hold the cube: A is SKAVA's best (primary access), B a replica.
SkavaDatalinkResponse twoNodeDatalink()
{
    SkavaDatalinkResponse r;
    r.valid = true;
    r.bestNode = u"A"_s;
    r.primaryAccessUrl = u"file:///srv/a/cube.fits"_s;
    r.dataset.dataproductType = u"cube"_s;
    r.replicas = { replica(u"A"_s, u"file:///srv/a/cube.fits"_s),
                   replica(u"B"_s, u"file:///mnt/b/archive/cube.fits"_s) };
    r.visivoBackends = { descriptor(u"https://a.example/visivo"_s, u"A"_s, { u"cube"_s }),
                         descriptor(u"https://b.example/visivo"_s, u"B"_s, { u"cube"_s }) };
    return r;
}

}  // namespace

void TestBackendRouting::registryAlwaysHasLocal()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    const auto nodes = s.getBackendNodes();
    bool foundLocal = false;
    for (const auto &n : nodes) {
        if (n.id == "local"_L1) {
            foundLocal = true;
            QVERIFY(n.isLocal);
        }
    }
    QVERIFY2(foundLocal, "the 'local' backend must always be present");
}

void TestBackendRouting::upsertInsertsAndUpdates()
{
    QTemporaryDir dir;
    Settings s(dir.path());

    s.upsertBackendNode(makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s));
    QCOMPARE(s.getBackendNode(u"src-a"_s).url, u"https://a.example/visivo"_s);

    // Upsert with the same id updates in place (no duplicate).
    s.upsertBackendNode(makeNode(u"src-a"_s, u"https://a2.example/visivo"_s, u"A"_s));
    QCOMPARE(s.getBackendNode(u"src-a"_s).url, u"https://a2.example/visivo"_s);

    int count = 0;
    for (const auto &n : s.getBackendNodes())
        if (n.id == "src-a"_L1) ++count;
    QCOMPARE(count, 1);
}

void TestBackendRouting::removeDropsNode()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s));
    QVERIFY(!s.getBackendNode(u"src-b"_s).id.isEmpty());

    s.removeBackendNode(u"src-b"_s);
    QVERIFY2(s.getBackendNode(u"src-b"_s).id.isEmpty(), "removed node must be gone");
}

void TestBackendRouting::localCannotBeRemoved()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.removeBackendNode(u"local"_s);  // must be a no-op
    QCOMPARE(s.getBackendNode(u"local"_s).id, u"local"_s);
}

void TestBackendRouting::defaultBackendIdFallsBackToLocal()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    QCOMPARE(s.getDefaultBackendId(), u"local"_s);
    s.setDefaultBackendId(QString());  // empty → coerced back to local
    QCOMPARE(s.getDefaultBackendId(), u"local"_s);
    s.setDefaultBackendId(u"src-a"_s);
    QCOMPARE(s.getDefaultBackendId(), u"src-a"_s);
}

void TestBackendRouting::routeByExactUrl()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-it"_s, u"https://srcit.inaf.it/visivo"_s, u"INAF-IT"_s));

    const auto pick = pickBackendForSkavaDataset(
            &s, datalinkWith(u"https://srcit.inaf.it/visivo"_s, QString()));
    QCOMPARE(pick.id, u"src-it"_s);
}

void TestBackendRouting::routeByUrlIgnoresTrailingSlash()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-it"_s, u"https://srcit.inaf.it/visivo"_s, u"INAF-IT"_s));

    // Descriptor URL has a trailing slash; registry URL does not — must match.
    const auto pick = pickBackendForSkavaDataset(
            &s, datalinkWith(u"https://srcit.inaf.it/visivo/"_s, QString()));
    QCOMPARE(pick.id, u"src-it"_s);
}

void TestBackendRouting::routeByNodeCode()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-it"_s, u"https://srcit.inaf.it/visivo"_s, u"INAF-IT"_s));

    // URL differs, but node_code matches the registry srcCode.
    const auto pick = pickBackendForSkavaDataset(
            &s, datalinkWith(u"https://other.example/visivo"_s, u"INAF-IT"_s));
    QCOMPARE(pick.id, u"src-it"_s);
}

void TestBackendRouting::routeNoMatchReturnsEmpty()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-it"_s, u"https://srcit.inaf.it/visivo"_s, u"INAF-IT"_s));

    const auto pick = pickBackendForSkavaDataset(
            &s, datalinkWith(u"https://unknown.example/visivo"_s, u"UNKNOWN"_s));
    QVERIFY2(pick.id.isEmpty(), "no descriptor match must yield an empty node (→ local fallback)");
}

void TestBackendRouting::routeEmptyDatalinkReturnsEmpty()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    SkavaDatalinkResponse empty;  // no visivoBackends
    empty.valid = true;
    QVERIFY(pickBackendForSkavaDataset(&s, empty).id.isEmpty());
}

void TestBackendRouting::routeNullSettingsReturnsEmpty()
{
    QVERIFY(pickBackendForSkavaDataset(
                    nullptr, datalinkWith(u"https://x.example/visivo"_s, u"X"_s))
                    .id.isEmpty());
}

void TestBackendRouting::routeUsesTheChosenNodesReplica()
{
    // Only node B has a registered backend: it must be sent B's path, not
    // the best node's primary access URL (which is a path on node A).
    const QList<BackendNode> registry = { makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s) };
    const SkavaRoute route = routeSkavaDataset(registry, twoNodeDatalink());
    QVERIFY(route.valid());
    QCOMPARE(route.node.id, u"src-b"_s);
    QCOMPARE(route.accessUrl, u"file:///mnt/b/archive/cube.fits"_s);
    QCOMPARE(route.nodeCode, u"B"_s);
    QCOMPARE(route.skipped.size(), 1);   // A: no registered backend
}

void TestBackendRouting::routePrefersRankingOrder()
{
    const QList<BackendNode> registry = { makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s),
                                          makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s) };
    const SkavaRoute route = routeSkavaDataset(registry, twoNodeDatalink());
    QCOMPARE(route.node.id, u"src-a"_s);
    QCOMPARE(route.accessUrl, u"file:///srv/a/cube.fits"_s);
}

void TestBackendRouting::routeSkipsUnsupportedKind()
{
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.visivoBackends[0].supportedKinds = { u"image"_s };     // A cannot open cubes
    const QList<BackendNode> registry = { makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s),
                                          makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s) };
    const SkavaRoute route = routeSkavaDataset(registry, dl);
    QCOMPARE(route.node.id, u"src-b"_s);
    QVERIFY(route.skipped.first().contains(u"does not open cube"_s));
}

void TestBackendRouting::routeSkipsAuthWithoutToken()
{
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.visivoBackends = { descriptor(u"https://a.example/visivo"_s, u"A"_s, {}, /*requiresAuth=*/true) };
    QList<BackendNode> registry = { makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s) };
    SkavaRoute route = routeSkavaDataset(registry, dl);
    QVERIFY(!route.valid());
    QVERIFY(route.skipped.first().contains(u"requires a token"_s));
    // No token in the registry, but the client would send the automatic one.
    QVERIFY(routeSkavaDataset(registry, dl, /*autoToken=*/true).valid());
    registry[0].token = u"secret"_s;
    route = routeSkavaDataset(registry, dl);
    QVERIFY(route.valid());
}

void TestBackendRouting::routeSkipsNodeWithoutReplica()
{
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.replicas = { replica(u"A"_s, u"file:///srv/a/cube.fits"_s) };   // B lists no copy
    const QList<BackendNode> registry = { makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s) };
    const SkavaRoute route = routeSkavaDataset(registry, dl);
    QVERIFY(!route.valid());
    QVERIFY(route.skipped.last().contains(u"no replica"_s));
}

void TestBackendRouting::kindMapping()
{
    QVERIFY(skavaBackendSupportsKind({}, u"cube"_s));
    QVERIFY(skavaBackendSupportsKind({ u"cube"_s }, QString()));
    QVERIFY(skavaBackendSupportsKind({ u"image"_s, u"cube"_s, u"dynspec"_s }, u"Cube"_s));
    QVERIFY(skavaBackendSupportsKind({ u"dynspec"_s }, u"timeseries"_s));
    QVERIFY(!skavaBackendSupportsKind({ u"image"_s }, u"cube"_s));
}

void TestBackendRouting::routeTreatsLoopbackNamesAsOneHost()
{
    // SKAVA in Docker advertises the desktop's own backend as host.docker.internal.
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.visivoBackends = { descriptor(u"http://host.docker.internal:8001/"_s, u"VIS-LOCAL"_s) };
    dl.bestNode = u"VIS-LOCAL"_s;
    const QList<BackendNode> registry = { makeNode(u"local"_s, u"http://127.0.0.1:8001"_s, QString()) };
    QVERIFY(routeSkavaDataset(registry, dl).valid());
    // A different port is a different backend.
    const QList<BackendNode> other = { makeNode(u"local"_s, u"http://127.0.0.1:8000"_s, QString()) };
    QVERIFY(!routeSkavaDataset(other, dl).valid());
}

void TestBackendRouting::findRegisteredBackendMatchesLikeRouting()
{
    const QList<BackendNode> registry = { makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s),
                                          makeNode(u"local"_s, u"http://127.0.0.1:8001"_s, QString()) };
    QCOMPARE(findRegisteredBackend(registry, descriptor(u"https://a.example/visivo/"_s, QString())), 0);
    QCOMPARE(findRegisteredBackend(registry, descriptor(u"https://other/visivo"_s, u"A"_s)), 0);
    QCOMPARE(findRegisteredBackend(registry, descriptor(u"http://host.docker.internal:8001"_s, u"X"_s)), 1);
    QCOMPARE(findRegisteredBackend(registry, descriptor(u"https://nowhere"_s, u"Z"_s)), -1);
    // Registered, but the node wants a token neither the registry nor the
    // automatic fallback provides: found (so the prompt edits THIS entry),
    // and not routable.
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.visivoBackends = { descriptor(u"https://a.example/visivo"_s, u"A"_s, {}, /*requiresAuth=*/true) };
    QCOMPARE(findRegisteredBackend(registry, dl.visivoBackends.first()), 0);
    QVERIFY(!routeSkavaDataset(registry, dl, /*autoToken=*/false).valid());
}
