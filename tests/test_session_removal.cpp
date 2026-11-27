// test_session_removal.cpp
// What a request to remove products from Session Data comes to: which of them
// can go, what has to be asked first, and which file is a temporary one to
// delete. A permanent copy the user saved must never be a candidate.

#include "TestSessionRemoval.h"

#include "SessionRemoval.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

AnalysisProduct product(const QString &id, const QString &label = QString(),
                        const QStringList &dependsOn = {})
{
    AnalysisProduct p;
    p.productId = id;
    p.label = label.isEmpty() ? id : label;
    p.kind = u"moment"_s;
    p.dependsOn = dependsOn;
    return p;
}

AnalysisProduct saved(const QString &id, const QStringList &dependsOn = {})
{
    AnalysisProduct p = product(id, QString(), dependsOn);
    p.params.insert(u"Storage"_s, u"Workspace Exports"_s);
    return p;
}

bool touch(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write("x") == 1;
}

} // namespace

void TestSessionRemoval::savedPermanently_onlyAWorkspaceExportCounts()
{
    AnalysisProduct p = product(u"a"_s);
    QVERIFY(!visivo::removal::savedPermanently(p));   // held in memory only
    p.params.insert(u"Storage"_s, u"temporary file on the backend, export to keep it"_s);
    QVERIFY(!visivo::removal::savedPermanently(p));
    p.params.insert(u"Storage"_s, u"kept in the session cache"_s);
    QVERIFY(!visivo::removal::savedPermanently(p));   // unknown wording: ask
    p.params.insert(u"Storage"_s, u"Workspace Exports"_s);
    QVERIFY(visivo::removal::savedPermanently(p));
}

void TestSessionRemoval::rowOfIdentity_followsTheLayerNotTheRow()
{
    int a = 0, b = 0, c = 0;   // three layers' identities
    const void *none = nullptr;
    QList<const void *> rows{ &a, &b, &c };
    QCOMPARE(visivo::removal::rowOfIdentity(rows, &b), 1);
    QCOMPARE(visivo::removal::rowOfIdentity(rows, none), -1);

    // A tool run twice: two layers, one name. Each product keeps the identity
    // of ITS layer, so removing the newer one removes row 2, not the older row 1.
    const void *olderProduct = &b, *newerProduct = &c;
    QCOMPARE(visivo::removal::rowOfIdentity(rows, newerProduct), 2);
    rows.removeAt(visivo::removal::rowOfIdentity(rows, newerProduct));
    QCOMPARE(rows, (QList<const void *>{ &a, &b }));
    QCOMPARE(visivo::removal::rowOfIdentity(rows, olderProduct), 1);   // untouched
    QCOMPARE(visivo::removal::rowOfIdentity(rows, newerProduct), -1);  // gone

    // Reordered by drag and drop: the row moves, the identity does not.
    rows.move(1, 0);
    QCOMPARE(visivo::removal::rowOfIdentity(rows, olderProduct), 0);
}

void TestSessionRemoval::aQueuedRemoval_isLookedUpAgainWhenItRuns()
{
    int a = 0, b = 0, c = 0;
    QList<const void *> rows{ &a, &b, &c };
    // Two Delete presses land before the first request runs: both carry the
    // identity of the layer that was current, not its row.
    const void *current = &b;
    const QList<const void *> queued{ current, current };
    int removals = 0;
    for (const void *identity : queued) {
        const int row = visivo::removal::rowOfIdentity(rows, identity);
        if (row < 0)
            continue;   // already gone: the neighbour is not taken instead
        rows.removeAt(row);
        ++removals;
    }
    QCOMPARE(removals, 1);
    QCOMPARE(rows, (QList<const void *>{ &a, &c }));
}

void TestSessionRemoval::plan_dropsUnknownAndRepeatedIds()
{
    const QList<AnalysisProduct> all{ product(u"a"_s), product(u"b"_s) };
    const auto plan = visivo::removal::planRemoval(
            all, { u"b"_s, u"ghost"_s, u"a"_s, u"b"_s });
    QCOMPARE(plan.ids, (QStringList{ u"b"_s, u"a"_s }));   // the order asked, once each
    QCOMPARE(plan.unsaved, plan.ids);
    QVERIFY(plan.refused.isEmpty());
}

void TestSessionRemoval::plan_aRunningOrLiveProductStays()
{
    AnalysisProduct running = product(u"r"_s, u"Moment 0"_s);
    running.status = u"running"_s;
    AnalysisProduct live = product(u"l"_s, u"Spectrum (live)"_s);
    live.status = u"live"_s;
    const auto plan = visivo::removal::planRemoval(
            { running, live, product(u"ok"_s) }, { u"r"_s, u"l"_s, u"ok"_s });
    QCOMPARE(plan.ids, (QStringList{ u"ok"_s }));
    QCOMPARE(plan.refused.size(), 2);
    QVERIFY(plan.refused.at(0).startsWith(u"Moment 0: "_s));
    QVERIFY(plan.refused.at(1).contains(u"live preview"_s));
}

void TestSessionRemoval::plan_aToolThatNamesItselfRunningInItsParametersStays()
{
    // The baseline registers its row when it starts and only sets the status
    // when it is done: its parameters are what say it is running.
    AnalysisProduct baseline = product(u"base"_s, u"Baseline-subtracted cube"_s);
    baseline.params.insert(u"Status"_s, u"running…"_s);
    const auto plan = visivo::removal::planRemoval({ baseline }, { u"base"_s });
    QVERIFY(plan.ids.isEmpty());
    QCOMPARE(plan.refused.size(), 1);
}

