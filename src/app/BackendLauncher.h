#ifndef BackendLauncher_h
#define BackendLauncher_h

#include "BackendClient.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>

/**
 * Manages the lifecycle of the bundled backend process.
 *
 * Usage:
 *   BackendLauncher launcher;
 *   launcher.startAsync(backendUrl, pythonExe, noAutostart);
 *   // listen to alreadyRunning() / backendStarted() / failed()
 *
 * The launcher is designed to live for the full application lifetime.
 * If it launched a backend process, that process is terminated when the
 * launcher is destroyed (i.e. when the app exits).
 *
 * Autostart can be suppressed by:
 *   - passing noAutostart = true
 *   - setting env var  VISIVO_NO_BACKEND_AUTOSTART=1
 *   - passing --no-backend-autostart on the command line (parsed by main.cpp)
 */
class BackendLauncher : public QObject
{
    Q_OBJECT

public:
    explicit BackendLauncher(QObject *parent = nullptr);
    ~BackendLauncher() override;

    /**
     * Begin the startup sequence asynchronously.
     *
     * Step 1 — health-check: is the backend already up?
     * Step 2 — if not and noAutostart is false: find + launch the backend.
     * Step 3 — poll health until responding (max ~30 s) or process crashes.
     *
     * All result signals are emitted on the GUI thread.
     *
     * @param backendUrl   e.g. "http://127.0.0.1:8000"
     * @param pythonExe    absolute path to the python3 interpreter
     * @param noAutostart  skip launch; only connect to an existing backend
     */
    void startAsync(const QString &backendUrl, const QString &pythonExe, bool noAutostart);

    /** Stop polling and mark cancelled.  Does not kill a running backend. */
    void cancel();

    /** stdout + stderr captured from the backend process (usable after any state). */
    QString capturedLogs() const;

    /** True after backendStarted() or alreadyRunning() has been emitted. */
    bool isReady() const;

signals:
    /** Human-readable status line suitable for the startup dialog. */
    void messageChanged(const QString &message);

    /** Backend was already reachable — nothing was launched. */
    void alreadyRunning();

    /** Backend process was launched and is now reachable. */
    void backendStarted();

    /** Startup failed; @p error is a human-readable explanation. */
    void failed(const QString &error);

private:
    /// First-run setup: create the backend's virtual environment and install
    /// its requirements, reporting progress through messageChanged(). Returns
    /// false after emitting failed(). Only called when a supported interpreter
    /// was found but cannot import the backend's dependencies.
    bool bootstrapEnvironment(const QString &backendDir, const QString &pythonExe);

    void doInitialHealthCheck();
    void onInitialHealthDone();
    void launchProcess();
    void startPolling();
    void onPollTick();
    void onPollHealthDone();
    void onProcessError(QProcess::ProcessError error);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void appendLog(const QString &text);

    QString m_backendUrl;
    QString m_pythonExe;
    bool m_noAutostart{ false };
    bool m_cancelled{ false };
    bool m_ready{ false };
    bool m_pollCheckInFlight{ false };
    bool m_processFinishedUnexpectedly{ false };

    QProcess *m_process{ nullptr };
    QTimer m_pollTimer;
    QElapsedTimer m_elapsed;
    QFutureWatcher<BackendHealthResult> m_initialHealthWatcher;
    QFutureWatcher<BackendHealthResult> m_pollHealthWatcher;
    QString m_capturedLogs;

    static constexpr int kMaxWaitMs      = 30'000;
    static constexpr int kPollIntervalMs =    600;
};

#endif // BackendLauncher_h
