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
#include "WorkspaceChrome.h"
#include "InspectorPanel.h"
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

// ── Setup: renderer, toolbar, sidebar, LUT customizer, FITS header ───────────
// Extracted verbatim from vtkWindowImage.cpp. One-time UI/renderer construction
// helpers invoked from the constructor (which stays in the main TU).

namespace {
QString compactDirPath(const QString &fullPath)
{
    const QStringList parts = fullPath.split(u'/', Qt::SkipEmptyParts);
    if (parts.size() <= 2) {
        return fullPath;
    }
    return QStringLiteral("\u2026/%1/%2").arg(parts[parts.size() - 2], parts.last());
}
}  // namespace

void vtkWindowImage::showLUTCustomizer()
{
    const int index = this->currentLayerIndex();
    if (!this->layers || index < 0 || index >= this->layers->rowCount()) {
        return;
    }
    qDebug().noquote() << QStringLiteral("[lut] open request index=%1 existing=%2")
                                  .arg(index)
                                  .arg(this->lutCustomizer != nullptr);
    if (!this->lutCustomizer) {
        this->lutCustomizer = new LUTCustomizerDialog(this);
        QObject::connect(this->lutCustomizer, &LUTCustomizerDialog::lutUpdated, this,
                         [this]() {
                             // Persist the chosen stretch on the active layer so it
                             // survives reloads / colormap swaps.
                             const int idx = this->currentLayerIndex();
                             if (this->layers && idx >= 0 && this->lutCustomizer) {
                                 this->layers->setScaleMode(idx, this->lutCustomizer->scaleMode(),
                                                            this->lutCustomizer->scaleGamma());
                             }
                             this->markDisplayStateAdjusted("lut editor");
                             this->showCurrentLayerSettings();
                         });
    }
    this->lutCustomizer->show();
    this->lutCustomizer->raise();
    this->lutCustomizer->activateWindow();
    qDebug().noquote() << QStringLiteral("[lut] dialog shown/raised");
    if (this->suspendLutEditorUpdates) {
        return;
    }
    QPointer<LUTCustomizerDialog> dialog = this->lutCustomizer;
    auto *dataset = this->layers->getImageData(index);
    auto *lut = this->layers->getLookupTable(index);
    const LutScale::Mode mode = this->layers->scaleMode(index);
    const double gamma = this->layers->scaleGamma(index);
    QTimer::singleShot(0, this, [this, dialog, dataset, lut, mode, gamma]() {
        if (!dialog || this->suspendLutEditorUpdates || !dataset || !lut) {
            return;
        }
        qDebug().noquote() << QStringLiteral("[lut] deferred init started");
        dialog->init(dataset, lut, mode, gamma);
    });
}

void vtkWindowImage::updateLUTCustomizer()
{
    if (this->lutCustomizer && !this->suspendLutEditorUpdates) {
        const int index = this->currentLayerIndex();
        this->lutCustomizer->init(this->layers->getImageData(index),
                                  this->layers->getLookupTable(index),
                                  this->layers->scaleMode(index),
                                  this->layers->scaleGamma(index));
    }
}

