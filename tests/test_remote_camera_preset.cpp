// test_remote_camera_preset.cpp
// See TestRemoteCameraPreset.h.

#include "TestRemoteCameraPreset.h"

#include "RemoteCameraPreset.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

void TestRemoteCameraPreset::thePresetIsNamedWithItsAngles()
{
    // Camera > Top is Elevation(90) on the local camera, and the same angles
    // go to the backend, as numbers in a JSON object it reads by these keys.
    const QJsonObject top =
            QJsonObject::fromVariantMap(visivo::remote::cameraPresetCommand(0.0, 90.0));
    QCOMPARE(top.value(QStringLiteral("type")).toString(), QStringLiteral("camera_preset"));
    QVERIFY(top.value(QStringLiteral("azimuth")).isDouble());
    QCOMPARE(top.value(QStringLiteral("azimuth")).toDouble(), 0.0);
    QCOMPARE(top.value(QStringLiteral("elevation")).toDouble(), 90.0);
    QCOMPARE(top.size(), 3);

    const QByteArray back = QJsonDocument(QJsonObject::fromVariantMap(
                                                  visivo::remote::cameraPresetCommand(-180.0, 0.0)))
                                    .toJson(QJsonDocument::Compact);
    QCOMPARE(QJsonDocument::fromJson(back).object().value(QStringLiteral("azimuth")).toDouble(),
             -180.0);
}

void TestRemoteCameraPreset::aLostSessionsCameraGoesToItsReplacement()
{
    // Issue #411: GET /v1/render/{id}/camera answered this before the session
    // was lost; its replacement is turned to it with set_camera, whose keys the
    // backend reads (render_session/_common.py, set_camera).
    const QJsonObject camera = QJsonDocument::fromJson(R"({
        "position": [10.5, -3, 400], "focal_point": [64, 64, 32], "view_up": [0, 1, 0],
        "view_angle": 30, "parallel": false, "parallel_scale": 55.2})")
                                       .object();
    const QJsonObject cmd =
            QJsonObject::fromVariantMap(visivo::remote::setCameraCommand(camera));
    QCOMPARE(cmd.value(QStringLiteral("type")).toString(), QStringLiteral("set_camera"));
    QCOMPARE(cmd.value(QStringLiteral("position")).toArray(),
             camera.value(QStringLiteral("position")).toArray());
    QCOMPARE(cmd.value(QStringLiteral("focal_point")).toArray().at(2).toDouble(), 32.0);
    QCOMPARE(cmd.value(QStringLiteral("view_up")).toArray().at(1).toDouble(), 1.0);
    QCOMPARE(cmd.value(QStringLiteral("view_angle")).toDouble(), 30.0);
    QCOMPARE(cmd.value(QStringLiteral("parallel")).toBool(), false);
    QCOMPARE(cmd.value(QStringLiteral("parallel_scale")).toDouble(), 55.2);

    // Without the three vectors there is no camera to give: nothing is sent.
    QVERIFY(visivo::remote::setCameraCommand(QJsonObject()).isEmpty());
    QJsonObject shortUp = camera;
    shortUp.insert(QStringLiteral("view_up"), QJsonArray{ 0, 1 });
    QVERIFY(visivo::remote::setCameraCommand(shortUp).isEmpty());
    QJsonObject text = camera;
    text.insert(QStringLiteral("position"), QJsonArray{ 1, QStringLiteral("x"), 3 });
    QVERIFY(visivo::remote::setCameraCommand(text).isEmpty());
    // The optional values only when they are what the backend takes.
    QJsonObject bare = camera;
    bare.remove(QStringLiteral("view_angle"));
    bare.insert(QStringLiteral("parallel"), QStringLiteral("no"));
    const QVariantMap bareCmd = visivo::remote::setCameraCommand(bare);
    QVERIFY(!bareCmd.isEmpty());
    QVERIFY(!bareCmd.contains(QStringLiteral("view_angle")));
    QVERIFY(!bareCmd.contains(QStringLiteral("parallel")));
}
