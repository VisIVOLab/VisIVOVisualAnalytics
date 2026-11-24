// test_astro_match.cpp
// See TestAstroMatch.h.

#include "TestAstroMatch.h"

#include "AstroUtils.h"
#include "LinkedViewFrame.h"

#include <QString>
#include <QTest>

#include <cmath>
#include <string>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

constexpr double kPi = 3.14159265358979323846;

// One 80-column card.
std::string card(const char *key, const QString &value, bool quoted = false)
{
    const QString v = quoted ? QStringLiteral("'%1'").arg(value, -8) : value.rightJustified(20);
    return QStringLiteral("%1= %2").arg(QLatin1String(key), -8).arg(v).leftJustified(80).toStdString();
}

QString num(double v)
{
    return QString::number(v, 'g', 15);
}

std::vector<std::string> header(const char *ctype1, const char *ctype2, double crval1, double crval2,
                                double crpix1, double crpix2, bool fk5, const double *cd,
                                double cdelt1, double cdelt2)
{
    std::vector<std::string> c{
        card("SIMPLE", u"T"_s), card("BITPIX", u"-32"_s), card("NAXIS", u"2"_s),
        card("NAXIS1", u"200"_s), card("NAXIS2", u"200"_s),
        card("CTYPE1", QLatin1String(ctype1), true), card("CTYPE2", QLatin1String(ctype2), true),
        card("CRVAL1", num(crval1)), card("CRVAL2", num(crval2)),
        card("CRPIX1", num(crpix1)), card("CRPIX2", num(crpix2))
    };
    if (cd) {
        c.push_back(card("CD1_1", num(cd[0])));
        c.push_back(card("CD1_2", num(cd[1])));
        c.push_back(card("CD2_1", num(cd[2])));
        c.push_back(card("CD2_2", num(cd[3])));
    } else {
        c.push_back(card("CDELT1", num(cdelt1)));
        c.push_back(card("CDELT2", num(cdelt2)));
    }
    if (fk5) {
        c.push_back(card("RADESYS", u"FK5"_s, true));
        c.push_back(card("EQUINOX", u"2000.0"_s));
    }
    return c;
}

// The grids the astropy numbers were made from: 1.8 arcsec/px north-up TAN; 1.2
// arcsec/px SIN turned by 30 degrees; a Galactic TAN of 2.16 arcsec/px; an FK5
// STG of 1.08 arcsec/px centred on a star of that Galactic map.
std::vector<std::string> named(const QString &name)
{
    const double p = 1.2 / 3600.0, c = std::cos(30.0 * kPi / 180.0), s = std::sin(30.0 * kPi / 180.0);
    const double cd[4] = { -p * c, p * s, p * s, p * c };
    if (name == u"tan"_s)
        return header("RA---TAN", "DEC--TAN", 150.1, 2.2, 100.5, 120.5, true, nullptr, -0.0005, 0.0005);
    if (name == u"sinrot"_s)
        return header("RA---SIN", "DEC--SIN", 150.12, 2.25, 64.5, 64.5, true, cd, 0, 0);
    if (name == u"galtan"_s)
        return header("GLON-TAN", "GLAT-TAN", 0.0, 2.5, 40.5, 40.5, false, nullptr, -0.0006, 0.0006);
    if (name == u"stg"_s)
        return header("RA---STG", "DEC--STG", 263.99589337289785, -27.61197071915682, 11.0, 7.0, true,
                      nullptr, -0.0003, 0.0003);
    // An ALMA continuum image (1.0278e-6 deg/px, shown at a reduced level, its
    // world the full-resolution pixels) and a cube of the same field (8.333e-7
    // deg/px), their reference points on the same sky position.
    if (name == u"continuum"_s)
        return header("RA---SIN", "DEC--SIN", 150.1, 2.2, 5184.5, 5184.5, true, nullptr, -1.0278e-6,
                      1.0278e-6);
    if (name == u"cube"_s)
        return header("RA---SIN", "DEC--SIN", 150.1, 2.2, 750.5, 750.5, true, nullptr, -8.333e-7,
                      8.333e-7);
    if (name == u"far"_s)   // the far side of the sky from "tan"
        return header("RA---TAN", "DEC--TAN", 330.0, 0.0, 10.0, 10.0, true, nullptr, -0.1, 0.1);
    if (name == u"linear"_s)
        return { card("SIMPLE", u"T"_s), card("BITPIX", u"-32"_s), card("NAXIS", u"2"_s),
                 card("NAXIS1", u"20"_s), card("NAXIS2", u"20"_s) };
    return {};
}

}   // namespace

