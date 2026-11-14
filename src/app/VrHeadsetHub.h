#pragma once

// VrHeadsetHub.h
// The desktop's side of the headset hub of a backend session (issue #238,
// backend #236): which headset pages are open in this session, the `show`
// command that sends them a cube, and the activity they report back.
//
//   GET  /v1/vr/headset            → {headsets: [{client, transport, label,
//                                     since, last_seen, in_vr, cube, staged_cube}]}
//                                     newest first, pages seen in the last 45 s
//   POST /v1/vr/headset/command    {client?, type: "show", dataset_id, path, kind?, box?}
//                                   → {seq, client}; 404 with a detail of its
//                                     own when no page is connected
//   GET  /v1/vr/activity?after=&wait= → {events: [{seq, client, time, kind,
//                                     cube_path?, summary, payload}], next}
//
// An older backend answers all three with FastAPI's bare 404 "Not Found":
// that is "no hub here", never "no headset" (visivo::isMissingRouteReply).
//
// The token is in none of these bodies; it travels in the request headers
// only (BackendClient::applySessionHeaders).
//
// Widget-free and header-only: unit-tested in tests/test_vr_headset_hub.cpp.

#include "MissingRoute.h"
#include "ProductRegistry.h"
#include "VrLink.h"

#include <QByteArray>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QVariantMap>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace VrHub {

/// The activity ring the backend keeps per session, and so the list here.
inline constexpr int kActivityCapacity = 256;

/// A time as epoch ms: a number is epoch seconds, a string ISO 8601 (a
/// zone-less one is UTC). -1 when it is neither.
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
        if (t.timeSpec() == Qt::LocalTime && !value.toString().contains(u'+')
            && !value.toString().endsWith(u'Z'))
            t.setTimeZone(QTimeZone::UTC);
        return t.toMSecsSinceEpoch();
    }
    return -1;
}

// ── Presence ────────────────────────────────────────────────────────────────

/// A cube a page shows (or has staged): what the `show` command carries.
struct Cube
{
    QString datasetId;
    QString path;
    QString kind;   ///< "" or "cube", "points", "velocity"
    QString box;    ///< a velocity field's box in Mpc, as the pairing body has it
    bool isSet() const { return !datasetId.isEmpty() || !path.isEmpty(); }
};

inline Cube parseCube(const QJsonValue &value)
{
    Cube c;
    const QJsonObject o = value.toObject();
    c.datasetId = o.value(u"dataset_id").toString();
    c.path = o.value(u"path").toString();
    c.kind = o.value(u"kind").toString();
    const QJsonValue box = o.value(u"box");
    if (box.isDouble())
        c.box = QString::number(box.toDouble(), 'g', 12);
    else if (box.isString())
        c.box = box.toString();
    return c;
}

struct Headset
{
    QString client;      ///< the page's id in the hub
    QString transport;   ///< "adb" | "pair" | "lan"
    QString label;
    qint64 sinceMs{ -1 };
    qint64 lastSeenMs{ -1 };
    bool inVr{ false };
    Cube cube;           ///< on show
    Cube staged;         ///< loading, not on show yet
};

struct Presence
{
    bool ok{ false };            ///< the hub answered
    bool unsupported{ false };   ///< the backend has no hub (older than this client)
    QList<Headset> headsets;     ///< newest first, as the hub lists them
    QString error;
};

inline Presence parsePresence(int httpStatus, const QByteArray &body, const QString &qtError = {})
{
    Presence p;
    if (visivo::isMissingRouteReply(httpStatus, body)) {
        p.unsupported = true;
        return p;
    }
    if (httpStatus != 200) {
        p.error = httpStatus == 0 ? qtError : QStringLiteral("HTTP %1").arg(httpStatus);
        return p;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject() || !doc.object().value(u"headsets").isArray()) {
        p.error = QStringLiteral("the backend's answer could not be read");
        return p;
    }
    p.ok = true;
    for (const QJsonValue v : doc.object().value(u"headsets").toArray()) {
        const QJsonObject o = v.toObject();
        Headset h;
        h.client = o.value(u"client").toString();
        if (h.client.isEmpty())
            continue;
        h.transport = o.value(u"transport").toString();
        h.label = o.value(u"label").toString();
        h.sinceMs = parseTime(o.value(u"since"));
        h.lastSeenMs = parseTime(o.value(u"last_seen"));
        h.inVr = o.value(u"in_vr").toBool();
        h.cube = parseCube(o.value(u"cube"));
        h.staged = parseCube(o.value(u"staged_cube"));
        p.headsets.append(h);
    }
    return p;
}

