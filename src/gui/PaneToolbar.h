#ifndef PANETOOLBAR_H
#define PANETOOLBAR_H

// PaneToolbar — the thin strip above the pane grid of the cube window and the particle
// window: 1 / 2 / 4 pane toggle, a slot for the window's own widgets, the Linked master
// button, the per-facet chips, and a slot at the right end for the cursor readout. Both
// windows build it from here, so they are the same strip by construction: the same
// height, margins, spacing, glyphs and styles. What a window links, and what each chip
// gates, is the window's; the strip only draws the controls and reports clicks.

#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

class QHBoxLayout;
class QToolButton;
class SegmentedToggle;

class PaneToolbar : public QWidget
{
    Q_OBJECT
public:
    /// One facet chip: a small bordered toggle that gates one kind of propagation.
    struct Chip
    {
        QString text;
        QString toolTip;
        bool checked{ true };
        std::function<void(bool)> onToggled;   ///< the chip was flipped by the user
    };

    explicit PaneToolbar(QWidget *parent = nullptr);

    SegmentedToggle *paneCountToggle() const { return m_countToggle; }
    QToolButton *linkedButton() const { return m_linked; }

    /// A window-specific widget between the 1/2/4 toggle and the Linked button (the
    /// cube's 3D/2D switch). Reparented into the strip.
    void addWindowWidget(QWidget *w);
    /// A facet chip after the Linked button (and after the chips added before). Returns it.
    QToolButton *addChip(const Chip &chip);
    /// The cursor readout, at the right end. Reparented into the strip.
    void setReadoutWidget(QWidget *w);

    /// Show the layout the window has now: the toggle's segment, and the "2" glyph as
    /// two rows or two columns. Does not emit paneCountRequested.
    void setLayoutState(int panes, bool stacked);
    /// The Linked button and the chips need something to link to: with none they are
    /// disabled (they keep their checked state); the chips also need Linked on.
    void setLinkAvailable(bool hasTargets, bool linkedOn);
    /// Set a chip's state without calling its callback.
    static void setChipChecked(QToolButton *chip, bool checked);

    int itemCount() const;

signals:
    /// The user chose 1, 2 or 4 panes in the toggle.
    void paneCountRequested(int panes);
    /// The user clicked the "2" segment while two panes are shown: flip the split.
    void splitFlipRequested();

private:
    QHBoxLayout *m_layout{ nullptr };
    SegmentedToggle *m_countToggle{ nullptr };
    QToolButton *m_linked{ nullptr };
    QList<QToolButton *> m_chips;
    int m_panes{ 1 };
    int m_windowWidgets{ 0 };   ///< widgets added between the toggle and Linked
    int m_chipItems{ 0 };       ///< layout items the chips (and their gaps) take
};

#endif // PANETOOLBAR_H
