#ifndef PANELAYOUTMODEL_H
#define PANELAYOUTMODEL_H

// Pure rules of the 1 / 2 / 4 pane grid the cube and the particle windows share
// (PaneGrid is the widget side). Header-only and free of Qt Widgets and VTK so the
// rules can be unit-tested: which slots a layout shows, which column a slot sits
// in, how the layout grows when a pane is added, and which slot a pane added next
// takes. Slots are numbered like the grid: 0 and 2 in the left column (top and
// bottom), 1 and 3 in the right one.

#include <array>

namespace visivo::panes {

constexpr int kSlots = 4;

/// 1, 2 or 4; anything else reads as 1.
constexpr int normalisedCount(int panes)
{
    return (panes == 1 || panes == 2 || panes == 4) ? panes : 1;
}

/// The slots a layout shows. Two panes sit side by side in slots {0, 1}, or one
/// above the other in {0, 2} when the grid is narrow (@p stacked).
constexpr std::array<bool, kSlots> visibleSlots(int panes, bool stacked)
{
    panes = normalisedCount(panes);
    if (panes == 1)
        return { true, false, false, false };
    if (panes == 2)
        return stacked ? std::array<bool, kSlots>{ true, false, true, false }
                       : std::array<bool, kSlots>{ true, true, false, false };
    return { true, true, true, true };
}

constexpr bool inLeftColumn(int slot) { return slot == 0 || slot == 2; }

/// The layout one more pane needs: 1 -> 2 -> 4; 4 has no room for another.
constexpr int grownCount(int panes)
{
    panes = normalisedCount(panes);
    return panes == 1 ? 2 : 4;
}

constexpr bool canGrow(int panes) { return normalisedCount(panes) < 4; }

/// The first free slot a new pane takes in @p visible, or -1 when every visible
/// slot is taken. @p taken[i] is true when slot i already holds a view.
constexpr int firstFreeSlot(const std::array<bool, kSlots> &visible,
                            const std::array<bool, kSlots> &taken)
{
    for (int i = 0; i < kSlots; ++i)
        if (visible[i] && !taken[i])
            return i;
    return -1;
}

/// The slot a new pane goes to. The layout grows when every visible slot is taken
/// and sets @p newCount; when it cannot grow, or has a free slot, @p newCount stays.
/// -1: no room (4 panes, all taken).
constexpr int slotForNewPane(int panes, bool stacked, const std::array<bool, kSlots> &taken,
                             int *newCount = nullptr)
{
    panes = normalisedCount(panes);
    int slot = firstFreeSlot(visibleSlots(panes, stacked), taken);
    if (slot >= 0) {
        if (newCount)
            *newCount = panes;
        return slot;
    }
    if (!canGrow(panes))
        return -1;
    const int grown = grownCount(panes);
    // Growing 2 -> 4 from a stacked pair keeps the two views where they are.
    slot = firstFreeSlot(visibleSlots(grown, stacked), taken);
    if (newCount)
        *newCount = grown;
    return slot;
}

/// Layout state shrinking to @p panes: when going 4/2 -> 1 the active pane's view
/// moves to slot 0. Returns the slot to swap into 0, or -1 for no swap.
constexpr int survivorSwapSlot(int panes, int currentCount, int activeSlot)
{
    return (normalisedCount(panes) == 1 && currentCount > 1 && activeSlot != 0) ? activeSlot : -1;
}

/// What the grid has to remember between events: which panes are popped out into their
/// own window and whether one pane fills the workspace. Pure state, the widget side is
/// PaneGrid's: it asks before acting and tells after. The rules are the cube window's
/// from the start: a pane that is already out cannot be popped out again, maximise and
/// pop-out do not compose (a pop-out is refused while one pane is maximised), and docking
/// a pane that is not out is a no-op, so a close event or a second click is harmless.
struct GridState
{
    bool maximised{ false };
    std::array<bool, kSlots> detached{};

    static constexpr bool validSlot(int slot) { return slot >= 0 && slot < kSlots; }
    bool isDetached(int slot) const { return validSlot(slot) && detached[slot]; }
    bool canDetach(int slot) const { return validSlot(slot) && !detached[slot] && !maximised; }

    /// True when the pane went out (false: refused, nothing changed).
    bool detach(int slot)
    {
        if (!canDetach(slot))
            return false;
        detached[slot] = true;
        return true;
    }
    /// True when the pane was out and is back (false: it was not out).
    bool dock(int slot)
    {
        if (!isDetached(slot))
            return false;
        detached[slot] = false;
        return true;
    }
    void maximise() { maximised = true; }
    void restore() { maximised = false; }
    int detachedCount() const
    {
        int n = 0;
        for (bool d : detached)
            n += d ? 1 : 0;
        return n;
    }
};

} // namespace visivo::panes

#endif // PANELAYOUTMODEL_H
