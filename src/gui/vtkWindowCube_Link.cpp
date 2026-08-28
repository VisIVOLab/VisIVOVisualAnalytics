// Linked views (F3) — partial class of vtkWindowCube.
//
// "Link views" synchronises camera, channel and colour-scale across every open
// cube viewer that has linking enabled, so two (or more) cubes can be compared
// side by side — rotate one and the others follow, scrub the velocity slider
// and they scan together, change the colour map and they all recolour. This is
// the Encube/Fluke comparative-grid idea, done by linking the existing separate
// viewers rather than a new multi-pane window.
//
// Mechanism: a static registry of all open cube viewers. A change in a linked
// viewer is broadcast to the other linked viewers; an `m_applyingLinkedUpdate`
// guard plus the fact that programmatic slice/colour updates don't re-fire the
// user-facing slots prevents echo loops.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "ChannelOverlay.h"
#include "CubeUiAssembler.h"
#include "LutScale.h"
#include "Pane2DView.h"
#include "SpectralUnitConvert.h"

#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkLookupTable.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkTrivialProducer.h>

#include <QAction>
#include <QScopedValueRollback>
#include <QSignalBlocker>

#include <algorithm>

using namespace Qt::StringLiterals;

// Registry of every open cube viewer (QPointer auto-nulls on destruction).
LinkedWindowRegistry<vtkWindowCube> vtkWindowCube::s_registry;

namespace {
// Centre and diagonal of a cube's displayed data bounds. Returns false if the
// image has no usable bounds (so the camera sync can be skipped safely).
bool dataBoundsCenterDiag(vtkImageData *im, double center[3], double &diag)
{
    if (!im)
        return false;
    double b[6];
    im->GetBounds(b);
    if (b[1] < b[0] || b[3] < b[2] || b[5] < b[4])
        return false;
    center[0] = 0.5 * (b[0] + b[1]);
    center[1] = 0.5 * (b[2] + b[3]);
    center[2] = 0.5 * (b[4] + b[5]);
    const double dx = b[1] - b[0], dy = b[3] - b[2], dz = b[5] - b[4];
    diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    return diag > 1e-9;
}

// Spectral-axis unit parsing + same-quantity value conversion now live in
// SpectralUnitConvert.h (E-04 shared-helper lift) so they can be unit-tested.
}  // namespace

void vtkWindowCube::setLinkViews(bool on)
{
    this->m_linkViews = on;
    this->m_linkNoReceiverHintShown = false;   // §5.5: toggling the master re-arms the hint
    this->refreshLinkedFacetChips();           // §5.5: grey/enable the per-facet toggles
    if (this->m_actLinkViews && this->m_actLinkViews->isChecked() != on) {
        QSignalBlocker b(this->m_actLinkViews);
        this->m_actLinkViews->setChecked(on);
    }
    if (!on) {
        this->m_linked2DValid = false;   // STEP6-P2: drop the shared 2-D camera so a later relink starts fresh
        this->statusBar()->showMessage(u"Linked views: off."_s, 3000);
        return;
    }

    // Install the camera observers once, lazily, remembering the exact
    // interactor so the destructor removes them from the right object.
    // InteractionEvent follows live drags (rotate / pan); EndInteractionEvent
    // catches mouse-wheel zoom, which does NOT emit InteractionEvent.
    if (this->m_cubeCameraObserverTag == 0) {
        if (auto *rw = ui->vtkCube->renderWindow()) {
            if (auto *iren = rw->GetInteractor()) {
                this->m_cubeCameraObserverTag = iren->AddObserver(
                        vtkCommand::InteractionEvent, this,
                        &vtkWindowCube::onCubeCameraInteraction);
                this->m_cubeCameraEndObserverTag = iren->AddObserver(
                        vtkCommand::EndInteractionEvent, this,
                        &vtkWindowCube::onCubeCameraInteraction);
                this->m_cubeCameraInteractor = iren;
            }
        }
    }

    // Push this viewer's current camera + channel to the other linked viewers so
    // they snap into alignment immediately (colour is left to adopt on the next
    // change, to avoid silently clobbering a peer's chosen colour map).
    const auto windows = s_registry.snapshot();   // snapshot — peers may mutate the list
    int peers = 0;
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews)
            ++peers;
    this->broadcastLinkedCamera();
    // STEP6-P4: snap same-grid peers to this window's current 2-D view too. propagate2DCamera
    // no-ops when the active pane isn't a 2-D view, so this is safe to call unconditionally.
    this->propagate2DCamera(this->m_activePane);
    if (ui->spinSlice) {
        this->m_lastLinkedChannel = ui->spinSlice->value() - 1;
        this->broadcastLinkedChannel(this->m_lastLinkedChannel);
    }
    this->statusBar()->showMessage(
            peers > 0 ? u"Linked views: on (%1 linked viewer(s) synced)."_s.arg(peers)
                      : u"Linked views: on — enable it on another cube to sync them."_s,
            5000);
}

