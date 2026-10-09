#ifndef CatalogueOverlayUtils_h
#define CatalogueOverlayUtils_h

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <array>
#include <vector>

struct CatalogueOverlayEntry
{
    enum class Frame
    {
        Sky,
        Image,
    };

    enum class Shape
    {
        Marker,
        Ellipse,
        Polyline,   ///< an open/closed run of points (Image frame, pixel coords)
    };

    Frame frame{ Frame::Sky };
    Shape shape{ Shape::Marker };
    double raDeg{ 0.0 };
    double decDeg{ 0.0 };
    /// True when raDeg/decDeg hold a real position. An Image-frame entry (a
    /// shape built in pixels) can still know where it is on the sky.
    bool hasSkyPosition{ false };
    double pixelX{ 0.0 };
    double pixelY{ 0.0 };
    /// Drawn shape, in the units of @ref frame: PIXELS for an Image-frame entry.
    /// Not an angular size — see @ref sizeArcsec.
    double radiusX{ 0.0 };
    double radiusY{ 0.0 };
    /// Angular size of the source (arcsec), when the catalogue states one.
    /// Separate from radiusX/radiusY on purpose: those are pixels for every
    /// Image-frame entry (VLKB ellipses, ds9 image regions), and a SED fit that
    /// took them for arcsec would return a mass wrong by the pixel scale
    /// squared. 0 when the catalogue gives no angular size.
    double sizeArcsec{ 0.0 };
    double angleDeg{ 0.0 };
    std::vector<std::array<double, 2>> polyline;   ///< Shape::Polyline vertices (pixels)
    std::array<double, 3> color{ -1.0, -1.0, -1.0 };  ///< per-entry RGB; <0 = default actor colour
    QString band;    ///< group tag (e.g. VLKB band "250") for per-group visibility toggles
    QString label;
    /// Every field of the source row we did not interpret ourselves, in the
    /// order the file gave them. A catalogue is not four numbers and a name: the
    /// fluxes, errors and flags next to them are usually the reason it was
    /// opened, and they used to be parsed and thrown away.
    QList<QPair<QString, QString>> extra;
    /// Entries sharing this key are ONE physical source seen in several bands
    /// (VLKB: the band-merged designation). Empty when the catalogue has no such
    /// relation, and the table then stays flat.
    QString groupKey;
};

struct CatalogueOverlayParseResult
{
    bool valid{ false };
    QString errorMessage;
    QString frameLabel;
    QString sourceLabel;
    int skippedEntries{ 0 };
    std::vector<CatalogueOverlayEntry> entries;
    /// Union of the extra field names, in first-seen order: the columns a table
    /// should add after the ones every catalogue has.
    QStringList extraFields;
};

// Raw VLKB catalogue query result, in Galactic coordinates (deg). The network
// worker fills this off the GUI thread; the GUI thread then projects it to image
// pixels (which needs the window's WCS) and hands it to applyCatalogueOverlayResult.
struct VlkbCatalogueRaw
{
    bool valid{ false };
    QString error;
    QString sourceLabel;
    struct Ellipse {
        double glon, glat, fwhmaArcsec, fwhmbArcsec, paDeg;
        QString band;
        QString label;
        /// The row's other columns (fluxes, flags…), already filtered to the ones
        /// that belong to this band or to no band at all.
        QList<QPair<QString, QString>> fields;
        QString groupKey;   ///< band-merged designation: one source, several bands
    };
    struct Polyline { std::vector<std::array<double, 2>> pts; QString label; };  // (glon,glat)
    std::vector<Ellipse> ellipses;
    std::vector<Polyline> polylines;
};

