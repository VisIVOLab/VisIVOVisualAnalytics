// vtkWindowCatalogue3D_VrHeadset.cpp
// Tools ▸ Open in VR Headset… (issue #190): this window's point table or 3-D
// catalogue, backend session and token handed to the WebXR page on a Quest,
// which draws it as points (VrHeadsetDialog, as for a cube). A sky catalogue
// goes with the window's frame, distance mode and cosmology, so the headset
// places its sources where this window does (issue #260).

#include "vtkWindowCatalogue3D.h"

#include "VrHeadsetDialog.h"
#include "app/BackendClient.h"
#include "app/VrLink.h"

#include <QTimer>

using namespace Qt::StringLiterals;

VrLink::Sky vtkWindowCatalogue3D::vrSky() const
{
    // A VBT and a Cartesian catalogue carry their own positions.
    if (this->m_pointTable || this->schema.isCartesian())
        return {};
    return this->headsetSky().sky;
}

CataloguePlacement::HeadsetSky vtkWindowCatalogue3D::headsetSky() const
{
    // The model the rows on screen were placed with, not the selector while a
    // request is out. Custom never goes: the headset's points come from the
    // backend, which knows the presets only.
    const QString frame = this->activeFrame == CoordFrame::Galactic ? u"galactic"_s
            : this->activeFrame == CoordFrame::Supergalactic ? u"supergalactic"_s
                                                             : u"fk5"_s;
    return CataloguePlacement::headsetSky(
            frame, this->schema.distanceMode == CatalogueDistanceMode::RedshiftSpace,
            this->m_appliedCosmologyModel);
}

void vtkWindowCatalogue3D::pushSkyToVrDialog()
{
    if (!m_vrHeadsetDialog || this->m_pointTable || this->schema.isCartesian())
        return;
    if (this->headsetSky().refuseCustom) {
        // Custom chosen while the headset window is open: the window goes back
        // to Planck18 and says why, as for the ray-traced view, rather than
        // the headset showing Planck18 in silence. Queued: this runs at the
        // end of a scene rebuild, which the revert rebuilds again. The dialog
        // keeps the placement it had until then.
        QTimer::singleShot(0, this, [this]() {
            this->dropCustomCosmologyForServerView(u"The headset view"_s);
        });
        return;
    }
    m_vrHeadsetDialog->setSky(this->vrSky());
}

void vtkWindowCatalogue3D::openInVrHeadset()
{
    // As for the ray-traced view: the backend draws these points and has no
    // custom model, so the selector goes back to Planck18, and says so.
    if (!this->m_pointTable && !this->schema.isCartesian())
        this->dropCustomCosmologyForServerView(u"The headset view"_s);
    if (m_vrHeadsetDialog) {
        m_vrHeadsetDialog->setSky(this->vrSky());
        m_vrHeadsetDialog->raise();
        m_vrHeadsetDialog->activateWindow();
        return;
    }
    const QString url = this->backendUrl;
    const QString token = BackendClient::effectiveToken(url, BackendClient(url, this->backendToken).token());
    VrLink::Inputs inputs;
    inputs.baseUrl = url;
    inputs.remote = !BackendClient::isLocalBackendUrl(url);
    inputs.token = token;
    inputs.sessionId = !this->remoteSessionId.isEmpty() ? this->remoteSessionId
                                                        : BackendClient::sessionIdForUrl(url, token);
    inputs.datasetId = this->remoteDatasetId;
    inputs.path = this->filepath;
    inputs.kind = QStringLiteral("points");
    inputs.sky = this->vrSky();
    m_vrHeadsetDialog = new VrHeadsetDialog(inputs, this);
    m_vrHeadsetDialog->show();
}
