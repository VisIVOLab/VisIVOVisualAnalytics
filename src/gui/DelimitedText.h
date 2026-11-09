#pragma once

// DelimitedText.h
// Which separator a delimited text table uses, and how its header, comments
// and rows are told apart, judged the way the backend judges them
// (backend/app/text_table.py, issue #155). The catalogue overlays read files
// with the client's own parser, and used to send `.csv` to the CSV reader and
// everything else to the DS9 region reader, so a tab-separated catalogue was
// read as regions and found none. They now follow the backend's rule:
//
// The separator (judged on the first 8192 bytes; when the file goes on, the
// last line of that read is cut and not judged):
//   - blank lines and lines starting with `#` (after spaces, tabs or a
//     byte-order mark) do not vote;
//   - fields are PARSED as Python's csv module parses them (RecordParser), so a
//     quoted field may hold any separator or a newline;
//   - the first of comma, tab, semicolon that splits the first lines (up to
//     ten) into the same number of fields, at least two.
//
// The reading, with N the field count of the first line not starting with `#`:
//   - the header is that line, unless the `#` line right above it (blank lines
//     between allowed) reads as N names and that first line holds a number:
//     then `#id,ra,dec` over `1,10,20` names the columns (headerFromComment);
//   - below the header, a `#` line is a comment only when it does NOT split
//     into N fields; one that does is a row (a source called `#S5`);
//   - a semicolon table writes numbers with a decimal comma only when two
//     columns say so (usesDecimalComma), and then only the columns that are
//     all numbers are read that way (decimalCommaColumns);
//   - a file no separator makes consistent is read as plain comma CSV, as
//     pandas reads it, when that gives a number somewhere (ragged rows, a
//     trailing comma, one column).
// Lines end in "\n", "\r\n" or a lone "\r", as Python reads them.
// Widget-free, so the tests can reach it.

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QTextStream>

#include <algorithm>
#include <utility>

namespace DelimitedText {

using namespace Qt::StringLiterals;

/// How much of a file the separator is judged on (the backend's SNIFF_BYTES).
inline constexpr qint64 kSniffBytes = 8192;
/// A wider look when the first read held fewer than two whole lines (a header
/// of hundreds of columns), as the backend's reader does.
inline constexpr qint64 kWideSniffBytes = qint64(1) << 20;
/// How many lines must agree on the field count.
inline constexpr int kSampleLines = 10;

/// The separators tried, in order: the first consistent one wins.
inline const QList<QChar> &separators()
{
    static const QList<QChar> list = { u',', u'\t', u';' };
    return list;
}

/// Parses records as Python's csv module does (the backend's sniff and, for
/// this purpose, pandas): a `"` opens a quoted field only at the START of a
/// field; inside one, `""` is a literal quote and a separator or line break is
/// text; a `"` anywhere else is an ordinary character (`3"` is three inches).
/// Fed one physical line at a time, with its state kept between lines, so a
/// record spanning many lines costs what its characters cost.
class RecordParser
{
public:
    explicit RecordParser(QChar separator) : separator_(separator) {}

    /// Add one physical line (without its line end). True when it completes
    /// the record; false when a quoted field is still open.
    bool feed(QStringView line)
    {
        if (state_ == State::InQuoted)
            field_.append(u'\n');   // the line break is part of the quoted field
        for (const QChar c : line) {
            switch (state_) {
            case State::StartField:
                if (c == u'"') {
                    state_ = State::InQuoted;
                } else if (c == separator_) {
                    endField();
                } else {
                    field_.append(c);
                    state_ = State::InField;
                }
                break;
            case State::InField:
                if (c == separator_)
                    endField();
                else
                    field_.append(c);
                break;
            case State::InQuoted:
                if (c == u'"')
                    state_ = State::QuoteInQuoted;
                else
                    field_.append(c);
                break;
            case State::QuoteInQuoted:
                if (c == u'"') {
                    field_.append(u'"');   // "" escapes a quote
                    state_ = State::InQuoted;
                } else if (c == separator_) {
                    endField();
                } else {
                    field_.append(c);      // text after the closing quote, kept
                    state_ = State::InField;
                }
                break;
            }
        }
        return state_ != State::InQuoted;
    }

