#include "ContourLayer.h"

#include "ColorMaps.h"

#include <vtkActor.h>
#include <vtkAlgorithmOutput.h>
#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkFlyingEdges2D.h>
#include <vtkIdList.h>
#include <vtkImageAlgorithm.h>
#include <vtkImageData.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataAlgorithm.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkStripper.h>
#include <vtkTransform.h>
#include <vtkTrivialProducer.h>
#include <vtkUnsignedCharArray.h>

#include <QColor>
#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

using namespace Qt::StringLiterals;
namespace vc = visivo::contours;

// ── Smoothing stage ──
// Gaussian smoothing of a copy of the image. VTK's own vtkImageGaussianSmooth
// would spread a blanked (NaN) edge over its neighbours and shrink the field;
// vc::smoothImage() leaves the blanks as they are.
class vtkVisivoContourSmooth : public vtkImageAlgorithm
{
public:
    static vtkVisivoContourSmooth *New();
    vtkTypeMacro(vtkVisivoContourSmooth, vtkImageAlgorithm);

    /// The width of the Gaussian in world units; the pixels it is applied on
    /// are those of the input (its spacing), so a decimated image is smoothed
    /// by the same width as the full one.
    void setSigmaWorld(double sigma)
    {
        if (sigma != m_sigmaWorld) {
            m_sigmaWorld = sigma;
            Modified();
        }
    }

protected:
    vtkVisivoContourSmooth()
    {
        SetNumberOfInputPorts(1);
        SetNumberOfOutputPorts(1);
    }

    int RequestInformation(vtkInformation *, vtkInformationVector **,
                           vtkInformationVector *outputVector) override
    {
        vtkDataObject::SetPointDataActiveScalarInfo(outputVector->GetInformationObject(0),
                                                    VTK_FLOAT, 1);
        return 1;
    }

    int RequestUpdateExtent(vtkInformation *, vtkInformationVector **inputVector,
                            vtkInformationVector *) override
    {
        // The whole image: a Gaussian needs the neighbours.
        vtkInformation *in = inputVector[0]->GetInformationObject(0);
        in->Set(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(),
                in->Get(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT()), 6);
        return 1;
    }

    int RequestData(vtkInformation *, vtkInformationVector **inputVector,
                    vtkInformationVector *outputVector) override
    {
        auto *in = vtkImageData::GetData(inputVector[0]);
        auto *out = vtkImageData::GetData(outputVector);
        if (!in || !out)
            return 0;
        vtkDataArray *src = in->GetPointData()->GetScalars();
        if (!src)
            return 0;
        int ext[6];
        double spacing[3];
        in->GetExtent(ext);
        in->GetSpacing(spacing);
        out->SetExtent(ext);
        out->SetOrigin(in->GetOrigin());
        out->SetSpacing(spacing);
        out->AllocateScalars(VTK_FLOAT, 1);
        vtkDataArray *dst = out->GetPointData()->GetScalars();
        dst->SetName(src->GetName());
        const int w = ext[1] - ext[0] + 1, h = ext[3] - ext[2] + 1, d = ext[5] - ext[4] + 1;
        const std::size_t plane = std::size_t(w) * std::size_t(h);
        const double sx = spacing[0] != 0.0 ? m_sigmaWorld / std::abs(spacing[0]) : 0.0;
        const double sy = spacing[1] != 0.0 ? m_sigmaWorld / std::abs(spacing[1]) : 0.0;
        std::vector<float> pixels(plane);
        for (int k = 0; k < d; ++k) {
            for (std::size_t i = 0; i < plane; ++i)
                pixels[i] = float(src->GetComponent(vtkIdType(std::size_t(k) * plane + i), 0));
            vc::smoothImage(pixels.data(), w, h, sx, sy,
                            static_cast<float *>(dst->GetVoidPointer(0)) + std::size_t(k) * plane);
        }
        return 1;
    }

private:
    double m_sigmaWorld{ 0.0 };
};
vtkStandardNewMacro(vtkVisivoContourSmooth);

// ── Colouring stage ──
// Keeps the solid or the dashed polylines of the contour strips and gives each
// the colour of its level. A line's level is the scalar of its points, which
// the flying-edges filter writes as the contour value; the nearest level in the
// list picks the colour, so a rounding of the scalar cannot lose it.
class vtkVisivoContourColour : public vtkPolyDataAlgorithm
{
public:
    static vtkVisivoContourColour *New();
    vtkTypeMacro(vtkVisivoContourColour, vtkPolyDataAlgorithm);

