// test_region_flux.cpp
// See TestRegionFlux.h.

#include "TestRegionFlux.h"

#include "RegionFlux.h"
#include "RegionModel.h"
#include "RegionTreeRows.h"

#include <QJsonObject>
#include <QTest>

#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;
using namespace visivo::flux;
using visivo::regions::Region;
using visivo::regions::Shape;

namespace {

Region circle(double cx, double cy, double r, const QString &name = QString())
{
    Region c;
    c.shape = Shape::Circle;
    c.center = { cx, cy };
    c.radius = r;
    c.name = name;
    return c;
}

constexpr double kFwhmToSigma = 0.42466090014400953;   // 1 / (2 √(2 ln 2))

} // namespace

void TestRegionFlux::unitsOfBrightnessAndMoments()
{
    struct Case
    {
        const char *bunit;
        bool perBeam;
        double scale;
        const char *label;
        bool velocity;
    };
    const Case cases[] = {
        { "Jy/beam", true, 1.0, "Jy", false },
        { "JY/BEAM", true, 1.0, "Jy", false },
        { "Jy beam-1", true, 1.0, "Jy", false },
        { "Jy/beam-1", true, 1.0, "Jy", false },
        { "mJy/beam", true, 1e-3, "Jy", false },
        // The moment-0 units the backend writes: BUNIT, a space, the spectral unit.
        { "Jy/beam m/s", true, 1e-3, "Jy km/s", true },
        { "Jy/beam km/s", true, 1.0, "Jy km/s", true },
        { "JY/BEAM m.s**-1", true, 1e-3, "Jy km/s", true },   // HCG 44, no CUNIT3
        { "Jy beam-1 km s-1", true, 1.0, "Jy km/s", true },
        { "Jy/beam.km/s", true, 1.0, "Jy km/s", true },        // CASA immoments
        { "Jy/beam m / s", true, 1e-3, "Jy km/s", true },
        { "mJy/beam km/s", true, 1e-3, "Jy km/s", true },
        { "Jy/beam Hz", true, 1.0, "Jy Hz", false },
        { "K", false, 1.0, "", false },
        { "K km/s", false, 1.0, "", false },
        { "Jy/pixel", false, 1.0, "", false },
        { "MJy/sr", false, 1.0, "", false },
        // The beam after the spectral unit, and a case-sensitive prefix.
        { "Jy km s-1 beam-1", true, 1.0, "Jy km/s", true },
        { "Jy km/s/beam", true, 1.0, "Jy km/s", true },
        { "Jy m/s/beam", true, 1e-3, "Jy km/s", true },
        { "MJy/beam", true, 1e6, "Jy", false },   // mega, as FITS reads it
        { "K km/s/beam", false, 1.0, "", false },
        { "", false, 1.0, "", false },
        { "m/s", false, 1.0, "", false },
    };
    for (const Case &c : cases) {
        const FluxUnit u = parseFluxUnit(QString::fromUtf8(c.bunit));
        QVERIFY2(u.perBeam == c.perBeam, c.bunit);
        if (!c.perBeam)
            continue;
        QVERIFY2(std::abs(u.scale - c.scale) < 1e-15, c.bunit);
        QCOMPARE(u.label, QString::fromUtf8(c.label));
        QVERIFY2(u.velocity == c.velocity, c.bunit);
    }
    // A frequency-integrated map: Jy Hz, or Jy km/s through the rest frequency
    // (Δv = c Δν / ν₀), as the backend's HI products convert it.
    const FluxUnit hz = parseFluxUnit(u"Jy/beam MHz"_s);
    QCOMPARE(hz.label, u"Jy MHz"_s);
    QCOMPARE(hz.frequencyToHz, 1e6);
    QVERIFY(!hz.velocity);
    const FluxUnit kms = withRestFrequency(hz, 1420405751.768);
    QVERIFY(kms.velocity && kms.fromFrequency);
    QCOMPARE(kms.label, u"Jy km/s"_s);
    QVERIFY(std::abs(kms.scale - 1e6 * 2.99792458e5 / 1420405751.768) < 1e-9);
    QCOMPARE(withRestFrequency(hz, 0.0).label, u"Jy MHz"_s);
    QCOMPARE(withRestFrequency(parseFluxUnit(u"Jy/beam m/s"_s), 1.4e9).scale, 1e-3);
}

