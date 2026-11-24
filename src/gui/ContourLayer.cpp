#include "ContourLayer.h"

#include "ColorMaps.h"

#include <vtkActor.h>
#include <vtkAlgorithmOutput.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkFlyingEdges2D.h>
#include <vtkFloatArray.h>
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
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkTransform.h>
#include <vtkTrivialProducer.h>
#include <vtkUnsignedCharArray.h>

#include <QColor>
#include <QFutureWatcher>
#include <QString>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

using namespace Qt::StringLiterals;
namespace vc = visivo::contours;

// ── Thinning stage ──
// Every n-th pixel of an image that is too large to contour in a blink: the
// first contours of a 100-megapixel image would otherwise take as long as
// reading it. The spacing grows by n, so the lines land where the full image's
// would, and the smoothing that follows is still by the same width in the sky.
class vtkVisivoContourDecimate : public vtkImageAlgorithm
{
public:
    static vtkVisivoContourDecimate *New();
    vtkTypeMacro(vtkVisivoContourDecimate, vtkImageAlgorithm);

    void setMaxPixels(long long pixels)
    {
        pixels = std::max(1LL, pixels);
        if (pixels != m_maxPixels) {
            m_maxPixels = pixels;
            Modified();
        }
    }
    /// Of the last RequestInformation: 1 = the image as it is.
    int stride() const { return m_stride; }
    long long inputPixels() const { return m_inputPixels; }

protected:
    vtkVisivoContourDecimate()
    {
        SetNumberOfInputPorts(1);
        SetNumberOfOutputPorts(1);
    }

    int RequestInformation(vtkInformation *, vtkInformationVector **inputVector,
                           vtkInformationVector *outputVector) override
    {
        vtkInformation *in = inputVector[0]->GetInformationObject(0);
        vtkInformation *out = outputVector->GetInformationObject(0);
        int ext[6];
        in->Get(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(), ext);
        const long long w = ext[1] - ext[0] + 1, h = ext[3] - ext[2] + 1;
        m_inputPixels = w * h;
        const int stride = vc::thinStride(w, h, m_maxPixels);
        m_stride = stride;
        if (stride == 1)
            return 1;   // the output information is the input's
        double spacing[3], origin[3];
        in->Get(vtkDataObject::SPACING(), spacing);
        in->Get(vtkDataObject::ORIGIN(), origin);
        const int outExt[6] = { 0, int((w + stride - 1) / stride) - 1,
                                0, int((h + stride - 1) / stride) - 1, 0, 0 };
        out->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(), outExt, 6);
        const double outSpacing[3] = { spacing[0] * stride, spacing[1] * stride, spacing[2] };
        out->Set(vtkDataObject::SPACING(), outSpacing, 3);
        const double outOrigin[3] = { origin[0] + ext[0] * spacing[0],
                                      origin[1] + ext[2] * spacing[1], origin[2] };
        out->Set(vtkDataObject::ORIGIN(), outOrigin, 3);
        vtkDataObject::SetPointDataActiveScalarInfo(out, VTK_FLOAT, 1);
        return 1;
    }

    int RequestUpdateExtent(vtkInformation *, vtkInformationVector **inputVector,
                            vtkInformationVector *) override
    {
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
        if (m_stride == 1) {
            out->ShallowCopy(in);
            return 1;
        }
        vtkDataArray *src = in->GetPointData()->GetScalars();
        if (!src || src->GetNumberOfComponents() != 1)
            return 0;
        int ext[6];
        in->GetExtent(ext);
        const int w = ext[1] - ext[0] + 1, h = ext[3] - ext[2] + 1;
        const int ow = (w + m_stride - 1) / m_stride, oh = (h + m_stride - 1) / m_stride;
        double spacing[3];
        in->GetSpacing(spacing);
        out->SetExtent(0, ow - 1, 0, oh - 1, 0, 0);
        out->SetSpacing(spacing[0] * m_stride, spacing[1] * m_stride, spacing[2]);
        out->SetOrigin(in->GetOrigin()[0] + ext[0] * spacing[0],
                       in->GetOrigin()[1] + ext[2] * spacing[1], in->GetOrigin()[2]);
        out->AllocateScalars(VTK_FLOAT, 1);
        vtkDataArray *dst = out->GetPointData()->GetScalars();
        dst->SetName(src->GetName());
        auto *to = static_cast<float *>(dst->GetVoidPointer(0));
        const auto *srcFloat = vtkFloatArray::SafeDownCast(src);
        for (int y = 0; y < oh; ++y) {
            const std::size_t row = std::size_t(y) * std::size_t(m_stride) * std::size_t(w);
            for (int x = 0; x < ow; ++x) {
                const std::size_t at = row + std::size_t(x) * std::size_t(m_stride);
                to[std::size_t(y) * std::size_t(ow) + std::size_t(x)] =
                        srcFloat ? srcFloat->GetValue(vtkIdType(at))
                                 : float(src->GetComponent(vtkIdType(at), 0));
            }
        }
        return 1;
    }

