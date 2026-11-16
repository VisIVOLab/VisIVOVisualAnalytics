// test_linked_camera_sync.cpp
// Unit tests for LinkedCameraSync (issues #338 and #358): a linked view sends
// its camera when a drawn frame shows it moved, whatever moved it, and never
// sends back a camera it adopted from a peer or set itself in a refit.

#include "TestLinkedCameraSync.h"
#include "LinkedCameraSync.h"

#include <QList>
#include <QTest>

#include <limits>

using LinkedCameraSync::Pose;
using LinkedCameraSync::Tracker;
using LinkedCameraSync::Watch;

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

// A 2-D view: parallel projection looking down -Z at a 196 x 280 image.
Pose sliceView()
{
    Pose p;
    p.position = { 98.0, 140.0, 500.0 };
    p.focalPoint = { 98.0, 140.0, 0.0 };
    p.viewUp = { 0.0, 1.0, 0.0 };
    p.viewAngle = 30.0;
    p.parallelScale = 154.0;
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


void TestLinkedCameraSync::twoD_resetInCode_isSent()
{
    // Shift+R on a zoomed slice: vtkInteractorStyle resets the camera in code
    // and draws, with no interaction event. The frame shows the move, so it is
    // sent like a wheel zoom.
    Tracker t = seenAt(sliceView());
    Pose zoomed = sliceView();
    zoomed.focalPoint = { 60.0, 90.0, 0.0 };
    zoomed.position = { 60.0, 90.0, 500.0 };
    zoomed.parallelScale = 40.0;
    QVERIFY(t.frame(zoomed, false));   // the wheel zoom
    QVERIFY(t.takePending());
    Pose reset = sliceView();
    reset.position[2] = 742.0;   // ResetCamera also moves the camera back
    QVERIFY(t.frame(reset, false));
    QVERIFY(t.takePending());
}

void TestLinkedCameraSync::twoD_refitInCode_isAdoptedNotSent()
{
    // A new channel, a resized pane or a new colour bar letterboxes the view
    // again in code. The view adopts that camera before drawing it, so the
    // refit is not pushed onto the linked views; the user's next zoom is.
    Tracker t = seenAt(sliceView());
    Pose refit = sliceView();
    refit.parallelScale = 160.0;
    t.adopt(refit);
    QVERIFY(!t.frame(refit, false));
    QVERIFY(!t.pending());
    Pose zoomed = refit;
    zoomed.parallelScale = 80.0;
    QVERIFY(t.frame(zoomed, false));
}

void TestLinkedCameraSync::twoD_paneAndSliceShareAView_withoutEcho()
{
    // The slice is zoomed; the Product pane below it and the linked window's
    // slice adopt the shared view (each against its own colour-bar gutter, so
    // three different cameras). None of them sends, and the slice's repaint
    // does not either: one send settles the group.
    Tracker slice = seenAt(sliceView());
    Tracker pane = seenAt(sliceView());
    Tracker peer = seenAt(sliceView());
    Pose zoomed = sliceView();
    zoomed.parallelScale = 50.0;
    QVERIFY(slice.frame(zoomed, false));
    QVERIFY(slice.takePending());
    Pose paneView = zoomed;
    paneView.focalPoint[0] += 6.0;   // a wider colour bar shifts the data area
    paneView.position[0] += 6.0;
    Pose peerView = zoomed;
    peerView.parallelScale = 51.0;
    pane.adopt(paneView);
    peer.adopt(peerView);
    QVERIFY(!pane.frame(paneView, true));
    QVERIFY(!peer.frame(peerView, true));
    QVERIFY(!pane.frame(paneView, false));
    QVERIFY(!peer.frame(peerView, false));
    QVERIFY(!slice.frame(zoomed, false));
}

void TestLinkedCameraSync::watch_firstFrame_isNotAMove()
{
    Watch w;
    QVERIFY(!w.moved(sliceView()));
    QVERIFY(!w.moved(sliceView()));
}

void TestLinkedCameraSync::watch_everyChange_isAMove()
{
    // Unlike the Tracker, every frame of a burst counts: the image viewer
    // restarts its settle debounce on each, so it streams once the view is quiet.
    Watch w;
    w.moved(sliceView());
    Pose p = sliceView();
    p.parallelScale = 100.0;
    QVERIFY(w.moved(p));
    p.parallelScale = 90.0;
    QVERIFY(w.moved(p));
    QVERIFY(!w.moved(p));   // a repaint at the same view
}

void TestLinkedCameraSync::watch_adoptedPose_isNotAMove()
{
    // A fresh load resets the camera onto the image it just loaded: that view
    // needs no tiles streamed for it.
    Watch w;
    w.moved(sliceView());
    Pose loaded = sliceView();
    loaded.parallelScale = 300.0;
    w.adopt(loaded);
    QVERIFY(!w.moved(loaded));
    Pose zoomed = loaded;
    zoomed.parallelScale = 30.0;
    QVERIFY(w.moved(zoomed));
}

void TestLinkedCameraSync::watch_nonFinitePose_isIgnored()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Watch w;
    Pose broken = sliceView();
    broken.focalPoint[1] = nan;
    QVERIFY(!w.moved(broken));   // not recorded as the first frame either
    QVERIFY(!w.moved(sliceView()));
    QVERIFY(!w.moved(broken));
    w.adopt(broken);              // nothing recorded
    QVERIFY(!w.moved(sliceView()));
}

