// test_remote_camera_preset.cpp
// See TestRemoteCameraPreset.h.

#include "TestRemoteCameraPreset.h"

#include "RemoteCameraPreset.h"
#include "RemotePointStyle.h"

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

void TestRemoteCameraPreset::thePointStyleFollowsTheSizeSlider()
{
    using visivo::remote::pointStyleCommand;
    // The splat default (25 on the slider) is 0.75 of the mean spacing, the
    // slider's own default 3, and every slider step in between is a size of
    // its own: a drag must not reach the backend as the same command.
    const QVariantMap splats = pointStyleCommand(true, 25, 2.0);
    QCOMPARE(splats.value(QStringLiteral("type")).toString(), QStringLiteral("set_point_style"));
    QCOMPARE(splats.value(QStringLiteral("mode")).toString(), QStringLiteral("gaussian"));
    QCOMPARE(splats.value(QStringLiteral("size")).toDouble(), 0.75);
    QCOMPARE(splats.value(QStringLiteral("intensity")).toInt(), 35);
    QCOMPARE(pointStyleCommand(true, 100, 2.0).value(QStringLiteral("size")).toDouble(), 3.0);
    // Splats end where the backend's point session clamps them, and every
    // step of the whole slider in between is a larger size: the right fifth
    // of the slider and its first steps used to be flat (issue #456).
    using namespace ParticleSplatDefaults;
    QCOMPARE(splatSizeForSlider(kSliderMinimum), 0.25);
    QCOMPARE(splatSizeForSlider(kSliderMaximum), 12.0);
    for (int value = kSliderMinimum; value < kSliderMaximum; ++value) {
        QVERIFY2(splatSizeForSlider(value + 1) > splatSizeForSlider(value),
                 qPrintable(QStringLiteral("splats flat at %1").arg(value)));
        QVERIFY2(pointSizeForSlider(value + 1) > pointSizeForSlider(value),
                 qPrintable(QStringLiteral("points flat at %1").arg(value)));
        QVERIFY(pointStyleCommand(true, value, 1.0) != pointStyleCommand(true, value + 1, 1.0));
        QVERIFY(pointStyleCommand(false, value + 1, 2.0).value(QStringLiteral("size")).toInt()
                >= pointStyleCommand(false, value, 2.0).value(QStringLiteral("size")).toInt());
    }
    // Splats are sized in the data, not on screen: the pixel ratio is not
    // applied to them.
    QCOMPARE(pointStyleCommand(true, 60, 1.0), pointStyleCommand(true, 60, 2.0));
    // The local splat is sized from the same step.
    QCOMPARE(ParticleSplatDefaults::splatRadiusForSlider(8.0, 100), 3.0);

    // Points go in the backend's physical pixels.
    const QVariantMap points = pointStyleCommand(false, 200, 2.0);
    QCOMPARE(points.value(QStringLiteral("mode")).toString(), QStringLiteral("points"));
    QCOMPARE(points.value(QStringLiteral("size")).toInt(), 16);
    QCOMPARE(pointStyleCommand(false, 200, 1.0).value(QStringLiteral("size")).toInt(), 8);
    // Where points were not clamped, they keep the size they had, so a view
    // opened at the slider's default does not change.
    for (int value = 75; value <= 300; ++value)
        QCOMPARE(pointSizeForSlider(value), 4.0 * value / 100.0);
    QCOMPARE(pointSizeForSlider(kDefaultSliderValue), 4.0);
    // The backend's points start at one physical pixel.
    QCOMPARE(pointStyleCommand(false, kSliderMinimum, 1.0).value(QStringLiteral("size")).toInt(),
             1);
    QCOMPARE(pointStyleCommand(false, kSliderMaximum, 2.0).value(QStringLiteral("size")).toInt(),
             32);
    // The splat default keeps its size too.
    QCOMPARE(splatSizeForSlider(initialSliderValue()), kInitialSize);
}

void TestRemoteCameraPreset::aTableWithoutLocalRowsStillGetsTheNewSize()
{
    // A table shown only by the backend has no local rows: the slider used to
    // return before the Remote view was told (issue #456).
    int local = 0;
    int server = 0;
    visivo::remote::SizeSliderTargets targets;
    targets.restyleLocal = [&local]() { ++local; };
    targets.updateServerViews = [&server]() { ++server; };
    visivo::remote::applySizeSlider(false, targets);
    QCOMPARE(local, 0);
    QCOMPARE(server, 1);
    visivo::remote::applySizeSlider(true, targets);
    QCOMPARE(local, 1);
    QCOMPARE(server, 2);
    // A window that has no server view leaves the local one restyled.
    targets.updateServerViews = nullptr;
    visivo::remote::applySizeSlider(true, targets);
    QCOMPARE(local, 2);
}
