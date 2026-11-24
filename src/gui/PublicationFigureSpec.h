#pragma once

// PublicationFigureSpec.h
// What the Publication Figure dialog asks of the backend (issue #41), as pure
// logic with no widget and no VTK so that it can be unit-tested: the options and
// their defaults (the one-click figure the tool always made), the request body of
// POST /v1/products/publication_figure, the output formats, the colour-map names
// the backend can set, and the window's overlays (contour lines, regions, text and
// arrow annotations) in the canonical region form the backend draws
// (backend/app/figure_overlays.py), so that nothing is drawn twice by two rules.

#include "ContourSettings.h"
#include "RegionModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace visivo::figure {

/// The file formats of a figure; the backend writes them with matplotlib (the 3-D
/// Save View As, renderexport::Format, draws its own with a Qt painter).
struct FormatInfo
{
    const char *key;        ///< what the backend reads from the extension
    const char *label;
};
inline const std::vector<FormatInfo> &formats()
{
    static const std::vector<FormatInfo> f{ { "png", "PNG image" },
                                            { "pdf", "PDF document" },
                                            { "svg", "SVG image" },
                                            { "eps", "Encapsulated PostScript" } };
    return f;
}

/// The names the backend resolves through matplotlib, in the spelling of the
/// viewer's colour-map registry (src/vtk/ColorMaps.cpp): those of the registry
/// that matplotlib also has, the white-to-hue maps last because they print well.
inline const QStringList &colourMaps()
{
    static const QStringList m{ "Inferno", "Viridis", "Magma", "Plasma", "Cividis", "CoolWarm",
                                "Gray", "Purples", "Blues", "Greens", "Reds", "Greys", "Oranges",
                                "Purples_r", "Blues_r", "Greens_r", "Reds_r", "Greys_r",
                                "Oranges_r", "hot", "cubehelix" };
    return m;
}

