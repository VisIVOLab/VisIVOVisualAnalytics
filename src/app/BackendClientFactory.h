#ifndef BackendClientFactory_h
#define BackendClientFactory_h

#include <QHash>
#include <QObject>
#include <QSharedPointer>
#include <QString>

class BackendClient;
class Settings;

/// Process-wide cache of ``BackendClient`` instances, one per registered
/// backend. Used by the distributed-backend architecture (see
/// ``docs/distributed-backend-design.md``).
///
/// Why a cache: each ``BackendClient`` accumulates a session id on the first
/// ``openDataset()`` call, and subsequent calls on the same backend MUST
/// reuse that session so the backend keeps the dataset metadata in scope.
/// Constructing a fresh ``BackendClient`` per request would drop the
/// session, force a re-open and break every viewer's data fetches.
///
/// Lookup is by stable backend ``id`` (e.g. ``"local"``, ``"src-it-inaf"``)
/// for direct hits, and by base URL for "open the dataset SKAVA pointed us
/// at" (where we only know the URL, not which registry entry it matches).
/// Cache invalidation is driven by ``Settings::updated`` so that edits to
/// the backend registry transparently re-key the cache.
class BackendClientFactory : public QObject
{
    Q_OBJECT
public:
    /// Returns the singleton attached to the application object.
    static BackendClientFactory *instance();

    /// Bind the factory to the application's Settings instance so it can
    /// look up backend URLs / tokens and react to registry edits. Must be
    /// called once during application startup; subsequent calls reset the
    /// cache and rebind.
    void setSettings(Settings *settings);

    /// Returns the ``BackendClient`` for the given backend ``id``. The
    /// instance is cached so callers can rely on its session id surviving
    /// across requests. Returns the ``local`` backend's client when ``id``
    /// is empty or unknown.
    QSharedPointer<BackendClient> get(const QString &nodeId);

    /// Convenience: the auto-managed local backend.
    QSharedPointer<BackendClient> getLocal();

    /// Look up by base URL (exact match). Useful when SKAVA gave us an
    /// access URL and we want to route through whichever registered backend
    /// owns that base. Returns the local backend's client when no match is
    /// found, so callers always have a usable client.
    QSharedPointer<BackendClient> getForUrl(const QString &baseUrl);

    /// Drop the cached client for ``nodeId`` (forces re-construction on the
    /// next ``get()``). Useful after a token rotation or backend change.
    void invalidate(const QString &nodeId);

    /// Drop all cached clients. Called automatically when Settings emit
    /// ``updated``.
    void invalidateAll();

private:
    explicit BackendClientFactory(QObject *parent = nullptr);
    QSharedPointer<BackendClient> createClient(const QString &url,
                                                  const QString &token) const;

    Settings *m_settings{ nullptr };
    QHash<QString, QSharedPointer<BackendClient>> m_byId;
};

#endif  // BackendClientFactory_h
