#include "app/BackendClient.h"
#include "TestBackendRouting.h"

#include <QCryptographicHash>

#include "Settings.h"
#include "app/ClusterConnection.h"
#include "app/ClusterProfile.h"
#include "app/LaunchOptions.h"
#include "app/RecentDatasetsManager.h"
#include "skava/BackendRouting.h"
#include "skava/SkavaClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

// A non-local registry node.
BackendNode makeNode(const QString &id, const QString &url, const QString &srcCode)
{
    BackendNode n;
    n.id = id;
    n.name = id;
    n.url = url;
    n.token = QString();
    n.srcCode = srcCode;
    n.isLocal = false;
    n.isBuiltin = false;
    return n;
}

// A DataLink response advertising a single visivo-backend descriptor.
SkavaDatalinkResponse datalinkWith(const QString &endpoint, const QString &nodeCode)
{
    SkavaVisivoBackendDescriptor d;
    d.endpoint = endpoint;
    d.nodeCode = nodeCode;
    SkavaDatalinkResponse r;
    r.valid = true;
    // As SKAVA answers: the descriptor's node is the best one, and the
    // primary access URL is its replica.
    r.bestNode = nodeCode;
    r.primaryAccessUrl = u"file:///data/best/cube.fits"_s;
    r.visivoBackends.append(d);
    return r;
}

SkavaVisivoBackendDescriptor descriptor(const QString &endpoint, const QString &nodeCode,
                                        const QStringList &kinds = {}, bool requiresAuth = false)
{
    SkavaVisivoBackendDescriptor d;
    d.endpoint = endpoint;
    d.nodeCode = nodeCode;
    d.supportedKinds = kinds;
    d.requiresAuth = requiresAuth;
    return d;
}

SkavaReplica replica(const QString &code, const QString &accessEndpoint)
{
    SkavaReplica r;
    r.code = code;
    r.accessEndpoint = accessEndpoint;
    return r;
}

// Two nodes hold the cube: A is SKAVA's best (primary access), B a replica.
SkavaDatalinkResponse twoNodeDatalink()
{
    SkavaDatalinkResponse r;
    r.valid = true;
    r.bestNode = u"A"_s;
    r.primaryAccessUrl = u"file:///srv/a/cube.fits"_s;
    r.dataset.dataproductType = u"cube"_s;
    r.replicas = { replica(u"A"_s, u"file:///srv/a/cube.fits"_s),
                   replica(u"B"_s, u"file:///mnt/b/archive/cube.fits"_s) };
    r.visivoBackends = { descriptor(u"https://a.example/visivo"_s, u"A"_s, { u"cube"_s }),
                         descriptor(u"https://b.example/visivo"_s, u"B"_s, { u"cube"_s }) };
    return r;
}

}  // namespace

void TestBackendRouting::registryAlwaysHasLocal()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    const auto nodes = s.getBackendNodes();
    bool foundLocal = false;
    for (const auto &n : nodes) {
        if (n.id == "local"_L1) {
            foundLocal = true;
            QVERIFY(n.isLocal);
        }
    }
    QVERIFY2(foundLocal, "the 'local' backend must always be present");
}

void TestBackendRouting::sessionBackendIsUsedButNeverSaved()
{
    // A remote backend chosen at launch serves this run; the saved local
    // backend is untouched and is what the next plain launch reads again.
    QTemporaryDir dir;
    Settings s(dir.path());
    s.setBackendUrl(u"http://127.0.0.1:8000"_s);
    s.setBackendToken(u"local-token"_s);

    Settings::setSessionBackend({ u"http://localhost:18910"_s, u"job-token"_s, u"PLEIADI"_s });
    QCOMPARE(s.getBackendUrl(), u"http://localhost:18910"_s);
    QCOMPARE(s.getBackendToken(), u"job-token"_s);
    QCOMPARE(s.getLocalBackend().url, u"http://localhost:18910"_s);
    s.setBackendUrl(u"http://localhost:18910"_s);   // what a Settings dialog would save back
    s.setBackendToken(u"job-token"_s);
    Settings::clearSessionBackend();

    QCOMPARE(s.getBackendUrl(), u"http://127.0.0.1:8000"_s);
    QCOMPARE(s.getBackendToken(), u"local-token"_s);
    QVERIFY(!Settings::sessionBackend());
}

void TestBackendRouting::sessionBackendNotWrittenIntoTheRegistry()
{
    // Saving the backend list during a remote run (the Settings dialog saves it
    // whatever else changed) keeps the local entry's stored URL and token.
    QTemporaryDir dir;
    Settings s(dir.path());
    s.setBackendUrl(u"http://127.0.0.1:8000"_s);
    s.setBackendToken(u"local-token"_s);
    Settings::setSessionBackend({ u"http://localhost:18910"_s, u"job-token"_s, u"PLEIADI"_s });
    s.setBackendNodes(s.getBackendNodes());
    Settings::clearSessionBackend();

    QSettings raw(dir.filePath(u"Settings.ini"_s), QSettings::IniFormat);
    const QString nodes = raw.value(u"Backends/nodes"_s).toString();
    QVERIFY(!nodes.contains(u"job-token"_s));
    QVERIFY(!nodes.contains(u"18910"_s));
    QCOMPARE(s.getLocalBackend().url, u"http://127.0.0.1:8000"_s);
}

void TestBackendRouting::launchOptions_localUnlessABackendIsGiven()
{
    std::optional<Settings::SessionBackend> out;
    QString error;
    const QProcessEnvironment none;
    QVERIFY(LaunchOptions::remoteBackend({ u"visivo"_s }, none, &out, &error));
    QVERIFY(!out);
    QVERIFY(LaunchOptions::remoteBackend({ u"visivo"_s, u"/data/cube.fits"_s }, none, &out, &error));
    QVERIFY(!out);

    QVERIFY(LaunchOptions::remoteBackend({ u"visivo"_s, u"--backend"_s, u"http://localhost:18910/"_s },
                                         none, &out, &error));
    QVERIFY(out);
    QCOMPARE(out->url, u"http://localhost:18910"_s);
    QCOMPARE(out->name, u"localhost:18910"_s);
    QVERIFY(out->token.isEmpty());

    QProcessEnvironment env;
    env.insert(u"VISIVO_BACKEND_URL"_s, u"http://env.example:9000"_s);
    env.insert(u"VISIVO_BACKEND_NAME"_s, u"ENV"_s);
    out.reset();
    QVERIFY(LaunchOptions::remoteBackend({ u"visivo"_s, u"--backend=http://cli.example:8001"_s,
                                           u"--backend-name"_s, u"PLEIADI"_s },
                                         env, &out, &error));
    QCOMPARE(out->url, u"http://cli.example:8001"_s);   // the command line wins
    QCOMPARE(out->name, u"PLEIADI"_s);
    out.reset();
    QVERIFY(LaunchOptions::remoteBackend({ u"visivo"_s }, env, &out, &error));
    QCOMPARE(out->url, u"http://env.example:9000"_s);
    QCOMPARE(out->name, u"ENV"_s);
}

void TestBackendRouting::launchOptions_optionsWithoutValueAreErrors()
{
    const QProcessEnvironment none;
    const QList<QStringList> bad = {
        { u"visivo"_s, u"--backend"_s },
        { u"visivo"_s, u"--backend="_s },
        { u"visivo"_s, u"--backend"_s, u"--backend-name"_s, u"X"_s },
        { u"visivo"_s, u"--backend"_s, u"http://h:1"_s, u"--backend-token-file"_s },
        { u"visivo"_s, u"--backend"_s, u"http://h:1"_s, u"--backend-name="_s },
        { u"visivo"_s, u"--backend-name"_s, u"X"_s },                       // without --backend
        { u"visivo"_s, u"--backend"_s, u"localhost:18910"_s },              // no scheme
        { u"visivo"_s, u"--backend"_s, u"ftp://host:1"_s },
        { u"visivo"_s, u"--backend"_s, u"http://h:1"_s, u"--backend-token-file"_s,
          u"/nonexistent/token"_s },
    };
    for (const QStringList &args : bad) {
        std::optional<Settings::SessionBackend> out;
        QString error;
        QVERIFY2(!LaunchOptions::remoteBackend(args, none, &out, &error), qPrintable(args.join(u' ')));
        QVERIFY(!error.isEmpty());
        QVERIFY(!out);
    }
}

