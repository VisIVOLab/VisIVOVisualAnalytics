#ifndef RegionMeasure_h
#define RegionMeasure_h

// Region measurements of a continuum image (issue #503), as plain functions of
// plain numbers so that they test without a window: the rows of the
// statistics (sum and the unweighted RMS beside the standard deviation, the
// peak and where it is), the point and line regions a click or a drag gives,
// the rows of the image noise, and the CSV of a line profile.

#include "RegionModel.h"
#include "app/BackendClient.h"

#include <QList>
#include <QLocale>
#include <QPair>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace visivo::measure {

using Rows = QList<QPair<QString, QString>>;

/// The two spreads of a region, named so that they cannot be taken for each
/// other: RMS is sqrt(<x^2>) about zero (CASA imstat's rms), the standard
/// deviation is about the mean. They differ by the mean.
inline QString rmsLabel() { return QStringLiteral("RMS (√⟨x²⟩)"); }
inline QString stdLabel() { return QStringLiteral("Std dev"); }
inline QString madLabel() { return QStringLiteral("σ (MAD)"); }

inline QString num(double v, int digits = 8)
{
    return QString::number(v, 'g', digits);
}

/// A value with its unit ("0.196 Jy/beam"); the unit may be empty.
inline QString withUnit(double v, const QString &unit, int digits = 8)
{
    const QString u = unit.trimmed();
    return u.isEmpty() ? num(v, digits) : QStringLiteral("%1 %2").arg(num(v, digits), u);
}

/// What the statistics rows need of a region's numbers.
struct Numbers
{
    long long validCount{ 0 };
    long long totalCount{ 0 };
    long long blankedCount{ 0 };
    double minValue{ 0. }, maxValue{ 0. }, sum{ 0. }, mean{ 0. }, median{ 0. };
    double stddev{ 0. }, rmsUnweighted{ 0. }, madSigma{ 0. };
    /// False for numbers from a backend that predates the unweighted RMS: the
    /// row is left out rather than shown as 0.
    bool hasRmsUnweighted{ true };
    bool hasPeak{ false };
    double peakX{ 0. }, peakY{ 0. };   ///< 0-based source pixels
};

/// The unweighted RMS from a sum of squares: sqrt(sum(x^2) / n).
inline double rmsFromSumSq(double sumSq, long long n)
{
    return n > 0 ? std::sqrt(std::max(0.0, sumSq) / static_cast<double>(n)) : 0.0;
}

/// "Statistics" section rows. @p unit is the image's BUNIT (shown after the
/// sum and the peak, never after the counts).
inline Rows statisticsRows(const Numbers &n, const QString &unit)
{
    const QLocale en(QLocale::English);
    Rows rows;
    rows.append({ QStringLiteral("Valid pixels"),
                  QStringLiteral("%1 / %2").arg(en.toString(n.validCount), en.toString(n.totalCount)) });
    rows.append({ QStringLiteral("Blanked / NaN"), en.toString(n.blankedCount) });
    rows.append({ QStringLiteral("Min"), num(n.minValue) });
    rows.append({ QStringLiteral("Max"), num(n.maxValue) });
    rows.append({ QStringLiteral("Sum"), withUnit(n.sum, unit) });
    rows.append({ QStringLiteral("Mean"), num(n.mean) });
    rows.append({ QStringLiteral("Median"), num(n.median) });
    rows.append({ stdLabel(), num(n.stddev) });
    if (n.hasRmsUnweighted)
        rows.append({ rmsLabel(), num(n.rmsUnweighted) });
    rows.append({ madLabel(), num(n.madSigma) });
    return rows;
}

/// The peak of a region and its position: pixel (0-based, as the readout) and,
/// when @p world is not empty, the sky position. For an unresolved source the
/// peak is its flux density in the image's unit (Jy/beam).
inline Rows peakRows(const Numbers &n, const QString &unit, const QString &world)
{
    Rows rows;
    rows.append({ QStringLiteral("Peak"), withUnit(n.maxValue, unit) });
    if (n.hasPeak) {
        QString where = QStringLiteral("pixel (%1, %2)")
                                .arg(std::llround(n.peakX)).arg(std::llround(n.peakY));
        if (!world.isEmpty())
            where += QStringLiteral("  ·  ") + world;
        rows.append({ QStringLiteral("Peak position"), where });
    }
    return rows;
}

/// The rows of a point region: the value at its pixel and where that is.
inline Rows pointRows(double value, double pixelX, double pixelY, const QString &world,
                      const QString &unit, bool blank)
{
    Rows rows;
    rows.append({ QStringLiteral("Value"),
                  blank ? QStringLiteral("blank (NaN)") : withUnit(value, unit) });
    QString where = QStringLiteral("pixel (%1, %2)").arg(std::llround(pixelX)).arg(std::llround(pixelY));
    if (!world.isEmpty())
        where += QStringLiteral("  ·  ") + world;
    rows.append({ QStringLiteral("Position"), where });
    return rows;
}