void TestRegionFlux::beamAreaIsTheGaussianSolidAngle()
{
    // 6 × 4 pixels FWHM: π·6·4/(4 ln 2) = 2π σx σy.
    const double pix = 2.0 / 3600.0;
    const double omega = beamAreaPixels(6 * pix, 4 * pix, -pix, pix);
    QVERIFY(std::abs(omega - 2 * visivo::regions::kPi * 6 * kFwhmToSigma * 4 * kFwhmToSigma) < 1e-9);
    QCOMPARE(beamAreaPixels(0.0, 4 * pix, -pix, pix), 0.0);
    QCOMPARE(beamAreaPixels(6 * pix, 4 * pix, 0.0, pix), 0.0);
    QCOMPARE(beamAreaPixels(std::nan(""), 4 * pix, -pix, pix), 0.0);
}

void TestRegionFlux::fluxOfAGaussianSourceIsItsKnownFlux()
{
    // A beam-shaped Gaussian of peak 0.5 Jy/beam in 10 channels of 5000 m/s:
    // its moment 0 sums to 0.5 · 50000 · Ω (Jy/beam m/s), a flux of 25 Jy km/s.
    const double pix = 2.0 / 3600.0;
    const double omega = beamAreaPixels(6 * pix, 4 * pix, -pix, pix);
    const int n = 64;
    const double c = (n - 1) / 2.0, sx = 4 * kFwhmToSigma, sy = 6 * kFwhmToSigma;
    double sum = 0.0;
    long count = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            if (std::hypot(x - c, y - c) > 25.0)
                continue;
            sum += 0.5 * 50000.0 * std::exp(-0.5 * (std::pow((x - c) / sx, 2) + std::pow((y - c) / sy, 2)));
            ++count;
        }
    const RegionFlux m = regionFlux(sum, count, omega, parseFluxUnit(u"Jy/beam m/s"_s), Noise{});
    QVERIFY(m.valid);
    QCOMPARE(m.unit, u"Jy km/s"_s);
    QVERIFY2(std::abs(m.flux - 25.0) < 25.0 * 1e-4, qPrintable(QString::number(m.flux)));
    QVERIFY(std::isnan(m.error));   // no noise given
    // The same cube in km/s: the same number.
    const RegionFlux k = regionFlux(sum * 1e-3, count, omega, parseFluxUnit(u"Jy/beam km/s"_s), Noise{});
    QVERIFY(std::abs(k.flux - m.flux) < 1e-9);
    // A single channel in Jy/beam: a flux density in Jy.
    const RegionFlux ch = regionFlux(sum / 50000.0, count, omega, parseFluxUnit(u"Jy/beam"_s), Noise{});
    QCOMPARE(ch.unit, u"Jy"_s);
    QVERIFY(std::abs(ch.flux - 0.5) < 0.5 * 1e-4);
    // Not per beam, or no beam: no flux.
    QVERIFY(!regionFlux(sum, count, omega, parseFluxUnit(u"K km/s"_s), Noise{}).valid);
    QVERIFY(!regionFlux(sum, count, 0.0, parseFluxUnit(u"Jy/beam"_s), Noise{}).valid);
    QVERIFY(!regionFlux(sum, 0, omega, parseFluxUnit(u"Jy/beam"_s), Noise{}).valid);
}

