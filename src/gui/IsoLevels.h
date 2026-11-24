#pragma once

// IsoLevels.h
// The levels of the 3-D isosurface (issue #526): how many, where (linear or
// log spaced between two values, or a typed list), and each one's colour and
// opacity. The numbers match the backend's (backend/app/iso_levels.py), which
// is asked for the same levels.
//
// No widgets, so it can be tested.

#include <QColor>
#include <QLocale>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace visivo::isolevels {

/// At most this many surfaces: more nested translucent shells stop reading as
/// separate surfaces (the backend refuses more).
constexpr int kMaxLevels = 8;
/// The outermost (lowest) surface's opacity by default; the innermost is opaque.
constexpr double kOuterOpacity = 0.3;

enum class Spacing { Linear, Log };

/// One surface: its value, its colour and its opacity.
struct Level
{
    double value{ 0.0 };
    QColor colour{ 255, 128, 255 };
    double opacity{ 1.0 };
};

/// `count` values from `lo` to `hi` inclusive, evenly spaced in the values or
/// (Log, both above zero) in their logarithm; one value is the middle of the
/// range. Empty, with `error` set, when the request cannot be met.
inline QVector<double> spaced(int count, double lo, double hi, Spacing spacing,
                              QString *error = nullptr)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return QVector<double>{};
    };
    if (count < 1 || count > kMaxLevels)
        return fail(QStringLiteral("Between 1 and %1 levels.").arg(kMaxLevels));
    if (!std::isfinite(lo) || !std::isfinite(hi))
        return fail(QStringLiteral("Min and max must be numbers."));
    if (hi < lo)
        std::swap(lo, hi);
    if (spacing == Spacing::Log && lo <= 0.0)
        return fail(QStringLiteral("Log spacing needs min and max above zero."));
    QVector<double> out;
    if (count == 1) {
        out << (spacing == Spacing::Log ? std::sqrt(lo * hi) : 0.5 * (lo + hi));
        return out;
    }
    for (int i = 0; i < count; ++i) {
        const double u = static_cast<double>(i) / (count - 1);
        out << (spacing == Spacing::Log ? lo * std::pow(hi / lo, u) : lo + u * (hi - lo));
    }
    out.first() = lo;   // the ends exactly, not a rounded power
    out.last() = hi;
    return out;
}

/// The values typed into a list, separated by commas, semicolons or white
/// space: ascending, without duplicates. Empty, with `error` set, when a word
/// is not a number, none is given, or there are more than kMaxLevels.
inline QVector<double> parseList(const QString &text, QString *error = nullptr)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return QVector<double>{};
    };
    QVector<double> out;
    const QStringList words = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")),
                                         Qt::SkipEmptyParts);
    for (const QString &word : words) {
        bool ok = false;
        const double v = QLocale::c().toDouble(word, &ok);
        if (!ok || !std::isfinite(v))
            return fail(QStringLiteral("\"%1\" is not a number.").arg(word));
        out << v;
    }
    if (out.isEmpty())
        return fail(QStringLiteral("Type at least one level."));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.size() > kMaxLevels)
        return fail(QStringLiteral("At most %1 levels.").arg(kMaxLevels));
    return out;
}

/// The opacity of the level of rank `rank` among `count` (0 the lowest value,
/// the outermost surface of an emission peak): the highest is opaque, the
/// lowest kOuterOpacity, evenly between. One level is opaque.
inline double defaultOpacity(int rank, int count)
{
    if (count <= 1)
        return 1.0;
    const double u = std::clamp(static_cast<double>(rank) / (count - 1), 0.0, 1.0);
    return kOuterOpacity + u * (1.0 - kOuterOpacity);
}

/// Levels at `values` (ascending) with the default opacities and the colours
/// `colourAt` gives (the colour map at that value).
inline QVector<Level> makeLevels(const QVector<double> &values,
                                 const std::function<QColor(double)> &colourAt)
{
    QVector<Level> out;
    for (int i = 0; i < values.size(); ++i) {
        Level l;
        l.value = values[i];
        l.colour = colourAt ? colourAt(values[i]) : QColor(255, 128, 255);
        l.opacity = defaultOpacity(i, values.size());
        out << l;
    }
    return out;
}

/// `fresh` (made by makeLevels), with the colour of any level of `previous` at
/// the same value kept: regenerating levels does not undo the colours the user
/// chose. Opacities are not kept: they follow the new set's own gradient, which
/// a level shared with the old set would otherwise break.
inline QVector<Level> keepEdits(QVector<Level> fresh, const QVector<Level> &previous)
{
    for (Level &level : fresh) {
        for (const Level &old : previous) {
            const double scale = std::max({ std::abs(level.value), std::abs(old.value), 1e-300 });
            if (std::abs(level.value - old.value) <= 1e-9 * scale) {
                level.colour = old.colour;
                break;
            }
        }
    }
    return fresh;
}

/// The values of `levels`.
inline QVector<double> values(const QVector<Level> &levels)
{
    QVector<double> out;
    for (const Level &l : levels)
        out << l.value;
    return out;
}

/// The legend's label of a level: its value to four significant digits, in
/// exponents when it needs them, with the unit when there is one.
inline QString label(double value, const QString &unit = {})
{
    QString text = QString::number(value, 'g', 4);
    return unit.isEmpty() ? text : text + QLatin1Char(' ') + unit;
}

/// A starting range for the editor: from `low` (the current threshold) up to
/// 90 % of `high` (the data's maximum), Log when that spans two decades or
/// more (a simulation's density).
struct Range
{
    double min{ 0.0 };
    double max{ 1.0 };
    Spacing spacing{ Spacing::Linear };
};

inline Range startingRange(double low, double high)
{
    Range r;
    if (!std::isfinite(high) || high <= 0.0) {
        r.min = std::isfinite(low) ? low : 0.0;
        r.max = r.min + 1.0;
        return r;
    }
    if (!std::isfinite(low) || low >= high)
        low = high * 0.1;
    r.min = low;
    r.max = 0.9 * high;   // the data's own maximum is a point, not a surface
    r.spacing = (low > 0.0 && high / low >= 100.0) ? Spacing::Log : Spacing::Linear;
    return r;
}

} // namespace visivo::isolevels
