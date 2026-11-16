#ifndef RegionListOverlay_h
#define RegionListOverlay_h

// RegionListOverlay.h
// The actors that draw a window's region list (issue #341) in one renderer.
//
// One overlay per place a list is shown: the image viewer's view, the cube
// viewer's 2-D slice and its moment window, and the cube's 3-D cutting plane
// (#343). Each draws the same RegionList: the unselected outlines in their own
// colours, the selected one thicker with a light fill, point regions as
// markers, and the selected region's handles. Positions are the list's
// full-resolution 0-based pixels, mapped to the renderer's world by `toWorld`
// (the identity on a 2-D view whose world is those pixels).

#include "RegionModel.h"

#include <vtkSmartPointer.h>

#include <array>
#include <functional>

class vtkActor;
class vtkContourTriangulator;
class vtkPolyData;
class vtkRenderer;

class RegionListOverlay
{
public:
    struct Style
    {
        double lineWidth{ 2.0 };
        double selectedLineWidth{ 3.5 };
        double pointSize{ 11.0 };
        double handleSize{ 9.0 };
        /// The selected region's light fill.
        bool fill{ true };
        /// Pull lines and markers this many depth units towards the camera, so
        /// a plane they lie on (the 3-D cutting plane) never hides them. Zero on
        /// a 2-D view, where nothing else shares their depth.
        double lineOffsetUnits{ 0.0 };
    };

    /// World position of a region-list pixel.
    using ToWorld = std::function<std::array<double, 3>(const visivo::regions::Vec2 &)>;

    /// The cyan the drawing tools have always used, for a region with no colour.
    static std::array<unsigned char, 3> colour(const visivo::regions::Region &region);

    RegionListOverlay();
    ~RegionListOverlay();
    RegionListOverlay(const RegionListOverlay &) = delete;
    RegionListOverlay &operator=(const RegionListOverlay &) = delete;

    /// Add the actors to @p renderer (once).
    void attach(vtkRenderer *renderer, const Style &style);
    void attach(vtkRenderer *renderer) { attach(renderer, Style()); }
    /// Remove them from the renderer they were added to.
    void detach();
    bool attached() const { return m_renderer != nullptr; }

    /// Draw @p list. @p knobOffset is how far the rotation knob stands off a
    /// box or an ellipse, in list pixels; @p withHandles false draws the
    /// selection without its handles (a view the region cannot be edited in).
    /// An empty @p toWorld places pixel (x, y) at world (x, y, 0).
    void update(const visivo::regions::RegionList &list, double knobOffset, const ToWorld &toWorld,
                bool withHandles = true);
    void update(const visivo::regions::RegionList &list, double knobOffset)
    {
        update(list, knobOffset, ToWorld());
    }

    /// Show or hide every actor at once (the list is kept).
    void setVisible(bool visible);
    bool visible() const { return m_visible; }

private:
    vtkRenderer *m_renderer{ nullptr };
    Style m_style;
    bool m_visible{ true };
    bool m_haveFill{ false };
    vtkSmartPointer<vtkPolyData> m_listData;        ///< outlines of the unselected regions
    vtkSmartPointer<vtkActor> m_listActor;
    vtkSmartPointer<vtkPolyData> m_selectedData;    ///< outline of the selected one
    vtkSmartPointer<vtkActor> m_selectedActor;
    vtkSmartPointer<vtkPolyData> m_fillData;
    vtkSmartPointer<vtkContourTriangulator> m_fillTriangulator;
    vtkSmartPointer<vtkActor> m_fillActor;
    vtkSmartPointer<vtkPolyData> m_pointData;       ///< point regions
    vtkSmartPointer<vtkActor> m_pointActor;
    vtkSmartPointer<vtkPolyData> m_handleData;      ///< the selected region's handles
    vtkSmartPointer<vtkActor> m_handleActor;
};

#endif   // RegionListOverlay_h