void TestAstroMatch::pixelToPixelAgreesWithAstropy_data()
{
    QTest::addColumn<QString>("from");
    QTest::addColumn<QString>("to");
    QTest::addColumn<double>("x");
    QTest::addColumn<double>("y");
    QTest::addColumn<double>("tx");
    QTest::addColumn<double>("ty");
    // astropy: b.world_to_pixel(a.pixel_to_world(x, y)), 0-based pixels. Several
    // land off the 200 x 200 image of the other grid: a view panned past an
    // edge is still placed.
    const auto row = [](const char *a, const char *b, double x, double y, double tx, double ty) {
        QTest::newRow(qPrintable(QStringLiteral("%1 to %2 (%3, %4)")
                                         .arg(QLatin1String(a), QLatin1String(b)).arg(x).arg(y)))
                << QString::fromLatin1(a) << QString::fromLatin1(b) << x << y << tx << ty;
    };
    row("tan", "sinrot", 80.0, 90.0, -7.032478534813748, -120.07857046872775);
    row("tan", "sinrot", 0.0, 0.0, -178.45453116468667, -176.99407509926925);
    row("tan", "sinrot", 150.3, -4.5, 13.417320122764686, -295.56208293558007);
    row("tan", "sinrot", 99.0, 119.0, 39.39892281193433, -96.65585792997447);
    row("galtan", "stg", 40.0, 41.0, 10.0, 5.999999999988305);
    row("galtan", "stg", 10.0, 12.0, -71.1691429369903, 25.379619604776583);
    row("galtan", "stg", 70.5, 3.25, -20.832191364423252, -86.03599752877969);
    row("stg", "galtan", 10.0, 6.0, 40.000000000080426, 41.00000000003784);
    row("stg", "galtan", 25.0, -8.0, 49.93539563675307, 43.556934568962845);
}

void TestAstroMatch::pixelToPixelAgreesWithAstropy()
{
    QFETCH(QString, from);
    QFETCH(QString, to);
    QFETCH(double, x);
    QFETCH(double, y);
    QFETCH(double, tx);
    QFETCH(double, ty);
    AstroUtils a(named(from)), b(named(to));
    QVERIFY(a.hasSky() && b.hasSky());
    double gx = 0.0, gy = 0.0;
    QVERIFY(a.pixelToPixel(b, x, y, gx, gy));
    // A hundredth of a pixel (a 10 mas): what libwcs and astropy can differ by
    // in the Galactic-FK5 rotation, far below what a view can show.
    QVERIFY2(std::abs(gx - tx) < 1e-2 && std::abs(gy - ty) < 1e-2,
             qPrintable(QStringLiteral("(%1, %2) vs (%3, %4)")
                                .arg(gx, 0, 'f', 6).arg(gy, 0, 'f', 6).arg(tx, 0, 'f', 6).arg(ty, 0, 'f', 6)));
}

void TestAstroMatch::matchViewKeepsCentreAndAngularHeight_data()
{
    QTest::addColumn<QString>("from");
    QTest::addColumn<QString>("to");
    QTest::addColumn<double>("x");
    QTest::addColumn<double>("y");
    QTest::addColumn<double>("scale");
    // The local pixel-scale ratio from astropy's numerical Jacobian (sqrt|det|):
    // 1.8" / 1.2", 2.16" / 1.08" and the inverse, to the digits that matter.
    QTest::newRow("tan to sinrot") << u"tan"_s << u"sinrot"_s << 80.0 << 90.0 << 1.499999401926722;
    QTest::newRow("galtan to stg") << u"galtan"_s << u"stg"_s << 40.0 << 41.0 << 1.9999999995393007;
    QTest::newRow("stg to galtan") << u"stg"_s << u"galtan"_s << 10.0 << 6.0 << 0.5000000001945532;
}

void TestAstroMatch::matchViewKeepsCentreAndAngularHeight()
{
    QFETCH(QString, from);
    QFETCH(QString, to);
    QFETCH(double, x);
    QFETCH(double, y);
    QFETCH(double, scale);
    AstroUtils a(named(from)), b(named(to));
    AstroUtils::View out;
    QVERIFY(a.matchView(b, { x, y, 200.0 }, out));
    double cx = 0.0, cy = 0.0;
    QVERIFY(a.pixelToPixel(b, x, y, cx, cy));
    QVERIFY(std::abs(out.cx - cx) < 1e-9 && std::abs(out.cy - cy) < 1e-9);
    QVERIFY2(std::abs(out.height - 200.0 * scale) < 0.05, qPrintable(QString::number(out.height)));
}

void TestAstroMatch::matchViewThereAndBackIsTheIdentity()
{
    AstroUtils a(named(u"tan"_s)), b(named(u"sinrot"_s));
    AstroUtils::View there, back;
    QVERIFY(a.matchView(b, { 90.0, 100.0, 120.0 }, there));
    QVERIFY(b.matchView(a, there, back));
    QVERIFY(std::abs(back.cx - 90.0) < 1e-6 && std::abs(back.cy - 100.0) < 1e-6
            && std::abs(back.height - 120.0) < 1e-4);
    // A view with no height is not a view.
    QVERIFY(!a.matchView(b, { 90.0, 100.0, 0.0 }, there));
}

