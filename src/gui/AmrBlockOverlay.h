#ifndef AmrBlockOverlay_h
#define AmrBlockOverlay_h

// AmrBlockOverlay.h
// The cells of an AMR mesh that a slice cuts (issue #550): one outlined
// rectangle per cell, coloured by its refinement level, drawn over the 2-D
// slice. The window gives them already in the slice's world; this only draws.

#include <QVector>

#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <array>

class vtkActor;
class vtkPoints;
class vtkPolyData;
class vtkRenderer;

class AmrBlockOverlay
{
public:
    /// The rectangles of one level, [x0, y0, x1, y1] in world, and their colour.
    struct Group
    {
        std::array<unsigned char, 3> rgb{ 255, 255, 255 };
        QVector<std::array<double, 4>> rects;
    };

    AmrBlockOverlay();
    ~AmrBlockOverlay();
    AmrBlockOverlay(const AmrBlockOverlay &) = delete;
    AmrBlockOverlay &operator=(const AmrBlockOverlay &) = delete;

    /// Add the actor to @p renderer (once; a different renderer moves it).
    void attach(vtkRenderer *renderer);
    void detach();
    /// Draw @p groups, replacing what was drawn.
    void show(const QVector<Group> &groups);
    void hide();
    bool shown() const;

private:
    vtkSmartPointer<vtkPolyData> m_data;
    vtkSmartPointer<vtkActor> m_actor;
    vtkWeakPointer<vtkRenderer> m_renderer;
};

#endif   // AmrBlockOverlay_h
