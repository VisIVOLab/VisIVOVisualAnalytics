#include "ClusterProfile.h"

#include <algorithm>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace {

const QString kKey = u"Clusters/profiles"_s;

QJsonObject toJson(const ClusterProfile &p)
{
    return { { u"name"_s, p.name },           { u"frontend"_s, p.frontend },
             { u"user"_s, p.user },           { u"repo_dir"_s, p.repoDir },
             { u"ship_launcher"_s, p.shipLauncher },
             { u"site"_s, p.site },           { u"image"_s, p.image },
             { u"queue_wait_min"_s, p.queueWaitMin },
             { u"partition"_s, p.partition }, { u"gres"_s, p.gres },
             { u"account"_s, p.account },     { u"qos"_s, p.qos },
             { u"cpus"_s, p.cpus },           { u"mem"_s, p.mem },
             { u"time"_s, p.time },           { u"sif"_s, p.sif },
             { u"data_root"_s, p.dataRoot },  { u"workspace"_s, p.workspace },
             { u"node_code"_s, p.nodeCode },
             { u"extra_args"_s, QJsonArray::fromStringList(p.extraArgs) } };
}

ClusterProfile fromJson(const QJsonObject &o)
{
    ClusterProfile p;
    p.name = o.value(u"name"_s).toString();
    p.frontend = o.value(u"frontend"_s).toString();
    p.user = o.value(u"user"_s).toString();
    p.repoDir = o.value(u"repo_dir"_s).toString();
    // A profile saved before the client shipped the launcher has no such key,
    // and its empty clone meant ~/VisIVOVisualAnalytics: it stays a clone.
    p.shipLauncher = o.value(u"ship_launcher"_s).toBool(false);
    p.site = o.value(u"site"_s).toString();
    p.image = o.value(u"image"_s).toString();
    p.queueWaitMin = o.value(u"queue_wait_min"_s).toInt(0);
    p.partition = o.value(u"partition"_s).toString();
    p.gres = o.value(u"gres"_s).toString();
    p.account = o.value(u"account"_s).toString();
    p.qos = o.value(u"qos"_s).toString();
    p.cpus = o.value(u"cpus"_s).toInt(16);
    p.mem = o.value(u"mem"_s).toString();
    p.time = o.value(u"time"_s).toString();
    p.sif = o.value(u"sif"_s).toString();
    p.dataRoot = o.value(u"data_root"_s).toString();
    p.workspace = o.value(u"workspace"_s).toString();
    p.nodeCode = o.value(u"node_code"_s).toString();
    for (const auto &v : o.value(u"extra_args"_s).toArray())
        p.extraArgs << v.toString();
    return p;
}

}  // namespace

QString ClusterProfile::sshTarget() const
{
    const QString host = frontend.trimmed();
    return user.trimmed().isEmpty() ? host : u"%1@%2"_s.arg(user.trimmed(), host);
}

namespace {
// True when the further options already give @p option (alone or as option=value).
bool givesOption(const QStringList &args, const QString &option)
{
    return std::any_of(args.begin(), args.end(), [&option](const QString &a) {
        return a == option || a.startsWith(option + u'=');
    });
}

constexpr auto kBackendImage = "docker://ghcr.io/visivolab/visivo-backend";
}  // namespace

QString ClusterProfile::effectiveSite() const
{
    if (!site.trimmed().isEmpty())
        return site.trimmed();
    for (qsizetype i = 0; i < extraArgs.size(); ++i) {
        if (extraArgs.at(i) == u"--site"_s && i + 1 < extraArgs.size())
            return extraArgs.at(i + 1);
        if (extraArgs.at(i).startsWith(u"--site="_s))
            return extraArgs.at(i).mid(7);
    }
    // Leonardo's login nodes only (login.leonardo.cineca.it, login01-ext.leonardo.cineca.it,
    // login*.leonardo.local): CINECA runs other clusters with other advice.
    const QString host = frontend.trimmed().toLower();
    if ((host.endsWith(u".cineca.it"_s) && host.contains(u"leonardo"_s))
        || (host.startsWith(u"login"_s) && host.endsWith(u".leonardo.local"_s)))
        return u"leonardo"_s;
    if (host.contains(u"pleiadi"_s))
        return u"pleiadi"_s;
    return {};
}

