#pragma once

// VrHeadsetApp.h
// The VisIVO VR headset app (issue #255): the backend's VR page packaged as
// an immersive Horizon OS app (tools/quest-app/), installed over adb and
// started by the desktop so that Open on Headset puts the user in VR, with
// no browser window and no Enter VR. The contract is on issue #255 and in
// docs/technical/backend-api/vr-launch.md.
//
// What is here, as pure functions: where the APK and its visivo-vr.json are,
// what that file says, the adb argument lists (version query, install,
// uninstall, wakefulness, the one `am start`) and how to read their output,
// the backend's assetlinks statement, the ticket request and answer, the
// launch status, the tracker that turns the status polls into "Starting…",
// "In VR" or one of the two timeouts, and the decision between showing the
// cube on a page already connected and launching the app.
//
// The ticket is a one-time credential: it is put in the launch address and
// nowhere else, and redactTicket() takes it out of anything shown or logged.
// The token never goes in the intent.
//
// Widget-free and header-only: unit-tested in tests/test_vr_headset_app.cpp.
// VrAppLauncher runs the commands and requests; this file runs nothing.

#include "AdbHeadset.h"
#include "MissingRoute.h"
#include "VrHeadsetHub.h"
#include "VrLink.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace VrHeadsetApp {

// ── The APK and visivo-vr.json ──────────────────────────────────────────────

/// What visivo-vr.json (written beside the APK by tools/quest-app/build.sh)
/// says about the APK it describes.
struct AppInfo
{
    bool ok{ false };
    QString error;           ///< why the APK cannot be used, for the user
    QString apkPath;
    QString jsonPath;
    QString package;         ///< it.inaf.oact.visivo.vr, or …vr.dev for a development build
    qint64 versionCode{ 0 };
    QString versionName;
    QString certSha256;      ///< "AB:CD:…", the signing certificate
    QString startPath;       ///< "/vr/?launch=immersive"
    bool development{ false };
};

/// An Android package name: dot-separated Java identifiers, at least two.
/// Checked because it goes into the device's shell line of `am start`.
inline bool validPackage(const QString &package)
{
    static const QRegularExpression re(
            QStringLiteral("^[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+$"));
    return package.size() <= 255 && re.match(package).hasMatch();
}

/// "AB:CD:…" or "abcd…" as upper-case hex without separators, so two
/// spellings of one fingerprint compare equal. Empty when it is not 32 bytes
/// of hex.
inline QString normalizedFingerprint(const QString &fingerprint)
{
    QString hex = fingerprint.trimmed().toUpper();
    hex.remove(u':');
    static const QRegularExpression re(QStringLiteral("^[0-9A-F]{64}$"));
    return re.match(hex).hasMatch() ? hex : QString();
}

/// The visivo-vr.json beside @p apkPath: the same name with .json.
inline QString jsonPathFor(const QString &apkPath)
{
    QString json = apkPath;
    if (json.endsWith(QStringLiteral(".apk"), Qt::CaseInsensitive))
        json.chop(4);
    return json + QStringLiteral(".json");
}

/// Reads visivo-vr.json's @p json for the APK at @p apkPath.
inline AppInfo parseAppInfo(const QByteArray &json, const QString &apkPath)
{
    AppInfo info;
    info.apkPath = apkPath;
    info.jsonPath = jsonPathFor(apkPath);
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        info.error = QStringLiteral("%1 could not be read.").arg(QFileInfo(info.jsonPath).fileName());
        return info;
    }
    const QJsonObject o = doc.object();
    info.package = o.value(u"package").toString();
    info.versionCode = static_cast<qint64>(o.value(u"version_code").toDouble(-1));
    info.versionName = o.value(u"version_name").toString();
    info.certSha256 = o.value(u"cert_sha256").toString();
    info.startPath = o.value(u"start_path").toString();
    info.development = o.value(u"mode").toString() == u"dev"
            || info.package.endsWith(QStringLiteral(".dev"));
    if (!validPackage(info.package)) {
        info.error = QStringLiteral("%1 names no valid package.").arg(QFileInfo(info.jsonPath).fileName());
        return info;
    }
    if (info.versionCode < 1) {
        info.error = QStringLiteral("%1 has no version_code.").arg(QFileInfo(info.jsonPath).fileName());
        return info;
    }
    if (normalizedFingerprint(info.certSha256).isEmpty()) {
        info.error = QStringLiteral("%1 has no certificate fingerprint.").arg(QFileInfo(info.jsonPath).fileName());
        return info;
    }
    info.ok = true;
    return info;
}

