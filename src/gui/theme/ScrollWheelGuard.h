#pragma once

#include <QObject>

/// Application-wide: a wheel turned over a spin box, combo box or slider that does
/// not have the keyboard focus, inside a scroll area, scrolls the area instead of
/// changing the value. Without it, scrolling a long settings page (the Inspector's
/// Parameters, a tool window) silently edits whatever control passes under the
/// cursor — a fitted V_sys became 1081 instead of 1120 that way. A control the
/// user has clicked (focused) takes the wheel as before; the wheel itself no
/// longer gives the focus (WheelFocus becomes StrongFocus when it is polished).
class ScrollWheelGuard : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};
