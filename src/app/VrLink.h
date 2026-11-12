#pragma once

// VrLink.h
// The address that opens the backend's WebXR page (/vr/) on a headset in the
// SAME backend session as the desktop, with the cube the desktop has open
// (issue #172).
//
// The headset reaches the backend as http://localhost:<port>: `adb reverse`
// forwards the headset's own localhost:<port> to this machine's, and only a
// localhost page is a secure context for WebXR without a certificate. So the
// link is built from the PORT of the client's backend URL, never its host,
// and it can only work when that URL is served on this machine's loopback —
// the local backend, or a remote one reached through a local tunnel
// (localhost:18960 for a cluster). A backend at another address is not
// reachable through `adb reverse` at all, and the builder says so instead of
// producing a link that would fail on the headset.
//
// A path prefix (issue #187): a backend behind a reverse proxy that strips
// /some/prefix (an HPC portal, reached through a local tunnel) serves the page
// at <prefix>/vr/ and the API at <prefix>/v1. The link opens the page there,
// and the page finds the prefix in its own address (static/vr/js/base-path.js).
//
// Everything the page needs rides the fragment (#token=…&session=…&dataset=…
// &path=…, and &kind=points|velocity&box=… for a point table or a velocity
// field, issue #190): a browser never sends the fragment to the server, so the token is
// in no access log, and the page removes it from the address bar. Every value
// is percent-encoded completely, so the link holds no character a shell or
// the fragment parser would read as syntax ('&', '#', '=', quotes, spaces).
//
// Widget-free and header-only: unit-tested in tests/test_vr_link.cpp.

#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace VrLink {

struct Inputs
{
    QString baseUrl;     ///< the client's backend base URL (http://127.0.0.1:8000, a tunnel…)
    QString token;       ///< the token that authenticates on that backend
    QString sessionId;   ///< X-Visivo-Session the desktop's requests carry
    QString datasetId;   ///< the cube open in the viewer
    QString path;        ///< the cube's path on the backend
    /// What the dataset is, for the page's mode (issue #190): empty or "cube",
    /// "points" (a VBT point table or a 3-D catalogue), "velocity" (a field).
    QString kind;
    /// A velocity field's box size in Mpc, to open its path again ("" = grid units).
    QString boxMpc;
    /// Not the desktop's local backend (BackendClient::isLocalBackendUrl):
    /// a cluster's, reached through a local tunnel. Only labels the link.
    bool remote{ false };
};

struct Link
{
    bool ok{ false };
    QString error;      ///< why there is no link, for the user (no secrets in it)
    int port{ 0 };      ///< the port to forward with `adb reverse`
    QString prefix;     ///< the backend's path prefix ('' at the root, else "/a/b")
    bool remote{ false };   ///< a remote backend through a local tunnel
    QString checkUrl;   ///< <backend>/v1/vr/backend_check, as this computer reaches it
    QString url;        ///< the full link: CONTAINS THE TOKEN, never log it
    QString pageUrl;    ///< the page without the fragment: safe to show and log
};

