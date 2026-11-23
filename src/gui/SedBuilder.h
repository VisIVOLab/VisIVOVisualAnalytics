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

#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>
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
    /// Included in the fit. Every band contributes one point by default; where
    /// a band has several detections of the same source (see @ref alternative)
    /// only the brightest starts ticked, and the user picks a different one
    /// from the plot or the table.
    bool use{ true };
    /// Another detection of this source in a band that already has one. A
    /// band-merged catalogue is a tree, not a list: one source can match
    /// several counterparts in a band, and the legacy client drew those as
    /// branches of the SED. Keeping them — rather than silently taking the
    /// brightest, which is what discarding them amounts to — is what lets the
    /// user see that the choice exists and change it.
    bool alternative{ false };
    /// The flux and error as the catalogue gave them. @ref fluxJy can move
    /// away from these when the user edits a cell or scales 350/500 µm to the
    /// 250 µm source size, and the table says so against these values.
    double catalogueFluxJy{ 0.0 };
    double catalogueErrorJy{ 0.0 };
    /// Factor applied to the flux and error by the 250 µm size scaling; 1 when
    /// the point is not scaled.
    double sizeScale{ 1.0 };
    /// The user typed this flux or error: the size scaling leaves it alone.
    bool edited{ false };
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
    /// Distance the catalogue gives for the source, in pc, or 0 when it gives
    /// none. Offered to the user, never applied on its own: the VLKB column is
    /// an older set of distances than the current Hi-GAL catalogues.
    double catalogueDistancePc{ 0.0 };
    QString catalogueDistanceColumn;
};

namespace SedBuilder {

/// Wavelengths (µm) of the survey bands whose catalogue columns are named by a
/// number that is not the wavelength. Hi-GAL/band-merged columns are named by
/// wavelength already; MSX 21 µm carries three sub-bands in the legacy view.
inline double bandTagToWavelengthUm(const QString &tag)
{
    static const QMap<QString, double> kSpecial{
        { QStringLiteral("21_a"), 8.28 },
        { QStringLiteral("21_c"), 12.13 },
        { QStringLiteral("21_d"), 14.65 },
        { QStringLiteral("21_e"), 21.34 },
    };
    const QString key = tag.trimmed().toLower();
    if (kSpecial.contains(key))
        return kSpecial.value(key);
    bool ok = false;
    const double value = key.toDouble(&ok);
    return (ok && value > 0.0) ? value : 0.0;
}

/// A flux column whose name does not spell its wavelength. The VLKB SED view
/// carries the photometry of the WISE/2MASS counterpart (flux34 is WISE 3.4 µm,
/// not 34 µm) and of the MSX counterpart (flux21_a…e, four bands) next to the
/// band they were matched in.
struct NamedFluxColumn
{
    const char *column;
    double wavelengthUm;
    const char *tag;
    /// VLKB band whose detection these fluxes belong to.
    const char *ownerBand;
    /// MSX quotes its uncertainties in percent of the flux (MSX6C readme).
    bool percentError;
};

inline const std::vector<NamedFluxColumn> &namedFluxColumns()
{
    static const std::vector<NamedFluxColumn> kColumns{
        { "flux2m_j", 1.235, "2MASS J", "22", false },
        { "flux2m_h", 1.662, "2MASS H", "22", false },
        { "flux2m_k", 2.159, "2MASS Ks", "22", false },
        { "flux34", 3.4, "WISE W1", "22", false },
        { "flux46", 4.6, "WISE W2", "22", false },
        { "flux12", 12.0, "WISE W3", "22", false },
        { "flux21_a", 8.28, "MSX A", "21", true },
        { "flux21_c", 12.13, "MSX C", "21", true },
        { "flux21_d", 14.65, "MSX D", "21", true },
        { "flux21_e", 21.34, "MSX E", "21", true },
    };
    return kColumns;
}

inline const NamedFluxColumn *namedFluxColumn(const QString &name)
{
    const QString key = name.trimmed();
    for (const NamedFluxColumn &column : namedFluxColumns()) {
        if (key.compare(QLatin1String(column.column), Qt::CaseInsensitive) == 0)
            return &column;
    }
    return nullptr;
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
    point.catalogueFluxJy = point.fluxJy;
    point.catalogueErrorJy = point.errorJy;
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
    if (const NamedFluxColumn *named = namedFluxColumn(name))
        return named->wavelengthUm;
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
        const NamedFluxColumn *named = namedFluxColumn(field.first);
        SedPoint point;
        point.wavelengthUm = wavelength;
        point.fluxJy = flux;
        point.band = named ? QString::fromLatin1(named->tag) : QString::number(wavelength);
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
        if (named && named->percentError)
            point.errorJy = flux * point.errorJy / 100.0;
        point.catalogueFluxJy = point.fluxJy;
        point.catalogueErrorJy = point.errorJy;
        point.sizeArcsec = sizeFromFields(entry.extra, QString());
        if (point.sizeArcsec <= 0.0 && entry.sizeArcsec > 0.0)
            point.sizeArcsec = entry.sizeArcsec;
        points.push_back(point);
    }
    return points;
}

