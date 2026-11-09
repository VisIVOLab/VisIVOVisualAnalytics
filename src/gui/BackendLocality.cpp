#include "BackendLocality.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUrl>

namespace visivo {

namespace {

// Scheme-aware default port, so an implicit https URL and an explicit :443
// are the same endpoint.
int portOf(const QUrl &u)
{
    return u.port(u.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443
                                                                                        : 80);
}

// 127.0.0.0/8 and ::1, whatever spelling the user typed (QUrl::host()
// already strips the brackets of an IPv6 literal). QHostAddress parses
// numeric addresses only — it does NOT resolve names.
bool isLoopback(const QString &h)
{
    if (h == QStringLiteral("localhost"))
        return true;
    const QHostAddress addr(h);
    return !addr.isNull() && addr.isLoopback();
}

bool sameEndpoint(const QUrl &a, const QUrl &b)
{
    const QString hostA = a.host().toLower();
    const QString hostB = b.host().toLower();
    if (hostA.isEmpty() || hostB.isEmpty())
        return false;
    return portOf(a) == portOf(b)
            && (hostA == hostB || (isLoopback(hostA) && isLoopback(hostB)));
}

// Read raw with QSettings, NOT through Settings: that constructor launches
// python3 (and possibly python) with 30-second synchronous waits to locate
// an interpreter, which is not something to do while composing a banner
// (main.cpp avoids it for the same reason when reading the theme).
QString rawSettingsPath()
{
    return QDir(QDir::home().absoluteFilePath(QCoreApplication::applicationName()))
            .absoluteFilePath(QStringLiteral("Settings.ini"));
}

} // namespace

bool backendIsLocal(const QString &backendUrl)
{
    // A run on a remote backend (--backend) has no local one, even when a
    // tunnel puts the remote backend at a localhost address.
    if (Settings::sessionBackend())
        return false;
    const QUrl url(backendUrl);
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return false;
    QSettings raw(rawSettingsPath(), QSettings::IniFormat);
    // The URL of the LOCAL backend this run started (no --backend, checked
    // above) is local whatever the registry says: no tunnel can hold its port
    // while it runs, so a remote node registered there is a stale entry (an
    // old tunnel to a cluster on the same port), not this backend.
    const QUrl localUrl(raw.value(QStringLiteral("Backend/url"),
                                  QStringLiteral("http://127.0.0.1:8000"))
                                .toString());
    const QString localHost = localUrl.host().toLower();
    if (!localHost.isEmpty() && portOf(localUrl) == portOf(url)
        && (localHost == host || (isLoopback(localHost) && isLoopback(host))))
        return true;
    // A loopback URL is not proof either: a node on a cluster is reached
    // through an SSH tunnel on a local port, and the backends registry, where
    // the user added it, says it is remote.
    const QJsonArray nodes =
            QJsonDocument::fromJson(raw.value(QStringLiteral("Backends/nodes")).toString().toUtf8())
                    .array();
    for (const QJsonValue &value : nodes) {
        const QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("is_local")).toBool(false))
            continue;
        const QUrl nodeUrl(node.value(QStringLiteral("url")).toString());
        const QString nodeHost = nodeUrl.host().toLower();
        const bool sameHost = nodeHost == host || (isLoopback(nodeHost) && isLoopback(host));
        if (sameHost && portOf(nodeUrl) == portOf(url))
            return false;
    }
    if (isLoopback(host))
        return true;
    // A hosts-file alias for 127.0.0.1 was matched against the local
    // backend's URL above; nothing is resolved on a UI path.
    return false;
}

QString backendDisplayName(const QString &backendUrl)
{
    const QUrl url(backendUrl);
    // An IPv6 literal keeps its brackets, or its port reads as one more group.
    const QString host = url.host().contains(QLatin1Char(':'))
            ? QStringLiteral("[%1]").arg(url.host())
            : url.host();
    const QString hostPort = url.host().isEmpty()
            ? backendUrl
            : (url.port() > 0 ? QStringLiteral("%1:%2").arg(host).arg(url.port()) : host);
    // This run's remote backend (--backend, --connect-cluster), by the name
    // the status bar gives it.
    if (const Settings::SessionBackend *session = Settings::sessionBackend()) {
        if (sameEndpoint(QUrl(session->url), url))
            return session->name.isEmpty() ? hostPort : session->name;
    }
    if (backendIsLocal(backendUrl))
        return QStringLiteral("Local");
    // A node of the backends registry (a dataset opened through discovery is
    // served by one), by the name the user gave it.
    QSettings raw(rawSettingsPath(), QSettings::IniFormat);
    const QJsonArray nodes =
            QJsonDocument::fromJson(raw.value(QStringLiteral("Backends/nodes")).toString().toUtf8())
                    .array();
    for (const QJsonValue &value : nodes) {
        const QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("is_local")).toBool(false))
            continue;
        if (!sameEndpoint(QUrl(node.value(QStringLiteral("url")).toString()), url))
            continue;
        const QString name = node.value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty())
            return name;
        const QString id = node.value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty())
            return id;
    }
    return hostPort;
}

} // namespace visivo
