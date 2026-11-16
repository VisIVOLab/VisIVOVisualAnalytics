// Region IO (CRTF / DS9) on the cube's 2-D view — partial class of vtkWindowCube.
//
// File interop for the region list (vtkWindowCube_RegionList.cpp): its regions
// are saved as a CASA CRTF or DS9 (.reg) file, and a region file is read by
// the backend with the cube's WCS into the list, every region and shape, with
// a report of the lines not read (RegionImport.h, shared with the image
// viewer).
//
// Both directions go through the backend's /v1/regions endpoints (the same that
// the image viewer uses), run off the GUI thread via QFutureWatcher so the
// synchronous BackendClient never blocks the UI or risks a nested-event-loop
// use-after-free if the window is closed mid-request.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "RegionImport.h"
#include "RegionListExport.h"
#include "app/BackendClient.h"

#include <vtkImageData.h>
#include <vtkRenderWindow.h>

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QtConcurrentRun>

#include <array>
#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

void vtkWindowCube::exportRegionsToFile()
{
    // The region list, all or the selected region, as in the image viewer.
    visivo::regions::exportRegionList(
            this, this->m_regions,
            { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
              this->remoteDatasetId, this->filepath });
}

void vtkWindowCube::importRegionsFromFile(const QString &path, const QString &note)
{
    QPointer<vtkWindowCube> self(this);
    visivo::regions::importRegionFile(
            this,
            { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
              this->remoteDatasetId, this->filepath },
            [self](const visivo::regions::ImportResult &result) {
                if (self)
                    self->applyImportedRegions(result);
            },
            path, note);
}

void vtkWindowCube::applyImportedRegions(const visivo::regions::ImportResult &result)
{
    // A drag still running ends here: the import is its own change, after
    // it, not inside its undo step.
    if (m_regionDrag.active) {
        m_regionDrag.active = false;
        this->m_regions.endGesture();
    }
    // Every region of the file joins the list as one change (one Undo takes
    // the import back); the first is selected.
    const std::vector<int> ids = this->m_regions.addAll(result.regions);
    if (ids.empty())
        return;
    this->onRegionListChanged();
    // The first region with an area is measured on the 2-D Slice (a moment
    // pane becomes the slice with that map as background); with neither on
    // screen the regions are kept, unmeasured.
    for (int id : ids) {
        const auto *r = this->m_regions.find(id);
        if (r && visivo::regions::hasArea(r->shape)) {
            if (this->ensureSliceForTools())
                this->analyzeRegion(id);
            break;
        }
    }
}
