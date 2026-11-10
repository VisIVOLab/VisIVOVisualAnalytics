// SampProtocol.h – SAMP messages, the client-side callback dispatch, and
// table/row identity, for VisIVO Visual Analytics
//
// Everything here is plain logic on QtCore (no network, no widgets), so the
// unit tests can drive it directly:
//
//  * decodeClientCall(): turns an XML-RPC call the hub made on our callback
//    server (samp.client.receiveNotification / receiveCall / receiveResponse)
//    into a SampInbound, after checking the private key;
//  * responseFor(): the samp.ok / samp.error answer a receiveCall gets back
//    through samp.hub.reply;
//  * row helpers for table.highlight.row and table.select.rowList;
//  * SampTableRegistry: which local catalogue a SAMP table (table-id and/or
//    url, SAMP 1.3 §A.4) is;
//  * SampRowMap: original-table row ↔ loaded entry, for a view that loaded a
//    filtered, sorted or sampled subset of the table.

#ifndef SAMPPROTOCOL_H
#define SAMPPROTOCOL_H

#include "SampXmlRpc.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

// ── Data types ────────────────────────────────────────────────────────────────

/// A single SAMP client registered in the Hub.
struct SampClientInfo
{
    QString id;           ///< Hub-assigned public ID (e.g. "c1")
    QString name;         ///< samp.name metadata string
    QString description;
    QString iconUrl;
    QStringList subscriptions;  ///< List of subscribed MTypes
};

/// An inbound SAMP message.
struct SampMessage
{
    QString mtype;
    QVariantMap params;
    QString senderId;
    QString msgId;   ///< set for a call (it needs a reply), empty for a notification
    /// Set for a call the GUI must answer (a load): pass it back to
    /// SampClient::replyToCall(). Unlike msgId it names the registration
    /// too, as a restarted hub may hand out the same msg-id again.
    QString replyToken;
};

Q_DECLARE_METATYPE(SampMessage)
Q_DECLARE_METATYPE(SampClientInfo)

namespace Samp {

// ── MTypes ────────────────────────────────────────────────────────────────────

inline constexpr const char *kImageLoadFits    = "image.load.fits";
inline constexpr const char *kTableLoadVotable = "table.load.votable";
inline constexpr const char *kTableLoadFits    = "table.load.fits";
inline constexpr const char *kCoordPointAtSky  = "coord.pointAt.sky";
inline constexpr const char *kTableHighlightRow = "table.highlight.row";
inline constexpr const char *kTableSelectRowList = "table.select.rowList";
inline constexpr const char *kAppPing          = "samp.app.ping";
inline constexpr const char *kHubShutdown      = "samp.hub.event.shutdown";
inline constexpr const char *kHubDisconnect    = "samp.hub.disconnect";
inline constexpr const char *kHubRegister      = "samp.hub.event.register";
inline constexpr const char *kHubUnregister    = "samp.hub.event.unregister";
inline constexpr const char *kHubMetadata      = "samp.hub.event.metadata";
inline constexpr const char *kHubSubscriptions = "samp.hub.event.subscriptions";

/// What VisIVO declares with samp.hub.declareSubscriptions.
inline QStringList subscribedMTypes()
{
    return {
        QString::fromLatin1(kTableLoadVotable),  QString::fromLatin1(kTableLoadFits),
        QString::fromLatin1(kImageLoadFits),     QString::fromLatin1(kCoordPointAtSky),
        QString::fromLatin1(kTableHighlightRow), QString::fromLatin1(kTableSelectRowList),
        QString::fromLatin1(kAppPing),           QString::fromLatin1(kHubShutdown),
        QString::fromLatin1(kHubDisconnect),     QString::fromLatin1(kHubRegister),
        QString::fromLatin1(kHubUnregister),     QString::fromLatin1(kHubMetadata),
        QString::fromLatin1(kHubSubscriptions),
    };
}

/// A load VisIVO answers only once it is done (or has failed): the sender of a
/// call learns whether the table or image really opened (SAMP 1.3 §3.9).
inline bool isLoadMType(const QString &mtype)
{
    return mtype == QLatin1String(kTableLoadVotable) || mtype == QLatin1String(kTableLoadFits)
           || mtype == QLatin1String(kImageLoadFits);
}

/// Does subscription @p pattern take @p mtype? SAMP 1.3 §3.4: an MType, or a
/// wildcard that ends a run of atoms with "*" ("table.*", or "*" for all).
inline bool mtypeMatches(const QString &pattern, const QString &mtype)
{
    if (pattern == u"*")
        return true;
    if (pattern.endsWith(u".*"))
        return mtype.startsWith(pattern.chopped(1));   // keeps the dot: "table."
    return pattern == mtype;
}

inline bool subscribes(const SampClientInfo &client, const QString &mtype)
{
    for (const QString &pattern : client.subscriptions) {
        if (mtypeMatches(pattern, mtype))
            return true;
    }
    return false;
}

/// One tool a viewer can send to, as a "Send to" menu names it.
struct Recipient
{
    QString id;
    QString label;
};

/// The tools among @p clients that take @p mtype, by name. Two tools of one
/// name (two TOPCATs) are told apart by their hub ids.
inline QList<Recipient> recipientsFor(const QList<SampClientInfo> &clients, const QString &mtype)
{
    QList<Recipient> out;
    QHash<QString, int> nameCount;
    for (const SampClientInfo &c : clients) {
        if (subscribes(c, mtype))
            ++nameCount[c.name.trimmed()];
    }
    for (const SampClientInfo &c : clients) {
        if (!subscribes(c, mtype))
            continue;
        const QString name = c.name.trimmed();
        QString label = name.isEmpty() ? c.id : name;
        if (!name.isEmpty() && nameCount.value(name) > 1)
            label += QStringLiteral(" [%1]").arg(c.id);
        out.append({ c.id, label });
    }
    std::stable_sort(out.begin(), out.end(), [](const Recipient &a, const Recipient &b) {
        return a.label.compare(b.label, Qt::CaseInsensitive) < 0;
    });
    return out;
}

/// coord.pointAt.sky's parameters: ICRS degrees as strings, RA in [0, 360).
/// Nullopt for a position that is not one (not finite, |Dec| > 90).
inline std::optional<QVariantMap> pointAtSkyParams(double raDeg, double decDeg)
{
    if (!std::isfinite(raDeg) || !std::isfinite(decDeg) || std::abs(decDeg) > 90.0)
        return std::nullopt;
    double ra = std::fmod(raDeg, 360.0);
    if (ra < 0.0)
        ra += 360.0;
    if (ra >= 360.0)
        ra = 0.0;   // fmod of a value just below 0 can round up to 360
    QVariantMap p;
    // Eight decimals of a degree: 0.04 mas, below any catalogue's precision.
    p.insert(QStringLiteral("ra"), QString::number(ra, 'f', 8));
    p.insert(QStringLiteral("dec"), QString::number(decDeg, 'f', 8));
    return p;
}

/// The hub's location and the registration secret, from a Standard Profile
/// lockfile (SAMP 1.3 §3.3: "key=value" lines, "#" comments).
struct LockInfo
{
    QString hubUrl;
    QString secret;
};

inline LockInfo parseLockFile(const QString &text)
{
    LockInfo info;
    const QStringList lines = text.split(u'\n');
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        const qsizetype eq = line.indexOf(u'=');
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (key == u"samp.hub.xmlrpc.url")
            info.hubUrl = value;
        else if (key == u"samp.secret")
            info.secret = value;
    }
    return info;
}

