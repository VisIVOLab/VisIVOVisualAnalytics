#ifndef TOOL_RESULT_RECORD_H
#define TOOL_RESULT_RECORD_H
// ToolResultRecord.h
// The serialised shape of a tool's result window, as pure QtCore logic.
//
// A result window is described ONCE and that description is used twice: shown
// immediately (ToolResultDialog::applyRecord) and stored on the product
// (AnalysisProduct::resultView) so opening its Session Data row puts the same
// window back. Two renderings of the same numbers drift apart — that is why the
// description is shared rather than re-derived.
//
// The shape lives here, apart from the widget, because it is the part that can
// be wrong without failing to compile. It was: the encoder appended each row as
// a bare QVariantList, and QList::append(const QList<T> &) CONCATENATES, so every
// row was flattened into two loose strings; the decoder then found no pairs and
// produced sections with no rows. The window came up with a title, a subtitle
// and an empty body, and nothing anywhere said so. Hence
// tests/test_tool_result_record.cpp.
//
//   { "title":    "Noise Estimation",
//     "subtitle": "Channels 0..397 …",
//     "sections": [ { "title": "Sigma",
//                     "rows": [ ["Mean", "0.0034779168"], … ] }, … ] }

#include <QList>
#include <QPair>
#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace visivo {

/// One label/value line.
using ResultRow = QPair<QString, QString>;
/// A titled block of lines.
using ResultSection = QPair<QString, QList<ResultRow>>;

/// Encode a result window. Sections and rows keep the order given: a QVariantMap
/// would sort them, and "max, mean, median, min" is not how anyone reads a
/// summary.
inline QVariantMap encodeResultRecord(const QString &title, const QString &subtitle,
                                      const QList<ResultSection> &sections)
{
    QVariantList sectionList;
    for (const ResultSection &section : sections) {
        QVariantList rows;
        for (const ResultRow &row : section.second) {
            // QVariant(...) is load-bearing — see the note at the top.
            rows.append(QVariant(QVariantList{ row.first, row.second }));
        }
        sectionList.append(QVariantMap{ { QStringLiteral("title"), section.first },
                                        { QStringLiteral("rows"), rows } });
    }
    return { { QStringLiteral("title"), title },
             { QStringLiteral("subtitle"), subtitle },
             { QStringLiteral("sections"), sectionList } };
}

/// Decode the sections of a record. Malformed rows are skipped rather than
/// guessed at: a record can come from a product registered by an older build.
inline QList<ResultSection> decodeResultSections(const QVariantMap &record)
{
    QList<ResultSection> out;
    const QVariantList sections = record.value(QStringLiteral("sections")).toList();
    for (const QVariant &sectionVar : sections) {
        const QVariantMap section = sectionVar.toMap();
        QList<ResultRow> rows;
        const QVariantList rowList = section.value(QStringLiteral("rows")).toList();
        for (const QVariant &rowVar : rowList) {
            const QVariantList row = rowVar.toList();
            if (row.size() == 2)
                rows.append({ row.at(0).toString(), row.at(1).toString() });
        }
        out.append({ section.value(QStringLiteral("title")).toString(), rows });
    }
    return out;
}

inline QString recordTitle(const QVariantMap &record)
{
    return record.value(QStringLiteral("title")).toString();
}
inline QString recordSubtitle(const QVariantMap &record)
{
    return record.value(QStringLiteral("subtitle")).toString();
}

}   // namespace visivo

#endif   // TOOL_RESULT_RECORD_H