/// The counterpart photometry that rides on a VLKB band entry: the WISE and
/// 2MASS fluxes on the 22 µm detection, the MSX A/C/D/E fluxes on the 21 µm
/// one. Read from that entry only, because the same columns are copied onto
/// every band of the row and belong to one counterpart.
inline std::vector<SedPoint> counterpartPoints(const CatalogueOverlayEntry &entry, int entryIndex,
                                               QStringList *skipped)
{
    std::vector<SedPoint> points;
    const QString owner = entry.band.trimmed();
    if (owner.isEmpty())
        return points;
    for (const NamedFluxColumn &column : namedFluxColumns()) {
        if (owner != QLatin1String(column.ownerBand))
            continue;
        const QString name = QString::fromLatin1(column.column);
        QString text;
        if (!fieldValue(entry.extra, { name }, &text))
            continue;
        bool ok = false;
        const double flux = text.trimmed().toDouble(&ok);
        // MSX writes the negative sensitivity limit where a band has no
        // detection: that is a missing band, not a flux.
        if (!ok || !std::isfinite(flux) || flux <= 0.0) {
            if (skipped)
                skipped->append(QString::fromLatin1(column.tag));
            continue;
        }
        SedPoint point;
        point.wavelengthUm = column.wavelengthUm;
        point.fluxJy = flux;
        point.band = QString::fromLatin1(column.tag);
        point.entryIndex = entryIndex;
        numericField(entry.extra, { QStringLiteral("err_%1").arg(name) }, &point.errorJy);
        if (!std::isfinite(point.errorJy) || point.errorJy < 0.0)
            point.errorJy = 0.0;
        if (column.percentError)
            point.errorJy = flux * point.errorJy / 100.0;
        point.catalogueFluxJy = point.fluxJy;
        point.catalogueErrorJy = point.errorJy;
        QString designation;
        if (fieldValue(entry.extra, designationCandidates(owner), &designation))
            point.label = designation.trimmed();
        if (point.label.isEmpty())
            point.label = entry.label;
        points.push_back(point);
    }
    return points;
}

/// Distance of the source in pc from a row's own columns, or 0. Columns that
/// name their unit are read in it. A bare "distance" is read as kpc when it is
/// below 50: the VLKB view declares pc but stores kpc (6.51 for a source at
/// 6.5 kpc), and no Galactic clump sits within 50 pc of the Sun.
inline double distancePcFromFields(const QList<QPair<QString, QString>> &fields,
                                   QString *column = nullptr)
{
    struct Candidate
    {
        const char *name;
        double toPc;
    };
    static const Candidate kCandidates[] = {
        { "distance_pc", 1.0 }, { "dist_pc", 1.0 },  { "distance_kpc", 1e3 },
        { "dist_kpc", 1e3 },    { "d_kpc", 1e3 },    { "distance", 0.0 },
        { "dist", 0.0 },
    };
    for (const Candidate &candidate : kCandidates) {
        double value = 0.0;
        const QString name = QString::fromLatin1(candidate.name);
        if (!numericField(fields, { name }, &value) || value <= 0.0)
            continue;
        if (column)
            *column = name;
        if (candidate.toPc > 0.0)
            return value * candidate.toPc;
        return value < 50.0 ? value * 1e3 : value;
    }
    return 0.0;
}