/// Hub lifecycle messages: handled by the client itself, never by a viewer.
inline bool isHubEvent(const QString &mtype)
{
    return mtype.startsWith(u"samp.hub.") || mtype == QLatin1String(kAppPing);
}

/// @p v in the Standard Profile's types: maps and lists kept, every scalar
/// a string (SAMP 1.3 §3.3). A boolean becomes "1" or "0", the SAMP
/// convention; a null becomes the empty string.
inline QVariant toSampValue(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::QVariantMap: {
        QVariantMap out;
        const QVariantMap in = v.toMap();
        for (auto it = in.constBegin(); it != in.constEnd(); ++it)
            out.insert(it.key(), toSampValue(it.value()));
        return out;
    }
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        QVariantList out;
        for (const QVariant &item : v.toList())
            out.append(toSampValue(item));
        return out;
    }
    case QMetaType::Bool:
        return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    case QMetaType::Double:
    case QMetaType::Float:
        return QString::number(v.toDouble(), 'g', 17);
    default:
        return v.toString();
    }
}

// ── Row indices ───────────────────────────────────────────────────────────────

/// A SAMP row index: 0-based, sent as a string ("row" of table.highlight.row,
/// each entry of "row-list"). A hub may hand an integer through; both are
/// accepted. Returns nullopt for anything that is not a non-negative integer.
inline std::optional<qint64> parseRowIndex(const QVariant &v)
{
    bool ok = false;
    qint64 row = -1;
    switch (v.typeId()) {
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        row = v.toLongLong(&ok);
        break;
    default:
        row = v.toString().trimmed().toLongLong(&ok);
        break;
    }
    if (!ok || row < 0)
        return std::nullopt;
    return row;
}

/// table.select.rowList's "row-list". Nullopt when it is missing or any
/// entry is not a row index: a half-understood selection is worse than none.
inline std::optional<QList<qint64>> parseRowList(const QVariant &v)
{
    if (v.typeId() != QMetaType::QVariantList && v.typeId() != QMetaType::QStringList)
        return std::nullopt;
    QList<qint64> rows;
    const QVariantList list = v.toList();
    rows.reserve(list.size());
    for (const QVariant &item : list) {
        const auto row = parseRowIndex(item);
        if (!row)
            return std::nullopt;
        rows.append(*row);
    }
    return rows;
}

