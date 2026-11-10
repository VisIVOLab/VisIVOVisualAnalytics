#pragma once

// Reopening a Recent Datasets entry whose file has gone (#150). The entry's id
// is the path the backend opened; once the file is deleted or moved, handing
// that path to the open route got the backend's "Unsupported file type…" (its
// supported-type check also fails for a file that is not there), which sent
// the user looking for a format problem. The reopen now asks first whether the
// file is still there, and when it is not says so and offers to forget it.

#include "DatasetName.h"

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace visivo {

/// True for an entry whose file lives on the backend a reopen uses, so that
/// asking it (or this machine, for the local backend) whether the file is
/// there gives a true answer. A SKAVA entry is resolved through discovery
/// again; a SODA cutout ("soda") or a product computed on a SKAVA node
/// ("node") lives on that node, where nothing here can look; HiPS has no file.
/// SODA cutouts recorded before they had their own source are told by the
/// name they were given.
inline bool recentEntryNamesAFile(const QString &type, const QString &source,
                                  const QString &displayName = QString())
{
    if (!source.isEmpty() && source != QLatin1String("remote"))
        return false;
    if (displayName.endsWith(QLatin1String(" [SODA cutout]")))
        return false;
    return type == QLatin1String("cube") || type == QLatin1String("image")
            || type == QLatin1String("velocity") || type == QLatin1String("catalogue")
            || type == QLatin1String("vbt") || type == QLatin1String("vbt-volume");
}

/// The file to look for on this machine: the data file of a VBT field
/// reference ("<data>.bin::vbt-field=<n>"), the path itself otherwise.
inline QString recentEntryFile(const QString &id)
{
    return datasetFilePath(id);
}

enum class LocalFileState {
    Present,      ///< there (readable or not: the open says what is wrong)
    Missing,      ///< not there, and the folder that should hold it could be read
    Unreadable,   ///< cannot tell: a folder on the way may not be searched
};

/// Whether @p path is on this machine. A path under a folder we may not
/// search does not exist for QFileInfo either, and calling such a file
/// deleted would be wrong: the nearest folder that does exist must be one we
/// can read and search for "missing" to be the answer.
inline LocalFileState localFileState(const QString &path)
{
    const QFileInfo info(path);
    if (info.exists())
        return LocalFileState::Present;
    // Up by name (QDir::cdUp refuses to step into a folder that is not there).
    QString dir = info.absolutePath();
    while (!QFileInfo::exists(dir)) {
        const QString parent = QFileInfo(dir).absolutePath();
        if (parent == dir)
            break;   // the root
        dir = parent;
    }
    const QFileInfo folder(dir);
    return folder.isDir() && folder.isReadable() && folder.isExecutable()
            ? LocalFileState::Missing
            : LocalFileState::Unreadable;
}

/// The backend's classifier (POST /v1/files/classify) answers a path that is
/// not there, or lies outside the folders it may read, with this error; it
/// answers both alike so that it cannot be used to probe what exists.
inline bool classifierSaysNotFound(bool valid, const QString &error)
{
    return !valid && error == QLatin1String("File not found.");
}

/// What the box says. @p where is empty for this machine (the local backend
/// reads its files here), or the remote backend's name.
inline QString recentMissingText(const QString &displayName, const QString &path,
                                 const QString &where)
{
    if (where.isEmpty())
        return QStringLiteral("“%1” was not found: there is no file at\n%2\n\n"
                              "It may have been moved, renamed or deleted.")
                .arg(displayName, path);
    return QStringLiteral("“%1” was not found on %2: there is no file at\n%3\n"
                          "or it lies outside the folders that backend may read.")
            .arg(displayName, where, path);
}

}   // namespace visivo
