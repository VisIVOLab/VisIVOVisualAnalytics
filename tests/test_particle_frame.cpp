#include "TestParticleFrame.h"

#include "gui/ParticleFrame.h"

#include <QJsonArray>
#include <QtTest>

using namespace Qt::Literals::StringLiterals;
using visivo::ParticleFrame;

namespace {

QJsonObject farCamera()
{
    return { { u"position"_s, QJsonArray{ 1e9 + 1.0, 0.0, -3e9 + 1.0 } },
             { u"focal_point"_s, QJsonArray{ 1e9 + 1.0, 0.0, -3e9 } },
             { u"view_up"_s, QJsonArray{ 0.0, 1.0, 0.0 } },
             { u"view_angle"_s, 30.0 },
             { u"parallel"_s, true },
             { u"parallel_scale"_s, 4.0 } };
}

float axis(const QJsonObject &cam, const QString &key, int i)
{
    return float(cam.value(key).toArray().at(i).toDouble());
}

} // namespace

void TestParticleFrame::theHeadersGiveTheFrame()
{
    const ParticleFrame f = ParticleFrame::fromHeaders("1000000000.0,-2.5,-3000000000.0", "2.0");
    QCOMPARE(f.origin[0], 1e9);
    QCOMPARE(f.origin[1], -2.5);
    QCOMPARE(f.origin[2], -3e9);
    QCOMPARE(f.scale, 2.0);
    QVERIFY(!f.isIdentity());
    // An origin without a scale: scale 1.
    QCOMPARE(ParticleFrame::fromHeaders("1.0,2.0,3.0", "").scale, 1.0);
}

void TestParticleFrame::aMissingOrBadFrameIsTheIdentity()
{
    // A backend that predates the frame sends neither header: its rows are in
    // the table's coordinates.
    QVERIFY(ParticleFrame::fromHeaders("", "").isIdentity());
    QVERIFY(ParticleFrame::fromHeaders("1.0,2.0", "1.0").isIdentity());
    QVERIFY(ParticleFrame::fromHeaders("1.0,nan,3.0", "1.0").isIdentity());
    QVERIFY(ParticleFrame::fromHeaders("1.0,x,3.0", "1.0").isIdentity());
    QVERIFY(ParticleFrame::fromHeaders("1.0,2.0,3.0", "0").isIdentity());
    QVERIFY(ParticleFrame::fromHeaders("1.0,2.0,3.0", "-1").isIdentity());
    QVERIFY(ParticleFrame::fromHeaders("1.0,2.0,3.0", "inf").isIdentity());
    const QJsonObject cam = farCamera();
    QCOMPARE(ParticleFrame().localCamera(cam), cam);
}

void TestParticleFrame::particlesFarFromZeroStayApartInTheTable()
{
    // 1e9, 1e9 + 1 and 1e9 + 2 are one float; as offsets they are three, and
    // the frame puts them back where the table has them.
    QCOMPARE(float(1e9), float(1e9 + 1.0));
    const ParticleFrame f = ParticleFrame::fromHeaders("1000000000.0,0.0,-3000000000.0", "1.0");
    const float rows[3][3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f } };
    for (int i = 0; i < 3; ++i) {
        const auto t = f.toTable(rows[i]);
        QCOMPARE(t[0], 1e9 + i);
        QCOMPARE(t[2], -3e9);
    }
    const ParticleFrame half = ParticleFrame::fromHeaders("10.0,0.0,0.0", "2.0");
    QCOMPARE(half.toTable(rows[1])[0], 12.0);
}

void TestParticleFrame::theCameraIsMovedIntoTheFrameInDouble()
{
    // In float the camera 1 from its focal point, 3e9 from 0, is on it.
    const QJsonObject table = farCamera();
    QCOMPARE(axis(table, u"position"_s, 2), axis(table, u"focal_point"_s, 2));
    const ParticleFrame f = ParticleFrame::fromHeaders("1000000000.0,0.0,-3000000000.0", "1.0");
    const QJsonObject cam = f.localCamera(table);
    QCOMPARE(axis(cam, u"position"_s, 0), 1.0f);
    QCOMPARE(axis(cam, u"position"_s, 2), 1.0f);
    QCOMPARE(axis(cam, u"focal_point"_s, 2), 0.0f);
    QCOMPARE(cam.value(u"view_up"_s), table.value(u"view_up"_s));
    QCOMPARE(cam.value(u"view_angle"_s).toDouble(), 30.0);
    QCOMPARE(cam.value(u"parallel_scale"_s).toDouble(), 4.0);
    // One scale on every axis: the parallel scale follows it.
    const QJsonObject scaled =
            ParticleFrame::fromHeaders("1000000000.0,0.0,-3000000000.0", "2.0").localCamera(table);
    QCOMPARE(axis(scaled, u"position"_s, 2), 0.5f);
    QCOMPARE(scaled.value(u"parallel_scale"_s).toDouble(), 2.0);
}

void TestParticleFrame::aBoxFarFromZeroIsCentred()
{
    const std::array<double, 6> box{ 1e9, 1e9 + 2.0, 0.0, 1.0, -3e9 - 1.0, -3e9 };
    const ParticleFrame f = ParticleFrame::centredOn(box);
    QCOMPARE(f.origin[0], 1e9 + 1.0);
    QCOMPARE(f.scale, 1.0);
    const auto local = f.toLocal(box);
    const std::array<double, 6> expected{ -1.0, 1.0, -0.5, 0.5, -0.5, 0.5 };
    for (size_t i = 0; i < 6; ++i)
        QCOMPARE(local[i], expected[i]);
}