void TestBackendRouting::launchOptions_tokenFromFileOrEnvironment()
{
    QTemporaryDir dir;
    const QString tokenPath = dir.filePath(u"token"_s);
    {
        QFile f(tokenPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("file-token\n");
    }
    const QString emptyPath = dir.filePath(u"empty"_s);
    {
        QFile f(emptyPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    QProcessEnvironment env;
    env.insert(u"VISIVO_BACKEND_TOKEN"_s, u"env-token"_s);
    std::optional<Settings::SessionBackend> out;
    QString error;
    QVERIFY(LaunchOptions::remoteBackend({ u"visivo"_s, u"--backend"_s, u"http://h:1"_s,
                                           u"--backend-token-file"_s, tokenPath },
                                         env, &out, &error));
    QCOMPARE(out->token, u"file-token"_s);   // trimmed, and the file wins
    out.reset();
    QVERIFY(LaunchOptions::remoteBackend({ u"visivo"_s, u"--backend"_s, u"http://h:1"_s }, env, &out,
                                         &error));
    QCOMPARE(out->token, u"env-token"_s);
    out.reset();
    QVERIFY(!LaunchOptions::remoteBackend({ u"visivo"_s, u"--backend"_s, u"http://h:1"_s,
                                            u"--backend-token-file"_s, emptyPath },
                                          env, &out, &error));
}

void TestBackendRouting::cluster_launcherOutputIsParsed()
{
    // As launch_backend_slurm.sh prints it (a real run, token replaced).
    const QString out = uR"([visivo] Token saved to /scratch/u/visivo-ws/token
[visivo] Submitting SLURM job...
[visivo]   partition: gpu
[visivo] Job ID: 995
[visivo] Waiting for compute node assignment...
[visivo] Compute node: r33c1s01
[visivo]  attempt 1/60 – not ready yet...
  VisIVO backend is READY
  Job ID:        995
  Compute node:  r33c1s01
  Port:          18900
  Frontend:      pleiadi-gpu
      ssh -N -L 18900:r33c1s01:18900 ${USER}@pleiadi-gpu
          Token:    tok_AbC-12.x
)"_s;
    const auto info = ClusterConnection::parseLauncherOutput(out);
    QCOMPARE(info.jobId, u"995"_s);
    QCOMPARE(info.node, u"r33c1s01"_s);
    QCOMPARE(info.port, 18900);
    QCOMPARE(info.token, u"tok_AbC-12.x"_s);
    QVERIFY(info.complete());
    // Before the job is ready only the id is known: incomplete, but recordable.
    const auto early = ClusterConnection::parseLauncherOutput(u"[visivo] Job ID: 42\n"_s);
    QCOMPARE(early.jobId, u"42"_s);
    QVERIFY(!early.complete());
}

void TestBackendRouting::cluster_remoteCommandLineQuotesEveryArgument()
{
    // What the remote POSIX shell makes of the line must be the arguments, as data.
    const QStringList args = { u"bash"_s, u"dir with space/scripts/launch.sh"_s, u"--mem"_s,
                               u"96G"_s, u"it's"_s, u"$(touch /tmp/x);`id`"_s, u""_s };
    const QString line = ClusterConnection::remoteCommandLine(args);
    QProcess sh;
    sh.start(u"/bin/sh"_s, { u"-c"_s, u"for a in "_s + line + u"; do printf '%s\\n' \"$a\"; done"_s });
    QVERIFY(sh.waitForFinished(5000));
    const QStringList back = QString::fromUtf8(sh.readAllStandardOutput()).split(u'\n');
    QCOMPARE(back.mid(0, args.size()), args);
}

void TestBackendRouting::cluster_localPortAvoidsTheLocalBackend()
{
    const auto freeExcept = [](QList<int> busy) { return [busy](int p) { return !busy.contains(p); }; };
    QCOMPARE(ClusterConnection::chooseLocalPort(18901, 8000, freeExcept({})), 18901);
    QCOMPARE(ClusterConnection::chooseLocalPort(18900, 18900, freeExcept({})), 18901);
    QCOMPARE(ClusterConnection::chooseLocalPort(18901, 18902, freeExcept({ 18901 })), 18903);
    QCOMPARE(ClusterConnection::chooseLocalPort(18901, 0, [](int) { return false; }), 0);
}

void TestBackendRouting::cluster_tokenNeverShown()
{
    QCOMPARE(ClusterConnection::redacted(u"          Token:    tok_secret"_s),
             u"          Token:    (received, not shown)"_s);
    const QString curl = ClusterConnection::redacted(
            u"curl -sf -H 'X-Visivo-Token: tok_secret' http://localhost:18900/v1/health"_s);
    QVERIFY(!curl.contains(u"tok_secret"_s));
    QCOMPARE(ClusterConnection::redacted(u"[visivo] Token saved to /x/token"_s),
             u"[visivo] Token saved to /x/token"_s);
}

void TestBackendRouting::cluster_cancelByIdOrByName()
{
    // By the unique name whenever it is known: it works for a job whose id never
    // arrived, and for one that has already ended (scancel by id fails then).
    QCOMPARE(ClusterConnection::cancelCommand(u"995"_s, u"visivo-ab-cd"_s),
             u"scancel -u \"$USER\" --name='visivo-ab-cd'"_s);
    QCOMPARE(ClusterConnection::cancelCommand(u"995"_s, QString()), u"scancel 995"_s);
    QCOMPARE(ClusterConnection::cancelCommand(QString(), u"visivo-ab-cd"_s),
             u"scancel -u \"$USER\" --name='visivo-ab-cd'"_s);
    QCOMPARE(ClusterConnection::cancelCommand(u"-"_s, u"visivo-ab-cd"_s),
             u"scancel -u \"$USER\" --name='visivo-ab-cd'"_s);
    // A record that is not a job id is never passed to the remote shell.
    QCOMPARE(ClusterConnection::cancelCommand(u"1; rm -rf ~"_s, QString()), QString());
    QVERIFY(ClusterConnection::cancelCommand(QString(), QString()).isEmpty());
    // Cut off while sbatch may still be submitting: cancelled again for a
    // while, and it fails (the record stays) if a job is left.
    const QString settling = ClusterConnection::cancelCommand(QString(), u"visivo-ab-cd"_s, true);
    // Run by bash, whatever the login shell: tcsh has no for/do.
    QVERIFY(settling.startsWith(u"'bash' '-c' "_s));
    QVERIFY(settling.contains(u"scancel"_s) && settling.contains(u"visivo-ab-cd"_s));
    QVERIFY(settling.contains(u"squeue"_s) && settling.contains(u"test -z"_s));
}

void TestBackendRouting::cluster_profileCannotInjectSshOptions()
{
    ClusterProfile p;
    p.name = u"X"_s;
    p.frontend = u"pleiadi-gpu.oact.inaf.it"_s;
    p.user = u"fv"_s;
    QVERIFY(ClusterConnection::profileProblem(p).isEmpty());
    p.frontend = u"-oProxyCommand=touch /tmp/x"_s;
    QVERIFY(!ClusterConnection::profileProblem(p).isEmpty());
    p.frontend = u"host"_s;
    p.user = u"-F/tmp/cfg"_s;
    QVERIFY(!ClusterConnection::profileProblem(p).isEmpty());
    p.user = u"a b"_s;
    QVERIFY(!ClusterConnection::profileProblem(p).isEmpty());
    // The token and the job name are the window's.
    p.user = u"fv"_s;
    p.extraArgs = { u"--backend-src"_s, u"/scratch/app"_s };
    QVERIFY(ClusterConnection::profileProblem(p).isEmpty());
    p.extraArgs = { u"--token"_s, u"abc"_s };
    QVERIFY(!ClusterConnection::profileProblem(p).isEmpty());
    p.extraArgs = { u"--job-name=other"_s };
    QVERIFY(!ClusterConnection::profileProblem(p).isEmpty());
}

void TestBackendRouting::cluster_extraOptionsRoundTripThroughTheDialogLine()
{
    const QStringList args{ u"--backend-src"_s, u"/scratch/my app"_s, u"--version"_s,
                            u"say \"hi\""_s, u"plain"_s };
    QCOMPARE(QProcess::splitCommand(ClusterProfile::joinCommand(args)), args);
    QCOMPARE(ClusterProfile::joinCommand({ u"--a"_s, u"b"_s }), u"--a b"_s);
}

void TestBackendRouting::cluster_profilesRoundTrip()
{
    QTemporaryDir dir;
    QSettings s(dir.filePath(u"Settings.ini"_s), QSettings::IniFormat);
    ClusterProfile p;
    p.name = u"PLEIADI"_s;
    p.frontend = u"pleiadi-gpu.oact.inaf.it"_s;
    p.user = u"fv"_s;
    p.partition = u"gpu"_s;
    p.account = u"INA25_proj"_s;
    p.qos = u"boost_qos_dbg"_s;
    p.gres = u"gpu:1"_s;
    p.cpus = 16;
    p.mem = u"96G"_s;
    p.sif = u"/scratch/fv/backend-egl.sif"_s;
    p.image = u"docker://example.org/backend:1"_s;
    p.site = u"pleiadi"_s;
    p.queueWaitMin = 30;
    p.dataRoot = u"/scratch/fv/data"_s;
    p.workspace = u"/scratch/fv/visivo-ws"_s;
    p.extraArgs = { u"--backend-src"_s, u"/scratch/fv/app"_s };
    ClusterProfiles::upsert(s, p);
    ClusterProfile q = p;
    q.name = u"OTHER"_s;
    q.frontend = u"other.example"_s;
    ClusterProfiles::upsert(s, q);
    p.mem = u"128G"_s;
    ClusterProfiles::upsert(s, p);   // replaces, does not add

    QCOMPARE(ClusterProfiles::load(s).size(), 2);
    const ClusterProfile back = ClusterProfiles::find(s, u"PLEIADI"_s);
    QCOMPARE(back.mem, u"128G"_s);
    QCOMPARE(back.sshTarget(), u"fv@pleiadi-gpu.oact.inaf.it"_s);
    QCOMPARE(back.site, u"pleiadi"_s);
    QCOMPARE(back.image, u"docker://example.org/backend:1"_s);
    QCOMPARE(back.queueWaitMin, 30);
    QCOMPARE(back.launcherArgs(),
             (QStringList{ u"--site"_s, u"pleiadi"_s, u"--partition"_s, u"gpu"_s, u"--account"_s,
                           u"INA25_proj"_s, u"--qos"_s,
                           u"boost_qos_dbg"_s, u"--gres"_s, u"gpu:1"_s, u"--cpus"_s, u"16"_s,
                           u"--mem"_s, u"128G"_s, u"--sif"_s, u"/scratch/fv/backend-egl.sif"_s,
                           u"--image"_s, u"docker://example.org/backend:1"_s,
                           u"--assign-timeout"_s, u"1800"_s, u"--data-root"_s, u"/scratch/fv/data"_s, u"--workspace"_s,
                           u"/scratch/fv/visivo-ws"_s, u"--backend-src"_s, u"/scratch/fv/app"_s }));
    QVERIFY(!ClusterProfiles::find(s, u"missing"_s).isValid());
}

void TestBackendRouting::cluster_shippedLauncherIsRunFromStandardInput()
{
    // A profile with no clone: `bash -s -- <options>`, the launcher on standard input.
    ClusterProfile p;
    p.name = u"Leonardo"_s;
    p.frontend = u"login.leonardo.cineca.it"_s;
    p.applyPreset(u"leonardo"_s);
    p.account = u"PROJ_x"_s;
    QVERIFY(p.usesShippedLauncher());
    const QStringList argv = ClusterConnection::remoteArgv(p, u"visivo-job"_s, u"1.4.0"_s);
    QCOMPARE(argv.mid(0, 3), (QStringList{ u"bash"_s, u"-s"_s, u"--"_s }));
    QCOMPARE(argv.mid(argv.size() - 2), (QStringList{ u"--job-name"_s, u"visivo-job"_s }));
    // The image of this client's version, with the GPU tag, and the queue wait.
    const int image = argv.indexOf(u"--image"_s);
    QVERIFY(image > 0);
    QCOMPARE(argv.at(image + 1), u"docker://ghcr.io/visivolab/visivo-backend:1.4.0-egl"_s);
    QCOMPARE(argv.at(argv.indexOf(u"--assign-timeout"_s) + 1), u"1800"_s);
    QCOMPARE(argv.at(argv.indexOf(u"--site"_s) + 1), u"leonardo"_s);
    QCOMPARE(argv.at(argv.indexOf(u"--account"_s) + 1), u"PROJ_x"_s);
    QVERIFY(!argv.join(u' ').contains(u"scripts/launch_backend_slurm.sh"_s));
}

void TestBackendRouting::cluster_cloneProfileRunsTheClonesLauncher()
{
    // The developer path: unchanged. The clone's launcher, no image from the client.
    ClusterProfile p;
    p.name = u"PLEIADI"_s;
    p.frontend = u"pleiadi-gpu.oact.inaf.it"_s;
    p.shipLauncher = false;
    p.repoDir = u"~/VisIVOVisualAnalytics"_s;
    p.sif = u"/scratch/x/backend-egl.sif"_s;
    QVERIFY(!p.usesShippedLauncher());
    const QStringList argv = ClusterConnection::remoteArgv(p, u"visivo-job"_s, u"1.4.0"_s);
    QCOMPARE(argv.mid(0, 2),
             (QStringList{ u"bash"_s, u"VisIVOVisualAnalytics/scripts/launch_backend_slurm.sh"_s }));
    QVERIFY(!argv.contains(u"--image"_s));
    QCOMPARE(argv.at(argv.indexOf(u"--sif"_s) + 1), u"/scratch/x/backend-egl.sif"_s);
    p.repoDir = u"/home/x/clone"_s;
    p.sif.clear();
    const QStringList noImage = ClusterConnection::remoteArgv(p, u"j"_s, u"1.4.0"_s);
    QCOMPARE(noImage.at(1), u"/home/x/clone/scripts/launch_backend_slurm.sh"_s);
    QVERIFY(!noImage.contains(u"--image"_s));   // the clone's own image, as before
    // Only a clone can be older than the client.
    QVERIFY(ClusterConnection::launcherFailure(p, 1, u"Unknown option: --job-name"_s, false)
                    .contains(u"older than this client"_s));
    // No folder named: the default clone, as before the client shipped the launcher.
    p.repoDir.clear();
    QCOMPARE(ClusterConnection::remoteArgv(p, u"j"_s, u"1.4.0"_s).at(1),
             u"VisIVOVisualAnalytics/scripts/launch_backend_slurm.sh"_s);
    p.shipLauncher = true;
    QVERIFY(!ClusterConnection::launcherFailure(p, 1, u"Unknown option: --job-name"_s, false)
                     .contains(u"older than this client"_s));
}

void TestBackendRouting::cluster_imageIsTheClientsVersion()
{
    ClusterProfile p;
    p.name = u"X"_s;
    p.frontend = u"login.example.org"_s;
    p.gres = u"gpu:1"_s;
    const QString ghcr = u"docker://ghcr.io/visivolab/visivo-backend:"_s;
    QCOMPARE(p.defaultImage(u"1.4.0"_s), ghcr + u"1.4.0-egl"_s);
    // No GPU asked for: the CPU image.
    p.gres.clear();
    QCOMPARE(p.defaultImage(u"1.4.0"_s), ghcr + u"1.4.0"_s);
    p.gres = u"0"_s;
    QCOMPARE(p.defaultImage(u"1.4.0"_s), ghcr + u"1.4.0"_s);
    p.gres = u"gpu:2"_s;
    // A client that is not a release has no image: the profile names one.
    QVERIFY(p.defaultImage(u"1.5.0-dev"_s).isEmpty());
    QVERIFY(p.defaultImage(QString()).isEmpty());
    QVERIFY(!p.launcherProblem(u"1.5.0-dev"_s).isEmpty());
    p.image = u"docker://ghcr.io/visivolab/visivo-backend:1.3.0-egl"_s;
    QVERIFY(p.launcherProblem(u"1.5.0-dev"_s).isEmpty());
    QVERIFY(p.defaultImage(u"1.4.0"_s).isEmpty());
    QVERIFY(p.launcherArgs(u"1.4.0"_s).contains(p.image));   // the profile's own, not the client's
    // An image file, or an image or file in the further options, is not replaced.
    p.image.clear();
    p.sif = u"/scratch/a.sif"_s;
    QVERIFY(p.defaultImage(u"1.4.0"_s).isEmpty());
    p.sif.clear();
    p.extraArgs = { u"--image=docker://x/y:1"_s };
    QVERIFY(p.defaultImage(u"1.4.0"_s).isEmpty());
    QVERIFY(p.launcherProblem(u"1.5.0-dev"_s).isEmpty());
    // PLEIADI is POWER9: no published image, the profile or clone gives one.
    p.extraArgs.clear();
    p.site = u"pleiadi"_s;
    QVERIFY(p.defaultImage(u"1.4.0"_s).isEmpty());
    QVERIFY(!p.launcherProblem(u"1.4.0"_s).isEmpty());
    p.shipLauncher = false;   // a clone brings its own image file
    QVERIFY(p.launcherProblem(u"1.4.0"_s).isEmpty());
}

void TestBackendRouting::cluster_buildThatIsNotAReleaseNeverPicksAnImage()
{
    // Images exist for release tags only; a build from main carries the last
    // release's number, so it is told to name an image instead.
    QCOMPARE(ClusterProfile::imageVersion(u"1.4.0"_s, true), u"1.4.0"_s);
    QVERIFY(ClusterProfile::imageVersion(u"1.4.0"_s, false).isEmpty());
    ClusterProfile p;
    p.name = u"X"_s;
    p.frontend = u"login.example.org"_s;
    p.gres = u"gpu:1"_s;
    const QString dev = ClusterProfile::imageVersion(u"1.4.0"_s, false);
    QVERIFY(p.defaultImage(dev).isEmpty());
    QVERIFY(p.launcherArgs(dev).indexOf(u"--image"_s) < 0);
    const QString problem = p.launcherProblem(dev);
    QVERIFY(problem.contains(u"not a release"_s));
    QVERIFY(problem.contains(u"Image to pull"_s));
    // An image, an image file or a clone of its own makes it usable.
    p.image = u"docker://ghcr.io/visivolab/visivo-backend:1.4.0-egl"_s;
    QVERIFY(p.launcherProblem(dev).isEmpty());
    QVERIFY(p.launcherArgs(dev).contains(p.image));
    p.image.clear();
    p.sif = u"/scratch/a.sif"_s;
    QVERIFY(p.launcherProblem(dev).isEmpty());
    p.sif.clear();
    p.shipLauncher = false;
    QVERIFY(p.launcherProblem(dev).isEmpty());
    // The release build picks the version's image.
    p.shipLauncher = true;
    const QString rel = ClusterProfile::imageVersion(u"1.4.0"_s, true);
    QVERIFY(p.launcherProblem(rel).isEmpty());
    QCOMPARE(p.defaultImage(rel), u"docker://ghcr.io/visivolab/visivo-backend:1.4.0-egl"_s);
    // This test binary is not built as a release.
    QVERIFY(ClusterProfile::clientImageVersion().isEmpty());
}

void TestBackendRouting::cluster_profileSavedBeforeTheShippedLauncherKeepsItsClone()
{
    // As an older client wrote it: no ship_launcher, no site, no image; an empty
    // repo_dir meant ~/VisIVOVisualAnalytics.
    QTemporaryDir dir;
    QSettings s(dir.filePath(u"Settings.ini"_s), QSettings::IniFormat);
    s.setValue(u"Clusters/profiles"_s,
               QStringLiteral("[{\"name\":\"PLEIADI\",\"frontend\":\"pleiadi-gpu.oact.inaf.it\","
                              "\"repo_dir\":\"\",\"sif\":\"/scratch/x/b.sif\",\"cpus\":16},"
                              "{\"name\":\"Mine\",\"frontend\":\"h.example.org\",\"repo_dir\":\"src/visivo\"}]"));
    const ClusterProfile old = ClusterProfiles::find(s, u"PLEIADI"_s);
    QVERIFY(old.isValid());
    QVERIFY(!old.usesShippedLauncher());
    const QStringList argv = ClusterConnection::remoteArgv(old, u"j"_s, QString());
    QCOMPARE(argv.at(1), u"VisIVOVisualAnalytics/scripts/launch_backend_slurm.sh"_s);
    QVERIFY(!argv.contains(u"--image"_s));
    QVERIFY(old.launcherProblem(QString()).isEmpty());   // no image asked of a dev build
    const ClusterProfile mine = ClusterProfiles::find(s, u"Mine"_s);
    QVERIFY(!mine.usesShippedLauncher());
    QCOMPARE(ClusterConnection::remoteArgv(mine, u"j"_s, QString()).at(1),
             u"src/visivo/scripts/launch_backend_slurm.sh"_s);
    // Saved again, the choice is explicit and stays through a reload.
    ClusterProfiles::upsert(s, old);
    QVERIFY(!ClusterProfiles::find(s, u"PLEIADI"_s).usesShippedLauncher());
    ClusterProfile fresh;
    fresh.name = u"New"_s;
    fresh.frontend = u"login.example.org"_s;
    QVERIFY(fresh.usesShippedLauncher());
    ClusterProfiles::upsert(s, fresh);
    QVERIFY(ClusterProfiles::find(s, u"New"_s).usesShippedLauncher());
}

void TestBackendRouting::cluster_sitePresets()
{
    ClusterProfile p;
    p.user = u"me"_s;
    p.account = u"MY_PROJECT"_s;
    p.dataRoot = u"/data/mine"_s;
    p.workspace = u"/ws/mine"_s;
    p.repoDir = u"clone"_s;
    p.qos = u"left_over"_s;
    p.applyPreset(u"leonardo"_s);
    QCOMPARE(p.name, u"Leonardo"_s);
    QCOMPARE(p.site, u"leonardo"_s);
    QCOMPARE(p.frontend, u"login.leonardo.cineca.it"_s);
    QCOMPARE(p.partition, u"boost_usr_prod"_s);
    QCOMPARE(p.gres, u"gpu:1"_s);
    QVERIFY(p.qos.isEmpty());
    QVERIFY(!p.time.isEmpty() && !p.mem.isEmpty() && p.cpus > 0);
    QVERIFY(p.queueWaitMin >= 30);   // a production queue: much longer than the launcher's 5 min
    // The user's own entries stay.
    QCOMPARE(p.user, u"me"_s);
    QCOMPARE(p.account, u"MY_PROJECT"_s);
    QCOMPARE(p.dataRoot, u"/data/mine"_s);
    QCOMPARE(p.workspace, u"/ws/mine"_s);
    QCOMPARE(p.repoDir, u"clone"_s);
    QVERIFY(p.launcherArgs(u"1.4.0"_s).contains(u"--site"_s));
    // A name the user chose is kept.
    p.name = u"my cluster"_s;
    p.applyPreset(u"pleiadi"_s);
    QCOMPARE(p.name, u"my cluster"_s);
    QCOMPARE(p.site, u"pleiadi"_s);
    QCOMPARE(p.frontend, u"pleiadi-gpu.oact.inaf.it"_s);
    QCOMPARE(p.partition, u"gpu"_s);
    QCOMPARE(p.queueWaitMin, 0);
    // "Other" keeps what is there and names no site.
    const QString frontend = p.frontend;
    p.applyPreset(QString());
    QVERIFY(p.site.isEmpty());
    QCOMPARE(p.frontend, frontend);
    QCOMPARE(ClusterProfile::presetLabel(u"leonardo"_s), u"Leonardo (CINECA)"_s);
    QCOMPARE(ClusterProfile::presetLabel(u"pleiadi"_s), u"PLEIADI (INAF)"_s);
    QCOMPARE(ClusterProfile::presetLabel(QString()), u"Other"_s);
    // Leonardo needs the project account.
    ClusterProfile l;
    l.applyPreset(u"leonardo"_s);
    QVERIFY(!l.launcherProblem(u"1.4.0"_s).isEmpty());
    l.account = u"A"_s;
    QVERIFY(l.launcherProblem(u"1.4.0"_s).isEmpty());
    // Only Leonardo's login nodes are Leonardo; CINECA's other hosts are not.
    ClusterProfile host;
    for (const QString &h : { u"login.leonardo.cineca.it"_s, u"LOGIN01-EXT.LEONARDO.CINECA.IT"_s,
                              u"login02.leonardo.local"_s }) {
        host.frontend = h;
        QCOMPARE(host.effectiveSite(), u"leonardo"_s);
    }
    for (const QString &h : { u"login.g100.cineca.it"_s, u"login.marconi.cineca.it"_s,
                              u"data.cineca.it"_s, u"login.example.local"_s, u"leonardo.example.org"_s }) {
        host.frontend = h;
        QVERIFY2(host.effectiveSite().isEmpty(), qPrintable(h));
    }
    // The site is also known from an older profile's options or host.
    ClusterProfile old;
    old.frontend = u"login01-ext.leonardo.cineca.it"_s;
    QCOMPARE(old.effectiveSite(), u"leonardo"_s);
    old.frontend = u"somewhere.example"_s;
    old.extraArgs = { u"--site"_s, u"pleiadi"_s };
    QCOMPARE(old.effectiveSite(), u"pleiadi"_s);
    old.extraArgs = { u"--site=leonardo"_s };
    QCOMPARE(old.effectiveSite(), u"leonardo"_s);
    QVERIFY(!old.launcherArgs().contains(u"--site"_s));   // the options carry it
}

void TestBackendRouting::cluster_sshFailureMessages()
{
    ClusterProfile leonardo;
    leonardo.frontend = u"login.leonardo.cineca.it"_s;
    leonardo.user = u"me"_s;
    leonardo.site = u"leonardo"_s;
    const QString renew = ClusterConnection::launcherFailure(leonardo, 255, u"Permission denied"_s, false);
    QVERIFY(renew.contains(u"renew your CINECA SSH certificate"_s));
    QVERIFY(renew.contains(u"step ssh login"_s));
    QVERIFY(!renew.contains(u"ssh-copy-id"_s));
    ClusterProfile other;
    other.frontend = u"login.example.org"_s;
    const QString generic = ClusterConnection::launcherFailure(other, 255, QString(), false);
    QVERIFY(generic.contains(u"ssh-copy-id"_s));
    QVERIFY(!generic.contains(u"CINECA"_s));
    // A host key that is not accepted yet: said first, on any site, and told how.
    const QString hostKey = u"Host key verification failed.\n"_s;
    for (const ClusterProfile &p : { leonardo, other }) {
        const QString m = ClusterConnection::launcherFailure(p, 255, hostKey, false);
        QVERIFY(m.contains(u"host key"_s));
        QVERIFY(m.contains(u"from a terminal"_s));
        QVERIFY(!m.contains(u"step ssh login"_s));
    }
    // The connection dropped after the launcher had been talking: ssh got in, so
    // the certificate is no suspect; the job's cancel is what is said.
    const QString dropped = ClusterConnection::launcherFailure(
            leonardo, 255, u"[visivo] Job ID: 42\n[visivo] Queue: PENDING (Priority), 60 s"_s, false);
    QVERIFY(dropped.contains(u"dropped"_s));
    QVERIFY(dropped.contains(u"cancelled"_s));
    QVERIFY(!dropped.contains(u"step ssh login"_s));
    QVERIFY(!dropped.contains(u"ssh-copy-id"_s));
    // The launcher's own failures are not ssh's.
    QVERIFY(ClusterConnection::launcherFailure(leonardo, 1, u"x"_s, true).contains(u"failed (exit 1)"_s));
    QVERIFY(ClusterConnection::launcherFailure(leonardo, 1, u"x"_s, false).contains(u"did not report"_s));
}

void TestBackendRouting::cluster_progressLinesBecomeStatus()
{
    using C = ClusterConnection;
    QCOMPARE(C::progressStatus(u"[visivo] Queue: PENDING (Priority), 125 s since submission"_s),
             u"Waiting in the queue: Priority (2 min)"_s);
    QCOMPARE(C::progressStatus(u"[visivo] Queue: PENDING (ReqNodeNotAvail, May be reserved), 0 s since submission"_s),
             u"Waiting in the queue: ReqNodeNotAvail, May be reserved (0 min)"_s);
    QVERIFY(C::progressStatus(u"[visivo] Queue: SUSPENDED (None), 40 s since submission"_s)
                    .contains(u"suspended"_s));
    QCOMPARE(C::progressStatus(u"[visivo] Pull: 40 s, 1200 MB fetched"_s),
             u"Pulling the backend image: 1200 MB fetched (40 s)"_s);
    QVERIFY(C::progressStatus(u"[visivo] Pull: done in 95 s"_s).contains(u"95"_s));
    QVERIFY(C::progressStatus(u"[visivo] Pulling docker://x/y:1 to /w/x.sif (a few GB; this can take minutes)..."_s)
                    .startsWith(u"Pulling"_s));
    QVERIFY(C::progressStatus(u"[visivo] Compute node: lrdn0001"_s).contains(u"lrdn0001"_s));
    QVERIFY(C::progressStatus(u"[visivo]  attempt 3/120 – not ready yet..."_s).contains(u"3 of 120"_s));
    // Anything else is a plain log line.
    QVERIFY(C::progressStatus(u"[visivo] Job ID: 42"_s).isEmpty());
    QVERIFY(C::progressStatus(QString()).isEmpty());
}

void TestBackendRouting::cluster_stdinPayloadRunsFromAFileWithItsOwnInput()
{
    // What the frontend's `bash -s` does with the payload, run for real: the
    // script runs from a file ($0 names it, so it can run itself again), a
    // command in it that reads standard input finds it closed, the arguments
    // arrive, and the copy is gone afterwards.
    const QString script = u"#!/usr/bin/env bash\n"
                           u"[ -f \"$0\" ] && echo \"file=yes\"\n"
                           u"echo \"stdin=${VISIVO_LAUNCHER_STDIN:-}\"\n"
                           u"echo \"args=$*\"\n"
                           u"cat > /dev/null\n"          // would swallow the rest of the payload
                           u"echo \"after-cat\"\n"
                           u"echo \"self=$0\"\n"
                           u"echo 'VISIVO_LAUNCHER_END'\n"   // the default tag, inside the script
                           u"exit 3\n"_s;
    QTemporaryDir tmp;
    QProcess bash;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(u"TMPDIR"_s, tmp.path());
    bash.setProcessEnvironment(env);
    bash.start(u"/bin/bash"_s, { u"-s"_s, u"--"_s, u"--partition"_s, u"a b"_s });
    QVERIFY(bash.waitForStarted(5000));
    bash.write(ClusterConnection::stdinPayload(script));
    bash.closeWriteChannel();
    QVERIFY(bash.waitForFinished(10000));
    const QString out = QString::fromUtf8(bash.readAllStandardOutput());
    QCOMPARE(bash.exitCode(), 3);   // the launcher's exit status comes back
    QVERIFY2(out.contains(u"file=yes"_s), qPrintable(out));
    QVERIFY(out.contains(u"stdin=1"_s));
    QVERIFY(out.contains(u"args=--partition a b"_s));
    QVERIFY(out.contains(u"after-cat"_s));
    QVERIFY(out.contains(u"self="_s + tmp.path()));
    QVERIFY(QDir(tmp.path()).entryList(QDir::Files).isEmpty());

    // The real launcher through the same payload: it runs from the copy, reads
    // its options and answers with its own exit status (an unknown site is refused).
    QFile real(QStringLiteral(VISIVO_REPO_ROOT) + u"/scripts/launch_backend_slurm.sh"_s);
    QVERIFY(real.open(QIODevice::ReadOnly));
    QProcess launcher;
    launcher.setProcessEnvironment(env);
    launcher.start(u"/bin/bash"_s, { u"-s"_s, u"--"_s, u"--site"_s, u"nowhere"_s });
    QVERIFY(launcher.waitForStarted(5000));
    launcher.write(ClusterConnection::stdinPayload(QString::fromUtf8(real.readAll())));
    launcher.closeWriteChannel();
    QVERIFY(launcher.waitForFinished(10000));
    QCOMPARE(launcher.exitCode(), 1);
    QVERIFY(QString::fromUtf8(launcher.readAllStandardError()).contains(u"unknown site 'nowhere'"_s));
    QVERIFY(QDir(tmp.path()).entryList(QDir::Files).isEmpty());
}

void TestBackendRouting::launchOptions_connectCluster()
{
    const QProcessEnvironment none;
    QString name, error;
    QVERIFY(LaunchOptions::clusterToConnect({ u"visivo"_s }, none, &name, &error));
    QVERIFY(name.isEmpty());
    QVERIFY(LaunchOptions::clusterToConnect({ u"visivo"_s, u"--connect-cluster"_s, u"PLEIADI"_s },
                                            none, &name, &error));
    QCOMPARE(name, u"PLEIADI"_s);
    QVERIFY(!LaunchOptions::clusterToConnect({ u"visivo"_s, u"--connect-cluster"_s }, none, &name, &error));
    QVERIFY(!LaunchOptions::clusterToConnect(
            { u"visivo"_s, u"--connect-cluster=X"_s, u"--backend"_s, u"http://h:1"_s }, none, &name, &error));
    QProcessEnvironment env;
    env.insert(u"VISIVO_BACKEND_URL"_s, u"http://h:1"_s);
    QVERIFY(!LaunchOptions::clusterToConnect({ u"visivo"_s, u"--connect-cluster"_s, u"X"_s }, env, &name,
                                             &error));
    QVERIFY(LaunchOptions::backendValueOptions().contains(u"--connect-cluster"_s));
}

void TestBackendRouting::recents_recordTheBackendTheyWereOpenedOn()
{
    QTemporaryDir dir;
    {
        RecentDatasetsManager local(dir.path());
        local.record(u"/Users/me/cube.fits"_s, u"cube.fits"_s, u"cube"_s);
        QVERIFY(local.find(u"/Users/me/cube.fits"_s).backend.isEmpty());   // the local backend
        QVERIFY(local.find(u"/Users/me/cube.fits"_s).backendKnown);        // known to be local
    }
    Settings::setSessionBackend({ u"http://127.0.0.1:18901"_s, u"t"_s, u"Pleiadi"_s, true });
    {
        RecentDatasetsManager onCluster(dir.path());
        onCluster.record(u"/scratch/d/big.fits"_s, u"big.fits"_s, u"cube"_s);
        RecentDataset skava;
        skava.id = u"skava:obs1"_s;
        skava.source = u"skava"_s;
        skava.obsId = u"obs1"_s;
        onCluster.record(skava);
    }
    Settings::setSessionBackend({ u"http://h:1"_s, u"t"_s, u"lab"_s, false });
    {
        RecentDatasetsManager viaBackend(dir.path());
        viaBackend.record(u"/data/x.fits"_s, u"x.fits"_s, u"image"_s);
    }
    Settings::clearSessionBackend();

    RecentDatasetsManager reread(dir.path());
    const RecentDataset big = reread.find(u"/scratch/d/big.fits"_s);
    QCOMPARE(big.backend, u"Pleiadi"_s);
    QVERIFY(big.backendIsCluster);
    const RecentDataset x = reread.find(u"/data/x.fits"_s);
    QCOMPARE(x.backend, u"lab"_s);
    QVERIFY(!x.backendIsCluster);
    // SKAVA entries are resolved through discovery again, wherever they are clicked.
    QVERIFY(reread.find(u"skava:obs1"_s).backend.isEmpty());
    QVERIFY(reread.find(u"/Users/me/cube.fits"_s).backend.isEmpty());
    QVERIFY(reread.find(u"missing"_s).id.isEmpty());
    QCOMPARE(LaunchOptions::recentToReopen({ u"v"_s, u"--reopen-recent"_s, u"/scratch/d/big.fits"_s }),
             u"/scratch/d/big.fits"_s);
    QVERIFY(LaunchOptions::recentToReopen({ u"v"_s }).isEmpty());
    QVERIFY(LaunchOptions::backendValueOptions().contains(u"--reopen-recent"_s));
}

void TestBackendRouting::recents_twoWindowsKeepEachOthersEntries()
{
    // A local window and a cluster window, both open, share Settings.ini.
    QTemporaryDir dir;
    RecentDatasetsManager first(dir.path());
    RecentDatasetsManager second(dir.path());
    first.record(u"/a.fits"_s, u"a"_s, u"cube"_s);
    second.record(u"/b.fits"_s, u"b"_s, u"cube"_s);   // must not drop /a.fits
    first.record(u"/c.fits"_s, u"c"_s, u"cube"_s);    // must not drop /b.fits
    RecentDatasetsManager reread(dir.path());
    QCOMPARE(reread.datasets().size(), 3);
    QCOMPARE(reread.datasets().at(0).id, u"/c.fits"_s);
    second.remove(u"/a.fits"_s);
    QCOMPARE(RecentDatasetsManager(dir.path()).datasets().size(), 2);
}

void TestBackendRouting::recents_samePathOnTwoBackendsAndOldEntries()
{
    QTemporaryDir dir;
    // An entry saved before backends were recorded: no backend keys at all.
    {
        QSettings raw(dir.filePath(u"Settings.ini"_s), QSettings::IniFormat);
        raw.setValue(u"RecentDatasets/count"_s, 1);
        raw.setValue(u"RecentDatasets/0/id"_s, u"/scratch/old.fits"_s);
        raw.setValue(u"RecentDatasets/0/type"_s, u"cube"_s);
        raw.setValue(u"RecentDatasets/0/source"_s, u"remote"_s);
    }
    RecentDatasetsManager m(dir.path());
    const RecentDataset old = m.find(u"/scratch/old.fits"_s, u"Pleiadi"_s);
    QCOMPARE(old.id, u"/scratch/old.fits"_s);   // unknown backend: matches any
    QVERIFY(!old.backendKnown);

    // The same path opened locally and on a cluster: two entries.
    m.record(u"/data/shared.fits"_s, u"shared"_s, u"cube"_s);
    Settings::setSessionBackend({ u"http://127.0.0.1:18901"_s, u"t"_s, u"Pleiadi"_s, true });
    m.record(u"/data/shared.fits"_s, u"shared"_s, u"cube"_s);
    Settings::clearSessionBackend();
    QCOMPARE(m.find(u"/data/shared.fits"_s, QString()).backend, QString());
    QCOMPARE(m.find(u"/data/shared.fits"_s, u"Pleiadi"_s).backend, u"Pleiadi"_s);
    QCOMPARE(m.datasets().size(), 3);
    // Removing one (the Data Hub's x) leaves the other.
    m.remove(u"/data/shared.fits"_s, u"Pleiadi"_s);
    QCOMPARE(m.find(u"/data/shared.fits"_s, u"Pleiadi"_s).id, QString());
    QCOMPARE(m.find(u"/data/shared.fits"_s, QString()).id, u"/data/shared.fits"_s);
    // Reopening an old entry on a known backend replaces it rather than adding one.
    Settings::setSessionBackend({ u"http://127.0.0.1:18901"_s, u"t"_s, u"Pleiadi"_s, true });
    m.record(u"/scratch/old.fits"_s, u"old"_s, u"cube"_s);
    Settings::clearSessionBackend();
    int copies = 0;
    for (const auto &e : RecentDatasetsManager(dir.path()).datasets())
        copies += e.id == u"/scratch/old.fits"_s;
    QCOMPARE(copies, 1);
}

void TestBackendRouting::upsertInsertsAndUpdates()
{
    QTemporaryDir dir;
    Settings s(dir.path());

    s.upsertBackendNode(makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s));
    QCOMPARE(s.getBackendNode(u"src-a"_s).url, u"https://a.example/visivo"_s);

    // Upsert with the same id updates in place (no duplicate).
    s.upsertBackendNode(makeNode(u"src-a"_s, u"https://a2.example/visivo"_s, u"A"_s));
    QCOMPARE(s.getBackendNode(u"src-a"_s).url, u"https://a2.example/visivo"_s);

    int count = 0;
    for (const auto &n : s.getBackendNodes())
        if (n.id == "src-a"_L1) ++count;
    QCOMPARE(count, 1);
}

void TestBackendRouting::removeDropsNode()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s));
    QVERIFY(!s.getBackendNode(u"src-b"_s).id.isEmpty());

    s.removeBackendNode(u"src-b"_s);
    QVERIFY2(s.getBackendNode(u"src-b"_s).id.isEmpty(), "removed node must be gone");
}

