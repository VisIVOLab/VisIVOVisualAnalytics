// vtkWindowCube_Fields.cpp
// Several fields of one simulation in one view (issue #525), as a partial class
// of vtkWindowCube.
//
// A PLUTO output or a VBT volume opens one variable per window. This adds the
// other variables of the same run to the window:
//   * 2-D: the slice becomes one picture of all the fields, layered with
//     opacity, added, split at a seam (field A left, field B right) or mirrored
//     about an axis. Each field keeps its own colour map, range, scale and
//     colour bar. The planes of the other fields are read from the backend at
//     the level the window's slice is read at (one plane each), so the memory
//     is a few planes, not volumes.
//   * 3-D: the fields are separate volumes of ONE ray cast (vtkMultiVolume), so
//     they interleave along every ray; each has its own colour and opacity
//     functions and the cast sums their emission and absorption, as the
//     headset's Overlay does in Sum mode. A field is read once, on the grid the
//     window's cube is drawn on (its preview, or the full grid), so a field
//     costs one cube of that size. It needs the local renderer: in Remote the
//     backend draws the box and knows one field.
//
// The arithmetic (the colours' sum and "over", the split layouts, the ramp's
// default) is in FieldOverlay.h; the ramp and the scale position are the
// window's own (fillRampOn, scalePositionOn), so a field's opacity curve is
// built as the window's is.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "ColorMaps.h"
#include "ColourBarTicks.h"
#include "InspectorPanel.h"
#include "VolumeColorbar.h"
#include "CubeLodVolumes.h"
#include "app/BackendClient.h"
#include "app/DatasetName.h"
#include "app/SimulationVolume.h"

#include <vtkCallbackCommand.h>
#include <vtkColorTransferFunction.h>
#include <vtkDataArray.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageStack.h>
#include <vtkLookupTable.h>
#include <vtkMultiVolume.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPointData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

using namespace Qt::StringLiterals;
namespace vf = visivo::fields;

SimField::SimField() = default;
SimField::~SimField() = default;
SimFieldViews::SimFieldViews() = default;
SimFieldViews::~SimFieldViews() = default;

namespace {

/// Planes kept per added field: the one shown and the ones just left.
constexpr int kFieldSliceCache = 6;
/// Voxels read to find a field's default threshold.
constexpr std::size_t kThresholdSample = 262144;
/// Space between neighbouring bars.
constexpr int kBarGap = 6;

/// Colour maps an added field starts with: distinct hues from black, so that
/// the sum of fields (Additive, the 3-D cast) reads as a dye on a base.
const char *defaultMapFor(int extraIndex)
{
    static const char *const kMaps[] = { "PureCyan", "PureMagenta", "PureGreen" };
    return kMaps[std::clamp(extraIndex, 0, 2)];
}

struct FieldOpenResult
{
    QString name;
    QString datasetId;
    QString error;
    visivo::units::PhysicalUnits units;
};

FieldOpenResult openFieldDataset(const QString &url, const QString &token, const QString &session,
                                 const QString &name, const QString &path,
                                 const std::array<int, 3> &dims)
{
    FieldOpenResult r;
    r.name = name;
    BackendClient client(url, token);
    client.setSessionId(session);
    const auto opened = client.openDataset(path);
    if (!opened.valid) {
        r.error = opened.error.isEmpty() ? u"The field could not be opened."_s : opened.error;
        return r;
    }
    // One grid only: the planes and the voxels are laid over this window's.
    if (opened.width != dims[0] || opened.height != dims[1] || opened.depth != dims[2]) {
        r.error = u"%1 is %2 x %3 x %4, not %5 x %6 x %7: fields of one grid only."_s
                          .arg(name).arg(opened.width).arg(opened.height).arg(opened.depth)
                          .arg(dims[0]).arg(dims[1]).arg(dims[2]);
        return r;
    }
    r.datasetId = opened.datasetId;
    r.units = opened.physicalUnits;
    return r;
}

/// \a img's values through \a lut as RGBA bytes (alpha 0 where blank). Values
/// are clamped to the table's range first, so a saturated pixel takes the top
/// colour whatever the table does outside its range (as the image viewer's
/// blend does).
void mapThroughTable(vtkLookupTable *lut, vtkImageData *img, std::vector<std::uint8_t> &out)
{
    const std::size_t n = static_cast<std::size_t>(img->GetNumberOfPoints());
    out.assign(n * 4, 0);
    if (!lut || img->GetScalarType() != VTK_FLOAT)
        return;
    const double *range = lut->GetRange();
    const float lo = static_cast<float>(std::min(range[0], range[1]));
    const float hi = static_cast<float>(std::max(range[0], range[1]));
    const float *src = static_cast<const float *>(img->GetScalarPointer());
    std::vector<float> clamped(n);
    for (std::size_t i = 0; i < n; ++i)
        clamped[i] = std::isnan(src[i]) ? src[i] : std::clamp(src[i], lo, hi);
    lut->MapScalarsThroughTable(clamped.data(), out.data(), VTK_FLOAT, static_cast<int>(n), 1,
                                VTK_RGBA);
}

/// A bar: the colours \a colourAt gives over lo..hi on \a mode's scale, five
/// ticks, the field's name, and the threshold when there is one.
void paintFieldBar(VolumeColorbar *bar, const std::function<void(double, double *)> &colourAt,
                   LutScale::Mode mode, double gamma, double lo, double hi, const QString &title,
                   std::optional<double> thresholdPosition, const QString &thresholdCaption)
{
    bar->setFromScale([=](double u) { return LutScale::valueAt(mode, u, lo, hi, gamma); }, colourAt);
    bar->setTitle(title);
    bar->setThreshold(thresholdPosition, thresholdCaption);
}

/// Bars side by side from the right edge: each one's offset is the widths of
/// the bars before it.
void stackBars(const QList<QPointer<VolumeColorbar>> &bars, int firstOffset)
{
    int offset = firstOffset;
    for (const auto &bar : bars) {
        if (!bar || bar->isHidden())
            continue;
        bar->setRightOffset(offset);
        offset += bar->width() + kBarGap;
    }
}

}   // namespace

// ── Helpers on the window's fields ──────────────────────────────────────────

SimField *vtkWindowCube::fieldNamed(const QString &name) const
{
    for (const auto &f : m_fields) {
        if (f->name == name)
            return f.get();
    }
    return nullptr;
}

