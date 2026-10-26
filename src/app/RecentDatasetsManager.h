#ifndef RecentDatasetsManager_h
#define RecentDatasetsManager_h

// ---------------------------------------------------------------------------
// RecentDatasetsManager
//
// Tracks the N most-recently-opened datasets across sessions.
// Persists to the application Settings INI file under the group
// [RecentDatasets] so it shares the same file without conflicts.
//
// Usage (from MainWindow):
//
//   this->recentMgr = new RecentDatasetsManager(settings->getWorkingPath(), this);
//   this->dataHub->setRecentDatasetsManager(this->recentMgr);
//
//   // After a successful open:
//   this->recentMgr->record("/data/cube.fits", "cube.fits", "cube");
//
// The manager deduplicates by id (the backend path string): if the same
// path is opened again it moves to the top of the list rather than adding
// a duplicate.
// ---------------------------------------------------------------------------

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

struct RecentDataset
{
    // Unique key AND the path/URI needed to reopen the dataset.
    // For backend-opened files this is the backend-visible path string.
    // For SKAVA-opened datasets this is ``skava:<obs_id>`` so two opens of
    // the same file via different routes don't collide in the recents list.
    QString id;

    // Short human-readable name shown in the Recent Datasets panel.
    QString displayName;

    // Dataset kind: "cube" | "image" | "catalogue" | "vbt" | "vbt-volume" | "hips" | "velocity"
    QString type;

    // Origin tag, used by the DataHub UI for a visual badge and by
    // ``MainWindow::reopenDataset`` to pick a re-routing path:
    //   * "remote"  — opened via the local backend with an absolute path
    //   * "skava"   — opened via the SKAVA discovery + visivo-backend routing.
    //                 ``obsId`` carries the discovery identifier so we can
    //                 re-query SKAVA on reopen rather than dead-linking
    //                 against a stale local path.
    QString source;

    // SKAVA obs_id (non-empty only when ``source == "skava"``).
    QString obsId;

    QDateTime lastOpened;
};

class RecentDatasetsManager : public QObject
{
    Q_OBJECT

public:
    // Maximum number of entries retained (oldest are dropped when exceeded).
    static constexpr int kMaxEntries = 15;

    // workDir — the same directory passed to Settings (contains Settings.ini).
    // Recent datasets are stored as a separate group inside Settings.ini.
    explicit RecentDatasetsManager(const QString &workDir, QObject *parent = nullptr);

    // Current list, most-recently-opened first.
    QList<RecentDataset> datasets() const;

    // Records a dataset open.
    // • If an entry with the same id already exists it is moved to position 0
    //   and its timestamp is updated.
    // • If the list exceeds kMaxEntries after insertion, the oldest entry is
    //   removed.
    // Emits changed() after every mutation.
    void record(const QString &id,
                const QString &displayName,
                const QString &type,
                const QString &source = QStringLiteral("remote"),
                const QString &obsId = QString());

    // Removes the entry with the given id (no-op if not found).
    void remove(const QString &id);

    // Removes all entries.
    void clear();

signals:
    // Emitted after any mutation (record, remove, clear).
    void changed();

private:
    void load();
    void save() const;

    QString m_settingsPath; // absolute path to Settings.ini
    QList<RecentDataset> m_datasets;
};

#endif // RecentDatasetsManager_h
