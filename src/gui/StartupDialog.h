#ifndef StartupDialog_h
#define StartupDialog_h

#include <QDialog>
#include <QPointer>

class BackendLauncher;
class Settings;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTextEdit;
class QWidget;

/**
 * Startup / splash dialog shown before the main window.
 *
 * Drives a three-step sequence:
 *   1. Backend   — health-check, optional auto-start via BackendLauncher
 *   2. Auth      — token detection; inline setup if missing
 *   3. Ready     — auto-accepts the dialog after a brief confirmation delay
 *
 * The dialog does NOT create MainWindow; it only signals readiness.
 * On QDialog::Accepted the caller should create and show MainWindow.
 * On QDialog::Rejected (Quit button) the caller should exit.
 *
 * @param launcher    BackendLauncher owned by the caller (must outlive dialog).
 * @param noAutostart Passed straight to launcher->startAsync().
 * @param parent      Optional parent widget.
 */
class StartupDialog : public QDialog
{
    Q_OBJECT

public:
    // Exposed so anonymous-namespace helpers in the .cpp can use them.
    enum class StepState { Pending, Active, Done, Error };

    explicit StartupDialog(BackendLauncher *launcher, bool noAutostart,
                           QWidget *parent = nullptr);
    ~StartupDialog() override;

    /** Token that was resolved or entered by the user (may be empty if skipped). */
    QString resolvedToken() const;

private slots:
    // ── BackendLauncher signals ───────────────────────────────────────────────
    void onBackendMessage(const QString &msg);
    void onBackendAlreadyRunning();
    void onBackendStarted();
    void onBackendFailed(const QString &error);

    // ── Button actions ────────────────────────────────────────────────────────
    void onRetry();
    void onShowLogs();
    void onQuit();
    void onSaveAndContinue();
    void onSkipAuth();

private:
    // ── Step / state helpers ──────────────────────────────────────────────────
    enum class StepId { Backend, Auth, Ready };

    void buildUi();
    void beginSequence();

    void setStep(StepId step, StepState state, const QString &detail = {});
    void setStatusMessage(const QString &msg);
    void setProgressActive(bool active);

    // Backend step
    void backendReady();

    // Auth step
    void beginAuthStep();
    void authReady(const QString &token);

    // Ready step
    void beginReadyStep();

    // Error / log helpers
    void showBackendError(const QString &error);
    void showLogPanel(bool visible);
    void appendLog(const QString &text);

    // Button visibility helpers
    void showIdleButtons();          // only Quit
    void showErrorButtons();         // Retry + Show Logs + Quit
    void showAuthButtons();          // Skip + Save & Continue + Quit

    // ── Owned objects ─────────────────────────────────────────────────────────
    BackendLauncher *m_launcher;     // borrowed — NOT owned
    Settings        *m_settings;     // owned by this dialog
    bool             m_noAutostart;
    QString          m_resolvedToken;

    // ── Step row widgets ──────────────────────────────────────────────────────
    //   Each row: [icon label] [text label] [detail label]
    struct StepRow {
        QPointer<QLabel> icon;
        QPointer<QLabel> text;
        QPointer<QLabel> detail;
    };
    StepRow m_rowBackend;
    StepRow m_rowAuth;
    StepRow m_rowReady;

    // ── Status / progress ─────────────────────────────────────────────────────
    QPointer<QLabel>       m_statusLabel;
    QPointer<QProgressBar> m_progress;

    // ── Auth input panel ──────────────────────────────────────────────────────
    QPointer<QWidget>  m_authPanel;
    QPointer<QLineEdit> m_tokenEdit;
    QPointer<QLabel>   m_tokenSaveHint;

    // ── Log panel ─────────────────────────────────────────────────────────────
    QPointer<QTextEdit> m_logView;
    bool m_logVisible{ false };

    // ── Buttons ───────────────────────────────────────────────────────────────
    QPointer<QPushButton> m_retryBtn;
    QPointer<QPushButton> m_showLogsBtn;
    QPointer<QPushButton> m_quitBtn;
    QPointer<QPushButton> m_skipBtn;
    QPointer<QPushButton> m_saveBtn;
};

#endif // StartupDialog_h