void TestLinkedCameraSync::twoD_refit_keepsTheUsersWaitingSend()
{
    // The user wheel-zooms the slice; before the send goes out (next turn of
    // the event loop) a slice of the scrub arrives and refits the view in
    // code, putting the shared view back. The refit is not a move, but the
    // zoom's send still goes out; adopt() (a linked view's camera) would drop it.
    Tracker t = seenAt(sliceView());
    Pose zoomed = sliceView();
    zoomed.parallelScale = 60.0;
    QVERIFY(t.frame(zoomed, false));
    Pose refitted = zoomed;
    refitted.position[2] = 520.0;   // the letterbox redone at the shared view
    t.refit(refitted);
    QVERIFY(!t.frame(refitted, false));   // the refit's frame is not a second move
    QVERIFY(t.takePending());             // the zoom is still sent
    QVERIFY(!t.takePending());

    Tracker u = seenAt(sliceView());
    QVERIFY(u.frame(zoomed, false));
    u.adopt(refitted);
    QVERIFY(!u.takePending());
}

void TestLinkedCameraSync::twoD_refit_ofANonFinitePose_recordsNothing()
{
    Tracker t = seenAt(sliceView());
    Pose broken = sliceView();
    broken.parallelScale = std::numeric_limits<double>::quiet_NaN();
    t.refit(broken);
    QVERIFY(!t.frame(sliceView(), false));   // last pose is still the original
    Pose zoomed = sliceView();
    zoomed.parallelScale = 70.0;
    QVERIFY(t.frame(zoomed, false));
    t.refit(broken);
    QVERIFY(t.pending());   // a refit never drops a waiting send
}

void TestLinkedCameraSync::twoD_lastFrameMoved_coversEveryFrameOfTheMove()
{
    // The slice keeps the shared view current on every frame of a drag, also
    // the ones that join a send already waiting; peer frames, repaints and
    // refits are not the user's move.
    Tracker t = seenAt(sliceView());
    Pose p = sliceView();
    p.parallelScale = 100.0;
    QVERIFY(t.frame(p, false));
    QVERIFY(t.lastFrameMoved());
    p.parallelScale = 90.0;
    QVERIFY(!t.frame(p, false));   // joins the waiting send
    QVERIFY(t.lastFrameMoved());
    QVERIFY(!t.frame(p, false));   // repaint
    QVERIFY(!t.lastFrameMoved());
    p.parallelScale = 80.0;
    QVERIFY(!t.frame(p, true));    // a peer's camera
    QVERIFY(!t.lastFrameMoved());
    p.parallelScale = 70.0;
    t.refit(p);
    QVERIFY(!t.frame(p, false));   // the refit's frame
    QVERIFY(!t.lastFrameMoved());
    Pose broken = p;
    broken.position[0] = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!t.frame(broken, false));
    QVERIFY(!t.lastFrameMoved());
}
