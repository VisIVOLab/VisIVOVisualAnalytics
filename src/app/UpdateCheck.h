#ifndef UPDATE_CHECK_H
#define UPDATE_CHECK_H
// UpdateCheck.h
// The decisions behind "a newer release is out", kept free of networking and
// widgets so they can be tested: what the GitHub release endpoint says, how two
// version strings compare, and when the next automatic check is due.
//
// The source of truth is the repository's GitHub Releases. Every published tag
// is a release with installers attached, so the newest non-draft,
// non-prerelease entry is the version a user should be running.

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

namespace visivo::update {

/// What the GitHub "latest release" endpoint reported.
struct LatestRelease
{
    bool valid{ false };
    QString version;   ///< the tag, without a leading "v" ("1.0.5")
    QUrl pageUrl;      ///< the release page, where the installers are
    QString error;     ///< why the reply could not be used, when !valid
};

/// The endpoint the client asks. Public API, no authentication.
QUrl latestReleaseApiUrl();

/// The page a user is sent to when the API could not name a release page.
QUrl releasesPageUrl();

/// Parse the JSON body of GET /repos/{owner}/{repo}/releases/latest.
/// A draft or a prerelease is not a version to offer, and a tag that is not a
/// version number ("nightly") is not one either.
LatestRelease parseLatestRelease(const QByteArray &json);

/// "v1.0.5" → "1.0.5"; surrounding blanks are dropped.
QString normalizedVersion(const QString &tag);

/// True when @p text is a plain dotted version ("1.0.5", "v1.0.5"); false for
/// a development build string such as "1.0.5-dev" or an empty one.
bool isComparableVersion(const QString &text);

/// True when @p candidate is a strictly newer version than @p current.
/// Both are compared as dotted numbers (1.0.10 > 1.0.9). A current version that
/// does not parse (a development build) is never told to update, so the check
/// stays quiet rather than nagging about a build it cannot place.
bool isNewer(const QString &candidate, const QString &current);

/// Whether the automatic check should run now. It runs at most once per
/// @p intervalHours; a missing or future timestamp (clock changed) means due.
bool isCheckDue(const QDateTime &lastCheck, const QDateTime &now, int intervalHours = 24);

}   // namespace visivo::update

#endif   // UPDATE_CHECK_H
