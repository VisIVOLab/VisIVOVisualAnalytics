// RecentDatasetsManager.cpp

#include "RecentDatasetsManager.h"

#include <QDateTime>
#include <QSettings>

using namespace Qt::StringLiterals;

// QSettings group + key names used for persistence.
static constexpr char kGroup[]     = "RecentDatasets";
static constexpr char kCount[]     = "count";
static constexpr char kId[]        = "id";
static constexpr char kName[]      = "displayName";
static constexpr char kType[]      = "type";
static constexpr char kSource[]    = "source";
static constexpr char kOpened[]    = "lastOpened";

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
                                   const QString &source)
{
    if (id.isEmpty()) {
        return;
    }

    // Remove any existing entry with the same id.
    for (auto it = m_datasets.begin(); it != m_datasets.end(); ++it) {
        if (it->id == id) {
            m_datasets.erase(it);
            break;
        }
    }

    // Prepend the new (or updated) entry.
    RecentDataset entry;
    entry.id          = id;
    entry.displayName = displayName.isEmpty() ? id : displayName;
    entry.type        = type;
    entry.source      = source.isEmpty() ? u"remote"_s : source;
    entry.lastOpened  = QDateTime::currentDateTime();
    m_datasets.prepend(entry);

    // Trim to max capacity.
    while (m_datasets.size() > kMaxEntries) {
        m_datasets.removeLast();
    }

    save();
    emit changed();
}

void RecentDatasetsManager::remove(const QString &id)
{
    const int before = m_datasets.size();
    m_datasets.removeIf([&id](const RecentDataset &e) { return e.id == id; });
    if (m_datasets.size() != before) {
        save();
        emit changed();
    }
}

void RecentDatasetsManager::clear()
{
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
        e.lastOpened  = s.value(QLatin1StringView(kOpened)).toDateTime();
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
        s.setValue(QLatin1StringView(kOpened), e.lastOpened);
        s.endGroup();
    }
    s.endGroup();
    s.sync();
}
