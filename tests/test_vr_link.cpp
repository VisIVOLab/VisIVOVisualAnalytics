#include "TestVrLink.h"

#include "app/AdbHeadset.h"
#include "app/VrBackendCheck.h"
#include "app/VrLink.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

using namespace Qt::StringLiterals;

Q_DECLARE_METATYPE(AdbHeadset::DeviceState)

namespace {

VrLink::Inputs localInputs()
{
    VrLink::Inputs in;
    in.baseUrl = u"http://127.0.0.1:8000"_s;
    in.token = u"tok-123"_s;
    in.sessionId = u"sess-abc"_s;
    in.datasetId = u"ds-42"_s;
    in.path = u"/data/HCG44.fits"_s;
    return in;
}

/// The fragment's parameters as the page reads them: split on '&', then '=',
/// each value decoded once (decodeURIComponent).
QMap<QString, QString> fragmentOf(const QString &url)
{
    QMap<QString, QString> out;
    const int hash = url.indexOf(u'#');
    if (hash < 0)
        return out;
    for (const QString &pair : url.mid(hash + 1).split(u'&')) {
        const int eq = pair.indexOf(u'=');
        out.insert(pair.left(eq), QUrl::fromPercentEncoding(pair.mid(eq + 1).toUtf8()));
    }
    return out;
}

}   // namespace

// ── VrLink ───────────────────────────────────────────────────────────────────

void TestVrLink::linkCarriesEverythingInTheFragment()
{
    const VrLink::Link link = VrLink::build(localInputs());
    QVERIFY(link.ok);
    QCOMPARE(link.port, 8000);
    QCOMPARE(link.pageUrl, u"http://localhost:8000/vr/"_s);
    QVERIFY(link.url.startsWith(link.pageUrl + u'#'));
    // Nothing in the query or the path: the server never sees any of it.
    QVERIFY(!link.url.left(link.url.indexOf(u'#')).contains(u'?'));
    const auto f = fragmentOf(link.url);
    QCOMPARE(f.value(u"token"_s), u"tok-123"_s);
    QCOMPARE(f.value(u"session"_s), u"sess-abc"_s);
    QCOMPARE(f.value(u"dataset"_s), u"ds-42"_s);
    QCOMPARE(f.value(u"path"_s), u"/data/HCG44.fits"_s);
    // Token first: the page's takeToken() strips it and keeps the rest.
    QVERIFY(link.url.mid(link.url.indexOf(u'#')).startsWith(u"#token="_s));
}

void TestVrLink::valuesAreFullyEncoded()
{
    VrLink::Inputs in = localInputs();
    in.token = u"a+b/c=d&e#f"_s;
    in.path = u"/data/My Cubes/it's & co #1/ngc 3137 μ.fits"_s;
    const VrLink::Link link = VrLink::build(in);
    QVERIFY(link.ok);
    const QString fragment = link.url.mid(link.url.indexOf(u'#') + 1);
    // Only parameter syntax and unreserved characters are left: no '#', no
    // quote, no space, no '+' (which a form decoder would turn into a space).
    for (const QChar c : fragment)
        QVERIFY2(c.isLetterOrNumber() || QStringLiteral("-._~%&=").contains(c),
                 qPrintable(u"unexpected character %1"_s.arg(c)));
    QCOMPARE(fragment.count(u'&'), 3);
    const auto f = fragmentOf(link.url);
    QCOMPARE(f.value(u"token"_s), in.token);
    QCOMPARE(f.value(u"path"_s), in.path);
}

void TestVrLink::portComesFromTheBackendUrl_data()
{
    QTest::addColumn<QString>("baseUrl");
    QTest::addColumn<int>("port");
    QTest::addColumn<QString>("pageUrl");
    QTest::newRow("local") << u"http://127.0.0.1:8000"_s << 8000 << u"http://localhost:8000/vr/"_s;
    QTest::newRow("trailing slash") << u"http://127.0.0.1:8001/"_s << 8001
                                    << u"http://localhost:8001/vr/"_s;
    // A cluster backend through its ssh tunnel: forwarding the tunnel's port
    // to the headset reaches the cluster the same way.
    QTest::newRow("cluster tunnel") << u"http://localhost:18960"_s << 18960
                                    << u"http://localhost:18960/vr/"_s;
    QTest::newRow("ipv6 loopback") << u"http://[::1]:8002"_s << 8002
                                   << u"http://localhost:8002/vr/"_s;
    QTest::newRow("no port") << u"http://localhost"_s << 80 << u"http://localhost:80/vr/"_s;
    QTest::newRow("upper case") << u"HTTP://LOCALHOST:8000"_s << 8000
                                << u"http://localhost:8000/vr/"_s;
}

