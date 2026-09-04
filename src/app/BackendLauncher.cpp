#include "BackendLauncher.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
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
 *
 * @param[out] searchedOut  All candidate paths that were checked (populated even
 *                          on failure, so callers can include them in error messages).
 */
QString findBackendDir(QStringList &searchedOut)
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
    searchedOut = candidates;
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c + u"/app/main.py"_s))
            return c;
    }
    return {};
}

/**
 * Minimum interpreter the backend is supported on.
 *
 * Not a stylistic floor: several dependencies (numpy 2, numba, the FastAPI
 * stack) have no wheels below this, so an older interpreter fails deep inside
 * pip with an unreadable error. It has to be checked up front and reported
 * plainly, because the default interpreter on a Mac is 3.9 and on many Linux
 * distributions 3.9 or 3.10 — a user who has "python3" does not necessarily
 * have a usable one.
 */
constexpr int kMinPythonMajor = 3;
constexpr int kMinPythonMinor = 11;

/** Runs @p exe with @p args and returns trimmed stdout, or {} on any failure. */
QString runCapture(const QString &exe, const QStringList &args, int timeoutMs = 8000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return {};
    }
    return QString::fromLocal8Bit(p.readAll()).trimmed();
}

/** ``(major, minor)`` of @p exe, or ``{0, 0}`` if it cannot be determined. */
QPair<int, int> pythonVersion(const QString &exe)
{
    const QString out = runCapture(
            exe, { u"-c"_s, u"import sys;print('%d.%d'%sys.version_info[:2])"_s });
    const QStringList parts = out.split(u'.');
    if (parts.size() < 2)
        return { 0, 0 };
    bool okMaj = false, okMin = false;
    const int maj = parts.at(0).toInt(&okMaj);
    const int min = parts.at(1).toInt(&okMin);
    return (okMaj && okMin) ? QPair<int, int>{ maj, min } : QPair<int, int>{ 0, 0 };
}

bool pythonVersionOk(const QPair<int, int> &v)
{
    return v.first > kMinPythonMajor
            || (v.first == kMinPythonMajor && v.second >= kMinPythonMinor);
}

/** True when @p exe can actually import what the backend needs to boot. */
bool hasBackendDeps(const QString &exe)
{
    QProcess p;
    p.start(exe, { u"-c"_s, u"import fastapi, uvicorn, astropy, numpy"_s });
    if (!p.waitForFinished(20000)) {
        p.kill();
        p.waitForFinished(1000);
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

/**
 * The user-data virtual environment — the one the setup_backend scripts create
 * and the one the resolution chain looks for.
 *
 * Per-user writable storage, never inside the application. On macOS the backend
 * ships in ``VisIVO.app/Contents/Resources``; writing a venv there would
 * invalidate the code signature, be refused outright under /Applications, and
 * be lost on every upgrade. An AppImage is read-only outright.
 *
 * Defined once because two things depend on it agreeing: candidate 3b of the
 * resolution chain, and the first-run bootstrap that creates it. Keep in sync
 * with scripts/setup_backend.{sh,command,bat}, which produce the same paths.
 */
QString userDataVenvDir()
{
#if defined(Q_OS_MACOS)
    return QDir::cleanPath(QDir::homePath()
                           + u"/Library/Application Support/VisIVO/.venv"_s);
#elif defined(Q_OS_WIN)
    const QString appData = qEnvironmentVariable("APPDATA");
    return appData.isEmpty() ? QString()
                             : QDir::cleanPath(appData + u"/VisIVO/.venv"_s);
#else
    const QString xdgDataHome =
            qEnvironmentVariable("XDG_DATA_HOME", QDir::homePath() + u"/.local/share"_s);
    return QDir::cleanPath(xdgDataHome + u"/VisIVO/.venv"_s);
#endif
}

/** Interpreter inside a venv directory, per platform. */
QString venvPythonPath(const QString &venvDir)
{
    if (venvDir.isEmpty())
        return {};
#if defined(Q_OS_WIN)
    return QDir::cleanPath(venvDir + u"/Scripts/python.exe"_s);
#else
    return QDir::cleanPath(venvDir + u"/bin/python"_s);
#endif
}

/**
 * Wait for @p proc while keeping the event loop running.
 *
 * The bootstrap runs on the GUI thread, so a bare waitForFinished() would block
 * every repaint and every queued signal — including the progress messages the
 * dialog is supposed to show. @p timeoutMs < 0 waits indefinitely.
 */
bool waitForProcessPumpingEvents(QProcess &proc, int timeoutMs)
{
    if (proc.state() == QProcess::NotRunning)
        return proc.exitStatus() == QProcess::NormalExit;

    QEventLoop loop;
    QObject::connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&proc, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QTimer timeout;
    bool timedOut = false;
    if (timeoutMs >= 0) {
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            loop.quit();
        });
        timeout.start(timeoutMs);
    }
    loop.exec();
    if (timedOut) {
        proc.kill();
        proc.waitForFinished(2000);
        return false;
    }
    return proc.state() == QProcess::NotRunning;
}