private:
    long long m_maxPixels{ ContourLayer::kDefaultMaxPixels };
    int m_stride{ 1 };
    long long m_inputPixels{ 0 };
};
vtkStandardNewMacro(vtkVisivoContourDecimate);

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
// Keeps the solid segments of the contour (all of them, or those of the levels
// at or above zero when the negative ones are dashed) and gives each the colour
// of its level. A segment's level is the scalar of its points, which the
// flying-edges filter writes as the contour value; the nearest level in the
// list picks the colour, so a rounding of the scalar cannot lose it.
class vtkVisivoContourColour : public vtkPolyDataAlgorithm
{
public:
    static vtkVisivoContourColour *New();
    vtkTypeMacro(vtkVisivoContourColour, vtkPolyDataAlgorithm);

    void configure(const std::vector<double> &levels,
                   const std::vector<std::array<unsigned char, 3>> &colours, bool dashNegative)
    {
        m_levels = levels;
        m_colours = colours;
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
        vtkNew<vtkUnsignedCharArray> rgb;
        rgb->SetNumberOfComponents(3);
        rgb->SetName("Colours");
        rgb->SetNumberOfTuples(nPoints);
        for (vtkIdType i = 0; i < nPoints; ++i) {
            const int idx = vc::nearestLevel(m_levels, scalars->GetComponent(i, 0));
            const auto &col =
                    m_colours[std::size_t(std::clamp<int>(idx, 0, int(m_colours.size()) - 1))];
            rgb->SetTuple3(i, col[0], col[1], col[2]);
        }
        out->SetPoints(in->GetPoints());
        if (!m_dashNegative || m_levels.front() >= 0.0) {
            out->SetLines(lines);   // nothing to leave out
        } else {
            vtkNew<vtkCellArray> kept;
            vtkNew<vtkIdList> ids;
            const vtkIdType nCells = lines->GetNumberOfCells();
            for (vtkIdType c = 0; c < nCells; ++c) {
                lines->GetCellAtId(c, ids);
                if (ids->GetNumberOfIds() < 2)
                    continue;
                if (scalars->GetComponent(ids->GetId(0), 0) < 0.0)
                    continue;
                kept->InsertNextCell(ids);
            }
            out->SetLines(kept);
        }
        out->GetPointData()->SetScalars(rgb);
        return 1;
    }

private:
    std::vector<double> m_levels;
    std::vector<std::array<unsigned char, 3>> m_colours;
    bool m_dashNegative{ true };
};
vtkStandardNewMacro(vtkVisivoContourColour);

// ── Dash stage ──
// The negative levels as dashes: the segments of those levels are joined into
// lines (a walk that is linear in their number) and each line is cut along its
// length, so the pattern runs on round the corners. The length is in world
// units; the layer follows the zoom, as a dash is a length on the screen. A
// contour with more segments than a dash pattern is worth (noise, mostly) is
// drawn as it is, solid.
class vtkVisivoContourDash : public vtkPolyDataAlgorithm
{
public:
    static vtkVisivoContourDash *New();
    vtkTypeMacro(vtkVisivoContourDash, vtkPolyDataAlgorithm);

