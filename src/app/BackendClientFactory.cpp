#include "BackendClientFactory.h"
#include "BackendClient.h"
#include "../Settings.h"

#include <QCoreApplication>
#include <QPointer>

using namespace Qt::StringLiterals;

namespace {

/// Singleton storage. The factory itself is a QObject parented to
/// QCoreApplication so it lives for the entire app lifetime.
static QPointer<BackendClientFactory> s_instance;

}  // namespace

BackendClientFactory *BackendClientFactory::instance()
{
    if (!s_instance) {
        // Parent to qApp so destruction is deterministic on shutdown.
        s_instance = new BackendClientFactory(qApp);
    }
    return s_instance.data();
}

BackendClientFactory::BackendClientFactory(QObject *parent)
    : QObject(parent)
{
}

void BackendClientFactory::setSettings(Settings *settings)
{
    if (m_settings == settings) return;
    if (m_settings) {
        QObject::disconnect(m_settings, &Settings::updated,
                             this, &BackendClientFactory::invalidateAll);
    }
    m_settings = settings;
    if (m_settings) {
        QObject::connect(m_settings, &Settings::updated,
                         this, &BackendClientFactory::invalidateAll);
    }
    invalidateAll();
}

QSharedPointer<BackendClient> BackendClientFactory::createClient(
        const QString &url,
        const QString &token) const
{
    // BackendClient takes ownership of nothing here — we wrap it in a
    // QSharedPointer so callers share session state safely. The clients
    // are not Qt objects we want auto-parented, so no parent.
    return QSharedPointer<BackendClient>::create(url, token);
}

QSharedPointer<BackendClient> BackendClientFactory::get(const QString &nodeId)
{
    if (!m_settings) {
        // Defensive: no Settings means we can't resolve IDs. Return a
        // backend client pointing at the empty URL, callers will surface
        // the error to the user. Better than crashing.
        return createClient(QString(), QString());
    }
    const QString resolvedId = nodeId.isEmpty() ? u"local"_s : nodeId;
    auto it = m_byId.find(resolvedId);
    if (it != m_byId.end()) {
        return it.value();
    }
    BackendNode node = m_settings->getBackendNode(resolvedId);
    if (node.url.isEmpty()) {
        // Unknown id → fall back to local for safety. We DO NOT cache this
        // under the bogus id so future Settings updates still route to the
        // correct backend.
        return getLocal();
    }
    auto client = createClient(node.url, node.token);
    m_byId.insert(resolvedId, client);
    return client;
}

QSharedPointer<BackendClient> BackendClientFactory::getLocal()
{
    return get(u"local"_s);
}

QSharedPointer<BackendClient> BackendClientFactory::getForUrl(const QString &baseUrl)
{
    if (!m_settings) {
        return createClient(baseUrl, QString());
    }
    QString cleaned = baseUrl;
    while (cleaned.endsWith(u'/')) cleaned.chop(1);
    if (cleaned.isEmpty()) {
        return getLocal();
    }
    for (const auto &node : m_settings->getBackendNodes()) {
        QString candidate = node.url;
        while (candidate.endsWith(u'/')) candidate.chop(1);
        if (candidate == cleaned) {
            return get(node.id);
        }
    }
    // No registered backend matches → fall back to local rather than
    // creating an anonymous client. The caller (SKAVA open flow) will
    // then trigger the download-via-local fallback.
    return getLocal();
}

void BackendClientFactory::invalidate(const QString &nodeId)
{
    m_byId.remove(nodeId);
}

void BackendClientFactory::invalidateAll()
{
    m_byId.clear();
}
