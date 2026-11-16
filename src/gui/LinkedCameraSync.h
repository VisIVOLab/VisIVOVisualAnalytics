#ifndef LINKED_CAMERA_SYNC_H
#define LINKED_CAMERA_SYNC_H

#include <array>
#include <cmath>

// ---------------------------------------------------------------------------
// LinkedCameraSync — when a linked cube window sends its 3-D camera to its
// peers (issue #338).
//
// Linked views used to send the camera from the interactor's
// InteractionEvent / EndInteractionEvent. Those come from mouse gestures
// only: a Camera-menu preset, the R key or any other camera set in code moved
// one window and left the others where they were. The window
// now compares the camera after every frame it draws, and a pose that changed
// is sent, whatever moved it. A refit for newly loaded data and a movie
// export's orbit are adopted rather than sent: they are not the user's move.
//
// Two rules keep linked windows from talking over each other:
//   - a camera adopted from a peer is not news: the follower records it, so
//     neither the frame drawn for it nor a later one sends it back;
//   - frames come in bursts (a drag draws one per mouse move), so a change
//     schedules one send for the next turn of the event loop, and later frames
//     before it goes out join it. The send then reads the camera as it is.
//
// Pure, so it is unit-tested (tests/test_linked_camera_sync.cpp); the window
// reads the camera and owns the timer.
// ---------------------------------------------------------------------------
namespace LinkedCameraSync {

/// What a linked peer receives of a camera. The viewport size is left out on
/// purpose: resizing a window changes no camera, and the peers frame their own
/// cube in their own viewport. So is the projection mode, which is not sent.
struct Pose
{
    std::array<double, 3> position{ 0.0, 0.0, 0.0 };
    std::array<double, 3> focalPoint{ 0.0, 0.0, 0.0 };
    std::array<double, 3> viewUp{ 0.0, 0.0, 0.0 };
    double viewAngle{ 0.0 };
    double parallelScale{ 0.0 };

    // Exact comparison: the pose is compared with the one read back from the
    // same camera, so an unchanged camera gives identical numbers.
    bool operator==(const Pose &o) const
    {
        return position == o.position && focalPoint == o.focalPoint && viewUp == o.viewUp
                && viewAngle == o.viewAngle && parallelScale == o.parallelScale;
    }
    bool operator!=(const Pose &o) const { return !(*this == o); }

    /// Every number is finite. A NaN never equals itself, so a broken camera
    /// would look like a move on every frame and two windows would trade it
    /// for ever; such a pose is neither sent nor recorded.
    bool finite() const
    {
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(position[i]) || !std::isfinite(focalPoint[i])
                || !std::isfinite(viewUp[i]))
                return false;
        return std::isfinite(viewAngle) && std::isfinite(parallelScale);
    }
};

class Tracker
{
public:
    /// A frame was drawn with the camera at @p pose. @p applyingPeer is true
    /// while the window is applying a peer's update. True when a send must be
    /// scheduled: the camera moved, not for a peer, and no send is waiting.
    bool frame(const Pose &pose, bool applyingPeer)
    {
        if (!pose.finite())
            return false;
        if (!m_have) {
            // The first frame seen has nothing to compare with; linking itself
            // sends the camera when it is switched on.
            m_have = true;
            m_last = pose;
            return false;
        }
        if (pose == m_last)
            return false;
        m_last = pose;
        if (applyingPeer)
            return false;
        if (m_pending)
            return false;   // the send already scheduled will carry this pose
        m_pending = true;
        return true;
    }

    /// The camera was just set to @p pose from a peer (or sent by the window
    /// itself): later frames at this pose are not a change. A send still
    /// waiting is dropped, since the peer's pose is now the shared one.
    void adopt(const Pose &pose)
    {
        m_pending = false;
        if (!pose.finite())
            return;
        m_have = true;
        m_last = pose;
    }

    /// The scheduled send is due. True when it is still wanted.
    bool takePending()
    {
        const bool was = m_pending;
        m_pending = false;
        return was;
    }

    bool pending() const { return m_pending; }

    /// Forget the last pose (linking switched off): the next frame is a first.
    void reset()
    {
        m_have = false;
        m_pending = false;
    }

private:
    Pose m_last;
    bool m_have{ false };
    bool m_pending{ false };
};

}  // namespace LinkedCameraSync

#endif   // LINKED_CAMERA_SYNC_H
