// test_sky_wcs.cpp
// See TestSkyWcs.h.

#include "TestSkyWcs.h"

#include "gui/RegionSkyConvert.h"
#include "gui/SkyWcs.h"

#include <QTest>

#include <algorithm>
#include <cmath>

using namespace visivo::skylink;
using namespace Qt::StringLiterals;
using visivo::regions::Region;
using visivo::regions::Shape;

namespace {

// One 80-column card, as the backend's /v1/files/header hands them over.
QString card(const QString &key, const QString &value, bool quoted = false)
{
    const QString v = quoted ? QStringLiteral("'%1'").arg(value, -8) : value.rightJustified(20);
    return QStringLiteral("%1= %2 / x").arg(key, -8).arg(v).leftJustified(80);
}

QStringList common(const QStringList &ctype, double crval1, double crval2, double crpix1, double crpix2)
{
    return { card(u"CTYPE1"_s, ctype[0], true),
             card(u"CTYPE2"_s, ctype[1], true),
             card(u"CRVAL1"_s, QString::number(crval1, 'g', 15)),
             card(u"CRVAL2"_s, QString::number(crval2, 'g', 15)),
             card(u"CRPIX1"_s, QString::number(crpix1, 'g', 15)),
             card(u"CRPIX2"_s, QString::number(crpix2, 'g', 15)) };
}

QStringList cdCards(const QStringList &ctype, double crval1, double crval2, double crpix1,
                    double crpix2, double cd11, double cd12, double cd21, double cd22)
{
    QStringList c = common(ctype, crval1, crval2, crpix1, crpix2);
    c << card(u"CD1_1"_s, QString::number(cd11, 'g', 15)) << card(u"CD1_2"_s, QString::number(cd12, 'g', 15))
      << card(u"CD2_1"_s, QString::number(cd21, 'g', 15)) << card(u"CD2_2"_s, QString::number(cd22, 'g', 15));
    return c;
}

QStringList cdeltCards(const QStringList &ctype, double crval1, double crval2, double crpix1,
                       double crpix2, double cdelt1, double cdelt2)
{
    QStringList c = common(ctype, crval1, crval2, crpix1, crpix2);
    c << card(u"CDELT1"_s, QString::number(cdelt1, 'g', 15))
      << card(u"CDELT2"_s, QString::number(cdelt2, 'g', 15));
    return c;
}

// The headers the astropy numbers below were made from.
SkyWcs named(const QString &name)
{
    const double p = 1.2 / 3600.0, c = std::cos(30.0 * kDeg), s = std::sin(30.0 * kDeg);
    if (name == u"tan"_s)
        return SkyWcs::fromCards(cdeltCards({ u"RA---TAN"_s, u"DEC--TAN"_s }, 150.1, 2.2, 100.5, 120.5,
                                            -0.0005, 0.0005));
    if (name == u"sinrot"_s)
        return SkyWcs::fromCards(cdCards({ u"RA---SIN"_s, u"DEC--SIN"_s }, 150.12, 2.25, 64.5, 64.5,
                                         -p * c, p * s, p * s, p * c));
    if (name == u"galcar"_s)
        return SkyWcs::fromCards(cdeltCards({ u"GLON-CAR"_s, u"GLAT-CAR"_s }, 10.0, 0.0, 50.5, 50.5,
                                            -0.002, 0.002));
    if (name == u"galcar2"_s)
        return SkyWcs::fromCards(cdeltCards({ u"GLON-CAR"_s, u"GLAT-CAR"_s }, 33.0, -4.0, 20.0, 30.0,
                                            -0.01, 0.01));
    if (name == u"arc"_s)
        return SkyWcs::fromCards(cdeltCards({ u"RA---ARC"_s, u"DEC--ARC"_s }, 83.8, -5.4, 300.5, 300.5,
                                            -0.001, 0.001));
    if (name == u"stg"_s)
        return SkyWcs::fromCards(cdeltCards({ u"RA---STG"_s, u"DEC--STG"_s }, 202.5, 47.2, 80.5, 80.5,
                                            -0.004, 0.004));
    if (name == u"galtan"_s)
        return SkyWcs::fromCards(cdeltCards({ u"GLON-TAN"_s, u"GLAT-TAN"_s }, 0.0, 2.5, 40.5, 40.5,
                                            -0.0006, 0.0006));
    return {};
}

double angleBetweenDeg(double a, double b)
{
    return std::abs(std::remainder(a - b, 360.0));
}

}   // namespace