void vtkWindowCube::onCubeCameraInteraction()
{
    if (this->m_linkViews && !this->m_applyingLinkedUpdate)
        this->broadcastLinkedCamera();
}

void vtkWindowCube::broadcastLinkedCamera()
{
    if (!this->m_linkCamera)   // STEP6-P5: gated by the "camera" facet
        return;
    auto *rw = ui->vtkCube->renderWindow();
    auto *ren = rw ? rw->GetRenderers()->GetFirstRenderer() : nullptr;
    auto *cam = ren ? ren->GetActiveCamera() : nullptr;
    if (!cam)
        return;
    double center[3], diag;
    if (!dataBoundsCenterDiag(
                vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0)),
                center, diag))
        return;   // no usable data bounds → don't push a meaningless camera

    double pos[3], foc[3], up[3];
    cam->GetPosition(pos);
    cam->GetFocalPoint(foc);
    cam->GetViewUp(up);
    // Express the camera RELATIVE to this cube's data: view direction (unit),
    // distance and parallel-scale as fractions of the data diagonal, and the
    // focal offset from the data centre as a fraction of the diagonal. Peers
    // reconstruct against their OWN bounds, so different-sized cubes stay framed.
    double dir[3] = { pos[0] - foc[0], pos[1] - foc[1], pos[2] - foc[2] };
    const double dist = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (dist < 1e-9)
        return;
    dir[0] /= dist; dir[1] /= dist; dir[2] /= dist;
    const double relDist = dist / diag;
    const double relScale = cam->GetParallelScale() / diag;
    const double relOffset[3] = { (foc[0] - center[0]) / diag, (foc[1] - center[1]) / diag,
                                  (foc[2] - center[2]) / diag };
    const double angle = cam->GetViewAngle();

    const auto windows = s_registry.snapshot();   // snapshot — peer calls may mutate the list
    for (const auto &p : windows)
        // STEP6-P5: a peer with its "camera" facet off has opted out of camera sync.
        if (p && p != this && p->m_linkViews && p->m_linkCamera)
            p->applyLinkedCamera(dir, up, relOffset, relDist, relScale, angle);
}

void vtkWindowCube::applyLinkedCamera(const double dir[3], const double up[3],
                                      const double relOffset[3], double relDist,
                                      double relScale, double viewAngle)
{
    auto *rw = ui->vtkCube->renderWindow();
    auto *ren = rw ? rw->GetRenderers()->GetFirstRenderer() : nullptr;
    auto *cam = ren ? ren->GetActiveCamera() : nullptr;
    if (!cam)
        return;
    double center[3], diag;
    if (!dataBoundsCenterDiag(
                vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0)),
                center, diag))
        return;

    // Reconstruct the camera against THIS cube's own bounds.
    const double foc[3] = { center[0] + relOffset[0] * diag, center[1] + relOffset[1] * diag,
                            center[2] + relOffset[2] * diag };
    const double dist = relDist * diag;
    const double pos[3] = { foc[0] + dir[0] * dist, foc[1] + dir[1] * dist,
                            foc[2] + dir[2] * dist };
    QScopedValueRollback<bool> guard(this->m_applyingLinkedUpdate, true);
    cam->SetFocalPoint(foc[0], foc[1], foc[2]);
    cam->SetPosition(pos[0], pos[1], pos[2]);
    cam->SetViewUp(up[0], up[1], up[2]);
    cam->SetParallelScale(relScale * diag);
    cam->SetViewAngle(viewAngle);
    ren->ResetCameraClippingRange();
    rw->Render();
}

