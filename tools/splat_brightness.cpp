// Gaussian splat brightness against the number of particles, through the
// desktop's own splat rules (issue #457). The desktop counterpart of
// validation/splat_brightness.py, which measures the backend's point session
// and the VR page.
//
// One truncated Plummer sphere (a = 1, cut at 10 a; eight markers at the
// corners of the cut cube pin the box the splats are sized by) is drawn from
// the first N rows of one draw, offscreen, through vtkPointGaussianMapper set
// up as vtkWindowCatalogue3D sets it up (ParticleSplatDefaults,
// ParticleSplatVtk), the render budget's level of detail included. Face on
// over +-4 a at 1000 px, it reports:
//   - the mean light at intensity 1 (nothing reaches white), against the
//     smallest N: the "mean ratio";
//   - the mean at the desktop's own intensity, 35, whose core is clipped at
//     white (not part of the pass);
//   - the light at R = 3 a (1 % of the central surface density, read at
//     intensity 100) over the light at the centre (intensity 1), against the
//     truncated sphere's analytic surface density.
// Pass: mean ratio 1 +- 5 %, profile within 10 %.
//
// --before draws with the rule the desktop had until issue #457 (8-bit
// opacity steps through the opacity function, an 8-bit framebuffer, the
// level of detail sized on the budget rather than on the rows it draws).
//
// --vbt draws as the VBT point viewer (vtkWindowVbt, particle ray-casting):
// every point, size step 3 (its slider's default), no render budget; with
// --before, its rule until issue #457 (opacity min(0.9, 20 / cbrt N) x
// intensity / 100, at least 0.05, radius longest side / cbrt N x size / 8).
//
// Build: cmake --build build --target splat_brightness
// Run:   build/splat_brightness [--sizes 1e5,1e6,3.66e6,1e7,3e7] [--budget 1000000]
//                               [--before] [--vbt] [--csv out.csv]

#include "ParticleSplatDefaults.h"
#include "ParticleSplatVtk.h"

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkFloatArray.h>
#include <vtkLookupTable.h>
#include <vtkMaskPoints.h>
#include <vtkNew.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPointData.h>
#include <vtkPointGaussianMapper.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>
#include <vtkImageData.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kA = 1.0;
constexpr double kCut = 10.0;
constexpr double kView = 4.0;
constexpr int kPixels = 1000;
constexpr double kRing = 3.0;
constexpr double kRingWidth = 0.1;
constexpr double kCore = 0.1;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> plummer(long long n)
{
    std::mt19937_64 rng(457);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const double mCut = std::pow(kCut, 3) / std::pow(kCut * kCut + kA * kA, 1.5);
    std::vector<float> xyz(static_cast<std::size_t>(n) * 3);
    for (long long i = 0; i < n; ++i) {
        const double m = uni(rng) * mCut;
        const double r = kA / std::sqrt(std::pow(m, -2.0 / 3.0) - 1.0);
        const double cosT = 2.0 * uni(rng) - 1.0;
        const double phi = 2.0 * kPi * uni(rng);
        const double sinT = std::sqrt(std::max(0.0, 1.0 - cosT * cosT));
        xyz[3 * i] = static_cast<float>(r * sinT * std::cos(phi));
        xyz[3 * i + 1] = static_cast<float>(r * sinT * std::sin(phi));
        xyz[3 * i + 2] = static_cast<float>(r * cosT);
    }
    int k = 0;
    for (int sx : { -1, 1 })
        for (int sy : { -1, 1 })
            for (int sz : { -1, 1 }) {
                xyz[3 * k] = float(sx * kCut);
                xyz[3 * k + 1] = float(sy * kCut);
                xyz[3 * k + 2] = float(sz * kCut);
                ++k;
            }
    return xyz;
}

