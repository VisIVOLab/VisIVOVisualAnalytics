#pragma once

// AdbHeadset.h
// What the "Open in VR Headset" dialog needs from the Android platform tools
// (adb) to open the backend's WebXR page on a Quest, as pure functions: where
// adb may be installed, how to read `adb devices -l`, the argument lists of
// the two commands it runs, and how to tell their success from their failure
// (issue #172). The dialog runs them with QProcess; this file runs nothing.
//
// Widget-free and header-only: unit-tested in tests/test_adb_headset.cpp.

#include <QByteArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <functional>

namespace AdbHeadset {

/// The Quest's own browser. Opened explicitly because a headset may also have
/// another browser installed that offers no WebXR.
inline const QString &questBrowserPackage()
{
    static const QString package = QStringLiteral("com.oculus.browser");
    return package;
}

enum class Platform { MacOS, Linux, Windows };

inline Platform currentPlatform()
{
#if defined(Q_OS_WIN)
    return Platform::Windows;
#elif defined(Q_OS_MACOS)
    return Platform::MacOS;
#else
    return Platform::Linux;
#endif
}

/// Directories adb is commonly installed in that are often NOT on the PATH of
/// an application started from the Dock, the Start menu or a desktop file
/// (Homebrew's prefix, the Android SDK's platform-tools). The caller searches
/// the PATH first, then these, in order.
inline QStringList candidateDirs(Platform platform, const QString &home,
                                 const QProcessEnvironment &env)
{
    QStringList dirs;
    const auto addSdk = [&dirs, &env](const char *var) {
        const QString root = env.value(QString::fromLatin1(var));
        if (!root.isEmpty())
            dirs << QDir(root).filePath(QStringLiteral("platform-tools"));
    };
    addSdk("ANDROID_HOME");
    addSdk("ANDROID_SDK_ROOT");
    switch (platform) {
    case Platform::MacOS:
        dirs << QStringLiteral("/opt/homebrew/bin") << QStringLiteral("/usr/local/bin")
             << QDir(home).filePath(QStringLiteral("Library/Android/sdk/platform-tools"));
        break;
    case Platform::Linux:
        dirs << QStringLiteral("/usr/bin") << QStringLiteral("/usr/local/bin")
             << QDir(home).filePath(QStringLiteral("Android/Sdk/platform-tools"))
             << QStringLiteral("/home/linuxbrew/.linuxbrew/bin");
        break;
    case Platform::Windows: {
        const QString local = env.value(QStringLiteral("LOCALAPPDATA"));
        if (!local.isEmpty())
            dirs << QDir(local).filePath(QStringLiteral("Android/Sdk/platform-tools"));
        dirs << QDir(home).filePath(QStringLiteral("AppData/Local/Android/Sdk/platform-tools"))
             << QStringLiteral("C:/platform-tools");
        break;
    }
    }
    dirs.removeDuplicates();
    return dirs;
}

/// The adb to run: the PATH's first (what the user gets in a terminal), else
/// the first of candidateDirs(). Empty when there is none.
inline QString findAdb(Platform platform, const QString &home, const QProcessEnvironment &env)
{
    const QString name = platform == Platform::Windows ? QStringLiteral("adb.exe")
                                                       : QStringLiteral("adb");
    QStringList path;
    for (QString dir : env.value(QStringLiteral("PATH"))
                               .split(platform == Platform::Windows ? u';' : u':',
                                      Qt::SkipEmptyParts)) {
        // Windows allows a quoted entry ("C:/Program Files/x"); the quotes
        // are not part of the directory.
        if (platform == Platform::Windows)
            dir.remove(u'"');
        if (!dir.trimmed().isEmpty())
            path << dir;
    }
    // Never with an empty list: findExecutable() then searches the process's
    // PATH instead of the one passed in.
    QString found = path.isEmpty() ? QString() : QStandardPaths::findExecutable(name, path);
    const QStringList dirs = candidateDirs(platform, home, env);
    if (found.isEmpty() && !dirs.isEmpty())
        found = QStandardPaths::findExecutable(name, dirs);
    return found;
}

enum class DeviceState {
    Ready,           ///< "device": authorised, commands will run
    Unauthorized,    ///< the headset has not accepted this computer's USB-debugging key
    Authorizing,     ///< "authorizing"/"connecting": the handshake is under way
    Offline,         ///< listed but not answering (asleep, cable, or adb restarting)
    NoPermissions,   ///< Linux: the user may not open the USB device (udev rules)
    Other,           ///< recovery, bootloader, sideload… nothing to run on
};

struct Device
{
    QString serial;   ///< what `adb -s` takes (a USB serial, or ip:port over Wi-Fi)
    DeviceState state{ DeviceState::Other };
    QString model;    ///< "Quest_3" from `-l`, empty when adb did not say
};

/// The devices in the output of `adb devices` or `adb devices -l`. The
/// header, adb's own daemon messages ("* daemon started successfully") and
/// blank lines are skipped.
inline QList<Device> parseDevices(const QString &output)
{
    QList<Device> devices;
    const QStringList lines = output.split(u'\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(u'*') || line.startsWith(QStringLiteral("List of devices"))
            || line.startsWith(QStringLiteral("adb server")) || line.startsWith(QStringLiteral("adb:")))
            continue;
        // Tab-separated, `-l` adds space-separated key:value fields.
        const QStringList fields = line.simplified().split(u' ', Qt::SkipEmptyParts);
        if (fields.size() < 2)
            continue;
        Device d;
        d.serial = fields.at(0);
        const QString state = fields.at(1);
        if (state == u"device")
            d.state = DeviceState::Ready;
        else if (state == u"unauthorized")
            d.state = DeviceState::Unauthorized;
        else if (state == u"authorizing" || state == u"connecting")
            d.state = DeviceState::Authorizing;
        else if (state == u"offline")
            d.state = DeviceState::Offline;
        else if (state == u"no" && fields.size() > 2 && fields.at(2).startsWith(u"permissions"))
            d.state = DeviceState::NoPermissions;
        else
            d.state = DeviceState::Other;
        for (int i = 2; i < fields.size(); ++i)
            if (fields.at(i).startsWith(u"model:"))
                d.model = fields.at(i).mid(6);
        devices << d;
    }
    return devices;
}

