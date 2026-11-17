#include "RegionListOverlay.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkContourTriangulator.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkCoordinate.h>
#include <vtkRenderer.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkUnsignedCharArray.h>

#include <QColor>

#include <algorithm>

using visivo::regions::Region;
using visivo::regions::Vec2;

namespace {

constexpr unsigned char kDefaultColour[3] = { 51, 230, 255 };

/// Append closed loops as polylines, colouring each cell when `colours` is given.
void appendLoops(const std::vector<std::vector<Vec2>> &loops, const RegionListOverlay::ToWorld &toWorld,
                 vtkPoints *points, vtkCellArray *lines, vtkUnsignedCharArray *colours,
                 const std::array<unsigned char, 3> &rgb, bool closed = true)
{
    for (const auto &loop : loops) {
        if (loop.size() < 2)
            continue;
        const vtkIdType first = points->GetNumberOfPoints();
        for (const Vec2 &p : loop) {
            const auto w = toWorld(p);
            points->InsertNextPoint(w[0], w[1], w[2]);
        }
        lines->InsertNextCell(static_cast<vtkIdType>(loop.size() + (closed ? 1 : 0)));
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(loop.size()); ++i)
            lines->InsertCellPoint(first + i);
        if (closed)
            lines->InsertCellPoint(first);
        if (colours)
            colours->InsertNextTypedTuple(rgb.data());
    }
}

}   // namespace

std::array<unsigned char, 3> RegionListOverlay::colour(const Region &r)
{
    const QColor c(r.color);
    if (r.color.isEmpty() || !c.isValid())
        return { kDefaultColour[0], kDefaultColour[1], kDefaultColour[2] };
    return { static_cast<unsigned char>(c.red()), static_cast<unsigned char>(c.green()),
             static_cast<unsigned char>(c.blue()) };
}

RegionListOverlay::RegionListOverlay()
    : m_listData(vtkSmartPointer<vtkPolyData>::New())
    , m_listActor(vtkSmartPointer<vtkActor>::New())
    , m_selectedData(vtkSmartPointer<vtkPolyData>::New())
    , m_selectedActor(vtkSmartPointer<vtkActor>::New())
    , m_fillData(vtkSmartPointer<vtkPolyData>::New())
    , m_fillTriangulator(vtkSmartPointer<vtkContourTriangulator>::New())
    , m_fillActor(vtkSmartPointer<vtkActor>::New())
    , m_pointData(vtkSmartPointer<vtkPolyData>::New())
    , m_pointActor(vtkSmartPointer<vtkActor>::New())
    , m_handleData(vtkSmartPointer<vtkPolyData>::New())
    , m_handleActor(vtkSmartPointer<vtkActor>::New())
{
}

RegionListOverlay::~RegionListOverlay()
{
    detach();
}

void RegionListOverlay::attach(vtkRenderer *renderer, const Style &style)
{
    if (!renderer || m_renderer)
        return;
    m_renderer = renderer;
    m_style = style;
    const auto offset = [&style](vtkPolyDataMapper *mapper) {
        if (style.lineOffsetUnits != 0.0) {
            mapper->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -style.lineOffsetUnits);
            mapper->SetRelativeCoincidentTopologyPointOffsetParameter(-style.lineOffsetUnits);
            mapper->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, -style.lineOffsetUnits);
        }
    };
    const auto lineActor = [&](vtkPolyData *data, vtkActor *actor, double width) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(data);
        mapper->SetScalarModeToUseCellData();
        mapper->SetColorModeToDirectScalars();
        offset(mapper);
        actor->SetMapper(mapper);
        actor->GetProperty()->SetLineWidth(width);
        actor->GetProperty()->LightingOff();
        actor->PickableOff();
        renderer->AddActor(actor);
    };
    lineActor(m_listData, m_listActor, style.lineWidth);
    lineActor(m_selectedData, m_selectedActor, style.selectedLineWidth);

    m_fillTriangulator->SetInputData(m_fillData);
    vtkNew<vtkPolyDataMapper> fillMapper;
    fillMapper->SetInputConnection(m_fillTriangulator->GetOutputPort());
    fillMapper->ScalarVisibilityOff();
    offset(fillMapper);
    m_fillActor->SetMapper(fillMapper);
    m_fillActor->GetProperty()->SetOpacity(0.15);
    m_fillActor->GetProperty()->LightingOff();
    m_fillActor->PickableOff();
    m_fillActor->VisibilityOff();
    renderer->AddActor(m_fillActor);

    // Point regions: a marker of fixed size on screen, in the region's colour.
    lineActor(m_pointData, m_pointActor, 1.0);
    m_pointActor->GetProperty()->SetPointSize(style.pointSize);

    // Handles of the selected region: white squares of fixed size on screen.
    vtkNew<vtkPolyDataMapper> handleMapper;
    handleMapper->SetInputData(m_handleData);
    handleMapper->ScalarVisibilityOff();
    offset(handleMapper);
    m_handleActor->SetMapper(handleMapper);
    m_handleActor->GetProperty()->SetColor(1.0, 1.0, 1.0);
    m_handleActor->GetProperty()->SetPointSize(style.handleSize);
    m_handleActor->GetProperty()->LightingOff();
    m_handleActor->PickableOff();
    renderer->AddActor(m_handleActor);
    setVisible(m_visible);
}