    /// Whether a quoted field is open (the record goes on on the next line).
    bool inQuotes() const { return state_ == State::InQuoted; }

    /// The record's fields, ending it however it stands (an open quote at the
    /// end of the file gives what was read, as a lenient reader does).
    QStringList take()
    {
        fields_.append(field_);
        field_.clear();
        state_ = State::StartField;
        return std::exchange(fields_, {});
    }

private:
    enum class State { StartField, InField, InQuoted, QuoteInQuoted };

    void endField()
    {
        fields_.append(field_);
        field_.clear();
        state_ = State::StartField;
    }

    QChar separator_;
    State state_{ State::StartField };
    QString field_;
    QStringList fields_;
};

/// Split one record on @p separator, by the csv rules of RecordParser.
inline QStringList splitLine(QStringView line, QChar separator)
{
    RecordParser parser(separator);
    parser.feed(line);
    return parser.take();
}

/// @p line without a leading byte-order mark: "﻿# note" is a comment.
inline QStringView withoutBom(QStringView line)
{
    return line.startsWith(QChar(0xFEFF)) ? line.mid(1) : line;
}

/// Starts with `#`, after spaces, tabs or a byte-order mark.
inline bool startsWithHash(QStringView line)
{
    return withoutBom(line).trimmed().startsWith(u'#');
}

/// Votes on the separator: not blank and not a `#` line.
inline bool isSignificantLine(QStringView line)
{
    const QStringView t = withoutBom(line).trimmed();
    return !t.isEmpty() && !t.startsWith(u'#');
}

// A number as the backend's _NUMBER reads it; a semicolon table's field
// written with a decimal comma, or a whole number; a decimal-point number.
inline bool isNumber(QStringView field)
{
    static const QRegularExpression re(u"^\\s*[+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?\\s*$"_s);
    return re.matchView(field).hasMatch();
}

inline bool isCommaOrInteger(QStringView field)
{
    static const QRegularExpression re(u"^\\s*[+-]?\\d+(,\\d+)?([eE][+-]?\\d+)?\\s*$"_s);
    return re.matchView(field).hasMatch();
}

inline bool isDecimalPointNumber(QStringView field)
{
    static const QRegularExpression re(u"^\\s*[+-]?\\d*\\.\\d+([eE][+-]?\\d+)?\\s*$"_s);
    return re.matchView(field).hasMatch();
}

/// What pandas reads as a missing value by default (it leaves a column
/// numeric): `NaN`, `NA`, `null`… An empty field is missing too.
inline bool isMissingValue(QStringView field)
{
    static const QStringList tokens = {
        u"#N/A"_s, u"#N/A N/A"_s, u"#NA"_s, u"-1.#IND"_s, u"-1.#QNAN"_s, u"-NaN"_s,
        u"-nan"_s, u"1.#IND"_s, u"1.#QNAN"_s, u"<NA>"_s, u"N/A"_s, u"NA"_s, u"NULL"_s,
        u"NaN"_s, u"None"_s, u"n/a"_s, u"nan"_s, u"null"_s,
    };
    const QStringView t = field.trimmed();
    return t.isEmpty() || tokens.contains(t);
}

/// A value pandas parses as a float: a number, or an infinity (`inf`,
/// `-Infinity`, any case).
inline bool isFloatValue(QStringView field)
{
    static const QRegularExpression inf(u"^\\s*[+-]?inf(inity)?\\s*$"_s,
                                        QRegularExpression::CaseInsensitiveOption);
    return isNumber(field) || inf.matchView(field).hasMatch();
}

/// The physical lines of @p text, split on "\n", "\r\n" or a lone "\r".
/// @p truncated says the text is the start of a longer file, whose last line
/// is probably cut and is not judged.
inline QStringList physicalLines(const QString &text, bool truncated)
{
    QStringList lines;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c != u'\n' && c != u'\r')
            continue;
        lines.append(text.mid(start, i - start));
        if (c == u'\r' && i + 1 < text.size() && text.at(i + 1) == u'\n')
            ++i;
        start = i + 1;
    }
    if (start < text.size())
        lines.append(text.mid(start));   // the last line, with no line end
    if (truncated && !lines.isEmpty())
        lines.removeLast();              // a read cut at a byte budget ends in half a row
    return lines;
}