void vtkWindowImage::setupRenderer()
{
    this->wcsOverlayInitialized = false;
    vtkNew<vtkRenderer> ren;
    ren->SetBackground(0.21, 0.23, 0.25);
    ren->GetActiveCamera()->ParallelProjectionOn();

    vtkNew<vtkGenericOpenGLRenderWindow> win;
    win->AddRenderer(ren);
    ui->vtk->setRenderWindow(win);
    ui->vtk->setEnableTouchEventProcessing(false);

    vtkNew<vtkInteractorStyleImage> style;
    win->GetInteractor()->SetInteractorStyle(style);
    win->GetInteractor()->AddObserver(vtkCommand::MouseMoveEvent, this,
                                      &vtkWindowImage::mouseCallback);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonPressEvent, this,
                                      &vtkWindowImage::beginImageInteraction);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonPressEvent, this,
                                      &vtkWindowImage::toggleProbeFreeze);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                                      &vtkWindowImage::endImageInteraction);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                                      &vtkWindowImage::finishRegionInteraction);
    win->GetInteractor()->AddObserver(vtkCommand::RightButtonPressEvent, this,
                                      &vtkWindowImage::finishRegionInteraction);
    win->GetInteractor()->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, this,
                                      &vtkWindowImage::finishRegionInteraction);
    // Viewport-driven LOD: when a camera interaction (pan / zoom) ends, re-arm a
    // short debounce; if the view settled on a different region/zoom we stream
    // the visible tiles at the appropriate pyramid level.
    win->GetInteractor()->AddObserver(vtkCommand::EndInteractionEvent, this,
                                      &vtkWindowImage::onImageInteractionSettled);
    this->m_lodSettleTimer = new QTimer(this);
    this->m_lodSettleTimer->setSingleShot(true);
    this->m_lodSettleTimer->setInterval(150);
    QObject::connect(this->m_lodSettleTimer, &QTimer::timeout, this,
                     &vtkWindowImage::maybeStreamViewportLod);

    this->coordinate->SetCoordinateSystemToDisplay();
    this->coordinate->SetViewport(ren);

    // Stack
    this->stack->AddImage(this->layers->getMasterLayerActor());
    this->stack->SetActiveLayer(0);
    ren->AddViewProp(this->stack);

    // Color bar
    this->colorbar->SetMaximumWidthInPixels(120);
    this->colorbar->SetPosition(0.9, 0.1);
    this->colorbar->SetLookupTable(this->layers->getLookupTable(this->layers->getMasterIndex()));
    ren->AddViewProp(this->colorbar);

    this->layerController =
            std::make_unique<ImageLayerController>(*(this->layers), this->stack, this->colorbar);

    // Legend: local AstroUtils-based legend not used in backend mode; remote WCS overlay actors are used instead.
    ren->AddViewProp(this->overlayXAxis);
    ren->AddViewProp(this->overlayYAxis);
    ren->AddViewProp(this->overlayXTitleActor);
    ren->AddViewProp(this->overlayYTitleActor);
    vtkNew<vtkPolyDataMapper> probeHorizontalMapper;
    probeHorizontalMapper->SetInputConnection(this->probeHorizontalLine->GetOutputPort());
    this->probeHorizontalActor->SetMapper(probeHorizontalMapper);
    this->probeHorizontalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->probeHorizontalActor->GetProperty()->SetLineWidth(1.5);
    this->probeHorizontalActor->VisibilityOff();
    ren->AddActor(this->probeHorizontalActor);
    vtkNew<vtkPolyDataMapper> probeVerticalMapper;
    probeVerticalMapper->SetInputConnection(this->probeVerticalLine->GetOutputPort());
    this->probeVerticalActor->SetMapper(probeVerticalMapper);
    this->probeVerticalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->probeVerticalActor->GetProperty()->SetLineWidth(1.5);
    this->probeVerticalActor->VisibilityOff();
    ren->AddActor(this->probeVerticalActor);
    // ── Measurement actors ─────────────────────────────────────────────────
    {
        const auto configureMeasActor = [&ren](vtkLineSource *line, vtkActor *actor) {
            vtkNew<vtkPolyDataMapper> mapper;
            mapper->SetInputConnection(line->GetOutputPort());
            actor->SetMapper(mapper);
            actor->GetProperty()->SetColor(1.0, 0.65, 0.0);
            actor->GetProperty()->SetLineWidth(2.0);
            actor->GetProperty()->SetLineStipplePattern(0xF0F0);
            actor->VisibilityOff();
            ren->AddActor(actor);
        };
        configureMeasActor(this->m_measLine1, this->m_measLineActor1);
        configureMeasActor(this->m_measLine2, this->m_measLineActor2);

        this->m_measLabelActor = vtkSmartPointer<vtkTextActor>::New();
        this->m_measLabelActor->GetTextProperty()->SetColor(1.0, 0.85, 0.1);
        this->m_measLabelActor->GetTextProperty()->SetFontSize(14);
        this->m_measLabelActor->GetTextProperty()->SetBold(true);
        this->m_measLabelActor->GetTextProperty()->SetShadow(true);
        this->m_measLabelActor->VisibilityOff();
        ren->AddViewProp(this->m_measLabelActor);
    }

    // ── Polarization vector overlay actor ─────────────────────────────────
    {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(this->m_polVecData);
        this->m_polVecActor->SetMapper(mapper);
        this->m_polVecActor->GetProperty()->SetColor(0.95, 0.95, 0.25);
        this->m_polVecActor->GetProperty()->SetLineWidth(1.2);
        this->m_polVecActor->VisibilityOff();
        ren->AddActor(this->m_polVecActor);
    }

    // ── Annotation placement preview actor ────────────────────────────────
    {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(this->m_annotPreviewData);
        this->m_annotPreviewActor->SetMapper(mapper);
        this->m_annotPreviewActor->GetProperty()->SetColor(1.0, 1.0, 0.3);
        this->m_annotPreviewActor->GetProperty()->SetLineWidth(2.0);
        this->m_annotPreviewActor->GetProperty()->SetOpacity(0.7);
        this->m_annotPreviewActor->VisibilityOff();
        ren->AddActor(this->m_annotPreviewActor);
    }

    vtkRenderer *renderer = ren;
    const auto configureRegionLineActor = [renderer](vtkLineSource *line, vtkActor *actor) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.2, 0.9, 1.0);
        actor->GetProperty()->SetLineWidth(2.0);
        actor->VisibilityOff();
        renderer->AddActor(actor);
    };
    configureRegionLineActor(this->regionTopLine, this->regionTopActor);
    configureRegionLineActor(this->regionBottomLine, this->regionBottomActor);
    configureRegionLineActor(this->regionLeftLine, this->regionLeftActor);
    configureRegionLineActor(this->regionRightLine, this->regionRightActor);
    this->regionCircleSource->SetNumberOfSides(96);
    this->regionCircleSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> regionCircleMapper;
    regionCircleMapper->SetInputConnection(this->regionCircleSource->GetOutputPort());
    this->regionCircleActor->SetMapper(regionCircleMapper);
    this->regionCircleActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionCircleActor->GetProperty()->SetLineWidth(2.0);
    this->regionCircleActor->VisibilityOff();
    ren->AddActor(this->regionCircleActor);
    this->regionAnnulusOuterSource->SetNumberOfSides(96);
    this->regionAnnulusOuterSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> regionAnnulusOuterMapper;
    regionAnnulusOuterMapper->SetInputConnection(this->regionAnnulusOuterSource->GetOutputPort());
    this->regionAnnulusOuterActor->SetMapper(regionAnnulusOuterMapper);
    this->regionAnnulusOuterActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionAnnulusOuterActor->GetProperty()->SetLineWidth(2.0);
    this->regionAnnulusOuterActor->GetProperty()->SetRepresentationToWireframe();
    this->regionAnnulusOuterActor->VisibilityOff();
    ren->AddActor(this->regionAnnulusOuterActor);
    this->regionAnnulusInnerSource->SetNumberOfSides(96);
    this->regionAnnulusInnerSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> regionAnnulusInnerMapper;
    regionAnnulusInnerMapper->SetInputConnection(this->regionAnnulusInnerSource->GetOutputPort());
    this->regionAnnulusInnerActor->SetMapper(regionAnnulusInnerMapper);
    this->regionAnnulusInnerActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionAnnulusInnerActor->GetProperty()->SetLineWidth(1.5);
    this->regionAnnulusInnerActor->GetProperty()->SetRepresentationToWireframe();
    this->regionAnnulusInnerActor->VisibilityOff();
    ren->AddActor(this->regionAnnulusInnerActor);
    vtkNew<vtkPolyDataMapper> regionAnnulusFillMapper;
    regionAnnulusFillMapper->SetInputData(this->regionAnnulusFillData);
    this->regionAnnulusFillActor->SetMapper(regionAnnulusFillMapper);
    this->regionAnnulusFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionAnnulusFillActor->GetProperty()->SetOpacity(0.18);
    this->regionAnnulusFillActor->GetProperty()->SetRepresentationToSurface();
    this->regionAnnulusFillActor->VisibilityOff();
    ren->AddActor(this->regionAnnulusFillActor);
    this->regionPolygonTriangulator->SetInputData(this->regionPolygonFillData);
    vtkNew<vtkPolyDataMapper> regionPolygonMapper;
    regionPolygonMapper->SetInputData(this->regionPolygonData);
    this->regionPolygonActor->SetMapper(regionPolygonMapper);
    this->regionPolygonActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionPolygonActor->GetProperty()->SetLineWidth(2.0);
    this->regionPolygonActor->VisibilityOff();
    ren->AddActor(this->regionPolygonActor);
    vtkNew<vtkPolyDataMapper> regionPolygonFillMapper;
    regionPolygonFillMapper->SetInputConnection(this->regionPolygonTriangulator->GetOutputPort());
    this->regionPolygonFillActor->SetMapper(regionPolygonFillMapper);
    this->regionPolygonFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->regionPolygonFillActor->GetProperty()->SetOpacity(0.18);
    this->regionPolygonFillActor->VisibilityOff();
    ren->AddActor(this->regionPolygonFillActor);
    this->catalogueOverlayData->SetPoints(this->catalogueOverlayPoints);
    this->catalogueOverlayData->SetLines(this->catalogueOverlayCells);
    vtkNew<vtkPolyDataMapper> catalogueOverlayMapper;
    catalogueOverlayMapper->SetInputData(this->catalogueOverlayData);
    this->catalogueOverlayActor->SetMapper(catalogueOverlayMapper);
    this->catalogueOverlayActor->GetProperty()->SetColor(1.0, 0.45, 0.15);
    this->catalogueOverlayActor->GetProperty()->SetLineWidth(1.5);
    this->catalogueOverlayActor->VisibilityOff();
    ren->AddActor(this->catalogueOverlayActor);
    this->catalogueHoverOverlayData->SetPoints(this->catalogueHoverOverlayPoints);
    this->catalogueHoverOverlayData->SetLines(this->catalogueHoverOverlayCells);
    vtkNew<vtkPolyDataMapper> catalogueHoverMapper;
    catalogueHoverMapper->SetInputData(this->catalogueHoverOverlayData);
    this->catalogueHoverOverlayActor->SetMapper(catalogueHoverMapper);
    this->catalogueHoverOverlayActor->GetProperty()->SetColor(1.0, 0.95, 0.45);
    this->catalogueHoverOverlayActor->GetProperty()->SetLineWidth(2.5);
    this->catalogueHoverOverlayActor->VisibilityOff();
    ren->AddActor(this->catalogueHoverOverlayActor);
    this->catalogueSelectionOverlayData->SetPoints(this->catalogueSelectionOverlayPoints);
    this->catalogueSelectionOverlayData->SetLines(this->catalogueSelectionOverlayCells);
    vtkNew<vtkPolyDataMapper> catalogueSelectionMapper;
    catalogueSelectionMapper->SetInputData(this->catalogueSelectionOverlayData);
    this->catalogueSelectionOverlayActor->SetMapper(catalogueSelectionMapper);
    this->catalogueSelectionOverlayActor->GetProperty()->SetColor(0.15, 0.95, 1.0);
    this->catalogueSelectionOverlayActor->GetProperty()->SetLineWidth(3.2);
    this->catalogueSelectionOverlayActor->VisibilityOff();
    ren->AddActor(this->catalogueSelectionOverlayActor);
    this->overlayXAxis->GetTitleTextProperty()->SetFontSize(16);
    this->overlayXAxis->GetTitleTextProperty()->SetBold(false);
    this->overlayYAxis->GetTitleTextProperty()->SetFontSize(16);
    this->overlayYAxis->GetTitleTextProperty()->SetOrientation(90.);
    this->overlayYAxis->GetTitleTextProperty()->SetBold(false);
    this->overlayXTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->overlayXTitleActor->GetTextProperty()->SetFontSize(14);
    this->overlayXTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->overlayXTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->overlayYTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->overlayYTitleActor->GetTextProperty()->SetFontSize(14);
    this->overlayYTitleActor->GetTextProperty()->SetOrientation(90.);
    this->overlayYTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->overlayYTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->ensureOverlayTickActors(ren);
    this->wcsOverlayInitialized = true;
    this->setWcsOverlayVisible(this->showWcsAxes);
    win->AddObserver(vtkCommand::EndEvent, this, &vtkWindowImage::updateWcsOverlay);
    win->AddObserver(vtkCommand::EndEvent, this, &vtkWindowImage::updateCatalogueOverlayLabels);

    // Loading placeholder — visible until first image data arrives.
    {
        auto *prop = this->m_loadingActor->GetTextProperty();
        prop->SetFontSize(16);
        double r, g, b;
        ThemeManager::instance().vtkTextColor(r, g, b);
        prop->SetColor(r, g, b);
        prop->SetJustificationToCentered();
        prop->SetVerticalJustificationToCentered();
        prop->BoldOff();
        prop->ShadowOff();
        this->m_loadingActor->SetInput("Loading image\xe2\x80\xa6");
        this->m_loadingActor->GetPositionCoordinate()
                ->SetCoordinateSystemToNormalizedViewport();
        this->m_loadingActor->SetPosition(0.5, 0.5);
        this->m_loadingActor->VisibilityOn();
        ren->AddActor2D(this->m_loadingActor);

        // Track runtime theme changes so the loading text stays readable.
        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                         this, [this](EffectiveTheme) {
                             double tr, tg, tb;
                             ThemeManager::instance().vtkTextColor(tr, tg, tb);
                             this->m_loadingActor->GetTextProperty()->SetColor(tr, tg, tb);
                         });
    }

    ren->ResetCamera();
    win->Render();
}

