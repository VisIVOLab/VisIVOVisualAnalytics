#ifndef ClusterConnection_h
#define ClusterConnection_h

#include "ClusterProfile.h"

#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

class QNetworkAccessManager;

/**
 * The backend of one desktop window on a cluster (issue #79): submits the
 * backend job on the SLURM frontend over ssh, opens and keeps an SSH tunnel to
 * the compute node, and waits for the backend to answer through it. stop()
 * closes the tunnel and cancels the job.
 *
 * The launcher (scripts/launch_backend_slurm.sh) is the one the client carries
 * (a Qt resource), sent over the ssh connection on standard input: the
 * cluster needs no clone and the launcher always matches the client. A profile
 * that names a clone runs that clone's copy instead (developers).
 *
 * Plain OpenSSH, as on macOS, Linux and Windows 10+, with key-based access to
 * the frontend (BatchMode: nothing is ever typed). The window that owns the
 * connection owns the job: one window per profile at a time.
 */
class ClusterConnection : public QObject
{
    Q_OBJECT

public:
    /// What the launcher reported. Complete when every field is set.
    struct LaunchInfo
    {
        QString jobId;
        QString node;
        int port = 0;
        QString token;
        bool complete() const { return !jobId.isEmpty() && !node.isEmpty() && port > 0 && !token.isEmpty(); }
    };

    /// The launcher's report, read from its output (as visivo_slurm_tunnel.sh reads it).
    static LaunchInfo parseLauncherOutput(const QString &output);
    /// @p args as one command line for the remote POSIX shell: each quoted, so a
    /// value with spaces or shell characters arrives as one argument, and as data.
    static QString remoteCommandLine(const QStringList &args);
    /// The command run over ssh, as separate arguments: `bash -s -- <options>`
    /// (the launcher comes on standard input, see stdinPayload) when the profile
    /// names no clone, else `bash <clone>/scripts/launch_backend_slurm.sh <options>`.
    /// @p clientVersion picks the image when the profile has none.
    static QStringList remoteArgv(const ClusterProfile &profile, const QString &jobName,
                                  const QString &clientVersion);
    /// What goes to `bash -s` on the frontend: the launcher @p script saved to a
    /// temporary file and run from it (so that it has a file name to run again
    /// under a login shell, and no command in it can read the rest of the
    /// script from standard input), the file removed afterwards.
    static QByteArray stdinPayload(const QString &script);
    /// Why the launcher failed, for the user: ssh failures (255) are told
    /// apart from the launcher's own, and say how to renew an expired
    /// certificate on Leonardo or accept a new host key. @p output is what the
    /// launcher and ssh printed; @p reported: the launcher gave job, node, port and token.
    static QString launcherFailure(const ClusterProfile &profile, int exitCode, const QString &output,
                                   bool reported);
    /// The short status a launcher output line stands for ("Waiting in the
    /// queue: Priority", "Pulling the backend image: 120 MB fetched"), or empty
    /// when the line is not a progress report.
    static QString progressStatus(const QString &line);
    /// The first port from @p start for which @p isFree holds and that is not
    /// @p avoid (the desktop's own local backend); 0 if none in 64 tries.
    static int chooseLocalPort(int start, int avoid, const std::function<bool(int)> &isFree);
    /// True when nothing listens on 127.0.0.1:@p port.
    static bool portIsFree(int port);
    /// A launcher output line as shown to the user: the token is never shown.
    static QString redacted(const QString &line);
    /// The ssh command that cancels a job: by its name when known, else by id.
    /// @p submitting: sbatch may still be running on the frontend, so it
    /// cancels again for a while and fails if a job is left.
    static QString cancelCommand(const QString &jobId, const QString &jobName,
                                 bool submitting = false);
    /// Why @p profile cannot be used for ssh (a host or user that ssh would read
    /// as an option, or with spaces), or empty.
    static QString profileProblem(const ClusterProfile &profile);

    explicit ClusterConnection(const ClusterProfile &profile, int avoidLocalPort,
                               QObject *parent = nullptr);
    ~ClusterConnection() override;

    void start();
    /// Closes the tunnel and cancels the job (waits up to ~20 s for scancel).
    void stop();

    QString url() const { return m_url; }
    QString token() const { return m_info.token; }
    QString jobId() const { return m_info.jobId; }
    const ClusterProfile &profile() const { return m_profile; }

signals:
    void message(const QString &text);
    /// What the connection is doing now, in a few words (for a status line).
    void progress(const QString &text);
    void connected(const QString &url, const QString &token);
    void failed(const QString &reason);
    /// The tunnel dropped after connecting and is being reopened.
    void tunnelLost();

private:
    void onLauncherOutput();
    void onLauncherFinished(int exitCode, QProcess::ExitStatus status);
    void openTunnel();
    void pollHealth();
    void fail(const QString &reason);
    void writeJobRecord();
    void cancelRecordedJob();
    QString jobRecordPath() const;

    ClusterProfile m_profile;
    int m_avoidPort = 0;
    QString m_ssh;
    std::unique_ptr<QLockFile> m_lock;
    QPointer<QProcess> m_launcher;
    QPointer<QProcess> m_tunnel;
    QByteArray m_launcherOutput;
    QString m_pendingLine;
    LaunchInfo m_info;
    QString m_jobName;   ///< unique per connection: cancels a job whose id never arrived
    int m_localPort = 0;
    QString m_url;
    bool m_stopping = false;
    bool m_connected = false;
    bool m_failed = false;
    int m_healthTries = 0;
    QTimer m_healthTimer;
    QNetworkAccessManager *m_nam = nullptr;
};

#endif
