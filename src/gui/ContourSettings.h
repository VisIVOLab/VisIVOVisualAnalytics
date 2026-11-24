#ifndef ContourSettings_h
#define ContourSettings_h

// ContourSettings.h
// What a set of contours is made of, with no widget and no VTK so that it can be
// unit-tested: the settings the image viewer's own contours, the contours drawn
// from another FITS file and the cube's slice contours all share, the level
// rules (a count between two bounds, n x sigma, logarithmic, a typed list), a
// noise-aware Gaussian smoothing of the pixels, and the contour lines written as
// canonical regions for a ds9 file (issues #519, #49).

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace visivo::contours {

enum class LevelMode { Count, Sigma, Log, List };
enum class ColourMode { Single, Map, PerLevel };

struct Settings
{
    LevelMode levelMode{ LevelMode::Count };
    /// Count and Log: how many levels, between lower and upper (both included).
    int count{ 15 };
    double lower{ 0.0 };
    double upper{ 1.0 };
    /// Sigma: the noise, in the data's unit (0 = not measured yet), the
    /// multiples of it that are drawn, and whether -n x sigma is drawn as well.
    double sigma{ 0.0 };
    QString multiples{ QStringLiteral("3, 5, 10, 20") };
    bool negative{ false };
    /// List: the levels as typed.
    QString list;
    /// Gaussian smoothing of the pixels before they are contoured; 0 = none.
    double smoothing{ 0.0 };
    bool smoothingInArcsec{ false };
    ColourMode colourMode{ ColourMode::Map };
    QString colour{ QStringLiteral("#ff6633") };   ///< Single: any QColor name
    QString colourMap{ QStringLiteral("Rainbow") };   ///< Map: a ColorMaps name, blue to red as the contours always were
    QString perLevel;                              ///< PerLevel: colours, cycled over the levels
    double lineWidth{ 1.5 };
    bool dashNegative{ true };
};

/// The numbers of a typed list, separated by commas, semicolons or blanks.
/// @p ok is false when a token is not a finite number (the others are kept).
inline QVector<double> parseNumberList(const QString &text, bool *ok = nullptr)
{
    QVector<double> out;
    bool allOk = true;
    QString flat = text;
    flat.replace(QLatin1Char(','), QLatin1Char(' '));
    flat.replace(QLatin1Char(';'), QLatin1Char(' '));
    for (const QString &token : flat.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        bool good = false;
        const double v = token.toDouble(&good);
        if (good && std::isfinite(v))
            out.append(v);
        else
            allOk = false;
    }
    if (ok)
        *ok = allOk;
    return out;
}

/// The colours typed for Per level, as names QColor reads.
inline QStringList colourList(const QString &text)
{
    QString flat = text;
    flat.replace(QLatin1Char(','), QLatin1Char(' '));
    flat.replace(QLatin1Char(';'), QLatin1Char(' '));
    return flat.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

/// The levels the settings ask for, ascending and without repeats. Empty, with
/// the reason in @p problem, when they ask for none that can be drawn.
inline std::vector<double> levels(const Settings &s, QString *problem = nullptr)
{
    const auto fail = [problem](const QString &why) {
        if (problem)
            *problem = why;
        return std::vector<double>{};
    };
    if (problem)
        problem->clear();
    std::vector<double> out;
    switch (s.levelMode) {
    case LevelMode::Count: {
        if (s.count < 1 || !std::isfinite(s.lower) || !std::isfinite(s.upper)
            || !(s.lower < s.upper))
            return fail(QStringLiteral("Contour levels need Lower < Upper and at least one level."));
        // The same spacing as vtkContourValues::GenerateValues: both ends are
        // levels, and a single level is the lower bound.
        for (int i = 0; i < s.count; ++i)
            out.push_back(s.count == 1
                                  ? s.lower
                                  : s.lower + (s.upper - s.lower) * i / double(s.count - 1));
        break;
    }
    case LevelMode::Log: {
        if (s.count < 1 || !std::isfinite(s.lower) || !std::isfinite(s.upper) || !(s.lower > 0.0)
            || !(s.lower < s.upper))
            return fail(QStringLiteral(
                    "Logarithmic levels need 0 < Lower < Upper and at least one level."));
        const double ratio = s.upper / s.lower;
        for (int i = 0; i < s.count; ++i)
            out.push_back(s.count == 1 ? s.lower
                                       : s.lower * std::pow(ratio, i / double(s.count - 1)));
        break;
    }
    case LevelMode::Sigma: {
        if (!(s.sigma > 0.0) || !std::isfinite(s.sigma))
            return fail(QStringLiteral(
                    "Levels in sigma need the noise: type it, or press Estimate."));
        bool ok = true;
        const QVector<double> multiples = parseNumberList(s.multiples, &ok);
        for (double m : multiples) {
            if (!(m > 0.0))
                continue;
            out.push_back(m * s.sigma);
            if (s.negative)
                out.push_back(-m * s.sigma);
        }
        if (out.empty())
            return fail(QStringLiteral("Type the multiples of sigma, such as 3, 5, 10, 20."));
        break;
    }
    case LevelMode::List: {
        bool ok = true;
        const QVector<double> typed = parseNumberList(s.list, &ok);
        for (double v : typed)
            out.push_back(v);
        if (out.empty())
            return fail(QStringLiteral("Type the levels, separated by commas."));
        break;
    }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// 1.4826 x the median absolute deviation of the finite values of @p sample: the
/// noise of an image that is mostly noise, which a few bright sources hardly
/// move. 0 when fewer than two values are finite or half of them are equal.
inline double robustSigma(std::vector<double> sample)
{
    sample.erase(std::remove_if(sample.begin(), sample.end(),
                                [](double v) { return !std::isfinite(v); }),
                 sample.end());
    if (sample.size() < 2)
        return 0.0;
    const auto median = [](std::vector<double> &v) {
        const std::size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(mid), v.end());
        double m = v[mid];
        if (v.size() % 2 == 0) {
            const double below = *std::max_element(v.begin(), v.begin() + std::ptrdiff_t(mid));
            m = 0.5 * (m + below);
        }
        return m;
    };
    const double centre = median(sample);
    for (double &v : sample)
        v = std::abs(v - centre);
    return 1.4826 * median(sample);
}

/// robustSigma() of @p n values read from @p data, thinned to about a million so
/// that a large image costs a few milliseconds.
template <typename T>
double robustSigma(const T *data, std::size_t n)
{
    constexpr std::size_t kMax = 1u << 20;
    const std::size_t stride = std::max<std::size_t>(1, n / kMax);
    std::vector<double> sample;
    sample.reserve(n / stride + 1);
    for (std::size_t i = 0; i < n; i += stride)
        sample.push_back(double(data[i]));
    return robustSigma(std::move(sample));
}

/// Index of the level nearest to @p value in the ascending @p lv; -1 if empty.
inline int nearestLevel(const std::vector<double> &lv, double value)
{
    if (lv.empty())
        return -1;
    const auto it = std::lower_bound(lv.begin(), lv.end(), value);
    if (it == lv.begin())
        return 0;
    if (it == lv.end())
        return int(lv.size()) - 1;
    const int hi = int(it - lv.begin());
    return std::abs(*it - value) < std::abs(value - lv[std::size_t(hi - 1)]) ? hi : hi - 1;
}

/// A separable Gaussian of width @p sigmaX, @p sigmaY (pixels) over the w x h
/// image @p in, written to @p out (they may not overlap). NaN pixels are not
/// data: they weigh nothing in their neighbours' average and stay NaN
/// themselves, so a blanked edge neither darkens nor invents a contour. Sigmas
/// under a twentieth of a pixel leave the image as it is.
inline void smoothImage(const float *in, int w, int h, double sigmaX, double sigmaY, float *out)
{
    const std::size_t count = std::size_t(w) * std::size_t(h);
    if (w <= 0 || h <= 0)
        return;
    if (sigmaX < 0.05 && sigmaY < 0.05) {
        std::copy(in, in + count, out);
        return;
    }
    const auto kernel = [](double sigma) {
        std::vector<float> k;
        if (sigma < 0.05) {
            k.push_back(1.0f);
            return k;
        }
        const int radius = std::max(1, int(std::ceil(4.0 * sigma)));
        for (int i = -radius; i <= radius; ++i)
            k.push_back(float(std::exp(-0.5 * double(i) * double(i) / (sigma * sigma))));
        return k;
    };
    const std::vector<float> kx = kernel(sigmaX), ky = kernel(sigmaY);
    const int rx = int(kx.size() / 2), ry = int(ky.size() / 2);

    // Along x: the weighted values and the weights, row by row.
    std::vector<float> num(count, 0.0f), den(count, 0.0f);
    for (int y = 0; y < h; ++y) {
        const float *row = in + std::size_t(y) * std::size_t(w);
        float *nrow = num.data() + std::size_t(y) * std::size_t(w);
        float *drow = den.data() + std::size_t(y) * std::size_t(w);
        for (int x = 0; x < w; ++x) {
            float sn = 0.0f, sd = 0.0f;
            const int a = std::max(0, x - rx), b = std::min(w - 1, x + rx);
            for (int i = a; i <= b; ++i) {
                const float v = row[i];
                if (std::isnan(v))
                    continue;
                const float wgt = kx[std::size_t(i - x + rx)];
                sn += wgt * v;
                sd += wgt;
            }
            nrow[x] = sn;
            drow[x] = sd;
        }
    }
    // Along y, one column at a time, then the quotient.
    std::vector<float> colN(std::size_t(h), 0.0f), colD(std::size_t(h), 0.0f);
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            float sn = 0.0f, sd = 0.0f;
            const int a = std::max(0, y - ry), b = std::min(h - 1, y + ry);
            for (int j = a; j <= b; ++j) {
                const std::size_t from = std::size_t(j) * std::size_t(w) + std::size_t(x);
                const float wgt = ky[std::size_t(j - y + ry)];
                sn += wgt * num[from];
                sd += wgt * den[from];
            }
            colN[std::size_t(y)] = sn;
            colD[std::size_t(y)] = sd;
        }
        for (int y = 0; y < h; ++y) {
            const std::size_t at = std::size_t(y) * std::size_t(w) + std::size_t(x);
            out[at] = (std::isnan(in[at]) || !(colD[std::size_t(y)] > 0.0f))
                              ? std::numeric_limits<float>::quiet_NaN()
                              : colN[std::size_t(y)] / colD[std::size_t(y)];
        }
    }
}

/// One contour line, ready to be written: the vertices in source pixels
/// (0-based), whether it closes on itself, and how it is drawn.
struct Line
{
    double level{ 0.0 };
    bool closed{ false };
    std::vector<std::array<double, 2>> pixels;
    QString colour;
    bool dashed{ false };
};

/// The lines as canonical image-frame regions (1-based pixels, as region files
/// count them) for `POST /v1/regions/export`: a closed line is a polygon, an
/// open one (it leaves the image) a polyline, which ds9 reads as a segment. Each
/// carries its colour, width and dash, and its level as a tag.
inline QJsonArray canonicalRegions(const std::vector<Line> &lines, double lineWidth)
{
    QJsonArray out;
    for (const Line &l : lines) {
        std::vector<std::array<double, 2>> pts = l.pixels;
        // A closed line repeats its first vertex at the end; a polygon does not.
        if (l.closed && pts.size() > 1 && pts.front() == pts.back())
            pts.pop_back();
        if (pts.size() < (l.closed ? 3u : 2u))
            continue;
        QJsonArray points;
        for (const auto &p : pts)
            points.append(QJsonArray{ p[0] + 1.0, p[1] + 1.0 });
        QJsonObject props{ { QStringLiteral("color"), l.colour },
                           { QStringLiteral("width"), lineWidth },
                           { QStringLiteral("tag"),
                             QJsonArray{ QStringLiteral("level=%1").arg(l.level, 0, 'g', 8) } } };
        if (l.dashed)
            props[QStringLiteral("dash")] = true;
        out.append(QJsonObject{ { QStringLiteral("shape"),
                                  l.closed ? QStringLiteral("polygon") : QStringLiteral("line") },
                                { QStringLiteral("frame"), QStringLiteral("image") },
                                { QStringLiteral("unit"), QStringLiteral("pix") },
                                { QStringLiteral("points"), points },
                                { QStringLiteral("props"), props } });
    }
    return out;
}

/// A table of the vertices of canonical contour regions (the output of
/// canonicalRegions(), in pixels or after the backend moved them to a sky
/// frame): one row per vertex with the line's level, its number and whether it
/// closes. @p xName and @p yName head the two coordinate columns.
inline QString verticesCsv(const QJsonArray &regions, const QString &xName, const QString &yName)
{
    QString out = QStringLiteral("level,line,closed,vertex,%1,%2\n").arg(xName, yName);
    int lineNo = 0;
    for (const QJsonValue &rv : regions) {
        const QJsonObject r = rv.toObject();
        double level = std::numeric_limits<double>::quiet_NaN();
        const QJsonArray tags = r.value(QStringLiteral("props")).toObject()
                                        .value(QStringLiteral("tag")).toArray();
        for (const QJsonValue &t : tags) {
            const QString tag = t.toString();
            if (tag.startsWith(QStringLiteral("level=")))
                level = tag.mid(6).toDouble();
        }
        const bool closed = r.value(QStringLiteral("shape")).toString() == QLatin1String("polygon");
        ++lineNo;
        int vertex = 0;
        for (const QJsonValue &pv : r.value(QStringLiteral("points")).toArray()) {
            const QJsonArray p = pv.toArray();
            out += QStringLiteral("%1,%2,%3,%4,%5,%6\n")
                           .arg(level, 0, 'g', 8)
                           .arg(lineNo)
                           .arg(closed ? 1 : 0)
                           .arg(++vertex)
                           .arg(p.at(0).toDouble(), 0, 'f', 8)
                           .arg(p.at(1).toDouble(), 0, 'f', 8);
        }
    }
    return out;
}

}   // namespace visivo::contours

#endif   // ContourSettings_h