void TestRegionFlux::backgroundIsSubtractedAndGivesTheError()
{
    const FluxUnit u = parseFluxUnit(u"Jy/beam km/s"_s);
    Noise bg;
    bg.mean = 2.0;
    bg.sigma = 3.0;
    bg.subtract = true;
    bg.count = 400;
    // 100 pixels summing to 1000, background of 400 pixels at 2 each, Ω = 25:
    // σ² N/Ω for the region's beams, σ² N²/(N_bg Ω) for the background mean.
    const RegionFlux f = regionFlux(1000.0, 100, 25.0, u, bg);
    QVERIFY(std::abs(f.flux - (1000.0 - 200.0) / 25.0) < 1e-12);
    QVERIFY(std::abs(f.error - 3.0 * std::sqrt(100.0 / 25.0 + 100.0 * 100.0 / (400.0 * 25.0))) < 1e-12);
    // A σ of zero (a clipped map) cannot be measured: no error.
    Noise flat = bg;
    flat.sigma = 0.0;
    QVERIFY(std::isnan(regionFlux(1000.0, 100, 25.0, u, flat).error));
    // A clipped (thresholded) moment map: more than a fifth zeros or blanks.
    QVERIFY(looksClipped(100, 30, 0));
    QVERIFY(looksClipped(70, 0, 30));
    QVERIFY(!looksClipped(100, 10, 5));
    QVERIFY(!looksClipped(0, 0, 0));
    // Map noise: the error, no subtraction.
    Noise map;
    map.mean = 2.0;   // ignored without subtract
    map.sigma = 3.0;
    const RegionFlux g = regionFlux(1000.0, 100, 25.0, u, map);
    QVERIFY(std::abs(g.flux - 40.0) < 1e-12);
    QVERIFY(std::abs(g.error - 6.0) < 1e-12);
}

void TestRegionFlux::hcg44NumbersOfTheUseCase()
{
    // Use case 10 (HCG 44, moment 0 over channels 21-101): the map's region
    // statistics give the published table's fluxes.
    const double omega = beamAreaPixels(0.0147306183353, 0.00907219853252, -0.00222222230691,
                                        0.00222222230691);
    QVERIFY2(std::abs(omega - 30.66) < 0.005, qPrintable(QString::number(omega)));
    const FluxUnit u = parseFluxUnit(u"JY/BEAM m.s**-1"_s);
    Noise bg;
    bg.mean = -3.259;
    bg.sigma = 48.171;
    bg.count = 2186;
    bg.subtract = true;
    struct Galaxy
    {
        long n;
        double mean;
        const char *text;
    };
    const Galaxy galaxies[] = {
        { 706, 140.92, "3.32 ± 0.27 Jy km/s" },    // NGC 3190
        { 1105, 157.66, "5.80 ± 0.35 Jy km/s" },   // NGC 3187
        { 1088, 35.15, "1.36 ± 0.35 Jy km/s" },    // NGC 3185
    };
    for (const Galaxy &g : galaxies) {
        const RegionFlux f = regionFlux(g.mean * double(g.n), g.n, omega, u, bg);
        QCOMPARE(formatFlux(f), QString::fromUtf8(g.text));
    }
    // NGC 3190 at 25 Mpc: 4.9 × 10^8 solar masses.
    const RegionFlux f = regionFlux(140.92 * 706, 706, omega, u, bg);
    QVERIFY(std::abs(hiMassSolar(f.flux, 25.0) / 1e8 - 4.89) < 0.01);
}

void TestRegionFlux::reducedMapsCountBlocks()
{
    // A map shown 1 pixel in 2×2: each displayed pixel is four of the cube's.
    const FluxUnit u = parseFluxUnit(u"Jy/beam"_s);
    Noise n;
    n.sigma = 1.0;
    const RegionFlux full = regionFlux(400.0, 400, 20.0, u, n);
    const RegionFlux reduced = regionFlux(100.0, 100, 20.0, u, n, 4.0);
    QVERIFY(std::abs(full.flux - reduced.flux) < 1e-12);
    QVERIFY(std::abs(full.error - reduced.error) < 1e-12);
}

void TestRegionFlux::madSigmaIsRobust()
{
    // ±1 about a median of 0: MAD 1, whatever two bright pixels and a blank do.
    std::vector<double> v{ 0.0 };
    for (int i = 0; i < 1000; ++i)
        v.push_back((i % 2 ? 1.0 : -1.0));
    v.push_back(1e9);    // a source
    v.push_back(-1e9);   // a negative bowl
    v.push_back(std::nan(""));
    const double s = madSigma(v);
    QVERIFY2(std::abs(s - 1.4826) < 1e-9, qPrintable(QString::number(s)));
    // An even count: the median is the mean of the two middle values.
    std::vector<double> even{ 1.0, 2.0, 3.0, 4.0 };   // median 2.5, |dev| 1.5 0.5 0.5 1.5
    QVERIFY(std::abs(madSigma(even) - 1.4826) < 1e-9);
    std::vector<double> none{ std::nan("") };
    QVERIFY(std::isnan(madSigma(none)));
}