/// Where the APK may be, in order:
/// 1. $VISIVO_QUEST_APK, a path to the .apk (its .json beside it);
/// 2. the application's own resources: <app>.app/Contents/Resources/quest-app
///    on macOS, <exe dir>/quest-app elsewhere (visivo-vr.apk);
/// 3. a development tree: tools/quest-app/out of the source tree the build
///    directory sits in (visivo-vr-dev.apk first: a backend on this computer
///    lists the development key by itself, the release key only when
///    backend/app/vr_app.json names it).
inline QStringList apkCandidates(AdbHeadset::Platform platform, const QString &appDir,
                                 const QProcessEnvironment &env)
{
    QStringList candidates;
    const QString fromEnv = env.value(QStringLiteral("VISIVO_QUEST_APK")).trimmed();
    if (!fromEnv.isEmpty())
        candidates << QDir::cleanPath(fromEnv);
    if (platform == AdbHeadset::Platform::MacOS)
        candidates << QDir::cleanPath(appDir + QStringLiteral("/../Resources/quest-app/visivo-vr.apk"));
    candidates << QDir::cleanPath(appDir + QStringLiteral("/quest-app/visivo-vr.apk"));
    // The build directory is <repo>/build (or deeper, inside a macOS bundle:
    // <repo>/build/VisIVO.app/Contents/MacOS).
    for (const QString &up : { QStringLiteral("/.."), QStringLiteral("/../.."),
                               QStringLiteral("/../../.."), QStringLiteral("/../../../..") }) {
        const QString out = appDir + up + QStringLiteral("/tools/quest-app/out/");
        candidates << QDir::cleanPath(out + QStringLiteral("visivo-vr-dev.apk"))
                   << QDir::cleanPath(out + QStringLiteral("visivo-vr.apk"));
    }
    candidates.removeDuplicates();
    return candidates;
}

/// The first candidate whose APK and .json both exist, read. Not ok, with
/// the reason, when there is none (the browser page is then the way) or when
/// $VISIVO_QUEST_APK names something unusable (said, not skipped: the user
/// asked for that file).
inline AppInfo findApp(AdbHeadset::Platform platform, const QString &appDir,
                       const QProcessEnvironment &env)
{
    const QString fromEnv = env.value(QStringLiteral("VISIVO_QUEST_APK")).trimmed();
    for (const QString &apk : apkCandidates(platform, appDir, env)) {
        const bool named = !fromEnv.isEmpty() && apk == QDir::cleanPath(fromEnv);
        const QString json = jsonPathFor(apk);
        if (!QFileInfo(apk).isFile() || !QFileInfo(json).isFile()) {
            if (named) {
                AppInfo info;
                info.apkPath = apk;
                info.jsonPath = json;
                info.error = QStringLiteral("VISIVO_QUEST_APK names %1, but it or %2 is missing.")
                                     .arg(QFileInfo(apk).fileName(), QFileInfo(json).fileName());
                return info;
            }
            continue;
        }
        QFile f(json);
        if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024) {
            AppInfo info;
            info.apkPath = apk;
            info.jsonPath = json;
            info.error = QStringLiteral("%1 could not be read.").arg(QFileInfo(json).fileName());
            return info;
        }
        return parseAppInfo(f.readAll(), apk);
    }
    AppInfo none;
    none.error = QStringLiteral("The VisIVO VR app is not bundled with this build.");
    return none;
}

// ── Installed and current ───────────────────────────────────────────────────

