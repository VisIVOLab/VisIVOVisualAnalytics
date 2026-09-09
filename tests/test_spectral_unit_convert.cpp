// test_spectral_unit_convert.cpp
// Unit tests for the spectral-unit helpers extracted from vtkWindowCube_Link.cpp
// in the E-04 shared-helper lift. These drive linked-channel velocity matching
// across cubes and had no tests where they lived; this locks their subtle
// behaviour (mhz-vs-velocity disambiguation, astropy m.s**-1 spelling).

#include "TestSpectralUnitConvert.h"
#include "SpectralUnitConvert.h"

#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;

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

// ── Placing a rest-frame spectral line on an observed axis ───────────────────
// These drive the spectrum viewer's "Load Lines…" overlay. Getting them wrong
// does not throw: it silently draws the marker somewhere else, or off-plot,
// which is exactly how that feature behaved before the conversion existed.

void TestSpectralUnitConvert::axisKind_fromCtypeSpellings()
{
    using namespace spectral;
    QCOMPARE(axisKindFromCtype(u"FREQ"_s), AxisKind::Frequency);
    QCOMPARE(axisKindFromCtype(u"freq"_s), AxisKind::Frequency);
    QCOMPARE(axisKindFromCtype(u"VOPT-LSR"_s), AxisKind::VelocityOptical);
    QCOMPARE(axisKindFromCtype(u"FELO-HEL"_s), AxisKind::VelocityOptical);
    QCOMPARE(axisKindFromCtype(u"VRAD"_s), AxisKind::VelocityRadio);
    QCOMPARE(axisKindFromCtype(u"VELO-LSR"_s), AxisKind::VelocityRelativistic);
    QCOMPARE(axisKindFromCtype(u"RA---SIN"_s), AxisKind::Unknown);
    QCOMPARE(axisKindFromCtype(QString()), AxisKind::Unknown);
}

void TestSpectralUnitConvert::observedFrequency_redshiftsTheLine()
{
    using namespace spectral;
    const double rest = 1.420405751786e9;   // HI 21 cm
    QCOMPARE(observedFrequencyHz(rest, 0.0), rest);
    // z = 1 halves the frequency.
    QVERIFY(std::abs(observedFrequencyHz(rest, 1.0) - rest / 2.0) < 1.0);
    // A catalogue systemic velocity is an OPTICAL velocity: z = v/c.
    const double z = redshiftFromOpticalVelocity(1000.0e3);
    QVERIFY(std::abs(z - 1000.0e3 / kCLightMs) < 1e-15);
}

void TestSpectralUnitConvert::axisValue_frequencyAxisIsIdentity()
{
    using namespace spectral;
    double out = 0.0;
    QVERIFY(axisValueForFrequency(1.4e9, AxisKind::Frequency, /*axisRestHz=*/0.0, out));
    QCOMPARE(out, 1.4e9);   // no rest frequency needed on a frequency axis
}

void TestSpectralUnitConvert::axisValue_opticalRadioAndRelativisticDiffer()
{
    using namespace spectral;
    const double rest = 1.420405751786e9;
    // A line observed at the frequency an optical velocity of 1000 km/s implies.
    const double vOpt = 1000.0e3;
    const double fObs = observedFrequencyHz(rest, redshiftFromOpticalVelocity(vOpt));
    double opt = 0.0, rad = 0.0, rel = 0.0;
    QVERIFY(axisValueForFrequency(fObs, AxisKind::VelocityOptical, rest, opt));
    QVERIFY(axisValueForFrequency(fObs, AxisKind::VelocityRadio, rest, rad));
    QVERIFY(axisValueForFrequency(fObs, AxisKind::VelocityRelativistic, rest, rel));
    // Round trip through the optical convention is exact…
    QVERIFY(std::abs(opt - vOpt) < 1.0);
    // …and the radio value is lower by ≈ v²/c (3.3 km/s here) — a real offset of
    // several channels, not a rounding difference.
    QVERIFY(rad < opt);
    const double expectedGap = vOpt * vOpt / kCLightMs;
    QVERIFY(std::abs((opt - rad) - expectedGap) < 0.05 * expectedGap);
    // The relativistic convention sits between the two.
    QVERIFY(rel < opt && rel > rad);
}

void TestSpectralUnitConvert::axisValue_hiLineAtRestSitsAtZeroVelocity()
{
    using namespace spectral;
    const double rest = 1.420405751786e9;
    for (const AxisKind k : { AxisKind::VelocityOptical, AxisKind::VelocityRadio,
                              AxisKind::VelocityRelativistic }) {
        double out = -1.0;
        QVERIFY(axisValueForFrequency(rest, k, rest, out));
        QVERIFY(std::abs(out) < 1e-6);
    }
}

void TestSpectralUnitConvert::axisValue_needsARestFrequencyOnAVelocityAxis()
{
    using namespace spectral;
    double out = 0.0;
    // No RESTFRQ in the header → a frequency list cannot be placed on a velocity
    // axis, and the caller must say so rather than draw at an invented position.
    QVERIFY(!axisValueForFrequency(1.4e9, AxisKind::VelocityOptical, 0.0, out));
    QVERIFY(!axisValueForFrequency(1.4e9, AxisKind::Unknown, 1.42e9, out));
    QVERIFY(!axisValueForFrequency(0.0, AxisKind::Frequency, 0.0, out));
}
