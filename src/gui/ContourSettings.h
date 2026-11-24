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
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    /// multiples of it that are drawn (a negative one, "-3", is below zero), and
    /// whether -n x sigma is drawn as well for each positive one.
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
            // A negative multiple is a level below zero (-3 draws -3 sigma);
            // "negative" adds the mirror of the positive ones.
            if (m == 0.0)
                continue;
            out.push_back(m * s.sigma);
            if (s.negative && m > 0.0)
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

/// What decides the levels an image gets at first.
struct DataStats
{
    double sigma{ 0.0 };           ///< 1.4826 x MAD; 0 when it cannot be told
    double min{ 0.0 }, max{ 0.0 };   ///< of the finite values
    double lowPositive{ 0.0 };     ///< 5th percentile of the positive finite values
    double highPositive{ 0.0 };    ///< 99.5th percentile of the same
    bool valid{ false };           ///< at least two finite values
};

/// The statistics of the finite values of @p sample.
inline DataStats dataStats(std::vector<double> sample)
{
    DataStats st;
    sample.erase(std::remove_if(sample.begin(), sample.end(),
                                [](double v) { return !std::isfinite(v); }),
                 sample.end());
    if (sample.size() < 2)
        return st;
    st.valid = true;
    const auto [lo, hi] = std::minmax_element(sample.begin(), sample.end());
    st.min = *lo;
    st.max = *hi;
    std::vector<double> positive;
    for (double v : sample)
        if (v > 0.0)
            positive.push_back(v);
    if (!positive.empty()) {
        const auto percentile = [&positive](double q) {
            const std::size_t at = std::min(positive.size() - 1,
                                            std::size_t(q * double(positive.size() - 1) + 0.5));
            std::nth_element(positive.begin(), positive.begin() + std::ptrdiff_t(at),
                             positive.end());
            return positive[at];
        };
        st.lowPositive = percentile(0.05);
        st.highPositive = percentile(0.995);
    }
    st.sigma = robustSigma(std::move(sample));
    return st;
}

/// Whether @p bunit is a radio brightness: Jy/beam and its kin.
inline bool isRadioUnit(const QString &bunit)
{
    const QString u = bunit.toLower();
    return u.contains(QLatin1String("jy")) || u.contains(QLatin1String("beam"));
}

/// The levels a file's contours start with (Load Contour from FITS): 3, 5, 10
/// and 20 sigma of its noise, and -3 sigma, dashed, when @p radio (a radio
/// image has a negative side that is noise or a sidelobe worth seeing). When
/// sigma says nothing about the image (the MAD is zero, as in a map that is
/// zero outside its mask; the data do not reach 3 sigma or go below -2 sigma, as
/// in a simulated field or an image on a sky level, which are not noise around
/// zero) the levels are logarithmic
/// between the 5th and 99.5th percentile of the positive values, and @p note
/// says so; with no positive values they are linear over the range.
inline Settings defaultSettings(const DataStats &st, bool radio, QString *note = nullptr)
{
    Settings s;
    s.colourMode = ColourMode::Single;
    if (note)
        note->clear();
    if (!st.valid)
        return s;
    s.lower = st.min;
    s.upper = st.max;
    // Noise is centred on zero and reaches below it, and the data reach its
    // 3-sigma level: an all-positive field (a density, an optical image on a sky
    // level) has a MAD that is not a noise, and 3 x it is just a small number.
    const bool reaches = st.sigma > 0.0 && std::isfinite(st.sigma) && 3.0 * st.sigma <= st.max
            && st.min <= -2.0 * st.sigma;
    if (reaches) {
        s.levelMode = LevelMode::Sigma;
        s.sigma = st.sigma;
        s.multiples = radio ? QStringLiteral("-3, 3, 5, 10, 20") : QStringLiteral("3, 5, 10, 20");
        return s;
    }
    s.count = 8;
    if (st.highPositive > st.lowPositive && st.lowPositive > 0.0) {
        s.levelMode = LevelMode::Log;
        s.lower = st.lowPositive;
        s.upper = st.highPositive;
        if (note)
            *note = QStringLiteral("sigma is not meaningful for this image: logarithmic levels "
                                   "between %1 and %2.")
                            .arg(s.lower, 0, 'g', 4).arg(s.upper, 0, 'g', 4);
    } else {
        s.levelMode = LevelMode::Count;
        if (note)
            *note = QStringLiteral("sigma is not meaningful for this image: levels spread evenly "
                                   "between %1 and %2.")
                            .arg(s.lower, 0, 'g', 4).arg(s.upper, 0, 'g', 4);
    }
    s.sigma = st.sigma;
    return s;
}

