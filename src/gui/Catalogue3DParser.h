#ifndef Catalogue3DParser_h
#define Catalogue3DParser_h

#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

struct Catalogue3DFieldInfo
{
    QString name;
    QString type;
};

// How a source's radial distance is derived from its redshift (R-14). The default
// places sources at their real-space best estimate; RedshiftSpace places them at
// the naive Hubble distance cz/H0 (redshift-space). When a source has an
// INDEPENDENT real-space distance, its displacement between the two modes is the
// line-of-sight peculiar velocity (the RSD); for a redshift-only source the two
// modes differ merely by cz/H0 − comoving D_C(z), which is not a peculiar velocity.
enum class CatalogueDistanceMode
{
    Comoving,        // real-space best estimate (measured/model comoving distance)
    RedshiftSpace,   // naive cz/H0 Hubble distance from redshift alone
};

struct Catalogue3DSchema
{
    QStringList headers;
    std::vector<Catalogue3DFieldInfo> fields;
    QString raFieldName;
    QString decFieldName;
    // Cartesian (3-D) catalogues (.speck / XYZ Mpc): positions are these fields,
    // not RA/Dec + distance. When set (and RA/Dec absent) the parser is Cartesian.
    QString xFieldName;
    QString yFieldName;
    QString zFieldName;

    // Runtime display choice (R-14): how redshift → radial distance is derived.
    // Carried on the schema (not a call-site param) so every entryDistanceMpc()
    // consumer — scene, hover readout, table, shells — stays mutually consistent.
    CatalogueDistanceMode distanceMode{ CatalogueDistanceMode::Comoving };

    bool isCartesian() const
    {
        return (raFieldName.isEmpty() || decFieldName.isEmpty())
                && !xFieldName.isEmpty() && !yFieldName.isEmpty() && !zFieldName.isEmpty();
    }

    // O(1) header → column-index lookup (lazily cached); -1 when absent. Lets
    // each Catalogue3DEntry store its row once as an index-ordered QVariantList
    // (see below) instead of a per-row name→value QMap, which cost gigabytes on
    // multi-million-row catalogues. Headers are set once at open and not mutated
    // afterwards, so a build-once flag is a correct (and robust) cache guard —
    // unlike a size check it does not thrash on duplicate header names (where
    // QHash::size() < headers.size()). For duplicate names, the last index wins.
    int indexOf(const QString &name) const
    {
        if (!this->headerIndexBuilt) {
            this->headerIndex.clear();
            this->headerIndex.reserve(this->headers.size());
            for (int i = 0; i < this->headers.size(); ++i)
                this->headerIndex.insert(this->headers.at(i), i);
            this->headerIndexBuilt = true;
        }
        return this->headerIndex.value(name, -1);
    }

private:
    mutable QHash<QString, int> headerIndex;     // built lazily from headers
    mutable bool headerIndexBuilt{ false };
};

struct Catalogue3DEntry
{
    int id{ 0 };
    // Row values in schema.headers order. Index access is direct; name access
    // goes through Catalogue3DSchema::indexOf(). This single compact store
    // replaces the old per-row QMap<QString,QVariant> *and* its duplicate
    // QStringList — together gigabytes of redundant keys/copies on
    // multi-million-row catalogues. QVariant preserves full numeric precision.
    QVariantList fields;
    double raDeg{ 0.0 };
    double decDeg{ 0.0 };
    double sceneX{ 0.0 };
    double sceneY{ 0.0 };
    double sceneZ{ 0.0 };
    // Override distance (Mpc) set by the cosmology selector.  When 0.0, the
    // parser falls back to computing the distance from the catalogue fields.
    double distanceMpc{ 0.0 };
};

struct Catalogue3DParseResult
{
    bool valid{ false };
    QString errorMessage;
    int skippedEntries{ 0 };
    Catalogue3DSchema schema;
    std::vector<Catalogue3DEntry> entries;
};