/// A table as SAMP names it: by table-id, by url, or both (SAMP 1.3 §A.4:
/// the receiver matches either).
struct TableRef
{
    QString tableId;
    QString url;
    bool isEmpty() const { return tableId.isEmpty() && url.isEmpty(); }
};

inline TableRef tableRefFrom(const QVariantMap &params)
{
    return { params.value(QStringLiteral("table-id")).toString().trimmed(),
             params.value(QStringLiteral("url")).toString().trimmed() };
}

inline QVariantMap highlightRowParams(const TableRef &table, qint64 row)
{
    QVariantMap p;
    if (!table.tableId.isEmpty())
        p.insert(QStringLiteral("table-id"), table.tableId);
    if (!table.url.isEmpty())
        p.insert(QStringLiteral("url"), table.url);
    p.insert(QStringLiteral("row"), QString::number(row));
    return p;
}

inline QVariantMap selectRowListParams(const TableRef &table, const QList<qint64> &rows)
{
    QVariantMap p;
    if (!table.tableId.isEmpty())
        p.insert(QStringLiteral("table-id"), table.tableId);
    if (!table.url.isEmpty())
        p.insert(QStringLiteral("url"), table.url);
    QVariantList list;
    list.reserve(rows.size());
    for (const qint64 r : rows)
        list.append(QString::number(r));
    p.insert(QStringLiteral("row-list"), list);
    return p;
}

// ── Message validation ────────────────────────────────────────────────────────

/// Why VisIVO cannot act on @p msg, or an empty string when it can. This is
/// what a receiveCall is answered with (samp.error carries the text), so it
/// checks what the handlers need and nothing more.
inline QString validateMessage(const SampMessage &msg)
{
    const QString &mt = msg.mtype;
    const QVariantMap &p = msg.params;
    if (mt == QLatin1String(kTableLoadVotable) || mt == QLatin1String(kTableLoadFits)
        || mt == QLatin1String(kImageLoadFits)) {
        if (p.value(QStringLiteral("url")).toString().trimmed().isEmpty())
            return QStringLiteral("%1 without a url").arg(mt);
        return {};
    }
    if (mt == QLatin1String(kCoordPointAtSky)) {
        bool raOk = false;
        bool decOk = false;
        const double ra = p.value(QStringLiteral("ra")).toString().toDouble(&raOk);
        const double dec = p.value(QStringLiteral("dec")).toString().toDouble(&decOk);
        if (!raOk || !decOk || !std::isfinite(ra) || !std::isfinite(dec)
            || ra < 0.0 || ra > 360.0 || dec < -90.0 || dec > 90.0)
            return QStringLiteral("coord.pointAt.sky needs ra in [0, 360] and dec in [-90, 90]");
        return {};
    }
    if (mt == QLatin1String(kTableHighlightRow)) {
        if (tableRefFrom(p).isEmpty())
            return QStringLiteral("table.highlight.row without a table-id or url");
        if (!parseRowIndex(p.value(QStringLiteral("row"))))
            return QStringLiteral("table.highlight.row without a valid row");
        return {};
    }
    if (mt == QLatin1String(kTableSelectRowList)) {
        if (tableRefFrom(p).isEmpty())
            return QStringLiteral("table.select.rowList without a table-id or url");
        if (!parseRowList(p.value(QStringLiteral("row-list"))))
            return QStringLiteral("table.select.rowList without a valid row-list");
        return {};
    }
    if (isHubEvent(mt))
        return {};
    return QStringLiteral("VisIVO does not handle %1").arg(mt);
}

/// A SAMP response map (samp.hub.reply's third argument): samp.ok when
/// @p error is empty, samp.error carrying it otherwise.
inline QVariantMap response(const QString &error = QString())
{
    QVariantMap r;
    if (error.isEmpty()) {
        r.insert(QStringLiteral("samp.status"), QStringLiteral("samp.ok"));
        r.insert(QStringLiteral("samp.result"), QVariantMap());
    } else {
        r.insert(QStringLiteral("samp.status"), QStringLiteral("samp.error"));
        QVariantMap err;
        err.insert(QStringLiteral("samp.errortxt"), error);
        r.insert(QStringLiteral("samp.error"), err);
    }
    return r;
}

/// A recipient's answer as an error text: empty for samp.ok (and for
/// samp.warning, which still did the job), else samp.errortxt or a stand-in.
inline QString responseError(const QVariantMap &response)
{
    const QString status = response.value(QStringLiteral("samp.status")).toString();
    if (status == u"samp.ok" || status == u"samp.warning")
        return {};
    const QString text = response.value(QStringLiteral("samp.error")).toMap()
                                 .value(QStringLiteral("samp.errortxt")).toString().trimmed();
    if (!text.isEmpty())
        return text;
    return status.isEmpty() ? QStringLiteral("no status in the answer")
                            : QStringLiteral("answered %1").arg(status);
}

/// The response to a call on @p msg judged on its parameters alone.
inline QVariantMap responseFor(const SampMessage &msg)
{
    return response(validateMessage(msg));
}

// ── Received files ────────────────────────────────────────────────────────────