/// The lines that vote, as read (not stripped).
inline QStringList significantLines(const QString &text, bool truncated)
{
    QStringList out;
    for (const QString &line : physicalLines(text, truncated)) {
        if (isSignificantLine(line))
            out.append(line);
    }
    return out;
}

/// @p lines as records split on @p separator: a quoted field that spans lines
/// joins them into one record, as a csv reader does. Empty records dropped.
inline QList<QStringList> joinRecords(const QStringList &lines, QChar separator)
{
    QList<QStringList> rows;
    RecordParser parser(separator);
    for (const QString &line : lines) {
        if (!parser.inQuotes() && line.isEmpty())
            continue;
        if (parser.feed(line))
            rows.append(parser.take());
    }
    if (parser.inQuotes())
        rows.append(parser.take());   // what a lenient reader returns
    return rows;
}

/// The separator that makes @p text a table, or a null QChar. One line with
/// commas is a sentence; up to ten lines that parse into the same number of
/// fields, two or more, are a table.
inline QChar sniffSeparator(const QString &text, bool truncated = false)
{
    const QStringList lines = significantLines(text, truncated);
    if (lines.size() < 2)
        return {};
    const QStringList sample = lines.mid(0, kSampleLines);
    for (const QChar separator : separators()) {
        const QList<QStringList> rows = joinRecords(sample, separator);
        if (rows.size() < 2 || rows.front().size() < 2)
            continue;
        const auto width = rows.front().size();
        if (std::all_of(rows.cbegin(), rows.cend(),
                        [width](const QStringList &row) { return row.size() == width; }))
            return separator;
    }
    return {};
}

/// Whether the data lines of a semicolon table write numbers with a decimal
/// comma, as a spreadsheet in a European locale exports them. "1,2" alone is
/// ambiguous (a decimal, or a list of two identifiers), so it takes TWO columns
/// of the sampled rows that read entirely as decimal-comma or whole numbers
/// (one comma at least), and no decimal-point number anywhere in the sample.
inline bool usesDecimalComma(const QStringList &dataLines)
{
    const QList<QStringList> rows = joinRecords(dataLines.mid(0, kSampleLines), u';');
    if (rows.isEmpty())
        return false;
    qsizetype width = 0;
    for (const QStringList &row : rows) {
        for (const QString &field : row) {
            if (isDecimalPointNumber(field))
                return false;
        }
        width = std::max(width, row.size());
    }
    int qualifying = 0;
    for (qsizetype c = 0; c < width; ++c) {
        bool any = false;
        bool allNumbers = true;
        bool sawComma = false;
        for (const QStringList &row : rows) {
            if (c >= row.size() || row.at(c).trimmed().isEmpty())
                continue;
            any = true;
            if (!isCommaOrInteger(row.at(c))) {
                allNumbers = false;
                break;
            }
            sawComma = sawComma || row.at(c).contains(u',');
        }
        if (any && allNumbers && sawComma)
            ++qualifying;
    }
    return qualifying >= 2;
}

/// A number in either notation, decimal comma or decimal point ("12,5",
/// "20.5", "31"), as the backend accepts it in a decimal-comma table's column.
inline bool isNumberEitherNotation(QStringView field)
{
    static const QRegularExpression re(
            u"^\\s*[+-]?(\\d+([.,]\\d*)?|[.,]\\d+)([eE][+-]?\\d+)?\\s*$"_s);
    return re.matchView(field).hasMatch();
}