void vtkWindowImage::showFitsHeaderDialog()
{
    BackendClient client(this->remoteBackendUrl, this->remoteBackendToken);
    const auto result = client.fileHeader(this->filepath);

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setWindowTitle(u"FITS Header — %1"_s.arg(QFileInfo(this->filepath).fileName()));
    dlg->resize(620, 480);

    auto *vbox = new QVBoxLayout(dlg);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    auto *header = new QFrame(dlg);
    header->setStyleSheet(
            QStringLiteral(
                    "QFrame {"
                    "  background-color: %1;"
                    "  border-bottom: 1px solid %2;"
                    "}"
                    "QFrame QLabel { background: transparent; }")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                         QLatin1StringView(VisivoTheme::kOutline())));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(16, 12, 16, 12);
    headerLayout->setSpacing(2);
    auto *title = new QLabel(u"FITS Header"_s, header);
    title->setStyleSheet(
            QStringLiteral("color: %1; font-size: 14px; font-weight: 700;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
    headerLayout->addWidget(title);
    auto *sub = new QLabel(this->filepath, header);
    sub->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; "
                            "font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    sub->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerLayout->addWidget(sub);
    vbox->addWidget(header);

    auto *body = new QPlainTextEdit(dlg);
    body->setReadOnly(true);
    body->setLineWrapMode(QPlainTextEdit::NoWrap);
    body->setStyleSheet(
            QStringLiteral(
                    "QPlainTextEdit {"
                    "  background-color: %1; color: %2;"
                    "  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "  font-size: 11px;"
                    "  border: none;"
                    "  padding: 12px 14px;"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kSurface()),
                         QLatin1StringView(VisivoTheme::kOnSurface())));
    if (result.valid && !result.cards.isEmpty()) {
        body->setPlainText(result.cards.join(u"\n"_s));
    } else if (!result.valid) {
        body->setPlainText(
                QStringLiteral("Could not load FITS header.\n\n%1")
                        .arg(result.error.isEmpty() ? u"Unknown error."_s : result.error));
    } else {
        body->setPlainText(u"(empty header)"_s);
    }
    vbox->addWidget(body, 1);

    dlg->show();
}