/// Percent-encode everything but RFC 3986's unreserved characters, so a value
/// cannot end its fragment parameter early or be read by a shell.
inline QString encodeValue(const QString &value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

/// The hosts `adb reverse tcp:P tcp:P` reaches: it connects to this
/// computer's 127.0.0.1 / ::1, so a backend bound to another 127/8 address
/// (127.0.0.2) or under another name is not behind it.
inline bool isLoopbackHost(const QString &host)
{
    using namespace Qt::StringLiterals;
    QString h = host.trimmed().toLower();
    if (h.startsWith(u'[') && h.endsWith(u']'))
        h = h.mid(1, h.size() - 2);
    if (h == u"localhost")
        return true;
    const QHostAddress address(h);
    return address == QHostAddress(QHostAddress::LocalHost)
            || address == QHostAddress(QHostAddress::LocalHostIPv6);
}

inline Link build(const Inputs &in)
{
    using namespace Qt::StringLiterals;
    Link link;
    const QUrl base(in.baseUrl.trimmed());
    const QString scheme = base.scheme().toLower();
    if (!base.isValid() || base.host().isEmpty() || (scheme != u"http" && scheme != u"https")) {
        link.error = QStringLiteral("The backend address is not an http(s) URL.");
        return link;
    }
    if (!isLoopbackHost(base.host())) {
        link.error = QStringLiteral(
                "This backend is at %1, not on this computer's localhost, so the headset "
                "cannot reach it through adb. Connect to it through a local tunnel "
                "(as for a cluster) and open the cube again.")
                             .arg(base.host());
        return link;
    }
    // A backend under a path prefix serves the page and the API under it.
    // Kept percent-encoded as written: it goes back into a URL as it is.
    // "." and ".." are resolved and empty segments dropped here rather than by
    // QUrl::NormalizePathSegments, whose result differs between Qt versions
    // (6.10 turns "/a/./b/../c/" into "/a//c"); the page collapses repeated
    // slashes the same way (base-path.js).
    QStringList segments;
    for (const QString &seg : base.path(QUrl::FullyEncoded).split(u'/', Qt::SkipEmptyParts)) {
        if (seg == QStringLiteral("."))
            continue;
        if (seg == QStringLiteral("..")) {
            if (!segments.isEmpty())
                segments.removeLast();
            continue;
        }
        segments << seg;
    }
    const QString prefix = segments.isEmpty() ? QString()
                                              : u'/' + segments.join(u'/');
    if (base.hasQuery() || base.hasFragment()) {
        link.error = QStringLiteral(
                "The backend address has a query or a fragment: give the headset the "
                "backend's own address (scheme, host, port and path only).");
        return link;
    }
    if (in.datasetId.isEmpty()) {
        link.error = QStringLiteral("The cube has no backend dataset yet: wait for it to load.");
        return link;
    }
    link.port = base.port(scheme == u"https" ? 443 : 80);

    // Host forced to localhost: on the headset 127.0.0.1 would work too, but
    // localhost is what browsers treat as a secure context by name, and what
    // the guide tells the user to type.
    QUrl page;
    page.setScheme(scheme);
    page.setHost(QStringLiteral("localhost"));
    page.setPort(link.port);
    page.setPath(prefix + QStringLiteral("/vr/"), QUrl::StrictMode);
    link.pageUrl = page.toString(QUrl::FullyEncoded);
    link.prefix = prefix;
    link.remote = in.remote;
    QUrl check = base;
    check.setPath(prefix + QStringLiteral("/v1/vr/backend_check"), QUrl::StrictMode);
    link.checkUrl = check.toString(QUrl::FullyEncoded);

    QStringList fragment;
    if (!in.token.isEmpty())
        fragment << u"token="_s + encodeValue(in.token);
    if (!in.sessionId.isEmpty())
        fragment << u"session="_s + encodeValue(in.sessionId);
    fragment << u"dataset="_s + encodeValue(in.datasetId);
    if (!in.path.isEmpty())
        fragment << u"path="_s + encodeValue(in.path);
    if (!in.kind.isEmpty() && in.kind != u"cube")
        fragment << u"kind="_s + encodeValue(in.kind);
    if (!in.boxMpc.isEmpty())
        fragment << u"box="_s + encodeValue(in.boxMpc);
    // Appended by hand: QUrl::setFragment would re-encode the '%' of the
    // values (or decode them, depending on the mode) and the page decodes once.
    link.url = link.pageUrl + u'#' + fragment.join(u'&');
    link.ok = true;
    return link;
}

/// @p text with every secret (as written, and as encoded in a link) replaced
/// by a placeholder: for anything shown to the user or logged that may echo
/// the link back, such as adb's own output.
inline QString redact(QString text, const QStringList &secrets)
{
    for (const QString &secret : secrets) {
        if (secret.isEmpty())
            continue;
        text.replace(encodeValue(secret), QStringLiteral("<redacted>"));
        text.replace(secret, QStringLiteral("<redacted>"));
    }
    return text;
}

}   // namespace VrLink
