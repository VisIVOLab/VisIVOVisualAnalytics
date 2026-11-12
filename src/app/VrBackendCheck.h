#pragma once

// VrBackendCheck.h
// What the backend's answer to GET /v1/vr/backend_check (issue #187) means for the
// headset, for "Open in VR Headset…": a backend on a cluster often runs an
// overlay of its code (--backend-src) older than this desktop, and the page
// then fails route by route with a bare 404. The check says so before the
// headset is involved.
//
// Widget-free and header-only: unit-tested in tests/test_vr_link.cpp.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QStringList>

namespace VrBackendCheck {

/// The headset protocol level this desktop's page expects (backend
/// app/routers/vr_backend_check.py VR_API).
inline constexpr int kRequiredApi = 2;

enum class Severity { Ok, Warning, Error };

struct Result
{
    Severity severity{ Severity::Ok };
    QString message;   ///< for the user, plain text ('' when there is nothing to say)
    QString build;     ///< the page build the backend serves, if it said
    bool depthOff{ false };   ///< the backend sends stereo frames without depth
};

/// @p httpStatus 0: no answer at all (the request failed); @p body the reply.
inline Result interpret(int httpStatus, const QByteArray &body, bool remote)
{
    using namespace Qt::StringLiterals;
    Result r;
    const QString where = remote ? u"On a cluster this usually means the backend runs an "
                                   u"older copy of its code (a --backend-src overlay or an "
                                   u"image older than this desktop): update it, see the guide."_s
                                 : u"Update the backend, or restart the desktop's local backend."_s;
    if (httpStatus == 0) {
        r.severity = Severity::Warning;
        r.message = u"The backend did not answer the headset check; the page may still work."_s;
        return r;
    }
    if (httpStatus == 401 || httpStatus == 403) {
        r.severity = Severity::Error;
        r.message = u"The backend refused this desktop's token, so the headset would be refused "
                    u"too."_s;
        return r;
    }
    if (httpStatus == 404) {
        r.severity = Severity::Warning;
        r.message = u"This backend predates the headset check (/v1/vr/backend_check): its "
                    u"headset page and routes may be older than this desktop. "_s + where;
        return r;
    }
    if (httpStatus >= 300 && httpStatus < 400) {
        // A portal's sign-in page, or a proxy moving the route: not followed.
        r.severity = Severity::Warning;
        r.message = u"The headset check was redirected (HTTP %1), as a portal's sign-in does; "
                    u"the page may still work."_s.arg(httpStatus);
        return r;
    }
    if (httpStatus != 200) {
        r.severity = Severity::Warning;
        r.message = u"The headset check failed (HTTP %1); the page may still work."_s.arg(httpStatus);
        return r;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject o = doc.object();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()
        || !o.value(u"page"_s).isObject()) {
        // A sign-in page after a redirect, a route of another shape: unknown,
        // never a reason to refuse.
        r.severity = Severity::Warning;
        r.message = u"The backend's answer to the headset check was not understood; the page "
                    u"may still work."_s;
        return r;
    }
    const QJsonObject page = o.value(u"page"_s).toObject();
    r.build = page.value(u"build"_s).toString();
    r.depthOff = o.value(u"stereo"_s).toObject().value(u"depth"_s).toString() == u"off";
    // Refused only when the backend says outright that it has no page.
    if (page.value(u"present"_s).isBool() && !page.value(u"present"_s).toBool()) {
        r.severity = Severity::Error;
        r.message = u"This backend serves no headset page (app/static/vr is missing from its "
                    u"code). "_s + where;
        return r;
    }
    QStringList missing;
    for (const QJsonValue &v : o.value(u"missing_routes"_s).toArray())
        missing << v.toString();
    const int api = o.value(u"vr_api"_s).toInt(0);
    if (!missing.isEmpty()) {
        r.severity = Severity::Warning;
        r.message = u"This backend lacks routes the headset page calls (%1). "_s.arg(
                            missing.join(u", "_s))
                + where;
        return r;
    }
    if (api < kRequiredApi) {
        r.severity = Severity::Warning;
        r.message = u"This backend speaks an older headset protocol (%1, this desktop expects "
                    u"%2). "_s.arg(api).arg(kRequiredApi)
                + where;
        return r;
    }
    if (r.depthOff)
        r.message = u"The backend sends refined frames without depth (VISIVO_XR_DEPTH=0): the "
                    u"headset warps them by the pose alone."_s;
    return r;
}

}   // namespace VrBackendCheck
