// test_workspace_exports_view.cpp
// The Workspace Exports list was rebuilt, one styled row per export, on every
// backend health tick; with 3405 exports the app stalled (#312).

#include "TestWorkspaceExportsView.h"

#include "app/WorkspaceExportsView.h"

#include <QTest>

using visivo::workspaceExportsSignature;

namespace {

BackendWorkspaceExportsList listing(int n)
{
    BackendWorkspaceExportsList l;
    l.valid = true;
    l.workspaceDir = QStringLiteral("/tmp/exports");
    for (int i = 0; i < n; ++i) {
        BackendWorkspaceExportEntry e;
        e.filename = QStringLiteral("cube_spectrum_%1.csv").arg(i);
        e.size = 1000 + i;
        e.modifiedTime = QStringLiteral("2026-11-15T12:00:%1Z").arg(i % 60, 2, 10, QLatin1Char('0'));
        e.absolutePath = l.workspaceDir + QLatin1Char('/') + e.filename;
        l.entries.append(e);
    }
    return l;
}

}  // namespace

void TestWorkspaceExportsView::anUnchangedListingHasTheSameSignature()
{
    QCOMPARE(workspaceExportsSignature(listing(5)), workspaceExportsSignature(listing(5)));
    QVERIFY(!workspaceExportsSignature(listing(0)).isEmpty());
}

void TestWorkspaceExportsView::anyChangeShownInARowChangesTheSignature()
{
    const QByteArray base = workspaceExportsSignature(listing(5));

    auto renamed = listing(5);
    renamed.entries[2].filename = QStringLiteral("other.csv");
    QVERIFY(workspaceExportsSignature(renamed) != base);

    auto resized = listing(5);
    resized.entries[4].size += 1;
    QVERIFY(workspaceExportsSignature(resized) != base);

    auto touched = listing(5);
    touched.entries[0].modifiedTime = QStringLiteral("2026-11-16T00:00:00Z");
    QVERIFY(workspaceExportsSignature(touched) != base);

    QVERIFY(workspaceExportsSignature(listing(4)) != base);   // one deleted
    QVERIFY(workspaceExportsSignature(listing(6)) != base);   // one added

    // Field boundaries count: "ab" + "c" is not "a" + "bc".
    auto a = listing(1), b = listing(1);
    a.entries[0].filename = QStringLiteral("ab");
    a.entries[0].modifiedTime = QStringLiteral("c");
    b.entries[0].filename = QStringLiteral("a");
    b.entries[0].modifiedTime = QStringLiteral("bc");
    QVERIFY(workspaceExportsSignature(a) != workspaceExportsSignature(b));
}

void TestWorkspaceExportsView::aFailedListingDiffersFromAnEmptyOne()
{
    BackendWorkspaceExportsList failed;
    failed.error = QStringLiteral("connection refused");
    QVERIFY(workspaceExportsSignature(failed) != workspaceExportsSignature(listing(0)));

    BackendWorkspaceExportsList other = failed;
    other.error = QStringLiteral("timed out");
    QVERIFY(workspaceExportsSignature(failed) != workspaceExportsSignature(other));
}

void TestWorkspaceExportsView::onlyTheMostRecentExportsGetARow()
{
    QCOMPARE(visivo::workspaceExportRowsShown(0), 0);
    QCOMPARE(visivo::workspaceExportRowsShown(7), 7);
    QCOMPARE(visivo::workspaceExportRowsShown(visivo::kWorkspaceExportRows),
             visivo::kWorkspaceExportRows);
    QCOMPARE(visivo::workspaceExportRowsShown(3405), visivo::kWorkspaceExportRows);

    QVERIFY(visivo::workspaceExportsOverflowText(7).isEmpty());
    QVERIFY(visivo::workspaceExportsOverflowText(visivo::kWorkspaceExportRows).isEmpty());
    const QString one = visivo::workspaceExportsOverflowText(visivo::kWorkspaceExportRows + 1);
    QVERIFY2(one.startsWith(QStringLiteral("1 older export not listed")), qPrintable(one));
    const QString many = visivo::workspaceExportsOverflowText(3405);
    QVERIFY2(many.startsWith(QStringLiteral("%1 older exports not listed")
                                     .arg(3405 - visivo::kWorkspaceExportRows)),
             qPrintable(many));
}