namespace CatalogueOverlayUtils {

/// Split one CSV record, honouring quotes. `split(',')` was enough while the only
/// columns read were numbers; now that whole catalogues come through - a VLKB
/// view with text columns, a local file with a description - a quoted comma
/// shifts every field after it, so positions and sizes get read out of the wrong
/// cells. RFC 4180 quoting: "" inside a quoted field is a literal quote.
inline QStringList splitSimpleCsvLine(const QString &line)
{
    QStringList out;
    QString field;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inQuotes) {
            if (c == u'"') {
                if (i + 1 < line.size() && line.at(i + 1) == u'"') {
                    field.append(u'"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field.append(c);
            }
        } else if (c == u'"') {
            inQuotes = true;
        } else if (c == u',') {
            out.append(field);
            field.clear();
        } else {
            field.append(c);
        }
    }
    out.append(field);
    return out;
}

/// True when the line leaves a quoted field open, so the next physical line is
/// still part of the same record. Splitting on newlines before parsing quotes
/// truncated any value containing one and turned its tail into a bogus record.
inline bool csvRecordIsIncomplete(const QString &accumulated)
{
    int quotes = 0;
    for (const QChar c : accumulated)
        if (c == u'"')
            ++quotes;
    return (quotes % 2) != 0;
}

/// Read ONE csv record from @p stream, however many physical lines it spans.
/// @p unterminated is set when the file ends with a quote still open - which is
/// a broken file, and the caller has to say so rather than use the fragment.
inline QString readCsvRecord(QTextStream &stream, bool *unterminated = nullptr)
{
    if (unterminated)
        *unterminated = false;
    if (stream.atEnd())
        return {};
    QString record = stream.readLine();
    while (csvRecordIsIncomplete(record)) {
        if (stream.atEnd()) {
            if (unterminated)
                *unterminated = true;
            break;
        }
        record += u'\n' + stream.readLine();
    }
    return record;
}

/// Make a field name unique against the ones already collected: a catalogue with
/// two columns called `flux` otherwise shows one and silently drops the other.
inline QString uniqueFieldName(const QString &name, const QStringList &taken)
{
    if (!taken.contains(name))
        return name;
    for (int n = 2; n < 1000; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(name).arg(n);
        if (!taken.contains(candidate))
            return candidate;
    }
    return name;
}

inline int findHeaderIndex(const QStringList &headers, const QStringList &aliases)
{
    for (int i = 0; i < headers.size(); ++i) {
        const QString normalized = headers.at(i).trimmed().toUpper();
        for (const QString &alias : aliases) {
            if (normalized == alias) {
                return i;
            }
        }
    }
    return -1;
}

inline CatalogueOverlayParseResult parseCsv(const QString &path)
{
    CatalogueOverlayParseResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.errorMessage = QStringLiteral("Unable to open catalogue file.");
        return result;
    }

    QTextStream stream(&file);
    QString headerLine;
    bool headerUnterminated = false;
    while (!stream.atEnd() && headerLine.trimmed().isEmpty()) {
        // The header obeys the same quoting rules as the rows: a column name
        // containing a newline used to cut the header in two and misalign every
        // field after it.
        headerLine = readCsvRecord(stream, &headerUnterminated);
    }
    if (headerUnterminated) {
        result.errorMessage = QStringLiteral(
                "CSV catalogue header ends inside a quoted field: the file is truncated "
                "or its quoting is broken.");
        return result;
    }
    if (headerLine.trimmed().isEmpty()) {
        result.errorMessage = QStringLiteral("CSV catalogue is empty.");
        return result;
    }

    const QStringList headers = splitSimpleCsvLine(headerLine);
    const int raIndex = findHeaderIndex(headers, { QStringLiteral("RA") });
    const int decIndex = findHeaderIndex(headers, { QStringLiteral("DEC"), QStringLiteral("DEC_DEG") });
    const int labelIndex = findHeaderIndex(headers, { QStringLiteral("LABEL"), QStringLiteral("ID"),
                                                      QStringLiteral("NAME") });
    if (raIndex < 0 || decIndex < 0) {
        result.errorMessage =
                QStringLiteral("CSV catalogue requires RA and DEC header columns in degrees.");
        return result;
    }

    // One name per column, uniqued once, and the extra-field list in header order.
    QMap<int, QString> columnNames;
    for (int c = 0; c < headers.size(); ++c) {
        if (c == raIndex || c == decIndex || c == labelIndex)
            continue;
        const QString raw = headers.at(c).trimmed();
        if (raw.isEmpty())
            continue;
        const QString unique = uniqueFieldName(raw, result.extraFields);
        columnNames.insert(c, unique);
        result.extraFields.append(unique);
    }