    static constexpr std::size_t kMaxSegments = 1'500'000;
    static constexpr double kMaxDashesPerLine = 20000.0;

    void configure(const std::vector<double> &levels,
                   const std::vector<std::array<unsigned char, 3>> &colours, bool dashNegative)
    {
        m_levels = levels;
        m_colours = colours;
        m_dashNegative = dashNegative;
        Modified();
    }
    void setDash(double on, double off)
    {
        if (on != m_on || off != m_off) {
            m_on = on;
            m_off = off;
            Modified();
        }
    }

protected:
    vtkVisivoContourDash()
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
        vtkPoints *inPoints = in->GetPoints();
        if (!m_dashNegative || !scalars || !lines || !inPoints || m_levels.empty()
            || m_colours.empty() || m_levels.front() >= 0.0)
            return 1;
        // The segments of the negative levels.
        std::vector<vtkIdType> segs;
        vtkNew<vtkIdList> ids;
        const vtkIdType nCells = lines->GetNumberOfCells();
        for (vtkIdType c = 0; c < nCells; ++c) {
            lines->GetCellAtId(c, ids);
            if (ids->GetNumberOfIds() != 2 || scalars->GetComponent(ids->GetId(0), 0) >= 0.0)
                continue;
            segs.push_back(ids->GetId(0));
            segs.push_back(ids->GetId(1));
        }
        if (segs.empty())
            return 1;
        const vtkIdType nIn = in->GetNumberOfPoints();
        vtkNew<vtkPoints> points;
        vtkNew<vtkCellArray> cells;
        vtkNew<vtkUnsignedCharArray> rgb;
        rgb->SetNumberOfComponents(3);
        rgb->SetName("Colours");
        const auto colourOf = [&](vtkIdType point) {
            const int idx = vc::nearestLevel(m_levels, scalars->GetComponent(point, 0));
            return m_colours[std::size_t(std::clamp<int>(idx, 0, int(m_colours.size()) - 1))];
        };
        const auto addLine = [&](const std::vector<std::array<double, 3>> &pts,
                                 const std::array<unsigned char, 3> &col) {
            vtkNew<vtkIdList> line;
            line->SetNumberOfIds(vtkIdType(pts.size()));
            for (std::size_t i = 0; i < pts.size(); ++i) {
                line->SetId(vtkIdType(i), points->InsertNextPoint(pts[i].data()));
                rgb->InsertNextTuple3(col[0], col[1], col[2]);
            }
            cells->InsertNextCell(line);
        };

        std::vector<unsigned char> ok(static_cast<std::size_t>(nIn));
        std::vector<std::array<double, 3>> xyz(static_cast<std::size_t>(nIn));
        for (vtkIdType i = 0; i < nIn; ++i) {
            inPoints->GetPoint(i, xyz[std::size_t(i)].data());
            const auto &p = xyz[std::size_t(i)];
            ok[std::size_t(i)] = std::isfinite(p[0]) && std::isfinite(p[1]);
        }
        const std::size_t nSeg = segs.size() / 2;
        if (nSeg > kMaxSegments || !(m_on > 0.0)) {
            for (std::size_t s = 0; s < nSeg; ++s) {
                const vtkIdType a = segs[2 * s], b = segs[2 * s + 1];
                if (ok[std::size_t(a)] && ok[std::size_t(b)])
                    addLine({ xyz[std::size_t(a)], xyz[std::size_t(b)] }, colourOf(a));
            }
        } else {
            const vc::Chains chains =
                    vc::chainSegments(segs.data(), nSeg, std::size_t(nIn), ok.data());
            // A dash is no shorter than a twenty-thousandth of the longest line,
            // or zooming in on a long contour would cut it into millions.
            double longest = 0.0;
            for (std::size_t c = 0; c < chains.count(); ++c) {
                double length = 0.0;
                for (std::size_t i = chains.start[c] + 1; i < chains.start[c + 1]; ++i) {
                    const auto &p = xyz[std::size_t(chains.ids[i - 1])];
                    const auto &q = xyz[std::size_t(chains.ids[i])];
                    length += std::hypot(q[0] - p[0], q[1] - p[1]);
                }
                longest = std::max(longest, length);
            }
            const double scale = std::max(1.0, longest / ((m_on + m_off) * kMaxDashesPerLine));
            std::vector<std::array<double, 2>> line;
            for (std::size_t c = 0; c < chains.count(); ++c) {
                const std::size_t from = chains.start[c], to = chains.start[c + 1];
                line.clear();
                for (std::size_t i = from; i < to; ++i) {
                    const auto &p = xyz[std::size_t(chains.ids[i])];
                    line.push_back({ p[0], p[1] });
                }
                const double z = xyz[std::size_t(chains.ids[from])][2];
                const auto col = colourOf(chains.ids[from]);
                vc::dashPolyline(line, m_on * scale, m_off * scale,
                                 [&](const std::vector<std::array<double, 2>> &piece) {
                                     std::vector<std::array<double, 3>> pts;
                                     pts.reserve(piece.size());
                                     for (const auto &p : piece)
                                         pts.push_back({ p[0], p[1], z });
                                     addLine(pts, col);
                                 });
            }
        }
        out->SetPoints(points);
        out->SetLines(cells);
        out->GetPointData()->SetScalars(rgb);
        return 1;
    }

