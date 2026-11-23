#ifndef PARTICLE_TABLE_RULE_H
#define PARTICLE_TABLE_RULE_H
// ParticleTableRule.h
// Whether a plain table (IPAC, CSV, FITS table) holds a simulation's particles
// rather than a catalogue of objects (issue #414, item 6). Gadget snapshots
// and VBT tables say so themselves; a text table only has its column names
// and its size. A particle table opened as a catalogue showed
// "CAT · N sources" and offered ellipsoids for millions of rows.
//
// The rule: x, y and z columns, at least one column only gas particles have
// (smoothing length, density, internal energy, temperature, also as their
// logarithms; a mass alone is also a halo or galaxy catalogue's), no sky
// position and no redshift (a galaxy catalogue with comoving x, y, z keeps its
// redshift), and at least kMinRows rows. The viewer can override it,
// remembered per file.

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace visivo::particles {

/// A table below this many rows is drawn well as a catalogue: the demo
/// particle tables have 3.7-3.9 million rows, galaxy and halo catalogues
/// with x, y, z are mostly well below a million.
inline constexpr qint64 kMinRows = 500000;

namespace detail {

/// A column's name without its unit ("rho [g/cm3]", "Mass(Msun)") and with
/// letters and digits only, lower case: "log-T" and "LogT" are one name.
inline QString key(const QString &column)
{
    QString name = column.trimmed();
    const qsizetype unit = name.indexOf(QRegularExpression(QStringLiteral("[\\[(]")));
    if (unit > 0)
        name.truncate(unit);
    QString k;
    for (const QChar c : name)
        if (c.isLetterOrNumber())
            k += c.toLower();
    return k;
}

inline bool hasAny(const QStringList &keys, std::initializer_list<const char *> names)
{
    for (const char *name : names)
        if (keys.contains(QLatin1String(name)))
            return true;
    return false;
}

} // namespace detail

/// Whether the columns @p columns of a table of @p rows rows look like a
/// simulation's particles.
inline bool looksLikeParticles(const QStringList &columns, qint64 rows)
{
    if (rows < kMinRows)
        return false;
    QStringList keys;
    keys.reserve(columns.size());
    for (const QString &c : columns)
        keys << detail::key(c);
    const auto triple = [&keys](const char *x, const char *y, const char *z) {
        return keys.contains(QLatin1String(x)) && keys.contains(QLatin1String(y))
                && keys.contains(QLatin1String(z));
    };
    const bool position = triple("x", "y", "z") || triple("posx", "posy", "posz")
            || triple("coordinatesx", "coordinatesy", "coordinatesz")
            || triple("coordinates0", "coordinates1", "coordinates2");
    if (!position)
        return false;
    // Positions on the sky or a redshift: a catalogue of objects, whatever
    // else it holds ("z" alone is the third coordinate here).
    if (detail::hasAny(keys, { "ra", "dec", "raj2000", "dej2000", "decj2000", "glon", "glat",
                               "elon", "elat", "redshift", "zspec", "zphot", "zcmb", "zhelio",
                               "zobs", "zcos", "cz", "logredshift" }))
        return false;
    return detail::hasAny(keys, { "hsml", "smoothinglength", "loghsml",
                                  "rho", "density", "logd", "logrho", "logdensity", "logdens",
                                  "internalenergy", "logu",
                                  "temperature", "temp", "logt", "logtemp", "logtemperature" });
}

/// What the viewer was told for a file: decide by the rule, or the user's
/// choice either way.
enum class Choice { Auto, Particles, Objects };

inline QString settingsKey(const QString &path)
{
    const QByteArray digest =
            QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("particleTables/") + QString::fromLatin1(digest);
}

inline Choice rememberedChoice(QSettings &settings, const QString &path)
{
    const QString value = settings.value(settingsKey(path)).toString();
    if (value == QLatin1String("particles"))
        return Choice::Particles;
    if (value == QLatin1String("objects"))
        return Choice::Objects;
    return Choice::Auto;
}

inline void rememberChoice(QSettings &settings, const QString &path, Choice choice)
{
    if (choice == Choice::Auto)
        settings.remove(settingsKey(path));
    else
        settings.setValue(settingsKey(path), choice == Choice::Particles
                                                     ? QStringLiteral("particles")
                                                     : QStringLiteral("objects"));
}

/// The rule with the user's choice on top.
inline bool treatAsParticles(Choice choice, const QStringList &columns, qint64 rows)
{
    if (choice == Choice::Particles)
        return true;
    if (choice == Choice::Objects)
        return false;
    return looksLikeParticles(columns, rows);
}

} // namespace visivo::particles

#endif // PARTICLE_TABLE_RULE_H
