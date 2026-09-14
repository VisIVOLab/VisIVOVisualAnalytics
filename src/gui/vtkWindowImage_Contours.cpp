#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeViewportGeometry.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "CatalogueTableModel.h"
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
#include "vtkFITSReader.h"
#include <vtkContourFilter.h>
#include <vtkFlyingEdges2D.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderer.h>
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
#include <QFileDialog>
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
// Extracted verbatim from vtkWindowImage.cpp. Contour pipeline + external-FITS contour overlay.

void vtkWindowImage::setupContourPipeline()
{
    auto *imageData = this->layers
            ? this->layers->getImageData(this->layers->getMasterIndex())
            : nullptr;
    if (!imageData) return;

    // The bounds follow the data until the user overrides them: setContoursVisible()
    // rebuilds this pipeline after every preview -> full swap, and re-reading the
    // range then would silently discard the numbers typed in the CONTOURS block.
    double range[2];
    imageData->GetScalarRange(range);
    if (!m_contourBoundsUserSet && std::isfinite(range[0]) && std::isfinite(range[1])
        && range[0] < range[1]) {
        m_contourLower = range[0];
        m_contourUpper = range[1];
    }

    const bool firstTime = !m_contourFilter;
    if (firstTime) {
        m_contourFilter = vtkSmartPointer<vtkFlyingEdges2D>::New();
        m_contourActor  = vtkSmartPointer<vtkActor>::New();
    }

    m_contourFilter->SetInputData(imageData);
    m_contourFilter->GenerateValues(m_contourLevel, m_contourLower, m_contourUpper);
    m_contourFilter->Update();

    if (firstTime) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(m_contourFilter->GetOutputPort());
        mapper->SetScalarRange(m_contourLower, m_contourUpper);
        m_contourActor->SetMapper(mapper);
        m_contourActor->VisibilityOff();

        auto *ren = ui->vtk->renderWindow()
                    ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                    : nullptr;
        if (ren) ren->AddViewProp(m_contourActor);
    } else if (m_contourActor->GetMapper()) {
        static_cast<vtkPolyDataMapper *>(m_contourActor->GetMapper())
                ->SetScalarRange(m_contourLower, m_contourUpper);
    }

    // Sync the sidebar line edits so the user sees the values the filter is
    // actually using, not the stale defaults from construction time.
    syncContourBoundFields();

    m_contourActor->SetVisibility(m_contoursVisible);
    m_contourPipelineDirty = false;
}

void vtkWindowImage::syncContourBoundFields()
{
    if (m_contourLowerEdit) {
        const QSignalBlocker b(m_contourLowerEdit);
        m_contourLowerEdit->setText(QString::number(m_contourLower, 'g', 9));
    }
    if (m_contourUpperEdit) {
        const QSignalBlocker b(m_contourUpperEdit);
        m_contourUpperEdit->setText(QString::number(m_contourUpper, 'g', 9));
    }
}

void vtkWindowImage::commitContourBounds(double lower, double upper)
{
    // Refuse rather than store: setupContourPipeline() feeds these members
    // straight to GenerateValues, so an unusable pair accepted here would come
    // back on the next rebuild even though updateContours() had rejected it.
    if (!std::isfinite(lower) || !std::isfinite(upper) || !(lower < upper)) {
        statusBar()->showMessage(
                u"Contour bounds need Lower < Upper, keeping the previous pair."_s, 4000);
        syncContourBoundFields();
        return;
    }
    m_contourLower = lower;
    m_contourUpper = upper;
    // From here the bounds are the user's, and data updates leave them alone.
    m_contourBoundsUserSet = true;
    updateContours();
}