/// How many distinct bands the SED covers (alternatives of one band count once).
inline std::size_t countUsableBands(const std::vector<SedPoint> &points)
{
    std::size_t bands = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i == 0 || points[i].wavelengthUm != points[i - 1].wavelengthUm)
            ++bands;
    }
    return bands;
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

    // In a band-merged group the photometry lives on the band entries. The
    // merged entry ("ft") carries only copies of columns that belong to one of
    // them, so reading it across as a flat row put the WISE fluxes in a second
    // time, at 34 and 46 µm.
    bool bandTagged = false;
    for (int index : indices) {
        if (index >= 0 && static_cast<std::size_t>(index) < entries.size())
            bandTagged = bandTagged
                    || bandTagToWavelengthUm(entries[static_cast<std::size_t>(index)].band) > 0.0;
    }

    std::vector<SedPoint> points;
    for (int index : indices) {
        if (index < 0 || static_cast<std::size_t>(index) >= entries.size())
            continue;
        const CatalogueOverlayEntry &entry = entries[static_cast<std::size_t>(index)];
        if (result.catalogueDistancePc <= 0.0)
            result.catalogueDistancePc =
                    distancePcFromFields(entry.extra, &result.catalogueDistanceColumn);
        const std::vector<SedPoint> counterparts =
                counterpartPoints(entry, index, &result.skippedBands);
        points.insert(points.end(), counterparts.begin(), counterparts.end());
        SedPoint point;
        if (pointFromEntry(entry, index, &point)) {
            points.push_back(point);
            continue;
        }
        if (!entry.band.trimmed().isEmpty() && bandTagToWavelengthUm(entry.band) > 0.0) {
            // A band we recognise but could not read a flux from: say so.
            if (counterparts.empty())
                result.skippedBands.append(entry.band.trimmed());
            continue;
        }
        if (bandTagged)
            continue;
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
    // Mark the branches instead of erasing them: within one wavelength the
    // brightest is the default and the rest are alternatives the user can pick.
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].wavelengthUm == points[i - 1].wavelengthUm) {
            points[i].alternative = true;
            points[i].use = false;
        }
    }

    if (countUsableBands(points) < 2) {
        result.errorMessage = QStringLiteral(
                "This source has %1 usable flux measurement(s); a SED fit needs at least 2.")
                                      .arg(countUsableBands(points));
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

/// Shortest wavelength in the default fit. Elia et al. (2017, 2021) fit one
/// cold greybody to λ ≥ 160 µm: at 70 µm and shorter the emission comes from
/// warm, internally heated dust that a single temperature does not describe.
constexpr double kDefaultFitMinWavelengthUm = 160.0;

/// Tick the bands at λ ≥ @p minWavelengthUm and untick the rest, keeping one
/// branch per band (the brightest). When that leaves fewer than two detected
/// bands every band is ticked instead, since a fit needs two, and false is
/// returned so the caller can say the range was not applied.
inline bool applyDefaultFitRange(std::vector<SedPoint> &points, double minWavelengthUm)
{
    std::vector<double> inRange;
    for (const SedPoint &point : points) {
        if (point.alternative || point.upperLimit || point.wavelengthUm < minWavelengthUm)
            continue;
        if (std::find(inRange.begin(), inRange.end(), point.wavelengthUm) == inRange.end())
            inRange.push_back(point.wavelengthUm);
    }
    const bool applied = inRange.size() >= 2;
    for (SedPoint &point : points)
        point.use = !point.alternative && (!applied || point.wavelengthUm >= minWavelengthUm);
    return applied;
}

/// SPIRE beams (HPBW, arcsec) used to deconvolve Hi-GAL source sizes, from
/// Giannini et al. (2012), whose method Elia et al. (2017, 2021) follow. 0 for
/// a wavelength without one.
inline double hiGalBeamArcsec(double wavelengthUm)
{
    if (std::abs(wavelengthUm - 250.0) < 1.0)
        return 17.8;
    if (std::abs(wavelengthUm - 350.0) < 1.0)
        return 25.0;
    if (std::abs(wavelengthUm - 500.0) < 1.0)
        return 35.7;
    return 0.0;
}

/// Circularised source size deconvolved by the beam. When the source is not
/// larger than the beam the observed size is returned and @p deconvolved is
/// set false, as Elia et al. (2021) do at 250 µm.
inline double deconvolvedSizeArcsec(double observedArcsec, double beamArcsec, bool *deconvolved)
{
    const bool ok = observedArcsec > beamArcsec && beamArcsec > 0.0;
    if (deconvolved)
        *deconvolved = ok;
    return ok ? std::sqrt(observedArcsec * observedArcsec - beamArcsec * beamArcsec)
              : observedArcsec;
}

struct SizeScaling
{
    bool valid{ false };
    QString errorMessage;
    double referenceSizeArcsec{ 0.0 };   ///< deconvolved 250 µm size
    /// One factor per point; 1 for every point that is not scaled.
    std::vector<double> factors;
    /// Bands whose size could not be deconvolved, scaled with the observed one.
    QStringList observedSizeUsed;
};

/// Factors that bring the 350 and 500 µm fluxes to the 250 µm source size,
/// F'_λ = F_λ · θ_250 / θ_λ with θ the deconvolved circularised size (Elia et
/// al. 2017 §3.1, after Giannini et al. 2012). The longer bands see a larger
/// area of the clump than 250 µm does; scaling makes every band in the fit
/// sample the same gas. The reference is the 250 µm point in use, or the
/// brightest 250 µm branch when none is ticked.
inline SizeScaling sizeScalingTo250(const std::vector<SedPoint> &points)
{
    SizeScaling out;
    out.factors.assign(points.size(), 1.0);
    const SedPoint *reference = nullptr;
    for (const SedPoint &point : points) {
        if (std::abs(point.wavelengthUm - 250.0) >= 1.0 || point.upperLimit
            || point.sizeArcsec <= 0.0)
            continue;
        if (point.use) {
            reference = &point;
            break;
        }
        if (!reference && !point.alternative)
            reference = &point;
    }
    if (!reference) {
        out.errorMessage = QStringLiteral("The source has no 250 µm detection with a size.");
        return out;
    }
    bool referenceDeconvolved = true;
    out.referenceSizeArcsec = deconvolvedSizeArcsec(reference->sizeArcsec, hiGalBeamArcsec(250.0),
                                                    &referenceDeconvolved);
    bool scaledAny = false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const SedPoint &point = points[i];
        const bool target = std::abs(point.wavelengthUm - 350.0) < 1.0
                || std::abs(point.wavelengthUm - 500.0) < 1.0;
        if (!target || point.sizeArcsec <= 0.0)
            continue;
        bool deconvolved = true;
        const double size = deconvolvedSizeArcsec(
                point.sizeArcsec, hiGalBeamArcsec(point.wavelengthUm), &deconvolved);
        // A deconvolved size over an observed one compares a source with its
        // beam image: a 250 µm source just above its beam deconvolves to a few
        // arcsec, and over an observed 24″ that gives a factor near 0.1. When
        // either side cannot be deconvolved both stay observed.
        if (deconvolved && referenceDeconvolved) {
            out.factors[i] = out.referenceSizeArcsec / size;
        } else {
            out.factors[i] = reference->sizeArcsec / point.sizeArcsec;
            if (!referenceDeconvolved)
                out.observedSizeUsed.append(QStringLiteral("250"));
            out.observedSizeUsed.append(QString::number(point.wavelengthUm));
        }
        scaledAny = true;
    }
    if (!scaledAny) {
        out.errorMessage = QStringLiteral("No 350 or 500 µm band with a size to scale.");
        return out;
    }
    out.observedSizeUsed.removeDuplicates();
    out.valid = true;
    return out;
}

