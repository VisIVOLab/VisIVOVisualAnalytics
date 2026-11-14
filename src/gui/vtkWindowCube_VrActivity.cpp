// vtkWindowCube_VrActivity.cpp
// The headset of this window's backend session (issue #238): Show on Headset
// sends this cube to the headset page already open (no code, no adb, no new
// browser window), Headset Activity lists what was done in the headset and
// opens its products in the desktop's own viewers, and the status bar says
// whether a headset is connected, in VR, and what it shows.
//
// What the headset does is the session's (HeadsetHubLink, one per session);
// a product opened here is recorded once in Provenance under an id made from
// the headset's own record, with the requests it was made by. Follow the
// Headset's Look (on by default, issue #268; the choice is kept in the app's
// settings) applies the headset's colour map, threshold and clip box to this
// window when they are about this cube and it is on show there, whether or
// not Headset Activity was ever opened (VrHeadsetFollow.h says which events).

#include "vtkWindowCube.h"

#include "ui_vtkWindowCube.h"
#include "ColorMaps.h"
#include "HeadsetActivityList.h"
#include "WorkspaceChrome.h"
#include "app/BackendClient.h"
#include "app/HeadsetHubLink.h"
#include "app/VrLink.h"
#include "PvController.h"
#include "RemoteLevels.h"

#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkTrivialProducer.h>

#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>

namespace {
// Points: about a third of a laptop-wide cube window.
constexpr int kHeadsetStatusMaxWidth = 320;
}

using namespace Qt::StringLiterals;

namespace {
const QString kHeadsetActivityTool = u"Headset Activity"_s;

bool isFitsPath(const QString &path)
{
    const QString p = path.toLower();
    return p.endsWith(u".fits"_s) || p.endsWith(u".fit"_s) || p.endsWith(u".fts"_s)
            || p.endsWith(u".fits.gz"_s) || p.endsWith(u".fits.fz"_s);
}
} // namespace

void vtkWindowCube::setupHeadsetHub()
{
    this->m_actShowCubeOnHeadset = new QAction(u"Show on Headset"_s, ui->menuTools);
    this->m_actShowCubeOnHeadset->setEnabled(false);
    this->m_actShowCubeOnHeadset->setToolTip(
            u"Show this cube on the VR headset connected to this session (open one first with "
            u"Open in VR Headset…)."_s);
    QObject::connect(this->m_actShowCubeOnHeadset, &QAction::triggered, this,
                     &vtkWindowCube::showCubeOnHeadset);
    ui->menuTools->addAction(this->m_actShowCubeOnHeadset);

    this->m_actHeadsetActivity = new QAction(u"Headset Activity"_s, ui->menuTools);
    this->m_actHeadsetActivity->setCheckable(true);
    this->m_actHeadsetActivity->setToolTip(
            u"What was done in the VR headset of this session, newest first: open its moment "
            u"maps, PV slices, spectra and cubes here. Settings in Inspector ▸ Parameters."_s);
    QObject::connect(this->m_actHeadsetActivity, &QAction::toggled, this, [this](bool on) {
        if (on) {
            this->ensureHeadsetActivityPanel();
            this->refreshHeadsetActivity();
            this->claimToolSettings(kHeadsetActivityTool, this->m_headsetActivitySettings);
        } else {
            this->releaseToolSettings(kHeadsetActivityTool);
            this->refreshToolSettingsPanel(/*reveal=*/false);
        }
    });
    ui->menuTools->addAction(this->m_actHeadsetActivity);

    this->m_headsetStatusLabel = new QLabel(this);
    this->m_headsetStatusLabel->setVisible(false);
    // The hard limit; the elided text keeps it readable (a font change
    // between refreshes then clips the end instead of moving the bar).
    this->m_headsetStatusLabel->setMaximumWidth(kHeadsetStatusMaxWidth);
    this->statusBar()->addPermanentWidget(this->m_headsetStatusLabel);

    {
        const QSettings settings;
        this->m_headsetFollow = VrHub::followLookEnabled(settings);
    }

    // After the constructor: the window's backend identity is settled then.
    QTimer::singleShot(0, this, [this]() {
        if (this->remoteBackendUrl.isEmpty() || this->remoteDatasetId.isEmpty())
            return;
        const VrLink::Inputs in = this->vrHeadsetInputs();
        this->m_headsetHub = HeadsetHubLink::forSession(in.baseUrl, in.token, in.sessionId);
        if (!this->m_headsetHub)
            return;
        QObject::connect(this->m_headsetHub, &HeadsetHubLink::headsetsChanged, this,
                         &vtkWindowCube::refreshHeadsetPresence);
        QObject::connect(this->m_headsetHub, &HeadsetHubLink::activityChanged, this,
                         &vtkWindowCube::refreshHeadsetActivity);
        QObject::connect(this->m_headsetHub, &HeadsetHubLink::stopped, this,
                         [this]() { this->refreshHeadsetPresence(); this->refreshHeadsetActivity(); });
        this->m_headsetHub->addUser(this);
        this->refreshHeadsetPresence();
        this->refreshHeadsetActivity();
    });
}

