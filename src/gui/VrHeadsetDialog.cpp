#include "VrHeadsetDialog.h"

#include "WindowCloseShortcut.h"
#include "theme/ToolDialogStyle.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QCloseEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {

// The first `adb devices` of a session starts adb's server, which takes a few
// seconds; a Quest waking up to answer `am start` can take as long.
constexpr int kDevicesTimeoutMs = 15000;
constexpr int kCommandTimeoutMs = 15000;
// Often enough that accepting the USB-debugging prompt in the headset shows up
// here without pressing Refresh, rarely enough to cost nothing.
constexpr int kPollIntervalMs = 3000;

const QString kGuideBase = u"https://visivo.readthedocs.io/en/latest/user-guide/vr-headset.html"_s;
const QString kGuideUrl = kGuideBase + u"#open-a-cube-from-the-desktop"_s;
// The headset used last time, selected first when it is there again.
const QString kLastSerialKey = u"vrHeadset/lastSerial"_s;
// A USB probe answer stays good this long (the dialog checks every 3 s).
constexpr qint64 kUsbProbeMaxAgeMs = 6000;

QString guideLink(const QString &text, const QString &anchor = u"open-a-cube-from-the-desktop"_s)
{
    return u"<a href=\"%1#%2\">%3</a>"_s.arg(kGuideBase, anchor, text);
}

}   // namespace

VrHeadsetDialog::VrHeadsetDialog(const VrLink::Inputs &inputs, QWidget *parent)
    : QDialog(parent), m_link(VrLink::build(inputs)), m_secrets{ inputs.token, inputs.sessionId }
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(u"Open in VR Headset"_s);
    visivo::installCloseWindowShortcut(this);

    auto *root = new QVBoxLayout(this);
    root->addWidget(ToolDialogStyle::makeBreadcrumb(this, u"Open in VR Headset"_s));

    auto *intro = new QLabel(
            u"Opens this cube in the browser of a Meta Quest connected to this computer "
            u"with adb (USB cable, or adb over Wi-Fi), in the same backend session. "
            u"Then press <i>Enter VR</i> on the page. %1"_s.arg(guideLink(u"How to set it up"_s)),
            this);
    intro->setWordWrap(true);
    intro->setOpenExternalLinks(true);
    root->addWidget(intro);

    m_stateLabel = new QLabel(this);
    m_stateLabel->setWordWrap(true);
    m_stateLabel->setOpenExternalLinks(true);
    m_stateLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    root->addWidget(m_stateLabel);

    auto *deviceRow = new QHBoxLayout;
    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->setToolTip(u"More than one device is connected: choose the headset."_s);
    m_deviceCombo->setVisible(false);
    deviceRow->addWidget(m_deviceCombo, 1);
    m_refreshButton = new QPushButton(u"Retry"_s, this);
    m_refreshButton->setToolTip(u"Look for adb and the connected headsets again (this window also "
                                u"checks by itself every few seconds)."_s);
    deviceRow->addStretch();
    deviceRow->addWidget(m_refreshButton);
    root->addLayout(deviceRow);

    m_resultLabel = new QLabel(this);
    // Plain text: it shows adb's own output, which must not be read as markup.
    m_resultLabel->setTextFormat(Qt::PlainText);
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_resultLabel);
    m_copyErrorButton = new QPushButton(u"Copy Error"_s, this);
    m_copyErrorButton->setToolTip(u"Copy the error and the state, without the token, for a support "
                                  u"request."_s);
    m_copyErrorButton->setVisible(false);
    auto *errorRow = new QHBoxLayout;
    errorRow->addStretch();
    errorRow->addWidget(m_copyErrorButton);
    root->addLayout(errorRow);

    auto *warning = new QLabel(
            u"The link carries this backend's access token: anyone who has it can use the "
            u"backend as you. Open or paste it only on your own headset."_s,
            this);
    warning->setWordWrap(true);
    warning->setStyleSheet(ToolDialogStyle::helperCss());
    root->addWidget(warning);

    auto *buttons = new QHBoxLayout;
    m_copyButton = new QPushButton(u"Copy Link"_s, this);
    m_copyButton->setToolTip(u"Copy the link, token included, to open it by hand: forward the "
                             u"port with adb reverse first."_s);
    m_openButton = new QPushButton(u"Open on Headset"_s, this);
    m_openButton->setDefault(true);
    auto *close = new QPushButton(u"Close"_s, this);
    buttons->addWidget(m_copyButton);
    buttons->addStretch();
    buttons->addWidget(close);
    buttons->addWidget(m_openButton);
    root->addLayout(buttons);

    connect(m_refreshButton, &QPushButton::clicked, this, &VrHeadsetDialog::refreshDevices);
    connect(m_openButton, &QPushButton::clicked, this, &VrHeadsetDialog::openOnHeadset);
    connect(m_copyButton, &QPushButton::clicked, this, &VrHeadsetDialog::copyLink);
    connect(m_copyErrorButton, &QPushButton::clicked, this, &VrHeadsetDialog::copyError);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_deviceCombo, &QComboBox::currentIndexChanged, this, [this]() { setBusy(false); });

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        // Also once a headset is ready: unplugged, it must stop saying Ready.
        // Never over a command still running (an open in progress included).
        if (!m_process && isVisible())
            refreshDevices(/*background=*/true);
    });

    resize(520, sizeHint().height());

    if (!m_link.ok) {
        // Nothing here can help: the headset could not reach this backend.
        m_stateLabel->setText(m_link.error.toHtmlEscaped());
        m_refreshButton->setEnabled(false);
        m_openButton->setEnabled(false);
        m_copyButton->setEnabled(false);
        return;
    }
    m_copyButton->setEnabled(true);
    refreshDevices();
    m_pollTimer->start();
}