/// The figure's colour map for the window's: the registry name when the backend
/// has it, else Inferno (the viewer's own maps, such as Rainbow or Glow, have no
/// matplotlib counterpart).
inline QString colourMapFor(const QString &windowMap)
{
    for (const QString &m : colourMaps())
        if (m.compare(windowMap, Qt::CaseInsensitive) == 0)
            return m;
    // The stand-ins the volume renderer's table (render_session/_common.py) has.
    if (windowMap.compare(QStringLiteral("Glow"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("hot");
    return QStringLiteral("Inferno");
}

/// Everything the dialog edits. The defaults are the one-click figure's.
struct Options
{
    // Image
    QString cmap{ QStringLiteral("Inferno") };
    QString stretch{ QStringLiteral("linear") };   ///< linear sqrt squared log asinh sinh power
    double gamma{ 1.0 };
    double percent{ 99.5 };
    bool useRange{ false };                        ///< vmin/vmax below, as the window shows
    double vmin{ 0.0 };
    double vmax{ 1.0 };
    // Text
    QString title;
    QString xlabel;
    QString ylabel;
    QString fontFamily{ QStringLiteral("sans-serif") };
    double titleSize{ 12.0 };
    double labelSize{ 10.0 };
    double tickSize{ 10.0 };
    double colorbarLabelSize{ 10.0 };
    // Axes and colour bar
    bool grid{ true };
    QString frame{ QStringLiteral("native") };     ///< native fk5 icrs galactic
    QString tickFormat{ QStringLiteral("auto") };  ///< auto sexagesimal degrees
    bool colorbar{ true };
    QString colorbarPosition{ QStringLiteral("right") };
    QString colorbarLabel;
    // Marks
    bool beam{ false };
    QString beamPosition{ QStringLiteral("bottom-left") };
    bool scaleBar{ false };
    double scaleBarLength{ 0.0 };                  ///< 0 = a round length
    QString scaleBarUnit{ QStringLiteral("arcsec") };
    QString scaleBarPosition{ QStringLiteral("bottom-right") };
    bool compass{ false };
    QString compassPosition{ QStringLiteral("top-right") };
    // What the window shows over the image
    bool contours{ false };
    bool regions{ false };
    bool annotations{ false };
    // Output
    double widthIn{ 8.0 };
    double heightIn{ 6.5 };
    int dpi{ 200 };
    bool crop{ true };
    QString format{ QStringLiteral("png") };
};

/// One text or arrow annotation of the image window (its world is the image's
/// 0-based pixels, as the regions'). The label is at (x, y), an arrow's tip at
/// (tipX, tipY).
struct Note
{
    bool arrow{ false };
    QString text;
    double x{ 0 }, y{ 0 };
    double tipX{ 0 }, tipY{ 0 };
};

/// The annotations as canonical image-frame regions (1-based pixels): an arrow a
/// `vector` from the label to the tip, the text a `text` at the label.
inline QJsonArray noteRegions(const std::vector<Note> &notes)
{
    QJsonArray out;
    for (const Note &n : notes) {
        if (n.arrow) {
            const double dx = n.tipX - n.x, dy = n.tipY - n.y;
            const double len = std::hypot(dx, dy);
            if (len > 0.0 && std::isfinite(len))
                out.append(QJsonObject{
                        { "shape", "vector" }, { "frame", "image" }, { "unit", "pix" },
                        { "center", QJsonArray{ n.x + 1.0, n.y + 1.0 } },
                        { "length", len },
                        { "angle_deg", std::atan2(dy, dx) * 180.0 / M_PI } });
        }
        if (!n.text.isEmpty())
            out.append(QJsonObject{ { "shape", "text" }, { "frame", "image" }, { "unit", "pix" },
                                    { "center", QJsonArray{ n.x + 1.0, n.y + 1.0 } },
                                    { "text", n.text } });
    }
    return out;
}

/// The visible regions of the window as canonical image-frame regions (1-based).
inline QJsonArray regionRegions(const std::vector<visivo::regions::Region> &regions)
{
    QJsonArray out;
    for (const auto &r : regions)
        if (r.visible)
            out.append(visivo::regions::toCanonical(r, 1.0));
    return out;
}

/// The contour lines of one or more layers (the image's own and the external
/// ones, already aligned to the image's pixels) as canonical lines. Joining the
/// segments is linear in their number but not free: call it off the GUI thread.
inline QJsonArray contourRegions(const std::vector<visivo::contours::LineSource> &sources)
{
    QJsonArray out;
    for (const auto &src : sources) {
        const auto lines = visivo::contours::joinLines(src);
        const QJsonArray part = visivo::contours::canonicalRegions(lines, src.lineWidth, src.sigma);
        for (const QJsonValue &v : part)
            out.append(v);
    }
    return out;
}

/// What a request body may draw over the image. The contour lines are made on
/// first use and kept, so a preview after each edit does not join them again.
class Overlays
{
public:
    std::vector<visivo::contours::LineSource> contourSources;
    QJsonArray regions;
    QJsonArray annotations;

    QJsonArray contours() const
    {
        std::call_once(m_once, [this] { m_contours = contourRegions(contourSources); });
        return m_contours;
    }
    bool hasContours() const { return !contourSources.empty(); }

private:
    mutable std::once_flag m_once;
    mutable QJsonArray m_contours;
};

/// The output name for @p stem in @p format ("field" + "pdf" = "field.pdf").
inline QString outputName(const QString &stem, const QString &format)
{
    const QString ext = format.isEmpty() ? QStringLiteral("png") : format;
    return (stem.isEmpty() ? QStringLiteral("figure") : stem) + QLatin1Char('.') + ext;
}

/// The request body. Every field is sent, so that the body documents the figure it
/// asks for; the backend's defaults are the options' defaults.
inline QJsonObject requestBody(const QString &datasetId, const Options &o, const Overlays &ov,
                               bool preview, const QString &outputStem = QString(),
                               int previewPx = 800)
{
    QJsonObject b{
        { "dataset_id", datasetId },
        { "cmap", o.cmap }, { "stretch", o.stretch }, { "gamma", o.gamma },
        { "percent", o.percent },
        { "title", o.title }, { "xlabel", o.xlabel }, { "ylabel", o.ylabel },
        { "add_grid", o.grid }, { "frame", o.frame }, { "tick_format", o.tickFormat },
        { "add_colorbar", o.colorbar }, { "colorbar_position", o.colorbarPosition },
        { "colorbar_label", o.colorbarLabel },
        { "font_family", o.fontFamily }, { "title_size", o.titleSize },
        { "label_size", o.labelSize }, { "tick_size", o.tickSize },
        { "colorbar_label_size", o.colorbarLabelSize },
        { "width_in", o.widthIn }, { "height_in", o.heightIn }, { "dpi", o.dpi },
        { "crop", o.crop },
        { "beam", o.beam }, { "beam_position", o.beamPosition },
        { "scale_bar", o.scaleBar }, { "scale_bar_length", o.scaleBarLength },
        { "scale_bar_unit", o.scaleBarUnit }, { "scale_bar_position", o.scaleBarPosition },
        { "compass", o.compass }, { "compass_position", o.compassPosition },
        { "preview", preview }, { "preview_px", previewPx },
    };
    if (o.useRange && o.vmin < o.vmax) {
        b["vmin"] = o.vmin;
        b["vmax"] = o.vmax;
    }
    QJsonArray regions;
    const auto add = [&regions](const QJsonArray &a) {
        for (const QJsonValue &v : a)
            regions.append(v);
    };
    if (o.contours)
        add(ov.contours());
    if (o.regions)
        add(ov.regions);
    if (o.annotations)
        add(ov.annotations);
    b["regions"] = regions;
    if (!preview)
        b["output_basename"] = outputName(outputStem, o.format);
    return b;
}

}   // namespace visivo::figure