void vtkWindowCube::broadcastLinkedChannel(int channel0)
{
    if (!this->m_linkViews || !this->m_linkChannel)   // §5.5: gated by the "channel" facet
        return;
    // Resolve this channel's spectral value (velocity) so peers can match by
    // physical velocity rather than raw index.
    bool ok = false;
    const double spectralValue = this->spectralAxisValue(channel0, &ok);
    // Use the EFFECTIVE descriptor unit (what spectralAxisValue actually uses),
    // not the raw header unit, so the peer eligibility check matches.
    const QString unit = this->spectralAxisDescriptor().unit.trimmed();
    const int maxIdx = std::max(0, this->remoteSliceCount() - 1);
    const int clamped = std::clamp(channel0, 0, maxIdx);
    const double fraction = maxIdx > 0 ? double(clamped) / double(maxIdx) : 0.0;
    const auto windows = s_registry.snapshot();   // snapshot
    for (const auto &p : windows)
        // §5.5: a peer with its "channel" facet off has opted out of channel sync.
        if (p && p != this && p->m_linkViews && p->m_linkChannel)
            p->applyLinkedChannel(channel0, fraction, spectralValue, ok, unit);
}

void vtkWindowCube::applyLinkedChannel(int channel0, double fraction, double spectralValue,
                                       bool hasSpectral, const QString &spectralUnit)
{
    const int maxIdx = std::max(0, this->remoteSliceCount() - 1);
    int target = -1;

    // Velocity matching is meaningful when the cubes' spectral axes are the same
    // physical quantity (same unit, OR convertible e.g. m/s↔km/s) AND the target
    // velocity lies within this cube's range (they overlap → likely the same
    // object). Otherwise matching by absolute velocity just saturates at an
    // extreme, which is meaningless for unrelated cubes.
    if (hasSpectral && !spectralUnit.isEmpty()) {
        const QString myUnit = this->spectralAxisDescriptor().unit.trimmed();
        double matchValue = spectralValue;
        bool canMatch = (spectralUnit == myUnit);   // identical unit: no conversion
        if (!canMatch)
            canMatch = spectral::convertSpectralValue(spectralValue, spectralUnit, myUnit, matchValue);

        bool ok0 = false, okN = false;
        const double v0 = this->spectralAxisValue(0, &ok0);
        const double vN = this->spectralAxisValue(maxIdx, &okN);
        if (canMatch && ok0 && okN) {
            const double lo = std::min(v0, vN), hi = std::max(v0, vN);
            const double tol = (hi - lo) * 1e-3;   // small slack at the ends
            if (matchValue >= lo - tol && matchValue <= hi + tol) {
                double bestDiff = -1.0;
                for (int c = 0; c <= maxIdx; ++c) {
                    bool ok = false;
                    const double v = this->spectralAxisValue(c, &ok);
                    if (!ok)
                        continue;
                    const double d = std::abs(v - matchValue);
                    if (target < 0 || d < bestDiff) {
                        bestDiff = d;
                        target = c;
                    }
                }
            }
        }
    }

    // Fallback (different objects / units): proportional channel mapping so both
    // cubes scan their full velocity range together, never saturating.
    if (target < 0)
        target = std::clamp(int(std::lround(fraction * maxIdx)), 0, maxIdx);

    // Remember the channel we're moving to so our own slice-result broadcast is
    // de-duped (no echo back to the sender).
    this->m_lastLinkedChannel = target;
    // Reflect the synced channel in the slider/spin widgets immediately — the
    // async slice result will set them again to the same value.
    if (ui->sliderSlice) {
        QSignalBlocker b(ui->sliderSlice);
        ui->sliderSlice->setValue(target + 1);
    }
    if (ui->spinSlice) {
        QSignalBlocker b(ui->spinSlice);
        ui->spinSlice->setValue(target + 1);
    }
    if (this->channelOverlay) {   // the floating scrubber too (immediate feedback)
        this->channelOverlay->setChannel(target + 1);
        this->channelOverlay->setSpectralLabel(this->formatSpectralAxisValue(target));
    }
    // Programmatic request; the result path sets the widgets via QSignalBlocker,
    // so it does not re-fire the user slot → no echo.
    QScopedValueRollback<bool> guard(this->m_applyingLinkedUpdate, true);
    this->requestRemoteSlice(target);
}