VrHeadsetDialog::~VrHeadsetDialog()
{
    // A command still running dies with the dialog; its finished() must not
    // reach the half-destroyed dialog's callbacks.
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    if (m_usbProbe) {
        m_usbProbe->disconnect(this);
        m_usbProbe->kill();
        m_usbProbe->waitForFinished(1000);
    }
}

void VrHeadsetDialog::stopAll()
{
    m_closed = true;
    m_openQueued = false;
    m_pollTimer->stop();
}

void VrHeadsetDialog::closeEvent(QCloseEvent *event)
{
    stopAll();
    QDialog::closeEvent(event);
}

void VrHeadsetDialog::hideEvent(QHideEvent *event)
{
    // Esc and reject() hide the dialog without a close event; a spontaneous
    // hide is the window being minimised, which stops nothing.
    if (!event->spontaneous())
        stopAll();
    QDialog::hideEvent(event);
}

void VrHeadsetDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // Shown again after a hide that did not delete it (raised by the cube
    // window): start checking again instead of staying inert.
    if (m_closed && m_link.ok && !event->spontaneous()) {
        m_closed = false;
        if (!m_process)
            refreshDevices();
        m_pollTimer->start();
    }
}

QString VrHeadsetDialog::redacted(const QString &text) const
{
    return VrLink::redact(text, m_secrets);
}

QString VrHeadsetDialog::selectedSerial() const
{
    return m_deviceCombo->currentData().toString();
}

void VrHeadsetDialog::setBusy(bool busy)
{
    m_refreshButton->setEnabled(!busy && m_link.ok);
    m_openButton->setEnabled(!busy && m_link.ok && m_deviceReady
                             && !selectedSerial().isEmpty());
    m_deviceCombo->setEnabled(!busy);
}

void VrHeadsetDialog::setResult(const QString &text, bool error)
{
    m_resultLabel->setText(text);
    m_resultLabel->setStyleSheet(error ? u"color: #e5534b;"_s : QString());
    m_errorText = error ? text : QString();
    m_copyErrorButton->setVisible(error || m_stateProblem);
}

void VrHeadsetDialog::setGuidance(const AdbHeadset::Guidance &g, const QString &extraHtml)
{
    QString html = u"<b>%1</b>"_s.arg(g.headline.toHtmlEscaped());
    if (!extraHtml.isEmpty())
        html += u" "_s + extraHtml;
    if (!g.steps.isEmpty()) {
        html += u"<ol style=\"margin-left: -20px\">"_s;
        for (const QString &step : g.steps)
            html += u"<li>%1</li>"_s.arg(step.toHtmlEscaped());
        html += u"</ol>"_s;
    }
    if (g.problem)
        html += guideLink(u"Help: the guide's troubleshooting"_s, g.anchor);
    m_stateLabel->setText(html);
    m_stateText = g.headline + u"\n"_s + g.steps.join(u"\n"_s);
    // A problem state is itself something to copy for support.
    m_stateProblem = g.problem;
    m_copyErrorButton->setVisible(g.problem || !m_errorText.isEmpty());
}

