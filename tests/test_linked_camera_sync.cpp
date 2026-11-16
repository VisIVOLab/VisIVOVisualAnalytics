// test_linked_camera_sync.cpp
// Unit tests for LinkedCameraSync (issue #338): a linked cube window sends its
// camera when a drawn frame shows it moved, whatever moved it, and never sends
// back a camera it adopted from a peer.

#include "TestLinkedCameraSync.h"
#include "LinkedCameraSync.h"

#include <QList>
#include <QTest>

#include <limits>

using LinkedCameraSync::Pose;
using LinkedCameraSync::Tracker;

namespace {
Pose frontView()
{
    Pose p;
    p.position = { 0.0, 0.0, 10.0 };
    p.focalPoint = { 0.0, 0.0, 0.0 };
    p.viewUp = { 0.0, 1.0, 0.0 };
    p.viewAngle = 30.0;
    p.parallelScale = 1.0;
    return p;
}

// A tracker that has seen one frame at @p pose: the state of a linked window
// that has drawn its view once.
Tracker seenAt(const Pose &pose)
{
    Tracker t;
    t.frame(pose, false);
    return t;
}
}  // namespace

void TestLinkedCameraSync::firstFrame_sendsNothing()
{
    // Linking sends the camera itself when switched on; the first frame only
    // gives the tracker something to compare with.
    Tracker t;
    QVERIFY(!t.frame(frontView(), false));
    QVERIFY(!t.pending());
}

void TestLinkedCameraSync::unchangedCamera_sendsNothing()
{
    // A frame drawn for something else (a new channel, a threshold) leaves
    // the camera where it was.
    Tracker t = seenAt(frontView());
    QVERIFY(!t.frame(frontView(), false));
    QVERIFY(!t.frame(frontView(), false));
    QVERIFY(!t.pending());
}

void TestLinkedCameraSync::everyPoseField_countsAsAMove()
{
    // Wheel zoom moves the position (or the parallel scale), a pan the focal
    // point too, a roll the view-up, a preset all of them.
    const Pose base = frontView();
    QList<Pose> moved;
    Pose p = base;
    p.position[2] = 9.0;   // wheel zoom (perspective)
    moved << p;
    p = base;
    p.focalPoint[0] = 0.5;   // pan
    moved << p;
    p = base;
    p.viewUp = { 1.0, 0.0, 0.0 };   // roll
    moved << p;
    p = base;
    p.viewAngle = 20.0;
    moved << p;
    p = base;
    p.parallelScale = 0.8;   // wheel zoom (parallel projection)
    moved << p;
    for (const Pose &m : moved) {
        Tracker t = seenAt(base);
        QVERIFY(t.frame(m, false));
        QVERIFY(t.pending());
    }
}

void TestLinkedCameraSync::burstOfFrames_joinsOneSend()
{
    // A drag draws a frame per mouse move: one send is scheduled, the frames
    // before it goes out join it, and the next move after it schedules again.
    Tracker t = seenAt(frontView());
    Pose p = frontView();
    p.position[0] = 1.0;
    QVERIFY(t.frame(p, false));
    p.position[0] = 2.0;
    QVERIFY(!t.frame(p, false));
    p.position[0] = 3.0;
    QVERIFY(!t.frame(p, false));
    QVERIFY(t.takePending());
    QVERIFY(!t.takePending());   // one send, not three
    p.position[0] = 4.0;
    QVERIFY(t.frame(p, false));
}

void TestLinkedCameraSync::frameWhileApplyingPeer_isRecordedNotSent()
{
    // A frame drawn while a peer's update is applied is the peer's move. It is
    // still recorded, so the next frame at that pose is not a move either.
    Tracker t = seenAt(frontView());
    Pose p = frontView();
    p.position[1] = 2.0;
    QVERIFY(!t.frame(p, true));
    QVERIFY(!t.pending());
    QVERIFY(!t.frame(p, false));
}

void TestLinkedCameraSync::adoptedPose_isNotSentBack()
{
    // The follower records the camera it adopted before drawing it; the frame
    // may come later (the widget repaints on its own schedule), outside the
    // apply guard, and must still not send it back.
    Tracker t = seenAt(frontView());
    Pose fromPeer = frontView();
    fromPeer.position = { 3.0, 4.0, 5.0 };
    t.adopt(fromPeer);
    QVERIFY(!t.frame(fromPeer, false));
    QVERIFY(!t.pending());
    // The user then moves this window: that is news again.
    Pose mine = fromPeer;
    mine.focalPoint[2] = 1.0;
    QVERIFY(t.frame(mine, false));
}

