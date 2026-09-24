#include "BackendLauncher.h"

#include "BackendClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QUrl>
#include <QtConcurrentRun>

#include <optional>
#include <utility>

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

/**
 * Highest minor version the PATH search will look for by name.
 *
 * Only a search bound, not a support claim: interpreters are still accepted on
 * the >= kMinPythonMinor test, so an unlisted newer one found as plain
 * "python3" is used all the same. Raise it when a release exists and the
 * scientific stack has wheels for it — being first to a new minor usually means
 * numba or numpy have not caught up.
 */
constexpr int kMaxProbedPythonMinor = 14;

/// Defined below, next to the other process helpers.
bool waitForProcessPumpingEvents(QProcess &proc, int timeoutMs);

/**
 * Runs @p exe with @p args and returns trimmed stdout, or {} on any failure.
 *
 * Pumps the event loop while waiting. launchProcess() runs on the GUI thread,
 * from a QFutureWatcher::finished handler, and the resolution chain probes each
 * candidate in turn — so a plain waitForFinished() freezes repainting and Quit
 * for as long as the slowest interpreter takes to start. The default timeout is
 * short for the same reason: `python -c` answers in well under a second, and a
 * candidate that cannot manage that in three is not one to wait on.
 */
/**
 * Environment for ANY invocation of a Python interpreter we make.
 *
 * Its only job is to keep bytecode out of wherever the interpreter lives. On a
 * bundled runtime that directory is inside the signed .app, and the 61
 * ``__pycache__`` directories a bare `python -c 'import sys'` leaves in the
 * standard library are enough to break the code signature — the version probe
 * alone did it, which is why this is here and not only on the backend process.
 */
QProcessEnvironment pythonInvocationEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // A user's own setting wins — but only a REAL one: CPython treats an
    // exported-but-empty value as unset, so `contains()` alone would hand the
    // bundle back to the default behaviour.
    if (!env.value(u"PYTHONPYCACHEPREFIX"_s).isEmpty()
        || !env.value(u"PYTHONDONTWRITEBYTECODE"_s).isEmpty())
        return env;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString dir = cache.isEmpty() ? QString() : QDir::cleanPath(cache + u"/pycache"_s);
    // If the cache directory cannot be made, fall back to writing no bytecode at
    // all rather than to a prefix that does not exist: an unwritable prefix does
    // not send CPython back to the bundle, but it does mean silently failing
    // cache writes on every import.
    if (!dir.isEmpty() && QDir().mkpath(dir)) {
        env.insert(u"PYTHONPYCACHEPREFIX"_s, dir);
    } else {
        env.insert(u"PYTHONDONTWRITEBYTECODE"_s, u"1"_s);
    }
    return env;
}

