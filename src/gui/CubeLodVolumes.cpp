#include "CubeLodVolumes.h"

#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkFloatArray.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkPicker.h>
#include <vtkPointData.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <algorithm>
#include <cstring>

CubeLodVolumes::CubeLodVolumes(vtkVolumeProperty *property)
    : m_property(property)
{
}

CubeLodVolumes::~CubeLodVolumes()
{
    this->clear();
}

namespace {

bool sameGeometry(const CubeLodAssembly::Geometry &a, const CubeLodAssembly::Geometry &b)
{
    return a.placement.origin == b.placement.origin && a.placement.spacing == b.placement.spacing
            && a.dims == b.dims && a.drawn == b.drawn;
}

vtkIdType sampleCount(const CubeLodAssembly::Dims &d)
{
    return vtkIdType(d[0]) * d[1] * d[2];
}

}   // namespace

bool CubeLodVolumes::holds(const std::string &key, const CubeLodAssembly::Geometry &geometry) const
{
    if (key.empty())
        return false;
    return std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry &e) {
        return e.key == key && sameGeometry(e.geometry, geometry);
    });
}

bool CubeLodVolumes::replace(vtkRenderer *renderer, std::vector<Block> blocks, OrderFunction order)
{
    if (!renderer || blocks.empty())
        return false;
    const bool sameRenderer = renderer == m_renderer;
    // The order function indexes the blocks as given: a block that cannot be
    // drawn makes the whole set unusable, rather than shifting the indices.
    // Checked before anything changes, so a refused set leaves the shown one.
    for (const Block &b : blocks) {
        const CubeLodAssembly::Dims &d = b.geometry.dims;
        if (d[0] < 1 || d[1] < 1 || d[2] < 1)
            return false;
        const vtkIdType count = sampleCount(d);
        if (b.array) {
            if (b.array->GetNumberOfComponents() != 1 || b.array->GetNumberOfTuples() != count)
                return false;
        } else if (b.samples.empty()) {
            if (!sameRenderer || !this->holds(b.key, b.geometry))
                return false;
        } else if (b.samples.size() != size_t(count)) {
            return false;
        }
    }

    if (!sameRenderer)
        this->clear();
    std::vector<Entry> previous = std::move(m_entries);
    m_entries.clear();
    m_entries.reserve(blocks.size());
    for (Block &b : blocks) {
        // The same block already on the GPU: kept, textures and all.
        if (!b.key.empty()) {
            auto kept = std::find_if(previous.begin(), previous.end(), [&](const Entry &e) {
                return e.volume && e.key == b.key && sameGeometry(e.geometry, b.geometry);
            });
            if (kept != previous.end()) {
                m_entries.push_back(std::move(*kept));
                kept->volume = nullptr;   // taken
                continue;
            }
        }
        const CubeLodAssembly::Dims &d = b.geometry.dims;
        const vtkIdType count = sampleCount(d);
        Entry e;
        e.geometry = b.geometry;
        e.key = b.key;
        e.image = vtkSmartPointer<vtkImageData>::New();
        e.image->SetDimensions(d[0], d[1], d[2]);
        const double s = b.geometry.placement.spacing;
        e.image->SetSpacing(s, s, s);
        e.image->SetOrigin(b.geometry.placement.origin.data());
        vtkSmartPointer<vtkFloatArray> scalars = b.array;
        if (!scalars) {
            // The block's own copy: the caller's vector goes when this returns.
            scalars = vtkSmartPointer<vtkFloatArray>::New();
            scalars->SetNumberOfComponents(1);
            scalars->SetNumberOfTuples(count);
            std::memcpy(scalars->GetPointer(0), b.samples.data(), size_t(count) * sizeof(float));
            std::vector<float>().swap(b.samples);
        }
        if (!scalars->GetName())
            scalars->SetName("intensity");
        e.image->GetPointData()->SetScalars(scalars);
        e.mapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
        e.mapper->SetInputData(e.image);
        // Each block draws only the region it owns; its ghost samples are
        // there for the interpolation up to its faces.
        e.mapper->CroppingOn();
        e.mapper->SetCroppingRegionFlagsToSubVolume();
        e.volume = vtkSmartPointer<vtkVolume>::New();
        e.volume->SetMapper(e.mapper);
        e.volume->SetProperty(m_property);
        e.volume->SetPickable(1);
        renderer->AddVolume(e.volume);
        m_entries.push_back(std::move(e));
    }
    // What the new set does not keep goes, its GPU textures with it.
    for (Entry &e : previous)
        if (e.volume)
            this->release(e);

    m_renderer = renderer;
    m_orderFunction = std::move(order);
    m_order.clear();   // indices of the old set mean nothing now
    for (Entry &e : m_entries)
        this->apply(e);
    // Re-sorted before every frame of this renderer: rotating the camera
    // changes which block is in front of which.
    if (!m_startObserver) {
        m_startObserver = vtkSmartPointer<vtkCallbackCommand>::New();
        m_startObserver->SetCallback(&CubeLodVolumes::onRendererStart);
        m_startObserver->SetClientData(this);
        m_startTag = renderer->AddObserver(vtkCommand::StartEvent, m_startObserver);
    }
    this->sortForCamera();
    return true;
}

