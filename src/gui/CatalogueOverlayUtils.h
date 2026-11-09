#ifndef CatalogueOverlayUtils_h
#define CatalogueOverlayUtils_h

#include "DelimitedText.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
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
    return DelimitedText::splitLine(line, u',');
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

/// A delimited text table (comma, tab or semicolon, as @p layout says) with RA
/// and DEC columns in degrees, as catalogue entries.
inline CatalogueOverlayParseResult parseDelimited(const QString &path,
                                                  const DelimitedText::Layout &layout)
{
    CatalogueOverlayParseResult result;
    // "CSV" at the start of a sentence and inside one; the others in lower case
    // inside one.
    const QString kind = DelimitedText::tableKind(layout.separator);
    const QString kindInText = kind == QStringLiteral("CSV") ? kind : kind.toLower();
    // Not QIODevice::Text: it drops a lone "\r", the line end of an old Mac
    // file, which the reader splits on as the backend does.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errorMessage = QStringLiteral("Unable to open catalogue file.");
        return result;
    }

    QTextStream stream(&file);
    DelimitedText::RecordReader reader(stream, layout);
    // The header obeys the same quoting rules as the rows: a column name
    // containing a newline used to cut the header in two and misalign every
    // field after it. Blank and comment lines before it are not the header,
    // unless a `#` line names the columns of a numeric first row
    // (DelimitedText::headerFromComment).
    QStringList headers;
    bool headerUnterminated = false;
    if (!reader.header(&headers, &headerUnterminated)) {
        result.errorMessage = QStringLiteral("%1 catalogue is empty.").arg(kind);
        return result;
    }
    if (headerUnterminated) {
        result.errorMessage = QStringLiteral(
                "%1 catalogue header ends inside a quoted field: the file is truncated "
                "or its quoting is broken.").arg(kind);
        return result;
    }

    const int raIndex = findHeaderIndex(headers, { QStringLiteral("RA") });
    const int decIndex = findHeaderIndex(headers, { QStringLiteral("DEC"), QStringLiteral("DEC_DEG") });
    const int labelIndex = findHeaderIndex(headers, { QStringLiteral("LABEL"), QStringLiteral("ID"),
                                                      QStringLiteral("NAME") });
    if (raIndex < 0 || decIndex < 0) {
        result.errorMessage =
                QStringLiteral("%1 catalogue requires RA and DEC header columns in degrees.")
                        .arg(kind);
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

    // The rows first, then the entries: whether a column holds decimal-comma
    // numbers is a property of the whole column.
    bool truncatedTail = false;
    QList<QStringList> rows;
    {
        QStringList cols;
        bool unterminated = false;
        while (reader.next(&cols, &unterminated)) {
            if (unterminated) {
                // A row that never closes its quote is not a row: count it as
                // skipped and stop, rather than inventing fields from the fragment.
                truncatedTail = true;
                ++result.skippedEntries;
                break;
            }
            rows.append(cols);
        }
    }

    // A decimal comma ("12,5") is read as the number it is, here and in the
    // extra fields a SED fit reads its fluxes from, but only in a column that
    // is all numbers: a note reading "1,2" stays the text it is.
    const QList<bool> commaColumns = layout.decimalComma
            ? DelimitedText::decimalCommaColumns(rows, headers.size())
            : QList<bool>();
    const auto cell = [&commaColumns](const QStringList &cols, int index) {
        const QString value = cols.at(index).trimmed();
        return index < commaColumns.size() && commaColumns.at(index)
                ? DelimitedText::normaliseDecimalComma(value)
                : value;
    };

    for (const QStringList &cols : std::as_const(rows)) {
        if (cols.size() <= std::max(raIndex, decIndex)) {
            ++result.skippedEntries;
            continue;
        }
        bool okRa = false;
        bool okDec = false;
        const double ra = cell(cols, raIndex).toDouble(&okRa);
        const double dec = cell(cols, decIndex).toDouble(&okDec);
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
            const QString value = cell(cols, c);
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
        result.errorMessage =
                QStringLiteral("No valid RA/DEC rows were found in the %1 catalogue.").arg(kindInText);
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

/// A comma-separated catalogue, whatever its first lines would sniff as.
inline CatalogueOverlayParseResult parseCsv(const QString &path)
{
    return parseDelimited(path, DelimitedText::layoutFor(path, u','));
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

/// Whether one line is a DS9 region statement: a coordinate system on its own
/// (`fk5`), a `global` line, or a shape whose keyword is followed directly by
/// its parenthesis (`circle(`, `-box(`), optionally after a coordinate system
/// (`fk5;point(`). Strict on purpose: a column called `global_id`, `Box
/// (arcsec)` or `Line (GHz)` is not a region.
inline bool isDs9Statement(QStringView raw)
{
    static const QRegularExpression shapeRe(
            QStringLiteral("^[+-]?(point|circle|ellipse|box|polygon|line|text|annulus|"
                           "vector|ruler|compass|projection|panda|epanda|bpanda|composite)\\("),
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression globalRe(QStringLiteral("^global(\\s|$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    static const QStringList frames = { QStringLiteral("fk4"), QStringLiteral("fk5"),
                                        QStringLiteral("icrs"), QStringLiteral("image"),
                                        QStringLiteral("physical"), QStringLiteral("galactic"),
                                        QStringLiteral("ecliptic"), QStringLiteral("linear"),
                                        QStringLiteral("amplifier"), QStringLiteral("detector"),
                                        QStringLiteral("wcs") };
    // What follows "#" on a region line is its attributes (text={…}).
    QString body = DelimitedText::withoutBom(raw).trimmed().toString().section(u'#', 0, 0).trimmed();
    const QString first = body.section(u';', 0, 0).trimmed().toLower();
    if (frames.contains(first)) {
        if (!body.contains(u';'))
            return true;   // a coordinate-system line on its own
        body = body.section(u';', 1).trimmed();
    }
    return globalRe.match(body).hasMatch() || shapeRe.match(body).hasMatch();
}

/// The first significant lines of @p text (blank and `#` lines aside), up to
/// the sample the table sniff looks at.
inline QStringList ds9SampleLines(const QString &text)
{
    QStringList out;
    for (const QString &line : DelimitedText::physicalLines(text, true)) {
        if (!DelimitedText::isSignificantLine(line))
            continue;
        out.append(line);
        if (out.size() >= DelimitedText::kSampleLines)
            break;
    }
    return out;
}

/// Whether @p text, which no separator makes a table, reads as DS9 regions:
/// it starts with ds9's `# Region file format` comment, or one of its first
/// lines is a region statement.
inline bool looksLikeDs9Regions(const QString &text)
{
    for (const QString &raw : DelimitedText::physicalLines(text, true)) {
        QStringView line = DelimitedText::withoutBom(raw).trimmed();
        if (!line.startsWith(u'#'))
            continue;
        line = line.mid(1).trimmed();
        if (line.startsWith(QStringLiteral("Region file format"), Qt::CaseInsensitive))
            return true;
    }
    const QStringList sample = ds9SampleLines(text);
    return std::any_of(sample.cbegin(), sample.cend(),
                       [](const QString &line) { return isDs9Statement(line); });
}

/// Whether @p text, although it splits like a table, is every line a region
/// statement: a file of bare `fk5;point(10.1,20.2)` lines splits on its commas
/// into two fields per line, the shape of a two-column table.
inline bool isAllDs9Statements(const QString &text)
{
    const QStringList sample = ds9SampleLines(text);
    return !sample.isEmpty()
            && std::all_of(sample.cbegin(), sample.cend(),
                           [](const QString &line) { return isDs9Statement(line); });
}

/// Read a catalogue overlay file by what it holds, not only by its name, as
/// the backend reads delimited text (issue #155):
///   - a `.reg` file is regions;
///   - a file whose first lines make a comma, tab or semicolon table is that
///     table, whatever it is called (`.csv`, `.tsv`, `.txt`…), unless every one
///     of those lines is a region statement;
///   - otherwise a file that reads as DS9 regions is regions;
///   - a `.csv` or `.tsv` that is neither is an error saying so, not a silent
///     attempt at regions;
///   - anything else is regions, as before.
inline CatalogueOverlayParseResult parseFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("reg"))
        return parseDs9(path);
    const DelimitedText::Head head = DelimitedText::readHead(path);
    if (!head.opened) {
        CatalogueOverlayParseResult result;
        result.errorMessage = QStringLiteral("Unable to open catalogue file.");
        return result;
    }
    const DelimitedText::Detection table = DelimitedText::detect(path);
    if (!table.layout.separator.isNull()) {
        if (isAllDs9Statements(head.text))
            return parseDs9(path);
        return parseDelimited(path, table.layout);
    }
    if (looksLikeDs9Regions(head.text))
        return parseDs9(path);
    if (suffix == QStringLiteral("csv") || suffix == QStringLiteral("tsv")) {
        CatalogueOverlayParseResult result;
        result.errorMessage = table.error;
        return result;
    }
    CatalogueOverlayParseResult regions = parseDs9(path);
    if (!regions.valid && !table.error.isEmpty()) {
        // Neither: say both, since the name did not tell which was meant.
        regions.errorMessage += QStringLiteral("\n\nNor is it a table: %1").arg(table.error);
    }
    return regions;
}

} // namespace CatalogueOverlayUtils

#endif
