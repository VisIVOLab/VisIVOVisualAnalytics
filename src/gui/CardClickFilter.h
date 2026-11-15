#ifndef CARDCLICKFILTER_H
#define CARDCLICKFILTER_H

#include <functional>
#include <utility>

#include <QEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QWidget>

namespace datahub {

// ---------------------------------------------------------------------------
// CardClickFilter
// Installed on a QFrame to emit a signal when the user left-clicks the card.
// Uses a lambda to avoid a separate MOC-registered class.
//
// The callback is queued, never run inside the release's delivery: it may open
// a dialog or otherwise spin a nested event loop, and the card's owner may
// rebuild its cards meanwhile (the Recent Datasets list does, on the manager's
// changed()). The deferred deletion of the card, and of this filter with it,
// would then run while Qt is still delivering the event through them (#310).
// A queued call made on this filter is dropped if the card is gone by then.
// The release that ends a double-click is ignored, so a double-click acts once.
// ---------------------------------------------------------------------------
class CardClickFilter : public QObject
{
public:
    std::function<void()> onClicked;
    explicit CardClickFilter(std::function<void()> fn, QObject *parent)
        : QObject(parent), onClicked(std::move(fn))
    {}
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonDblClick) {
            if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                m_skipRelease = true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                if (m_skipRelease) {
                    m_skipRelease = false;
                    return false;
                }
                if (auto *w = qobject_cast<QWidget *>(obj)) {
                    if (w->rect().contains(me->position().toPoint()) && onClicked) {
                        QMetaObject::invokeMethod(
                                this, [fn = onClicked]() { fn(); }, Qt::QueuedConnection);
                    }
                }
            }
        }
        return false;
    }

private:
    bool m_skipRelease = false;
};

}  // namespace datahub

#endif // CARDCLICKFILTER_H