    bool truncatedTail = false;
    while (!stream.atEnd()) {
        bool unterminated = false;
        QString line = readCsvRecord(stream, &unterminated);
        if (unterminated) {
            // A row that never closes its quote is not a row: count it as skipped
            // and stop, rather than inventing fields from the fragment.
            truncatedTail = true;
            ++result.skippedEntries;
            break;
        }
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList cols = splitSimpleCsvLine(line);
        if (cols.size() <= std::max(raIndex, decIndex)) {
            ++result.skippedEntries;
            continue;
        }
        bool okRa = false;
        bool okDec = false;
        const double ra = cols.at(raIndex).trimmed().toDouble(&okRa);
        const double dec = cols.at(decIndex).trimmed().toDouble(&okDec);
        if (!okRa || !okDec) {
            ++result.skippedEntries;
            continue;
        }

        CatalogueOverlayEntry entry;
        entry.raDeg = ra;
        entry.decDeg = dec;
        if (labelIndex >= 0 && labelIndex < cols.size()) {
            entry.label = cols.at(labelIndex).trimmed();
        }
        // …and everything else the row carries, under its own header.
        for (int c = 0; c < cols.size() && c < headers.size(); ++c) {
            if (c == raIndex || c == decIndex || c == labelIndex)
                continue;
            const QString rawKey = headers.at(c).trimmed();
            const QString value = cols.at(c).trimmed();
            if (rawKey.isEmpty() || value.isEmpty())
                continue;
            // Stable per COLUMN, not per row: every row must use the same name
            // for the same column, so the uniquing is done once on the header.
            const QString key = columnNames.value(c);
            if (key.isEmpty())
                continue;
            entry.extra.append({ key, value });
        }
        result.entries.push_back(entry);
    }

    if (result.entries.empty()) {
        result.errorMessage = QStringLiteral("No valid RA/DEC rows were found in the CSV catalogue.");
        return result;
    }

    result.valid = true;
    result.frameLabel = QStringLiteral("FK5/ICRS (RA/DEC degrees)");
    result.sourceLabel = QFileInfo(path).fileName();
    if (truncatedTail) {
        // Loaded, but the user has to know the file ended mid-record.
        result.errorMessage = QStringLiteral(
                "The last row ends inside a quoted field and was skipped: the file looks "
                "truncated.");
    }
    return result;
}

