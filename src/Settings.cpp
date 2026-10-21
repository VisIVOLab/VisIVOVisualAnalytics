#include "Settings.h"

#include <algorithm>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>

#include <limits>

using namespace Qt::StringLiterals;

Settings::Settings(const QString &workDir, QObject *parent) : QObject(parent), workDir(workDir)
{
    const QString settingsPath = QDir(workDir).absoluteFilePath(u"Settings.ini"_s);
    this->settings = new QSettings(settingsPath, QSettings::IniFormat, this);

    this->setPythonLocation();

    if (!QFileInfo::exists(settingsPath)) {
        qWarning() << settingsPath << "does not exist. Creating one with default settings...";
        this->resetDefaults();
    }
}

Settings::~Settings() = default;

QString Settings::getWorkingPath() const
{
    return this->workDir;
}

void Settings::reload()
{
    this->settings->sync();
}

void Settings::resetDefaults()
{
    this->settings->setValue(Settings::ColorScheme.first, Settings::ColorScheme.second);
    this->settings->setValue(this->PythonLocation.first, this->PythonLocation.second);
    this->settings->setValue(Settings::PanoramicView.first, Settings::PanoramicView.second);
    this->settings->setValue(Settings::PanoramicZoom.first, Settings::PanoramicZoom.second);
    this->settings->setValue(Settings::MaxGlyphs.first, Settings::MaxGlyphs.second);
    this->settings->setValue(Settings::VLKBUrl.first, Settings::VLKBUrl.second);
    this->settings->setValue(Settings::VLKBSearch.first, Settings::VLKBSearch.second);
    this->settings->setValue(Settings::BackendUrl.first, Settings::BackendUrl.second);
    this->settings->setValue(Settings::BackendToken.first, Settings::BackendToken.second);
    this->settings->setValue(Settings::PlotTheme.first, Settings::PlotTheme.second);
    this->settings->setValue(Settings::CopilotProvider.first, Settings::CopilotProvider.second);
    this->settings->setValue(Settings::CopilotKey.first, Settings::CopilotKey.second);
    this->settings->setValue(Settings::CopilotUrl.first, Settings::CopilotUrl.second);
    this->settings->setValue(Settings::CopilotModel.first, Settings::CopilotModel.second);

    emit this->updated();
}

//----------------------------------------------------------------------------
// UI
//----------------------------------------------------------------------------
const std::pair<QString, QVariant> Settings::ColorScheme = {
    u"UI/theme"_s, static_cast<int>(Qt::ColorScheme::Unknown)
};

Qt::ColorScheme Settings::getColorScheme() const
{
    return static_cast<Qt::ColorScheme>(
            this->settings->value(Settings::ColorScheme.first, Settings::ColorScheme.second)
                    .toInt());
}

void Settings::setColorScheme(Qt::ColorScheme scheme)
{
    this->settings->setValue(Settings::ColorScheme.first, static_cast<int>(scheme));
}

//----------------------------------------------------------------------------
// Python
//----------------------------------------------------------------------------
void Settings::setPythonLocation()
{
    QProcess p;
    const QStringList args = { u"-c"_s, u"import sys; print(sys.executable, end='')"_s };
    p.start(u"python3"_s, args);
    p.waitForFinished();
    QString path = p.readAllStandardOutput();
    if (path.isEmpty()) {
        // Try again with python
        p.start(u"python"_s, args);
        p.waitForFinished();
        path = p.readAllStandardOutput();
    }

    this->PythonLocation = { u"Python/location"_s, path };
}

QString Settings::getPythonLocation() const
{
    const QString path = this->settings->value(this->PythonLocation.first).toString();
    return path.isEmpty() ? this->PythonLocation.second.toString() : path;
}

void Settings::setPythonLocation(const QString &location)
{
    this->settings->setValue(this->PythonLocation.first, location);
}

//----------------------------------------------------------------------------
// PanoramicView
//----------------------------------------------------------------------------
const std::pair<QString, QVariant> Settings::PanoramicView = {
    u"PanoramicView/url"_s, u"http://vlkb.ia2.inaf.it/panoramicview/openlayers.html"_s
};

