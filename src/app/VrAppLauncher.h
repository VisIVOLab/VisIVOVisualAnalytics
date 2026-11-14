#ifndef VRAPPLAUNCHER_H
#define VRAPPLAUNCHER_H

// VrAppLauncher — installs and starts the VisIVO VR headset app over adb
// (issue #255), on the GUI thread's event loop, for "Open in VR Headset…".
//
// Open on Headset is one chain, each step asynchronous:
//   0. a page already connected (GET /v1/vr/headset) takes a `show`, and
//      nothing else is done;
//   1. installed and current? (`pm list packages --show-versioncode`), else
//      install (`adb install -r`, 120 s); INSTALL_FAILED_UPDATE_INCOMPATIBLE
//      asks the caller (uninstallRequested) before anything is uninstalled;
//   2. the backend lists the app (/.well-known/assetlinks.json of its origin);
//   3. `adb reverse tcp:P tcp:P`;
//   4. the headset is awake (`dumpsys power`), else "Put the headset on";
//   5. a one-time ticket (POST /v1/vr/launch);
//   6. asked again: a page that connected meanwhile takes a `show` instead
//      of a launch (a second launch would leave the user in a void);
//   7. ONE `am start` (VrHeadsetApp::LaunchGate), then
//   8. GET /v1/vr/launch/{launch} every few hundred ms until the page's
//      hello names the launch (In VR) or a timeout says why not.
//
// The ticket never reaches a log or a message: every adb line logged and
// every adb output shown goes through redact(). The token never goes in the
// intent. Nothing here touches a widget: tests/test_vr_headset_app.cpp runs
// the chain against a fake adb and a fake backend.

#include "VrHeadsetApp.h"
#include "VrLink.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QProcess;
class QTimer;
class BackendClient;
struct BackendVrHubReply;

class VrAppLauncher : public QObject
{
    Q_OBJECT

public:
    /// Where and what to launch.
    struct Target
    {
        QString adb;                  ///< the adb executable
        QString serial;               ///< the headset (`adb -s`)
        VrHeadsetApp::AppInfo app;    ///< the APK and its visivo-vr.json
        VrLink::Inputs inputs;        ///< backend URL, token, session, the cube
        VrLink::Link link;            ///< the port and path prefix the headset reaches
    };

    enum class Outcome {
        InVr,        ///< launched: the page's hello named the launch
        Shown,       ///< a page was already connected: it got a `show`
        Installed,   ///< install() alone: the app is installed and current
        Asleep,      ///< asleep, or the launch was not taken up in time
        NoConnect,   ///< started, but the page never connected
        NotListed,   ///< the backend does not list the app: nothing launched
        Declined,    ///< the user would not uninstall the other build
        Cancelled,   ///< cancel() (the dialog closed)
        Failed,      ///< anything else; the message says what
    };
    Q_ENUM(Outcome)

    explicit VrAppLauncher(QObject *parent = nullptr);
    ~VrAppLauncher() override;

    /// Tests shorten these; the defaults are the contract's.
    void setTimings(int exchangeTimeoutMs, int clientTimeoutMs, int pollIntervalMs);
    void setAdbTimeouts(int commandMs, int installMs);

    /// Something is running (a check, an install, an open).
    bool busy() const { return m_step != Step::Idle; }
    /// A launch is being followed: Open must wait.
    bool following() const { return m_step == Step::Following; }

    /// Is the app installed and current on the headset? Answers with checked().
    void check(const Target &target);
    /// Install or update the app only. Answers with finished(Installed) or a failure.
    void install(const Target &target);
    /// Open on Headset: the whole chain above. Answers with finished().
    void open(const Target &target);
    /// The answer to uninstallRequested(): true uninstalls the other build
    /// and installs this one, false ends with Declined.
    void answerUninstall(bool uninstall);
    /// Stop: a command running is killed, answers still out are dropped,
    /// finished(Cancelled) is NOT emitted (the caller is going away).
    void cancel();

    /// @p text with the token, the session id, every ticket of this launcher
    /// and any `ticket=` value taken out.
    QString redact(const QString &text) const;

signals:
    /// What is happening, for the dialog's result line.
    void progress(const QString &text);
    /// check(), and the first step of install()/open(): what is installed.
    void checked(VrHeadsetApp::Installed state, qint64 installedVersion, const QString &error);
    /// An app with this package signed by another key is installed: ask the
    /// user, then call answerUninstall().
    void uninstallRequested(const QString &package);
    /// The end of install() or open(). @p client: the page shown or launched.
    void finished(VrAppLauncher::Outcome outcome, const QString &message, const QString &client);

private:
    enum class Step {
        Idle,
        Checking,
        Installing,
        AwaitingAnswer,
        Uninstalling,
        Precheck,
        Reverse,
        Wake,
        Ticket,
        HubCheck,
        AmStart,
        Following,
        Showing,
    };
    enum class Mode { Check, Install, Open };
    enum class AdbEnd { Finished, TimedOut, FailedToStart, Crashed };
    using AdbDone = std::function<void(AdbEnd end, int exitCode, const QString &output)>;
    using HubDone = std::function<void(const BackendVrHubReply &reply)>;

    void start(Mode mode, const Target &target);
    void queryVersion();
    void runInstall();
    void precheck();
    void reverse();
    void checkWake();
    void requestTicket();
    void checkHub();
    void sendShow(const QString &client);
    void amStart();
    void poll();
    void endFollowing();
    void finish(Outcome outcome, const QString &message, const QString &client = {});

    void runAdb(const QStringList &args, int timeoutMs, AdbDone done);
    /// @p work on a copy of the backend client off the GUI thread; @p done
    /// on the GUI thread unless cancel() or a newer start came meanwhile.
    void runBackend(std::function<BackendVrHubReply(const BackendClient &)> work, HubDone done);
    QString adbFailure(AdbEnd end, const QString &output) const;
    qint64 now() const { return m_clock.elapsed(); }

    Step m_step{ Step::Idle };
    Mode m_mode{ Mode::Check };
    Target m_target;
    quint64 m_generation{ 0 };     ///< bumped by start and cancel: older answers are dropped
    bool m_uninstalledOnce{ false };
    VrHeadsetApp::Ticket m_ticket;
    QStringList m_tickets;         ///< every ticket seen, for redact()
    VrHeadsetApp::LaunchGate m_gate;
    VrHeadsetApp::LaunchTracker m_tracker;
    bool m_pollOut{ false };
    QElapsedTimer m_clock;

    int m_exchangeTimeoutMs{ int(VrHeadsetApp::LaunchTracker::kExchangeTimeoutMs) };
    int m_clientTimeoutMs{ int(VrHeadsetApp::LaunchTracker::kClientTimeoutMs) };
    int m_pollIntervalMs{ 300 };
    int m_commandTimeoutMs{ 15000 };
    int m_installTimeoutMs{ 120000 };

    QPointer<QProcess> m_process;
    QTimer *m_pollTimer{ nullptr };
    QNetworkAccessManager *m_nam{ nullptr };
};

#endif // VRAPPLAUNCHER_H