void vtkWindowImage::setupViewerToolbar()
{
    VisivoTheme::ViewerToolbarSpec spec;
    spec.filePath = this->filepath;
    spec.primaryTag = QStringLiteral("IMAGE");
    spec.secondaryTag = QStringLiteral("remote · full-res");

    VisivoTheme::ViewerToolbarWidgets w;
    QWidget *bar = VisivoTheme::makeViewerToolbar(spec, &w, this);

    if (w.primaryTag) {
        QObject::connect(w.primaryTag, &QToolButton::clicked, this, [this]() {
            this->showFitsHeaderDialog();
        });
    }

    auto *qtBar = new QToolBar(this);
    qtBar->setObjectName(u"viewerToolbarHost"_s);
    qtBar->setMovable(false);
    qtBar->setFloatable(false);
    qtBar->setContextMenuPolicy(Qt::PreventContextMenu);
    qtBar->setStyleSheet(
            QStringLiteral(
                    "QToolBar#viewerToolbarHost {"
                    "  background-color: transparent;"
                    "  border: none;"
                    "  padding: 6px 10px 4px 10px;"
                    "  spacing: 0px;"
                    "}"));
    qtBar->addWidget(bar);
    this->addToolBar(Qt::TopToolBarArea, qtBar);

    // Shared chrome wiring (⌘K, dataset switcher, theme toggle, live health
    // chip). installStatusRail()/start() run later from the constructor, after
    // the window's own permanent status labels are added.
    m_workspaceChrome = new WorkspaceChrome(
            this, this->remoteBackendUrl, this->remoteBackendToken,
            this->remoteSessionId, this);
    m_workspaceChrome->attachCommandBar(w);
}

void vtkWindowImage::setupInspector()
{
    if (!this->m_workspaceChrome)
        return;
    InspectorPanel *panel = this->m_workspaceChrome->installInspectorDock();
    if (!panel)
        return;

    panel->setProperties({
            { u"Dataset"_s, QFileInfo(this->filepath).fileName() },
            { u"Type"_s, u"IMAGE"_s },
            { u"Path"_s, this->filepath },
            { u"Session"_s, this->remoteSessionId },
            { u"Backend"_s, this->remoteBackendUrl },
    });
    // Migrate the former rail pages (layers, catalogue, info/stats) here.
    // Design #1d: layer/catalogue (view) controls live in the LEFT column's
    // LAYERS block; only info/stats stays in the Inspector Properties.
    if (this->m_inspectorPageLayer)     this->m_workspaceChrome->addSessionLayers(this->m_inspectorPageLayer);
    if (this->m_inspectorPageCatalogue) this->m_workspaceChrome->addSessionLayers(this->m_inspectorPageCatalogue);
    if (this->m_inspectorPageInfo)      panel->addPropertiesSection(this->m_inspectorPageInfo);
    // Re-home the kept-alive Tools page as a HIDDEN child of the inspector (not a
    // free child of the QMainWindow, where it could paint over the left dock).
    if (this->m_orphanToolsPage) {
        this->m_orphanToolsPage->setParent(panel);
        this->m_orphanToolsPage->hide();
    }

    // View → "Inspector Panel" toggles the dock.
    if (auto *dock = this->m_workspaceChrome->inspectorDock()) {
        if (this->actionShowControlsSidebar) {
            QObject::connect(this->actionShowControlsSidebar, &QAction::toggled,
                             dock, &QDockWidget::setVisible);
            QObject::connect(dock, &QDockWidget::visibilityChanged,
                             this->actionShowControlsSidebar, &QAction::setChecked);
        }
    }

    const auto add = [panel](const QString &grp, const QString &label, QAction *a) {
        if (a) panel->addTool(grp, label, a);
    };
    add(u"Spectral"_s, u"Extract Spectrum"_s, this->actionExtractSpectrum);
    add(u"Regions"_s, u"Box Region"_s, this->actionBoxRegion);
    add(u"Regions"_s, u"Circle Region"_s, this->actionCircleRegion);
    add(u"Regions"_s, u"Polygon Region"_s, this->actionPolygonRegion);
    add(u"Regions"_s, u"Annulus Region"_s, this->actionAnnulusRegion);
    add(u"Catalogue"_s, u"Load Catalogue Overlay…"_s, this->actionLoadCatalogueOverlay);
    add(u"Catalogue"_s, u"Cross-match…"_s, this->actionCrossmatch);
    add(u"Contours"_s, u"Show Contours"_s, this->m_actToggleContours);
    add(u"Contours"_s, u"Load Contour from FITS…"_s, this->m_actLoadExternalContour);
    add(u"Measure"_s, u"Ruler (Distance)"_s, this->m_actRuler);
    add(u"Measure"_s, u"Angle (3 Points)"_s, this->m_actAngle);

    // Single-view window: no pane grid, no cross-viewer link.
    panel->setPaneLayoutEnabled(false);
    panel->setLinkedSupported(false);

    // Belt-and-suspenders: neutralise any stray direct-child widget painting at (0,0).
    this->parkOrphanDirectChildren();
}

void vtkWindowImage::parkOrphanDirectChildren()
{
    if (!this->m_hiddenPageHolder)
        return;
    const auto children = this->findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
    for (QWidget *w : children) {
        if (w == this->centralWidget() || w == this->m_hiddenPageHolder
            || w == this->menuBar() || w == this->statusBar()
            || qobject_cast<QDockWidget *>(w) || qobject_cast<QToolBar *>(w)
            || w->isWindow())
            continue;
        qWarning().noquote() << QStringLiteral("[workspace] parking orphan direct child: %1 (%2)")
                                        .arg(w->objectName(),
                                             QString::fromLatin1(w->metaObject()->className()));
        w->setParent(this->m_hiddenPageHolder);
        w->hide();
    }
}

