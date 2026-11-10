#include "TestRecentReopen.h"

#include "app/RecentDatasetsManager.h"
#include "app/RecentReopen.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

void TestRecentReopen::entriesThatNameAFile()
{
    for (const char *type : { "cube", "image", "velocity", "catalogue", "vbt", "vbt-volume" })
        QVERIFY2(visivo::recentEntryNamesAFile(QString::fromLatin1(type), QStringLiteral("remote")),
                 type);
    // SKAVA entries are resolved through discovery again; HiPS has no file.
    QVERIFY(!visivo::recentEntryNamesAFile(QStringLiteral("cube"), QStringLiteral("skava")));
    QVERIFY(!visivo::recentEntryNamesAFile(QStringLiteral("hips"), QStringLiteral("remote")));
}

void TestRecentReopen::nodeFilesAreNotLookedForHere()
{
    // A SODA cutout and a node product live on their SKAVA node: "not found"
    // on this window's backend would be a confident wrong answer.
    QVERIFY(!visivo::recentEntryNamesAFile(QStringLiteral("cube"), QStringLiteral("soda")));
    QVERIFY(!visivo::recentEntryNamesAFile(QStringLiteral("image"), QStringLiteral("node")));
    // A cutout recorded before it had its own source is told by its name.
    QVERIFY(!visivo::recentEntryNamesAFile(QStringLiteral("cube"), QStringLiteral("remote"),
                                           QStringLiteral("obs42 [SODA cutout]")));
    QVERIFY(visivo::recentEntryNamesAFile(QStringLiteral("cube"), QStringLiteral("remote"),
                                          QStringLiteral("hcg44.fits")));
}

void TestRecentReopen::aFieldIsLookedForAsItsDataFile()
{
    QCOMPARE(visivo::recentEntryFile(QStringLiteral("/sim/volume.bin::vbt-field=2")),
             QStringLiteral("/sim/volume.bin"));
    QCOMPARE(visivo::recentEntryFile(QStringLiteral("/data/hcg44.fits")),
             QStringLiteral("/data/hcg44.fits"));
}

void TestRecentReopen::aFileBehindAClosedFolderIsNotCalledMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString present = dir.filePath(QStringLiteral("cube.fits"));
    QFile f(present);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
    QCOMPARE(visivo::localFileState(present), visivo::LocalFileState::Present);
    QCOMPARE(visivo::localFileState(dir.filePath(QStringLiteral("gone.fits"))),
             visivo::LocalFileState::Missing);
    // A deleted folder too: the nearest folder that exists could be read.
    QCOMPARE(visivo::localFileState(dir.filePath(QStringLiteral("old/run/gone.fits"))),
             visivo::LocalFileState::Missing);

    // A folder we may not search: what is inside cannot be told apart.
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("closed")));
    const QString closed = dir.filePath(QStringLiteral("closed"));
    QFile inner(closed + QStringLiteral("/cube.fits"));
    QVERIFY(inner.open(QIODevice::WriteOnly));
    inner.close();
    QVERIFY(QFile::setPermissions(closed, QFileDevice::Permissions()));
    if (QFileInfo(closed + QStringLiteral("/cube.fits")).exists()) {
        QFile::setPermissions(closed, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                              | QFileDevice::ExeOwner);
        QSKIP("running with privileges that ignore folder permissions");
    }
    QCOMPARE(visivo::localFileState(closed + QStringLiteral("/cube.fits")),
             visivo::LocalFileState::Unreadable);
    QFile::setPermissions(closed, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                          | QFileDevice::ExeOwner);
}

void TestRecentReopen::onlyTheClassifiersNotFoundIsMissing()
{
    QVERIFY(visivo::classifierSaysNotFound(false, QStringLiteral("File not found.")));
    // A backend that is down, or a file it cannot read, is not "missing": the
    // open goes on and says what went wrong.
    QVERIFY(!visivo::classifierSaysNotFound(false, QStringLiteral("Connection refused")));
    QVERIFY(!visivo::classifierSaysNotFound(false, QString()));
    QVERIFY(!visivo::classifierSaysNotFound(true, QStringLiteral("File not found.")));
}

void TestRecentReopen::theMessageSaysWhere()
{
    const QString here = visivo::recentMissingText(QStringLiteral("hcg44.fits"),
                                                   QStringLiteral("/data/hcg44.fits"), QString());
    QVERIFY(here.contains(QStringLiteral("“hcg44.fits” was not found")));
    QVERIFY(here.contains(QStringLiteral("/data/hcg44.fits")));
    QVERIFY(!here.contains(QStringLiteral("Unsupported")));
    const QString there = visivo::recentMissingText(QStringLiteral("cube.fits"),
                                                    QStringLiteral("/scratch/cube.fits"),
                                                    QStringLiteral("pleiadi-gpu"));
    QVERIFY(there.contains(QStringLiteral("not found on pleiadi-gpu")));
}

void TestRecentReopen::removingAnEntryRemovesOnlyThatOne()
{
    // The same path on two backends, and an old entry of unknown backend: the
    // one clicked goes, the others stay.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QStringLiteral("/data/cube.fits");
    RecentDataset onLab;
    onLab.id = path;
    onLab.type = QStringLiteral("cube");
    onLab.backend = QStringLiteral("lab");
    onLab.backendKnown = true;
    RecentDataset local = onLab;
    local.backend.clear();
    RecentDatasetsManager mgr(dir.path());
    mgr.record(onLab);
    mgr.record(local);
    QCOMPARE(mgr.datasets().size(), 2);
    mgr.removeEntry(local);
    QCOMPARE(mgr.datasets().size(), 1);
    QCOMPARE(mgr.datasets().first().backend, QStringLiteral("lab"));

    RecentDataset unknown;
    unknown.id = path;
    unknown.backendKnown = false;
    mgr.removeEntry(unknown);   // no such entry: nothing else goes
    QCOMPARE(mgr.datasets().size(), 1);
}
