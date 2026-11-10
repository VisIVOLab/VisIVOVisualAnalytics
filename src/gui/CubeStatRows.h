#pragma once

// What the Inspector's "3D cube" Min / Max / Mean / RMS rows say (#150).
//
// The rows describe the whole cube, and only the whole cube. The backend's
// full-resolution scan (GET /v1/datasets/{id}/stats) is the reference once it
// has landed, so a later preview refresh or a sub-cube load does not
// overwrite it. Before it lands the rows show what the loaded grid measured,
// when that grid is the whole cube: the strided preview of a cube above the
// full-resolution threshold measures no mean or RMS, and a camera-ROI load is
// only part of the cube, so those rows read "-" with a tooltip saying why,
// where they used to show the preview path's 0 placeholders as if measured.
// Widget-free so that it is unit-tested.

#include <QString>

#include <array>
#include <functional>
#include <optional>

namespace visivo {

/// What the grid on screen measured itself.
struct CubeGridStats
{
    std::array<double, 2> range{ 0., 0. };            ///< visible min / max
    std::optional<std::array<double, 2>> meanRms;     ///< empty for the strided preview
    bool wholeCube{ true };   ///< false for a full-resolution load of a camera ROI
};

struct CubeStatTexts
{
    QString min, max, mean, rms;
    QString toolTip;   ///< why some rows read "-"; empty when all are shown
};

/// @p full is the backend's {min, max, mean, rms}, when it answered; @p grid
/// the loaded grid's numbers (empty before any grid is shown);
/// @p backendFailed true when the backend answered without statistics.
/// @p fmt formats one value with its unit.
inline CubeStatTexts cubeStatTexts(const std::optional<std::array<double, 4>> &full,
                                   const std::optional<CubeGridStats> &grid, bool backendFailed,
                                   const std::function<QString(double)> &fmt)
{
    CubeStatTexts t;
    if (full) {
        t.min = fmt((*full)[0]);
        t.max = fmt((*full)[1]);
        t.mean = fmt((*full)[2]);
        t.rms = fmt((*full)[3]);
        return t;
    }
    const QString none = QStringLiteral("-");
    t.min = t.max = t.mean = t.rms = none;
    const QString why = backendFailed
            ? QStringLiteral("The backend could not compute the full-cube statistics.")
            : QStringLiteral("Computing the full-cube statistics…");
    if (!grid || !grid->wholeCube) {
        // Nothing loaded yet, or only a region of the cube: nothing of it is
        // the whole cube's.
        t.toolTip = grid ? why + QStringLiteral(" The loaded region is only part of the cube.")
                         : why;
        return t;
    }
    t.min = fmt(grid->range[0]);
    t.max = fmt(grid->range[1]);
    if (grid->meanRms) {
        t.mean = fmt((*grid->meanRms)[0]);
        t.rms = fmt((*grid->meanRms)[1]);
        return t;
    }
    t.toolTip = backendFailed
            ? why + QStringLiteral(" Load full resolution (without a camera ROI) to measure "
                                   "Mean and RMS.")
            : why;
    return t;
}

}   // namespace visivo