void TestSkyWcs::projectionsAgreeWithAstropy_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<double>("x");
    QTest::addColumn<double>("y");
    QTest::addColumn<double>("lon");
    QTest::addColumn<double>("lat");
    // astropy.wcs.WCS(header).wcs_pix2world([[x, y]], 0): 0-based pixels.
    const auto row = [](const char *name, double x, double y, double lon, double lat) {
        QTest::newRow(qPrintable(QStringLiteral("%1 (%2, %3)").arg(QLatin1String(name)).arg(x).arg(y)))
                << QString::fromLatin1(name) << x << y << lon << lat;
    };
    row("tan", 0.0, 0.0, 150.149784689971, 2.140249214468);
    row("tan", 10.5, 20.25, 150.144531333758, 2.150374363532);
    row("tan", 99.0, 119.0, 150.100250184365, 2.199749999979);
    row("tan", 150.3, -4.5, 150.074582322554, 2.137999814015);
    row("sinrot", 0.0, 0.0, 150.127753362672, 2.221085773812);
    row("sinrot", 10.5, 20.25, 150.128097574036, 2.228681444156);
    row("sinrot", 99.0, 119.0, 150.119001247633, 2.271938136831);
    row("sinrot", 30.0, 100.0, 150.135766159248, 2.254953223989);
    row("galcar", 0.0, 0.0, 10.099, -0.099);
    row("galcar", 99.0, 119.0, 9.901, 0.139);
    row("galcar", 150.3, -4.5, 9.7984, -0.108);
    row("galcar2", 0.0, 0.0, 33.190531392714, -4.289977963042);
    row("galcar2", 99.0, 119.0, 32.198926684815, -3.099609889959);
    row("galcar2", 150.3, -4.5, 31.683256603361, -4.333947604833);
    row("arc", 0.0, 0.0, 84.100986556411, -5.699424603577);
    row("arc", 99.0, 119.0, 84.001454443324, -5.580466459918);
    row("stg", 0.0, 0.0, 202.965240009305, 46.881055959516);
    row("stg", 99.0, 119.0, 202.384856787028, 47.357942320123);
    row("stg", 150.3, -4.5, 202.085811129499, 46.863251948886);
    row("galtan", 0.0, 0.0, 0.023722148860, 2.476299789372);
    row("galtan", 99.0, 119.0, 359.964264694841, 2.547699494105);
    row("galtan", 30.0, 100.0, 0.005705588112, 2.536299982584);
}

void TestSkyWcs::projectionsAgreeWithAstropy()
{
    QFETCH(QString, name);
    QFETCH(double, x);
    QFETCH(double, y);
    QFETCH(double, lon);
    QFETCH(double, lat);
    const SkyWcs w = named(name);
    QVERIFY(w.valid());
    double gotLon = 0.0, gotLat = 0.0;
    QVERIFY(w.pixelToLonLat(x, y, gotLon, gotLat));
    // Well under a milli-arcsecond: far below anything a pixel can show.
    QVERIFY2(angleBetweenDeg(gotLon, lon) * std::max(0.05, std::cos(lat * kDeg)) < 3e-9,
             qPrintable(QStringLiteral("lon %1 vs %2").arg(gotLon, 0, 'f', 12).arg(lon, 0, 'f', 12)));
    QVERIFY2(std::abs(gotLat - lat) < 3e-9,
             qPrintable(QStringLiteral("lat %1 vs %2").arg(gotLat, 0, 'f', 12).arg(lat, 0, 'f', 12)));
}

void TestSkyWcs::skyToPixelInvertsPixelToSky_data()
{
    QTest::addColumn<QString>("name");
    for (const char *n : { "tan", "sinrot", "galcar", "galcar2", "arc", "stg", "galtan" })
        QTest::newRow(n) << QString::fromLatin1(n);
}

