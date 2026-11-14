#ifndef FitsSpectrumTable_h
#define FitsSpectrumTable_h

// FitsSpectrumTable.h
// A spectrum saved as a FITS binary table, read into an axis and its values.
//
// The backend's POST /v1/products/spectrum_table writes it with format=fits
// (issue #245): a SPECTRUM extension with the columns CHANNEL (J), SPECTRAL
// (D, TUNIT the spectral unit) and VALUE (D, TUNIT the cube's BUNIT), and
// SPECTYPE, XPIX, YPIX, RADIUS and NPIX in its header. It ends up in the same
// spectrum product as the CSV form, with the same axis labels.
//
// Header-only and Qt Core only (no cfitsio), so the test suite, which links
// neither cfitsio nor widgets, reads the same bytes the app does. The cards
// are split by FitsHeaderCard.h, the header editor's own parser. Only what a
// spectrum table needs is read: uncompressed, scalar numeric columns.

#include "gui/FitsHeaderCard.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace visivo {

namespace fitsspectrum_detail {

constexpr qint64 kBlock = 2880;
constexpr qint64 kCard = 80;

inline qint64 roundUpToBlock(qint64 n)
{
    return (n + kBlock - 1) / kBlock * kBlock;
}

/// One HDU's header: its cards by keyword (the first of each), and where its
/// data starts. False when there is no END before the bytes run out.
inline bool readHeader(const QByteArray &file, qint64 at, QHash<QString, QString> &cards,
                       qint64 &dataAt)
{
    cards.clear();
    for (qint64 pos = at; pos + kCard <= file.size(); pos += kCard) {
        const FitsHeaderCard card =
                parseFitsHeaderCard(QString::fromLatin1(file.constData() + pos, kCard));
        if (card.key == QStringLiteral("END")) {
            dataAt = at + roundUpToBlock(pos + kCard - at);
            return true;
        }
        if (!card.commentary && !cards.contains(card.key))
            cards.insert(card.key, card.value);
    }
    return false;
}

inline qint64 intCard(const QHash<QString, QString> &cards, const QString &key, qint64 fallback,
                      bool *ok = nullptr)
{
    bool good = false;
    const qint64 v = cards.value(key).toLongLong(&good);
    if (ok)
        *ok = good;
    return good ? v : fallback;
}

/// @p a times @p b, or -1 when it is negative or more than @p limit. Every
/// size is checked against the file's own before it is used, so no product
/// or sum of a header's numbers can overflow.
inline qint64 productWithin(qint64 a, qint64 b, qint64 limit)
{
    if (a < 0 || b < 0)
        return -1;
    if (a == 0 || b == 0)
        return 0;
    return a > limit / b ? -1 : a * b;
}

/// The bytes of an HDU's data (before the padding to a whole block), or -1
/// when the header does not say or they cannot fit in @p limit bytes.
inline qint64 dataBytes(const QHash<QString, QString> &cards, qint64 limit)
{
    bool ok = false;
    const qint64 bitpix = intCard(cards, QStringLiteral("BITPIX"), 0, &ok);
    if (!ok || !(bitpix == 8 || bitpix == 16 || bitpix == 32 || bitpix == 64 || bitpix == -32
                 || bitpix == -64))
        return -1;
    const qint64 naxis = intCard(cards, QStringLiteral("NAXIS"), -1);
    if (naxis < 0 || naxis > 999)
        return -1;
    if (naxis == 0)
        return 0;
    qint64 count = 1;
    for (qint64 i = 1; i <= naxis; ++i) {
        count = productWithin(count, intCard(cards, QStringLiteral("NAXIS%1").arg(i), -1), limit);
        if (count < 0)
            return -1;
    }
    const qint64 pcount = intCard(cards, QStringLiteral("PCOUNT"), 0);
    const qint64 gcount = intCard(cards, QStringLiteral("GCOUNT"), 1);
    if (pcount < 0 || pcount > limit)
        return -1;
    const qint64 total = productWithin(count + pcount, gcount, limit);
    return total < 0 ? -1 : productWithin(std::abs(bitpix) / 8, total, limit);
}

struct Column
{
    QString name;
    QString unit;
    char type{ 0 };
    qint64 repeat{ 1 };
    qint64 offset{ 0 };   ///< byte offset within a row
    double scale{ 1.0 };
    double zero{ 0.0 };
    bool hasNull{ false };
    qint64 null{ 0 };
    /// A single number this reader can decode.
    bool numeric() const
    {
        return repeat == 1
                && (type == 'B' || type == 'I' || type == 'J' || type == 'K' || type == 'E'
                    || type == 'D');
    }
};

/// Bytes of one cell of TFORM type @p type repeated @p repeat times; -1 unknown.
inline qint64 cellBytes(char type, qint64 repeat)
{
    switch (type) {
    case 'L': case 'B': case 'A': return repeat;
    case 'X': return (repeat + 7) / 8;
    case 'I': return 2 * repeat;
    case 'J': case 'E': return 4 * repeat;
    case 'K': case 'D': case 'C': case 'P': return 8 * repeat;
    case 'M': case 'Q': return 16 * repeat;
    default: return -1;
    }
}

inline double cellValue(const char *p, const Column &c)
{
    double raw = 0.0;
    qint64 integer = 0;
    bool isInteger = true;
    switch (c.type) {
    case 'B': integer = static_cast<unsigned char>(*p); break;
    case 'I': integer = qFromBigEndian<qint16>(p); break;
    case 'J': integer = qFromBigEndian<qint32>(p); break;
    case 'K': integer = qFromBigEndian<qint64>(p); break;
    case 'E': {
        const quint32 bits = qFromBigEndian<quint32>(p);
        float f;
        std::memcpy(&f, &bits, sizeof f);
        raw = f;
        isInteger = false;
        break;
    }
    case 'D': {
        const quint64 bits = qFromBigEndian<quint64>(p);
        std::memcpy(&raw, &bits, sizeof raw);
        isInteger = false;
        break;
    }
    default: return std::nan("");
    }
    if (isInteger) {
        // TNULL is the stored integer, before TSCAL and TZERO.
        if (c.hasNull && integer == c.null)
            return std::nan("");
        raw = static_cast<double>(integer);
    }
    return c.zero + c.scale * raw;
}

/// "name (unit)", or whichever of the two there is.
inline QString labelWithUnit(const QString &name, const QString &unit)
{
    if (!name.isEmpty() && !unit.isEmpty())
        return QStringLiteral("%1 (%2)").arg(name, unit);
    return name.isEmpty() ? unit : name;
}

}   // namespace fitsspectrum_detail