void vtkWindowImage::updateContours()
{
    // Typing a level before the pipeline exists is not an error: build it, then
    // apply. setupContourPipeline() keeps the bounds already entered.
    if (!m_contourFilter && m_contourPipelineDirty)
        setupContourPipeline();
    if (!m_contourFilter) return;
    if (m_contourLevel < 1 || !(m_contourLower < m_contourUpper)) {
        statusBar()->showMessage(
                u"Contour levels need Lower < Upper and at least one level."_s, 4000);
        return;
    }
    m_contourFilter->GenerateValues(m_contourLevel, m_contourLower, m_contourUpper);
    m_contourFilter->Modified();
    m_contourFilter->Update();
    if (m_contourActor && m_contourActor->GetMapper()) {
        static_cast<vtkPolyDataMapper *>(m_contourActor->GetMapper())
                ->SetScalarRange(m_contourLower, m_contourUpper);
    }
    if (ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
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
    if (m_contourActor) {
        m_contourActor->SetVisibility(vis);
    }
    // The tool's parameters appear with the tool and leave with it.
    if (m_workspaceChrome) {
        if (auto *panel = m_workspaceChrome->inspectorPanel()) {
            if (vis && m_contourSettingsBlock) {
                panel->setToolSettings(u"Contours"_s, m_contourSettingsBlock);
                // …and SHOW it: settings mounted in a tab nobody is looking at
                // are the same as no settings.
                m_workspaceChrome->revealParametersTab();
            } else if (panel->activeToolSettings() == u"Contours"_s) {
                panel->clearToolSettings();
            }
        }
    }
    if (ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::addExternalContours()
{
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                : nullptr;
    if (!ren) return;

    const QString path = QFileDialog::getOpenFileName(
            this, u"Load Contour FITS"_s, QString(),
            u"FITS files (*.fits *.fit);;All files (*)"_s);
    if (path.isEmpty()) return;

    bool ok = false;
    const QString levelsStr = QInputDialog::getText(
            this, u"Contour Levels"_s,
            u"Enter contour levels (comma-separated):"_s,
            QLineEdit::Normal,
            u"0.1, 0.5, 1.0, 2.0, 5.0"_s, &ok);
    if (!ok || levelsStr.trimmed().isEmpty()) return;

    QVector<double> levels;
    for (const auto &s : levelsStr.split(u','))  {
        bool lok = false;
        const double v = s.trimmed().toDouble(&lok);
        if (lok && std::isfinite(v)) levels.append(v);
    }
    if (levels.isEmpty()) return;

    vtkNew<vtkFITSReader> reader;
    reader->SetFileName(path.toStdString().c_str());
    reader->Update();
    auto *extData = reader->GetOutput();
    if (!extData || extData->GetNumberOfPoints() == 0) {
        QMessageBox::warning(this, u"Contour"_s,
                u"Could not read FITS data from:\n%1"_s.arg(path));
        return;
    }

    vtkNew<vtkContourFilter> contour;
    contour->SetInputData(extData);
    contour->SetNumberOfContours(levels.size());
    for (int i = 0; i < levels.size(); ++i)
        contour->SetValue(i, levels[i]);
    contour->Update();

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(contour->GetOutputPort());
    mapper->ScalarVisibilityOff();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(1.0, 0.4, 0.2);
    actor->GetProperty()->SetLineWidth(1.5);
    actor->VisibilityOn();

    ren->AddActor(actor);
    m_externalContourLayers.append({ QFileInfo(path).fileName(), actor });
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::overlayExternalContourData(const QString &label, vtkImageData *data)
{
    if (!data || data->GetNumberOfPoints() == 0) return;
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!ren) return;

    double range[2];
    data->GetScalarRange(range);
    if (range[1] <= range[0]) return;

    constexpr int nLevels = 8;
    vtkNew<vtkFlyingEdges2D> contour;
    contour->SetInputData(data);
    contour->GenerateValues(nLevels, range[0] + (range[1] - range[0]) * 0.1,
                            range[0] + (range[1] - range[0]) * 0.9);
    contour->Update();

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(contour->GetOutputPort());
    mapper->ScalarVisibilityOff();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(0.2, 1.0, 0.4);
    actor->GetProperty()->SetLineWidth(1.5);
    actor->VisibilityOn();

    ren->AddActor(actor);
    m_externalContourLayers.append({ label, actor });
    statusBar()->showMessage(
            QStringLiteral("Contour overlay from cube: %1").arg(label), 3000);
    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::clearAllContours()
{
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer()
                : nullptr;
    for (auto &layer : m_externalContourLayers) {
        if (ren && layer.actor) ren->RemoveActor(layer.actor);
    }
    m_externalContourLayers.clear();
    // Through the action, so the Tools entry and the left dock's checkbox both
    // follow; calling setContoursVisible() directly left them showing "on".
    if (m_actToggleContours)
        m_actToggleContours->setChecked(false);
    else
        setContoursVisible(false);
    if (ren && ui->vtk->renderWindow())
        ui->vtk->renderWindow()->Render();
}