/// What the hub status says, as one comparable string: the connection and
/// each peer's id, name and subscriptions (in any order). A refresh that finds
/// the same is not reported again: the hub sends VisIVO events about its own
/// registration right after it connects, and each used to log the same peer
/// list once more, in VisIVO and in the backend.
inline QString hubStatusKey(bool connected, const QString &hubUrl, const QString &selfId,
                            const QList<SampClientInfo> &clients)
{
    QStringList peers;
    peers.reserve(clients.size());
    for (const SampClientInfo &c : clients) {
        QStringList subs = c.subscriptions;
        subs.sort();
        peers.append(c.id + QChar(0x1f) + c.name + QChar(0x1f) + subs.join(QChar(0x1e)));
    }
    peers.sort();
    return QStringList{ connected ? QStringLiteral("1") : QStringLiteral("0"), hubUrl, selfId,
                        peers.join(QChar(0x1d)) }
            .join(QChar(0x1c));
}

/// What to call a received table or image: the sender's "name", else the
/// last segment of its URL (decoded), else empty.
inline QString displayName(const QString &url, const QString &name)
{
    if (!name.trimmed().isEmpty())
        return name.trimmed();
    const QString path = QUrl(url).path();
    return path.mid(path.lastIndexOf(u'/') + 1).trimmed();
}

/// Does @p fileName end as the backend expects a file of @p mtype to? A
/// catalogue's format is read from its extension (CatalogueFormat.h:
/// .vot/.votable/.xml, .fits/.fit); an image may also be compressed.
inline bool hasFormatExtension(const QString &fileName, const QString &mtype)
{
    const QString lower = fileName.toLower();
    auto endsWithAny = [&lower](std::initializer_list<const char *> endings) {
        for (const char *ending : endings) {
            if (lower.endsWith(QLatin1String(ending)))
                return true;
        }
        return false;
    };
    if (mtype == QLatin1String(kTableLoadVotable))
        return endsWithAny({ ".vot", ".votable", ".xml" });
    if (mtype == QLatin1String(kTableLoadFits))
        return endsWithAny({ ".fits", ".fit" });
    if (mtype == QLatin1String(kImageLoadFits))
        return endsWithAny({ ".fits", ".fit", ".fts", ".fits.gz", ".fit.gz", ".fits.fz", ".fz" });
    return true;
}

/// The MType a catalogue of @p format goes out as. It mirrors the backend's
/// /v1/samp/send-catalogue: a FITS table stays FITS, everything else (CSV,
/// VOTable, IPAC, .speck) is sent as a VOTable.
inline QString catalogueLoadMType(const QString &format)
{
    const QString f = format.trimmed().toLower();
    if (f == u"fits" || f == u"fit" || f == u"fits-table")
        return QString::fromLatin1(kTableLoadFits);
    return QString::fromLatin1(kTableLoadVotable);
}

/// The file name to stage a received table or image under. The backend tells
/// a catalogue's format by its extension, and a hub's URL often has none
/// (TOPCAT serves ".../dynamic/4/t0"), so the MType decides it when the name
/// does not: table.load.votable → .vot, table/image.load.fits → .fits.
inline QString stagedFileName(const QString &url, const QString &name, const QString &mtype)
{
    auto sanitize = [](const QString &raw) {
        QString out;
        out.reserve(raw.size());
        for (const QChar c : raw) {
            const bool keep = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')
                              || (c >= u'0' && c <= u'9') || c == u'.' || c == u'-' || c == u'_';
            out.append(keep ? c : u'_');
        }
        while (out.startsWith(u'.'))
            out.remove(0, 1);   // no hidden files, no ".."
        return out.left(120);
    };
    const QString path = QUrl(url).path();
    QString base = sanitize(path.mid(path.lastIndexOf(u'/') + 1));
    if (base.isEmpty())
        base = sanitize(name.trimmed());
    const bool votable = mtype == QLatin1String(kTableLoadVotable);
    if (base.isEmpty())
        base = votable ? QStringLiteral("samp-table") : QStringLiteral("samp-data");

    if (!hasFormatExtension(base, mtype))
        base += votable ? QStringLiteral(".vot") : QStringLiteral(".fits");
    return base;
}

// ── Callback dispatch ─────────────────────────────────────────────────────────

/// One call the hub made on VisIVO's callback server.
struct Inbound
{
    enum class Kind { Notification, Call, Response };
    Kind kind{ Kind::Notification };
    SampMessage message;     ///< Notification / Call
    QString responderId;     ///< Response
    QString msgTag;          ///< Response
    QVariantMap response;    ///< Response
    QString error;           ///< non-empty: answer the hub with an XML-RPC fault
};

