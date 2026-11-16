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

bool CubeLodVolumes::replace(vtkRenderer *renderer, std::vector<Block> blocks, OrderFunction order)
{
    this->clear();
    if (!renderer || blocks.empty())
        return false;
    // The order function indexes the blocks as given: a block that cannot be
    // drawn makes the whole set unusable, rather than shifting the indices.
    for (const Block &b : blocks) {
        const CubeLodAssembly::Dims &d = b.geometry.dims;
        if (d[0] < 1 || d[1] < 1 || d[2] < 1
            || b.samples.size() != size_t(d[0]) * size_t(d[1]) * size_t(d[2]))
            return false;
    }
    m_renderer = renderer;
    m_orderFunction = std::move(order);
    // One clearance for the whole set, so both blocks of a face move it to
    // the same place: at most a tenth of the finest block's spacing, which
    // keeps every moved face well inside the ghost sample beyond it (half a
    // spacing) whatever the preview's stride.
    m_clearance = CubeLodAssembly::kEyeFaceClearance;
    for (const Block &b : blocks)
        m_clearance = std::min(m_clearance, b.geometry.placement.spacing / 10.0);
    m_entries.reserve(blocks.size());
    for (Block &b : blocks) {
        const CubeLodAssembly::Dims &d = b.geometry.dims;
        const vtkIdType count = vtkIdType(d[0]) * d[1] * d[2];
        Entry e;
        e.geometry = b.geometry;
        e.image = vtkSmartPointer<vtkImageData>::New();
        e.image->SetDimensions(d[0], d[1], d[2]);
        const double s = b.geometry.placement.spacing;
        e.image->SetSpacing(s, s, s);
        e.image->SetOrigin(b.geometry.placement.origin.data());
        // The block's own copy: the caller's vector goes when this returns.
        auto scalars = vtkSmartPointer<vtkFloatArray>::New();
        scalars->SetName("intensity");
        scalars->SetNumberOfComponents(1);
        scalars->SetNumberOfTuples(count);
        std::memcpy(scalars->GetPointer(0), b.samples.data(), size_t(count) * sizeof(float));
        std::vector<float>().swap(b.samples);
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
        this->apply(e);
        renderer->AddVolume(e.volume);
        m_entries.push_back(std::move(e));
    }
    // Re-sorted before every frame of this renderer: rotating the camera
    // changes which block is in front of which.
    m_startObserver = vtkSmartPointer<vtkCallbackCommand>::New();
    m_startObserver->SetCallback(&CubeLodVolumes::onRendererStart);
    m_startObserver->SetClientData(this);
    m_startTag = renderer->AddObserver(vtkCommand::StartEvent, m_startObserver);
    this->sortForCamera();
    return true;
}

void CubeLodVolumes::clear()
{
    if (m_renderer) {
        if (m_startTag)
            m_renderer->RemoveObserver(m_startTag);
        // RemoveVolume releases each block's GPU textures.
        for (Entry &e : m_entries)
            m_renderer->RemoveVolume(e.volume);
    }
    // Detached explicitly, so the samples go with the block rather than
    // whenever the last reference to the array does.
    for (Entry &e : m_entries)
        if (e.image)
            e.image->GetPointData()->SetScalars(nullptr);
    m_entries.clear();
    m_order.clear();
    m_orderFunction = nullptr;
    m_eye.reset();
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
        // Faces off the eye, or the rays running inside a shared face would
        // be drawn by neither block (CubeLodAssembly::clearOfEye).
        const CubeLodAssembly::Crop planes = CubeLodAssembly::cropPlanes(
                e.geometry, m_eye ? CubeLodAssembly::clearOfEye(*drawn, *m_eye, m_clearance) : *drawn);
        double current[6];
        e.mapper->GetCroppingRegionPlanes(current);
        if (!std::equal(planes.begin(), planes.end(), current))
            e.mapper->SetCroppingRegionPlanes(planes.data());
    }
    e.volume->SetVisibility(m_visible && drawn ? 1 : 0);
}

void CubeLodVolumes::sortForCamera()
{
    if (!m_renderer || m_entries.empty())
        return;
    vtkCamera *camera = m_renderer->GetActiveCamera();
    if (!camera)
        return;
    double position[3];
    camera->GetPosition(position);
    std::array<double, 3> eye{ position[0], position[1], position[2] };
    // The crops follow the eye only where a face comes within reach of it;
    // apply() sets the planes only when they change, so this is cheap.
    if (m_eye != eye) {
        m_eye = eye;
        for (Entry &e : m_entries)
            this->apply(e);
    }
    if (m_entries.size() < 2 || !m_orderFunction)
        return;
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