    void configure(const std::vector<double> &levels,
                   const std::vector<std::array<unsigned char, 3>> &colours, bool dashed,
                   bool dashNegative)
    {
        m_levels = levels;
        m_colours = colours;
        m_dashed = dashed;
        m_dashNegative = dashNegative;
        Modified();
    }

protected:
    vtkVisivoContourColour()
    {
        SetNumberOfInputPorts(1);
        SetNumberOfOutputPorts(1);
    }

    int RequestData(vtkInformation *, vtkInformationVector **inputVector,
                    vtkInformationVector *outputVector) override
    {
        auto *in = vtkPolyData::GetData(inputVector[0]);
        auto *out = vtkPolyData::GetData(outputVector);
        if (!in || !out)
            return 0;
        out->Initialize();
        vtkDataArray *scalars = in->GetPointData()->GetScalars();
        vtkCellArray *lines = in->GetLines();
        if (!scalars || !lines || !in->GetPoints() || m_levels.empty() || m_colours.empty())
            return 1;
        const vtkIdType nPoints = in->GetNumberOfPoints();
        vtkNew<vtkCellArray> kept;
        vtkNew<vtkUnsignedCharArray> rgb;
        rgb->SetNumberOfComponents(3);
        rgb->SetName("Colours");
        rgb->SetNumberOfTuples(nPoints);
        rgb->FillValue(0);
        vtkNew<vtkIdList> ids;
        const vtkIdType nCells = lines->GetNumberOfCells();
        for (vtkIdType c = 0; c < nCells; ++c) {
            lines->GetCellAtId(c, ids);
            if (ids->GetNumberOfIds() < 2)
                continue;
            const double level = scalars->GetComponent(ids->GetId(0), 0);
            if ((m_dashNegative && level < 0.0) != m_dashed)
                continue;
            const int idx = vc::nearestLevel(m_levels, level);
            const auto &col =
                    m_colours[std::size_t(std::clamp<int>(idx, 0, int(m_colours.size()) - 1))];
            for (vtkIdType i = 0; i < ids->GetNumberOfIds(); ++i)
                rgb->SetTuple3(ids->GetId(i), col[0], col[1], col[2]);
            kept->InsertNextCell(ids);
        }
        out->SetPoints(in->GetPoints());
        out->SetLines(kept);
        out->GetPointData()->SetScalars(rgb);
        return 1;
    }

private:
    std::vector<double> m_levels;
    std::vector<std::array<unsigned char, 3>> m_colours;
    bool m_dashed{ false };
    bool m_dashNegative{ true };
};
vtkStandardNewMacro(vtkVisivoContourColour);

// ── The layer ──

ContourLayer::ContourLayer()
    : m_source(vtkSmartPointer<vtkTrivialProducer>::New()),
      m_smooth(vtkSmartPointer<vtkVisivoContourSmooth>::New()),
      m_edges(vtkSmartPointer<vtkFlyingEdges2D>::New()),
      m_strips(vtkSmartPointer<vtkStripper>::New()),
      m_solid(vtkSmartPointer<vtkVisivoContourColour>::New()),
      m_dashed(vtkSmartPointer<vtkVisivoContourColour>::New()),
      m_solidMapper(vtkSmartPointer<vtkPolyDataMapper>::New()),
      m_dashedMapper(vtkSmartPointer<vtkPolyDataMapper>::New()),
      m_solidActor(vtkSmartPointer<vtkActor>::New()),
      m_dashedActor(vtkSmartPointer<vtkActor>::New()),
      m_transform(vtkSmartPointer<vtkTransform>::New())
{
    m_edges->ComputeScalarsOn();
    // One polyline per line of the contour, not one cell per grid edge: a dash
    // pattern runs along a polyline, and a file wants whole lines.
    m_strips->SetJoinContiguousSegments(true);
    m_strips->SetMaximumLength(std::numeric_limits<int>::max());
    m_strips->SetInputConnection(m_edges->GetOutputPort());
    m_solid->SetInputConnection(m_strips->GetOutputPort());
    m_dashed->SetInputConnection(m_strips->GetOutputPort());
    const auto wire = [this](vtkPolyDataMapper *mapper, vtkVisivoContourColour *filter,
                             vtkActor *actor) {
        mapper->SetInputConnection(filter->GetOutputPort());
        mapper->SetColorModeToDirectScalars();
        mapper->ScalarVisibilityOn();
        mapper->SetScalarModeToUsePointData();
        actor->SetMapper(mapper);
        actor->GetProperty()->LightingOff();
        actor->SetUserTransform(m_transform);
        actor->VisibilityOff();
    };
    wire(m_solidMapper, m_solid, m_solidActor);
    wire(m_dashedMapper, m_dashed, m_dashedActor);
    m_dashedActor->GetProperty()->SetLineStipplePattern(0xF0F0);
    m_dashedActor->GetProperty()->SetLineStippleRepeatFactor(2);
    setSettings(m_settings);
}