void CubeLodVolumes::release(Entry &e)
{
    // RemoveVolume releases the block's GPU textures.
    if (m_renderer && e.volume)
        m_renderer->RemoveVolume(e.volume);
    // Detached explicitly, so the samples go with the block (or stay only in
    // the cache that shares them) rather than whenever the last reference to
    // the image does.
    if (e.image)
        e.image->GetPointData()->SetScalars(nullptr);
    e.volume = nullptr;
}

void CubeLodVolumes::clear()
{
    if (m_renderer && m_startTag)
        m_renderer->RemoveObserver(m_startTag);
    for (Entry &e : m_entries)
        this->release(e);
    m_entries.clear();
    m_order.clear();
    m_orderFunction = nullptr;
    m_startTag = 0;
    m_startObserver = nullptr;
    m_renderer = nullptr;
}

long long CubeLodVolumes::bytes() const
{
    long long total = 0;
    for (const Entry &e : m_entries)
        total += (long long)e.geometry.dims[0] * e.geometry.dims[1] * e.geometry.dims[2]
                * (long long)sizeof(float);
    return total;
}

void CubeLodVolumes::copySettingsFrom(vtkGPUVolumeRayCastMapper *mapper)
{
    if (!mapper)
        return;
    m_blendMode = mapper->GetBlendMode();
    m_jitter = mapper->GetUseJittering() != 0;
    for (Entry &e : m_entries)
        this->apply(e);
}

void CubeLodVolumes::setVisible(bool visible)
{
    m_visible = visible;
    for (Entry &e : m_entries)
        this->apply(e);
}

void CubeLodVolumes::setUserCrop(const std::optional<CubeLodAssembly::Crop> &crop)
{
    m_userCrop = crop;
    for (Entry &e : m_entries)
        this->apply(e);
}

void CubeLodVolumes::addToPickList(vtkPicker *picker) const
{
    if (!picker)
        return;
    for (const Entry &e : m_entries)
        if (e.volume->GetVisibility())
            picker->AddPickList(e.volume);
}

