#include "AnalysisParamStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QSettings>

#include <utility>

using namespace Qt::StringLiterals;

AnalysisParamStore::AnalysisParamStore(QString iniPath) : m_iniPath(std::move(iniPath)) {}

QString AnalysisParamStore::defaultIniPath()
{
    // Mirrors main.cpp / Settings: ~/<applicationName>/Settings.ini.
    const QString workDir = QDir::home().absoluteFilePath(QCoreApplication::applicationName());
    return QDir(workDir).absoluteFilePath(u"Settings.ini"_s);
}

QString AnalysisParamStore::datasetToken(const QString &datasetKey)
{
    // SHA-1 hex: fixed length, no '/' or other QSettings-special characters, and
    // stable across sessions for the same key. The user never sees it.
    return QString::fromLatin1(
            QCryptographicHash::hash(datasetKey.toUtf8(), QCryptographicHash::Sha1).toHex());
}

void AnalysisParamStore::save(const QString &datasetKey, const QString &group,
                              const QVariantMap &params) const
{
    if (datasetKey.isEmpty() || group.isEmpty()) {
        return;
    }
    QSettings settings(m_iniPath, QSettings::IniFormat);
    settings.beginGroup(u"AnalysisParams"_s);
    settings.beginGroup(datasetToken(datasetKey));
    // Replace the group wholesale so a changed param set doesn't leave stale keys.
    settings.remove(group);
    settings.beginGroup(group);
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        settings.setValue(it.key(), it.value());
    }
    settings.endGroup();
    settings.endGroup();
    settings.endGroup();
}

QVariantMap AnalysisParamStore::load(const QString &datasetKey, const QString &group) const
{
    QVariantMap out;
    if (datasetKey.isEmpty() || group.isEmpty()) {
        return out;
    }
    QSettings settings(m_iniPath, QSettings::IniFormat);
    settings.beginGroup(u"AnalysisParams"_s);
    settings.beginGroup(datasetToken(datasetKey));
    settings.beginGroup(group);
    const QStringList keys = settings.childKeys();
    for (const QString &key : keys) {
        out.insert(key, settings.value(key));
    }
    return out;
}