void vtkWindowCube::refreshHeadsetPresence()
{
    const QList<VrHub::Headset> none;
    const QList<VrHub::Headset> &headsets = this->m_headsetHub ? this->m_headsetHub->headsets() : none;
    const QString text = VrHub::statusText(headsets);
    if (this->m_headsetStatusLabel) {
        // A long cube name must not push the status bar's other labels away:
        // the middle goes, the whole line stays in the tooltip.
        const QFontMetrics fm(this->m_headsetStatusLabel->font());
        this->m_headsetStatusLabel->setText(fm.elidedText(text, Qt::ElideMiddle, kHeadsetStatusMaxWidth));
        this->m_headsetStatusLabel->setToolTip(
                headsets.isEmpty() ? QString() : VrHub::headsetLine(headsets.first()));
        this->m_headsetStatusLabel->setVisible(!text.isEmpty());
    }
    if (this->m_actShowCubeOnHeadset) {
        const bool connected = !headsets.isEmpty();
        this->m_actShowCubeOnHeadset->setEnabled(connected && !this->m_headsetShowing);
        this->m_actShowCubeOnHeadset->setToolTip(
                connected ? u"Show this cube on the headset: %1."_s.arg(VrHub::headsetLine(headsets.first()))
                          : u"No VR headset is connected to this session: open one with Open in "
                            u"VR Headset…"_s);
    }
    if (this->m_headsetActivityState) {
        QString state = headsets.isEmpty() ? u"No headset is connected to this session."_s
                                           : VrHub::headsetLine(headsets.first()) + u'.';
        if (this->m_headsetHub && !this->m_headsetHub->supported())
            state = u"This backend has no headset hub (it is older than this client)."_s;
        // A cube name has no spaces: without break points its minimum width
        // would widen the whole Parameters page past the Inspector.
        this->m_headsetActivityState->setText(HeadsetActivityList::wrappable(state));
    }
    // A look that came before the headset was listed as showing this cube
    // (presence is polled every few seconds) is applied now.
    this->followHeadsetLook();
}

VrLink::Inputs vtkWindowCube::vrHeadsetInputs() const
{
    // The identity this window's requests actually carry: the token resolved
    // the way BackendClient resolves it, and the window's session or else the
    // one minted for this backend (as the headset page is given them).
    const QString url = this->remoteBackendUrl;
    const QString token = BackendClient::effectiveToken(url, BackendClient(url, this->remoteBackendToken).token());
    VrLink::Inputs inputs;
    inputs.baseUrl = url;
    inputs.token = token;
    inputs.sessionId = !this->remoteSessionId.isEmpty() ? this->remoteSessionId
                                                        : BackendClient::sessionIdForUrl(url, token);
    inputs.datasetId = this->remoteDatasetId;
    inputs.path = this->filepath;
    inputs.remote = !BackendClient::isLocalBackendUrl(url);
    return inputs;
}