void TestBackendRouting::localCannotBeRemoved()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.removeBackendNode(u"local"_s);  // must be a no-op
    QCOMPARE(s.getBackendNode(u"local"_s).id, u"local"_s);
}

void TestBackendRouting::defaultBackendIdFallsBackToLocal()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    QCOMPARE(s.getDefaultBackendId(), u"local"_s);
    s.setDefaultBackendId(QString());  // empty → coerced back to local
    QCOMPARE(s.getDefaultBackendId(), u"local"_s);
    s.setDefaultBackendId(u"src-a"_s);
    QCOMPARE(s.getDefaultBackendId(), u"src-a"_s);
}

void TestBackendRouting::routeByExactUrl()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-it"_s, u"https://srcit.inaf.it/visivo"_s, u"INAF-IT"_s));

    const auto pick = pickBackendForSkavaDataset(
            &s, datalinkWith(u"https://srcit.inaf.it/visivo"_s, QString()));
    QCOMPARE(pick.id, u"src-it"_s);
}

void TestBackendRouting::routeByUrlIgnoresTrailingSlash()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-it"_s, u"https://srcit.inaf.it/visivo"_s, u"INAF-IT"_s));

    // Descriptor URL has a trailing slash; registry URL does not — must match.
    const auto pick = pickBackendForSkavaDataset(
            &s, datalinkWith(u"https://srcit.inaf.it/visivo/"_s, QString()));
    QCOMPARE(pick.id, u"src-it"_s);
}