/// What the dialog says about the devices as a whole.
enum class Status {
    NoDevice,        ///< nothing listed
    Unauthorized,    ///< something listed, none ready, at least one waiting for the prompt
    NoPermissions,   ///< none ready, one the user may not open
    Authorizing,     ///< none ready, one still in the authorisation handshake
    Offline,         ///< none ready, the rest offline or in another mode
    Ready,           ///< at least one ready
};

inline Status statusOf(const QList<Device> &devices)
{
    bool unauthorized = false, noPermissions = false, authorizing = false;
    for (const Device &d : devices) {
        if (d.state == DeviceState::Ready)
            return Status::Ready;
        unauthorized |= d.state == DeviceState::Unauthorized;
        noPermissions |= d.state == DeviceState::NoPermissions;
        authorizing |= d.state == DeviceState::Authorizing;
    }
    if (devices.isEmpty())
        return Status::NoDevice;
    if (unauthorized)
        return Status::Unauthorized;
    if (noPermissions)
        return Status::NoPermissions;
    if (authorizing)
        return Status::Authorizing;
    return Status::Offline;
}

inline QList<Device> readyDevices(const QList<Device> &devices)
{
    QList<Device> ready;
    for (const Device &d : devices)
        if (d.state == DeviceState::Ready)
            ready << d;
    return ready;
}

/// @p text as one word for the POSIX shell on the device: `adb shell` joins
/// its arguments with spaces and hands the line to `sh -c`, so an unquoted
/// '&' would end the command and '#' would start a comment. Inside single
/// quotes nothing is special but the quote itself, written as '\''.
inline QString shellQuote(const QString &text)
{
    QString quoted = text;
    quoted.replace(u'\'', QStringLiteral("'\\''"));
    return u'\'' + quoted + u'\'';
}

/// `-s <serial>` when a device is named (needed as soon as two are attached).
inline QStringList serialArgs(const QString &serial)
{
    return serial.isEmpty() ? QStringList() : QStringList{ QStringLiteral("-s"), serial };
}

/// Forward the headset's localhost:<port> to this computer's localhost:<port>.
inline QStringList reverseArgs(const QString &serial, int port)
{
    const QString spec = QStringLiteral("tcp:%1").arg(port);
    return serialArgs(serial) << QStringLiteral("reverse") << spec << spec;
}

/// Ask the package manager whether @p package is installed.
inline QStringList packageQueryArgs(const QString &serial, const QString &package)
{
    return serialArgs(serial) << QStringLiteral("shell") << QStringLiteral("pm")
                              << QStringLiteral("list") << QStringLiteral("packages")
                              << shellQuote(package);
}

/// `pm list packages <name>` lists every package whose name CONTAINS name;
/// only an exact "package:<name>" line means it is installed.
inline bool packageListed(const QString &output, const QString &package)
{
    const QString wanted = QStringLiteral("package:") + package;
    for (const QString &line : output.split(u'\n'))
        if (line.trimmed() == wanted)
            return true;
    return false;
}

