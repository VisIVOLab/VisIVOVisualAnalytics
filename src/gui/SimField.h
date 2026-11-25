#pragma once
// SimField.h
// One more field of the simulation a cube window shows (issue #525): another
// variable of the same run, on the same grid, drawn with the window's own
// field in the slice and in the 3-D box. The window keeps its first field's
// look where it always did (the slice's lookup table, the volume's transfer
// functions); a SimField carries the same things for each field added.
//
// The arithmetic of combining the fields is in FieldOverlay.h; the window's
// side is vtkWindowCube_Fields.cpp.

#include "LutScale.h"
#include "app/PhysicalUnits.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <vtkSmartPointer.h>

#include <array>
#include <cstdint>
#include <optional>

class VolumeColorbar;
class vtkColorTransferFunction;
class vtkGPUVolumeRayCastMapper;
class vtkImageData;
class vtkImageSlice;
class vtkImageStack;
class vtkLookupTable;
class vtkMultiVolume;
class vtkPiecewiseFunction;
class vtkTrivialProducer;
class vtkVolume;
class vtkVolumeProperty;

struct SimField
{
    SimField();
    ~SimField();
    SimField(const SimField &) = delete;
    SimField &operator=(const SimField &) = delete;

    QString name;
    QString path;          ///< the field's reference (what opens it)
    QString datasetId;     ///< on the backend; empty until opened
    bool opening{ false }; ///< a fetch (open, volume) is out
    /// Names the reads in flight for this field: a result that comes back with
    /// another id (the field was removed, added again under the same name, or
    /// the units changed since it was asked) is dropped. New at every one of those.
    std::uint64_t readId;
    void renewReadId();
    visivo::units::PhysicalUnits units;   ///< the field's own, from its open
    bool scaleChosen{ false };            ///< the default scale was picked (or the user chose)

    // What the user sets, shared by the 2-D and the 3-D view.
    bool shown{ true };
    double opacity{ 1.0 };
    QString colourMap;
    LutScale::Mode scale{ LutScale::Mode::Linear };   ///< Linear or Log
    std::optional<double> lo, hi;      ///< typed Min and Max; none: the data's
    std::optional<double> threshold;   ///< 3-D: where the ramp starts; none: 95th percentile

    // 2-D: the slices fetched at the displayed level, and the table.
    vtkSmartPointer<vtkLookupTable> lut;
    QHash<QString, vtkSmartPointer<vtkImageData>> slices;   ///< "axis:index"
    QStringList sliceOrder;                                 ///< oldest first
    QSet<QString> sliceInFlight;                            ///< keys being fetched
    std::array<double, 2> sliceRange{ 0.0, 0.0 };           ///< the range in force
    bool sliceRangeKnown{ false };

    // 3-D: the field's voxels on the displayed cube's grid, and what draws them.
    vtkSmartPointer<vtkImageData> volumeImage;
    vtkSmartPointer<vtkTrivialProducer> source;
    vtkSmartPointer<vtkColorTransferFunction> colours;
    vtkSmartPointer<vtkPiecewiseFunction> ramp;
    vtkSmartPointer<vtkVolumeProperty> property;
    vtkSmartPointer<vtkVolume> volume;
    std::array<double, 2> dataRange{ 0.0, 0.0 };   ///< visible voxels' min..max
    double sentinel{ -1.0 };                       ///< what blank voxels were set to
    double defaultThreshold{ 0.0 };
    bool volumeError{ false };                     ///< the last volume fetch failed

    QPointer<VolumeColorbar> bar3d;
    QPointer<VolumeColorbar> bar2d;
};

/// What the window draws the fields with: one ray cast for the 3-D box and one
/// more image on the slice. Kept apart from the window's own pipelines, which
/// are untouched while no field is added.
struct SimFieldViews
{
    SimFieldViews();
    ~SimFieldViews();
    SimFieldViews(const SimFieldViews &) = delete;
    SimFieldViews &operator=(const SimFieldViews &) = delete;

    // 3-D: the window's own volume stands in the cast as port 0.
    vtkSmartPointer<vtkMultiVolume> multi;
    vtkSmartPointer<vtkGPUVolumeRayCastMapper> mapper;
    vtkSmartPointer<vtkVolume> mainProxy;
    bool multiInRenderer{ false };

    // 2-D: the composite, an RGBA picture on the slice's frame, drawn on top of
    // the window's own slice (which it hides while it is on).
    vtkSmartPointer<vtkImageSlice> base;        ///< the window's slice actor
    vtkSmartPointer<vtkImageSlice> composite;
    vtkSmartPointer<vtkImageData> compositeImage;
    vtkSmartPointer<vtkImageStack> stack;       ///< the slice's stack of images
    bool compositeShown{ false };
    bool updatePending{ false };   ///< a redraw of the picture is queued (the table changed)
    /// The 3-D cast was left out because the window's blend mode or the
    /// headset's clip is something it cannot draw: the plain volume shows.
    bool castSuppressed{ false };
    QPointer<VolumeColorbar> mainBar2d;   ///< the window's own field's bar on the combined slice
};
