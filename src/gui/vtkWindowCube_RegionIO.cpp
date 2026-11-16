// Region IO (CRTF / DS9) on the cube's 2-D view — partial class of vtkWindowCube.
//
// File interop for the region list (vtkWindowCube_RegionList.cpp): its regions
// are saved as a CASA CRTF or DS9 (.reg) file, and a region file is loaded
// into the list and its first region analysed — parity with the image
// viewer's region IO, and with CARTA's region import/export.
//
// Both directions go through the backend's /v1/regions endpoints (the same that
// the image viewer uses), run off the GUI thread via QFutureWatcher so the
// synchronous BackendClient never blocks the UI or risks a nested-event-loop
// use-after-free if the window is closed mid-request.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

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

void vtkWindowCube::importRegionsFromFile()
{
    const QString path = QFileDialog::getOpenFileName(
            this, u"Import Region File"_s, QFileInfo(this->filepath).absolutePath(),
            u"Region files (*.crtf *.reg);;CRTF (*.crtf);;DS9 (*.reg);;All files (*)"_s);
    if (path.isEmpty())
        return;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, u"Import Region"_s, u"Cannot open file:\n%1"_s.arg(path));
        return;
    }
    const QString text = QString::fromUtf8(in.readAll());
    in.close();

    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken,
                  dsId = this->remoteDatasetId;
    this->statusBar()->showMessage(u"Parsing region file…"_s, 0);
    QPointer<vtkWindowCube> self(this);
    auto *watcher = new QFutureWatcher<BackendRegionParseResult>(this);
    QObject::connect(
            watcher, &QFutureWatcher<BackendRegionParseResult>::finished, this,
            [self, watcher]() {
                const BackendRegionParseResult r = watcher->result();
                watcher->deleteLater();
                if (!self)
                    return;
                self->statusBar()->clearMessage();
                if (!r.valid) {
                    QMessageBox::warning(self, u"Import Region"_s,
                                         u"Parse failed:\n%1"_s.arg(r.error));
                    return;
                }
                self->applyImportedRegion(r.regions);
            });
    // Reframe to image pixels (needs the dataset's WCS) so the region maps onto
    // the cube grid regardless of whether the file is in sky or pixel coords.
    watcher->setFuture(QtConcurrent::run(
            [url, token, text, dsId]() -> BackendRegionParseResult {
                BackendClient c(url, token);
                return c.parseRegions(text, u"auto"_s, dsId, u"image"_s);
            }));
}

void vtkWindowCube::applyImportedRegion(const QJsonArray &regions)
{
    if (regions.isEmpty()) {
        QMessageBox::information(this, u"Import Region"_s, u"No regions found in the file."_s);
        return;
    }
    // Every region of the file joins the list, at the backend's sub-pixel
    // position (image frame, 1-based pixels, rotation kept). A shape the list
    // does not hold is counted and named, not dropped silently.
    std::vector<visivo::regions::Region> read;
    QStringList skipped;
    for (const QJsonValue &v : regions) {
        visivo::regions::Region r;
        QString why;
        if (!visivo::regions::fromCanonical(v.toObject(), 1.0, &r, &why)) {
            skipped << why;
            continue;
        }
        r.sourceFrame = u"image"_s;
        read.push_back(r);
    }
    // One change for the whole file: one Undo takes the import back.
    const std::vector<int> ids = this->m_regions.addAll(read);
    const int firstId = ids.empty() ? -1 : ids.front();
    const int added = int(ids.size());
    if (firstId < 0) {
        QMessageBox::warning(this, u"Import Region"_s,
                             u"None of the %1 region(s) in the file could be shown: %2."_s
                                     .arg(regions.size())
                                     .arg(skipped.join(u"; "_s)));
        return;
    }
    this->m_regions.select(firstId);
    this->onRegionListChanged();
    // Measured on the 2-D Slice: a moment pane becomes the slice with that map
    // as background. With neither on screen the regions are kept, unmeasured.
    if (this->ensureSliceForTools())
        this->analyzeRegion(firstId);
    QString msg = u"Imported %1 region(s); the first is selected."_s.arg(added);
    if (!skipped.isEmpty())
        msg += u" %1 not shown: %2."_s.arg(skipped.size()).arg(skipped.first());
    this->statusBar()->showMessage(msg, 10000);
}