/** Returns true if @p path points to an existing, executable file. */
bool isValidPythonExe(const QString &path)
{
    if (path.isEmpty())
        return false;
    const QFileInfo fi(path);
    return fi.exists() && fi.isFile() && fi.isExecutable();
}

/**
 * Resolve the Python interpreter to use for launching the backend.
 *
 * Priority:
 *  1. VISIVO_BACKEND_PYTHON env var — explicit override, highest priority.
 *  2. $VIRTUAL_ENV/bin/python       — active venv in the calling shell.
 *  3. <backendDir>/../.venv/bin/python — project .venv in dev tree.
 *  4. Bundled Python alongside the app executable.
 *  5. @p settingsPython (detected at app startup via PATH).
 *  6. Hard "python3" / "python" — last resort, may not have uvicorn.
 *
 * @param settingsPython  Value from Settings::getPythonLocation() (may be empty).
 * @param backendDir      Resolved backend directory (used for .venv search).
 * @param logOut          Accumulates human-readable log lines for the caller.
 */
struct PythonResolution
{
    QString exe;     // resolved interpreter path (or name for PATH lookup)
    QString source;  // human-readable description for logging
};

PythonResolution resolvePythonExe(const QString &settingsPython,
                                   const QString &backendDir,
                                   QStringList   &logOut)
{
    // ── 1. Explicit override ─────────────────────────────────────────────────
    const QString envOverride = qEnvironmentVariable("VISIVO_BACKEND_PYTHON");
    if (!envOverride.isEmpty()) {
        if (isValidPythonExe(envOverride)) {
            logOut << u"[launcher] Python (VISIVO_BACKEND_PYTHON): %1"_s.arg(envOverride);
            return { envOverride, u"VISIVO_BACKEND_PYTHON env var"_s };
        }
        logOut << u"[launcher] VISIVO_BACKEND_PYTHON=%1 — not executable, skipping"_s
                          .arg(envOverride);
    }

    // ── 2. Active virtual environment ────────────────────────────────────────
    const QString virtualEnv = qEnvironmentVariable("VIRTUAL_ENV");
    if (!virtualEnv.isEmpty()) {
#if defined(Q_OS_WIN)
        const QString venvPython = QDir::cleanPath(virtualEnv + u"/Scripts/python.exe"_s);
#else
        const QString venvPython = QDir::cleanPath(virtualEnv + u"/bin/python"_s);
#endif
        if (isValidPythonExe(venvPython)) {
            logOut << u"[launcher] Python (active venv, $VIRTUAL_ENV=%1): %2"_s
                              .arg(virtualEnv, venvPython);
            return { venvPython, u"active virtual environment ($VIRTUAL_ENV)"_s };
        }
        logOut << u"[launcher] VIRTUAL_ENV set but %1 not found, skipping"_s.arg(venvPython);
    }

    // ── 3. Project .venv in dev tree ─────────────────────────────────────────
    //  Look for <backendDir>/../.venv/bin/python, which is the standard layout
    //  when the developer runs:  python -m venv .venv && source .venv/bin/activate
    //  from the repository root next to backend/.
    if (!backendDir.isEmpty()) {
#if defined(Q_OS_WIN)
        const QString devVenvPython =
                QDir::cleanPath(backendDir + u"/../.venv/Scripts/python.exe"_s);
#else
        const QString devVenvPython =
                QDir::cleanPath(backendDir + u"/../.venv/bin/python"_s);
#endif
        if (isValidPythonExe(devVenvPython)) {
            logOut << u"[launcher] Python (project .venv): %1"_s.arg(devVenvPython);
            return { devVenvPython, u"project .venv (dev tree)"_s };
        }
    }

    // ── 3b. User-data venv ───────────────────────────────────────────────────
    //  Created either by scripts/setup_backend.{sh,command,bat} or by the
    //  first-run bootstrap below. Lives in writable user storage so a signed
    //  bundle / read-only AppImage stays untouched. The path comes from
    //  userDataVenvDir() so the chain and the bootstrap cannot disagree.
    {
        const QString userVenvPython = venvPythonPath(userDataVenvDir());
        if (!userVenvPython.isEmpty() && isValidPythonExe(userVenvPython)) {
            logOut << u"[launcher] Python (user-data venv): %1"_s.arg(userVenvPython);
            return { userVenvPython, u"user-data venv"_s };
        }
    }

    // ── 4. Bundled Python alongside the executable ───────────────────────────
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList bundledCandidates = {
#if defined(Q_OS_MACOS)
        // <app>.app/Contents/MacOS/python3  (thin wrapper bundled by pyinstaller etc.)
        QDir::cleanPath(appDir + u"/python3"_s),
#endif
        QDir::cleanPath(appDir + u"/python3"_s),
        QDir::cleanPath(appDir + u"/python"_s),
    };
    for (const QString &c : bundledCandidates) {
        if (isValidPythonExe(c)) {
            logOut << u"[launcher] Python (bundled runtime): %1"_s.arg(c);
            return { c, u"bundled backend runtime"_s };
        }
    }

    // ── 5. Settings / PATH-detected python ───────────────────────────────────
    if (isValidPythonExe(settingsPython)) {
        logOut << u"[launcher] Python (system, via Settings): %1"_s.arg(settingsPython);
        logOut << u"[launcher] NOTE: using system Python — ensure uvicorn and dependencies "
                  u"are installed in this environment."_s;
        return { settingsPython, u"system Python (application settings)"_s };
    }

    // ── 6. Hard fallback — let the OS resolve from PATH ──────────────────────
    logOut << u"[launcher] WARNING: no validated Python found, falling back to 'python3' on PATH."_s;
    logOut << u"[launcher] If the backend fails to start, install uvicorn: "
              u"pip install uvicorn fastapi"_s;
    return { u"python3"_s, u"system PATH fallback (python3)"_s };
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
                u"Start the backend manually, then click Retry."_s);
        return;
    }

    launchProcess();
}