/// `pm list packages --show-versioncode <package>`: the installed version.
inline QStringList versionQueryArgs(const QString &serial, const QString &package)
{
    return AdbHeadset::serialArgs(serial)
            << QStringLiteral("shell") << QStringLiteral("pm") << QStringLiteral("list")
            << QStringLiteral("packages") << QStringLiteral("--show-versioncode")
            << AdbHeadset::shellQuote(package);
}

/// The versionCode of @p package in that command's output, -1 when it is not
/// installed. `pm list packages <name>` lists every package whose name
/// CONTAINS name (the release query also lists the .dev app): only the line
/// of exactly this package counts.
inline qint64 installedVersion(const QString &output, const QString &package)
{
    const QString wanted = QStringLiteral("package:") + package;
    for (const QString &raw : output.split(u'\n')) {
        const QStringList fields = raw.simplified().split(u' ', Qt::SkipEmptyParts);
        if (fields.isEmpty() || fields.first() != wanted)
            continue;
        for (const QString &f : fields) {
            if (!f.startsWith(QStringLiteral("versionCode:")))
                continue;
            bool ok = false;
            const qint64 v = f.mid(12).toLongLong(&ok);
            return ok && v >= 0 ? v : 0;
        }
        return 0;   // listed, version not said: older than any build of ours
    }
    return -1;
}

enum class Installed { Unknown, Absent, Older, Current };

/// Current iff the installed versionCode is at least the APK's.
inline Installed installedState(qint64 installed, qint64 wanted)
{
    if (installed < 0)
        return Installed::Absent;
    return installed >= wanted ? Installed::Current : Installed::Older;
}

inline QStringList installArgs(const QString &serial, const QString &apkPath)
{
    // Not quoted: `adb install` takes a local file, no device shell is involved.
    return AdbHeadset::serialArgs(serial) << QStringLiteral("install") << QStringLiteral("-r") << apkPath;
}

inline QStringList uninstallArgs(const QString &serial, const QString &package)
{
    return AdbHeadset::serialArgs(serial) << QStringLiteral("uninstall") << package;
}

struct InstallResult
{
    enum class Outcome { Success, UpdateIncompatible, Failed };
    Outcome outcome{ Outcome::Failed };
    QString message;   ///< adb's reason ("INSTALL_FAILED_…"), '' on success
};

/// `adb install` prints "Success" on its last line when it worked; otherwise
/// a "Failure [INSTALL_FAILED_…]" or "adb: failed to install …" line.
/// INSTALL_FAILED_UPDATE_INCOMPATIBLE means an app with this package signed
/// by another key is installed: the user is asked before it is uninstalled.
inline InstallResult parseInstall(int exitCode, const QString &output)
{
    InstallResult r;
    QStringList lines;
    for (const QString &line : output.split(u'\n'))
        if (!line.trimmed().isEmpty())
            lines << line.trimmed();
    if (output.contains(QStringLiteral("INSTALL_FAILED_UPDATE_INCOMPATIBLE"))) {
        r.outcome = InstallResult::Outcome::UpdateIncompatible;
        r.message = QStringLiteral("INSTALL_FAILED_UPDATE_INCOMPATIBLE");
        return r;
    }
    if (exitCode == 0 && !lines.isEmpty() && lines.last() == u"Success") {
        r.outcome = InstallResult::Outcome::Success;
        return r;
    }
    static const QRegularExpression code(QStringLiteral("INSTALL_[A-Z_]+"));
    const QRegularExpressionMatch m = code.match(output);
    r.message = m.hasMatch() ? m.captured(0)
                             : (lines.isEmpty() ? QStringLiteral("no answer from adb")
                                                : lines.last().left(300));
    return r;
}

/// `adb uninstall` prints "Success" too.
inline bool uninstalled(int exitCode, const QString &output)
{
    return exitCode == 0 && output.trimmed().endsWith(QStringLiteral("Success"));
}

// ── The backend must list the app ───────────────────────────────────────────

