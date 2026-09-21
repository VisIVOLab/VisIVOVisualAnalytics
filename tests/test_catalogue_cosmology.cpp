// test_catalogue_cosmology.cpp
// See TestCatalogueCosmology.h.

#include "TestCatalogueCosmology.h"

#include "Catalogue3DParser.h"

#include <QTest>

using Catalogue3DParser::detail::comovingDistanceMpc;
using Catalogue3DParser::detail::hubbleDistanceMpc;

namespace {

/// astropy 7: `Planck18.comoving_distance(z).to(u.Mpc).value`.
struct Reference
{
    double z;
    double comovingMpc;
};

constexpr Reference kAstropyPlanck18[] = {
    { 0.001,    4.4304 },
    { 0.010,   44.2052 },
    { 0.050,  218.9345 },
    { 0.100,  432.5660 },
    { 0.200,  843.5778 },
    { 0.500, 1946.4174 },
    { 1.000, 3395.6341 },
    { 2.000, 5308.1890 },
    { 3.000, 6504.0044 },
    { 5.000, 7946.2926 },
    { 10.000, 9636.2848 },
    { 20.000, 10956.6100 },
};

}   // namespace

void TestCatalogueCosmology::comovingDistanceMatchesAstropyPlanck18()
{
    // 0.1 % is the budget, and it is a MODEL budget, not an integration one:
    // the local integrator is a 1000-step trapezoid over a flat Ωm = 0.3111
    // universe with no radiation term, while astropy's Planck18 carries photons
    // and a massive neutrino and uses Ωm = 0.30966. Those differences nearly
    // cancel — measured 0.001 % at z = 0.1, 0.03 % at z = 5, 0.06 % at z = 20,
    // which is why the table runs to z = 20 — and anything approaching the
    // budget means one of the two has been changed.
    for (const auto &point : kAstropyPlanck18) {
        const double computed = comovingDistanceMpc(point.z);
        const double relative = std::abs(computed - point.comovingMpc) / point.comovingMpc;
        QVERIFY2(relative < 1e-3,
                 qPrintable(QStringLiteral("z=%1: %2 Mpc vs astropy %3 Mpc (%4 %)")
                                    .arg(point.z)
                                    .arg(computed, 0, 'f', 3)
                                    .arg(point.comovingMpc, 0, 'f', 3)
                                    .arg(relative * 100.0, 0, 'f', 4)));
    }
}

void TestCatalogueCosmology::comovingDistanceIsZeroAtRedshiftZero()
{
    QCOMPARE(comovingDistanceMpc(0.0), 0.0);
}

void TestCatalogueCosmology::negativeRedshiftsAreRefused()
{
    // A catalogue column that is empty, unparseable or a blueshift must not
    // come back as a negative radius: the caller reads 0 as "no distance".
    QCOMPARE(comovingDistanceMpc(-0.1), 0.0);
    QCOMPARE(hubbleDistanceMpc(-0.1), 0.0);
}

void TestCatalogueCosmology::comovingDistanceGrowsWithRedshift()
{
    double previous = 0.0;
    for (double z = 0.01; z <= 6.0; z += 0.05) {
        const double current = comovingDistanceMpc(z);
        QVERIFY2(current > previous, qPrintable(QStringLiteral("not monotonic at z=%1").arg(z)));
        previous = current;
    }
}

void TestCatalogueCosmology::theHubbleLawIsTheLowRedshiftLimit()
{
    // cz/H0 is what the viewer's "redshift-space" distance mode shows. At z
    // where the universe has not had time to matter it must agree with the
    // integral, or the two modes differ for a numerical reason rather than a
    // physical one.
    for (const double z : { 0.001, 0.002, 0.005 }) {
        const double relative =
                std::abs(hubbleDistanceMpc(z) - comovingDistanceMpc(z)) / comovingDistanceMpc(z);
        QVERIFY2(relative < 2e-3, qPrintable(QStringLiteral("z=%1: %2 %")
                                                     .arg(z)
                                                     .arg(relative * 100.0, 0, 'f', 4)));
    }
}

void TestCatalogueCosmology::theHubbleLawOverstatesDistanceAtHighRedshift()
{
    // And by z = 1 it is nowhere near: ~4437 Mpc against ~3396. The modes are
    // labelled differently for this reason, and a viewer that quietly used one
    // for the other would misplace every source by a quarter of its distance.
    QVERIFY(hubbleDistanceMpc(1.0) > comovingDistanceMpc(1.0) * 1.2);
}