void vtkWindowCube::showCubeOnHeadset()
{
    if (!this->m_headsetHub || this->m_headsetHub->headsets().isEmpty() || this->m_headsetShowing)
        return;
    const VrLink::Inputs in = this->vrHeadsetInputs();
    const QJsonObject body = VrHub::showCommandBody(in, this->m_headsetHub->headsets().first().client);
    this->m_headsetShowing = true;
    this->refreshHeadsetPresence();
    this->statusBar()->showMessage(u"Sending this cube to the headset…"_s, 4000);
    auto *watcher = new QFutureWatcher<BackendVrHubReply>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendVrHubReply>::finished, this, [this, watcher]() {
        const BackendVrHubReply reply = watcher->result();
        watcher->deleteLater();
        this->m_headsetShowing = false;
        const VrHub::CommandResult r = VrHub::interpretCommand(reply.httpStatus, reply.body, reply.error);
        if (r.outcome == VrHub::CommandResult::Outcome::Sent) {
            this->statusBar()->showMessage(u"Shown on the headset: it loads this cube now."_s, 6000);
            // Selections made there follow this cube from now on (issue #188).
            this->startSelectionShare();
        } else {
            this->statusBar()->showMessage(u"Not shown on the headset: %1"_s.arg(r.message), 8000);
        }
        if (this->m_headsetHub)
            this->m_headsetHub->refreshPresence();
        this->refreshHeadsetPresence();
    });
    watcher->setFuture(QtConcurrent::run([in, body]() {
        BackendClient client(in.baseUrl, in.token);
        client.setSessionId(in.sessionId);
        return client.sendVrHeadsetCommand(body);
    }));
}

void vtkWindowCube::ensureHeadsetActivityPanel()
{
    if (this->m_headsetActivitySettings)
        return;
    auto *panel = new QWidget(this);
    panel->hide();
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    this->m_headsetActivityState = new QLabel(panel);
    this->m_headsetActivityState->setWordWrap(true);
    this->m_headsetActivityState->setTextFormat(Qt::PlainText);
    layout->addWidget(this->m_headsetActivityState);
    this->m_headsetFollowLook = new QCheckBox(u"Follow the headset's look"_s, panel);
    this->m_headsetFollowLook->setToolTip(
            u"Apply the colour map, threshold and clip box set in the headset to this cube's "
            u"3-D view, as they change, while the headset shows this cube. Off: this window "
            u"keeps its own. The choice is remembered."_s);
    this->m_headsetFollowLook->setChecked(this->m_headsetFollow);
    QObject::connect(this->m_headsetFollowLook, &QCheckBox::toggled, this, [this](bool on) {
        this->m_headsetFollow = on;
        {
            QSettings settings;
            VrHub::setFollowLookEnabled(settings, on);
        }
        if (on) {
            // What the headset shows now, even if it was seen before.
            this->m_headsetFollowMarks = {};
            this->followHeadsetLook();
        } else {
            // The clip box is the headset's: the whole cube comes back. The
            // colours and threshold stay as they are, now this window's own.
            this->m_headsetClip = {};
            this->applyHeadsetClip();
        }
    });
    layout->addWidget(this->m_headsetFollowLook);
    this->m_headsetActivityList = new HeadsetActivityList(panel);
    this->m_headsetActivityList->setMinimumHeight(220);
    QObject::connect(this->m_headsetActivityList, &HeadsetActivityList::openRequested, this,
                     &vtkWindowCube::openHeadsetProduct);
    layout->addWidget(this->m_headsetActivityList, 1);
    auto *hint = new QLabel(u"Newest first. Open shows a product in the desktop's own viewer and "
                            u"records it once in Provenance."_s,
                            panel);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    this->m_headsetActivitySettings = panel;
    this->refreshHeadsetPresence();
}

void vtkWindowCube::refreshHeadsetActivity()
{
    this->followHeadsetLook();
    if (!this->m_headsetActivityList || !this->m_actHeadsetActivity
        || !this->m_actHeadsetActivity->isChecked())
        return;   // rebuilt when the tool is opened
    const QList<VrHub::Event> none;
    this->m_headsetActivityList->setEvents(this->m_headsetHub ? this->m_headsetHub->activity().events()
                                                              : none);
}