private:
    std::vector<double> m_levels;
    std::vector<std::array<unsigned char, 3>> m_colours;
    bool m_dashNegative{ true };
    double m_on{ 6.0 };
    double m_off{ 4.0 };
};
vtkStandardNewMacro(vtkVisivoContourDash);

// ── The build ──
// What a build needs, copied so that a worker thread shares nothing that the
// GUI thread changes: the image (its scalars are only read), the levels and
// the styling. With @c edges set (the geometry has not changed) only the
// colouring and the dashes are made again.
struct ContourLayer::Job
{
    vtkSmartPointer<vtkImageData> image;
    vtkSmartPointer<vtkPolyData> edges;
    std::vector<double> levels;
    std::vector<std::array<unsigned char, 3>> colours;
    bool dashNegative{ true };
    double sigmaWorld{ 0.0 };
    bool smoothing{ false };
    long long maxPixels{ ContourLayer::kDefaultMaxPixels };
    double dashOn{ 6.0 }, dashOff{ 4.0 };
    unsigned long gen{ 0 }, styleGen{ 0 };
};

struct ContourLayer::Built
{
    vtkSmartPointer<vtkPolyData> edges;    ///< the segments of the contour
    vtkSmartPointer<vtkPolyData> solid;    ///< coloured, the levels drawn solid
    vtkSmartPointer<vtkPolyData> dashed;   ///< the negative levels, in dashes
    unsigned long gen{ 0 }, styleGen{ 0 };
};

namespace {

/// A detached copy of a filter's output.
vtkSmartPointer<vtkPolyData> detached(vtkPolyData *output)
{
    auto copy = vtkSmartPointer<vtkPolyData>::New();
    if (output)
        copy->ShallowCopy(output);
    return copy;
}

ContourLayer::Built buildContours(const ContourLayer::Job &job)
{
    ContourLayer::Built out;
    out.gen = job.gen;
    out.styleGen = job.styleGen;
    vtkSmartPointer<vtkPolyData> edges = job.edges;
    if (!edges) {
        if (job.image && !job.levels.empty()) {
            vtkNew<vtkVisivoContourDecimate> thin;
            thin->setMaxPixels(job.maxPixels);
            thin->SetInputData(job.image);
            vtkNew<vtkVisivoContourSmooth> smooth;
            smooth->setSigmaWorld(job.sigmaWorld);
            smooth->SetInputConnection(thin->GetOutputPort());
            vtkNew<vtkFlyingEdges2D> flying;
            flying->ComputeScalarsOn();
            flying->SetInputConnection(job.smoothing ? smooth->GetOutputPort()
                                                     : thin->GetOutputPort());
            flying->SetNumberOfContours(int(job.levels.size()));
            for (std::size_t i = 0; i < job.levels.size(); ++i)
                flying->SetValue(int(i), job.levels[i]);
            flying->Update();
            edges = detached(flying->GetOutput());
        } else {
            edges = vtkSmartPointer<vtkPolyData>::New();
        }
    }
    out.edges = edges;
    vtkNew<vtkVisivoContourColour> solid;
    solid->configure(job.levels, job.colours, job.dashNegative);
    solid->SetInputData(edges);
    solid->Update();
    out.solid = detached(solid->GetOutput());
    vtkNew<vtkVisivoContourDash> dashed;
    dashed->configure(job.levels, job.colours, job.dashNegative);
    dashed->setDash(job.dashOn, job.dashOff);
    dashed->SetInputData(edges);
    dashed->Update();
    out.dashed = detached(dashed->GetOutput());
    return out;
}

}   // namespace