void TestVrLink::portComesFromTheBackendUrl()
{
    QFETCH(QString, baseUrl);
    QFETCH(int, port);
    QFETCH(QString, pageUrl);
    VrLink::Inputs in = localInputs();
    in.baseUrl = baseUrl;
    const VrLink::Link link = VrLink::build(in);
    QVERIFY2(link.ok, qPrintable(link.error));
    QCOMPARE(link.port, port);
    QCOMPARE(link.pageUrl, pageUrl);
}

void TestVrLink::aPathPrefixIsKeptInThePageAndInfoUrls_data()
{
    QTest::addColumn<QString>("baseUrl");
    QTest::addColumn<QString>("prefix");
    QTest::addColumn<QString>("pageUrl");
    QTest::addColumn<QString>("checkUrl");
    QTest::newRow("root") << u"http://127.0.0.1:8000"_s << QString()
                          << u"http://localhost:8000/vr/"_s << u"http://127.0.0.1:8000/v1/vr/backend_check"_s;
    // A reverse proxy that strips its prefix (an HPC portal) behind a tunnel.
    QTest::newRow("prefix") << u"http://localhost:18900/visivo"_s << u"/visivo"_s
                            << u"http://localhost:18900/visivo/vr/"_s
                            << u"http://localhost:18900/visivo/v1/vr/backend_check"_s;
    QTest::newRow("trailing slashes") << u"http://127.0.0.1:9000/node/r33/proxy/8000//"_s
                                      << u"/node/r33/proxy/8000"_s
                                      << u"http://localhost:9000/node/r33/proxy/8000/vr/"_s
                                      << u"http://127.0.0.1:9000/node/r33/proxy/8000/v1/vr/backend_check"_s;
    QTest::newRow("dot segments") << u"http://127.0.0.1:9000/a/./b/../c/"_s << u"/a/c"_s
                                  << u"http://localhost:9000/a/c/vr/"_s
                                  << u"http://127.0.0.1:9000/a/c/v1/vr/backend_check"_s;
    QTest::newRow("encoded") << u"http://127.0.0.1:9000/my%20portal"_s << u"/my%20portal"_s
                             << u"http://localhost:9000/my%20portal/vr/"_s
                             << u"http://127.0.0.1:9000/my%20portal/v1/vr/backend_check"_s;
}

void TestVrLink::aPathPrefixIsKeptInThePageAndInfoUrls()
{
    QFETCH(QString, baseUrl);
    QFETCH(QString, prefix);
    QFETCH(QString, pageUrl);
    QFETCH(QString, checkUrl);
    VrLink::Inputs in = localInputs();
    in.baseUrl = baseUrl;
    const VrLink::Link link = VrLink::build(in);
    QVERIFY2(link.ok, qPrintable(link.error));
    QCOMPARE(link.prefix, prefix);
    QCOMPARE(link.pageUrl, pageUrl);
    QCOMPARE(link.checkUrl, checkUrl);
    QVERIFY(link.url.startsWith(pageUrl + u"#token="_s));
    QVERIFY(!link.checkUrl.contains(in.token));
}

void TestVrLink::aTunnelledRemoteBackendIsLabelledRemote()
{
    VrLink::Inputs in = localInputs();
    in.baseUrl = u"http://localhost:18900"_s;
    in.token = u"cluster-token"_s;
    in.remote = true;
    const VrLink::Link link = VrLink::build(in);
    QVERIFY(link.ok);
    QVERIFY(link.remote);
    QCOMPARE(link.port, 18900);
    QCOMPARE(fragmentOf(link.url).value(u"token"_s), u"cluster-token"_s);
    QVERIFY(!VrLink::build(localInputs()).remote);
}