void vtkWindowCube::recordHeadsetProduct(const VrHub::Event &event, const VrHub::Product &product,
                                         const QString &renderer)
{
    // Recorded as a product of the cube it was made from, so only by that
    // cube's window; opened from another cube's window it is shown, and
    // recorded when it is opened from its own. Recorded already (by a window
    // of the same cube): that row, and its owner, stay.
    if (!VrHub::aboutCube(event, this->filepath, this->remoteDatasetId))
        return;
    if (!ProductRegistry::instance().product(VrHub::productId(event, product)).productId.isEmpty())
        return;
    const QVariantMap params = VrHub::provenanceParams(product);
    const QString kind = product.recipeKind.isEmpty() ? product.kind : product.recipeKind;
    // The same id however often it is opened: one row in Provenance.
    this->registerAnalysisProduct(kind, product.label.isEmpty() ? kind : product.label, params,
                                  /*window=*/nullptr, VrHub::productId(event, product), renderer,
                                  /*revealProvenance=*/false, product.origin,
                                  /*selectInSession=*/false, product.replay, product.caveat);
}

void vtkWindowCube::adoptHeadsetPv(const std::vector<std::array<double, 2>> &vertices, int width,
                                   QVariantMap &params, QList<ReplayStep> &replay, QString &caveat)
{
    const auto sameCut = [&vertices, width](const std::optional<HeadsetPv> &pv) {
        return pv && pv->vertices == vertices && pv->width == width;
    };
    std::optional<HeadsetPv> carried;
    if (sameCut(this->m_headsetPvAsked))
        carried = std::move(this->m_headsetPvAsked);
    else if (sameCut(this->m_headsetPvShown))
        carried = this->m_headsetPvShown;   // the same cut, recomputed
    this->m_headsetPvAsked.reset();
    // The row moves on to another cut (a path drawn here, the headset's path
    // edited, another headset PV): the record it carried gets a row of its
    // own, as a product opened from a file does.
    if (this->m_headsetPvShown
        && (!carried
            || VrHub::productId(carried->event, carried->product)
                    != VrHub::productId(this->m_headsetPvShown->event, this->m_headsetPvShown->product))) {
        const HeadsetPv previous = *this->m_headsetPvShown;
        this->m_headsetPvShown.reset();
        this->recordHeadsetProduct(previous.event, previous.product);
    }
    this->m_headsetPvShown = carried;
    if (!carried)
        return;
    // Its own row from an earlier opening, if any: the view's row is it now.
    const QString carriedId = VrHub::productId(carried->event, carried->product);
    const AnalysisProduct held = ProductRegistry::instance().product(carriedId);
    if (!held.productId.isEmpty() && held.ownerId == this->m_windowScope && !held.window)
        ProductRegistry::instance().removeProduct(carriedId);
    // The view's own description of the cut stays; the headset's parameters
    // complete it, with where it was made and its file.
    const QVariantMap headset = VrHub::provenanceParams(carried->product);
    for (auto it = headset.cbegin(); it != headset.cend(); ++it)
        if (!params.contains(it.key()))
            params.insert(it.key(), it.value());
    replay = carried->product.replay;
    caveat = carried->product.caveat;
}

