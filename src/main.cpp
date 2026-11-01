#include "MainWindow.h"
#include "Version.h"
#include "app/BackendClient.h"
#include "app/BackendLauncher.h"
#include "app/BackendUrl.h"
#include "app/ClusterConnection.h"
#include "app/LaunchOptions.h"
#include "gui/ClusterConnectDialog.h"
#include "app/ThemeManager.h"
#include "gui/StartupDialog.h"
#include "gui/theme/ScrollWheelGuard.h"
#include "Settings.h"

#include <QVTKOpenGLNativeWidget.h>

#include <QApplication>

#include "DiagnosticsManager.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <QIcon>
#include <QUuid>
#include <QLocale>
#include <QProcessEnvironment>
#include <QMessageBox>
#include <QSettings>
#include <QSurfaceFormat>
#include <QWebEngineUrlScheme>

#include <clocale>
#include <memory>
#include <optional>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkOutputWindow.h>
#include <vtkWindow.h>

// On hybrid-graphics Windows laptops (NVIDIA Optimus / AMD PowerXpress), the
// driver decides which GPU runs the app by *inspecting these exported symbols
// from the .exe itself*. They must live in the main executable's exports table
// — exporting them from a DLL has no effect. Without this hint, VisIVO falls
// onto the Intel iGPU while the Oculus / SteamVR compositor binds to the
// discrete GPU, and xrCreateSession fails with XR_ERROR_GRAPHICS_DEVICE_INVALID.
#ifdef _WIN32
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {
// VTK's default "output window" on Windows is vtkWin32OutputWindow, a modal
// popup that overrides DisplayText() so SetDisplayMode(...) on the global
// instance has no effect — the popup still appears. The only way to fully
// silence it is to swap the global instance for one that NEVER touches the OS
// GUI. We use a minimal subclass that forwards everything to qWarning(), so
// VTK diagnostics stay visible in stdout/stderr and any Qt log handler the
// app installs catches them along with regular Qt messages.
class VTKToQtOutputWindow : public vtkOutputWindow
{
public:
    vtkTypeMacro(VTKToQtOutputWindow, vtkOutputWindow);
    static VTKToQtOutputWindow *New()
    {
        auto *self = new VTKToQtOutputWindow;
        self->InitializeObjectBase();
        return self;
    }
    void DisplayText(const char *msg) override
    {
        if (msg && *msg) {
            qWarning().noquote() << "[vtk]" << msg;
        }
    }
protected:
    VTKToQtOutputWindow()  = default;
    ~VTKToQtOutputWindow() override = default;
private:
    VTKToQtOutputWindow(const VTKToQtOutputWindow &) = delete;
    void operator=(const VTKToQtOutputWindow &)      = delete;
};
} // namespace

using namespace Qt::StringLiterals;


/// Files the desktop asks us to open — a Finder double-click, a drop on the Dock
/// icon, `open -a`, a file manager's "Open with", or a path on the command line.
///
/// macOS does NOT pass a double-clicked file in argv: it sends an Apple Event
/// that Qt delivers as QEvent::FileOpen, and only to an application object that
/// is already running. So the filter is installed immediately after
/// QApplication is constructed — before the startup dialog, which takes seconds
/// while the backend comes up and would otherwise swallow the event — and the
/// paths wait in a queue until MainWindow exists. Afterwards each event is
/// forwarded straight through, so double-clicking a second file while VisIVO is
/// running opens it in the same instance.
class DesktopOpenRelay : public QObject
{
public:
    ~DesktopOpenRelay() override
    {
        // Order matters. Members are destroyed before ~QObject deletes its
        // children, so the lock would be released while the QLocalServer was
        // still listening — long enough for a new primary to take the lock and
        // listen, and then have its socket unlinked by this one's cleanup
        //. Close and delete the server first, then let go of the lock.
        if (m_server) {
            m_server->close();
            delete m_server;
            m_server = nullptr;
        }
        m_lock.reset();
    }