/// Decode a samp.client.* callback. The first argument of each is the private
/// key the hub gave VisIVO at registration: anything else on the loopback port
/// is refused, so another local process cannot inject messages.
inline Inbound decodeClientCall(const SampXmlRpc::Call &call, const QString &privateKey)
{
    Inbound in;
    if (!call.isValid()) {
        in.error = call.error.isEmpty() ? QStringLiteral("malformed call") : call.error;
        return in;
    }
    const QVariantList &p = call.params;
    if (privateKey.isEmpty() || p.isEmpty() || p.first().toString() != privateKey) {
        in.error = QStringLiteral("Bad private key");
        return in;
    }
    auto readMessage = [&in](const QVariant &v) {
        const QVariantMap m = v.toMap();
        in.message.mtype = m.value(QStringLiteral("samp.mtype")).toString().trimmed();
        in.message.params = m.value(QStringLiteral("samp.params")).toMap();
        if (in.message.mtype.isEmpty())
            in.error = QStringLiteral("message without samp.mtype");
    };
    if (call.method == u"samp.client.receiveNotification") {
        if (p.size() != 3) {
            in.error = QStringLiteral("receiveNotification takes 3 arguments");
            return in;
        }
        in.kind = Inbound::Kind::Notification;
        in.message.senderId = p.at(1).toString();
        readMessage(p.at(2));
    } else if (call.method == u"samp.client.receiveCall") {
        if (p.size() != 4) {
            in.error = QStringLiteral("receiveCall takes 4 arguments");
            return in;
        }
        in.kind = Inbound::Kind::Call;
        in.message.senderId = p.at(1).toString();
        in.message.msgId = p.at(2).toString();
        if (in.message.msgId.isEmpty())
            in.error = QStringLiteral("receiveCall without a msg-id");
        else
            readMessage(p.at(3));
    } else if (call.method == u"samp.client.receiveResponse") {
        if (p.size() != 4) {
            in.error = QStringLiteral("receiveResponse takes 4 arguments");
            return in;
        }
        in.kind = Inbound::Kind::Response;
        in.responderId = p.at(1).toString();
        in.msgTag = p.at(2).toString();
        in.response = p.at(3).toMap();
    } else {
        in.error = QStringLiteral("No such method: %1").arg(call.method);
    }
    return in;
}

}  // namespace Samp

// ── Calls the GUI answers ─────────────────────────────────────────────────────

/// The load calls still waiting for the GUI's answer. Each is known by a token
/// that joins the registration (the worker's connection generation) to the
/// hub's msg-id: a hub restarted while a load was in progress can reuse the
/// msg-id, and the old load's completion must not answer the new call.
class SampPendingCalls
{
public:
    /// Record a call of @p registration; returns its token.
    QString add(quint64 registration, const QString &msgId)
    {
        const QString token = QString::number(registration) + u'/' + msgId;
        m_calls.insert(token, msgId);
        return token;
    }

    /// The msg-id to reply to, once: only for a token of @p currentRegistration
    /// still pending. Nullopt for an unknown, answered or stale token.
    std::optional<QString> take(const QString &token, quint64 currentRegistration)
    {
        const qsizetype slash = token.indexOf(u'/');
        bool ok = false;
        const quint64 registration = token.left(slash).toULongLong(&ok);
        if (slash <= 0 || !ok || registration != currentRegistration)
            return std::nullopt;
        const auto it = m_calls.constFind(token);
        if (it == m_calls.constEnd())
            return std::nullopt;
        const QString msgId = it.value();
        m_calls.erase(it);
        return msgId;
    }

    void clear() { m_calls.clear(); }
    qsizetype size() const { return m_calls.size(); }

private:
    QHash<QString, QString> m_calls;   ///< token → msg-id
};

// ── Reconnecting ──────────────────────────────────────────────────────────────

/// Whether VisIVO should be trying, quietly, to reach a hub: one that went
/// away, or none running yet when the user asked to connect (TOPCAT started
/// after VisIVO). The worker's signals are queued: one can reach the GUI after
/// the user chose Disconnect, and must not then start the retries again.
class SampReconnectPolicy
{
public:
    /// Connect To Hub: its first failure is reported, then it keeps trying.
    void userConnects()
    {
        m_userDisconnected = false;
        m_waiting = false;
    }
    /// Disconnect From Hub: no retries, whatever hub loss is still in flight.
    void userDisconnects()
    {
        m_userDisconnected = true;
        m_waiting = false;
    }
    /// The hub went away. True when the retries should start.
    bool hubLost()
    {
        if (m_userDisconnected)
            return false;
        m_waiting = true;
        return true;
    }
    /// An attempt to connect failed. True when the retries should start (the
    /// first failure after Connect: later ones are already retries).
    bool attemptFailed()
    {
        if (m_userDisconnected || m_waiting)
            return false;
        m_waiting = true;
        return true;
    }
    void connected() { m_waiting = false; }
    bool waiting() const { return m_waiting; }

private:
    bool m_userDisconnected{ false };
    bool m_waiting{ false };
};

// ── Table identity ────────────────────────────────────────────────────────────

