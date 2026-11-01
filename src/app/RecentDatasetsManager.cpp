// RecentDatasetsManager.cpp

#include "RecentDatasetsManager.h"

#include "Settings.h"

#include <QDateTime>
#include <QLockFile>
#include <QSettings>

#include <memory>

using namespace Qt::StringLiterals;

// QSettings group + key names used for persistence.
static constexpr char kGroup[]     = "RecentDatasets";
static constexpr char kCount[]     = "count";
static constexpr char kId[]        = "id";
static constexpr char kName[]      = "displayName";
static constexpr char kType[]      = "type";
static constexpr char kSource[]    = "source";
static constexpr char kObsId[]     = "obsId";
static constexpr char kSkavaUrl[]  = "skavaUrl";
static constexpr char kProtocol[]  = "skavaProtocol";
static constexpr char kEndpoints[] = "skavaEndpointsRecorded";
static constexpr char kTapUrl[]    = "skavaTapUrl";
static constexpr char kDatalink[]  = "skavaDatalinkUrl";
static constexpr char kAccessUrl[] = "accessUrl";
static constexpr char kServedBy[]  = "servedBy";
static constexpr char kOpened[]    = "lastOpened";
static constexpr char kBackend[]   = "backend";
static constexpr char kCluster[]   = "backendIsCluster";
static constexpr char kKnown[]     = "backendKnown";

namespace {
/// Same dataset: the same id on the same backend. An entry of unknown backend
/// (saved before backends were recorded) is taken as the same.
bool sameEntry(const RecentDataset &e, const QString &id, const std::optional<QString> &backend)
{
    return e.id == id && (!backend || !e.backendKnown || e.backend == *backend);
}

/// Serialises the read-modify-write of the list across windows (processes)
/// sharing Settings.ini.
std::unique_ptr<QLockFile> lockList(const QString &settingsPath)
{
    auto lock = std::make_unique<QLockFile>(settingsPath + u".recents.lock"_s);
    lock->setStaleLockTime(10000);
    lock->tryLock(2000);   // a stuck lock costs at most two seconds, never the write
    return lock;
}
}  // namespace

// ---------------------------------------------------------------------------

RecentDatasetsManager::RecentDatasetsManager(const QString &workDir, QObject *parent)
    : QObject(parent)
    , m_settingsPath(workDir + u"/Settings.ini"_s)
{
    load();
}

