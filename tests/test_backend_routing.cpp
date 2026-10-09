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
    r.visivoBackends.append(d);
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