/// Where the headset's browser checks the app: the ORIGIN's
/// /.well-known/assetlinks.json (Digital Asset Links are per origin, so a
/// path prefix is not part of it). Through adb reverse the headset's
/// localhost:P is this computer's localhost:P, so the desktop asks the same
/// backend the headset will.
inline QString assetlinksUrl(const QString &baseUrl)
{
    QUrl base(baseUrl.trimmed());
    if (!base.isValid() || base.host().isEmpty())
        return {};
    QUrl url;
    url.setScheme(base.scheme());
    url.setHost(base.host());
    url.setPort(base.port());
    url.setPath(QStringLiteral("/.well-known/assetlinks.json"));
    return url.toString(QUrl::FullyEncoded);
}

enum class Listing { Listed, NotListed, Unreadable };

/// Does the backend's assetlinks answer (@p httpStatus, @p body) hold a
/// statement for @p package with @p certSha256? A 404 (no statement at
/// all) is NotListed; no answer or an unreadable one is Unreadable.
inline Listing parseAssetlinks(int httpStatus, const QByteArray &body, const QString &package,
                               const QString &certSha256)
{
    if (httpStatus == 404)
        return Listing::NotListed;
    if (httpStatus != 200)
        return Listing::Unreadable;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isArray())
        return Listing::Unreadable;
    const QString wanted = normalizedFingerprint(certSha256);
    if (wanted.isEmpty())
        return Listing::NotListed;
    for (const QJsonValue v : doc.array()) {
        const QJsonObject statement = v.toObject();
        const QJsonObject target = statement.value(u"target").toObject();
        // Only a statement that delegates URL handling to an Android app
        // lets the headset's browser open the origin in the app.
        if (target.value(u"namespace").toString() != u"android_app"
            || !statement.value(u"relation").toArray().contains(
                    QJsonValue(QStringLiteral("delegate_permission/common.handle_all_urls")))
            || target.value(u"package_name").toString() != package)
            continue;
        for (const QJsonValue fp : target.value(u"sha256_cert_fingerprints").toArray())
            if (normalizedFingerprint(fp.toString()) == wanted)
                return Listing::Listed;
    }
    return Listing::NotListed;
}

/// What the user is told when the backend does not list the app.
inline QString notListedMessage(const AppInfo &app)
{
    return app.development
            ? QStringLiteral("This backend does not list the VisIVO VR app (development key "
                             "missing: build the app on the computer that runs the backend, then "
                             "restart the backend).")
            : QStringLiteral("This backend does not list the VisIVO VR app (release key not in "
                             "vr_app.json: update the backend).");
}

// ── The headset must be awake ───────────────────────────────────────────────

/// The power state: the grep runs on the device, so only one line comes back.
inline QStringList wakefulnessArgs(const QString &serial)
{
    return AdbHeadset::serialArgs(serial) << QStringLiteral("shell")
                                          << QStringLiteral("dumpsys power | grep mWakefulness=");
}

enum class Wake { Awake, NotAwake, Unknown };

/// "mWakefulness=Awake" → Awake; Asleep, Dozing, Dreaming → NotAwake (the
/// headset's browser drops a launch then, silently); no such line → Unknown
/// (launched anyway: the exchange timeout still catches a dropped launch).
inline Wake parseWakefulness(const QString &output)
{
    static const QRegularExpression re(QStringLiteral("mWakefulness=(\\w+)"));
    const QRegularExpressionMatch m = re.match(output);
    if (!m.hasMatch())
        return Wake::Unknown;
    return m.captured(1) == u"Awake" ? Wake::Awake : Wake::NotAwake;
}

/// Said for an asleep headset, and for a launch the headset never took up.
inline QString putHeadsetOnMessage()
{
    return QStringLiteral("Put the headset on, then press Open again.");
}

// ── The ticket ──────────────────────────────────────────────────────────────

