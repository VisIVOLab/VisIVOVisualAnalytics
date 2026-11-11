#pragma once

#include "app/AdbHeadset.h"
#include "app/VrLink.h"

#include <QDialog>
#include <QPointer>

#include <functional>

class QComboBox;
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
    void setResult(const QString &text, bool error);
    /// What to tell the user when adb itself did not run to the end.
    static QString adbFailure(Outcome outcome);
    QString redacted(const QString &text) const;
    QString selectedSerial() const;

    VrLink::Link m_link;
    QStringList m_secrets;   ///< token and session id: redacted from all output
    QString m_adb;

    QLabel *m_stateLabel{ nullptr };
    QComboBox *m_deviceCombo{ nullptr };
    QPushButton *m_refreshButton{ nullptr };
    QPushButton *m_openButton{ nullptr };
    QPushButton *m_copyButton{ nullptr };
    QLabel *m_resultLabel{ nullptr };
    QTimer *m_pollTimer{ nullptr };
    QPointer<QProcess> m_process;
    bool m_deviceReady{ false };
    bool m_backgroundRun{ false };   ///< m_process is the periodic device check
    bool m_openQueued{ false };      ///< Open clicked during that check
    QString m_queuedSerial;          ///< the device chosen for that click
    QStringList m_readySerials;      ///< ready devices, as of the last check
    bool m_closed{ false };
};
