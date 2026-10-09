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

// ── Annotations ──
// Extracted verbatim from vtkWindowImage.cpp. Text/arrow annotations, JSON save/load, blink.

namespace {
void buildArrowGeometry(vtkPoints *points, vtkCellArray *lines,
                        double x1, double y1, double x2, double y2)
{
    points->Reset();
    lines->Reset();

    // Shaft: from label pos (x1,y1) to tip (x2,y2)
    points->InsertNextPoint(x1, y1, 0.0); // 0
    points->InsertNextPoint(x2, y2, 0.0); // 1
    lines->InsertNextCell(2);
    lines->InsertCellPoint(0);
    lines->InsertCellPoint(1);

    // Arrowhead at (x2, y2) — fixed size, never larger than 30% of shaft
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len > 1e-6) {
        constexpr double fixedHeadLen = 8.0;
        const double headLen = std::min(fixedHeadLen, len * 0.3);
        const double ndx = dx / len;
        const double ndy = dy / len;
        const double px = -ndy;
        const double py = ndx;
        const double headWidth = headLen * 0.4;
        const double bx = x2 - ndx * headLen;
        const double by = y2 - ndy * headLen;

        points->InsertNextPoint(bx + px * headWidth, by + py * headWidth, 0.0); // 2
        points->InsertNextPoint(bx - px * headWidth, by - py * headWidth, 0.0); // 3

        lines->InsertNextCell(2);
        lines->InsertCellPoint(1);
        lines->InsertCellPoint(2);

        lines->InsertNextCell(2);
        lines->InsertCellPoint(1);
        lines->InsertCellPoint(3);
    }
}
}  // namespace

void vtkWindowImage::addTextAnnotation()
{
    bool ok = false;
    const QString text = QInputDialog::getText(
            this, u"Add Text Annotation"_s, u"Text:"_s, QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty()) return;

    m_pendingAnnotText = text.trimmed();
    m_annotPlaceMode = AnnotPlaceMode::Text;
    ui->vtk->setCursor(Qt::CrossCursor);
    statusBar()->showMessage(u"Click on the image to place the text annotation."_s, 0);
    this->syncModeStrip();
}

void vtkWindowImage::addArrowAnnotation()
{
    bool ok = false;
    const QString text = QInputDialog::getText(
            this, u"Add Arrow Annotation"_s, u"Label (optional):"_s,
            QLineEdit::Normal, QString(), &ok);
    if (!ok) return;

    m_pendingAnnotText = text.trimmed();
    m_annotPlaceMode = AnnotPlaceMode::ArrowTip;
    this->syncModeStrip();
    ui->vtk->setCursor(Qt::CrossCursor);
    statusBar()->showMessage(u"Click on the image to place the arrow tip."_s, 0);
}

void vtkWindowImage::placeAnnotationAt(double worldX, double worldY)
{
    switch (m_annotPlaceMode) {
    case AnnotPlaceMode::Text: {
        Annotation ann;
        ann.type = Annotation::Text;
        ann.text = m_pendingAnnotText;
        ann.worldX = worldX;
        ann.worldY = worldY;
        m_annotations.push_back(ann);
        cancelAnnotationPlacement();
        refreshAnnotationOverlay();
        break;
    }
    case AnnotPlaceMode::ArrowTip:
        m_pendingArrowTipX = worldX;
        m_pendingArrowTipY = worldY;
        m_annotPlaceMode = AnnotPlaceMode::ArrowLabel;
        updateAnnotationPreview(worldX, worldY);
        statusBar()->showMessage(u"Move mouse to see the arrow, click to place the label."_s, 0);
        break;
    case AnnotPlaceMode::ArrowLabel: {
        Annotation ann;
        ann.type = Annotation::Arrow;
        ann.text = m_pendingAnnotText;
        ann.worldX = worldX;
        ann.worldY = worldY;
        ann.arrowEndX = m_pendingArrowTipX;
        ann.arrowEndY = m_pendingArrowTipY;
        m_annotations.push_back(ann);
        cancelAnnotationPlacement();
        refreshAnnotationOverlay();
        break;
    }
    default:
        break;
    }
}

