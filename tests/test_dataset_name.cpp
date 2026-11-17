#include "TestDatasetName.h"

#include "app/DatasetName.h"

#include <QFileInfo>
#include <QtTest>

void TestDatasetName::aFileKeepsItsName()
{
    const QString path = QStringLiteral("/data/cubes/hcg44.fits");
    QVERIFY(!visivo::isFieldReference(path));
    QCOMPARE(visivo::datasetFilePath(path), path);
    QCOMPARE(visivo::datasetDisplayName(path), QStringLiteral("hcg44.fits"));
    QCOMPARE(visivo::datasetExportStem(path), path);
}

void TestDatasetName::aFieldIsShownAsVolumeAndField()
{
    const QString data = QStringLiteral("/sim/run1/volume.bin");
    const QString first = data + QStringLiteral("::vbt-field=0");
    const QString second = data + QStringLiteral("::vbt-field=1");
    QVERIFY(visivo::isFieldReference(second));
    QCOMPARE(visivo::datasetFilePath(second), data);
    // Before the backend listed the fields, the index; after, the name.
    QCOMPARE(visivo::datasetDisplayName(second), QStringLiteral("volume · field 1"));
    visivo::rememberFieldNames({ { QStringLiteral("Density"), first },
                                 { QStringLiteral("Gas temperature"), second } });
    QCOMPARE(visivo::datasetDisplayName(first), QStringLiteral("volume · Density"));
    QCOMPARE(visivo::datasetDisplayName(second), QStringLiteral("volume · Gas temperature"));
}

void TestDatasetName::aFieldNamesItsExportsAfterVolumeAndField()
{
    const QString ref = QStringLiteral("/sim/run2/clumps.bin::vbt-field=0");
    visivo::rememberFieldNames({ { QStringLiteral("CDM/Density"), ref } });
    const QFileInfo stem(visivo::datasetExportStem(ref));
    QCOMPARE(stem.absolutePath(), QStringLiteral("/sim/run2"));
    // No separator or colon from the reference reaches a file name.
    QCOMPARE(stem.completeBaseName(), QStringLiteral("clumps_CDM_Density"));
}

void TestDatasetName::aParticleTypeIsShownAsSnapshotAndType()
{
    // A particle type of a Gadget snapshot (#31): the first file, then the type.
    const QString file = QStringLiteral("/runs/zoom/snapdir_010/snap_010.0");
    const QString gas = file + QStringLiteral("::gadget-type=0");
    const QString stars = file + QStringLiteral("::gadget-type=4");
    QVERIFY(visivo::isFieldReference(gas));
    QCOMPARE(visivo::datasetFilePath(stars), file);
    QCOMPARE(visivo::datasetDisplayName(stars), QStringLiteral("snap_010.0 · type 4"));
    visivo::rememberFieldNames({ { QStringLiteral("gas"), gas },
                                 { QStringLiteral("stars"), stars } });
    QCOMPARE(visivo::datasetDisplayName(gas), QStringLiteral("snap_010.0 · gas"));
    const QFileInfo stem(visivo::datasetExportStem(stars));
    QCOMPARE(stem.absolutePath(), QStringLiteral("/runs/zoom/snapdir_010"));
    // The file's dot does not cut the name short.
    QCOMPARE(stem.completeBaseName(), QStringLiteral("snap_010_0_stars"));
}