void TestSkyWcs::skyToPixelInvertsPixelToSky()
{
    QFETCH(QString, name);
    const SkyWcs w = named(name);
    QVERIFY(w.valid());
    for (const auto &p : { std::array<double, 2>{ 0.0, 0.0 }, { 10.5, 20.25 }, { 99.0, 119.0 },
                           { 150.3, -4.5 }, { 30.0, 100.0 } }) {
        Vec3 sky;
        QVERIFY(w.pixelToSky(p[0], p[1], sky));
        double x = 0.0, y = 0.0;
        QVERIFY(w.skyToPixel(sky, x, y));
        QVERIFY2(std::abs(x - p[0]) < 1e-6 && std::abs(y - p[1]) < 1e-6,
                 qPrintable(QStringLiteral("%1: (%2, %3) came back as (%4, %5)")
                                    .arg(name).arg(p[0]).arg(p[1]).arg(x).arg(y)));
    }
}

void TestSkyWcs::headersWithoutAUsableSolutionAreRefused()
{
    // A simulation in pixel coordinates.
    QVERIFY(!SkyWcs::fromCards({ card(u"CTYPE1"_s, u"X"_s, true), card(u"CTYPE2"_s, u"Y"_s, true) }).valid());
    // No cards at all.
    QVERIFY(!SkyWcs::fromCards({}).valid());
    // A projection this class does not do.
    QVERIFY(!SkyWcs::fromCards(cdeltCards({ u"RA---AIT"_s, u"DEC--AIT"_s }, 10.0, 10.0, 1.0, 1.0, -0.1, 0.1))
                     .valid());
    // A singular matrix.
    QVERIFY(!SkyWcs::fromCards(cdeltCards({ u"RA---TAN"_s, u"DEC--TAN"_s }, 10.0, 10.0, 1.0, 1.0, 0.0, 0.1))
                     .valid());
    // B1950 axes are not J2000: refused rather than half a degree off.
    QStringList fk4 = cdeltCards({ u"RA---TAN"_s, u"DEC--TAN"_s }, 10.0, 10.0, 1.0, 1.0, -0.1, 0.1);
    fk4.push_back(card(u"RADESYS"_s, u"FK4"_s, true));
    QVERIFY(!SkyWcs::fromCards(fk4).valid());
    QStringList b1950 = cdeltCards({ u"RA---TAN"_s, u"DEC--TAN"_s }, 10.0, 10.0, 1.0, 1.0, -0.1, 0.1);
    b1950.push_back(card(u"EQUINOX"_s, u"1950.0"_s));
    QVERIFY(!SkyWcs::fromCards(b1950).valid());
    // A slanted SIN is not the plain one; the zero PV2_1/PV2_2 CASA writes is.
    QStringList slant = cdeltCards({ u"RA---SIN"_s, u"DEC--SIN"_s }, 10.0, 10.0, 1.0, 1.0, -0.1, 0.1);
    slant.push_back(card(u"PV2_1"_s, u"0.0"_s));
    slant.push_back(card(u"PV2_2"_s, u"0.0"_s));
    QVERIFY(SkyWcs::fromCards(slant).valid());
    slant.push_back(card(u"PV2_1"_s, u"0.3"_s));   // the first occurrence of a key wins
    QVERIFY(SkyWcs::fromCards(slant).valid());
    QStringList slanted = cdeltCards({ u"RA---SIN"_s, u"DEC--SIN"_s }, 10.0, 10.0, 1.0, 1.0, -0.1, 0.1);
    slanted.push_back(card(u"PV2_1"_s, u"0.3"_s));
    QVERIFY(!SkyWcs::fromCards(slanted).valid());
    // Longitude and latitude from different systems.
    QVERIFY(!SkyWcs::fromCards(cdeltCards({ u"RA---TAN"_s, u"GLAT-TAN"_s }, 10.0, 10.0, 1.0, 1.0, -0.1, 0.1))
                     .valid());
}

