#include "BackendLauncher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/**
 * Locate the backend directory relative to the application executable.
 *
 * Looks for a directory containing app/main.py in the following locations:
 *   macOS bundle  : <app>.app/Contents/Resources/backend
 *   Linux install : <exe_dir>/backend
 *   Dev tree      : <exe_dir>/../backend
 */
QString findBackendDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
#if defined(Q_OS_MACOS)
        QDir::cleanPath(appDir + u"/../Resources/backend"_s),
#endif
        QDir::cleanPath(appDir + u"/backend"_s),
        QDir::cleanPath(appDir + u"/../backend"_s),
        // Development: project root/backend
        QDir::cleanPath(appDir + u"/../../backend"_s),
        QDir::cleanPath(appDir + u"/../../../backend"_s),
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c + u"/app/main.py"_s))
            return c;
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// BackendLauncher
// ---------------------------------------------------------------------------

BackendLauncher::BackendLauncher(QObject *parent) : QObject(parent)
{
    m_pollTimer.setSingleShot(false);
    m_pollTimer.setInterval(kPollIntervalMs);

    QObject::connect(&m_pollTimer, &QTimer::timeout, this, &BackendLauncher::onPollTick);

    QObject::connect(&m_initialHealthWatcher,
                     &QFutureWatcher<BackendHealthResult>::finished,
                     this, &BackendLauncher::onInitialHealthDone);

    QObject::connect(&m_pollHealthWatcher,
                     &QFutureWatcher<BackendHealthResult>::finished,
                     this, &BackendLauncher::onPollHealthDone);
}

BackendLauncher::~BackendLauncher()
{
    cancel();
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->terminate();
        m_process->waitForFinished(3000);
    }
}

void BackendLauncher::startAsync(const QString &backendUrl,
                                 const QString &pythonExe,
                                 bool noAutostart)
{
    m_backendUrl = backendUrl;
    m_pythonExe  = pythonExe;
    m_noAutostart = noAutostart;
    m_cancelled = false;
    m_ready = false;
    m_processFinishedUnexpectedly = false;
    m_pollCheckInFlight = false;
    m_capturedLogs.clear();

    emit messageChanged(u"Checking backend…"_s);
    doInitialHealthCheck();
}

void BackendLauncher::cancel()
{
    m_cancelled = true;
    m_pollTimer.stop();
}

QString BackendLauncher::capturedLogs() const
{
    return m_capturedLogs;
}

bool BackendLauncher::isReady() const
{
    return m_ready;
}

// ---------------------------------------------------------------------------
// Step 1 — initial health check
// ---------------------------------------------------------------------------

void BackendLauncher::doInitialHealthCheck()
{
    const QString url = m_backendUrl;
    m_initialHealthWatcher.setFuture(QtConcurrent::run([url]() -> BackendHealthResult {
        BackendClient bc(url);
        return bc.health();
    }));
}

void BackendLauncher::onInitialHealthDone()
{
    if (m_cancelled)
        return;

    const auto result = m_initialHealthWatcher.result();
    if (result.ok) {
        m_ready = true;
        emit messageChanged(u"Connected to existing backend"_s);
        emit alreadyRunning();
        return;
    }

    if (m_noAutostart) {
        emit failed(
                u"The backend is not running and auto-start is disabled.\n\n"
                "Start the backend manually, then click Retry."_s);
        return;
    }

    launchProcess();
}

// ---------------------------------------------------------------------------
// Step 2 — launch backend process
// ---------------------------------------------------------------------------

