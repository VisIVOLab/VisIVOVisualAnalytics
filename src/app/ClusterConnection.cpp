#include "ClusterConnection.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QUrl>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace {
// The options every ssh here runs with: never prompt (BatchMode), and notice a
// dead connection within about a minute.
QStringList sshOptions()
{
    return { u"-o"_s, u"BatchMode=yes"_s, u"-o"_s, u"ConnectTimeout=20"_s,
             u"-o"_s, u"ServerAliveInterval=15"_s, u"-o"_s, u"ServerAliveCountMax=4"_s };
}

QString profileKey(const QString &name)
{
    return QString::fromLatin1(
            QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}
}  // namespace

// ── Pure helpers ──────────────────────────────────────────────────────────────

ClusterConnection::LaunchInfo ClusterConnection::parseLauncherOutput(const QString &output)
{
    LaunchInfo info;
    const auto first = [&output](const QString &pattern) {
        const auto m = QRegularExpression(pattern).match(output);
        return m.hasMatch() ? m.captured(1) : QString();
    };
    info.jobId = first(u"Job ID:\\s+([0-9]+)"_s);
    info.node = first(u"Compute node:\\s+(\\S+)"_s);
    info.port = first(u"\\bPort:\\s+([0-9]+)"_s).toInt();
    info.token = first(u"Token:\\s+(\\S+)"_s);
    return info;
}

QString ClusterConnection::remoteCommandLine(const QStringList &args)
{
    QStringList quoted;
    for (QString a : args)
        quoted << u"'"_s + a.replace(u"'"_s, u"'\\''"_s) + u"'"_s;
    return quoted.join(u' ');
}

int ClusterConnection::chooseLocalPort(int start, int avoid, const std::function<bool(int)> &isFree)
{
    for (int port = start; port < start + 64 && port < 65536; ++port)
        if (port != avoid && isFree(port))
            return port;
    return 0;
}

bool ClusterConnection::portIsFree(int port)
{
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, static_cast<quint16>(port));
}

QString ClusterConnection::redacted(const QString &line)
{
    static const QRegularExpression token(u"(Token:\\s+)\\S+"_s);
    static const QRegularExpression header(u"(X-Visivo-Token:\\s*)[^'\"\\s]+"_s);
    QString out = line;
    out.replace(token, u"\\1(received, not shown)"_s);
    out.replace(header, u"\\1***"_s);
    return out;
}

QString ClusterConnection::cancelCommand(const QString &jobId, const QString &jobName)
{
    // By the job's unique name when known: that also cancels a job whose id
    // never arrived, and succeeds when the job has already ended (scancel by id
    // fails then). The id serves records that carry no name.
    static const QRegularExpression digits(u"^[0-9]+$"_s);
    if (!jobName.isEmpty())
        return u"scancel -u \"$USER\" --name="_s + remoteCommandLine({ jobName });
    if (digits.match(jobId).hasMatch())
        return u"scancel "_s + jobId;
    return {};
}

QString ClusterConnection::profileProblem(const ClusterProfile &profile)
{
    static const QRegularExpression bad(u"^-|\\s"_s);
    if (bad.match(profile.frontend.trimmed()).hasMatch())
        return u"The frontend \"%1\" is not a host name."_s.arg(profile.frontend);
    if (bad.match(profile.user.trimmed()).hasMatch())
        return u"The user \"%1\" is not a login name."_s.arg(profile.user);
    return {};
}

// ── The connection ────────────────────────────────────────────────────────────

ClusterConnection::ClusterConnection(const ClusterProfile &profile, int avoidLocalPort,
                                     QObject *parent)
    : QObject(parent), m_profile(profile), m_avoidPort(avoidLocalPort)
{
    m_healthTimer.setInterval(1000);
    QObject::connect(&m_healthTimer, &QTimer::timeout, this, &ClusterConnection::pollHealth);
}

ClusterConnection::~ClusterConnection()
{
    stop();
}

QString ClusterConnection::jobRecordPath() const
{
    // One record per job, so a leftover job whose cancel failed keeps its
    // record while the next window records its own.
    return QDir::home().absoluteFilePath(
            u".visivo_clusters/%1-%2.job"_s.arg(profileKey(m_profile.name), m_jobName));
}