    /// Paths from the command line (Linux / Windows associations pass them this
    /// way), keeping only what exists — the flags this app accepts are not files.
    void takeCommandLine(const QStringList &args)
    {
        for (int i = 1; i < args.size(); ++i) {
            const QString a = args.at(i);
            if (LaunchOptions::backendValueOptions().contains(a)) {
                ++i;   // its value (a URL, a token file, a name) is not a file to open
                continue;
            }
            if (a.startsWith(QLatin1Char('-')))
                continue;
            if (QFileInfo::exists(a))
                m_pending << QFileInfo(a).absoluteFilePath();
        }
    }

    /// Called once MainWindow is up: flush the queue and keep forwarding.
    void attach(MainWindow *window)
    {
        m_window = window;
        flush();
    }

    bool hasPending() const { return !m_pending.isEmpty(); }
    QStringList pending() const { return m_pending; }

    /// Decide what this process is, and act on it. Returns false when the files
    /// were handed to an already-running instance and this one should exit.
    ///
    /// Linux and Windows deliver a double-click as a new PROCESS with the path in
    /// argv, so "is there already a VisIVO?" has to be answered before the
    /// startup sequence. A QLockFile — not the socket — is the authority: two
    /// launches racing to probe-and-replace a stale socket could unlink each
    /// other's live endpoint, and on Windows several QLocalServers may listen on
    /// one pipe name, so `listen()` succeeding proves nothing about ownership
    ///. Only the lock holder listens.
    bool becomePrimaryOrForward(bool mayForward = true)
    {
        if (!mayForward) {
            // A run on a remote backend: its files open here, and it never takes
            // the seat, so a later plain launch never sends its files to it.
            return true;
        }
        const QString base = instanceKey();
        m_lock = std::make_unique<QLockFile>(
                QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                        .absoluteFilePath(base + QStringLiteral(".lock")));
        m_lock->setStaleLockTime(0);   // never steal by age; a live holder answers
        if (m_lock->tryLock(300)) {
            // Primary. Any socket left by a crash is ours to clear, and doing it
            // under the lock is what makes that safe.
            QLocalServer::removeServer(base);
            startListening(base);
            return true;
        }
        m_lock.reset();
        if (m_pending.isEmpty()) {
            // A bare second launch: a normal, independent instance.
            startTakeoverPolling(base);
            return true;
        }
        // Files to hand over. The primary may still be starting up, so retry a
        // little before falling back to opening them here.
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (forwardToRunningInstance(base, m_pending))
                return false;
            QThread::msleep(200);
        }
        // The handover failed — most likely the primary died mid-flight — so
        // these files open here, and this instance watches for the seat too
        //: otherwise the surviving window could never receive the next
        // double-click and a third instance would start.
        startTakeoverPolling(base);
        return true;
    }

