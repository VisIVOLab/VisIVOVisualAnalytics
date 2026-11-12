// vtkWindowCube_VrShare.cpp
// Selections shared with the VR headset (issue #188). The headset's page and
// this window work in the same backend session (the pairing link of
// Tools ▸ Views ▸ Open in VR Headset…). A Kinematic Lasso or a box made in
// the headset is shown here as the lasso's mask (overlay, isosurface, channel
// profile) with the statistics the backend measured on the full cube; the
// lasso on screen here is sent to the headset with "Show in Headset". The side
// that made a selection records it: a selection received is presented with
// the creator's record attached and records no step of its own; an Apply of
// it records the creator's grow (or the box) as its replay.

#include "vtkWindowCube.h"

#include "ui_vtkWindowCube.h"
#include "theme/ToolDialogStyle.h"
#include "app/BackendClient.h"
#include "app/SharedSelectionLink.h"

#include <vtkGenericOpenGLRenderWindow.h>

#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {
const QString &showInHeadsetTip()
{
    static const QString tip =
            u"Send the Kinematic Lasso selection on screen to the VR headset's page, in this "
            u"window's session (open the headset first with Open in VR Headset…). Selections "
            u"made in the headset appear here by themselves."_s;
    return tip;
}
const QString &showInHeadsetDisabledTip()
{
    static const QString tip =
            u"make a Kinematic Lasso selection first: it is the selection sent to the headset"_s;
    return tip;
}
} // namespace

void vtkWindowCube::setupShowInHeadsetAction()
{
    this->m_actShowInHeadset = new QAction(u"Show Selection in VR Headset"_s, ui->menuTools);
    this->m_actShowInHeadset->setEnabled(false);
    this->m_actShowInHeadset->setToolTip(showInHeadsetDisabledTip());
    QObject::connect(this->m_actShowInHeadset, &QAction::triggered, this,
                     &vtkWindowCube::showSelectionInHeadset);
    ui->menuTools->addAction(this->m_actShowInHeadset);
}

void vtkWindowCube::startSelectionShare()
{
    if (this->remoteBackendUrl.isEmpty() || this->remoteDatasetId.isEmpty())
        return;
    if (!this->m_shareLink) {
        this->m_shareLink = new SharedSelectionLink(this);
        QObject::connect(this->m_shareLink, &SharedSelectionLink::received, this,
                         &vtkWindowCube::onSharedSelectionReceived);
        QObject::connect(this->m_shareLink, &SharedSelectionLink::published, this,
                         [this](qint64 seq, qint64 count, const QString &error, qint64 ms) {
                             const QString text = seq > 0
                                     ? u"Shown in the VR headset: %1 voxels (%2 ms)."_s.arg(count).arg(ms)
                                     : u"Not sent to the VR headset: %1"_s.arg(error);
                             if (this->m_lassoShareLabel) this->m_lassoShareLabel->setText(text);
                             this->statusBar()->showMessage(text, 6000);
                         });
        QObject::connect(this->m_shareLink, &SharedSelectionLink::stopped, this,
                         [this](const QString &why) {
                             if (this->m_lassoShareLabel)
                                 this->m_lassoShareLabel->setText(
                                         u"Selections are not shared with the headset: %1."_s.arg(why));
                         });
    }
    // The identity the headset's page was given (openInVrHeadset): the token
    // as BackendClient resolves it and this window's session, so both ends
    // read the same session's hub.
    const QString url = this->remoteBackendUrl;
    const QString token = BackendClient::effectiveToken(url, BackendClient(url, this->remoteBackendToken).token());
    const QString session = !this->remoteSessionId.isEmpty() ? this->remoteSessionId
                                                             : BackendClient::sessionIdForUrl(url, token);
    this->m_shareLink->start(url, token, session, this->remoteDatasetId, this->remoteDatasetWidth,
                             this->remoteDatasetHeight, this->remoteDatasetDepth);
}

void vtkWindowCube::showSelectionInHeadset()
{
    if (this->m_lassoMaskBytes.isEmpty()) {
        this->statusBar()->showMessage(u"Show in VR Headset: no Kinematic Lasso selection on screen."_s, 4000);
        return;
    }
    this->startSelectionShare();
    if (!this->m_shareLink) {
        this->statusBar()->showMessage(u"Show in VR Headset: this cube has no backend session."_s, 4000);
        return;
    }
    const visivo::SharedSelectionOrigin &from = this->m_lassoMaskShared;
    // A selection received without a grow (a box, a lasso sent without its
    // request) has none to pass on: sent as a mask only.
    const bool hasGrow = !from.received || from.hasGrow;
    const QVariantMap params = hasGrow
            ? visivo::lassoDisplayParams(this->m_lassoMaskGrow, this->m_lassoMaskNoiseWhere,
                                         this->m_lassoMaskHandEdited, this->m_lassoMaskTrimBox)
            : (from.kind == "box"_L1 ? visivo::sharedBoxDisplayParams(from.box, from.parameters)
                                     : from.parameters);
    const QJsonObject meta = visivo::desktopLassoMeta(this->m_lassoMaskGrow, hasGrow, params,
                                                      this->m_lassoMaskHandEdited, this->m_lassoMaskTrimBox);
    if (this->m_lassoShareLabel) this->m_lassoShareLabel->setText(u"Sending to the VR headset…"_s);
    this->m_shareLink->publish(u"lasso"_s, this->m_lassoMaskBytes,
                               { this->m_lassoMaskOrigin[0], this->m_lassoMaskOrigin[1], this->m_lassoMaskOrigin[2] },
                               { this->m_lassoMaskDims[0], this->m_lassoMaskDims[1], this->m_lassoMaskDims[2] },
                               meta);
}