double surfaceDensity(double bigR)
{
    if (bigR >= kCut)
        return 0.0;
    const double zt = std::sqrt(kCut * kCut - bigR * bigR);
    const int steps = 4000;
    double sum = 0.0;
    for (int i = 0; i <= steps; ++i) {
        const double z = zt * i / steps;
        const double rho = std::pow(1.0 + (bigR * bigR + z * z) / (kA * kA), -2.5);
        sum += (i == 0 || i == steps ? 0.5 : 1.0) * rho;
    }
    return 2.0 * sum * zt / steps;
}

double ringMean(double r0, double r1)
{
    const int steps = 200;
    double num = 0.0, den = 0.0;
    for (int i = 0; i <= steps; ++i) {
        const double r = r0 + (r1 - r0) * i / steps;
        const double w = (i == 0 || i == steps ? 0.5 : 1.0) * r;
        num += w * surfaceDensity(r);
        den += w;
    }
    return num / den;
}

double analyticProfile()
{
    return ringMean(kRing - kRingWidth / 2, kRing + kRingWidth / 2) / ringMean(0.0, kCore);
}

struct Picture {
    std::vector<double> light;
    bool clipped = false;
};

// The window's red channel as light in units of the splat colour (white).
Picture readBack(vtkRenderWindow *window)
{
    vtkNew<vtkWindowToImageFilter> grab;
    grab->SetInput(window);
    grab->SetInputBufferTypeToRGB();
    grab->ReadFrontBufferOff();
    grab->Update();
    vtkImageData *image = grab->GetOutput();
    auto *pixels = vtkUnsignedCharArray::SafeDownCast(image->GetPointData()->GetScalars());
    Picture out;
    out.light.resize(std::size_t(kPixels) * kPixels);
    for (vtkIdType i = 0; i < vtkIdType(out.light.size()); ++i) {
        const unsigned char red = pixels->GetValue(3 * i);
        out.light[std::size_t(i)] = red / 255.0;
        out.clipped |= red == 255;
    }
    return out;
}

double radiusOf(int pixel)
{
    return (pixel + 0.5) / kPixels * 2.0 * kView - kView;
}

double meanWhere(const std::vector<double> &light, double r0, double r1)
{
    double sum = 0.0;
    long long count = 0;
    for (int y = 0; y < kPixels; ++y)
        for (int x = 0; x < kPixels; ++x) {
            const double r = std::hypot(radiusOf(x), radiusOf(y));
            if (r >= r0 && r < r1) {
                sum += light[std::size_t(y) * kPixels + x];
                ++count;
            }
        }
    return count ? sum / count : 0.0;
}

double mean(const std::vector<double> &light)
{
    double sum = 0.0;
    for (double v : light)
        sum += v;
    return sum / double(light.size());
}

struct Scene {
    vtkNew<vtkRenderWindow> window;
    vtkNew<vtkRenderer> renderer;
    vtkNew<vtkPolyData> cloud;
    vtkNew<vtkMaskPoints> decimator;
    vtkNew<vtkPointGaussianMapper> mapper;
    vtkNew<vtkPiecewiseFunction> opacity;
    vtkNew<vtkLookupTable> lut;
    vtkNew<vtkActor> actor;
    vtkSmartPointer<vtkToneMappingPass> pass = ParticleSplatVtk::floatTargetPass();

    explicit Scene(bool before)
    {
        window->SetOffScreenRendering(1);
        window->SetSize(kPixels, kPixels);
        window->SetMultiSamples(0);
        window->AddRenderer(renderer);
        renderer->SetBackground(0.0, 0.0, 0.0);
        vtkCamera *camera = renderer->GetActiveCamera();
        camera->ParallelProjectionOn();
        camera->SetPosition(0.0, 0.0, 100.0);
        camera->SetFocalPoint(0.0, 0.0, 0.0);
        camera->SetViewUp(0.0, 1.0, 0.0);
        camera->SetParallelScale(kView);
        camera->SetClippingRange(50.0, 150.0);
        lut->SetNumberOfTableValues(2);
        lut->SetTableRange(-1.0e15, 1.0e15);
        lut->SetTableValue(0, 1.0, 1.0, 1.0, 1.0);
        lut->SetTableValue(1, 1.0, 1.0, 1.0, 1.0);
        lut->Build();
        // As vtkWindowCatalogue3D's scene sets its splats up.
        mapper->SetEmissive(true);
        mapper->SetBoundScale(3.0f);
        mapper->SetOpacityArray("Morphology");
        mapper->SetScalarOpacityFunction(opacity);
        mapper->SetLookupTable(lut);
        mapper->SetScalarRange(-0.5, 0.5);
        mapper->ScalarVisibilityOn();
        if (!before) {
            ParticleSplatVtk::setUnitOpacity(opacity);
            renderer->SetPass(pass);
        }
        actor->SetMapper(mapper);
        renderer->AddActor(actor);
    }

