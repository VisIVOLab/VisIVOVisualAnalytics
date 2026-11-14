#include "TestVrHeadsetApp.h"

#include "VrAppLauncher.h"
#include "VrHeadsetApp.h"
#include "VrHeadsetHub.h"
#include "VrLink.h"

#include <QDir>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;

#ifndef VISIVO_REPO_ROOT
#    define VISIVO_REPO_ROOT "."
#endif

namespace {

const QString kCert = u"AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89"_s;
const QString kDevPackage = u"it.inaf.oact.visivo.vr.dev"_s;
const QString kTicket = u"Qx7vAbc_def-1234567890123456789012345678901"_s;   // 43 characters
const QString kLaunch = u"sBYiD_0jN5yb"_s;
const QString kToken = u"tok-SECRET-xyz"_s;

QByteArray appJson(const QString &package = kDevPackage, int versionCode = 2, const QString &cert = kCert)
{
    return QJsonDocument(QJsonObject{ { u"package"_s, package },
                                      { u"version_code"_s, versionCode },
                                      { u"version_name"_s, u"1.0.1"_s },
                                      { u"mode"_s, package.endsWith(u".dev"_s) ? u"dev"_s : u"release"_s },
                                      { u"cert_sha256"_s, cert },
                                      { u"apk_sha256"_s, u"00"_s },
                                      { u"start_path"_s, u"/vr/?launch=immersive"_s },
                                      { u"default_origin"_s, u"http://localhost:8000"_s } })
            .toJson();
}

QByteArray assetlinks(const QString &package = kDevPackage, const QString &cert = kCert)
{
    return QJsonDocument(QJsonArray{ QJsonObject{
                                 { u"relation"_s, QJsonArray{ u"delegate_permission/common.handle_all_urls"_s } },
                                 { u"target"_s, QJsonObject{ { u"namespace"_s, u"android_app"_s },
                                                             { u"package_name"_s, package },
                                                             { u"sha256_cert_fingerprints"_s,
                                                               QJsonArray{ cert } } } } } })
            .toJson();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

VrHub::Headset headset(const QString &client, const QString &transport, const QString &launch = {})
{
    VrHub::Headset h;
    h.client = client;
    h.transport = transport;
    h.launch = launch;
    return h;
}

/// adb as the launcher sees it: a script in @p dir that logs each call's
/// arguments to dir/log and answers from the files beside it (installed:
/// the versionCode on the headset; mode: incompatible | asleep; started:
/// written by `am start`, read by the fake backend).
QString writeFakeAdb(const QString &dir)
{
    const QString path = dir + u"/adb"_s;
    const QByteArray script = R"SH(#!/bin/sh
d=$(dirname "$0")
printf '%s\n' "$*" >> "$d/log"
mode=$(cat "$d/mode" 2>/dev/null)
case "$*" in
  *"pm list packages"*)
    if [ -f "$d/installed" ]; then
      echo "package:it.inaf.oact.visivo.vr.dev.other versionCode:9"
      echo "package:it.inaf.oact.visivo.vr.dev versionCode:$(cat "$d/installed")"
    fi ;;
  *" install -r "*)
    echo "Performing Streamed Install"
    if [ "$mode" = incompatible ] && [ ! -f "$d/uninstalled" ]; then
      echo "adb: failed to install x.apk: Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match]"
      exit 1
    fi
    echo 2 > "$d/installed"; echo "Success" ;;
  *" uninstall "*) touch "$d/uninstalled"; rm -f "$d/installed"; echo "Success" ;;
  *" reverse "*) ;;
  *"dumpsys power"*)
    if [ "$mode" = asleep ]; then echo "  mWakefulness=Asleep"; else echo "  mWakefulness=Awake"; fi ;;
  *"am start"*)
    echo "Starting: Intent { act=android.intent.action.VIEW dat=http://localhost/... }"
    touch "$d/started" ;;
esac
exit 0
)SH";
    writeFile(path, script);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

QStringList adbCalls(const QString &dir)
{
    return QString::fromUtf8(readFile(dir + u"/log"_s)).split(u'\n', Qt::SkipEmptyParts);
}

int countCalls(const QStringList &calls, const QString &needle)
{
    return int(std::count_if(calls.cbegin(), calls.cend(), [&](const QString &c) { return c.contains(needle); }));
}

/// The backend's launch routes, its assetlinks and the hub, as the fields
/// below say. The launch is exchanged (and, with connect, its page's hello
/// names it) once the fake adb has written its `started` file.
class FakeBackend : public QObject
{
public:
    explicit FakeBackend(const QString &adbDir) : m_adbDir(adbDir)
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buffer = new QByteArray;
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QObject::destroyed, s, [buffer]() { delete buffer; });
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buffer]() {
                    *buffer += s->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const QByteArray head = buffer->left(end);
                    const QByteArray lower = head.toLower();
                    qsizetype length = 0;
                    const qsizetype at = lower.indexOf("content-length:");
                    if (at >= 0)
                        length = lower.mid(at + 15, lower.indexOf("\r\n", at) - at - 15).trimmed().toLongLong();
                    if (buffer->size() < end + 4 + length)
                        return;
                    const QByteArray line = head.left(head.indexOf("\r\n"));
                    lines << QString::fromLatin1(line);
                    heads << head;
                    bodies << buffer->mid(end + 4, length);
                    buffer->clear();
                    const auto [code, payload] = answer(line);
                    s->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\nContent-Type: application/json\r\n"
                             + "Content-Length: " + QByteArray::number(payload.size())
                             + "\r\nConnection: close\r\n\r\n" + payload);
                    s->disconnectFromHost();
                });
            }
        });
    }

    std::pair<int, QByteArray> answer(const QByteArray &line)
    {
        if (line.startsWith("GET /.well-known/assetlinks.json "))
            return { assetlinksStatus, assetlinksBody };
        if (line.startsWith("POST /v1/vr/launch ")) {
            // A fresh launch each time, as the backend makes them.
            ++launches;
            const QString launch = launches == 1 ? kLaunch : u"launch-%1-xyz"_s.arg(launches);
            const QString ticket = launches == 1 ? kTicket : kTicket.left(40) + u"%1"_s.arg(launches, 3, 10, QChar(u'0'));
            const QJsonObject o{ { u"ticket"_s, ticket }, { u"launch"_s, launch },
                                 { u"expires_in"_s, 60.0 }, { u"page"_s, u"/vr/?launch=immersive"_s } };
            return { 200, QJsonDocument(o).toJson(QJsonDocument::Compact) };
        }
        if (line.startsWith("GET /v1/vr/launch/")) {
            const bool started = QFileInfo::exists(m_adbDir + u"/started"_s);
            QJsonObject o{ { u"launch"_s, kLaunch }, { u"expires_in"_s, 30.0 } };
            o.insert(u"state"_s, started && launchMode != u"never" ? u"exchanged"_s : u"pending"_s);
            o.insert(u"client"_s, started && launchMode == u"connect" ? QJsonValue(u"vr-app-1"_s) : QJsonValue());
            return { 200, QJsonDocument(o).toJson(QJsonDocument::Compact) };
        }
        if (line.startsWith("GET /v1/vr/headset "))
            return { 200, launches > 0 && !headsetsOnceTicketed.isEmpty() ? headsetsOnceTicketed : headsets };
        if (line.startsWith("POST /v1/vr/headset/command "))
            return { 200, R"({"seq":1,"client":"vr-app-1"})" };
        return { 404, R"({"detail":"Not Found"})" };
    }

    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }
    int count(const QString &prefix) const
    {
        return int(std::count_if(lines.cbegin(), lines.cend(), [&](const QString &l) { return l.startsWith(prefix); }));
    }
    int find(const QString &prefix) const
    {
        for (int i = 0; i < lines.size(); ++i)
            if (lines.at(i).startsWith(prefix))
                return i;
        return -1;
    }
    int rfind(const QString &prefix) const
    {
        for (int i = int(lines.size()) - 1; i >= 0; --i)
            if (lines.at(i).startsWith(prefix))
                return i;
        return -1;
    }

    QTcpServer server;
    QStringList lines;
    QList<QByteArray> heads;
    QList<QByteArray> bodies;
    int assetlinksStatus{ 200 };
    QByteArray assetlinksBody = assetlinks();
    QByteArray headsets = R"({"headsets":[]})";
    QString launchMode = u"connect"_s;   ///< connect | noclient | never
    int launches{ 0 };
    /// The hub's answer once a ticket was issued (a page that connects meanwhile).
    QByteArray headsetsOnceTicketed;