QString vtkWindowCube::mainFieldName() const
{
    return visivo::isFieldReference(this->filepath) ? visivo::fieldNames().value(this->filepath)
                                                    : QFileInfo(this->filepath).completeBaseName();
}

QList<QPair<QString, QString>> vtkWindowCube::fieldsToAdd() const
{
    QList<QPair<QString, QString>> out;
    for (const auto &sibling : m_siblingFieldList) {
        if (sibling.second == this->filepath || this->fieldNamed(sibling.first))
            continue;
        out << sibling;
    }
    return out;
}

QString vtkWindowCube::whyNoFieldCanBeAdded() const
{
    if (!m_simulationVolume || m_siblingFieldList.size() < 2)
        return u"This dataset has no other field of the same run."_s;
    if (int(m_fields.size()) + 1 >= vf::kMaxFields)
        return u"At most %1 fields in one view, this one included."_s.arg(vf::kMaxFields);
    if (this->fieldsToAdd().isEmpty())
        return u"Every other field is in the view already."_s;
    return {};
}

vtkImageData *vtkWindowCube::sliceFrameImage() const
{
    if (m_fieldViews && m_fieldViews->compositeShown && m_fieldViews->compositeImage)
        return m_fieldViews->compositeImage;
    return vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
}

QString vtkWindowCube::fieldSliceKey(int index) const
{
    return u"%1:%2"_s.arg(visivo::simvol::axisLetter(m_sliceAxis)).arg(index);
}

// ── Adding and removing ─────────────────────────────────────────────────────

void vtkWindowCube::addFieldFromMenu()
{
    const QString why = this->whyNoFieldCanBeAdded();
    if (!why.isEmpty()) {
        this->statusBar()->showMessage(why, 6000);
        return;
    }
    QMenu menu(this);
    for (const auto &f : this->fieldsToAdd()) {
        QAction *a = menu.addAction(f.first);
        a->setData(f.second);
    }
    if (QAction *chosen = menu.exec(QCursor::pos()))
        this->addField(chosen->text(), chosen->data().toString());
}

void vtkWindowCube::addField(const QString &name, const QString &path)
{
    const QString why = this->whyNoFieldCanBeAdded();
    if (!why.isEmpty() || this->fieldNamed(name)) {
        if (!why.isEmpty())
            this->statusBar()->showMessage(why, 6000);
        return;
    }
    auto field = std::make_unique<SimField>();
    field->name = name;
    field->path = path;
    field->colourMap = QString::fromLatin1(defaultMapFor(int(m_fields.size())));
    field->lut = vtkSmartPointer<vtkLookupTable>::New();
    field->opening = true;
    SimField *raw = field.get();
    m_fields.push_back(std::move(field));
    this->rebuildFieldsPanel();
    this->statusBar()->showMessage(u"Adding the field %1..."_s.arg(name));

    // Opened on the backend as its own dataset (one cube per field), in this
    // window's session; its planes and voxels come once it is.
    auto *watcher = new QFutureWatcher<FieldOpenResult>(this);
    connect(watcher, &QFutureWatcher<FieldOpenResult>::finished, this, [this, watcher, raw]() {
        const FieldOpenResult r = watcher->result();
        watcher->deleteLater();
        SimField *f = this->fieldNamed(r.name);
        if (!f || f != raw)
            return;   // removed meanwhile
        f->opening = false;
        if (!r.error.isEmpty()) {
            this->statusBar()->showMessage(r.error, 9000);
            this->removeField(r.name);
            return;
        }
        f->datasetId = r.datasetId;
        f->units = r.units;
        // The values come in the units this window shows, as its own field's do.
        BackendClient::setDisplayValueScale(
                f->datasetId, visivo::units::columnScale(f->units, visivo::units::valueColumn(),
                                                         m_showPhysical));
        this->statusBar()->showMessage(u"Field %1 added."_s.arg(r.name), 4000);
        this->requestFieldSlices();
        this->fetchFieldVolume(*f);
    });
    watcher->setFuture(QtConcurrent::run(
            &openFieldDataset, this->remoteBackendUrl, this->remoteBackendToken,
            this->remoteSessionId, name, path,
            std::array<int, 3>{ this->remoteDatasetWidth, this->remoteDatasetHeight,
                                this->remoteDatasetDepth }));
}

