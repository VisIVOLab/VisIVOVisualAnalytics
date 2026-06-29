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

#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkTrivialProducer.h>

#include <QAction>
#include <QScopedValueRollback>

#include <algorithm>

using namespace Qt::StringLiterals;

// Registry of every open cube viewer (QPointer auto-nulls on destruction).
QList<QPointer<vtkWindowCube>> vtkWindowCube::s_cubeWindows;

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
}  // namespace

void vtkWindowCube::setLinkViews(bool on)
{
    this->m_linkViews = on;
    if (this->m_actLinkViews && this->m_actLinkViews->isChecked() != on) {
        QSignalBlocker b(this->m_actLinkViews);
        this->m_actLinkViews->setChecked(on);
    }
    if (!on) {
        this->statusBar()->showMessage(u"Linked views: off."_s, 3000);
        return;
    }

    // Install the camera-interaction observer once, lazily, remembering the
    // exact interactor so the destructor removes it from the right object.
    if (this->m_cubeCameraObserverTag == 0) {
        if (auto *rw = ui->vtkCube->renderWindow()) {
            if (auto *iren = rw->GetInteractor()) {
                this->m_cubeCameraObserverTag = iren->AddObserver(
                        vtkCommand::InteractionEvent, this,
                        &vtkWindowCube::onCubeCameraInteraction);
                this->m_cubeCameraInteractor = iren;
            }
        }
    }

    // Push this viewer's current camera + channel to the other linked viewers so
    // they snap into alignment immediately (colour is left to adopt on the next
    // change, to avoid silently clobbering a peer's chosen colour map).
    const auto windows = s_cubeWindows;   // snapshot — peers may mutate the list
    int peers = 0;
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews)
            ++peers;
    this->broadcastLinkedCamera();
    if (ui->spinSlice)
        this->broadcastLinkedChannel(ui->spinSlice->value() - 1);
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

    const auto windows = s_cubeWindows;   // snapshot — peer calls may mutate the list
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews)
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
    if (!this->m_linkViews)
        return;
    // Resolve this channel's spectral value (velocity) so peers can match by
    // physical velocity rather than raw index.
    bool ok = false;
    const double spectralValue = this->spectralAxisValue(channel0, &ok);
    // Use the EFFECTIVE descriptor unit (what spectralAxisValue actually uses),
    // not the raw header unit, so the peer eligibility check matches.
    const QString unit = this->spectralAxisDescriptor().unit.trimmed();
    const auto windows = s_cubeWindows;   // snapshot
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews)
            p->applyLinkedChannel(channel0, spectralValue, ok, unit);
}

void vtkWindowCube::applyLinkedChannel(int channel0, double spectralValue, bool hasSpectral,
                                       const QString &spectralUnit)
{
    const int maxIdx = std::max(0, this->remoteSliceCount() - 1);
    int target = std::clamp(channel0, 0, maxIdx);

    // Prefer matching by spectral value (velocity) when both cubes expose the
    // same spectral unit: pick the channel whose spectral value is closest.
    if (hasSpectral && !spectralUnit.isEmpty()
        && spectralUnit == this->spectralAxisDescriptor().unit.trimmed()) {
        double bestDiff = -1.0;
        int best = -1;
        for (int c = 0; c <= maxIdx; ++c) {
            bool ok = false;
            const double v = this->spectralAxisValue(c, &ok);
            if (!ok)
                continue;
            const double d = std::abs(v - spectralValue);
            if (best < 0 || d < bestDiff) {
                bestDiff = d;
                best = c;
            }
        }
        if (best >= 0)
            target = best;
    }

    // The programmatic slice request updates the slider via QSignalBlocker, so
    // it does not re-fire the user slot → no echo.
    QScopedValueRollback<bool> guard(this->m_applyingLinkedUpdate, true);
    this->requestRemoteSlice(target);
}

void vtkWindowCube::broadcastLinkedColorMap(const QString &name)
{
    if (!this->m_linkViews || name.isEmpty())
        return;
    const auto windows = s_cubeWindows;   // snapshot
    for (const auto &p : windows)
        if (p && p != this && p->m_linkViews)
            p->applyLinkedColorMap(name);
}

void vtkWindowCube::applyLinkedColorMap(const QString &name)
{
    if (name == this->m_activeColorMapName)
        return;
    QScopedValueRollback<bool> guard(this->m_applyingLinkedUpdate, true);
    this->applyColorMapByName(name);
}