private:
    QString m_adbDir;
};

/// A temporary adb, APK and backend, and the target that names them.
struct Rig
{
    QTemporaryDir dir;
    QString adb;
    std::unique_ptr<FakeBackend> backend;
    VrAppLauncher::Target target;

    Rig()
    {
        adb = writeFakeAdb(dir.path());
        backend = std::make_unique<FakeBackend>(dir.path());
        const QString apk = dir.path() + u"/out/visivo-vr-dev.apk"_s;
        writeFile(apk, "PK fake apk");
        writeFile(VrHeadsetApp::jsonPathFor(apk), appJson());
        target.adb = adb;
        target.serial = u"2G0YC1ZF8B07WD"_s;
        target.app = VrHeadsetApp::parseAppInfo(appJson(), apk);
        target.inputs.baseUrl = backend->url();
        target.inputs.token = kToken;
        target.inputs.sessionId = u"sess-1"_s;
        target.inputs.datasetId = u"ds1"_s;
        target.inputs.path = u"/d/WALLABY.fits"_s;
        target.link = VrLink::build(target.inputs);
    }
    void set(const QString &name, const QByteArray &value) { writeFile(dir.path() + u'/' + name, value); }
    QStringList calls() const { return adbCalls(dir.path()); }
};

struct Ended
{
    VrAppLauncher::Outcome outcome{ VrAppLauncher::Outcome::Failed };
    QString message;
    QString client;
};

/// Runs @p go on a fresh launcher and waits for its finished().
Ended runToEnd(VrAppLauncher &launcher, const std::function<void()> &go, QStringList *progress = nullptr)
{
    Ended e;
    bool done = false;
    QObject ctx;
    QObject::connect(&launcher, &VrAppLauncher::finished, &ctx,
                     [&](VrAppLauncher::Outcome o, const QString &m, const QString &c) {
                         e = { o, m, c };
                         done = true;
                     });
    if (progress)
        QObject::connect(&launcher, &VrAppLauncher::progress, &ctx,
                         [progress](const QString &t) { progress->append(t); });
    go();
    QElapsedTimer waited;
    waited.start();
    while (!done && waited.elapsed() < 15000)
        QTest::qWait(10);
    if (!done)
        e = { VrAppLauncher::Outcome::Cancelled, u"no answer within 15 s"_s, {} };
    return e;
}

#define SKIP_WITHOUT_SH()                                                                         \
    do {                                                                                          \
        if (!QFileInfo::exists(u"/bin/sh"_s))                                                     \
            QSKIP("the fake adb is a POSIX shell script");                                        \
    } while (0)

}   // namespace

// ── VrHeadsetApp ────────────────────────────────────────────────────────────

void TestVrHeadsetApp::appInfoIsReadFromTheJson()
{
    const VrHeadsetApp::AppInfo info = VrHeadsetApp::parseAppInfo(appJson(), u"/x/visivo-vr-dev.apk"_s);
    QVERIFY2(info.ok, qPrintable(info.error));
    QCOMPARE(info.package, kDevPackage);
    QCOMPARE(info.versionCode, qint64(2));
    QCOMPARE(info.jsonPath, u"/x/visivo-vr-dev.json"_s);
    QVERIFY(info.development);
    QVERIFY(!VrHeadsetApp::parseAppInfo(appJson(u"it.inaf.oact.visivo.vr"_s), u"/x/visivo-vr.apk"_s).development);

    // A package that could break the device's shell line, no version, no
    // certificate, not JSON: unusable, said why.
    QVERIFY(!VrHeadsetApp::parseAppInfo(appJson(u"it.inaf;rm -rf /"_s), u"/x/a.apk"_s).ok);
    QVERIFY(!VrHeadsetApp::parseAppInfo(appJson(u"single"_s), u"/x/a.apk"_s).ok);
    QVERIFY(!VrHeadsetApp::parseAppInfo(appJson(kDevPackage, 0), u"/x/a.apk"_s).ok);
    QVERIFY(!VrHeadsetApp::parseAppInfo(appJson(kDevPackage, 2, u"AB:CD"_s), u"/x/a.apk"_s).ok);
    const VrHeadsetApp::AppInfo broken = VrHeadsetApp::parseAppInfo("{nope", u"/x/a.apk"_s);
    QVERIFY(!broken.ok);
    QVERIFY(!broken.error.isEmpty());
    // Both spellings of a fingerprint are the same fingerprint.
    QCOMPARE(VrHeadsetApp::normalizedFingerprint(kCert),
             VrHeadsetApp::normalizedFingerprint(kCert.toLower().remove(u':')));
}