void RegionListOverlay::detach()
{
    if (!m_renderer) {
        m_labels.clear();
        m_labelsShown = 0;
        return;
    }
    for (vtkActor *a : { m_listActor.Get(), m_selectedActor.Get(), m_fillActor.Get(),
                         m_pointActor.Get(), m_handleActor.Get() })
        m_renderer->RemoveActor(a);
    for (const auto &t : m_labels)
        m_renderer->RemoveViewProp(t);
    m_labels.clear();
    m_labelsShown = 0;
    m_renderer = nullptr;
}

void RegionListOverlay::setVisible(bool visible)
{
    m_visible = visible;
    for (vtkActor *a : { m_listActor.Get(), m_selectedActor.Get(), m_pointActor.Get(),
                         m_handleActor.Get() })
        a->SetVisibility(visible);
    m_fillActor->SetVisibility(visible && m_haveFill);
    for (int i = 0; i < int(m_labels.size()); ++i)
        m_labels[std::size_t(i)]->SetVisibility(visible && i < m_labelsShown);
}

void RegionListOverlay::update(const visivo::regions::RegionList &list, double knobOffset,
                               const ToWorld &toWorldIn, bool withHandles)
{
    const ToWorld toWorld = toWorldIn ? toWorldIn : ToWorld([](const Vec2 &p) {
        return std::array<double, 3>{ p[0], p[1], 0.0 };
    });
    vtkNew<vtkPoints> listPoints;
    vtkNew<vtkCellArray> listLines;
    vtkNew<vtkUnsignedCharArray> listColours;
    listColours->SetNumberOfComponents(3);
    vtkNew<vtkPoints> selPoints;
    vtkNew<vtkCellArray> selLines;
    vtkNew<vtkUnsignedCharArray> selColours;
    selColours->SetNumberOfComponents(3);
    vtkNew<vtkPoints> markerPoints;
    vtkNew<vtkCellArray> markerCells;
    vtkNew<vtkUnsignedCharArray> markerColours;
    markerColours->SetNumberOfComponents(3);
    vtkNew<vtkPoints> handlePoints;
    vtkNew<vtkCellArray> handleCells;

    // A hidden region (#388) is not drawn, selected or not: no outline, no
    // handles, no fill.
    const Region *sel = list.selected();
    if (sel && !sel->visible)
        sel = nullptr;
    const int selectedId = sel ? sel->id : -1;
    // A label: a text region's string at its position, or the text a region
    // file gave another region, just above it (as ds9 places it).
    int labels = 0;
    const auto label = [&](const QString &text, const Vec2 &at, const std::array<unsigned char, 3> &rgb,
                           bool selected) {
        if (text.isEmpty() || !m_renderer)
            return;
        if (labels >= int(m_labels.size())) {
            auto t = vtkSmartPointer<vtkTextActor>::New();
            t->GetPositionCoordinate()->SetCoordinateSystemToWorld();
            t->GetTextProperty()->SetJustificationToCentered();
            t->GetTextProperty()->SetVerticalJustificationToBottom();
            t->GetTextProperty()->SetFontSize(13);
            t->GetTextProperty()->ShadowOn();
            t->PickableOff();
            m_renderer->AddViewProp(t);
            m_labels.push_back(t);
        }
        vtkTextActor *t = m_labels[std::size_t(labels++)];
        t->SetInput(text.toUtf8().constData());
        const auto w = toWorld(at);
        t->GetPositionCoordinate()->SetValue(w[0], w[1], w[2]);
        t->GetTextProperty()->SetColor(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0);
        t->GetTextProperty()->SetBold(selected);
    };
    for (const Region &r : list.regions()) {
        if (!r.visible)
            continue;
        const auto rgb = colour(r);
        const bool isSel = r.id == selectedId;
        if (r.shape == visivo::regions::Shape::Text) {
            label(r.text, r.center, rgb, isSel);
        } else {
            const QString fileLabel = r.props.value(QStringLiteral("text")).toString();
            if (!fileLabel.isEmpty()) {
                const auto b = visivo::regions::bounds(r);
                label(fileLabel, { (b[0] + b[1]) / 2.0, b[3] }, rgb, isSel);
            }
        }
        const auto paths = visivo::regions::openPaths(r);
        for (const auto &path : paths)
            appendLoops({ path }, toWorld, isSel ? selPoints.Get() : listPoints.Get(),
                        isSel ? selLines.Get() : listLines.Get(),
                        isSel ? selColours.Get() : listColours.Get(), rgb, /*closed=*/false);
        if (r.shape == visivo::regions::Shape::Point) {
            const auto w = toWorld(r.center);
            const vtkIdType id = markerPoints->InsertNextPoint(w[0], w[1], w[2]);
            markerCells->InsertNextCell(1, &id);
            markerColours->InsertNextTypedTuple(rgb.data());
        }
        if (r.id == selectedId)
            appendLoops(visivo::regions::outlines(r), toWorld, selPoints, selLines, selColours, rgb);
        else
            appendLoops(visivo::regions::outlines(r), toWorld, listPoints, listLines, listColours, rgb);
    }

    m_listData->Initialize();
    m_listData->SetPoints(listPoints);
    m_listData->SetLines(listLines);
    m_listData->GetCellData()->SetScalars(listColours);
    m_pointData->Initialize();
    m_pointData->SetPoints(markerPoints);
    m_pointData->SetVerts(markerCells);
    m_pointData->GetCellData()->SetScalars(markerColours);

    if (sel && withHandles) {
        // The knob hangs on a short stem from the edge it turns.
        for (const auto &h : visivo::regions::handles(*sel, knobOffset)) {
            const auto w = toWorld(h.pos);
            const vtkIdType id = handlePoints->InsertNextPoint(w[0], w[1], w[2]);
            handleCells->InsertNextCell(1, &id);
            if (h.kind == visivo::regions::HandleKind::Rotate) {
                const Vec2 base = visivo::regions::toWorld(*sel, { 0.0, visivo::regions::knobEdge(*sel) });
                appendLoops({ { base, h.pos } }, toWorld, selPoints, selLines, selColours,
                            colour(*sel), /*closed=*/false);
            }
        }
        if (sel->shape == visivo::regions::Shape::Point || sel->shape == visivo::regions::Shape::Text) {
            const auto w = toWorld(sel->center);
            const vtkIdType id = handlePoints->InsertNextPoint(w[0], w[1], w[2]);
            handleCells->InsertNextCell(1, &id);
        }
    }
    m_selectedData->Initialize();
    m_selectedData->SetPoints(selPoints);
    m_selectedData->SetLines(selLines);
    m_selectedData->GetCellData()->SetScalars(selColours);
    m_handleData->Initialize();
    m_handleData->SetPoints(handlePoints);
    m_handleData->SetVerts(handleCells);

    // A light fill says which region is selected; an annulus is filled between
    // its rings (the inner loop runs the other way round, a hole).
    m_fillData->Initialize();
    m_haveFill = false;
    if (m_style.fill && sel && visivo::regions::hasArea(sel->shape)
        && sel->shape != visivo::regions::Shape::Point) {
        auto loops = visivo::regions::outlines(*sel);
        if (loops.size() > 1)
            std::reverse(loops[1].begin(), loops[1].end());
        vtkNew<vtkPoints> fp;
        vtkNew<vtkCellArray> fl;
        appendLoops(loops, toWorld, fp, fl, nullptr, {});
        m_fillData->SetPoints(fp);
        m_fillData->SetLines(fl);
        const auto rgb = colour(*sel);
        m_fillActor->GetProperty()->SetColor(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0);
        m_haveFill = fl->GetNumberOfCells() > 0;
    }
    m_fillData->Modified();
    m_fillActor->SetVisibility(m_visible && m_haveFill);
    m_labelsShown = labels;
    for (int i = 0; i < int(m_labels.size()); ++i)
        m_labels[std::size_t(i)]->SetVisibility(m_visible && i < m_labelsShown);
    m_listData->Modified();
    m_selectedData->Modified();
    m_pointData->Modified();
    m_handleData->Modified();
}