void TestRegionFlux::nearestBackgroundIsChosen()
{
    std::vector<Region> all;
    Region target = circle(10, 10, 3, u"src"_s);
    target.id = 1;
    Region far = circle(100, 100, 5, u"far"_s);
    far.id = 2;
    far.props[u"source"_s] = false;
    Region nearBox;
    nearBox.shape = Shape::Annulus;
    nearBox.center = { 12, 10 };
    nearBox.radii = { 5, 8 };
    nearBox.id = 3;
    nearBox.props[u"source"_s] = false;
    Region plain = circle(11, 10, 2, u"plain"_s);   // nearer, but a source
    plain.id = 4;
    Region text;
    text.shape = Shape::Text;
    text.center = { 10, 10 };
    text.id = 5;
    text.props[u"source"_s] = false;   // no area: never a background
    all = { target, far, nearBox, plain, text };
    const Region *bg = nearestBackground(all, target);
    QVERIFY(bg);
    QCOMPARE(bg->id, 3);
    // A background region itself analysed: another background, not itself.
    const Region *other = nearestBackground(all, nearBox);
    QVERIFY(other);
    QCOMPARE(other->id, 2);
    QVERIFY(!nearestBackground({ target, plain }, target));
}

void TestRegionFlux::backgroundFlagRoundTripsAndUndoes()
{
    visivo::regions::RegionList list;
    const int id = list.add(circle(5, 5, 2));
    QVERIFY(!visivo::regions::isBackground(*list.find(id)));
    QVERIFY(list.setBackground(id, true));
    QVERIFY(visivo::regions::isBackground(*list.find(id)));
    QVERIFY(!list.setBackground(id, true));   // no change, no undo step
    // ds9's `background` property: written back on export, read on import.
    const QJsonObject o = visivo::regions::toCanonical(*list.find(id), 0.0);
    QCOMPARE(o.value(u"props"_s).toObject().value(u"source"_s).toBool(true), false);
    Region back;
    QVERIFY(visivo::regions::fromCanonical(o, 0.0, &back));
    QVERIFY(visivo::regions::isBackground(back));
    // Session Data says so.
    QVERIFY(visivo::regions::regionTreeRow(*list.find(id)).text.endsWith(u" · background"_s));
    QVERIFY(list.undo());
    QVERIFY(!visivo::regions::isBackground(*list.find(id)));
    QVERIFY(!visivo::regions::regionTreeRow(*list.find(id)).text.contains(u"background"_s));
    // A region without an area cannot be one.
    Region t;
    t.shape = Shape::Text;
    const int tid = list.add(t);
    QVERIFY(!list.setBackground(tid, true));
}