/// The columns of a decimal-comma table that are read as numbers: those whose
/// every non-empty value, over ALL @p rows, is a number in either notation
/// (a later "20.5" does not make RA text). A column with any text in it stays
/// text, as the backend leaves it, so a note reading "1,2" is not rewritten.
inline QList<bool> decimalCommaColumns(const QList<QStringList> &rows, qsizetype width)
{
    QList<bool> numeric(width, true);
    for (const QStringList &row : rows) {
        for (qsizetype c = 0; c < width && c < row.size(); ++c) {
            if (numeric[c] && !row.at(c).trimmed().isEmpty()
                && !isNumberEitherNotation(row.at(c)))
                numeric[c] = false;
        }
    }
    return numeric;
}

/// "12,5" as "12.5", so QString::toDouble reads it; anything else unchanged.
/// For a column decimalCommaColumns() calls numeric.
inline QString normaliseDecimalComma(const QString &field)
{
    if (!field.contains(u',') || !isNumberEitherNotation(field))
        return field;
    QString out = field;
    out.replace(u',', u'.');
    return out;
}

/// How a file is laid out as a table.
struct Layout
{
    QChar separator;              ///< null when the file is not a table
    int fieldCount{ 0 };          ///< N: fields of the first line not starting with `#`
    int firstLine{ 0 };           ///< physical index of that line
    bool headerFromComment{ false };
    QStringList headerNames;      ///< the `#` line's names, when headerFromComment
    bool decimalComma{ false };   ///< a semicolon table with "12,5" numbers
    /// Read as plain comma CSV, as pandas reads it, although no separator was
    /// consistent: short rows are padded and fields past the header dropped.
    bool commaFallback{ false };
};

/// The names a `#` line above the first line gives the columns, or empty when
/// it is a comment: with its `#`s and surrounding spaces removed it must split
/// into exactly as many names as @p first has fields, every one non-empty and
/// none a number, and @p first must hold a number (so it is data, not the
/// header). `# Sources, v2, J2000` above a real header is a comment.
inline QStringList headerFromComment(QStringView comment, const QStringList &first,
                                     QChar separator)
{
    if (std::none_of(first.cbegin(), first.cend(),
                     [](const QString &f) { return isNumber(f); }))
        return {};
    QStringView body = withoutBom(comment).trimmed();
    while (body.startsWith(u'#'))
        body = body.mid(1);
    QStringList names = splitLine(body.trimmed(), separator);
    if (names.size() != first.size())
        return {};
    for (QString &name : names) {
        name = name.trimmed();
        if (name.isEmpty() || isNumber(name))
            return {};
    }
    return names;
}

/// Below the header, a line starting a record with `#` is a comment only when
/// it does not split into the table's field count.
inline bool isCommentRow(QStringView line, const Layout &layout)
{
    return startsWithHash(line) && splitLine(line, layout.separator).size() != layout.fieldCount;
}

/// Physical lines from a text stream, ended by "\n", "\r\n" or a lone "\r"
/// (QTextStream::readLine knows only the first two, and a file from an old
/// Mac would come back as one line).
class LineReader
{
public:
    explicit LineReader(QTextStream &stream) : stream_(stream) {}

    bool atEnd() const { return pos_ >= buffer_.size() && stream_.atEnd(); }

    QString readLine()
    {
        for (;;) {
            qsizetype i = pos_;
            while (i < buffer_.size() && buffer_.at(i) != u'\n' && buffer_.at(i) != u'\r')
                ++i;
            if (i < buffer_.size()) {
                // A "\r" that ends the buffer may be the first half of "\r\n".
                if (buffer_.at(i) == u'\r' && i + 1 == buffer_.size() && refill())
                    continue;
                QString line = buffer_.mid(pos_, i - pos_);
                pos_ = i + 1;
                if (buffer_.at(i) == u'\r' && pos_ < buffer_.size() && buffer_.at(pos_) == u'\n')
                    ++pos_;
                return line;
            }
            if (!refill()) {
                QString line = buffer_.mid(pos_);
                pos_ = buffer_.size();
                return line;
            }
        }
    }

private:
    bool refill()
    {
        if (stream_.atEnd())
            return false;
        if (pos_ > 0) {
            buffer_.remove(0, pos_);
            pos_ = 0;
        }
        const QString chunk = stream_.read(1 << 16);
        buffer_ += chunk;
        return !chunk.isEmpty();
    }

