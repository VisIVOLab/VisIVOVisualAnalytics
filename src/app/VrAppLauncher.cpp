// VrAppLauncher.cpp — see VrAppLauncher.h (issue #255).

#include "VrAppLauncher.h"

#include "BackendClient.h"
#include "VrHeadsetHub.h"

#include <QDebug>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

namespace {
// The backend's own assetlinks route answers at once; a tunnel to a cluster
// in well under a second.
constexpr int kAssetlinksTimeoutMs = 8000;
}   // namespace

VrAppLauncher::VrAppLauncher(QObject *parent) : QObject(parent)
{
    m_clock.start();
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(m_pollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &VrAppLauncher::poll);
}

VrAppLauncher::~VrAppLauncher()
{
    cancel();
}

void VrAppLauncher::setTimings(int exchangeTimeoutMs, int clientTimeoutMs, int pollIntervalMs)
{
    m_exchangeTimeoutMs = exchangeTimeoutMs;
    m_clientTimeoutMs = clientTimeoutMs;
    m_pollIntervalMs = pollIntervalMs;
    m_pollTimer->setInterval(pollIntervalMs);
}

void VrAppLauncher::setAdbTimeouts(int commandMs, int installMs)
{
    m_commandTimeoutMs = commandMs;
    m_installTimeoutMs = installMs;
}

QString VrAppLauncher::redact(const QString &text) const
{
    QStringList secrets{ m_target.inputs.token, m_target.inputs.sessionId };
    secrets << m_tickets;
    return VrHeadsetApp::redactTicket(VrLink::redact(text, secrets));
}

// ── The chain ───────────────────────────────────────────────────────────────

void VrAppLauncher::check(const Target &target)
{
    start(Mode::Check, target);
}

void VrAppLauncher::install(const Target &target)
{
    start(Mode::Install, target);
}

void VrAppLauncher::open(const Target &target)
{
    start(Mode::Open, target);
}

void VrAppLauncher::start(Mode mode, const Target &target)
{
    // One thing at a time: a click that lands while a launch is followed is
    // not a second launch.
    if (busy())
        return;
    if (!target.app.ok) {
        // Answered later, like every other end: the caller may still be
        // wiring itself up when it calls.
        QTimer::singleShot(0, this, [this, error = target.app.error]() {
            emit finished(Outcome::Failed, error, {});
        });
        return;
    }
    ++m_generation;
    m_mode = mode;
    m_target = target;
    m_uninstalledOnce = false;
    m_ticket = VrHeadsetApp::Ticket();
    m_tracker = VrHeadsetApp::LaunchTracker(m_exchangeTimeoutMs, m_clientTimeoutMs);
    // A page already connected takes the cube with no install, no wake check
    // and no ticket.
    if (mode == Mode::Open)
        checkHub();
    else
        queryVersion();
}

void VrAppLauncher::queryVersion()
{
    m_step = Step::Checking;
    const QString package = m_target.app.package;
    // The mode and generation of THIS run: a receiver of checked() may start
    // another run (a check alone is idle by then), whose state must not steer
    // this callback.
    const Mode mode = m_mode;
    const quint64 generation = m_generation;
    const qint64 wanted = m_target.app.versionCode;
    runAdb(VrHeadsetApp::versionQueryArgs(m_target.serial, package), m_commandTimeoutMs,
           [this, package, mode, generation, wanted](AdbEnd end, int exitCode, const QString &output) {
               // A check alone ends here: idle before it is announced, so
               // the receiver may start the next thing at once.
               if (mode == Mode::Check)
                   m_step = Step::Idle;
               if (end != AdbEnd::Finished || exitCode != 0) {
                   const QString why = adbFailure(end, output);
                   emit checked(VrHeadsetApp::Installed::Unknown, -1, why);
                   if (mode != Mode::Check && generation == m_generation)
                       finish(Outcome::Failed, why);
                   return;
               }
               const qint64 installed = VrHeadsetApp::installedVersion(output, package);
               const VrHeadsetApp::Installed state = VrHeadsetApp::installedState(installed, wanted);
               emit checked(state, installed, {});
               // Cancelled (or anything else started) by the receiver.
               if (mode == Mode::Check || generation != m_generation)
                   return;
               if (state == VrHeadsetApp::Installed::Current) {
                   if (m_mode == Mode::Install)
                       finish(Outcome::Installed, u"VisIVO VR is installed and current."_s);
                   else
                       precheck();
                   return;
               }
               runInstall();
           });
}