/// Whether @p bytes are a FITS file (a primary header starts them).
inline bool looksLikeFits(const QByteArray &bytes)
{
    return bytes.startsWith("SIMPLE  =");
}

/// The first binary table of @p file with a spectrum in it: SPECTRAL and VALUE
/// by name, else its first two numeric columns. @p xLabel and @p yLabel are
/// the CSV export's: "<SPECTYPE> (<unit>)" and "Intensity (<BUNIT>)", "Mean
/// intensity" for an aperture (RADIUS > 0); any other table gets its column
/// names. Blank values (NaN, an integer's TNULL) come back as NaN. False,
/// with @p error, when there is no such table or it has fewer than two rows.
inline bool readFitsSpectrumTable(const QByteArray &file, QVector<double> &axis,
                                  QVector<double> &values, QString *xLabel = nullptr,
                                  QString *yLabel = nullptr, QString *error = nullptr)
{
    using namespace fitsspectrum_detail;
    axis.clear();
    values.clear();
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (!looksLikeFits(file))
        return fail(QStringLiteral("not a FITS file"));

    QHash<QString, QString> cards;
    qint64 at = 0;
    while (at < file.size()) {
        qint64 dataAt = 0;
        if (!readHeader(file, at, cards, dataAt))
            return fail(QStringLiteral("a FITS header has no END"));
        // What is left of the file: no HDU's data can be larger.
        const qint64 left = std::max<qint64>(0, file.size() - dataAt);
        const qint64 bytes = dataBytes(cards, left);
        if (bytes < 0)
            return fail(QStringLiteral("a FITS header has no valid data size, or the file is "
                                       "cut short"));
        const qint64 next = dataAt + roundUpToBlock(bytes);
        if (cards.value(QStringLiteral("XTENSION")) != QStringLiteral("BINTABLE")) {
            at = next;
            continue;
        }

        // dataBytes() has checked NAXIS1 * NAXIS2 against what is left.
        const qint64 rowBytes = intCard(cards, QStringLiteral("NAXIS1"), -1);
        const qint64 rows = intCard(cards, QStringLiteral("NAXIS2"), -1);
        const qint64 fields = intCard(cards, QStringLiteral("TFIELDS"), -1);
        if (intCard(cards, QStringLiteral("NAXIS"), -1) != 2
            || intCard(cards, QStringLiteral("BITPIX"), 0) != 8 || rowBytes <= 0 || rows < 0
            || rows > left / rowBytes || fields <= 0 || fields > 999)
            return fail(QStringLiteral("the FITS table is malformed or cut short"));

        QVector<Column> columns;
        qint64 offset = 0;
        bool layoutOk = true;
        for (qint64 i = 1; i <= fields; ++i) {
            const QString n = QString::number(i);
            Column c;
            c.name = cards.value(QStringLiteral("TTYPE") + n);
            c.unit = cards.value(QStringLiteral("TUNIT") + n);
            const QString form = cards.value(QStringLiteral("TFORM") + n).trimmed().toUpper();
            int digits = 0;
            while (digits < form.size() && form.at(digits).isDigit())
                ++digits;
            if (digits >= form.size()) {
                layoutOk = false;
                break;
            }
            bool repeatOk = true;
            c.repeat = digits > 0 ? form.left(digits).toLongLong(&repeatOk) : 1;
            c.type = form.at(digits).toLatin1();
            // No wider than the row, so cellBytes() cannot overflow either.
            const qint64 width = repeatOk && c.repeat <= rowBytes ? cellBytes(c.type, c.repeat) : -1;
            if (width < 0 || width > rowBytes - offset) {
                layoutOk = false;
                break;
            }
            c.offset = offset;
            offset += width;
            bool ok = false;
            const double scale = cards.value(QStringLiteral("TSCAL") + n).toDouble(&ok);
            if (ok)
                c.scale = scale;
            const double zero = cards.value(QStringLiteral("TZERO") + n).toDouble(&ok);
            if (ok)
                c.zero = zero;
            c.null = intCard(cards, QStringLiteral("TNULL") + n, 0, &c.hasNull);
            columns.append(c);
        }
        if (!layoutOk) {
            at = next;
            continue;
        }

        const auto byName = [&columns](const char *name) -> const Column * {
            for (const Column &c : columns)
                if (c.numeric() && c.name.trimmed().compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
                    return &c;
            return nullptr;
        };
        const Column *x = byName("SPECTRAL");
        const Column *y = byName("VALUE");
        const bool backendTable = x && y && x != y;
        if (!backendTable) {
            // Not ours: its first two numeric columns, as the CSV reader does.
            QVector<const Column *> numeric;
            for (const Column &c : columns)
                if (c.numeric())
                    numeric.append(&c);
            if (numeric.size() < 2) {
                at = next;
                continue;
            }
            x = numeric.at(0);
            y = numeric.at(1);
        }

        axis.reserve(rows);
        values.reserve(rows);
        for (qint64 r = 0; r < rows; ++r) {
            const char *row = file.constData() + dataAt + r * rowBytes;
            axis.append(cellValue(row + x->offset, *x));
            values.append(cellValue(row + y->offset, *y));
        }

        const QString spectype = cards.value(QStringLiteral("SPECTYPE"));
        if (backendTable && cards.contains(QStringLiteral("SPECTYPE"))) {
            // The CSV form's labels (worker_spectrum_table). Not on a table
            // read by its first two columns: SPECTYPE names SPECTRAL only.
            if (xLabel) {
                *xLabel = labelWithUnit(spectype, x->unit);
                if (xLabel->isEmpty())
                    *xLabel = QStringLiteral("Channel");
            }
            if (yLabel) {
                const bool aperture =
                        cards.value(QStringLiteral("RADIUS")).toDouble() > 0.0;
                *yLabel = labelWithUnit(aperture ? QStringLiteral("Mean intensity")
                                                 : QStringLiteral("Intensity"),
                                        y->unit);
            }
        } else {
            if (xLabel)
                *xLabel = labelWithUnit(x->name.trimmed(), x->unit);
            if (yLabel)
                *yLabel = labelWithUnit(y->name.trimmed(), y->unit);
        }
        if (values.size() < 2) {
            axis.clear();
            values.clear();
            return fail(QStringLiteral("the FITS table holds fewer than two channels"));
        }
        return true;
    }
    return fail(QStringLiteral("the FITS file has no spectrum table"));
}

}   // namespace visivo

#endif   // FitsSpectrumTable_h