namespace Catalogue3DParser {

namespace detail {

inline QString normalizeColName(const QString &raw)
{
    QString s;
    s.reserve(raw.size());
    for (const QChar c : raw) {
        if (c.isLetterOrNumber())
            s += c.toUpper();
    }
    return s;
}

inline bool maybeNumeric(const QVariant &raw, double &value)
{
    if (!raw.isValid() || raw.isNull()) {
        return false;
    }
    bool ok = false;
    const double parsed = raw.toDouble(&ok);
    if (!ok || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

inline bool maybeNumeric(const QString &raw, double &value)
{
    return maybeNumeric(QVariant(raw.trimmed()), value);
}

inline QString fieldString(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema,
                           const QString &fieldName)
{
    const int idx = schema.indexOf(fieldName);
    if (idx < 0 || idx >= entry.fields.size())
        return {};
    return entry.fields.at(idx).toString().trimmed();
}

inline double fieldDouble(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema,
                          const QString &fieldName, bool *ok = nullptr)
{
    const int idx = schema.indexOf(fieldName);
    double parsed = 0.0;
    const bool valid = (idx >= 0 && idx < entry.fields.size())
            && maybeNumeric(entry.fields.at(idx), parsed);
    if (ok) {
        *ok = valid;
    }
    return valid ? parsed : 0.0;
}

inline QString firstExistingField(const QMap<QString, QString> &normalizedToActual,
                                  std::initializer_list<const char *> aliases)
{
    for (const char *alias : aliases) {
        const QString key = QString::fromUtf8(alias);
        const auto it = normalizedToActual.find(key);
        if (it != normalizedToActual.end()) {
            return it.value();
        }
    }
    return {};
}

inline double comovingDistanceMpc(double z)
{
    if (z <= 0.0)
        return 0.0;
    // R-12: Planck 2018 (Planck Collaboration VI 2020, TT,TE,EE+lowE+lensing):
    // H0 = 67.66, Ωm = 0.3111, ΩΛ = 1 − Ωm (flat). The UI labels this local
    // integrator "Planck18 (local)", so the constants must actually BE Planck18
    // — they were Planck 2015 (67.74 / 0.3089), ~1% off, which made the label
    // disagree with the astropy-backed Planck18 used for other models.
    constexpr double H0      = 67.66;
    constexpr double cKms    = 2.99792458e5;
    constexpr double DH      = cKms / H0;
    constexpr double Omega_m = 0.3111;
    constexpr double Omega_L = 1.0 - Omega_m;
    constexpr int    N       = 1000;

    auto E_inv = [&](double zp) -> double {
        const double factor = Omega_m * (1.0 + zp) * (1.0 + zp) * (1.0 + zp) + Omega_L;
        return (factor > 0.0) ? 1.0 / std::sqrt(factor) : 0.0;
    };

    const double dz = z / static_cast<double>(N);
    double integral = 0.5 * (E_inv(0.0) + E_inv(z));
    for (int i = 1; i < N; ++i)
        integral += E_inv(dz * static_cast<double>(i));
    integral *= dz;
    return DH * integral;
}

// Naive redshift-space (Hubble-law) distance cz/H0 (R-14): the recession velocity
// v = cz placed at Hubble flow, ignoring peculiar motion and cosmological curvature.
// This is the *redshift-space* radial coordinate. It differs from an INDEPENDENT
// real-space distance by the line-of-sight peculiar velocity; from the comoving
// integral comovingDistanceMpc(z) it differs only by cz/H0 − D_C(z) (the low-z
// linear approximation vs the exact integral), which is not a peculiar velocity.
inline double hubbleDistanceMpc(double z)
{
    if (z <= 0.0)
        return 0.0;
    constexpr double H0   = 67.66;         // must match comovingDistanceMpc (Planck18)
    constexpr double cKms = 2.99792458e5;
    return cKms * z / H0;
}

inline QString detectedNameField(const Catalogue3DSchema &schema)
{
    QMap<QString, QString> normalizedToActual;
    for (const auto &field : schema.fields) {
        normalizedToActual[normalizeColName(field.name)] = field.name;
    }
    return firstExistingField(normalizedToActual,
                              { "NAME", "ID", "LABEL", "SOURCENAME", "SOURCE", "OBJNAME",
                                "CLUSTERS", "CLUSTER", "OBJECT" });
}

inline QString detectedMorphologyField(const Catalogue3DSchema &schema)
{
    QMap<QString, QString> normalizedToActual;
    for (const auto &field : schema.fields) {
        normalizedToActual[normalizeColName(field.name)] = field.name;
    }
    return firstExistingField(normalizedToActual,
                              { "MORPHOLOGY", "TYPE", "CLASS", "MORPH", "CLASSIFICATION",
                                "DIFFUSETYPE", "DIFFMORPHFINAL", "DIFFUSEMORPHOLOGY",
                                "RADIOMORPHOLOGY", "RADIOMORPHOLOGYLITERATURE" });
}

inline QString detectedRedshiftField(const Catalogue3DSchema &schema)
{
    if (schema.isCartesian())   // no redshift column — positions are explicit X/Y/Z
        return {};
    QMap<QString, QString> normalizedToActual;
    for (const auto &field : schema.fields) {
        normalizedToActual[normalizeColName(field.name)] = field.name;
    }
    return firstExistingField(normalizedToActual,
                              { "Z", "REDSHIFT", "REDSHIFTZ", "ZSPEC", "ZMEAN", "ZPHOT",
                                "ZPHOTOZ" });
}

inline QString detectedDistanceField(const Catalogue3DSchema &schema)
{
    QMap<QString, QString> normalizedToActual;
    for (const auto &field : schema.fields) {
        normalizedToActual[normalizeColName(field.name)] = field.name;
    }
    return firstExistingField(normalizedToActual,
                              { "COMOVINGDISTANCE", "COMOVINGDIST", "DC", "DCMPC",
                                "DISTANCE", "DIST", "DMPC", "DISTMPC",
                                "DL", "DLMPC", "LUMINOSITYDISTANCE",
                                "DA", "DAMPC", "ANGULARDIAMETERDISTANCE" });
}

// The KIND of a detected distance column, so a luminosity/angular-diameter
// distance can be converted to the COMOVING radial coordinate the scene uses.
enum class DistanceKind { Comoving, Luminosity, AngularDiameter };

inline DistanceKind distanceFieldKind(const QString &fieldName)
{
    const QString n = normalizeColName(fieldName);
    if (n.startsWith(QStringLiteral("DL")) || n == QStringLiteral("LUMINOSITYDISTANCE"))
        return DistanceKind::Luminosity;
    if (n.startsWith(QStringLiteral("DA")) || n == QStringLiteral("ANGULARDIAMETERDISTANCE"))
        return DistanceKind::AngularDiameter;
    // COMOVINGDISTANCE/DC and the generic DISTANCE/DIST are treated as comoving.
    return DistanceKind::Comoving;
}

inline QString detectedMajorAxisField(const Catalogue3DSchema &schema)
{
    QMap<QString, QString> normalizedToActual;
    for (const auto &field : schema.fields) {
        normalizedToActual[normalizeColName(field.name)] = field.name;
    }
    return firstExistingField(normalizedToActual,
                              { "MAJORAXIS", "MAJAXIS", "BMAJ", "THETAMAJ",
                                "MAJORAXISARCMIN" });
}

inline QString detectedMinorAxisField(const Catalogue3DSchema &schema)
{
    QMap<QString, QString> normalizedToActual;
    for (const auto &field : schema.fields) {
        normalizedToActual[normalizeColName(field.name)] = field.name;
    }
    return firstExistingField(normalizedToActual,
                              { "MINORAXIS", "MINAXIS", "BMIN", "THETAMIN",
                                "MINORAXISARCMIN" });
}

inline QString detectedFluxField(const Catalogue3DSchema &schema)
{
    QMap<QString, QString> normalizedToActual;
    for (const auto &field : schema.fields) {
        normalizedToActual[normalizeColName(field.name)] = field.name;
    }
    return firstExistingField(normalizedToActual,
                              { "FLUX", "FLUXDENSITY", "S", "S14", "SMJY", "FLUXMJY",
                                "DIFFUSEFLUXDENSITY", "DIFFUSEFLUXDENSITYS128GHZMJY",
                                "FLUXDENSITYS128GHZ", "FLUXDENSITYS128GHZTOTALMJY" });
}

inline QString detectedLlsField(const Catalogue3DSchema &schema)
{
    QMap<QString, QString> normalizedToActual;
    for (const auto &field : schema.fields) {
        normalizedToActual[normalizeColName(field.name)] = field.name;
    }
    return firstExistingField(normalizedToActual,
                              { "LLS", "LLSKPC", "SIZE", "SIZELLSAXISMINXMAJKPCXKPC",
                                "SIZEFULLEXTENT", "LARGESTLINEARSIZE" });
}

inline void parseSizePair(const QString &raw, double &minorValue, double &majorValue)
{
    minorValue = 0.0;
    majorValue = 0.0;
    const QRegularExpression re(QStringLiteral("([+-]?\\d+(?:\\.\\d+)?)\\s*x\\s*([+-]?\\d+(?:\\.\\d+)?)"),
                                QRegularExpression::CaseInsensitiveOption);
    const auto match = re.match(raw);
    if (!match.hasMatch()) {
        return;
    }
    bool okMinor = false;
    bool okMajor = false;
    const double first = match.captured(1).toDouble(&okMinor);
    const double second = match.captured(2).toDouble(&okMajor);
    if (okMinor && okMajor) {
        minorValue = std::min(first, second);
        majorValue = std::max(first, second);
    }
}

inline double entryDistanceMpc(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema)
{
    // Cartesian catalogues: distance = radial distance of the explicit position
    // from the origin (observer). No redshift/cosmology involved.
    if (schema.isCartesian()) {
        return std::sqrt(entry.sceneX * entry.sceneX + entry.sceneY * entry.sceneY
                         + entry.sceneZ * entry.sceneZ);
    }
    // Redshift (used for the redshift-space mode, the distance-TYPE conversion, and
    // as a fallback).
    const QString redshiftField = detectedRedshiftField(schema);
    bool okRedshift = false;
    const double redshift = fieldDouble(entry, schema, redshiftField, &okRedshift);
    // Redshift-space mode (R-14): place the source at the naive Hubble distance
    // cz/H0 from its redshift alone — deliberately ignoring measured/model
    // (real-space) distances and any cosmology-selector override. Against a source
    // that also has an independent real-space distance, the mode toggle exposes its
    // line-of-sight peculiar-velocity displacement. Falls through to the real-space
    // logic when the source has no usable redshift.
    if (schema.distanceMode == CatalogueDistanceMode::RedshiftSpace
            && okRedshift && redshift > 0.0)
        return hubbleDistanceMpc(redshift);
    // Prefer the cosmology-selector override if it has been set.
    if (entry.distanceMpc > 0.0)
        return entry.distanceMpc;
    const QString distanceField = detectedDistanceField(schema);
    bool okDistance = false;
    const double distance = fieldDouble(entry, schema, distanceField, &okDistance);
    if (okDistance && distance > 0.0) {
        // Convert a luminosity/angular-diameter distance to the COMOVING radial
        // coordinate the scene places sources at (D_L = (1+z)·D_C ⇒ D_C = D_L/(1+z);
        // D_A = D_C/(1+z) ⇒ D_C = D_A·(1+z)). Without a redshift we can't convert,
        // so the value is used as-is (the best available). A bare DISTANCE column
        // is assumed comoving.
        const DistanceKind kind = distanceFieldKind(distanceField);
        if (kind != DistanceKind::Comoving && okRedshift && redshift > 0.0) {
            if (kind == DistanceKind::Luminosity)
                return distance / (1.0 + redshift);
            return distance * (1.0 + redshift);   // AngularDiameter → comoving
        }
        return distance;
    }
    if (okRedshift && redshift > 0.0) {
        return comovingDistanceMpc(redshift);
    }
    return 300.0;
}

inline QString entryDisplayName(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema)
{
    const QString nameField = detectedNameField(schema);
    const QString name = fieldString(entry, schema, nameField);
    return name.isEmpty() ? QStringLiteral("Source %1").arg(entry.id) : name;
}

inline QString entryMorphology(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema)
{
    const QString field = detectedMorphologyField(schema);
    const QString morph = fieldString(entry, schema, field).toLower().trimmed();
    return morph.isEmpty() ? QStringLiteral("other") : morph;
}

inline double entryMajorAxisArcmin(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema)
{
    return fieldDouble(entry, schema, detectedMajorAxisField(schema));
}

inline double entryMinorAxisArcmin(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema)
{
    return fieldDouble(entry, schema, detectedMinorAxisField(schema));
}

inline double entryFluxMJy(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema)
{
    return fieldDouble(entry, schema, detectedFluxField(schema));
}

inline double entryLlsMajorKpc(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema)
{
    const QString llsField = detectedLlsField(schema);
    double minor = 0.0;
    double major = 0.0;
    parseSizePair(fieldString(entry, schema, llsField), minor, major);
    return major;
}

inline double entryLlsMinorKpc(const Catalogue3DEntry &entry, const Catalogue3DSchema &schema)
{
    const QString llsField = detectedLlsField(schema);
    double minor = 0.0;
    double major = 0.0;
    parseSizePair(fieldString(entry, schema, llsField), minor, major);
    return minor;
}

inline void fillCartesian(Catalogue3DEntry &e, const Catalogue3DSchema &schema)
{
    const double d = entryDistanceMpc(e, schema);
    constexpr double pi = 3.14159265358979323846;
    const double ra  = e.raDeg * pi / 180.0;
    const double dec = e.decDeg * pi / 180.0;
    e.sceneX = d * std::cos(dec) * std::cos(ra);
    e.sceneY = d * std::cos(dec) * std::sin(ra);
    e.sceneZ = d * std::sin(dec);
}

} // namespace detail

inline Catalogue3DParseResult parseBackendSubset(const Catalogue3DSchema &schema,
                                                 const QList<QVariantMap> &rows)
{
    Catalogue3DParseResult result;
    result.schema = schema;
    const bool cartesian = schema.isCartesian();
    if (!cartesian && (schema.raFieldName.isEmpty() || schema.decFieldName.isEmpty())) {
        result.errorMessage = QStringLiteral("Backend catalogue schema does not provide RA/Dec or X/Y/Z coordinate fields.");
        return result;
    }

    int entryId = 0;
    for (const QVariantMap &row : rows) {
        Catalogue3DEntry entry;

        if (cartesian) {
            bool okX = false, okY = false, okZ = false;
            const double x = row.value(schema.xFieldName).toDouble(&okX);
            const double y = row.value(schema.yFieldName).toDouble(&okY);
            const double z = row.value(schema.zFieldName).toDouble(&okZ);
            if (!okX || !okY || !okZ || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                ++result.skippedEntries;
                continue;
            }
            // Positions ARE the X/Y/Z fields — feed them straight into the scene
            // so the default "computed:x/y/z" axis mapping renders them directly.
            entry.sceneX = x;
            entry.sceneY = y;
            entry.sceneZ = z;
        } else {
            bool okRa = false, okDec = false;
            const double raDeg = row.value(schema.raFieldName).toDouble(&okRa);
            const double decDeg = row.value(schema.decFieldName).toDouble(&okDec);
            if (!okRa || !okDec || !std::isfinite(raDeg) || !std::isfinite(decDeg)) {
                ++result.skippedEntries;
                continue;
            }
            entry.raDeg = raDeg;
            entry.decDeg = decDeg;
        }

        entry.id = entryId++;
        // Store the row once, in schema.headers order (index access; name
        // access via schema.indexOf()). QVariant keeps full numeric precision.
        entry.fields.reserve(schema.headers.size());
        for (const QString &header : schema.headers) {
            entry.fields.append(row.value(header));
        }
        if (!cartesian) {
            detail::fillCartesian(entry, schema);
        }
        result.entries.push_back(std::move(entry));
    }

    if (result.entries.empty()) {
        result.errorMessage = cartesian
                ? QStringLiteral("No valid X/Y/Z entries were returned by the backend subset.")
                : QStringLiteral("No valid RA/Dec entries were returned by the backend subset.");
        return result;
    }

    result.valid = true;
    return result;
}

} // namespace Catalogue3DParser

#endif // Catalogue3DParser_h
