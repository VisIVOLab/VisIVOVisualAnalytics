#ifndef SNAPSHOT_TIMELINE_STEPS_H
#define SNAPSHOT_TIMELINE_STEPS_H

#include <QScopedValueRollback>
#include <QString>
#include <QStringList>
#include <QVector>

// ---------------------------------------------------------------------------
// SnapshotTimelineSteps — the rules of the particle window's snapshot timeline
// and of the comparison views it steps together (issues #429, #430, #446).
//
// A step closes every view of a comparison and opens the next output in as
// many new windows, each given back the state of the view it replaces. What
// can go wrong there is mostly order: which output is next when some cannot be
// opened, which colour column is "the same quantity" in a file whose columns
// differ, whether a rebuilt scene keeps the camera the step gave it, and
// whether a replacement window's restored controls talk to the group before
// it knows whether it is linked. Pure, so it is unit-tested
// (tests/test_snapshot_timeline_steps.cpp); the window does the rest.
// ---------------------------------------------------------------------------
namespace SnapshotTimelineSteps {

/// A window opened from several catalogue files (snap_z0_1_1_1.vbt ... _8)
/// holds pieces of one output, not a run's outputs: it has no timeline.
inline bool timelineApplies(qsizetype cataloguePaths)
{
    return cataloguePaths <= 1;
}

/// Whether output @p k of a series can be opened. The backend marks those
/// without the particle type shown (an early output with no stars yet) or
/// that do not open; an older backend sends no marks, and every output counts.
inline bool outputAvailable(const QVector<bool> &available, int k)
{
    return k >= 0 && (k >= available.size() || available.at(k));
}

/// The output a step from @p from lands on: the first that can be opened in
/// the direction of @p delta, starting @p delta away. -1 when there is none
/// before the end of the series (Play then stops).
inline int stepTarget(const QVector<bool> &available, int count, int from, int delta)
{
    if (delta == 0)
        return -1;
    const int direction = delta > 0 ? 1 : -1;
    for (int k = from + delta; k >= 0 && k < count; k += direction) {
        if (outputAvailable(available, k))
            return k;
    }
    return -1;
}

/// The output a slider dragged from @p from to @p wanted opens: @p wanted, or
/// the nearest that can be opened beyond it in the direction it moved, or
/// else the nearest back towards @p from. -1 when only @p from is left.
inline int sliderTarget(const QVector<bool> &available, int count, int from, int wanted)
{
    if (wanted < 0 || wanted >= count || wanted == from)
        return -1;
    if (outputAvailable(available, wanted))
        return wanted;
    const int direction = wanted > from ? 1 : -1;
    const int ahead = stepTarget(available, count, wanted, direction);
    if (ahead >= 0)
        return ahead;
    const int back = stepTarget(available, count, wanted, -direction);
    return back == from ? -1 : back;
}

/// Whether a series reply replaces the series a window holds: a failed or
/// malformed reply never hides a timeline that works (mid-playback).
inline bool acceptsSeriesReply(bool heldValid, bool replyValid)
{
    return replyValid || !heldValid;
}

/// The colour key that shows, among @p headers, the quantity a view showed
/// as @p savedKey (named @p savedName). A column key ("raw:<n>") is an index
/// into the file's columns, and another output may list them in another
/// order or without one: the quantity is found by its name (exactly, else
/// ignoring case). Empty when the output has no such column. Keys that name
/// no column (morphology, speed) are kept as they are.
inline QString quantityKey(const QString &savedKey, const QString &savedName,
                           const QStringList &headers)
{
    if (!savedKey.startsWith(QStringLiteral("raw:")))
        return savedKey;
    if (savedName.isEmpty())
        return {};
    qsizetype index = headers.indexOf(savedName);
    if (index < 0) {
        for (qsizetype i = 0; i < headers.size(); ++i) {
            if (headers.at(i).compare(savedName, Qt::CaseInsensitive) == 0) {
                index = i;
                break;
            }
        }
    }
    return index < 0 ? QString() : QStringLiteral("raw:%1").arg(index);
}

/// Whether rebuilding the scene keeps the camera instead of framing the data
/// again. The first build frames it. Afterwards a view that is still loading
/// its rows' other columns (the same rows, so the same extent) keeps it, and
/// so does a view linked in a comparison: framing one view there would move
/// every view of the group, and a timeline step would lose the camera it was
/// given back.
inline bool rebuildKeepsCamera(bool sceneFramed, bool hydrating, bool linkedInGroup)
{
    return sceneFramed && (hydrating || linkedInGroup);
}

/// A comparison view's link state, as the representation broadcast sees it.
struct LinkFlags
{
    bool inGroup{ false };
    bool linked{ false };
    bool representationLinked{ false };
    bool applying{ false };   ///< applying a peer's (or a restored) state
};

/// Whether a change of @p sender's shape, size, budget or ray-traced light
/// is sent to its group, and whether @p receiver takes it.
inline bool representationSends(const LinkFlags &sender)
{
    return sender.inGroup && sender.linked && sender.representationLinked && !sender.applying;
}
inline bool representationReceives(const LinkFlags &receiver)
{
    return receiver.inGroup && receiver.linked && receiver.representationLinked;
}

/// How a window that replaces a view on a step takes that view's state: its
/// own link flags first, then the controls under the applying guard. A new
/// window starts with its source's flags; restoring its controls before its
/// own flags (or without the guard) sent an unlinked view's shape to the
/// linked ones, e.g. Gaussian splats in a linked view became Points.
template <typename Restore>
void restoreWithOwnFlags(bool &linked, bool &representationLinked, bool &applying,
                         bool savedLinked, bool savedRepresentationLinked, Restore &&restore)
{
    linked = savedLinked;
    representationLinked = savedRepresentationLinked;
    const QScopedValueRollback<bool> guard(applying, true);
    restore();
}

} // namespace SnapshotTimelineSteps

#endif // SNAPSHOT_TIMELINE_STEPS_H