/// Open @p url with a VIEW intent, in @p package when given (the Quest
/// Browser), else in whatever the headset chooses. The URL is quoted for the
/// device's shell (see shellQuote).
inline QStringList viewArgs(const QString &serial, const QString &url, const QString &package)
{
    QStringList args = serialArgs(serial);
    args << QStringLiteral("shell") << QStringLiteral("am") << QStringLiteral("start")
         << QStringLiteral("-a") << QStringLiteral("android.intent.action.VIEW")
         << QStringLiteral("-d") << shellQuote(url);
    if (!package.isEmpty())
        args << shellQuote(package);
    return args;
}

/// `am start` exits 0 even when it could not start anything, and says so on
/// its output instead ("Error: Activity not started, unable to resolve
/// Intent", "Error type 3", "Exception occurred while executing"). Its
/// "Warning: Activity not started, intent has been delivered to currently
/// running top-most instance" is a success: the open browser got the URL.
inline bool viewStarted(int exitCode, const QString &output)
{
    if (exitCode != 0)
        return false;
    for (const QString &line : output.split(u'\n')) {
        const QString l = line.trimmed();
        if (l.startsWith(QStringLiteral("Error")) || l.contains(QStringLiteral("Exception")))
            return false;
    }
    return true;
}

// ── Next-step guidance (epic #166) ──────────────────────────────────────────

/// The command that installs adb on @p platform, to type in a terminal.
inline QString installCommand(Platform platform)
{
    switch (platform) {
    case Platform::MacOS:
        return QStringLiteral("brew install android-platform-tools");
    case Platform::Linux:
        return QStringLiteral("sudo apt install adb");
    case Platform::Windows:
        return QStringLiteral("winget install Google.PlatformTools");
    }
    return QString();
}

/// Is a Quest on this computer's USB at all, whatever adb says? Known only on
/// macOS (system_profiler); elsewhere Unknown.
enum class UsbQuest { Unknown, Absent, Present };

/// Meta's (Oculus') USB vendor id.
inline const QString &questVendorId()
{
    static const QString id = QStringLiteral("0x2833");
    return id;
}

/// The program and arguments that list this Mac's USB devices as JSON. Cheap
/// (about 0.1 s) and run asynchronously by the dialog.
inline QString usbProbeProgram() { return QStringLiteral("/usr/sbin/system_profiler"); }
inline QStringList usbProbeArgs() { return { QStringLiteral("-json"), QStringLiteral("SPUSBHostDataType") }; }

/// Present when the JSON of `system_profiler -json SPUSBHostDataType` (or the
/// older SPUSBDataType) lists a device with Meta's vendor id, Absent when it
/// lists none, Unknown when the output cannot be read.
inline UsbQuest usbQuestFromProfiler(const QByteArray &json)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return UsbQuest::Unknown;
    const QJsonObject root = doc.object();
    if (!root.contains(QStringLiteral("SPUSBHostDataType")) && !root.contains(QStringLiteral("SPUSBDataType")))
        return UsbQuest::Unknown;
    std::function<bool(const QJsonValue &)> walk = [&walk](const QJsonValue &v) -> bool {
        if (v.isArray()) {
            for (const QJsonValue &x : v.toArray())
                if (walk(x))
                    return true;
            return false;
        }
        if (!v.isObject())
            return false;
        const QJsonObject o = v.toObject();
        for (const char *key : { "USBDeviceKeyVendorID", "vendor_id" }) {
            const QString id = o.value(QLatin1String(key)).toString().trimmed().toLower();
            if (id.startsWith(questVendorId()))
                return true;
        }
        return walk(o.value(QStringLiteral("_items")));
    };
    for (const QJsonValue &v : root)
        if (walk(v))
            return UsbQuest::Present;
    return UsbQuest::Absent;
}

/// What the dialog says for a state: a headline, the steps to take (in
/// order), and the anchor of the user guide's section about it.
struct Guidance
{
    QString headline;
    QStringList steps;
    QString anchor;
    bool problem{ true };   ///< false only when ready
};

inline QStringList developerModeSteps()
{
    return {
        QStringLiteral("On your phone, open the Meta Horizon app: Menu → Devices → your headset → "
                       "Headset settings → Developer mode, and turn it on (the first time, Meta asks "
                       "you to register as a developer)."),
        QStringLiteral("Restart the headset, connect it with a USB-C data cable (a charge-only cable "
                       "carries no data), and put it on."),
        QStringLiteral("In the headset, accept \"Allow USB debugging\" and tick \"Always allow from "
                       "this computer\"."),
    };
}