void TestVrHeadsetApp::apkIsFoundInOrder()
{
    QTemporaryDir tmp;
    const QString repo = tmp.path() + u"/repo"_s;
    const QString exeDir = repo + u"/build/VisIVO.app/Contents/MacOS"_s;
    QDir().mkpath(exeDir);
    const QProcessEnvironment none;
    using AdbHeadset::Platform;

    // Nothing anywhere: no app, and the browser is the way.
    QVERIFY(!VrHeadsetApp::findApp(Platform::MacOS, exeDir, none).ok);

    // A development build in the source tree the build directory sits in.
    const QString dev = repo + u"/tools/quest-app/out/visivo-vr-dev.apk"_s;
    QVERIFY(writeFile(dev, "PK"));
    QVERIFY(writeFile(VrHeadsetApp::jsonPathFor(dev), appJson()));
    QCOMPARE(VrHeadsetApp::findApp(Platform::MacOS, exeDir, none).apkPath, QDir::cleanPath(dev));

    // An APK without its .json is not one: the package would be a guess.
    const QString bundled = repo + u"/build/VisIVO.app/Contents/Resources/quest-app/visivo-vr.apk"_s;
    QVERIFY(writeFile(bundled, "PK"));
    QCOMPARE(VrHeadsetApp::findApp(Platform::MacOS, exeDir, none).apkPath, QDir::cleanPath(dev));
    // With it, the bundle's comes before the source tree's.
    QVERIFY(writeFile(VrHeadsetApp::jsonPathFor(bundled), appJson(u"it.inaf.oact.visivo.vr"_s)));
    QCOMPARE(VrHeadsetApp::findApp(Platform::MacOS, exeDir, none).package, u"it.inaf.oact.visivo.vr"_s);

    // $VISIVO_QUEST_APK first of all; a missing file there is said, not skipped.
    const QString own = tmp.path() + u"/mine/visivo-vr-dev.apk"_s;
    QVERIFY(writeFile(own, "PK"));
    QVERIFY(writeFile(VrHeadsetApp::jsonPathFor(own), appJson(kDevPackage, 7)));
    QProcessEnvironment env;
    env.insert(u"VISIVO_QUEST_APK"_s, own);
    QCOMPARE(VrHeadsetApp::findApp(Platform::MacOS, exeDir, env).versionCode, qint64(7));
    env.insert(u"VISIVO_QUEST_APK"_s, tmp.path() + u"/missing.apk"_s);
    const VrHeadsetApp::AppInfo missing = VrHeadsetApp::findApp(Platform::MacOS, exeDir, env);
    QVERIFY(!missing.ok);
    QVERIFY(missing.error.contains(u"VISIVO_QUEST_APK"_s));

    // Elsewhere the resources are beside the executable.
    const QStringList linux = VrHeadsetApp::apkCandidates(Platform::Linux, u"/opt/visivo/bin"_s, none);
    QCOMPARE(linux.first(), u"/opt/visivo/bin/quest-app/visivo-vr.apk"_s);
    QVERIFY(!linux.join(u'\n').contains(u"Resources"_s));
}

void TestVrHeadsetApp::installedVersionNeedsTheExactPackage()
{
    const QString out = u"package:it.inaf.oact.visivo.vr.dev versionCode:3\n"
                        u"package:it.inaf.oact.visivo.vr versionCode:12\r\n"_s;
    QCOMPARE(VrHeadsetApp::installedVersion(out, u"it.inaf.oact.visivo.vr"_s), qint64(12));
    QCOMPARE(VrHeadsetApp::installedVersion(out, kDevPackage), qint64(3));
    // `pm list packages it.inaf.oact.visivo.vr` lists the .dev app too: the
    // release app is still absent.
    QCOMPARE(VrHeadsetApp::installedVersion(u"package:it.inaf.oact.visivo.vr.dev versionCode:3\n"_s,
                                            u"it.inaf.oact.visivo.vr"_s),
             qint64(-1));
    QCOMPARE(VrHeadsetApp::installedVersion(QString(), kDevPackage), qint64(-1));
    // Listed without a version (an old pm): older than anything we ship.
    QCOMPARE(VrHeadsetApp::installedVersion(u"package:it.inaf.oact.visivo.vr.dev\n"_s, kDevPackage), qint64(0));

    const QStringList args = VrHeadsetApp::versionQueryArgs(u"SER"_s, kDevPackage);
    QCOMPARE(args, QStringList({ u"-s"_s, u"SER"_s, u"shell"_s, u"pm"_s, u"list"_s, u"packages"_s,
                                 u"--show-versioncode"_s, u"'it.inaf.oact.visivo.vr.dev'"_s }));
}

void TestVrHeadsetApp::installStateComparesVersions()
{
    using VrHeadsetApp::Installed;
    QCOMPARE(VrHeadsetApp::installedState(-1, 2), Installed::Absent);
    QCOMPARE(VrHeadsetApp::installedState(1, 2), Installed::Older);
    QCOMPARE(VrHeadsetApp::installedState(2, 2), Installed::Current);
    QCOMPARE(VrHeadsetApp::installedState(5, 2), Installed::Current);
}

void TestVrHeadsetApp::installOutputIsUnderstood()
{
    using O = VrHeadsetApp::InstallResult::Outcome;
    QCOMPARE(VrHeadsetApp::parseInstall(0, u"Performing Streamed Install\nSuccess\n"_s).outcome, O::Success);
    const auto incompatible = VrHeadsetApp::parseInstall(
            1, u"Performing Streamed Install\nadb: failed to install a.apk: Failure "
               u"[INSTALL_FAILED_UPDATE_INCOMPATIBLE: Package it.inaf.oact.visivo.vr signatures do not match]\n"_s);
    QCOMPARE(incompatible.outcome, O::UpdateIncompatible);
    const auto full = VrHeadsetApp::parseInstall(1, u"Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE]\n"_s);
    QCOMPARE(full.outcome, O::Failed);
    QCOMPARE(full.message, u"INSTALL_FAILED_INSUFFICIENT_STORAGE"_s);
    // "Success" somewhere but not last, or a failing exit, is not success.
    QCOMPARE(VrHeadsetApp::parseInstall(0, u"Success\nerror: device offline\n"_s).outcome, O::Failed);
    QCOMPARE(VrHeadsetApp::parseInstall(1, u"Success\n"_s).outcome, O::Failed);
    QVERIFY(VrHeadsetApp::uninstalled(0, u"Success\n"_s));
    QVERIFY(!VrHeadsetApp::uninstalled(0, u"Failure [DELETE_FAILED_INTERNAL_ERROR]\n"_s));
    QCOMPARE(VrHeadsetApp::installArgs(u"S"_s, u"/a b/visivo-vr.apk"_s),
             QStringList({ u"-s"_s, u"S"_s, u"install"_s, u"-r"_s, u"/a b/visivo-vr.apk"_s }));
    QCOMPARE(VrHeadsetApp::uninstallArgs(u"S"_s, kDevPackage),
             QStringList({ u"-s"_s, u"S"_s, u"uninstall"_s, kDevPackage }));
}