void TestSkyWcs::aPixelBeyondTheSinHorizonHasNoSky()
{
    // 1 degree per pixel: 90 pixels from the centre is the horizon of a SIN map.
    const SkyWcs w = SkyWcs::fromCards(
            cdeltCards({ u"RA---SIN"_s, u"DEC--SIN"_s }, 0.0, 0.0, 1.0, 1.0, -1.0, 1.0));
    Vec3 sky;
    QVERIFY(w.pixelToSky(10.0, 10.0, sky));
    QVERIFY(!w.pixelToSky(120.0, 0.0, sky));
    // The opposite side of the sky has no pixel on a TAN map.
    const SkyWcs t = SkyWcs::fromCards(
            cdeltCards({ u"RA---TAN"_s, u"DEC--TAN"_s }, 0.0, 0.0, 1.0, 1.0, -1.0, 1.0));
    double x = 0.0, y = 0.0;
    QVERIFY(!t.skyToPixel({ -1.0, 0.0, 0.0 }, x, y));
}

void TestSkyWcs::matchViewKeepsCentreAndAngularHeight()
{
    // Two images of one field: 1.8 arcsec/px north-up, 1.2 arcsec/px turned by 30
    // degrees (the headers the astropy numbers came from).
    const SkyWcs a = named(u"tan"_s), b = named(u"sinrot"_s);
    // A view of image a: centred on pixel (80, 90), 200 pixels tall.
    const View va{ 80.0, 90.0, 200.0 };
    View vb;
    QVERIFY(matchView(a, va, b, vb));
    // Same sky at the centre...
    Vec3 skyA, skyB;
    QVERIFY(a.pixelToSky(va.cx, va.cy, skyA));
    QVERIFY(b.pixelToSky(vb.cx, vb.cy, skyB));
    QVERIFY(separationDeg(skyA, skyB) < 1e-9);
    // ...and the same angular height: 200 px x 1.8" = 360" = 300 px x 1.2".
    const double heightA = va.height * a.pixelScaleDeg(va.cx, va.cy);
    const double heightB = vb.height * b.pixelScaleDeg(vb.cx, vb.cy);
    QVERIFY(std::abs(heightA - heightB) < 1e-9);
    QVERIFY2(std::abs(vb.height - 300.0) < 0.05, qPrintable(QString::number(vb.height)));
}

void TestSkyWcs::matchViewAcrossFramesAndProjections()
{
    // A Galactic TAN map and an ICRS STG map whose reference is a star of the
    // first one: the centre of a view must land on that reference pixel.
    const SkyWcs gal = named(u"galtan"_s);
    Vec3 star;
    QVERIFY(gal.pixelToSky(40.0, 41.0, star));
    const double ra = std::fmod(std::atan2(star[1], star[0]) / kDeg + 360.0, 360.0);
    const SkyWcs other = SkyWcs::fromCards(cdeltCards({ u"RA---STG"_s, u"DEC--STG"_s }, ra,
                                                      std::asin(star[2]) / kDeg, 11.0, 7.0, -0.0003,
                                                      0.0003));
    QVERIFY(other.valid());
    View out;
    QVERIFY(matchView(gal, { 40.0, 41.0, 50.0 }, other, out));
    // The reference pixel is 1-based (11, 7): 0-based (10, 6).
    QVERIFY2(std::abs(out.cx - 10.0) < 1e-6 && std::abs(out.cy - 6.0) < 1e-6,
             qPrintable(QStringLiteral("(%1, %2)").arg(out.cx).arg(out.cy)));
    // 0.0006 deg vs 0.0003 deg per pixel: twice as many pixels for the same sky.
    QVERIFY2(std::abs(out.height - 100.0) < 0.01, qPrintable(QString::number(out.height)));
}

void TestSkyWcs::matchViewThereAndBackIsTheIdentity()
{
    const SkyWcs a = named(u"tan"_s), b = named(u"sinrot"_s);
    View there;
    QVERIFY(matchView(a, { 90.0, 100.0, 120.0 }, b, there));
    View back;
    QVERIFY(matchView(b, there, a, back));
    QVERIFY(std::abs(back.cx - 90.0) < 1e-6 && std::abs(back.cy - 100.0) < 1e-6
            && std::abs(back.height - 120.0) < 1e-6);
    // A view with no height is not a view.
    QVERIFY(!matchView(a, { 90.0, 100.0, 0.0 }, b, there));
    // A centre the other image cannot show (the far side of its TAN plane).
    const SkyWcs far = SkyWcs::fromCards(
            cdeltCards({ u"RA---TAN"_s, u"DEC--TAN"_s }, 330.0, 0.0, 10.0, 10.0, -0.1, 0.1));
    QVERIFY(!matchView(a, { 90.0, 100.0, 120.0 }, far, there));
}

