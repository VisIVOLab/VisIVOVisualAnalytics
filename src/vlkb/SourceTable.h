#ifndef SourceTable_h
#define SourceTable_h

// SourceTable.h
// The logic behind "VLKB cutouts for a list of sources", kept free of widgets
// and of the network so it can be tested.
//
// Ported from the legacy client's UserTableWindow (ViaLactea Visual Analytics):
// a tab-separated table of sources (a header line, then one source per line,
// with at least an identifier and galactic l, b) is queried against the VLKB
// one source at a time; the answers are summarised as a coverage matrix of
// sources × surveys, and for each requested (survey, species, transition) the
// best dataset is chosen per source and sent as one asynchronous cutout job.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace visivo::vlkb {

/// A table as read from the file: the header and the rows, cells as text.
struct SourceTable
{
    QStringList columns;
    QList<QStringList> rows;
    QString error;   ///< non-empty when the file could not be used at all
    QStringList warnings;   ///< rows that were skipped, and why
    QString delimiter;   ///< what separated the cells: "tab", "comma", "semicolon", "spaces"
};

/// Parse a delimited table. The first line that is neither blank nor a comment
/// ('#') is the header, and fixes the separator: tab if it has one (the legacy
/// format), else comma, else semicolon, else runs of spaces. Line ends may be
/// LF or CRLF; blank and comment lines are skipped; cells are trimmed and lose
/// surrounding double quotes; a row with fewer cells than the header is
/// padded, one with more is reported and truncated.
SourceTable parseSourceTable(const QByteArray &text);

/// The column most likely to hold @p role ("id", "glon" or "glat"), by name,
/// case-insensitive; -1 if none matches. Used only to pre-select the combos.
int guessColumn(const QStringList &columns, const QString &role);

/// One source to query.
struct UserSource
{
    QString id;
    double glon{ 0.0 };
    double glat{ 0.0 };
    int row{ -1 };   ///< index into SourceTable::rows
};

/// Read the sources from the chosen columns. Rows whose coordinates are not
/// finite numbers, or whose latitude is outside [-90, 90], are skipped and
/// described in @p problems; an empty identifier becomes "row N".
QList<UserSource> pickSources(const SourceTable &table, int idColumn, int lonColumn,
                              int latColumn, QStringList *problems = nullptr);

/// One dataset of a VLKB inventory answer (the SIAv2 VOTable).
struct InventoryEntry
{
    QString type;        ///< "image" or "cube"
    QString survey;      ///< obs_collection
    QString species;     ///< obs_title before '|'
    QString transition;  ///< obs_title after '|'
    QString ivoID;       ///< obs_publisher_did, what a cutout is requested by
    int overlap{ 0 };    ///< 3 = the dataset fully covers the region; 0 = merge entry
};

/// Parse a VLKB SIAv2 VOTable. Rows without an obs_publisher_did, and merge
/// entries (overlap 0), are dropped: neither can be cut out.
QList<InventoryEntry> parseInventory(const QByteArray &votable);

/// What a cutout is asked for: a survey and one of its species/transitions.
struct ProductKey
{
    QString type;
    QString survey;
    QString species;
    QString transition;

    QString label() const;   ///< "survey — species transition"
    bool operator==(const ProductKey &o) const
    {
        return type == o.type && survey == o.survey && species == o.species
               && transition == o.transition;
    }
};
size_t qHash(const ProductKey &k, size_t seed = 0);

ProductKey keyOf(const InventoryEntry &e);

/// The best dataset of @p entries for @p key: one that fully covers the region
/// if there is one, else the first that matches; false if none matches.
bool bestEntry(const QList<InventoryEntry> &entries, const ProductKey &key, InventoryEntry *out);

/// Every product key found for any source, sorted for display.
QList<ProductKey> productsFound(const QList<QList<InventoryEntry>> &perSource);

/// How a survey covers one source, compared with what the queries found for
/// that survey across all sources.
enum class Coverage { None, Partial, Full };

/// Coverage of @p survey (of @p type) at one source: Full if the source has
/// every species/transition of that survey seen anywhere in the table, Partial
/// if some, None if none. @p detail lists each product with yes/no.
Coverage coverageOf(const QList<InventoryEntry> &sourceEntries, const QString &type,
                    const QString &survey, const QList<ProductKey> &allProducts,
                    QString *detail = nullptr);

}   // namespace visivo::vlkb

#endif   // SourceTable_h