void TestVrLink::backendCheckReadsTheAnswer()
{
    using VrBackendCheck::Severity;
    const QByteArray good = R"({"vr_api": 2, "page": {"present": true, "build": "abc123def456"},
        "missing_routes": [], "stereo": {"depth": "auto"}})";
    auto r = VrBackendCheck::interpret(200, good, true);
    QCOMPARE(r.severity, Severity::Ok);
    QVERIFY(r.message.isEmpty());
    QCOMPARE(r.build, u"abc123def456"_s);
    // An overlay older than the route: a warning naming the overlay, not a refusal.
    r = VrBackendCheck::interpret(404, R"({"detail":"Not Found"})", true);
    QCOMPARE(r.severity, Severity::Warning);
    QVERIFY(r.message.contains(u"--backend-src"_s));
    QVERIFY(!VrBackendCheck::interpret(404, {}, false).message.contains(u"--backend-src"_s));
    r = VrBackendCheck::interpret(200, R"({"vr_api": 2, "page": {"present": true},
        "missing_routes": ["mask_stats_bin", "render_ws"]})", true);
    QCOMPARE(r.severity, Severity::Warning);
    QVERIFY(r.message.contains(u"mask_stats_bin, render_ws"_s));
    r = VrBackendCheck::interpret(200, R"({"vr_api": 1, "page": {"present": true}, "missing_routes": []})", false);
    QCOMPARE(r.severity, Severity::Warning);
    QVERIFY(r.message.contains(u"older headset protocol"_s));
    // No page at all, or the token refused: the headset cannot work.
    QCOMPARE(VrBackendCheck::interpret(200, R"({"vr_api": 2, "page": {"present": false}})", true).severity,
             Severity::Error);
    QCOMPARE(VrBackendCheck::interpret(401, {}, true).severity, Severity::Error);
    QCOMPARE(VrBackendCheck::interpret(403, {}, true).severity, Severity::Error);
    // Not understood is never a refusal: a sign-in page, another shape, no page
    // object; a page without an explicit "present": false is not refused either.
    QCOMPARE(VrBackendCheck::interpret(200, "<html>Sign in</html>", true).severity,
             Severity::Warning);
    QCOMPARE(VrBackendCheck::interpret(200, R"({"token_ok": true, "build": "x"})", true).severity,
             Severity::Warning);
    QCOMPARE(VrBackendCheck::interpret(200, R"([1, 2])", true).severity, Severity::Warning);
    QCOMPARE(VrBackendCheck::interpret(
                     200, R"({"vr_api": 2, "page": {"build": "b"}, "missing_routes": []})", true)
                     .severity,
             Severity::Ok);
    r = VrBackendCheck::interpret(302, {}, true);
    QCOMPARE(r.severity, Severity::Warning);
    QVERIFY(r.message.contains(u"redirected"_s));
    QCOMPARE(VrBackendCheck::interpret(0, {}, true).severity, Severity::Warning);
    QCOMPARE(VrBackendCheck::interpret(502, {}, true).severity, Severity::Warning);
    r = VrBackendCheck::interpret(200, R"({"vr_api": 2, "page": {"present": true}, "missing_routes": [],
        "stereo": {"depth": "off"}})", true);
    QCOMPARE(r.severity, Severity::Ok);
    QVERIFY(r.depthOff && r.message.contains(u"without depth"_s));
}

void TestVrLink::aBackendNotOnLoopbackHasNoLink_data()
{
    QTest::addColumn<QString>("baseUrl");
    QTest::newRow("lan address") << u"http://192.168.1.20:8000"_s;
    QTest::newRow("cluster node") << u"http://compute-node42:8000"_s;
    QTest::newRow("looks local") << u"http://localhost.example.org:8000"_s;
    QTest::newRow("dot localhost") << u"http://visivo.localhost:8000"_s;
    // adb reverse reaches 127.0.0.1 / ::1 only, not the rest of 127/8.
    QTest::newRow("other loopback") << u"http://127.0.0.2:8000"_s;
    QTest::newRow("query") << u"http://127.0.0.1:9000/?x=1"_s;
    QTest::newRow("not a url") << u"127.0.0.1:8000"_s;
    QTest::newRow("websocket") << u"ws://127.0.0.1:8000"_s;
    QTest::newRow("empty") << QString();
}