// ---------------------------------------------------------------------------
// First-run bootstrap — create the backend's virtual environment
// ---------------------------------------------------------------------------

/**
 * Create ``<backendDir>/../.venv`` and install the backend's requirements.
 *
 * Runs once, on a machine that has a supported Python but has never run the
 * backend. It is deliberately synchronous with progress reported through
 * messageChanged(): the install is hundreds of megabytes and several minutes,
 * and a silent wait of that length reads as a hang.
 *
 * @returns true on success. On failure it has already emitted failed() with
 *          what pip actually said, so the caller just returns.
 */
bool BackendLauncher::bootstrapEnvironment(const QString &backendDir, const QString &pythonExe)
{
    const QString venvDir = userDataVenvDir();
    const QString requirements = backendDir + u"/requirements.txt"_s;
    appendLog(u"[launcher] Managed venv : %1"_s.arg(venvDir));

    if (venvDir.isEmpty()) {
        emit failed(u"Cannot determine where to create the backend environment "
                    u"(no writable application-data directory)."_s);
        return false;
    }
    if (!QFileInfo::exists(requirements)) {
        emit failed(u"Cannot set up the backend: %1 is missing."_s.arg(requirements));
        return false;
    }

    // ── 1. the virtual environment ───────────────────────────────────────────
    if (!QFileInfo::exists(venvDir)) {
        emit messageChanged(u"Preparing the backend environment…"_s);
        appendLog(u"[launcher] Creating venv at %1"_s.arg(venvDir));
        QProcess venv;
        venv.setProcessChannelMode(QProcess::MergedChannels);
        venv.start(pythonExe, { u"-m"_s, u"venv"_s, venvDir });
        if (!waitForProcessPumpingEvents(venv, 120000) || venv.exitCode() != 0) {
            const QString out = QString::fromLocal8Bit(venv.readAll()).trimmed();
            appendLog(out);
            emit failed(u"Could not create the backend environment at %1.\n\n%2\n\n"
                        u"On Debian and Ubuntu this usually means the python3-venv "
                        u"package is not installed."_s.arg(venvDir, out));
            return false;
        }
    }

    const QString venvPython = venvPythonPath(venvDir);
    if (!isValidPythonExe(venvPython)) {
        emit failed(u"The backend environment was created but has no interpreter "
                    u"at %1."_s.arg(venvPython));
        return false;
    }

    // ── 2. the dependencies ──────────────────────────────────────────────────
    emit messageChanged(u"Installing backend dependencies — this takes a few "
                        u"minutes the first time…"_s);
    appendLog(u"[launcher] pip install -r %1"_s.arg(requirements));

    QProcess pip;
    pip.setProcessChannelMode(QProcess::MergedChannels);
    pip.setWorkingDirectory(backendDir);
    QObject::connect(&pip, &QProcess::readyReadStandardOutput, this, [this, &pip]() {
        const QString chunk = QString::fromLocal8Bit(pip.readAllStandardOutput());
        for (const QString &line : chunk.split(u'\n', Qt::SkipEmptyParts)) {
            const QString t = line.trimmed();
            appendLog(t);
            // Surface only the package-level lines; pip's per-file chatter would
            // flicker the dialog without telling anyone anything.
            if (t.startsWith(u"Collecting "_s) || t.startsWith(u"Installing collected"_s))
                emit messageChanged(t);
        }
    });
    pip.start(venvPython, { u"-m"_s, u"pip"_s, u"install"_s, u"--upgrade"_s,
                            u"-r"_s, requirements });
    // No timeout: a cold install of this dependency set over a slow link
    // legitimately runs for many minutes, and killing it half-way leaves an
    // environment that looks present and is not. Events are pumped while we
    // wait — this runs on the GUI thread (launchProcess() is reached from a
    // QFutureWatcher::finished handler), so a plain waitForFinished() would
    // freeze the startup dialog for the whole install and none of the progress
    // messages below would ever be painted.
    waitForProcessPumpingEvents(pip, -1);

    if (pip.exitStatus() != QProcess::NormalExit || pip.exitCode() != 0) {
        emit failed(u"Installing the backend dependencies failed.\n\n"
                    u"The Diagnostics log has pip's output. A common cause is no "
                    u"network access; you can also prepare the environment by hand:\n\n"
                    u"  %1 -m venv %2\n  %3 -m pip install -r %4"_s
                            .arg(pythonExe, venvDir, venvPython, requirements));
        return false;
    }

    emit messageChanged(u"Backend environment ready."_s);
    appendLog(u"[launcher] Bootstrap complete"_s);
    return true;
}

