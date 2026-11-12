// vtkWindowVectorField_VrHeadset.cpp
// "Open in VR Headset…" (issue #190): this velocity field, backend session and
// token handed to the WebXR page on a Quest, which draws the streamlines and
// arrows the backend traces on the same grid (VrHeadsetDialog, as for a cube).

#include "vtkWindowVectorField.h"

#include "VrHeadsetDialog.h"
#include "app/BackendClient.h"
#include "app/VrLink.h"

#include <algorithm>

void vtkWindowVectorField::openInVrHeadset()
{
    if (m_vrHeadsetDialog) {
        m_vrHeadsetDialog->raise();
        m_vrHeadsetDialog->activateWindow();
        return;
    }
    const QString url = this->m_backendUrl;
    const QString token = BackendClient::effectiveToken(url, BackendClient(url, this->m_backendToken).token());
    VrLink::Inputs inputs;
    inputs.baseUrl = url;
    inputs.remote = !BackendClient::isLocalBackendUrl(url);
    inputs.token = token;
    inputs.sessionId = !this->m_sessionId.isEmpty() ? this->m_sessionId
                                                    : BackendClient::sessionIdForUrl(url, token);
    inputs.datasetId = this->m_datasetId;
    inputs.path = this->m_path;
    inputs.kind = QStringLiteral("velocity");
    // The box the field is drawn with here (0: grid units): the backend traces
    // the headset's lines with it, as this view shows them, and the page opens
    // the path again with it when the session no longer holds the dataset.
    inputs.boxMpc = QString::number(std::max(0.0, this->m_boxMpc), 'g', 12);
    m_vrHeadsetDialog = new VrHeadsetDialog(inputs, this);
    m_vrHeadsetDialog->show();
}
