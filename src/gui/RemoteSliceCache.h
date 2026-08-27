#ifndef REMOTE_SLICE_CACHE_H
#define REMOTE_SLICE_CACHE_H

#include <QHash>
#include <QList>
#include <QString>

/// LRU cache of decoded remote (server-side) 2-D slices, keyed by an opaque
/// "datasetId|axis|index" string the owner computes.
///
/// Extracted verbatim from vtkWindowCube's inline cache members (E-04 god-object
/// decomposition, step 1): a disjoint, GUI-independent slice of state, so it can
/// be unit-tested headless. Behaviour is unchanged — same insert / evict / touch
/// order as the original `cacheRemoteSliceResult` / `touchRemoteSliceCacheKey`
/// code. Templated on the slice value type so the cache carries no VTK / backend
/// dependency (the test instantiates it with a fake slice); the window uses
/// `RemoteSliceCache<RemoteCubeSliceResult>`.
///
/// @tparam Slice a value type with a `bool valid` member and an `imageData`
///         member that is contextually convertible to bool (present = usable).
template <class Slice>
class RemoteSliceCache
{
public:
    explicit RemoteSliceCache(int capacity = 8) : m_capacity(capacity) {}

    /// Insert a valid result under @p key, refreshing its LRU recency and
    /// evicting the least-recently-used entries once capacity is exceeded.
    /// Invalid / image-less results are ignored (matches the original guard).
    void insert(const QString &key, const Slice &result)
    {
        if (!result.valid || !result.imageData) {
            return;
        }
        m_cache.insert(key, result);
        this->touch(key);
        while (m_lru.size() > m_capacity) {
            const QString evicted = m_lru.takeFirst();
            m_cache.remove(evicted);
        }
    }

    /// Look up a slice; on a hit refresh its recency and return a pointer to the
    /// cached value (valid until the next mutation of this cache). nullptr on miss.
    const Slice *find(const QString &key)
    {
        const auto it = m_cache.constFind(key);
        if (it == m_cache.cend()) {
            return nullptr;
        }
        this->touch(key);
        return &it.value();
    }

    /// Membership test without affecting recency (used by the prefetch guard).
    bool contains(const QString &key) const { return m_cache.contains(key); }

    /// Mark @p key most-recently-used. A no-op for a key not in the cache — only
    /// cached keys may enter the recency list, otherwise a ghost key would inflate
    /// m_lru and make a later insert() over-evict real entries.
    void touch(const QString &key)
    {
        if (!m_cache.contains(key)) {
            return;
        }
        m_lru.removeAll(key);
        m_lru.append(key);
    }

    void clear()
    {
        m_cache.clear();
        m_lru.clear();
    }

    int size() const { return m_cache.size(); }
    int capacity() const { return m_capacity; }

private:
    QHash<QString, Slice> m_cache;
    QList<QString> m_lru;       // front = least-recently-used, back = most-recent
    int m_capacity;
};

#endif  // REMOTE_SLICE_CACHE_H
