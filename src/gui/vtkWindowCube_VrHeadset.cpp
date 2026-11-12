// vtkWindowCube_VrHeadset.cpp
// Tools ▸ Views ▸ Open in VR Headset… (issue #172): hand this window's cube,
// backend session and token to the WebXR page on a Quest (VrHeadsetDialog).

#include "vtkWindowCube.h"

#include "VrHeadsetDialog.h"
#include "app/BackendClient.h"
#include "app/VrLink.h"

void vtkWindowCube::openInVrHeadset()
{
    if (m_vrHeadsetDialog) {
        m_vrHeadsetDialog->raise();
        m_vrHeadsetDialog->activateWindow();
        return;
    }
    // The identity this window's requests actually carry: the token resolved
    // the way BackendClient resolves it (the local backend's token file when
    // the window was given none, a rotation the session mint recorded), and the
    // window's session or else the one minted for this backend. The headset's
    // page then lands in the same session and sees the same datasets, which a
    // strict multi-user backend requires.
    const QString url = this->remoteBackendUrl;
    const QString token = BackendClient::effectiveToken(url, BackendClient(url, this->remoteBackendToken).token());
    VrLink::Inputs inputs;
    inputs.baseUrl = url;
    inputs.token = token;
    inputs.sessionId = !this->remoteSessionId.isEmpty() ? this->remoteSessionId
                                                        : BackendClient::sessionIdForUrl(url, token);
    inputs.datasetId = this->remoteDatasetId;
    inputs.path = this->filepath;
    // Remote as BackendClient decides it (a cluster's backend through a local
    // tunnel): the dialog says so, the link is the same (issue #187).
    inputs.remote = !BackendClient::isLocalBackendUrl(url);
    m_vrHeadsetDialog = new VrHeadsetDialog(inputs, this);
    m_vrHeadsetDialog->show();
    // From now on the headset's selections appear here (issue #188).
    this->startSelectionShare();
}
