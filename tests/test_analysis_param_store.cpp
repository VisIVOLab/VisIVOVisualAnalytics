#include "TestAnalysisParamStore.h"

#include "AnalysisParamStore.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

// A temp ini path unique per test run. QTemporaryDir cleans itself up.
static QTemporaryDir *g_dir = nullptr;

void TestAnalysisParamStore::init()
{
    g_dir = new QTemporaryDir();
    QVERIFY(g_dir->isValid());
    m_iniPath = QDir(g_dir->path()).absoluteFilePath(u"Settings.ini"_s);
}

void TestAnalysisParamStore::cleanup()
{
    delete g_dir;
    g_dir = nullptr;
}

void TestAnalysisParamStore::roundTrip_savesAndLoads()
{
    AnalysisParamStore store(m_iniPath);
    QVariantMap params{ { u"pa"_s, 42.5 }, { u"incl"_s, 60.0 }, { u"vsys"_s, 1234.5 },
                        { u"x0"_s, 512 } };
    store.save(u"/data/cube.fits"_s, u"kinematic"_s, params);

    // A fresh store instance (simulating a new session) reads it back.
    AnalysisParamStore reopened(m_iniPath);
    const QVariantMap got = reopened.load(u"/data/cube.fits"_s, u"kinematic"_s);
    QCOMPARE(got.value(u"pa"_s).toDouble(), 42.5);
    QCOMPARE(got.value(u"incl"_s).toDouble(), 60.0);
    QCOMPARE(got.value(u"vsys"_s).toDouble(), 1234.5);
    QCOMPARE(got.value(u"x0"_s).toInt(), 512);
    QCOMPARE(got.size(), 4);
}

void TestAnalysisParamStore::missing_returnsEmpty()
{
    AnalysisParamStore store(m_iniPath);
    QVERIFY(store.load(u"/never/saved.fits"_s, u"kinematic"_s).isEmpty());
    // Saved under a different group → still empty for this group.
    store.save(u"/data/cube.fits"_s, u"noise"_s, { { u"x0"_s, 1 } });
    QVERIFY(store.load(u"/data/cube.fits"_s, u"kinematic"_s).isEmpty());
}

void TestAnalysisParamStore::datasetsAreIsolated()
{
    AnalysisParamStore store(m_iniPath);
    store.save(u"/data/a.fits"_s, u"kinematic"_s, { { u"pa"_s, 10.0 } });
    store.save(u"/data/b.fits"_s, u"kinematic"_s, { { u"pa"_s, 20.0 } });
    QCOMPARE(store.load(u"/data/a.fits"_s, u"kinematic"_s).value(u"pa"_s).toDouble(), 10.0);
    QCOMPARE(store.load(u"/data/b.fits"_s, u"kinematic"_s).value(u"pa"_s).toDouble(), 20.0);
}

void TestAnalysisParamStore::groupsAreIsolated()
{
    AnalysisParamStore store(m_iniPath);
    store.save(u"/data/cube.fits"_s, u"kinematic"_s, { { u"pa"_s, 30.0 } });
    store.save(u"/data/cube.fits"_s, u"noise"_s, { { u"x0"_s, 5 }, { u"x1"_s, 99 } });
    const auto kin = store.load(u"/data/cube.fits"_s, u"kinematic"_s);
    const auto noise = store.load(u"/data/cube.fits"_s, u"noise"_s);
    QCOMPARE(kin.size(), 1);
    QCOMPARE(noise.size(), 2);
    QCOMPARE(noise.value(u"x1"_s).toInt(), 99);
}

void TestAnalysisParamStore::resaveReplacesStaleKeys()
{
    AnalysisParamStore store(m_iniPath);
    store.save(u"/data/cube.fits"_s, u"noise"_s, { { u"x0"_s, 1 }, { u"stale"_s, 7 } });
    // Re-save without the "stale" key → it must be gone, not linger.
    store.save(u"/data/cube.fits"_s, u"noise"_s, { { u"x0"_s, 2 } });
    const auto got = store.load(u"/data/cube.fits"_s, u"noise"_s);
    QCOMPARE(got.size(), 1);
    QCOMPARE(got.value(u"x0"_s).toInt(), 2);
    QVERIFY(!got.contains(u"stale"_s));
}

void TestAnalysisParamStore::emptyKeyOrGroup_noop()
{
    AnalysisParamStore store(m_iniPath);
    store.save(QString(), u"kinematic"_s, { { u"pa"_s, 1.0 } });
    store.save(u"/data/cube.fits"_s, QString(), { { u"pa"_s, 1.0 } });
    QVERIFY(store.load(QString(), u"kinematic"_s).isEmpty());
    QVERIFY(store.load(u"/data/cube.fits"_s, QString()).isEmpty());
}