void TestVrLink::aBackendNotOnLoopbackHasNoLink()
{
    QFETCH(QString, baseUrl);
    VrLink::Inputs in = localInputs();
    in.baseUrl = baseUrl;
    const VrLink::Link link = VrLink::build(in);
    QVERIFY(!link.ok);
    QVERIFY(link.url.isEmpty());
    QVERIFY(!link.error.isEmpty());
    QVERIFY(!link.error.contains(in.token));
}

void TestVrLink::noDatasetNoLink()
{
    VrLink::Inputs in = localInputs();
    in.datasetId.clear();
    const VrLink::Link link = VrLink::build(in);
    QVERIFY(!link.ok);
    QVERIFY(link.url.isEmpty());
}

void TestVrLink::emptyTokenAndSessionAreLeftOut()
{
    VrLink::Inputs in = localInputs();
    in.token.clear();
    in.sessionId.clear();
    in.path.clear();
    const VrLink::Link link = VrLink::build(in);
    QVERIFY(link.ok);
    QCOMPARE(link.url, u"http://localhost:8000/vr/#dataset=ds-42"_s);
}

void TestVrLink::pageUrlHoldsNoSecret()
{
    const VrLink::Inputs in = localInputs();
    const VrLink::Link link = VrLink::build(in);
    QVERIFY(!link.pageUrl.contains(in.token));
    QVERIFY(!link.pageUrl.contains(in.sessionId));
    QVERIFY(!link.pageUrl.contains(u'#'));
}

void TestVrLink::redactHidesRawAndEncodedSecrets()
{
    VrLink::Inputs in = localInputs();
    in.token = u"a+b/c"_s;   // encoded in the link as a%2Bb%2Fc
    const VrLink::Link link = VrLink::build(in);
    const QStringList secrets{ in.token, in.sessionId };
    const QString log = VrLink::redact(u"adb shell am start -d '%1'"_s.arg(link.url), secrets);
    QVERIFY(!log.contains(in.token));
    QVERIFY(!log.contains(VrLink::encodeValue(in.token)));
    QVERIFY(!log.contains(in.sessionId));
    QVERIFY(log.contains(u"localhost:8000/vr/"_s));   // still says what ran
    // Raw form too: adb or the device may echo it decoded.
    QCOMPARE(VrLink::redact(u"token a+b/c here"_s, secrets), u"token <redacted> here"_s);
    // An empty secret must not turn every gap between characters into one.
    QCOMPARE(VrLink::redact(u"abc"_s, { QString() }), u"abc"_s);
}

// ── AdbHeadset ───────────────────────────────────────────────────────────────

void TestVrLink::parsesDevicesList_data()
{
    QTest::addColumn<QString>("output");
    QTest::addColumn<QStringList>("serials");
    QTest::addColumn<QList<AdbHeadset::DeviceState>>("states");
    QTest::addColumn<QStringList>("models");
    using S = AdbHeadset::DeviceState;

    QTest::newRow("empty") << u"List of devices attached\n\n"_s << QStringList()
                           << QList<S>() << QStringList();
    QTest::newRow("daemon starting")
            << u"* daemon not running; starting now at tcp:5037\n* daemon started "
               u"successfully\nList of devices attached\n\n"_s
            << QStringList() << QList<S>() << QStringList();
    QTest::newRow("one quest -l")
            << u"List of devices attached\n2G0YC1ZF8B0123         device usb:1-1 "
               u"product:eureka model:Quest_3 device:eureka transport_id:3\n\n"_s
            << QStringList{ u"2G0YC1ZF8B0123"_s } << QList<S>{ S::Ready }
            << QStringList{ u"Quest_3"_s };
    QTest::newRow("tab separated, no -l")
            << u"List of devices attached\r\n2G0YC1ZF8B0123\tdevice\r\n\r\n"_s
            << QStringList{ u"2G0YC1ZF8B0123"_s } << QList<S>{ S::Ready } << QStringList{ QString() };
    QTest::newRow("unauthorized")
            << u"List of devices attached\n2G0YC1ZF8B0123         unauthorized usb:1-1 "
               u"transport_id:4\n"_s
            << QStringList{ u"2G0YC1ZF8B0123"_s } << QList<S>{ S::Unauthorized }
            << QStringList{ QString() };
    QTest::newRow("authorizing")
            << u"List of devices attached\n2G0YC1ZF8B0123\tauthorizing\n192.168.1.30:5555\tconnecting\n"_s
            << QStringList{ u"2G0YC1ZF8B0123"_s, u"192.168.1.30:5555"_s }
            << QList<S>{ S::Authorizing, S::Authorizing } << QStringList{ QString(), QString() };
    QTest::newRow("offline") << u"List of devices attached\n192.168.1.30:5555\toffline\n"_s
                             << QStringList{ u"192.168.1.30:5555"_s } << QList<S>{ S::Offline }
                             << QStringList{ QString() };
    QTest::newRow("no permissions")
            << u"List of devices attached\n2G0YC1ZF8B0123\tno permissions (user in plugdev "
               u"group; are your udev rules wrong?); see [http://developer.android.com/tools/"
               u"device.html]\n"_s
            << QStringList{ u"2G0YC1ZF8B0123"_s } << QList<S>{ S::NoPermissions }
            << QStringList{ QString() };
    QTest::newRow("several")
            << u"List of devices attached\n"
               u"2G0YC1ZF8B0123         device usb:1-1 product:eureka model:Quest_3 "
               u"transport_id:3\n"
               u"192.168.1.30:5555      device product:hollywood model:Quest_2 transport_id:5\n"
               u"emulator-5554          offline transport_id:1\n"
               u"R58M1234               recovery\n"_s
            << QStringList{ u"2G0YC1ZF8B0123"_s, u"192.168.1.30:5555"_s, u"emulator-5554"_s,
                            u"R58M1234"_s }
            << QList<S>{ S::Ready, S::Ready, S::Offline, S::Other }
            << QStringList{ u"Quest_3"_s, u"Quest_2"_s, QString(), QString() };
}

