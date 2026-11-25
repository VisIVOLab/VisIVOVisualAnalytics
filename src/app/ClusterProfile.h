#ifndef ClusterProfile_h
#define ClusterProfile_h

#include <QList>
#include <QSettings>
#include <QString>
#include <QStringList>

/**
 * A cluster the desktop connects to (issue #79): where its SLURM frontend is,
 * how to submit the backend job there, and the name it is shown with.
 *
 * Saved in the client's settings under Clusters/profiles (one JSON array), and
 * read with a plain QSettings: a window started with `--connect-cluster NAME`
 * reads its profile before the full Settings object exists.
 */
struct ClusterProfile
{
    QString name;            ///< shown in the status bar ("PLEIADI"); also the key
    QString frontend;        ///< SSH host of the SLURM frontend
    QString user;            ///< SSH login; empty: the local user name
    bool shipLauncher = true;   ///< true: the client sends the launcher it carries; false: the clone's own is run
    QString repoDir;         ///< the clone on the frontend (relative to its home), used when shipLauncher is false; empty: VisIVOVisualAnalytics
    QString site;            ///< "leonardo", "pleiadi" or empty: a known site (the launcher's --site)
    QString partition;
    QString account;         ///< SLURM account (project); needed on Leonardo, MareNostrum 5
    QString qos;             ///< SLURM quality of service; optional
    QString gres;            ///< e.g. "gpu:1"
    int cpus = 16;
    QString mem;             ///< e.g. "96G"
    QString time;            ///< SLURM time limit; empty: the launcher's default
    QString sif;             ///< the backend image file on the cluster
    QString image;           ///< an image to pull (docker://...); empty: the one of this client's version
    int queueWaitMin = 0;    ///< minutes to wait for a node; 0: the launcher's default (5)
    QString dataRoot;        ///< the data the backend may open
    QString workspace;       ///< the backend's workspace (exports, pyramids, logs)
    QString nodeCode;        ///< SRC node code, for SKAVA; optional
    QStringList extraArgs;   ///< further launcher options, passed unchanged

    bool isValid() const { return !name.trimmed().isEmpty() && !frontend.trimmed().isEmpty(); }
    /// The SSH destination: user@frontend, or the frontend alone.
    QString sshTarget() const;
    /// True when the client sends the launcher it carries and runs it from
    /// standard input, so nothing is needed on the frontend. A profile saved
    /// before the client shipped the launcher loads as a clone (see
    /// ClusterProfiles::load), so it keeps running its clone.
    bool usesShippedLauncher() const { return shipLauncher; }
    /// The site, from the profile, else from a `--site` in the further options,
    /// else from the frontend's name (a Leonardo login node, or PLEIADI);
    /// empty for any other cluster, CINECA's others included.
    QString effectiveSite() const;
    /// The image to pull when the profile names none: this client's version of
    /// the published backend image, with the EGL tag when it asks for a GPU.
    /// Empty when @p clientVersion is not X.Y.Z (pass clientImageVersion(): it
    /// is empty in a build that is not a release), or the profile names its own
    /// image or file, or a clone supplies one (as before the client shipped the launcher).
    QString defaultImage(const QString &clientVersion) const;
    /// The version whose published image this client may start by itself:
    /// @p appVersion in a release build (@p releaseBuild: the VISIVO_RELEASE_BUILD
    /// CMake option, which only the release workflow sets), else empty. Images
    /// are published for release tags only, and a build from main carries the
    /// number of the last release, so it would ask for an image that may not exist.
    static QString imageVersion(const QString &appVersion, bool releaseBuild);
    /// imageVersion() for this build and the running application's version.
    static QString clientImageVersion();
    /// The options for scripts/launch_backend_slurm.sh, in order.
    /// @p clientVersion picks the image when the profile has none.
    QStringList launcherArgs(const QString &clientVersion = {}) const;
    /// What stops the launcher from being run, or empty: a missing project
    /// account on a site that needs one, no image to start.
    QString launcherProblem(const QString &clientVersion) const;
    /// Fills in what a known site defines (frontend, partition, GRES, QOS,
    /// resources, time, queue wait); the user's own entries (login, account,
    /// data and workspace) and the clone are left as they are.
    void applyPreset(const QString &site);
    /// The known sites, as ids ("leonardo", "pleiadi"), and how they are shown.
    static QStringList presetSites();
    static QString presetLabel(const QString &site);
    /// @p args as one line that QProcess::splitCommand reads back as they are
    /// (an argument with a space or a quote is quoted).
    static QString joinCommand(const QStringList &args);
};

namespace ClusterProfiles {

QList<ClusterProfile> load(QSettings &settings);
void save(QSettings &settings, const QList<ClusterProfile> &profiles);
/// The profile called @p name (case-sensitive), or an invalid one.
ClusterProfile find(QSettings &settings, const QString &name);
/// Adds @p profile, or replaces the one with its name.
void upsert(QSettings &settings, const ClusterProfile &profile);

}  // namespace ClusterProfiles

#endif
