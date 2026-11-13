#pragma once

#include "app/AdbHeadset.h"
#include "app/BackendClient.h"
#include "app/VrLink.h"
#include "app/VrHeadsetHub.h"
#include "app/VrPair.h"

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
class QToolButton;
class QWidget;

/**
 * Tools ▸ Views ▸ Open in VR Headset… (issues #172, #214).
 *
 * By default it pairs the headset by code (issue #214): the backend asks its
 * pairing service for a short code (POST /v1/vr/pair), shown here large with
 * its countdown and a QR code of the public page that carries it. The user
 * types the code on the headset, and the backend connects to the headset
 * itself, with no cable and no address to type. The dialog polls
 * GET /v1/vr/pair for the state (waiting, connected, expired) and offers
 * Disconnect and New Code. The token never leaves the backend; the code is
 * not logged. Closing the dialog revokes a code nobody used.
 *
 * A headset page already open in this backend session (issue #238) comes
 * first: the dialog asks the backend's headset hub (GET /v1/vr/headset)
 * before anything else, and when a page is there it shows it (how it is
 * connected, whether it is in VR, the cube on show) with Show on Headset,
 * which sends this window's cube to that page: no code, no adb, no new
 * browser window. Pairing and the USB cable stay one click away (Pair
 * Another Headset) for a second headset. No page, or a backend without the
 * hub: the dialog is what it was, pairing by code.
 *
 * Under "Advanced (USB cable)" is the adb way below, opened by itself (with
 * the reason) when the backend has no pairing service or cannot reach it.
 * adb is only run once that section is open.
 *
 * The USB way opens the backend's WebXR page (/vr/) on a Meta Quest attached to this
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
    // ── A headset already connected (issue #238) ───────────────────────────
    /// Ask the hub which pages are open; none (or no hub): pair as before.
    /// @p quiet: a refresh of what is shown, which never falls back to pairing.
    void checkHeadsets(bool quiet = false);
    void showHeadsets();
    /// Send this window's cube to the newest page (Show on Headset).
    void showOnHeadset();
    /// Open the pairing and USB sections; the first time, ask for a code.
    void startPairing();
    // ── Pairing by code (issue #214) ──────────────────────────────────────
    /// Ask for a code; a previous one still pending is revoked first.
    void newCode();
    void pollPairing();
    void disconnectHeadsets();
    /// DELETE /v1/vr/pair/{code_or_id} for each of @p ids, off the GUI
    /// thread, even if the dialog is deleted meanwhile.
    void revoke(const QStringList &ids);
    /// Run @p work on a copy of the pairing client off the GUI thread; @p done
    /// gets its answer unless the dialog is gone or a newer code was asked
    /// for, in which case @p obsolete (if any) gets it, on the GUI thread,
    /// with the client copy the request used.
    void runPairRequest(std::function<BackendVrPairReply(const BackendClient &)> work,
                        std::function<void(const BackendVrPairReply &)> done,
                        std::function<void(const BackendVrPairReply &, const BackendClient &)>
                                obsolete = {});
    /// Everything on the pairing panel, from m_pair.
    void showPairing();
    void showQr(const QString &payload);
    /// Open or close the USB section; the first opening starts adb.
    void setAdvancedOpen(bool open, const QString &reason = QString());
    void startUsb();
    /// What to tell the user when adb itself did not run to the end.
    static QString adbFailure(Outcome outcome);
    QString redacted(const QString &text) const;
    QString selectedSerial() const;

    // A headset already connected
    QWidget *m_hubPanel{ nullptr };
    QLabel *m_hubLabel{ nullptr };
    QLabel *m_hubResult{ nullptr };
    QPushButton *m_showButton{ nullptr };
    QToolButton *m_pairAnotherToggle{ nullptr };
    QWidget *m_pairSection{ nullptr };   ///< pairing + USB, hidden while a page is used
    QList<VrHub::Headset> m_headsets;
    bool m_pairingStarted{ false };      ///< a code was asked for (newCode ran)
    bool m_showing{ false };             ///< a show command is out

    // Pairing by code
    VrLink::Inputs m_inputs;          ///< for the POST body (the token is not sent in it)
    BackendClient m_pairClient;       ///< template: each request runs on its own copy
    VrPair::Tracker m_pair;
    int m_pairSerial{ 0 };            ///< bumped by New Code: older answers are dropped
    VrPair::PollGate m_pollGate;      ///< one status poll at a time
    qint64 m_lastPollMs{ 0 };
    QTimer *m_pairTimer{ nullptr };   ///< the countdown, and the poll every few seconds
    QWidget *m_pairPanel{ nullptr };
    QLabel *m_codeLabel{ nullptr };
    QLabel *m_qrLabel{ nullptr };
    QLabel *m_pairHint{ nullptr };
    QLabel *m_pairStatus{ nullptr };
    QPushButton *m_newCodeButton{ nullptr };
    QPushButton *m_disconnectButton{ nullptr };
    QString m_qrPayload;              ///< what the QR shows now ('' none)
    QToolButton *m_advancedToggle{ nullptr };
    QWidget *m_advancedPanel{ nullptr };
    QLabel *m_advancedReason{ nullptr };
    bool m_usbStarted{ false };

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
