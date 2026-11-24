#include "RegionSessionController.h"

#include "RegionTreeRows.h"
#include "SessionDataTree.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPoint>
#include <QStatusBar>

using namespace Qt::StringLiterals;
using visivo::regions::Region;

RegionSessionController::RegionSessionController(QMainWindow *window, SessionDataTree *tree,
                                                 visivo::regions::RegionList *list, Hooks hooks)
    : QObject(window)
    , m_window(window)
    , m_tree(tree)
    , m_list(list)
    , m_hooks(std::move(hooks))
{
    if (!m_tree)
        return;
    connect(m_tree, &SessionDataTree::regionSelected, this, &RegionSessionController::onSelected);
    connect(m_tree, &SessionDataTree::regionVisibilityRequested, this,
            &RegionSessionController::onVisibility);
    connect(m_tree, &SessionDataTree::regionActivated, this, [this](int id) {
        if (m_list->find(id) && m_hooks.properties)
            m_hooks.properties(id);
    });
    connect(m_tree, &SessionDataTree::regionContextMenuRequested, this,
            &RegionSessionController::showRowMenu);
    connect(m_tree, &SessionDataTree::regionGroupContextMenuRequested, this,
            &RegionSessionController::showGroupMenu);
}

void RegionSessionController::refresh()
{
    if (m_tree)
        m_tree->setRegions(visivo::regions::buildRegionTree(*m_list));
}

void RegionSessionController::status(const QString &message)
{
    if (m_window)
        m_window->statusBar()->showMessage(message, 5000);
}

std::vector<int> RegionSessionController::idsOf(int batch) const
{
    std::vector<int> ids;
    for (const Region &r : m_list->regions())
        if (batch < 0 || (batch > 0 && r.batch == batch && !r.sourceFile.isEmpty()))
            ids.push_back(r.id);
    return ids;
}

void RegionSessionController::onSelected(int id)
{
    if (!editable() || !m_list->find(id) || m_list->selectedId() == id) {
        this->refresh();   // the tree shows the list's selection again
        return;
    }
    m_list->select(id);
    m_hooks.changed();
}

void RegionSessionController::onVisibility(const QList<int> &ids, bool visible)
{
    // While a drag runs the list is mid-gesture: the box the user clicked goes
    // back to what the list says.
    if (!editable() || m_list->setVisible(std::vector<int>(ids.begin(), ids.end()), visible) == 0) {
        this->refresh();
        return;
    }
    m_hooks.changed();
}

void RegionSessionController::rename(int id)
{
    const Region *r = m_list->find(id);
    if (!r || !m_window)
        return;
    // The dialog runs its own event loop: the window, or the region, may go.
    const QPointer<QMainWindow> guard(m_window);
    const QPointer<RegionSessionController> self(this);
    bool ok = false;
    const QString name = QInputDialog::getText(m_window, u"Rename Region"_s, u"Name:"_s,
                                               QLineEdit::Normal, r->name, &ok);
    if (!ok || !guard || !self || !editable())
        return;
    if (m_list->rename(id, name))
        m_hooks.changed();
}

void RegionSessionController::showRowMenu(int id, const QPoint &globalPos)
{
    const Region *r = m_list->find(id);
    if (!r)
        return;
    const bool visible = r->visible;
    // No parent: a stack menu owned by the window would be deleted twice if
    // the window went away while the menu runs its loop.
    QMenu menu;
    menu.setTitle(r->name);
    QAction *props = menu.addAction(u"Properties…"_s);
    QAction *analyse = menu.addAction(u"Analyse"_s);
    analyse->setEnabled(m_hooks.canAnalyse ? m_hooks.canAnalyse(*r)
                                           : visivo::regions::hasArea(r->shape));
    menu.addSeparator();
    QAction *rename = menu.addAction(u"Rename…"_s);
    QAction *toggle = menu.addAction(visible ? u"Hide"_s : u"Show"_s);
    QAction *background = menu.addAction(u"Background Region"_s);
    background->setCheckable(true);
    background->setChecked(visivo::regions::isBackground(*r));
    QAction *remove = menu.addAction(u"Delete"_s);
    menu.addSeparator();
    QAction *exportOne = menu.addAction(u"Export…"_s);
    for (QAction *a : { rename, toggle, remove, background })
        a->setEnabled(editable());
    background->setEnabled(editable() && visivo::regions::hasArea(r->shape));
    const QPointer<RegionSessionController> self(this);
    QAction *chosen = menu.exec(globalPos);
    if (!self || !chosen || !m_list->find(id))
        return;
    if (chosen == props) {
        if (m_hooks.properties)
            m_hooks.properties(id);
    } else if (chosen == analyse) {
        // The window measures the selected region, through its own gate.
        if (editable() && m_list->selectedId() != id) {
            m_list->select(id);
            m_hooks.changed();
        }
        if (self && m_list->find(id) && m_hooks.analyse)
            m_hooks.analyse(id);
    } else if (chosen == rename) {
        this->rename(id);
    } else if (chosen == toggle) {
        if (editable() && m_list->setVisible(id, !visible))
            m_hooks.changed();
    } else if (chosen == background) {
        // Region flux (issue #401) subtracts the nearest background region.
        if (editable() && m_list->setBackground(id, background->isChecked()))
            m_hooks.changed();
    } else if (chosen == remove) {
        if (!editable())
            return;
        const QString name = m_list->find(id)->name;
        if (m_list->remove(id)) {
            m_hooks.changed();
            if (self)
                this->status(u"%1 deleted (Undo brings it back)."_s.arg(name));
        }
    } else if (chosen == exportOne) {
        if (m_hooks.exportRegions)
            m_hooks.exportRegions({ id });
    }
}

void RegionSessionController::showGroupMenu(int batch, const QPoint &globalPos)
{
    const std::vector<int> ids = idsOf(batch);
    if (ids.empty())
        return;
    int hidden = 0;
    for (int id : ids)
        if (const Region *r = m_list->find(id); r && !r->visible)
            ++hidden;
    const int n = int(ids.size());
    QMenu menu;
    QAction *showAll = menu.addAction(u"Show All"_s);
    QAction *hideAll = menu.addAction(u"Hide All"_s);
    showAll->setEnabled(hidden > 0 && editable());
    hideAll->setEnabled(hidden < n && editable());
    menu.addSeparator();
    QAction *deleteAll = menu.addAction(n == 1 ? u"Delete"_s : u"Delete All %1"_s.arg(n));
    deleteAll->setEnabled(editable());
    menu.addSeparator();
    QAction *exportAll = menu.addAction(u"Export All…"_s);
    const QPointer<RegionSessionController> self(this);
    QAction *chosen = menu.exec(globalPos);
    // The list may have changed while the menu was open: act on what is left
    // of the group now.
    if (!self || !chosen)
        return;
    const std::vector<int> now = idsOf(batch);
    if (now.empty())
        return;
    if (chosen == showAll || chosen == hideAll) {
        if (editable() && m_list->setVisible(now, chosen == showAll) > 0)
            m_hooks.changed();
    } else if (chosen == deleteAll) {
        if (!editable())
            return;
        const int gone = m_list->removeAll(now);
        if (gone > 0) {
            m_hooks.changed();
            if (self)
                this->status(u"%1 region(s) deleted (Undo brings them back)."_s.arg(gone));
        }
    } else if (chosen == exportAll) {
        if (m_hooks.exportRegions)
            m_hooks.exportRegions(now);
    }
}