void TestBackendRouting::routeByNodeCode()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-it"_s, u"https://srcit.inaf.it/visivo"_s, u"INAF-IT"_s));

    // URL differs, but node_code matches the registry srcCode.
    const auto pick = pickBackendForSkavaDataset(
            &s, datalinkWith(u"https://other.example/visivo"_s, u"INAF-IT"_s));
    QCOMPARE(pick.id, u"src-it"_s);
}

void TestBackendRouting::routeNoMatchReturnsEmpty()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    s.upsertBackendNode(makeNode(u"src-it"_s, u"https://srcit.inaf.it/visivo"_s, u"INAF-IT"_s));

    const auto pick = pickBackendForSkavaDataset(
            &s, datalinkWith(u"https://unknown.example/visivo"_s, u"UNKNOWN"_s));
    QVERIFY2(pick.id.isEmpty(), "no descriptor match must yield an empty node (→ local fallback)");
}

void TestBackendRouting::routeEmptyDatalinkReturnsEmpty()
{
    QTemporaryDir dir;
    Settings s(dir.path());
    SkavaDatalinkResponse empty;  // no visivoBackends
    empty.valid = true;
    QVERIFY(pickBackendForSkavaDataset(&s, empty).id.isEmpty());
}

void TestBackendRouting::routeNullSettingsReturnsEmpty()
{
    QVERIFY(pickBackendForSkavaDataset(
                    nullptr, datalinkWith(u"https://x.example/visivo"_s, u"X"_s))
                    .id.isEmpty());
}

