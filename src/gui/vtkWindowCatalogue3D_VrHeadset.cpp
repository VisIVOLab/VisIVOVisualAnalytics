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

using namespace Qt::StringLiterals;

VrLink::Sky vtkWindowCatalogue3D::vrSky() const
{
    // A VBT and a Cartesian catalogue carry their own positions.
    if (this->m_pointTable || this->schema.isCartesian())
        return {};
    VrLink::Sky sky;
    sky.frame = this->activeFrame == CoordFrame::Galactic ? u"galactic"_s
            : this->activeFrame == CoordFrame::Supergalactic ? u"supergalactic"_s
                                                             : u"fk5"_s;
    sky.distanceMode = this->schema.distanceMode == CatalogueDistanceMode::RedshiftSpace
            ? u"redshift_space"_s
            : u"comoving"_s;
    // The model the rows on screen were placed with, not the selector while a
    // request is out. Custom never goes: the headset's points come from the
    // backend, which knows the presets only (openInVrHeadset drops it first).
    sky.cosmology = this->m_appliedCosmologyModel.isEmpty()
                    || this->m_appliedCosmologyModel == visivo::cosmology::customName()
            ? u"Planck18"_s
            : this->m_appliedCosmologyModel;
    return sky;
}

void vtkWindowCatalogue3D::pushSkyToVrDialog()
{
    if (m_vrHeadsetDialog)
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
