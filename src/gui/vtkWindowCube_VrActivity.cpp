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
// Headset's Look (off by default) applies the headset's colour map,
// threshold and clip box to this window when they are about this cube.

#include "vtkWindowCube.h"

#include "ui_vtkWindowCube.h"
#include "ColorMaps.h"
#include "WorkspaceChrome.h"
#include "app/BackendClient.h"
#include "app/HeadsetHubLink.h"
#include "app/VrLink.h"
#include "PvController.h"

#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkTrivialProducer.h>

#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>

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
    this->statusBar()->addPermanentWidget(this->m_headsetStatusLabel);

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
        this->m_headsetStatusLabel->setText(text);
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
        this->m_headsetActivityState->setText(state);
    }
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
            u"3-D view, as they change. Off: this window keeps its own."_s);
    this->m_headsetFollowLook->setChecked(false);
    QObject::connect(this->m_headsetFollowLook, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            this->m_headsetLookApplied = { -1, 0 };
            this->m_headsetClipApplied = { -1, 0 };
            this->followHeadsetLook();
        } else {
            // The clip box is the headset's: the whole cube comes back. The
            // colours and threshold stay as they are, now this window's own.
            this->m_headsetClip = {};
            this->applyHeadsetClip();
        }
    });
    layout->addWidget(this->m_headsetFollowLook);
    this->m_headsetActivityList = new QListWidget(panel);
    this->m_headsetActivityList->setSelectionMode(QAbstractItemView::NoSelection);
    this->m_headsetActivityList->setMinimumHeight(220);
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
    if (this->m_headsetFollowLook && this->m_headsetFollowLook->isChecked())
        this->followHeadsetLook();
    if (!this->m_headsetActivityList || !this->m_actHeadsetActivity
        || !this->m_actHeadsetActivity->isChecked())
        return;   // rebuilt when the tool is opened
    QListWidget *list = this->m_headsetActivityList;
    list->clear();
    const QList<VrHub::Event> none;
    const QList<VrHub::Event> &events =
            this->m_headsetHub ? this->m_headsetHub->activity().events() : none;
    if (events.isEmpty()) {
        list->addItem(u"Nothing done in the headset yet."_s);
        return;
    }
    for (const VrHub::Event &e : events) {
        auto *item = new QListWidgetItem(list);
        auto *row = new QWidget(list);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(4, 2, 4, 2);
        auto *text = new QLabel(VrHub::eventRowText(e), row);
        text->setTextFormat(Qt::PlainText);
        text->setToolTip(e.cubePath.isEmpty() ? QString() : u"Cube: %1"_s.arg(e.cubePath));
        h->addWidget(text, 1);
        if (e.kind == u"product"_s) {
            const VrHub::Product p = VrHub::parseProduct(e);
            auto *open = new QToolButton(row);
            open->setText(u"Open"_s);
            const VrHub::OpenIn where = VrHub::openIn(p);
            open->setEnabled(where != VrHub::OpenIn::Nothing);
            open->setToolTip(where == VrHub::OpenIn::Nothing
                                     ? u"The headset sent no file for it."_s
                                     : u"Open it here."_s);
            QObject::connect(open, &QToolButton::clicked, this,
                             [this, e]() { this->openHeadsetProduct(e); });
            h->addWidget(open);
        }
        item->setSizeHint(row->sizeHint());
        list->setItemWidget(item, row);
    }
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
    QVariantMap params = product.parameters;
    params.insert(u"Made in"_s, u"the VR headset"_s);
    if (!product.path.isEmpty())
        params.insert(u"File"_s, product.workspaceFilename.isEmpty() ? product.path
                                                                      : product.workspaceFilename);
    const QString kind = product.recipeKind.isEmpty() ? product.kind : product.recipeKind;
    // The same id however often it is opened: one row in Provenance.
    this->registerAnalysisProduct(kind, product.label.isEmpty() ? kind : product.label, params,
                                  /*window=*/nullptr, VrHub::productId(event, product), renderer,
                                  /*revealProvenance=*/false, product.origin,
                                  /*selectInSession=*/false, product.replay, product.caveat);
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
            // This cube's PV view, along the headset's path. The view's row is
            // this window's live PV diagram (replaced by the next PV here); the
            // headset's product keeps its own row, with its record.
            this->recordHeadsetProduct(event, p);
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
        QVariantMap params = product.parameters;
        params.insert(u"Made in"_s, u"the VR headset"_s);
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
        const QString local = dir.filePath(u"spectrum.csv"_s);
        if (!client.downloadExport(filename, local, r.error))
            return r;
        QFile f(local);
        if (!f.open(QIODevice::ReadOnly)) {
            r.error = f.errorString();
            return r;
        }
        r.ok = VrHub::parseSpectrumCsv(f.readAll(), r.axis, r.values, &r.xLabel, &r.yLabel);
        if (!r.ok)
            r.error = u"the file holds no spectrum"_s;
        return r;
    }));
}

