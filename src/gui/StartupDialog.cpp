#include "StartupDialog.h"

#include "app/BackendClient.h"
#include "app/BackendLauncher.h"
#include "Settings.h"
#include "theme/VisivoTheme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

using namespace Qt::StringLiterals;

// ---------------------------------------------------------------------------
// Style constants (consistent with VisivoTheme dark palette)
// ---------------------------------------------------------------------------

namespace {

constexpr int kDialogWidth = 460;

// Step state → CSS colour for the icon label. Pending uses a theme-aware
// muted colour so it stays legible on both light and dark surfaces; Active /
// Done / Error are intentionally fixed (brand blue + universal semantic hues).
QString iconColor(StartupDialog::StepState state)
{
    switch (state) {
    case StartupDialog::StepState::Pending:
        return QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());
    case StartupDialog::StepState::Active:  return u"#048ABF"_s;
    case StartupDialog::StepState::Done:    return u"#4CAF50"_s;
    case StartupDialog::StepState::Error:   return u"#EF5350"_s;
    }
    return QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());
}

// Step state → Unicode icon character
const char *iconGlyph(StartupDialog::StepState state)
{
    switch (state) {
    case StartupDialog::StepState::Pending: return "○";
    case StartupDialog::StepState::Active:  return "◉";
    case StartupDialog::StepState::Done:    return "✓";
    case StartupDialog::StepState::Error:   return "✗";
    }
    return "○";
}