/// adb is not installed (or not where the application looks for it).
inline Guidance adbMissingGuidance(Platform platform)
{
    Guidance g;
    g.headline = QStringLiteral("adb was not found.");
    g.steps << QStringLiteral("Install the Android platform tools: in a terminal, run  %1")
                       .arg(installCommand(platform));
    if (platform == Platform::Linux)
        g.steps << QStringLiteral("(Fedora: sudo dnf install android-tools; Arch: sudo pacman -S android-tools.)");
    if (platform == Platform::Windows)
        g.steps << QStringLiteral("(Or download the SDK Platform-Tools from developer.android.com and unpack "
                                  "them in %LOCALAPPDATA%\\Android\\Sdk\\platform-tools.)");
    g.steps << QStringLiteral("Then press Retry (this window also checks by itself every few seconds).");
    g.anchor = QStringLiteral("open-a-cube-from-the-desktop");
    return g;
}

/// The guidance for what adb reports (@p status) and, for NoDevice, whether
/// the Quest is on USB anyway (@p usb: macOS only).
inline Guidance guidanceFor(Status status, UsbQuest usb = UsbQuest::Unknown)
{
    Guidance g;
    g.anchor = QStringLiteral("troubleshooting");
    switch (status) {
    case Status::NoDevice:
        if (usb == UsbQuest::Present) {
            // The case adb cannot explain: the cable works, the headset will not talk.
            g.headline = QStringLiteral("The Quest is connected by USB, but adb does not see it: "
                                        "developer mode or USB debugging is off on the headset.");
            g.steps = developerModeSteps();
            g.steps << QStringLiteral("Still nothing: unplug and reconnect the cable, or run  adb kill-server  "
                                      "in a terminal, then press Retry.");
        } else {
            g.headline = usb == UsbQuest::Absent
                    ? QStringLiteral("No headset on USB: this computer does not see the Quest at all.")
                    : QStringLiteral("No headset connected.");
            if (usb == UsbQuest::Absent)
                g.steps << QStringLiteral("Check that the headset is on and the cable is a USB-C data "
                                          "cable, firmly in at both ends (try another port).");
            g.steps << developerModeSteps();
            g.steps << QStringLiteral("Over Wi-Fi instead: see the guide (adb connect <headset-ip>:5555).");
        }
        break;
    case Status::Unauthorized:
        g.headline = QStringLiteral("The headset has not allowed this computer yet.");
        g.steps << QStringLiteral("Put the headset on: it shows \"Allow USB debugging?\".")
                << QStringLiteral("Tick \"Always allow from this computer\" and choose Allow.")
                << QStringLiteral("No prompt in the headset: unplug and reconnect the cable, then look "
                                  "again (or run  adb kill-server  and press Retry).");
        break;
    case Status::NoPermissions:
        g.headline = QStringLiteral("adb may not open the headset's USB device.");
        g.steps << QStringLiteral("Install the udev rules for Android devices (Debian/Ubuntu: "
                                  "sudo apt install android-sdk-platform-tools-common), add yourself "
                                  "to the plugdev group, log out and in, reconnect the cable.");
        break;
    case Status::Authorizing:
        g.headline = QStringLiteral("Waiting for the headset to authorise this computer…");
        g.steps << QStringLiteral("If the headset asks, accept \"Allow USB debugging\".");
        break;
    case Status::Offline:
        g.headline = QStringLiteral("The headset is offline.");
        g.steps << QStringLiteral("Wake it up (put it on), or unplug and reconnect the cable.")
                << QStringLiteral("Still offline: run  adb kill-server  in a terminal, then press Retry.");
        break;
    case Status::Ready:
        g.headline = QStringLiteral("Ready.");
        g.steps << QStringLiteral("Press Open on Headset, put the headset on, then press Enter VR on the page.");
        g.anchor = QStringLiteral("open-a-cube-from-the-desktop");
        g.problem = false;
        break;
    }
    return g;
}

/// @p path with the home directory @p home written as "~" (what Copy Error
/// puts in a support request: the user's name is not needed there).
inline QString homeShortened(const QString &path, const QString &home)
{
    // Either separator, whatever the platform this runs on.
    const QString h = QString(home).replace(u'\\', u'/');
    const QString p = QString(path).replace(u'\\', u'/');
    if (h.isEmpty() || h == QStringLiteral("/"))
        return path;
    if (p == h)
        return QStringLiteral("~");
    if (p.startsWith(h + u'/'))
        return QStringLiteral("~") + p.mid(h.size());
    return path;
}

/// Which of @p serials to select: the headset used last time when it is
/// there, else the first. -1 when there is none.
inline int preferredIndex(const QStringList &serials, const QString &last)
{
    if (serials.isEmpty())
        return -1;
    const int i = last.isEmpty() ? -1 : serials.indexOf(last);
    return i >= 0 ? i : 0;
}

}   // namespace AdbHeadset
