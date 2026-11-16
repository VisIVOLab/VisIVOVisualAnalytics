#pragma once
// TestLinkedCameraSync.h
// QTest class for LinkedCameraSync: when a linked cube window sends its 3-D
// camera to its peers (issue #338). Header-only pure logic; no GUI/VTK link.

#include <QObject>

class TestLinkedCameraSync : public QObject
{
    Q_OBJECT

private slots:
    void firstFrame_sendsNothing();
    void unchangedCamera_sendsNothing();
    void everyPoseField_countsAsAMove();
    void burstOfFrames_joinsOneSend();
    void frameWhileApplyingPeer_isRecordedNotSent();
    void adoptedPose_isNotSentBack();
    void adopt_dropsAWaitingSend();
    void reset_makesTheNextFrameAFirst();
    void twoWindows_doNotPingPong();
    void threeWindows_oneSendReachesBoth();
    void relinkWhileASendWaits_sendsOnce();
    void nonFinitePose_isNeitherSentNorRecorded();
};