QPushButton *makeButton(const QString &text, QWidget *parent = nullptr)
{
    auto *btn = new QPushButton(text, parent);
    btn->setMinimumWidth(110);
    return btn;
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

StartupDialog::StartupDialog(BackendLauncher *launcher, bool noAutostart, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint),
      m_launcher(launcher),
      m_noAutostart(noAutostart)
{
    const QString workDir = QDir::home().absoluteFilePath(qApp->applicationName());
    m_settings = new Settings(workDir, this);

    setWindowTitle(u"VisIVO Visual Analytics, Starting"_s);
    setFixedWidth(kDialogWidth);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    buildUi();

    // Wire launcher signals.
    QObject::connect(m_launcher, &BackendLauncher::messageChanged,
                     this, &StartupDialog::onBackendMessage);
    QObject::connect(m_launcher, &BackendLauncher::alreadyRunning,
                     this, &StartupDialog::onBackendAlreadyRunning);
    QObject::connect(m_launcher, &BackendLauncher::backendStarted,
                     this, &StartupDialog::onBackendStarted);
    QObject::connect(m_launcher, &BackendLauncher::failed,
                     this, &StartupDialog::onBackendFailed);

    // Kick off the sequence after the event loop starts (gives the dialog
    // time to paint before any blocking call).
    QTimer::singleShot(120, this, &StartupDialog::beginSequence);
}

StartupDialog::~StartupDialog() = default;

QString StartupDialog::resolvedToken() const
{
    return m_resolvedToken;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void StartupDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(32, 28, 32, 24);
    root->setSpacing(0);

    // ── Branding ──────────────────────────────────────────────────────────────
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(14);

        // Theme-aware logo: white mark on dark, colored mark on light.
        if (auto *logoLabel = VisivoTheme::createVisivoLogoLabel(40, this)) {
            row->addWidget(logoLabel);
        } else {
            auto *fallback = new QLabel(u"⬡"_s, this);
            fallback->setFixedSize(40, 40);
            row->addWidget(fallback);
        }

        auto *titleCol = new QVBoxLayout;
        titleCol->setSpacing(2);

        auto *titleLabel = new QLabel(u"VisIVO Visual Analytics"_s, this);
        titleLabel->setStyleSheet(
                u"font-size: 16px; font-weight: bold; color: %1;"_s
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        titleCol->addWidget(titleLabel);

        const QString version = QApplication::applicationVersion();
        if (!version.isEmpty()) {
            auto *versionLabel = new QLabel(u"v%1"_s.arg(version), this);
            versionLabel->setStyleSheet(
                    u"font-size: 11px; color: %1;"_s
                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
            titleCol->addWidget(versionLabel);
        }
        row->addLayout(titleCol);
        row->addStretch();
        root->addLayout(row);
    }

    root->addSpacing(20);

    // Theme-aware colours used across the dialog body.
    const QLatin1StringView surfaceLow      = QLatin1StringView(VisivoTheme::kSurfaceContainerLow());
    const QLatin1StringView onSurfaceTxt    = QLatin1StringView(VisivoTheme::kOnSurface());
    const QLatin1StringView mutedTxt        = QLatin1StringView(VisivoTheme::kOnSurfaceVariant());
    // The log panel uses an even-darker surface in dark mode and a slight
    // tint in light mode to read like a console area.
    const QLatin1StringView logBg = VisivoTheme::isDark()
            ? "#060E18"_L1
            : QLatin1StringView(VisivoTheme::kSurfaceContainerHighest());

    // ── Step rows ─────────────────────────────────────────────────────────────
    {
        auto *stepsFrame = new QFrame(this);
        stepsFrame->setStyleSheet(
                u"QFrame { background: %1; border-radius: 8px; }"_s.arg(surfaceLow));
        auto *grid = new QGridLayout(stepsFrame);
        grid->setContentsMargins(16, 14, 16, 14);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(10);
        grid->setColumnStretch(1, 1);

        auto makeRow = [&](int row, const QString &labelText) -> StepRow {
            StepRow r;
            r.icon = new QLabel(u"○"_s, stepsFrame);
            r.icon->setStyleSheet(
                    u"font-size: 16px; color: %1; font-weight: bold;"_s.arg(mutedTxt));
            r.icon->setFixedWidth(20);
            r.icon->setAlignment(Qt::AlignCenter);
            r.text = new QLabel(labelText, stepsFrame);
            r.text->setStyleSheet(u"font-size: 13px; color: %1;"_s.arg(mutedTxt));
            r.detail = new QLabel(stepsFrame);
            r.detail->setStyleSheet(
                    u"font-size: 12px; color: %1; font-style: italic;"_s.arg(mutedTxt));
            r.detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(r.icon,   row, 0, Qt::AlignVCenter);
            grid->addWidget(r.text,   row, 1, Qt::AlignVCenter);
            grid->addWidget(r.detail, row, 2, Qt::AlignVCenter);
            return r;
        };

        m_rowBackend = makeRow(0, u"Backend"_s);
        m_rowAuth    = makeRow(1, u"Authentication"_s);
        m_rowReady   = makeRow(2, u"Loading application"_s);
        root->addWidget(stepsFrame);
    }

    root->addSpacing(16);

    // ── Progress bar (indeterminate / busy indicator) ─────────────────────────
    {
        m_progress = new QProgressBar(this);
        m_progress->setRange(0, 0);   // marquee / indeterminate
        m_progress->setFixedHeight(3);
        m_progress->setTextVisible(false);
        m_progress->setStyleSheet(
                u"QProgressBar { background: %1; border: none; border-radius: 2px; }"
                u"QProgressBar::chunk { background: #048ABF; border-radius: 2px; }"_s
                        .arg(surfaceLow));
        root->addWidget(m_progress);
    }

    root->addSpacing(10);

    // ── Status message ────────────────────────────────────────────────────────
    {
        m_statusLabel = new QLabel(u"Starting…"_s, this);
        m_statusLabel->setStyleSheet(u"font-size: 12px; color: %1;"_s.arg(mutedTxt));
        m_statusLabel->setAlignment(Qt::AlignLeft);
        m_statusLabel->setWordWrap(true);
        root->addWidget(m_statusLabel);
    }

    root->addSpacing(10);

    // ── Auth input panel (hidden until needed) ────────────────────────────────
    {
        m_authPanel = new QWidget(this);
        m_authPanel->setVisible(false);
        auto *authLayout = new QVBoxLayout(m_authPanel);
        authLayout->setContentsMargins(0, 0, 0, 0);
        authLayout->setSpacing(6);

        auto *authDesc = new QLabel(
                u"VLKB services require an API token to access remote data.\n"
                u"Paste your token below to continue."_s,
                m_authPanel);
        authDesc->setStyleSheet(u"font-size: 12px; color: %1;"_s.arg(mutedTxt));
        authDesc->setWordWrap(true);
        authLayout->addWidget(authDesc);

        m_tokenEdit = new QLineEdit(m_authPanel);
        m_tokenEdit->setPlaceholderText(u"Paste your API token here…"_s);
        m_tokenEdit->setEchoMode(QLineEdit::Password);
        authLayout->addWidget(m_tokenEdit);

        m_tokenSaveHint = new QLabel(
                u"The token will be saved to  ~/.visivo_token  and to application settings."_s,
                m_authPanel);
        m_tokenSaveHint->setStyleSheet(
                u"font-size: 11px; color: %1; font-style: italic;"_s.arg(mutedTxt));
        m_tokenSaveHint->setWordWrap(true);
        authLayout->addWidget(m_tokenSaveHint);

        root->addWidget(m_authPanel);
    }

    // ── Log panel (hidden until "Show Logs" is clicked) ───────────────────────
    {
        m_logView = new QTextEdit(this);
        m_logView->setReadOnly(true);
        m_logView->setVisible(false);
        m_logView->setFixedHeight(160);
        m_logView->setStyleSheet(
                u"QTextEdit { background: %1; color: %2; "
                u"font-family: monospace; font-size: 11px; border: none; }"_s
                        .arg(logBg, onSurfaceTxt));
        root->addWidget(m_logView);
    }

    root->addSpacing(16);

    // ── Button row ────────────────────────────────────────────────────────────
    {
        auto *btnRow = new QHBoxLayout;
        btnRow->setSpacing(8);

        m_showLogsBtn = makeButton(u"Show Logs"_s, this);
        m_showLogsBtn->setVisible(false);
        QObject::connect(m_showLogsBtn, &QPushButton::clicked,
                         this, &StartupDialog::onShowLogs);

        btnRow->addWidget(m_showLogsBtn);
        btnRow->addStretch();

        m_skipBtn = makeButton(u"Skip (no VLKB)"_s, this);
        m_skipBtn->setVisible(false);
        m_skipBtn->setToolTip(u"Continue without a token, VLKB data access will be unavailable."_s);
        QObject::connect(m_skipBtn, &QPushButton::clicked,
                         this, &StartupDialog::onSkipAuth);

        m_saveBtn = makeButton(u"Save && Continue"_s, this);
        m_saveBtn->setVisible(false);
        QObject::connect(m_saveBtn, &QPushButton::clicked,
                         this, &StartupDialog::onSaveAndContinue);

        m_retryBtn = makeButton(u"Retry"_s, this);
        m_retryBtn->setVisible(false);
        QObject::connect(m_retryBtn, &QPushButton::clicked,
                         this, &StartupDialog::onRetry);

        m_quitBtn = makeButton(u"Quit"_s, this);
        QObject::connect(m_quitBtn, &QPushButton::clicked,
                         this, &StartupDialog::onQuit);

        btnRow->addWidget(m_skipBtn);
        btnRow->addWidget(m_saveBtn);
        btnRow->addWidget(m_retryBtn);
        btnRow->addWidget(m_quitBtn);
        root->addLayout(btnRow);
    }

    adjustSize();
}

// ---------------------------------------------------------------------------
// Sequence driver
// ---------------------------------------------------------------------------

void StartupDialog::beginSequence()
{
    setStep(StepId::Backend, StepState::Active, u"Checking…"_s);
    setProgressActive(true);
    setStatusMessage(u"Checking backend…"_s);
    showIdleButtons();

    const QString backendUrl  = m_settings->getBackendUrl();
    const QString pythonExe   = m_settings->getPythonLocation();
    m_launcher->startAsync(backendUrl, pythonExe, m_noAutostart,
                           m_settings->getBackendToken());
}

// ---------------------------------------------------------------------------
// BackendLauncher signal handlers
// ---------------------------------------------------------------------------

void StartupDialog::onBackendMessage(const QString &msg)
{
    setStatusMessage(msg);
}

void StartupDialog::onBackendAlreadyRunning()
{
    setStep(StepId::Backend, StepState::Done, u"Connected"_s);
    backendReady();
}

void StartupDialog::onBackendStarted()
{
    setStep(StepId::Backend, StepState::Done, u"Started"_s);
    backendReady();
}

void StartupDialog::onBackendFailed(const QString &error)
{
    showBackendError(error);
}

// ---------------------------------------------------------------------------
// Step transitions
// ---------------------------------------------------------------------------

void StartupDialog::backendReady()
{
    setStep(StepId::Auth, StepState::Active, u"Checking…"_s);
    setStatusMessage(u"Configuring authentication…"_s);
    QTimer::singleShot(200, this, &StartupDialog::beginAuthStep);
}

void StartupDialog::beginAuthStep()
{
    // A remote backend chosen for this run brings its own token (or needs
    // none: its health check has already answered). The local backend's token
    // file is not its token, and nothing typed here may overwrite that file.
    if (Settings::sessionBackend()) {
        authReady(m_settings->getBackendToken());
        return;
    }

    // Resolve token: Settings → env var / token file.
    QString token = m_settings->getBackendToken();
    if (token.isEmpty())
        token = BackendClient::readTokenFile();

    if (!token.isEmpty()) {
        authReady(token);
        return;
    }

    // No token found — show inline setup.
    setStep(StepId::Auth, StepState::Active, u"Required"_s);
    setStatusMessage(u"Authentication required"_s);
    setProgressActive(false);
    m_authPanel->setVisible(true);
    m_tokenEdit->setFocus();
    showAuthButtons();
    adjustSize();
}

void StartupDialog::authReady(const QString &token)
{
    m_resolvedToken = token;
    setStep(StepId::Auth, StepState::Done, u"Ready"_s);
    setStatusMessage(u"Authentication configured"_s);
    m_authPanel->setVisible(false);
    adjustSize();
    QTimer::singleShot(300, this, &StartupDialog::beginReadyStep);
}

void StartupDialog::beginReadyStep()
{
    setStep(StepId::Ready, StepState::Active);
    setStatusMessage(u"Loading application…"_s);
    setProgressActive(true);
    showIdleButtons();
    // Brief pause so the user can read the "Ready" state before the window opens.
    QTimer::singleShot(600, this, [this]() {
        setStep(StepId::Ready, StepState::Done, u"Ready"_s);
        setStatusMessage(u"Ready"_s);
        setProgressActive(false);
        QTimer::singleShot(400, this, [this]() { accept(); });
    });
}

// ---------------------------------------------------------------------------
// Error display
// ---------------------------------------------------------------------------

void StartupDialog::showBackendError(const QString &error)
{
    setStep(StepId::Backend, StepState::Error, u"Failed"_s);
    setProgressActive(false);
    setStatusMessage(error);
    showErrorButtons();
    // Pre-populate log view.
    const QString logs = m_launcher->capturedLogs();
    if (!logs.isEmpty())
        appendLog(logs);
}

// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------

void StartupDialog::onRetry()
{
    // Reset all step rows to pending and restart.
    setStep(StepId::Backend, StepState::Pending);
    setStep(StepId::Auth,    StepState::Pending);
    setStep(StepId::Ready,   StepState::Pending);
    m_authPanel->setVisible(false);
    showLogPanel(false);
    m_logView->clear();
    adjustSize();

    QTimer::singleShot(100, this, &StartupDialog::beginSequence);
}

void StartupDialog::onShowLogs()
{
    showLogPanel(!m_logVisible);
    // Append any new logs captured since last open.
    const QString logs = m_launcher->capturedLogs();
    if (!logs.isEmpty() && m_logView->toPlainText().isEmpty())
        m_logView->setPlainText(logs);
}

void StartupDialog::onQuit()
{
    m_launcher->cancel();
    reject();
}

void StartupDialog::onSaveAndContinue()
{
    const QString token = m_tokenEdit->text().trimmed();
    if (token.isEmpty()) {
        m_tokenSaveHint->setText(
                u"⚠ Token cannot be empty. Paste a valid token or click Skip."_s);
        m_tokenSaveHint->setStyleSheet(u"font-size: 11px; color: #EF5350;"_s);
        return;
    }

    if (Settings::sessionBackend()) {   // not reached (beginAuthStep), but never the local file
        authReady(token);
        return;
    }

    // Write to ~/.visivo_token (same file BackendClient::readTokenFile() reads).
    const QString tokenPath = QDir::home().absoluteFilePath(u".visivo_token"_s);
    QFile f(tokenPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        f.write(token.toUtf8());
        f.close();
    } else {
        // Write failed — continue anyway; token will be used from Settings only.
        qWarning() << "StartupDialog: could not write" << tokenPath << f.errorString();
    }

    // Persist to Settings so other code paths also see it.
    m_settings->setBackendToken(token);

    authReady(token);
}

void StartupDialog::onSkipAuth()
{
    authReady(QString()); // empty token — VLKB features will fail gracefully
}

// ---------------------------------------------------------------------------
// Step appearance helpers
// ---------------------------------------------------------------------------

void StartupDialog::setStep(StepId step, StepState state, const QString &detail)
{
    StepRow *row = nullptr;
    switch (step) {
    case StepId::Backend: row = &m_rowBackend; break;
    case StepId::Auth:    row = &m_rowAuth;    break;
    case StepId::Ready:   row = &m_rowReady;   break;
    }
    if (!row) return;

    // Icon
    if (row->icon) {
        row->icon->setText(QString::fromUtf8(iconGlyph(state)));
        row->icon->setStyleSheet(
                u"font-size: 16px; font-weight: bold; color: %1;"_s
                .arg(iconColor(state)));
    }
    // Text colour: bright when active/done/error, muted when pending.
    // Both ends of the spectrum come from the theme palette so the contrast
    // tracks the surrounding card whether dark or light.
    if (row->text) {
        const bool active = (state != StepState::Pending);
        const QString textColor = active
                ? QString::fromLatin1(VisivoTheme::kOnSurface())
                : QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());
        row->text->setStyleSheet(
                u"font-size: 13px; color: %1;"_s.arg(textColor));
    }
    // Detail
    if (row->detail) {
        row->detail->setText(detail);
        const QString detailColor = [&] {
            switch (state) {
            case StepState::Done:  return u"#4CAF50"_s;
            case StepState::Error: return u"#EF5350"_s;
            default:               return QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());
            }
        }();
        row->detail->setStyleSheet(
                u"font-size: 12px; color: %1; font-style: italic;"_s.arg(detailColor));
    }
}