void VrHeadsetDialog::probeUsb()
{
    if (AdbHeadset::currentPlatform() != AdbHeadset::Platform::MacOS || m_usbProbe || m_closed)
        return;
    if (m_usbProbedAt.isValid() && m_usbProbedAt.elapsed() < kUsbProbeMaxAgeMs)
        return;
    auto *probe = new QProcess(this);
    m_usbProbe = probe;
    auto *timer = new QTimer(probe);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, probe, [probe]() { probe->kill(); });
    connect(probe, &QProcess::finished, this, [this, probe](int exitCode, QProcess::ExitStatus st) {
        const QByteArray out = probe->readAllStandardOutput();
        probe->deleteLater();
        m_usbProbe.clear();
        m_usbProbedAt.start();
        const AdbHeadset::UsbQuest was = m_usbQuest;
        m_usbQuest = st == QProcess::NormalExit && exitCode == 0 ? AdbHeadset::usbQuestFromProfiler(out)
                                                                 : AdbHeadset::UsbQuest::Unknown;
        // Only while adb still lists nothing: otherwise adb's own state is the news.
        if (m_noDevice && was != m_usbQuest && !m_closed)
            setGuidance(AdbHeadset::guidanceFor(AdbHeadset::Status::NoDevice, m_usbQuest));
    });
    connect(probe, &QProcess::errorOccurred, this, [this, probe](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        probe->deleteLater();
        m_usbProbe.clear();
        m_usbProbedAt.start();
        m_usbQuest = AdbHeadset::UsbQuest::Unknown;
    });
    timer->start(5000);
    probe->start(AdbHeadset::usbProbeProgram(), AdbHeadset::usbProbeArgs());
}

void VrHeadsetDialog::copyError()
{
    QStringList lines;
    lines << u"Open in VR Headset: what the dialog says"_s;
    if (!m_errorText.isEmpty())
        lines << u"Error: "_s + m_errorText;
    lines << u"State: "_s + m_stateText;
    lines << u"adb: "_s + (m_adb.isEmpty() ? u"not found"_s : AdbHeadset::homeShortened(m_adb, QDir::homePath()));
    lines << u"Backend port: %1"_s.arg(m_link.port);
    if (AdbHeadset::currentPlatform() == AdbHeadset::Platform::MacOS)
        lines << u"Quest on USB: "_s
                        + (m_usbQuest == AdbHeadset::UsbQuest::Present ? u"yes"_s
                           : m_usbQuest == AdbHeadset::UsbQuest::Absent ? u"no"_s
                                                                        : u"unknown"_s);
    QGuiApplication::clipboard()->setText(redacted(lines.join(u'\n')));
    // Said once, however often the button is pressed.
    const QString note = u"\n(Copied, without the token.)"_s;
    if (!m_resultLabel->text().endsWith(note))
        m_resultLabel->setText(m_resultLabel->text() + note);
}

void VrHeadsetDialog::runAdb(const QStringList &args, int timeoutMs, Done done, bool background)
{
    // After Close nothing new starts: not the next step of an open, not a
    // queued open, not a check. The dialog is deleted only later.
    if (m_closed)
        return;
    // The arguments hold the link, token included: log them redacted.
    qInfo().noquote() << "VR headset: adb" << redacted(args.join(u' '));

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_process = process;
    m_backgroundRun = background;
    // The periodic check must not grey the buttons every few seconds: a click
    // landing then would be lost, and a disabled combo closes its open popup.
    if (!background)
        setBusy(true);

    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    // One answer per run, whichever comes first: the process ending, failing
    // to start, or the timeout.
    auto finish = [this, process, timer, done](Outcome outcome, int exitCode) {
        if (m_process != process)
            return;
        m_process.clear();
        const bool wasBackground = m_backgroundRun;
        m_backgroundRun = false;
        timer->stop();
        const QString output = QString::fromLocal8Bit(process->readAll());
        process->disconnect(this);
        // A process killed on timeout is still exiting: delete it once it has,
        // or QProcess's destructor blocks the GUI thread waiting for it.
        if (process->state() == QProcess::NotRunning)
            process->deleteLater();
        else
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
        if (!wasBackground)
            setBusy(false);
        done(outcome, exitCode, output);
        // An Open clicked during the check runs now, on the devices it found.
        if (wasBackground && m_openQueued && !m_process && !m_closed) {
            m_openQueued = false;
            startOpen(m_queuedSerial);
        }
    };
    connect(timer, &QTimer::timeout, this, [process, finish]() {
        process->kill();
        finish(Outcome::TimedOut, -1);
    });
    connect(process, &QProcess::finished, this, [finish](int exitCode, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? Outcome::Finished : Outcome::Crashed, exitCode);
    });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        // A crash or kill is followed by finished(); only a failed start is not.
        if (error == QProcess::FailedToStart)
            finish(Outcome::FailedToStart, -1);
    });
    timer->start(timeoutMs);
    process->start(m_adb, args);
}