void TestVrHeadsetApp::assetlinksMustListPackageAndCertificate()
{
    using VrHeadsetApp::Listing;
    QCOMPARE(VrHeadsetApp::parseAssetlinks(200, assetlinks(), kDevPackage, kCert), Listing::Listed);
    // The same fingerprint spelt without colons, in lower case.
    QCOMPARE(VrHeadsetApp::parseAssetlinks(200, assetlinks(kDevPackage, kCert.toLower().remove(u':')),
                                           kDevPackage, kCert),
             Listing::Listed);
    // The release app listed, the development one not; another key.
    QCOMPARE(VrHeadsetApp::parseAssetlinks(200, assetlinks(u"it.inaf.oact.visivo.vr"_s), kDevPackage, kCert),
             Listing::NotListed);
    QString other = kCert;
    other.replace(0, 2, u"00"_s);
    QCOMPARE(VrHeadsetApp::parseAssetlinks(200, assetlinks(kDevPackage, other), kDevPackage, kCert),
             Listing::NotListed);
    // The right package and key, but not a statement that hands URLs to an
    // Android app: the headset's browser would refuse the app.
    QJsonArray links = QJsonDocument::fromJson(assetlinks()).array();
    QJsonObject st = links.at(0).toObject();
    st.insert(u"relation"_s, QJsonArray{ u"delegate_permission/common.get_login_creds"_s });
    QCOMPARE(VrHeadsetApp::parseAssetlinks(200, QJsonDocument(QJsonArray{ st }).toJson(), kDevPackage, kCert),
             Listing::NotListed);
    st = links.at(0).toObject();
    QJsonObject target = st.value(u"target"_s).toObject();
    target.insert(u"namespace"_s, u"web"_s);
    st.insert(u"target"_s, target);
    QCOMPARE(VrHeadsetApp::parseAssetlinks(200, QJsonDocument(QJsonArray{ st }).toJson(), kDevPackage, kCert),
             Listing::NotListed);
    // No statement at all (a backend without the app) and unreadable answers.
    QCOMPARE(VrHeadsetApp::parseAssetlinks(404, R"({"detail":"Not found"})", kDevPackage, kCert),
             Listing::NotListed);
    QCOMPARE(VrHeadsetApp::parseAssetlinks(0, {}, kDevPackage, kCert), Listing::Unreadable);
    QCOMPARE(VrHeadsetApp::parseAssetlinks(200, "<html>", kDevPackage, kCert), Listing::Unreadable);
    QCOMPARE(VrHeadsetApp::parseAssetlinks(302, {}, kDevPackage, kCert), Listing::Unreadable);

    VrHeadsetApp::AppInfo dev = VrHeadsetApp::parseAppInfo(appJson(), u"/a.apk"_s);
    QVERIFY(VrHeadsetApp::notListedMessage(dev).contains(u"development key"_s));
    dev.development = false;
    QVERIFY(VrHeadsetApp::notListedMessage(dev).contains(u"vr_app.json"_s));
}

void TestVrHeadsetApp::assetlinksAreAskedAtTheOrigin()
{
    // Digital Asset Links are per origin: a path prefix is not part of it.
    QCOMPARE(VrHeadsetApp::assetlinksUrl(u"http://127.0.0.1:8000"_s),
             u"http://127.0.0.1:8000/.well-known/assetlinks.json"_s);
    QCOMPARE(VrHeadsetApp::assetlinksUrl(u"http://localhost:18960/portal/visivo/"_s),
             u"http://localhost:18960/.well-known/assetlinks.json"_s);
    QVERIFY(VrHeadsetApp::assetlinksUrl(u"not a url"_s).isEmpty());
}

void TestVrHeadsetApp::wakefulnessIsRead()
{
    using VrHeadsetApp::Wake;
    QCOMPARE(VrHeadsetApp::parseWakefulness(u"  mWakefulness=Awake\n"_s), Wake::Awake);
    QCOMPARE(VrHeadsetApp::parseWakefulness(u"  mWakefulness=Asleep\n"_s), Wake::NotAwake);
    QCOMPARE(VrHeadsetApp::parseWakefulness(u"mWakefulness=Dozing"_s), Wake::NotAwake);
    QCOMPARE(VrHeadsetApp::parseWakefulness(u"mWakefulness=Dreaming"_s), Wake::NotAwake);
    QCOMPARE(VrHeadsetApp::parseWakefulness(QString()), Wake::Unknown);
    QCOMPARE(VrHeadsetApp::wakefulnessArgs(u"S"_s),
             QStringList({ u"-s"_s, u"S"_s, u"shell"_s, u"dumpsys power | grep mWakefulness="_s }));
    QCOMPARE(VrHeadsetApp::putHeadsetOnMessage(), u"Put the headset on, then press Open again."_s);
}

void TestVrHeadsetApp::launchBodyCarriesTheCubeButNoToken()
{
    VrLink::Inputs in;
    in.token = kToken;
    in.sessionId = u"sess-1"_s;
    in.datasetId = u"ds1"_s;
    in.path = u"/d/m33.fits"_s;
    const QJsonObject body = VrHeadsetApp::launchRequestBody(in);
    const QJsonObject cube = body.value(u"cube"_s).toObject();
    QCOMPARE(cube.value(u"dataset_id"_s).toString(), u"ds1"_s);
    QCOMPARE(cube.value(u"path"_s).toString(), u"/d/m33.fits"_s);
    QCOMPARE(cube.value(u"kind"_s).toString(), u"cube"_s);
    QVERIFY(!cube.contains(u"box"_s));
    const QByteArray text = QJsonDocument(body).toJson();
    QVERIFY(!text.contains(kToken.toUtf8()));
    QVERIFY(!text.contains("sess-1"));
    in.kind = u"points"_s;
    QCOMPARE(VrHeadsetApp::launchRequestBody(in).value(u"cube"_s).toObject().value(u"kind"_s).toString(),
             u"points"_s);
    // No cube: the app's connected empty room.
    in.datasetId.clear();
    QVERIFY(VrHeadsetApp::launchRequestBody(in).isEmpty());
}

