#pragma once
// TestLinkedCameraSync.h
// QTest class for LinkedCameraSync: when a linked cube window sends its 3-D
// camera to its peers (issue #338), the same for the 2-D views (issue #358),
// and the image viewer's own camera watch. Header-only pure logic; no GUI/VTK
// link.

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
    // Issue #358: the 2-D views (slice, Product panes).
    void twoD_resetInCode_isSent();
    void twoD_refitInCode_isAdoptedNotSent();
    void twoD_paneAndSliceShareAView_withoutEcho();
    void twoD_refit_keepsTheUsersWaitingSend();
    void twoD_refit_ofANonFinitePose_recordsNothing();
    void twoD_lastFrameMoved_coversEveryFrameOfTheMove();
    // Issue #358: the image viewer's tile streaming follows its own camera.
    void watch_firstFrame_isNotAMove();
    void watch_everyChange_isAMove();
    void watch_adoptedPose_isNotAMove();
    void watch_nonFinitePose_isIgnored();
};
