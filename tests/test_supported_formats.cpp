#include "TestSupportedFormats.h"

#include "app/SupportedFormats.h"

#include <QSet>
#include <QtTest>

using visivo::FileType;
using namespace Qt::StringLiterals;

void TestSupportedFormats::everyTypeHasARowAndAStableKey()
{
    // Every enumerator up to AllFiles (the last one): a value added without a
    // row gets fileTypeInfo's fallback, whose type is not the one asked for.
    QList<FileType> all;
    for (int i = 0; i <= static_cast<int>(FileType::AllFiles); ++i)
        all.append(static_cast<FileType>(i));
    QCOMPARE(visivo::fileTypes().size(), all.size());
    QCOMPARE(visivo::fileTypes().back().type, FileType::AllFiles);
    QSet<QString> keys;
    for (const FileType type : all) {
        const visivo::FileTypeInfo &info = visivo::fileTypeInfo(type);
        QCOMPARE(info.type, type);
        QVERIFY(!info.label.isEmpty());
        QVERIFY(!info.key.isEmpty());
        keys.insert(QString(info.key));
        // The key read back is the type it was written for.
        QCOMPARE(visivo::fileTypeFromKey(QString(info.key)), std::optional<FileType>(type));
        // Suffixes are stored lower case, with their dot.
        for (const QString &suffix : info.suffixes) {
            QVERIFY(suffix.startsWith(u'.'));
            QCOMPARE(suffix, suffix.toLower());
        }
    }
    QCOMPARE(keys.size(), all.size());
    QVERIFY(!visivo::fileTypeFromKey(u"no-such-type").has_value());
    QVERIFY(!visivo::fileTypeFromKey(u"").has_value());
}

void TestSupportedFormats::allSupportedIsTheUnionWithoutRepeats()
{
    const QStringList all = visivo::fileTypeSuffixes(FileType::AllSupported);
    for (const FileType type : { FileType::Fits, FileType::Catalogue, FileType::Vbt,
                                 FileType::Hdf5, FileType::CommaSeparated }) {
        for (const QString &suffix : visivo::fileTypeSuffixes(type))
            QVERIFY2(all.contains(suffix), qPrintable(suffix));
    }
    // .fits is both a FITS image and a FITS table: listed once.
    QCOMPARE(all.count(u".fits"_s), 1);
    QCOMPARE(QSet<QString>(all.begin(), all.end()).size(), all.size());
    QVERIFY(visivo::fileTypeSuffixes(FileType::AllFiles).isEmpty());
}

void TestSupportedFormats::multiPartSuffixesMatchWhole()
{
    QVERIFY(visivo::fileTypeMatches(u"m31.fits.gz", FileType::Fits));
    QVERIFY(visivo::fileTypeMatches(u"cube.fts.gz", FileType::Fits));
    QVERIFY(visivo::fileTypeMatches(u"cube.fits.fz", FileType::Fits));
    QVERIFY(visivo::fileTypeMatches(u"snap.bin.head", FileType::Vbt));
    QVERIFY(visivo::fileTypeMatches(u"snap.bin", FileType::Vbt));
    // A gzipped FITS table is not offered as a catalogue: the catalogue
    // opener is asked for it by its last suffix, which says nothing.
    QVERIFY(!visivo::fileTypeMatches(u"table.fits.gz", FileType::Catalogue));
    QVERIFY(visivo::fileTypeMatches(u"table.fits", FileType::Catalogue));
    // Any gzip is not a FITS.
    QVERIFY(!visivo::fileTypeMatches(u"notes.csv.gz", FileType::Fits));
    QVERIFY(!visivo::fileTypeMatches(u"notes.csv.gz", FileType::AllSupported));
    // A suffix inside the name is not its suffix.
    QVERIFY(!visivo::fileTypeMatches(u"m31.fits.bak", FileType::Fits));
    QVERIFY(!visivo::fileTypeMatches(u"fits", FileType::Fits));
}

void TestSupportedFormats::matchingIgnoresCase()
{
    QVERIFY(visivo::fileTypeMatches(u"M31.FITS.GZ", FileType::Fits));
    QVERIFY(visivo::fileTypeMatches(u"Gaia.VOT", FileType::Catalogue));
    QVERIFY(visivo::fileTypeMatches(u"Snap.Bin.Head", FileType::Vbt));
    QVERIFY(visivo::fileTypeMatches(u"PSR.H5", FileType::Hdf5));
    QVERIFY(visivo::fileTypeMatches(u"SOURCES.CSV", FileType::CommaSeparated));
}