QString runCapture(const QString &exe, const QStringList &args, int timeoutMs = 3000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setProcessEnvironment(pythonInvocationEnvironment());
    p.start(exe, args);
    if (!waitForProcessPumpingEvents(p, timeoutMs)) {
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

/**
 * The child's environment, with VISIVO_TOKEN pinned.
 *
 * Left to itself the backend mints a token at import time and writes it to
 * ~/.visivo_token. That is fine for an operator starting uvicorn by hand and
 * wrong for a backend we spawn: BackendClient caches the token when it is
 * constructed, which is BEFORE the child runs, so the file it read is the
 * previous run's. The app then talks to its own backend for a few seconds —
 * until the child rewrites the file — and every request after that comes back
 * 401 "Host requires authentication", with a status bar reading "offline" and
 * nothing to suggest the two ever agreed.
 *
 * Pinning the value removes the race rather than narrowing it. The order is
 * BackendClient::readTokenFile()'s own — VISIVO_TOKEN, then ~/.visivo_token —
 * so the launcher and the client cannot pick differently. Only when neither
 * exists is one generated, and it is written to the file first, so a client
 * that re-reads still finds the same string.
 */
QProcessEnvironment backendEnvironment()
{
    // Same bytecode redirection as every other python invocation: the backend
    // is the one that imports the most, so it writes the most.
    QProcessEnvironment env = pythonInvocationEnvironment();

    if (!env.value(u"VISIVO_TOKEN"_s).trimmed().isEmpty())
        return env;   // already pinned by the shell; the client reads it too

    const QString existing = BackendClient::readTokenFile().trimmed();
    if (!existing.isEmpty()) {
        env.insert(u"VISIVO_TOKEN"_s, existing);
        return env;
    }

    // 32 bytes from the system CSPRNG, base64url. QUuid would have been 122
    // bits of entropy in a 36-character string, which is fine in practice but
    // makes the security property something the reader has to work out; this
    // says it outright and matches what the backend generates for itself.
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(raw.data()),
                                         reinterpret_cast<quint32 *>(raw.data() + raw.size()));
    const QString token = QString::fromLatin1(
            raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    // Persist it BEFORE pinning it. A token the child accepts and no client can
    // discover is worse than no token at all: the backend would come up, every
    // request would 401, and nothing would say why. If the file cannot be
    // written, leave the environment alone and let the backend mint and print
    // its own — no better, but no worse than before this function existed.
    const QString path = QDir(QStandardPaths::writableLocation(
                                      QStandardPaths::HomeLocation))
                                 .absoluteFilePath(u".visivo_token"_s);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return env;
    // Tighten the mode while the file is still empty: under the usual 022 umask
    // it is created world-readable, and chmod-after-write leaves a window in
    // which the token is on disk and readable by any local user.
    if (!f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        f.close();
        QFile::remove(path);
        return env;
    }
    if (f.write(token.toUtf8() + '\n') < 0) {
        f.close();
        QFile::remove(path);
        return env;
    }
    f.close();
    env.insert(u"VISIVO_TOKEN"_s, token);
    return env;
}

/** True when @p exe can actually import what the backend needs to boot. */
bool hasBackendDeps(const QString &exe)
{
    QProcess p;
    p.setProcessEnvironment(pythonInvocationEnvironment());
    p.start(exe, { u"-c"_s, u"import fastapi, uvicorn, astropy, numpy"_s });
    // Pumped, like runCapture: importing astropy and numpy is seconds, not
    // milliseconds, and this also runs on the GUI thread.
    if (!waitForProcessPumpingEvents(p, 20000)) {
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
 *  3b. The user-data venv, created by setup_backend or the first-run bootstrap.
 *  4. Bundled Python alongside the app executable.
 *  5. @p settingsPython (detected at app startup via PATH).
 *  6. Version-named interpreters on PATH — python3.13 down to python3.11.
 *  7. Bare "python3" / "python" from PATH — last resort.
 *
 * A candidate is only accepted when it is BOTH executable AND new enough. That
 * matters more than it sounds: macOS ships 3.9 as /usr/bin/python3, so on a
 * stock Mac with Homebrew's python@3.11 installed, step 5 finds the 3.9 first.
 * Checking the version once, after picking, made the launcher give up with
 * "VisIVO needs Python 3.11" while a perfectly good 3.11 sat at
 * /opt/homebrew/bin/python3.11 — which is exactly how Homebrew installs it,
 * under a version-named binary that step 7 never looks for. Hence both the
 * per-candidate gate and step 6.
 *
 * @param settingsPython  Value from Settings::getPythonLocation() (may be empty).
 * @param backendDir      Resolved backend directory (used for .venv search).
 * @param logOut          Accumulates human-readable log lines for the caller.
 */
struct PythonResolution
{
    QString exe;      // resolved interpreter path (empty when none qualified)
    QString source;   // human-readable description for logging
    QStringList rejected;  // "path (3.9)" for each candidate that was too old
};

PythonResolution resolvePythonExe(const QString &settingsPython,
                                   const QString &backendDir,
                                   QStringList   &logOut)
{
    QStringList rejected;

    // Accept a candidate only if it exists, runs, and is new enough. Anything
    // that fails is logged and the search continues to the next candidate.
    const auto accept = [&](const QString &exe, const QString &what)
            -> std::optional<PythonResolution> {
        if (exe.isEmpty() || !isValidPythonExe(exe))
            return std::nullopt;
        const QPair<int, int> v = pythonVersion(exe);
        if (!pythonVersionOk(v)) {
            const QString ver = v.first ? u"%1.%2"_s.arg(v.first).arg(v.second)
                                        : u"no version"_s;
            logOut << u"[launcher] %1: %2 is %3 - too old, continuing"_s
                              .arg(what, exe, ver);
            rejected << u"%1 (%2)"_s.arg(exe, ver);
            return std::nullopt;
        }
        logOut << u"[launcher] Python (%1): %2  [%3.%4]"_s
                          .arg(what, exe).arg(v.first).arg(v.second);
        return PythonResolution{ exe, what, {} };
    };

    // ── 1. Explicit override ─────────────────────────────────────────────────
    const QString envOverride = qEnvironmentVariable("VISIVO_BACKEND_PYTHON");
    if (!envOverride.isEmpty()) {
        if (auto r = accept(envOverride, u"VISIVO_BACKEND_PYTHON env var"_s))
            return *r;
        if (!isValidPythonExe(envOverride))
            logOut << u"[launcher] VISIVO_BACKEND_PYTHON=%1 - not executable, skipping"_s
                              .arg(envOverride);
    }

    // ── 2. Active virtual environment ────────────────────────────────────────
    const QString virtualEnv = qEnvironmentVariable("VIRTUAL_ENV");
    if (!virtualEnv.isEmpty()) {
        const QString venvPython = venvPythonPath(virtualEnv);
        if (auto r = accept(venvPython, u"active venv ($VIRTUAL_ENV)"_s))
            return *r;
        if (!isValidPythonExe(venvPython))
            logOut << u"[launcher] VIRTUAL_ENV set but %1 not found, skipping"_s
                              .arg(venvPython);
    }

    // ── 3. Project .venv in dev tree ─────────────────────────────────────────
    //  Both layouts a developer actually produces: `python -m venv .venv` run
    //  from the repository root, and the same run from inside backend/. Only
    //  the first was checked, and this repository uses the second — so the
    //  working 3.11 environment sitting right next to the code was invisible.
    if (!backendDir.isEmpty()) {
        const QString repoRoot = QDir::cleanPath(backendDir + u"/.."_s);
        for (const auto &c : { std::pair{ backendDir + u"/.venv"_s,
                                          u"backend/.venv (dev tree)"_s },
                               std::pair{ repoRoot + u"/.venv"_s,
                                          u"project .venv (dev tree)"_s } }) {
            if (auto r = accept(venvPythonPath(c.first), c.second))
                return *r;
        }
    }

    // ── 3b. User-data venv ───────────────────────────────────────────────────
    //  Created either by scripts/setup_backend.{sh,command,bat} or by the
    //  first-run bootstrap. Lives in writable user storage so a signed bundle
    //  or a read-only AppImage stays untouched. The path comes from
    //  userDataVenvDir() so the chain and the bootstrap cannot disagree.
    if (auto r = accept(venvPythonPath(userDataVenvDir()), u"user-data venv"_s))
        return *r;

    // ── 4. Bundled Python shipped with the release ───────────────────────────
    //  A release carries its own relocatable interpreter with the backend's
    //  dependencies already installed, so a user who has no Python — or only
    //  the 3.9 macOS ships — still gets a working backend without running any
    //  setup script. Where it sits differs per package, and all three layouts
    //  are searched rather than assumed:
    //
    //    macOS    <App>.app/Contents/Resources/python/bin/python3   (inside the
    //             bundle, so the .app stays self-contained in the DMG)
    //    Windows  <pkg>/bin/python/python.exe                (next to the .exe)
    //    Linux    <pkg>/python/bin/python3                   (BESIDE the
    //             AppImage, not inside it: the AppImage is a read-only
    //             squashfs built by linuxdeploy, which walks everything in the
    //             AppDir; keeping the runtime out of its way is simpler than
    //             arguing with it. $APPIMAGE names the file being run, so its
    //             directory is where to look.)
    //
    //  The plain appDir/python3 entries stay for a hand-made layout.
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList bundled{
        QDir::cleanPath(appDir + u"/../Resources/python/bin/python3"_s),   // macOS .app
        QDir::cleanPath(appDir + u"/python/python.exe"_s),                 // Windows package
        QDir::cleanPath(appDir + u"/python/bin/python3"_s),                // generic layout
        QDir::cleanPath(appDir + u"/../python/bin/python3"_s),             // installed tree
        QDir::cleanPath(appDir + u"/python3"_s),
        QDir::cleanPath(appDir + u"/python"_s),
    };
    if (const QString appImage = qEnvironmentVariable("APPIMAGE"); !appImage.isEmpty()) {
        const QString dir = QFileInfo(appImage).absolutePath();
        bundled.prepend(QDir::cleanPath(dir + u"/python/bin/python3"_s));
    }
    for (const QString &c : bundled) {
        if (auto r = accept(c, u"bundled backend runtime"_s))
            return *r;
    }

    // ── 5. Settings / PATH-detected python ───────────────────────────────────
    if (auto r = accept(settingsPython, u"system Python (application settings)"_s))
        return *r;

    // ── 6/7. PATH, version-named first ───────────────────────────────────────
    //  Newest first, then the unversioned names. Homebrew, pyenv, deadsnakes
    //  and the python.org installers all provide pythonX.Y; on macOS the bare
    //  python3 is very often Apple's 3.9, so it must be looked at last.
    QStringList pathNames;
    for (int minor = kMaxProbedPythonMinor; minor >= kMinPythonMinor; --minor)
        pathNames << u"python%1.%2"_s.arg(kMinPythonMajor).arg(minor);
    pathNames << u"python3"_s << u"python"_s;

    // findExecutable() searches the PATH this process inherited, and a bundle
    // opened from the Finder inherits the launchd PATH — /usr/bin:/bin:/usr/sbin
    // :/sbin — not the shell's. Homebrew and MacPorts would be invisible to a
    // tester who double-clicks the app, which is precisely how a tester starts
    // it. Naming the standard prefixes costs a stat each.
    const QStringList extraDirs = {
#if defined(Q_OS_MACOS)
        u"/opt/homebrew/bin"_s,      // Homebrew, Apple silicon
        u"/usr/local/bin"_s,         // Homebrew, Intel
        u"/opt/local/bin"_s,         // MacPorts
        u"/Library/Frameworks/Python.framework/Versions/Current/bin"_s,  // python.org
#else
        u"/usr/local/bin"_s,
#endif
    };
    for (const QString &name : std::as_const(pathNames)) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            if (auto r = accept(found, u"%1 on PATH"_s.arg(name)))
                return *r;
        }
        for (const QString &dir : extraDirs) {
            const QString candidate = dir + u'/' + name;
            if (candidate == found)
                continue;   // already tried above
            if (auto r = accept(candidate, u"%1 in %2"_s.arg(name, dir)))
                return *r;
        }
    }

    logOut << u"[launcher] No interpreter met the minimum version."_s;
    return { {}, {}, rejected };
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

void BackendLauncher::stopBackendProcess(int graceMs)
{
    if (!m_process || m_process->state() != QProcess::Running)
        return;
#ifdef Q_OS_UNIX
    // setsid() in the child made its pid the group id, so one signal reaches
    // uvicorn AND every worker it spawned. Ask politely first: the backend's
    // lifespan hook shuts the pool down cleanly when it is not mid-request.
    const auto pgid = static_cast<pid_t>(m_process->processId());
    if (pgid > 0)
        ::killpg(pgid, SIGTERM);
#endif
    m_process->terminate();
    if (m_process->waitForFinished(graceMs))
        return;
#ifdef Q_OS_UNIX
    // Still there: a worker inside a long native call, or a server still
    // draining. Take the group down rather than leave orphans behind.
    if (pgid > 0)
        ::killpg(pgid, SIGKILL);
#endif
    m_process->kill();
    m_process->waitForFinished(1000);
}

BackendLauncher::~BackendLauncher()
{
    cancel();
    stopBackendProcess(3000);
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
        // Bootstrap runs the SAME interpreter the probes do — which, when the
        // bundled runtime failed a dependency check, is the one inside the .app.
        venv.setProcessEnvironment(pythonInvocationEnvironment());
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
    emit messageChanged(u"Installing backend dependencies - this takes a few "
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
    pip.setProcessEnvironment(pythonInvocationEnvironment());
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

    // The chain only returns an interpreter that is already new enough, so an
    // empty result means every candidate was missing or too old. Naming the
    // ones that were rejected is the difference between a user who installs
    // Python 3.11 and one who reinstalls the 3.9 they already have.
    if (pythonExe.isEmpty()) {
        QString detail;
        if (!resolved.rejected.isEmpty())
            detail = u"\n\nToo old:\n  %1"_s.arg(resolved.rejected.join(u"\n  "_s));
        emit failed(
                u"VisIVO needs Python %1.%2 or newer and could not find one.%3\n\n"
                u"Install a newer Python - on macOS, `brew install python@%1.%2` - "
                u"then restart VisIVO, or point it at an interpreter with the "
                u"VISIVO_BACKEND_PYTHON environment variable."_s
                        .arg(kMinPythonMajor).arg(kMinPythonMinor).arg(detail));
        return;
    }
    appendLog(u"[launcher] Using Python: %1  (%2)"_s.arg(pythonExe, pythonSource));

    if (!hasBackendDeps(pythonExe)) {
        appendLog(u"[launcher] Backend dependencies missing, bootstrapping"_s);
        if (!bootstrapEnvironment(backendDir, pythonExe))
            return;   // bootstrapEnvironment() has already emitted failed()
        // Go straight to the venv the bootstrap just built, rather than running
        // the chain again. Re-resolving would restart from the top and could
        // hand back the very interpreter whose missing dependencies triggered
        // the bootstrap — an earlier candidate that is new enough but bare, say
        // a backend/.venv or a $VIRTUAL_ENV — and then fail, reporting that the
        // install did not take when in fact it went somewhere else.
        const QString venvExe = venvPythonPath(userDataVenvDir());
        if (venvExe.isEmpty() || !isValidPythonExe(venvExe) || !hasBackendDeps(venvExe)) {
            emit failed(u"The backend environment was created but its dependencies "
                        u"are still missing. See the log for what pip reported."_s);
            return;
        }
        pythonExe = venvExe;
        pythonSource = u"user-data venv (just bootstrapped)"_s;
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
    m_process->setProcessEnvironment(backendEnvironment());
#ifdef Q_OS_UNIX
    // Give the backend its own process GROUP, so that stopping it can stop the
    // whole family. The backend runs a ProcessPoolExecutor, and its workers are
    // separate processes that do not die with their parent: quitting the client
    // was leaving them behind (317 of them, 16.8 GB, measured after a day's
    // work). The backend's own shutdown hook cleans them up when it gets to
    // run — but uvicorn drains in-flight requests BEFORE running it, so a quit
    // during a long moment map never reaches that code and the client kills the
    // server first. A group signal does not depend on the server cooperating.
    m_process->setChildProcessModifier([] { ::setsid(); });
#endif

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