void CubeLodVolumes::apply(Entry &e) const
{
    e.mapper->SetBlendMode(m_blendMode);
    e.mapper->SetUseJittering(m_jitter ? 1 : 0);
    // Fixed distances, not the renderer's time-driven ones: the renderer
    // splits a frame's time budget between its props, so up to 22 blocks
    // would each get a sliver of it and draw at the coarsest image sample
    // distance while the camera moves. Half a sample of the block along the
    // ray, as the mapper's own default for one volume; the opacity is
    // corrected per unit distance (the shared property), so blocks of
    // different spacings still composite to the same colours.
    e.mapper->SetAutoAdjustSampleDistances(0);
    e.mapper->SetSampleDistance(float(e.geometry.placement.spacing / 2.0));
    e.mapper->SetImageSampleDistance(1.0f);
    const std::optional<CubeLodAssembly::Crop> drawn = CubeLodAssembly::clipped(e.geometry, m_userCrop);
    if (drawn) {
        const CubeLodAssembly::Crop planes = CubeLodAssembly::cropPlanes(e.geometry, *drawn);
        double current[6];
        e.mapper->GetCroppingRegionPlanes(current);
        if (!std::equal(planes.begin(), planes.end(), current))
            e.mapper->SetCroppingRegionPlanes(planes.data());
    }
    e.volume->SetVisibility(m_visible && drawn ? 1 : 0);
}

CubeLodPick::Result CubeLodVolumes::pickAlong(const double p0[3], const double p1[3],
                                              RayVoxelPick::Criterion criterion, double threshold,
                                              const RayVoxelPick::BlankTest &isBlank) const
{
    std::vector<CubeLodPick::Piece> pieces;
    pieces.reserve(m_entries.size());
    for (const Entry &e : m_entries) {
        if (!e.volume || !e.volume->GetVisibility())
            continue;
        const std::optional<CubeLodAssembly::Crop> drawn = CubeLodAssembly::clipped(e.geometry, m_userCrop);
        auto *values = vtkFloatArray::SafeDownCast(e.image->GetPointData()->GetScalars());
        if (!drawn || !values)
            continue;
        CubeLodPick::Piece p;
        const CubeLodAssembly::Dims d = e.geometry.dims;
        for (int a = 0; a < 3; ++a) {
            p.grid.extent[2 * a] = 0;
            p.grid.extent[2 * a + 1] = d[size_t(a)] - 1;
            p.grid.origin[a] = e.geometry.placement.origin[size_t(a)];
            p.grid.spacing[a] = e.geometry.placement.spacing;
        }
        std::copy(drawn->begin(), drawn->end(), p.owned.begin());
        const float *v = values->GetPointer(0);
        const vtkIdType nx = d[0], nxy = vtkIdType(d[0]) * d[1];
        p.sample = [v, nx, nxy](int x, int y, int z) { return double(v[x + nx * y + nxy * z]); };
        pieces.push_back(std::move(p));
    }
    return CubeLodPick::along(pieces, p0, p1, criterion, threshold, isBlank);
}

void CubeLodVolumes::sortForCamera()
{
    if (!m_renderer || m_entries.size() < 2 || !m_orderFunction)
        return;
    vtkCamera *camera = m_renderer->GetActiveCamera();
    if (!camera)
        return;
    double position[3];
    camera->GetPosition(position);
    std::array<double, 3> eye{ position[0], position[1], position[2] };
    if (camera->GetParallelProjection()) {
        // Every ray runs along the projection direction: the order is that of
        // an eye far back along it, whatever the camera's distance.
        double dop[3];
        camera->GetDirectionOfProjection(dop);
        for (int i = 0; i < 3; ++i)
            eye[i] = position[i] - dop[i] * 1e7;
    }
    this->drawInOrder(m_orderFunction(eye));
}

void CubeLodVolumes::drawInOrder(const std::vector<int> &order)
{
    if (!m_renderer || order == m_order)
        return;
    // The prop collection is reordered in place: removing a volume from the
    // renderer would release its textures and upload it again every frame.
    vtkPropCollection *props = m_renderer->GetViewProps();
    for (int index : order) {
        if (index < 0 || index >= int(m_entries.size()))
            continue;
        vtkVolume *v = m_entries[size_t(index)].volume;
        props->RemoveItem(v);
        props->AddItem(v);
    }
    m_order = order;
}

void CubeLodVolumes::onRendererStart(vtkObject * /*caller*/, unsigned long /*eventId*/, void *clientData,
                                     void * /*callData*/)
{
    if (auto *self = static_cast<CubeLodVolumes *>(clientData))
        self->sortForCamera();
}
