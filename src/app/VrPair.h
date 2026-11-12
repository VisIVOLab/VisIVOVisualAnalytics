#pragma once

// VrPair.h
// "Open in VR Headset…" by pairing code (issue #214, contract visivo-pair/1
// of epic #210): what the backend's three pairing routes answer, and what the
// dialog shows for it.
//
//   POST   /v1/vr/pair {session, dataset, path, kind?, box?}
//          → {code, expires_at, page_url}, or 409 {detail, reason} with reason
//            "not-configured" (no VISIVO_VR_PAIR_URL) or "unreachable"
//   GET    /v1/vr/pair → {enabled, pending:[{code, expires_at}],
//                         connected:[{id, since, dataset, transport}]}
//   DELETE /v1/vr/pair/{code_or_id} revokes a code or disconnects a headset
//
// The headset needs no cable and no address: the user types the code on the
// public loader page (or scans the QR code of page_url, which carries the code
// in its fragment), and the backend connects to the headset through the
// pairing service. The desktop never names that service or that page: both
// come from the backend's configuration, and page_url is used as given.
//
// The token is never part of any of this: not in the request body, not in
// page_url, not in the QR code. The code is shown on purpose (that is how the
// user gets it to the headset) but it is a short-lived credential, so it is
// not logged at info level either.
//
// Widget-free and header-only: unit-tested in tests/test_vr_pair.cpp.

#include "VrLink.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>