void TestVrHeadsetApp::ticketAnswerIsChecked()
{
    const QByteArray good = QJsonDocument(QJsonObject{ { u"ticket"_s, kTicket }, { u"launch"_s, kLaunch },
                                                       { u"expires_in"_s, 60.0 },
                                                       { u"page"_s, u"/vr/?launch=immersive"_s } })
                                    .toJson();
    const VrHeadsetApp::Ticket t = VrHeadsetApp::parseTicket(200, good);
    QVERIFY2(t.ok, qPrintable(t.error));
    QCOMPARE(t.ticket, kTicket);
    QCOMPARE(t.launch, kLaunch);
    QCOMPARE(t.page, u"/vr/?launch=immersive"_s);

    // An older backend: no launch route.
    const VrHeadsetApp::Ticket old = VrHeadsetApp::parseTicket(404, R"({"detail":"Not Found"})");
    QVERIFY(!old.ok);
    QVERIFY(old.unsupported);
    // A refusal says why; no answer says Qt's words.
    QVERIFY(VrHeadsetApp::parseTicket(401, R"({"detail":"Invalid token"})").error.contains(u"Invalid token"_s));
    QVERIFY(VrHeadsetApp::parseTicket(0, {}, u"Connection refused"_s).error.contains(u"Connection refused"_s));
    // Anything that could break out of the shell's quotes is refused.
    for (const char *bad : { R"({"ticket":"abc'def-1234567890123","launch":"sBYiD_0jN5yb","page":"/vr/"})",
                             R"({"ticket":"Qx7vAbc_def-1234567890","launch":"sB YiD","page":"/vr/"})",
                             R"({"ticket":"Qx7vAbc_def-1234567890","launch":"sBYiD_0jN5yb","page":"/vr/' ; reboot '"})",
                             R"({"ticket":"Qx7vAbc_def-1234567890","launch":"sBYiD_0jN5yb","page":"/vr/#x"})",
                             R"({"ticket":"short","launch":"sBYiD_0jN5yb","page":"/vr/"})" }) {
        const VrHeadsetApp::Ticket r = VrHeadsetApp::parseTicket(200, bad);
        QVERIFY2(!r.ok, bad);
        QVERIFY(r.ticket.isEmpty());
    }
}

void TestVrHeadsetApp::amStartIsTheContractsCommand()
{
    const QString url = VrHeadsetApp::launchUrl(8781, u"/portal"_s, u"/vr/?launch=immersive"_s, kTicket);
    QCOMPARE(url, u"http://localhost:8781/portal/vr/?launch=immersive#ticket="_s + kTicket);
    QCOMPARE(VrHeadsetApp::launchUrl(8000, QString(), u"/vr/?launch=immersive"_s, kTicket),
             u"http://localhost:8000/vr/?launch=immersive#ticket="_s + kTicket);
    const QStringList args = VrHeadsetApp::amStartArgs(u"SER"_s, kDevPackage, url);
    // One shell argument, exactly the contract's line: adb hands it to the
    // device's shell as it is.
    QCOMPARE(args.size(), 4);
    QCOMPARE(args.mid(0, 3), QStringList({ u"-s"_s, u"SER"_s, u"shell"_s }));
    QCOMPARE(args.at(3), u"am start -a android.intent.action.VIEW -n it.inaf.oact.visivo.vr.dev/.LauncherActivity "
                         u"-d 'http://localhost:8781/portal/vr/?launch=immersive#ticket="_s
                                 + kTicket + u"'"_s);
    QVERIFY(!args.join(u' ').contains(kToken));
}

void TestVrHeadsetApp::ticketsAreRedacted()
{
    const QString line = u"Starting: Intent { dat=http://localhost:8000/vr/?launch=immersive#ticket="_s + kTicket
            + u" }"_s;
    const QString out = VrHeadsetApp::redactTicket(line);
    QVERIFY(!out.contains(kTicket));
    QVERIFY(out.contains(u"ticket=<redacted>"_s));
    QCOMPARE(VrHeadsetApp::redactTicket(u"nothing here"_s), u"nothing here"_s);
}

void TestVrHeadsetApp::launchStatusIsParsed()
{
    auto s = VrHeadsetApp::parseLaunchStatus(
            200, R"({"launch":"sBYiD_0jN5yb","state":"exchanged","expires_in":0,"exchanged_at":1.5,"client":"vr-1"})");
    QVERIFY(s.ok);
    QCOMPARE(s.state, u"exchanged"_s);
    QCOMPARE(s.client, u"vr-1"_s);
    s = VrHeadsetApp::parseLaunchStatus(200, R"({"state":"pending","client":null})");
    QVERIFY(s.ok);
    QVERIFY(s.client.isEmpty());
    QVERIFY(VrHeadsetApp::parseLaunchStatus(404, R"({"detail":"Unknown launch."})").unknown);
    QVERIFY(!VrHeadsetApp::parseLaunchStatus(200, R"({"state":"weird"})").ok);
    QVERIFY(!VrHeadsetApp::parseLaunchStatus(0, {}, u"timeout"_s).ok);
}

void TestVrHeadsetApp::trackerReachesInVr()
{
    using P = VrHeadsetApp::LaunchTracker::Phase;
    VrHeadsetApp::LaunchTracker t;
    QCOMPARE(t.phase(), P::Idle);
    QVERIFY(!t.active());
    t.begin(kLaunch, 1000);
    QVERIFY(t.active());
    QCOMPARE(t.message(), u"Starting on the headset…"_s);
    t.polled(VrHeadsetApp::parseLaunchStatus(200, R"({"state":"pending"})"), 1300);
    QCOMPARE(t.phase(), P::Starting);
    // Measured on a Quest 3: exchanged at 1.4 s, the hello 0.1 s later.
    t.polled(VrHeadsetApp::parseLaunchStatus(200, R"({"state":"exchanged"})"), 2400);
    QCOMPARE(t.phase(), P::Connecting);
    QCOMPARE(t.message(), u"Starting on the headset…"_s);
    t.polled(VrHeadsetApp::parseLaunchStatus(200, R"({"state":"exchanged","client":"vr-1"})"), 2500);
    QCOMPARE(t.phase(), P::InVr);
    QCOMPARE(t.client(), u"vr-1"_s);
    QCOMPARE(t.message(), u"In VR."_s);
    QVERIFY(!t.active());
    // The hello may be seen in the very first answer.
    t.begin(kLaunch, 0);
    t.polled(VrHeadsetApp::parseLaunchStatus(200, R"({"state":"exchanged","client":"vr-2"})"), 600);
    QCOMPARE(t.phase(), P::InVr);
}

void TestVrHeadsetApp::trackerTimesOutWithoutExchange()
{
    using P = VrHeadsetApp::LaunchTracker::Phase;
    VrHeadsetApp::LaunchTracker t;
    t.begin(kLaunch, 0);
    t.tick(7999);
    QCOMPARE(t.phase(), P::Starting);
    t.tick(8000);
    QCOMPARE(t.phase(), P::Asleep);
    QCOMPARE(t.message(), u"Put the headset on, then press Open again."_s);
    // Expired before 8 s: the same.
    t.begin(kLaunch, 0);
    t.polled(VrHeadsetApp::parseLaunchStatus(200, R"({"state":"expired"})"), 3000);
    QCOMPARE(t.phase(), P::Asleep);
    // Failed polls do not stop the clock.
    t.begin(kLaunch, 0);
    t.polled(VrHeadsetApp::parseLaunchStatus(0, {}, u"refused"_s), 9000);
    QCOMPARE(t.phase(), P::Asleep);
    // The backend forgot the launch.
    t.begin(kLaunch, 0);
    t.polled(VrHeadsetApp::parseLaunchStatus(404, {}), 100);
    QCOMPARE(t.phase(), P::Lost);
}