QString ClusterProfile::defaultImage(const QString &clientVersion) const
{
    static const QRegularExpression release(u"^[0-9]+\\.[0-9]+\\.[0-9]+$"_s);
    if (!usesShippedLauncher() || !image.trimmed().isEmpty() || !sif.trimmed().isEmpty()
        || givesOption(extraArgs, u"--image"_s) || givesOption(extraArgs, u"--sif"_s)
        || !release.match(clientVersion).hasMatch())
        return {};
    // The published images are for x86 and ARM; PLEIADI's nodes are POWER9.
    if (effectiveSite() == u"pleiadi"_s)
        return {};
    // The launcher asks for no GPU with an empty or "0" gres: the CPU image then.
    const QString g = gres.trimmed();
    const bool gpu = !g.isEmpty() && g != u"0"_s;
    return QString::fromLatin1(kBackendImage) + u':' + clientVersion + (gpu ? u"-egl"_s : QString());
}

QString ClusterProfile::launcherProblem(const QString &clientVersion) const
{
    if (effectiveSite() == u"leonardo"_s && account.trimmed().isEmpty()
        && !givesOption(extraArgs, u"--account"_s))
        return u"Leonardo needs your project account (the Account field)."_s;
    if (usesShippedLauncher() && sif.trimmed().isEmpty() && image.trimmed().isEmpty()
        && !givesOption(extraArgs, u"--image"_s) && !givesOption(extraArgs, u"--sif"_s)
        && defaultImage(clientVersion).isEmpty()) {
        static const QRegularExpression release(u"^[0-9]+\\.[0-9]+\\.[0-9]+$"_s);
        if (!release.match(clientVersion).hasMatch())
            return u"This build of VisIVO is not a release, so it does not know which published "
                   u"backend image to start. Fill in \"Image to pull\" (the docker://... "
                   u"reference of the version to run), or a Backend image file (.sif) on the "
                   u"cluster, or name a VisIVO clone that has one."_s;
        return u"No backend image to start. Fill in the Backend image (docker://... to pull, or "
               u"a .sif file on the cluster), or name a VisIVO clone that has one. The published "
               u"images are for x86 and ARM nodes, not for this cluster."_s;
    }
    return {};
}

QString ClusterProfile::imageVersion(const QString &appVersion, bool releaseBuild)
{
    return releaseBuild ? appVersion : QString();
}

QString ClusterProfile::clientImageVersion()
{
#ifdef VISIVO_RELEASE_BUILD
    return imageVersion(QCoreApplication::applicationVersion(), true);
#else
    return imageVersion(QCoreApplication::applicationVersion(), false);
#endif
}

QStringList ClusterProfile::launcherArgs(const QString &clientVersion) const
{
    QStringList args;
    const auto add = [&args](const QString &option, const QString &value) {
        if (!value.trimmed().isEmpty())
            args << option << value.trimmed();
    };
    if (!site.trimmed().isEmpty() && !givesOption(extraArgs, u"--site"_s))
        add(u"--site"_s, site);
    add(u"--partition"_s, partition);
    add(u"--account"_s, account);
    add(u"--qos"_s, qos);
    add(u"--gres"_s, gres);
    if (cpus > 0)
        args << u"--cpus"_s << QString::number(cpus);
    add(u"--mem"_s, mem);
    add(u"--time"_s, time);
    add(u"--sif"_s, sif);
    add(u"--image"_s, image.trimmed().isEmpty() ? defaultImage(clientVersion) : image);
    if (queueWaitMin > 0 && !givesOption(extraArgs, u"--assign-timeout"_s))
        args << u"--assign-timeout"_s << QString::number(queueWaitMin * 60);
    add(u"--data-root"_s, dataRoot);
    add(u"--workspace"_s, workspace);
    add(u"--node-code"_s, nodeCode);
    for (const QString &a : extraArgs)
        if (!a.isEmpty())
            args << a;
    return args;
}

QStringList ClusterProfile::presetSites()
{
    return { u"leonardo"_s, u"pleiadi"_s };
}

QString ClusterProfile::presetLabel(const QString &site)
{
    if (site == u"leonardo"_s)
        return u"Leonardo (CINECA)"_s;
    if (site == u"pleiadi"_s)
        return u"PLEIADI (INAF)"_s;
    return u"Other"_s;
}