void TestSkyWcs::pixelScaleIsArcsecondsPerPixel()
{
    QVERIFY(std::abs(named(u"tan"_s).pixelScaleDeg(100.0, 120.0) * 3600.0 - 1.8) < 1e-4);
    // The rotated one: 1.2 arcsec whichever way the pixel is turned.
    QVERIFY(std::abs(named(u"sinrot"_s).pixelScaleDeg(64.0, 64.0) * 3600.0 - 1.2) < 1e-4);
    // CAR is true to scale along its native equator (the reference row) and
    // shrinks in longitude away from it: at 3 degrees of native latitude (300
    // pixels up) a step is 0.01 degrees in latitude and 0.01 cos(3 deg) in
    // longitude, whose geometric mean is the scale.
    const SkyWcs car = named(u"galcar2"_s);
    QVERIFY(std::abs(car.pixelScaleDeg(20.0, 30.0) - 0.01) < 1e-8);
    QVERIFY(std::abs(car.pixelScaleDeg(20.0, 330.0) - 0.01 * std::sqrt(std::cos(3.0 * kDeg))) < 1e-7);
}

void TestSkyWcs::regionsKeepTheirSizeOnTheSky()
{
    // 1.8 arcsec/px to 1.2 arcsec/px: lengths grow by 1.5.
    const SkyWcs a = named(u"tan"_s), b = named(u"sinrot"_s);
    Region circle;
    circle.shape = Shape::Circle;
    circle.center = { 90.0, 100.0 };
    circle.radius = 20.0;
    const auto c = convertRegion(circle, a, b);
    QVERIFY(c.has_value());
    QVERIFY2(std::abs(c->radius - 30.0) < 0.02, qPrintable(QString::number(c->radius)));
    Vec3 sa, sb;
    QVERIFY(a.pixelToSky(90.0, 100.0, sa));
    QVERIFY(b.pixelToSky(c->center[0], c->center[1], sb));
    QVERIFY(separationDeg(sa, sb) < 1e-9);

    Region ring;
    ring.shape = Shape::Annulus;
    ring.center = { 90.0, 100.0 };
    ring.radii = { 10.0, 20.0 };
    const auto r = convertRegion(ring, a, b);
    QVERIFY(r.has_value());
    QVERIFY(std::abs(r->radii[0] - 15.0) < 0.02 && std::abs(r->radii[1] - 30.0) < 0.02);

    Region ell;
    ell.shape = Shape::Ellipse;
    ell.center = { 90.0, 100.0 };
    ell.semiAxes = { 20.0, 8.0 };
    const auto e = convertRegion(ell, a, b);
    QVERIFY(e.has_value());
    QVERIFY(std::abs(e->semiAxes[0] - 30.0) < 0.02 && std::abs(e->semiAxes[1] - 12.0) < 0.02);
}

void TestSkyWcs::aBoxTurnsWithTheOtherGrid()
{
    // b's pixel axes are those of a turned by 30 degrees (and 1.5 times finer):
    // a box along a's x axis is 30 degrees clockwise in b.
    const SkyWcs a = named(u"tan"_s), b = named(u"sinrot"_s);
    Region box;
    box.shape = Shape::Box;
    box.center = { 90.0, 100.0 };
    box.size = { 40.0, 20.0 };
    box.angleDeg = 0.0;
    const auto out = convertRegion(box, a, b);
    QVERIFY(out.has_value());
    QVERIFY2(std::abs(out->angleDeg - (-30.0)) < 0.05, qPrintable(QString::number(out->angleDeg)));
    QVERIFY(std::abs(out->size[0] - 60.0) < 0.05 && std::abs(out->size[1] - 30.0) < 0.05);
    // A box already at 45 degrees ends at 15.
    box.angleDeg = 45.0;
    const auto out45 = convertRegion(box, a, b);
    QVERIFY(out45.has_value());
    QVERIFY2(std::abs(out45->angleDeg - 15.0) < 0.05, qPrintable(QString::number(out45->angleDeg)));
}

