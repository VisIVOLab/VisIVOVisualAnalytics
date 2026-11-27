// test_shared_rows.cpp
// SharedRows is how the panes of one particle window hold the particles once: a copy
// shares, a read never copies, a change makes a private copy first.

#include "TestSharedRows.h"

#include "SharedRows.h"

#include <QTest>

#include <array>
#include <vector>

namespace {
struct Row
{
    int id{ 0 };
    double x{ 0.0 };
};
std::vector<Row> make(int n)
{
    std::vector<Row> v;
    for (int i = 0; i < n; ++i)
        v.push_back({ i, i * 0.5 });
    return v;
}
} // namespace

void TestSharedRows::copy_sharesTheRows()
{
    SharedRows<Row> a(make(1000));
    SharedRows<Row> b = a;
    QVERIFY(a.sharesWith(b));
    QCOMPARE(a.handles(), 2L);
    // The very same storage: the address of a row is the same in both.
    QCOMPARE(static_cast<const void *>(&a[10]), static_cast<const void *>(&b[10]));
    QCOMPARE(b.size(), std::size_t(1000));
}

void TestSharedRows::read_neverCopies()
{
    SharedRows<Row> a(make(100));
    SharedRows<Row> b = a;
    double sum = 0;
    for (const Row &r : a)       // range-for on a non-const handle reads, it does not detach
        sum += r.x;
    for (std::size_t i = 0; i < b.size(); ++i)
        sum += b[i].x;
    const std::vector<Row> &view = b;   // the conversion for functions taking a vector
    QCOMPARE(view.size(), std::size_t(100));
    QVERIFY(sum > 0);
    QVERIFY(a.sharesWith(b));
    QCOMPARE(a.handles(), 2L);
}

void TestSharedRows::mut_leavesTheOthersAlone()
{
    SharedRows<Row> a(make(10));
    SharedRows<Row> b = a;
    b.mut()[3].x = 99.0;
    QVERIFY(!a.sharesWith(b));
    QCOMPARE(a[3].x, 1.5);
    QCOMPARE(b[3].x, 99.0);
    QCOMPARE(a.handles(), 1L);
    QCOMPARE(b.handles(), 1L);
}

void TestSharedRows::mut_onAPrivateHandleCopiesNothing()
{
    SharedRows<Row> a(make(10));
    const void *before = a.identity();
    a.mut()[0].x = 7.0;
    QCOMPARE(a.identity(), before);   // nobody else held it: changed in place
    QCOMPARE(a[0].x, 7.0);
}

void TestSharedRows::assignFromVector_rebindsOnlyThisHandle()
{
    SharedRows<Row> a(make(5));
    SharedRows<Row> b = a;
    b = make(8);   // a load: b has its own rows now
    QVERIFY(!a.sharesWith(b));
    QCOMPARE(a.size(), std::size_t(5));
    QCOMPARE(b.size(), std::size_t(8));
}

void TestSharedRows::empty_handleIsUsable()
{
    SharedRows<Row> a;
    QVERIFY(a.empty());
    QCOMPARE(a.size(), std::size_t(0));
    QVERIFY(a.begin() == a.end());
    QVERIFY(a.data() == nullptr);
    QVERIFY(!a.sharesWith(a));   // no rows: nothing to share
    QCOMPARE(a.handles(), 0L);
    a.mut().push_back({ 1, 2.0 });
    QCOMPARE(a.size(), std::size_t(1));
}

void TestSharedRows::conversionToVector_isTheSameStorage()
{
    SharedRows<Row> a(make(4));
    const std::vector<Row> &v = a.rows();
    QCOMPARE(static_cast<const void *>(v.data()), static_cast<const void *>(a.data()));
}

void TestSharedRows::resetThenMut_startsFresh()
{
    SharedRows<Row> a(make(6));
    SharedRows<Row> b = a;
    a.reset();
    QVERIFY(a.empty());
    QCOMPARE(b.size(), std::size_t(6));   // the other handle kept its rows
    auto &rows = a.mut();
    rows.resize(3);
    QCOMPARE(a.size(), std::size_t(3));
    QCOMPARE(b.size(), std::size_t(6));
}

void TestSharedRows::appendThroughMut_afterSharing()
{
    SharedRows<Row> a(make(3));
    SharedRows<Row> b = a;
    // The append pattern of "Load more": the reference is taken after mut(), so it
    // cannot point into the storage that was just left behind.
    auto &rows = b.mut();
    rows.insert(rows.end(), { Row{ 9, 9.0 } });
    QCOMPARE(b.size(), std::size_t(4));
    QCOMPARE(a.size(), std::size_t(3));
    QCOMPARE(b[3].id, 9);
}
