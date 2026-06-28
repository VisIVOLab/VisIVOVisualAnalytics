#ifndef CARDCLICKFILTER_H
#define CARDCLICKFILTER_H

#include <functional>
#include <utility>

#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QWidget>

namespace datahub {

// ---------------------------------------------------------------------------
// CardClickFilter
// Installed on a QFrame to emit a signal when the user left-clicks the card.
// Uses a lambda to avoid a separate MOC-registered class.
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
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                if (auto *w = qobject_cast<QWidget *>(obj)) {
                    if (w->rect().contains(me->pos())) {
                        onClicked();
                    }
                }
            }
        }
        return false;
    }
};

}  // namespace datahub

#endif // CARDCLICKFILTER_H
