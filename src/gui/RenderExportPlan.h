#pragma once

// Save View As… (issue #528): what a saved picture of a 3-D view is, as pure
// logic with no Qt Widgets, no VTK and no GL, so the size and format rules can
// be unit-tested. The window that saves, the dialog that asks and the writer
// that draws are in RenderExport.h and SaveViewDialog.h.

#include <QSize>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace visivo::renderexport {

enum class Format { Png, Pdf, Svg };

/// The size of the picture over the size of the view on screen. `Custom` gives
/// a width and the height follows the view's shape (the camera frames the same
/// scene, so the shape is not free).
enum class SizeChoice { Current, Twice, Fourfold, Custom };

/// What the user asked for.
struct Request
{
    Format format = Format::Png;
    SizeChoice size = SizeChoice::Current;
    int customWidth = 0;
    bool transparent = false;
    /// The colour bar, the axes box and the title drawn over the render, as
    /// vectors in a PDF or SVG and as pixels in a PNG.
    bool annotations = true;
};

/// The largest side and the most pixels of a render: a tiled read-back holds
/// the picture three times over (the VTK image, the Qt image, the converted
/// one), so this keeps a 4x save of a large window under about a gigabyte.
inline constexpr int kMaxSide = 16384;
inline constexpr qint64 kMaxPixels = 100'000'000;

/// The filter text of the file dialog for `format`.
inline QString filterFor(Format format)
{
    switch (format) {
    case Format::Pdf: return QStringLiteral("PDF document (*.pdf)");
    case Format::Svg: return QStringLiteral("SVG image (*.svg)");
    case Format::Png: break;
    }
    return QStringLiteral("PNG image (*.png)");
}

/// The suffix, without the dot, a file of `format` carries.
inline QString suffixFor(Format format)
{
    switch (format) {
    case Format::Pdf: return QStringLiteral("pdf");
    case Format::Svg: return QStringLiteral("svg");
    case Format::Png: break;
    }
    return QStringLiteral("png");
}

/// The format a file name asks for by its suffix, or `fallback` when it has
/// none of the three: the user may retype the name in the file dialog.
inline Format formatOfName(const QString &name, Format fallback)
{
    const QString lower = name.toLower();
    if (lower.endsWith(QStringLiteral(".pdf")))
        return Format::Pdf;
    if (lower.endsWith(QStringLiteral(".svg")))
        return Format::Svg;
    if (lower.endsWith(QStringLiteral(".png")))
        return Format::Png;
    return fallback;
}

/// `path` with the suffix of `format`, unless it has it already.
inline QString withSuffix(const QString &path, Format format)
{
    const QString dot = QStringLiteral(".") + suffixFor(format);
    return path.endsWith(dot, Qt::CaseInsensitive) ? path : path + dot;
}

/// The height that keeps the shape of `view` at `width`.
inline int heightForWidth(const QSize &view, int width)
{
    if (view.width() <= 0 || view.height() <= 0 || width <= 0)
        return 0;
    return std::max(1, static_cast<int>(std::lround(double(width) * view.height() / view.width())));
}

/// The size of the picture for `request` over a view of `view` device pixels.
/// Empty when the view has no size.
inline QSize outputSize(const QSize &view, const Request &request)
{
    if (view.width() <= 0 || view.height() <= 0)
        return {};
    switch (request.size) {
    case SizeChoice::Current: return view;
    case SizeChoice::Twice: return view * 2;
    case SizeChoice::Fourfold: return view * 4;
    case SizeChoice::Custom:
        if (request.customWidth <= 0)
            return {};
        return { request.customWidth, heightForWidth(view, request.customWidth) };
    }
    return view;
}

/// Why `size` cannot be rendered, or an empty text when it can.
inline QString sizeProblem(const QSize &size)
{
    if (size.width() < 1 || size.height() < 1)
        return QStringLiteral("The size is empty.");
    if (size.width() > kMaxSide || size.height() > kMaxSide)
        return QStringLiteral("A side is over %1 pixels.").arg(kMaxSide);
    if (qint64(size.width()) * size.height() > kMaxPixels)
        return QStringLiteral("The picture is over %1 million pixels.").arg(kMaxPixels / 1'000'000);
    return {};
}

/// How many tiles per side the read-back needs: the smallest whole number of
/// view-sized tiles that covers `out` (the picture is then scaled down to
/// `out` when it is not a whole multiple).
inline int tileScale(const QSize &view, const QSize &out)
{
    if (view.width() <= 0 || view.height() <= 0)
        return 1;
    const double sx = double(out.width()) / view.width();
    const double sy = double(out.height()) / view.height();
    return std::max(1, static_cast<int>(std::ceil(std::max(sx, sy) - 1e-9)));
}

/// A suggested file name: the dataset's (or the window's) name, cleaned for a
/// file system, with the suffix of `format`.
inline QString suggestedName(const QString &base, Format format)
{
    QString name = base.trimmed();
    for (QChar &c : name) {
        if (c == u'/' || c == u'\\' || c == u':' || c == u'*' || c == u'?' || c == u'"'
            || c == u'<' || c == u'>' || c == u'|')
            c = u'_';
    }
    name.replace(u' ', u'_');
    if (name.isEmpty())
        name = QStringLiteral("view3d");
    return name + QStringLiteral("_view.") + suffixFor(format);
}

} // namespace visivo::renderexport
