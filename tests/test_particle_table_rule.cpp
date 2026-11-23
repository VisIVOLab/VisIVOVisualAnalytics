#include "TestParticleTableRule.h"

#include "app/ParticleTableRule.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace visivo::particles;

namespace {

const QStringList kDemoTable{ u"x"_s,  u"y"_s,  u"z"_s,       u"vx"_s,   u"vy"_s,
                              u"vz"_s, u"LogMass"_s, u"LogD"_s, u"LogT"_s, u"LogMet"_s };

} // namespace

void TestParticleTableRule::particleExportsAreParticles()
{
    // The demo IPAC tables of issue #414 (3.9 M and 3.7 M rows).
    QVERIFY(looksLikeParticles(kDemoTable, 3'900'000));
    QVERIFY(looksLikeParticles(kDemoTable, 3'700'000));
    // A Gadget gas block written as CSV, and one quantity is enough.
    QVERIFY(looksLikeParticles({ u"X"_s, u"Y"_s, u"Z"_s, u"Mass"_s, u"HSML"_s, u"Rho"_s },
                               2'000'000));
    QVERIFY(looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"smoothing_length"_s }, kMinRows));
    QVERIFY(looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"mass"_s, u"hsml"_s }, kMinRows));
    QVERIFY(looksLikeParticles({ u"pos_x"_s, u"pos_y"_s, u"pos_z"_s, u"density"_s },
                               10'000'000));
    QVERIFY(looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"id"_s, u"temperature"_s },
                               1'000'000));
    // Units and punctuation in the names, and HDF5-style position names.
    QVERIFY(looksLikeParticles({ u"x [Mpc]"_s, u"y [Mpc]"_s, u"z [Mpc]"_s, u"rho [g/cm3]"_s },
                               1'000'000));
    QVERIFY(looksLikeParticles({ u"X(kpc)"_s, u"Y(kpc)"_s, u"Z(kpc)"_s, u"log-T"_s },
                               1'000'000));
    QVERIFY(looksLikeParticles({ u"Coordinates_x"_s, u"Coordinates_y"_s, u"Coordinates_z"_s,
                                 u"Masses"_s, u"Density"_s },
                               1'000'000));
}

void TestParticleTableRule::cataloguesOfObjectsAreNot()
{
    const qint64 big = 3'000'000;
    // A galaxy catalogue with comoving positions keeps its redshift.
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"redshift"_s, u"stellar_mass"_s,
                                  u"mass"_s },
                                big));
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"z_spec"_s, u"logmass"_s }, big));
    QVERIFY(!looksLikeParticles({ u"X"_s, u"Y"_s, u"Z"_s, u"zphot"_s, u"Mass"_s }, big));
    // On the sky, with a distance: objects.
    QVERIFY(!looksLikeParticles({ u"ra"_s, u"dec"_s, u"z"_s, u"mass"_s }, big));
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"RA"_s, u"DEC"_s, u"rho"_s }, big));
    // Positions alone, or with quantities particles do not have.
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s }, big));
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"vx"_s, u"vy"_s, u"vz"_s }, big));
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"u"_s, u"g"_s, u"r"_s }, big));
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"mvir"_s, u"rvir"_s }, big));
    // A halo or semi-analytic catalogue: a mass is not enough.
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"vx"_s, u"vy"_s, u"vz"_s, u"mass"_s,
                                  u"npart"_s },
                                big));
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"logmass"_s, u"metallicity"_s },
                                big));
    // A redshift with its unit or punctuation is still a redshift.
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"z"_s, u"z_spec (-)"_s, u"rho"_s }, big));
    // No position columns.
    QVERIFY(!looksLikeParticles({ u"mass"_s, u"rho"_s, u"hsml"_s }, big));
    QVERIFY(!looksLikeParticles({ u"x"_s, u"y"_s, u"mass"_s }, big));
}

void TestParticleTableRule::smallTablesStayCatalogues()
{
    QVERIFY(!looksLikeParticles(kDemoTable, kMinRows - 1));
    QVERIFY(!looksLikeParticles(kDemoTable, 50'000));
    QVERIFY(!looksLikeParticles(kDemoTable, 0));
    QVERIFY(looksLikeParticles(kDemoTable, kMinRows));
}

void TestParticleTableRule::theUsersChoiceWinsAndIsRememberedPerFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(u"choice.ini"_s), QSettings::IniFormat);
    const QString demo = u"/data/snap_p50n576_z0.tbl"_s;
    const QString galaxies = u"/data/galaxies.csv"_s;
    QCOMPARE(rememberedChoice(settings, demo), Choice::Auto);
    QVERIFY(treatAsParticles(Choice::Auto, kDemoTable, 3'900'000));

    rememberChoice(settings, demo, Choice::Objects);
    rememberChoice(settings, galaxies, Choice::Particles);
    QCOMPARE(rememberedChoice(settings, demo), Choice::Objects);
    QCOMPARE(rememberedChoice(settings, galaxies), Choice::Particles);
    QVERIFY(!treatAsParticles(rememberedChoice(settings, demo), kDemoTable, 3'900'000));
    QVERIFY(treatAsParticles(rememberedChoice(settings, galaxies),
                             { u"x"_s, u"y"_s, u"z"_s, u"redshift"_s }, 10));
    // Another file is not affected, and Auto forgets the choice.
    QCOMPARE(rememberedChoice(settings, u"/data/other.tbl"_s), Choice::Auto);
    rememberChoice(settings, demo, Choice::Auto);
    QCOMPARE(rememberedChoice(settings, demo), Choice::Auto);
    QVERIFY(!settings.contains(settingsKey(demo)));
}