ContourLayer::~ContourLayer()
{
    attach(nullptr);
}

void ContourLayer::attach(vtkRenderer *renderer)
{
    if (m_renderer == renderer)
        return;
    if (m_renderer) {
        m_renderer->RemoveViewProp(m_solidActor);
        m_renderer->RemoveViewProp(m_dashedActor);
    }
    m_renderer = renderer;
    if (m_renderer) {
        m_renderer->AddViewProp(m_solidActor);
        m_renderer->AddViewProp(m_dashedActor);
    }
}

void ContourLayer::setInputData(vtkImageData *image)
{
    m_source->SetOutput(image);
    m_source->Modified();
    m_inputPort = m_source->GetOutputPort();
    m_hasInput = image != nullptr;
    rebuildInput();
    setVisible(m_visible);
}

void ContourLayer::setInputConnection(vtkAlgorithmOutput *port)
{
    m_inputPort = port;
    m_hasInput = port != nullptr;
    rebuildInput();
    setVisible(m_visible);
}

void ContourLayer::rebuildInput()
{
    if (!m_inputPort)
        return;
    m_smooth->SetInputConnection(m_inputPort);
    m_edges->SetInputConnection(m_smoothing ? m_smooth->GetOutputPort() : m_inputPort);
    m_edges->Modified();
}

void ContourLayer::setUserTransform(vtkTransform *transform)
{
    m_transform->DeepCopy(transform);
}

void ContourLayer::setPixelScale(double worldPerPixel, double arcsecPerPixel)
{
    m_worldPerPixel = worldPerPixel > 0.0 ? worldPerPixel : 1.0;
    m_arcsecPerPixel = arcsecPerPixel > 0.0 ? arcsecPerPixel : 0.0;
}

QString ContourLayer::setSettings(const vc::Settings &settings)
{
    m_settings = settings;
    QString problem;
    m_levels = vc::levels(m_settings, &problem);

    // The smoothing: a width in pixels is the image's, one in arcsec needs the
    // sky size of a pixel.
    double sigmaPixels = std::max(0.0, m_settings.smoothing);
    if (m_settings.smoothingInArcsec && sigmaPixels > 0.0) {
        if (m_arcsecPerPixel > 0.0) {
            sigmaPixels /= m_arcsecPerPixel;
        } else {
            sigmaPixels = 0.0;
            if (problem.isEmpty())
                problem = u"The pixel size on the sky is not known: type the smoothing in pixels."_s;
        }
    }
    m_smooth->setSigmaWorld(sigmaPixels * m_worldPerPixel);
    const bool smoothing = sigmaPixels >= 0.05;
    if (smoothing != m_smoothing) {
        m_smoothing = smoothing;
        rebuildInput();
    }

    m_edges->SetNumberOfContours(int(m_levels.size()));
    for (std::size_t i = 0; i < m_levels.size(); ++i)
        m_edges->SetValue(int(i), m_levels[i]);
    m_edges->Modified();
    applyStyle();
    return problem;
}

