#include "LutWindowLevelStyle.h"

#include "WindowLevelDrag.h"

#include <QGuiApplication>

#include <vtkObjectFactory.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

vtkStandardNewMacro(LutWindowLevelStyle);

// The left button is NOT passed to the base class: its OnLeftButtonDown starts
// the image-property window/level this style exists to replace. (The macOS
// trackpad's phantom presses are dealt with by the widget, which turns touch
// processing off, so a drag here is a deliberate one.)
void LutWindowLevelStyle::OnLeftButtonDown()
{
    if (!setRange || !this->GetInteractor())
        return;
    const int *p = this->GetInteractor()->GetEventPosition();
    m_x0 = p[0];
    m_y0 = p[1];
    m_lo0 = getLo ? getLo() : 0.;
    m_hi0 = getHi ? getHi() : 1.;
    m_dragging = true;
    m_moved = false;
}

void LutWindowLevelStyle::OnLeftButtonUp()
{
    const bool changed = m_dragging && m_moved;
    m_dragging = false;
    m_moved = false;
    // A plain click (a ruler point, a probe) is not a drag: nothing to refresh.
    if (changed && dragFinished)
        dragFinished();
}

void LutWindowLevelStyle::OnMouseMove()
{
    if (!m_dragging) {
        Superclass::OnMouseMove();   // pan/zoom in progress, hover: the base class's
        return;
    }
    // A release the widget never saw (the pointer left the window, a dialog took
    // the grab) would leave the drag running on plain hovering.
    if (!(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        m_dragging = false;
        m_moved = false;
        Superclass::OnMouseMove();
        return;
    }
    const int *p = this->GetInteractor()->GetEventPosition();
    int size[2] = { 1, 1 };
    if (auto *rw = this->GetInteractor()->GetRenderWindow())
        if (const int *s = rw->GetSize()) {
            size[0] = s[0];
            size[1] = s[1];
        }
    const auto range = WindowLevelDrag::rangeAfterDrag(m_lo0, m_hi0, p[0] - m_x0, p[1] - m_y0,
                                                       size[0], size[1]);
    m_moved = true;
    setRange(range[0], range[1]);
}

void LutWindowLevelStyle::OnChar()
{
    const char key = this->GetInteractor() ? this->GetInteractor()->GetKeyCode() : 0;
    if ((key == 'r' || key == 'R') && resetDisplay) {
        resetDisplay();
        return;
    }
    Superclass::OnChar();
}
