// test_remote_slice_cache.cpp
// Unit tests for RemoteSliceCache<Slice> — the LRU cache extracted from
// vtkWindowCube in E-04 step 1. Uses a fake slice type so the test carries no
// VTK / backend dependency (matching the suite's Qt-only design).

#include "TestRemoteSliceCache.h"
#include "RemoteSliceCache.h"

#include <QTest>

namespace {
// Minimal stand-in for RemoteCubeSliceResult: the cache only requires a `valid`
// flag and an `imageData` member that is contextually convertible to bool.
struct FakeSlice
{
    bool valid{ true };
    bool imageData{ true };   // stands in for vtkSmartPointer<vtkImageData>
    int tag{ 0 };             // payload, to assert which value came back
};

FakeSlice slice(int tag) { return FakeSlice{ true, true, tag }; }
}  // namespace

void TestRemoteSliceCache::find_missReturnsNull()
{
    RemoteSliceCache<FakeSlice> cache(3);
    QCOMPARE(cache.find(QStringLiteral("absent")), nullptr);
    QCOMPARE(cache.size(), 0);
}

void TestRemoteSliceCache::insertThenFind_hit()
{
    RemoteSliceCache<FakeSlice> cache(3);
    cache.insert(QStringLiteral("k"), slice(42));
    const FakeSlice *hit = cache.find(QStringLiteral("k"));
    QVERIFY(hit != nullptr);
    QCOMPARE(hit->tag, 42);
    QCOMPARE(cache.size(), 1);
}

void TestRemoteSliceCache::insert_invalidIgnored()
{
    RemoteSliceCache<FakeSlice> cache(3);
    FakeSlice bad = slice(1);
    bad.valid = false;
    cache.insert(QStringLiteral("k"), bad);
    QCOMPARE(cache.size(), 0);
    QCOMPARE(cache.find(QStringLiteral("k")), nullptr);
}

void TestRemoteSliceCache::insert_noImageDataIgnored()
{
    RemoteSliceCache<FakeSlice> cache(3);
    FakeSlice bad = slice(1);
    bad.imageData = false;
    cache.insert(QStringLiteral("k"), bad);
    QCOMPARE(cache.size(), 0);
}

void TestRemoteSliceCache::lru_evictsLeastRecentlyUsed()
{
    RemoteSliceCache<FakeSlice> cache(3);
    cache.insert(QStringLiteral("a"), slice(1));
    cache.insert(QStringLiteral("b"), slice(2));
    cache.insert(QStringLiteral("c"), slice(3));
    cache.insert(QStringLiteral("d"), slice(4));   // capacity 3 → "a" (oldest) evicted
    QCOMPARE(cache.size(), 3);
    QCOMPARE(cache.find(QStringLiteral("a")), nullptr);
    QVERIFY(cache.find(QStringLiteral("b")) != nullptr);
    QVERIFY(cache.find(QStringLiteral("d")) != nullptr);
}

void TestRemoteSliceCache::find_refreshesRecency_savesFromEviction()
{
    RemoteSliceCache<FakeSlice> cache(3);
    cache.insert(QStringLiteral("a"), slice(1));
    cache.insert(QStringLiteral("b"), slice(2));
    cache.insert(QStringLiteral("c"), slice(3));
    // Touch "a" via find → it becomes most-recent, so the next insert must evict
    // "b" (now the oldest), NOT "a".
    QVERIFY(cache.find(QStringLiteral("a")) != nullptr);
    cache.insert(QStringLiteral("d"), slice(4));
    QVERIFY(cache.contains(QStringLiteral("a")));
    QVERIFY(!cache.contains(QStringLiteral("b")));
}

void TestRemoteSliceCache::touch_refreshesRecency()
{
    RemoteSliceCache<FakeSlice> cache(2);
    cache.insert(QStringLiteral("a"), slice(1));
    cache.insert(QStringLiteral("b"), slice(2));
    cache.touch(QStringLiteral("a"));               // "a" now most-recent
    cache.insert(QStringLiteral("c"), slice(3));    // evicts "b" (the older)
    QVERIFY(cache.contains(QStringLiteral("a")));
    QVERIFY(!cache.contains(QStringLiteral("b")));
    QVERIFY(cache.contains(QStringLiteral("c")));
}

void TestRemoteSliceCache::touch_absentKeyIsNoOp_noOverEviction()
{
    // Touching a key that is not cached must NOT enter the recency list: if it
    // did, the next insert would evict TWO real entries instead of one.
    RemoteSliceCache<FakeSlice> cache(2);
    cache.insert(QStringLiteral("a"), slice(1));
    cache.insert(QStringLiteral("b"), slice(2));
    cache.touch(QStringLiteral("ghost"));           // no-op
    cache.insert(QStringLiteral("c"), slice(3));    // evicts exactly one (a)
    QCOMPARE(cache.size(), 2);
    QVERIFY(cache.contains(QStringLiteral("b")));    // b survives — only a evicted
    QVERIFY(cache.contains(QStringLiteral("c")));
    QVERIFY(!cache.contains(QStringLiteral("a")));
}

void TestRemoteSliceCache::reinsertSameKey_updatesValue_noGrowth()
{
    RemoteSliceCache<FakeSlice> cache(3);
    cache.insert(QStringLiteral("k"), slice(1));
    cache.insert(QStringLiteral("k"), slice(2));    // same key → replace, not grow
    QCOMPARE(cache.size(), 1);
    const FakeSlice *hit = cache.find(QStringLiteral("k"));
    QVERIFY(hit != nullptr);
    QCOMPARE(hit->tag, 2);
}