void vtkWindowCube::followHeadsetLook()
{
    if (!this->m_headsetHub || !this->m_headsetFollowLook || !this->m_headsetFollowLook->isChecked())
        return;
    // The latest look and clip of this cube (the list is newest first),
    // each applied once.
    const VrHub::Event *look = nullptr;
    const VrHub::Event *clip = nullptr;
    for (const VrHub::Event &e : this->m_headsetHub->activity().events()) {
        if (!VrHub::aboutCube(e, this->filepath, this->remoteDatasetId))
            continue;
        if (!look && e.kind == u"look"_s)
            look = &e;
        else if (!clip && e.kind == u"clip"_s)
            clip = &e;
    }
    if (look && QPair<int, qint64>{ look->epoch, look->seq } != this->m_headsetLookApplied) {
        this->m_headsetLookApplied = { look->epoch, look->seq };
        this->applyHeadsetLook(VrHub::parseLook(look->payload));
    }
    if (clip && QPair<int, qint64>{ clip->epoch, clip->seq } != this->m_headsetClipApplied) {
        this->m_headsetClipApplied = { clip->epoch, clip->seq };
        this->m_headsetClip = VrHub::parseClip(clip->payload);
        this->applyHeadsetClip();
    }
}

void vtkWindowCube::applyHeadsetLook(const VrHub::Look &look)
{
    QStringList names;
    for (const std::string &n : ColorMaps::GetColorMapNames())
        names << QString::fromStdString(n);
    const QString colormap = VrHub::desktopColormap(look.colormap, names);
    if (!colormap.isEmpty() && colormap != this->m_activeColorMapName)
        this->applyColorMapByName(colormap);
    if (look.threshold) {
        // As if typed in the threshold field: the 3-D view (and a remote one)
        // follow it, and it counts as chosen, not as the backend's default.
        ui->lineThreshold->setText(QString::number(*look.threshold, 'g', 6));
        ui->lineThreshold->setModified(true);
        this->thresholdLineChanged();
    }
    QStringList said;
    if (!look.colormap.isEmpty() && colormap.isEmpty())
        said << u"colour map %1 has no counterpart here"_s.arg(look.colormap);
    this->statusBar()->showMessage(said.isEmpty() ? u"Following the headset's look."_s
                                                  : u"Following the headset's look (%1)."_s.arg(said.join(u"; "_s)),
                                   4000);
}

void vtkWindowCube::applyHeadsetClip()
{
    const bool wanted = this->m_headsetClip.set && this->m_headsetFollowLook
            && this->m_headsetFollowLook->isChecked();
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
    const int full[3] = { std::max(1, this->remoteDatasetWidth), std::max(1, this->remoteDatasetHeight),
                          std::max(1, this->remoteDatasetDepth) };
    double planes[6];
    for (int i = 0; i < 3; ++i) {
        const double s = (!this->usingHighResCube && full[i] > 1 && ddim[i] > 1)
                ? double(ddim[i] - 1) / double(full[i] - 1)
                : 1.0;
        // Half a voxel out on each side: both end voxels stay inside.
        planes[2 * i] = (this->m_headsetClip.min[i] - 0.5) * s;
        planes[2 * i + 1] = (this->m_headsetClip.max[i] + 0.5) * s;
    }
    this->volumeMapper->SetCroppingRegionPlanes(planes);
    this->volumeMapper->SetCroppingRegionFlagsToSubVolume();
    this->volumeMapper->CroppingOn();
    if (auto *rw = ui->vtkCube->renderWindow())
        rw->Render();
}