void vtkWindowImage::updateAnnotationPreview(double worldX, double worldY)
{
    if (m_annotPlaceMode != AnnotPlaceMode::ArrowLabel) {
        m_annotPreviewActor->VisibilityOff();
        return;
    }

    vtkNew<vtkPoints> pts;
    vtkNew<vtkCellArray> cells;
    buildArrowGeometry(pts, cells, worldX, worldY, m_pendingArrowTipX, m_pendingArrowTipY);
    m_annotPreviewData->SetPoints(pts);
    m_annotPreviewData->SetLines(cells);
    m_annotPreviewData->Modified();
    m_annotPreviewActor->VisibilityOn();
}

void vtkWindowImage::cancelAnnotationPlacement()
{
    m_annotPlaceMode = AnnotPlaceMode::None;
    this->syncModeStrip();
    m_pendingAnnotText.clear();
    m_annotPreviewActor->VisibilityOff();
    const bool needsCross = this->probeModeActive
            || this->regionMode != RegionMode::None
            || this->m_measMode != MeasurementMode::None;
    ui->vtk->setCursor(needsCross ? Qt::CrossCursor : Qt::ArrowCursor);
    statusBar()->clearMessage();
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::clearAnnotations()
{
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (ren) {
        for (auto &a : m_annotTextActors) if (a) ren->RemoveViewProp(a);
        for (auto &a : m_annotArrowActors) if (a) ren->RemoveActor(a);
    }
    m_annotations.clear();
    m_annotTextActors.clear();
    m_annotArrowActors.clear();
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::refreshAnnotationOverlay()
{
    auto *ren = ui->vtk->renderWindow()
                ? ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!ren) return;

    for (auto &a : m_annotTextActors) if (a) ren->RemoveViewProp(a);
    for (auto &a : m_annotArrowActors) if (a) ren->RemoveActor(a);
    m_annotTextActors.clear();
    m_annotArrowActors.clear();

    for (const auto &ann : m_annotations) {
        auto textActor = vtkSmartPointer<vtkTextActor>::New();
        textActor->SetInput(ann.text.toStdString().c_str());
        textActor->GetTextProperty()->SetColor(1.0, 1.0, 0.3);
        textActor->GetTextProperty()->SetFontSize(14);
        textActor->GetTextProperty()->SetBold(true);
        textActor->GetTextProperty()->SetShadow(true);
        textActor->GetPositionCoordinate()->SetCoordinateSystemToWorld();
        textActor->SetPosition(ann.worldX, ann.worldY);
        ren->AddViewProp(textActor);
        m_annotTextActors.push_back(textActor);

        if (ann.type == Annotation::Arrow) {
            vtkNew<vtkPoints> pts;
            vtkNew<vtkCellArray> cells;
            buildArrowGeometry(pts, cells,
                               ann.worldX, ann.worldY,
                               ann.arrowEndX, ann.arrowEndY);
            vtkNew<vtkPolyData> arrowData;
            arrowData->SetPoints(pts);
            arrowData->SetLines(cells);
            vtkNew<vtkPolyDataMapper> mapper;
            mapper->SetInputData(arrowData);
            auto arrowActor = vtkSmartPointer<vtkActor>::New();
            arrowActor->SetMapper(mapper);
            arrowActor->GetProperty()->SetColor(1.0, 1.0, 0.3);
            arrowActor->GetProperty()->SetLineWidth(2.0);
            ren->AddActor(arrowActor);
            m_annotArrowActors.push_back(arrowActor);
        } else {
            m_annotArrowActors.push_back(nullptr);
        }
    }

    // The actors are rebuilt from scratch here, so the visibility toggle has to
    // be re-applied: otherwise hiding the annotations and then adding one shows
    // the whole set again.
    if (!m_annotationsVisible) {
        for (auto &actor : m_annotTextActors)
            if (actor) actor->SetVisibility(0);
        for (auto &actor : m_annotArrowActors)
            if (actor) actor->SetVisibility(0);
    }
    this->refreshOverlaysBlock();

    ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::saveAnnotationsJson()
{
    if (m_annotations.empty()) {
        statusBar()->showMessage(u"No annotations to save."_s, 2000);
        return;
    }

    const QString defaultPath = annotationsJsonPath();
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save Annotations"_s,
            defaultPath.isEmpty() ? QDir::homePath() : defaultPath,
            u"JSON files (*.json);;All files (*)"_s);
    if (path.isEmpty()) return;

    QJsonArray arr;
    for (const auto &ann : m_annotations) {
        QJsonObject obj;
        obj[u"type"_s] = (ann.type == Annotation::Arrow) ? u"arrow"_s : u"text"_s;
        obj[u"text"_s] = ann.text;
        obj[u"x"_s] = ann.worldX;
        obj[u"y"_s] = ann.worldY;
        if (ann.type == Annotation::Arrow) {
            obj[u"arrowEndX"_s] = ann.arrowEndX;
            obj[u"arrowEndY"_s] = ann.arrowEndY;
        }
        arr.append(obj);
    }

    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        statusBar()->showMessage(
                QStringLiteral("Saved %1 annotations to %2")
                        .arg(m_annotations.size())
                        .arg(QFileInfo(path).fileName()),
                3000);
    } else {
        QMessageBox::warning(this, u"Save Annotations"_s,
                u"Could not write to:\n%1"_s.arg(path));
    }
}