/// The rows a table VisIVO sent as a subset holds: row k of that table is
/// row rows[k] of the catalogue it was cut from (both 0-based). A table sent
/// whole has none, and its rows are the catalogue's own.
class SampSubset
{
public:
    explicit SampSubset(std::vector<qint64> rows) : m_rows(std::move(rows))
    {
        m_positionOf.reserve(static_cast<qsizetype>(m_rows.size()));
        for (std::size_t k = 0; k < m_rows.size(); ++k) {
            if (!m_positionOf.contains(m_rows[k]))
                m_positionOf.insert(m_rows[k], static_cast<qint64>(k));
        }
    }
    /// The catalogue row of row @p k of the subset table, or -1.
    qint64 catalogueRow(qint64 k) const
    {
        return k >= 0 && static_cast<std::size_t>(k) < m_rows.size()
                ? m_rows[static_cast<std::size_t>(k)] : -1;
    }
    /// Where catalogue row @p row is in the subset table, or -1 (not in it).
    qint64 subsetRow(qint64 row) const { return m_positionOf.value(row, -1); }
    std::size_t size() const { return m_rows.size(); }

private:
    std::vector<qint64> m_rows;
    QHash<qint64, qint64> m_positionOf;
};
using SampSubsetPtr = std::shared_ptr<const SampSubset>;

namespace Samp {

/// A row of a table in another tool, as a row of the local catalogue
/// (@p subset: what that table holds, null for the whole catalogue).
inline qint64 toCatalogueRow(const SampSubsetPtr &subset, qint64 row)
{
    return subset ? subset->catalogueRow(row) : row;
}

/// A row of the local catalogue as a row of a table in another tool; -1 when
/// that table, a subset, does not hold it.
inline qint64 toTableRow(const SampSubsetPtr &subset, qint64 row)
{
    return subset ? subset->subsetRow(row) : row;
}

/// The rows of @p rows that table holds, renumbered as it numbers them.
inline QList<qint64> toTableRows(const SampSubsetPtr &subset, const QList<qint64> &rows)
{
    if (!subset)
        return rows;
    QList<qint64> out;
    out.reserve(rows.size());
    for (const qint64 r : rows) {
        const qint64 k = subset->subsetRow(r);
        if (k >= 0)
            out.append(k);
    }
    return out;
}

/// The other way: rows a tool named in its table, as catalogue rows (rows
/// it does not hold are dropped).
inline QList<qint64> toCatalogueRows(const SampSubsetPtr &subset, const QList<qint64> &rows)
{
    if (!subset)
        return rows;
    QList<qint64> out;
    out.reserve(rows.size());
    for (const qint64 k : rows) {
        const qint64 r = subset->catalogueRow(k);
        if (r >= 0)
            out.append(r);
    }
    return out;
}

}  // namespace Samp

/// Which local catalogue each SAMP table is. A local catalogue is keyed by the
/// path its viewer opened on the backend. A table VisIVO sent is recorded
/// with the table-id it chose and, once the client has expanded it, the URL;
/// a table VisIVO received is recorded with the table-id and URL the sender
/// gave. Lookups follow SAMP: a match on table-id or on url is enough.
class SampTableRegistry
{
public:
    struct Entry
    {
        Samp::TableRef ref;
        QString localKey;
        SampSubsetPtr subset;   ///< set for a table VisIVO sent as a subset
    };

    /// The most identities kept; the oldest go first.
    static constexpr qsizetype kMaxEntries = 256;

    /// Record that SAMP knows @p localKey as @p ref. Every identity of a
    /// catalogue is kept (a table sent twice, a url under two table-ids: the
    /// other tool may use any of them); a table-id or url that named ANOTHER
    /// catalogue now names @p localKey.
    void remember(const Samp::TableRef &ref, const QString &localKey,
                  const SampSubsetPtr &subset = {})
    {
        if (ref.isEmpty() || localKey.isEmpty())
            return;
        for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
            const Entry &e = m_entries.at(i);
            const bool sameId = !ref.tableId.isEmpty() && e.ref.tableId == ref.tableId;
            const bool sameUrl = !ref.url.isEmpty() && e.ref.url == ref.url;
            const bool reassigned = (sameId || sameUrl) && e.localKey != localKey;
            const bool duplicate = e.localKey == localKey && e.ref.tableId == ref.tableId
                                   && e.ref.url == ref.url;
            if (reassigned || duplicate)
                m_entries.removeAt(i);
        }
        m_entries.append({ ref, localKey, subset });
        while (m_entries.size() > kMaxEntries)
            m_entries.removeFirst();
    }

    /// The URL a sent table ended up with (it is only known once the client
    /// expands the backend's placeholder, after the table-id was recorded).
    void setUrlForTableId(const QString &tableId, const QString &url)
    {
        if (tableId.isEmpty() || url.isEmpty())
            return;
        for (Entry &e : m_entries) {
            if (e.ref.tableId == tableId)
                e.ref.url = url;
        }
    }

    /// The entry a SAMP message names (table-id first, then url), or nullopt.
    std::optional<Entry> find(const Samp::TableRef &ref) const
    {
        for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
            const Entry &e = m_entries.at(i);
            if (!ref.tableId.isEmpty() && e.ref.tableId == ref.tableId)
                return e;
        }
        for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
            const Entry &e = m_entries.at(i);
            if (!ref.url.isEmpty() && e.ref.url == ref.url)
                return e;
        }
        return std::nullopt;
    }

    /// The local catalogue a SAMP message names, or an empty string.
    QString localKeyFor(const Samp::TableRef &ref) const
    {
        const auto e = find(ref);
        return e ? e->localKey : QString();
    }

    /// How SAMP knows @p localKey (its latest identity), or nullopt when it
    /// was never exchanged.
    std::optional<Samp::TableRef> refFor(const QString &localKey) const
    {
        for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
            if (m_entries.at(i).localKey == localKey)
                return m_entries.at(i).ref;
        }
        return std::nullopt;
    }

    /// Every identity SAMP knows @p localKey by, the first one recorded first:
    /// a table received and then sent back is one table under two names in
    /// the other tool (its original and the copy), and a row picked here is
    /// the same row of both.
    QList<Samp::TableRef> refsFor(const QString &localKey) const
    {
        QList<Samp::TableRef> refs;
        for (const Entry &e : entriesFor(localKey))
            refs.append(e.ref);
        return refs;
    }

    /// As refsFor(), with the subset each table holds.
    QList<Entry> entriesFor(const QString &localKey) const
    {
        QList<Entry> out;
        for (const Entry &e : m_entries) {
            if (e.localKey == localKey)
                out.append(e);
        }
        return out;
    }

    qsizetype size() const { return m_entries.size(); }