QString Settings::getPanoramicView() const
{
    const QString url = this->settings->value(Settings::PanoramicView.first).toString();
    return url.isEmpty() ? Settings::PanoramicView.second.toString() : url;
}

void Settings::setPanoramicView(const QString &url)
{
    this->settings->setValue(Settings::PanoramicView.first, url);
}

// The page's own starting view is unusable: it calls zoomToExtent on a rectangle
// OUTSIDE the map (y 16000..20000 where the map spans -4398..0), so it opens on
// empty space and the user has to hunt for the Galactic plane. The client frames
// the plane itself once the page has loaded.
//
// 0 means "fit the survey's height to the pane", which is the right answer for
// any window size; a level from 1 to 10 pins it instead, for opening somewhere
// specific.
const std::pair<QString, QVariant> Settings::PanoramicZoom = { u"PanoramicView/zoom"_s, 0 };

int Settings::getPanoramicZoom() const
{
    bool ok = false;
    const int z = this->settings->value(Settings::PanoramicZoom.first).toInt(&ok);
    if (!ok)
        return Settings::PanoramicZoom.second.toInt();
    return std::clamp(z, 0, 10);   // the survey pyramid has 11 levels
}

void Settings::setPanoramicZoom(int zoom)
{
    this->settings->setValue(Settings::PanoramicZoom.first, std::clamp(zoom, 0, 10));
}

//----------------------------------------------------------------------------
// Visualization
//----------------------------------------------------------------------------
// Two million points is already more than a viewer draws comfortably, and the
// service refuses a larger point subset outright. The old default was INT_MAX,
// which is not a limit but the absence of one: opening a 21-million-row table
// asked the service for every row of every field — 1.8 GB of arrays and 2.5 GB
// of base64 — and killed it, which reached the user as "connection refused".
const std::pair<QString, QVariant> Settings::MaxGlyphs = { u"Visualization/max_glyphs"_s,
                                                           2'000'000 };

int Settings::getMaxGlyphs() const
{
    const int stored =
            this->settings->value(Settings::MaxGlyphs.first, Settings::MaxGlyphs.second).toInt();
    // A stored INT_MAX is the OLD default, and it does not mean "two billion
    // glyphs" — it means "no limit", which is what let a 21-million-row table
    // ask the service for every row of every field and kill it. The value is
    // written into the ini on first run, so everyone who has already used the
    // app carries it: changing the compiled default alone would have fixed
    // nothing for them. A non-positive value is equally meaningless.
    if (stored <= 0 || stored >= std::numeric_limits<int>::max())
        return Settings::MaxGlyphs.second.toInt();
    return stored;
}

void Settings::setMaxGlyphs(int n)
{
    this->settings->setValue(Settings::MaxGlyphs.first, n);
}

//----------------------------------------------------------------------------
// VLKB
//----------------------------------------------------------------------------
const std::pair<QString, QVariant> Settings::VLKBUrl = { u"VLKB/url"_s,
                                                         u"https://vlkb.ia2.inaf.it"_s };

QString Settings::getVLKBUrl() const
{
    const QString url = this->settings->value(Settings::VLKBUrl.first).toString();
    return url.isEmpty() ? Settings::VLKBUrl.second.toString() : url;
}

void Settings::setVLKBUrl(const QString &url)
{
    this->settings->setValue(Settings::VLKBUrl.first, url);
}

const std::pair<QString, QVariant> Settings::VLKBSearch = { u"VLKB/search_on_import"_s, false };

bool Settings::getSearchOnImportFlag() const
{
    return this->settings->value(Settings::VLKBSearch.first, Settings::VLKBSearch.second).toBool();
}

void Settings::setSearchOnImportFlag(bool flag)
{
    this->settings->setValue(Settings::VLKBSearch.first, flag);
}

//----------------------------------------------------------------------------
// Backend (R1, R8)
//----------------------------------------------------------------------------
const std::pair<QString, QVariant> Settings::BackendUrl = {
    u"Backend/url"_s, u"http://127.0.0.1:8000"_s
};

