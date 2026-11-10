#ifndef VBTBYTEORDER_H
#define VBTBYTEORDER_H

// The byte order a VisIVO Binary Table is read in (issue #165).
//
// Some converters write a VBT's data in one byte order and its `.head` in the
// other; read as declared, every value is absurd. The backend's open answer
// then carries `byte_order_warning` (app/vbt_byte_order.py), and the user may
// open the table in the other order: the file is never rewritten. A choice
// made with a button is remembered here, per backend and data file, for the
// life of the client, and the opens that follow on their own (the reopen after
// the question, a reconnect after a session expiry, another field of the same
// volume) send it as `byte_order`. An open the user starts (a new window, a
// recent entry) sends nothing: the backend reads the file as it was last told
// and warns again, so the question comes back and the choice can be undone.
//
// Widget-free, so the parsing, the wording and the memory are testable.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QString>

namespace visivo {

struct VbtByteOrderWarning
{
    bool present{ false };
    QString declared;     ///< "little" | "big": the order the header declares
    QString suggested;    ///< the other one, which the values look like
    QString confidence;   ///< "overwhelming" | "strong"
    double implausibleDeclared{ 0.0 };   ///< share of sampled values absurd as declared
    double implausibleSuggested{ 0.0 };  ///< the same in the suggested order
    QString field;            ///< the first sampled field, with its first value
    QString firstDeclared;    ///< read as declared
    QString firstSuggested;   ///< read in the suggested order
};

/// The `byte_order_warning` of an open answer; `present` false when it is
/// absent or null (a correct table, a backend older than #165).
inline VbtByteOrderWarning parseVbtByteOrderWarning(const QJsonValue &value)
{
    VbtByteOrderWarning w;
    if (!value.isObject())
        return w;
    const QJsonObject o = value.toObject();
    w.declared = o.value(QStringLiteral("declared")).toString();
    w.suggested = o.value(QStringLiteral("suggested")).toString();
    w.confidence = o.value(QStringLiteral("confidence")).toString();
    const auto valid = [](const QString &s) {
        return s == QLatin1String("little") || s == QLatin1String("big");
    };
    if (!valid(w.declared) || !valid(w.suggested) || w.declared == w.suggested)
        return VbtByteOrderWarning{};
    const QJsonObject evidence = o.value(QStringLiteral("evidence")).toObject();
    w.implausibleDeclared = evidence.value(QStringLiteral("implausible_declared")).toDouble();
    w.implausibleSuggested = evidence.value(QStringLiteral("implausible_suggested")).toDouble();
    const QJsonValue fields = evidence.value(QStringLiteral("fields"));
    if (fields.isArray() && !fields.toArray().isEmpty()) {
        const QJsonObject first = fields.toArray().at(0).toObject();
        w.field = first.value(QStringLiteral("name")).toString();
        w.firstDeclared = first.value(QStringLiteral("first_declared")).toString();
        w.firstSuggested = first.value(QStringLiteral("first_suggested")).toString();
    }
    w.present = true;
    return w;
}

/// The question, as the user reads it.
inline QString vbtByteOrderQuestion(const VbtByteOrderWarning &w)
{
    return QStringLiteral("This table's header says %1-endian, but its values look %2-endian.")
            .arg(w.declared, w.suggested);
}

/// What the question rests on: the share of absurd values either way, and
/// the first value of the first field read both ways.
inline QString vbtByteOrderEvidence(const VbtByteOrderWarning &w)
{
    QString text = QStringLiteral("%1% of the sampled values are not plausible read as "
                                  "%2-endian; %3% read as %4-endian.")
                           .arg(QString::number(100.0 * w.implausibleDeclared, 'f', 0), w.declared,
                                QString::number(100.0 * w.implausibleSuggested, 'f', 0),
                                w.suggested);
    if (!w.field.isEmpty())
        text += QStringLiteral("\nFirst %1: %2 as declared, %3 as %4-endian.")
                        .arg(w.field, w.firstDeclared, w.firstSuggested, w.suggested);
    text += QStringLiteral("\nThe file is not changed either way.");
    return text;
}

/// The question as the box shows it, as rich text: the file's name on a bold
/// first line (a macOS message box shows no window title, so the title alone
/// would never be seen), then the question.
inline QString vbtByteOrderBoxText(const VbtByteOrderWarning &w, const QString &fileName)
{
    return QStringLiteral("<b>Byte order of %1</b><br>%2")
            .arg(fileName.toHtmlEscaped(), vbtByteOrderQuestion(w).toHtmlEscaped());
}

/// The evidence as rich text, for the same box.
inline QString vbtByteOrderEvidenceHtml(const VbtByteOrderWarning &w)
{
    return vbtByteOrderEvidence(w).toHtmlEscaped().replace(QLatin1Char('\n'),
                                                           QStringLiteral("<br>"));
}

/// Whether the suggested order is the default answer: on overwhelming
/// evidence (at least 40% of the sampled values absurd as declared, at most
/// one in a hundred in the other order, over two or more fields), or when the
/// table is already read in it (@p readIn, a choice made before). On weaker
/// evidence the header stays the default: it is the file's own statement, and
/// a wrong default there would read a correct table backwards.
inline bool vbtByteOrderDefaultsToSuggested(const VbtByteOrderWarning &w,
                                            const QString &readIn = {})
{
    return w.present
            && (w.confidence == QLatin1String("overwhelming") || readIn == w.suggested);
}

/// The data file a VBT path names: a `.head`, the data, or a field reference
/// (`<data>::vbt-field=N`) all name the same file.
inline QString vbtDataPath(const QString &path)
{
    QString data = path;
    const qsizetype ref = data.lastIndexOf(QLatin1String("::vbt-field="));
    if (ref >= 0)
        data.truncate(ref);
    if (data.endsWith(QLatin1String(".head")))
        data.chop(5);
    return data;
}

namespace detail {
struct VbtByteOrderMemory
{
    QMutex mutex;
    QHash<QString, QString> byBackendAndPath;   // url + '\n' + data path -> order
    /// session id + '\n' + data path -> (read in, declared), from open answers
    QHash<QString, QPair<QString, QString>> readBySession;
};
inline VbtByteOrderMemory &vbtByteOrderMemory()
{
    static VbtByteOrderMemory memory;
    return memory;
}
} // namespace detail

/// Remember @p order ("little" | "big") for the VBT @p path on @p backendUrl;
/// an empty order forgets it.
inline void rememberVbtByteOrder(const QString &backendUrl, const QString &path,
                                 const QString &order)
{
    auto &m = detail::vbtByteOrderMemory();
    const QString data = vbtDataPath(path);
    const QString key = backendUrl + QLatin1Char('\n') + data;
    QMutexLocker lock(&m.mutex);
    if (order.isEmpty())
        m.byBackendAndPath.remove(key);
    else
        m.byBackendAndPath.insert(key, order);
}

/// The order chosen for @p path on @p backendUrl, or empty: send nothing.
inline QString rememberedVbtByteOrder(const QString &backendUrl, const QString &path)
{
    auto &m = detail::vbtByteOrderMemory();
    QMutexLocker lock(&m.mutex);
    return m.byBackendAndPath.value(backendUrl + QLatin1Char('\n') + vbtDataPath(path));
}

/// Note what a VBT open answered on backend session @p sessionId: the order
/// @p path is read in and the one its header declares (empty: not a VBT).
inline void noteVbtReadOrder(const QString &sessionId, const QString &path,
                             const QString &readIn, const QString &declared)
{
    if (sessionId.isEmpty() || readIn.isEmpty() || declared.isEmpty())
        return;
    auto &m = detail::vbtByteOrderMemory();
    QMutexLocker lock(&m.mutex);
    m.readBySession.insert(sessionId + QLatin1Char('\n') + vbtDataPath(path),
                           { readIn, declared });
}

/// (read in, declared) of the VBT @p path as the last open on backend session
/// @p sessionId answered; empty strings when it was not a VBT. What a recipe
/// records: the backend's answer, on that backend.
inline QPair<QString, QString> vbtReadOrder(const QString &sessionId, const QString &path)
{
    auto &m = detail::vbtByteOrderMemory();
    QMutexLocker lock(&m.mutex);
    return m.readBySession.value(sessionId + QLatin1Char('\n') + vbtDataPath(path));
}

} // namespace visivo

#endif // VBTBYTEORDER_H