void TestBackendRouting::routeUsesTheChosenNodesReplica()
{
    // Only node B has a registered backend: it must be sent B's path, not
    // the best node's primary access URL (which is a path on node A).
    const QList<BackendNode> registry = { makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s) };
    const SkavaRoute route = routeSkavaDataset(registry, twoNodeDatalink());
    QVERIFY(route.valid());
    QCOMPARE(route.node.id, u"src-b"_s);
    QCOMPARE(route.accessUrl, u"file:///mnt/b/archive/cube.fits"_s);
    QCOMPARE(route.nodeCode, u"B"_s);
    QCOMPARE(route.skipped.size(), 1);   // A: no registered backend
}

void TestBackendRouting::routePrefersRankingOrder()
{
    const QList<BackendNode> registry = { makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s),
                                          makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s) };
    const SkavaRoute route = routeSkavaDataset(registry, twoNodeDatalink());
    QCOMPARE(route.node.id, u"src-a"_s);
    QCOMPARE(route.accessUrl, u"file:///srv/a/cube.fits"_s);
}

void TestBackendRouting::routeSkipsUnsupportedKind()
{
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.visivoBackends[0].supportedKinds = { u"image"_s };     // A cannot open cubes
    const QList<BackendNode> registry = { makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s),
                                          makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s) };
    const SkavaRoute route = routeSkavaDataset(registry, dl);
    QCOMPARE(route.node.id, u"src-b"_s);
    QVERIFY(route.skipped.first().contains(u"does not open cube"_s));
}

