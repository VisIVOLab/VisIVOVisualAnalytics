#include "TestImageLayerContext.h"

#include "ImageLayerContext.h"

#include <QtTest>

using namespace Qt::Literals::StringLiterals;
using namespace visivo::layerctx;

void TestImageLayerContext::theSelectedRowGivesItsOwnFile()
{
    const QStringList paths{ u"/data/base.fits"_s, u"/data/model.fits"_s, u"/data/mask.fits"_s };
    QCOMPARE(pathOfLayer(paths, 1, paths.first()), u"/data/model.fits"_s);
    QCOMPARE(pathOfLayer(paths, 2, paths.first()), u"/data/mask.fits"_s);
    QCOMPARE(pathOfLayer(paths, 0, paths.first()), u"/data/base.fits"_s);
}

void TestImageLayerContext::noSelectionFallsBackToTheBaseFile()
{
    const QStringList paths{ u"/data/base.fits"_s, u"/data/model.fits"_s };
    QCOMPARE(pathOfLayer(paths, -1, u"/data/base.fits"_s), u"/data/base.fits"_s);
    QCOMPARE(pathOfLayer(paths, 5, u"/data/base.fits"_s), u"/data/base.fits"_s);
    // A layer that carries no path (a product) names the base rather than nothing.
    QCOMPARE(pathOfLayer({ u"/data/base.fits"_s, QString() }, 1, u"/data/base.fits"_s),
             u"/data/base.fits"_s);
}

void TestImageLayerContext::bunitIsReadFromTheCardsOfThatLayer()
{
    const QStringList model{ u"SIMPLE  =                    T"_s,
                             u"BUNIT   = 'Jy/pixel '           / brightness unit"_s,
                             u"END"_s };
    const QStringList map{ u"SIMPLE  =                    T"_s,
                           u"BUNIT   = 'Jy/beam '"_s };
    QCOMPARE(headerKeyword(model, u"BUNIT"_s), u"Jy/pixel"_s);
    QCOMPARE(headerKeyword(map, u"bunit"_s), u"Jy/beam"_s);
    QCOMPARE(headerKeyword(map, u"BTYPE"_s), QString());
}

void TestImageLayerContext::aSlashInTheUnitIsKept()
{
    QCOMPARE(headerKeyword({ u"BUNIT   = 'Jy/beam km/s'          / moment 0"_s }, u"BUNIT"_s),
             u"Jy/beam km/s"_s);
}

void TestImageLayerContext::commentaryCardsHaveNoValue()
{
    // A HISTORY line that mentions BUNIT is not the keyword.
    QCOMPARE(headerKeyword({ u"HISTORY BUNIT = 'K'"_s }, u"BUNIT"_s), QString());
}

void TestImageLayerContext::aCollapsedAxisIsNamedAsAnAxis()
{
    QCOMPARE(axisTypeLabel(u"VRAD"_s, u"m s-1"_s, true), u"VRAD (m s-1, collapsed)"_s);
    QCOMPARE(axisTypeLabel(u"STOKES"_s, QString(), true), u"STOKES (collapsed)"_s);
    QCOMPARE(axisRowText(3, u"VRAD"_s, u"m s-1"_s, true), u"Axis 3: VRAD (m s-1, collapsed)"_s);
}

void TestImageLayerContext::aSkyAxisKeepsItsUnitOnly()
{
    QCOMPARE(axisTypeLabel(u"RA---SIN"_s, u"deg"_s, false), u"RA---SIN (deg)"_s);
    QCOMPARE(axisTypeLabel(u"GLON-CAR"_s, QString(), false), u"GLON-CAR"_s);
}
