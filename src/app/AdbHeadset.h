#pragma once

// AdbHeadset.h
// What the "Open in VR Headset" dialog needs from the Android platform tools
// (adb) to open the backend's WebXR page on a Quest, as pure functions: where
// adb may be installed, how to read `adb devices -l`, the argument lists of
// the two commands it runs, and how to tell their success from their failure
// (issue #172). The dialog runs them with QProcess; this file runs nothing.
//
// Widget-free and header-only: unit-tested in tests/test_adb_headset.cpp.

#include <QDir>
#include <QList>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

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

}   // namespace AdbHeadset