void TestRegionFlux::distancesAndMasses()
{
    const TypedDistance a = parseDistance(u"25"_s);
    QVERIFY(a.ok && !a.fromRedshift);
    QCOMPARE(a.mpc(), 25.0);
    QVERIFY(parseDistance(u" 25 Mpc "_s).ok);
    QVERIFY(parseDistance(u"25 mpc"_s).ok);
    const TypedDistance z = parseDistance(u"z=0.005"_s);
    QVERIFY(z.ok && z.fromRedshift);
    // Planck18 luminosity distance at z = 0.005: astropy gives 22.239 Mpc.
    QVERIFY2(std::abs(z.mpc() - 22.24) < 0.03, qPrintable(QString::number(z.mpc())));
    QVERIFY(parseDistance(u"z 0.005"_s).ok);
    QVERIFY(parseDistance(u"Z: 0.005"_s).ok);
    for (const QString &bad : { u""_s, u"abc"_s, u"-3"_s, u"0"_s, u"z=-1"_s, u"25 kpc"_s })
        QVERIFY2(!parseDistance(bad).ok, qPrintable(bad));
    QVERIFY(std::abs(hiMassSolar(1.0, 1.0) - 2.356e5) < 1e-9);
    // From a redshift: M_HI = 2.356e5 D_L² S / (1 + z) (Meyer et al. 2017).
    QVERIFY(std::abs(z.hiMass(1.0) - 2.356e5 * z.mpc() * z.mpc() / 1.005) < 1e-3);
    QVERIFY(std::abs(a.hiMass(1.0) - 2.356e5 * 625.0) < 1e-6);
    // Below cz = 1000 km/s a redshift distance is flagged.
    QVERIFY(z.uncertain() == false);   // cz = 1499 km/s
    QVERIFY(parseDistance(u"z=0.002"_s).uncertain());
    QVERIFY(parseDistance(u"z=0.002"_s).describe().contains(u"peculiar"_s));
    QVERIFY(!a.uncertain());
    // The local Planck18 integrator against astropy's Planck18 (the backend's).
    const double astropy[][2] = { { 0.001, 4.434262480323607 }, { 0.005, 22.23909890586761 },
                                  { 0.05, 229.88062846485786 }, { 0.5, 2919.6249521806776 },
                                  { 1.0, 6791.2689423031325 } };
    for (const auto &zd : astropy)
        QVERIFY2(std::abs(luminosityDistanceMpc(zd[0]) / zd[1] - 1.0) < 1e-3,
                 qPrintable(QString::number(luminosityDistanceMpc(zd[0]))));
    QVERIFY(std::abs(hiMassSolar(2.0, 10.0) - 2.356e5 * 200.0) < 1e-6);
}

void TestRegionFlux::fluxAndMassText()
{
    RegionFlux f;
    f.valid = true;
    f.flux = 3.3234;
    f.error = 0.2311;
    f.unit = u"Jy km/s"_s;
    QCOMPARE(formatFlux(f), u"3.32 ± 0.23 Jy km/s"_s);
    f.flux = 123.4;
    f.error = 23.0;
    QCOMPARE(formatFlux(f), u"123 ± 23 Jy km/s"_s);
    f.error = std::nan("");
    QCOMPARE(formatFlux(f), u"123.4 Jy km/s"_s);
    QCOMPARE(formatHiMass(4.89e8, 0.34e8), u"(4.89 ± 0.34) × 10^8 M☉"_s);
    QCOMPARE(formatHiMass(4.89e8, std::nan("")), u"4.89 × 10^8 M☉"_s);
    QCOMPARE(formatHiMass(-1e7, 1e7), u"-1e+07 M☉"_s);
}

void TestRegionFlux::everyUnitKindIsRecognised()
{
    QVERIFY(parseFluxUnit(u"Jy/beam"_s).kind == UnitKind::PerBeam);
    const FluxUnit pixel = parseFluxUnit(u"Jy/pixel"_s);
    QVERIFY(pixel.kind == UnitKind::PerPixel);
    QCOMPARE(pixel.label, u"Jy"_s);
    QVERIFY(parseFluxUnit(u"JY/PIX"_s).kind == UnitKind::PerPixel);
    const FluxUnit sr = parseFluxUnit(u"MJy/sr"_s);
    QVERIFY(sr.kind == UnitKind::PerSteradian);
    QCOMPARE(sr.scale, 1e6);
    const FluxUnit k = parseFluxUnit(u"K"_s);
    QVERIFY(k.kind == UnitKind::Temperature);
    QCOMPARE(k.label, u"K"_s);
    const FluxUnit w = parseFluxUnit(u"K km/s"_s);   // a CHIMPS moment 0 (#416)
    QVERIFY(w.kind == UnitKind::Temperature);
    QVERIFY(w.velocity);
    QCOMPARE(w.label, u"K km/s"_s);
    QVERIFY(parseFluxUnit(u"mK"_s).scale == 1e-3);
    QVERIFY(parseFluxUnit(QString()).kind == UnitKind::Unknown);
    QVERIFY(parseFluxUnit(u"(unknown) × km/s"_s).kind == UnitKind::Unknown);
    QVERIFY(parseFluxUnit(u"km/s"_s).kind == UnitKind::Unknown);
    QVERIFY(parseFluxUnit(u"ADU"_s).kind == UnitKind::Unknown);
}