/// A line for the panel: the noise and the levels the contours are drawn at.
inline QString describeLevels(const Settings &s, const std::vector<double> &lv)
{
    if (lv.empty())
        return QString();
    QString out;
    if (s.levelMode == LevelMode::Sigma && s.sigma > 0.0)
        out = QStringLiteral("σ = %1. ").arg(s.sigma, 0, 'g', 4);
    constexpr std::size_t kShown = 8;
    QStringList shown;
    for (std::size_t i = 0; i < std::min(lv.size(), kShown); ++i)
        shown << QString::number(lv[i], 'g', 4);
    out += QStringLiteral("%1 level(s): %2").arg(lv.size()).arg(shown.join(QStringLiteral(", ")));
    if (lv.size() > kShown)
        out += QStringLiteral(", … %1").arg(lv.back(), 0, 'g', 4);
    return out;
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
///
/// A vertex that is not a number is dropped and the line split there: a contour
/// that runs along a blanked (NaN) edge of the image has such vertices, and
/// Qt writes them as JSON null, which the backend cannot read. A piece of a
/// polygon that was cut is a polyline; pieces with too few vertices are skipped.
inline QJsonArray canonicalRegions(const std::vector<Line> &lines, double lineWidth,
                                   double sigma = 0.0)
{
    QJsonArray out;
    const auto append = [&](const Line &l, const std::vector<std::array<double, 2>> &pts,
                            bool closed) {
        QJsonArray points;
        for (const auto &p : pts)
            points.append(QJsonArray{ p[0] + 1.0, p[1] + 1.0 });
        QJsonArray tags{ QStringLiteral("level=%1").arg(l.level, 0, 'g', 8) };
        // Levels in sigma: the file says which noise they are multiples of.
        if (sigma > 0.0) {
            tags.append(QStringLiteral("sigma=%1").arg(sigma, 0, 'g', 8));
            tags.append(QStringLiteral("n_sigma=%1").arg(l.level / sigma, 0, 'g', 4));
        }
        QJsonObject props{ { QStringLiteral("color"), l.colour },
                           { QStringLiteral("width"), lineWidth },
                           { QStringLiteral("tag"), tags } };
        if (l.dashed)
            props[QStringLiteral("dash")] = true;
        out.append(QJsonObject{ { QStringLiteral("shape"),
                                  closed ? QStringLiteral("polygon") : QStringLiteral("line") },
                                { QStringLiteral("frame"), QStringLiteral("image") },
                                { QStringLiteral("unit"), QStringLiteral("pix") },
                                { QStringLiteral("points"), points },
                                { QStringLiteral("props"), props } });
    };
    for (const Line &l : lines) {
        std::vector<std::array<double, 2>> pts = l.pixels;
        // A closed line repeats its first vertex at the end; a polygon does not.
        if (l.closed && pts.size() > 1 && pts.front() == pts.back())
            pts.pop_back();
        const bool whole = std::all_of(pts.begin(), pts.end(), [](const std::array<double, 2> &p) {
            return std::isfinite(p[0]) && std::isfinite(p[1]);
        });
        if (whole) {
            if (pts.size() >= (l.closed ? 3u : 2u))
                append(l, pts, l.closed);
            continue;
        }
        std::vector<std::array<double, 2>> run;
        const auto flush = [&]() {
            if (run.size() >= 2)
                append(l, run, false);
            run.clear();
        };
        for (const auto &p : pts) {
            if (std::isfinite(p[0]) && std::isfinite(p[1]))
                run.push_back(p);
            else
                flush();
        }
        flush();
    }
    return out;
}

/// The step, in pixels along each axis, at which a @p width x @p height image is
/// sampled so that no more than @p maxPixels of it are contoured (1 = all of
/// it). A large image is thinned, as its preview is, because the cost of the
/// first contours grows with it.
inline int thinStride(long long width, long long height, long long maxPixels)
{
    if (width <= 0 || height <= 0 || maxPixels <= 0 || width * height <= maxPixels)
        return 1;
    int stride = int(std::ceil(std::sqrt(double(width) * double(height) / double(maxPixels))));
    while (((width + stride - 1) / stride) * ((height + stride - 1) / stride) > maxPixels)
        ++stride;
    return std::max(stride, 1);
}

// ── Joining segments into lines ──
// The contour filter gives one segment per grid cell it crosses; a dash pattern
// and a region file need the lines those segments make. VTK's own joiner is far
// too slow on a contour of a large image (minutes for a few million segments),
// and here every point is shared by at most two segments, so a walk from one
// segment to the next is all it takes: linear in the number of segments.

/// Lines as runs of point ids, one after another.
struct Chains
{
    std::vector<std::int64_t> ids;          ///< the ids of every line, end to end
    std::vector<std::size_t> start{ 0 };    ///< line i is ids[start[i], start[i + 1])
    std::vector<unsigned char> closed;      ///< 1 when line i ends where it starts
    bool cancelled{ false };                ///< stopped early: what is here is partial
    std::size_t count() const { return closed.size(); }
};

/// Join the segments @p segs (pairs of point ids, @p nSegments of them, over
/// @p nPoints points) into lines. A segment with an end that is not usable
/// (@p pointOk, when given, is 0 there) is left out, which splits the line it
/// was in. @p cancel, when it turns true, stops the walk.
template <typename Id>
Chains chainSegments(const Id *segs, std::size_t nSegments, std::size_t nPoints,
                     const unsigned char *pointOk = nullptr,
                     const std::atomic<bool> *cancel = nullptr)
{
    Chains out;
    constexpr std::int64_t none = -1;
    // The (at most two) segments of each point; a third one cannot happen on a
    // contour and would only be left as a line of its own.
    std::vector<std::int64_t> slot(2 * nPoints, none);
    std::vector<unsigned char> done(nSegments, 0);
    for (std::size_t s = 0; s < nSegments; ++s) {
        const std::int64_t a = std::int64_t(segs[2 * s]), b = std::int64_t(segs[2 * s + 1]);
        if (a == b || a < 0 || b < 0 || std::size_t(a) >= nPoints || std::size_t(b) >= nPoints
            || (pointOk && (!pointOk[a] || !pointOk[b]))) {
            done[s] = 1;
            continue;
        }
        for (const std::int64_t p : { a, b }) {
            std::int64_t &first = slot[std::size_t(2 * p)];
            (first == none ? first : slot[std::size_t(2 * p + 1)]) = std::int64_t(s);
        }
    }
    const auto next = [&](std::int64_t p) {
        for (std::size_t k = 0; k < 2; ++k) {
            const std::int64_t s = slot[std::size_t(2 * p) + k];
            if (s != none && !done[std::size_t(s)])
                return s;
        }
        return none;
    };
    const auto other = [&](std::int64_t s, std::int64_t p) {
        const std::int64_t a = std::int64_t(segs[2 * std::size_t(s)]);
        return a == p ? std::int64_t(segs[2 * std::size_t(s) + 1]) : a;
    };
    std::vector<std::int64_t> ahead, behind;
    std::size_t lines = 0;
    for (std::size_t s0 = 0; s0 < nSegments; ++s0) {
        if (done[s0])
            continue;
        if (cancel && (lines++ & 0xFF) == 0 && cancel->load(std::memory_order_relaxed)) {
            out.cancelled = true;
            break;
        }
        done[s0] = 1;
        const std::int64_t a = std::int64_t(segs[2 * s0]), b = std::int64_t(segs[2 * s0 + 1]);
        ahead.clear();
        behind.clear();
        for (std::int64_t cur = b, s; (s = next(cur)) != none;) {
            done[std::size_t(s)] = 1;
            cur = other(s, cur);
            ahead.push_back(cur);
        }
        const bool closed = !ahead.empty() && ahead.back() == a;
        if (!closed) {
            for (std::int64_t cur = a, s; (s = next(cur)) != none;) {
                done[std::size_t(s)] = 1;
                cur = other(s, cur);
                behind.push_back(cur);
            }
        }
        out.ids.insert(out.ids.end(), behind.rbegin(), behind.rend());
        out.ids.push_back(a);
        out.ids.push_back(b);
        out.ids.insert(out.ids.end(), ahead.begin(), ahead.end());
        out.start.push_back(out.ids.size());
        out.closed.push_back(closed ? 1 : 0);
    }
    return out;
}

/// The contour segments of an image, with their vertices where they are to be
/// written (source pixels, 0-based), and how the lines they make are drawn: a
/// snapshot the export joins into lines off the GUI thread, so that nothing of
/// the live pipeline is shared with it.
struct LineSource
{
    std::vector<std::array<double, 2>> points;   ///< NaN where a vertex is not a number
    std::vector<double> level;                   ///< the contour value at each point
    std::vector<std::int64_t> segments;          ///< pairs of point ids
    std::vector<double> levels;                  ///< the levels drawn, ascending
    std::vector<QString> colours;                ///< "#rrggbb", one per level
    bool dashNegative{ true };
    double lineWidth{ 1.0 };
    /// The noise the levels are multiples of (0 when they are not).
    double sigma{ 0.0 };
    /// How it was made, when that is worth telling (the image was thinned).
    QString note;
};

/// The lines of @p src, each with the colour of its level and dashed when it is
/// below zero and the source says so. Vertices that are not numbers cut the
/// lines. @p cancel, when it turns true, returns what is joined so far.
inline std::vector<Line> joinLines(const LineSource &src, const std::atomic<bool> *cancel = nullptr)
{
    std::vector<Line> out;
    if (src.levels.empty() || src.colours.empty())
        return out;
    std::vector<unsigned char> ok(src.points.size());
    for (std::size_t i = 0; i < ok.size(); ++i)
        ok[i] = std::isfinite(src.points[i][0]) && std::isfinite(src.points[i][1]);
    const Chains chains = chainSegments(src.segments.data(), src.segments.size() / 2,
                                        src.points.size(), ok.data(), cancel);
    out.reserve(chains.count());
    for (std::size_t c = 0; c < chains.count(); ++c) {
        const std::size_t from = chains.start[c], to = chains.start[c + 1];
        if (to - from < 2)
            continue;
        Line line;
        const int idx = nearestLevel(src.levels, src.level[std::size_t(chains.ids[from])]);
        line.level = src.levels[std::size_t(idx)];
        line.dashed = src.dashNegative && line.level < 0.0;
        line.colour = src.colours[std::min<std::size_t>(std::size_t(idx), src.colours.size() - 1)];
        line.closed = chains.closed[c] != 0;
        line.pixels.reserve(to - from);
        for (std::size_t i = from; i < to; ++i)
            line.pixels.push_back(src.points[std::size_t(chains.ids[i])]);
        out.push_back(std::move(line));
    }
    return out;
}

// ── Dashes ──

/// The pieces of the polyline @p pts that a dash pattern leaves: @p on long,
/// then @p off of gap, from the first vertex, measured along the line (a
/// vertex at a turn does not restart it). Each piece goes to @p put as a
/// vector of points. A line shorter than a dash comes out whole.
template <typename Sink>
void dashPolyline(const std::vector<std::array<double, 2>> &pts, double on, double off, Sink &&put)
{
    if (pts.size() < 2)
        return;
    const double period = on + off;
    if (!(on > 0.0) || !(period > on)) {
        put(pts);
        return;
    }
    std::vector<std::array<double, 2>> piece;
    double phase = 0.0;   // position inside the pattern, 0..period
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const auto &p = pts[i], &q = pts[i + 1];
        const double length = std::hypot(q[0] - p[0], q[1] - p[1]);
        double at = 0.0;   // along this segment
        while (at < length) {
            const bool drawing = phase < on;
            const double boundary = drawing ? on : period;
            const double step = std::min(length - at, boundary - phase);
            if (drawing) {
                if (piece.empty())
                    piece.push_back({ p[0] + (q[0] - p[0]) * at / length,
                                      p[1] + (q[1] - p[1]) * at / length });
                const double t = (at + step) / length;
                piece.push_back({ p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t });
            }
            const bool reached = step >= boundary - phase;
            at = step >= length - at ? length : at + step;
            phase = reached ? boundary : phase + step;
            if (reached) {
                if (drawing) {
                    put(piece);
                    piece.clear();
                }
                if (boundary == period)
                    phase = 0.0;
            }
        }
    }
    if (piece.size() >= 2)
        put(piece);
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
