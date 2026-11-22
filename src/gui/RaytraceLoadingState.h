#ifndef RAYTRACELOADINGSTATE_H
#define RAYTRACELOADINGSTATE_H

// What the ray-traced view says while it prepares its data (RaytraceViewport,
// issue #447). Widget-free, so the rules can be unit-tested.
//
// Before anything is on screen the view is black, which looks like a failed
// render: a cover over the whole view says what is happening. Once a frame is
// on screen it stays a valid view while new data is fetched (a radius or a
// filter changed: some 30 s for 3.66 M particles over a tunnel), so the view
// keeps its frame and the mouse, and a small badge in a corner says the same.
// An error ends the spinner either way; it is never left running.

namespace visivo {

class RaytraceLoadingState
{
public:
    enum class Show { Nothing, Cover, Badge };

    /// A fetch (or the backend's frame in place of the GPU view) started.
    void begin()
    {
        m_busy = true;
        m_error = false;
    }
    /// A frame is on screen: the GPU view's first after new data, the
    /// backend's frame, or a still. ``fetching``: another fetch is still in
    /// flight (the frame is of earlier data, painted after that fetch began,
    /// or a still shown meanwhile), so the wait it shows goes on.
    void frameShown(bool fetching = false)
    {
        m_hasFrame = true;
        if (fetching)
            return;
        m_busy = false;
        m_error = false;
    }
    /// What is on screen is no frame any more (the GPU view could not start
    /// and the backend's frame has not come yet).
    void frameLost() { m_hasFrame = false; }
    /// The fetch was superseded by settings the frame on screen already
    /// shows: nothing to wait for.
    void finish()
    {
        m_busy = false;
        m_error = false;
    }
    /// The fetch failed: said, without a spinner.
    void fail()
    {
        m_busy = false;
        m_error = true;
    }

    bool busy() const { return m_busy; }
    bool error() const { return m_error; }
    bool hasFrame() const { return m_hasFrame; }

    Show show() const
    {
        if (!m_busy && !m_error)
            return Show::Nothing;
        return m_hasFrame ? Show::Badge : Show::Cover;
    }

private:
    bool m_hasFrame{ false };
    bool m_busy{ false };
    bool m_error{ false };
};

} // namespace visivo

#endif // RAYTRACELOADINGSTATE_H