void TestVrLink::parsesDevicesList()
{
    QFETCH(QString, output);
    QFETCH(QStringList, serials);
    QFETCH(QList<AdbHeadset::DeviceState>, states);
    QFETCH(QStringList, models);
    const QList<AdbHeadset::Device> devices = AdbHeadset::parseDevices(output);
    QCOMPARE(devices.size(), serials.size());
    for (int i = 0; i < devices.size(); ++i) {
        QCOMPARE(devices.at(i).serial, serials.at(i));
        QCOMPARE(devices.at(i).state, states.at(i));
        QCOMPARE(devices.at(i).model, models.at(i));
    }
}

void TestVrLink::statusOfDevices()
{
    using AdbHeadset::Device;
    using AdbHeadset::Status;
    using S = AdbHeadset::DeviceState;
    const Device ready{ u"a"_s, S::Ready, {} };
    const Device unauthorized{ u"b"_s, S::Unauthorized, {} };
    const Device offline{ u"c"_s, S::Offline, {} };
    const Device noPerm{ u"d"_s, S::NoPermissions, {} };
    const Device authorizing{ u"e"_s, S::Authorizing, {} };
    QCOMPARE(AdbHeadset::statusOf({}), Status::NoDevice);
    QCOMPARE(AdbHeadset::statusOf({ offline }), Status::Offline);
    QCOMPARE(AdbHeadset::statusOf({ offline, unauthorized }), Status::Unauthorized);
    QCOMPARE(AdbHeadset::statusOf({ noPerm, offline }), Status::NoPermissions);
    QCOMPARE(AdbHeadset::statusOf({ authorizing, offline }), Status::Authorizing);
    QCOMPARE(AdbHeadset::statusOf({ authorizing, unauthorized }), Status::Unauthorized);
    // One ready device is enough, whatever else is attached.
    QCOMPARE(AdbHeadset::statusOf({ unauthorized, ready, offline }), Status::Ready);
    QCOMPARE(AdbHeadset::readyDevices({ unauthorized, ready, offline }).size(), 1);
}

void TestVrLink::shellQuoteSurvivesTheShell_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("link") << VrLink::build(localInputs()).url;
    QTest::newRow("shell syntax") << u"a&b;c|d#e $HOME `id` \"q\" \\n *"_s;
    QTest::newRow("single quotes") << u"it's 'quoted' ''"_s;
    QTest::newRow("empty") << QString();
}