void TestVrHeadsetApp::trackerTimesOutWithoutHello()
{
    using P = VrHeadsetApp::LaunchTracker::Phase;
    VrHeadsetApp::LaunchTracker t;
    t.begin(kLaunch, 0);
    // Exchanged at 7.9 s: the 10 s for the hello start then, not at the launch.
    t.polled(VrHeadsetApp::parseLaunchStatus(200, R"({"state":"exchanged"})"), 7900);
    t.tick(9000);
    QCOMPARE(t.phase(), P::Connecting);
    t.polled(VrHeadsetApp::parseLaunchStatus(0, {}, u"connection refused"_s), 17000);
    QCOMPARE(t.phase(), P::Connecting);
    t.tick(17900);
    QCOMPARE(t.phase(), P::NoConnect);
    QVERIFY(t.message().startsWith(u"VisIVO VR started but could not connect to this computer"_s));
    QVERIFY(t.message().contains(u"connection refused"_s));
    // Shorter timeouts, as the launcher's tests use.
    VrHeadsetApp::LaunchTracker quick(100, 200);
    quick.begin(kLaunch, 0);
    quick.tick(100);
    QCOMPARE(quick.phase(), P::Asleep);
}

void TestVrHeadsetApp::trackerIgnoresAnswersAfterTheEnd()
{
    using P = VrHeadsetApp::LaunchTracker::Phase;
    VrHeadsetApp::LaunchTracker t;
    t.begin(kLaunch, 0);
    t.tick(8000);
    QCOMPARE(t.phase(), P::Asleep);
    // A poll that was still out when the timeout came: the user has been told.
    t.polled(VrHeadsetApp::parseLaunchStatus(200, R"({"state":"exchanged","client":"vr-1"})"), 8100);
    QCOMPARE(t.phase(), P::Asleep);
    QVERIFY(t.client().isEmpty());
    t.reset();
    QCOMPARE(t.phase(), P::Idle);
}

void TestVrHeadsetApp::gateSendsOneLaunchPerTicket()
{
    VrHeadsetApp::LaunchGate gate;
    QVERIFY(gate.claim(kLaunch));
    QVERIFY(!gate.claim(kLaunch));
    QVERIFY(gate.claimed(kLaunch));
    QVERIFY(gate.claim(u"another1234"_s));
    QVERIFY(!gate.claim(QString()));
}

void TestVrHeadsetApp::aConnectedPageIsShownNotLaunchedAgain()
{
    using VrHeadsetApp::OpenAction;
    const QList<VrHub::Headset> none;
    // While a launch is followed: nothing, whatever else is true.
    QCOMPARE(VrHeadsetApp::decideOpen(true, true, none).action, OpenAction::Busy);
    // Nothing connected: launch the app, or the browser without it.
    QCOMPARE(VrHeadsetApp::decideOpen(false, true, none).action, OpenAction::Launch);
    QCOMPARE(VrHeadsetApp::decideOpen(false, false, none).action, OpenAction::Browser);

    // The app's page (its hello named a launch) is chosen over newer pages.
    const QList<VrHub::Headset> mixed{ headset(u"pair-1"_s, u"pair"_s), headset(u"usb-1"_s, u"adb"_s),
                                       headset(u"app-1"_s, u"adb"_s, kLaunch) };
    auto d = VrHeadsetApp::decideOpen(false, true, mixed);
    QCOMPARE(d.action, OpenAction::Show);
    QCOMPARE(d.client, u"app-1"_s);
    // Then a USB page, then any page: with the app, never a second launch.
    d = VrHeadsetApp::decideOpen(false, true, { headset(u"pair-1"_s, u"pair"_s), headset(u"usb-1"_s, u"adb"_s) });
    QCOMPARE(d.client, u"usb-1"_s);
    d = VrHeadsetApp::decideOpen(false, true, { headset(u"pair-1"_s, u"pair"_s) });
    QCOMPARE(d.action, OpenAction::Show);
    QCOMPARE(d.client, u"pair-1"_s);
    // Without the app a page paired by code (another headset, maybe) is left
    // alone and this headset opens the browser; the app's or a USB page still
    // takes the cube.
    QCOMPARE(VrHeadsetApp::decideOpen(false, false, { headset(u"pair-1"_s, u"pair"_s) }).action,
             OpenAction::Browser);
    d = VrHeadsetApp::decideOpen(false, false, mixed);
    QCOMPARE(d.action, OpenAction::Show);
    QCOMPARE(d.client, u"app-1"_s);
}

void TestVrHeadsetApp::presenceCarriesTheLaunch()
{
    const VrHub::Presence p = VrHub::parsePresence(
            200, R"({"headsets":[{"client":"vr-1","transport":"adb","in_vr":true,"launch":"sBYiD_0jN5yb"},
                                 {"client":"vr-2","transport":"pair","launch":null}]})");
    QVERIFY(p.ok);
    QCOMPARE(p.headsets.at(0).launch, kLaunch);
    QVERIFY(p.headsets.at(1).launch.isEmpty());
    QCOMPARE(VrHeadsetApp::runningClient(p.headsets), u"vr-1"_s);
}

// ── VrAppLauncher ───────────────────────────────────────────────────────────

