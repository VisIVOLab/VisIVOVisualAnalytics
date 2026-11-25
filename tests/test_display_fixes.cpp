// test_display_fixes.cpp
// See TestDisplayFixes.h.

#include "TestDisplayFixes.h"

#include "app/BackendHeaders.h"
#include "app/FontSize.h"
#include "app/ResponseHeaders.h"
#include "gui/CubeWcsUtil.h"

#include <QtTest>

using namespace Qt::StringLiterals;

void TestDisplayFixes::aFontSizeIsNeverScaledBelowItsMinimum()
{
    using visivo::fontsize::scaled;
    QCOMPARE(scaled(10.0, 0.82, 6.0), 8.2);
    // A small font stops at the minimum.
    QCOMPARE(scaled(6.0, 0.82, 6.0), 6.0);
    // QFont::pointSizeF() is -1 for a font set in pixels: -1 * 0.82 was handed to
    // setPointSizeF ("Point size <= 0 (-0.82)").
    QCOMPARE(scaled(-1.0, 0.82, 6.0), 6.0);
    QCOMPARE(scaled(0.0, 0.82, 6.0), 6.0);
}

void TestDisplayFixes::anEmptyHeaderIsPresentNotMissing()
{
    // A file with no unit: the backend sends the headers empty.
    QMap<QByteArray, QByteArray> headers;
    headers.insert(QByteArray(VisivoHeaders::BUNIT).toLower(), QByteArray());
    headers.insert(QByteArray(VisivoHeaders::SPECTRAL_AXIS_UNIT).toLower(), QByteArray(""));
    QVERIFY(!visivo::responseheaders::isMissingRequired(headers, VisivoHeaders::BUNIT));
    QVERIFY(!visivo::responseheaders::isMissingRequired(headers,
                                                        VisivoHeaders::SPECTRAL_AXIS_UNIT));
}

void TestDisplayFixes::anAbsentRequiredHeaderIsMissing()
{
    const QMap<QByteArray, QByteArray> none;
    QVERIFY(visivo::responseheaders::isMissingRequired(none, VisivoHeaders::BUNIT));
    QVERIFY(visivo::responseheaders::isMissingRequired(none, VisivoHeaders::WIDTH));
}

void TestDisplayFixes::anOptionalHeaderIsNeverMissing()
{
    const QMap<QByteArray, QByteArray> none;
    QVERIFY(!visivo::responseheaders::isMissingRequired(none, VisivoHeaders::EW_UNIT));
}

void TestDisplayFixes::theAxesOfASimulationBoxAreLinearSpace()
{
    // What the PLUTO and VBT exports write; the Sanity check read the Z as an
    // ambiguous spectral axis.
    QVERIFY(CubeWcs::isLinearSpatialAxis(u"X"_s));
    QVERIFY(CubeWcs::isLinearSpatialAxis(u" y "_s));
    QVERIFY(CubeWcs::isLinearSpatialAxis(u"Z"_s));
    QVERIFY(!CubeWcs::isLinearSpatialAxis(u"FREQ"_s));
    QVERIFY(!CubeWcs::isLinearSpatialAxis(u"VRAD"_s));
    QVERIFY(!CubeWcs::isLinearSpatialAxis(u"RA---SIN"_s));
    QVERIFY(!CubeWcs::isLinearSpatialAxis(QString()));
}
