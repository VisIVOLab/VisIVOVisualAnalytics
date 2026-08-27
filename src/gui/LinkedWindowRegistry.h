#ifndef LINKED_WINDOW_REGISTRY_H
#define LINKED_WINDOW_REGISTRY_H

#include <QList>
#include <QPointer>

/// Registry of the open viewer windows that participate in linked-view sync.
///
/// Extracted from vtkWindowCube's `static QList<QPointer<vtkWindowCube>>
/// s_cubeWindows` (E-04 god-object decomposition, step 2): the one piece of the
/// linked-view subsystem that is genuinely disjoint from the window's VTK state,
/// so it can be unit-tested headless. Behaviour is verbatim — `add`/`remove` map
/// to the old `append`/`removeAll`, and `snapshot()` hands back the QPointer list
/// the broadcast methods already copied before iterating (QPointers auto-null on
/// window destruction, so a peer that dies mid-broadcast is safely skipped).
///
/// The echo-suppression state machine (the RAII `m_applyingLinked*` guards, the
/// `m_lastLinkedChannel` dedup and the async linked-request token set) stays on
/// the window for now — it is deeply coupled to the VTK apply paths and is a
/// later, higher-risk extraction step.
///
/// @tparam W the window type (QObject-derived, so QPointer can track it).
template <class W>
class LinkedWindowRegistry
{
public:
    /// Register a window (call from its constructor).
    void add(W *window) { m_windows.append(window); }

    /// Unregister a window (call from its destructor). No-op if not present.
    void remove(W *window) { m_windows.removeAll(window); }

    /// A copy of the current registry for safe iteration: peers may be destroyed
    /// by an apply call during the loop, and the copied QPointers auto-null so the
    /// caller's `if (p && p != self ...)` guard skips them. Callers apply their own
    /// self-exclusion and link-facet predicate.
    QList<QPointer<W>> snapshot() const { return m_windows; }

    /// Number of registered entries (including any that have since auto-nulled).
    int size() const { return m_windows.size(); }

private:
    QList<QPointer<W>> m_windows;
};

#endif  // LINKED_WINDOW_REGISTRY_H
