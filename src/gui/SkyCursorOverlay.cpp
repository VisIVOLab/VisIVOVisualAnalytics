#include "SkyCursorOverlay.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>

SkyCursorOverlay::SkyCursorOverlay()
    : m_points(vtkSmartPointer<vtkPoints>::New()),
      m_data(vtkSmartPointer<vtkPolyData>::New()),
      m_actor(vtkSmartPointer<vtkActor>::New())
{
    m_points->SetNumberOfPoints(4);
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    const vtkIdType horizontal[2] = { 0, 1 }, vertical[2] = { 2, 3 };
    lines->InsertNextCell(2, horizontal);
    lines->InsertNextCell(2, vertical);
    m_data->SetPoints(m_points);
    m_data->SetLines(lines);
    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(m_data);
    m_actor->SetMapper(mapper);
    m_actor->GetProperty()->SetColor(1.0, 0.55, 0.1);
    m_actor->GetProperty()->SetLineWidth(1.5);
    m_actor->GetProperty()->SetOpacity(0.85);
    // The cursor is not data: it must not move the camera's clipping range, nor
    // be picked by a click meant for the image.
    m_actor->PickableOff();
    m_actor->VisibilityOff();
}

SkyCursorOverlay::~SkyCursorOverlay()
{
    detach();
}

void SkyCursorOverlay::attach(vtkRenderer *renderer)
{
    if (!renderer || m_renderer == renderer)
        return;
    detach();
    renderer->AddActor(m_actor);
    m_renderer = renderer;
}

void SkyCursorOverlay::detach()
{
    if (m_renderer)
        m_renderer->RemoveActor(m_actor);
    m_renderer = nullptr;
}

void SkyCursorOverlay::show(double x, double y, double xmin, double xmax, double ymin, double ymax)
{
    m_points->SetPoint(0, xmin, y, 0.0);
    m_points->SetPoint(1, xmax, y, 0.0);
    m_points->SetPoint(2, x, ymin, 0.0);
    m_points->SetPoint(3, x, ymax, 0.0);
    m_points->Modified();
    m_data->Modified();
    m_actor->VisibilityOn();
}

void SkyCursorOverlay::hide()
{
    m_actor->VisibilityOff();
}

bool SkyCursorOverlay::shown() const
{
    return m_actor->GetVisibility() != 0;
}