void vtkWindowCube::openHeadsetProduct(const VrHub::Event &event)
{
    const VrHub::Product p = VrHub::parseProduct(event);
    const QString label = p.label.isEmpty() ? p.kind : p.label;
    const bool thisCube = VrHub::aboutCube(event, this->filepath, this->remoteDatasetId)
            || (event.cubePath.isEmpty() && p.request.value(u"dataset_id"_s).toString() == this->remoteDatasetId);
    switch (VrHub::openIn(p)) {
    case VrHub::OpenIn::ImageViewer:
    case VrHub::OpenIn::CubeWindow:
        // The image viewer for a 2-D map, a cube window for a cube: the
        // backend says which when it opens the file.
        this->recordHeadsetProduct(event, p);
        emit this->openBackendFitsRequested(p.path, label);
        return;
    case VrHub::OpenIn::PvViewer: {
        const auto vertices = VrHub::pvVertices(p);
        if (thisCube && vertices.size() >= 2 && this->pvController && !this->pvController->isBusy()
            && !this->remoteDatasetId.isEmpty()) {
            // This cube's PV view, along the headset's path. The view's row
            // (this window's live PV diagram) takes the headset's record when
            // the result comes: one row, not two (issue #249). Not when the
            // record is not this window's to make (another cube's, or recorded
            // already by another window of this cube). A row of its own this
            // window made (opened before, then the view moved on) is taken
            // back into the view's row.
            this->m_headsetPvAsked.reset();
            const AnalysisProduct held = ProductRegistry::instance().product(VrHub::productId(event, p));
            if (VrHub::aboutCube(event, this->filepath, this->remoteDatasetId)
                && (held.productId.isEmpty() || (held.ownerId == this->m_windowScope && !held.window)))
                this->m_headsetPvAsked = HeadsetPv{ event, p, vertices, VrHub::pvWidth(p) };
            this->pvPolylineVertices = vertices;
            this->pvWidthPixels = VrHub::pvWidth(p);
            this->pvValid = true;
            this->showPersistentStatusMessage(u"Computing the headset's PV diagram..."_s);
            this->pvController->startExtraction(vertices, this->pvWidthPixels);
            return;
        }
        if (!p.path.isEmpty()) {
            this->recordHeadsetProduct(event, p);
            emit this->openBackendFitsRequested(p.path, label);
            return;
        }
        this->statusBar()->showMessage(
                u"This PV slice was cut from another cube: open it from that cube's window."_s, 6000);
        return;
    }
    case VrHub::OpenIn::SpectrumPlot:
        this->openHeadsetSpectrum(event, p);
        return;
    case VrHub::OpenIn::File:
        if (isFitsPath(p.path)) {
            this->recordHeadsetProduct(event, p);
            emit this->openBackendFitsRequested(p.path, label);
        } else {
            this->statusBar()->showMessage(
                    u"%1 is in the Workspace Exports: open it from the Data Hub."_s.arg(
                            p.workspaceFilename.isEmpty() ? p.path : p.workspaceFilename),
                    8000);
        }
        return;
    case VrHub::OpenIn::Nothing:
        break;
    }
    this->statusBar()->showMessage(u"The headset sent nothing to open for this."_s, 5000);
}

