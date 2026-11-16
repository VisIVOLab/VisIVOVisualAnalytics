#pragma once

// VrPairService.h
// Which headset pairing service and public loader a backend started by this
// desktop is given, as VISIVO_VR_PAIR_URL and VISIVO_VR_PAGE_URL (issues #214
// and #344). Three sources, in this order:
//
//   1. the variables already set in the environment the desktop starts from;
//   2. the two fields of Settings ▸ Backends ▸ Headset Pairing;
//   3. the build's defaults (VrPairingDefaults.h, from the CMake cache options
//      VISIVO_DEFAULT_VR_PAIR_URL and VISIVO_DEFAULT_VR_PAGE_URL).
//
// An empty field means "use the default". "Pair headsets by code" switched off
// means the desktop passes nothing. The backend itself has no built-in host:
// one started by hand or on a cluster stays configured by its own environment.
//
// Widget-free and header-only: unit-tested in tests/test_vr_pair.cpp.

#include "VrPairingDefaults.h"

#include <QProcessEnvironment>
#include <QString>
#include <QUrl>

namespace VrPairService {

inline const QString kPairUrlVar = QStringLiteral("VISIVO_VR_PAIR_URL");
inline const QString kPageUrlVar = QStringLiteral("VISIVO_VR_PAGE_URL");

/// What the user chose in Settings ▸ Backends ▸ Headset Pairing.
struct Fields
{
    bool enabled{ true };   ///< "Pair headsets by code"
    QString serviceUrl;     ///< empty: the default
    QString pageUrl;        ///< empty: the default
};

/// What this build offers when a field is empty. Empty: no default.
struct Defaults
{
    QString serviceUrl;
    QString pageUrl;
};

/// The defaults compiled into this build.
inline Defaults buildDefaults()
{
    return { QString::fromUtf8(VrPairingDefaults::kServiceUrl).trimmed(),
             QString::fromUtf8(VrPairingDefaults::kPageUrl).trimmed() };
}

/// The fields as "Restore defaults" leaves them: pairing on, both fields empty,
/// so a later build's defaults apply too.
inline Fields restoredDefaults()
{
    return {};
}

/// The loader's address as the backend reads it (backend/app/vr_pair_host.py,
/// page_url): the loader finds its config.json and scripts relative to its own
/// address, so a directory given without its trailing slash gets one, while a
/// path whose last segment names a file (index.html, index%2Ehtml) is kept.
/// The rest of the text is left exactly as given.
inline QString loaderAddress(const QString &url)
{
    const QString text = url.trimmed();
    if (text.isEmpty())
        return text;
    // Split on the text as given, as Python's urlsplit does, rather than on
    // QUrl's decoded path: an encoded slash (%2F) must stay inside its segment.
    // The path ends where the query or the fragment starts (neither may
    // appear unencoded in a path or a host).
    qsizetype end = text.size();
    for (const QChar mark : { u'?', u'#' }) {
        const qsizetype at = text.indexOf(mark);
        if (at >= 0 && at < end)
            end = at;
    }
    const QString head = text.left(end);
    // The authority starts with // at the very start or straight after the
    // scheme's colon; a // further on belongs to the path.
    const qsizetype colon = head.indexOf(u':');
    const qsizetype firstSlash = head.indexOf(u'/');
    qsizetype authority = -1;
    if (head.startsWith(u"//"))
        authority = 0;
    else if (colon > 0 && (firstSlash < 0 || colon < firstSlash)
             && QStringView(head).mid(colon + 1).startsWith(u"//"))
        authority = colon + 1;
    const qsizetype pathStart =
            authority < 0 ? 0 : head.indexOf(u'/', authority + 2);
    const QString path = pathStart < 0 ? QString() : head.mid(pathStart);
    if (path.endsWith(u'/'))
        return text;
    const QString last = path.mid(path.lastIndexOf(u'/') + 1);
    if (QUrl::fromPercentEncoding(last.toUtf8()).contains(u'.'))
        return text;
    return head + u'/' + text.mid(end);
}

namespace detail {
/// Service addresses compared as the backend uses them: it strips the
/// trailing slashes before adding /v1/host.
inline bool sameService(QString a, QString b)
{
    while (a.endsWith(u'/'))
        a.chop(1);
    while (b.endsWith(u'/'))
        b.chop(1);
    return a == b;
}
}   // namespace detail

/// Whether an empty *Headset page* stands for the default loader while the
/// pairing service in effect is @p serviceUrl. The default loader talks to the
/// default service only (its config.json names that one), so with any other
/// service an empty page passes nothing, as before there was a default: the
/// backend then gives the code without a QR address.
inline bool defaultPageApplies(const QString &serviceUrl, const Defaults &defaults)
{
    return !defaults.pageUrl.isEmpty() && !defaults.serviceUrl.isEmpty()
            && detail::sameService(serviceUrl.trimmed(), defaults.serviceUrl);
}

/// The two values the desktop passes to a backend it starts. Empty: not passed.
struct Values
{
    QString serviceUrl;
    QString pageUrl;
};

/// What the desktop adds to @p env: each value the environment does not set
/// already. Nothing when pairing is switched off.
inline Values toPass(const QProcessEnvironment &env, const Fields &fields,
                        const Defaults &defaults)
{
    if (!fields.enabled)
        return {};
    const QString envService = env.value(kPairUrlVar).trimmed();
    const QString envPage = env.value(kPageUrlVar).trimmed();

    const QString fieldService = fields.serviceUrl.trimmed();
    const QString service = fieldService.isEmpty() ? defaults.serviceUrl.trimmed()
                                                   : fieldService;
    const QString serviceInEffect = envService.isEmpty() ? service : envService;

    QString page = loaderAddress(fields.pageUrl);
    if (page.isEmpty() && defaultPageApplies(serviceInEffect, defaults))
        page = loaderAddress(defaults.pageUrl);

    return { envService.isEmpty() ? service : QString(), envPage.isEmpty() ? page : QString() };
}

/// Puts the values toPass() chose into @p env.
inline void apply(QProcessEnvironment &env, const Fields &fields, const Defaults &defaults)
{
    const Values add = toPass(env, fields, defaults);
    if (!add.serviceUrl.isEmpty())
        env.insert(kPairUrlVar, add.serviceUrl);
    if (!add.pageUrl.isEmpty())
        env.insert(kPageUrlVar, add.pageUrl);
}

}   // namespace VrPairService
