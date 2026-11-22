#pragma once
// TestRemoteCameraPreset.h
// QTest class for the Camera menu's presets in a Remote 3-D view (issue
// #390): the command the backend turns its own camera with, and the point
// style the size slider sends it (issue #456). No window.

#include <QObject>

class TestRemoteCameraPreset : public QObject
{
    Q_OBJECT

private slots:
    void thePresetIsNamedWithItsAngles();
    void aLostSessionsCameraGoesToItsReplacement();
    void thePointStyleFollowsTheSizeSlider();
};