    QTextStream &stream_;
    QString buffer_;
    qsizetype pos_{ 0 };
};

/// Where the table starts in the file at @p path, read with @p separator: the
/// first line not starting with `#`, its field count, and the header names of
/// a `#` line above it when that is the header.
inline Layout layoutFor(const QString &path, QChar separator)
{
    Layout layout;
    layout.separator = separator;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return layout;
    QTextStream stream(&file);
    LineReader lines(stream);
    QString candidate;
    for (int index = 0; !lines.atEnd(); ++index) {
        QString line = lines.readLine();
        if (index == 0)
            line = withoutBom(line).toString();
        if (line.trimmed().isEmpty())
            continue;   // blanks keep the candidate
        if (startsWithHash(line)) {
            candidate = line;
            continue;
        }
        const QStringList first = splitLine(line, separator);
        layout.firstLine = index;
        layout.fieldCount = int(first.size());
        if (!candidate.isEmpty()) {
            layout.headerNames = headerFromComment(candidate, first, separator);
            layout.headerFromComment = !layout.headerNames.isEmpty();
        }
        return layout;
    }
    return layout;   // no table line: a header-only or empty file
}

/// Whether a file no separator made consistent still reads as plain comma CSV,
/// as pandas reads it (index_col=False): the header is the first line not
/// starting with `#`; rows may be shorter (padded) and, when the first row is
/// longer, the fields past the header are dropped, but a later row longer than
/// that is an error; and some column must parse as floats with a value in it.
/// Prose is all text and is refused. Judged on the first bytes, @p lines.
inline bool commaReadingHasNumbers(const QStringList &lines)
{
    QStringList table;
    bool started = false;
    for (const QString &line : lines) {
        if (!started && (line.trimmed().isEmpty() || startsWithHash(line)))
            continue;
        started = true;
        table.append(line);
    }
    const QList<QStringList> rows = joinRecords(table, u',');
    if (rows.size() < 2)
        return false;
    const qsizetype width = rows.front().size();
    const qsizetype allowed = std::max(width, rows.at(1).size());
    QList<bool> numeric(width, true);
    QList<bool> valued(width, false);
    for (qsizetype r = 1; r < rows.size(); ++r) {
        const QStringList &row = rows.at(r);
        if (row.size() > allowed)
            return false;   // pandas: "Expected n fields, saw m"
        for (qsizetype c = 0; c < width && c < row.size(); ++c) {
            if (isMissingValue(row.at(c)))
                continue;
            if (isFloatValue(row.at(c)))
                valued[c] = true;
            else
                numeric[c] = false;
        }
    }
    for (qsizetype c = 0; c < width; ++c) {
        if (numeric[c] && valued[c])
            return true;
    }
    return false;
}

/// The first bytes of a file, decoded as UTF-8, and whether it goes on.
struct Head
{
    bool opened{ false };
    QString text;
    bool truncated{ false };
};

inline Head readHead(const QString &path, qint64 nbytes = kSniffBytes)
{
    Head head;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return head;
    const QByteArray raw = file.read(nbytes + 1);
    head.opened = true;
    head.truncated = raw.size() > nbytes;
    head.text = QString::fromUtf8(raw.left(nbytes));
    return head;
}

/// What a file is, as a delimited text table.
struct Detection
{
    Layout layout;   ///< null separator when the file is not a table
    QString error;   ///< why it is not, when it is not
};