void vtkWindowCube::removeField(const QString &name)
{
    for (auto it = m_fields.begin(); it != m_fields.end(); ++it) {
        if ((*it)->name != name)
            continue;
        if ((*it)->bar3d)
            (*it)->bar3d->deleteLater();
        if ((*it)->bar2d)
            (*it)->bar2d->deleteLater();
        m_fields.erase(it);
        break;
    }
    this->rebuildFieldVolumes();
    this->updateFieldComposite();
    this->rebuildFieldsPanel();
    this->refreshVolumeColorbar();
    if (ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::fieldLookChanged(SimField *field, bool rebuild3d)
{
    if (field && field->volumeImage) {
        this->buildFieldVolume(*field);
        if (rebuild3d)
            this->rebuildFieldVolumes();
    } else if (rebuild3d) {
        this->rebuildFieldVolumes();
    }
    this->updateFieldComposite();
    this->refreshVolumeColorbar();
    if (ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
}

// ── 2-D: one picture of the fields ──────────────────────────────────────────

bool vtkWindowCube::fieldComposite2dWanted() const
{
    return m_simulationVolume && m_fieldViews && !m_fields.empty()
            && m_sliceBackdropProductId.isEmpty();
}

void vtkWindowCube::fieldsFollowSlice()
{
    if (m_fields.empty() || !m_fieldViews)
        return;
    this->updateFieldComposite();
}

void vtkWindowCube::requestFieldSlices()
{
    if (!this->fieldComposite2dWanted() || this->m_displayedSliceIndex < 0)
        return;
    const int index = this->m_displayedSliceIndex;
    const QString key = this->fieldSliceKey(index);
    const QString axis = visivo::simvol::axisLetter(m_sliceAxis);
    for (const auto &f : m_fields) {
        if (!f->shown || f->datasetId.isEmpty() || f->slices.contains(key)
            || f->sliceInFlight.contains(key))
            continue;
        f->sliceInFlight.insert(key);
        auto *watcher = new QFutureWatcher<RemoteCubeSliceResult>(this);
        const QString name = f->name;
        connect(watcher, &QFutureWatcher<RemoteCubeSliceResult>::finished, this,
                [this, watcher, name, key]() {
                    const RemoteCubeSliceResult result = watcher->result();
                    watcher->deleteLater();
                    SimField *field = this->fieldNamed(name);
                    if (!field)
                        return;
                    field->sliceInFlight.remove(key);
                    this->applyFieldSliceResult(name, result);
                });
        watcher->setFuture(QtConcurrent::run(&fetchRemoteFieldSlice, this->remoteBackendUrl,
                                             f->datasetId, index, this->remoteSessionId,
                                             this->remoteBackendToken, axis));
    }
}

void vtkWindowCube::applyFieldSliceResult(const QString &name, const RemoteCubeSliceResult &result)
{
    SimField *f = this->fieldNamed(name);
    if (!f)
        return;
    if (!result.valid || !result.imageData) {
        this->statusBar()->showMessage(u"The plane of %1 could not be read: %2"_s.arg(
                                               name, result.errorMessage), 8000);
        return;
    }
    const QString key = u"%1:%2"_s.arg(visivo::simvol::axisLetter(result.axis)).arg(result.index);
    f->slices.insert(key, result.imageData);
    f->sliceOrder.removeAll(key);
    f->sliceOrder << key;
    while (f->sliceOrder.size() > kFieldSliceCache)
        f->slices.remove(f->sliceOrder.takeFirst());
    if (key == this->fieldSliceKey(this->m_displayedSliceIndex))
        this->updateFieldComposite();
}

int vtkWindowCube::fieldSeamColumn(int width, double origin, double stride) const
{
    const int axis = this->slicePlaneAxes()[0];
    const double crval = this->remoteDatasetCrval[axis];
    const double crpix = this->remoteDatasetCrpix[axis];
    const double cdelt = this->remoteDatasetCdelt[axis];
    const auto pixelOf = [&](double code) {
        return (code - crval) / cdelt + crpix - 1.0;
    };
    if (!(cdelt != 0.0) || !std::isfinite(cdelt))
        return width / 2;
    double fullIndex;
    if (m_fieldSeam) {
        // Typed in the units shown; the file's coordinates are in code units.
        const double factor = m_axisUnits.factor[std::size_t(axis)];
        fullIndex = pixelOf(*m_fieldSeam / (factor != 0.0 ? factor : 1.0));
    } else if (m_fieldLayout == vf::Layout::Mirrored) {
        // The axis of an axisymmetric run is x = 0 when the grid holds it, else
        // the grid's own edge.
        const double zero = pixelOf(0.0);
        fullIndex = std::isfinite(zero) ? zero : -0.5;
    } else {
        return width / 2;
    }
    return vf::seamColumnFromIndex(fullIndex, origin, stride);
}

void vtkWindowCube::setFieldCompositeVisible(bool on)
{
    if (!m_fieldViews)
        return;
    SimFieldViews &v = *m_fieldViews;
    const bool changed = v.compositeShown != on;
    v.compositeShown = on;
    if (v.composite)
        v.composite->SetVisibility(on ? 1 : 0);
    if (v.base)
        v.base->SetVisibility(on ? 0 : 1);
    this->m_sliceColorbar->SetVisibility(on ? 0 : 1);
    if (on)
        this->hideSliceDetail();
    if (changed) {
        // The frame the pane fits and the axes follow what is drawn.
        this->layout2dPaneFor(/*slice=*/true, /*force=*/true);
    }
    this->refreshFieldBars2d();
    if (this->sliceWin)
        this->sliceWin->Render();
}

void vtkWindowCube::updateFieldComposite()
{
    if (!this->fieldComposite2dWanted()) {
        this->setFieldCompositeVisible(false);
        return;
    }
    auto *mainImage =
            vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
    if (!mainImage || this->m_displayedSliceIndex < 0) {
        this->setFieldCompositeVisible(false);
        return;
    }
    // Which extra fields take part, and whether their planes are here.
    const QString key = this->fieldSliceKey(this->m_displayedSliceIndex);
    int extrasShown = 0;
    bool missing = false;
    for (const auto &f : m_fields) {
        if (!f->shown)
            continue;
        ++extrasShown;
        if (f->datasetId.isEmpty() || !f->slices.contains(key))
            missing = true;
    }
    if (extrasShown == 0) {
        // Nothing to combine: the window's own slice, as ever.
        this->setFieldCompositeVisible(false);
        return;
    }
    if (missing) {
        this->requestFieldSlices();
        // The window's own slice is on screen until every plane is here; an
        // older picture would show another plane than the slider says.
        this->setFieldCompositeVisible(false);
        return;
    }

    int dims[3] = { 0, 0, 0 };
    mainImage->GetDimensions(dims);
    const std::size_t pixels = std::size_t(dims[0]) * std::size_t(dims[1]);

    struct Layer
    {
        std::vector<std::uint8_t> rgba;
        double opacity{ 1.0 };
    };
    std::vector<Layer> layers;
    if (m_mainFieldShown) {
        Layer l;
        l.opacity = m_mainFieldOpacity;
        mapThroughTable(this->lutSlice, mainImage, l.rgba);
        layers.push_back(std::move(l));
    }
    for (const auto &f : m_fields) {
        if (!f->shown)
            continue;
        vtkImageData *img = f->slices.value(key);
        int d[3] = { 0, 0, 0 };
        img->GetDimensions(d);
        const double *fo = img->GetOrigin(), *fs = img->GetSpacing();
        const double *mo = mainImage->GetOrigin(), *ms = mainImage->GetSpacing();
        if (d[0] != dims[0] || d[1] != dims[1] || std::abs(fo[0] - mo[0]) > 1e-9
            || std::abs(fo[1] - mo[1]) > 1e-9 || std::abs(fs[0] - ms[0]) > 1e-9
            || std::abs(fs[1] - ms[1]) > 1e-9) {
            this->statusBar()->showMessage(
                    u"%1's plane is %2 x %3 pixels, not %4 x %5 as this window's: not combined."_s
                            .arg(f->name).arg(d[0]).arg(d[1]).arg(dims[0]).arg(dims[1]), 8000);
            this->setFieldCompositeVisible(false);
            return;
        }
        // The table: typed Min/Max, else this plane's range (kept while the
        // range is locked, as the window's own slice does).
        double lo = 0.0, hi = 1.0;
        const bool typed = f->lo && f->hi;
        if (typed) {
            lo = *f->lo;
            hi = *f->hi;
        } else if (this->m_sliceRangeLocked && f->sliceRangeKnown) {
            lo = f->sliceRange[0];
            hi = f->sliceRange[1];
        } else {
            double r[2] = { 0.0, 1.0 };
            if (vtkDataArray *s = img->GetPointData()->GetScalars())
                s->GetFiniteRange(r, 0);
            lo = f->lo.value_or(r[0]);
            hi = f->hi.value_or(r[1]);
        }
        if (f->scale == LutScale::Mode::Log && !typed) {
            const double floorValue = LutScale::sampledPositiveFloor(img);
            if (floorValue > 0.0)
                lo = floorValue;
        }
        if (!(hi > lo))
            hi = lo + std::max(1e-12, std::abs(lo) * 1e-6);
        f->sliceRange = { lo, hi };
        f->sliceRangeKnown = true;
        LutScale::build(f->lut, f->colourMap.toStdString(), f->scale, lo, hi,
                        this->m_sliceScaleGamma);
        f->lut->SetNanColor(0., 0., 0., 0.);
        Layer l;
        l.opacity = f->opacity;
        mapThroughTable(f->lut, img, l.rgba);
        layers.push_back(std::move(l));
    }
    if (layers.empty()) {
        this->setFieldCompositeVisible(false);
        return;
    }

    // The composite's pixels and where they sit on the slice's frame.
    vf::Layout layout = m_fieldLayout;
    if (vf::usesHalves(layout) && layers.size() < 2)
        layout = vf::Layout::Layered;
    std::vector<visivo::blend::Rgba8> out;
    int outWidth = dims[0];
    int indexOffset = 0;
    if (vf::usesHalves(layout)) {
        const int seam = this->fieldSeamColumn(dims[0], mainImage->GetOrigin()[0],
                                               mainImage->GetSpacing()[0]);
        const vf::SplitLayout split = vf::splitLayout(dims[0], seam, layout == vf::Layout::Mirrored);
        outWidth = split.outWidth;
        indexOffset = split.indexOffset;
        out.resize(std::size_t(std::max(0, outWidth)) * std::size_t(dims[1]));
        vf::composeSplit(split, layers[0].rgba.data(), layers[1].rgba.data(), layers[0].opacity,
                         layers[1].opacity, dims[1], out.data());
    } else {
        out.resize(pixels);
        std::vector<vf::ColourLayer> colour;
        for (const Layer &l : layers)
            colour.push_back({ l.rgba.data(), l.opacity });
        vf::compose(layout, colour, pixels, out.data());
    }
    if (outWidth <= 0) {
        this->setFieldCompositeVisible(false);
        return;
    }

    // One RGBA picture on the slice's own frame: the right half keeps its place
    // when mirrored, so everything drawn in plane pixels still lands.
    SimFieldViews &v = *m_fieldViews;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, outWidth - 1, 0, dims[1] - 1, 0, 0);
    const double *origin = mainImage->GetOrigin();
    const double *spacing = mainImage->GetSpacing();
    image->SetOrigin(origin[0] + indexOffset * spacing[0], origin[1], origin[2]);
    image->SetSpacing(spacing[0], spacing[1], spacing[2]);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
    std::memcpy(image->GetScalarPointer(), out.data(), out.size() * sizeof(visivo::blend::Rgba8));

    bool geometryChanged = !v.compositeImage;
    if (v.compositeImage) {
        double a[6], b[6];
        v.compositeImage->GetBounds(a);
        image->GetBounds(b);
        for (int i = 0; i < 6; ++i)
            geometryChanged = geometryChanged || std::abs(a[i] - b[i]) > 1e-9;
    }
    v.compositeImage = image;
    if (!v.composite) {
        vtkNew<vtkImageSliceMapper> mapper;
        mapper->SetInputData(image);
        v.composite = vtkSmartPointer<vtkImageSlice>::New();
        v.composite->SetMapper(mapper);
        v.composite->GetProperty()->SetInterpolationTypeToNearest();
        v.composite->GetProperty()->SetLayerNumber(2);
        v.composite->PickableOff();
        if (v.stack)
            v.stack->AddImage(v.composite);
    } else if (auto *mapper = vtkImageSliceMapper::SafeDownCast(v.composite->GetMapper())) {
        mapper->SetInputData(image);
    }
    const bool wasShown = v.compositeShown;
    this->setFieldCompositeVisible(true);
    if (wasShown && geometryChanged)
        this->layout2dPaneFor(/*slice=*/true, /*force=*/true);
    this->refreshFieldBars2d();
    if (this->sliceWin)
        this->sliceWin->Render();
}

void vtkWindowCube::refreshFieldBars2d()
{
    if (!m_fieldViews)
        return;
    SimFieldViews &v = *m_fieldViews;
    const bool on = v.compositeShown;
    const auto ensure = [this](QPointer<VolumeColorbar> &bar) {
        if (!bar) {
            bar = new VolumeColorbar(this);
            bar->hide();
            bar->attachTo(ui->vtkImage);
        }
    };
    QList<QPointer<VolumeColorbar>> order;
    if (on && m_mainFieldShown) {
        ensure(v.mainBar2d);
        const double *r = this->lutSlice->GetRange();
        paintFieldBar(
                v.mainBar2d,
                [this](double value, double *rgb) { this->lutSlice->GetColor(value, rgb); },
                this->m_sliceScaleMode, this->m_sliceScaleGamma, r[0], r[1],
                this->mainFieldName(), std::nullopt, QString());
        v.mainBar2d->show();
        order << v.mainBar2d;
    } else if (v.mainBar2d) {
        v.mainBar2d->hide();
    }
    for (const auto &f : m_fields) {
        if (on && f->shown && f->sliceRangeKnown) {
            ensure(f->bar2d);
            vtkLookupTable *lut = f->lut;
            paintFieldBar(
                    f->bar2d, [lut](double value, double *rgb) { lut->GetColor(value, rgb); },
                    f->scale, this->m_sliceScaleGamma, f->sliceRange[0], f->sliceRange[1], f->name,
                    std::nullopt, QString());
            f->bar2d->show();
            order << f->bar2d;
        } else if (f->bar2d) {
            f->bar2d->hide();
        }
    }
    stackBars(order, 0);
}

// ── 3-D: fields in one box ──────────────────────────────────────────────────

bool vtkWindowCube::fieldsDrawn3d() const
{
    return m_fieldViews && m_fieldViews->multiInRenderer && m_fieldViews->multi;
}

bool vtkWindowCube::fieldCastVisible() const
{
    return this->fieldsDrawn3d() && m_fieldViews->multi->GetVisibility() != 0;
}

void vtkWindowCube::fetchFieldVolume(SimField &field)
{
    if (field.datasetId.isEmpty() || field.opening || field.volumeImage)
        return;
    if (this->m_effectiveRenderMode == RenderMode::Remote) {
        // The backend draws the box in Remote, and knows one field.
        this->statusBar()->showMessage(
                u"%1 is in the 2-D view; the 3-D box needs the local renderer (View > Render Mode)."_s
                        .arg(field.name), 9000);
        return;
    }
    if (this->m_lod && this->m_lod->active()) {
        this->statusBar()->showMessage(
                u"Clear the refined box to draw %1 in the 3-D box."_s.arg(field.name), 9000);
        return;
    }
    auto *main = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    int dims[3] = { 0, 0, 0 };
    if (main)
        main->GetDimensions(dims);
    // Not drawn yet (the placeholder), or only one voxel: the cube's arrival
    // (applyCubeOpenResult) brings the field's voxels.
    if (!main || !this->m_initialThresholdApplied || dims[0] < 2 || dims[1] < 2 || dims[2] < 2)
        return;
    field.opening = true;
    field.volumeError = false;
    std::optional<std::array<int, 6>> roi;
    if (this->usingHighResCube)
        roi = this->currentRemoteRoi;
    auto *watcher = new QFutureWatcher<RemoteFieldVolumeResult>(this);
    connect(watcher, &QFutureWatcher<RemoteFieldVolumeResult>::finished, this, [this, watcher]() {
        const RemoteFieldVolumeResult r = watcher->result();
        watcher->deleteLater();
        this->applyFieldVolumeResult(r);
    });
    this->statusBar()->showMessage(u"Reading %1 for the 3-D box..."_s.arg(field.name));
    watcher->setFuture(QtConcurrent::run(&fetchRemoteFieldVolume, this->remoteBackendUrl,
                                         this->remoteSessionId, this->remoteBackendToken,
                                         field.name, field.datasetId,
                                         std::array<int, 3>{ dims[0], dims[1], dims[2] }, roi,
                                         std::max(1, this->m_previewStride)));
}

void vtkWindowCube::applyFieldVolumeResult(const RemoteFieldVolumeResult &result)
{
    SimField *f = this->fieldNamed(result.name);
    if (!f)
        return;   // removed meanwhile
    f->opening = false;
    if (!result.valid || !result.image) {
        f->volumeError = true;
        this->statusBar()->showMessage(
                u"%1 is not in the 3-D box: %2"_s.arg(result.name, result.errorMessage), 10000);
        this->rebuildFieldVolumes();
        return;
    }
    // The cube may have changed grid while this was read: then it was read for
    // the old one, and is read again.
    auto *main = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    int md[3] = { 0, 0, 0 }, fd[3] = { 0, 0, 0 };
    if (main)
        main->GetDimensions(md);
    result.image->GetDimensions(fd);
    if (!main || md[0] != fd[0] || md[1] != fd[1] || md[2] != fd[2]) {
        this->fetchFieldVolume(*f);
        return;
    }
    // On the cube's own frame, and its scalars named alike (the cast reads one
    // name from every input).
    result.image->SetOrigin(main->GetOrigin());
    result.image->SetSpacing(main->GetSpacing());
    if (vtkDataArray *src = main->GetPointData()->GetScalars()) {
        if (vtkDataArray *dst = result.image->GetPointData()->GetScalars())
            dst->SetName(src->GetName());
    }
    const bool first = !f->scaleChosen;
    f->scaleChosen = true;
    f->volumeImage = result.image;
    f->dataRange = result.visibleRange;
    f->sentinel = result.invisibleSentinel;

    // The ramp starts at a high quantile of the field's own voxels.
    {
        const std::size_t n = static_cast<std::size_t>(result.image->GetNumberOfPoints());
        std::size_t step = std::max<std::size_t>(1, n / kThresholdSample);
        if (step > 1)
            step |= 1;   // odd: no lock-step with the rows' length
        const float *p = static_cast<const float *>(result.image->GetScalarPointer());
        std::vector<float> sample;
        sample.reserve(n / step + 1);
        for (std::size_t i = 0; i < n; i += step) {
            if (std::isfinite(p[i]) && p[i] >= f->dataRange[0])   // not the blank sentinel
                sample.push_back(p[i]);
        }
        const double q = vf::quantile(sample, vf::kDefaultThresholdQuantile);
        f->defaultThreshold = std::isfinite(q) ? q : f->dataRange[0];
    }
    // The scale a field opens with, once: log for a positive field over three decades.
    if (first && vf::prefersLog(f->dataRange[0], f->dataRange[1]))
        f->scale = LutScale::Mode::Log;

    if (!f->source)
        f->source = vtkSmartPointer<vtkTrivialProducer>::New();
    f->source->SetOutput(f->volumeImage);
    this->buildFieldVolume(*f);
    this->rebuildFieldVolumes();
    this->rebuildFieldsPanel();
    const double mb = double(result.image->GetNumberOfPoints()) * 4.0 / (1024.0 * 1024.0);
    this->statusBar()->showMessage(u"%1 is in the 3-D box (%2 MB)."_s.arg(f->name).arg(mb, 0, 'f', 1),
                                   6000);
    this->refreshVolumeColorbar();
    if (ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::buildFieldVolume(SimField &f)
{
    if (!f.volumeImage)
        return;
    if (!f.colours) {
        f.colours = vtkSmartPointer<vtkColorTransferFunction>::New();
        f.ramp = vtkSmartPointer<vtkPiecewiseFunction>::New();
        f.property = vtkSmartPointer<vtkVolumeProperty>::New();
        f.volume = vtkSmartPointer<vtkVolume>::New();
        f.property->SetColor(f.colours);
        f.property->SetScalarOpacity(f.ramp);
        f.property->SetInterpolationTypeToLinear();
        f.property->ShadeOff();
        f.volume->SetProperty(f.property);
        if (vtkVolumeProperty *main = this->volume->GetProperty())
            f.property->SetScalarOpacityUnitDistance(main->GetScalarOpacityUnitDistance());
    }
    double lo = f.lo.value_or(f.dataRange[0]);
    double hi = f.hi.value_or(f.dataRange[1]);
    if (!(hi > lo))
        hi = lo + std::max(1e-12, std::abs(lo) * 1e-6);
    if (f.scale == LutScale::Mode::Log) {
        const auto r = LutScale::rangeFor(LutScale::Mode::Log, lo, hi);
        lo = r[0];
        hi = r[1];
    }
    // Colours: the map over Min..Max on the field's scale, as the window's own.
    vtkNew<vtkLookupTable> lut;
    LutScale::build(lut, f.colourMap.toStdString(), f.scale, lo, hi, 2.0);
    ColorMaps::SetColorTransferFunction(lut, f.colours);

    // Opacity: the window's ramp from the threshold, the sentinel transparent,
    // all of it scaled by the field's opacity.
    const double threshold = std::clamp(f.threshold.value_or(f.defaultThreshold), lo, hi);
    vtkNew<vtkPiecewiseFunction> shape;
    fillRampOn(shape, f.scale, 2.0, threshold, lo, hi);
    std::vector<std::pair<double, double>> nodes;
    nodes.emplace_back(f.sentinel, 0.0);
    for (int i = 0; i < shape->GetSize(); ++i) {
        double node[4];
        shape->GetNodeValue(i, node);
        nodes.emplace_back(node[0], node[1]);
    }
    vf::scaleOpacityNodes(nodes, f.opacity);
    f.ramp->RemoveAllPoints();
    for (const auto &n : nodes)
        f.ramp->AddPoint(n.first, n.second);
}

void vtkWindowCube::rebuildFieldVolumes()
{
    if (!m_fieldViews)
        return;
    SimFieldViews &v = *m_fieldViews;
    auto *rw = ui->vtkCube->renderWindow();
    auto *renderer = rw ? rw->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!renderer)
        return;
    if (v.multiInRenderer && v.multi)
        renderer->RemoveViewProp(v.multi);
    v.multiInRenderer = false;

    std::vector<SimField *> drawn;
    for (const auto &f : m_fields) {
        if (f->shown && f->volumeImage && f->volume)
            drawn.push_back(f.get());
    }
    if (drawn.empty() || (this->m_lod && this->m_lod->active())) {
        v.multi = nullptr;
        v.mapper = nullptr;
        v.mainProxy = nullptr;
        this->syncFieldVolumeVisibility();   // the window's own volume again
        this->refreshFieldBars3d();
        return;
    }

    // ONE ray cast of every volume: the window's own as port 0 (its transfer
    // functions are shared, so its threshold and map keep working), each field
    // on the next ports. VTK's multi-input cast composites front to back, with
    // each input's own colour and opacity functions; it supports no MIP and no
    // cropping.
    v.mapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
    v.mapper->SetBlendModeToComposite();
    v.mapper->SetAutoAdjustSampleDistances(1);
    v.mapper->SetUseJittering(1);
    v.mapper->SetInputConnection(0, this->cubeDisplaySource->GetOutputPort());
    v.mainProxy = vtkSmartPointer<vtkVolume>::New();
    if (m_mainFieldShown) {
        v.mainProxy->SetProperty(this->volume->GetProperty());
    } else {
        // Hidden in the box: the same colours, nothing opaque.
        vtkNew<vtkVolumeProperty> hidden;
        hidden->DeepCopy(this->volume->GetProperty());
        vtkNew<vtkPiecewiseFunction> none;
        none->AddPoint(0.0, 0.0);
        none->AddPoint(1.0, 0.0);
        hidden->SetScalarOpacity(none);
        v.mainProxy->SetProperty(hidden);
    }
    v.multi = vtkSmartPointer<vtkMultiVolume>::New();
    v.multi->SetMapper(v.mapper);
    v.multi->SetVolume(v.mainProxy, 0);
    int port = 1;
    for (SimField *f : drawn) {
        v.mapper->SetInputConnection(port, f->source->GetOutputPort());
        v.multi->SetVolume(f->volume, port);
        ++port;
    }
    renderer->AddVolume(v.multi);
    v.multiInRenderer = true;
    this->syncFieldVolumeVisibility();
    this->refreshFieldBars3d();
}

void vtkWindowCube::syncFieldVolumeVisibility()
{
    if (!m_fieldViews || (m_fields.empty() && !m_fieldViews->multi))
        return;   // no field: the window's own volume is left as it was
    if (this->fieldsDrawn3d()) {
        // The one cast stands in for the window's own volume; a refined box
        // (its blocks) stands in for both.
        this->volume->SetVisibility(0);
        const bool lod = this->m_lod && this->m_lod->active();
        m_fieldViews->multi->SetVisibility(
                (this->m_show3dRendering && !this->m_isosurfaceDrawn && !lod) ? 1 : 0);
    } else if (!(this->m_lod && this->m_lod->active())) {
        this->volume->SetVisibility(this->m_show3dRendering ? 1 : 0);
    }
}

void vtkWindowCube::refreshFieldBars3d()
{
    const bool show = this->fieldsDrawn3d() && this->m_show3dRendering && !this->m_isosurfaceDrawn;
    QList<QPointer<VolumeColorbar>> order;
    for (const auto &f : m_fields) {
        const bool mine = show && f->shown && f->volumeImage && f->volume;
        if (!mine) {
            if (f->bar3d)
                f->bar3d->hide();
            continue;
        }
        if (!f->bar3d) {
            f->bar3d = new VolumeColorbar(this);
            f->bar3d->hide();
            f->bar3d->attachTo(ui->vtkCube);
        }
        double lo = f->lo.value_or(f->dataRange[0]);
        double hi = f->hi.value_or(f->dataRange[1]);
        if (!(hi > lo))
            hi = lo + std::max(1e-12, std::abs(lo) * 1e-6);
        if (f->scale == LutScale::Mode::Log) {
            const auto r = LutScale::rangeFor(LutScale::Mode::Log, lo, hi);
            lo = r[0];
            hi = r[1];
        }
        const double threshold = std::clamp(f->threshold.value_or(f->defaultThreshold), lo, hi);
        vtkColorTransferFunction *ctf = f->colours;
        paintFieldBar(f->bar3d, [ctf](double value, double *rgb) { ctf->GetColor(value, rgb); },
                      f->scale, 2.0, lo, hi, f->name,
                      scalePositionOn(f->scale, 2.0, threshold, lo, hi),
                      u"threshold %1"_s.arg(QString::number(threshold, 'g', 3)));
        f->bar3d->show();
        order << f->bar3d;
    }
    this->layoutFieldBars3d();
}

void vtkWindowCube::layoutFieldBars3d()
{
    QList<QPointer<VolumeColorbar>> bars;
    for (const auto &f : m_fields)
        bars << f->bar3d;
    // After the window's own bar, when it is on screen.
    const int first = (this->m_volumeColorbar && !this->m_volumeColorbar->isHidden())
            ? this->m_volumeColorbar->width() + kBarGap
            : 0;
    stackBars(bars, first);
}

void vtkWindowCube::fieldsFollowMainCube()
{
    if (m_fields.empty() || !m_fieldViews)
        return;
    auto *main = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!main)
        return;
    int md[3] = { 0, 0, 0 };
    main->GetDimensions(md);
    bool dropped = false;
    for (const auto &f : m_fields) {
        if (f->volumeImage) {
            int fd[3] = { 0, 0, 0 };
            f->volumeImage->GetDimensions(fd);
            if (fd[0] == md[0] && fd[1] == md[1] && fd[2] == md[2])
                continue;
            // Another grid is drawn now (the preview gave way to the full cube,
            // or the other way round): this field's voxels are read again.
            f->volumeImage = nullptr;
            dropped = true;
        }
        f->volumeError = false;
    }
    if (dropped)
        this->rebuildFieldVolumes();
    for (const auto &f : m_fields) {
        if (!f->volumeImage && !f->opening && !f->datasetId.isEmpty())
            this->fetchFieldVolume(*f);
    }
}

// ── The Inspector's rows ────────────────────────────────────────────────────

void vtkWindowCube::addFieldInspectorRows(InspectorPanel *panel)
{
    if (!panel || !m_simulationVolume || m_siblingFieldList.size() < 2)
        return;
    panel->addPropertyCaption(u"FIELDS"_s);
    auto *host = new QWidget();
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    m_fieldsHost = host;
    panel->addPropertiesSection(host);
    this->rebuildFieldsPanel();
}

void vtkWindowCube::rebuildFieldsPanel()
{
    if (!m_fieldsHost)
        return;
    auto *layout = qobject_cast<QVBoxLayout *>(m_fieldsHost->layout());
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    // Add Field, and how the 2-D slice combines the fields.
    auto *top = new QWidget();
    auto *topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    auto *add = new QPushButton(u"Add Field…"_s);
    add->setToolTip(u"Add another variable of this run to the same view: in the slice, and in "
                    u"the 3-D box (local rendering)."_s);
    const QString why = this->whyNoFieldCanBeAdded();
    add->setEnabled(why.isEmpty());
    if (!why.isEmpty())
        add->setToolTip(why);
    connect(add, &QPushButton::clicked, this, &vtkWindowCube::addFieldFromMenu);
    topLayout->addWidget(add);
    topLayout->addStretch(1);
    layout->addWidget(top);

    auto *layoutRow = new QWidget();
    auto *lr = new QHBoxLayout(layoutRow);
    lr->setContentsMargins(0, 0, 0, 0);
    lr->addWidget(new QLabel(u"Slice"_s));
    m_fieldLayoutCombo = new QComboBox();
    for (const auto l : { vf::Layout::Layered, vf::Layout::Additive, vf::Layout::Split,
                          vf::Layout::Mirrored })
        m_fieldLayoutCombo->addItem(QString::fromLatin1(vf::layoutLabel(l)), int(l));
    m_fieldLayoutCombo->setCurrentIndex(int(m_fieldLayout));
    m_fieldLayoutCombo->setToolTip(
            u"Layered: each field over the one below, by its opacity. Additive: the colours added "
            u"(the image viewer's Additive blend). Split: the first field left of the seam, the "
            u"second right of it. Mirrored: the first field reflected about the axis, the second "
            u"as it is."_s);
    connect(m_fieldLayoutCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_fieldLayout = static_cast<vf::Layout>(i);
        if (m_fieldSeamEdit)
            m_fieldSeamEdit->setEnabled(vf::usesHalves(m_fieldLayout));
        this->updateFieldComposite();
    });
    lr->addWidget(m_fieldLayoutCombo, 1);
    m_fieldSeamEdit = new QLineEdit();
    m_fieldSeamEdit->setPlaceholderText(u"seam / axis"_s);
    m_fieldSeamEdit->setToolTip(
            u"Split: where the first field gives way to the second. Mirrored: where the axis of "
            u"symmetry is. In the units shown, along the slice's horizontal axis; empty: the "
            u"middle (Split) or x = 0 (Mirrored)."_s);
    m_fieldSeamEdit->setEnabled(vf::usesHalves(m_fieldLayout));
    if (m_fieldSeam)
        m_fieldSeamEdit->setText(QString::number(*m_fieldSeam, 'g', 6));
    connect(m_fieldSeamEdit, &QLineEdit::editingFinished, this, [this]() {
        bool ok = false;
        const double v = m_fieldSeamEdit->text().toDouble(&ok);
        m_fieldSeam = ok ? std::optional<double>(v) : std::nullopt;
        this->updateFieldComposite();
    });
    lr->addWidget(m_fieldSeamEdit);
    layout->addWidget(layoutRow);

    // One block per field: this window's, then the ones added.
    const auto block = [&](const QString &title, bool shown, double opacity,
                           const std::function<void(bool)> &onShown,
                           const std::function<void(double)> &onOpacity, SimField *field) {
        auto *frame = new QFrame();
        frame->setFrameShape(QFrame::StyledPanel);
        auto *v = new QVBoxLayout(frame);
        v->setContentsMargins(6, 4, 6, 4);
        v->setSpacing(3);
        auto *head = new QHBoxLayout();
        auto *check = new QCheckBox(title);
        check->setChecked(shown);
        check->setToolTip(u"Show or hide this field in the slice and in the 3-D box."_s);
        connect(check, &QCheckBox::toggled, this, [onShown](bool on) { onShown(on); });
        head->addWidget(check, 1);
        if (field) {
            auto *remove = new QToolButton();
            remove->setText(u"Remove"_s);
            const QString name = field->name;
            connect(remove, &QToolButton::clicked, this, [this, name]() { this->removeField(name); });
            head->addWidget(remove);
        }
        v->addLayout(head);
        auto *opRow = new QHBoxLayout();
        opRow->addWidget(new QLabel(u"Opacity"_s));
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setValue(int(std::lround(opacity * 100.0)));
        slider->setToolTip(field ? u"The field's weight in the slice, and the scale of its 3-D "
                                   u"opacity curve."_s
                                 : u"This field's weight in the slice (its 3-D opacity is the "
                                   u"window's own controls)."_s);
        connect(slider, &QSlider::valueChanged, this, [onOpacity](int p) { onOpacity(p / 100.0); });
        opRow->addWidget(slider, 1);
        v->addLayout(opRow);
        if (field) {
            const QString name = field->name;
            auto *mapRow = new QHBoxLayout();
            auto *combo = new QComboBox();
            for (const auto &n : ColorMaps::GetColorMapNames())
                combo->addItem(QString::fromStdString(n));
            combo->setCurrentText(field->colourMap);
            connect(combo, &QComboBox::currentTextChanged, this, [this, name](const QString &t) {
                if (SimField *f = this->fieldNamed(name)) {
                    f->colourMap = t;
                    this->fieldLookChanged(f, false);
                }
            });
            mapRow->addWidget(combo, 1);
            auto *log = new QCheckBox(u"Log"_s);
            log->setChecked(field->scale == LutScale::Mode::Log);
            connect(log, &QCheckBox::toggled, this, [this, name](bool on) {
                if (SimField *f = this->fieldNamed(name)) {
                    f->scale = on ? LutScale::Mode::Log : LutScale::Mode::Linear;
                    f->scaleChosen = true;
                    f->sliceRangeKnown = false;
                    this->fieldLookChanged(f, false);
                }
            });
            mapRow->addWidget(log);
            v->addLayout(mapRow);

            const auto number = [this, name](const char *placeholder, const QString &tip,
                                             std::optional<double> current,
                                             std::optional<double> SimField::*member) {
                auto *edit = new QLineEdit();
                edit->setPlaceholderText(QString::fromLatin1(placeholder));
                edit->setToolTip(tip);
                if (current)
                    edit->setText(QString::number(*current, 'g', 6));
                connect(edit, &QLineEdit::editingFinished, this, [this, name, edit, member]() {
                    SimField *f = this->fieldNamed(name);
                    if (!f)
                        return;
                    bool ok = false;
                    const double x = edit->text().toDouble(&ok);
                    f->*member = ok ? std::optional<double>(x) : std::nullopt;
                    f->sliceRangeKnown = false;
                    this->fieldLookChanged(f, true);
                });
                return edit;
            };
            auto *range = new QHBoxLayout();
            range->addWidget(number("Min (auto)", u"The colour map's low end; empty: the data's."_s,
                                    field->lo, &SimField::lo));
            range->addWidget(number("Max (auto)", u"The colour map's high end; empty: the data's."_s,
                                    field->hi, &SimField::hi));
            v->addLayout(range);
            v->addWidget(number("Threshold (auto)",
                                u"3-D: the value where the field starts to show; below it the "
                                u"field is transparent. Empty: the 95th percentile of its voxels."_s,
                                field->threshold, &SimField::threshold));
            if (field->opening)
                v->addWidget(new QLabel(u"Reading..."_s));
            else if (field->volumeError)
                v->addWidget(new QLabel(u"Not in the 3-D box (see the status line)."_s));
        }
        layout->addWidget(frame);
    };

    block(this->mainFieldName() + u" (this window)"_s, m_mainFieldShown, m_mainFieldOpacity,
          [this](bool on) {
              m_mainFieldShown = on;
              this->rebuildFieldVolumes();
              this->updateFieldComposite();
              if (ui->vtkCube->renderWindow())
                  ui->vtkCube->renderWindow()->Render();
          },
          [this](double o) {
              m_mainFieldOpacity = o;
              this->updateFieldComposite();
          },
          nullptr);
    for (const auto &f : m_fields) {
        const QString name = f->name;
        block(name, f->shown, f->opacity,
              [this, name](bool on) {
                  if (SimField *x = this->fieldNamed(name)) {
                      x->shown = on;
                      this->fieldLookChanged(x, true);
                      if (on)
                          this->requestFieldSlices();
                  }
              },
              [this, name](double o) {
                  if (SimField *x = this->fieldNamed(name)) {
                      x->opacity = o;
                      this->fieldLookChanged(x, false);
                  }
              },
              f.get());
    }
    layout->addStretch(1);
}

void vtkWindowCube::fieldsFollowUnits()
{
    if (m_fields.empty())
        return;
    for (const auto &f : m_fields) {
        if (f->datasetId.isEmpty())
            continue;
        BackendClient::setDisplayValueScale(
                f->datasetId, visivo::units::columnScale(f->units, visivo::units::valueColumn(),
                                                         m_showPhysical));
        // What was read is in the old units: planes now, voxels when the cube
        // comes back (fieldsFollowMainCube reads a field that has none).
        f->slices.clear();
        f->sliceOrder.clear();
        f->sliceInFlight.clear();
        f->sliceRangeKnown = false;
        f->volumeImage = nullptr;
    }
    this->rebuildFieldVolumes();
    this->updateFieldComposite();
}

void vtkWindowCube::sliceLutModified(vtkObject *, unsigned long, void *clientData, void *)
{
    static_cast<vtkWindowCube *>(clientData)->queueFieldCompositeUpdate();
}

void vtkWindowCube::queueFieldCompositeUpdate()
{
    if (!m_fieldViews || !m_fieldViews->compositeShown || m_fieldViews->updatePending)
        return;
    // The table is touched several times by one change (map, range, invert):
    // the picture is drawn once, after them.
    m_fieldViews->updatePending = true;
    QTimer::singleShot(0, this, [this]() {
        if (!m_fieldViews)
            return;
        m_fieldViews->updatePending = false;
        this->updateFieldComposite();
    });
}

void vtkWindowCube::watchSliceLutForFields()
{
    vtkNew<vtkCallbackCommand> cb;
    cb->SetClientData(this);
    cb->SetCallback(&vtkWindowCube::sliceLutModified);
    this->lutSlice->AddObserver(vtkCommand::ModifiedEvent, cb);
}