// ── The layer ──

struct ContourLayer::Impl
{
    QFutureWatcher<ContourLayer::Built> watcher;
    bool running{ false };
    /// What the settings and the input ask for now (they count up with every
    /// change of the geometry, and of the colours and dashes).
    unsigned long gen{ 1 }, styleGen{ 1 };
    /// What is on the screen, and the segments it was made of.
    unsigned long shownGen{ 0 }, shownStyleGen{ 0 }, edgesGen{ 0 };
    vtkSmartPointer<vtkPolyData> edges;
    /// The live input as it was last looked at.
    vtkImageData *liveImage{ nullptr };
    vtkMTimeType liveTime{ 0 };
};

ContourLayer::ContourLayer()
    : m_impl(std::make_unique<Impl>()),
      m_solidOut(vtkSmartPointer<vtkTrivialProducer>::New()),
      m_dashedOut(vtkSmartPointer<vtkTrivialProducer>::New()),
      m_solidMapper(vtkSmartPointer<vtkPolyDataMapper>::New()),
      m_dashedMapper(vtkSmartPointer<vtkPolyDataMapper>::New()),
      m_solidActor(vtkSmartPointer<vtkActor>::New()),
      m_dashedActor(vtkSmartPointer<vtkActor>::New()),
      m_transform(vtkSmartPointer<vtkTransform>::New())
{
    const auto wire = [this](vtkPolyDataMapper *mapper, vtkTrivialProducer *out, vtkActor *actor) {
        out->SetOutput(vtkSmartPointer<vtkPolyData>::New());
        mapper->SetInputConnection(out->GetOutputPort());
        mapper->SetColorModeToDirectScalars();
        mapper->ScalarVisibilityOn();
        mapper->SetScalarModeToUsePointData();
        actor->SetMapper(mapper);
        actor->GetProperty()->LightingOff();
        actor->SetUserTransform(m_transform);
        actor->VisibilityOff();
    };
    wire(m_solidMapper, m_solidOut, m_solidActor);
    wire(m_dashedMapper, m_dashedOut, m_dashedActor);
    QObject::connect(&m_impl->watcher, &QFutureWatcherBase::finished, &m_impl->watcher, [this]() {
        m_impl->running = false;
        const Built built = m_impl->watcher.result();
        adopt(built);
        schedule();   // what changed while it ran
    });
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
        if (m_renderObserver)
            m_renderer->RemoveObserver(m_renderObserver);
        m_renderObserver = 0;
    }
    m_renderer = renderer;
    if (m_renderer) {
        m_renderer->AddViewProp(m_solidActor);
        m_renderer->AddViewProp(m_dashedActor);
        // The dashes are sized to the view, and a live input looked at, just
        // before each render.
        vtkNew<vtkCallbackCommand> command;
        command->SetClientData(this);
        command->SetCallback(&ContourLayer::onRenderStart);
        m_renderObserver = m_renderer->AddObserver(vtkCommand::StartEvent, command);
        m_pixelsPerWorld = 0.0;
    }
}

void ContourLayer::onRenderStart(vtkObject *, unsigned long, void *client, void *)
{
    static_cast<ContourLayer *>(client)->followCamera();
}