void TestRegionFlux::aJyPerPixelMapSumsWithoutTheBeam()
{
    const FluxUnit u = parseFluxUnit(u"Jy/pixel"_s);
    Noise n;
    n.sigma = 0.5;
    // No beam: the pixels count as independent, σ√N.
    const RegionFlux free = regionFlux(120.0, 100, 0.0, u, n);
    QVERIFY(free.valid);
    QCOMPARE(free.flux, 120.0);
    QVERIFY(std::abs(free.error - 0.5 * 10.0) < 1e-12);
    QCOMPARE(free.unit, u"Jy"_s);
    // With a beam of 10 pixels: N·Ω correlated samples, σ√(N·Ω).
    const RegionFlux beamed = regionFlux(120.0, 100, 10.0, u, n);
    QCOMPARE(beamed.flux, 120.0);
    QVERIFY(std::abs(beamed.error - 0.5 * std::sqrt(1000.0)) < 1e-12);
    QVERIFY(fluxCaveat(u, false).contains(u"independent pixels"_s));
}

void TestRegionFlux::aSurfaceBrightnessMapIsMultipliedByThePixelSolidAngle()
{
    // MJy/sr on 6″ pixels: Ω_pix = (6/206265)² sr.
    const double cdelt = 6.0 / 3600.0;
    const double pixelSr = std::pow(6.0 / 206264.80624709636, 2.0);
    const RegionFlux f =
            regionFlux(50.0, 10, 0.0, parseFluxUnit(u"MJy/sr"_s), Noise{}, 1.0, cdelt * cdelt);
    QVERIFY(f.valid);
    QVERIFY(std::abs(f.flux / (50.0 * 1e6 * pixelSr) - 1.0) < 1e-9);
    QCOMPARE(f.unit, u"Jy"_s);
    // Without a pixel size there is nothing to multiply by.
    QVERIFY(!regionFlux(50.0, 10, 0.0, parseFluxUnit(u"MJy/sr"_s), Noise{}).valid);
}

void TestRegionFlux::aTemperatureMapGivesBrightnessTimesAreaNotJy()
{
    // CHIMPS-like moment 0 on 7.32″ pixels: Σ W ΔΩ in K km/s arcsec².
    const double cdelt = 7.32 / 3600.0;
    const FluxUnit u = parseFluxUnit(u"K km/s"_s);
    Noise n;
    n.sigma = 0.2;
    const RegionFlux f = regionFlux(40.0, 20, 0.0, u, n, 1.0, cdelt * cdelt);
    QVERIFY(f.valid);
    QVERIFY(std::abs(f.flux - 40.0 * 7.32 * 7.32) < 1e-9);
    QCOMPARE(f.unit, u"K km/s arcsec²"_s);
    QVERIFY(std::abs(f.error - 0.2 * std::sqrt(20.0) * 7.32 * 7.32) < 1e-9);
    QVERIFY(fluxCaveat(u, false).contains(u"not converted to Jy"_s));
    QVERIFY(!hiMassApplies(u, 0.0));
}

void TestRegionFlux::aRegionSmallerThanTheBeamIsOneSample()
{
    // 5 pixels in a 25-pixel beam: fully correlated, error Nσ/Ω, not σ√(N/Ω).
    Noise n;
    n.sigma = 1.0;
    const RegionFlux f = regionFlux(10.0, 5, 25.0, parseFluxUnit(u"Jy/beam"_s), n);
    QVERIFY(std::abs(f.error - 5.0 / 25.0) < 1e-12);
}