/// How the page reaches the backend, in words.
inline QString transportText(const QString &transport)
{
    if (transport == u"pair")
        return QStringLiteral("paired by code");
    if (transport == u"adb")
        return QStringLiteral("USB cable");
    if (transport == u"lan")
        return QStringLiteral("local network");
    return transport;
}

/// The file name of a cube, for a line of text.
inline QString cubeName(const Cube &c)
{
    if (!c.path.isEmpty())
        return QFileInfo(c.path).fileName();
    return c.datasetId;
}

/// One headset in a sentence: "Headset (paired by code), in VR, showing m33.fits".
inline QString headsetLine(const Headset &h)
{
    QString text = h.label.isEmpty() ? QStringLiteral("Headset") : h.label;
    const QString how = transportText(h.transport);
    if (!how.isEmpty())
        text += QStringLiteral(" (%1)").arg(how);
    text += h.inVr ? QStringLiteral(", in VR") : QStringLiteral(", not in VR");
    if (h.cube.isSet())
        text += QStringLiteral(", showing %1").arg(cubeName(h.cube));
    else if (h.staged.isSet())
        text += QStringLiteral(", loading %1").arg(cubeName(h.staged));
    else
        text += QStringLiteral(", no cube on show");
    return text;
}

/// The status bar's word on the headsets: "" when none is connected.
inline QString statusText(const QList<Headset> &headsets)
{
    if (headsets.isEmpty())
        return {};
    const Headset &h = headsets.first();
    QString text = h.inVr ? QStringLiteral("Headset: in VR") : QStringLiteral("Headset: connected");
    if (h.cube.isSet())
        text += QStringLiteral(" · %1").arg(cubeName(h.cube));
    if (headsets.size() > 1)
        text += QStringLiteral(" (+%1 more)").arg(headsets.size() - 1);
    return text;
}

/// Does @p h show the cube with @p datasetId / @p path?
inline bool showsCube(const Headset &h, const QString &datasetId, const QString &path)
{
    if (!h.cube.isSet())
        return false;
    if (!datasetId.isEmpty() && h.cube.datasetId == datasetId)
        return true;
    return !path.isEmpty() && h.cube.path == path;
}

// ── Commands ────────────────────────────────────────────────────────────────

/// POST /v1/vr/headset/command for a `show` of the window's cube. No @p client:
/// the newest page. The token is not in it (the link's values except that).
inline QJsonObject showCommandBody(const VrLink::Inputs &in, const QString &client = {})
{
    QJsonObject body{ { QStringLiteral("type"), QStringLiteral("show") },
                      { QStringLiteral("dataset_id"), in.datasetId },
                      { QStringLiteral("path"), in.path } };
    if (!client.isEmpty())
        body.insert(QStringLiteral("client"), client);
    if (!in.kind.isEmpty() && in.kind != u"cube")
        body.insert(QStringLiteral("kind"), in.kind);
    if (!in.boxMpc.isEmpty())
        body.insert(QStringLiteral("box"), in.boxMpc);
    return body;
}

struct CommandResult
{
    enum class Outcome { Sent, NoHeadset, Unsupported, Failed };
    Outcome outcome{ Outcome::Failed };
    qint64 seq{ 0 };
    QString client;     ///< the page it was queued for
    QString message;    ///< for the user when not Sent
};