void TestLinkedCameraSync::adopt_dropsAWaitingSend()
{
    // A peer's camera arriving after this window scheduled a send is the newer
    // shared pose: the waiting send would push the peer's pose straight back.
    Tracker t = seenAt(frontView());
    Pose mine = frontView();
    mine.position[0] = 1.0;
    QVERIFY(t.frame(mine, false));
    Pose fromPeer = frontView();
    fromPeer.position[0] = -1.0;
    t.adopt(fromPeer);
    QVERIFY(!t.takePending());
}

void TestLinkedCameraSync::reset_makesTheNextFrameAFirst()
{
    // Linking switched off and on again: frames drawn meanwhile are forgotten,
    // and the first one after is not a move (linking sends the camera itself).
    Tracker t = seenAt(frontView());
    Pose p = frontView();
    p.position[0] = 1.0;
    QVERIFY(t.frame(p, false));
    t.reset();
    QVERIFY(!t.pending());
    p.position[0] = 7.0;
    QVERIFY(!t.frame(p, false));
    QVERIFY(!t.frame(p, false));
}

void TestLinkedCameraSync::twoWindows_doNotPingPong()
{
    // Window A is wheel-zoomed; B follows. B's frames, and A's next frames,
    // send nothing: the exchange stops after one send.
    Tracker a = seenAt(frontView());
    Tracker b = seenAt(frontView());
    Pose zoomed = frontView();
    zoomed.position[2] = 8.0;

    QVERIFY(a.frame(zoomed, false));   // A draws the zoom
    QVERIFY(a.takePending());          // A sends it (its last pose is already the zoom)

    // B rebuilds the camera against its own cube: a different pose, adopted.
    Pose bPose = zoomed;
    bPose.focalPoint = { 0.1, 0.2, 0.3 };
    b.adopt(bPose);
    QVERIFY(!b.frame(bPose, true));    // drawn under the apply guard
    QVERIFY(!b.frame(bPose, false));   // and repainted later
    QVERIFY(!b.pending());

    QVERIFY(!a.frame(zoomed, false));  // A repaints: nothing to send
    QVERIFY(!a.pending());
}

void TestLinkedCameraSync::threeWindows_oneSendReachesBoth()
{
    // A is rotated; B and C adopt it. Neither sends, and A's repaint does not
    // either, so the group settles after one send.
    Tracker a = seenAt(frontView());
    Tracker b = seenAt(frontView());
    Tracker c = seenAt(frontView());
    Pose turned = frontView();
    turned.position = { 10.0, 0.0, 0.0 };
    QVERIFY(a.frame(turned, false));
    QVERIFY(a.takePending());
    Pose bPose = turned;
    bPose.focalPoint = { 1.0, 1.0, 1.0 };
    Pose cPose = turned;
    cPose.focalPoint = { 2.0, 2.0, 2.0 };
    b.adopt(bPose);
    c.adopt(cPose);
    QVERIFY(!b.frame(bPose, true));
    QVERIFY(!c.frame(cPose, true));
    QVERIFY(!b.frame(bPose, false));
    QVERIFY(!c.frame(cPose, false));
    QVERIFY(!a.frame(turned, false));
}

void TestLinkedCameraSync::relinkWhileASendWaits_sendsOnce()
{
    // Linking off with a send scheduled, then on again and a move: the old
    // timer finds nothing to send, the new move schedules exactly one send.
    Tracker t = seenAt(frontView());
    Pose p = frontView();
    p.position[0] = 1.0;
    QVERIFY(t.frame(p, false));   // send scheduled (timer 1)
    t.reset();                    // linking off
    t.adopt(p);                   // linking on: the window sends and adopts
    QVERIFY(!t.takePending());    // timer 1 fires: nothing to send
    p.position[0] = 2.0;
    QVERIFY(t.frame(p, false));   // timer 2
    QVERIFY(t.takePending());
    QVERIFY(!t.takePending());
}

void TestLinkedCameraSync::nonFinitePose_isNeitherSentNorRecorded()
{
    // NaN never equals itself: recorded, a broken camera would be a move on
    // every frame and two linked windows would trade it for ever.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    Tracker t = seenAt(frontView());
    Pose broken = frontView();
    broken.position[0] = nan;
    QVERIFY(!broken.finite());
    QVERIFY(!t.frame(broken, false));
    QVERIFY(!t.frame(broken, false));
    QVERIFY(!t.pending());
    Pose far = frontView();
    far.parallelScale = inf;
    QVERIFY(!far.finite());
    QVERIFY(!t.frame(far, false));
    // Adopting one records nothing either, but still drops a waiting send.
    Pose moved = frontView();
    moved.position[0] = 1.0;
    QVERIFY(t.frame(moved, false));
    t.adopt(broken);
    QVERIFY(!t.pending());
    QVERIFY(!t.frame(moved, false));   // last recorded pose is still `moved`
    QVERIFY(frontView().finite());
}