private:
    QList<Entry> m_entries;
};

/// Tables VisIVO is sending, by the table-id it chose, until the hub says
/// they went out (then they join the SampTableRegistry). A tool may highlight
/// a row of one before that, naming it by table-id or by url.
class SampPendingTables
{
public:
    /// A send of @p localKey under @p tableId began at @p nowMs (ms since the
    /// epoch; sends older than kMaxAgeMs are dropped then: a send whose
    /// table never went out must not keep its identity for ever).
    void add(const QString &tableId, const QString &localKey, const SampSubsetPtr &subset = {},
             qint64 nowMs = 0)
    {
        expire(nowMs);
        if (!tableId.isEmpty() && !localKey.isEmpty())
            m_tables.insert(tableId, { localKey, QString(), subset, nowMs });
    }
    /// A staging can take minutes (millions of rows): an hour is far past it.
    static constexpr qint64 kMaxAgeMs = 3600LL * 1000;
    /// Drop the sends begun more than kMaxAgeMs before @p nowMs.
    void expire(qint64 nowMs)
    {
        for (auto it = m_tables.begin(); it != m_tables.end();) {
            if (nowMs - it->startedMs > kMaxAgeMs)
                it = m_tables.erase(it);
            else
                ++it;
        }
    }
    /// A send that failed: forget it.
    void remove(const QString &tableId) { m_tables.remove(tableId); }
    qsizetype size() const { return m_tables.size(); }
    /// The URL the tools fetch it from, once the backend staged it.
    void setUrl(const QString &tableId, const QString &url)
    {
        const auto it = m_tables.find(tableId);
        if (it != m_tables.end())
            it->url = url;
    }
    struct Pending
    {
        QString localKey;
        QString url;
        SampSubsetPtr subset;
        qint64 startedMs{ 0 };
    };
    /// The catalogue of a send that went out (and forget it), or empty.
    QString take(const QString &tableId) { return m_tables.take(tableId).localKey; }
    /// As take(), with the subset the table holds.
    Pending takeEntry(const QString &tableId) { return m_tables.take(tableId); }
    /// The send a message names, matched on table-id or url, or nullopt.
    std::optional<Pending> find(const Samp::TableRef &ref) const
    {
        for (auto it = m_tables.cbegin(); it != m_tables.cend(); ++it) {
            if ((!ref.tableId.isEmpty() && it.key() == ref.tableId)
                || (!ref.url.isEmpty() && it->url == ref.url))
                return it.value();
        }
        return std::nullopt;
    }
    /// The catalogue a message names, matched on table-id or url, or empty.
    QString localKeyFor(const Samp::TableRef &ref) const
    {
        const auto p = find(ref);
        return p ? p->localKey : QString();
    }

private:
    QHash<QString, Pending> m_tables;
};

// ── The backend's outbound queue ──────────────────────────────────────────────

/// Messages the client fetched from a backend's leased queue
/// (GET /v1/samp/pending?lease=true): which are still waiting to be sent, and
/// which were sent and must be acknowledged (POST /v1/samp/pending/ack). A
/// message not acknowledged comes back when its lease runs out, so delivery is
/// at least once; this keeps the repeats down without holding acks for ever.
class SampBackendAcks
{
public:
    /// Attempts at posting one batch of acks before it is given up (the
    /// messages are then sent again when their leases run out).
    static constexpr int kMaxAttempts = 5;
    /// The most acks kept per backend; the oldest go first.
    static constexpr qsizetype kMaxPending = 1024;