void TestSkyWcs::polygonAndLineRoundTrip()
{
    const SkyWcs a = named(u"tan"_s), b = named(u"sinrot"_s);
    Region poly;
    poly.shape = Shape::Polygon;
    poly.points = { { 70.0, 80.0 }, { 110.0, 82.0 }, { 105.0, 120.0 }, { 72.0, 118.0 } };
    poly.name = u"Polygon 1"_s;
    poly.color = u"#ff8800"_s;
    const auto there = convertRegion(poly, a, b);
    QVERIFY(there.has_value());
    // The vertices went somewhere else, and the descriptive parts stayed.
    QVERIFY(there->points[0] != poly.points[0]);
    QCOMPARE(there->name, poly.name);
    QCOMPARE(there->color, poly.color);
    const auto back = convertRegion(*there, b, a);
    QVERIFY(back.has_value());
    for (std::size_t i = 0; i < poly.points.size(); ++i) {
        QVERIFY(std::abs(back->points[i][0] - poly.points[i][0]) < 1e-6);
        QVERIFY(std::abs(back->points[i][1] - poly.points[i][1]) < 1e-6);
    }
    // A box, there and back.
    Region box;
    box.shape = Shape::Box;
    box.center = { 90.0, 100.0 };
    box.size = { 40.0, 20.0 };
    box.angleDeg = 25.0;
    const auto b2 = convertRegion(box, a, b);
    QVERIFY(b2.has_value());
    const auto b3 = convertRegion(*b2, b, a);
    QVERIFY(b3.has_value());
    QVERIFY(std::abs(b3->center[0] - 90.0) < 1e-6 && std::abs(b3->center[1] - 100.0) < 1e-6);
    QVERIFY(std::abs(b3->size[0] - 40.0) < 1e-3 && std::abs(b3->size[1] - 20.0) < 1e-3);
    QVERIFY(std::abs(b3->angleDeg - 25.0) < 1e-3);
}

void TestSkyWcs::aRegionOffTheProjectionIsNotPlaced()
{
    // 1 degree per pixel SIN: a region 150 pixels from the centre is past the horizon.
    const SkyWcs sin = SkyWcs::fromCards(
            cdeltCards({ u"RA---SIN"_s, u"DEC--SIN"_s }, 0.0, 0.0, 1.0, 1.0, -1.0, 1.0));
    const SkyWcs tan = SkyWcs::fromCards(
            cdeltCards({ u"RA---TAN"_s, u"DEC--TAN"_s }, 0.0, 0.0, 1.0, 1.0, -1.0, 1.0));
    Region p;
    p.shape = Shape::Point;
    p.center = { 150.0, 0.0 };
    QVERIFY(!convertRegion(p, sin, tan).has_value());
    int missed = 0;
    Region nearby = p;
    nearby.center = { 10.0, 10.0 };
    const auto list = convertRegions({ nearby, p }, sin, tan, &missed);
    QCOMPARE(list.size(), std::size_t(1));
    QCOMPARE(missed, 1);
}

void TestSkyWcs::pathConversionFailsAsAWhole()
{
    const SkyWcs a = named(u"tan"_s), b = named(u"sinrot"_s);
    const auto path = convertPath({ { 80.0, 90.0 }, { 100.0, 110.0 }, { 120.0, 95.0 } }, a, b);
    QCOMPARE(path.size(), std::size_t(3));
    const auto back = convertPath({ { path[0][0], path[0][1] }, { path[1][0], path[1][1] },
                                    { path[2][0], path[2][1] } },
                                  b, a);
    QCOMPARE(back.size(), std::size_t(3));
    QVERIFY(std::abs(back[1][0] - 100.0) < 1e-6 && std::abs(back[1][1] - 110.0) < 1e-6);
    // One vertex that cannot be placed loses the path: a PV along a part of
    // the path would be a different cut.
    const SkyWcs sin = SkyWcs::fromCards(
            cdeltCards({ u"RA---SIN"_s, u"DEC--SIN"_s }, 0.0, 0.0, 1.0, 1.0, -1.0, 1.0));
    QVERIFY(convertPath({ { 0.0, 0.0 }, { 150.0, 0.0 } }, sin, sin).empty());
}
