#pragma once

// SupportedFormats.h
// The kinds of file the application opens, by name, for the file browser's
// type drop-down (issue #153). One table, so the Open dialog and every caller
// that asks for one kind of file read the same lists instead of six copies of
// them, some of which had forgotten `.fz`.
//
// The lists follow what the backend opens (backend/app/classify.py and the
// openers behind it):
//   - FITS: `.fits .fit .fts` and the compressed names the backend opens from a
//     decompressed copy (compressed_fits.COMPRESSED_FITS_SUFFIXES). Velocity
//     fields are FITS too; the classifier tells them apart by their axes.
//   - Catalogues: what /v1/catalogue/open reads (CSV, VOTable, IPAC `.tbl`,
//     `.speck`, FITS tables) under the names CatalogueFormat.h maps to those
//     readers. `.txt` is there because the classifier takes delimited text by
//     content; the catalogue reader itself reads comma-separated text, which is
//     why `.tsv` is not offered.
//   - VisIVO Binary Tables: the `.bin.head` header, or either half of the pair
//     (the classifier resolves a picked `.bin` to the header beside it).
//   - HDF5: the names hdf5_dynspec.is_hdf5_path accepts.
//
// These are a VIEW filter. The backend classifies a file by its content, so a
// FITS or a CSV under some other name still opens: it is listed under
// "All files".

#include <QLatin1StringView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace visivo {

enum class FileType {
    AllSupported,     ///< the union of the specific groups below
    Fits,
    Catalogue,
    Vbt,
    Hdf5,
    CommaSeparated,   ///< plain CSV only, for readers that take nothing else
    AllFiles,         ///< every file, dotfiles included; stays LAST (the tests walk up to it)
};

struct FileTypeInfo
{
    FileType type;
    /// Stable name for QSettings: the remembered choice must survive a
    /// reordering of the enum.
    QLatin1StringView key;
    QString label;
    /// Lower case, with the leading dot. Multi-part ones (`.fits.gz`,
    /// `.bin.head`) are matched whole. Empty for AllSupported (computed) and
    /// AllFiles (no restriction).
    QStringList suffixes;
};

/// Every group, in the order a drop-down lists them.
inline const QList<FileTypeInfo> &fileTypes()
{
    using namespace Qt::StringLiterals;
    static const QList<FileTypeInfo> table = {
        { FileType::AllSupported, "all-supported"_L1, u"All supported files"_s, {} },
        { FileType::Fits, "fits"_L1, u"FITS images and cubes"_s,
          { u".fits"_s, u".fit"_s, u".fts"_s, u".fits.gz"_s, u".fit.gz"_s, u".fts.gz"_s,
            u".fz"_s } },
        { FileType::Catalogue, "catalogue"_L1, u"Catalogues"_s,
          { u".csv"_s, u".txt"_s, u".vot"_s, u".votable"_s, u".xml"_s, u".tbl"_s, u".speck"_s,
            u".fits"_s, u".fit"_s } },
        { FileType::Vbt, "vbt"_L1, u"VisIVO Binary Tables"_s,
          { u".bin.head"_s, u".head"_s, u".bin"_s } },
        { FileType::Hdf5, "hdf5"_L1, u"HDF5"_s,
          { u".h5"_s, u".hdf5"_s, u".hdf"_s, u".bfdata"_s } },
        { FileType::CommaSeparated, "csv"_L1, u"Comma-separated tables"_s,
          { u".csv"_s, u".txt"_s } },
        { FileType::AllFiles, "all-files"_L1, u"All files"_s, {} },
    };
    return table;
}

inline const FileTypeInfo &fileTypeInfo(FileType type)
{
    for (const FileTypeInfo &info : fileTypes()) {
        if (info.type == type)
            return info;
    }
    return fileTypes().back();   // unreachable: every enumerator has a row
}

/// The suffixes a group matches; for AllSupported, the union of the others
/// without repeats, in table order. Empty for AllFiles.
inline QStringList fileTypeSuffixes(FileType type)
{
    if (type != FileType::AllSupported)
        return fileTypeInfo(type).suffixes;
    // Built once: the browser asks for it for every file of a folder.
    static const QStringList all = [] {
        QStringList union_;
        for (const FileTypeInfo &info : fileTypes()) {
            for (const QString &suffix : info.suffixes) {
                if (!union_.contains(suffix))
                    union_.append(suffix);
            }
        }
        return union_;
    }();
    return all;
}

/// A name the platforms hide: `.DS_Store`, `.git`, `._image.fits`.
inline bool isHiddenFileName(QStringView name)
{
    return name.startsWith(u'.');
}

/// Whether a FILE called `name` (no directory part) is listed under `type`.
/// Dotfiles are listed only under AllFiles. Case-insensitive, so
/// `M31.FITS.GZ` is a FITS like `m31.fits.gz`.
inline bool fileTypeMatches(QStringView name, FileType type)
{
    if (type == FileType::AllFiles)
        return true;
    if (name.isEmpty() || isHiddenFileName(name))
        return false;
    for (const QString &suffix : fileTypeSuffixes(type)) {
        if (name.endsWith(suffix, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

/// The group stored under `key`, if any.
inline std::optional<FileType> fileTypeFromKey(QStringView key)
{
    for (const FileTypeInfo &info : fileTypes()) {
        if (info.key == key)
            return info.type;
    }
    return std::nullopt;
}

/// The drop-down's text: the label, then its patterns as native dialogs show
/// them ("FITS images and cubes (*.fits *.fit …)"). The two catch-alls carry
/// no list: one would be every pattern, the other has none.
inline QString fileTypeDisplayLabel(FileType type)
{
    const FileTypeInfo &info = fileTypeInfo(type);
    if (info.suffixes.isEmpty())
        return info.label;
    QStringList patterns;
    patterns.reserve(info.suffixes.size());
    for (const QString &suffix : info.suffixes)
        patterns.append(QLatin1Char('*') + suffix);
    return info.label + QLatin1String(" (") + patterns.join(QLatin1Char(' ')) + QLatin1Char(')');
}

/// The kinds the unified Open offers: every kind the application opens.
/// CommaSeparated is left out, a subset of Catalogue kept for readers that
/// take nothing else.
inline QList<FileType> openableFileTypes()
{
    return { FileType::AllSupported, FileType::Fits, FileType::Catalogue, FileType::Vbt,
             FileType::Hdf5 };
}

/// What a caller offers, in table order, with AllFiles always there: whatever
/// a caller expects, the user can still reach a file it did not foresee.
inline QList<FileType> offeredFileTypes(const QList<FileType> &requested)
{
    QList<FileType> offered;
    for (const FileTypeInfo &info : fileTypes()) {
        if (info.type == FileType::AllFiles || requested.contains(info.type))
            offered.append(info.type);
    }
    return offered;
}

/// The group a browser opens on: the one remembered for this kind of open when
/// it is still offered, else the caller's preference when offered, else the
/// first one offered.
inline FileType initialFileType(const QList<FileType> &offered, FileType preferred,
                                QStringView rememberedKey)
{
    if (const auto remembered = fileTypeFromKey(rememberedKey);
        remembered && offered.contains(*remembered))
        return *remembered;
    if (offered.contains(preferred))
        return preferred;
    return offered.isEmpty() ? FileType::AllFiles : offered.front();
}

} // namespace visivo
