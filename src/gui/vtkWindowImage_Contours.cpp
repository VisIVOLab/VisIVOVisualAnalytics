#include "vtkWindowImage.h"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <vtkTransform.h>
#include "ui_vtkWindowImage.h"
#include "CubeViewportGeometry.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "CatalogueTableModel.h"
#include "ContourExport.h"
#include "ContourSettingsPanel.h"
#include "ColorMaps.h"
#include "CatalogueOverlayUtils.h"
#include "DiagnosticsWindow.h"
#include "ImageLayerController.h"
#include "InspectorPanel.h"
#include "WorkspaceChrome.h"
#include "ImageLayerImportService.h"
#include "ImageLayerLoadTask.h"
#include "LUTCustomizerDialog.h"
#include "LayerListModel.h"
#include "ProfileWidget.h"
#include "RemoteFileBrowserDialog.h"
#include "qcustomplot.h"
#include "SidebarPanel.h"
#include "theme/SegmentedToggle.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"
#include "ToolResultDialog.h"
#include "vtkInteractorStyleProfile.h"
#include "vtkLegendScaleActorWCS.h"
#include "wcs.h"

#include <vtkCamera.h>
#include <vtkAxisActor2D.h>
#include <vtkContourTriangulator.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageSlice.h>
#include <vtkImageStack.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleUser.h>
#include <vtkLineSource.h>
#include <vtkLookupTable.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkPolyData.h>
#include <vtkDataArray.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDebug>
#include <QKeySequence>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QPlainTextEdit>
#include <QDialog>
#include <QSlider>
#include <QStringList>
#include <QToolBar>
#include <QToolButton>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

using namespace Qt::StringLiterals;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)


// ── Contours ──
// The image's own contours and those of other files are ContourLayers
// (ContourLayer.h) drawn with the settings of ContourSettingsPanel: levels by
// count, sigma, logarithm or list, an optional smoothing, colours, width and
// dashed negative levels (issue #519).

namespace vc = visivo::contours;

namespace {

/// Two bounds that are the same number as far as typing is concerned: the panel
/// shows nine digits, so an untouched bound comes back slightly rounded.
bool sameBound(double a, double b)
{
    return std::abs(a - b) <= 1e-6 * std::max({ std::abs(a), std::abs(b), 1e-30 });
}

vtkRenderer *rendererOf(vtkRenderWindow *win)
{
    return win ? win->GetRenderers()->GetFirstRenderer() : nullptr;
}

}   // namespace

ContourLayer *vtkWindowImage::ownContourLayer()
{
    if (!m_contourLayer) {
        m_contourLayer = std::make_unique<ContourLayer>();
        m_contourLayer->attach(rendererOf(ui->vtk->renderWindow()));
        m_contourLayer->setPixelScale(1.0, contourArcsecPerPixel());
    }
    return m_contourLayer.get();
}

ContourLayer *vtkWindowImage::editedContourLayer()
{
    const int at = m_contourTarget ? m_contourTarget->currentIndex() : 0;
    if (at <= 0 || at > m_externalContourLayers.size())
        return ownContourLayer();
    return m_externalContourLayers.at(at - 1).layer.get();
}

vtkImageData *vtkWindowImage::contourDataOf(const ContourLayer *layer) const
{
    if (layer == m_contourLayer.get()) {
        return this->layers ? this->layers->getImageData(this->layers->getMasterIndex())
                            : nullptr;
    }
    for (const auto &ext : m_externalContourLayers)
        if (ext.layer.get() == layer)
            return ext.data;
    return nullptr;
}

double vtkWindowImage::contourArcsecPerPixel() const
{
    // The sky size of a pixel along the latitude axis, which is never stretched
    // by the projection.
    const double deg = std::abs(this->remoteDatasetCdelt[1]);
    return std::isfinite(deg) ? deg * 3600.0 : 0.0;
}

void vtkWindowImage::setupContourPipeline()
{
    auto *imageData = this->layers
            ? this->layers->getImageData(this->layers->getMasterIndex())
            : nullptr;
    if (!imageData) return;

    ContourLayer *layer = ownContourLayer();
    vc::Settings s = layer->settings();

    // The bounds follow the data until the user overrides them: setContoursVisible()
    // rebuilds this pipeline after every preview -> full swap, and re-reading the
    // range then would silently discard the numbers typed in the panel.
    double range[2];
    imageData->GetScalarRange(range);
    if (!m_contourBoundsUserSet && std::isfinite(range[0]) && std::isfinite(range[1])
        && range[0] < range[1]) {
        s.lower = range[0];
        s.upper = range[1];
    }
    // Levels in sigma need a noise: start from the pixels' own.
    if (!(s.sigma > 0.0))
        s.sigma = ContourLayer::noiseOf(imageData);

    layer->setInputData(imageData);
    layer->setPixelScale(1.0, contourArcsecPerPixel());
    const QString problem = layer->setSettings(s);
    layer->setVisible(m_contoursVisible);
    // The panel shows the values the layer is actually using, not the stale
    // defaults from construction time.
    if (m_contourPanel && editedContourLayer() == layer) {
        m_contourPanel->setSettings(s);
        m_contourPanel->setProblem(problem);
    }
    m_contourPipelineDirty = false;
}