void vtkWindowImage::loadAnnotationsJson()
{
    const QString defaultDir = filepath.isEmpty()
            ? QDir::homePath()
            : QFileInfo(filepath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
            this, u"Load Annotations"_s, defaultDir,
            u"JSON files (*.json);;All files (*)"_s);
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, u"Load Annotations"_s,
                u"Could not read:\n%1"_s.arg(path));
        return;
    }

    const QJsonArray arr = QJsonDocument::fromJson(file.readAll()).array();
    if (arr.isEmpty()) {
        statusBar()->showMessage(u"No annotations found in file."_s, 2000);
        return;
    }

    clearAnnotations();
    for (const auto &val : arr) {
        const QJsonObject obj = val.toObject();
        Annotation ann;
        ann.type = obj[u"type"_s].toString() == u"arrow"_s ? Annotation::Arrow : Annotation::Text;
        ann.text = obj[u"text"_s].toString();
        ann.worldX = obj[u"x"_s].toDouble();
        ann.worldY = obj[u"y"_s].toDouble();
        ann.arrowEndX = obj[u"arrowEndX"_s].toDouble();
        ann.arrowEndY = obj[u"arrowEndY"_s].toDouble();
        m_annotations.push_back(ann);
    }
    refreshAnnotationOverlay();
    statusBar()->showMessage(
            QStringLiteral("Loaded %1 annotations from %2")
                    .arg(m_annotations.size())
                    .arg(QFileInfo(path).fileName()),
                3000);
}

QString vtkWindowImage::annotationsJsonPath() const
{
    if (filepath.isEmpty()) return {};
    return filepath + u".annotations.json"_s;
}

void vtkWindowImage::toggleBlink(bool active)
{
    if (!this->layers || this->layers->rowCount() < 2) {
        if (active) {
            statusBar()->showMessage(u"Blink requires at least two layers."_s, 2000);
            if (m_actBlink) m_actBlink->setChecked(false);
        }
        return;
    }

    if (active) {
        m_blinkState = false;
        m_blinkTimer.start(m_blinkIntervalMs);
    } else {
        m_blinkTimer.stop();
        for (int i = 0; i < this->layers->rowCount(); ++i) {
            auto *actor = this->layers->getLayerActor(i);
            if (actor) actor->SetVisibility(true);
        }
        vtkRender();
    }
}

void vtkWindowImage::blinkTick()
{
    if (!this->layers || this->layers->rowCount() < 2) {
        m_blinkTimer.stop();
        return;
    }

    m_blinkState = !m_blinkState;
    for (int i = 0; i < this->layers->rowCount(); ++i) {
        auto *actor = this->layers->getLayerActor(i);
        if (!actor) continue;
        if (i == 0)
            actor->SetVisibility(!m_blinkState);
        else if (i == 1)
            actor->SetVisibility(m_blinkState);
        else
            actor->SetVisibility(false);
    }
    vtkRender();
}