QString Settings::getBackendUrl() const
{
    // Trimmed here rather than at each reader. A value stored with a trailing
    // space parses as the right host and the DEFAULT port, and BackendClient
    // concatenates the string straight into a request URL — so one caller binds
    // a port while another queries an invalid address, and the app is quietly
    // offline. Roughly thirty call sites read this; one of them remembering to
    // trim is not a fix.
    const QString url =
            this->settings->value(Settings::BackendUrl.first).toString().trimmed();
    return url.isEmpty() ? Settings::BackendUrl.second.toString() : url;
}

void Settings::setBackendUrl(const QString &url)
{
    this->settings->setValue(Settings::BackendUrl.first, url);
}

// Token default is empty: BackendClient will auto-resolve from env / ~/.visivo_token.
const std::pair<QString, QVariant> Settings::BackendToken = { u"Backend/token"_s, u""_s };

//----------------------------------------------------------------------------
// Plot theme
//----------------------------------------------------------------------------
const std::pair<QString, QVariant> Settings::PlotTheme = { u"Plots/theme"_s, 0 };

int Settings::getPlotTheme() const
{
    return this->settings->value(Settings::PlotTheme.first, Settings::PlotTheme.second).toInt();
}

void Settings::setPlotTheme(int theme)
{
    this->settings->setValue(Settings::PlotTheme.first, theme);
}

//----------------------------------------------------------------------------
// Cube pipeline tuning (Phase 3.6)
//----------------------------------------------------------------------------
const std::pair<QString, QVariant> Settings::PreviewTargetMb       = { u"Cube/preview_target_mb"_s,       100 };
const std::pair<QString, QVariant> Settings::LocalFullThresholdMb  = { u"Cube/local_full_threshold_mb"_s, 256 };

int Settings::getPreviewTargetMb() const
{
    // Clamp to a sane range — values <5 MB would render an unusable
    // preview, >1024 would defeat the purpose. The QSettings store
    // can hold anything; we just refuse silly values at read time.
    const int v = this->settings->value(Settings::PreviewTargetMb.first,
                                         Settings::PreviewTargetMb.second).toInt();
    return std::clamp(v, 5, 1024);
}

void Settings::setPreviewTargetMb(int mb)
{
    this->settings->setValue(Settings::PreviewTargetMb.first, std::clamp(mb, 5, 1024));
}

int Settings::getLocalFullThresholdMb() const
{
    const int v = this->settings->value(Settings::LocalFullThresholdMb.first,
                                         Settings::LocalFullThresholdMb.second).toInt();
    return std::clamp(v, 16, 16384);
}

void Settings::setLocalFullThresholdMb(int mb)
{
    this->settings->setValue(Settings::LocalFullThresholdMb.first, std::clamp(mb, 16, 16384));
}

QString Settings::getBackendToken() const
{
    // Trimmed for the same reason, and one of its own: a token copied out of a
    // terminal commonly brings a newline with it, and the server compares the
    // bytes exactly.
    return this->settings->value(Settings::BackendToken.first,
                                  Settings::BackendToken.second).toString().trimmed();
}

void Settings::setBackendToken(const QString &token)
{
    this->settings->setValue(Settings::BackendToken.first, token);
}

//----------------------------------------------------------------------------
// SKAVA discovery service
//----------------------------------------------------------------------------
const std::pair<QString, QVariant> Settings::SkavaUrl = {
    u"Skava/url"_s, u"http://localhost:8000"_s
};
const std::pair<QString, QVariant> Settings::SkavaToken = { u"Skava/token"_s, u""_s };
const std::pair<QString, QVariant> Settings::SkavaProtocol = { u"Skava/protocol"_s, u"skava"_s };
const std::pair<QString, QVariant> Settings::SkavaTapUrl = { u"Skava/tapUrl"_s, u""_s };
const std::pair<QString, QVariant> Settings::SkavaDatalinkUrl = { u"Skava/datalinkUrl"_s, u""_s };
const std::pair<QString, QVariant> Settings::CopilotProvider = { u"Copilot/provider"_s, 3 };
const std::pair<QString, QVariant> Settings::CopilotKey = { u"Copilot/key"_s, u""_s };
const std::pair<QString, QVariant> Settings::CopilotUrl = { u"Copilot/url"_s, u""_s };
const std::pair<QString, QVariant> Settings::CopilotModel = { u"Copilot/model"_s, u""_s };