void vtkWindowCube::broadcastLinkedColour()
{
    if (!this->m_linkViews || !this->m_linkLut)   // §5.5: gated by the "LUT" facet
        return;
    const QString name = this->m_activeColorMapName;
    const int mode = static_cast<int>(this->m_volumeScaleMode);
    const double gamma = this->m_volumeScaleGamma;
    const auto windows = s_registry.snapshot();   // snapshot
    for (const auto &p : windows)
        // §5.5: a peer with its "LUT" facet off has opted out of colour sync.
        if (p && p != this && p->m_linkViews && p->m_linkLut)
            p->applyLinkedColour(name, mode, gamma);
}

void vtkWindowCube::applyLinkedColour(const QString &name, int scaleMode, double gamma)
{
    const bool sameName = (name.isEmpty() || name == this->m_activeColorMapName);
    const bool sameScale = (static_cast<int>(this->m_volumeScaleMode) == scaleMode
                            && std::abs(this->m_volumeScaleGamma - gamma) < 1e-9);
    if (sameName && sameScale)
        return;
    QScopedValueRollback<bool> guard(this->m_applyingLinkedUpdate, true);
    this->m_volumeScaleMode = static_cast<LutScale::Mode>(scaleMode);
    this->m_volumeScaleGamma = gamma;
    // applyColorMapByName rebuilds the volume LUT through the (now updated)
    // stretch, so this applies both colormap and stretch in one go.
    this->applyColorMapByName(name.isEmpty() ? this->m_activeColorMapName : name);
}

bool vtkWindowCube::sameSpatialGrid2D(const vtkWindowCube *peer) const
{
    // STEP6-P4 / D1: a world-coordinate 2-D camera transfers directly only when both cubes share
    // the spatial world grid — same in-plane dimensions, spacing and origin. (Depth is irrelevant
    // to the 2-D pan/zoom.) Compares the backend-reported geometry the panes are built from.
    if (!peer)
        return false;
    auto eq = [](double a, double b) { return std::abs(a - b) <= 1e-9 * (1. + std::abs(a)); };
    return this->remoteDatasetWidth == peer->remoteDatasetWidth
            && this->remoteDatasetHeight == peer->remoteDatasetHeight
            && eq(this->remoteDatasetSpacing[0], peer->remoteDatasetSpacing[0])
            && eq(this->remoteDatasetSpacing[1], peer->remoteDatasetSpacing[1])
            && eq(this->remoteDatasetOrigin[0], peer->remoteDatasetOrigin[0])
            && eq(this->remoteDatasetOrigin[1], peer->remoteDatasetOrigin[1]);
}

void vtkWindowCube::broadcastLinked2DCamera(const double dataCenterXY[2], double worldHeight)
{
    // STEP6-P4: push the gutter-independent 2-D view to linked peer windows on the SAME spatial
    // grid. Guarded by m_applyingLinkedUpdate so a cross-window apply can't re-broadcast.
    if (!this->m_linkViews || !this->m_linkCamera || this->m_applyingLinkedUpdate)
        return;
    if (!(worldHeight > 0.))
        return;
    const auto windows = s_registry.snapshot();   // snapshot — peers may mutate the list
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews && p->m_linkCamera && this->sameSpatialGrid2D(p))
            p->applyLinked2DCamera(dataCenterXY, worldHeight);
}

