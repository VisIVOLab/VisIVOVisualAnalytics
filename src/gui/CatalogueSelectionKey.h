#ifndef CatalogueSelectionKey_h
#define CatalogueSelectionKey_h

// CatalogueSelectionKey.h
// Which source the 3-D catalogue window has selected, in terms that survive a
// filter or another page of rows (issue #298). An index into the window's
// entries does not: a filter replaces them, so index 12 becomes another source
// and the panel would describe one the marker is not on.
//
// Widget-free and header-only: unit-tested in tests/test_catalogue_parser.cpp.

#include "Catalogue3DParser.h"

#include <QString>

#include <vector>

namespace CatalogueSelection {

struct Key
{
    /// The row in the table as opened on the backend (Catalogue3DEntry::
    /// sourceRow), the same through every filter and page; -1 when unknown.
    qint64 sourceRow{ -1 };
    /// The catalogue's own name for the source, used only without a row. Not
    /// the "Row N" / "Source N" fallback, which is no name at all.
    QString name;

    bool valid() const { return this->sourceRow >= 0 || !this->name.isEmpty(); }
};

/// The key of entry @p index of @p entries; an invalid key for none.
inline Key keyOf(const std::vector<Catalogue3DEntry> &entries, const Catalogue3DSchema &schema,
                 int index)
{
    Key key;
    if (index < 0 || index >= static_cast<int>(entries.size()))
        return key;
    const Catalogue3DEntry &e = entries[static_cast<std::size_t>(index)];
    key.sourceRow = e.sourceRow;
    if (key.sourceRow < 0) {
        namespace cp = Catalogue3DParser::detail;
        key.name = cp::fieldString(e, schema, cp::detectedNameField(schema));
    }
    return key;
}

/// Where the source @p key names is in @p entries now, or -1 when it is not
/// there (filtered out) or cannot be told apart: two rows of the same name
/// with no row numbers, where picking one would be a guess.
inline int indexOf(const std::vector<Catalogue3DEntry> &entries, const Catalogue3DSchema &schema,
                   const Key &key)
{
    if (!key.valid())
        return -1;
    if (key.sourceRow >= 0) {
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].sourceRow == key.sourceRow)
                return static_cast<int>(i);
        return -1;
    }
    namespace cp = Catalogue3DParser::detail;
    const QString nameField = cp::detectedNameField(schema);
    int found = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (cp::fieldString(entries[i], schema, nameField) != key.name)
            continue;
        if (found >= 0)
            return -1;   // ambiguous
        found = static_cast<int>(i);
    }
    return found;
}

}   // namespace CatalogueSelection

#endif // CatalogueSelectionKey_h
