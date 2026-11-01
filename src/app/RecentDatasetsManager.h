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

#include <optional>

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

    // Where the dataset was found and served from, so a reopen asks the
    // same catalogue (even after the settings moved to another one) and the
    // panel can say which node served it: the SKAVA service and protocol
    // ("skava" / "vo"), the replica's access URL, the serving node's code.
    // Empty on entries from before this was recorded.
    QString skavaUrl;
    QString skavaProtocol;
    // True when the entry recorded its VO endpoints: then empty ones mean
    // "derived from the catalogue URL", not "unknown" (older entries).
    bool    skavaEndpointsRecorded{ false };
    QString skavaTapUrl;        ///< VO protocol: the TAP endpoint used (empty = derived)
    QString skavaDatalinkUrl;   ///< VO protocol: the DataLink endpoint used (empty = derived)
    QString accessUrl;
    QString servedBy;

    // The backend the window that opened it ran on: empty for the local
    // backend, else the name of a remote one (Settings::sessionBackend()). A
    // path on a cluster means nothing to the local backend, so a reopen from a
    // window on another backend offers to go there instead. Not used for SKAVA
    // entries, which are resolved through discovery again on reopen.
    QString backend;
    // True when ``backend`` is a cluster profile the desktop can connect to
    // (the window was opened with --connect-cluster); false for --backend.
    bool    backendIsCluster{ false };
    // False for entries saved before the backend was recorded: their backend
    // is unknown (not "local"), and they reopen in any window as they did.
    bool    backendKnown{ false };

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
    // The same, with every field of the entry (``lastOpened`` is set here).
    void record(RecentDataset entry);

    // The entry with the given id (on @p backend when given; an entry of
    // unknown backend matches any), or an entry with an empty id.
    RecentDataset find(const QString &id, const std::optional<QString> &backend = std::nullopt) const;

    // Removes the entry with the given id (no-op if not found).
    // With @p backend, only the entry on that backend (and one of unknown backend).
    void remove(const QString &id, const std::optional<QString> &backend = std::nullopt);

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