void TestBackendRouting::routeSkipsAuthWithoutToken()
{
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.visivoBackends = { descriptor(u"https://a.example/visivo"_s, u"A"_s, {}, /*requiresAuth=*/true) };
    QList<BackendNode> registry = { makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s) };
    SkavaRoute route = routeSkavaDataset(registry, dl);
    QVERIFY(!route.valid());
    QVERIFY(route.skipped.first().contains(u"requires a token"_s));
    // The automatic token is the local backend's: it does not stand in for a
    // remote node's missing one...
    QVERIFY(!routeSkavaDataset(registry, dl, /*autoToken=*/true).valid());
    // ...but it does for the local backend itself.
    {
        SkavaDatalinkResponse local = twoNodeDatalink();
        local.visivoBackends = { descriptor(u"http://127.0.0.1:8000"_s, u"A"_s, {}, /*requiresAuth=*/true) };
        const QList<BackendNode> reg = { makeNode(u"src-a"_s, u"http://127.0.0.1:8000"_s, u"A"_s) };
        BackendClient::setLocalBackendUrl(QString());
        QVERIFY(routeSkavaDataset(reg, local, /*autoToken=*/true).valid());
        QVERIFY(!routeSkavaDataset(reg, local, /*autoToken=*/false).valid());
    }
    registry[0].token = u"secret"_s;
    route = routeSkavaDataset(registry, dl);
    QVERIFY(route.valid());
}

