// BackendUrl.cpp — see BackendUrl.h.

#include "BackendUrl.h"

#include <QHostInfo>
#include <QNetworkInterface>
#include <QUrl>

namespace BackendUrl {

QString hostOf(const QString &url)
{
    return QUrl(url.trimmed()).host().trimmed().toLower();
}

int portOf(const QString &url, int fallback)
{
    // The same trim as hostOf(): a trailing space in the stored setting gave
    // the right host and the default port, so uvicorn bound 8000 while the
    // client polled 9000 and gave up thirty seconds later.
    return QUrl(url.trimmed()).port(fallback);
}

QStringList thisMachineNames()
{
    const QString hostName = QHostInfo::localHostName().trimmed().toLower();
    if (hostName.isEmpty()) {
        return {};
    }
    QStringList names{ hostName };

    // Only the forms the system actually reported, never one we composed.
    // macOS reports the name already suffixed ("mybook.local") while Linux
    // usually reports the bare label, and the user may have typed either — but
    // appending a domain or ".local" of our own invention would claim a name
    // that may belong to a different host on this network, which is how a
    // confident wrong answer gets made.
    const QString shortName = hostName.section(QLatin1Char('.'), 0, 0);
    if (shortName != hostName) {
        names << shortName;
    }
    return names;
}

Locality localityOf(const QString &url,
                    const QList<QHostAddress> &localAddresses,
                    const QStringList &localNames)
{
    const QString host = hostOf(url);

    if (host.isEmpty()) {
        // Empty setting: the caller's default is loopback, so this must not
        // become a refusal to start anything over a field the user never
        // touched.
        if (url.trimmed().isEmpty()) {
            return Locality::ThisMachine;
        }
        // Text that is not a URL — "localhost:9000" without a scheme, say,
        // which QUrl reads as a scheme and a path. Every other reader builds a
        // broken request from it, so the client used to bind 127.0.0.1:8000,
        // poll an address that does not resolve, time out after thirty seconds
        // and leave the orphaned server behind to make the next attempt fail
        // with "address already in use". Saying so is better than any of that.
        return Locality::Malformed;
    }

    QHostAddress address;
    if (address.setAddress(host)) {
        if (address.isLoopback()) {
            return Locality::ThisMachine;
        }
        // 0.0.0.0 and :: mean "every interface on this machine", which is a
        // bind target rather than a destination — and an ordinary thing to have
        // configured when the backend should be reachable from outside.
        if (address == QHostAddress(QHostAddress::Any)
            || address == QHostAddress(QHostAddress::AnyIPv4)
            || address == QHostAddress(QHostAddress::AnyIPv6)) {
            return Locality::ThisMachine;
        }
        for (const QHostAddress &local : localAddresses) {
            // isEqual ignores the scope id and matches a v4 address against its
            // v4-mapped v6 form, which is how one interface is reported two
            // different ways.
            if (address.isEqual(local, QHostAddress::TolerantConversion)) {
                return Locality::ThisMachine;
            }
        }
        // The one case we can be sure about: a literal address the kernel does
        // not hold. No bind to it can succeed here.
        return Locality::NotThisMachine;
    }

    if (host == QStringLiteral("localhost") || host.endsWith(QStringLiteral(".localhost"))) {
        return Locality::ThisMachine;
    }
    for (const QString &name : localNames) {
        if (!name.isEmpty() && host == name) {
            return Locality::ThisMachine;
        }
    }
    return Locality::Unknown;
}

Locality localityOf(const QString &url)
{
    return localityOf(url, QNetworkInterface::allAddresses(), thisMachineNames());
}

}   // namespace BackendUrl