private:
    /// Watch the lock: when the primary exits, the surviving instance becomes
    /// the one that receives files.
    void startTakeoverPolling(const QString &base)
    {
        m_takeoverKey = base;
        auto *retry = new QTimer(this);
        retry->setInterval(5000);
        QObject::connect(retry, &QTimer::timeout, this, [this, retry]() {
            if (tryTakeOver())
                retry->stop();
        });
        retry->start();
    }

    /// Become the primary if the seat is free (a secondary polls this).
    bool tryTakeOver()
    {
        if (m_takeoverKey.isEmpty() || m_lock)
            return true;
        auto lock = std::make_unique<QLockFile>(
                QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                        .absoluteFilePath(m_takeoverKey + QStringLiteral(".lock")));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0))
            return false;
        m_lock = std::move(lock);
        QLocalServer::removeServer(m_takeoverKey);
        startListening(m_takeoverKey);
        return true;
    }

    /// Accept handovers. The wire format is a QDataStream-serialised QStringList,
    /// not newline-separated text: a Unix filename may contain a newline or end
    /// in a space, and readyRead delivers arbitrary fragments — half a path, or
    /// half a UTF-8 character. A stream transaction consumes only whole frames.
    void startListening(const QString &name)
    {
        m_server = new QLocalServer(this);
        QObject::connect(m_server, &QLocalServer::newConnection, this, [this]() {
            while (QLocalSocket *c = m_server->nextPendingConnection()) {
                // "I am alive and reading": the sender waits for this BEFORE
                // writing anything. If our event loop were stuck it would never
                // arrive, the sender would send nothing at all, and it can then
                // open the files itself with no risk of us opening them too
                // (an ack AFTER the payload cannot undo the delivery).
                c->write("RDY");
                c->flush();
                // Shared, because a QDataStream is not a QObject and cannot be
                // parented to the socket; both lambdas hold it, so it dies with
                // the connection and never outlives its device.
                auto stream = std::make_shared<QDataStream>(c);
                stream->setVersion(QDataStream::Qt_6_0);
                QObject::connect(c, &QLocalSocket::readyRead, c, [this, c, stream]() {
                    QStringList paths;
                    stream->startTransaction();
                    *stream >> paths;
                    if (!stream->commitTransaction())
                        return;   // incomplete frame — wait for the rest
                    for (const QString &p : paths)
                        if (QFileInfo::exists(p))
                            m_pending << QFileInfo(p).absoluteFilePath();
                    // Accepted. The sender waits for this before exiting: RDY
                    // only proved the transport was being read, and a primary
                    // that died between RDY and here would have lost the files
                    // silently.
                    c->write("DONE");
                    c->flush();
                    flush();
                });
                QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
            }
        });
        m_server->listen(name);
    }

    /// Read until @p token appears, or the deadline passes. A single
    /// waitForReadyRead() can return a fragment — "DO" then "NE" — and treating
    /// that as failure made the sender open files the primary had already
    /// accepted. Bytes already buffered are consumed first.
    static bool waitForToken(QLocalSocket &sock, const QByteArray &token, int msec)
    {
        QByteArray buf = sock.readAll();
        QDeadlineTimer deadline(msec);
        while (!buf.contains(token)) {
            const int left = int(deadline.remainingTime());
            if (left <= 0 || !sock.waitForReadyRead(left))
                return false;
            buf += sock.readAll();
            if (buf.size() > 64)
                buf = buf.right(64);   // bounded: the tokens are 3-4 bytes
        }
        return true;
    }

    /// Two-phase handover: wait for the primary's "RDY", THEN write. Nothing is
    /// written to a primary that is not reading, so a timeout can never leave a
    /// request that is delivered late while this process also opened the files.
    static bool forwardToRunningInstance(const QString &name, const QStringList &paths)
    {
        if (paths.isEmpty())
            return false;
        QLocalSocket sock;
        sock.connectToServer(name);
        if (!sock.waitForConnected(300))
            return false;
        if (!waitForToken(sock, QByteArrayLiteral("RDY"), 1500))
            return false;   // no RDY: primary busy or dead — we sent nothing
        QByteArray block;
        {
            QDataStream out(&block, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_0);
            out << paths;
        }
        sock.write(block);
        // waitForBytesWritten promises only that SOME bytes went out, and this
        // process exits right after; drain before letting it.
        while (sock.bytesToWrite() > 0)
            if (!sock.waitForBytesWritten(1000))
                return false;
        // …and wait to be told the frame was ACCEPTED, not merely transported.
        // On timeout this returns false and the caller opens the files here: a
        // file opening twice is visible and recoverable, a file that silently
        // never opens is not, so the trade is deliberate.
        return waitForToken(sock, QByteArrayLiteral("DONE"), 3000);
    }

public:
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            auto *fe = static_cast<QFileOpenEvent *>(event);
            QString path = fe->file();
            if (path.isEmpty() && fe->url().isLocalFile())
                path = fe->url().toLocalFile();
            if (!path.isEmpty()) {
                m_pending << path;
                flush();
                return true;
            }
        }
        return QObject::eventFilter(obj, event);
    }