void vtkWindowCube::openHeadsetSpectrum(const VrHub::Event &event, const VrHub::Product &product)
{
    // A spectrum is drawn by the window that holds it: open in another window
    // still, it is not made (and recorded) a second time. The row of a window
    // since closed gives way to this one. Asked again right before it is
    // made, since a download may have let another window in meanwhile.
    if (!VrHub::aboutCube(event, this->filepath, this->remoteDatasetId)) {
        // A spectrum is a product of its cube: made here it would be recorded
        // (and replayed) against this one.
        this->statusBar()->showMessage(
                u"This spectrum was taken from %1: open it from that cube's window."_s.arg(
                        event.cubePath.isEmpty() ? u"another cube"_s : QFileInfo(event.cubePath).fileName()),
                8000);
        return;
    }
    const QString headsetId = VrHub::productId(event, product);
    const auto heldElsewhere = [this, headsetId]() {
        bool held = false;
        for (const AnalysisProduct &ap : ProductRegistry::instance().all()) {
            if (!ap.productId.startsWith(headsetId + u'@') || ap.ownerId == this->m_windowScope)
                continue;
            if (ap.window)
                held = true;
            else
                ProductRegistry::instance().removeProduct(ap.productId);
        }
        if (held)
            this->statusBar()->showMessage(
                    u"This spectrum is already open in another window: see its Session Data."_s, 6000);
        return held;
    };
    if (heldElsewhere())
        return;
    const auto show = [this, event, product, heldElsewhere](const QVector<double> &axis,
                                                            const QVector<double> &values,
                                                            const QString &xLabel,
                                                            const QString &yLabel) {
        if (heldElsewhere())
            return;
        QVector<double> key = axis;
        if (key.size() != values.size()) {
            key.resize(values.size());
            for (int i = 0; i < key.size(); ++i)
                key[i] = i;
        }
        const QVariantMap params = VrHub::provenanceParams(product);
        const QString label = product.label.isEmpty() ? u"Headset spectrum"_s : product.label;
        const QString id = this->addSpectrumProduct(
                u"spectrum"_s, VrHub::productId(event, product), label, key, values,
                xLabel.isEmpty() ? u"Channel"_s : xLabel, yLabel, params);
        // With the requests that made it, under the same id (one row).
        if (!id.isEmpty())
            this->registerAnalysisProduct(u"spectrum"_s, label, params, this, id, u"spectrum1d"_s,
                                          /*revealProvenance=*/false, product.origin,
                                          /*selectInSession=*/false, product.replay, product.caveat);
    };
    if (!product.values.isEmpty()) {
        show(product.axis, product.values, product.xLabel, product.yLabel);
        return;
    }
    // Read from the file the backend wrote under the exports.
    const VrLink::Inputs in = this->vrHeadsetInputs();
    const QString filename = product.workspaceFilename;
    struct Read
    {
        bool ok{ false };
        QString error;
        QVector<double> axis, values;
        QString xLabel, yLabel;
    };
    auto *watcher = new QFutureWatcher<Read>(this);
    QObject::connect(watcher, &QFutureWatcher<Read>::finished, this, [this, watcher, show, product]() {
        const Read r = watcher->result();
        watcher->deleteLater();
        if (!r.ok) {
            this->statusBar()->showMessage(u"The headset's spectrum could not be read: %1"_s.arg(r.error), 8000);
            return;
        }
        show(r.axis, r.values, product.xLabel.isEmpty() ? r.xLabel : product.xLabel,
             product.yLabel.isEmpty() ? r.yLabel : product.yLabel);
    });
    watcher->setFuture(QtConcurrent::run([in, filename]() {
        Read r;
        QTemporaryDir dir;
        if (!dir.isValid()) {
            r.error = u"no temporary folder"_s;
            return r;
        }
        BackendClient client(in.baseUrl, in.token);
        client.setSessionId(in.sessionId);
        // CSV or FITS table, as the headset asked for: read by what it holds.
        const QString local = dir.filePath(u"spectrum"_s);
        if (!client.downloadExport(filename, local, r.error))
            return r;
        QFile f(local);
        if (!f.open(QIODevice::ReadOnly)) {
            r.error = f.errorString();
            return r;
        }
        r.ok = VrHub::parseSpectrumFile(f.readAll(), r.axis, r.values, &r.xLabel, &r.yLabel,
                                        &r.error);
        return r;
    }));
}

void vtkWindowCube::followHeadsetLook()
{
    if (!this->m_headsetHub || !this->m_headsetFollow)
        return;
    // Not before the cube's first threshold is set and its bounds come from
    // the data: the first load would overwrite the headset's threshold with
    // its default, or the 0..1 placeholder would clamp it, and the look would
    // count as applied. Asked again when both are (applyRemotePreviewResult,
    // adoptRemoteLevels).
    if (!this->m_initialThresholdApplied || !this->m_boundsFromData)
        return;
    // The latest look and clip of this cube, each applied once, and only
    // while the headset that set them shows this cube.
    const VrHub::FollowStep step =
            VrHub::nextToFollow(this->m_headsetHub->activity().events(), this->m_headsetHub->headsets(),
                                this->filepath, this->remoteDatasetId, this->m_headsetFollowMarks);
    if (!step.any())
        return;
    HeadsetLookApplied applied;
    if (step.look)
        applied = this->applyHeadsetLook(VrHub::parseLook(step.look->payload));
    if (step.clip) {
        this->m_headsetClip = VrHub::parseClip(step.clip->payload);
        this->applyHeadsetClip();
    }
    // Said, so a change made in VR is not a surprise here.
    const QString text = VrHub::followedText(applied.colormap, applied.threshold, step.clip.has_value(),
                                             applied.unmatchedColormap);
    if (!text.isEmpty())
        this->statusBar()->showMessage(text + u'.', 4000);
}