inline CatalogueOverlayParseResult parseDs9(const QString &path)
{
    CatalogueOverlayParseResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.errorMessage = QStringLiteral("Unable to open DS9 region file.");
        return result;
    }

    const QRegularExpression pointRe(
            QStringLiteral("^point\\s*\\(\\s*([^,\\)]+)\\s*,\\s*([^,\\)]+)\\s*\\)"),
            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression circleRe(
            QStringLiteral("^circle\\s*\\(\\s*([^,\\)]+)\\s*,\\s*([^,\\)]+)\\s*,\\s*([^\\)]+)\\)"),
            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression ellipseRe(
            QStringLiteral("^ellipse\\s*\\(\\s*([^,\\)]+)\\s*,\\s*([^,\\)]+)\\s*,\\s*([^,\\)]+)\\s*,\\s*([^,\\)]+)\\s*,\\s*([^\\)]+)\\)"),
            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression textRe(QStringLiteral("text\\s*=\\s*\\{([^}]*)\\}"),
                                    QRegularExpression::CaseInsensitiveOption);

    QTextStream stream(&file);
    QString currentFrame = QStringLiteral("fk5");
    while (!stream.atEnd()) {
        const QString rawLine = stream.readLine().trimmed();
        if (rawLine.isEmpty() || rawLine.startsWith(u'#')) {
            continue;
        }

        QString line = rawLine.section(u'#', 0, 0).trimmed();
        if (line.isEmpty()) {
            continue;
        }

        const QStringList semicolonParts = line.split(u';', Qt::SkipEmptyParts);
        if (semicolonParts.size() >= 2) {
            const QString maybeFrame = semicolonParts.front().trimmed().toLower();
            if (maybeFrame == QStringLiteral("fk5") || maybeFrame == QStringLiteral("icrs")
                || maybeFrame == QStringLiteral("image") || maybeFrame == QStringLiteral("physical")
                || maybeFrame == QStringLiteral("galactic")) {
                currentFrame = maybeFrame;
                line = semicolonParts.mid(1).join(QStringLiteral(";")).trimmed();
            }
        }

        const QString lower = line.toLower();
        if (lower == QStringLiteral("fk5") || lower == QStringLiteral("icrs")) {
            currentFrame = lower;
            continue;
        }
        if (lower == QStringLiteral("global")) {
            continue;
        }
        if (lower == QStringLiteral("image") || lower == QStringLiteral("physical")
            || lower == QStringLiteral("galactic")) {
            currentFrame = lower;
            continue;
        }

        if (currentFrame != QStringLiteral("fk5") && currentFrame != QStringLiteral("icrs")
            && currentFrame != QStringLiteral("image")) {
            ++result.skippedEntries;
            continue;
        }

        const auto pointMatch = pointRe.match(line);
        const auto circleMatch = circleRe.match(line);
        const auto ellipseMatch = ellipseRe.match(line);
        QString label;
        const auto textMatch = textRe.match(rawLine);
        if (textMatch.hasMatch()) {
            label = textMatch.captured(1).trimmed();
        }

        CatalogueOverlayEntry entry;
        entry.label = label;
        // Every other key=value attribute on the line, which is where a source
        // finder puts what it measured (flux, snr, fit flags…). Only `text` is
        // consumed above; the rest used to be dropped with the comment.
        {
            static const QRegularExpression attrRe(
                    QStringLiteral("([A-Za-z_][A-Za-z0-9_.]*)\\s*=\\s*(\\{[^}]*\\}|\"[^\"]*\"|[^\\s]+)"));
            auto it = attrRe.globalMatch(rawLine.section(u'#', 1));
            while (it.hasNext()) {
                const auto m = it.next();
                const QString key = m.captured(1).trimmed();
                if (key.compare(QStringLiteral("text"), Qt::CaseInsensitive) == 0)
                    continue;
                QString value = m.captured(2).trimmed();
                if (value.size() >= 2 && (value.front() == u'{' || value.front() == u'"'))
                    value = value.mid(1, value.size() - 2).trimmed();
                if (key.isEmpty() || value.isEmpty())
                    continue;
                entry.extra.append({ key, value });
            }
        }
        if (currentFrame == QStringLiteral("image")) {
            bool okX = false;
            bool okY = false;
            if (!ellipseMatch.hasMatch()) {
                ++result.skippedEntries;
                continue;
            }
            entry.frame = CatalogueOverlayEntry::Frame::Image;
            entry.shape = CatalogueOverlayEntry::Shape::Ellipse;
            entry.pixelX = ellipseMatch.captured(1).trimmed().toDouble(&okX) - 1.0;
            entry.pixelY = ellipseMatch.captured(2).trimmed().toDouble(&okY) - 1.0;
            bool okR1 = false;
            bool okR2 = false;
            bool okAngle = false;
            entry.radiusX = ellipseMatch.captured(3).trimmed().toDouble(&okR1);
            entry.radiusY = ellipseMatch.captured(4).trimmed().toDouble(&okR2);
            entry.angleDeg = ellipseMatch.captured(5).trimmed().toDouble(&okAngle);
            if (!okX || !okY || !okR1 || !okR2 || !okAngle) {
                ++result.skippedEntries;
                continue;
            }
        } else {
            bool okRa = false;
            bool okDec = false;
            double ra = 0.0;
            double dec = 0.0;
            entry.frame = CatalogueOverlayEntry::Frame::Sky;
            entry.shape = CatalogueOverlayEntry::Shape::Marker;
            if (pointMatch.hasMatch()) {
                ra = pointMatch.captured(1).trimmed().toDouble(&okRa);
                dec = pointMatch.captured(2).trimmed().toDouble(&okDec);
            } else if (circleMatch.hasMatch()) {
                ra = circleMatch.captured(1).trimmed().toDouble(&okRa);
                dec = circleMatch.captured(2).trimmed().toDouble(&okDec);
            } else {
                ++result.skippedEntries;
                continue;
            }

            if (!okRa || !okDec) {
                ++result.skippedEntries;
                continue;
            }
            entry.raDeg = ra;
            entry.decDeg = dec;
        }
        // Only now that the region is accepted: a skipped shape used to leave a
        // column with no value in any row.
        for (const auto &kv : entry.extra) {
            if (!result.extraFields.contains(kv.first))
                result.extraFields.append(kv.first);
        }
        result.entries.push_back(entry);
    }

    if (result.entries.empty()) {
        result.errorMessage =
                QStringLiteral("No supported fk5/icrs point/circle or image ellipse entries were found in the DS9 file.");
        return result;
    }

    result.valid = true;
    result.frameLabel = QStringLiteral("DS9 regions");
    result.sourceLabel = QFileInfo(path).fileName();
    return result;
}

inline CatalogueOverlayParseResult parseFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("csv")) {
        return parseCsv(path);
    }
    return parseDs9(path);
}

} // namespace CatalogueOverlayUtils

#endif