void TestBackendRouting::routeSkipsNodeWithoutReplica()
{
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.replicas = { replica(u"A"_s, u"file:///srv/a/cube.fits"_s) };   // B lists no copy
    const QList<BackendNode> registry = { makeNode(u"src-b"_s, u"https://b.example/visivo"_s, u"B"_s) };
    const SkavaRoute route = routeSkavaDataset(registry, dl);
    QVERIFY(!route.valid());
    QVERIFY(route.skipped.last().contains(u"no replica"_s));
}

void TestBackendRouting::kindMapping()
{
    QVERIFY(skavaBackendSupportsKind({}, u"cube"_s));
    QVERIFY(skavaBackendSupportsKind({ u"cube"_s }, QString()));
    QVERIFY(skavaBackendSupportsKind({ u"image"_s, u"cube"_s, u"dynspec"_s }, u"Cube"_s));
    QVERIFY(skavaBackendSupportsKind({ u"dynspec"_s }, u"timeseries"_s));
    QVERIFY(!skavaBackendSupportsKind({ u"image"_s }, u"cube"_s));
}

void TestBackendRouting::routeTreatsLoopbackNamesAsOneHost()
{
    // SKAVA in Docker advertises the desktop's own backend as host.docker.internal.
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.visivoBackends = { descriptor(u"http://host.docker.internal:8001/"_s, u"VIS-LOCAL"_s) };
    dl.bestNode = u"VIS-LOCAL"_s;
    const QList<BackendNode> registry = { makeNode(u"local"_s, u"http://127.0.0.1:8001"_s, QString()) };
    QVERIFY(routeSkavaDataset(registry, dl).valid());
    // A different port is a different backend.
    const QList<BackendNode> other = { makeNode(u"local"_s, u"http://127.0.0.1:8000"_s, QString()) };
    QVERIFY(!routeSkavaDataset(other, dl).valid());
}