void VrAppLauncher::runInstall()
{
    m_step = Step::Installing;
    emit progress(u"Installing VisIVO VR on the headset…"_s);
    runAdb(VrHeadsetApp::installArgs(m_target.serial, m_target.app.apkPath), m_installTimeoutMs,
           [this](AdbEnd end, int exitCode, const QString &output) {
               if (end != AdbEnd::Finished) {
                   finish(Outcome::Failed, u"Installing VisIVO VR failed: "_s + adbFailure(end, output));
                   return;
               }
               const VrHeadsetApp::InstallResult r = VrHeadsetApp::parseInstall(exitCode, output);
               using O = VrHeadsetApp::InstallResult::Outcome;
               if (r.outcome == O::UpdateIncompatible && !m_uninstalledOnce) {
                   // Someone's own build, signed by another key: never
                   // uninstalled without asking (it may hold their settings).
                   m_step = Step::AwaitingAnswer;
                   emit uninstallRequested(m_target.app.package);
                   return;
               }
               if (r.outcome != O::Success) {
                   finish(Outcome::Failed,
                          u"Installing VisIVO VR failed: %1."_s.arg(redact(r.message)));
                   return;
               }
               const quint64 generation = m_generation;
               emit checked(VrHeadsetApp::Installed::Current, m_target.app.versionCode, {});
               if (generation != m_generation)
                   return;   // the receiver cancelled
               if (m_mode == Mode::Install)
                   finish(Outcome::Installed, u"VisIVO VR %1 is installed."_s.arg(m_target.app.versionName));
               else
                   precheck();
           });
}

void VrAppLauncher::answerUninstall(bool uninstall)
{
    if (m_step != Step::AwaitingAnswer)
        return;
    if (!uninstall) {
        finish(Outcome::Declined,
               u"VisIVO VR was not installed: another build of it is on the headset. Uninstall "
               u"it there, or open the page in the browser."_s);
        return;
    }
    m_step = Step::Uninstalling;
    m_uninstalledOnce = true;
    emit progress(u"Uninstalling the other build of VisIVO VR…"_s);
    runAdb(VrHeadsetApp::uninstallArgs(m_target.serial, m_target.app.package), m_commandTimeoutMs,
           [this](AdbEnd end, int exitCode, const QString &output) {
               if (end != AdbEnd::Finished || !VrHeadsetApp::uninstalled(exitCode, output)) {
                   finish(Outcome::Failed, u"The other build could not be uninstalled: "_s
                                                   + adbFailure(end, output));
                   return;
               }
               runInstall();
           });
}

void VrAppLauncher::precheck()
{
    m_step = Step::Precheck;
    emit progress(u"Checking that this backend lists VisIVO VR…"_s);
    const QString url = VrHeadsetApp::assetlinksUrl(m_target.inputs.baseUrl);
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    QNetworkRequest request{ QUrl(url) };
    request.setTransferTimeout(kAssetlinksTimeoutMs);
    // The headset's browser would not follow a redirect to another origin either.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = m_nam->get(request);
    const quint64 generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->read(256 * 1024);
        switch (VrHeadsetApp::parseAssetlinks(status, body, m_target.app.package, m_target.app.certSha256)) {
        case VrHeadsetApp::Listing::Listed:
            reverse();
            return;
        case VrHeadsetApp::Listing::NotListed:
            finish(Outcome::NotListed, VrHeadsetApp::notListedMessage(m_target.app));
            return;
        case VrHeadsetApp::Listing::Unreadable:
            finish(Outcome::Failed,
                   status == 0 ? u"The backend did not answer the app check: %1"_s.arg(reply->errorString())
                               : u"The backend's app check could not be read (HTTP %1)."_s.arg(status));
            return;
        }
    });
}

void VrAppLauncher::reverse()
{
    m_step = Step::Reverse;
    runAdb(AdbHeadset::reverseArgs(m_target.serial, m_target.link.port), m_commandTimeoutMs,
           [this](AdbEnd end, int exitCode, const QString &output) {
               if (end != AdbEnd::Finished || exitCode != 0) {
                   finish(Outcome::Failed, u"adb reverse failed: "_s + adbFailure(end, output));
                   return;
               }
               checkWake();
           });
}

void VrAppLauncher::checkWake()
{
    m_step = Step::Wake;
    runAdb(VrHeadsetApp::wakefulnessArgs(m_target.serial), m_commandTimeoutMs,
           [this](AdbEnd end, int, const QString &output) {
               // grep finding nothing exits 1: Unknown, launched anyway.
               if (end != AdbEnd::Finished) {
                   finish(Outcome::Failed, adbFailure(end, output));
                   return;
               }
               if (VrHeadsetApp::parseWakefulness(output) == VrHeadsetApp::Wake::NotAwake) {
                   finish(Outcome::Asleep, VrHeadsetApp::putHeadsetOnMessage());
                   return;
               }
               requestTicket();
           });
}