    void load(const std::vector<float> &xyz, long long n, long long stride)
    {
        vtkNew<vtkFloatArray> coords;
        coords->SetNumberOfComponents(3);
        coords->SetArray(const_cast<float *>(xyz.data()), n * 3, 1);
        vtkNew<vtkPoints> points;
        points->SetData(coords);
        vtkNew<vtkFloatArray> scalars;
        scalars->SetName("Morphology");
        scalars->SetNumberOfValues(n);
        scalars->FillValue(0.0f);
        cloud->Initialize();
        cloud->SetPoints(points);
        cloud->GetPointData()->SetScalars(scalars);
        if (stride > 1) {
            decimator->SetInputData(cloud);
            decimator->RandomModeOff();
            decimator->SetOnRatio(int(stride));
            decimator->GenerateVerticesOn();
            decimator->SingleVertexPerCellOn();
            mapper->SetInputConnection(decimator->GetOutputPort());
        } else {
            mapper->SetInputData(cloud);
        }
    }
};

struct Row {
    long long n = 0;
    long long stride = 1;
    double mean = 0.0;
    double mean35 = 0.0;
    double profile = 0.0;
    bool clipped = false;
};

std::vector<long long> parseSizes(const std::string &text)
{
    std::vector<long long> out;
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ','))
        out.push_back(static_cast<long long>(std::llround(std::stod(item))));
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<long long> sizes = { 100000, 1000000, 3660000, 10000000, 30000000 };
    long long budget = 1000000;
    bool before = false;
    bool vbt = false;
    std::string csvPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sizes" && i + 1 < argc)
            sizes = parseSizes(argv[++i]);
        else if (arg == "--budget" && i + 1 < argc)
            budget = std::atoll(argv[++i]);
        else if (arg == "--before")
            before = true;
        else if (arg == "--vbt")
            vbt = true;
        else if (arg == "--csv" && i + 1 < argc)
            csvPath = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--sizes a,b,...] [--budget N] [--before] [--vbt] [--csv path]\n",
                         argv[0]);
            return 2;
        }
    }
    long long largest = 0;
    for (long long n : sizes)
        largest = std::max(largest, n);
    const std::vector<float> xyz = plummer(largest);
    const double analytic = analyticProfile();
    // The desktop's default size: the size slider's Gaussian start.
    double size = 3.0 * ParticleSplatDefaults::initialSliderValue() / 100.0;
    if (vbt) {
        size = 3.0;
        budget = 0;
    }

    Scene scene(before);
    std::vector<Row> rows;
    for (long long n : sizes) {
        const long long stride = ParticleSplatDefaults::renderStride(n, budget);
        scene.load(xyz, n, stride);
        double bounds[6];
        scene.cloud->GetBounds(bounds);
        std::vector<Picture> pictures;
        // The VBT viewer's splats are four times wider: its core stays below
        // white only at a sixteenth of the lowest intensity its slider has.
        const double low = vbt ? 1.0 / 16.0 : 1.0;
        for (double intensity : { low, 100.0 * low, ParticleSplatDefaults::kIntensity }) {
            if (before && vbt) {
                const double longest = std::max({ bounds[1] - bounds[0], bounds[3] - bounds[2],
                                                  bounds[5] - bounds[4], 1.0e-6 });
                const double cbrtN = std::cbrt(double(n));
                const double value = std::clamp(std::min(0.9, 20.0 / cbrtN) * intensity / 100.0,
                                                0.05, 0.9);
                scene.opacity->RemoveAllPoints();
                scene.opacity->AddPoint(-1.0e15, value);
                scene.opacity->AddPoint(1.0e15, value);
                scene.mapper->SetScaleFactor(longest / cbrtN * size / 8.0);
            } else if (before) {
                // The rule until issue #457: the level of detail counted as
                // min(rows, budget), the opacity a whole 8-bit step, at least
                // eight, stored as the colour's alpha a quarter step up.
                const double counted = double(budget > 0 ? std::min(n, budget) : n);
                const auto style = ParticleSplatDefaults::splatStyle(bounds, counted, size);
                const double steps = ParticleSplatDefaults::alphaStepsForDepth(style.depth, intensity);
                const double value = (steps + 0.25) / 255.0;
                scene.opacity->RemoveAllPoints();
                scene.opacity->AddPoint(-1.0e15, value);
                scene.opacity->AddPoint(1.0e15, value);
                scene.mapper->SetScaleFactor(style.radius);
            } else {
                const auto style = ParticleSplatDefaults::splatStyle(
                        bounds, double(n) / double(stride), size, intensity);
                scene.mapper->SetScaleFactor(style.radius);
                ParticleSplatVtk::setSplatGain(scene.mapper, style.gain);
            }
            scene.window->Render();
            pictures.push_back(readBack(scene.window));
        }
        Row row;
        row.n = n;
        row.stride = stride;
        row.mean = mean(pictures[0].light);
        row.clipped = pictures[0].clipped;
        row.mean35 = mean(pictures[2].light);
        const double core = meanWhere(pictures[0].light, 0.0, kCore);
        const double ring = meanWhere(pictures[1].light, kRing - kRingWidth / 2,
                                      kRing + kRingWidth / 2) / 100.0;
        row.profile = core > 0.0 ? ring / core : 0.0;
        rows.push_back(row);
        std::fprintf(stderr, "N = %lld done\n", n);
    }

    std::printf("# Desktop splat brightness against N (issue #457)%s%s\n\n",
                vbt ? " - VBT point viewer" : " - catalogue viewer", before ? " - before" : "");
    std::printf("Plummer sphere a = 1, cut at 10 a, face on over +-4 a at %d px, splat size %.2f, "
                "render budget %lld. Analytic Sigma(3a)/Sigma(0) of the cut sphere: %.5f.\n\n",
                kPixels, size, budget, analytic);
    std::printf("| N | stride | mean at the lowest intensity | mean ratio to first N | profile / analytic - 1 | "
                "mean ratio at the desktop's intensity 35 | pass |\n");
    std::printf("|---:|---:|---:|---:|---:|---:|:---:|\n");
    std::ofstream csv;
    if (!csvPath.empty()) {
        csv.open(csvPath);
        csv << "n,stride,mean,ratio,profile,profile_err,ratio35,clipped,pass\n";
    }
    bool allPass = true;
    for (const Row &row : rows) {
        const double ratio = row.mean / rows.front().mean;
        const double err = row.profile / analytic - 1.0;
        const bool pass = std::abs(ratio - 1.0) <= 0.05 && std::abs(err) <= 0.10;
        allPass &= pass;
        const double ratio35 = row.mean35 / rows.front().mean35;
        std::printf("| %lld | %lld | %.5f | %.3f | %+.3f | %.3f | %s |\n", row.n, row.stride,
                    row.mean, ratio, err, ratio35, pass ? "yes" : "no");
        if (csv.is_open())
            csv << row.n << ',' << row.stride << ',' << row.mean << ',' << ratio << ','
                << row.profile << ',' << err << ',' << ratio35 << ',' << (row.clipped ? 1 : 0) << ','
                << (pass ? 1 : 0) << '\n';
    }
    return allPass ? 0 : 1;
}
