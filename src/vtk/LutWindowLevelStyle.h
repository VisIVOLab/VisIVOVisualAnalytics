#ifndef LutWindowLevelStyle_h
#define LutWindowLevelStyle_h

#include <functional>

#include <vtkInteractorStyleImage.h>

/**
 * Interactor style of a 2-D image whose left-drag rewrites the colour range of
 * the lookup table (the one the colour bar and the range fields show) instead of
 * vtkInteractorStyleImage's window/level of the image property.
 *
 * The property's window/level is a post-scale of the already coloured RGBA that
 * neither the colour bar nor the range fields see: a drag left the display in a
 * state nothing on screen described, and nothing but an undocumented key took it
 * back. Here drag, colour bar and range fields are one state. Pan (middle) and
 * zoom (wheel, right) are the base class's.
 */
class LutWindowLevelStyle : public vtkInteractorStyleImage
{
public:
    static LutWindowLevelStyle *New();
    vtkTypeMacro(LutWindowLevelStyle, vtkInteractorStyleImage);

    /// The range the drag starts from.
    std::function<double()> getLo, getHi;
    /// Receives each new range while the drag goes on.
    std::function<void(double, double)> setRange;
    /// Called once when a drag ends, so the owner can refresh what shows the range.
    std::function<void()> dragFinished;
    /// The 'r' key: back to the initial display. Without it the key is the base class's.
    std::function<void()> resetDisplay;

    void OnLeftButtonDown() override;
    void OnLeftButtonUp() override;
    void OnMouseMove() override;
    void OnChar() override;

private:
    int m_x0{ 0 }, m_y0{ 0 };
    double m_lo0{ 0. }, m_hi0{ 1. };
    bool m_dragging{ false };
    bool m_moved{ false };   ///< the range changed during this press
};

#endif