inline CommandResult interpretCommand(int httpStatus, const QByteArray &body, const QString &qtError)
{
    CommandResult r;
    if (visivo::isMissingRouteReply(httpStatus, body)) {
        r.outcome = CommandResult::Outcome::Unsupported;
        r.message = QStringLiteral("This backend cannot send cubes to a headset (it is older "
                                   "than this client); update it.");
        return r;
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (httpStatus == 404) {
        r.outcome = CommandResult::Outcome::NoHeadset;
        r.message = QStringLiteral("No headset page is connected any more.");
        return r;
    }
    if (httpStatus >= 200 && httpStatus < 300) {
        r.outcome = CommandResult::Outcome::Sent;
        r.seq = static_cast<qint64>(o.value(u"seq").toDouble());
        r.client = o.value(u"client").toString();
        return r;
    }
    const QString detail = o.value(u"detail").toString().left(300);
    r.message = httpStatus == 0
            ? QStringLiteral("The backend did not answer: %1").arg(qtError)
            : QStringLiteral("The backend refused it (HTTP %1)%2")
                      .arg(httpStatus)
                      .arg(detail.isEmpty() ? QString() : QStringLiteral(": %1").arg(detail));
    return r;
}

// ── Activity ────────────────────────────────────────────────────────────────

struct Event
{
    /// Bumped by the link each time the backend restarted (its seqs began
    /// again at 1): (epoch, seq) is the event's identity and its order.
    int epoch{ 0 };
    qint64 seq{ 0 };
    QString client;
    qint64 timeMs{ -1 };
    QString kind;        ///< look | clip | product | cube
    QString cubePath;
    QString summary;
    QJsonObject payload;
};

/// The cube an event is about, for coalescing: its path, else the payload's
/// dataset id; '' when it names none.
inline QString cubeKey(const Event &e)
{
    if (!e.cubePath.isEmpty())
        return e.cubePath;
    const QString id = e.payload.value(u"dataset_id").toString();
    return id.isEmpty() ? QString() : QStringLiteral("dataset:") + id;
}

inline bool newerThan(const Event &a, const Event &b)
{
    return a.epoch != b.epoch ? a.epoch > b.epoch : a.seq > b.seq;
}

struct ActivityPage
{
    bool ok{ false };
    bool unsupported{ false };
    QList<Event> events;
    qint64 next{ 0 };
    QString error;
};

inline ActivityPage parseActivity(int httpStatus, const QByteArray &body, const QString &qtError = {})
{
    ActivityPage page;
    if (visivo::isMissingRouteReply(httpStatus, body)) {
        page.unsupported = true;
        return page;
    }
    if (httpStatus != 200) {
        page.error = httpStatus == 0 ? qtError : QStringLiteral("HTTP %1").arg(httpStatus);
        return page;
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (!o.value(u"events").isArray()) {
        page.error = QStringLiteral("the backend's answer could not be read");
        return page;
    }
    page.ok = true;
    page.next = static_cast<qint64>(o.value(u"next").toDouble());
    for (const QJsonValue v : o.value(u"events").toArray()) {
        const QJsonObject e = v.toObject();
        Event ev;
        ev.seq = static_cast<qint64>(e.value(u"seq").toDouble());
        if (ev.seq <= 0)
            continue;
        ev.client = e.value(u"client").toString();
        ev.timeMs = parseTime(e.value(u"time"));
        ev.kind = e.value(u"kind").toString();
        ev.cubePath = e.value(u"cube_path").toString();
        ev.summary = e.value(u"summary").toString();
        ev.payload = e.value(u"payload").toObject();
        page.next = std::max(page.next, ev.seq);
        page.events.append(ev);
    }
    return page;
}

/// The headset's activity as the desktop lists it: newest first, one entry per
/// event, at most kActivityCapacity. A `look` or a `clip` replaces the older
/// one of the same cube, as the hub coalesces them.
class ActivityLog
{
public:
    /// Adds @p events (any order, repeats ignored); true when the list changed.
    bool add(const QList<Event> &events)
    {
        bool changed = false;
        for (const Event &e : events) {
            const bool known = std::any_of(m_events.cbegin(), m_events.cend(), [&e](const Event &x) {
                return x.epoch == e.epoch && x.seq == e.seq;
            });
            if (known)
                continue;
            const QString key = cubeKey(e);
            if (coalesced(e.kind) && !key.isEmpty()) {
                // Only the latest look (or clip) of a cube is kept; one whose
                // cube is not named is kept as it is.
                bool superseded = false;
                for (qsizetype i = m_events.size() - 1; i >= 0; --i) {
                    const Event &x = m_events.at(i);
                    if (x.kind != e.kind || cubeKey(x) != key)
                        continue;
                    if (newerThan(x, e))
                        superseded = true;
                    else
                        m_events.removeAt(i);
                }
                if (superseded)
                    continue;
            }
            m_events.append(e);
            changed = true;
        }
        if (!changed)
            return false;
        std::sort(m_events.begin(), m_events.end(), newerThan);
        while (m_events.size() > kActivityCapacity)
            m_events.removeLast();
        return true;
    }
    const QList<Event> &events() const { return m_events; }
    /// The latest @p kind of the cube with key @p key (cubeKey), if any.
    std::optional<Event> latest(const QString &kind, const QString &key) const
    {
        for (const Event &e : m_events)   // newest first
            if (e.kind == kind && cubeKey(e) == key)
                return e;
        return std::nullopt;
    }
    void clear() { m_events.clear(); }

    static bool coalesced(const QString &kind) { return kind == u"look" || kind == u"clip"; }

private:
    QList<Event> m_events;
};

/// "14:02:07  Moment 0 of m33.fits": a list row.
inline QString eventRowText(const Event &e)
{
    const QString time = e.timeMs > 0
            ? QDateTime::fromMSecsSinceEpoch(e.timeMs).toString(QStringLiteral("HH:mm:ss"))
            : QStringLiteral("--:--:--");
    QString summary = e.summary;
    if (summary.isEmpty()) {
        if (e.kind == u"look")
            summary = QStringLiteral("Look changed");
        else if (e.kind == u"clip")
            summary = QStringLiteral("Clip box changed");
        else if (e.kind == u"cube")
            summary = QStringLiteral("Switched cube");
        else
            summary = e.kind;
    }
    return QStringLiteral("%1  %2").arg(time, summary);
}

// ── Products ────────────────────────────────────────────────────────────────

/// Where a product made in the headset opens on the desktop.
enum class OpenIn {
    Nothing,
    ImageViewer,    ///< a moment map: its FITS in the image viewer
    PvViewer,       ///< a PV slice: the cube window's PV view (or its FITS)
    SpectrumPlot,   ///< a spectrum: a spectrum product of the cube window
    CubeWindow,     ///< an isolated or removed cube
    File,           ///< a catalogue or flow layer: its file, opened by kind
};

struct Product
{
    QString kind;               ///< moment | pv | spectrum | isolate | remove | catalogue | flow
    QString label;
    QString path;               ///< the file the backend wrote ('' none)
    QString workspaceFilename;  ///< its name in the Workspace Exports ('' none)
    QString datasetId;
    QJsonObject request;        ///< the request, under the API's names
    /// From the visivo-recipe/1 record, for Provenance.
    QString recipeId;
    QString recipeKind;
    QList<ReplayStep> replay;
    QString caveat;
    QVariantMap parameters;
    QVariantMap origin;
    /// A spectrum sent with its values (else read from its file).
    QVector<double> axis;
    QVector<double> values;
    QString xLabel;
    QString yLabel;
};

inline QVector<double> doubles(const QJsonValue &v)
{
    QVector<double> out;
    for (const QJsonValue x : v.toArray())
        out.append(x.isDouble() ? x.toDouble() : std::nan(""));
    return out;
}

inline Product parseProduct(const Event &e)
{
    const QJsonObject &p = e.payload;
    Product out;
    out.kind = p.value(u"product").toString();
    if (out.kind.isEmpty())
        out.kind = p.value(u"type").toString();
    out.path = p.value(u"path").toString();
    if (out.path.isEmpty())
        out.path = p.value(u"output_path").toString();
    out.workspaceFilename = p.value(u"workspace_filename").toString();
    if (out.workspaceFilename.isEmpty() && !out.path.isEmpty())
        out.workspaceFilename = QFileInfo(out.path).fileName();
    out.datasetId = p.value(u"dataset_id").toString();
    out.request = p.value(u"request").toObject();
    out.label = p.value(u"label").toString();

    const QJsonObject recipe = p.value(u"recipe").toObject();
    const QJsonObject rp = recipe.value(u"product").toObject();
    out.recipeId = rp.value(u"id").toString();
    out.recipeKind = rp.value(u"kind").toString();
    if (out.label.isEmpty())
        out.label = rp.value(u"label").toString();
    for (const QJsonValue v : rp.value(u"replay").toArray()) {
        const QJsonObject s = v.toObject();
        ReplayStep step;
        step.route = s.value(u"route").toString();
        step.request = s.value(u"request").toObject().toVariantMap();
        step.body = s.value(u"body").toString();
        if (!step.route.isEmpty())
            out.replay.append(step);
    }
    out.caveat = rp.value(u"replay_caveat").toString();
    out.parameters = recipe.value(u"parameters").toObject().toVariantMap();
    out.origin = recipe.value(u"origin").toObject().toVariantMap();

    out.values = doubles(p.value(u"values"));
    out.axis = doubles(p.value(u"axis"));
    out.xLabel = p.value(u"x_label").toString();
    out.yLabel = p.value(u"y_label").toString();
    if (out.label.isEmpty())
        out.label = e.summary;
    return out;
}

inline OpenIn openIn(const Product &p)
{
    const bool hasFile = !p.path.isEmpty();
    if (p.kind == u"moment")
        return hasFile ? OpenIn::ImageViewer : OpenIn::Nothing;
    if (p.kind == u"pv")
        return hasFile || p.request.value(u"vertices").toArray().size() >= 2 ? OpenIn::PvViewer
                                                                              : OpenIn::Nothing;
    if (p.kind == u"spectrum")
        return hasFile || !p.values.isEmpty() ? OpenIn::SpectrumPlot : OpenIn::Nothing;
    if (p.kind == u"isolate" || p.kind == u"remove")
        return hasFile ? OpenIn::CubeWindow : OpenIn::Nothing;
    if (p.kind == u"catalogue" || p.kind == u"flow")
        return hasFile ? OpenIn::File : OpenIn::Nothing;
    return OpenIn::Nothing;
}

/// The product's id in the session's registry: the same however often it is
/// opened, and from whichever window, so it is recorded once.
inline QString productId(const Event &e, const Product &p)
{
    const QString local = !p.recipeId.isEmpty() ? p.recipeId : QStringLiteral("e%1-%2").arg(e.epoch).arg(e.seq);
    return QStringLiteral("headset-%1-%2").arg(e.client, local);
}

/// The PV request's path, as the desktop's PV takes it (pixels of the cube).
inline std::vector<std::array<double, 2>> pvVertices(const Product &p)
{
    std::vector<std::array<double, 2>> out;
    for (const QJsonValue v : p.request.value(u"vertices").toArray()) {
        const QJsonArray xy = v.toArray();
        if (xy.size() >= 2 && xy.at(0).isDouble() && xy.at(1).isDouble())
            out.push_back({ xy.at(0).toDouble(), xy.at(1).toDouble() });
    }
    return out;
}

inline int pvWidth(const Product &p)
{
    return std::max(1, p.request.value(u"width_pixels").toInt(1));
}

/// A spectrum file (the CSV the backend wrote under the exports): the first two
/// numeric columns, a header line and '#' comments skipped. False when it has
/// fewer than two rows.
inline bool parseSpectrumCsv(const QByteArray &text, QVector<double> &axis, QVector<double> &values,
                             QString *xLabel = nullptr, QString *yLabel = nullptr)
{
    axis.clear();
    values.clear();
    bool headerSeen = false;
    for (const QByteArray &raw : text.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QList<QByteArray> cells = line.contains(',') ? line.split(',') : line.simplified().split(' ');
        if (cells.size() < 2)
            continue;
        bool okX = false, okY = false;
        const double x = cells.at(0).trimmed().toDouble(&okX);
        const QByteArray yCell = cells.at(1).trimmed();
        double y = yCell.toDouble(&okY);
        if (!okY && (yCell.compare("nan", Qt::CaseInsensitive) == 0 || yCell.isEmpty())) {
            y = std::nan("");
            okY = true;
        }
        if (!okX || !okY) {
            if (!headerSeen && axis.isEmpty()) {
                headerSeen = true;
                if (xLabel)
                    *xLabel = QString::fromUtf8(cells.at(0).trimmed());
                if (yLabel)
                    *yLabel = QString::fromUtf8(cells.at(1).trimmed());
            }
            continue;
        }
        axis.append(x);
        values.append(y);
    }
    return values.size() >= 2;
}

// ── The headset's look ──────────────────────────────────────────────────────

struct Look
{
    QString colormap;
    std::optional<double> threshold;   ///< data value
    std::optional<double> colourMax;   ///< data value
    std::optional<double> opacity;
    std::optional<double> gamma;
    QString mode;
    QString stretch;
};

inline std::optional<double> number(const QJsonObject &o, std::initializer_list<const char16_t *> keys)
{
    for (const char16_t *k : keys) {
        const QJsonValue v = o.value(QStringView(k));
        if (v.isDouble() && std::isfinite(v.toDouble()))
            return v.toDouble();
    }
    return std::nullopt;
}

inline Look parseLook(const QJsonObject &payload)
{
    Look l;
    l.colormap = payload.value(u"colormap").toString();
    l.threshold = number(payload, { u"threshold" });
    l.colourMax = number(payload, { u"colour_max", u"color_max" });
    l.opacity = number(payload, { u"opacity" });
    l.gamma = number(payload, { u"gamma" });
    const QJsonValue mode = payload.value(u"mode");
    l.mode = mode.isDouble() ? QString::number(mode.toInt()) : mode.toString();
    l.stretch = payload.value(u"stretch").toString();
    return l;
}

/// The desktop colour map for the page's @p name among @p desktopNames: the
/// same name in any case, the page's "grey" as "Gray"; '' when there is none.
inline QString desktopColormap(const QString &name, const QStringList &desktopNames)
{
    if (name.isEmpty())
        return {};
    const QString wanted = name.compare(u"grey", Qt::CaseInsensitive) == 0 ? QStringLiteral("gray") : name;
    for (const QString &d : desktopNames)
        if (d.compare(wanted, Qt::CaseInsensitive) == 0)
            return d;
    return {};
}

/// A clip box in voxels of the full cube, both ends included.
struct Clip
{
    bool set{ false };                 ///< false: no clip (the whole cube)
    std::array<double, 3> min{ 0, 0, 0 };
    std::array<double, 3> max{ 0, 0, 0 };
};

/// The clip of a `clip` event: payload.box (or the payload itself) as
/// {min: [x, y, z], max: [x, y, z]}, [x0, x1, y0, y1, z0, z1] or
/// [[x0, x1], [y0, y1], [z0, z1]]; null or anything else is no clip.
inline Clip parseClip(const QJsonObject &payload)
{
    Clip c;
    const QJsonValue boxValue = payload.contains(u"box") ? payload.value(u"box") : QJsonValue(payload);
    const auto put = [&c](int axis, double a, double b) {
        c.min[axis] = std::min(a, b);
        c.max[axis] = std::max(a, b);
    };
    if (boxValue.isObject()) {
        const QJsonArray lo = boxValue.toObject().value(u"min").toArray();
        const QJsonArray hi = boxValue.toObject().value(u"max").toArray();
        if (lo.size() != 3 || hi.size() != 3)
            return c;
        for (int i = 0; i < 3; ++i) {
            if (!lo.at(i).isDouble() || !hi.at(i).isDouble())
                return Clip{};
            put(i, lo.at(i).toDouble(), hi.at(i).toDouble());
        }
        c.set = true;
        return c;
    }
    const QJsonArray a = boxValue.toArray();
    if (a.size() == 6) {
        for (int i = 0; i < 3; ++i) {
            if (!a.at(2 * i).isDouble() || !a.at(2 * i + 1).isDouble())
                return Clip{};
            put(i, a.at(2 * i).toDouble(), a.at(2 * i + 1).toDouble());
        }
        c.set = true;
    } else if (a.size() == 3) {
        for (int i = 0; i < 3; ++i) {
            const QJsonArray r = a.at(i).toArray();
            if (r.size() != 2 || !r.at(0).isDouble() || !r.at(1).isDouble())
                return Clip{};
            put(i, r.at(0).toDouble(), r.at(1).toDouble());
        }
        c.set = true;
    }
    return c;
}

/// Is the event about the cube at @p path (or with @p datasetId)?
inline bool aboutCube(const Event &e, const QString &path, const QString &datasetId)
{
    if (!e.cubePath.isEmpty())
        return e.cubePath == path;
    const QString id = e.payload.value(u"dataset_id").toString();
    return !id.isEmpty() && id == datasetId;
}

} // namespace VrHub