void TestVrHeadsetApp::launcherInstallsThenLaunchesOnce()
{
    SKIP_WITHOUT_SH();
    Rig rig;
    QVERIFY(rig.backend->server.isListening());
    VrAppLauncher launcher;
    launcher.setTimings(3000, 3000, 30);
    QList<VrHeadsetApp::Installed> states;
    connect(&launcher, &VrAppLauncher::checked, this,
            [&states](VrHeadsetApp::Installed s, qint64, const QString &) { states << s; });
    QStringList progress;
    const Ended e = runToEnd(launcher, [&]() { launcher.open(rig.target); }, &progress);
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::InVr);
    QCOMPARE(e.client, u"vr-app-1"_s);
    QVERIFY(!launcher.busy());
    QCOMPARE(states.first(), VrHeadsetApp::Installed::Absent);
    QCOMPARE(states.last(), VrHeadsetApp::Installed::Current);

    // The order of the contract, and exactly one am start.
    const QStringList calls = rig.calls();
    QCOMPARE(countCalls(calls, u"am start"_s), 1);
    QCOMPARE(countCalls(calls, u" install -r "_s), 1);
    const auto at = [&calls](const QString &needle) {
        for (int i = 0; i < calls.size(); ++i)
            if (calls.at(i).contains(needle))
                return i;
        return -1;
    };
    QVERIFY(at(u"pm list packages"_s) < at(u" install -r "_s));
    QVERIFY(at(u" install -r "_s) < at(u" reverse tcp:"_s));
    QVERIFY(at(u" reverse tcp:"_s) < at(u"dumpsys power"_s));
    QVERIFY(at(u"dumpsys power"_s) < at(u"am start"_s));
    const QString am = calls.at(at(u"am start"_s));
    QVERIFY(am.contains(u"-n it.inaf.oact.visivo.vr.dev/.LauncherActivity"_s));
    QVERIFY(am.contains(u"'http://localhost:%1/vr/?launch=immersive#ticket=%2'"_s.arg(rig.target.link.port)
                                .arg(kTicket)));
    // The token never goes to adb, the ticket never to the user.
    QVERIFY(!calls.join(u'\n').contains(kToken));
    for (const QString &p : progress)
        QVERIFY2(!p.contains(kTicket), qPrintable(p));
    QVERIFY(progress.contains(u"Installing VisIVO VR on the headset…"_s));
    QVERIFY(progress.contains(u"Starting on the headset…"_s));
    QVERIFY(launcher.redact(am).contains(u"ticket=<redacted>"_s));
    QVERIFY(!launcher.redact(am).contains(kTicket));

    // The backend: assetlinks before the ticket, the ticket before the hub
    // check, and the launch's cube in the ticket's body.
    FakeBackend &b = *rig.backend;
    QVERIFY(b.find(u"GET /.well-known/assetlinks.json "_s) >= 0);
    QVERIFY(b.find(u"GET /.well-known/assetlinks.json "_s) < b.find(u"POST /v1/vr/launch "_s));
    // The hub asked first, and again between the ticket and the launch.
    QVERIFY(b.find(u"GET /v1/vr/headset "_s) < b.find(u"GET /.well-known/assetlinks.json "_s));
    QVERIFY(b.rfind(u"GET /v1/vr/headset "_s) > b.find(u"POST /v1/vr/launch "_s));
    QCOMPARE(b.count(u"GET /v1/vr/headset "_s), 2);
    QVERIFY(b.find(u"GET /v1/vr/launch/sBYiD_0jN5yb "_s) > 0);
    const int post = b.find(u"POST /v1/vr/launch "_s);
    QVERIFY(b.heads.at(post).toLower().contains("x-visivo-token: tok-secret-xyz"));
    QVERIFY(b.heads.at(post).toLower().contains("x-visivo-session: sess-1"));
    QCOMPARE(QJsonDocument::fromJson(b.bodies.at(post)).object().value(u"cube"_s).toObject().value(u"path"_s).toString(),
             u"/d/WALLABY.fits"_s);
    QCOMPARE(b.count(u"POST /v1/vr/launch "_s), 1);

    // Open again with the app current: no install this time.
    rig.set(u"log"_s, QByteArray());
    QFile::remove(rig.dir.path() + u"/started"_s);
    const Ended again = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(again.outcome, VrAppLauncher::Outcome::InVr);
    QCOMPARE(countCalls(rig.calls(), u" install -r "_s), 0);
}

void TestVrHeadsetApp::launcherShowsInsteadOfASecondLaunch()
{
    SKIP_WITHOUT_SH();
    Rig rig;
    rig.set(u"installed"_s, "2");
    rig.backend->headsets = R"({"headsets":[{"client":"vr-app-1","transport":"adb","in_vr":true,"launch":"old12345"}]})";
    VrAppLauncher launcher;
    launcher.setTimings(3000, 3000, 30);
    const Ended e = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::Shown);
    QCOMPARE(e.client, u"vr-app-1"_s);
    // Nothing on the headset, no ticket: an asleep headset still gets the cube.
    QVERIFY(rig.calls().isEmpty());
    QCOMPARE(rig.backend->count(u"POST /v1/vr/launch "_s), 0);
    const int cmd = rig.backend->find(u"POST /v1/vr/headset/command "_s);
    QVERIFY(cmd >= 0);
    const QJsonObject body = QJsonDocument::fromJson(rig.backend->bodies.at(cmd)).object();
    QCOMPARE(body.value(u"type"_s).toString(), u"show"_s);
    QCOMPARE(body.value(u"client"_s).toString(), u"vr-app-1"_s);
    QCOMPARE(body.value(u"dataset_id"_s).toString(), u"ds1"_s);

    // A second Open while a launch is followed starts nothing.
    rig.backend->headsets = R"({"headsets":[]})";
    rig.backend->launchMode = u"never"_s;
    launcher.setTimings(600, 600, 30);
    QSignalSpy ended(&launcher, &VrAppLauncher::finished);
    launcher.open(rig.target);
    QTRY_VERIFY_WITH_TIMEOUT(launcher.following(), 10000);
    launcher.open(rig.target);
    launcher.install(rig.target);
    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 10000);
    QCOMPARE(countCalls(rig.calls(), u"am start"_s), 1);
    QCOMPARE(rig.backend->count(u"POST /v1/vr/launch "_s), 1);

    // A page that connects between the ticket and the launch (started from
    // its icon, say) gets the cube: the ticket is left to expire.
    rig.backend->headsetsOnceTicketed = R"({"headsets":[{"client":"vr-icon","transport":"adb","in_vr":true}]})";
    rig.backend->launches = 0;   // the hub's first answer: nobody there yet
    rig.set(u"log"_s, QByteArray());
    const Ended late = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(late.outcome, VrAppLauncher::Outcome::Shown);
    QCOMPARE(countCalls(rig.calls(), u"am start"_s), 0);
    QCOMPARE(countCalls(rig.calls(), u"dumpsys power"_s), 1);   // it got as far as the launch
    QCOMPARE(rig.backend->count(u"POST /v1/vr/launch "_s), 2);
    const int show = rig.backend->rfind(u"POST /v1/vr/headset/command "_s);
    QCOMPARE(QJsonDocument::fromJson(rig.backend->bodies.at(show)).object().value(u"client"_s).toString(),
             u"vr-icon"_s);
}

void TestVrHeadsetApp::launcherAsksBeforeUninstalling()
{
    SKIP_WITHOUT_SH();
    Rig rig;
    rig.set(u"installed"_s, "1");
    rig.set(u"mode"_s, "incompatible");
    VrAppLauncher launcher;
    launcher.setTimings(3000, 3000, 30);
    QStringList asked;
    connect(&launcher, &VrAppLauncher::uninstallRequested, this, [&](const QString &package) {
        asked << package;
        launcher.answerUninstall(false);
    });
    Ended e = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(asked, QStringList{ kDevPackage });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::Declined);
    QCOMPARE(countCalls(rig.calls(), u" uninstall "_s), 0);
    QCOMPARE(countCalls(rig.calls(), u"am start"_s), 0);

    // Yes: uninstalled, installed, launched; asked once.
    launcher.disconnect(this);
    asked.clear();
    connect(&launcher, &VrAppLauncher::uninstallRequested, this, [&](const QString &package) {
        asked << package;
        launcher.answerUninstall(true);
    });
    e = runToEnd(launcher, [&]() { launcher.install(rig.target); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::Installed);
    QCOMPARE(asked.size(), 1);
    QCOMPARE(countCalls(rig.calls(), u" uninstall it.inaf.oact.visivo.vr.dev"_s), 1);
    QCOMPARE(countCalls(rig.calls(), u"am start"_s), 0);   // install() alone never launches
}

