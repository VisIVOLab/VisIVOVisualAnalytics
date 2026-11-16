#ifndef CUBE_LOD_VOLUMES_H
#define CUBE_LOD_VOLUMES_H
// CubeLodVolumes.h
// The volumes of a refined 3-D view of a large cube (issue #321): a box of
// finer blocks and the coarse pieces of the preview around it, drawn in place
// of the window's single preview volume. A C++ port of the backend's
// out-of-core renderer (backend/app/render_session/_ooc.py BlockVolumes):
//
//   - every block shares the window's vtkVolumeProperty, the same object, so
//     the colour map, threshold, scale and opacity are identical everywhere
//     and follow every edit made to them;
//   - every block is cropped to the region it owns, with the planes of
//     CubeLodAssembly::cropPlanes, so neighbours meet without a gap or a
//     double-drawn seam; a face the camera sits on is moved a hair off it
//     before each frame (CubeLodAssembly::clearOfEye), or the rays running
//     inside it would be drawn by neither neighbour;
//   - the blocks are drawn back to front, re-sorted before every frame by
//     reordering the renderer's props (RemoveVolume would release the GPU
//     textures and upload every block again each frame);
//   - the blend mode and jittering of the window's mapper are copied to
//     every block; each block samples at half its own spacing.
//
// Each block holds its samples in a vtkFloatArray: its own copy of the
// samples it was given, or an array shared with the window's cache of blocks
// already read. replace() keeps the volumes of blocks present in both sets
// (same key, same place), so a new set uploads only what is new to the GPU.

#include "CubeLodAssembly.h"
#include "CubeLodPick.h"

#include <vtkFloatArray.h>
#include <vtkSmartPointer.h>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class vtkCallbackCommand;
class vtkObject;
class vtkGPUVolumeRayCastMapper;
class vtkImageData;
class vtkPicker;
class vtkRenderer;
class vtkVolume;
class vtkVolumeProperty;

class CubeLodVolumes
{
public:
    /// One volume to draw: its samples (x fastest, ghosts included), where
    /// they sit in the window's world, and the world box it owns.
    struct Block
    {
        /// The samples, copied into the block; ignored when @p array is set.
        std::vector<float> samples;
        /// The samples as an array the block shares (not copied).
        vtkSmartPointer<vtkFloatArray> array;
        CubeLodAssembly::Geometry geometry;
        /// What the block is (a node of a level, a piece of the preview): a
        /// block of the shown set with the same key and geometry is kept as it
        /// is, and then needs neither samples nor array.
        std::string key;
    };

    /// Back-to-front order of the blocks (indices into what replace() got)
    /// for an eye at @p eyeWorld, the camera's position in the window's world.
    using OrderFunction = std::function<std::vector<int>(const std::array<double, 3> &eyeWorld)>;

    /// @p property is the window's volume property, shared by every block.
    explicit CubeLodVolumes(vtkVolumeProperty *property);
    ~CubeLodVolumes();
    CubeLodVolumes(const CubeLodVolumes &) = delete;
    CubeLodVolumes &operator=(const CubeLodVolumes &) = delete;

    /// Draw exactly @p blocks in @p renderer, in the order @p order gives.
    /// Blocks already shown under the same key and geometry stay on the GPU
    /// as they are; the rest of what was shown is removed (its GPU textures
    /// released). False, with what was shown left unchanged, when a block's
    /// samples do not fill its dims or a block without samples is not shown.
    bool replace(vtkRenderer *renderer, std::vector<Block> blocks, OrderFunction order);
    /// True when a block of @p key and @p geometry is shown: replace() can
    /// keep it, so its samples need not be built again.
    bool holds(const std::string &key, const CubeLodAssembly::Geometry &geometry) const;
    /// Remove every block from the renderer and release them.
    void clear();
    /// True while blocks are shown in a renderer.
    bool active() const { return m_renderer != nullptr && !m_entries.empty(); }
    int blockCount() const { return int(m_entries.size()); }
    /// GPU bytes of the blocks' samples (float32).
    long long bytes() const;

    /// The blend mode and jittering of @p mapper, applied to every block now
    /// and to the blocks of later replace() calls. Sample distances are the
    /// blocks' own (apply()).
    void copySettingsFrom(vtkGPUVolumeRayCastMapper *mapper);
    void setVisible(bool visible);
    bool visible() const { return m_visible; }
    /// An extra clip box in world units (x0, x1, y0, y1, z0, z1), as the
    /// headset's clip; nullopt for none. A block wholly outside it is hidden.
    void setUserCrop(const std::optional<CubeLodAssembly::Crop> &crop);
    /// Make every block pickable by @p picker (PickFromList).
    void addToPickList(vtkPicker *picker) const;
    /// Re-sort the blocks for the renderer's current camera, and move the
    /// faces the camera sits on off it.
    void sortForCamera();
    /// The view ray p0 → p1 (world) walked through the visible blocks, each
    /// on its own grid within the box it draws (CubeLodPick::along).
    CubeLodPick::Result pickAlong(const double p0[3], const double p1[3],
                                  RayVoxelPick::Criterion criterion, double threshold,
                                  const RayVoxelPick::BlankTest &isBlank) const;

private:
    struct Entry
    {
        vtkSmartPointer<vtkImageData> image;
        vtkSmartPointer<vtkGPUVolumeRayCastMapper> mapper;
        vtkSmartPointer<vtkVolume> volume;
        CubeLodAssembly::Geometry geometry;
        std::string key;
    };
    void apply(Entry &e) const;
    void release(Entry &e);
    void drawInOrder(const std::vector<int> &order);
    static void onRendererStart(vtkObject *caller, unsigned long eventId, void *clientData,
                                void *callData);

    vtkSmartPointer<vtkVolumeProperty> m_property;
    vtkRenderer *m_renderer{ nullptr };
    vtkSmartPointer<vtkCallbackCommand> m_startObserver;
    unsigned long m_startTag{ 0 };
    std::vector<Entry> m_entries;
    std::vector<int> m_order;   ///< the order last drawn
    OrderFunction m_orderFunction;
    bool m_visible{ true };
    std::optional<CubeLodAssembly::Crop> m_userCrop;
    /// The camera's position at the last frame (world): faces near it are
    /// moved off it (CubeLodAssembly::clearOfEye).
    std::optional<std::array<double, 3>> m_eye;
    /// How far a face is moved off the eye, for the whole set (replace()).
    double m_clearance{ CubeLodAssembly::kEyeFaceClearance };
    // The window's mapper settings (copySettingsFrom).
    int m_blendMode{ 0 };
    bool m_jitter{ true };
};

#endif   // CUBE_LOD_VOLUMES_H
