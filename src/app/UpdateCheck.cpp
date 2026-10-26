#include "UpdateCheck.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

using namespace Qt::StringLiterals;

namespace visivo::update {

namespace {

/// A version the comparison can trust: dotted numbers and nothing after them.
/// QVersionNumber::fromString would read "1.0.5-dev" as 1.0.5 and report the
/// suffix separately; a suffix means a build we cannot place, so it is refused.
QVersionNumber strictVersion(const QString &text)
{
    static const QRegularExpression pattern(u"^\\d+(\\.\\d+)*$"_s);
    const QString v = normalizedVersion(text);
    if (!pattern.match(v).hasMatch())
        return {};
    return QVersionNumber::fromString(v);
}

}   // namespace

QUrl latestReleaseApiUrl()
{
    return QUrl(u"https://api.github.com/repos/VisIVOLab/VisIVOVisualAnalytics/releases/latest"_s);
}

QUrl releasesPageUrl()
{
    return QUrl(u"https://github.com/VisIVOLab/VisIVOVisualAnalytics/releases/latest"_s);
}

QString normalizedVersion(const QString &tag)
{
    QString v = tag.trimmed();
    if (v.startsWith(u'v') || v.startsWith(u'V'))
        v.remove(0, 1);
    return v;
}

LatestRelease parseLatestRelease(const QByteArray &json)
{
    LatestRelease out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        out.error = u"the release service did not return JSON"_s;
        return out;
    }
    const QJsonObject o = doc.object();
    if (o.value(u"draft"_s).toBool() || o.value(u"prerelease"_s).toBool()) {
        out.error = u"the newest release is not a final release"_s;
        return out;
    }
    const QString version = normalizedVersion(o.value(u"tag_name"_s).toString());
    if (strictVersion(version).isNull()) {
        out.error = version.isEmpty() ? u"the reply names no release"_s
                                      : u"the release tag \"%1\" is not a version"_s.arg(version);
        return out;
    }
    // Only this project's own release pages are followed; anything else falls
    // back to the releases page rather than opening an arbitrary URL. The path
    // is normalised first, so "/VisIVOLab/VisIVOVisualAnalytics/../../x" is
    // judged as the "/x" it resolves to, and must then be a release route.
    static const QRegularExpression releaseRoute(
            u"^/VisIVOLab/VisIVOVisualAnalytics/releases(/tag/[A-Za-z0-9._-]+|/latest)?/?$"_s);
    const QUrl page = QUrl(o.value(u"html_url"_s).toString())
                              .adjusted(QUrl::NormalizePathSegments);
    const bool pageOk = page.isValid() && page.scheme() == u"https"_s
                        && page.host() == u"github.com"_s && page.userInfo().isEmpty()
                        && page.port() == -1 && !page.hasQuery() && !page.hasFragment()
                        && releaseRoute.match(page.path()).hasMatch();
    out.valid = true;
    out.version = version;
    out.pageUrl = pageOk ? page : releasesPageUrl();
    return out;
}

bool isComparableVersion(const QString &text)
{
    return !strictVersion(text).isNull();
}

bool isNewer(const QString &candidate, const QString &current)
{
    const QVersionNumber c = strictVersion(candidate);
    const QVersionNumber cur = strictVersion(current);
    if (c.isNull() || cur.isNull())
        return false;
    return QVersionNumber::compare(c, cur) > 0;
}

bool isCheckDue(const QDateTime &lastCheck, const QDateTime &now, int intervalHours)
{
    if (!lastCheck.isValid() || lastCheck > now)
        return true;
    return lastCheck.secsTo(now) >= qint64(intervalHours) * 3600;
}

}   // namespace visivo::update