void TestSupportedFormats::dotfilesAreListedOnlyUnderAllFiles()
{
    QVERIFY(visivo::isHiddenFileName(u".DS_Store"));
    QVERIFY(!visivo::isHiddenFileName(u"m31.fits"));
    // macOS resource forks keep the name they shadow.
    for (const FileType type : { FileType::AllSupported, FileType::Fits, FileType::Catalogue,
                                 FileType::Vbt, FileType::Hdf5, FileType::CommaSeparated }) {
        QVERIFY(!visivo::fileTypeMatches(u"._m31.fits", type));
        QVERIFY(!visivo::fileTypeMatches(u".DS_Store", type));
        QVERIFY(!visivo::fileTypeMatches(u".fits", type));
    }
    QVERIFY(visivo::fileTypeMatches(u"._m31.fits", FileType::AllFiles));
    QVERIFY(visivo::fileTypeMatches(u".DS_Store", FileType::AllFiles));
}

void TestSupportedFormats::allFilesListsAnything()
{
    QVERIFY(visivo::fileTypeMatches(u"README", FileType::AllFiles));
    QVERIFY(visivo::fileTypeMatches(u"run.log", FileType::AllFiles));
    QVERIFY(visivo::fileTypeMatches(u"data.tsv", FileType::AllFiles));
}

void TestSupportedFormats::otherKindsAreNotListed()
{
    QVERIFY(!visivo::fileTypeMatches(u"sources.csv", FileType::Fits));
    QVERIFY(!visivo::fileTypeMatches(u"m31.fits", FileType::Vbt));
    QVERIFY(!visivo::fileTypeMatches(u"m31.fits", FileType::Hdf5));
    QVERIFY(!visivo::fileTypeMatches(u"m31.fits", FileType::CommaSeparated));
    QVERIFY(!visivo::fileTypeMatches(u"gaia.vot", FileType::CommaSeparated));
    QVERIFY(!visivo::fileTypeMatches(u"README", FileType::AllSupported));
    QVERIFY(!visivo::fileTypeMatches(u"run.log", FileType::AllSupported));
    QVERIFY(!visivo::fileTypeMatches(u"", FileType::AllSupported));
    // The 3-D catalogue reader takes commas only.
    QVERIFY(!visivo::fileTypeMatches(u"data.tsv", FileType::Catalogue));
}

void TestSupportedFormats::displayLabelShowsPatterns()
{
    QCOMPARE(visivo::fileTypeDisplayLabel(FileType::Vbt),
             u"VisIVO Binary Tables (*.bin.head *.head *.bin)"_s);
    QVERIFY(visivo::fileTypeDisplayLabel(FileType::Fits).contains(u"*.fz"));
    QCOMPARE(visivo::fileTypeDisplayLabel(FileType::AllSupported), u"All supported files"_s);
    QCOMPARE(visivo::fileTypeDisplayLabel(FileType::AllFiles), u"All files"_s);
}

void TestSupportedFormats::allFilesIsAlwaysOffered()
{
    QCOMPARE(visivo::offeredFileTypes({ FileType::Fits }),
             (QList<FileType>{ FileType::Fits, FileType::AllFiles }));
    QCOMPARE(visivo::offeredFileTypes({}), QList<FileType>{ FileType::AllFiles });
    // Table order, whatever order the caller wrote, and no repeats.
    QCOMPARE(visivo::offeredFileTypes({ FileType::AllFiles, FileType::Hdf5, FileType::Fits,
                                        FileType::Hdf5 }),
             (QList<FileType>{ FileType::Fits, FileType::Hdf5, FileType::AllFiles }));
    const QList<FileType> open = visivo::offeredFileTypes(visivo::openableFileTypes());
    QCOMPARE(open.front(), FileType::AllSupported);
    QCOMPARE(open.back(), FileType::AllFiles);
    QVERIFY(!open.contains(FileType::CommaSeparated));
}

void TestSupportedFormats::initialTypePrefersTheRememberedOne()
{
    const QList<FileType> offered = { FileType::Fits, FileType::AllFiles };
    QCOMPARE(visivo::initialFileType(offered, FileType::Fits, u"all-files"), FileType::AllFiles);
    // Nothing remembered, or a key from another build: the caller's choice.
    QCOMPARE(visivo::initialFileType(offered, FileType::Fits, u""), FileType::Fits);
    QCOMPARE(visivo::initialFileType(offered, FileType::Fits, u"bogus"), FileType::Fits);
    // A remembered type this caller does not offer is not imposed on it.
    QCOMPARE(visivo::initialFileType(offered, FileType::Fits, u"hdf5"), FileType::Fits);
    // A preference that is not offered falls back to the first offered.
    QCOMPARE(visivo::initialFileType(offered, FileType::Vbt, u""), FileType::Fits);
    QCOMPARE(visivo::initialFileType({}, FileType::Vbt, u""), FileType::AllFiles);
}
