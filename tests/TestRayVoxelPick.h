#ifndef TestRayVoxelPick_h
#define TestRayVoxelPick_h

#include "vtk/RayVoxelPick.h"

#include <QtTest>

#include <cmath>
#include <vector>

/// The geometry behind the cube viewer's 3-D spectral picker.
///
/// The claim under test is narrow and checkable: the voxel returned is the one
/// the renderer would have shown, given the blend mode the user is looking
/// through. Everything here is the pure walk — no VTK, no render window.
class TestRayVoxelPick : public QObject
{
    Q_OBJECT

private:
    // A 20x20x40 cube of zeros. z is the spectral axis, as in a real cube.
    static constexpr int kNx = 20, kNy = 20, kNz = 40;
    std::vector<double> m_data;

    RayVoxelPick::Grid grid() const
    {
        RayVoxelPick::Grid g;
        g.extent[0] = 0; g.extent[1] = kNx - 1;
        g.extent[2] = 0; g.extent[3] = kNy - 1;
        g.extent[4] = 0; g.extent[5] = kNz - 1;
        g.origin[0] = g.origin[1] = g.origin[2] = 0.;
        g.spacing[0] = g.spacing[1] = g.spacing[2] = 1.;
        return g;
    }
    RayVoxelPick::Sampler sampler()
    {
        return [this](int x, int y, int z) {
            if (x < 0 || x >= kNx || y < 0 || y >= kNy || z < 0 || z >= kNz)
                return std::nan("");
            return m_data[static_cast<std::size_t>((z * kNy + y) * kNx + x)];
        };
    }
    void set(int x, int y, int z, double v)
    {
        m_data[static_cast<std::size_t>((z * kNy + y) * kNx + x)] = v;
    }

private slots:
    void init();



    /// MIP shows the brightest sample along the ray, so a click must return it —
    /// not the nearest voxel, and not the one on the displayed slice.
    void maximum_returnsTheBrightestVoxelAlongTheRay();



    /// The whole point of the feature: the peak may lie in a channel far from
    /// the one on the cutting plane. Picking must not silently answer with the
    /// displayed channel.
    void maximum_findsAPeakThatIsNotOnTheDisplayedSlice();

    void minimum_returnsTheFaintestVoxel();



    /// Composite shows the FRONT surface. A brighter voxel hidden behind it is
    /// not what the user clicked, so it must not win.
    void composite_stopsAtTheFirstVoxelAboveThreshold();



    /// Reverse the ray and the front surface is the other one — the criterion is
    /// about the viewer's side, not about z increasing.
    void composite_frontSurfaceFollowsTheRayDirection();

    void composite_missesWhenNothingReachesTheThreshold();



    /// Side-on: the ray crosses SKY positions at roughly fixed velocity. The
    /// answer is still the brightest sample, but now it identifies a position,
    /// which is why the caller reports the channel back to the user.
    void maximum_worksOnASideOnRay();



    /// A ray that never enters the cube must report a miss, not the nearest voxel.
    void missesTheCubeEntirely();



    /// NaN blanks are skipped, not treated as a maximum or as zero.
    void ignoresNonFiniteSamples();



    /// A one-voxel spike must not be stepped over — that is what fixes the
    /// sampling rate at two steps per voxel.
    void doesNotStepOverASingleVoxelSpike();



    /// The returned world position must map back to the voxel that was picked,
    /// or the marker drawn in the 3-D view lands somewhere else.
    void worldPositionMatchesThePickedVoxel();

    void rejectsADegenerateGrid();

/// The guarantee a fixed-step march could not give: every voxel the ray crosses
/// is visited. A capped half-voxel march skipped 1807 channels on a cube this
/// long, which is why the walk is a DDA.
    void visitsEveryVoxelOnALongAxis();

/// VTK's convention is world = origin + index*spacing with index ABSOLUTE, so a
/// grid whose extent does not start at zero must not be shifted by extent[0].
    void handlesAnExtentThatDoesNotStartAtZero();

/// Blanks reach this code as a finite sentinel below the data minimum, not as
/// NaN, so MinIP would answer with a blank on every ray that crossed one.
    void blankTestExcludesTheFiniteSentinel();

/// An oblique ray must still visit a contiguous chain of voxels, with no gaps.
    void obliqueRayVisitsAContiguousChain();
};

#endif   // TestRayVoxelPick_h