vtkImageData *ContourLayer::liveImage() const
{
    if (!m_inputPort || !m_inputPort->GetProducer())
        return nullptr;
    return vtkImageData::SafeDownCast(
            m_inputPort->GetProducer()->GetOutputDataObject(m_inputPort->GetIndex()));
}

long long ContourLayer::inputPixels(int *width, int *height) const
{
    vtkImageData *image = m_image ? m_image.GetPointer() : liveImage();
    if (!image || !image->GetPointData()->GetScalars())
        return 0;
    int ext[6];
    image->GetExtent(ext);
    const int w = ext[1] - ext[0] + 1, h = ext[3] - ext[2] + 1;
    if (width)
        *width = w;
    if (height)
        *height = h;
    return w > 0 && h > 0 ? (long long)w * (long long)h : 0;
}

void ContourLayer::followCamera()
{
    if (!m_renderer)
        return;
    // A live input (the cube's slice) has changed: contour it again.
    if (m_inputPort) {
        vtkImageData *live = liveImage();
        const vtkMTimeType time = live ? live->GetMTime() : 0;
        if (live != m_impl->liveImage || time != m_impl->liveTime) {
            m_impl->liveImage = live;
            m_impl->liveTime = time;
            changed(/*geometry=*/true);
        }
    }
    if (!m_dashedActor->GetVisibility())
        return;
    vtkCamera *camera = m_renderer->GetActiveCamera();
    const int *size = m_renderer->GetSize();
    if (!camera || !camera->GetParallelProjection() || !size || size[1] <= 0
        || !(camera->GetParallelScale() > 0.0))
        return;
    // A dash is a length on the screen: 8 pixels drawn, 5 left blank. Resize
    // only when the zoom has moved enough to be seen, as it makes them again.
    const double pixelsPerWorld = double(size[1]) / (2.0 * camera->GetParallelScale());
    if (m_pixelsPerWorld > 0.0 && std::abs(pixelsPerWorld / m_pixelsPerWorld - 1.0) < 0.15)
        return;
    m_pixelsPerWorld = pixelsPerWorld;
    m_dashOn = 8.0 / pixelsPerWorld;
    m_dashOff = 5.0 / pixelsPerWorld;
    if (m_settings.dashNegative)
        changed(/*geometry=*/false);
}

void ContourLayer::setInputData(vtkImageData *image)
{
    m_image = image;
    m_inputPort = nullptr;
    m_hasInput = image != nullptr;
    changed(/*geometry=*/true);
    setVisible(m_visible);
}

void ContourLayer::setInputConnection(vtkAlgorithmOutput *port)
{
    m_image = nullptr;
    m_inputPort = port;
    m_hasInput = port != nullptr;
    m_impl->liveImage = liveImage();
    m_impl->liveTime = m_impl->liveImage ? m_impl->liveImage->GetMTime() : 0;
    changed(/*geometry=*/true);
    setVisible(m_visible);
}

void ContourLayer::setMaxPixels(long long pixels)
{
    if (pixels != m_maxPixels) {
        m_maxPixels = pixels;
        changed(/*geometry=*/true);
    }
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
    const auto colours = m_colours;
    const bool dashNegative = m_settings.dashNegative;
    m_settings = settings;
    QString problem;
    const std::vector<double> levels = vc::levels(m_settings, &problem);

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
    const double sigmaWorld = sigmaPixels * m_worldPerPixel;
    const bool smoothing = sigmaPixels >= 0.05;
    const bool geometry = levels != m_levels || smoothing != m_smoothing
            || (smoothing && sigmaWorld != m_sigmaWorld);
    m_levels = levels;
    m_smoothing = smoothing;
    m_sigmaWorld = sigmaWorld;
    applyStyle();
    if (geometry)
        changed(/*geometry=*/true);
    else if (m_colours != colours || dashNegative != m_settings.dashNegative)
        changed(/*geometry=*/false);

    m_problem = problem;
    return this->problem();
}