void TestVrLink::shellQuoteSurvivesTheShell()
{
#ifdef Q_OS_WIN
    QSKIP("needs a POSIX sh, as on the headset");
#else
    // What `adb shell` does with its arguments: joins them with spaces and
    // hands the line to sh on the device. A POSIX sh here reads it the same way.
    QFETCH(QString, text);
    QProcess sh;
    sh.start(u"/bin/sh"_s, { u"-c"_s, u"printf '%s' "_s + AdbHeadset::shellQuote(text) });
    QVERIFY(sh.waitForFinished(5000));
    QCOMPARE(sh.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(sh.readAllStandardOutput()), text);
#endif
}

void TestVrLink::commandArgumentLists()
{
    QCOMPARE(AdbHeadset::reverseArgs(u"2G0Y"_s, 8000),
             (QStringList{ u"-s"_s, u"2G0Y"_s, u"reverse"_s, u"tcp:8000"_s, u"tcp:8000"_s }));
    QCOMPARE(AdbHeadset::reverseArgs(QString(), 18960),
             (QStringList{ u"reverse"_s, u"tcp:18960"_s, u"tcp:18960"_s }));
    const QString url = VrLink::build(localInputs()).url;
    QCOMPARE(AdbHeadset::viewArgs(u"2G0Y"_s, url, AdbHeadset::questBrowserPackage()),
             (QStringList{ u"-s"_s, u"2G0Y"_s, u"shell"_s, u"am"_s, u"start"_s, u"-a"_s,
                           u"android.intent.action.VIEW"_s, u"-d"_s, u"'"_s + url + u"'"_s,
                           u"'com.oculus.browser'"_s }));
    // Without the Quest Browser: a plain VIEW, the headset picks the app.
    const QStringList plain = AdbHeadset::viewArgs(u"2G0Y"_s, url, QString());
    QCOMPARE(plain.last(), u"'"_s + url + u"'"_s);
    QCOMPARE(AdbHeadset::packageQueryArgs(QString(), u"com.oculus.browser"_s),
             (QStringList{ u"shell"_s, u"pm"_s, u"list"_s, u"packages"_s,
                           u"'com.oculus.browser'"_s }));
}

void TestVrLink::packageListedNeedsTheExactName()
{
    const QString pkg = AdbHeadset::questBrowserPackage();
    QVERIFY(AdbHeadset::packageListed(u"package:com.oculus.browser\r\n"_s, pkg));
    QVERIFY(AdbHeadset::packageListed(
            u"package:com.oculus.browser.beta\npackage:com.oculus.browser\n"_s, pkg));
    // pm matches substrings: a near name is not the browser.
    QVERIFY(!AdbHeadset::packageListed(u"package:com.oculus.browser.beta\n"_s, pkg));
    QVERIFY(!AdbHeadset::packageListed(QString(), pkg));
}

void TestVrLink::viewStartedReadsAmOutput()
{
    QVERIFY(AdbHeadset::viewStarted(
            0, u"Starting: Intent { act=android.intent.action.VIEW dat=http://localhost:8000/... "
               u"pkg=com.oculus.browser }\n"_s));
    // The browser was already open and got the URL: a success.
    QVERIFY(AdbHeadset::viewStarted(
            0, u"Starting: Intent { act=android.intent.action.VIEW }\nWarning: Activity not "
               u"started, intent has been delivered to currently running top-most instance.\n"_s));
    QVERIFY(!AdbHeadset::viewStarted(
            0, u"Starting: Intent { act=android.intent.action.VIEW }\nError: Activity not "
               u"started, unable to resolve Intent { act=android.intent.action.VIEW }\n"_s));
    QVERIFY(!AdbHeadset::viewStarted(
            0, u"Exception occurred while executing 'start':\njava.lang.SecurityException\n"_s));
    QVERIFY(!AdbHeadset::viewStarted(1, QString()));
}