void vtkWindowCube::applyLinked2DCamera(const double dataCenterXY[2], double worldHeight)
{
    // Receiver: adopt the shared 2-D view on every visible 2-D pane. m_applyingLinkedUpdate guards
    // against re-broadcast; the pane setters are themselves echo-guarded (Pane2DView) or fire no
    // InteractionEvent (slice programmatic set), so this cannot loop.
    if (!this->m_linkViews || !this->m_linkCamera || !(worldHeight > 0.))
        return;
    QScopedValueRollback<bool> guard(this->m_applyingLinkedUpdate, true);
    // Remember it so THIS window's own fits re-adopt the shared view too (STEP6-P2).
    this->m_linked2DCenter[0] = dataCenterXY[0];
    this->m_linked2DCenter[1] = dataCenterXY[1];
    this->m_linked2DWorldHeight = worldHeight;
    this->m_linked2DValid = true;
    this->applyNormalizedTo2DPanes(dataCenterXY, worldHeight, /*skipSlot=*/-1);
}

void vtkWindowCube::broadcastLinked2DSliceLut()
{
    // STEP6-P4: push this window's 2-D SLICE colour (name + scale + gamma + invert) to linked
    // peers. Gated by the LUT facet; range is NOT shared (data-dependent), so no grid gate — same
    // as the 3-D volume-colour path. m_applyingLinkedUpdate blocks re-broadcast from a receiver.
    if (!this->m_linkViews || !this->m_linkLut || this->m_applyingLinkedUpdate)
        return;
    const QString name = this->m_activeSliceLutName;
    const int mode = static_cast<int>(this->m_sliceScaleMode);
    const double gamma = this->m_sliceScaleGamma;
    const bool invert = this->m_sliceInvert;
    const auto windows = s_registry.snapshot();   // snapshot
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews && p->m_linkLut)
            p->applyLinked2DSliceLut(name, mode, gamma, invert);
}

void vtkWindowCube::applyLinked2DSliceLut(const QString &name, int scaleMode, double gamma,
                                          bool invert)
{
    if (!this->m_linkViews || !this->m_linkLut)
        return;
    const bool same = (name.isEmpty() || name == this->m_activeSliceLutName)
            && static_cast<int>(this->m_sliceScaleMode) == scaleMode
            && std::abs(this->m_sliceScaleGamma - gamma) < 1e-9
            && this->m_sliceInvert == invert;
    if (same)
        return;
    QScopedValueRollback<bool> guard(this->m_applyingLinkedUpdate, true);
    this->m_sliceScaleMode = static_cast<LutScale::Mode>(scaleMode);
    this->m_sliceScaleGamma = gamma;
    this->m_sliceInvert = invert;
    // Rebuild lutSlice through the (now updated) stretch, KEEPING this cube's own table range.
    this->applySliceLutByName(name.isEmpty() ? this->m_activeSliceLutName : name);
    this->updateDisplaySectionForActivePane();   // resync the DISPLAY combo if the slice is active
    // applySliceLutByName renders only when viewingSlice(); in a multi-pane layout the slice can be
    // VISIBLE without being the active/global view, so repaint + refresh its LUT strip explicitly
    // whenever a slice pane is on screen.
    const int ss = this->sliceSlot();
    if (ss >= 0) {
        if (this->sliceWin)
            this->sliceWin->Render();
        this->refreshPaneLutStrip(ss);
    }
}

QString vtkWindowCube::productStem(const QString &productId)
{
    const int at = productId.indexOf(QLatin1Char('@'));
    return at > 0 ? productId.left(at) : productId;
}