void ClusterConnection::start()
{
    if (const QString problem = profileProblem(m_profile); !problem.isEmpty()) {
        fail(problem);
        return;
    }
    m_ssh = QStandardPaths::findExecutable(u"ssh"_s);
    if (m_ssh.isEmpty()) {
        fail(u"No ssh client found. Install OpenSSH (on Windows: Settings ▸ Apps ▸ Optional "
             u"features ▸ OpenSSH Client)."_s);
        return;
    }
    // One window per cluster profile: the window owns the job it submitted.
    m_lock = std::make_unique<QLockFile>(
            QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                    .absoluteFilePath(u"visivo-cluster-%1.lock"_s.arg(profileKey(m_profile.name))));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(100)) {
        m_lock.reset();
        fail(u"\"%1\" is already connected in another VisIVO window."_s.arg(m_profile.name));
        return;
    }
    // A job recorded by an earlier window that did not get to cancel it.
    cancelRecordedJob();

    // ssh starts in the home directory, so a home-relative path needs no "~/"
    // (which the quoting would keep literal).
    QString repo = m_profile.repoDir.trimmed();
    if (repo.startsWith(u"~/"_s))
        repo = repo.mid(2);
    if (repo.isEmpty() || repo == u"~"_s)
        repo = u"VisIVOVisualAnalytics"_s;
    m_jobName = u"visivo-%1-%2"_s.arg(profileKey(m_profile.name).left(8))
                        .arg(QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex().left(8)));
    // Recorded before anything is submitted: a job whose id never arrives (a
    // Cancel or a crash right after sbatch) is still cancelled, by its name.
    writeJobRecord();
    QStringList remote{ u"bash"_s, repo + u"/scripts/launch_backend_slurm.sh"_s };
    remote << m_profile.launcherArgs() << u"--job-name"_s << m_jobName;
    emit message(u"Submitting the backend job on %1…"_s.arg(m_profile.sshTarget()));

    m_launcher = new QProcess(this);
    m_launcher->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(m_launcher, &QProcess::readyRead, this, &ClusterConnection::onLauncherOutput);
    QObject::connect(m_launcher, &QProcess::finished, this, &ClusterConnection::onLauncherFinished);
    QObject::connect(m_launcher, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            fail(u"Could not start ssh (%1)."_s.arg(m_ssh));
    });
    m_launcher->start(m_ssh, sshOptions() << m_profile.sshTarget() << remoteCommandLine(remote));
}

void ClusterConnection::onLauncherOutput()
{
    const QByteArray chunk = m_launcher->readAll();
    m_launcherOutput += chunk;
    m_pendingLine += QString::fromUtf8(chunk);
    qsizetype nl;
    while ((nl = m_pendingLine.indexOf(u'\n')) >= 0) {
        const QString line = m_pendingLine.left(nl).trimmed();
        m_pendingLine.remove(0, nl + 1);
        if (!line.isEmpty())
            emit message(redacted(line));
    }
    // Recorded as soon as it is known: a job is never left without a record.
    if (m_info.jobId.isEmpty()) {
        const LaunchInfo partial = parseLauncherOutput(QString::fromUtf8(m_launcherOutput));
        if (!partial.jobId.isEmpty()) {
            m_info.jobId = partial.jobId;
            writeJobRecord();
        }
    }
}

void ClusterConnection::onLauncherFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_stopping)
        return;
    const LaunchInfo info = parseLauncherOutput(QString::fromUtf8(m_launcherOutput));
    if (!info.jobId.isEmpty())
        m_info.jobId = info.jobId;
    if (status != QProcess::NormalExit || exitCode != 0 || !info.complete()) {
        if (exitCode == 255)
            fail(u"ssh could not reach or log in to %1, or the connection dropped. It needs "
                 u"key-based access (ssh-copy-id), as nothing can be typed here."_s
                         .arg(m_profile.sshTarget()));
        else if (m_launcherOutput.contains("Unknown option: --job-name"))
            fail(u"The VisIVO clone on the frontend is older than this client: update it "
                 u"(git pull) and connect again."_s);
        else
            fail(info.complete() ? u"The launcher on the frontend failed (exit %1)."_s.arg(exitCode)
                                 : u"The launcher did not report the job, node, port and token "
                                   u"(exit %1); see the messages above."_s.arg(exitCode));
        return;
    }
    m_info = info;
    m_localPort = chooseLocalPort(18901, m_avoidPort, &ClusterConnection::portIsFree);
    if (m_localPort == 0) {
        fail(u"No free local port for the tunnel."_s);
        return;
    }
    m_url = u"http://127.0.0.1:%1"_s.arg(m_localPort);
    emit message(u"Job %1 runs on %2. Opening the tunnel on localhost:%3…"_s.arg(
            m_info.jobId, m_info.node).arg(m_localPort));
    openTunnel();
    m_healthTries = 0;
    m_healthTimer.start();
}

