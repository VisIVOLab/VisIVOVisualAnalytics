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
    QString repoDir;         ///< the clone on the frontend, relative to its home
    QString partition;
    QString gres;            ///< e.g. "gpu:1"
    int cpus = 16;
    QString mem;             ///< e.g. "96G"
    QString time;            ///< SLURM time limit; empty: the launcher's default
    QString sif;             ///< the backend image on the cluster
    QString dataRoot;        ///< the data the backend may open
    QString workspace;       ///< the backend's workspace (exports, pyramids, logs)
    QString nodeCode;        ///< SRC node code, for SKAVA; optional
    QStringList extraArgs;   ///< further launcher options, passed unchanged

    bool isValid() const { return !name.trimmed().isEmpty() && !frontend.trimmed().isEmpty(); }
    /// The SSH destination: user@frontend, or the frontend alone.
    QString sshTarget() const;
    /// The options for scripts/launch_backend_slurm.sh, in order.
    QStringList launcherArgs() const;
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