QString ContourLayer::problem() const
{
    // Say when the image is thinned.
    QString out = m_problem;
    int w = 0, h = 0;
    if (inputPixels(&w, &h) > 0) {
        const QString note = vc::thinningNote(w, h, m_maxPixels);
        if (!note.isEmpty())
            out = out.isEmpty() ? note : out + u"\n"_s + note;
    }
    return out;
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
    const double width = std::clamp(m_settings.lineWidth, 0.5, 20.0);
    m_solidActor->GetProperty()->SetLineWidth(width);
    m_dashedActor->GetProperty()->SetLineWidth(width);
}


void ContourLayer::changed(bool geometry)
{
    ++(geometry ? m_impl->gen : m_impl->styleGen);
    schedule();
}

bool ContourLayer::busy() const
{
    return m_impl->running || m_impl->shownGen != m_impl->gen
            || m_impl->shownStyleGen != m_impl->styleGen;
}

ContourLayer::Job ContourLayer::makeJob(bool needGeometry, vtkImageData *image) const
{
    Job job;
    if (needGeometry) {
        // The pixels are shared (a fixed image is only read) or were copied (a
        // live one may be written to again while the worker reads it); the rest
        // of the image's description is the worker's own.
        job.image = vtkSmartPointer<vtkImageData>::New();
        if (image)
            job.image->ShallowCopy(image);
    } else {
        job.edges = m_impl->edges;
    }
    job.levels = m_levels;
    job.colours = m_colours;
    job.dashNegative = m_settings.dashNegative;
    job.smoothing = m_smoothing;
    job.sigmaWorld = m_sigmaWorld;
    job.maxPixels = m_maxPixels;
    job.dashOn = m_dashOn;
    job.dashOff = m_dashOff;
    job.gen = m_impl->gen;
    job.styleGen = m_impl->styleGen;
    return job;
}

void ContourLayer::schedule()
{
    if (!m_visible || !m_hasInput || m_impl->running)
        return;
    if (m_impl->shownGen == m_impl->gen && m_impl->shownStyleGen == m_impl->styleGen)
        return;
    const bool needGeometry = m_impl->edgesGen != m_impl->gen || !m_impl->edges;
    vtkImageData *source = nullptr;
    vtkSmartPointer<vtkImageData> copy;
    if (needGeometry) {
        source = m_image ? m_image.GetPointer() : liveImage();
        if (!source || !source->GetPointData()->GetScalars())
            return;   // nothing to contour yet
        if (!m_image) {
            copy = vtkSmartPointer<vtkImageData>::New();
            copy->DeepCopy(source);
            source = copy;
        }
    }
    const Job job = makeJob(needGeometry, source);
    m_impl->running = true;
    m_impl->watcher.setFuture(QtConcurrent::run([job]() {
        try {
            return buildContours(job);
        } catch (...) {   // out of memory on a huge image: draw nothing, do not end the app
            Built none;
            none.edges = vtkSmartPointer<vtkPolyData>::New();
            none.solid = vtkSmartPointer<vtkPolyData>::New();
            none.dashed = vtkSmartPointer<vtkPolyData>::New();
            none.gen = job.gen;
            none.styleGen = job.styleGen;
            return none;
        }
    }));
}

void ContourLayer::adopt(const Built &built)
{
    // One that is already out of date is shown all the same, and built again
    // (schedule() sees the generation): while the input keeps changing, as a
    // slice being dragged does, the lines would otherwise never appear.
    if (!built.solid)
        return;
    m_impl->edges = built.edges;
    m_impl->edgesGen = built.gen;
    m_impl->shownGen = built.gen;
    m_impl->shownStyleGen = built.styleGen;
    m_solidOut->SetOutput(built.solid);
    m_solidOut->Modified();
    m_dashedOut->SetOutput(built.dashed);
    m_dashedOut->Modified();
    if (m_renderer && m_renderer->GetRenderWindow())
        m_renderer->GetRenderWindow()->Render();
}

void ContourLayer::update()
{
    if (!m_hasInput)
        return;
    vtkImageData *source = m_image ? m_image.GetPointer() : liveImage();
    if (!source || !source->GetPointData()->GetScalars())
        return;
    // A live input is not copied: nothing else runs while this does.
    adopt(buildContours(makeJob(/*needGeometry=*/true, source)));
}

