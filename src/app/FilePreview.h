#pragma once

// FilePreview.h
// What the file browser's details pane shows for the selected file (issue
// #159): the reply of POST /v1/files/preview, read into one struct, and the
// words around it. The backend reads only the start of the file (byte- and
// line-capped), so a preview is cheap on a remote backend too:
//
//   {path, size_bytes, kind, format, text, truncated}
//   kind: fits | text | vbt | hdf5 | compressed | binary | directory
//
// Errors are plain HTTP statuses with FastAPI's "detail": 403 outside the data
// roots, 404 a missing file ("File not found."), 422 bad arguments, 500 a read
// error. An older backend has no such route. It answers 404 for the route itself, and
// then the browser falls back to the FITS header route for a FITS file, as it
// did before. Widget-free, so the parsing is unit-tested.

#include "MissingRoute.h"   // isMissingRouteReply

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace visivo {

struct FilePreview
{
    bool valid{ false };
    QString error;
    QString path;
    qint64 sizeBytes{ -1 };   ///< -1: not given
    QString kind;             ///< fits, text, vbt, hdf5, compressed, binary, directory
    QString format;           ///< short label: "CSV table", "FITS image", "HDF5 file"…
    QString text;             ///< what to show, monospace
    bool truncated{ false };  ///< only the start of the file is in `text`
    /// The backend has no preview route at all (an older one): the caller
    /// may fall back to the FITS header route.
    bool routeMissing{ false };
};

/// What the browser does when the backend has no preview route.
enum class PreviewFallback {
    None,          ///< the route answered: show its reply
    Folder,        ///< a folder: say so, as before the route existed
    FitsHeader,    ///< a FITS file: the older /v1/files/header route
    Unsupported,   ///< anything else: this backend previews FITS headers only
};

inline PreviewFallback previewFallback(bool routeMissing, bool isFolder, bool isFits)
{
    if (!routeMissing)
        return PreviewFallback::None;
    if (isFolder)
        return PreviewFallback::Folder;
    return isFits ? PreviewFallback::FitsHeader : PreviewFallback::Unsupported;
}

/// The preview of a folder on a backend without the route.
inline FilePreview folderPlaceholder(const QString &path)
{
    FilePreview p;
    p.valid = true;
    p.path = path;
    p.kind = QStringLiteral("directory");
    return p;
}

/// The answer for a file other than FITS on a backend without the route.
inline FilePreview unsupportedOnOldBackend(const QString &path)
{
    FilePreview p;
    p.path = path;
    p.error = QStringLiteral("This backend previews FITS headers only; a newer backend "
                             "previews every file.");
    return p;
}

/// The message of an error reply: its `detail` (or `error`) when the body is
/// JSON with one, the body as it is otherwise.
inline QString replyErrorText(const QByteArray &body)
{
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    // A 422 from FastAPI's validation: a list of problems, each with a "msg".
    const QJsonValue detail = object.value(u"detail");
    if (detail.isArray()) {
        QStringList messages;
        for (const QJsonValue &item : detail.toArray()) {
            const QString msg = item.toObject().value(u"msg").toString().trimmed();
            if (!msg.isEmpty())
                messages << msg;
        }
        if (!messages.isEmpty())
            return messages.join(u"; ");
    }
    for (const char *key : { "detail", "error" }) {
        const QJsonValue v = object.value(QLatin1StringView(key));
        if (v.isString() && !v.toString().trimmed().isEmpty())
            return v.toString().trimmed();
    }
    return QString::fromUtf8(body).trimmed();
}

/// Read a 2xx reply of /v1/files/preview. A body without `kind` is not a
/// preview: its `error`/`detail`, if any, says why.
inline FilePreview parseFilePreviewReply(const QByteArray &body)
{
    FilePreview p;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        p.error = QStringLiteral("The backend's preview reply is not JSON.");
        return p;
    }
    const QJsonObject o = doc.object();
    p.path = o.value(u"path").toString();
    p.kind = o.value(u"kind").toString().trimmed().toLower();
    p.format = o.value(u"format").toString().trimmed();
    p.text = o.value(u"text").toString();
    p.truncated = o.value(u"truncated").toBool(false);
    const QJsonValue size = o.value(u"size_bytes");
    if (size.isDouble())
        p.sizeBytes = size.toInteger(-1);
    if (p.kind.isEmpty()) {
        const QString why = replyErrorText(body);
        p.error = why.isEmpty() || why.startsWith(u'{')
                ? QStringLiteral("The backend sent no preview for this file.")
                : why;
        return p;
    }
    p.valid = true;
    return p;
}

/// A FITS header from the older /v1/files/header route, as a preview.
inline FilePreview previewFromHeaderCards(const QString &path, const QStringList &cards)
{
    FilePreview p;
    p.valid = true;
    p.path = path;
    p.kind = QStringLiteral("fits");
    p.format = QStringLiteral("FITS header");
    p.text = cards.join(u'\n');
    return p;
}

/// The caption above the preview text: the format, and a note when only the
/// start of the file is shown.
inline QString filePreviewCaption(const FilePreview &p)
{
    QString caption = p.format;
    if (caption.isEmpty()) {
        if (p.kind == u"directory")
            caption = QStringLiteral("Folder");
        else if (p.kind == u"binary")
            caption = QStringLiteral("Binary file");
        else
            caption = QStringLiteral("Preview");
    }
    if (p.truncated)
        caption += QStringLiteral(" · start of the file only");
    return caption;
}

/// The text to show when the reply has none: a binary or compressed file
/// whose content cannot be shown as text.
inline QString filePreviewBody(const FilePreview &p)
{
    if (!p.text.isEmpty())
        return p.text;
    if (p.kind == u"directory")
        return QStringLiteral("Folder selected.");
    if (p.kind == u"compressed")
        return QStringLiteral("Compressed file: its content is shown once it is opened.");
    if (p.kind == u"binary")
        return QStringLiteral("Binary file: no text preview.");
    return QStringLiteral("The file is empty.");
}

}  // namespace visivo
