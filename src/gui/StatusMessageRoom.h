#pragma once

// StatusMessageRoom.h
// Gives a window's temporary status-bar messages room to be read (issue #508).
//
// QStatusBar draws showMessage() in the gap left of its permanent widgets. A
// permanent widget added with a stretch (the image window's cursor readout)
// takes the whole gap, and a row of permanent labels wider than the bar leaves
// none: either way the message is drawn with zero width and nobody sees it,
// whatever it says. Rather than reshape every bar, this watches the bar's
// messageChanged and, while a message is up, folds away what is in its way:
//
//   - widgets named with yieldToMessages() (an elastic readout) are hidden for
//     as long as any message shows, since their stretch would take the gap;
//   - then, if the other permanent widgets still crowd the message out, the
//     leftmost ones are hidden until it fits (the rail on the right stays).
//
// What was folded away comes back when the message goes. The bar's call sites
// stay as they are.
//
//   visivo::StatusMessageRoom::attach(statusBar())->yieldToMessages(readout);

#include <QList>
#include <QObject>
#include <QPointer>

class QEvent;
class QStatusBar;
class QWidget;

namespace visivo {

class StatusMessageRoom : public QObject
{
public:
    /// The bar's one instance, made on the first call; a child of the bar.
    static StatusMessageRoom *attach(QStatusBar *bar);

    /// `w` is hidden while a message shows, whatever room there is.
    void yieldToMessages(QWidget *w);

    /// The width a message of `textWidth` px wants: its text and a margin, and
    /// at most 60% of the bar, so a long sentence does not clear the whole bar.
    static int roomWanted(int textWidth, int barWidth);

private:
    explicit StatusMessageRoom(QStatusBar *bar);
    void onMessage();
    void fold(QWidget *w);
    void restore();
    bool eventFilter(QObject *watched, QEvent *event) override;

    QPointer<QStatusBar> m_bar;
    QList<QPointer<QWidget>> m_yielding;
    QList<QPointer<QWidget>> m_folded;   // hidden by us, to show again
    bool m_ours = false;                 // a show/hide in progress is ours
};

} // namespace visivo