void TestBackendRouting::findRegisteredBackendMatchesLikeRouting()
{
    const QList<BackendNode> registry = { makeNode(u"src-a"_s, u"https://a.example/visivo"_s, u"A"_s),
                                          makeNode(u"local"_s, u"http://127.0.0.1:8001"_s, QString()) };
    QCOMPARE(findRegisteredBackend(registry, descriptor(u"https://a.example/visivo/"_s, QString())), 0);
    QCOMPARE(findRegisteredBackend(registry, descriptor(u"https://other/visivo"_s, u"A"_s)), 0);
    QCOMPARE(findRegisteredBackend(registry, descriptor(u"http://host.docker.internal:8001"_s, u"X"_s)), 1);
    QCOMPARE(findRegisteredBackend(registry, descriptor(u"https://nowhere"_s, u"Z"_s)), -1);
    // Registered, but the node wants a token neither the registry nor the
    // automatic fallback provides: found (so the prompt edits THIS entry),
    // and not routable.
    SkavaDatalinkResponse dl = twoNodeDatalink();
    dl.visivoBackends = { descriptor(u"https://a.example/visivo"_s, u"A"_s, {}, /*requiresAuth=*/true) };
    QCOMPARE(findRegisteredBackend(registry, dl.visivoBackends.first()), 0);
    QVERIFY(!routeSkavaDataset(registry, dl, /*autoToken=*/false).valid());
}

void TestBackendRouting::launchOptions_oneInstanceSeatPerBackend()
{
    // A second launch hands its files to the running instance on the SAME
    // backend only (#150): a path means something to its own backend.
    const QString home = u"/Users/me"_s;
    QCOMPARE(LaunchOptions::instanceSeat(std::nullopt, QString()), QString());
    const Settings::SessionBackend a{ u"http://Node-A:18960"_s, u"t"_s, u"a"_s };
    const Settings::SessionBackend aAgain{ u"http://node-a:18960"_s, u"other"_s, u"renamed"_s };
    const Settings::SessionBackend b{ u"http://node-b:18960"_s, u"t"_s, u"b"_s };
    // Same address, whatever the token, the name or the host's case.
    QCOMPARE(LaunchOptions::instanceSeat(a, QString()),
             LaunchOptions::instanceSeat(aAgain, QString()));
    QVERIFY(LaunchOptions::instanceSeat(a, QString()) != LaunchOptions::instanceSeat(b, QString()));
    // A cluster window is its own seat, by profile name.
    QCOMPARE(LaunchOptions::instanceSeat(std::nullopt, u"pleiadi"_s), u"cluster:pleiadi"_s);

    const QString local = LaunchOptions::instanceKey(home, QString());
    const QString remote =
            LaunchOptions::instanceKey(home, LaunchOptions::instanceSeat(a, QString()));
    const QString cluster = LaunchOptions::instanceKey(home, u"cluster:pleiadi"_s);
    QVERIFY(local != remote && local != cluster && remote != cluster);
    // The local seat keeps the key earlier releases used, so a running older
    // instance is still found: "visivo-va-" + 16 hex digits of SHA-256(home).
    QCOMPARE(local, u"visivo-va-"_s
                     + QString::fromLatin1(QCryptographicHash::hash(home.toUtf8(),
                                                                    QCryptographicHash::Sha256)
                                                   .toHex()
                                                   .left(16)));
    QCOMPARE(remote.size(), local.size());
    QVERIFY(LaunchOptions::instanceKey(u"/Users/other"_s, QString()) != local);
}

void TestBackendRouting::launchOptions_datasetPathsFollowTheBackend()
{
    // A file here, a path that is not, options and their values (issue #108).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString here = dir.filePath(u"cube.fits"_s);
    QFile file(here);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    const QString remotePath = u"/scratch/node/data/WALLABY.fits"_s;
    const QStringList args{ u"visivo"_s,
                            u"--backend"_s, u"http://localhost:18960"_s,
                            u"--backend-token-file"_s, u"/home/me/.token"_s,
                            u"--backend-name"_s, u"pleiadi-gpu"_s,
                            u"--no-backend-autostart"_s,
                            remotePath, here };

    // On a remote backend every path is the backend's, as it was given.
    QStringList missing;
    QCOMPARE(LaunchOptions::datasetPaths(args, true, &missing),
             QStringList({ remotePath, here }));
    QVERIFY(missing.isEmpty());

    // On the local backend only what exists here, the rest reported.
    QCOMPARE(LaunchOptions::datasetPaths(args, false, &missing),
             QStringList({ QFileInfo(here).absoluteFilePath() }));
    QCOMPARE(missing, QStringList({ remotePath }));
    // A missing relative path is reported absolute: it may be handed to a
    // running instance, whose working directory is another (#150).
    missing.clear();
    LaunchOptions::datasetPaths({ u"visivo"_s, u"gone/cube.fits"_s }, false, &missing);
    QCOMPARE(missing, QStringList({ QDir::current().absoluteFilePath(u"gone/cube.fits"_s) }));

    // Option values are never files, even when one exists by that name.
    QCOMPARE(LaunchOptions::datasetPaths({ u"visivo"_s, u"--backend-token-file"_s, here }, false),
             QStringList());
}
