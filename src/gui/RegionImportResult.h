#ifndef RegionImportResult_h
#define RegionImportResult_h

// RegionImportResult.h
// A region file read by the backend (`POST /v1/regions/parse` with the
// dataset, regions in 1-based image pixels) turned into regions of the list,
// and every line that did not become one, with its reason (issue #342).
//
// The backend reports the statements it could not read (an unknown shape, a
// sky region on an image without a celestial WCS, a region off the
// projection, …). What it read but the list cannot hold is reported here the
// same way, so nothing in the file is dropped silently. Rings read from one
// multi-radius annulus share the file line; they are named as one group
// ("Annulus, line 4, ring 1/3") and move together in the viewers.
//
// Pure QtCore, unit-tested in tests/test_region_model.cpp.

#include "RegionModel.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace visivo::regions {

struct ImportDiagnostic
{
    int line{ 0 };     ///< 1-based line of the file; 0 when unknown
    QString text;      ///< that line, as written
    QString reason;
};

struct ImportResult
{
    std::vector<Region> regions;
    std::vector<ImportDiagnostic> diagnostics;
    /// The backend returned only its first diagnostics; more lines were skipped.
    bool truncated{ false };
    int skippedByBackend{ 0 };   ///< its n_skipped, which counts past the cap
    int rejectedHere{ 0 };       ///< read by the backend, refused by the list

    /// Every line not read: the backend's count (past its cap) and ours.
    int notRead() const { return skippedByBackend + rejectedHere; }
};

/// Build the result of a parse response. @p fileText gives the line text for
/// the regions the list cannot hold (the backend's diagnostics carry theirs).
inline ImportResult importFromParse(const QJsonArray &regions, const QJsonArray &diagnostics,
                                    bool truncated, int skippedByBackend, const QString &fileText)
{
    ImportResult out;
    out.truncated = truncated;
    out.skippedByBackend = skippedByBackend;
    const QStringList lines = fileText.split(QLatin1Char('\n'));
    const auto lineText = [&lines](int line) {
        return line >= 1 && line <= lines.size() ? lines.at(line - 1).trimmed() : QString();
    };
    for (const QJsonValue &v : diagnostics) {
        const QJsonObject d = v.toObject();
        const int line = d.value(QStringLiteral("line")).toInt(0);
        QString text = d.value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            text = lineText(line);
        out.diagnostics.push_back({ line, text, d.value(QStringLiteral("reason")).toString() });
    }
    for (const QJsonValue &v : regions) {
        const QJsonObject o = v.toObject();
        Region r;
        QString why;
        if (!fromCanonical(o, 1.0, &r, &why)) {
            const int line = o.value(QStringLiteral("source_line")).toInt(0);
            out.diagnostics.push_back({ line, lineText(line), why });
            ++out.rejectedHere;
            continue;
        }
        r.sourceFrame = QStringLiteral("image");
        r.sourceUnit = QStringLiteral("pix");
        // A text region is called by its string.
        if (r.shape == Shape::Text && r.name.isEmpty())
            r.name = r.text;
        out.regions.push_back(r);
    }
    // How many rings each annulus statement kept, to name them "ring i/N".
    QHash<int, int> rings, ringSeen;
    for (const Region &r : out.regions)
        if (isRing(r.shape) && r.sourceLine > 0)
            ++rings[r.sourceLine];
    for (Region &r : out.regions) {
        const int n = r.sourceLine > 0 ? rings.value(r.sourceLine, 0) : 0;
        if (isRing(r.shape) && n > 1) {
            const int k = ++ringSeen[r.sourceLine];
            const QString base = r.name.isEmpty() ? shapeLabel(r.shape) : r.name;
            r.name = QStringLiteral("%1, line %2, ring %3/%4").arg(base).arg(r.sourceLine).arg(k).arg(n);
        }
    }
    std::sort(out.diagnostics.begin(), out.diagnostics.end(),
              [](const ImportDiagnostic &a, const ImportDiagnostic &b) { return a.line < b.line; });
    return out;
}

/// "4 regions read; 2 lines not read." for the status bar.
inline QString importSummary(const ImportResult &r)
{
    const int skipped = r.notRead();
    QString s = QStringLiteral("%1 region(s) read").arg(r.regions.size());
    if (skipped > 0)
        s += QStringLiteral("; %1 not read (see the report)").arg(skipped);
    return s + QLatin1Char('.');
}

}   // namespace visivo::regions

#endif   // RegionImportResult_h