void ClusterConnection::openTunnel()
{
    if (m_stopping)
        return;
    m_tunnel = new QProcess(this);
    m_tunnel->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QStringList args = sshOptions();
    args << u"-N"_s << u"-o"_s << u"ExitOnForwardFailure=yes"_s << u"-L"_s
         << u"127.0.0.1:%1:%2:%3"_s.arg(m_localPort).arg(m_info.node).arg(m_info.port)
         << m_profile.sshTarget();
    // Reopened when it ends (a network change, the laptop sleeping), as the
    // shell helper's loop does.
    QObject::connect(m_tunnel, &QProcess::finished, this, [this]() {
        if (m_tunnel)
            m_tunnel->deleteLater();
        if (m_stopping || m_failed)
            return;
        if (m_connected) {
            emit tunnelLost();
        } else {
            // Not up yet: the port may have been taken between the probe and
            // ssh's bind (another window starting): try the next free one.
            const int port = chooseLocalPort(m_localPort + 1, m_avoidPort, &ClusterConnection::portIsFree);
            if (port > 0) {
                m_localPort = port;
                m_url = u"http://127.0.0.1:%1"_s.arg(m_localPort);
            }
        }
        QTimer::singleShot(3000, this, &ClusterConnection::openTunnel);
    });
    m_tunnel->start(m_ssh, args);
}

void ClusterConnection::pollHealth()
{
    if (m_stopping || m_failed)
        return;
    if (++m_healthTries > 60) {
        m_healthTimer.stop();
        fail(u"The backend did not answer through the tunnel within a minute."_s);
        return;
    }
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(m_url + u"/v1/health"_s));
    req.setRawHeader("X-Visivo-Token", m_info.token.toUtf8());
    req.setTransferTimeout(3000);
    QNetworkReply *reply = m_nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_connected || m_stopping || m_failed)
            return;
        if (reply->error() == QNetworkReply::NoError) {
            m_healthTimer.stop();
            m_connected = true;
            emit message(u"Connected to %1."_s.arg(m_profile.name));
            emit connected(m_url, m_info.token);
        }
    });
}

void ClusterConnection::fail(const QString &reason)
{
    if (m_failed)
        return;
    m_failed = true;
    m_healthTimer.stop();
    emit failed(reason);
}

void ClusterConnection::writeJobRecord()
{
    // One line: <job id or -> <ssh target> <job name>.
    QDir().mkpath(QFileInfo(jobRecordPath()).absolutePath());
    QFile f(jobRecordPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(u"%1 %2 %3\n"_s
                        .arg(m_info.jobId.isEmpty() ? u"-"_s : m_info.jobId, m_profile.sshTarget(),
                             m_jobName)
                        .toUtf8());
}

void ClusterConnection::cancelRecordedJob()
{
    // Every job of this profile left by a window that did not cancel it.
    const QDir dir(QDir::home().absoluteFilePath(u".visivo_clusters"_s));
    const QStringList records =
            dir.entryList({ profileKey(m_profile.name) + u"-*.job"_s }, QDir::Files);
    for (const QString &file : records) {
        const QString path = dir.absoluteFilePath(file);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QStringList parts = QString::fromUtf8(f.readAll()).split(u' ', Qt::SkipEmptyParts);
        f.close();
        const QString job = parts.value(0).trimmed();
        const QString target = parts.value(1).trimmed();
        const QString name = parts.value(2).trimmed();
        const QString command = cancelCommand(job, name);
        ClusterProfile recorded;
        recorded.frontend = target;
        if (command.isEmpty() || target.isEmpty() || !profileProblem(recorded).isEmpty()) {
            QFile::remove(path);   // unreadable: nothing it could cancel
            continue;
        }
        emit message(u"Cancelling the job left by an earlier window (%1)…"_s.arg(
                job == u"-"_s ? name : job));
        QProcess cancel;
        cancel.start(m_ssh, sshOptions() << target << command);
        // Kept when the cancel did not go through: the next window tries again.
        if (cancel.waitForFinished(20000) && cancel.exitStatus() == QProcess::NormalExit
            && cancel.exitCode() == 0)
            QFile::remove(path);
        else
            emit message(u"Could not cancel it now; it will be tried again next time."_s);
    }
}

void ClusterConnection::stop()
{
    if (m_stopping)
        return;
    m_stopping = true;
    m_healthTimer.stop();
    if (m_launcher && m_launcher->state() != QProcess::NotRunning) {
        m_launcher->kill();
        m_launcher->waitForFinished(2000);
    }
    if (m_tunnel && m_tunnel->state() != QProcess::NotRunning) {
        m_tunnel->terminate();
        if (!m_tunnel->waitForFinished(3000))
            m_tunnel->kill();
    }
    // By its unique name (also when the launcher was cut off before reporting
    // the id), or by id for a record without a name.
    const QString command = cancelCommand(m_info.jobId, m_jobName);
    if (!command.isEmpty() && !m_ssh.isEmpty()) {
        QProcess cancel;
        cancel.start(m_ssh, sshOptions() << m_profile.sshTarget() << command);
        if (cancel.waitForFinished(20000) && cancel.exitStatus() == QProcess::NormalExit
            && cancel.exitCode() == 0)
            QFile::remove(jobRecordPath());
        // Otherwise the record stays: the next window for this profile cancels it.
    }
    if (m_lock)
        m_lock->unlock();
}
