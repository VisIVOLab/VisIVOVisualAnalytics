// vtkWindowCatalogue3D_VrHeadset.cpp
// Tools ▸ Open in VR Headset… (issue #190): this window's point table or 3-D
// catalogue, backend session and token handed to the WebXR page on a Quest,
// which draws it as points (VrHeadsetDialog, as for a cube).

#include "vtkWindowCatalogue3D.h"

#include "VrHeadsetDialog.h"
#include "app/BackendClient.h"
#include "app/VrLink.h"

void vtkWindowCatalogue3D::openInVrHeadset()
{
    if (m_vrHeadsetDialog) {
        m_vrHeadsetDialog->raise();
        m_vrHeadsetDialog->activateWindow();
        return;
    }
    const QString url = this->backendUrl;
    const QString token = BackendClient::effectiveToken(url, BackendClient(url, this->backendToken).token());
    VrLink::Inputs inputs;
    inputs.baseUrl = url;
    inputs.token = token;
    inputs.sessionId = !this->remoteSessionId.isEmpty() ? this->remoteSessionId
                                                        : BackendClient::sessionIdForUrl(url, token);
    inputs.datasetId = this->remoteDatasetId;
    inputs.path = this->filepath;
    inputs.kind = QStringLiteral("points");
    m_vrHeadsetDialog = new VrHeadsetDialog(inputs, this);
    m_vrHeadsetDialog->show();
}
