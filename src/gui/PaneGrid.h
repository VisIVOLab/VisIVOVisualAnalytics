#ifndef PANEGRID_H
#define PANEGRID_H

// PaneGrid — the 1 / 2 / 4 pane workspace the cube window built for itself and the
// particle window now uses too. It owns the CHROME of the four slots (frame with the
// active border, a header with dot, title dropdown, qualifier, a per-view control slot,
// maximise and pop-out buttons, and a host the view is mounted in), the two columns
// that arrange them, the pop-out of a pane into its own window with a placeholder in
// the grid, and the in-workspace maximise. It owns NO views: what a slot shows, which
// slot is active and what a layout change does to the views stay with the window,
// which listens to the signals below. The rules of the layout itself (which slots a
// layout shows, how it grows) are in PaneLayoutModel.h.

#include "PaneLayoutModel.h"

#include <QByteArray>
#include <QFrame>
#include <QLabel>
#include <QObject>
#include <QPointer>
#include <QSplitter>
#include <QString>
#include <QToolButton>

#include <array>
#include <functional>

class QHBoxLayout;
class QWidget;

class PaneGrid : public QObject
{
    Q_OBJECT
public:
    struct Slot
    {
        QPointer<QFrame> frame;          ///< header + view, carries the active border
        QPointer<QWidget> header;        ///< a press makes the pane active
        QPointer<QLabel> dot;            ///< active indicator
        QPointer<QToolButton> titleBtn;  ///< plain text at 1 pane, dropdown at more
        QPointer<QLabel> qualifier;      ///< mono meta text beside the title
        QPointer<QWidget> viewHost;      ///< the mounted view is reparented in here
        QHBoxLayout *viewHostLayout{ nullptr };
        QHBoxLayout *controls{ nullptr };   ///< per-view control slot in the header
        QPointer<QToolButton> maxBtn;    ///< maximise / restore
        QPointer<QToolButton> detachBtn; ///< pop out / dock back
        // The SAME frame lives temporarily in its own window while popped out; the grid
        // keeps its place with a placeholder, so the layout does not re-flow.
        QPointer<QWidget> detachedWindow;
        QPointer<QWidget> placeholder;
        QByteArray detachedGeometry;     ///< where the pop-out window was left, for a re-detach
    };

    /// @p mainSplitter must be empty (horizontal): the two columns go into it.
    /// @p window owns the pop-out windows (they close with it).
    PaneGrid(QSplitter *mainSplitter, QWidget *window, QObject *parent = nullptr);
    ~PaneGrid() override;

    /// Build the four slots, and the columns unless @p installColumns is false (the
    /// window then calls installColumns() once the splitter is empty). Once.
    void build(bool installColumns = true);
    /// Put the two columns into the main splitter.
    void installColumns();

    Slot &slot(int i) { return m_slots[i]; }
    const Slot &slot(int i) const { return m_slots[i]; }
    QSplitter *mainSplitter() const { return m_main; }
    QSplitter *leftColumn() const { return m_left; }
    QSplitter *rightColumn() const { return m_right; }
    QSplitter *columnOf(int slot) const { return visivo::panes::inLeftColumn(slot) ? m_left : m_right; }

    /// Put @p w in the header of @p slot, just before the maximise button (the cube's
    /// colour-map strip, the particle panes' link toggle).
    void addHeaderWidget(int slot, QWidget *w);

    /// Called to name a pop-out window; the default is "Pane N".
    void setTitleProvider(std::function<QString(int)> provider) { m_title = std::move(provider); }

    bool isDetached(int slot) const { return m_state.isDetached(slot); }
    bool isMaximised() const { return m_state.maximised; }
    /// Move the pane's frame to its own window. False when it already is, or there is
    /// no such pane. Emits paneDetached afterwards.
    bool detach(int slot);
    /// Put a popped-out pane back where it was. Idempotent. Emits paneDocked.
    bool dock(int slot);

    /// Show only @p slot (and its column), turning its button into "restore".
    void showOnly(int slot);
    /// Undo showOnly's buttons (visibility is the layout's to reapply).
    void resetMaximiseButtons();
    /// Show the frames of @p visible and the columns that hold any.
    void applyVisibility(const std::array<bool, visivo::panes::kSlots> &visible);

    /// Pixel-equal halves for a fresh 2- or 4-pane arrangement.
    void equaliseSplits(int panes, bool stacked);

    /// The look of the active pane: brand-blue border and dot and a tinted header, the
    /// others near-flush. @p armed (a tool listens in this pane) wears a thick warning border.
    void styleSlot(int slot, bool active, bool armed = false);

    /// A press ANYWHERE in a pane (its view too, not only the header) emits
    /// headerPressed. Off by default: the cube has its own rule for idle views. The grid
    /// then watches the whole application, so views created later are covered.
    void setPressActivates(bool on);

signals:
    /// A press on a pane header: the window makes that pane active.
    void headerPressed(int slot);
    /// The ⤢ / ⤡ button: the window maximises or restores.
    void maximiseToggled(int slot);
    void paneDetached(int slot);
    void paneDocked(int slot);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *makeDetachPlaceholder(int slot);

    QPointer<QSplitter> m_main;
    QPointer<QWidget> m_window;
    QPointer<QSplitter> m_left;     ///< slot 0 (top) + slot 2 (bottom)
    QPointer<QSplitter> m_right;    ///< slot 1 (top) + slot 3 (bottom)
    std::array<Slot, visivo::panes::kSlots> m_slots;
    std::function<QString(int)> m_title;
    visivo::panes::GridState m_state;
    bool m_pressActivates{ false };
};

#endif // PANEGRID_H