void TestAstroMatch::aCentreOffTheProjectionIsRefused()
{
    // The far side of the TAN plane has no pixel on it.
    AstroUtils a(named(u"tan"_s)), far(named(u"far"_s));
    AstroUtils::View out;
    QVERIFY(!a.matchView(far, { 90.0, 100.0, 120.0 }, out));
    double x = 0.0, y = 0.0;
    QVERIFY(!a.pixelToPixel(far, 90.0, 100.0, x, y));
}

void TestAstroMatch::imagesWithoutSkyAreRefused()
{
    AstroUtils a(named(u"tan"_s)), plain(named(u"linear"_s));
    QVERIFY(!plain.hasSky());
    double x = 0.0, y = 0.0;
    QVERIFY(!a.pixelToPixel(plain, 10.0, 10.0, x, y));
    QVERIFY(!plain.pixelToPixel(a, 10.0, 10.0, x, y));
}

namespace {

// The cube's 2-D slice at preview stride 2 (1500 px, 750 samples): its world is
// the cube's pixels at any stride, so a fitted view is 1500 world units tall.
// The window frames it in a 900 x 700 viewport with a gutter, so the data area
// is a fraction of the viewport (0.8 here, as measured in the app).
const visivo::linkview::Frame kCubeFrame{ 900.0, 700.0, 40.0, 70.0, 640.0, 560.0 };

double arcsec(double pixels, double cdelt)
{
    return pixels * cdelt * 3600.0;
}

}   // namespace

void TestAstroMatch::aCubeViewKeepsItsAngularHeightOnAnImage()
{
    AstroUtils cube(named(u"cube"_s)), image(named(u"continuum"_s));
    // The cube zoomed on a feature off its centre: the data area 300 pixels tall.
    const visivo::linkview::Normalized shared{ 820.0, 700.0, 300.0 };
    const AstroUtils::View viewport = visivo::linkview::viewportOf(kCubeFrame, shared);
    // The viewport is what the cube shows: 300 * 700 / 560 pixels tall.
    QVERIFY(std::abs(viewport.height - 300.0 * 700.0 / 560.0) < 1e-9);
    AstroUtils::View onImage;
    QVERIFY(cube.matchView(image, viewport, onImage));
    // An image window shows the whole viewport it is given: the angular height
    // of the cube's viewport, not of its data area.
    const double cubeSky = arcsec(viewport.height, 8.333e-7);
    const double imageSky = arcsec(onImage.height, 1.0278e-6);
    QVERIFY2(std::abs(imageSky / cubeSky - 1.0) < 0.01, qPrintable(QString::number(imageSky / cubeSky)));
    // Handing the data area's height over as it was (the bug) is 20% short.
    AstroUtils::View naive;
    QVERIFY(cube.matchView(image, { shared.cx, shared.cy, shared.height }, naive));
    QVERIFY(std::abs(arcsec(naive.height, 1.0278e-6) / cubeSky - 1.0) > 0.1);
}

void TestAstroMatch::anImageViewKeepsItsAngularHeightOnACube()
{
    AstroUtils cube(named(u"cube"_s)), image(named(u"continuum"_s));
    // An image window zoomed on a feature: its viewport 2000 full-resolution pixels tall.
    const AstroUtils::View shown{ 5300.0, 5000.0, 2000.0 };
    AstroUtils::View onCube;
    QVERIFY(image.matchView(cube, shown, onCube));
    const auto shared = visivo::linkview::normalizedOf(kCubeFrame, onCube);
    // The data area is 0.8 of the viewport: the shared height is 0.8 of the viewport's.
    QVERIFY(std::abs(shared.height / onCube.height - 560.0 / 700.0) < 1e-9);
    // What the cube window then shows is the viewport of that shared view.
    const AstroUtils::View cubeViewport = visivo::linkview::viewportOf(kCubeFrame, shared);
    const double imageSky = arcsec(shown.height, 1.0278e-6);
    const double cubeSky = arcsec(cubeViewport.height, 8.333e-7);
    QVERIFY2(std::abs(cubeSky / imageSky - 1.0) < 0.01, qPrintable(QString::number(cubeSky / imageSky)));
    // and its centre is the image's, on the sky.
    double cx = 0.0, cy = 0.0;
    QVERIFY(image.pixelToPixel(cube, shown.cx, shown.cy, cx, cy));
    QVERIFY(std::abs(cubeViewport.cx - cx) < 1e-6 && std::abs(cubeViewport.cy - cy) < 1e-6);
    // There and back is the identity.
    AstroUtils::View back;
    QVERIFY(cube.matchView(image, cubeViewport, back));
    QVERIFY(std::abs(back.cx - shown.cx) < 1e-3 && std::abs(back.cy - shown.cy) < 1e-3
            && std::abs(back.height / shown.height - 1.0) < 1e-6);
}