void TestVrLink::findsAdbOnPathThenInSdk()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX executables");
#else
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto makeAdb = [](const QString &dir) {
        QDir().mkpath(dir);
        QFile f(QDir(dir).filePath(u"adb"_s));
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write("#!/bin/sh\n");
        f.close();
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
        return f.fileName();
    };
    const QString onPath = makeAdb(tmp.filePath(u"bin"_s));
    const QString inSdk = makeAdb(tmp.filePath(u"sdk/platform-tools"_s));
    QVERIFY(!onPath.isEmpty() && !inSdk.isEmpty());
    QDir().mkpath(tmp.filePath(u"empty"_s));

    QProcessEnvironment env;
    env.insert(u"PATH"_s, tmp.filePath(u"empty"_s) + u':' + tmp.filePath(u"bin"_s));
    env.insert(u"ANDROID_HOME"_s, tmp.filePath(u"sdk"_s));
    // The PATH wins: it is the adb the user gets in a terminal.
    QCOMPARE(AdbHeadset::findAdb(AdbHeadset::Platform::MacOS, tmp.path(), env), onPath);
    // An app started from the Dock has a short PATH: the SDK is found anyway.
    env.insert(u"PATH"_s, tmp.filePath(u"empty"_s));
    QCOMPARE(AdbHeadset::findAdb(AdbHeadset::Platform::MacOS, tmp.path(), env), inSdk);

    const QStringList mac = AdbHeadset::candidateDirs(AdbHeadset::Platform::MacOS, u"/Users/u"_s,
                                                      QProcessEnvironment());
    QVERIFY(mac.contains(u"/opt/homebrew/bin"_s));
    QVERIFY(mac.contains(u"/Users/u/Library/Android/sdk/platform-tools"_s));
    QProcessEnvironment winEnv;
    winEnv.insert(u"LOCALAPPDATA"_s, u"C:/Users/u/AppData/Local"_s);
    QVERIFY(AdbHeadset::candidateDirs(AdbHeadset::Platform::Windows, u"C:/Users/u"_s, winEnv)
                    .contains(u"C:/Users/u/AppData/Local/Android/Sdk/platform-tools"_s));
    // Windows PATH entries may be quoted; the quotes are not part of the folder.
    {
        QFile exe(QDir(tmp.filePath(u"bin"_s)).filePath(u"adb.exe"_s));
        QVERIFY(exe.open(QIODevice::WriteOnly));
        exe.close();
        exe.setPermissions(exe.permissions() | QFileDevice::ExeOwner);
        QProcessEnvironment quoted;
        quoted.insert(u"PATH"_s, u"\""_s + tmp.filePath(u"bin"_s) + u"\";C:/nowhere"_s);
        QCOMPARE(AdbHeadset::findAdb(AdbHeadset::Platform::Windows, tmp.path(), quoted),
                 exe.fileName());
    }
    QVERIFY(AdbHeadset::candidateDirs(AdbHeadset::Platform::Linux, u"/home/u"_s,
                                      QProcessEnvironment())
                    .contains(u"/home/u/Android/Sdk/platform-tools"_s));
#endif
}

// ── Next-step guidance (epic #166) ───────────────────────────────────────────

void TestVrLink::installCommandPerPlatform()
{
    using AdbHeadset::Platform;
    QCOMPARE(AdbHeadset::installCommand(Platform::MacOS), u"brew install android-platform-tools"_s);
    QCOMPARE(AdbHeadset::installCommand(Platform::Linux), u"sudo apt install adb"_s);
    QCOMPARE(AdbHeadset::installCommand(Platform::Windows), u"winget install Google.PlatformTools"_s);
    for (Platform p : { Platform::MacOS, Platform::Linux, Platform::Windows }) {
        const AdbHeadset::Guidance g = AdbHeadset::adbMissingGuidance(p);
        QVERIFY(g.problem);
        QVERIFY(g.steps.first().contains(AdbHeadset::installCommand(p)));
        QVERIFY(g.steps.join(u' ').contains(u"Retry"_s));
    }
}

