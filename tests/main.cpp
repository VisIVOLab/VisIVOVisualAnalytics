// tests/main.cpp
// Combined QTest runner for VisIVO Visual Analytics unit tests.
// Runs all test classes in sequence; returns non-zero if any test fails.
// QCoreApplication is used — no GUI, no display required.

#include "TestBackendClient.h"
#include "TestCatalogueParser.h"
#include "TestBackendRouting.h"
#include "TestImageLod.h"
#include "TestAnalysisParamStore.h"
#include "TestBackendContract.h"
#include "TestRemoteSliceCache.h"
#include "TestLinkedWindowRegistry.h"
#include "TestSpectralUnitConvert.h"
#include "TestCubeRegionGeom.h"
#include "TestCubeToolGate.h"
#include "TestToolResultRecord.h"
#include "TestProductOrigin.h"
#include "TestCatalogueCsv.h"
#include "TestFitsHeaderString.h"
#include "TestStokesCompanion.h"
#include "TestSpectralAxisInfer.h"
#include "TestKinematicLevelField.h"
#include "TestRayVoxelPick.h"
#include "TestSedBuilder.h"
#include "TestVlkbPosString.h"
#include "TestSkyOverlap.h"
#include "TestFitsHeaderCard.h"
#include "TestViewerModeState.h"
#include "TestLayerCardGeometry.h"
#include "TestRegionReadout.h"

#include <QCoreApplication>
#include <QTest>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    int status = 0;
    { TestBackendClient    t; status |= QTest::qExec(&t, argc, argv); }
    { TestCatalogueParser  t; status |= QTest::qExec(&t, argc, argv); }
    { TestBackendRouting   t; status |= QTest::qExec(&t, argc, argv); }
    { TestImageLod         t; status |= QTest::qExec(&t, argc, argv); }
    { TestAnalysisParamStore t; status |= QTest::qExec(&t, argc, argv); }
    { TestBackendContract  t; status |= QTest::qExec(&t, argc, argv); }
    { TestRemoteSliceCache t; status |= QTest::qExec(&t, argc, argv); }
    { TestLinkedWindowRegistry t; status |= QTest::qExec(&t, argc, argv); }
    { TestSpectralUnitConvert t; status |= QTest::qExec(&t, argc, argv); }
    { TestCubeRegionGeom   t; status |= QTest::qExec(&t, argc, argv); }
    { TestCubeToolGate     t; status |= QTest::qExec(&t, argc, argv); }
    { TestToolResultRecord t; status |= QTest::qExec(&t, argc, argv); }
    { TestProductOrigin   t; status |= QTest::qExec(&t, argc, argv); }
    { TestCatalogueCsv    t; status |= QTest::qExec(&t, argc, argv); }
    { TestFitsHeaderString t; status |= QTest::qExec(&t, argc, argv); }
    { TestStokesCompanion t; status |= QTest::qExec(&t, argc, argv); }
    { TestSpectralAxisInfer t; status |= QTest::qExec(&t, argc, argv); }
    { TestKinematicLevelField t; status |= QTest::qExec(&t, argc, argv); }
    { TestRayVoxelPick     t; status |= QTest::qExec(&t, argc, argv); }
    { TestSedBuilder       t; status |= QTest::qExec(&t, argc, argv); }
    { TestVlkbPosString    t; status |= QTest::qExec(&t, argc, argv); }
    { TestSkyOverlap       t; status |= QTest::qExec(&t, argc, argv); }
    { TestFitsHeaderCard   t; status |= QTest::qExec(&t, argc, argv); }
    { TestViewerModeState  t; status |= QTest::qExec(&t, argc, argv); }
    { TestLayerCardGeometry t; status |= QTest::qExec(&t, argc, argv); }
    { TestRegionReadout    t; status |= QTest::qExec(&t, argc, argv); }
    return status;
}
