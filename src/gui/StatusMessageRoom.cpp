#include "StatusMessageRoom.h"

#include <QEvent>
#include <QLabel>
#include <QProgressBar>
#include <QSizeGrip>
#include <QStatusBar>
#include <QWidget>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace visivo {

namespace {
constexpr int kMessageMargin = 24;   // the bar's own indent either side of the text
constexpr int kWidgetSlack = 4;      // the layout's spacing between two widgets
} // namespace

StatusMessageRoom::StatusMessageRoom(QStatusBar *bar) : QObject(bar), m_bar(bar)
{
    setObjectName(u"visivoStatusMessageRoom"_s);
    QObject::connect(bar, &QStatusBar::messageChanged, this,
                     [this](const QString &) { onMessage(); });
}

StatusMessageRoom *StatusMessageRoom::attach(QStatusBar *bar)
{
    if (!bar)
        return nullptr;
    // By name, as a QObject: the class has no meta-object of its own for
    // qobject_cast, and only this file makes objects of that name.
    QObject *found = bar->findChild<QObject *>(u"visivoStatusMessageRoom"_s,
                                               Qt::FindDirectChildrenOnly);
    return found ? static_cast<StatusMessageRoom *>(found) : new StatusMessageRoom(bar);
}

void StatusMessageRoom::yieldToMessages(QWidget *w)
{
    if (w && !m_yielding.contains(w))
        m_yielding.append(w);
}

int StatusMessageRoom::roomWanted(int textWidth, int barWidth)
{
    return std::min(textWidth + kMessageMargin, barWidth * 3 / 5);
}

void StatusMessageRoom::fold(QWidget *w)
{
    m_ours = true;
    w->hide();
    m_ours = false;
    w->installEventFilter(this);
    m_folded.append(w);
}

void StatusMessageRoom::restore()
{
    const QList<QPointer<QWidget>> folded = std::exchange(m_folded, {});
    for (const QPointer<QWidget> &w : folded) {
        if (!w)
            continue;
        w->removeEventFilter(this);
        // The window may have hidden it for its own reasons while it was folded
        // (an emptied label); an unseen hide() of an already hidden widget
        // makes no event, so an empty label stays folded.
        const auto *label = qobject_cast<QLabel *>(w);
        if (w->isHidden() && w->parentWidget() == m_bar && !(label && label->text().isEmpty())) {
            m_ours = true;
            w->show();
            m_ours = false;
        }
    }
}

bool StatusMessageRoom::eventFilter(QObject *watched, QEvent *event)
{
    // A show or hide that is not ours: the window has taken the widget back.
    if (!m_ours && (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent)) {
        watched->removeEventFilter(this);
        m_folded.removeAll(qobject_cast<QWidget *>(watched));
    }
    return false;
}

void StatusMessageRoom::onMessage()
{
    // A new message replaces the old: start from the bar as the window left it.
    restore();
    // The bar's current message, not the signal's argument: BusyStatus puts its
    // own line up from inside messageChanged(""), and the two signals can reach
    // us in either order.
    if (!m_bar)
        return;
    const QString message = m_bar->currentMessage();
    if (message.isEmpty())
        return;

    // The permanent widgets, left to right. The bar keeps them in a nested
    // layout, so they are found as its children and ordered by where they sit;
    // the normal widgets are already hidden by the bar while a message shows.
    QList<QWidget *> shown = m_bar->findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
    shown.removeIf([](QWidget *w) { return w->isHidden() || qobject_cast<QSizeGrip *>(w); });
    std::stable_sort(shown.begin(), shown.end(),
                     [](QWidget *a, QWidget *b) { return a->x() < b->x(); });
    QList<QWidget *> crowd;
    for (QWidget *w : std::as_const(shown)) {
        if (m_yielding.contains(w)) {
            fold(w);
        } else {
            crowd.append(w);
        }
    }

    const int barWidth = m_bar->width();
    const int wanted =
            roomWanted(m_bar->fontMetrics().horizontalAdvance(message), barWidth);
    int taken = 0;
    for (QWidget *w : std::as_const(crowd))
        taken += w->sizeHint().width() + kWidgetSlack;

    // Leftmost first: those are the readouts that come and go; the status rail
    // on the right is what the user keeps an eye on.
    for (QWidget *w : std::as_const(crowd)) {
        if (taken + wanted <= barWidth)
            break;
        // The busy bar is the window's own to show and hide (BusyStatus).
        if (qobject_cast<QProgressBar *>(w))
            continue;
        taken -= w->sizeHint().width() + kWidgetSlack;
        fold(w);
    }
}

} // namespace visivo