void vtkWindowCube::noteLocalShareSelection()
{
    if (this->m_shareLink) this->m_shareLink->noteLocalSelection();
}

void vtkWindowCube::onSharedSelectionReceived(const visivo::SharedSelectionInfo &info,
                                              const QByteArray &mask, qint64 elapsedMs, bool own)
{
    // The latest selection wins, whichever side made it: a grow of this
    // window still in flight is dropped when it lands.
    if (this->m_lassoSelectWatcher.isRunning() || this->m_lassoLevelsWatcher.isRunning())
        this->m_lassoDiscardResult = true;
    this->m_lassoPendingRegrow = false;
    // The controls regrow a selection made HERE: they must not regrow the
    // headset's from this window's last seed. A double-click starts a new one.
    this->m_lassoLevels = {};
    this->m_lassoHasSeed = false;
    this->m_lassoMaskHandEdited = false;
    if (this->m_lassoEditActive) this->setLassoEditActive(false);
    if (this->m_lassoEditToggle) {
        QSignalBlocker b(this->m_lassoEditToggle);
        this->m_lassoEditToggle->setChecked(false);
    }

    RemoteKinematicSelectResult r;
    r.valid = true;
    r.roiWidth = info.dims[0]; r.roiHeight = info.dims[1]; r.roiDepth = info.dims[2];
    r.originX = info.origin[0]; r.originY = info.origin[1]; r.originZ = info.origin[2];
    r.fullWidth = info.fullShape[0]; r.fullHeight = info.fullShape[1]; r.fullDepth = info.fullShape[2];
    r.count = int(info.count);
    for (int i = 0; i < 6; ++i)
        r.bbox[i] = info.box[i];
    r.data = mask;
    // The creator's record, attached: an Apply records this grow (or the box).
    if (info.hasGrow) r.grow = info.grow;
    r.noiseWhere = u"set on the VR page"_s;
    r.trimBox = info.trimBox;
    r.shared.received = true;
    // This window's own selection, drawn back from the hub: the link chose it
    // as the hub's latest (another one was on screen meanwhile).
    r.shared.own = own;
    r.shared.source = info.source;
    r.shared.kind = info.kind;
    r.shared.hasGrow = info.kind == "lasso"_L1 && info.hasGrow;
    r.shared.box = info.box;
    r.shared.parameters = info.meta.value("parameters"_L1).toObject().toVariantMap();
    r.shared.seq = info.seq;

    this->ensureLassoPanel();
    this->presentLassoSelection(r);
    const QString note = own ? u"This window's selection, back from the session's hub: %1 voxels"_s.arg(info.count)
                             : visivo::sharedSelectionNote(info);
    if (this->m_lassoCountLabel)
        this->m_lassoCountLabel->setText(u"%1 voxels selected"_s.arg(info.count));
    if (this->m_lassoShareLabel)
        this->m_lassoShareLabel->setText(
                note + (info.statsValid ? QString()
                                        : u" (no statistics: %1)"_s.arg(info.statsError)));
    this->setLassoApplyEnabled(true);
    this->updateLassoSeedMarkerVisibility();
    this->refreshKinematicFitAvailability();
    // Non-modal: the status bar and the panel's line, nothing to dismiss.
    this->statusBar()->showMessage(u"%1. Shown in %2 ms."_s.arg(note).arg(elapsedMs), 10000);
    if (ui->vtkCube && ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::addLassoShareControls(QWidget *body, QVBoxLayout *outer)
{
    auto *btn = new QPushButton(u"Show in VR Headset"_s, body);
    btn->setToolTip(showInHeadsetTip());
    QObject::connect(btn, &QPushButton::clicked, this, &vtkWindowCube::showSelectionInHeadset);
    outer->addWidget(btn);
    auto *label = new QLabel(body);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);   // the note quotes the cube's BUNIT and the backend's words
    label->setStyleSheet(ToolDialogStyle::helperCss());
    outer->addWidget(label);
    this->m_lassoShowInHeadsetBtn = btn;
    this->m_lassoShareLabel = label;
    this->refreshShowInHeadset();
}

void vtkWindowCube::refreshShowInHeadset()
{
    const bool on = !this->m_lassoMaskBytes.isEmpty();
    if (this->m_lassoShowInHeadsetBtn) this->m_lassoShowInHeadsetBtn->setEnabled(on);
    if (this->m_actShowInHeadset) {
        // A tool that cannot act says why (as the cube tools' gate does).
        this->m_actShowInHeadset->setEnabled(on);
        this->m_actShowInHeadset->setToolTip(on ? showInHeadsetTip() : showInHeadsetDisabledTip());
        this->m_actShowInHeadset->setStatusTip(this->m_actShowInHeadset->toolTip());
    }
}