void TestSessionRemoval::plan_theWindowCanRefuseAProduct()
{
    const auto refuse = [](const AnalysisProduct &p) {
        return p.productId == u"master"_s ? u"it is the image the window opened on"_s : QString();
    };
    const auto plan = visivo::removal::planRemoval(
            { product(u"master"_s, u"Layer 1"_s), product(u"x"_s) }, { u"master"_s, u"x"_s },
            refuse);
    QCOMPARE(plan.ids, (QStringList{ u"x"_s }));
    QCOMPARE(plan.refused, (QStringList{ u"Layer 1: it is the image the window opened on"_s }));
}

void TestSessionRemoval::plan_listsWhatWasMadeFromTheProductsThatGo()
{
    // c is made from b, b from a: asking for a reaches both, however far down.
    const QList<AnalysisProduct> all{ product(u"a"_s), product(u"b"_s, {}, { u"a"_s }),
                                      product(u"c"_s, {}, { u"b"_s }), product(u"other"_s) };
    const auto plan = visivo::removal::planRemoval(all, { u"a"_s });
    QCOMPARE(plan.ids, (QStringList{ u"a"_s }));
    QCOMPARE(plan.dependants.size(), 2);
    QVERIFY(plan.dependants.contains(u"b"_s));
    QVERIFY(plan.dependants.contains(u"c"_s));
    QVERIFY(!plan.dependants.contains(u"other"_s));
    QVERIFY(plan.needsConfirmation());
}

void TestSessionRemoval::plan_aDependantAskedForIsNotListedTwice()
{
    const QList<AnalysisProduct> all{ product(u"a"_s), product(u"b"_s, {}, { u"a"_s }) };
    const auto plan = visivo::removal::planRemoval(all, { u"a"_s, u"b"_s });
    QCOMPARE(plan.ids, (QStringList{ u"a"_s, u"b"_s }));
    QVERIFY(plan.dependants.isEmpty());
}

void TestSessionRemoval::plan_aDependantThatCannotGoIsLeftOut()
{
    AnalysisProduct busy = product(u"b"_s, {}, { u"a"_s });
    busy.status = u"running"_s;
    const auto plan = visivo::removal::planRemoval(
            { saved(u"a"_s), busy, product(u"c"_s, {}, { u"b"_s }) }, { u"a"_s });
    QVERIFY(plan.dependants.isEmpty());   // and c, behind it, is not reached either
}

void TestSessionRemoval::confirmation_onlyWhenSomethingWouldBeLost()
{
    // A permanent copy on disk, nothing made from it: nothing to ask.
    auto plan = visivo::removal::planRemoval({ saved(u"a"_s) }, { u"a"_s });
    QVERIFY(!plan.needsConfirmation());
    QVERIFY(visivo::removal::confirmationText({ saved(u"a"_s) }, plan).isEmpty());

    // The same with something made from it: asked.
    plan = visivo::removal::planRemoval({ saved(u"a"_s), saved(u"b"_s, { u"a"_s }) }, { u"a"_s });
    QVERIFY(plan.needsConfirmation());

    // Held only in memory: asked.
    plan = visivo::removal::planRemoval({ product(u"m"_s) }, { u"m"_s });
    QVERIFY(plan.needsConfirmation());
}

void TestSessionRemoval::confirmationText_namesTheProductsAndWhatWasMadeFromThem()
{
    const QList<AnalysisProduct> all{ product(u"m"_s, u"Moment 0"_s),
                                      product(u"r"_s, u"Residual"_s, { u"m"_s }) };
    auto plan = visivo::removal::planRemoval(all, { u"m"_s });
    const QString one = visivo::removal::confirmationText(all, plan);
    QVERIFY(one.contains(u"“Moment 0”"_s));
    QVERIFY(one.contains(u"permanent copy"_s));
    QVERIFY(one.contains(u"“Residual”"_s));

    const QList<AnalysisProduct> two{ product(u"a"_s), product(u"b"_s) };
    plan = visivo::removal::planRemoval(two, { u"a"_s, u"b"_s });
    QVERIFY(visivo::removal::confirmationText(two, plan).contains(u"Remove 2 items"_s));
}

void TestSessionRemoval::temporaryFile_onlyATemporaryOneUnderTheTempRoot()
{
    QTemporaryDir root;
    QTemporaryDir elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const QString inTemp = root.path() + u"/baseline.fits"_s;
    const QString outside = elsewhere.path() + u"/mine.fits"_s;
    QVERIFY(touch(inTemp));
    QVERIFY(touch(outside));

    AnalysisProduct p = product(u"b"_s);
    p.params.insert(u"Output path"_s, inTemp);
    p.params.insert(u"Storage"_s, u"temporary file on the backend, export to keep it"_s);
    QCOMPARE(visivo::removal::temporaryFileToDelete(p, root.path()),
             QFileInfo(inTemp).canonicalFilePath());

    // The same file, once the product says it is kept: never a candidate.
    AnalysisProduct kept = p;
    kept.params.insert(u"Storage"_s, u"Workspace Exports"_s);
    QVERIFY(visivo::removal::temporaryFileToDelete(kept, root.path()).isEmpty());

    // A temporary product whose path is a user's file elsewhere: left alone.
    AnalysisProduct stray = p;
    stray.params.insert(u"Output path"_s, outside);
    QVERIFY(visivo::removal::temporaryFileToDelete(stray, root.path()).isEmpty());

    // Gone already, or no path, or no storage note: nothing to delete.
    AnalysisProduct gone = p;
    gone.params.insert(u"Output path"_s, root.path() + u"/missing.fits"_s);
    QVERIFY(visivo::removal::temporaryFileToDelete(gone, root.path()).isEmpty());
    QVERIFY(visivo::removal::temporaryFileToDelete(product(u"n"_s), root.path()).isEmpty());
}