private:
    // ONE at a time, and never on top of a modal dialog. Opening a dataset runs
    // synchronous backend calls that spin nested event loops, so two queued
    // opens interleave: the second one's dialog suspends the first one's health
    // request until its timeout fires, and a perfectly reachable backend is
    // reported unavailable.
    void flush()
    {
        if (!m_window || m_busy || m_pending.isEmpty())
            return;   // still starting up, or an open is in flight
        if (QApplication::activeModalWidget()) {
            // A dialog owns the interaction (the startup sequence, an open error,
            // a file chooser). Come back rather than stacking another on top.
            QTimer::singleShot(250, this, [this]() { flush(); });
            return;
        }
        const QString path = m_pending.takeFirst();
        m_busy = true;
        // Queued, not direct: this can run from inside the event filter.
        QMetaObject::invokeMethod(
                m_window,
                [this, w = m_window, path]() {
                    if (w)
                        w->openDatasetPath(path);
                    m_busy = false;
                    flush();   // the next one, now that this open has returned
                },
                Qt::QueuedConnection);
    }

    /// Socket + lock name, one per account. NOT from the environment ($USER is
    /// unset in a Finder/launchd-started macOS app, $USERNAME is not unique
    /// across Windows accounts) and NOT from qHash, whose algorithm is not
    /// stable across Qt versions — two builds would derive different names
    ///. A SHA-256 digest of the home path is stable everywhere, and
    /// truncating it keeps the name inside the ~104-byte Unix-socket path limit.
    static QString instanceKey()
    {
        const QByteArray digest = QCryptographicHash::hash(QDir::homePath().toUtf8(),
                                                           QCryptographicHash::Sha256);
        return QStringLiteral("visivo-va-") + QString::fromLatin1(digest.toHex().left(16));
    }

    QStringList m_pending;
    QPointer<MainWindow> m_window;
    QLocalServer *m_server{ nullptr };
    std::unique_ptr<QLockFile> m_lock;   ///< held for this process's lifetime
    QString m_takeoverKey;               ///< set in a secondary, which polls for the seat
    bool m_busy{ false };
};

int main(int argc, char *argv[])
{
    // Register custom URL scheme needed for authentication
    QWebEngineUrlScheme scheme("vlva"_ba);
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    scheme.setDefaultPort(QWebEngineUrlScheme::PortUnspecified);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme);
    QWebEngineUrlScheme::registerScheme(scheme);

    // Enable OpenGL shared contexts
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // Set OpenGL profile needed for VTK
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

    // Replace VTK's global output window with one that forwards to qWarning,
    // bypassing the Windows-only vtkWin32OutputWindow popup (whose own
    // DisplayText override ignores SetDisplayMode, so the more obvious fix
    // doesn't work — see VTKToQtOutputWindow above). Must run BEFORE any
    // VTK code can lazily build the default instance via the factory.
    {
        vtkNew<VTKToQtOutputWindow> qtOut;
        vtkOutputWindow::SetInstance(qtOut);
    }

    QApplication app(argc, argv);

    // Build the diagnostics singleton here, on the main thread, rather than
    // wherever the first diagnostic happens to be published. It is a
    // function-local static parented to qApp, so whichever thread touches it
    // first is the thread it is constructed on — and the first publisher is
    // often a worker (the backend health check runs in QtConcurrent and reports
    // a failure). Qt then refuses to parent it across threads and says so:
    //
    //   QObject: Cannot create children for a parent that is in a different thread.
    //
    // Measured: the object's thread affinity ends up correct either way, so
    // nothing malfunctions — the parent is simply never set, and a singleton
    // that lives for the whole process does not need one. What it costs is the
    // warning itself, printed at exactly the moment something has gone wrong
    // and someone is reading the log for a reason.
    DiagnosticsManager::instance();

    // Before anything can block: a Finder double-click delivers its FileOpen
    // event as soon as the app object exists, and the startup dialog below runs
    // for as long as the backend takes to come up.
    DesktopOpenRelay openRelay;
    app.installEventFilter(&openRelay);
    // Scrolling a settings page must not edit the controls under the cursor.
    ScrollWheelGuard scrollWheelGuard;
    app.installEventFilter(&scrollWheelGuard);

    // A second launch that carries files (every Linux / Windows double-click
    // starts a new process) hands them to the instance already running and
    // exits, so the file opens in the window the user is looking at instead of
    // starting a second app and a second backend session. A launch with NO
    // files always starts its own instance — running two VisIVOs against two
    // backends stays possible.
    // A remote backend chosen for this run (--backend). With none, everything
    // below is the local-backend start-up it has always been.
    std::optional<Settings::SessionBackend> remoteBackend;
    {
        QString error;
        if (!LaunchOptions::remoteBackend(QCoreApplication::arguments(),
                                          QProcessEnvironment::systemEnvironment(),
                                          &remoteBackend, &error)) {
            QMessageBox::critical(nullptr, u"VisIVO Visual Analytics"_s, error);
            return 2;
        }
    }
    if (remoteBackend) {
        Settings::setSessionBackend(*remoteBackend);
        BackendClient::setNoLocalBackend(true);
        qInfo().noquote() << "Remote backend for this run:" << remoteBackend->name
                          << remoteBackend->url;
    }
    // Or a cluster to connect to (--connect-cluster NAME): the backend job is
    // submitted below, once the theme is in place for the progress screen.
    QString clusterName;
    {
        QString error;
        if (!LaunchOptions::clusterToConnect(QCoreApplication::arguments(),
                                             QProcessEnvironment::systemEnvironment(),
                                             &clusterName, &error)) {
            QMessageBox::critical(nullptr, u"VisIVO Visual Analytics"_s, error);
            return 2;
        }
    }

    openRelay.takeCommandLine(QCoreApplication::arguments());
    if (!openRelay.becomePrimaryOrForward(!remoteBackend && clusterName.isEmpty()))
        return 0;   // the files went to the instance already running

    // Set locale to C.UTF-8
    std::setlocale(LC_NUMERIC, "C.UTF-8");
    QLocale::setDefault(QLocale::c());

    // Give this desktop instance a private backend session so several desktops
    // sharing one backend don't see each other's datasets (or copilot config).
    // A fresh id per launch is enough for isolation; the backend evicts it on idle.
    BackendClient::setDefaultSessionId(QUuid::createUuid().toString(QUuid::WithoutBraces));

    // Set application info
    QApplication::setApplicationName(u"VisIVO Visual Analytics"_s);
    QApplication::setApplicationVersion(QStringLiteral(VISIVO_VERSION_STR));