namespace VrPair {

/// The contract's default life of a pair code, used only when the backend's
/// expiry cannot be placed on this computer's clock.
inline constexpr qint64 kDefaultTtlMs = 300 * 1000;
/// Once the code has been used, how long the headset may take to finish
/// connecting (the WebRTC offer, answer and ICE) before the dialog says it
/// did not.
inline constexpr qint64 kConnectGraceMs = 45 * 1000;

/// Why pairing cannot be used, in which case the dialog opens on the USB
/// (adb) way instead and says why.
enum class Unavailable {
    None,
    NotConfigured,   ///< 409 not-configured: the backend has no pairing service
    Unreachable,     ///< 409 unreachable: it has one and cannot reach it
    OldBackend,      ///< no /v1/vr/pair route at all (a backend older than this desktop)
};

/// A code the backend handed out, on THIS computer's clock.
struct Code
{
    QString code;          ///< as the backend wrote it: what DELETE and pending use
    QString pageUrl;       ///< the public loader with #pair=<code>
    qint64 expiresAtMs{ 0 };   ///< local epoch ms
};

struct CreateResult
{
    bool ok{ false };
    Code code;
    Unavailable unavailable{ Unavailable::None };
    QString message;   ///< for the user when !ok (plain text, no secret in it)
};

struct Connection
{
    QString id;
    QString since;
    QString dataset;
    QString transport;
};

struct Pending
{
    QString code;
};

struct Status
{
    bool valid{ false };   ///< the answer was understood
    bool enabled{ false };
    QList<Pending> pending;
    QList<Connection> connected;
};

/// The pairing service and the public loader page for a backend this desktop
/// starts (VISIVO_VR_PAIR_URL, VISIVO_VR_PAGE_URL), from Settings ▸ Backends.
/// Neither has a built-in value: the server that hosts them is the operator's
/// choice and moves, so changing it is changing the setting. A variable
/// already set in the environment wins over the setting (the launcher's rule
/// for every VISIVO_* it passes); an empty setting passes nothing, and the
/// backend then answers 409 not-configured.
inline void applyServiceSettings(QProcessEnvironment &env, const QString &pairUrl,
                                 const QString &pageUrl)
{
    using namespace Qt::StringLiterals;
    const auto put = [&env](const QString &name, const QString &value) {
        if (!env.value(name).trimmed().isEmpty() || value.trimmed().isEmpty())
            return;
        env.insert(name, value.trimmed());
    };
    put(u"VISIVO_VR_PAIR_URL"_s, pairUrl);
    put(u"VISIVO_VR_PAGE_URL"_s, pageUrl);
}

/// The POST body: the same values the adb link's fragment carries, except the
/// token. kind is left out for a cube (as in the link), box when there is none.
inline QJsonObject requestBody(const VrLink::Inputs &in)
{
    using namespace Qt::StringLiterals;
    QJsonObject body{ { u"session"_s, in.sessionId },
                      { u"dataset"_s, in.datasetId },
                      { u"path"_s, in.path } };
    if (!in.kind.isEmpty() && in.kind != u"cube")
        body.insert(u"kind"_s, in.kind);
    if (!in.boxMpc.isEmpty())
        body.insert(u"box"_s, in.boxMpc);
    return body;
}

/// expires_at as epoch ms: a number is epoch seconds, a string ISO 8601
/// (a zone-less one is UTC, which is what the backend writes). -1 if neither.
inline qint64 parseTime(const QJsonValue &value)
{
    if (value.isDouble()) {
        const double s = value.toDouble();
        return std::isfinite(s) && s > 0 ? static_cast<qint64>(std::llround(s * 1000.0)) : -1;
    }
    if (value.isString()) {
        QDateTime t = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
        if (!t.isValid())
            t = QDateTime::fromString(value.toString(), Qt::ISODate);
        if (!t.isValid())
            return -1;
        // No offset written: parsed as local time, but it is UTC.
        if (t.timeSpec() == Qt::LocalTime)
            t.setTimeZone(QTimeZone::UTC);
        return t.toMSecsSinceEpoch();
    }
    return -1;
}

/// The HTTP Date header as epoch ms, -1 when absent or unreadable. HTTP
/// writes the zone as "GMT" (RFC 9110's IMF-fixdate), which Qt's RFC 2822
/// reader refuses: it wants a numeric offset, so GMT becomes +0000.
inline qint64 parseHttpDate(const QString &date)
{
    QString text = date.trimmed();
    if (text.endsWith(u" GMT") || text.endsWith(u" UTC"))
        text = text.left(text.size() - 3) + u"+0000";
    const QDateTime t = QDateTime::fromString(text, Qt::RFC2822Date);
    return t.isValid() ? t.toMSecsSinceEpoch() : -1;
}

/// The backend's expiry moved onto this computer's clock. With the server's
/// Date it is the time left as the server counts it, so a desktop whose clock
/// is off by minutes still counts down right; without it the expiry is taken
/// as it is, unless it is plainly off (already past, or more than a day
/// ahead), in which case the contract's default life is assumed.
inline qint64 localExpiry(qint64 expiresAtServerMs, qint64 serverNowMs, qint64 localNowMs)
{
    if (expiresAtServerMs < 0)
        return localNowMs + kDefaultTtlMs;
    if (serverNowMs >= 0)
        return localNowMs + (expiresAtServerMs - serverNowMs);
    const qint64 left = expiresAtServerMs - localNowMs;
    if (left <= 0 || left > 24LL * 3600 * 1000)
        return localNowMs + kDefaultTtlMs;
    return expiresAtServerMs;
}

/// The reason in a refusal's body: FastAPI's {"detail": …}, a string here.
inline QString detailOf(const QByteArray &body)
{
    const QJsonValue d = QJsonDocument::fromJson(body).object().value(u"detail");
    return d.isString() ? d.toString() : QString();
}

/// A 404/405 that is no route's own answer: the route is missing (same rule
/// as visivo::isMissingRouteReply, repeated to keep this header standalone).
inline bool isMissingRoute(int httpStatus, const QByteArray &body)
{
    if (httpStatus != 404 && httpStatus != 405)
        return false;
    const QJsonValue d = QJsonDocument::fromJson(body).object().value(u"detail");
    return !d.isString() || d.toString() == u"Not Found" || d.toString() == u"Method Not Allowed";
}

/// What the answer to POST /v1/vr/pair means. @p httpStatus 0: no answer;
/// @p transportError Qt's words for that case; @p serverDate the reply's Date.
inline CreateResult interpretCreate(int httpStatus, const QByteArray &body,
                                    const QString &transportError, const QString &serverDate,
                                    qint64 localNowMs)
{
    using namespace Qt::StringLiterals;
    CreateResult r;
    if (httpStatus == 0) {
        r.message = transportError.isEmpty()
                ? u"The backend did not answer the pairing request."_s
                : u"The backend did not answer the pairing request (%1)."_s.arg(transportError);
        return r;
    }
    if (httpStatus == 409) {
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        const QString reason = o.value(u"reason"_s).toString();
        const QString detail = o.value(u"detail"_s).toString();
        if (reason == u"not-configured") {
            r.unavailable = Unavailable::NotConfigured;
            r.message = u"This backend has no pairing service configured, so the headset "
                        u"connects through a USB cable instead."_s;
        } else if (reason == u"unreachable") {
            r.unavailable = Unavailable::Unreachable;
            r.message = u"The backend cannot reach its pairing service right now, so the "
                        u"headset connects through a USB cable instead."_s;
        } else {
            r.message = detail.isEmpty() ? u"The backend refused to pair (HTTP 409)."_s
                                         : u"The backend refused to pair: %1"_s.arg(detail);
        }
        if ((r.unavailable != Unavailable::None) && !detail.isEmpty())
            r.message += u" (The backend says: %1)"_s.arg(detail);
        return r;
    }
    if (isMissingRoute(httpStatus, body)) {
        r.unavailable = Unavailable::OldBackend;
        r.message = u"This backend is older than this desktop and cannot pair by code, so "
                    u"the headset connects through a USB cable instead."_s;
        return r;
    }
    if (httpStatus == 401 || httpStatus == 403) {
        r.message = u"The backend refused this desktop's token."_s;
        return r;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        const QString detail = detailOf(body);
        r.message = detail.isEmpty() ? u"Pairing failed (HTTP %1)."_s.arg(httpStatus)
                                     : u"Pairing failed: %1"_s.arg(detail);
        return r;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    const QJsonObject o = doc.object();
    r.code.code = o.value(u"code"_s).toString().trimmed();
    r.code.pageUrl = o.value(u"page_url"_s).toString().trimmed();
    if (!doc.isObject() || r.code.code.isEmpty()) {
        r.message = u"The backend's answer to the pairing request was not understood."_s;
        return r;
    }
    r.code.expiresAtMs = localExpiry(parseTime(o.value(u"expires_at"_s)),
                                     parseHttpDate(serverDate), localNowMs);
    r.ok = true;
    return r;
}

/// The answer to GET /v1/vr/pair; invalid when it is not one.
inline Status parseStatus(int httpStatus, const QByteArray &body)
{
    using namespace Qt::StringLiterals;
    Status s;
    if (httpStatus != 200)
        return s;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return s;
    const QJsonObject o = doc.object();
    if (!o.value(u"enabled"_s).isBool())
        return s;
    s.enabled = o.value(u"enabled"_s).toBool();
    for (const QJsonValue &v : o.value(u"pending"_s).toArray())
        s.pending.append({ v.toObject().value(u"code"_s).toString() });
    for (const QJsonValue &v : o.value(u"connected"_s).toArray()) {
        const QJsonObject c = v.toObject();
        Connection conn;
        conn.id = c.value(u"id"_s).toString();
        conn.since = c.value(u"since"_s).toString();
        conn.dataset = c.value(u"dataset"_s).toString();
        conn.transport = c.value(u"transport"_s).toString();
        if (!conn.id.isEmpty())
            s.connected.append(conn);
    }
    s.valid = true;
    return s;
}

/// The code as the user reads and types it: K7Q4MX → K7Q-4MX. A code of
/// another length, or already grouped, is shown as the backend wrote it.
inline QString displayCode(const QString &code)
{
    QString bare = code;
    bare.remove(u'-');
    bare.remove(u' ');
    if (bare.size() != 6)
        return code;
    return bare.left(3).toUpper() + u'-' + bare.mid(3).toUpper();
}

/// "4:59" for the time left, rounded up to the second ("0:00" once over).
inline QString countdown(qint64 msLeft)
{
    const qint64 s = msLeft <= 0 ? 0 : (msLeft + 999) / 1000;
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

/// What the QR code encodes: page_url, which must be an http(s) address that
/// carries this code as #pair=…, and nothing that looks like a token. Empty
/// when there is nothing safe to encode (the code is still shown to type).
inline QString qrPayload(const Code &code)
{
    using namespace Qt::StringLiterals;
    const QUrl url(code.pageUrl, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || (scheme != u"https" && scheme != u"http"))
        return {};
    const QUrlQuery fragment(url.fragment(QUrl::FullyEncoded));
    const QUrlQuery query(url);
    if (fragment.hasQueryItem(u"token"_s) || query.hasQueryItem(u"token"_s))
        return {};
    // The fragment's code must be this code, so a scan and the typed code
    // never pair two different things.
    QString inFragment = fragment.queryItemValue(u"pair"_s, QUrl::FullyDecoded);
    inFragment.remove(u'-');
    QString bare = code.code;
    bare.remove(u'-');
    if (inFragment.compare(bare, Qt::CaseInsensitive) != 0)
        return {};
    return code.pageUrl;
}

/// What the dialog shows, and what it offers.
enum class State {
    Requesting,     ///< asking the backend for a code
    Waiting,        ///< code shown, counting down; nobody has used it yet
    Connecting,     ///< the code was used: the headset is connecting
    Connected,      ///< a headset is connected (Disconnect)
    Expired,        ///< the code ran out unused (New code)
    Lost,           ///< the code was used but no headset came (New code)
    Disconnected,   ///< the headset that was connected is gone (New code)
    Unavailable,    ///< no pairing here: the USB way, with the reason
    Failed,         ///< the request failed otherwise (Try again)
};

/// The pairing state over time: fed by the POST answer, each poll and a
/// clock tick. All times are local epoch ms, passed in for the tests.
class Tracker
{
public:
    explicit Tracker(QString datasetId = QString()) : m_datasetId(std::move(datasetId)) { }

    State state() const { return m_state; }
    const Code &code() const { return m_code; }
    QString message() const { return m_message; }
    Unavailable unavailable() const { return m_unavailable; }
    /// The headsets on this dataset (Disconnect acts on these).
    const QList<Connection> &connections() const { return m_connections; }
    qint64 msLeft(qint64 nowMs) const { return m_code.expiresAtMs - nowMs; }
    /// Polling is worth it: something can still change by itself.
    bool polling() const
    {
        return m_state == State::Waiting || m_state == State::Connecting
                || m_state == State::Connected;
    }
    /// The code is still pending on the backend, so closing should revoke it.
    bool codeOutstanding(qint64 nowMs) const
    {
        return m_state == State::Waiting && msLeft(nowMs) > 0;
    }

    void requesting()
    {
        m_state = State::Requesting;
        m_code = Code();
        m_message.clear();
        m_unavailable = Unavailable::None;
        m_connections.clear();
        m_codeGoneAtMs = -1;
        m_everConnected = false;
    }

    void created(const CreateResult &r, qint64 nowMs)
    {
        m_unavailable = r.unavailable;
        m_message = r.message;
        if (!r.ok) {
            m_state = r.unavailable == Unavailable::None ? State::Failed : State::Unavailable;
            return;
        }
        m_code = r.code;
        m_state = msLeft(nowMs) > 0 ? State::Waiting : State::Expired;
    }

    /// A poll's answer. An answer not understood (a dropped poll) changes nothing.
    void polled(const Status &s, qint64 nowMs)
    {
        using namespace Qt::StringLiterals;
        if (!s.valid || !polling())
            return;
        if (!s.enabled) {
            // The backend restarted without a pairing service.
            m_state = State::Unavailable;
            m_unavailable = Unavailable::NotConfigured;
            m_message = u"The backend no longer has a pairing service configured, so the "
                        u"headset connects through a USB cable instead."_s;
            m_connections.clear();
            return;
        }
        m_connections.clear();
        for (const Connection &c : s.connected)
            if (m_datasetId.isEmpty() || c.dataset.isEmpty() || c.dataset == m_datasetId)
                m_connections.append(c);
        if (!m_connections.isEmpty()) {
            m_state = State::Connected;
            m_everConnected = true;
            return;
        }
        if (m_everConnected) {
            m_state = State::Disconnected;
            return;
        }
        bool pending = false;
        for (const Pending &p : s.pending)
            pending = pending || p.code == m_code.code;
        if (pending) {
            m_state = State::Waiting;
            m_codeGoneAtMs = -1;
            tick(nowMs);
            return;
        }
        // Gone from the pending list without a headset: used and still
        // connecting, or simply run out.
        if (m_state == State::Waiting && msLeft(nowMs) <= 0) {
            m_state = State::Expired;
            return;
        }
        if (m_codeGoneAtMs < 0)
            m_codeGoneAtMs = nowMs;
        m_state = State::Connecting;
        tick(nowMs);
    }

    /// The clock moved: the countdown, and a headset that never came.
    void tick(qint64 nowMs)
    {
        if (m_state == State::Waiting && msLeft(nowMs) <= 0)
            m_state = State::Expired;
        else if (m_state == State::Connecting && m_codeGoneAtMs >= 0
                 && nowMs - m_codeGoneAtMs > kConnectGraceMs)
            m_state = State::Lost;
    }

    /// Disconnect pressed: the headsets are being let go.
    void disconnecting()
    {
        if (m_state == State::Connected)
            m_state = State::Disconnected;
        m_connections.clear();
    }

private:
    QString m_datasetId;
    State m_state{ State::Requesting };
    Code m_code;
    QString m_message;
    Unavailable m_unavailable{ Unavailable::None };
    QList<Connection> m_connections;
    qint64 m_codeGoneAtMs{ -1 };
    bool m_everConnected{ false };
};

/// The status line for @p t at @p nowMs (plain text).
inline QString statusText(const Tracker &t, qint64 nowMs)
{
    using namespace Qt::StringLiterals;
    switch (t.state()) {
    case State::Requesting:
        return u"Asking the backend for a pairing code…"_s;
    case State::Waiting:
        return u"Waiting for the headset. The code expires in %1."_s.arg(countdown(t.msLeft(nowMs)));
    case State::Connecting:
        return u"The code was entered: the headset is connecting…"_s;
    case State::Connected: {
        const int n = static_cast<int>(t.connections().size());
        return n > 1 ? u"%1 headsets connected."_s.arg(n) : u"Headset connected."_s;
    }
    case State::Expired:
        return u"The code expired. Press New Code for another one."_s;
    case State::Lost:
        return u"The code was used, but no headset connected. Press New Code and try again."_s;
    case State::Disconnected:
        return u"The headset disconnected. Press New Code to pair again."_s;
    case State::Unavailable:
    case State::Failed:
        return t.message();
    }
    return {};
}

}   // namespace VrPair