void TestVrHeadsetApp::aCheckedReceiverMayStartTheNextRun()
{
    SKIP_WITHOUT_SH();
    Rig rig;
    rig.set(u"installed"_s, "1");   // older than the APK's 2
    VrAppLauncher launcher;
    VrAppLauncher::Target newer = rig.target;
    newer.app.versionCode = 2;
    bool first = true;
    connect(&launcher, &VrAppLauncher::checked, this, [&](VrHeadsetApp::Installed state, qint64, const QString &) {
        // The check alone ends idle: an install started from its answer is
        // a run of its own, which the check's callback must not steer.
        if (first) {
            first = false;
            QCOMPARE(state, VrHeadsetApp::Installed::Older);
            QVERIFY(!launcher.busy());
            launcher.install(newer);
        }
    });
    const Ended e = runToEnd(launcher, [&]() { launcher.check(rig.target); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::Installed);
    QCOMPARE(countCalls(rig.calls(), u" install -r "_s), 1);
}

void TestVrHeadsetApp::launcherWaitsForAnAsleepHeadset()
{
    SKIP_WITHOUT_SH();
    Rig rig;
    rig.set(u"installed"_s, "2");
    rig.set(u"mode"_s, "asleep");
    VrAppLauncher launcher;
    const Ended e = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::Asleep);
    QCOMPARE(e.message, u"Put the headset on, then press Open again."_s);
    // Nothing launched, no ticket wasted.
    QCOMPARE(countCalls(rig.calls(), u"am start"_s), 0);
    QCOMPARE(rig.backend->count(u"POST /v1/vr/launch "_s), 0);
}

void TestVrHeadsetApp::launcherSaysPutTheHeadsetOnWhenTheLaunchIsDropped()
{
    SKIP_WITHOUT_SH();
    Rig rig;
    rig.set(u"installed"_s, "2");
    rig.backend->launchMode = u"never"_s;
    VrAppLauncher launcher;
    launcher.setTimings(400, 400, 30);
    const Ended e = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::Asleep);
    QCOMPARE(e.message, u"Put the headset on, then press Open again."_s);
    QCOMPARE(countCalls(rig.calls(), u"am start"_s), 1);
    // Polled while it waited, then left alone.
    const int polls = rig.backend->count(u"GET /v1/vr/launch/"_s);
    QVERIFY(polls >= 2);
    QTest::qWait(150);
    QCOMPARE(rig.backend->count(u"GET /v1/vr/launch/"_s), polls);
}

void TestVrHeadsetApp::launcherSaysWhenThePageNeverConnects()
{
    SKIP_WITHOUT_SH();
    Rig rig;
    rig.set(u"installed"_s, "2");
    rig.backend->launchMode = u"noclient"_s;
    VrAppLauncher launcher;
    launcher.setTimings(2000, 300, 30);
    const Ended e = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::NoConnect);
    QVERIFY(e.message.startsWith(u"VisIVO VR started but could not connect to this computer"_s));
    QCOMPARE(countCalls(rig.calls(), u"am start"_s), 1);
}

void TestVrHeadsetApp::launcherRefusesABackendThatDoesNotListTheApp()
{
    SKIP_WITHOUT_SH();
    Rig rig;
    rig.set(u"installed"_s, "2");
    rig.backend->assetlinksStatus = 404;
    rig.backend->assetlinksBody = R"({"detail":"Not found"})";
    VrAppLauncher launcher;
    Ended e = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::NotListed);
    QVERIFY(e.message.startsWith(u"This backend does not list the VisIVO VR app"_s));
    QCOMPARE(countCalls(rig.calls(), u" reverse "_s), 0);
    QCOMPARE(countCalls(rig.calls(), u"am start"_s), 0);
    QCOMPARE(rig.backend->count(u"POST /v1/vr/launch "_s), 0);

    // Listed, but for another key (someone else's build): the same.
    rig.backend->assetlinksStatus = 200;
    rig.backend->assetlinksBody = assetlinks(kDevPackage, u"00:"_s.repeated(31) + u"00"_s);
    e = runToEnd(launcher, [&]() { launcher.open(rig.target); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::NotListed);

    // No APK at all: said, nothing run.
    VrAppLauncher::Target noApp = rig.target;
    noApp.app = VrHeadsetApp::AppInfo();
    noApp.app.error = u"The VisIVO VR app is not bundled with this build."_s;
    rig.set(u"log"_s, QByteArray());
    e = runToEnd(launcher, [&]() { launcher.open(noApp); });
    QCOMPARE(e.outcome, VrAppLauncher::Outcome::Failed);
    QVERIFY(rig.calls().isEmpty());
}

void TestVrHeadsetApp::launchBodyMatchesTheBackendsRequestModel()
{
    QFile f(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/backend/app/routers/vr_launch.py"_s);
    if (!f.exists())
        QSKIP("the backend's launch routes (#255) are not in this tree");
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString launch = QString::fromUtf8(f.readAll());
    QVERIFY(launch.contains(u"class LaunchRequest(BaseModel):\n    cube: HeadsetCube"_s));
    for (const QString &field : { u"ticket: str"_s, u"launch: str"_s, u"page: str"_s, u"state: str"_s,
                                  u"client: str | None"_s })
        QVERIFY2(launch.contains(field), qPrintable(field));
    QVERIFY(launch.contains(u"@router.post(\"/v1/vr/launch\""_s));
    QVERIFY(launch.contains(u"@router.get(\"/v1/vr/launch/{launch}\""_s));
    QVERIFY(launch.contains(u"\"/.well-known/assetlinks.json\""_s));

    QFile h(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/backend/app/routers/headset.py"_s);
    QVERIFY(h.open(QIODevice::ReadOnly));
    const QString headsetSource = QString::fromUtf8(h.readAll());
    const qsizetype at = headsetSource.indexOf(u"class HeadsetCube(BaseModel):"_s);
    QVERIFY(at >= 0);
    const QString model = headsetSource.mid(at, headsetSource.indexOf(u"\n\n\n"_s, at) - at);
    VrLink::Inputs in;
    in.datasetId = u"ds1"_s;
    in.path = u"/d/c.fits"_s;
    const QJsonObject cube = VrHeadsetApp::launchRequestBody(in).value(u"cube"_s).toObject();
    for (auto it = cube.begin(); it != cube.end(); ++it)
        QVERIFY2(model.contains(u"    %1:"_s.arg(it.key())), qPrintable(it.key()));
    // The hub lists the launch each page named.
    QVERIFY(headsetSource.contains(u"launch: str | None"_s));
}
