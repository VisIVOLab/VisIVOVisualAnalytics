#ifndef RegionSessionController_h
#define RegionSessionController_h

// RegionSessionController.h
// The Regions node of Session Data (issue #388), for the image viewer and the
// cube viewer alike.
//
// The window's RegionList stays the one source of truth. After every change
// the window calls refresh(), which writes the list into the tree
// (RegionTreeRows.h → SessionDataTree::setRegions); every gesture in the tree
// (a row selected, a check box, a menu entry) comes back here and becomes an
// edit of the list, one undo step each, followed by the window's own
// "the list changed" handler, which redraws every view and calls refresh().
// setRegions() never emits, so nothing loops.

#include "RegionModel.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <vector>

class QMainWindow;
class QPoint;
class SessionDataTree;

class RegionSessionController : public QObject
{
    Q_OBJECT
public:
    /// What the window does for the tree.
    struct Hooks
    {
        /// False while a region is being dragged: the list is mid-gesture.
        std::function<bool()> editable;
        /// The list changed: redraw every view, refresh the actions, and call refresh().
        std::function<void()> changed;
        std::function<void(int id)> properties;
        std::function<void(int id)> analyse;
        /// Write these regions to a file (the export dialogs).
        std::function<void(const std::vector<int> &ids)> exportRegions;
    };

    /// @p list is the window's own list; the window deletes this controller
    /// before the list goes (its destructor).
    RegionSessionController(QMainWindow *window, SessionDataTree *tree,
                            visivo::regions::RegionList *list, Hooks hooks);

    /// Write the list into the tree.
    void refresh();

private:
    void onSelected(int id);
    void onVisibility(const QList<int> &ids, bool visible);
    void showRowMenu(int id, const QPoint &globalPos);
    void showGroupMenu(int batch, const QPoint &globalPos);
    void rename(int id);
    void status(const QString &message);
    /// The ids of group @p batch, or of every region when @p batch < 0.
    std::vector<int> idsOf(int batch) const;
    bool editable() const { return !m_hooks.editable || m_hooks.editable(); }

    QPointer<QMainWindow> m_window;
    QPointer<SessionDataTree> m_tree;
    visivo::regions::RegionList *m_list;
    Hooks m_hooks;
};

#endif   // RegionSessionController_h