// ---------------------------------------------------------------------------
// Step 2 — launch backend process
// ---------------------------------------------------------------------------

void BackendLauncher::launchProcess()
{
    // ── Locate backend directory first (needed for .venv search) ─────────────
    QStringList searchedPaths;
    const QString backendDir = findBackendDir(searchedPaths);
    if (backendDir.isEmpty()) {
        emit failed(
                u"Backend directory not found.\n\n"
                u"Expected a backend/ directory containing app/main.py "
                u"alongside the application.\n\n"
                u"Searched:\n  • "_s
                + searchedPaths.join(u"\n  • "_s));
        return;
    }
    appendLog(u"[launcher] Backend dir : %1"_s.arg(backendDir));

    // ── Resolve Python interpreter (priority chain) ───────────────────────────
    QStringList resolutionLog;
    // Not a structured binding: the bootstrap below may replace the interpreter
    // with the one it just created, so these have to be assignable.
    const PythonResolution resolved = resolvePythonExe(m_pythonExe, backendDir, resolutionLog);
    QString pythonExe = resolved.exe;
    QString pythonSource = resolved.source;
    for (const QString &line : std::as_const(resolutionLog))
        appendLog(line);

    if (pythonExe.isEmpty()) {
        emit failed(
                u"Python interpreter not found.\n\n"
                u"Install Python 3 and restart the application."_s);
        return;
    }
    appendLog(u"[launcher] Using Python: %1  (%2)"_s.arg(pythonExe, pythonSource));

    // ── Is this interpreter usable, and does it have the backend's deps? ─────
    //
    // Both questions have to be asked here rather than left to uvicorn. A Mac's
    // /usr/bin/python3 is 3.9 and will be found by the PATH fallback; letting it
    // through produces a pip resolution failure pages long, or an ImportError
    // from inside the server, neither of which tells the user what to install.
    const auto version = pythonVersion(pythonExe);
    const QString versionText = version.first
            ? u"%1.%2"_s.arg(version.first).arg(version.second)
            : u"unknown"_s;
    appendLog(u"[launcher] Python ver : %1"_s.arg(versionText));

    if (!pythonVersionOk(version)) {
        emit failed(
                u"VisIVO needs Python %1.%2 or newer, and the interpreter it found "
                u"is %3.\n\n%4\n\nInstall a newer Python, then either put it first "
                u"on your PATH or point VisIVO at it with the "
                u"VISIVO_BACKEND_PYTHON environment variable."_s
                        .arg(kMinPythonMajor).arg(kMinPythonMinor)
                        .arg(versionText, pythonExe));
        return;
    }

    if (!hasBackendDeps(pythonExe)) {
        appendLog(u"[launcher] Backend dependencies missing — bootstrapping"_s);
        if (!bootstrapEnvironment(backendDir, pythonExe))
            return;   // bootstrapEnvironment() has already emitted failed()
        // The bootstrap creates backendDir/../.venv, which is candidate 3 of the
        // resolution chain, so re-resolving now picks it up.
        QStringList reLog;
        const PythonResolution reResolved = resolvePythonExe(m_pythonExe, backendDir, reLog);
        const QString venvExe = reResolved.exe;
        const QString venvSource = reResolved.source;
        for (const QString &line : std::as_const(reLog))
            appendLog(line);
        if (venvExe.isEmpty() || !hasBackendDeps(venvExe)) {
            emit failed(u"The backend environment was created but its dependencies "
                        u"are still missing. See the log for what pip reported."_s);
            return;
        }
        pythonExe = venvExe;
        pythonSource = venvSource;
        appendLog(u"[launcher] Using Python: %1  (%2)"_s.arg(pythonExe, pythonSource));
    }

    // ── Extract host and port from the backend URL ────────────────────────────
    const QUrl parsedUrl(m_backendUrl);
    const int    port = parsedUrl.port(8000);
    const QString host = parsedUrl.host().isEmpty() ? u"127.0.0.1"_s : parsedUrl.host();

    const QStringList args = {
        u"-m"_s, u"uvicorn"_s, u"app.main:app"_s,
        u"--host"_s, host,
        u"--port"_s, QString::number(port),
        u"--no-access-log"_s,
    };
    appendLog(u"[launcher] Command     : %1 %2"_s.arg(pythonExe, args.join(u" "_s)));

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
    m_process->start(pythonExe, args);

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
                    u"Use 'Show Logs' for details."_s.arg(kMaxWaitMs / 1000));
        return;
    }

    // Guard: process crashed before becoming reachable.
    if (m_processFinishedUnexpectedly) {
        m_pollTimer.stop();
        emit failed(
                u"The backend process exited unexpectedly before becoming reachable.\n\n"
                u"Use 'Show Logs' for details."_s);
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
