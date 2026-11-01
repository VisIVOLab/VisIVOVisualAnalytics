#include "ClusterProfile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::StringLiterals;

namespace {

const QString kKey = u"Clusters/profiles"_s;

QJsonObject toJson(const ClusterProfile &p)
{
    return { { u"name"_s, p.name },           { u"frontend"_s, p.frontend },
             { u"user"_s, p.user },           { u"repo_dir"_s, p.repoDir },
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

QStringList ClusterProfile::launcherArgs() const
{
    QStringList args;
    const auto add = [&args](const QString &option, const QString &value) {
        if (!value.trimmed().isEmpty())
            args << option << value.trimmed();
    };
    add(u"--partition"_s, partition);
    add(u"--account"_s, account);
    add(u"--qos"_s, qos);
    add(u"--gres"_s, gres);
    if (cpus > 0)
        args << u"--cpus"_s << QString::number(cpus);
    add(u"--mem"_s, mem);
    add(u"--time"_s, time);
    add(u"--sif"_s, sif);
    add(u"--data-root"_s, dataRoot);
    add(u"--workspace"_s, workspace);
    add(u"--node-code"_s, nodeCode);
    for (const QString &a : extraArgs)
        if (!a.isEmpty())
            args << a;
    return args;
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
