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
 * backend job on the SLURM frontend over ssh (scripts/launch_backend_slurm.sh in
 * the profile's clone there), opens and keeps an SSH tunnel to the compute
 * node, and waits for the backend to answer through it. stop() closes the
 * tunnel and cancels the job.
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
    /// The first port from @p start for which @p isFree holds and that is not
    /// @p avoid (the desktop's own local backend); 0 if none in 64 tries.
    static int chooseLocalPort(int start, int avoid, const std::function<bool(int)> &isFree);
    /// True when nothing listens on 127.0.0.1:@p port.
    static bool portIsFree(int port);
    /// A launcher output line as shown to the user: the token is never shown.
    static QString redacted(const QString &line);
    /// The ssh command that cancels a job: by id when known, else by its name.
    static QString cancelCommand(const QString &jobId, const QString &jobName);
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