/// The body of POST /v1/vr/launch: the cube to open, or {} for the app's
/// connected empty room. No token: it travels in the headers. No box, as for
/// the hub's `show` of a whole cube. A sky catalogue carries its placement
/// (`sky`, issue #260), as the `show` does.
inline QJsonObject launchRequestBody(const VrLink::Inputs &in)
{
    if (in.datasetId.isEmpty() || in.path.isEmpty())
        return {};
    QJsonObject cube{ { QStringLiteral("dataset_id"), in.datasetId },
                      { QStringLiteral("path"), in.path },
                      { QStringLiteral("kind"), in.kind.isEmpty() ? QStringLiteral("cube") : in.kind } };
    if (in.sky.isSet())
        cube.insert(QStringLiteral("sky"), VrHub::skyObject(in.sky));
    return QJsonObject{ { QStringLiteral("cube"), cube } };
}

/// A ticket or a launch id as the backend makes them ([A-Za-z0-9_-]): checked
/// before either goes into an address, a shell line or a URL path.
inline bool validToken(const QString &value, int minLength, int maxLength)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]+$"));
    return value.size() >= minLength && value.size() <= maxLength && re.match(value).hasMatch();
}

struct Ticket
{
    bool ok{ false };
    bool unsupported{ false };   ///< the backend has no launch route (older than this client)
    QString ticket;              ///< NEVER log it
    QString launch;
    QString page;                ///< "/vr/?launch=immersive"
    double expiresIn{ 0 };
    QString error;
};