/// The table layout of the file at @p path, or why it has none, in the words
/// the backend uses for the same file.
inline Detection detect(const QString &path)
{
    Detection out;
    const QString name = QFileInfo(path).fileName();
    Head head;
    QChar separator;
    QStringList lines;
    for (const qint64 budget : { kSniffBytes, kWideSniffBytes }) {
        head = readHead(path, budget);
        if (!head.opened) {
            out.error = u"Unable to open %1."_s.arg(name);
            return out;
        }
        separator = sniffSeparator(head.text, head.truncated);
        lines = significantLines(head.text, head.truncated);
        if (!separator.isNull() || lines.size() >= 2 || !head.truncated)
            break;
    }
    if (separator.isNull()) {
        if (lines.isEmpty()) {
            out.error = head.text.trimmed().isEmpty()
                    ? u"%1 is empty."_s.arg(name)
                    : u"%1 holds no table: every line is a '#' comment."_s.arg(name);
            return out;
        }
        if (lines.size() == 1 && !head.truncated) {
            // A header and no rows yet: the first separator that splits it in two.
            for (const QChar candidate : separators()) {
                if (splitLine(lines.front(), candidate).size() >= 2) {
                    separator = candidate;
                    break;
                }
            }
        }
    }
    if (separator.isNull()) {
        if (commaReadingHasNumbers(physicalLines(head.text, head.truncated))) {
            out.layout = layoutFor(path, u',');
            out.layout.commaFallback = true;
            return out;
        }
        out.error = u"%1 is not a delimited text table: no separator (comma, tab or "
                    "semicolon) splits its first lines into the same number of fields, "
                    "two or more."_s.arg(name);
        return out;
    }
    out.layout = layoutFor(path, separator);
    if (separator == u';')
        out.layout.decimalComma = usesDecimalComma(
                out.layout.headerFromComment ? lines : lines.mid(1));
    return out;
}

/// Reads a table's header, then its records one at a time, however many
/// physical lines each spans, by the rules of @p layout. Linear in the file:
/// the quote state is carried, not recounted.
class RecordReader
{
public:
    RecordReader(QTextStream &stream, Layout layout)
        : lines_(stream), layout_(std::move(layout)), parser_(layout_.separator)
    {
    }

    /// The header: the first line not starting with `#`, or the names of the
    /// `#` line above it (that first line is then the first row). False when
    /// the file holds no table line.
    bool header(QStringList *fields, bool *unterminated)
    {
        *unterminated = false;
        while (line_ < layout_.firstLine && !lines_.atEnd())
            nextLine();
        if (layout_.headerFromComment) {
            *fields = layout_.headerNames;
            return true;
        }
        if (lines_.atEnd())
            return false;
        *fields = readRecord(nextLine(), unterminated);
        return true;
    }

    /// The next row as fields, or false at the end. @p unterminated is set when
    /// the file ends inside a quoted field: a broken file, which the caller has
    /// to report rather than use the fragment.
    bool next(QStringList *fields, bool *unterminated)
    {
        *unterminated = false;
        while (!lines_.atEnd()) {
            const QString line = nextLine();
            // Judged at the start of a record only: a `#` line inside a quoted
            // field spanning lines is part of that field (readRecord takes it).
            if (line.trimmed().isEmpty() || isCommentRow(line, layout_))
                continue;
            *fields = readRecord(line, unterminated);
            if (layout_.commaFallback && fields->size() > layout_.fieldCount)
                fields->resize(layout_.fieldCount);   // pandas drops them (index_col=False)
            return true;
        }
        return false;
    }

private:
    QString nextLine()
    {
        QString line = lines_.readLine();
        if (line_ == 0)
            line = withoutBom(line).toString();
        ++line_;
        return line;
    }

    QStringList readRecord(const QString &first, bool *unterminated)
    {
        bool complete = parser_.feed(first);
        while (!complete) {
            if (lines_.atEnd()) {
                *unterminated = true;
                break;
            }
            complete = parser_.feed(nextLine());
        }
        return parser_.take();
    }

    LineReader lines_;
    Layout layout_;
    RecordParser parser_;
    int line_{ 0 };
};

/// How a message names a table with this separator.
inline QString tableKind(QChar separator)
{
    if (separator == u'\t')
        return u"Tab-separated"_s;
    if (separator == u';')
        return u"Semicolon-separated"_s;
    return u"CSV"_s;
}

} // namespace DelimitedText
