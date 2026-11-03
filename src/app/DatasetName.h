#pragma once

// How a dataset's path is shown and named (issue #83). A field of a VisIVO
// Binary Table volume opens as a cube whose path is a reference to the field,
// "<data>.bin::vbt-field=<index>" (backend/app/volume_source.py): the viewers
// use it as any path for the backend, and show it as "<volume> · <field>".

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

namespace visivo {

inline const QString &vbtFieldSeparator()
{
    static const QString sep = QStringLiteral("::vbt-field=");
    return sep;
}

/// The data file of a field reference; the path itself for any other dataset.
inline QString datasetFilePath(const QString &path)
{
    const int at = path.lastIndexOf(vbtFieldSeparator());
    return at < 0 ? path : path.left(at);
}

inline bool isFieldReference(const QString &path)
{
    return path.lastIndexOf(vbtFieldSeparator()) >= 0;
}

inline QHash<QString, QString> &fieldNames()
{
    static QHash<QString, QString> names;   // reference → field name (GUI thread)
    return names;
}

/// The field names of a VBT volume as the backend listed them at open.
inline void rememberFieldNames(const QList<QPair<QString, QString>> &fields)
{
    for (const auto &f : fields)
        fieldNames().insert(f.second, f.first);
}

/// The name a dataset is shown with: its file name, or "volume · field".
inline QString datasetDisplayName(const QString &path)
{
    if (!isFieldReference(path))
        return QFileInfo(path).fileName();
    QString volume = QFileInfo(datasetFilePath(path)).fileName();
    if (volume.endsWith(QLatin1String(".bin")))
        volume.chop(4);
    const QString field = fieldNames().value(
            path, QStringLiteral("field %1").arg(path.mid(path.lastIndexOf(vbtFieldSeparator())
                                                         + vbtFieldSeparator().size())));
    return QStringLiteral("%1 · %2").arg(volume, field);
}

/// A path whose completeBaseName() names what is derived from the dataset
/// (exports): the file's own, or "volume_field" for a field, in its folder.
inline QString datasetExportStem(const QString &path)
{
    if (!isFieldReference(path))
        return path;
    const QFileInfo data(datasetFilePath(path));
    QString stem = datasetDisplayName(path);
    stem.replace(QStringLiteral(" · "), QStringLiteral("_"));
    for (QChar &c : stem)
        if (!(c.isLetterOrNumber() || c == u'_' || c == u'-' || c == u'.'))
            c = u'_';
    return data.dir().filePath(stem);
}

} // namespace visivo
