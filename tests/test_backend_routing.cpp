#include "app/BackendClient.h"
#include "TestBackendRouting.h"

#include "Settings.h"
#include "app/ClusterConnection.h"
#include "app/ClusterProfile.h"
#include "app/LaunchOptions.h"
#include "app/RecentDatasetsManager.h"
#include "skava/BackendRouting.h"
#include "skava/SkavaClient.h"

#include <QFile>
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
    QCOMPARE(back.launcherArgs(),
             (QStringList{ u"--partition"_s, u"gpu"_s, u"--account"_s, u"INA25_proj"_s, u"--qos"_s,
                           u"boost_qos_dbg"_s, u"--gres"_s, u"gpu:1"_s, u"--cpus"_s, u"16"_s,
                           u"--mem"_s, u"128G"_s, u"--sif"_s, u"/scratch/fv/backend-egl.sif"_s,
                           u"--data-root"_s, u"/scratch/fv/data"_s, u"--workspace"_s,
                           u"/scratch/fv/visivo-ws"_s, u"--backend-src"_s, u"/scratch/fv/app"_s }));
    QVERIFY(!ClusterProfiles::find(s, u"missing"_s).isValid());
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
