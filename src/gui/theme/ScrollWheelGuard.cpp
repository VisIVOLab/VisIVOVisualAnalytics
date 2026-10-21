#include "ScrollWheelGuard.h"

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QScrollBar>
#include <QWidget>

bool ScrollWheelGuard::eventFilter(QObject *watched, QEvent *event)
{
    // Qt's default WheelFocus lets the first wheel tick over a control give it the
    // focus (before any event filter runs), after which the following ticks edit
    // it: such controls take the focus on a click or Tab only.
    if (event->type() == QEvent::Polish) {
        auto *pw = qobject_cast<QWidget *>(watched);
        const bool isControl = qobject_cast<QAbstractSpinBox *>(watched)
                || qobject_cast<QComboBox *>(watched)
                || (qobject_cast<QAbstractSlider *>(watched) && !qobject_cast<QScrollBar *>(watched));
        if (pw && isControl && pw->focusPolicy() == Qt::WheelFocus)
            pw->setFocusPolicy(Qt::StrongFocus);
        return QObject::eventFilter(watched, event);
    }
    if (event->type() != QEvent::Wheel)
        return QObject::eventFilter(watched, event);
    auto *w = qobject_cast<QWidget *>(watched);
    if (!w)
        return false;
    // The control itself, or its line edit / popup child.
    QWidget *control = w;
    while (control && !qobject_cast<QAbstractSpinBox *>(control)
           && !qobject_cast<QComboBox *>(control)
           && !(qobject_cast<QAbstractSlider *>(control) && !qobject_cast<QScrollBar *>(control)))
        control = control->parentWidget();
    if (!control || control->hasFocus() || control->isAncestorOf(QApplication::focusWidget()))
        return false;
    // An open combo list is a popup window of its own: its wheel scrolls the list.
    if (w->window() != control->window())
        return false;
    // Only inside something that scrolls; elsewhere the wheel keeps its meaning.
    for (QWidget *p = control->parentWidget(); p; p = p->parentWidget()) {
        if (auto *area = qobject_cast<QAbstractScrollArea *>(p)) {
            QCoreApplication::sendEvent(area->viewport(), event);
            return true;
        }
    }
    return false;
}