/// The observed points the Elia et al. (2017) bolometric luminosity of a
/// protostellar clump integrates: one flux per wavelength from 20 to 160 µm
/// (MSX 21, WISE 22, MIPSGAL 24, PACS 70 and 160), the ticked branch where one
/// is ticked, otherwise the brightest. Empty when there is no 70 µm detection,
/// since then the clump counts as starless and the greybody alone is L_bol.
inline std::vector<std::pair<double, double>> observedLuminosityPoints(
        const std::vector<SedPoint> &points)
{
    std::vector<std::pair<double, double>> out;
    bool has70 = false;
    for (std::size_t first = 0; first < points.size();) {
        std::size_t last = first;
        while (last + 1 < points.size()
               && points[last + 1].wavelengthUm == points[first].wavelengthUm)
            ++last;
        const double wavelength = points[first].wavelengthUm;
        const SedPoint *chosen = nullptr;
        for (std::size_t j = first; j <= last; ++j) {
            if (points[j].upperLimit)
                continue;
            if (points[j].use) {
                chosen = &points[j];
                break;
            }
            if (!chosen && !points[j].alternative)
                chosen = &points[j];
        }
        if (chosen && wavelength >= 20.0 && wavelength <= 160.0) {
            out.emplace_back(wavelength, chosen->fluxJy);
            has70 = has70 || std::abs(wavelength - 70.0) < 1.0;
        }
        first = last + 1;
    }
    if (!has70)
        out.clear();
    return out;
}