vtkWindowCube::HeadsetLookApplied vtkWindowCube::applyHeadsetLook(const VrHub::Look &look)
{
    HeadsetLookApplied applied;
    QStringList names;
    for (const std::string &n : ColorMaps::GetColorMapNames())
        names << QString::fromStdString(n);
    const QString colormap = VrHub::desktopColormap(look.colormap, names);
    if (!colormap.isEmpty() && colormap != this->m_activeColorMapName) {
        this->applyColorMapByName(colormap);
        applied.colormap = true;
    }
    if (!look.colormap.isEmpty() && colormap.isEmpty())
        applied.unmatchedColormap = look.colormap;
    if (look.threshold) {
        // Compared as the field will show it (clamped, its own format): the
        // same threshold again is not a change to announce or re-render.
        const auto [lo, hi] = this->thresholdSpan();
        const float wanted = static_cast<float>(*look.threshold);
        if (visivo::thresholdText(std::clamp(wanted, lo, hi)) != ui->lineThreshold->text()) {
            // As if typed in the threshold field: the 3-D view (and a remote
            // one) follow it, and it counts as chosen, not as the backend's
            // default.
            ui->lineThreshold->setText(QString::number(*look.threshold, 'g', 6));
            ui->lineThreshold->setModified(true);
            this->thresholdLineChanged();
            applied.threshold = true;
        }
    }
    return applied;
}

void vtkWindowCube::applyHeadsetClip()
{
    const bool wanted = this->m_headsetClip.set && this->m_headsetFollow;
    // A view the backend renders is cropped there (its set_crop, as the
    // headset's refined frames are); dropped by itself when there is none.
    if (wanted || this->m_headsetCropping) {
        QVariantMap cmd;
        cmd[u"type"_s] = u"set_crop"_s;
        cmd[u"box"_s] = wanted ? VrHub::cropCommandBox(this->m_headsetClip,
                                                       { this->remoteDatasetWidth, this->remoteDatasetHeight,
                                                         this->remoteDatasetDepth })
                               : QVariant();
        this->sendRemoteRenderStateCommand(cmd);
    }
    if (!wanted) {
        if (this->m_headsetCropping && this->volumeMapper) {
            this->volumeMapper->CroppingOff();
            if (auto *rw = ui->vtkCube->renderWindow())
                rw->Render();
        }
        this->m_headsetCropping = false;
        return;
    }
    // Followed from now on, here and in a remote view's bootstrap, whether or
    // not the local volume exists yet.
    this->m_headsetCropping = true;
    if (!this->volumeMapper)
        return;
    auto *display = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!display)
        return;
    // Full-cube voxels to the display's world, as the lasso places its mask
    // (updateLassoDisplayPlacement): full resolution (or an ROI) is in
    // full-cube voxels; a preview is scaled by (displayDim-1)/(fullDim-1).
    int ddim[3];
    display->GetDimensions(ddim);
    double dorigin[3], dspacing[3];
    display->GetOrigin(dorigin);
    display->GetSpacing(dspacing);
    const int full[3] = { std::max(1, this->remoteDatasetWidth), std::max(1, this->remoteDatasetHeight),
                          std::max(1, this->remoteDatasetDepth) };
    double planes[6];
    for (int i = 0; i < 3; ++i) {
        const double s = (!this->usingHighResCube && full[i] > 1 && ddim[i] > 1)
                ? double(ddim[i] - 1) / double(full[i] - 1)
                : 1.0;
        // Faces at the outer edges of the end voxels (both stay inside), as
        // the render session draws them, then as the mapper must be told
        // them to draw them there.
        planes[2 * i] = VrHub::gpuCropPlane((this->m_headsetClip.min[i] - 0.5) * s, dorigin[i],
                                            dspacing[i], ddim[i]);
        planes[2 * i + 1] = VrHub::gpuCropPlane((this->m_headsetClip.max[i] + 0.5) * s, dorigin[i],
                                                dspacing[i], ddim[i]);
    }
    this->volumeMapper->SetCroppingRegionPlanes(planes);
    this->volumeMapper->SetCroppingRegionFlagsToSubVolume();
    this->volumeMapper->CroppingOn();
    if (auto *rw = ui->vtkCube->renderWindow())
        rw->Render();
}
