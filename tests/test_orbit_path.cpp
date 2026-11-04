#include "TestOrbitPath.h"

#include "gui/OrbitPath.h"

#include <QJsonArray>
#include <QtTest>

#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace {

QJsonObject camera()
{
    return { { u"position"_s, QJsonArray{ 1.0, 2.0, 13.0 } },
             { u"focal_point"_s, QJsonArray{ 1.0, 2.0, 3.0 } },
             { u"view_up"_s, QJsonArray{ 0.0, 2.0, 0.0 } },   // not unit: normalised
             { u"view_angle"_s, 35.0 } };
}

double at(const QJsonObject &cam, int axis)
{
    return cam.value(u"position"_s).toArray().at(axis).toDouble();
}

} // namespace

void TestOrbitPath::aTurnCirclesTheFocalPointAtItsDistance()
{
    const auto cams = visivo::orbitCameras(camera(), 4, 360.0);
    QCOMPARE(cams.size(), 4);
    // A quarter turn per frame about +y: (0, 0, 10) → (10, 0, 0) → (0, 0, -10) → (-10, 0, 0),
    // the fourth short of the start so a loop does not repeat it.
    const double expect[4][2] = { { 0, 10 }, { 10, 0 }, { 0, -10 }, { -10, 0 } };
    for (int i = 0; i < 4; ++i) {
        QVERIFY(std::abs(at(cams[i], 0) - 1.0 - expect[i][0]) < 1e-9);
        QVERIFY(std::abs(at(cams[i], 1) - 2.0) < 1e-9);    // the height is kept
        QVERIFY(std::abs(at(cams[i], 2) - 3.0 - expect[i][1]) < 1e-9);
    }
}

void TestOrbitPath::anArcEndsOnItsAngle()
{
    const auto cams = visivo::orbitCameras(camera(), 3, 90.0);
    QCOMPARE(cams.size(), 3);
    QVERIFY(std::abs(at(cams[2], 0) - 11.0) < 1e-9);   // 90° about +y: on +x
    QVERIFY(std::abs(at(cams[2], 2) - 3.0) < 1e-9);
    QVERIFY(visivo::orbitCameras(camera(), 0, 90.0).isEmpty());
}

void TestOrbitPath::theOtherFieldsAreKept()
{
    const auto cams = visivo::orbitCameras(camera(), 2, 180.0);
    QCOMPARE(cams[1].value(u"view_angle"_s).toDouble(), 35.0);
    QCOMPARE(cams[1].value(u"focal_point"_s).toArray(), camera().value(u"focal_point"_s).toArray());
}

void TestOrbitPath::turningKeepsTheDistanceAndTiltsTheUp()
{
    // Looking down -z from (1, 2, 13) at (1, 2, 3), up +y.
    const QJsonObject turned = visivo::turnCamera(camera(), 90.0, 0.0);
    QVERIFY(std::abs(at(turned, 0) - 11.0) < 1e-9 && std::abs(at(turned, 2) - 3.0) < 1e-9);
    // 90° of elevation: above the focal point, the view up now along -z.
    const QJsonObject above = visivo::turnCamera(camera(), 0.0, 90.0);
    QVERIFY(std::abs(at(above, 1) - 12.0) < 1e-9 && std::abs(at(above, 2) - 3.0) < 1e-9);
    const QJsonArray up = above.value(u"view_up"_s).toArray();
    QVERIFY(std::abs(up.at(2).toDouble() + 1.0) < 1e-9);
}

void TestOrbitPath::dollyAndPan()
{
    const QJsonObject closer = visivo::dollyCamera(camera(), 2.0);
    QVERIFY(std::abs(at(closer, 2) - 8.0) < 1e-9);   // half the distance
    const QJsonObject panned = visivo::panCamera(camera(), 0.5, 0.0);
    // Half the height shown at the focal point (2·10·tan 17.5°), the camera to -x.
    const double h = 2.0 * 10.0 * std::tan(35.0 * M_PI / 360.0);
    QVERIFY(std::abs(at(panned, 0) - (1.0 - 0.5 * h)) < 1e-9);
    const QJsonArray fp = panned.value(u"focal_point"_s).toArray();
    QVERIFY(std::abs(fp.at(0).toDouble() - (1.0 - 0.5 * h)) < 1e-9);
}
