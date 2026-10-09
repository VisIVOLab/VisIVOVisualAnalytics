// test_spectral_axis_infer.cpp
// Unit tests for the FITS CTYPE→spectral-axis classifier lifted from
// vtkWindowCube_Wcs.cpp in E-04. Encodes radio spectral-axis conventions
// (FREQ/VRAD/VOPT/FELO/VELO/CHAN) with subtle branch ordering; previously
// untested. VTK-free.

#include "TestSpectralAxisInfer.h"
#include "SpectralAxisInfer.h"

#include <QTest>

using spectralaxis::inferSpectralAxisDescriptor;
using spectralaxis::spectralAxisKindLabel;

void TestSpectralAxisInfer::nonPhysical_isChannel()
{
    // No WCS → raw channel index, regardless of the CTYPE string.
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("FREQ"), QStringLiteral("Hz"), false);
    QCOMPARE(d.kind, SpectralAxisKind::Channel);
    QVERIFY(!d.physical);
    QCOMPARE(d.label, QStringLiteral("Channel"));
}

void TestSpectralAxisInfer::freq_isFrequencyTrusted()
{
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("FREQ"), QStringLiteral("Hz"), true);
    QCOMPARE(d.kind, SpectralAxisKind::Frequency);
    QVERIFY(d.trusted);
    QVERIFY(d.physical);
    QCOMPARE(d.label, QStringLiteral("Frequency"));
}

void TestSpectralAxisInfer::vrad_isRadioVelocity()
{
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("VRAD"), QStringLiteral("m/s"), true);
    QCOMPARE(d.kind, SpectralAxisKind::RadioVelocity);
    QVERIFY(d.trusted);
}

void TestSpectralAxisInfer::vopt_isOpticalVelocity()
{
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("VOPT"), QStringLiteral("m/s"), true);
    QCOMPARE(d.kind, SpectralAxisKind::OpticalVelocity);
    QVERIFY(d.trusted);
}

void TestSpectralAxisInfer::felo_isOpticalVelocity()
{
    // FELO (the legacy FELO-HEL spelling) maps to optical velocity.
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("FELO-HEL"), QStringLiteral("m/s"), true);
    QCOMPARE(d.kind, SpectralAxisKind::OpticalVelocity);
    QVERIFY(d.trusted);
}

void TestSpectralAxisInfer::velo_isGenericVelocityInferred()
{
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("VELO-LSR"), QStringLiteral("m/s"), true);
    QCOMPARE(d.kind, SpectralAxisKind::GenericVelocity);
    QVERIFY(d.inferred);
    QVERIFY(!d.trusted);
}

void TestSpectralAxisInfer::containsVelo_isGenericVelocity()
{
    // The fallback also fires when VELO merely appears in the CTYPE.
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("XVELO"), QStringLiteral("m/s"), true);
    QCOMPARE(d.kind, SpectralAxisKind::GenericVelocity);
}

void TestSpectralAxisInfer::chan_isChannelNonPhysical()
{
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("CHANNEL"), QString(), true);
    QCOMPARE(d.kind, SpectralAxisKind::Channel);
    QVERIFY(!d.physical);   // an explicit CHAN axis demotes to non-physical
    QCOMPARE(d.sourceLabel, QStringLiteral("Channel"));
}

void TestSpectralAxisInfer::unknownNonEmpty_isGenericSpectralInferred()
{
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("WAVE"), QStringLiteral("m"), true);
    QCOMPARE(d.kind, SpectralAxisKind::GenericSpectral);
    QVERIFY(d.inferred);
    QCOMPARE(d.label, QStringLiteral("WAVE"));   // label = the raw CTYPE
}

void TestSpectralAxisInfer::unknownEmpty_isGenericSpectralNotInferred()
{
    const auto d = inferSpectralAxisDescriptor(QString(), QStringLiteral("m"), true);
    QCOMPARE(d.kind, SpectralAxisKind::GenericSpectral);
    QVERIFY(!d.inferred);                         // nothing to infer from
    QCOMPARE(d.label, QStringLiteral("Spectral Axis"));
}

void TestSpectralAxisInfer::caseInsensitive_lowercaseFreq()
{
    // CTYPE is upper-cased before matching.
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("freq"), QStringLiteral("Hz"), true);
    QCOMPARE(d.kind, SpectralAxisKind::Frequency);
}

void TestSpectralAxisInfer::unitIsTrimmed()
{
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("FREQ"),
                                               QStringLiteral("  Hz  "), true);
    QCOMPARE(d.unit, QStringLiteral("Hz"));
}

void TestSpectralAxisInfer::branchOrder_feloBeforeVelo()
{
    // "FELO" contains no "VELO", but the ordering guarantees the VOPT/FELO branch
    // wins over the generic VELO fallback for anything starting FELO. Verify FELO
    // is optical (trusted), not the inferred generic velocity.
    const auto d = inferSpectralAxisDescriptor(QStringLiteral("FELO"), QStringLiteral("m/s"), true);
    QCOMPARE(d.kind, SpectralAxisKind::OpticalVelocity);
    QVERIFY(d.trusted);
    QVERIFY(!d.inferred);
}

void TestSpectralAxisInfer::label_matchesKind()
{
    QCOMPARE(spectralAxisKindLabel(SpectralAxisKind::Frequency), QStringLiteral("Frequency"));
    QCOMPARE(spectralAxisKindLabel(SpectralAxisKind::RadioVelocity), QStringLiteral("Radio Velocity"));
    QCOMPARE(spectralAxisKindLabel(SpectralAxisKind::OpticalVelocity), QStringLiteral("Optical Velocity"));
    QCOMPARE(spectralAxisKindLabel(SpectralAxisKind::GenericVelocity), QStringLiteral("Velocity"));
    QCOMPARE(spectralAxisKindLabel(SpectralAxisKind::GenericSpectral), QStringLiteral("Spectral Axis"));
    QCOMPARE(spectralAxisKindLabel(SpectralAxisKind::Channel), QStringLiteral("Channel"));
}
