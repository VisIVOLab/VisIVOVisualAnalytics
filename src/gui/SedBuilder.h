#ifndef SedBuilder_h
#define SedBuilder_h

// SedBuilder.h
// Turn catalogue rows into a spectral energy distribution.
//
// A SED is a list of (wavelength, flux, error) triples, and a catalogue almost
// never states it that way. Two shapes have to be recognised:
//
//  * VLKB band-merged (compactsources.sed_view_final): one physical source is
//    several overlay entries sharing a groupKey, each tagged with its band
//    ("70", "250", "870"…), whose own columns are flux70 / err_flux70 / …
//  * a flat catalogue: one row carrying several flux columns at once
//    (flux_250, F350, S_870um…), each of which is one SED point.
//
// The rules live here, alone and without Qt widgets, because this is where a
// SED goes wrong quietly: a misread column gives a fit that runs and is wrong.
// Everything is header-only so the unit tests can exercise it directly.

#include "CatalogueOverlayUtils.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <vector>

namespace visivo {

/// One photometric point of a SED.
struct SedPoint
{
    double wavelengthUm{ 0.0 };
    double fluxJy{ 0.0 };
    double errorJy{ 0.0 };
    /// Source angular size at this band (arcsec, geometric mean of the axes);
    /// 0 when the catalogue does not give one. The optically thick fit needs it.
    double sizeArcsec{ 0.0 };
    bool upperLimit{ false };
    /// Included in the fit. A point is dropped by default only when it carries
    /// no usable flux; the user can untick the rest.
    bool use{ true };
    QString band;        ///< the catalogue's own band tag, for display
    QString label;       ///< per-band designation, when the catalogue has one
    /// Index of the overlay entry this point came from, or -1 when several
    /// bands were read out of one flat row.
    int entryIndex{ -1 };
};

struct SedBuildResult
{
    bool valid{ false };
    QString errorMessage;
    QString sourceLabel;
    std::vector<SedPoint> points;
    /// Bands found in the row but unusable (no flux, non-numeric), named so the
    /// user is told why a band they expected is missing rather than left to
    /// wonder.
    QStringList skippedBands;
};

namespace SedBuilder {

/// Wavelengths (µm) of the survey bands whose catalogue columns are named by a
/// number that is not the wavelength. Hi-GAL/band-merged columns are named by
/// wavelength already; MSX 21 µm carries three sub-bands in the legacy view.
inline double bandTagToWavelengthUm(const QString &tag)
{
    static const QMap<QString, double> kSpecial{
        { QStringLiteral("21_c"), 12.1 },
        { QStringLiteral("21_d"), 14.7 },
        { QStringLiteral("21_e"), 21.0 },
    };
    const QString key = tag.trimmed().toLower();
    if (kSpecial.contains(key))
        return kSpecial.value(key);
    bool ok = false;
    const double value = key.toDouble(&ok);
    return (ok && value > 0.0) ? value : 0.0;
}

/// Case-insensitive lookup of the first field whose name matches any candidate.
inline bool fieldValue(const QList<QPair<QString, QString>> &fields,
                       const QStringList &candidates, QString *out)
{
    for (const QString &candidate : candidates) {
        for (const auto &field : fields) {
            if (field.first.compare(candidate, Qt::CaseInsensitive) == 0) {
                if (out)
                    *out = field.second;
                return true;
            }
        }
    }
    return false;
}

inline bool numericField(const QList<QPair<QString, QString>> &fields,
                         const QStringList &candidates, double *out)
{
    QString text;
    if (!fieldValue(fields, candidates, &text))
        return false;
    bool ok = false;
    const double value = text.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(value))
        return false;
    if (out)
        *out = value;
    return true;
}

/// Column names that hold the flux of band ``tag``, most specific first.
inline QStringList fluxCandidates(const QString &tag)
{
    if (tag.isEmpty())
        return { QStringLiteral("flux"), QStringLiteral("fint"), QStringLiteral("f_int"),
                 QStringLiteral("flux_jy") };
    return { QStringLiteral("flux%1").arg(tag), QStringLiteral("flux_%1").arg(tag),
             QStringLiteral("fint%1").arg(tag), QStringLiteral("f%1").arg(tag),
             QStringLiteral("flux"), QStringLiteral("fint") };
}

inline QStringList errorCandidates(const QString &tag)
{
    if (tag.isEmpty())
        return { QStringLiteral("err_flux"), QStringLiteral("e_flux"),
                 QStringLiteral("err_fint"), QStringLiteral("flux_err") };
    return { QStringLiteral("err_flux%1").arg(tag), QStringLiteral("err_flux_%1").arg(tag),
             QStringLiteral("e_flux%1").arg(tag), QStringLiteral("err_fint%1").arg(tag),
             QStringLiteral("err_flux"), QStringLiteral("e_flux") };
}

inline QStringList designationCandidates(const QString &tag)
{
    if (tag.isEmpty())
        return { QStringLiteral("designation"), QStringLiteral("name"), QStringLiteral("id") };
    return { QStringLiteral("designation%1").arg(tag), QStringLiteral("designation_%1").arg(tag),
             QStringLiteral("designation") };
}

/// Angular size of a band's source, in arcsec: √(FWHM_a·FWHM_b) when the
/// catalogue gives both axes, otherwise whichever one it gives.
inline double sizeFromFields(const QList<QPair<QString, QString>> &fields, const QString &tag)
{
    double a = 0.0;
    double b = 0.0;
    const bool hasA = numericField(fields, { QStringLiteral("fwhma%1").arg(tag),
                                             QStringLiteral("fwhma"), QStringLiteral("fwhm_a") }, &a);
    const bool hasB = numericField(fields, { QStringLiteral("fwhmb%1").arg(tag),
                                             QStringLiteral("fwhmb"), QStringLiteral("fwhm_b") }, &b);
    if (hasA && hasB && a > 0.0 && b > 0.0)
        return std::sqrt(a * b);
    if (hasA && a > 0.0)
        return a;
    if (hasB && b > 0.0)
        return b;
    return 0.0;
}

/// Read one SED point out of one overlay entry tagged with a band.
/// Returns false when the entry carries no usable flux at that band.
inline bool pointFromEntry(const CatalogueOverlayEntry &entry, int entryIndex, SedPoint *out)
{
    const QString tag = entry.band.trimmed();
    const double wavelength = bandTagToWavelengthUm(tag);
    if (wavelength <= 0.0)
        return false;

    double flux = 0.0;
    if (!numericField(entry.extra, fluxCandidates(tag), &flux) || flux <= 0.0)
        return false;

    SedPoint point;
    point.wavelengthUm = wavelength;
    point.fluxJy = flux;
    point.band = tag;
    point.entryIndex = entryIndex;
    numericField(entry.extra, errorCandidates(tag), &point.errorJy);
    if (!std::isfinite(point.errorJy) || point.errorJy < 0.0)
        point.errorJy = 0.0;
    point.sizeArcsec = sizeFromFields(entry.extra, tag);
    if (point.sizeArcsec <= 0.0)
        point.sizeArcsec = entry.sizeArcsec > 0.0 ? entry.sizeArcsec : 0.0;
    QString designation;
    if (fieldValue(entry.extra, designationCandidates(tag), &designation))
        point.label = designation.trimmed();
    if (point.label.isEmpty())
        point.label = entry.label;
    if (out)
        *out = point;
    return true;
}

/// Wavelength in µm encoded in a flux column name: flux250, flux_250, F_350um,
/// S870, S_1.2mm. Returns 0 when the name carries no wavelength.
///
/// The unit suffix is *converted*, not skipped: a column named S_1.2mm read as
/// 1.2 µm would put a sub-mm point four orders of magnitude out of place, at
/// the far end of the SED where the greybody slope is fitted.
inline double wavelengthFromFluxColumn(const QString &name)
{
    static const QRegularExpression re(
            QStringLiteral("^(?:flux|fint|f|s)[_ ]?([0-9]+(?:\\.[0-9]+)?)\\s*"
                           "(um|µm|micron|microns|mm|cm|m|nm)?$"),
            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(name.trimmed());
    if (!m.hasMatch())
        return 0.0;
    bool ok = false;
    const double value = m.captured(1).toDouble(&ok);
    if (!ok || value <= 0.0)
        return 0.0;

    const QString unit = m.captured(2).trimmed().toLower();
    if (unit.isEmpty() || unit == QStringLiteral("um") || unit == QStringLiteral("\u00b5m")
        || unit.startsWith(QStringLiteral("micron")))
        return value;
    if (unit == QStringLiteral("nm"))
        return value * 1e-3;
    if (unit == QStringLiteral("mm"))
        return value * 1e3;
    if (unit == QStringLiteral("cm"))
        return value * 1e4;
    if (unit == QStringLiteral("m"))
        return value * 1e6;
    return 0.0;
}

/// Build a SED out of one flat catalogue row that carries several flux columns.
inline std::vector<SedPoint> pointsFromWideRow(const CatalogueOverlayEntry &entry, int entryIndex,
                                               QStringList *skipped)
{
    std::vector<SedPoint> points;
    for (const auto &field : entry.extra) {
        const double wavelength = wavelengthFromFluxColumn(field.first);
        if (wavelength <= 0.0)
            continue;
        bool ok = false;
        const double flux = field.second.trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(flux) || flux <= 0.0) {
            if (skipped)
                skipped->append(field.first);
            continue;
        }
        SedPoint point;
        point.wavelengthUm = wavelength;
        point.fluxJy = flux;
        point.band = QString::number(wavelength);
        point.entryIndex = entryIndex;
        point.label = entry.label;
        // The error column mirrors the flux column's own spelling.
        const QString stem = field.first.trimmed();
        numericField(entry.extra,
                     { QStringLiteral("err_%1").arg(stem), QStringLiteral("e_%1").arg(stem),
                       QStringLiteral("%1_err").arg(stem), QStringLiteral("%1_error").arg(stem) },
                     &point.errorJy);
        if (!std::isfinite(point.errorJy) || point.errorJy < 0.0)
            point.errorJy = 0.0;
        point.sizeArcsec = sizeFromFields(entry.extra, QString());
        if (point.sizeArcsec <= 0.0 && entry.sizeArcsec > 0.0)
            point.sizeArcsec = entry.sizeArcsec;
        points.push_back(point);
    }
    return points;
}

/// Build the SED of the entries at ``indices``.
///
/// Band-tagged entries (VLKB) contribute one point each; when the selection is
/// a single untagged row, its flux columns are read across instead. Points are
/// returned sorted by wavelength, with duplicates at the same wavelength
/// resolved in favour of the brighter measurement — two entries of the same
/// source in the same band is a catalogue artefact, not two data points.
inline SedBuildResult buildSed(const std::vector<CatalogueOverlayEntry> &entries,
                               const std::vector<int> &indices)
{
    SedBuildResult result;
    if (indices.empty()) {
        result.errorMessage = QStringLiteral("No catalogue source selected.");
        return result;
    }

    std::vector<SedPoint> points;
    for (int index : indices) {
        if (index < 0 || static_cast<std::size_t>(index) >= entries.size())
            continue;
        const CatalogueOverlayEntry &entry = entries[static_cast<std::size_t>(index)];
        SedPoint point;
        if (pointFromEntry(entry, index, &point)) {
            points.push_back(point);
            continue;
        }
        if (!entry.band.trimmed().isEmpty() && bandTagToWavelengthUm(entry.band) > 0.0) {
            // A band we recognise but could not read a flux from: say so.
            result.skippedBands.append(entry.band.trimmed());
            continue;
        }
        const std::vector<SedPoint> wide = pointsFromWideRow(entry, index, &result.skippedBands);
        points.insert(points.end(), wide.begin(), wide.end());
        if (result.sourceLabel.isEmpty())
            result.sourceLabel = entry.label;
    }

    if (result.sourceLabel.isEmpty() && !indices.empty()) {
        const int first = indices.front();
        if (first >= 0 && static_cast<std::size_t>(first) < entries.size()) {
            const CatalogueOverlayEntry &entry = entries[static_cast<std::size_t>(first)];
            result.sourceLabel = entry.groupKey.isEmpty() ? entry.label : entry.groupKey;
        }
    }

    std::sort(points.begin(), points.end(), [](const SedPoint &a, const SedPoint &b) {
        if (a.wavelengthUm != b.wavelengthUm)
            return a.wavelengthUm < b.wavelengthUm;
        return a.fluxJy > b.fluxJy;
    });
    points.erase(std::unique(points.begin(), points.end(),
                             [](const SedPoint &a, const SedPoint &b) {
                                 return a.wavelengthUm == b.wavelengthUm;
                             }),
                 points.end());

    if (points.size() < 2) {
        result.errorMessage = QStringLiteral(
                "This source has %1 usable flux measurement(s); a SED fit needs at least 2.")
                                      .arg(points.size());
        return result;
    }

    result.valid = true;
    result.points = std::move(points);
    return result;
}

/// Indices of every entry belonging to the same physical source as ``index``.
/// For a grouped (band-merged) catalogue that is the whole group; otherwise it
/// is the entry itself.
inline std::vector<int> sourceGroupIndices(const std::vector<CatalogueOverlayEntry> &entries,
                                           int index)
{
    std::vector<int> out;
    if (index < 0 || static_cast<std::size_t>(index) >= entries.size())
        return out;
    const QString key = entries[static_cast<std::size_t>(index)].groupKey;
    if (key.isEmpty()) {
        out.push_back(index);
        return out;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].groupKey == key)
            out.push_back(static_cast<int>(i));
    }
    if (out.empty())
        out.push_back(index);
    return out;
}

/// Representative source size (arcsec) for the optically thick fit: the median
/// of the per-band sizes that are known. A single band's beam-dominated size is
/// a poor estimate, and the mean is dragged by the longest wavelength.
inline double representativeSizeArcsec(const std::vector<SedPoint> &points)
{
    std::vector<double> sizes;
    for (const SedPoint &point : points) {
        if (point.use && point.sizeArcsec > 0.0)
            sizes.push_back(point.sizeArcsec);
    }
    if (sizes.empty())
        return 0.0;
    std::sort(sizes.begin(), sizes.end());
    const std::size_t mid = sizes.size() / 2;
    return (sizes.size() % 2 == 0) ? 0.5 * (sizes[mid - 1] + sizes[mid]) : sizes[mid];
}

}   // namespace SedBuilder
}   // namespace visivo

#endif   // SedBuilder_h
