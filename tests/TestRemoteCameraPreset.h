#pragma once
// TestRemoteCameraPreset.h
// QTest class for the Camera menu's presets in a Remote 3-D view (issue
// #390): the command the backend turns its own camera with. No window.

#include <QObject>

class TestRemoteCameraPreset : public QObject
{
    Q_OBJECT

private slots:
    void thePresetIsNamedWithItsAngles();
    void aLostSessionsCameraGoesToItsReplacement();
};