/// L_bol in L☉ following Elia et al. (2017): the observed points from
/// @ref observedLuminosityPoints joined by a trapezoid in F_ν against ν, plus
/// the greybody from 160 µm on. The model curve is the backend's sampling
/// (wavelengths ascending, Jy). When the SED has no observed 160 µm flux the
/// trapezoid ends on the model's. Returns 0 when there is nothing to add.
inline double observedPlusGreybodyLuminosityLsun(
        std::vector<std::pair<double, double>> observed,
        const std::vector<double> &modelWavelengthUm, const std::vector<double> &modelFluxJy,
        double distancePc)
{
    constexpr double kSpeedOfLightUmHz = 2.99792458e14;
    constexpr double kParsecM = 3.0856775814913673e16;
    constexpr double kSolarLuminosityW = 3.828e26;
    constexpr double kJansky = 1e-26;
    constexpr double kSplitUm = 160.0;
    constexpr double kPi = 3.14159265358979323846;
    const std::size_t n = std::min(modelWavelengthUm.size(), modelFluxJy.size());
    if (observed.empty() || n < 2 || distancePc <= 0.0 || modelWavelengthUm.front() > kSplitUm
        || modelWavelengthUm[n - 1] <= kSplitUm)
        return 0.0;

    const auto modelAt = [&](double wavelength) {
        for (std::size_t i = 1; i < n; ++i) {
            if (modelWavelengthUm[i] < wavelength)
                continue;
            const double x0 = std::log(modelWavelengthUm[i - 1]);
            const double x1 = std::log(modelWavelengthUm[i]);
            const double y0 = std::log(std::max(modelFluxJy[i - 1], 1e-300));
            const double y1 = std::log(std::max(modelFluxJy[i], 1e-300));
            const double t = x1 > x0 ? (std::log(wavelength) - x0) / (x1 - x0) : 0.0;
            return std::exp(y0 + t * (y1 - y0));
        }
        return 0.0;
    };
    const auto trapezoid = [&](const std::vector<std::pair<double, double>> &curve) {
        double sum = 0.0;
        for (std::size_t i = 1; i < curve.size(); ++i) {
            const double nu0 = kSpeedOfLightUmHz / curve[i - 1].first;
            const double nu1 = kSpeedOfLightUmHz / curve[i].first;
            sum += 0.5 * (curve[i - 1].second + curve[i].second) * (nu0 - nu1);
        }
        return sum;
    };

    std::sort(observed.begin(), observed.end());
    if (observed.back().first < kSplitUm - 1.0)
        observed.emplace_back(kSplitUm, modelAt(kSplitUm));
    std::vector<std::pair<double, double>> tail{ { kSplitUm, modelAt(kSplitUm) } };
    for (std::size_t i = 0; i < n; ++i) {
        if (modelWavelengthUm[i] > kSplitUm)
            tail.emplace_back(modelWavelengthUm[i], modelFluxJy[i]);
    }
    const double fluxWm2 = (trapezoid(observed) + trapezoid(tail)) * kJansky;
    const double distanceM = distancePc * kParsecM;
    return 4.0 * kPi * distanceM * distanceM * fluxWm2 / kSolarLuminosityW;
}

}   // namespace SedBuilder
}   // namespace visivo

#endif   // SedBuilder_h