void StartupDialog::setStatusMessage(const QString &msg)
{
    if (m_statusLabel)
        m_statusLabel->setText(msg);
}

void StartupDialog::setProgressActive(bool active)
{
    if (m_progress) {
        m_progress->setVisible(active);
        if (active) {
            // Force marquee mode (range 0,0 = indeterminate).
            m_progress->setRange(0, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Button visibility helpers
// ---------------------------------------------------------------------------

void StartupDialog::showIdleButtons()
{
    m_retryBtn->setVisible(false);
    m_showLogsBtn->setVisible(false);
    m_skipBtn->setVisible(false);
    m_saveBtn->setVisible(false);
    m_quitBtn->setVisible(true);
}

void StartupDialog::showErrorButtons()
{
    m_retryBtn->setVisible(true);
    m_showLogsBtn->setVisible(true);
    m_skipBtn->setVisible(false);
    m_saveBtn->setVisible(false);
    m_quitBtn->setVisible(true);
}

void StartupDialog::showAuthButtons()
{
    m_retryBtn->setVisible(false);
    m_showLogsBtn->setVisible(false);
    m_skipBtn->setVisible(true);
    m_saveBtn->setVisible(true);
    m_quitBtn->setVisible(true);
}

// ---------------------------------------------------------------------------
// Log panel
// ---------------------------------------------------------------------------

void StartupDialog::showLogPanel(bool visible)
{
    m_logVisible = visible;
    m_logView->setVisible(visible);
    m_showLogsBtn->setText(visible ? u"Hide Logs"_s : u"Show Logs"_s);
    adjustSize();
}

void StartupDialog::appendLog(const QString &text)
{
    if (m_logView)
        m_logView->setPlainText(text);
}