void vtkWindowImage::setupSidebar()
{
    // Remove the UI-file sidebar container and replace it with SidebarPanel.
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->sidebarContainer);
    ui->sidebarContainer->hide();

    // ── Shared style constants (same palette as VBT / catalogue viewers) ──────
    const QString hdrStyle     = VisivoTheme::sidebarHeaderStyleSheet();
    const QString cardStyle    = VisivoTheme::sidebarCardStyleSheet();
    const QString subHdrStyle  = VisivoTheme::sidebarSubHeaderStyleSheet();
    const QString keyStyle     = VisivoTheme::infoKeyStyleSheet();
    const QString valStyle     = VisivoTheme::infoValueStyleSheet();
    const QString toolBtnStyle = VisivoTheme::sidebarToolButtonStyleSheet();

    // ── Page 0: Layer Settings (from UI file) ──────────────────────────────────
    // Apply styles to existing UI-file page widgets (page3D is hidden; not styled)
    for (QLabel *lbl : { ui->labelLayerHdr, ui->labelCatalogueHdr }) {
        lbl->setStyleSheet(hdrStyle);
    }
    for (QLabel *lbl : { ui->labelLayerListHdr, ui->labelInventoryHdr, ui->labelObjectsHdr }) {
        lbl->setStyleSheet(subHdrStyle);
    }
    for (QFrame *f : { ui->frameLayerControls, ui->frameCatalogueActions }) {
        f->setStyleSheet(cardStyle);
    }

    // ── Page 1: Tools ─────────────────────────────────────────────────────────
    auto *pageTools = new QWidget;
    auto *toolsLayout = new QVBoxLayout(pageTools);
    toolsLayout->setContentsMargins(8, 8, 8, 8);
    toolsLayout->setSpacing(8);

    {
        auto *hdr = new QLabel(u"TOOLS"_s, pageTools);
        hdr->setStyleSheet(hdrStyle);
        toolsLayout->addWidget(hdr);
    }

    // Helper: create a full-width tool button bound to an action
    const auto makeToolBtn = [&toolBtnStyle](QAction *action, QWidget *parent) -> QToolButton * {
        auto *btn = new QToolButton(parent);
        btn->setDefaultAction(action);
        btn->setStyleSheet(toolBtnStyle);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return btn;
    };

    // Card: Analysis Tools (probe + region stats)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Analysis Tools"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(ui->actionProfile,          card));
        cl->addWidget(makeToolBtn(this->actionBoxRegion,      card));
        cl->addWidget(makeToolBtn(this->actionCircleRegion,   card));
        cl->addWidget(makeToolBtn(this->actionPolygonRegion,  card));
        cl->addWidget(makeToolBtn(this->actionAnnulusRegion,  card));
        toolsLayout->addWidget(card);
    }

    // Card: Catalogue Overlay
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Catalogue Overlay"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(this->actionLoadCatalogueOverlay,  card));
        cl->addWidget(makeToolBtn(this->actionShowCatalogueOverlay,  card));
        cl->addWidget(makeToolBtn(this->actionShowCatalogueLabels,   card));
        cl->addWidget(makeToolBtn(this->actionClearCatalogueOverlay, card));
        toolsLayout->addWidget(card);
        m_dynspecHiddenCards.append(QPointer<QFrame>(card));
    }

    // Card: Measurement Tools (Ruler / Angle)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Measurement"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actRuler,     card));
        cl->addWidget(makeToolBtn(m_actAngle,     card));
        cl->addWidget(makeToolBtn(m_actClearMeas, card));
        toolsLayout->addWidget(card);
    }

    // Card: Contours (same Level / Lower / Upper as the cube viewer)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *cardHdr = new QLabel(u"Contours"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actToggleContours, card));

        auto *formLayout = new QFormLayout();
        formLayout->setHorizontalSpacing(8);
        formLayout->setVerticalSpacing(4);

        m_contourLevelSpin = new QSpinBox(card);
        m_contourLevelSpin->setRange(1, 100);
        m_contourLevelSpin->setValue(m_contourLevel);
        formLayout->addRow(u"Level"_s, m_contourLevelSpin);

        m_contourLowerEdit = new QLineEdit(QString::number(m_contourLower, 'g', 6), card);
        m_contourLowerEdit->setValidator(new QDoubleValidator(card));
        formLayout->addRow(u"Lower"_s, m_contourLowerEdit);

        m_contourUpperEdit = new QLineEdit(QString::number(m_contourUpper, 'g', 6), card);
        m_contourUpperEdit->setValidator(new QDoubleValidator(card));
        formLayout->addRow(u"Upper"_s, m_contourUpperEdit);
        QSpinBox  *levelSpin = m_contourLevelSpin;
        QLineEdit *lowerEdit = m_contourLowerEdit;
        QLineEdit *upperEdit = m_contourUpperEdit;

        cl->addLayout(formLayout);
        toolsLayout->addWidget(card);

        QObject::connect(levelSpin, qOverload<int>(&QSpinBox::valueChanged), this,
                         [this](int v) { m_contourLevel = v; updateContours(); });
        QObject::connect(lowerEdit, &QLineEdit::editingFinished, this,
                         [this, lowerEdit]() {
                             m_contourLower = lowerEdit->text().toDouble();
                             updateContours();
                         });
        QObject::connect(upperEdit, &QLineEdit::editingFinished, this,
                         [this, upperEdit]() {
                             m_contourUpper = upperEdit->text().toDouble();
                             updateContours();
                         });

        // External contour overlay actions (mirror the Tools menu)
        cl->addWidget(makeToolBtn(m_actLoadExternalContour, card));
        cl->addWidget(makeToolBtn(m_actClearAllContours,    card));
    }

    // Card: I/O — Export to Workspace (mirror the Tools menu)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"I/O"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actExportToWorkspace, card));
        toolsLayout->addWidget(card);
    }

    // Card: WCS Display — toggle button for axes + SegmentedToggle for
    // format (Sexagesimal / Decimal) and frame (Galactic / FK5 / Ecliptic),
    // consistent with the cube viewer's mode selectors.
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *cardHdr = new QLabel(u"WCS Display"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(this->actionShowWcsAxes, card));

        const QString secStyle =
                QStringLiteral("QLabel { color: %1; font-size: 9px; margin-top: 4px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

        // Coordinate format: Sexagesimal | Decimal
        auto *fmtHdr = new QLabel(u"Coordinate format"_s, card);
        fmtHdr->setStyleSheet(secStyle);
        cl->addWidget(fmtHdr);
        auto *fmtToggle = new SegmentedToggle(card);
        fmtToggle->addItem(u"Sexagesimal"_s, u"HMS / DMS"_s);
        fmtToggle->addItem(u"Decimal"_s,     u"Decimal degrees"_s);
        fmtToggle->setCurrentIndex(
                this->actionWcsDecimal->isChecked() ? 1 : 0);
        cl->addWidget(fmtToggle);
        QObject::connect(fmtToggle, &SegmentedToggle::currentIndexChanged, this,
                         [this](int idx) {
                             if (idx == 0 && !this->actionWcsSexagesimal->isChecked())
                                 this->actionWcsSexagesimal->trigger();
                             else if (idx == 1 && !this->actionWcsDecimal->isChecked())
                                 this->actionWcsDecimal->trigger();
                         });

        // Coordinate frame: Galactic | FK5 | Ecliptic
        auto *frameHdr = new QLabel(u"Coordinate frame"_s, card);
        frameHdr->setStyleSheet(secStyle);
        cl->addWidget(frameHdr);
        auto *frameToggle = new SegmentedToggle(card);
        frameToggle->addItem(u"Galactic"_s, u"Galactic l, b"_s);
        frameToggle->addItem(u"FK5"_s,      u"RA, Dec (J2000)"_s);
        frameToggle->addItem(u"Ecliptic"_s, u"Ecliptic λ, β"_s);
        if (ui->actionGalactic->isChecked())       frameToggle->setCurrentIndex(0);
        else if (ui->actionFK5->isChecked())       frameToggle->setCurrentIndex(1);
        else if (ui->actionEcliptic->isChecked())  frameToggle->setCurrentIndex(2);
        cl->addWidget(frameToggle);
        QObject::connect(frameToggle, &SegmentedToggle::currentIndexChanged, this,
                         [this](int idx) {
                             QAction *targets[] = { ui->actionGalactic,
                                                    ui->actionFK5,
                                                    ui->actionEcliptic };
                             if (idx >= 0 && idx < 3 && !targets[idx]->isChecked())
                                 targets[idx]->trigger();
                         });

        toolsLayout->addWidget(card);
        m_dynspecHiddenCards.append(QPointer<QFrame>(card));
    }

    // Card: Histogram
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Histogram"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        auto *btn = new QToolButton(card);
        btn->setText(u"Pixel Histogram…"_s);
        btn->setStyleSheet(toolBtnStyle);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btn, &QToolButton::clicked, this, &vtkWindowImage::showHistogramPanel);
        cl->addWidget(btn);
        toolsLayout->addWidget(card);
    }

    // Card: Annotations
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Annotations"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        auto *btnText = new QToolButton(card);
        btnText->setText(u"Add Text…"_s);
        btnText->setStyleSheet(toolBtnStyle);
        btnText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnText->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnText, &QToolButton::clicked, this, &vtkWindowImage::addTextAnnotation);
        cl->addWidget(btnText);

        auto *btnArrow = new QToolButton(card);
        btnArrow->setText(u"Add Arrow…"_s);
        btnArrow->setStyleSheet(toolBtnStyle);
        btnArrow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnArrow->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnArrow, &QToolButton::clicked, this, &vtkWindowImage::addArrowAnnotation);
        cl->addWidget(btnArrow);

        auto *btnClear = new QToolButton(card);
        btnClear->setText(u"Clear All"_s);
        btnClear->setStyleSheet(toolBtnStyle);
        btnClear->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnClear->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnClear, &QToolButton::clicked, this, &vtkWindowImage::clearAnnotations);
        cl->addWidget(btnClear);

        auto *rowIO = new QHBoxLayout;
        rowIO->setSpacing(4);
        auto *btnSave = new QToolButton(card);
        btnSave->setText(u"Save"_s);
        btnSave->setStyleSheet(toolBtnStyle);
        btnSave->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnSave->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnSave, &QToolButton::clicked, this, &vtkWindowImage::saveAnnotationsJson);
        rowIO->addWidget(btnSave);

        auto *btnLoad = new QToolButton(card);
        btnLoad->setText(u"Load"_s);
        btnLoad->setStyleSheet(toolBtnStyle);
        btnLoad->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnLoad->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(btnLoad, &QToolButton::clicked, this, &vtkWindowImage::loadAnnotationsJson);
        rowIO->addWidget(btnLoad);

        cl->addLayout(rowIO);
        toolsLayout->addWidget(card);
    }

    // Card: Blink / Compare
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Blink / Compare"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actBlink, card));

        auto *speedLbl = new QLabel(u"Speed"_s, card);
        speedLbl->setStyleSheet(
                QStringLiteral("QLabel { color: %1; font-size: 9px; "
                               "font-weight: 600; margin-top: 2px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        cl->addWidget(speedLbl);

        auto *speedSlider = new QSlider(Qt::Horizontal, card);
        speedSlider->setRange(50, 1000);
        speedSlider->setValue(m_blinkIntervalMs);
        speedSlider->setToolTip(u"Blink interval in ms (50–1000)"_s);
        cl->addWidget(speedSlider);
        QObject::connect(speedSlider, &QSlider::valueChanged, this,
                         [this](int val) {
                             m_blinkIntervalMs = val;
                             if (m_blinkTimer.isActive())
                                 m_blinkTimer.setInterval(val);
                         });

        toolsLayout->addWidget(card);
    }

    // Card: Stokes Analysis (radio polarimetry workflow)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Stokes Analysis"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        cl->addWidget(makeToolBtn(m_actLoadStokesCompanions, card));
        cl->addWidget(makeToolBtn(m_actComputeStokesP, card));
        cl->addWidget(makeToolBtn(m_actComputeStokesPdebiased, card));
        cl->addWidget(makeToolBtn(m_actComputeStokesPA, card));
        cl->addWidget(makeToolBtn(m_actComputeStokesFrac, card));
        cl->addWidget(makeToolBtn(m_actComputeSpectralIndex, card));
        cl->addWidget(makeToolBtn(m_actComputeFaradayRM, card));
        cl->addWidget(makeToolBtn(m_actTogglePolVecs, card));

        // Pol-vector parameters
        const QString secStyle =
                QStringLiteral("QLabel { color: %1; font-size: 9px; "
                               "font-weight: 600; margin-top: 4px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

        auto *gridLbl = new QLabel(u"Vector grid step (px)"_s, card);
        gridLbl->setStyleSheet(secStyle);
        cl->addWidget(gridLbl);
        auto *gridSpin = new QSpinBox(card);
        gridSpin->setRange(2, 200);
        gridSpin->setValue(m_polVecGridStep);
        cl->addWidget(gridSpin);
        QObject::connect(gridSpin, qOverload<int>(&QSpinBox::valueChanged),
                         this, [this](int v) {
            m_polVecGridStep = v;
            if (m_polVecsVisible) rebuildPolarizationVectors();
            if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
        });

        auto *snrLbl = new QLabel(u"SNR threshold (σ_MAD multiples)"_s, card);
        snrLbl->setStyleSheet(secStyle);
        cl->addWidget(snrLbl);
        auto *snrEdit = new QLineEdit(QString::number(m_polVecSnrMin, 'f', 1), card);
        snrEdit->setValidator(new QDoubleValidator(0.0, 100.0, 2, card));
        cl->addWidget(snrEdit);
        QObject::connect(snrEdit, &QLineEdit::editingFinished,
                         this, [this, snrEdit]() {
            m_polVecSnrMin = snrEdit->text().toDouble();
            if (m_polVecsVisible) rebuildPolarizationVectors();
            if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
        });

        auto *scaleLbl = new QLabel(u"Vector length scale"_s, card);
        scaleLbl->setStyleSheet(secStyle);
        cl->addWidget(scaleLbl);
        auto *scaleSlider = new QSlider(Qt::Horizontal, card);
        scaleSlider->setRange(10, 300);
        scaleSlider->setValue(static_cast<int>(m_polVecScale * 100.0));
        cl->addWidget(scaleSlider);
        QObject::connect(scaleSlider, &QSlider::valueChanged,
                         this, [this](int v) {
            m_polVecScale = v / 100.0;
            if (m_polVecsVisible) rebuildPolarizationVectors();
            if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
        });

        toolsLayout->addWidget(card);
        m_dynspecHiddenCards.append(QPointer<QFrame>(card));
    }

    // Card: Time-Series Tools (only visible in dynamic-spectrum mode)
    {
        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *cardHdr = new QLabel(u"Time-Series Tools"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        const QString miniHdrStyle =
                QStringLiteral("QLabel { color: %1; font-size: 9px; "
                               "font-weight: 600; margin-top: 4px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

        // ── Incoherent Dedispersion ────────────────────────────────────
        cl->addWidget(([&]() {
            auto *l = new QLabel(u"Dedispersion (DM, pc cm⁻³)"_s, card);
            l->setStyleSheet(miniHdrStyle);
            return l;
        })());
        auto *dmEdit = new QLineEdit(u"0.0"_s, card);
        dmEdit->setValidator(new QDoubleValidator(0.0, 1e6, 4, card));
        dmEdit->setObjectName(u"dynspecDmEdit"_s);
        cl->addWidget(dmEdit);
        auto *dedispBtn = new QToolButton(card);
        dedispBtn->setText(u"Compute Dedispersion"_s);
        dedispBtn->setStyleSheet(toolBtnStyle);
        dedispBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        dedispBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(dedispBtn, &QToolButton::clicked, this,
                         &vtkWindowImage::runDynspecDedispersion);
        cl->addWidget(dedispBtn);

        // ── Pulse profile folding ──────────────────────────────────────
        cl->addWidget(([&]() {
            auto *l = new QLabel(u"Pulse profile (period, s)"_s, card);
            l->setStyleSheet(miniHdrStyle);
            return l;
        })());
        auto *periodEdit = new QLineEdit(u"1.0"_s, card);
        periodEdit->setValidator(new QDoubleValidator(1e-9, 1e6, 9, card));
        periodEdit->setObjectName(u"dynspecPeriodEdit"_s);
        cl->addWidget(periodEdit);
        auto *foldBtn = new QToolButton(card);
        foldBtn->setText(u"Compute Pulse Profile"_s);
        foldBtn->setStyleSheet(toolBtnStyle);
        foldBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        foldBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(foldBtn, &QToolButton::clicked, this,
                         &vtkWindowImage::runDynspecPulseProfile);
        cl->addWidget(foldBtn);

        // ── RFI flagging mask ──────────────────────────────────────────
        cl->addWidget(([&]() {
            auto *l = new QLabel(u"RFI mask (σ-clip, σ-MAD multiple)"_s, card);
            l->setStyleSheet(miniHdrStyle);
            return l;
        })());
        auto *sigmaEdit = new QLineEdit(u"5.0"_s, card);
        sigmaEdit->setValidator(new QDoubleValidator(0.5, 50.0, 2, card));
        sigmaEdit->setObjectName(u"dynspecSigmaEdit"_s);
        cl->addWidget(sigmaEdit);
        auto *rfiBtn = new QToolButton(card);
        rfiBtn->setText(u"Compute RFI Mask"_s);
        rfiBtn->setStyleSheet(toolBtnStyle);
        rfiBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        rfiBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QObject::connect(rfiBtn, &QToolButton::clicked, this,
                         &vtkWindowImage::runDynspecRfiMask);
        cl->addWidget(rfiBtn);

        toolsLayout->addWidget(card);
        card->setVisible(false);  // shown only when setDynamicSpectrumMode(true)
        m_dynspecToolsCard = card;
    }

    toolsLayout->addStretch(1);

    // ── Page 2: Info / Stats ─────────────────────────────────────────────────
    auto *pageInfo = new QWidget;
    auto *infoLayout = new QVBoxLayout(pageInfo);
    infoLayout->setContentsMargins(8, 6, 8, 6);
    infoLayout->setSpacing(4);

    {
        auto *hdr = new QLabel(u"INFO / STATS"_s, pageInfo);
        hdr->setStyleSheet(hdrStyle);
        infoLayout->addWidget(hdr);
    }

    // Field-name style: small, muted, uppercase feel
    const QString fnHdrStyle =
            QStringLiteral("QLabel { color: %1; font-size: 9px; font-weight: 600; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));
    // Field-value style: readable, proportional (not monospace — avoids extra width)
    const QString fnValStyle =
            QStringLiteral("QLabel { color: %1; font-size: 10px; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface()));

    // ── Card 1: Dataset (stacked label-over-value — no two-column layout) ─────
    // Each field is a small header label stacked above its value label.
    // Every value is full-card-width, word-wrapped where needed.
    // This eliminates all horizontal overflow regardless of filename length.
    {
        auto *card = new QFrame(pageInfo);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(10, 8, 10, 8);
        cl->setSpacing(0);

        // Helper: add one stacked field (header + value) with optional word-wrap/tooltip
        const auto addField = [&](const QString &header, const QString &value,
                                  const QString &tip = {}, bool wrap = false) {
            auto *hdrLbl = new QLabel(header);
            hdrLbl->setStyleSheet(fnHdrStyle);
            cl->addWidget(hdrLbl);

            auto *valLbl = new QLabel(value);
            valLbl->setStyleSheet(fnValStyle);
            valLbl->setWordWrap(wrap);
            valLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            if (!tip.isEmpty())
                valLbl->setToolTip(tip);
            cl->addWidget(valLbl);
        };

        const QFileInfo fi(this->filepath);
        const int nLayers = this->layers ? this->layers->rowCount() : 1;

        addField(u"Active Layer"_s, fi.fileName(), fi.absoluteFilePath(), /*wrap=*/true);
        cl->addSpacing(5);
        addField(u"Layers"_s, QString::number(nLayers));
        cl->addSpacing(5);
        addField(u"Directory"_s, compactDirPath(fi.absolutePath()), fi.absolutePath(), /*wrap=*/true);
        cl->addSpacing(5);
        addField(u"WCS"_s, this->remoteWcsStatus);

        infoLayout->addWidget(card);
    }

    // ── Card 2: Axes (compact grid: Ax# | Type [unit] | Δ) ───────────────────
    // Axis type names are always short — a 3-column grid stays within sidebar width.
    {
        bool hasAnyAxis = false;
        for (int ax = 0; ax < 3 && !hasAnyAxis; ++ax)
            hasAnyAxis = !this->remoteDatasetCtype[ax].isEmpty();

        if (hasAnyAxis) {
            auto *card = new QFrame(pageInfo);
            card->setStyleSheet(cardStyle);
            auto *gl = new QGridLayout(card);
            gl->setContentsMargins(10, 6, 10, 6);
            gl->setSpacing(2);
            gl->setColumnStretch(1, 1); // type column absorbs available width

            // Tiny column headers
            const auto gridHdr = [&fnHdrStyle](const QString &t,
                                               Qt::Alignment align = Qt::AlignLeft) -> QLabel * {
                auto *l = new QLabel(t);
                l->setStyleSheet(fnHdrStyle);
                l->setAlignment(align);
                return l;
            };
            gl->addWidget(gridHdr(u"Ax"_s),                     0, 0);
            gl->addWidget(gridHdr(u"Type [unit]"_s),            0, 1);
            gl->addWidget(gridHdr(u"\u0394"_s, Qt::AlignRight), 0, 2);

            auto *sep = new QFrame;
            sep->setFrameShape(QFrame::HLine);
            sep->setStyleSheet(
                    u"QFrame { border: none; border-top: 1px solid rgba(4,138,191,0.14);"
                    u"         margin-bottom: 1px; }"_s);
            gl->addWidget(sep, 1, 0, 1, 3);

            int gridRow = 2;
            for (int ax = 0; ax < 3; ++ax) {
                const QString &ctype = this->remoteDatasetCtype[ax];
                if (ctype.isEmpty())
                    continue;
                const QString &cunit = this->remoteDatasetCunit[ax];
                const QString typeStr = cunit.isEmpty()
                        ? ctype
                        : QStringLiteral("%1 [%2]").arg(ctype, cunit);
                const QString deltaStr =
                        QStringLiteral("%1").arg(this->remoteDatasetCdelt[ax], 0, 'g', 5);

                auto *axLbl = new QLabel(QString::number(ax + 1));
                axLbl->setStyleSheet(fnHdrStyle);

                auto *typeLbl = new QLabel(typeStr);
                typeLbl->setStyleSheet(fnValStyle);
                typeLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);

                auto *deltaLbl = new QLabel(deltaStr);
                deltaLbl->setStyleSheet(fnValStyle);
                deltaLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                deltaLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);

                gl->addWidget(axLbl,    gridRow, 0);
                gl->addWidget(typeLbl,  gridRow, 1);
                gl->addWidget(deltaLbl, gridRow, 2);
                ++gridRow;
            }

            infoLayout->addWidget(card);
        }
    }

    // ── WCS Warning card (amber, only when present) ───────────────────────────
    if (!this->remoteWcsWarningMessage.isEmpty()) {
        auto *warnCard = new QFrame(pageInfo);
        warnCard->setStyleSheet(cardStyle);
        auto *wl = new QHBoxLayout(warnCard);
        wl->setContentsMargins(10, 6, 10, 6);
        wl->setSpacing(6);
        auto *warnIcon = new QLabel(u"\u26A0"_s); // ⚠
        warnIcon->setStyleSheet(u"QLabel { color: #CC8800; font-size: 12px; }"_s);
        warnIcon->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        auto *warnMsg = new QLabel(this->remoteWcsWarningMessage);
        warnMsg->setStyleSheet(u"QLabel { color: #BB9900; font-size: 9px; }"_s);
        warnMsg->setWordWrap(true);
        wl->addWidget(warnIcon);
        wl->addWidget(warnMsg, 1);
        infoLayout->addWidget(warnCard);
    }

    infoLayout->addStretch(1);

    // VLKB catalogue overlays: compact sources / filaments over the image footprint.
    QObject::connect(ui->btnSources, &QPushButton::clicked, this,
                     &vtkWindowImage::overlayVlkbSources);
    QObject::connect(ui->btnFilaments, &QPushButton::clicked, this,
                     &vtkWindowImage::overlayVlkbFilaments);
    QObject::connect(&this->m_vlkbCatWatcher, &QFutureWatcher<VlkbCatalogueRaw>::finished, this,
                     &vtkWindowImage::onVlkbCatalogueReady);

    // Design #1d: no rail in the central widget. Stash the setting pages for the
    // Inspector's Properties tab (setupInspector()); the Tools page is superseded
    // by the Inspector's Analysis tab (kept alive, hidden). Parent + hide until
    // setupInspector() reparents them so they don't flash as top-level windows.
    // Bug-class fix (see vtkWindowCube_Setup.cpp): park ALL assembler pages in one
    // hidden holder so none is a free child of the QMainWindow painting at (0,0)
    // over the left dock. setupInspector() reparents the shown ones out of it.
    auto *holder = new QWidget(this);
    holder->setObjectName(u"hiddenSidebarPageHolder"_s);
    holder->hide();
    auto *holderLayout = new QVBoxLayout(holder);
    holderLayout->setContentsMargins(0, 0, 0, 0);
    for (QWidget *p : { ui->pageLayer, ui->pageCatalogue, pageTools, pageInfo })
        holderLayout->addWidget(p);
    this->m_hiddenPageHolder       = holder;
    this->m_inspectorPageLayer     = ui->pageLayer;
    this->m_inspectorPageCatalogue = ui->pageCatalogue;
    this->m_inspectorPageInfo      = pageInfo;
    this->m_orphanToolsPage        = pageTools;

    this->actionShowControlsSidebar = new QAction(u"Inspector Panel"_s, this);
    this->actionShowControlsSidebar->setCheckable(true);
    this->actionShowControlsSidebar->setChecked(true);
    ui->menuView->addAction(this->actionShowControlsSidebar);
}
