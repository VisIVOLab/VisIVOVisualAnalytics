#pragma once

// How a dataset's path is shown and named (issue #83). A field of a VisIVO
// Binary Table volume opens as a cube whose path is a reference to the field,
// "<data>.bin::vbt-field=<index>" (backend/app/volume_source.py), and a
// particle type of a Gadget snapshot as a table of points whose path is
// "<first file>::gadget-type=<type>" (issue #31, backend/app/gadget_snapshot.py):
// the viewers use either as any path for the backend, and show it as
// "<volume> · <field>", "<snapshot> · <particle type>".

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

inline const QString &gadgetTypeSeparator()
{
    static const QString sep = QStringLiteral("::gadget-type=");
    return sep;
}

namespace detail {
/// Where a reference's separator starts in @p path and which one it is; -1
/// for a path that is no reference.
inline int referenceAt(const QString &path, const QString **separator = nullptr)
{
    for (const QString *sep : { &vbtFieldSeparator(), &gadgetTypeSeparator() }) {
        const int at = path.lastIndexOf(*sep);
        if (at >= 0) {
            if (separator)
                *separator = sep;
            return at;
        }
    }
    return -1;
}
} // namespace detail

/// The data file of a reference (a VBT field, a snapshot's particle type);
/// the path itself for any other dataset.
inline QString datasetFilePath(const QString &path)
{
    const int at = detail::referenceAt(path);
    return at < 0 ? path : path.left(at);
}

/// A reference to part of a file (a VBT field, a snapshot's particle type):
/// no file another tool could load as it is.
inline bool isFieldReference(const QString &path)
{
    return detail::referenceAt(path) >= 0;
}

inline QHash<QString, QString> &fieldNames()
{
    static QHash<QString, QString> names;   // reference → field name (GUI thread)
    return names;
}

/// The field names of a VBT volume, or the particle types of a snapshot, as
/// the backend listed them at open: (name, reference) pairs.
inline void rememberFieldNames(const QList<QPair<QString, QString>> &fields)
{
    for (const auto &f : fields)
        fieldNames().insert(f.second, f.first);
}

/// Datasets another application sent (SAMP), staged on the backend under a
/// hashed name that means nothing to the user: path → the sender's name for
/// it (GUI thread). They are temporary copies the backend deletes later.
inline QHash<QString, QString> &receivedNames()
{
    static QHash<QString, QString> names;
    return names;
}

/// Record @p path as a received copy; @p name may be empty (a sender that
/// gave none), and then the file's name is shown.
inline void rememberReceivedName(const QString &path, const QString &name)
{
    if (!path.isEmpty())
        receivedNames().insert(path, name.trimmed());
}

inline bool isReceivedCopy(const QString &path)
{
    return receivedNames().contains(path);
}

/// What an Inspector's Path row says: the path, or for a received copy what
/// it is, then the path (elided in the middle, whole in the tooltip).
inline QString datasetPathLabel(const QString &path)
{
    return isReceivedCopy(path)
            ? QStringLiteral("temporary copy received over SAMP: %1").arg(path)
            : path;
}

/// The name a dataset is shown with: the sender's for a received copy, its
/// file name, "volume · field" or "snapshot · particle type".
inline QString datasetDisplayName(const QString &path)
{
    if (const auto it = receivedNames().constFind(path);
        it != receivedNames().constEnd() && !it.value().isEmpty())
        return it.value();
    const QString *separator = nullptr;
    const int at = detail::referenceAt(path, &separator);
    if (at < 0)
        return QFileInfo(path).fileName();
    QString volume = QFileInfo(path.left(at)).fileName();
    if (volume.endsWith(QLatin1String(".bin")))
        volume.chop(4);
    // Before the backend listed the names, the index.
    const QString what = separator == &gadgetTypeSeparator() ? QStringLiteral("type %1")
                                                             : QStringLiteral("field %1");
    const QString field = fieldNames().value(path, what.arg(path.mid(at + separator->size())));
    return QStringLiteral("%1 · %2").arg(volume, field);
}

/// A path whose completeBaseName() names what is derived from the dataset
/// (exports): the file's own, or "volume_field" for a field ("snap_010_0_gas"
/// for a snapshot's particle type), in its folder. No dot: completeBaseName()
/// would cut the name at it.
inline QString datasetExportStem(const QString &path)
{
    if (!isFieldReference(path))
        return path;
    const QFileInfo data(datasetFilePath(path));
    QString stem = datasetDisplayName(path);
    stem.replace(QStringLiteral(" · "), QStringLiteral("_"));
    for (QChar &c : stem)
        if (!(c.isLetterOrNumber() || c == u'_' || c == u'-'))
            c = u'_';
    return data.dir().filePath(stem);
}

} // namespace visivo