int Settings::getCopilotProvider() const
{
    return this->settings->value(Settings::CopilotProvider.first,
                                 Settings::CopilotProvider.second).toInt();
}
void Settings::setCopilotProvider(int choice)
{
    this->settings->setValue(Settings::CopilotProvider.first, choice);
}
QString Settings::getCopilotKey() const
{
    return this->settings->value(Settings::CopilotKey.first, Settings::CopilotKey.second).toString();
}
void Settings::setCopilotKey(const QString &key)
{
    this->settings->setValue(Settings::CopilotKey.first, key);
}
QString Settings::getCopilotUrl() const
{
    return this->settings->value(Settings::CopilotUrl.first, Settings::CopilotUrl.second).toString();
}
void Settings::setCopilotUrl(const QString &url)
{
    this->settings->setValue(Settings::CopilotUrl.first, url);
}
QString Settings::getCopilotModel() const
{
    return this->settings->value(Settings::CopilotModel.first, Settings::CopilotModel.second).toString();
}
void Settings::setCopilotModel(const QString &model)
{
    this->settings->setValue(Settings::CopilotModel.first, model);
}

QString Settings::getSkavaUrl() const
{
    const QString url = this->settings->value(Settings::SkavaUrl.first).toString();
    return url.isEmpty() ? Settings::SkavaUrl.second.toString() : url;
}

void Settings::setSkavaUrl(const QString &url)
{
    this->settings->setValue(Settings::SkavaUrl.first, url);
}

QString Settings::getSkavaToken() const
{
    return this->settings->value(Settings::SkavaToken.first,
                                  Settings::SkavaToken.second).toString();
}

void Settings::setSkavaToken(const QString &token)
{
    this->settings->setValue(Settings::SkavaToken.first, token);
}

QString Settings::getSkavaProtocol() const
{
    const QString p = this->settings->value(Settings::SkavaProtocol.first,
                                            Settings::SkavaProtocol.second).toString();
    return p == "vo"_L1 ? p : u"skava"_s;
}

void Settings::setSkavaProtocol(const QString &protocol)
{
    this->settings->setValue(Settings::SkavaProtocol.first, protocol == "vo"_L1 ? u"vo"_s : u"skava"_s);
}

QString Settings::getSkavaTapUrl() const
{
    return this->settings->value(Settings::SkavaTapUrl.first, Settings::SkavaTapUrl.second).toString().trimmed();
}

void Settings::setSkavaTapUrl(const QString &url)
{
    this->settings->setValue(Settings::SkavaTapUrl.first, url.trimmed());
}

QString Settings::getSkavaDatalinkUrl() const
{
    return this->settings->value(Settings::SkavaDatalinkUrl.first, Settings::SkavaDatalinkUrl.second)
            .toString().trimmed();
}

void Settings::setSkavaDatalinkUrl(const QString &url)
{
    this->settings->setValue(Settings::SkavaDatalinkUrl.first, url.trimmed());
}

//----------------------------------------------------------------------------
// Multi-backend registry (distributed VisIVO backend)
//----------------------------------------------------------------------------
const std::pair<QString, QVariant> Settings::BackendNodes =
        { u"Backends/nodes"_s, u"[]"_s };
const std::pair<QString, QVariant> Settings::DefaultBackendId =
        { u"Backends/default"_s, u"local"_s };