// ── Point and line regions of the list ───────────────────────────────────────

/// A point region at source pixel (x, y).
inline regions::Region pointRegion(double x, double y)
{
    regions::Region r;
    r.shape = regions::Shape::Point;
    r.center = { x, y };
    return r;
}

/// A line region from (x0, y0) to (x1, y1), in source pixels; nullopt when the
/// two ends are the same place (a drag that did not go anywhere).
inline std::optional<regions::Region> lineRegion(double x0, double y0, double x1, double y1)
{
    if (std::hypot(x1 - x0, y1 - y0) < 0.5)
        return std::nullopt;
    regions::Region r;
    r.shape = regions::Shape::Line;
    r.points = { { x0, y0 }, { x1, y1 } };
    return r;
}

// ── Line profile ─────────────────────────────────────────────────────────────

/// The summary rows under the profile plot.
inline Rows profileRows(const BackendRegionProfileResult &p)
{
    Rows rows;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (double v : p.values)
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    const QString unit = p.bunit.trimmed();
    const QString length = p.offsetUnit == QLatin1String("arcsec") && !p.offsets.isEmpty()
            ? QStringLiteral("%1\"").arg(p.offsets.last(), 0, 'f', 2)
            : QStringLiteral("%1 px").arg(p.lengthPix, 0, 'f', 2);
    rows.append({ QStringLiteral("Length"), length });
    rows.append({ QStringLiteral("Samples"),
                  QStringLiteral("%1 valid of %2 (every pixel along the line, width %3 px)")
                          .arg(p.validSamples).arg(p.values.size()).arg(p.widthPix) });
    if (std::isfinite(lo)) {
        rows.append({ QStringLiteral("Min"), withUnit(lo, unit) });
        rows.append({ QStringLiteral("Max"), withUnit(hi, unit) });
    }
    return rows;
}

// ── Image noise ──────────────────────────────────────────────────────────────

/// The rows of the noise result: RMS and sigma(MAD) in the image's unit, the
/// per-pixel and per-beam conversion for a Jy/beam image, and where the number
/// comes from (the background regions with each one's own value, or the
/// sigma-clipped plane).
inline Rows noiseRows(const BackendImageNoiseResult &r)
{
    const QLocale en(QLocale::English);
    const QString unit = r.bunit.trimmed();
    Rows rows;
    rows.append({ rmsLabel(), withUnit(r.rms, unit) });
    rows.append({ madLabel(), withUnit(r.madSigma, unit) });
    if (r.beamAreaPix > 0.0) {
        rows.append({ QStringLiteral("Beam area"), QStringLiteral("%1 pixels").arg(r.beamAreaPix, 0, 'f', 2) });
        // Jy/beam divided by the pixels a beam covers: the same noise as a
        // flux density per pixel.
        rows.append({ QStringLiteral("RMS per pixel"), QStringLiteral("%1 Jy/pixel").arg(num(r.rmsPerPixel, 6)) });
        rows.append({ QStringLiteral("σ (MAD) per pixel"),
                      QStringLiteral("%1 Jy/pixel").arg(num(r.madSigmaPerPixel, 6)) });
    }
    if (r.mode == QLatin1String("regions")) {
        rows.append({ QStringLiteral("From"),
                      QStringLiteral("the average of %1 background region%2 (%3 pixels)")
                              .arg(r.regions.size()).arg(r.regions.size() == 1 ? "" : "s")
                              .arg(en.toString(r.validCount)) });
        if (std::isfinite(r.rmsScatter))
            rows.append({ QStringLiteral("Scatter between regions"), withUnit(r.rmsScatter, unit, 4) });
        for (const auto &p : r.regions) {
            rows.append({ p.name.isEmpty() ? QStringLiteral("Region") : p.name,
                          p.measured ? QStringLiteral("RMS %1  ·  σ (MAD) %2  ·  %3 px")
                                               .arg(num(p.rms, 6), num(p.madSigma, 6), en.toString(p.validCount))
                                     : QStringLiteral("fewer than two valid pixels: not used") });
        }
    } else {
        QString from = QStringLiteral("the whole image, %1σ-clipped: %2 of %3 pixels kept")
                               .arg(r.clipSigma, 0, 'g', 3)
                               .arg(en.toString(r.usedCount), en.toString(r.validCount));
        if (!r.exact)
            from += QStringLiteral("; the clip window from a random sample of %1 pixels")
                            .arg(en.toString(r.sampleCount));
        rows.append({ QStringLiteral("From"), from });
    }
    return rows;
}

}   // namespace visivo::measure

#endif // RegionMeasure_h