void TestRegionFlux::unitsWithUnreadTailsAreUnknown()
{
    QVERIFY(parseFluxUnit(u"K km/s/beam"_s).kind == UnitKind::Unknown);
    QVERIFY(parseFluxUnit(u"K (T_A*)"_s).kind == UnitKind::Unknown);
    QVERIFY(parseFluxUnit(u"KM/S"_s).kind == UnitKind::Unknown);
    QVERIFY(parseFluxUnit(u"JY/BEAM KM/S"_s).kind == UnitKind::PerBeam);
    // A frequency-integrated Jy/pixel or K map goes to km/s like a Jy/beam one.
    const FluxUnit pixelHz = withRestFrequency(parseFluxUnit(u"Jy/pixel Hz"_s), kHiRestHz);
    QVERIFY(pixelHz.velocity);
    QCOMPARE(pixelHz.label, u"Jy km/s"_s);
    QVERIFY(hiMassApplies(pixelHz, kHiRestHz));
    const FluxUnit kelvinHz = withRestFrequency(parseFluxUnit(u"K MHz"_s), 345.796e9);
    QCOMPARE(kelvinHz.label, u"K km/s"_s);
    // A reduced map counts blocks for the non-beam kinds as well.
    Noise n;
    n.sigma = 1.0;
    const FluxUnit u = parseFluxUnit(u"Jy/pixel"_s);
    const RegionFlux full = regionFlux(400.0, 400, 0.0, u, n);
    const RegionFlux reduced = regionFlux(100.0, 100, 0.0, u, n, 4.0);
    QVERIFY(std::abs(full.flux - reduced.flux) < 1e-12);
    QVERIFY(std::abs(full.error - reduced.error) < 1e-12);
}

void TestRegionFlux::aFluxIsRefusedWithTheReason()
{
    QVERIFY(fluxRefusal(parseFluxUnit(u"Jy/beam"_s), u"Jy/beam"_s, 0.0, 0.0)
                    .contains(u"pixel size"_s));
    QVERIFY(fluxRefusal(parseFluxUnit(QString()), QString(), 25.0, 1e-6)
                    .contains(u"unknown"_s));
    QVERIFY(fluxRefusal(parseFluxUnit(u"(unknown) × km/s"_s), u"(unknown) × km/s"_s, 25.0, 1e-6)
                    .contains(u"unknown"_s));
    QVERIFY(fluxRefusal(parseFluxUnit(u"ADU"_s), u"ADU"_s, 25.0, 1e-6).contains(u"ADU"_s));
    QVERIFY(fluxRefusal(parseFluxUnit(u"Jy/beam"_s), u"Jy/beam"_s, 0.0, 1e-6)
                    .contains(u"no beam"_s));
    QVERIFY(fluxRefusal(parseFluxUnit(u"K"_s), u"K"_s, 0.0, 0.0).contains(u"pixel size"_s));
    QVERIFY(fluxRefusal(parseFluxUnit(u"Jy/beam"_s), u"Jy/beam"_s, 25.0, 1e-6).isEmpty());
    QVERIFY(fluxRefusal(parseFluxUnit(u"Jy/pixel"_s), u"Jy/pixel"_s, 0.0, 0.0).isEmpty());
    QVERIFY(fluxRefusal(parseFluxUnit(u"K km/s"_s), u"K km/s"_s, 0.0, 1e-6).isEmpty());
}

void TestRegionFlux::anHiMassOnlyForTheTwentyOneCentimetreLine()
{
    const FluxUnit jyKms = parseFluxUnit(u"Jy/beam km/s"_s);
    QString note;
    QVERIFY(hiMassApplies(jyKms, 1420405751.768, &note));
    QVERIFY(note.isEmpty());
    QVERIFY(!hiMassApplies(jyKms, 230.538e9));   // a CO 2-1 moment 0
    QVERIFY(hiMassApplies(jyKms, 0.0, &note));   // no rest frequency: assumed, and said
    QVERIFY(note.contains(u"21-cm"_s));
    QVERIFY(hiMassApplies(parseFluxUnit(u"Jy/pixel km/s"_s), 1420405751.768));
    QVERIFY(!hiMassApplies(parseFluxUnit(u"Jy/beam"_s), 1420405751.768));
    QVERIFY(!hiMassApplies(parseFluxUnit(u"K km/s"_s), 1420405751.768));
}