void VrAppLauncher::requestTicket()
{
    m_step = Step::Ticket;
    const QJsonObject body = VrHeadsetApp::launchRequestBody(m_target.inputs);
    runBackend([body](const BackendClient &c) { return c.createVrLaunch(body); },
               [this](const BackendVrHubReply &reply) {
                   VrHeadsetApp::Ticket t = VrHeadsetApp::parseTicket(reply.httpStatus, reply.body, reply.error);
                   if (!t.ok) {
                       finish(Outcome::Failed, redact(t.error));
                       return;
                   }
                   m_ticket = t;
                   m_tickets << t.ticket;
                   checkHub();
               });
}

void VrAppLauncher::checkHub()
{
    // First of all, and again right before the launch (a ticket in hand), so
    // a page that connected meanwhile takes the cube instead.
    m_step = Step::HubCheck;
    runBackend([](const BackendClient &c) { return c.vrHeadsets(); },
               [this](const BackendVrHubReply &reply) {
                   const VrHub::Presence p = VrHub::parsePresence(reply.httpStatus, reply.body, reply.error);
                   // Unanswered: launched anyway (the hub may be an old backend's).
                   const VrHeadsetApp::OpenDecision d =
                           VrHeadsetApp::decideOpen(false, true, p.ok ? p.headsets : QList<VrHub::Headset>());
                   if (d.action == VrHeadsetApp::OpenAction::Show)
                       sendShow(d.client);
                   else if (m_ticket.ok)
                       amStart();
                   else
                       queryVersion();
               });
}

void VrAppLauncher::sendShow(const QString &client)
{
    m_step = Step::Showing;
    emit progress(u"VisIVO VR is already running: sending the cube to it…"_s);
    const QJsonObject body = VrHub::showCommandBody(m_target.inputs, client);
    runBackend([body](const BackendClient &c) { return c.sendVrHeadsetCommand(body); },
               [this, client](const BackendVrHubReply &reply) {
                   const VrHub::CommandResult r = VrHub::interpretCommand(reply.httpStatus, reply.body, reply.error);
                   using O = VrHub::CommandResult::Outcome;
                   if (r.outcome == O::Sent) {
                       finish(Outcome::Shown, u"Sent: the headset shows this cube now."_s,
                              r.client.isEmpty() ? client : r.client);
                   } else if (r.outcome == O::NoHeadset) {
                       // The page left between the check and the show: no
                       // page, so the launch goes on (and is the one launch).
                       if (m_ticket.ok)
                           amStart();
                       else
                           queryVersion();
                   } else {
                       finish(Outcome::Failed, r.message);
                   }
               });
}

void VrAppLauncher::amStart()
{
    m_step = Step::AmStart;
    if (!m_gate.claim(m_ticket.launch)) {
        finish(Outcome::Failed, u"This launch was already sent: press Open again."_s);
        return;
    }
    emit progress(u"Starting on the headset…"_s);
    const QString url = VrHeadsetApp::launchUrl(m_target.link.port, m_target.link.prefix, m_ticket.page,
                                                m_ticket.ticket);
    runAdb(VrHeadsetApp::amStartArgs(m_target.serial, m_target.app.package, url), m_commandTimeoutMs,
           [this](AdbEnd end, int exitCode, const QString &output) {
               if (end == AdbEnd::FailedToStart) {
                   finish(Outcome::Failed, adbFailure(end, output));
                   return;
               }
               if (end == AdbEnd::Finished && !AdbHeadset::viewStarted(exitCode, output)) {
                   finish(Outcome::Failed,
                          u"The headset could not start VisIVO VR: %1"_s.arg(redact(output.trimmed().left(300))));
                   return;
               }
               // Started, or adb gave up waiting (the intent may still have
               // gone): followed either way, and never sent again.
               m_step = Step::Following;
               m_pollOut = false;
               // "Starting on the headset…" is already said (before the
               // command): the next words are the outcome.
               m_tracker.begin(m_ticket.launch, now());
               m_pollTimer->start();
               poll();
           });
}