namespace {

QJsonObject backendNodeToJson(const BackendNode &n)
{
    return QJsonObject{
        { u"id"_s,         n.id },
        { u"name"_s,       n.name },
        { u"url"_s,        n.url },
        { u"token"_s,      n.token },
        { u"src_code"_s,   n.srcCode },
        { u"is_local"_s,   n.isLocal },
        { u"is_builtin"_s, n.isBuiltin },
    };
}

BackendNode backendNodeFromJson(const QJsonObject &obj)
{
    BackendNode n;
    n.id        = obj.value(u"id"_s).toString();
    n.name      = obj.value(u"name"_s).toString();
    n.url       = obj.value(u"url"_s).toString();
    n.token     = obj.value(u"token"_s).toString();
    n.srcCode   = obj.value(u"src_code"_s).toString();
    n.isLocal   = obj.value(u"is_local"_s).toBool(false);
    n.isBuiltin = obj.value(u"is_builtin"_s).toBool(false);
    return n;
}

}  // namespace

BackendNode Settings::buildLocalBackend() const
{
    BackendNode n;
    n.id = u"local"_s;
    n.name = u"Local"_s;
    n.url = this->getBackendUrl();
    n.token = this->getBackendToken();
    n.srcCode = QString();
    n.isLocal = true;
    n.isBuiltin = true;
    return n;
}

QList<BackendNode> Settings::getBackendNodes() const
{
    const QString raw = this->settings->value(Settings::BackendNodes.first,
                                                Settings::BackendNodes.second).toString();
    const QJsonArray arr = QJsonDocument::fromJson(raw.toUtf8()).array();
    QList<BackendNode> out;
    out.reserve(arr.size());
    bool hasLocal = false;
    for (const auto &val : arr) {
        BackendNode n = backendNodeFromJson(val.toObject());
        if (n.id == "local"_L1) {
            hasLocal = true;
            // Override with current local backend values so the
            // user-editable Backend/url+token always wins.
            n = buildLocalBackend();
        }
        if (!n.id.isEmpty()) {
            out.append(n);
        }
    }
    // Always present the local backend, even on a fresh install.
    if (!hasLocal) {
        out.prepend(buildLocalBackend());
    }
    return out;
}

void Settings::setBackendNodes(const QList<BackendNode> &nodes)
{
    QJsonArray arr;
    bool sawLocal = false;
    for (const auto &n : nodes) {
        if (n.id == "local"_L1) sawLocal = true;
        arr.append(backendNodeToJson(n));
    }
    if (!sawLocal) {
        arr.prepend(backendNodeToJson(buildLocalBackend()));
    }
    this->settings->setValue(
            Settings::BackendNodes.first,
            QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

BackendNode Settings::getBackendNode(const QString &id) const
{
    for (const auto &n : getBackendNodes()) {
        if (n.id == id) return n;
    }
    return {};
}

BackendNode Settings::getLocalBackend() const
{
    return buildLocalBackend();
}

QString Settings::getDefaultBackendId() const
{
    return this->settings->value(Settings::DefaultBackendId.first,
                                  Settings::DefaultBackendId.second).toString();
}

void Settings::setDefaultBackendId(const QString &id)
{
    this->settings->setValue(Settings::DefaultBackendId.first,
                              id.isEmpty() ? u"local"_s : id);
}

void Settings::upsertBackendNode(const BackendNode &node)
{
    if (node.id.isEmpty()) {
        qWarning() << "Settings::upsertBackendNode refused: id is empty";
        return;
    }
    auto all = getBackendNodes();
    bool replaced = false;
    for (auto &existing : all) {
        if (existing.id == node.id) {
            // Preserve isLocal/isBuiltin protections on the local entry —
            // user can edit URL / token / name but not the flags.
            if (existing.id == "local"_L1) {
                BackendNode updated = node;
                updated.isLocal = true;
                updated.isBuiltin = true;
                this->setBackendUrl(updated.url);
                this->setBackendToken(updated.token);
                existing = updated;
            } else {
                existing = node;
            }
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        all.append(node);
    }
    setBackendNodes(all);
}

void Settings::removeBackendNode(const QString &id)
{
    if (id == "local"_L1) {
        qWarning() << "Settings::removeBackendNode refused: 'local' is protected";
        return;
    }
    auto all = getBackendNodes();
    all.removeIf([&id](const BackendNode &n) { return n.id == id; });
    setBackendNodes(all);
    if (getDefaultBackendId() == id) {
        setDefaultBackendId(u"local"_s);
    }
}