void BackendLauncher::launchProcess()
{
    if (m_pythonExe.isEmpty()) {
        emit failed(
                u"Python interpreter not found.\n\n"
                "Install Python 3 and restart the application."_s);
        return;
    }

    const QString backendDir = findBackendDir();
    if (backendDir.isEmpty()) {
        emit failed(
                u"Backend directory not found.\n\n"
                "Expected a backend/ directory containing app/main.py "
                "alongside the application."_s);
        return;
    }

    appendLog(u"[launcher] Backend dir : %1"_s.arg(backendDir));
    appendLog(u"[launcher] Python      : %1"_s.arg(m_pythonExe));

    // Extract host and port from the backend URL.
    const QUrl parsedUrl(m_backendUrl);
    const int    port = parsedUrl.port(8000);
    const QString host = parsedUrl.host().isEmpty() ? u"127.0.0.1"_s : parsedUrl.host();

    const QStringList args = {
        u"-m"_s, u"uvicorn"_s, u"app.main:app"_s,
        u"--host"_s, host,
        u"--port"_s, QString::number(port),
        u"--no-access-log"_s,
    };
    appendLog(u"[launcher] Command     : %1 %2"_s.arg(m_pythonExe, args.join(u" "_s)));

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(backendDir);

    // Capture stdout + stderr as they arrive.
    QObject::connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        const QString text = QString::fromLocal8Bit(m_process->readAllStandardOutput()).trimmed();
        if (!text.isEmpty())
            appendLog(text);
    });
    QObject::connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        const QString text = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
        if (!text.isEmpty())
            appendLog(text);
    });
    QObject::connect(m_process,
                     &QProcess::errorOccurred,
                     this, &BackendLauncher::onProcessError);
    QObject::connect(m_process,
                     &QProcess::finished,
                     this, &BackendLauncher::onProcessFinished);

    emit messageChanged(u"Starting backend…"_s);
    m_process->start(m_pythonExe, args);

    if (!m_process->waitForStarted(3000)) {
        const QString err = m_process->errorString();
        appendLog(u"[launcher] Failed to start process: %1"_s.arg(err));
        emit failed(u"Could not start the backend process.\n\n%1"_s.arg(err));
        return;
    }

    emit messageChanged(u"Waiting for backend…"_s);
    m_elapsed.start();
    startPolling();
}

// ---------------------------------------------------------------------------
// Step 3 — poll until backend responds
// ---------------------------------------------------------------------------

void BackendLauncher::startPolling()
{
    m_pollCheckInFlight = false;
    m_pollTimer.start();
}

void BackendLauncher::onPollTick()
{
    if (m_cancelled || m_pollCheckInFlight)
        return;

    // Timeout guard.
    if (m_elapsed.elapsed() > kMaxWaitMs) {
        m_pollTimer.stop();
        if (m_process) {
            appendLog(QString::fromLocal8Bit(m_process->readAllStandardOutput()));
            appendLog(QString::fromLocal8Bit(m_process->readAllStandardError()));
        }
        emit failed(u"The backend did not respond within %1 seconds.\n\n"
                    "Use 'Show Logs' for details."_s.arg(kMaxWaitMs / 1000));
        return;
    }

    // Guard: process crashed before becoming reachable.
    if (m_processFinishedUnexpectedly) {
        m_pollTimer.stop();
        emit failed(
                u"The backend process exited unexpectedly before becoming reachable.\n\n"
                "Use 'Show Logs' for details."_s);
        return;
    }

    m_pollCheckInFlight = true;
    const QString url = m_backendUrl;
    m_pollHealthWatcher.setFuture(QtConcurrent::run([url]() -> BackendHealthResult {
        BackendClient bc(url);
        return bc.health();
    }));
}

void BackendLauncher::onPollHealthDone()
{
    m_pollCheckInFlight = false;
    if (m_cancelled)
        return;

    if (m_pollHealthWatcher.result().ok) {
        m_pollTimer.stop();
        m_ready = true;
        emit messageChanged(u"Backend is running"_s);
        emit backendStarted();
    }
    // Otherwise keep polling — next tick will try again.
}

// ---------------------------------------------------------------------------
// QProcess event handlers
// ---------------------------------------------------------------------------

void BackendLauncher::onProcessError(QProcess::ProcessError error)
{
    if (m_cancelled)
        return;
    appendLog(u"[launcher] Process error %1: %2"_s
              .arg(static_cast<int>(error))
              .arg(m_process ? m_process->errorString() : QString()));
}

void BackendLauncher::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    const QString statusStr =
            (status == QProcess::NormalExit) ? u"normal exit"_s : u"crash"_s;
    appendLog(u"[launcher] Process finished: code=%1 status=%2"_s
              .arg(exitCode)
              .arg(statusStr));

    // Flush any remaining buffered output.
    if (m_process) {
        const QString out = QString::fromLocal8Bit(m_process->readAllStandardOutput()).trimmed();
        const QString err = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
        if (!out.isEmpty()) appendLog(out);
        if (!err.isEmpty()) appendLog(err);
    }

    // Only treat as unexpected if the poll loop is still running
    // (i.e. backend never became reachable).
    if (!m_ready && m_pollTimer.isActive()) {
        m_processFinishedUnexpectedly = true;
        // onPollTick will pick this up on the next tick.
    }
}

void BackendLauncher::appendLog(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;
    m_capturedLogs += trimmed + u'\n';
}
