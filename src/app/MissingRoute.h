#pragma once

// MissingRoute.h
// A backend older than this client has no route for the features added since
// it was built, and FastAPI answers such a request with a bare 404
// {"detail":"Not Found"} (405 "Method Not Allowed" for a path that exists
// with another verb). Shown as it is, "Not Found" says nothing to the user;
// these helpers tell that answer apart from a route's own 404 ("Unknown
// render session_id.", "File not found.") and say what is wrong instead.
// Widget-free, so it is unit-tested.

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace visivo {

/// A 404 or 405 that is not a route's own answer: the route is not there.
/// FastAPI says so with "Not Found" / "Method Not Allowed"; a proxy or tunnel
/// in front of an older backend may say it with an HTML page or nothing. A
/// route answers a missing thing (a file, a session) with a JSON detail of
/// its own, which must not be taken for an old backend.
inline bool isMissingRouteReply(int httpStatus, const QByteArray &body)
{
    if (httpStatus != 404 && httpStatus != 405)
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    const QJsonValue detail = doc.isObject() ? doc.object().value(u"detail") : QJsonValue();
    if (!detail.isString())
        return true;   // not a route's JSON error
    const QString text = detail.toString();
    return text == u"Not Found" || text == u"Method Not Allowed";
}

/// What to tell the user when the backend has no route for a feature:
/// `backendName` as visivo::backendDisplayName gives it ("Local" for the
/// local backend), `feature` as a plural noun phrase ("ray-traced views").
inline QString missingFeatureMessage(const QString &backendName, const QString &feature)
{
    const QString who = backendName.isEmpty() || backendName == u"Local"
            ? QStringLiteral("The local backend")
            : QStringLiteral("The backend on %1").arg(backendName);
    return QStringLiteral("%1 does not have %2 (it is older than this client); update it.")
            .arg(who, feature);
}

} // namespace visivo