void vtkWindowCube::broadcastLinked2DImageLut(const QString &stem, const QString &name,
                                              int scaleMode, double gamma, bool invert)
{
    // STEP6-P4: push a MOMENT / image-product colour to linked peers, matched by the
    // window-independent stem. Range is NOT shared (like the slice/volume paths), so no grid gate.
    if (!this->m_linkViews || !this->m_linkLut || this->m_applyingLinkedUpdate || stem.isEmpty())
        return;
    const auto windows = s_registry.snapshot();   // snapshot
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews && p->m_linkLut)
            p->applyLinked2DImageLut(stem, name, scaleMode, gamma, invert);
}

void vtkWindowCube::applyLinked2DImageLut(const QString &stem, const QString &name, int scaleMode,
                                          double gamma, bool invert)
{
    if (!this->m_linkViews || !this->m_linkLut || stem.isEmpty())
        return;
    const LutScale::Mode mode = static_cast<LutScale::Mode>(scaleMode);
    bool touchedActive = false;
    {
        // Both guards: m_applyingLinkedUpdate blocks a re-broadcast; m_applyingPaneDisplay blocks
        // the receiver pane's lutChanged from re-entering propagatePaneLutWithinWindow.
        QScopedValueRollback<bool> gLink(this->m_applyingLinkedUpdate, true);
        QScopedValueRollback<bool> gPane(this->m_applyingPaneDisplay, true);
        // Update the cache for EVERY product with this stem, so a fresh mount adopts the colour.
        // (Range/invert stay per-pane; the cache holds colormap+scale+gamma.)
        for (auto it = this->m_products2d.begin(); it != this->m_products2d.end(); ++it)
            if (productStem(it.key()) == stem) {
                it->lutName = name;
                it->scaleMode = mode;
                it->gamma = gamma;
            }
        // Recolour every pane showing a product with this stem — VISIBLE or PARKED (mountView's
        // same-product path keeps the pane's own LUT, so a parked pane would otherwise re-show its
        // stale colour; matches the intra-window propagatePaneLutWithinWindow, which also has no
        // visibility guard).
        for (int j = 0; j < 4; ++j) {
            if (!m_paneViews[j] || m_slots[j].view.kind != ViewId::Product
                || productStem(m_slots[j].view.productId) != stem
                || !this->m_products2d.contains(m_slots[j].view.productId))
                continue;
            m_paneViews[j]->applyLut(name, mode, gamma, invert);
            this->refreshPaneLutStrip(j);
            if (j == this->m_activePane)
                touchedActive = true;
        }
    }
    // If the ACTIVE pane was recoloured from a peer, resync the DISPLAY dock combos so they don't
    // show the old colormap. Done AFTER the guards release (it manages m_applyingPaneDisplay).
    if (touchedActive)
        this->updateDisplaySectionForActivePane();
}

void vtkWindowCube::broadcastLinkedBlendMode()
{
    if (!this->m_linkViews)
        return;
    const int idx = this->m_volumeBlendMode;
    const auto windows = s_registry.snapshot();   // snapshot
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews)
            p->applyLinkedBlendMode(idx);
}

void vtkWindowCube::applyLinkedBlendMode(int idx)
{
    idx = std::clamp(idx, 0, 2);   // 0=Composite, 1=MIP, 2=MinIP
    if (idx == this->m_volumeBlendMode)
        return;
    QScopedValueRollback<bool> guard(this->m_applyingLinkedUpdate, true);
    this->applyVolumeBlendMode(idx);   // sets m_volumeBlendMode + renders
    if (this->m_uiAssembler)
        this->m_uiAssembler->syncVolumeModeSidebar(idx);
    // Reflect in the menu actions (QActionGroup) without re-triggering them.
    QAction *act = idx == 1 ? this->m_actionMIP
                 : idx == 2 ? this->m_actionMinIP
                            : this->m_actionComposite;
    if (act) {
        QSignalBlocker b(act);
        act->setChecked(true);
    }
}
