#include "BackendRouting.h"

#include <QUrl>

#include "../app/BackendClient.h"

using namespace Qt::StringLiterals;

namespace {

QString trimSlash(QString s)
{
    while (s.endsWith(u'/'))
        s.chop(1);
    return s;
}

/// The URL as a registry key: no trailing slash, and the names of the host's
/// own loopback folded together. SKAVA running in Docker reaches a backend on
/// the desktop's machine as host.docker.internal, while the desktop registers
/// the same backend as 127.0.0.1 or localhost: one machine, three spellings.
QString urlKey(const QString &raw)
{
    QUrl url(trimSlash(raw.trimmed()));
    const QString host = url.host().toLower();
    if (host == "localhost"_L1 || host == "127.0.0.1"_L1 || host == "::1"_L1
        || host == "host.docker.internal"_L1)
        url.setHost(u"127.0.0.1"_s);
    return trimSlash(url.toString());
}

}  // namespace

bool skavaBackendSupportsKind(const QStringList &supportsKinds, const QString &dataproductType)
{
    const QString type = dataproductType.trimmed().toLower();
    if (supportsKinds.isEmpty() || type.isEmpty())
        return true;
    // ObsCore types and the VisIVO backend's kinds differ for one family:
    // a dynamic spectrum is published as a timeseries or a "dynamic-spectrum".
    const QString kind = (type == "timeseries"_L1 || type.startsWith("dynamic"_L1)) ? u"dynspec"_s : type;
    for (const QString &k : supportsKinds) {
        if (k.trimmed().toLower() == kind)
            return true;
    }
    return false;
}

int findRegisteredBackend(const QList<BackendNode> &registry,
                          const SkavaVisivoBackendDescriptor &desc)
{
    const QString descKey = urlKey(desc.endpoint);
    if (!trimSlash(desc.endpoint).isEmpty()) {
        for (int i = 0; i < registry.size(); ++i) {
            if (urlKey(registry[i].url) == descKey)
                return i;
        }
    }
    if (!desc.nodeCode.isEmpty()) {
        for (int i = 0; i < registry.size(); ++i) {
            if (registry[i].srcCode == desc.nodeCode)
                return i;
        }
    }
    return -1;
}

SkavaRouting routeSkavaCandidates(const QList<BackendNode> &registry,
                                  const SkavaDatalinkResponse &datalink,
                                  bool autoToken,
                                  const QString &preferredNode)
{
    SkavaRouting routing;
    const QString type = datalink.dataset.dataproductType;
    for (const auto &desc : datalink.visivoBackends) {
        const QString descUrl = trimSlash(desc.endpoint);
        const QString label = desc.nodeCode.isEmpty() ? descUrl : desc.nodeCode;

        const int idx = findRegisteredBackend(registry, desc);
        const BackendNode *match = idx >= 0 ? &registry[idx] : nullptr;
        if (!match) {
            routing.skipped << u"%1: no registered backend for %2"_s.arg(label, descUrl);
            continue;
        }
        if (!skavaBackendSupportsKind(desc.supportedKinds, type)) {
            routing.skipped << u"%1: the backend does not open %2 datasets (it serves %3)"_s.arg(
                    label, type, desc.supportedKinds.join(u", "_s));
            continue;
        }
        // The automatic token (env / ~/.visivo_token) is the local backend's
        // and is sent to it alone: another backend needs its own.
        if (desc.requiresAuth && match->token.isEmpty()
            && !(autoToken && BackendClient::isLocalBackendUrl(match->url))) {
            routing.skipped << u"%1: the backend requires a token, and \"%2\" has none"_s.arg(
                    label, match->name.isEmpty() ? match->id : match->name);
            continue;
        }
        // The file is opened where it lies: never hand one node's path to
        // another node's backend.
        const QString accessUrl = datalink.accessUrlForNode(desc.nodeCode);
        if (accessUrl.isEmpty()) {
            routing.skipped << u"%1: SKAVA lists no replica of this dataset on that node"_s.arg(label);
            continue;
        }
        SkavaRoute route;
        route.node = *match;
        route.accessUrl = accessUrl;
        route.nodeCode = desc.nodeCode;
        route.endpoint = descUrl;
        if (const SkavaReplica *rep = datalink.replica(desc.nodeCode)) {
            route.rankReason = rep->rankReason;
            route.score = rep->score;
        }
        // The node the user asked for goes first; the others keep SKAVA's order.
        if (!preferredNode.isEmpty() && desc.nodeCode == preferredNode)
            routing.candidates.prepend(route);
        else
            routing.candidates.append(route);
    }
    return routing;
}

SkavaRoute routeSkavaDataset(const QList<BackendNode> &registry,
                             const SkavaDatalinkResponse &datalink,
                             bool autoToken)
{
    const SkavaRouting routing = routeSkavaCandidates(registry, datalink, autoToken);
    SkavaRoute route = routing.candidates.isEmpty() ? SkavaRoute{} : routing.candidates.first();
    route.skipped = routing.skipped;
    return route;
}

SkavaRouting routeSkavaCandidates(Settings *settings, const SkavaDatalinkResponse &datalink,
                                  const QString &preferredNode)
{
    if (!settings)
        return {};
    // A node registered without a token is not unauthenticated when it is the
    // local backend: BackendClient sends VISIVO_TOKEN or ~/.visivo_token to it.
    return routeSkavaCandidates(settings->getBackendNodes(), datalink,
                                !BackendClient::readTokenFile().isEmpty(), preferredNode);
}

SkavaRoute routeSkavaDataset(Settings *settings, const SkavaDatalinkResponse &datalink)
{
    if (!settings)
        return {};
    return routeSkavaDataset(settings->getBackendNodes(), datalink,
                             !BackendClient::readTokenFile().isEmpty());
}

BackendNode pickBackendForSkavaDataset(Settings *settings,
                                       const SkavaDatalinkResponse &datalink)
{
    return routeSkavaDataset(settings, datalink).node;
}