void vtkWindowImage::onContourPanelChanged()
{
    if (!m_contourPanel)
        return;
    // Typing before the pipeline exists is not an error: build it, then apply.
    if (m_contourPipelineDirty && m_contoursVisible && m_contourTarget
        && m_contourTarget->currentIndex() <= 0)
        setupContourPipeline();
    ContourLayer *layer = editedContourLayer();
    if (!layer)
        return;
    vc::Settings s = m_contourPanel->settings();
    const vc::Settings before = layer->settings();
    const bool own = layer == m_contourLayer.get();
    // From here the bounds are the user's, and data updates leave them alone.
    if (own && (!sameBound(s.lower, before.lower) || !sameBound(s.upper, before.upper)))
        m_contourBoundsUserSet = true;
    if (s.levelMode == vc::LevelMode::Sigma && !(s.sigma > 0.0)) {
        if (vtkImageData *data = contourDataOf(layer)) {
            s.sigma = ContourLayer::noiseOf(data);
            m_contourPanel->setSettings(s);
        }
    }
    const QString problem = layer->setSettings(s);
    m_contourPanel->setProblem(problem);
    if (ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::estimateContourNoise()
{
    ContourLayer *layer = editedContourLayer();
    if (!layer || !m_contourPanel)
        return;
    const bool own = layer == m_contourLayer.get();
    vtkImageData *data = contourDataOf(layer);
    // The result may land after the layer is gone (Clear All Contours): look it
    // up again by address before touching it.
    const auto apply = [this, layer](double sigma, const QString &how) {
        const bool known = layer == m_contourLayer.get()
                || std::any_of(m_externalContourLayers.begin(), m_externalContourLayers.end(),
                               [layer](const ExternalContourLayer &e) { return e.layer.get() == layer; });
        if (!known)
            return;
        if (!(sigma > 0.0)) {
            statusBar()->showMessage(u"The noise could not be measured: type it."_s, 6000);
            return;
        }
        vc::Settings s = layer->settings();
        const bool shown = m_contourPanel && editedContourLayer() == layer;
        // The panel may hold edits not applied yet.
        if (shown)
            s = m_contourPanel->settings();
        s.sigma = sigma;
        if (shown)
            m_contourPanel->setSettings(s);
        const QString problem = layer->setSettings(s);
        if (shown)
            m_contourPanel->setProblem(problem);
        statusBar()->showMessage(u"Noise σ = %1 (%2)"_s.arg(sigma, 0, 'g', 6).arg(how), 8000);
        if (ui->vtk->renderWindow())
            ui->vtk->renderWindow()->Render();
    };

    // The image's own noise is the one Tools > Estimate Noise reports: the
    // backend's sigma-clipped RMS of the whole image, so that 3, 5, 10 sigma
    // match it. Without a backend dataset (or for another file's pixels) it is
    // 1.4826 x the median absolute deviation of what is on screen.
    if (own && !this->remoteDatasetId.isEmpty()) {
        statusBar()->showMessage(u"Estimating the noise of the image…"_s, 0);
        auto *watcher = new QFutureWatcher<BackendImageNoiseResult>(this);
        QObject::connect(watcher, &QFutureWatcherBase::finished, this,
                         [this, watcher, apply]() {
            watcher->deleteLater();
            const BackendImageNoiseResult r = watcher->result();
            statusBar()->clearMessage();
            if (r.valid && r.rms > 0.0) {
                apply(r.rms, u"sigma-clipped RMS of the whole image"_s);
            } else {
                vtkImageData *shown = this->layers
                        ? this->layers->getImageData(this->layers->getMasterIndex())
                        : nullptr;
                apply(shown ? ContourLayer::noiseOf(shown) : 0.0,
                      u"1.4826 × MAD of the pixels shown"_s);
            }
        });
        const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
        const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
        watcher->setFuture(QtConcurrent::run([url, token, session, dataset]() {
            BackendClient client(url, token);
            client.setSessionId(session);
            return client.imageNoise(dataset, QJsonArray());
        }));
        return;
    }
    apply(data ? ContourLayer::noiseOf(data) : 0.0, u"1.4826 × MAD of the pixels"_s);
}

void vtkWindowImage::exportContours()
{
    // A hidden layer may still hold the preview the full image replaced.
    if (m_contourPipelineDirty && (!m_contourTarget || m_contourTarget->currentIndex() <= 0))
        setupContourPipeline();
    ContourLayer *layer = editedContourLayer();
    if (!layer || !layer->hasInput()) {
        statusBar()->showMessage(u"Show the contours first: there is nothing to export."_s, 5000);
        return;
    }
    // The world of this window is the image's pixels (0-based), as the regions'.
    const auto lines = layer->lines(
            [](double x, double y) { return std::array<double, 2>{ x, y }; });
    const QString frame = ui->actionGalactic->isChecked()
            ? u"galactic"_s
            : (ui->actionEcliptic->isChecked() ? u"ecliptic"_s : u"fk5"_s);
    vc::exportContours(this, lines, layer->settings().lineWidth,
                       { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
                         this->remoteDatasetId, this->filepath },
                       frame);
}

void vtkWindowImage::setContoursVisible(bool vis)
{
    // Lazy pipeline rebuild: applyRemoteMasterLayer skips setupContourPipeline
    // on the preview→full swap when contours are invisible, so we have to
    // wire the filter to the current master image data the first time the
    // user actually turns contours on. setupContourPipeline clears the dirty
    // flag, so subsequent toggles are cheap (just SetVisibility + Render).
    if (vis && m_contourPipelineDirty) {
        setupContourPipeline();
    }
    m_contoursVisible = vis;
    if (m_contourLayer)
        m_contourLayer->setVisible(vis);
    // The tool's parameters appear with the tool and leave with it.
    // The panel follows the tools that are on: this one claims it while it is
    // shown (and the tab is revealed, or the settings would appear where nobody
    // is looking), and hands it back to whichever tool is still on when it goes.
    // Contours of other files keep it: their style is edited there too.
    if (vis && m_contourSettingsBlock) {
        refreshContourTargets();
        claimToolSettings(u"Contours"_s, m_contourSettingsBlock);
    } else if (m_externalContourLayers.isEmpty()) {
        releaseToolSettings(u"Contours"_s);
        refreshToolSettingsPanel(/*reveal=*/false);
    }
    if (ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::refreshContourTargets()
{
    if (!m_contourTarget)
        return;
    const int keep = m_contourTarget->currentIndex();
    {
        const QSignalBlocker b(m_contourTarget);
        m_contourTarget->clear();
        m_contourTarget->addItem(u"This image"_s);
        for (const auto &ext : std::as_const(m_externalContourLayers))
            m_contourTarget->addItem(ext.label);
        m_contourTarget->setCurrentIndex(
                std::clamp(keep, 0, int(m_externalContourLayers.size())));
    }
    if (m_contourTargetRow)
        m_contourTargetRow->setVisible(!m_externalContourLayers.isEmpty());
    // The panel shows the layer that is picked.
    if (ContourLayer *layer = editedContourLayer(); layer && m_contourPanel) {
        m_contourPanel->setSettings(layer->settings());
        m_contourPanel->setProblem(QString());
    }
}

void vtkWindowImage::addExternalContours()
{
    if (!ui->vtk->renderWindow() || !ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer())
        return;

    // The same picker and the same alignment as Add Image Layer: the file is a
    // backend path, and its contours are drawn where its WCS puts it on this
    // image. They used to be drawn from the raw pixel grid, at the master's
    // pixel (0, 0) and at the other file's pixel scale (issue #500).
    RemoteFileBrowserDialog dialog(this->remoteBackendUrl, this->remoteBackendToken, this);
    dialog.setPurpose(u"Load Contour from FITS"_s);
    dialog.setFileTypes({ visivo::FileType::Fits }, visivo::FileType::Fits, u"image-layer"_s);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString path = dialog.selectedFilePath();
    if (path.isEmpty())
        return;

    this->showPersistentStatusMessage(
            u"Loading contours of %1..."_s.arg(QFileInfo(path).fileName()));
    auto *watcher = new QFutureWatcher<ImageLayerLoadResult>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path]() {
        watcher->deleteLater();
        this->statusBar()->clearMessage();
        const ImageLayerLoadResult result = watcher->result();
        if (!result.valid || !result.imageData) {
            QMessageBox::warning(this, u"Contour"_s,
                                 u"No contours from %1:\n%2"_s.arg(
                                         QFileInfo(path).fileName(),
                                         QString::fromStdString(result.errorMessage)));
            return;
        }
        this->applyExternalContours(QFileInfo(path).fileName(), result);
    });
    watcher->setFuture(QtConcurrent::run(
            &vtkWindowImage::loadAlignedLayer, this->layerGoesThroughBackend(path),
            this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId, path,
            this->filepath, false));
}

void vtkWindowImage::applyExternalContours(const QString &label,
                                           const ImageLayerLoadResult &result)
{
    auto *ren = rendererOf(ui->vtk->renderWindow());
    if (!ren || !result.imageData)
        return;
    // Placed as a layer is: the image in master pixels (its own decimation
    // and the WCS alignment in spacing and origin), turned about its origin.
    vtkImageData *data = result.imageData;
    data->SetSpacing(result.spacing[0], result.spacing[1], result.spacing[2]);
    data->SetOrigin(result.origin[0], result.origin[1], result.origin[2]);

    auto layer = std::make_shared<ContourLayer>();
    layer->attach(ren);
    vtkNew<vtkTransform> transform;
    transform->Translate(result.origin[0], result.origin[1], 0.);
    transform->RotateWXYZ(result.rotationDegrees, 0., 0., 1.);
    transform->Translate(-result.origin[0], -result.origin[1], 0.);
    layer->setUserTransform(transform);
    layer->setInputData(data);
    layer->setPixelScale(1.0, contourArcsecPerPixel());

    // Radio contours are multiples of the noise: start from 3, 5, 10 and 20
    // sigma of this file's pixels, in one colour; the panel changes all of it.
    vc::Settings s;
    s.levelMode = vc::LevelMode::Sigma;
    s.sigma = ContourLayer::noiseOf(data);
    s.colourMode = vc::ColourMode::Single;
    double range[2];
    data->GetScalarRange(range);
    if (std::isfinite(range[0]) && std::isfinite(range[1]) && range[0] < range[1]) {
        s.lower = range[0];
        s.upper = range[1];
    }
    const QString problem = layer->setSettings(s);
    layer->setVisible(true);

    m_externalContourLayers.append({ label, layer, data });
    if (m_contourTarget) {
        // Show the new layer's settings, and the panel itself: the file was
        // loaded to be styled.
        refreshContourTargets();
        const QSignalBlocker b(m_contourTarget);
        m_contourTarget->setCurrentIndex(int(m_externalContourLayers.size()));
        if (m_contourPanel) {
            m_contourPanel->setSettings(s);
            m_contourPanel->setProblem(problem);
        }
    }
    if (m_contourSettingsBlock)
        claimToolSettings(u"Contours"_s, m_contourSettingsBlock);
    if (!problem.isEmpty())
        statusBar()->showMessage(problem, 6000);
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::overlayExternalContourData(const QString &label, vtkImageData *data)
{
    if (!data || data->GetNumberOfPoints() == 0) return;
    auto *ren = rendererOf(ui->vtk->renderWindow());
    if (!ren) return;

    double range[2];
    data->GetScalarRange(range);
    if (range[1] <= range[0]) return;

    // Eight levels between a tenth and nine tenths of the range, in green: the
    // cube's current slice, which is on its own grid and so is not aligned.
    vc::Settings s;
    s.levelMode = vc::LevelMode::Count;
    s.count = 8;
    s.lower = range[0] + (range[1] - range[0]) * 0.1;
    s.upper = range[0] + (range[1] - range[0]) * 0.9;
    s.colourMode = vc::ColourMode::Single;
    s.colour = u"#33ff66"_s;
    auto layer = std::make_shared<ContourLayer>();
    layer->attach(ren);
    layer->setInputData(data);
    layer->setPixelScale(1.0, contourArcsecPerPixel());
    layer->setSettings(s);
    layer->setVisible(true);

    m_externalContourLayers.append({ label, layer, data });
    refreshContourTargets();
    statusBar()->showMessage(
            QStringLiteral("Contour overlay from cube: %1").arg(label), 3000);
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::clearAllContours()
{
    m_externalContourLayers.clear();   // the layers take their actors off with them
    refreshContourTargets();
    // Through the action, so the Tools entry and the left dock's checkbox both
    // follow; calling setContoursVisible() directly left them showing "on".
    if (m_actToggleContours)
        m_actToggleContours->setChecked(false);
    else
        setContoursVisible(false);
    // With the own contours already off nothing was toggled, and the settings
    // of the files just cleared would stay in the Parameters tab.
    if (!m_contoursVisible) {
        releaseToolSettings(u"Contours"_s);
        refreshToolSettingsPanel(/*reveal=*/false);
    }
    if (ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}