QString VrHeadsetDialog::adbFailure(Outcome outcome)
{
    switch (outcome) {
    case Outcome::FailedToStart:
        return u"adb could not be started: check that it is installed and runs from a terminal."_s;
    case Outcome::Crashed:
        return u"adb stopped unexpectedly: try again, or run adb kill-server in a terminal."_s;
    case Outcome::TimedOut:
        return u"adb did not answer: reconnect the headset; if it persists, run adb kill-server "
               u"in a terminal."_s;
    case Outcome::Finished:
        break;
    }
    return QString();
}

void VrHeadsetDialog::refreshDevices(bool background)
{
    if (m_process)
        return;
    m_adb = AdbHeadset::findAdb(AdbHeadset::currentPlatform(), QDir::homePath(),
                                QProcessEnvironment::systemEnvironment());
    if (m_adb.isEmpty()) {
        m_deviceReady = false;
        m_noDevice = false;
        m_deviceCombo->clear();
        m_deviceCombo->setVisible(false);
        setGuidance(AdbHeadset::adbMissingGuidance(AdbHeadset::currentPlatform()));
        setBusy(false);
        return;
    }
    runAdb({ u"devices"_s, u"-l"_s }, kDevicesTimeoutMs,
           [this](Outcome outcome, int exitCode, const QString &output) {
               if (outcome != Outcome::Finished || exitCode != 0) {
                   m_deviceReady = false;
                   m_noDevice = false;
                   m_deviceCombo->clear();
                   m_deviceCombo->setVisible(false);
                   const QString why = outcome == Outcome::Finished
                           ? u"adb failed: %1"_s.arg(redacted(output.trimmed()))
                           : adbFailure(outcome);
                   AdbHeadset::Guidance g;
                   g.headline = why;
                   g.steps << u"Run  adb kill-server  in a terminal, then press Retry."_s;
                   g.anchor = u"troubleshooting"_s;
                   setGuidance(g);
                   m_errorText = why;
                   setBusy(false);
                   return;
               }
               showDevices(AdbHeadset::parseDevices(output));
           },
           background);
}

void VrHeadsetDialog::showDevices(const QList<AdbHeadset::Device> &devices)
{
    using AdbHeadset::Status;
    const Status status = AdbHeadset::statusOf(devices);
    const QList<AdbHeadset::Device> ready = AdbHeadset::readyDevices(devices);

    // Refill the list only when the devices changed, and never under the
    // user's open popup (the next check catches up): refilling it every few
    // seconds would close the popup and reset the highlight.
    QList<QPair<QString, QString>> items;   // label, serial
    for (const AdbHeadset::Device &d : ready)
        items.append({ d.model.isEmpty() ? d.serial : u"%1 (%2)"_s.arg(d.model, d.serial),
                       d.serial });
    bool same = items.size() == m_deviceCombo->count();
    for (int i = 0; same && i < items.size(); ++i)
        same = m_deviceCombo->itemText(i) == items.at(i).first
                && m_deviceCombo->itemData(i).toString() == items.at(i).second;
    if (!same && !m_deviceCombo->view()->isVisible()) {
        // Keep the user's choice when that device is still there.
        const QString previous = selectedSerial();
        m_deviceCombo->blockSignals(true);
        m_deviceCombo->clear();
        QStringList serials;
        for (const auto &item : items) {
            m_deviceCombo->addItem(item.first, item.second);
            serials << item.second;
        }
        // The user's choice when still there, else the headset used last time.
        int keep = m_deviceCombo->findData(previous);
        if (keep < 0)
            keep = AdbHeadset::preferredIndex(serials, QSettings().value(kLastSerialKey).toString());
        if (keep >= 0)
            m_deviceCombo->setCurrentIndex(keep);
        m_deviceCombo->blockSignals(false);
        m_deviceCombo->setVisible(ready.size() > 1);
    }
    m_deviceReady = status == Status::Ready;
    m_readySerials.clear();
    for (const AdbHeadset::Device &d : ready)
        m_readySerials << d.serial;

    m_noDevice = status == Status::NoDevice;
    if (m_noDevice)
        probeUsb();      // the answer updates the label when it comes
    if (status == Status::Ready) {
        const AdbHeadset::Device &d = ready.first();
        AdbHeadset::Guidance g = AdbHeadset::guidanceFor(Status::Ready);
        g.headline = ready.size() > 1 ? u"%1 devices ready: choose the headset."_s.arg(ready.size())
                                      : u"Ready: %1"_s.arg(d.model.isEmpty() ? d.serial : d.model);
        setGuidance(g);
    } else {
        setGuidance(AdbHeadset::guidanceFor(
                status, m_noDevice ? m_usbQuest : AdbHeadset::UsbQuest::Unknown));
    }
    setBusy(false);
}