void ClusterProfile::applyPreset(const QString &siteId)
{
    site = presetSites().contains(siteId) ? siteId : QString();
    if (siteId == u"leonardo"_s) {
        // Production: one A100 of a boost node, an interactive session of some
        // hours. The default QOS (normal) takes a one-node job; boost_qos_dbg
        // is for tests and the big-production QOS needs many nodes.
        if (name.trimmed().isEmpty())
            name = u"Leonardo"_s;
        frontend = u"login.leonardo.cineca.it"_s;
        partition = u"boost_usr_prod"_s;
        qos.clear();
        gres = u"gpu:1"_s;
        cpus = 8;
        mem = u"120G"_s;
        time = u"04:00:00"_s;
        queueWaitMin = 30;
    } else if (siteId == u"pleiadi"_s) {
        // The values of the PLEIADI-GPU documentation. Its POWER9 nodes run
        // an image built there: the profile names it (Backend image) or a clone.
        if (name.trimmed().isEmpty())
            name = u"PLEIADI"_s;
        frontend = u"pleiadi-gpu.oact.inaf.it"_s;
        partition = u"gpu"_s;
        qos.clear();
        gres = u"gpu:1"_s;
        cpus = 16;
        mem = u"96G"_s;
        time.clear();
        queueWaitMin = 0;
    }
}

QStringList ClusterProfile::changeSite(const QString &siteId)
{
    const QString target = presetSites().contains(siteId) ? siteId : QString();
    const QString from = effectiveSite();
    if (target == from)
        return {};
    QStringList reset;
    if (!target.isEmpty() && !from.isEmpty()) {
        const auto clear = [&reset](QString &value, const QString &label) {
            if (!value.trimmed().isEmpty())
                reset << label;
            value.clear();
        };
        clear(repoDir, u"VisIVO clone"_s);
        clear(sif, u"Backend image file"_s);
        if (!image.trimmed().startsWith(u"docker://"_s))
            clear(image, u"Image to pull"_s);
        clear(dataRoot, u"Data root"_s);
        clear(workspace, u"Workspace"_s);
        clear(nodeCode, u"SRC node code"_s);
        if (std::any_of(extraArgs.begin(), extraArgs.end(), [](const QString &a) { return !a.isEmpty(); }))
            reset << u"More options"_s;
        extraArgs.clear();
        if (!shipLauncher)
            reset << u"Send the launcher from this client"_s;
        shipLauncher = true;
    }
    applyPreset(siteId);
    return reset;
}

QString ClusterProfile::joinCommand(const QStringList &args)
{
    QStringList out;
    for (const QString &a : args) {
        const bool plain = !a.isEmpty()
                && std::none_of(a.begin(), a.end(),
                                [](QChar c) { return c.isSpace() || c == u'"'; });
        // splitCommand reads three quotes inside a quoted part as one.
        out << (plain ? a : u"\""_s + QString(a).replace(u"\""_s, u"\"\"\""_s) + u"\""_s);
    }
    return out.join(u' ');
}

QList<ClusterProfile> ClusterProfiles::load(QSettings &settings)
{
    QList<ClusterProfile> out;
    const QJsonArray arr = QJsonDocument::fromJson(settings.value(kKey).toString().toUtf8()).array();
    for (const auto &v : arr) {
        const ClusterProfile p = fromJson(v.toObject());
        if (p.isValid())
            out << p;
    }
    return out;
}

void ClusterProfiles::save(QSettings &settings, const QList<ClusterProfile> &profiles)
{
    QJsonArray arr;
    for (const auto &p : profiles)
        arr.append(toJson(p));
    settings.setValue(kKey, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

ClusterProfile ClusterProfiles::find(QSettings &settings, const QString &name)
{
    for (const auto &p : load(settings))
        if (p.name == name)
            return p;
    return {};
}

void ClusterProfiles::upsert(QSettings &settings, const ClusterProfile &profile)
{
    QList<ClusterProfile> all = load(settings);
    bool replaced = false;
    for (auto &p : all) {
        if (p.name == profile.name) {
            p = profile;
            replaced = true;
        }
    }
    if (!replaced)
        all << profile;
    save(settings, all);
}