#ifndef Q_OS_MACOS
    // NOT on macOS. There the Dock icon comes from the bundle's .icns, which is
    // drawn with Apple's rounded-rectangle mask; calling setWindowIcon replaces
    // it at runtime with this flat square PNG, so the icon visibly changes shape
    // the moment the application finishes launching (reported as "rounded while
    // loading, square once open"). macOS windows carry no icon of their own, so
    // there is nothing else this call would buy us there.
    QApplication::setWindowIcon(QIcon(":/icons/VisIVO_512.png"));
#endif
    QApplication::setOrganizationName(u"Osservatorio Astrofisico di Catania"_s);
    QApplication::setOrganizationDomain(u"it.inaf.oact"_s);

    // Apply theme — must run after QApplication is constructed but before any
    // window is created so palette + stylesheet are inherited by all widgets.
    // Read the persisted scheme directly from QSettings to avoid the full
    // Settings object (which spawns a python process to locate the interpreter).
    {
        const QString workDir  = QDir::home().absoluteFilePath(app.applicationName());
        const QString iniPath  = QDir(workDir).absoluteFilePath(u"Settings.ini"_s);
        QSettings rawSettings(iniPath, QSettings::IniFormat);
        const auto storedInt   = rawSettings.value(u"UI/theme"_s,
                                                     static_cast<int>(Qt::ColorScheme::Unknown))
                                         .toInt();
        const auto scheme      = static_cast<Qt::ColorScheme>(storedInt);
        ThemeManager::instance().apply(scheme, &app);
        // The local backend's URL, before the launcher builds its first
        // clients: only that backend gets the ~/.visivo_token token.
        BackendClient::setLocalBackendUrl(
                rawSettings.value(u"Backend/url"_s, u"http://127.0.0.1:8000"_s).toString().trimmed());
    }

    // ── A window on a cluster (--connect-cluster NAME) ────────────────────────
    // This window submits the backend job, keeps the tunnel open and owns both:
    // they are closed and cancelled when it quits. From then on the run is a
    // remote one, exactly as with --backend.
    std::unique_ptr<ClusterConnection> cluster;
    if (!clusterName.isEmpty()) {
        QSettings rawSettings(QDir(QDir::home().absoluteFilePath(app.applicationName()))
                                      .absoluteFilePath(u"Settings.ini"_s),
                              QSettings::IniFormat);
        const ClusterProfile profile = ClusterProfiles::find(rawSettings, clusterName);
        if (!profile.isValid()) {
            QMessageBox::critical(nullptr, u"VisIVO Visual Analytics"_s,
                                  u"There is no cluster profile named \"%1\". Create it from the "
                                  u"Data Hub: Connect to a cluster…"_s.arg(clusterName));
            return 2;
        }
        // The tunnel never takes the port of this machine's own local backend.
        const int localPort = BackendUrl::portOf(
                rawSettings.value(u"Backend/url"_s, u"http://127.0.0.1:8000"_s).toString().trimmed(),
                8000);
        cluster = std::make_unique<ClusterConnection>(profile, localPort);
        ClusterConnectDialog connecting(cluster.get());
        if (connecting.exec() != QDialog::Accepted)
            return 0;   // cancelled or failed; the connection has cancelled its job
        remoteBackend = Settings::SessionBackend{ cluster->url(), cluster->token(), profile.name,
                                                  /*ownsClusterJob=*/true };
        Settings::setSessionBackend(*remoteBackend);
        BackendClient::setNoLocalBackend(true);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, cluster.get(), &ClusterConnection::stop);
        QObject::connect(cluster.get(), &ClusterConnection::tunnelLost, &app, [name = profile.name]() {
            qWarning().noquote() << "The tunnel to" << name << "dropped; reopening it.";
        });
        qInfo().noquote() << "Connected to cluster" << profile.name << "job" << cluster->jobId();
    }

    // Customize QDebug message format
    qSetMessagePattern("%{time} "
                       "%{if-debug}D %{endif}"
                       "%{if-info}I %{endif}"
                       "%{if-warning}W %{endif}"
                       "%{if-critical}C %{endif}"
                       "%{if-fatal}F %{endif}"
#ifndef NDEBUG
                       "%{file} %{function}:%{line} "
#endif
                       "%{if-category}%{category} %{endif}%{message}");

    // ── Backend auto-start suppression ────────────────────────────────────────
    // Honour either --no-backend-autostart CLI flag or VISIVO_NO_BACKEND_AUTOSTART
    // env var so that developers / CI / users with an external backend can skip
    // the launch step and connect to an already-running instance.
    const QStringList cliArgs = QCoreApplication::arguments();
    // A remote backend is never started here.
    const bool noAutostart =
            cliArgs.contains(u"--no-backend-autostart"_s)
            || !qEnvironmentVariable("VISIVO_NO_BACKEND_AUTOSTART").isEmpty()
            || remoteBackend.has_value();

    // ── Backend launcher (lives for the full application lifetime) ────────────
    // Owned here in main() so the backend process is terminated cleanly when
    // the application exits.
    BackendLauncher launcher;
    if (remoteBackend)
        launcher.setRemoteBackendName(remoteBackend->name);

    // ── Startup dialog ────────────────────────────────────────────────────────
    // Drives the backend + auth sequence before the main window is shown.
    // Creates MainWindow only after everything is ready.
    StartupDialog startup(&launcher, noAutostart);
    if (startup.exec() != QDialog::Accepted) {
        // User clicked Quit or startup failed unrecoverably.
        return 0;
    }

    // ── Main window ───────────────────────────────────────────────────────────
    MainWindow w;
    w.show();

    // Whatever the desktop asked for while we were starting up — and everything
    // it asks for from now on. (The command line was taken before the startup
    // dialog, so a second instance could forward instead of booting.)
    openRelay.attach(&w);

    // A window opened on a cluster from another window's Recent Datasets
    // (--reopen-recent ID): reopen that entry here, on this backend.
    if (const QString recentId = LaunchOptions::recentToReopen(QCoreApplication::arguments());
        !recentId.isEmpty() && remoteBackend) {
        QTimer::singleShot(0, &w, [&w, recentId]() {
            if (!w.recentDatasetsManager())
                return;
            const RecentDataset entry =
                    w.recentDatasetsManager()->find(recentId, Settings::sessionBackend()->name);
            if (entry.id.isEmpty()) {
                QMessageBox::information(&w, u"Reopen"_s,
                                         u"%1 is no longer in Recent Datasets: open it with Open…"_s
                                                 .arg(recentId));
                return;
            }
            w.reopenDataset(entry);
        });
    }

    return QApplication::exec();
}