void VrHeadsetDialog::openOnHeadset()
{
    if (m_process) {
        // Only the periodic check can be running while Open is enabled: wait
        // for it rather than drop the click.
        if (m_backgroundRun) {
            m_openQueued = true;
            m_queuedSerial = selectedSerial();
        }
        return;
    }
    startOpen(selectedSerial());
}

void VrHeadsetDialog::startOpen(const QString &serial)
{
    // The device the user chose, or none: if it went away during the check,
    // the list may now select another device (a phone, say), and opening
    // there would be wrong.
    if (serial.isEmpty() || !m_deviceReady || !m_readySerials.contains(serial)) {
        setResult(u"The chosen headset is not ready any more: see the state above, then press "
                  u"Open on Headset again."_s,
                  true);
        return;
    }
    m_lastOpenSerial = serial;
    setResult(u"Forwarding port %1 to the headset…"_s.arg(m_link.port), false);
    runAdb(AdbHeadset::reverseArgs(serial, m_link.port), kCommandTimeoutMs,
           [this, serial](Outcome outcome, int exitCode, const QString &output) {
               if (outcome != Outcome::Finished || exitCode != 0) {
                   setResult((outcome == Outcome::Finished
                                      ? u"adb reverse failed: %1"_s.arg(redacted(output.trimmed()))
                                      : adbFailure(outcome))
                                     + u" Press Retry, then Open on Headset again."_s,
                             true);
                   return;
               }
               // Open in the Quest Browser when it is there (another browser
               // may offer no WebXR); on a headset without it, let Android
               // choose rather than fail.
               runAdb(AdbHeadset::packageQueryArgs(serial, AdbHeadset::questBrowserPackage()),
                      kCommandTimeoutMs,
                      [this, serial](Outcome outcome, int exitCode, const QString &output) {
                          const bool haveBrowser = outcome == Outcome::Finished && exitCode == 0
                                  && AdbHeadset::packageListed(
                                          output, AdbHeadset::questBrowserPackage());
                          openPage(serial,
                                   haveBrowser ? AdbHeadset::questBrowserPackage() : QString());
                      });
           });
}

void VrHeadsetDialog::openPage(const QString &serial, const QString &package)
{
    setResult(package.isEmpty() ? u"Opening the page on the headset…"_s
                                : u"Opening the page in the Quest Browser…"_s,
              false);
    runAdb(AdbHeadset::viewArgs(serial, m_link.url, package), kCommandTimeoutMs,
           [this](Outcome outcome, int exitCode, const QString &output) {
               if (outcome == Outcome::TimedOut) {
                   setResult(u"The headset did not answer: wake it up and try again."_s, true);
                   return;
               }
               if (outcome != Outcome::Finished) {
                   setResult(adbFailure(outcome), true);
                   return;
               }
               if (!AdbHeadset::viewStarted(exitCode, output)) {
                   setResult(u"The headset could not open the page: %1"_s.arg(
                                     redacted(output.trimmed())),
                             true);
                   return;
               }
               QSettings().setValue(kLastSerialKey, m_lastOpenSerial);
               setResult(u"Opened on the headset. Put it on: the cube loads on the page, then "
                         u"press Enter VR. Repeat after unplugging the cable or restarting the "
                         u"headset (the port forwarding ends then)."_s,
                         false);
           });
}

void VrHeadsetDialog::copyLink()
{
    if (!m_link.ok)
        return;
    QGuiApplication::clipboard()->setText(m_link.url);
    setResult(u"Link copied, token included. On the headset it works only after "
              u"adb reverse tcp:%1 tcp:%1; paste it only into your own headset's browser."_s
                      .arg(m_link.port),
              false);
}
