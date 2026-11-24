#ifndef SkyCursorOverlay_h
#define SkyCursorOverlay_h

// SkyCursorOverlay.h
// The crosshair a linked window shows where the other window's cursor is
// (issue #522): two thin lines across the image through one world point. It is
// not the pixel probe's crosshair (yellow, the user's own cursor in this
// window); this one is orange and always belongs to a peer.

#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

class vtkActor;
class vtkPoints;
class vtkPolyData;
class vtkRenderer;

class SkyCursorOverlay
{
public:
    SkyCursorOverlay();
    ~SkyCursorOverlay();
    SkyCursorOverlay(const SkyCursorOverlay &) = delete;
    SkyCursorOverlay &operator=(const SkyCursorOverlay &) = delete;

    /// Add the actor to @p renderer (once; a different renderer moves it).
    void attach(vtkRenderer *renderer);
    void detach();
    /// Draw the cross through world (x, y), spanning [xmin, xmax] x [ymin, ymax].
    void show(double x, double y, double xmin, double xmax, double ymin, double ymax);
    void hide();
    bool shown() const;

private:
    vtkSmartPointer<vtkPoints> m_points;
    vtkSmartPointer<vtkPolyData> m_data;
    vtkSmartPointer<vtkActor> m_actor;
    vtkWeakPointer<vtkRenderer> m_renderer;
};

#endif   // SkyCursorOverlay_h
