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
    // The identity this window's requests actually carry (vrHeadsetInputs):
    // the headset's page then lands in the same session and sees the same
    // datasets, which a strict multi-user backend requires. A remote backend
    // (a cluster's, through a local tunnel) is said so (issue #187).
    const VrLink::Inputs inputs = this->vrHeadsetInputs();
    m_vrHeadsetDialog = new VrHeadsetDialog(inputs, this);
    m_vrHeadsetDialog->show();
    // From now on the headset's selections appear here (issue #188).
    this->startSelectionShare();
}
