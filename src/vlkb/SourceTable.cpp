#include "SourceTable.h"

#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace visivo::vlkb {

namespace {

/// Split one line into cells. @p sep is the separator, or a null QChar for
/// runs of whitespace. Double quotes group a cell, so a separator inside them
/// is text; a doubled quote inside a quoted cell is a literal quote.
QStringList tokenize(const QString &line, QChar sep)
{
    QStringList cells;
    QString cell;
    bool inQuotes = false;
    bool pending = false;   // whitespace mode: a cell has started
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (c == u'"') {
            if (inQuotes && i + 1 < line.size() && line[i + 1] == u'"') {
                cell += u'"';
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
            pending = true;
            continue;
        }
        const bool isSep = !inQuotes && (sep.isNull() ? c.isSpace() : c == sep);
        if (isSep) {
            if (sep.isNull()) {
                if (pending) {
                    cells << cell.trimmed();
                    cell.clear();
                    pending = false;
                }
            } else {
                cells << cell.trimmed();
                cell.clear();
            }
            continue;
        }
        cell += c;
        pending = true;
    }
    if (!sep.isNull() || pending)
        cells << cell.trimmed();
    return cells;
}

/// The separator of a header line, judged outside quoted text: tab, else
/// comma, else semicolon, else whitespace (a null QChar).
QChar detectSeparator(const QString &header, QString *name)
{
    QString outside;
    bool inQuotes = false;
    for (const QChar c : header) {
        if (c == u'"')
            inQuotes = !inQuotes;
        else if (!inQuotes)
            outside += c;
    }
    if (outside.contains(u'\t')) {
        *name = u"tab"_s;
        return u'\t';
    }
    if (outside.contains(u',')) {
        *name = u"comma"_s;
        return u',';
    }
    if (outside.contains(u';')) {
        *name = u"semicolon"_s;
        return u';';
    }
    *name = u"spaces"_s;
    return QChar();
}

}   // namespace

SourceTable parseSourceTable(const QByteArray &text)
{
    SourceTable t;
    QChar sep;
    const QStringList lines = QString::fromUtf8(text).split(u'\n');
    int lineNo = 0;
    for (QString line : lines) {
        ++lineNo;
        if (line.endsWith(u'\r'))
            line.chop(1);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(u'#'))
            continue;
        if (t.columns.isEmpty()) {
            sep = detectSeparator(line, &t.delimiter);
            t.columns = tokenize(line, sep);
            continue;
        }
        QStringList cells = tokenize(line, sep);
        if (cells.size() > t.columns.size()) {
            t.warnings << u"line %1: %2 cells for %3 columns, extra cells ignored"_s.arg(lineNo)
                                  .arg(cells.size())
                                  .arg(t.columns.size());
            cells = cells.mid(0, t.columns.size());
        }
        while (cells.size() < t.columns.size())
            cells << QString();
        t.rows << cells;
    }
    if (t.columns.isEmpty())
        t.error = u"The file is empty."_s;
    else if (t.columns.size() < 3)
        t.error = u"The header has %1 column(s); a source table needs at least an "
                  u"identifier, a galactic longitude and a galactic latitude."_s
                          .arg(t.columns.size());
    else if (t.rows.isEmpty())
        t.error = u"The table has a header but no sources."_s;
    return t;
}

int guessColumn(const QStringList &columns, const QString &role)
{
    static const QHash<QString, QStringList> names = {
        { u"id"_s, { u"id"_s, u"name"_s, u"designation"_s, u"source"_s, u"source_id"_s,
                     u"objname"_s } },
        { u"glon"_s, { u"glon"_s, u"l"_s, u"lon"_s, u"gal_lon"_s, u"glon_deg"_s } },
        { u"glat"_s, { u"glat"_s, u"b"_s, u"lat"_s, u"gal_lat"_s, u"glat_deg"_s } },
    };
    const QStringList wanted = names.value(role);
    for (const QString &w : wanted)
        for (int i = 0; i < columns.size(); ++i)
            if (columns[i].compare(w, Qt::CaseInsensitive) == 0)
                return i;
    return -1;
}

QList<UserSource> pickSources(const SourceTable &table, int idColumn, int lonColumn,
                              int latColumn, QStringList *problems)
{
    QList<UserSource> out;
    const int n = int(table.columns.size());
    if (lonColumn < 0 || lonColumn >= n || latColumn < 0 || latColumn >= n) {
        if (problems)
            *problems << u"Choose the longitude and latitude columns."_s;
        return out;
    }
    for (int r = 0; r < table.rows.size(); ++r) {
        const QStringList &cells = table.rows[r];
        bool okLon = false, okLat = false;
        const double l = cells.value(lonColumn).toDouble(&okLon);
        const double b = cells.value(latColumn).toDouble(&okLat);
        const QString id = (idColumn >= 0 && idColumn < n) ? cells.value(idColumn) : QString();
        const QString name = id.isEmpty() ? u"row %1"_s.arg(r + 1) : id;
        if (!okLon || !okLat || !std::isfinite(l) || !std::isfinite(b)) {
            if (problems)
                *problems << u"%1: the coordinates are not numbers"_s.arg(name);
            continue;
        }
        if (b < -90.0 || b > 90.0) {
            if (problems)
                *problems << u"%1: latitude %2 is outside [-90, 90]"_s.arg(name).arg(b);
            continue;
        }
        out << UserSource{ name, l, b, r };
    }
    return out;
}