void VrAppLauncher::poll()
{
    if (m_step != Step::Following)
        return;
    m_tracker.tick(now());
    if (!m_tracker.active()) {
        endFollowing();
        return;
    }
    if (m_pollOut)
        return;
    m_pollOut = true;
    const QString launch = m_ticket.launch;
    runBackend([launch](const BackendClient &c) { return c.vrLaunchStatus(launch); },
               [this](const BackendVrHubReply &reply) {
                   m_pollOut = false;
                   m_tracker.polled(VrHeadsetApp::parseLaunchStatus(reply.httpStatus, reply.body, reply.error),
                                    now());
                   if (!m_tracker.active())
                       endFollowing();
               });
}

void VrAppLauncher::endFollowing()
{
    m_pollTimer->stop();
    using P = VrHeadsetApp::LaunchTracker::Phase;
    switch (m_tracker.phase()) {
    case P::InVr:
        finish(Outcome::InVr, m_tracker.message(), m_tracker.client());
        return;
    case P::Asleep:
        finish(Outcome::Asleep, m_tracker.message());
        return;
    case P::NoConnect:
        finish(Outcome::NoConnect, redact(m_tracker.message()));
        return;
    default:
        finish(Outcome::Failed, m_tracker.message());
        return;
    }
}

void VrAppLauncher::finish(Outcome outcome, const QString &message, const QString &client)
{
    m_step = Step::Idle;
    m_pollTimer->stop();
    m_pollOut = false;
    ++m_generation;   // nothing still out may act after the end
    emit finished(outcome, message, client);
}

void VrAppLauncher::cancel()
{
    ++m_generation;
    m_step = Step::Idle;
    m_pollOut = false;
    if (m_pollTimer)
        m_pollTimer->stop();
    if (QProcess *p = m_process.data()) {
        m_process.clear();
        p->disconnect(this);
        p->kill();
        // Deleted once it has exited, so the GUI thread never waits for it.
        connect(p, &QProcess::finished, p, &QObject::deleteLater);
    }
}

// ── Running things ──────────────────────────────────────────────────────────

void VrAppLauncher::runAdb(const QStringList &args, int timeoutMs, AdbDone done)
{
    // The arguments may hold the launch address, ticket included.
    qInfo().noquote() << "VR headset app: adb" << redact(args.join(u' '));
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_process = process;
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    const quint64 generation = m_generation;
    auto end = [this, process, timer, done, generation](AdbEnd how, int exitCode) {
        if (m_process != process)
            return;
        m_process.clear();
        timer->stop();
        const QString output = QString::fromLocal8Bit(process->readAll());
        process->disconnect(this);
        if (process->state() == QProcess::NotRunning)
            process->deleteLater();
        else
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
        if (generation != m_generation)
            return;
        done(how, exitCode, output);
    };
    connect(timer, &QTimer::timeout, this, [process, end]() {
        process->kill();
        end(AdbEnd::TimedOut, -1);
    });
    connect(process, &QProcess::finished, this, [end](int exitCode, QProcess::ExitStatus status) {
        end(status == QProcess::NormalExit ? AdbEnd::Finished : AdbEnd::Crashed, exitCode);
    });
    connect(process, &QProcess::errorOccurred, this, [end](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            end(AdbEnd::FailedToStart, -1);
    });
    timer->start(timeoutMs);
    process->start(m_target.adb, args);
}

void VrAppLauncher::runBackend(std::function<BackendVrHubReply(const BackendClient &)> work, HubDone done)
{
    BackendClient client(m_target.inputs.baseUrl, m_target.inputs.token);
    client.setSessionId(m_target.inputs.sessionId);
    const quint64 generation = m_generation;
    QPointer<VrAppLauncher> self(this);
    // Not parented: deleted when its answer comes, even after the launcher.
    auto *watcher = new QFutureWatcher<BackendVrHubReply>;
    connect(watcher, &QFutureWatcher<BackendVrHubReply>::finished, watcher,
            [self, watcher, generation, done]() {
                const BackendVrHubReply reply = watcher->result();
                watcher->deleteLater();
                if (!self || generation != self->m_generation)
                    return;
                done(reply);
            });
    watcher->setFuture(QtConcurrent::run([client, work]() { return work(client); }));
}

QString VrAppLauncher::adbFailure(AdbEnd end, const QString &output) const
{
    switch (end) {
    case AdbEnd::FailedToStart:
        return u"adb could not be started: check that it is installed and runs from a terminal."_s;
    case AdbEnd::Crashed:
        return u"adb stopped unexpectedly: try again, or run adb kill-server in a terminal."_s;
    case AdbEnd::TimedOut:
        return u"The headset did not answer: wake it up and try again."_s;
    case AdbEnd::Finished:
        break;
    }
    const QString text = redact(output.trimmed().left(300));
    return text.isEmpty() ? u"adb failed."_s : text;
}