    /// A fetched message: false when it is already waiting to be sent (a
    /// lease that ran out during a slow send hands it out again), so it is
    /// not queued twice.
    bool fetched(const QString &jobId)
    {
        if (jobId.isEmpty())
            return true;
        if (m_inFlight.contains(jobId))
            return false;
        m_inFlight.insert(jobId);
        return true;
    }
    /// The message left the outbox: @p delivered when the hub took it (or
    /// refused it for good), and it is then acknowledged to @p backend.
    void finished(const QString &backend, const QString &jobId, bool delivered)
    {
        if (jobId.isEmpty())
            return;
        m_inFlight.remove(jobId);
        if (!delivered)
            return;   // left to the lease: the backend hands it out again
        QStringList &acks = m_acks[backend];
        acks.append(jobId);
        while (acks.size() > kMaxPending)
            acks.removeFirst();
    }
    /// The connection is gone: nothing is in the outbox any more.
    void clearInFlight() { m_inFlight.clear(); }
    /// The acks to post to @p backend now (they are taken).
    QStringList take(const QString &backend) { return m_acks.take(backend); }
    /// How posting @p jobs to @p backend went. A backend without the ack
    /// route (404) has drained its queue already; any other failure puts the
    /// batch back, up to kMaxAttempts in a row.
    void posted(const QString &backend, const QStringList &jobs, bool ok, int httpStatus)
    {
        if (ok || httpStatus == 404) {
            m_failures.remove(backend);
            return;
        }
        const int failures = ++m_failures[backend];
        if (failures >= kMaxAttempts) {
            m_failures.remove(backend);
            return;
        }
        QStringList &acks = m_acks[backend];
        acks = jobs + acks;
        while (acks.size() > kMaxPending)
            acks.removeFirst();
    }
    qsizetype pending(const QString &backend) const { return m_acks.value(backend).size(); }
    bool inFlight(const QString &jobId) const { return m_inFlight.contains(jobId); }

private:
    QHash<QString, QStringList> m_acks;   ///< by backend URL
    QHash<QString, int> m_failures;
    QSet<QString> m_inFlight;
};

// ── Row mapping ───────────────────────────────────────────────────────────────

/// Original-table row ↔ index of the entry a view loaded. A view may hold a
/// filtered, sorted or evenly sampled subset ("100000 of 3931546 rows"), so a
/// SAMP row index (0-based, in the ORIGINAL table) is looked up through the
/// row each entry came from, never used as an entry index.
class SampRowMap
{
public:
    /// @p rowOfEntry[i] is the original row of entry i (negative: unknown).
    void reset(const std::vector<qint64> &rowOfEntry)
    {
        m_rowOfEntry = rowOfEntry;
        m_entryOfRow.clear();
        m_entryOfRow.reserve(static_cast<qsizetype>(rowOfEntry.size()));
        for (std::size_t i = 0; i < rowOfEntry.size(); ++i) {
            if (rowOfEntry[i] >= 0 && !m_entryOfRow.contains(rowOfEntry[i]))
                m_entryOfRow.insert(rowOfEntry[i], static_cast<int>(i));
        }
    }

    void clear()
    {
        m_rowOfEntry.clear();
        m_entryOfRow.clear();
    }

    bool isEmpty() const { return m_entryOfRow.isEmpty(); }

    /// The loaded entry of original row @p row, or -1 when that row is not
    /// loaded (filtered out, not in the sample, not yet paged in).
    int entryFor(qint64 row) const { return m_entryOfRow.value(row, -1); }

    /// The original row of entry @p entry, or -1.
    qint64 rowFor(int entry) const
    {
        if (entry < 0 || static_cast<std::size_t>(entry) >= m_rowOfEntry.size())
            return -1;
        return m_rowOfEntry[static_cast<std::size_t>(entry)];
    }

    struct Split
    {
        QList<int> entries;   ///< loaded entries, in the order of the rows given
        int missing{ 0 };     ///< rows that are not loaded here
    };

    /// The original rows of @p entries, ascending and each once: what a
    /// selection of loaded entries sends. Entries with no known row are left
    /// out (and counted in @p unknown, when given).
    QList<qint64> rowsForEntries(const QList<int> &entries, int *unknown = nullptr) const
    {
        QList<qint64> rows;
        rows.reserve(entries.size());
        int missing = 0;
        for (const int e : entries) {
            const qint64 r = rowFor(e);
            if (r >= 0)
                rows.append(r);
            else
                ++missing;
        }
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        if (unknown)
            *unknown = missing;
        return rows;
    }

    /// The loaded entries of @p rows, and how many of them are not loaded.
    Split entriesFor(const QList<qint64> &rows) const
    {
        Split s;
        s.entries.reserve(rows.size());
        for (const qint64 r : rows) {
            const int e = entryFor(r);
            if (e >= 0)
                s.entries.append(e);
            else
                ++s.missing;
        }
        return s;
    }

private:
    std::vector<qint64> m_rowOfEntry;
    QHash<qint64, int> m_entryOfRow;
};

#endif  // SAMPPROTOCOL_H