inline Ticket parseTicket(int httpStatus, const QByteArray &body, const QString &qtError = {})
{
    Ticket t;
    if (visivo::isMissingRouteReply(httpStatus, body)) {
        t.unsupported = true;
        t.error = QStringLiteral("This backend cannot start the VisIVO VR app (it is older than "
                                 "this client): update it, or open the page in the browser.");
        return t;
    }
    if (httpStatus != 200) {
        // Never the body of a 200 (it holds the ticket); a refusal's detail is safe.
        const QString detail = QJsonDocument::fromJson(body).object().value(u"detail").toString().left(200);
        t.error = httpStatus == 0
                ? QStringLiteral("The backend did not answer: %1").arg(qtError)
                : QStringLiteral("The backend refused the launch (HTTP %1)%2")
                          .arg(httpStatus)
                          .arg(detail.isEmpty() ? QString() : QStringLiteral(": %1").arg(detail));
        return t;
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    t.ticket = o.value(u"ticket").toString();
    t.launch = o.value(u"launch").toString();
    t.page = o.value(u"page").toString();
    t.expiresIn = o.value(u"expires_in").toDouble();
    // The page goes into the device's shell line inside single quotes: no
    // quote, no fragment of its own, no space.
    static const QRegularExpression page(QStringLiteral("^/[A-Za-z0-9/_.?=&%-]*$"));
    if (!validToken(t.ticket, 16, 256) || !validToken(t.launch, 8, 32)
        || !page.match(t.page).hasMatch()) {
        t.ticket.clear();
        t.error = QStringLiteral("The backend's launch answer could not be read.");
        return t;
    }
    t.ok = true;
    return t;
}

/// http://localhost:<port><prefix><page>#ticket=<ticket>: host always
/// localhost (the headset's, forwarded by adb reverse), the backend's own
/// port and path prefix. CONTAINS THE TICKET: never log it.
inline QString launchUrl(int port, const QString &prefix, const QString &page, const QString &ticket)
{
    return QStringLiteral("http://localhost:%1%2%3#ticket=%4").arg(port).arg(prefix, page, ticket);
}

/// The one `am start`, as the contract writes it: a single argument, so
/// `adb shell` hands the device's shell exactly this line.
inline QString amStartCommand(const QString &package, const QString &url)
{
    return QStringLiteral("am start -a android.intent.action.VIEW -n %1/.LauncherActivity -d %2")
            .arg(package, AdbHeadset::shellQuote(url));
}

inline QStringList amStartArgs(const QString &serial, const QString &package, const QString &url)
{
    return AdbHeadset::serialArgs(serial) << QStringLiteral("shell") << amStartCommand(package, url);
}

/// `ticket=<value>` as `ticket=<redacted>` wherever it appears (adb echoes
/// the intent's data back; a log line may carry the address).
inline QString redactTicket(QString text)
{
    static const QRegularExpression re(QStringLiteral("ticket=[A-Za-z0-9_%-]+"));
    text.replace(re, QStringLiteral("ticket=<redacted>"));
    return text;
}

// ── Following the launch ────────────────────────────────────────────────────

struct LaunchStatus
{
    bool ok{ false };
    bool unknown{ false };   ///< 404: the backend does not know the launch (restarted?)
    QString state;           ///< pending | exchanged | expired
    QString client;          ///< the hub page whose hello named the launch ('' none yet)
    QString error;
};

inline LaunchStatus parseLaunchStatus(int httpStatus, const QByteArray &body, const QString &qtError = {})
{
    LaunchStatus s;
    if (httpStatus == 404) {
        s.unknown = true;
        return s;
    }
    if (httpStatus != 200) {
        s.error = httpStatus == 0 ? qtError : QStringLiteral("HTTP %1").arg(httpStatus);
        return s;
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    s.state = o.value(u"state").toString();
    s.client = o.value(u"client").toString();
    if (s.state != u"pending" && s.state != u"exchanged" && s.state != u"expired") {
        s.error = QStringLiteral("the backend's launch status could not be read");
        return s;
    }
    s.ok = true;
    return s;
}

/// Turns the polls of GET /v1/vr/launch/{launch} into what the dialog says.
/// Times are the caller's monotonic ms (the backend's exchanged_at is on its
/// own clock and not used).
class LaunchTracker
{
public:
    enum class Phase {
        Idle,
        Starting,     ///< `am start` sent, the ticket not exchanged yet
        Connecting,   ///< exchanged: the page started, no hub hello yet
        InVr,         ///< the page's hello named the launch: connected
        Asleep,       ///< not exchanged in time (or expired): the headset dropped it
        NoConnect,    ///< exchanged, but no hello in time
        Lost,         ///< the backend forgot the launch (it restarted)
    };

    /// The contract's timeouts: 8 s for the exchange after the launch, 10 s
    /// more for the hello.
    static constexpr qint64 kExchangeTimeoutMs = 8000;
    static constexpr qint64 kClientTimeoutMs = 10000;

    explicit LaunchTracker(qint64 exchangeTimeoutMs = kExchangeTimeoutMs,
                           qint64 clientTimeoutMs = kClientTimeoutMs)
        : m_exchangeTimeout(exchangeTimeoutMs), m_clientTimeout(clientTimeoutMs)
    {
    }

    /// The `am start` of @p launch went out at @p nowMs.
    void begin(const QString &launch, qint64 nowMs)
    {
        m_launch = launch;
        m_phase = Phase::Starting;
        m_startedAt = nowMs;
        m_exchangedAt = -1;
        m_client.clear();
        m_lastError.clear();
    }

    void reset() { *this = LaunchTracker(m_exchangeTimeout, m_clientTimeout); }

    /// A poll's answer. Answers after the end (a poll still out) change nothing.
    void polled(const LaunchStatus &s, qint64 nowMs)
    {
        if (!active())
            return;
        if (s.unknown) {
            m_phase = Phase::Lost;
            return;
        }
        if (!s.ok) {
            m_lastError = s.error;   // a failed poll: the deadlines still run
            tick(nowMs);
            return;
        }
        if (!s.client.isEmpty()) {
            m_client = s.client;
            m_phase = Phase::InVr;
            return;
        }
        if (s.state == u"expired" && m_phase == Phase::Starting) {
            m_phase = Phase::Asleep;
            return;
        }
        if (s.state == u"exchanged" && m_phase == Phase::Starting) {
            m_phase = Phase::Connecting;
            m_exchangedAt = nowMs;
        }
        tick(nowMs);
    }

    /// The deadlines, checked on every poll tick even when no answer comes.
    void tick(qint64 nowMs)
    {
        if (m_phase == Phase::Starting && nowMs - m_startedAt >= m_exchangeTimeout)
            m_phase = Phase::Asleep;
        else if (m_phase == Phase::Connecting && nowMs - m_exchangedAt >= m_clientTimeout)
            m_phase = Phase::NoConnect;
    }

    Phase phase() const { return m_phase; }
    /// Still following: Open must not launch again meanwhile.
    bool active() const { return m_phase == Phase::Starting || m_phase == Phase::Connecting; }
    QString launch() const { return m_launch; }
    QString client() const { return m_client; }
    QString lastError() const { return m_lastError; }

    /// What the dialog says for the phase.
    QString message() const
    {
        switch (m_phase) {
        case Phase::Idle:
            return {};
        case Phase::Starting:
        case Phase::Connecting:
            return QStringLiteral("Starting on the headset…");
        case Phase::InVr:
            return QStringLiteral("In VR.");
        case Phase::Asleep:
            return putHeadsetOnMessage();
        case Phase::NoConnect:
            return m_lastError.isEmpty()
                    ? QStringLiteral("VisIVO VR started but could not connect to this computer.")
                    : QStringLiteral("VisIVO VR started but could not connect to this computer (%1).")
                              .arg(m_lastError);
        case Phase::Lost:
            return QStringLiteral("The backend no longer knows this launch (was it restarted?): "
                                  "press Open again.");
        }
        return {};
    }

private:
    qint64 m_exchangeTimeout;
    qint64 m_clientTimeout;
    Phase m_phase{ Phase::Idle };
    QString m_launch;
    QString m_client;
    QString m_lastError;
    qint64 m_startedAt{ -1 };
    qint64 m_exchangedAt{ -1 };
};

/// Exactly one `am start` per ticket: claim() is true the first time for a
/// launch id, false ever after (a retried step, a queued click).
class LaunchGate
{
public:
    bool claim(const QString &launch)
    {
        if (launch.isEmpty() || m_claimed.contains(launch))
            return false;
        m_claimed << launch;
        // A dialog stays open for hours at most: a short memory is enough.
        while (m_claimed.size() > 64)
            m_claimed.removeFirst();
        return true;
    }
    bool claimed(const QString &launch) const { return m_claimed.contains(launch); }

private:
    QStringList m_claimed;
};

// ── Show, or launch ─────────────────────────────────────────────────────────

/// The page of the app already running for this backend, if any: the one
/// whose hello named a launch, else a USB page, else the newest page. A
/// second launch while a page of the app is connected would leave the user
/// in a void, so any page there takes a `show` instead.
inline QString runningClient(const QList<VrHub::Headset> &headsets)
{
    for (const VrHub::Headset &h : headsets)
        if (!h.launch.isEmpty())
            return h.client;
    for (const VrHub::Headset &h : headsets)
        if (h.transport == u"adb")
            return h.client;
    return headsets.isEmpty() ? QString() : headsets.first().client;
}

enum class OpenAction {
    Busy,       ///< a launch is being followed: nothing new
    Show,       ///< a page is connected: the hub's `show`, never a second launch
    Launch,     ///< the app is installed (or will be): one `am start`
    Browser,    ///< no app to use: the Quest Browser page, as before #255
};

struct OpenDecision
{
    OpenAction action{ OpenAction::Browser };
    QString client;   ///< for Show
};

/// What Open on Headset does. @p appUsable: an APK is at hand (installed,
/// or installed by this Open). Without the app, a connected page still takes
/// the cube when it is the app's or a USB page; another page (a headset
/// paired by code) is left alone, and the browser opens on this headset.
inline OpenDecision decideOpen(bool launchActive, bool appUsable, const QList<VrHub::Headset> &headsets)
{
    OpenDecision d;
    if (launchActive) {
        d.action = OpenAction::Busy;
        return d;
    }
    if (appUsable) {
        d.client = runningClient(headsets);
        d.action = d.client.isEmpty() ? OpenAction::Launch : OpenAction::Show;
        return d;
    }
    for (const VrHub::Headset &h : headsets) {
        if (!h.launch.isEmpty() || h.transport == u"adb") {
            d.action = OpenAction::Show;
            d.client = runningClient(headsets);
            return d;
        }
    }
    d.action = OpenAction::Browser;
    return d;
}

}   // namespace VrHeadsetApp
