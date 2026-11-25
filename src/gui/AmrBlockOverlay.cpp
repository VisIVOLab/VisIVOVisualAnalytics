#include "AmrBlockOverlay.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkUnsignedCharArray.h>

namespace {
/// Above the slice's own plane, so the lines are not lost in the image.
constexpr double kLineZ = 0.1;
}   // namespace

AmrBlockOverlay::AmrBlockOverlay()
    : m_data(vtkSmartPointer<vtkPolyData>::New()), m_actor(vtkSmartPointer<vtkActor>::New())
{
    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(m_data);
    mapper->SetColorModeToDirectScalars();
    mapper->SetScalarModeToUseCellData();
    mapper->ScalarVisibilityOn();
    m_actor->SetMapper(mapper);
    m_actor->GetProperty()->SetLineWidth(1.0);
    m_actor->GetProperty()->SetLighting(false);
    // Not data: it must not be picked by a click meant for the image.
    m_actor->PickableOff();
    m_actor->VisibilityOff();
}

AmrBlockOverlay::~AmrBlockOverlay()
{
    detach();
}

void AmrBlockOverlay::attach(vtkRenderer *renderer)
{
    if (!renderer || m_renderer == renderer)
        return;
    detach();
    renderer->AddActor(m_actor);
    m_renderer = renderer;
}

void AmrBlockOverlay::detach()
{
    if (m_renderer)
        m_renderer->RemoveActor(m_actor);
    m_renderer = nullptr;
}

void AmrBlockOverlay::show(const QVector<Group> &groups)
{
    vtkIdType rects = 0;
    for (const Group &g : groups)
        rects += g.rects.size();

    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetNumberOfPoints(4 * rects);
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    lines->AllocateEstimate(4 * rects, 2);
    auto colours = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colours->SetNumberOfComponents(3);
    colours->SetNumberOfTuples(4 * rects);

    vtkIdType p = 0, c = 0;
    for (const Group &g : groups) {
        for (const auto &r : g.rects) {
            points->SetPoint(p, r[0], r[1], kLineZ);
            points->SetPoint(p + 1, r[2], r[1], kLineZ);
            points->SetPoint(p + 2, r[2], r[3], kLineZ);
            points->SetPoint(p + 3, r[0], r[3], kLineZ);
            for (int e = 0; e < 4; ++e) {
                const vtkIdType edge[2] = { p + e, p + (e + 1) % 4 };
                lines->InsertNextCell(2, edge);
                colours->SetTuple3(c++, g.rgb[0], g.rgb[1], g.rgb[2]);
            }
            p += 4;
        }
    }
    m_data->SetPoints(points);
    m_data->SetLines(lines);
    m_data->GetCellData()->SetScalars(colours);
    m_data->Modified();
    m_actor->SetVisibility(rects > 0);
}

void AmrBlockOverlay::hide()
{
    m_actor->VisibilityOff();
}

bool AmrBlockOverlay::shown() const
{
    return m_actor->GetVisibility() != 0;
}
