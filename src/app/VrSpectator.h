#pragma once

// VrSpectator.h
// The headset's view on the desktop (issue #281): what the spectator routes
// of the headset hub answer, and what the desktop makes of it.
//
//   POST /v1/vr/headset/spectator          {client?, on, fps?, width?}
//                                          → {client, on, fps, width, lease_s, instance};
//                                            404 with a detail: no such page connected
//   GET  /v1/vr/headset/spectator/frame?client=&after=&wait=
//                                          → 200 image/jpeg with X-Visivo-Frame-Seq,
//                                            -Width, -Height, -Eye, -Age-Ms, -Cost-Ms;
//                                            204 none newer in time; 409 not watched;
//                                            404 page unknown
//   POST /v1/vr/headset/spectator/snapshot {client, basename?}
//                                          → {valid, workspace_filename, output_path,
//                                             width, height, seq}; 409 no frame yet
//
// Watching lasts 10 s after the start and after every frame read (the
// backend's lease), so a reader that keeps polling keeps it alive.
//
// An older backend answers with FastAPI's bare 404 "Not Found": "no spectator
// view here", not "no headset" (visivo::isMissingRouteReply).
//
// Widget-free and header-only: unit-tested in tests/test_vr_spectator.cpp.

#include "MissingRoute.h"

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace VrSpectator {

/// What the desktop asks for by default (the page caps them too).
inline constexpr int kDefaultFps = 15;
inline constexpr int kDefaultWidth = 512;
inline constexpr int kMaxFps = 30;
inline constexpr int kMinWidth = 160;
inline constexpr int kMaxWidth = 960;
/// The frame read's wait (s; the backend caps it at 5, the lease is 10).
inline constexpr int kWaitS = 3;

/// The body of POST /v1/vr/headset/spectator (no token: it is in the headers).
inline QJsonObject watchBody(const QString &client, bool on, int fps = kDefaultFps,
                             int width = kDefaultWidth)
{
    QJsonObject o;
    if (!client.isEmpty())
        o.insert(QStringLiteral("client"), client);
    o.insert(QStringLiteral("on"), on);
    if (on) {
        o.insert(QStringLiteral("fps"), std::clamp(fps, 1, kMaxFps));
        o.insert(QStringLiteral("width"), std::clamp(width, kMinWidth, kMaxWidth));
    }
    return o;
}

struct WatchResult
{
    enum class Outcome { Watching, Stopped, NoHeadset, Unsupported, Failed };
    Outcome outcome{ Outcome::Failed };
    QString client;
    int fps{ 0 };
    int width{ 0 };
    QString message;   ///< for the user, when it did not work
};

/// What a POST /v1/vr/headset/spectator answer means.
inline WatchResult interpretWatch(int httpStatus, const QByteArray &body, const QString &qtError)
{
    WatchResult r;
    if (visivo::isMissingRouteReply(httpStatus, body)) {
        r.outcome = WatchResult::Outcome::Unsupported;
        r.message = QStringLiteral("This backend cannot show the headset's view (it is older than "
                                   "this client); update it.");
        return r;
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (httpStatus == 404) {
        r.outcome = WatchResult::Outcome::NoHeadset;
        r.message = QStringLiteral("No headset page is connected any more.");
        return r;
    }
    if (httpStatus >= 200 && httpStatus < 300) {
        r.outcome = o.value(u"on").toBool() ? WatchResult::Outcome::Watching : WatchResult::Outcome::Stopped;
        r.client = o.value(u"client").toString();
        r.fps = o.value(u"fps").toInt();
        r.width = o.value(u"width").toInt();
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

/// The frame read's address.
inline QUrl frameUrl(const QString &baseUrl, const QString &client, qint64 after, int waitS = kWaitS)
{
    QUrl url(baseUrl + QStringLiteral("/v1/vr/headset/spectator/frame"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client"), client);
    q.addQueryItem(QStringLiteral("after"), QString::number(std::max<qint64>(0, after)));
    q.addQueryItem(QStringLiteral("wait"), QString::number(std::clamp(waitS, 0, 5)));
    url.setQuery(q);
    return url;
}

struct FrameReply
{
    enum class Kind {
        Frame,        ///< a new frame: jpeg and its numbers
        None,         ///< nothing newer in time (204): read again
        NotWatched,   ///< 409: the watch ended (start it again)
        Gone,         ///< 404: the page is no longer known
        Unsupported,  ///< a backend without the route
        Failed        ///< anything else: read again later
    };
    Kind kind{ Kind::Failed };
    qint64 seq{ 0 };
    int width{ 0 };
    int height{ 0 };
    QString eye;
    double ageMs{ std::numeric_limits<double>::quiet_NaN() };
    double costMs{ std::numeric_limits<double>::quiet_NaN() };   ///< the headset's capture cost per frame
    QByteArray jpeg;
    QString message;
};

/// A frame read's answer. @p headers: lower-case names to values.
inline FrameReply parseFrameReply(int httpStatus, const QHash<QByteArray, QByteArray> &headers,
                                  const QByteArray &body, const QString &qtError)
{
    FrameReply r;
    if (visivo::isMissingRouteReply(httpStatus, body)) {
        r.kind = FrameReply::Kind::Unsupported;
        r.message = QStringLiteral("This backend cannot show the headset's view (it is older than "
                                   "this client); update it.");
        return r;
    }
    if (httpStatus == 204) {
        r.kind = FrameReply::Kind::None;
        return r;
    }
    if (httpStatus == 409) {
        r.kind = FrameReply::Kind::NotWatched;
        return r;
    }
    if (httpStatus == 404) {
        r.kind = FrameReply::Kind::Gone;
        r.message = QStringLiteral("The headset page is no longer connected.");
        return r;
    }
    const auto num = [&headers](const char *name) {
        bool ok = false;
        const double v = headers.value(QByteArray(name)).toDouble(&ok);
        return ok && std::isfinite(v) ? v : std::numeric_limits<double>::quiet_NaN();
    };
    if (httpStatus == 200) {
        const double seq = num("x-visivo-frame-seq");
        // A JPEG starts with FF D8 FF; anything else is no frame.
        if (std::isnan(seq) || seq < 1 || body.size() < 4 || static_cast<unsigned char>(body[0]) != 0xFF
            || static_cast<unsigned char>(body[1]) != 0xD8) {
            r.message = QStringLiteral("The backend sent something that is not a frame.");
            return r;
        }
        r.kind = FrameReply::Kind::Frame;
        r.seq = static_cast<qint64>(seq);
        const double w = num("x-visivo-frame-width"), h = num("x-visivo-frame-height");
        r.width = std::isnan(w) ? 0 : static_cast<int>(w);
        r.height = std::isnan(h) ? 0 : static_cast<int>(h);
        r.eye = QString::fromLatin1(headers.value(QByteArrayLiteral("x-visivo-frame-eye")));
        r.ageMs = num("x-visivo-frame-age-ms");
        r.costMs = num("x-visivo-frame-cost-ms");
        r.jpeg = body;
        return r;
    }
    const QString detail = QJsonDocument::fromJson(body).object().value(u"detail").toString().left(300);
    r.message = httpStatus == 0
            ? QStringLiteral("The backend did not answer: %1").arg(qtError)
            : QStringLiteral("The backend refused it (HTTP %1)%2")
                      .arg(httpStatus)
                      .arg(detail.isEmpty() ? QString() : QStringLiteral(": %1").arg(detail));
    return r;
}

/// The wait before the next read after @p fails failures in a row (ms).
inline int retryDelayMs(int fails)
{
    static constexpr int kDelays[] = { 500, 1000, 2000, 5000 };
    const int i = std::clamp(fails - 1, 0, int(std::size(kDelays)) - 1);
    return kDelays[i];
}

/// The body of POST /v1/vr/headset/spectator/snapshot.
inline QJsonObject snapshotBody(const QString &client, const QString &basename = {})
{
    QJsonObject o;
    o.insert(QStringLiteral("client"), client);
    if (!basename.isEmpty())
        o.insert(QStringLiteral("basename"), basename);
    return o;
}

struct SnapshotResult
{
    bool ok{ false };
    QString filename;   ///< in the Workspace Exports
    QString path;       ///< on the backend's machine
    int width{ 0 };
    int height{ 0 };
    QString message;
};

inline SnapshotResult interpretSnapshot(int httpStatus, const QByteArray &body, const QString &qtError)
{
    SnapshotResult r;
    if (visivo::isMissingRouteReply(httpStatus, body)) {
        r.message = QStringLiteral("This backend cannot save the headset's view (it is older than "
                                   "this client); update it.");
        return r;
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (httpStatus == 200 && o.value(u"valid").toBool()) {
        r.ok = true;
        r.filename = o.value(u"workspace_filename").toString();
        r.path = o.value(u"output_path").toString();
        r.width = o.value(u"width").toInt();
        r.height = o.value(u"height").toInt();
        return r;
    }
    if (httpStatus == 409) {
        r.message = QStringLiteral("No frame from the headset yet.");
        return r;
    }
    const QString detail = o.value(u"detail").toString().left(300);
    r.message = httpStatus == 0
            ? QStringLiteral("The backend did not answer: %1").arg(qtError)
            : QStringLiteral("Not saved (HTTP %1)%2")
                      .arg(httpStatus)
                      .arg(detail.isEmpty() ? QString() : QStringLiteral(": %1").arg(detail));
    return r;
}

/// The frame rate seen, from the times (ms, any monotonic clock) frames came:
/// over the last two seconds.
class RateMeter
{
public:
    void add(qint64 ms)
    {
        m_times.append(ms);
        while (!m_times.isEmpty() && ms - m_times.first() > 2000)
            m_times.removeFirst();
    }
    double fps(qint64 nowMs) const
    {
        QList<qint64> recent;
        for (qint64 t : m_times)
            if (nowMs - t <= 2000)
                recent.append(t);
        if (recent.size() < 2)
            return 0.0;
        return 1000.0 * double(recent.size() - 1) / double(std::max<qint64>(1, recent.last() - recent.first()));
    }
    void clear() { m_times.clear(); }

private:
    QList<qint64> m_times;
};

/// The window's status line: rate, size, how old the frame is, and what
/// capturing costs the headset (when it said).
inline QString statusLine(double fps, int width, int height, double ageMs, double costMs)
{
    QString s = QStringLiteral("%1 fps, %2×%3").arg(fps, 0, 'f', 1).arg(width).arg(height);
    if (std::isfinite(ageMs))
        s += QStringLiteral(", %1 ms old").arg(qRound(ageMs));
    if (std::isfinite(costMs))
        s += QStringLiteral(", headset capture %1 ms/frame").arg(costMs, 0, 'f', 2);
    return s;
}

} // namespace VrSpectator