QList<InventoryEntry> parseInventory(const QByteArray &votable)
{
    QList<InventoryEntry> out;
    int idx = 0;
    int iType = -1, iSurvey = -1, iTitle = -1, iDid = -1, iOverlap = -1;
    QXmlStreamReader xml(votable);
    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == u"FIELD"_s) {
            const QString name = xml.attributes().value(u"name"_s).toString();
            if (name == u"dataproduct_type"_s)
                iType = idx;
            else if (name == u"obs_collection"_s)
                iSurvey = idx;
            else if (name == u"obs_title"_s)
                iTitle = idx;
            else if (name == u"obs_publisher_did"_s)
                iDid = idx;
            else if (name == u"overlapSky"_s)
                iOverlap = idx;
            ++idx;
            continue;
        }
        if (!(xml.isStartElement() && xml.name() == u"TR"_s))
            continue;
        InventoryEntry e;
        int col = 0;
        while (!xml.atEnd() && !xml.hasError()
               && !(xml.isEndElement() && xml.name() == u"TR"_s)) {
            xml.readNext();
            if (!(xml.isStartElement() && xml.name() == u"TD"_s))
                continue;
            const QString val = xml.readElementText();
            if (col == iType)
                e.type = val.simplified();
            else if (col == iSurvey)
                e.survey = val.simplified();
            else if (col == iTitle) {
                const QStringList parts = val.split(u'|');
                e.species = parts.first().simplified();
                e.transition = parts.last().simplified();
            } else if (col == iDid)
                e.ivoID = val.simplified();
            else if (col == iOverlap)
                e.overlap = val.toInt();
            ++col;
        }
        // A dataset without a DID cannot be cut out, and overlap 0 marks the
        // archive's merge entries, which the legacy client skipped too.
        if (e.ivoID.isEmpty() || e.overlap == 0)
            continue;
        out << e;
    }
    return out;
}

QString ProductKey::label() const
{
    const QString band = transition == species ? species : u"%1 %2"_s.arg(species, transition);
    return u"%1 — %2"_s.arg(survey, band.trimmed());
}

size_t qHash(const ProductKey &k, size_t seed)
{
    return ::qHash(k.type + u'\x1f' + k.survey + u'\x1f' + k.species + u'\x1f' + k.transition,
                   seed);
}

ProductKey keyOf(const InventoryEntry &e)
{
    return ProductKey{ e.type, e.survey, e.species, e.transition };
}

bool bestEntry(const QList<InventoryEntry> &entries, const ProductKey &key, InventoryEntry *out)
{
    const InventoryEntry *best = nullptr;
    for (const InventoryEntry &e : entries) {
        if (!(keyOf(e) == key))
            continue;
        if (!best || (e.overlap == 3 && best->overlap != 3))
            best = &e;
    }
    if (!best)
        return false;
    if (out)
        *out = *best;
    return true;
}

QList<ProductKey> productsFound(const QList<QList<InventoryEntry>> &perSource)
{
    QSet<ProductKey> seen;
    QList<ProductKey> out;
    for (const auto &entries : perSource)
        for (const InventoryEntry &e : entries) {
            const ProductKey k = keyOf(e);
            if (!seen.contains(k)) {
                seen.insert(k);
                out << k;
            }
        }
    std::sort(out.begin(), out.end(), [](const ProductKey &a, const ProductKey &b) {
        if (a.type != b.type)
            return a.type < b.type;
        if (a.survey != b.survey)
            return a.survey < b.survey;
        if (a.species != b.species)
            return a.species < b.species;
        return a.transition < b.transition;
    });
    return out;
}

Coverage coverageOf(const QList<InventoryEntry> &sourceEntries, const QString &type,
                    const QString &survey, const QList<ProductKey> &allProducts, QString *detail)
{
    QSet<ProductKey> here;
    for (const InventoryEntry &e : sourceEntries)
        here.insert(keyOf(e));
    int expected = 0, found = 0;
    QStringList lines;
    for (const ProductKey &k : allProducts) {
        if (k.type != type || k.survey != survey)
            continue;
        ++expected;
        const bool has = here.contains(k);
        found += has ? 1 : 0;
        lines << u"%1: %2"_s.arg(k.label(), has ? u"yes"_s : u"no"_s);
    }
    if (detail)
        *detail = lines.join(u'\n');
    if (expected == 0 || found == 0)
        return Coverage::None;
    return found == expected ? Coverage::Full : Coverage::Partial;
}

}   // namespace visivo::vlkb
