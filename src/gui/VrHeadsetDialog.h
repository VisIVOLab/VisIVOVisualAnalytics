#pragma once

#include "app/AdbHeadset.h"
#include "app/VrLink.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>

#include <functional>

class QComboBox;
class QNetworkAccessManager;
class QLabel;
class QProcess;
class QPushButton;
class QCloseEvent;
class QHideEvent;
class QShowEvent;
class QTimer;

/**
 * Tools ▸ Views ▸ Open in VR Headset… (issue #172).
 *
 * Opens the backend's WebXR page (/vr/) on a Meta Quest attached to this
 * computer with adb, over USB or adb over Wi-Fi, carrying the desktop's token,
 * backend session, dataset id and cube path in the link's fragment, so the
 * headset continues on the same cube in the same session.
 *
 * "Open on Headset" runs `adb reverse tcp:P tcp:P` (the headset's localhost:P
 * becomes this computer's, so the page is a secure context for WebXR without a
 * certificate), then `am start` with a VIEW intent for the Quest Browser.
 * "Copy Link" is for doing the same by hand.
 *
 * A remote backend (a cluster's) works the same way when this computer
 * reaches it through a local tunnel: adb reverse forwards the headset's
 * localhost:P to the tunnel's local port P (issue #187). The dialog says
 * which backend it pairs with, and asks it (/v1/vr/backend_check) whether it serves
 * the page and routes this desktop expects.
 *
 * The link holds the token. It is never logged: every log line and every
 * line of adb output shown here goes through VrLink::redact first. It still
 * reaches the command line of the adb process here and of `am` on the
 * headset; the guide says so.
 *
 * Non-modal, deleted on close. Every adb run is asynchronous with a timeout,
 * and a QProcess parented to the dialog, so closing the dialog mid-command
 * kills the command instead of leaving a callback to a dead dialog.
 */
class VrHeadsetDialog : public QDialog
{
    Q_OBJECT

public:
    VrHeadsetDialog(const VrLink::Inputs &inputs, QWidget *parent = nullptr);
    ~VrHeadsetDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    /// How an adb run ended: its exit code means something only when Finished.
    enum class Outcome { Finished, TimedOut, FailedToStart, Crashed };
    using Done = std::function<void(Outcome outcome, int exitCode, const QString &output)>;

    /// @p background: the periodic check, which leaves the buttons and the
    /// device list alone unless what adb reports has changed.
    void refreshDevices(bool background = false);
    void showDevices(const QList<AdbHeadset::Device> &devices);
    /// The state label from @p g: the headline, the numbered steps, the guide link.
    void setGuidance(const AdbHeadset::Guidance &g, const QString &extraHtml = QString());
    /// macOS: is the Quest on USB even though adb lists nothing? Run off the
    /// GUI thread (an asynchronous system_profiler), at most every few seconds;
    /// the state label is updated when it answers.
    void probeUsb();
    /// Copies the last error, the state and the adb found, redacted.
    void copyError();
    void openOnHeadset();
    /// Open on @p serial, the device chosen when Open was clicked: refused,
    /// never redirected, when that device is no longer ready.
    void startOpen(const QString &serial);
    /// Closed (or hidden by Esc): nothing new is started after that, since
    /// WA_DeleteOnClose deletes the dialog only later.
    void stopAll();
    void openPage(const QString &serial, const QString &package);
    void copyLink();
    /// Run adb with @p args; @p done gets how it ended, its exit code and its
    /// merged output. One at a time: the buttons are off meanwhile, except for
    /// a @p background run (the periodic device check), during which a click on
    /// Open is queued and run when the check ends.
    void runAdb(const QStringList &args, int timeoutMs, Done done, bool background = false);
    void setBusy(bool busy);
    /// Asks the backend what it serves to a headset (GET /v1/vr/backend_check, issue
    /// #187), so an old cluster overlay shows here, not as a 404 in the headset.
    void checkBackend();
    void setResult(const QString &text, bool error);
    /// What to tell the user when adb itself did not run to the end.
    static QString adbFailure(Outcome outcome);
    QString redacted(const QString &text) const;
    QString selectedSerial() const;

    VrLink::Link m_link;
    QStringList m_secrets;   ///< token and session id: redacted from all output
    QString m_adb;

    QString m_token;         ///< for the backend check's header only, never shown
    QLabel *m_backendLabel{ nullptr };
    QNetworkAccessManager *m_network{ nullptr };
    bool m_backendBlocked{ false };   ///< the check said the headset cannot work
    QLabel *m_stateLabel{ nullptr };
    QComboBox *m_deviceCombo{ nullptr };
    QPushButton *m_refreshButton{ nullptr };
    QPushButton *m_openButton{ nullptr };
    QPushButton *m_copyButton{ nullptr };
    QPushButton *m_copyErrorButton{ nullptr };
    QLabel *m_resultLabel{ nullptr };
    QTimer *m_pollTimer{ nullptr };
    QPointer<QProcess> m_process;
    QPointer<QProcess> m_usbProbe;   ///< system_profiler, independent of the adb runs
    AdbHeadset::UsbQuest m_usbQuest{ AdbHeadset::UsbQuest::Unknown };
    QElapsedTimer m_usbProbedAt;
    bool m_noDevice{ false };        ///< the last check listed nothing (the USB probe matters then)
    QString m_stateText;             ///< the state label as plain text, for Copy Error
    QString m_errorText;             ///< the last error shown, plain text ('' when none)
    bool m_stateProblem{ false };    ///< the state label shows a problem (Copy Error offered)
    QString m_lastOpenSerial;       ///< the device of the open under way (remembered when it works)
    bool m_deviceReady{ false };
    bool m_backgroundRun{ false };   ///< m_process is the periodic device check
    bool m_openQueued{ false };      ///< Open clicked during that check
    QString m_queuedSerial;          ///< the device chosen for that click
    QStringList m_readySerials;      ///< ready devices, as of the last check
    bool m_closed{ false };
};
