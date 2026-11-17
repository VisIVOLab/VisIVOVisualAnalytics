// test_remote_camera_preset.cpp
// See TestRemoteCameraPreset.h.

#include "TestRemoteCameraPreset.h"

#include "RemoteCameraPreset.h"

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