QList<RecentDataset> RecentDatasetsManager::datasets() const
{
    return m_datasets;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

void RecentDatasetsManager::record(const QString &id,
                                   const QString &displayName,
                                   const QString &type,
                                   const QString &source,
                                   const QString &obsId)
{
    RecentDataset entry;
    entry.id          = id;
    entry.displayName = displayName;
    entry.type        = type;
    entry.source      = source;
    entry.obsId       = obsId;
    record(std::move(entry));
}

RecentDataset RecentDatasetsManager::find(const QString &id,
                                          const std::optional<QString> &backend) const
{
    for (const auto &e : m_datasets)
        if (sameEntry(e, id, backend))
            return e;
    return {};
}

void RecentDatasetsManager::record(RecentDataset entry)
{
    if (entry.id.isEmpty()) {
        return;
    }
    // Several windows (a local one, one per cluster) share Settings.ini: start
    // from what is on disk, so this write does not drop another window's entries.
    const auto lock = lockList(m_settingsPath);
    load();

    // Where it was opened, unless the caller said so (SKAVA entries reopen
    // through discovery, wherever they are clicked).
    if (entry.source != u"skava"_s && !entry.backendKnown) {
        if (entry.backend.isEmpty()) {
            if (const Settings::SessionBackend *session = Settings::sessionBackend()) {
                entry.backend = session->name;
                entry.backendIsCluster = session->ownsClusterJob;
            }
        }
        entry.backendKnown = true;
    }

    // Remove the existing entry for the same dataset on the same backend: the
    // same path on another backend is another dataset.
    m_datasets.removeIf([&entry](const RecentDataset &e) {
        return sameEntry(e, entry.id,
                         entry.backendKnown ? std::optional<QString>(entry.backend) : std::nullopt);
    });

    // Prepend the new (or updated) entry.
    if (entry.displayName.isEmpty())
        entry.displayName = entry.id;
    if (entry.source.isEmpty())
        entry.source = u"remote"_s;
    entry.lastOpened  = QDateTime::currentDateTime();
    m_datasets.prepend(entry);

    // Trim to max capacity.
    while (m_datasets.size() > kMaxEntries) {
        m_datasets.removeLast();
    }

    save();
    emit changed();
}

void RecentDatasetsManager::remove(const QString &id, const std::optional<QString> &backend)
{
    const auto lock = lockList(m_settingsPath);
    load();
    const int before = m_datasets.size();
    m_datasets.removeIf([&](const RecentDataset &e) { return sameEntry(e, id, backend); });
    if (m_datasets.size() != before) {
        save();
        emit changed();
    }
}

void RecentDatasetsManager::clear()
{
    const auto lock = lockList(m_settingsPath);
    load();
    if (m_datasets.isEmpty()) {
        return;
    }
    m_datasets.clear();
    save();
    emit changed();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void RecentDatasetsManager::load()
{
    QSettings s(m_settingsPath, QSettings::IniFormat);
    s.beginGroup(QLatin1StringView(kGroup));
    const int count = s.value(QLatin1StringView(kCount), 0).toInt();
    m_datasets.clear();
    m_datasets.reserve(count);
    for (int i = 0; i < count; ++i) {
        s.beginGroup(QString::number(i));
        RecentDataset e;
        e.id          = s.value(QLatin1StringView(kId)).toString();
        e.displayName = s.value(QLatin1StringView(kName)).toString();
        e.type        = s.value(QLatin1StringView(kType)).toString();
        e.source      = s.value(QLatin1StringView(kSource), u"remote"_s).toString();
        // ``obsId`` is new in the SKAVA-routing patch; old entries don't
        // have it and load as an empty string, which makes ``reopenDataset``
        // fall back to path-based reopen unchanged.
        e.obsId       = s.value(QLatin1StringView(kObsId)).toString();
        e.skavaUrl    = s.value(QLatin1StringView(kSkavaUrl)).toString();
        e.skavaProtocol = s.value(QLatin1StringView(kProtocol)).toString();
        e.skavaEndpointsRecorded = s.value(QLatin1StringView(kEndpoints), false).toBool();
        e.skavaTapUrl = s.value(QLatin1StringView(kTapUrl)).toString();
        e.skavaDatalinkUrl = s.value(QLatin1StringView(kDatalink)).toString();
        e.accessUrl   = s.value(QLatin1StringView(kAccessUrl)).toString();
        e.servedBy    = s.value(QLatin1StringView(kServedBy)).toString();
        e.lastOpened  = s.value(QLatin1StringView(kOpened)).toDateTime();
        e.backend     = s.value(QLatin1StringView(kBackend)).toString();
        e.backendIsCluster = s.value(QLatin1StringView(kCluster), false).toBool();
        e.backendKnown = s.value(QLatin1StringView(kKnown), false).toBool();
        s.endGroup();
        if (!e.id.isEmpty()) {
            m_datasets.append(std::move(e));
        }
    }
    s.endGroup();
}

void RecentDatasetsManager::save() const
{
    QSettings s(m_settingsPath, QSettings::IniFormat);
    s.beginGroup(QLatin1StringView(kGroup));
    // Remove stale sub-groups from a previous run that might have had more entries.
    s.remove(QString()); // clears the entire group before rewriting
    s.setValue(QLatin1StringView(kCount), m_datasets.size());
    for (int i = 0; i < m_datasets.size(); ++i) {
        const auto &e = m_datasets[i];
        s.beginGroup(QString::number(i));
        s.setValue(QLatin1StringView(kId),     e.id);
        s.setValue(QLatin1StringView(kName),   e.displayName);
        s.setValue(QLatin1StringView(kType),   e.type);
        s.setValue(QLatin1StringView(kSource), e.source);
        if (!e.obsId.isEmpty()) {
            s.setValue(QLatin1StringView(kObsId), e.obsId);
        }
        if (!e.skavaUrl.isEmpty())      s.setValue(QLatin1StringView(kSkavaUrl), e.skavaUrl);
        if (!e.skavaProtocol.isEmpty()) s.setValue(QLatin1StringView(kProtocol), e.skavaProtocol);
        if (e.skavaEndpointsRecorded)   s.setValue(QLatin1StringView(kEndpoints), true);
        if (!e.skavaTapUrl.isEmpty())   s.setValue(QLatin1StringView(kTapUrl), e.skavaTapUrl);
        if (!e.skavaDatalinkUrl.isEmpty()) s.setValue(QLatin1StringView(kDatalink), e.skavaDatalinkUrl);
        if (!e.accessUrl.isEmpty())     s.setValue(QLatin1StringView(kAccessUrl), e.accessUrl);
        if (!e.servedBy.isEmpty())      s.setValue(QLatin1StringView(kServedBy), e.servedBy);
        s.setValue(QLatin1StringView(kOpened), e.lastOpened);
        if (e.backendKnown) {
            s.setValue(QLatin1StringView(kKnown), true);
            if (!e.backend.isEmpty()) {
                s.setValue(QLatin1StringView(kBackend), e.backend);
                s.setValue(QLatin1StringView(kCluster), e.backendIsCluster);
            }
        }
        s.endGroup();
    }
    s.endGroup();
    s.sync();
}