void TestVrLink::readsQuestOnUsbFromSystemProfiler()
{
    using AdbHeadset::UsbQuest;
    // The shape of `system_profiler -json SPUSBHostDataType` on macOS 26: a
    // Quest under a hub, next to other devices.
    const QByteArray withQuest = R"json({"SPUSBHostDataType":[{"_name":"USB 3.1 Bus"},
        {"_items":[{"_items":[{"_name":"LG Monitor Controls","USBDeviceKeyVendorID":"0x043e"},
                              {"_name":"Quest 3","USBDeviceKeyVendorID":"0x2833","USBDeviceKeyProductID":"0x0186"}],
                    "_name":"USB2.1 Hub","USBDeviceKeyVendorID":"0x0bda"}],"_name":"USB 3.1 Bus"}]})json";
    QCOMPARE(AdbHeadset::usbQuestFromProfiler(withQuest), UsbQuest::Present);
    const QByteArray without = R"json({"SPUSBHostDataType":[{"_items":[{"_name":"HD Pro Webcam C920","USBDeviceKeyVendorID":"0x046d"}]}]})json";
    QCOMPARE(AdbHeadset::usbQuestFromProfiler(without), UsbQuest::Absent);
    // The older SPUSBDataType writes "0x2833  (Oculus VR, Inc.)".
    const QByteArray older = R"json({"SPUSBDataType":[{"_items":[{"_name":"Quest 2","vendor_id":"0x2833  (Oculus VR, Inc.)"}]}]})json";
    QCOMPARE(AdbHeadset::usbQuestFromProfiler(older), UsbQuest::Present);
    QCOMPARE(AdbHeadset::usbQuestFromProfiler("not json"), UsbQuest::Unknown);
    QCOMPARE(AdbHeadset::usbQuestFromProfiler(R"json({"other":[]})json"), UsbQuest::Unknown);
    QCOMPARE(AdbHeadset::usbProbeArgs(), QStringList({ u"-json"_s, u"SPUSBHostDataType"_s }));
}

void TestVrLink::guidanceNamesTheNextStep()
{
    using AdbHeadset::Status;
    using AdbHeadset::UsbQuest;
    // The case adb cannot explain: on USB, but not listed.
    const AdbHeadset::Guidance usb = AdbHeadset::guidanceFor(Status::NoDevice, UsbQuest::Present);
    QVERIFY(usb.headline.contains(u"developer mode or USB debugging is off on the headset"_s));
    QVERIFY(usb.steps.join(u' ').contains(u"Developer mode"_s));
    QVERIFY(usb.steps.join(u' ').contains(u"Allow USB debugging"_s));
    QVERIFY(AdbHeadset::guidanceFor(Status::NoDevice, UsbQuest::Absent).steps.join(u' ').contains(u"data cable"_s));
    QCOMPARE(AdbHeadset::guidanceFor(Status::NoDevice).headline, u"No headset connected."_s);
    const AdbHeadset::Guidance unauth = AdbHeadset::guidanceFor(Status::Unauthorized);
    QVERIFY(unauth.steps.join(u' ').contains(u"Allow USB debugging"_s));
    QVERIFY(unauth.steps.join(u' ').contains(u"Always allow from this computer"_s));
    for (Status st : { Status::NoDevice, Status::Unauthorized, Status::NoPermissions, Status::Authorizing,
                       Status::Offline }) {
        const AdbHeadset::Guidance g = AdbHeadset::guidanceFor(st);
        QVERIFY(g.problem);
        QVERIFY(!g.headline.isEmpty());
        QVERIFY(!g.steps.isEmpty());
        QCOMPARE(g.anchor, u"troubleshooting"_s);
    }
    QVERIFY(!AdbHeadset::guidanceFor(Status::Ready).problem);
}

void TestVrLink::prefersTheLastHeadset()
{
    const QStringList serials{ u"phone-1"_s, u"2G0YC1ZF8B0123"_s };
    QCOMPARE(AdbHeadset::preferredIndex(serials, u"2G0YC1ZF8B0123"_s), 1);
    QCOMPARE(AdbHeadset::preferredIndex(serials, u"gone"_s), 0);
    QCOMPARE(AdbHeadset::preferredIndex(serials, QString()), 0);
    QCOMPARE(AdbHeadset::preferredIndex({}, u"x"_s), -1);
    // Copy Error shortens the home directory to "~".
    QCOMPARE(AdbHeadset::homeShortened(u"/Users/john/Library/Android/sdk/platform-tools/adb"_s, u"/Users/john"_s),
             u"~/Library/Android/sdk/platform-tools/adb"_s);
    QCOMPARE(AdbHeadset::homeShortened(u"/opt/homebrew/bin/adb"_s, u"/Users/john"_s), u"/opt/homebrew/bin/adb"_s);
    QCOMPARE(AdbHeadset::homeShortened(u"/Users/johnny/adb"_s, u"/Users/john"_s), u"/Users/johnny/adb"_s);
    QCOMPARE(AdbHeadset::homeShortened(u"C:\\Users\\Ann\\AppData\\adb.exe"_s, u"C:/Users/Ann"_s), u"~/AppData/adb.exe"_s);
}