void ContourLayer::applyStyle()
{
    const std::size_t n = m_levels.size();
    m_colours.assign(std::max<std::size_t>(n, 1), { 255, 102, 51 });
    const auto toRgb = [](const QColor &c) {
        return std::array<unsigned char, 3>{ static_cast<unsigned char>(c.red()),
                                             static_cast<unsigned char>(c.green()),
                                             static_cast<unsigned char>(c.blue()) };
    };
    QColor single(m_settings.colour);
    if (!single.isValid())
        single = QColor(255, 102, 51);
    switch (m_settings.colourMode) {
    case vc::ColourMode::Single:
        m_colours.assign(m_colours.size(), toRgb(single));
        break;
    case vc::ColourMode::Map: {
        vtkNew<vtkLookupTable> lut;
        ColorMaps::SetColorMap(lut, m_settings.colourMap.isEmpty()
                                            ? ColorMaps::DefaultColorMap
                                            : m_settings.colourMap.toStdString());
        lut->SetRange(0.0, 1.0);
        for (std::size_t i = 0; i < m_colours.size(); ++i) {
            // Along the levels, not along their values: the colours of a
            // logarithmic or a typed list are as far apart as its steps.
            const double t = n > 1 ? double(i) / double(n - 1) : 0.5;
            double rgb[3];
            lut->GetColor(t, rgb);
            m_colours[i] = { static_cast<unsigned char>(std::lround(255.0 * rgb[0])),
                             static_cast<unsigned char>(std::lround(255.0 * rgb[1])),
                             static_cast<unsigned char>(std::lround(255.0 * rgb[2])) };
        }
        break;
    }
    case vc::ColourMode::PerLevel: {
        std::vector<std::array<unsigned char, 3>> typed;
        for (const QString &name : vc::colourList(m_settings.perLevel)) {
            const QColor c(name);
            if (c.isValid())
                typed.push_back(toRgb(c));
        }
        if (typed.empty())
            typed.push_back(toRgb(single));
        for (std::size_t i = 0; i < m_colours.size(); ++i)
            m_colours[i] = typed[i % typed.size()];
        break;
    }
    }
    m_solid->configure(m_levels, m_colours, /*dashed=*/false, m_settings.dashNegative);
    m_dashed->configure(m_levels, m_colours, /*dashed=*/true, m_settings.dashNegative);
    const double width = std::clamp(m_settings.lineWidth, 0.5, 20.0);
    m_solidActor->GetProperty()->SetLineWidth(width);
    m_dashedActor->GetProperty()->SetLineWidth(width);
}

void ContourLayer::setVisible(bool visible)
{
    m_visible = visible;
    // Nothing to draw before there is an image: the pipeline would only complain.
    const bool on = visible && m_hasInput;
    m_solidActor->SetVisibility(on);
    m_dashedActor->SetVisibility(on);
}

std::vector<vc::Line> ContourLayer::lines(
        const std::function<std::array<double, 2>(double, double)> &worldToPixel) const
{
    std::vector<vc::Line> out;
    if (!m_hasInput || m_levels.empty())
        return out;
    m_strips->Update();
    vtkPolyData *poly = m_strips->GetOutput();
    vtkDataArray *scalars = poly ? poly->GetPointData()->GetScalars() : nullptr;
    vtkCellArray *cells = poly ? poly->GetLines() : nullptr;
    if (!scalars || !cells || !poly->GetPoints())
        return out;
    vtkNew<vtkIdList> ids;
    const vtkIdType nCells = cells->GetNumberOfCells();
    for (vtkIdType c = 0; c < nCells; ++c) {
        cells->GetCellAtId(c, ids);
        const vtkIdType n = ids->GetNumberOfIds();
        if (n < 2)
            continue;
        vc::Line line;
        const int idx = vc::nearestLevel(m_levels, scalars->GetComponent(ids->GetId(0), 0));
        line.level = m_levels[std::size_t(idx)];
        line.dashed = m_settings.dashNegative && line.level < 0.0;
        const auto &rgb = m_colours[std::min<std::size_t>(std::size_t(idx), m_colours.size() - 1)];
        line.colour = QColor(rgb[0], rgb[1], rgb[2]).name();
        line.closed = ids->GetId(0) == ids->GetId(n - 1);
        line.pixels.reserve(std::size_t(n));
        for (vtkIdType i = 0; i < n; ++i) {
            double p[3], q[3];
            poly->GetPoint(ids->GetId(i), p);
            // The layer's own turn first (an image aligned to another one).
            m_transform->TransformPoint(p, q);
            line.pixels.push_back(worldToPixel(q[0], q[1]));
        }
        out.push_back(std::move(line));
    }
    return out;
}

double ContourLayer::noiseOf(vtkImageData *image)
{
    vtkDataArray *scalars = image ? image->GetPointData()->GetScalars() : nullptr;
    if (!scalars || scalars->GetNumberOfComponents() != 1)
        return 0.0;
    const std::size_t n = std::size_t(scalars->GetNumberOfTuples());
    constexpr std::size_t kMax = 1u << 20;
    const std::size_t stride = std::max<std::size_t>(1, n / kMax);
    std::vector<double> sample;
    sample.reserve(n / stride + 1);
    for (std::size_t i = 0; i < n; i += stride)
        sample.push_back(scalars->GetComponent(vtkIdType(i), 0));
    return vc::robustSigma(std::move(sample));
}