void ContourLayer::setVisible(bool visible)
{
    m_visible = visible;
    // Nothing to draw before there is an image.
    const bool on = visible && m_hasInput;
    m_solidActor->SetVisibility(on);
    m_dashedActor->SetVisibility(on);
    if (on)
        schedule();
}

vc::LineSource ContourLayer::lineSource(
        const std::function<std::array<double, 2>(double, double)> &worldToPixel,
        std::size_t maxSegments, bool *tooMany)
{
    vc::LineSource out;
    if (tooMany)
        *tooMany = false;
    if (!m_hasInput || m_levels.empty())
        return out;
    // The export asked for the contours: have them as they are now.
    if (m_impl->edgesGen != m_impl->gen || !m_impl->edges)
        update();
    vtkPolyData *poly = m_impl->edges;
    vtkDataArray *scalars = poly ? poly->GetPointData()->GetScalars() : nullptr;
    vtkCellArray *cells = poly ? poly->GetLines() : nullptr;
    if (!scalars || !cells || !poly->GetPoints())
        return out;
    const vtkIdType nCells = cells->GetNumberOfCells();
    if (std::size_t(nCells) > maxSegments) {
        if (tooMany)
            *tooMany = true;
        return out;
    }
    const vtkIdType nPoints = poly->GetNumberOfPoints();
    out.points.resize(std::size_t(nPoints));
    out.level.resize(std::size_t(nPoints));
    for (vtkIdType i = 0; i < nPoints; ++i) {
        double p[3], q[3];
        poly->GetPoint(i, p);
        // The layer's own turn first (an image aligned to another one).
        m_transform->TransformPoint(p, q);
        out.points[std::size_t(i)] = worldToPixel(q[0], q[1]);
        out.level[std::size_t(i)] = scalars->GetComponent(i, 0);
    }
    out.segments.reserve(std::size_t(nCells) * 2);
    vtkNew<vtkIdList> ids;
    for (vtkIdType c = 0; c < nCells; ++c) {
        cells->GetCellAtId(c, ids);
        if (ids->GetNumberOfIds() != 2)
            continue;
        out.segments.push_back(ids->GetId(0));
        out.segments.push_back(ids->GetId(1));
    }
    out.levels = m_levels;
    out.colours.reserve(m_colours.size());
    for (const auto &rgb : m_colours)
        out.colours.push_back(QColor(rgb[0], rgb[1], rgb[2]).name());
    out.dashNegative = m_settings.dashNegative;
    out.lineWidth = m_settings.lineWidth;
    if (m_settings.levelMode == vc::LevelMode::Sigma)
        out.sigma = m_settings.sigma;
    int w = 0, h = 0;
    if (inputPixels(&w, &h) > 0) {
        const int stride = vc::thinStride(w, h, m_maxPixels);
        if (stride > 1)
            out.note = u"drawn on 1 pixel in %1 along each axis of the image"_s.arg(stride);
    }
    return out;
}

namespace {

/// About a million of the pixels of an image, evenly spread.
std::vector<double> sampleOf(vtkImageData *image)
{
    vtkDataArray *scalars = image ? image->GetPointData()->GetScalars() : nullptr;
    if (!scalars || scalars->GetNumberOfComponents() != 1)
        return {};
    const std::size_t n = std::size_t(scalars->GetNumberOfTuples());
    constexpr std::size_t kMax = 1u << 20;
    const std::size_t stride = std::max<std::size_t>(1, n / kMax);
    std::vector<double> sample;
    sample.reserve(n / stride + 1);
    for (std::size_t i = 0; i < n; i += stride)
        sample.push_back(scalars->GetComponent(vtkIdType(i), 0));
    return sample;
}

}   // namespace

double ContourLayer::noiseOf(vtkImageData *image)
{
    return vc::robustSigma(sampleOf(image));
}

vc::DataStats ContourLayer::statsOf(vtkImageData *image)
{
    return vc::dataStats(sampleOf(image));
}
