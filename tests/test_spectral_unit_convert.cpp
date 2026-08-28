// test_spectral_unit_convert.cpp
// Unit tests for the spectral-unit helpers extracted from vtkWindowCube_Link.cpp
// in the E-04 shared-helper lift. These drive linked-channel velocity matching
// across cubes and had no tests where they lived; this locks their subtle
// behaviour (mhz-vs-velocity disambiguation, astropy m.s**-1 spelling).

#include "TestSpectralUnitConvert.h"
#include "SpectralUnitConvert.h"

#include <QTest>

using spectral::SpectralUnit;
using spectral::parseSpectralUnit;
using spectral::convertSpectralValue;

void TestSpectralUnitConvert::parse_velocity_kms()
{
    const SpectralUnit u = parseSpectralUnit(QStringLiteral("km/s"));
    QVERIFY(u.ok);
    QCOMPARE(u.quantity, 1);            // velocity
    QCOMPARE(u.siFactor, 1.0e3);
}

void TestSpectralUnitConvert::parse_velocity_ms()
{
    const SpectralUnit u = parseSpectralUnit(QStringLiteral("m/s"));
    QVERIFY(u.ok);
    QCOMPARE(u.quantity, 1);
    QCOMPARE(u.siFactor, 1.0);
}

void TestSpectralUnitConvert::parse_velocity_astropySpellings()
{
    // astropy normalises "m/s" as "m.s**-1" and "km/s" as "km.s**-1".
    const SpectralUnit m = parseSpectralUnit(QStringLiteral("m.s**-1"));
    QVERIFY(m.ok);
    QCOMPARE(m.quantity, 1);
    QCOMPARE(m.siFactor, 1.0);
    const SpectralUnit km = parseSpectralUnit(QStringLiteral("km.s**-1"));
    QVERIFY(km.ok);
    QCOMPARE(km.quantity, 1);
    QCOMPARE(km.siFactor, 1.0e3);
}

void TestSpectralUnitConvert::parse_frequency_all()
{
    QCOMPARE(parseSpectralUnit(QStringLiteral("Hz")).siFactor, 1.0);
    QCOMPARE(parseSpectralUnit(QStringLiteral("kHz")).siFactor, 1.0e3);
    QCOMPARE(parseSpectralUnit(QStringLiteral("MHz")).siFactor, 1.0e6);
    QCOMPARE(parseSpectralUnit(QStringLiteral("GHz")).siFactor, 1.0e9);
    for (const char *f : { "Hz", "kHz", "MHz", "GHz" }) {
        const SpectralUnit u = parseSpectralUnit(QString::fromLatin1(f));
        QVERIFY(u.ok);
        QCOMPARE(u.quantity, 2);        // frequency
    }
}

void TestSpectralUnitConvert::parse_mhzIsFrequencyNotVelocity()
{
    // The whole point of checking frequency first: "MHz" normalises to "mhz",
    // which must be frequency, not mistaken for a velocity ("m..." prefix).
    const SpectralUnit u = parseSpectralUnit(QStringLiteral("MHz"));
    QCOMPARE(u.quantity, 2);
    QVERIFY(u.quantity != 1);
}

void TestSpectralUnitConvert::parse_caseInsensitive()
{
    QCOMPARE(parseSpectralUnit(QStringLiteral("KM/S")).quantity, 1);
    QCOMPARE(parseSpectralUnit(QStringLiteral("hZ")).quantity, 2);
}

void TestSpectralUnitConvert::parse_whitespaceStripped()
{
    const SpectralUnit u = parseSpectralUnit(QStringLiteral("  km / s "));
    QVERIFY(u.ok);
    QCOMPARE(u.quantity, 1);
    QCOMPARE(u.siFactor, 1.0e3);
}

void TestSpectralUnitConvert::parse_unknownIsNotOk()
{
    QVERIFY(!parseSpectralUnit(QStringLiteral("Jy")).ok);
    QVERIFY(!parseSpectralUnit(QString()).ok);
    QVERIFY(!parseSpectralUnit(QStringLiteral("pixel")).ok);
}

void TestSpectralUnitConvert::convert_kmsToMs()
{
    double out = 0.0;
    QVERIFY(convertSpectralValue(1.5, QStringLiteral("km/s"), QStringLiteral("m/s"), out));
    QCOMPARE(out, 1500.0);
}

void TestSpectralUnitConvert::convert_msToKms()
{
    double out = 0.0;
    QVERIFY(convertSpectralValue(1500.0, QStringLiteral("m/s"), QStringLiteral("km/s"), out));
    QCOMPARE(out, 1.5);
}

void TestSpectralUnitConvert::convert_frequencyHzToKhz()
{
    double out = 0.0;
    QVERIFY(convertSpectralValue(2000.0, QStringLiteral("Hz"), QStringLiteral("kHz"), out));
    QCOMPARE(out, 2.0);
}

void TestSpectralUnitConvert::convert_sameUnitIsIdentity()
{
    double out = 0.0;
    QVERIFY(convertSpectralValue(42.0, QStringLiteral("km/s"), QStringLiteral("km/s"), out));
    QCOMPARE(out, 42.0);
}

void TestSpectralUnitConvert::convert_crossQuantityFails()
{
    // velocity ↔ frequency needs a rest frequency → refused.
    double out = -1.0;
    QVERIFY(!convertSpectralValue(100.0, QStringLiteral("km/s"), QStringLiteral("Hz"), out));
    QCOMPARE(out, -1.0);   // out left untouched on failure
}

void TestSpectralUnitConvert::convert_unknownUnitFails()
{
    double out = -1.0;
    QVERIFY(!convertSpectralValue(1.0, QStringLiteral("Jy"), QStringLiteral("m/s"), out));
    QCOMPARE(out, -1.0);
}
