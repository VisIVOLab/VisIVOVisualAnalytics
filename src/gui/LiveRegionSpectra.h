#ifndef LiveRegionSpectra_h
#define LiveRegionSpectra_h

// LiveRegionSpectra.h
// The logic behind the cube window's live region spectrum (issue #521): the
// spectrum of the selected region follows it while it is moved or resized,
// from the 3-D view's samples, and the backend's full-resolution spectrum
// replaces it once the region is let go.
//
//   * Throttle: a drag produces a mouse move per few milliseconds; the preview
//     is recomputed at most once per interval, with a trailing run so the last
//     position is always drawn.
//   * LatestWins: a full-resolution request answers for the region as it was
//     when asked. Each request takes a ticket; an answer whose ticket is no
//     longer the latest is dropped.
//   * SpectrumCache: full-resolution spectra by region geometry, so selecting
//     a region again (or putting it back with Undo) asks the backend nothing.
//   * overlayPlan(): which regions are laid over the selected one, in which
//     colour and under which name.
//
// Pure QtCore and header-only: unit-tested in tests/test_live_region_spectra.cpp.

#include "RegionModel.h"

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace visivo::liveregion {

/// At most one run per interval, the last request of a burst always served.
/// The caller owns the clock and the timer: request() says what to do now.
class Throttle
{
public:
    enum class Action {
        Run,    ///< do the work now
        Arm,    ///< start a single-shot timer of delayMs, then call fired()
        Wait    ///< a timer is already armed: it will serve this request
    };
    struct Decision
    {
        Action action{ Action::Run };
        int delayMs{ 0 };
    };

    explicit Throttle(int intervalMs = 150) : m_interval(intervalMs) {}

    int intervalMs() const { return m_interval; }
    bool armed() const { return m_armed; }

    /// A change happened at @p nowMs (any monotonic clock, in milliseconds).
    Decision request(std::int64_t nowMs)
    {
        if (m_armed)
            return { Action::Wait, 0 };
        if (!m_everRan || nowMs - m_lastRun >= m_interval) {
            m_everRan = true;
            m_lastRun = nowMs;
            return { Action::Run, 0 };
        }
        m_armed = true;
        return { Action::Arm, int(m_interval - (nowMs - m_lastRun)) };
    }

    /// The armed timer fired at @p nowMs: the caller runs the work now.
    void fired(std::int64_t nowMs)
    {
        m_armed = false;
        m_everRan = true;
        m_lastRun = nowMs;
    }

    /// The work was done outside the throttle (a release, a selection): the
    /// clock restarts and a waiting timer is no longer owed.
    void ranNow(std::int64_t nowMs)
    {
        m_armed = false;
        m_everRan = true;
        m_lastRun = nowMs;
    }

private:
    int m_interval;
    bool m_armed{ false };
    bool m_everRan{ false };
    std::int64_t m_lastRun{ 0 };
};

/// Tickets for requests in flight: only the latest one is current.
class LatestWins
{
public:
    /// A new request; every earlier ticket is stale from now on.
    std::uint64_t next() { return ++m_latest; }
    bool isCurrent(std::uint64_t ticket) const { return ticket != 0 && ticket == m_latest; }
    /// Nothing in flight answers any more (the live spectrum was switched off).
    void invalidate() { ++m_latest; }

private:
    std::uint64_t m_latest{ 0 };
};

/// What identifies a region's pixels: its geometry in the file's pixel frame.
/// The name, colour and visibility do not change its spectrum.
inline QString geometrySignature(const regions::Region &r)
{
    QJsonObject canonical = regions::toCanonical(r, 0.0);
    canonical.remove(QStringLiteral("props"));   // name, colour, file properties
    canonical.remove(QStringLiteral("source_line"));
    return QString::fromLatin1(QJsonDocument(canonical).toJson(QJsonDocument::Compact));
}

/// A bounded memory of spectra by key, oldest dropped first.
class SpectrumCache
{
public:
    explicit SpectrumCache(std::size_t capacity = 32) : m_capacity(capacity) {}

    const QVector<double> *find(const QString &key) const
    {
        const auto it = m_values.find(key);
        return it == m_values.end() ? nullptr : &it->second;
    }
    void insert(const QString &key, const QVector<double> &values)
    {
        if (m_capacity == 0)
            return;
        if (m_values.find(key) == m_values.end()) {
            m_order.push_back(key);
            while (m_order.size() > m_capacity) {
                m_values.erase(m_order.front());
                m_order.pop_front();
            }
        }
        m_values[key] = values;
    }
    void clear()
    {
        m_values.clear();
        m_order.clear();
    }
    std::size_t size() const { return m_values.size(); }

private:
    struct Hash
    {
        std::size_t operator()(const QString &s) const { return std::size_t(qHash(s)); }
    };
    std::size_t m_capacity;
    std::unordered_map<QString, QVector<double>, Hash> m_values;
    std::deque<QString> m_order;
};

/// The colour a region is drawn in when it has none of its own
/// (RegionListOverlay's default).
inline QString defaultRegionColour() { return QStringLiteral("#33e6ff"); }

inline QString regionColour(const regions::Region &r)
{
    return r.color.isEmpty() ? defaultRegionColour() : r.color;
}

struct OverlayEntry
{
    int regionId{ -1 };
    QString name;
    QString colour;   ///< "#rrggbb"
};

/// Colours for curves that must be told apart: regions made with the default
/// colour all share one, so overlaid curves would be identical.
inline const QStringList &overlayPalette()
{
    static const QStringList p{ QStringLiteral("#4e79a7"), QStringLiteral("#f28e2b"),
                                QStringLiteral("#e15759"), QStringLiteral("#59a14f"),
                                QStringLiteral("#edc948"), QStringLiteral("#b07aa1"),
                                QStringLiteral("#ff9da7"), QStringLiteral("#9c755f"),
                                QStringLiteral("#76b7b2"), QStringLiteral("#bab0ac") };
    return p;
}

/// The curve colour of every region that is drawn in the overlay (the
/// selected one and the entries of overlayPlan). A colour held by one region
/// alone stays; regions that share a colour each get one from the palette,
/// starting at their id and skipping what is already taken (so it mostly
/// stays when other regions come and go).
inline std::unordered_map<int, QString> overlayColours(const std::vector<regions::Region> &list,
                                                       int selectedId)
{
    std::vector<const regions::Region *> shown;
    for (const regions::Region &r : list)
        if (r.id == selectedId || (r.visible && regions::hasArea(r.shape)))
            shown.push_back(&r);
    QHash<QString, int> uses;
    for (const regions::Region *r : shown)
        ++uses[regionColour(*r).toLower()];
    std::unordered_map<int, QString> out;
    QSet<QString> taken;
    for (const regions::Region *r : shown) {
        const QString c = regionColour(*r).toLower();
        if (uses.value(c) == 1) {
            out[r->id] = c;
            taken.insert(c);
        }
    }
    const QStringList &pal = overlayPalette();
    const int n = int(pal.size());
    for (const regions::Region *r : shown) {
        if (out.count(r->id))
            continue;
        const int start = ((r->id % n) + n) % n;
        int pick = start;
        for (int k = 0; k < n; ++k) {
            const int i = (start + k) % n;
            if (!taken.contains(pal[i])) {
                pick = i;
                break;
            }
        }
        out[r->id] = pal[pick];   // more regions than colours: reuse
        taken.insert(pal[pick]);
    }
    return out;
}

/// The regions laid over the selected one: every visible region with pixels in
/// it (not a line, a vector or a text), in list order, the selected one apart
/// (it is the main curve). A region without a name is called by its id. The
/// colour is the one the curve and its legend swatch are drawn in
/// (overlayColours).
inline std::vector<OverlayEntry> overlayPlan(const std::vector<regions::Region> &list,
                                             int selectedId)
{
    const auto colours = overlayColours(list, selectedId);
    std::vector<OverlayEntry> out;
    for (const regions::Region &r : list) {
        if (r.id == selectedId || !r.visible || !regions::hasArea(r.shape))
            continue;
        out.push_back({ r.id,
                        r.name.isEmpty() ? QStringLiteral("Region %1").arg(r.id) : r.name,
                        colours.at(r.id) });
    }
    return out;
}

/// The regions "Keep spectra of all regions" makes a product of: every one
/// with pixels in it, hidden or not (a hidden region is still a region of the
/// file), in list order.
inline std::vector<int> keepAllTargets(const std::vector<regions::Region> &list)
{
    std::vector<int> out;
    for (const regions::Region &r : list)
        if (regions::hasArea(r.shape))
            out.push_back(r.id);
    return out;
}

/// Which region spectra were turned into products already, so that exporting
/// twice from the live pane, or Keep all after it, does not make a second
/// product of the same spectrum. One is a region's geometry on one cube.
class KeptLedger
{
public:
    static QString key(const QString &signature, const QString &cubeKey)
    {
        return signature + QLatin1Char('|') + cubeKey;
    }
    void record(int regionId, const QString &key, const QString &productId)
    {
        m_kept[regionId] = { key, productId };
    }
    /// The product of @p regionId made for @p key that still exists (@p exists
    /// says which products do); empty when there is none.
    template <class Exists>
    QString find(int regionId, const QString &key, Exists exists) const
    {
        const auto it = m_kept.find(regionId);
        if (it == m_kept.end() || it->second.first != key || !exists(it->second.second))
            return QString();
        return it->second.second;
    }

private:
    std::unordered_map<int, std::pair<QString, QString>> m_kept;
};

}   // namespace visivo::liveregion

#endif
