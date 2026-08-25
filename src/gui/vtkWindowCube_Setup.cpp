#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "qcustomplot.h"
#include "ColorMaps.h"
#include "ChannelOverlay.h"
#include "CubeUiAssembler.h"
#include "CubeViewportGeometry.h"
#include "CubeWcsUtil.h"
#include "CubeVRController.h"
#include "DiagnosticsWindow.h"
#include "theme/SegmentedToggle.h"
#include "CatalogueOverlayUtils.h"
#include "CatalogueTableModel.h"
#include "LUTCustomizerDialog.h"
#include "BaselineDialog.h"
#include "LinewidthDialog.h"
#include "MomentMapController.h"
#include "NoiseController.h"
#include "StackDialog.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"
#include "WorkspaceChrome.h"
#include "ToolResultDialog.h"
#include "ProfileWidget.h"
#include "PvController.h"
#include "PvDiagramWidget.h"
#include "RemoteRenderWidget.h"
#include "SidebarPanel.h"
#include "Settings.h"
#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "vtkInteractorStyleProfile.h"
#include "vtkLegendScaleActorWCS.h"
#include "wcs.h"

#include <QVTKInteractor.h>
#include <vtkActor.h>
#include <vtkAxisActor2D.h>
#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkColorTransferFunction.h>
#include <vtkContourTriangulator.h>
#include <vtkCoordinate.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges2D.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageMapToColors.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleUser.h>
#include <vtkCubeSource.h>
#include <vtkLineSource.h>
#include <vtkLookupTable.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkCubeAxesActor.h>
#include <vtkOutlineFilter.h>
#include <vtkPropPicker.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkPlaneSource.h>
#include <vtkPiecewiseFunction.h>
#include <vtkTexture.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkRenderWindow.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkDataArray.h>
#include <vtkIdTypeArray.h>
#include <vtkPointData.h>
#include <vtkTextProperty.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <QFileInfo>
#include <QActionGroup>
#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolBar>
#include <QToolButton>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include "ChannelMapsDialog.h"
#include "ChannelMapsWindow.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QList>
#include <QMetaObject>
#include <QMenu>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QSplitter>
#include <QTimer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QGroupBox>
#include <QSlider>
#include <QSpinBox>
#include <QTableView>
#include <QThread>
#include <QtConcurrentRun>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

using namespace Qt::StringLiterals;

// ── Setup: toolbar, sidebar, docks, renderers, camera presets ────────────────
// Extracted verbatim from vtkWindowCube.cpp. One-time UI/renderer construction
// helpers invoked from the (large) constructor, which stays in the main TU.

namespace {
vtkSmartPointer<vtkImageData> createPlaceholderImageData()
{
    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, 0, 0, 0, 0, 0);
    image->AllocateScalars(VTK_FLOAT, 1);
    image->SetScalarComponentFromFloat(0, 0, 0, 0, 0.f);
    return image;
}
}  // namespace

void vtkWindowCube::setupViewerToolbar()
{
    // Workspace command bar (design #1d): VisIVO mark, dataset pill, centred ⌘K
    // search field, remote chip + theme toggle. Hosted via QMainWindow::addToolBar
    // so we don't have to restructure the .ui central widget — the toolbar sits
    // between the menu bar and centralwidget by default.
    VisivoTheme::ViewerToolbarSpec spec;
    spec.filePath = this->filepath;
    spec.primaryTag = QStringLiteral("CUBE · %1×%2×%3")
                              .arg(this->remoteDatasetWidth)
                              .arg(this->remoteDatasetHeight)
                              .arg(this->remoteDatasetDepth);
    spec.secondaryTag = QStringLiteral("remote · full-res");

    VisivoTheme::ViewerToolbarWidgets w;
    QWidget *bar = VisivoTheme::makeViewerToolbar(spec, &w, this);

    // Click on the "CUBE · …" tag opens the FITS header viewer.
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
    // Strip QToolBar's default chrome — let the inner widget supply its own.
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
    // chip). The status rail is installed later from the constructor, once the
    // window's own permanent status-bar labels exist, so the rail slots land to
    // their right. See vtkWindowCube.cpp.
    m_workspaceChrome = new WorkspaceChrome(
            this, this->remoteBackendUrl, this->remoteBackendToken,
            this->remoteSessionId, this);
    m_workspaceChrome->attachCommandBar(w);
}

void vtkWindowCube::setupSidebar()
{
    // The UI file creates a hand-rolled sidebarContainer in mainHLayout.
    // Remove it from the layout and replace it with SidebarPanel, reusing the
    // four page content widgets (they are reparented into SidebarPanel's scroll areas).
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->sidebarContainer);
    ui->sidebarContainer->hide();

    // Construct the assembler (owns all sidebar widget pointers).
    m_uiAssembler = std::make_unique<CubeUiAssembler>(ui);

    // Build pages — each method reparents ui->* widgets and stores QPointers.
    QWidget *page3dSettings = m_uiAssembler->buildThresholdPage();
    QWidget *page2dSettings = m_uiAssembler->build2dViewPage();
    QWidget *pageTools      = m_uiAssembler->buildToolsPage();

    // ── Append "Export & Inspection" card to the Tools page so every menu
    // action that has no representation in the assembler-built cards is
    // also accessible from the sidebar (keeps menu ↔ sidebar coherent).
    if (auto *toolsLayout = qobject_cast<QVBoxLayout *>(pageTools->layout())) {
        const QString cardStyle   = VisivoTheme::sidebarCardStyleSheet();
        const QString subHdrStyle = VisivoTheme::sidebarSubHeaderStyleSheet();
        const QString toolBtnStyle = VisivoTheme::sidebarToolButtonStyleSheet();
        const auto makeBtn = [&toolBtnStyle](QAction *act, QWidget *parent) {
            auto *b = new QToolButton(parent);
            b->setDefaultAction(act);
            b->setStyleSheet(toolBtnStyle);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setToolButtonStyle(Qt::ToolButtonTextOnly);
            return b;
        };

        auto *card = new QFrame(pageTools);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *hdr = new QLabel(u"Export & Inspection"_s, card);
        hdr->setStyleSheet(subHdrStyle);
        cl->addWidget(hdr);

        if (m_actExportSubCube)     cl->addWidget(makeBtn(m_actExportSubCube,     card));
        if (m_actExportChannel2d)   cl->addWidget(makeBtn(m_actExportChannel2d,   card));
        if (m_actExportMomentFits)  cl->addWidget(makeBtn(m_actExportMomentFits,  card));
        if (m_actSliceHistogram)    cl->addWidget(makeBtn(m_actSliceHistogram,    card));
        if (m_actSendSliceToImage)  cl->addWidget(makeBtn(m_actSendSliceToImage,  card));

        // Insert before any trailing stretch the assembler may have added.
        int insertIdx = toolsLayout->count();
        for (int i = toolsLayout->count() - 1; i >= 0; --i) {
            if (toolsLayout->itemAt(i)->spacerItem()) { insertIdx = i; break; }
        }
        toolsLayout->insertWidget(insertIdx, card);
    }

    CubeUiAssembler::InfoStatsInput infoIn;
    infoIn.filepath         = this->filepath;
    infoIn.datasetWidth     = this->remoteDatasetWidth;
    infoIn.datasetHeight    = this->remoteDatasetHeight;
    infoIn.datasetDepth     = this->remoteDatasetDepth;
    infoIn.wcsFrameLabel    = this->currentWcsFrameLabel();
    infoIn.spectralAxisTitle = this->spectralAxisTitle();
    infoIn.wcsStatus        = this->remoteWcsStatus;
    infoIn.wcsWarningMessage = this->remoteWcsWarningMessage;
    QWidget *pageInfoStats  = m_uiAssembler->buildInfoStatsPage(infoIn);

    // Capture the default LUT names chosen by the assembler.
    this->m_activeColorMapName  = m_uiAssembler->lutCombo()
                                          ? m_uiAssembler->lutCombo()->currentText() : QString();
    this->m_activeSliceLutName  = m_uiAssembler->sliceLutCombo()
                                          ? m_uiAssembler->sliceLutCombo()->currentText() : QString();
    this->m_activeMomentLutName = m_uiAssembler->momentLutCombo()
                                          ? m_uiAssembler->momentLutCombo()->currentText() : QString();

    // ── Extras → distributed across 3D View / 2D View settings ───────────────
    // What used to live in a separate "Cube Extras" tab is now redistributed
    // where it semantically belongs: 3D-scoped affordances (cutting-plane
    // visibility, 3D WCS axes, click-to-pick spectrum on the plane) go into
    // the 3D View Settings tab; the slice animator goes into 2D View Settings.
    // Visual style matches the rest of the sidebar via CubeUiAssembler's
    // shared header / card QSS. All widgets remain *mirrors* of the existing
    // QAction objects (menu + sidebar share one source of truth).
    const auto buildSection = [](const QString &title, QWidget **outCardOpt)
            -> std::pair<QLabel*, QVBoxLayout*> {
        auto *hdr = new QLabel(title);
        hdr->setStyleSheet(CubeUiAssembler::sidebarHdrStyle());
        auto *card = new QFrame();
        card->setStyleSheet(CubeUiAssembler::sidebarCardStyle());
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);
        if (outCardOpt) *outCardOpt = card;
        return { hdr, cl };
    };
    // Append a section (header label + card frame) to a page, keeping the
    // trailing addStretch() at the very bottom so layouts don't collapse.
    const auto appendToPage = [](QWidget *page, QLabel *hdr, QWidget *card) {
        auto *root = qobject_cast<QVBoxLayout *>(page->layout());
        if (!root) return;
        QLayoutItem *trailingStretch = root->takeAt(root->count() - 1);
        root->addWidget(hdr);
        root->addWidget(card);
        if (trailingStretch) root->addItem(trailingStretch);
    };

    // ─── Section: Cutting Plane (→ 3D View Settings) ────────────────────────
    // The "Show cutting plane" toggle is a full-width QToolButton bound to
    // its QAction via setDefaultAction() — same visual treatment as Show
    // WCS Axes / Show Contours (selectable button instead of a checkbox).
    // The opacity row mirrors RENDERING THRESHOLD: full-width slider with
    // a read-only QLineEdit beneath showing the current percentage.
    QSlider   *opacitySlider = nullptr;
    QLineEdit *opacityVal = nullptr;
    {
        QWidget *card = nullptr;
        auto [hdr, cl] = buildSection(u"CUTTING PLANE"_s, &card);
        auto *planeShowBtn = CubeUiAssembler::makeSidebarButton(u"Show cutting plane"_s);
        planeShowBtn->setEnabled(true);
        planeShowBtn->setDefaultAction(this->actionShowCuttingPlane);
        cl->addWidget(planeShowBtn);

        opacitySlider = new QSlider(Qt::Horizontal, card);
        opacitySlider->setRange(0, 100);
        opacitySlider->setValue(50);
        opacitySlider->setSingleStep(5);
        opacitySlider->setPageStep(25);
        opacitySlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        opacitySlider->setToolTip(u"Cutting plane opacity (0–100%)."_s);
        cl->addWidget(opacitySlider);

        opacityVal = new QLineEdit(u"50 %"_s, card);
        opacityVal->setReadOnly(true);
        opacityVal->setFocusPolicy(Qt::NoFocus);
        opacityVal->setAlignment(Qt::AlignCenter);
        opacityVal->setToolTip(u"Current cutting plane opacity."_s);
        cl->addWidget(opacityVal);
        appendToPage(page3dSettings, hdr, card);
        this->m_cuttingPlaneOpacitySlider = opacitySlider;   // re-hosted by the flat left dock
    }

    // ─── Section: 3D Reference (→ 3D View Settings) ─────────────────────────
    {
        QWidget *card = nullptr;
        auto [hdr, cl] = buildSection(u"3D REFERENCE"_s, &card);
        auto *axesBtn = CubeUiAssembler::makeSidebarButton(u"Show 3D WCS axes"_s);
        axesBtn->setEnabled(true);
        axesBtn->setDefaultAction(this->actionShow3dWcsAxes);
        cl->addWidget(axesBtn);
        appendToPage(page3dSettings, hdr, card);
    }

    // ─── Section: 3D Interaction (→ 3D View Settings) ───────────────────────
    {
        QWidget *card = nullptr;
        auto [hdr, cl] = buildSection(u"3D INTERACTION"_s, &card);
        auto *pickBtn = CubeUiAssembler::makeSidebarButton(u"Pick spectrum on plane click"_s);
        pickBtn->setEnabled(true);
        pickBtn->setDefaultAction(this->actionPickSpectrum3d);
        pickBtn->setToolTip(
                u"Click the cutting plane in the 3D view to extract the spectral profile at "
                u"that (RA, DEC) position into the probe plot."_s);
        cl->addWidget(pickBtn);
        appendToPage(page3dSettings, hdr, card);
    }

    // ─── Section: Slice Animation (→ 2D View Settings) ──────────────────────
    QPushButton *playBtn = nullptr;
    QComboBox   *fpsCombo = nullptr;
    QComboBox   *modeCombo = nullptr;
    {
        QWidget *card = nullptr;
        auto [hdr, cl] = buildSection(u"SLICE ANIMATION"_s, &card);
        playBtn = new QPushButton(u"▶  Play"_s, card);
        playBtn->setCheckable(true);
        playBtn->setChecked(this->actionPlaySlices->isChecked());
        cl->addWidget(playBtn);
        auto *fpsRow = new QHBoxLayout;
        fpsRow->setSpacing(6);
        fpsRow->addWidget(new QLabel(u"Speed"_s, card));
        // Predefined fps values only — matches the View › Animation Speed
        // menu radios and prevents the user typing a weird value the menu
        // can't reflect. Editable=false ensures no free-form entry.
        fpsCombo = new QComboBox(card);
        for (int fps : { 2, 5, 10, 15, 30 }) {
            fpsCombo->addItem(u"%1 fps"_s.arg(fps), fps);
        }
        const int initFpsIdx = fpsCombo->findData(this->animationFps);
        fpsCombo->setCurrentIndex(initFpsIdx >= 0 ? initFpsIdx : fpsCombo->findData(5));
        fpsRow->addWidget(fpsCombo, /*stretch=*/1);
        cl->addLayout(fpsRow);
        auto *modeRow = new QHBoxLayout;
        modeRow->setSpacing(6);
        modeRow->addWidget(new QLabel(u"Mode"_s, card));
        modeCombo = new QComboBox(card);
        modeCombo->addItem(u"Loop"_s,        static_cast<int>(AnimationMode::Loop));
        modeCombo->addItem(u"Bounce"_s,      static_cast<int>(AnimationMode::Bounce));
        modeCombo->addItem(u"Stop at End"_s, static_cast<int>(AnimationMode::Once));
        modeCombo->setCurrentIndex(modeCombo->findData(static_cast<int>(this->animationMode)));
        modeRow->addWidget(modeCombo, /*stretch=*/1);
        cl->addLayout(modeRow);
        appendToPage(page2dSettings, hdr, card);
    }

    // The three checkable toggles (Show cutting plane, Show 3D WCS axes,
    // Pick spectrum on plane click) are bound to their QAction via
    // QToolButton::setDefaultAction() in the section blocks above — Qt
    // takes care of bidirectional state sync, no manual wiring needed.

    // Play button ↔ actionPlaySlices, and label flips ▶/⏸
    const auto refreshPlayLabel = [playBtn](bool playing) {
        playBtn->setText(playing ? u"⏸  Pause"_s : u"▶  Play"_s);
    };
    refreshPlayLabel(playBtn->isChecked());
    QObject::connect(this->actionPlaySlices, &QAction::toggled, playBtn,
                     [playBtn, refreshPlayLabel](bool v) {
                         if (playBtn->isChecked() != v) {
                             const QSignalBlocker block(playBtn);
                             playBtn->setChecked(v);
                         }
                         refreshPlayLabel(v);
                     });
    QObject::connect(playBtn, &QPushButton::toggled, this->actionPlaySlices,
                     [this](bool v) {
                         if (this->actionPlaySlices->isChecked() != v) {
                             this->actionPlaySlices->setChecked(v);
                         }
                     });

    // Opacity slider ↔ cuttingPlaneOpacityGroup.
    QObject::connect(opacitySlider, &QSlider::valueChanged, this,
                     [this, opacityVal](int pct) {
                         opacityVal->setText(u"%1 %"_s.arg(pct));
                         this->setCuttingPlaneOpacity(pct / 100.0);
                         if (this->cuttingPlaneOpacityGroup) {
                             for (auto *a : this->cuttingPlaneOpacityGroup->actions()) {
                                 if (a->data().toInt() == pct) {
                                     const QSignalBlocker block(a);
                                     a->setChecked(true);
                                     break;
                                 }
                             }
                         }
                     });
    QObject::connect(this->cuttingPlaneOpacityGroup, &QActionGroup::triggered, opacitySlider,
                     [opacitySlider](QAction *a) {
                         const int pct = a->data().toInt();
                         if (opacitySlider->value() != pct) {
                             const QSignalBlocker block(opacitySlider);
                             opacitySlider->setValue(pct);
                         }
                     });

    // FPS combo ↔ animationFpsGroup. Only preset values are exposed; the
    // combo's currentData carries the fps int — no free-form entry possible.
    QObject::connect(fpsCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this, fpsCombo](int) {
                         const int v = fpsCombo->currentData().toInt();
                         this->animationFps = std::max(1, v);
                         if (this->sliceAnimationTimer.isActive()) {
                             this->sliceAnimationTimer.start(
                                     std::max(1, 1000 / this->animationFps));
                         }
                         if (this->animationFpsGroup) {
                             for (auto *a : this->animationFpsGroup->actions()) {
                                 if (a->data().toInt() == v) {
                                     const QSignalBlocker block(a);
                                     a->setChecked(true);
                                     return;
                                 }
                             }
                         }
                     });
    QObject::connect(this->animationFpsGroup, &QActionGroup::triggered, fpsCombo,
                     [fpsCombo](QAction *a) {
                         const int v = a->data().toInt();
                         const int idx = fpsCombo->findData(v);
                         if (idx >= 0 && fpsCombo->currentIndex() != idx) {
                             const QSignalBlocker block(fpsCombo);
                             fpsCombo->setCurrentIndex(idx);
                         }
                     });

    // Mode combo ↔ animationModeGroup
    QObject::connect(modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this, modeCombo](int) {
                         const auto m = static_cast<AnimationMode>(
                                 modeCombo->currentData().toInt());
                         this->animationMode = m;
                         this->animationDirection = +1;
                         if (this->animationModeGroup) {
                             for (auto *a : this->animationModeGroup->actions()) {
                                 if (a->data().toInt() == static_cast<int>(m)) {
                                     const QSignalBlocker block(a);
                                     a->setChecked(true);
                                     break;
                                 }
                             }
                         }
                     });
    QObject::connect(this->animationModeGroup, &QActionGroup::triggered, modeCombo,
                     [modeCombo](QAction *a) {
                         const int v = a->data().toInt();
                         const int idx = modeCombo->findData(v);
                         if (idx >= 0 && modeCombo->currentIndex() != idx) {
                             const QSignalBlocker block(modeCombo);
                             modeCombo->setCurrentIndex(idx);
                         }
                     });

    // Design #1d: the cube viewer has NO rail in the central widget. The setting
    // pages built above are migrated into the right-hand Inspector's Properties
    // tab by setupInspector(); the Tools page is superseded by the Inspector's
    // Analysis tab (kept alive but hidden so its QAction-mirror buttons don't
    // become stray top-level windows). Parent them to the window and hide until
    // setupInspector() reparents the ones it hosts.
    // Bug-class fix: never leave a CubeUiAssembler page as a free child of the
    // QMainWindow — a hidden page with no layout still paints its section headers
    // at (0,0), over the left Session Data dock. Park ALL pages inside one hidden
    // holder (a real container with a layout); setupInspector() reparents the
    // ones it shows out of it. Anything left behind stays contained and unpainted.
    auto *holder = new QWidget(this);
    holder->setObjectName(u"hiddenSidebarPageHolder"_s);
    holder->hide();
    auto *holderLayout = new QVBoxLayout(holder);
    holderLayout->setContentsMargins(0, 0, 0, 0);
    for (QWidget *p : { page3dSettings, page2dSettings, pageTools, pageInfoStats })
        holderLayout->addWidget(p);
    this->m_hiddenPageHolder  = holder;
    this->m_inspectorPage3d   = page3dSettings;
    this->m_inspectorPage2d   = page2dSettings;
    this->m_inspectorPageInfo = pageInfoStats;
    this->m_orphanToolsPage   = pageTools;

    // ── Bidirectional sync: rendering mode combo ↔ menu actions ──────────────
    QObject::connect(m_uiAssembler->renderModeCombo(), &SegmentedToggle::currentIndexChanged,
                     this, [this](int idx) {
        const bool wantIsosurface = (idx == 0);
        if (wantIsosurface == ui->actionIsosurface->isChecked())
            return;
        const QSignalBlocker b1(ui->actionIsosurface);
        const QSignalBlocker b2(ui->actionVolume);
        ui->actionIsosurface->setChecked(wantIsosurface);
        ui->actionVolume->setChecked(!wantIsosurface);
        this->updateVolumeModeVisibility(!wantIsosurface);
        this->changeCubeRender();
    });
    QObject::connect(ui->actionIsosurface, &QAction::toggled, this, [this](bool checked) {
        if (auto *c = m_uiAssembler->renderModeCombo(); checked && c && c->currentIndex() != 0) {
            const QSignalBlocker b(c);
            c->setCurrentIndex(0);
        }
        if (checked)
            this->updateVolumeModeVisibility(false);
    });
    QObject::connect(ui->actionVolume, &QAction::toggled, this, [this](bool checked) {
        if (auto *c = m_uiAssembler->renderModeCombo(); checked && c && c->currentIndex() != 1) {
            const QSignalBlocker b(c);
            c->setCurrentIndex(1);
        }
        if (checked)
            this->updateVolumeModeVisibility(true);
    });

    // ── Volume mode combo → apply blend mode ──────────────────────────────────
    QObject::connect(m_uiAssembler->volumeModeCombo(), &SegmentedToggle::currentIndexChanged,
                     this, [this](int idx) {
        this->applyVolumeBlendMode(idx);
        const QSignalBlocker b1(this->m_actionComposite);
        const QSignalBlocker b2(this->m_actionMIP);
        const QSignalBlocker b3(this->m_actionMinIP);
        if (this->m_actionComposite) this->m_actionComposite->setChecked(idx == 0);
        if (this->m_actionMIP)       this->m_actionMIP->setChecked(idx == 1);
        if (this->m_actionMinIP)     this->m_actionMinIP->setChecked(idx == 2);
    });

    // ── LUT combo → apply 3D volume color map ────────────────────────────────
    QObject::connect(m_uiAssembler->lutCombo(), &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applyColorMapByName(name);
    });

    // ── Bidirectional sync: 2D mode toggle ↔ actionSlice / actionMomentMap ───
    QObject::connect(m_uiAssembler->combo2dMode(), &SegmentedToggle::currentIndexChanged,
                     this, [this](int idx) {
        const bool wantSlice = (idx == 0);
        if (wantSlice == ui->actionSlice->isChecked())
            return;
        if (wantSlice) {
            const QSignalBlocker b1(ui->actionSlice);
            const QSignalBlocker b2(ui->actionMomentMap);
            ui->actionSlice->setChecked(true);
            ui->actionMomentMap->setChecked(false);
            this->update2dModeVisibility(true);
            this->changeImageRenderer();
        } else {
            this->requestMomentMode();
        }
    });
    QObject::connect(ui->actionSlice, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            m_uiAssembler->sync2dModeSidebar(true);
            this->update2dModeVisibility(true);
        }
    });
    QObject::connect(ui->actionMomentMap, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            m_uiAssembler->sync2dModeSidebar(false);
            this->update2dModeVisibility(false);
        }
    });

    // ── Slice LUT combo → apply slice color map ───────────────────────────────
    QObject::connect(m_uiAssembler->sliceLutCombo(), &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applySliceLutByName(name);
    });

    // ── Moment LUT combo → apply moment color map ─────────────────────────────
    QObject::connect(m_uiAssembler->momentLutCombo(), &QComboBox::currentTextChanged,
                     this, [this](const QString &name) {
        this->applyMomentLutByName(name);
    });

    // ── Advanced LUT editor buttons ───────────────────────────────────────────
    // 3D volume (inside m_colorMapCard — hidden automatically in Isosurface mode)
    QObject::connect(m_uiAssembler->lutEdit3dButton(), &QToolButton::clicked,
                     this, &vtkWindowCube::showLUTCustomizer3D);
    // 2D slice (inside m_sliceLutCard — hidden automatically in Moment mode)
    QObject::connect(m_uiAssembler->lutEditSliceButton(), &QToolButton::clicked,
                     this, &vtkWindowCube::showLUTCustomizer);
    // 2D moment (inside m_momentLutCard — hidden automatically in Slice mode)
    QObject::connect(m_uiAssembler->lutEditMomentButton(), &QToolButton::clicked,
                     this, &vtkWindowCube::showLUTCustomizer);

    // View menu toggle action — now toggles the right-hand Inspector dock
    // (wired to it in setupInspector(), which runs after the dock exists).
    this->actionShowControlsSidebar = new QAction(u"Inspector Panel"_s, this);
    this->actionShowControlsSidebar->setCheckable(true);
    this->actionShowControlsSidebar->setChecked(true);
}

void vtkWindowCube::setupWorkspaceDocks()
{
    // Allow docks to be nested and tabbed for maximum layout flexibility.
    this->setDockNestingEnabled(true);
    this->setDockOptions(QMainWindow::AllowNestedDocks
                         | QMainWindow::AllowTabbedDocks
                         | QMainWindow::AnimatedDocks);

    // Remove the viewer container (vtkCube + vtkImage) from the central layout.
    // The VTK widgets will be reparented into dock widgets below; the pointer
    // values (ui->vtkCube, ui->vtkImage) remain valid and all rendering code
    // continues to work without any changes.
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->viewerContainer);
    ui->viewerContainer->hide();

    // The central widget now hosts the 3D/2D pane splitter (added below) — it is
    // the CENTRAL widget and must expand to take the residual space, with the
    // Session/Inspector docks flanking it. (Do NOT constrain it to Maximum, or
    // an empty/thin central steals priority from the docks.)
    ui->centralwidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Shared dock stylesheet. The title-bar text color tracks the theme so the
    // section labels stay legible whether the app shell is dark or light.
    const QString dockStyle =
            QStringLiteral(
                    "QDockWidget {"
                    "  border: 1px solid rgba(4, 138, 191, 0.22);"
                    "}"
                    "QDockWidget::title {"
                    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "      stop:0 rgba(4,138,191,0.18), stop:1 rgba(4,138,191,0.06));"
                    "  color: %1;"
                    "  font-size: 10px;"
                    "  font-weight: 600;"
                    "  text-transform: uppercase;"
                    "  padding: 5px 10px;"
                    "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
                    "}"
                    "QDockWidget::close-button, QDockWidget::float-button {"
                    "  border: none;"
                    "  background: transparent;"
                    "  subcontrol-position: top right;"
                    "  subcontrol-origin: margin;"
                    "  top: 3px;"
                    "}"
                    "QDockWidget::close-button:hover, QDockWidget::float-button:hover {"
                    "  background: rgba(4, 138, 191, 0.25);"
                    "  border-radius: 3px;"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

    // ── 3D View pane (goes into the central splitter, not a dock — design #1d)
    // The 3D and 2D views are the CENTRAL widget so QMainWindow gives them the
    // residual space; Session Data / Inspector are the only two docks flanking
    // them. (Views-as-docks + an empty central widget squeezed the viewports.)
    //
    // The 3D pane is a QStackedWidget so the user can flip between the local
    // QVTKOpenGLNativeWidget pipeline and a server-rendered JPEG stream.
    // ui->vtkCube becomes page 0 (Local); page 1 (Remote) is a container with a
    // dismissible banner above the RemoteRenderWidget.
    this->m_renderStack = new QStackedWidget(this);

    // Local page (0): wrap ui->vtkCube in a container so we can stack a
    // soft "Remote rendering recommended" banner on top when the cube
    // is big enough that the local preview is just a context placeholder
    // and full-quality should come from the V100 stream. The banner
    // stays hidden for cubes that fit locally.
    auto *localContainer = new QWidget(this->m_renderStack);
    auto *localLayout = new QVBoxLayout(localContainer);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(0);

    this->m_localRecommendRemoteBanner = new QFrame(localContainer);
    this->m_localRecommendRemoteBanner->setObjectName(
            u"localRecommendRemoteBanner"_s);
    // Same amber palette as the existing remote banner — visual
    // consistency cue that both relate to the Remote rendering mode.
    this->m_localRecommendRemoteBanner->setStyleSheet(
            u"QFrame#localRecommendRemoteBanner {"
            u"  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
            u"      stop:0 rgba(245, 158, 11, 0.22),"
            u"      stop:1 rgba(217, 119, 6, 0.16));"
            u"  border-bottom: 1px solid rgba(217, 119, 6, 0.40);"
            u"}"
            u"QFrame#localRecommendRemoteBanner QLabel {"
            u"  color: #f3f4f6; padding: 4px 8px;"
            u"}"
            u"QFrame#localRecommendRemoteBanner QPushButton {"
            u"  background: rgba(255, 255, 255, 0.14);"
            u"  border: 1px solid rgba(255, 255, 255, 0.25);"
            u"  border-radius: 3px; color: #f3f4f6;"
            u"  padding: 2px 10px; font-weight: 600;"
            u"}"
            u"QFrame#localRecommendRemoteBanner QPushButton:hover {"
            u"  background: rgba(255, 255, 255, 0.22);"
            u"}"
            u"QFrame#localRecommendRemoteBanner QToolButton {"
            u"  border: none; color: #f3f4f6; padding: 0 8px; font-weight: 600;"
            u"}"
            u"QFrame#localRecommendRemoteBanner QToolButton:hover {"
            u"  background: rgba(255, 255, 255, 0.10); border-radius: 3px;"
            u"}"_s);
    auto *recLayout = new QHBoxLayout(this->m_localRecommendRemoteBanner);
    recLayout->setContentsMargins(8, 2, 4, 2);
    recLayout->setSpacing(8);
    this->m_localRecommendRemoteLabel =
            new QLabel(u""_s, this->m_localRecommendRemoteBanner);
    recLayout->addWidget(this->m_localRecommendRemoteLabel, 1);
    this->m_localRecommendRemoteSwitchBtn = new QPushButton(
            u"Switch to Remote"_s, this->m_localRecommendRemoteBanner);
    this->m_localRecommendRemoteSwitchBtn->setToolTip(
            u"Render the cube on the backend GPU and stream it here — "
            u"full quality without downloading the full data."_s);
    recLayout->addWidget(this->m_localRecommendRemoteSwitchBtn);
    auto *dismissBtn =
            new QToolButton(this->m_localRecommendRemoteBanner);
    dismissBtn->setText(u"✕"_s);
    dismissBtn->setToolTip(u"Dismiss this suggestion"_s);
    recLayout->addWidget(dismissBtn);
    this->m_localRecommendRemoteBanner->setVisible(false);
    localLayout->addWidget(this->m_localRecommendRemoteBanner);
    localLayout->addWidget(ui->vtkCube, 1);

    QObject::connect(this->m_localRecommendRemoteSwitchBtn,
                     &QPushButton::clicked, this, [this]() {
        this->applyRenderMode(RenderMode::Remote, true);
    });
    QObject::connect(dismissBtn, &QToolButton::clicked, this, [this]() {
        this->m_localRecommendRemoteBanner->setVisible(false);
    });

    this->m_renderStack->addWidget(localContainer);   // page 0 — Local

    this->m_remoteRenderContainer = new QWidget(this->m_renderStack);
    auto *remoteLayout = new QVBoxLayout(this->m_remoteRenderContainer);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(0);

    // Banner: amber strip with a label + close button. The close ("×")
    // sends the user back to Local rendering; we do NOT delete the
    // session — switching back to Remote later reuses it without paying
    // the EGL warm-up cost again.
    auto *banner = new QFrame(this->m_remoteRenderContainer);
    banner->setObjectName(u"remoteRenderBanner"_s);
    banner->setStyleSheet(
            u"QFrame#remoteRenderBanner {"
            u"  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
            u"      stop:0 rgba(245, 158, 11, 0.22),"
            u"      stop:1 rgba(217, 119, 6, 0.16));"
            u"  border-bottom: 1px solid rgba(217, 119, 6, 0.40);"
            u"}"
            u"QFrame#remoteRenderBanner QLabel { color: #f3f4f6; padding: 4px 8px; }"
            u"QFrame#remoteRenderBanner QToolButton {"
            u"  border: none; color: #f3f4f6; padding: 0 8px; font-weight: 600;"
            u"}"
            u"QFrame#remoteRenderBanner QToolButton:hover {"
            u"  background: rgba(255, 255, 255, 0.10); border-radius: 3px;"
            u"}"_s);
    auto *bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(8, 2, 4, 2);
    bannerLayout->setSpacing(8);
    this->m_remoteRenderBannerLabel = new QLabel(
            u"Remote rendering (GPU on backend) — click ✕ to switch to Local"_s,
            banner);
    bannerLayout->addWidget(this->m_remoteRenderBannerLabel, 1);
    this->m_remoteRenderBannerClose = new QToolButton(banner);
    this->m_remoteRenderBannerClose->setText(u"✕"_s);
    this->m_remoteRenderBannerClose->setToolTip(u"Switch to Local rendering"_s);
    bannerLayout->addWidget(this->m_remoteRenderBannerClose);
    remoteLayout->addWidget(banner);

    this->m_remoteRenderWidget = new RemoteRenderWidget(this->m_remoteRenderContainer);
    remoteLayout->addWidget(this->m_remoteRenderWidget, 1);
    // §5.1b: watch the remote 3D surface so a press on it activates its pane and the
    // activating gesture is swallowed — the same click-to-activate rule the local VTK
    // widgets get (the filter runs before the widget's own mouse handlers).
    this->m_remoteRenderWidget->installEventFilter(this);

    this->m_renderStack->addWidget(this->m_remoteRenderContainer);  // page 1 — Remote
    this->m_renderStack->setCurrentIndex(0);

    Q_UNUSED(dockStyle);   // dock chrome no longer used for the view panes

    // ── Central pane splitter: 3D (left) + 2D (right) ────────────────────────
    // Reusing ui->centralwidget's layout keeps the views as the CENTRAL widget
    // (residual space) instead of leaving an empty central that competes with
    // the docks.
    this->m_paneSplitter = new QSplitter(Qt::Horizontal, ui->centralwidget);
    this->m_paneSplitter->setObjectName(u"paneSplitter"_s);
    this->m_paneSplitter->setChildrenCollapsible(false);
    this->m_paneSplitter->addWidget(this->m_renderStack);   // 3D pane
    this->m_paneSplitter->addWidget(ui->vtkImage);          // 2D pane
    this->m_paneSplitter->setStretchFactor(0, 1);
    this->m_paneSplitter->setStretchFactor(1, 1);
    mainLayout->addWidget(this->m_paneSplitter);
    // dockCubeView / dockSliceView intentionally stay null — the views are no
    // longer docks; applyPaneLayout()/resetWorkspaceLayout() drive the splitter.

    // Equalise the two panes once the splitter has a real width.
    QTimer::singleShot(0, this, [this]() {
        if (!this->m_paneSplitter) return;
        const int w = std::max(400, this->m_paneSplitter->width());
        this->m_paneSplitter->setSizes({ w / 2, w / 2 });
    });
}

void vtkWindowCube::resetWorkspaceLayout()
{
    // §3.1: "Reset layout" re-applies the current pane count through the slot model
    // (equalising the splitters) — it no longer pokes individual frames.
    if (!this->m_paneSplitter)
        return;
    this->applyPaneLayout(this->m_paneCount);
    const int w = std::max(400, this->m_paneSplitter->width());
    this->m_paneSplitter->setSizes({ w / 2, w / 2 });
}

void vtkWindowCube::setupRenderModeUi()
{
    // Status-bar pill — created here so updateRenderModeBadges() can rely
    // on it existing immediately even before the first mode switch.
    this->m_renderModeBadge = new QLabel(u"Render: LOCAL"_s, this);
    this->m_renderModeBadge->setStyleSheet(
            u"QLabel {"
            u"  padding: 1px 8px; margin-right: 6px;"
            u"  border-radius: 9px;"
            u"  background: rgba(120, 120, 120, 0.18);"
            u"  color: palette(window-text);"
            u"  font-size: 10px; font-weight: 600;"
            u"  text-transform: uppercase; letter-spacing: 0.04em;"
            u"}"_s);
    this->statusBar()->addPermanentWidget(this->m_renderModeBadge);

    this->m_renderFpsBadge = new QLabel(QString(), this);
    this->m_renderFpsBadge->setStyleSheet(
            u"QLabel { padding: 1px 6px; color: palette(window-text); }"_s);
    this->m_renderFpsBadge->hide();   // only visible while in Remote mode
    this->statusBar()->addPermanentWidget(this->m_renderFpsBadge);

    // View → Rendering submenu.
    auto *renderMenu = ui->menuView->addMenu(u"Rendering"_s);
    auto *renderGroup = new QActionGroup(this);
    renderGroup->setExclusive(true);

    this->m_actionRenderAuto = renderMenu->addAction(u"Auto"_s);
    this->m_actionRenderAuto->setCheckable(true);
    this->m_actionRenderAuto->setChecked(true);
    this->m_actionRenderAuto->setToolTip(
            u"Pick Local or Remote based on dataset size and backend support"_s);
    renderGroup->addAction(this->m_actionRenderAuto);

    this->m_actionRenderLocal = renderMenu->addAction(u"Local (CPU/GPU on this machine)"_s);
    this->m_actionRenderLocal->setCheckable(true);
    renderGroup->addAction(this->m_actionRenderLocal);

    this->m_actionRenderRemote = renderMenu->addAction(u"Remote (GPU on backend)"_s);
    this->m_actionRenderRemote->setCheckable(true);
    this->m_actionRenderRemote->setToolTip(
            u"Stream JPEG frames from an EGL-equipped backend over WebSocket"_s);
    renderGroup->addAction(this->m_actionRenderRemote);

    QObject::connect(this->m_actionRenderAuto, &QAction::triggered, this,
                     [this]() { this->applyRenderMode(RenderMode::Auto, true); });
    QObject::connect(this->m_actionRenderLocal, &QAction::triggered, this,
                     [this]() { this->applyRenderMode(RenderMode::Local, true); });
    QObject::connect(this->m_actionRenderRemote, &QAction::triggered, this,
                     [this]() { this->applyRenderMode(RenderMode::Remote, true); });

    // Banner close ("✕") → drop back to Local without tearing the session.
    if (this->m_remoteRenderBannerClose) {
        QObject::connect(this->m_remoteRenderBannerClose, &QToolButton::clicked, this, [this]() {
            this->applyRenderMode(RenderMode::Local, true);
        });
    }

    // RemoteRenderWidget → status-bar wiring. Safe to connect now even
    // before any session exists; the widget just stays silent.
    if (this->m_remoteRenderWidget) {
        QObject::connect(this->m_remoteRenderWidget,
                         &RemoteRenderWidget::framesPerSecondChanged,
                         this, &vtkWindowCube::onRemoteRenderFpsChanged);
        QObject::connect(this->m_remoteRenderWidget,
                         &RemoteRenderWidget::connectionStateChanged,
                         this, &vtkWindowCube::onRemoteRenderConnectionStateChanged);
    }

    // Soft recommendation — DO NOT auto-switch. The local preview is
    // already useful and the user might be happy with it; we just
    // surface the option to switch to remote via a non-intrusive
    // banner on the Local page. The banner exposes a button that does
    // the actual mode change when the user decides. This trades a
    // 150-ms auto-switch (used to feel pushy on medium cubes) for a
    // calmer, opt-in flow that respects whatever the user is already
    // looking at.
    const QString autoWorkDir = QDir::home().absoluteFilePath(qApp->applicationName());
    Settings autoSettings(autoWorkDir);
    const double cubeFullMb = (static_cast<double>(this->remoteDatasetWidth)
                               * static_cast<double>(this->remoteDatasetHeight)
                               * static_cast<double>(this->remoteDatasetDepth)
                               * 4.0) / (1024.0 * 1024.0);
    const int    thresholdMb = autoSettings.getLocalFullThresholdMb();
    const bool   sizeWantsRemote = cubeFullMb > static_cast<double>(thresholdMb);
    const bool   backendWantsRemote =
            (this->recommendedLoadMode == QLatin1String("preview_only"));
    qDebug().noquote() << QStringLiteral(
            "[remote-render] auto-decision cube=%1MB threshold=%2MB "
            "backend_hint=%3 -> recommendRemote=%4")
                                  .arg(cubeFullMb, 0, 'f', 1)
                                  .arg(thresholdMb)
                                  .arg(this->recommendedLoadMode)
                                  .arg((sizeWantsRemote || backendWantsRemote) ? "yes" : "no");
    if ((sizeWantsRemote || backendWantsRemote)
        && !this->remoteBackendUrl.isEmpty()
        && this->m_localRecommendRemoteBanner
        && this->m_localRecommendRemoteLabel) {
        const double sizeGb = cubeFullMb / 1024.0;
        QString reason;
        if (backendWantsRemote) {
            reason = u"backend tagged this cube as too large for a local "
                     "full-resolution load."_s;
        } else {
            reason = QStringLiteral(
                    "cube is %1 GB at full resolution (threshold %2 MB).")
                     .arg(sizeGb, 0, 'f', 2)
                     .arg(thresholdMb);
        }
        this->m_localRecommendRemoteLabel->setText(
                u"Remote rendering recommended — " + reason
                + u" Click ⮕ for full-quality interactive 3D without the transfer."_s);
        this->m_localRecommendRemoteBanner->setVisible(true);
    }

    this->updateRenderModeBadges();
}

void vtkWindowCube::setupCubeRenderer()
{
    vtkNew<vtkRenderer> ren;
    ren->SetBackground(0.0, 0.0, 0.0157);   // #000004 — scientific colormaps need black (VisivoTheme.h)

    vtkNew<vtkGenericOpenGLRenderWindow> win;
    win->AddRenderer(ren);
    ui->vtkCube->setRenderWindow(win);
    ui->vtkCube->setEnableTouchEventProcessing(false);

    // Isosurface
    this->isosurfaceFilter->SetInputConnection(this->cubeDisplaySource->GetOutputPort());
    this->isosurfaceFilter->SetValue(0, this->lowerBound);
    vtkNew<vtkPolyDataMapper> isosurfaceMapper;
    isosurfaceMapper->SetInputConnection(this->isosurfaceFilter->GetOutputPort());
    isosurfaceMapper->ScalarVisibilityOff();
    this->isosurface->SetMapper(isosurfaceMapper);
    this->isosurface->GetProperty()->SetColor(1., 0.5, 1.);
    // Isosurface visibility managed by scheduleIsosurfaceRecompute / setCubeRenderModeLocally

    // Volume
    const auto cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    const double cubeRange[2] = { this->currentCubeVisibleRange[0], this->currentCubeVisibleRange[1] };
    vtkNew<vtkLookupTable> lutVolume;
    lutVolume->SetTableRange(cubeRange);
    ColorMaps::SetColorMap(lutVolume);
    ColorMaps::SetColorTransferFunction(lutVolume, this->volumeColorTransferFunction);
    this->volumeColorTransferFunction->SetObjectName(lutVolume->GetObjectName());
    // Opacity transfer function. Previous defaults (5% at threshold, 30% at
    // max) made the volume rendering near-invisible — even the brightest
    // peaks topped out at 30% opacity, indistinguishable from background.
    // The new ramp goes:
    //     sentinel / min  → 0.0    (fully transparent: blanks + below-noise)
    //     lowerBound      → 0.12   (faint emission becomes visible)
    //     midpoint        → 0.45   (mid-range structures stand out)
    //     max             → 0.85   (peaks read clearly without saturating)
    // This produces a much more legible default render for HI / molecular
    // cubes whose emission is concentrated in the lower half of the range.
    // Users can still customise via the LUT editor (3D View → Advanced…).
    const double midpoint = 0.5 * (this->lowerBound + cubeRange[1]);
    this->volumeOpacity->RemoveAllPoints();
    this->volumeOpacity->AddPoint(this->currentCubeInvisibleSentinel, 0.0);
    this->volumeOpacity->AddPoint(cubeRange[0],    0.0);
    this->volumeOpacity->AddPoint(this->lowerBound, 0.12);
    this->volumeOpacity->AddPoint(midpoint,         0.45);
    this->volumeOpacity->AddPoint(cubeRange[1],     0.85);
    vtkNew<vtkVolumeProperty> volumeProperty;
    volumeProperty->SetColor(this->volumeColorTransferFunction);
    volumeProperty->SetScalarOpacity(this->volumeOpacity);
    volumeProperty->SetInterpolationTypeToLinear();
    vtkNew<vtkGPUVolumeRayCastMapper> volumeMapper;
    volumeMapper->SetInputConnection(this->cubeDisplaySource->GetOutputPort());
    volumeMapper->SetBlendModeToComposite(); // default
    // Lower sample density during interaction so frames stay responsive
    // immediately after a full-resolution swap (when GPU caches are cold).
    volumeMapper->SetAutoAdjustSampleDistances(1);
    volumeMapper->SetUseJittering(1);
    this->volumeMapper = volumeMapper;       // keep reference for blend-mode changes
    this->volume->SetMapper(volumeMapper);
    this->volume->SetProperty(volumeProperty);
    // By default, we show the isosurface

    // Outline
    vtkNew<vtkOutlineFilter> outline;
    outline->SetInputConnection(this->cubeDisplaySource->GetOutputPort());
    vtkNew<vtkPolyDataMapper> outlineMapper;
    outlineMapper->SetInputConnection(outline->GetOutputPort());
    vtkNew<vtkActor> outlineActor;
    outlineActor->SetMapper(outlineMapper);
    ren->AddViewProp(outlineActor);

    // ── Noise-region preview cube (3D) ────────────────────────────────────
    // Semi-transparent amber box inside the volume that mirrors the slice's
    // (x0..x1, y0..y1) rectangle and extrudes it along Z over the user's
    // chosen channel range. Two actors share the same cube source: a
    // filled-face actor at low opacity (translucent fill) and a wireframe
    // actor at higher opacity (crisp edges). Both default to hidden — they
    // get bounds and visibility from setNoiseRegionPreview() while the
    // NoiseRegionDialog is open and through the result dialog's lifetime.
    {
        vtkNew<vtkPolyDataMapper> faceMapper;
        faceMapper->SetInputConnection(this->noisePreviewCubeSource->GetOutputPort());
        faceMapper->ScalarVisibilityOff();
        this->noisePreviewCubeFaceActor->SetMapper(faceMapper);
        this->noisePreviewCubeFaceActor->GetProperty()->SetColor(0.95, 0.65, 0.15);
        this->noisePreviewCubeFaceActor->GetProperty()->SetOpacity(0.18);
        this->noisePreviewCubeFaceActor->GetProperty()->SetRepresentationToSurface();
        this->noisePreviewCubeFaceActor->GetProperty()->LightingOff();
        this->noisePreviewCubeFaceActor->PickableOff();
        this->noisePreviewCubeFaceActor->VisibilityOff();
        ren->AddViewProp(this->noisePreviewCubeFaceActor);

        vtkNew<vtkPolyDataMapper> edgeMapper;
        edgeMapper->SetInputConnection(this->noisePreviewCubeSource->GetOutputPort());
        edgeMapper->ScalarVisibilityOff();
        this->noisePreviewCubeEdgeActor->SetMapper(edgeMapper);
        this->noisePreviewCubeEdgeActor->GetProperty()->SetColor(0.95, 0.65, 0.15);
        this->noisePreviewCubeEdgeActor->GetProperty()->SetOpacity(0.95);
        this->noisePreviewCubeEdgeActor->GetProperty()->SetRepresentationToWireframe();
        this->noisePreviewCubeEdgeActor->GetProperty()->SetLineWidth(2.0);
        this->noisePreviewCubeEdgeActor->GetProperty()->LightingOff();
        this->noisePreviewCubeEdgeActor->PickableOff();
        this->noisePreviewCubeEdgeActor->VisibilityOff();
        ren->AddViewProp(this->noisePreviewCubeEdgeActor);
    }

    {
        // The cutting plane shows the actual 2D slice content textured onto
        // the plane in the 3D view (instead of a plain green fill). The
        // texture pipeline reuses sliceColors → it updates automatically
        // whenever a new slice tile arrives via remoteSliceDisplaySource.
        // vtkPlaneSource emits texture coordinates (origin→(0,0), point1→
        // (1,0), point2→(0,1)) that match the slice image orientation.
        vtkNew<vtkPolyDataMapper> planeMapper;
        planeMapper->SetInputConnection(this->remoteCuttingPlaneSource->GetOutputPort());
        planeMapper->ScalarVisibilityOff();
        this->remoteCuttingPlaneActor->SetMapper(planeMapper);

        // The slice texture is attached AFTER setupSliceRenderer() finishes
        // (see the ctor, right after this->setupMomentRenderer()). Here we
        // only configure the plane's solid-colour fallback: a translucent
        // white surface with a red outline so the plane is still identifiable
        // before the first slice tile arrives.
        this->remoteCuttingPlaneActor->GetProperty()->SetColor(1.0, 1.0, 1.0);
        this->remoteCuttingPlaneActor->GetProperty()->SetOpacity(0.5);
        this->remoteCuttingPlaneActor->GetProperty()->EdgeVisibilityOn();
        this->remoteCuttingPlaneActor->GetProperty()->SetEdgeColor(1.0, 0.0, 0.0);
        this->remoteCuttingPlaneActor->GetProperty()->SetLineWidth(3.0);
        this->remoteCuttingPlaneActor->GetProperty()->LightingOff();
        this->remoteCuttingPlaneActor->GetProperty()->BackfaceCullingOff();
        this->remoteCuttingPlaneActor->GetProperty()->FrontfaceCullingOff();
        this->remoteCuttingPlaneActor->VisibilityOn();
        ren->AddViewProp(this->remoteCuttingPlaneActor);
        qDebug().noquote()
                << QStringLiteral("[remote-plane] actor added renderer=%1 actor=%2 visible=%3")
                           .arg(reinterpret_cast<quintptr>(ren.GetPointer()), 0, 16)
                           .arg(reinterpret_cast<quintptr>(this->remoteCuttingPlaneActor.GetPointer()),
                                0, 16)
                           .arg(this->remoteCuttingPlaneActor->GetVisibility());
    }

    // Slice overlay in cube renderer not used in backend mode; cutting plane actor serves this role.

    // Axes
    vtkNew<vtkAxesActor> axes;
    this->axesWidget->SetOrientationMarker(axes);
    this->axesWidget->SetInteractor(win->GetInteractor());
    this->axesWidget->SetEnabled(1);
    this->axesWidget->InteractiveOff();

    // ── Loading placeholder text actor ────────────────────────────────────────
    // Shown until the first real volume preview arrives from the backend.
    // Hidden (VisibilityOff) in remotePreviewWatcher::finished.
    {
        auto *prop = this->m_cube3dLoadingActor->GetTextProperty();
        prop->SetFontSize(16);
        double r, g, b;
        ThemeManager::instance().vtkTextColor(r, g, b);
        prop->SetColor(r, g, b);
        prop->SetJustificationToCentered();
        prop->SetVerticalJustificationToCentered();
        prop->BoldOff();
        prop->ShadowOff();
        this->m_cube3dLoadingActor->SetInput("Loading 3D view\xe2\x80\xa6");  // UTF-8 ellipsis
        this->m_cube3dLoadingActor->GetPositionCoordinate()
                ->SetCoordinateSystemToNormalizedViewport();
        this->m_cube3dLoadingActor->SetPosition(0.5, 0.5);
        this->m_cube3dLoadingActor->VisibilityOn();
        ren->AddViewProp(this->m_cube3dLoadingActor);

        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                         this, [this](EffectiveTheme) {
                             double tr, tg, tb;
                             ThemeManager::instance().vtkTextColor(tr, tg, tb);
                             this->m_cube3dLoadingActor->GetTextProperty()->SetColor(tr, tg, tb);
                         });
    }

    ren->ResetCamera();
    win->Render();

    ren->GetActiveCamera()->GetPosition(this->initialCameraPosition);
    ren->GetActiveCamera()->GetFocalPoint(this->initialCameraFocalPoint);
}

void vtkWindowCube::setupSliceRenderer()
{
    this->sliceWcsOverlayInitialized = false;
    vtkNew<vtkRenderer> ren;
    ren->SetBackground(0.0, 0.0, 0.0157);   // #000004 — scientific colormaps need black (VisivoTheme.h)
    ren->GetActiveCamera()->ParallelProjectionOn();

    this->sliceWin->AddRenderer(ren);
    ui->vtkImage->setRenderWindow(this->sliceWin);
    ui->vtkImage->setEnableTouchEventProcessing(false);

    vtkNew<vtkInteractorStyleImage> style;
    this->sliceWin->GetInteractor()->SetInteractorStyle(style);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::MouseMoveEvent, this,
                                                 &vtkWindowCube::mouseCallback);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::LeftButtonPressEvent, this,
                                                 &vtkWindowCube::toggleProbeFreeze);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                                                 &vtkWindowCube::finishRegionInteraction);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::RightButtonPressEvent, this,
                                                 &vtkWindowCube::finalizePvInteraction);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, this,
                                                 &vtkWindowCube::finalizePvInteraction);
    // STEP6-P1: the 2-D slice participates in the Linked-2D camera. InteractionEvent follows live
    // pan (drag); EndInteractionEvent catches mouse-wheel zoom (no InteractionEvent). The handler
    // early-returns unless Linked·camera is on, so this is free otherwise.
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::InteractionEvent, this,
                                                 &vtkWindowCube::onSliceCameraInteraction);
    this->sliceWin->GetInteractor()->AddObserver(vtkCommand::EndInteractionEvent, this,
                                                 &vtkWindowCube::onSliceCameraInteraction);
    this->coordinate->SetCoordinateSystemToDisplay();
    this->coordinate->SetViewport(ren);

    // Slice — remote backend always provides slice tiles; initialize with a placeholder.
    this->lutSlice->SetTableRange(0., 0.);
    // Blank (NaN) pixels — masked / removed / off-footprint — render TRANSPARENT so
    // they read as empty background, not an opaque white silhouette that looks like
    // leftover data (matters e.g. for a Kinematic-Lasso "Remove" residual cube).
    this->lutSlice->SetNanColor(0., 0., 0., 0.);
    ColorMaps::SetColorMap(this->lutSlice);
    {
        auto *img = vtkImageData::SafeDownCast(this->remoteSliceDisplaySource->GetOutputDataObject(0));
        if (img) {
            this->sliceColors->SetInputData(img);
        } else {
            this->sliceColors->SetInputData(createPlaceholderImageData());
        }
    }
    this->sliceColors->SetLookupTable(this->lutSlice);
    vtkNew<vtkImageSliceMapper> sliceMapper;
    sliceMapper->SetInputConnection(this->sliceColors->GetOutputPort());
    sliceMapper->BorderOn();
    vtkNew<vtkImageSlice> sliceActor;
    sliceActor->SetMapper(sliceMapper);
    sliceActor->GetProperty()->SetInterpolationTypeToNearest();
    ren->AddViewProp(sliceActor);

    // Color bar — parked in the right gutter, laid out by layout2dPane().
    this->m_sliceColorbar->SetLookupTable(this->lutSlice);
    CubeWcs::applyOverlayFont(this->m_sliceColorbar->GetLabelTextProperty());   // §3.0c #8
    ren->AddViewProp(this->m_sliceColorbar);
    // Placeholder layout; layout2dPaneFor(slice) below re-lays it with the real data area.
    CubeGeom::layoutColorbarInGutter(this->m_sliceColorbar, ren,
                                     this->gutter2dFor(/*slice=*/true), CubeGeom::ImageRect{},
                                     this->dprFor(/*slice=*/true));

    // Contours
    this->contours->SetInputConnection(this->remoteSliceDisplaySource->GetOutputPort());
    this->contours->GenerateValues(this->level, this->lowerBound, this->upperBound);
    vtkNew<vtkPolyDataMapper> contoursMapper;
    contoursMapper->SetInputConnection(this->contours->GetOutputPort());
    contoursMapper->SetScalarRange(this->lowerBound, this->upperBound);
    this->contoursActor->SetMapper(contoursMapper);
    this->contoursActor->VisibilityOff();
    ren->AddViewProp(this->contoursActor);

    // Legend: local AstroUtils-based legend not used in backend mode; remote WCS overlay actors are used instead.
    ren->AddViewProp(this->sliceOverlayXAxis);
    ren->AddViewProp(this->sliceOverlayYAxis);
    ren->AddViewProp(this->sliceOverlayXTitleActor);
    ren->AddViewProp(this->sliceOverlayYTitleActor);
    vtkNew<vtkPolyDataMapper> sliceProbeHorizontalMapper;
    sliceProbeHorizontalMapper->SetInputConnection(this->sliceProbeHorizontalLine->GetOutputPort());
    this->sliceProbeHorizontalActor->SetMapper(sliceProbeHorizontalMapper);
    this->sliceProbeHorizontalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->sliceProbeHorizontalActor->GetProperty()->SetLineWidth(1.5);
    this->sliceProbeHorizontalActor->VisibilityOff();
    ren->AddActor(this->sliceProbeHorizontalActor);
    vtkNew<vtkPolyDataMapper> sliceProbeVerticalMapper;
    sliceProbeVerticalMapper->SetInputConnection(this->sliceProbeVerticalLine->GetOutputPort());
    this->sliceProbeVerticalActor->SetMapper(sliceProbeVerticalMapper);
    this->sliceProbeVerticalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->sliceProbeVerticalActor->GetProperty()->SetLineWidth(1.5);
    this->sliceProbeVerticalActor->VisibilityOff();
    ren->AddActor(this->sliceProbeVerticalActor);
    this->catalogueOverlayData->SetPoints(this->catalogueOverlayPoints);
    this->catalogueOverlayData->SetLines(this->catalogueOverlayCells);
    vtkNew<vtkPolyDataMapper> sliceCatalogueOverlayMapper;
    sliceCatalogueOverlayMapper->SetInputData(this->catalogueOverlayData);
    this->sliceCatalogueOverlayActor->SetMapper(sliceCatalogueOverlayMapper);
    this->sliceCatalogueOverlayActor->GetProperty()->SetColor(1.0, 0.45, 0.15);
    this->sliceCatalogueOverlayActor->GetProperty()->SetLineWidth(1.5);
    this->sliceCatalogueOverlayActor->VisibilityOff();
    ren->AddActor(this->sliceCatalogueOverlayActor);
    vtkNew<vtkPolyDataMapper> sliceCatalogueHoverMapper;
    sliceCatalogueHoverMapper->SetInputData(this->catalogueHoverOverlayData);
    this->sliceCatalogueHoverOverlayActor->SetMapper(sliceCatalogueHoverMapper);
    this->sliceCatalogueHoverOverlayActor->GetProperty()->SetColor(1.0, 0.95, 0.45);
    this->sliceCatalogueHoverOverlayActor->GetProperty()->SetLineWidth(2.5);
    this->sliceCatalogueHoverOverlayActor->VisibilityOff();
    ren->AddActor(this->sliceCatalogueHoverOverlayActor);
    vtkNew<vtkPolyDataMapper> sliceCatalogueSelectionMapper;
    sliceCatalogueSelectionMapper->SetInputData(this->catalogueSelectionOverlayData);
    this->sliceCatalogueSelectionOverlayActor->SetMapper(sliceCatalogueSelectionMapper);
    this->sliceCatalogueSelectionOverlayActor->GetProperty()->SetColor(0.15, 0.95, 1.0);
    this->sliceCatalogueSelectionOverlayActor->GetProperty()->SetLineWidth(3.2);
    this->sliceCatalogueSelectionOverlayActor->VisibilityOff();
    ren->AddActor(this->sliceCatalogueSelectionOverlayActor);
    this->slicePvData->SetPoints(this->slicePvPoints);
    this->slicePvData->SetLines(this->slicePvCells);
    vtkNew<vtkPolyDataMapper> slicePvMapper;
    slicePvMapper->SetInputData(this->slicePvData);
    this->slicePvActor->SetMapper(slicePvMapper);
    this->slicePvActor->GetProperty()->SetColor(1.0, 0.4, 0.1);
    this->slicePvActor->GetProperty()->SetLineWidth(2.5);
    this->slicePvActor->VisibilityOff();
    ren->AddActor(this->slicePvActor);
    vtkRenderer *sliceRenderer = ren;
    const auto configurePvBoundaryActor = [sliceRenderer](vtkPoints *points, vtkCellArray *cells,
                                                          vtkPolyData *polyData, vtkActor *actor) {
        polyData->SetPoints(points);
        polyData->SetLines(cells);
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(polyData);
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(1.0, 0.72, 0.2);
        actor->GetProperty()->SetLineWidth(1.5);
        actor->GetProperty()->SetOpacity(0.8);
        actor->VisibilityOff();
        sliceRenderer->AddActor(actor);
    };
    configurePvBoundaryActor(this->slicePvUpperPoints, this->slicePvUpperCells,
                             this->slicePvUpperData, this->slicePvUpperActor);
    configurePvBoundaryActor(this->slicePvLowerPoints, this->slicePvLowerCells,
                             this->slicePvLowerData, this->slicePvLowerActor);
    const auto configureRegionLineActor = [sliceRenderer](vtkLineSource *line, vtkActor *actor) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.2, 0.9, 1.0);
        actor->GetProperty()->SetLineWidth(2.0);
        actor->VisibilityOff();
        sliceRenderer->AddActor(actor);
    };
    configureRegionLineActor(this->sliceRegionTopLine, this->sliceRegionTopActor);
    configureRegionLineActor(this->sliceRegionBottomLine, this->sliceRegionBottomActor);
    configureRegionLineActor(this->sliceRegionLeftLine, this->sliceRegionLeftActor);
    configureRegionLineActor(this->sliceRegionRightLine, this->sliceRegionRightActor);
    // Noise-region preview rectangle: amber, slightly thicker, drawn over
    // the slice while the NoiseRegionDialog is open. Distinct colour from
    // the active Box region (cyan above) so the two never get confused.
    const auto configureNoisePreviewActor = [sliceRenderer](vtkLineSource *line, vtkActor *actor) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.95, 0.65, 0.15);  // amber
        actor->GetProperty()->SetLineWidth(2.5);
        actor->GetProperty()->SetOpacity(0.9);
        actor->VisibilityOff();
        sliceRenderer->AddActor(actor);
    };
    configureNoisePreviewActor(this->noisePreviewTopLine,    this->noisePreviewTopActor);
    configureNoisePreviewActor(this->noisePreviewBottomLine, this->noisePreviewBottomActor);
    configureNoisePreviewActor(this->noisePreviewLeftLine,   this->noisePreviewLeftActor);
    configureNoisePreviewActor(this->noisePreviewRightLine,  this->noisePreviewRightActor);
    this->sliceRegionCircleSource->SetNumberOfSides(96);
    this->sliceRegionCircleSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> sliceRegionCircleMapper;
    sliceRegionCircleMapper->SetInputConnection(this->sliceRegionCircleSource->GetOutputPort());
    this->sliceRegionCircleActor->SetMapper(sliceRegionCircleMapper);
    this->sliceRegionCircleActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionCircleActor->GetProperty()->SetLineWidth(2.0);
    this->sliceRegionCircleActor->VisibilityOff();
    ren->AddActor(this->sliceRegionCircleActor);
    this->sliceRegionAnnulusOuterSource->SetNumberOfSides(96);
    this->sliceRegionAnnulusOuterSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> sliceRegionAnnulusOuterMapper;
    sliceRegionAnnulusOuterMapper->SetInputConnection(this->sliceRegionAnnulusOuterSource->GetOutputPort());
    this->sliceRegionAnnulusOuterActor->SetMapper(sliceRegionAnnulusOuterMapper);
    this->sliceRegionAnnulusOuterActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionAnnulusOuterActor->GetProperty()->SetLineWidth(2.0);
    this->sliceRegionAnnulusOuterActor->GetProperty()->SetRepresentationToWireframe();
    this->sliceRegionAnnulusOuterActor->VisibilityOff();
    ren->AddActor(this->sliceRegionAnnulusOuterActor);
    this->sliceRegionAnnulusInnerSource->SetNumberOfSides(96);
    this->sliceRegionAnnulusInnerSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> sliceRegionAnnulusInnerMapper;
    sliceRegionAnnulusInnerMapper->SetInputConnection(this->sliceRegionAnnulusInnerSource->GetOutputPort());
    this->sliceRegionAnnulusInnerActor->SetMapper(sliceRegionAnnulusInnerMapper);
    this->sliceRegionAnnulusInnerActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionAnnulusInnerActor->GetProperty()->SetLineWidth(1.5);
    this->sliceRegionAnnulusInnerActor->GetProperty()->SetRepresentationToWireframe();
    this->sliceRegionAnnulusInnerActor->VisibilityOff();
    ren->AddActor(this->sliceRegionAnnulusInnerActor);
    vtkNew<vtkPolyDataMapper> sliceRegionAnnulusFillMapper;
    sliceRegionAnnulusFillMapper->SetInputData(this->sliceRegionAnnulusFillData);
    this->sliceRegionAnnulusFillActor->SetMapper(sliceRegionAnnulusFillMapper);
    this->sliceRegionAnnulusFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionAnnulusFillActor->GetProperty()->SetOpacity(0.18);
    this->sliceRegionAnnulusFillActor->GetProperty()->SetRepresentationToSurface();
    this->sliceRegionAnnulusFillActor->VisibilityOff();
    ren->AddActor(this->sliceRegionAnnulusFillActor);
    this->sliceRegionPolygonTriangulator->SetInputData(this->sliceRegionPolygonFillData);
    vtkNew<vtkPolyDataMapper> sliceRegionPolygonMapper;
    sliceRegionPolygonMapper->SetInputData(this->sliceRegionPolygonData);
    this->sliceRegionPolygonActor->SetMapper(sliceRegionPolygonMapper);
    this->sliceRegionPolygonActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionPolygonActor->GetProperty()->SetLineWidth(2.0);
    this->sliceRegionPolygonActor->VisibilityOff();
    ren->AddActor(this->sliceRegionPolygonActor);
    vtkNew<vtkPolyDataMapper> sliceRegionPolygonFillMapper;
    sliceRegionPolygonFillMapper->SetInputConnection(this->sliceRegionPolygonTriangulator->GetOutputPort());
    this->sliceRegionPolygonFillActor->SetMapper(sliceRegionPolygonFillMapper);
    this->sliceRegionPolygonFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->sliceRegionPolygonFillActor->GetProperty()->SetOpacity(0.18);
    this->sliceRegionPolygonFillActor->VisibilityOff();
    ren->AddActor(this->sliceRegionPolygonFillActor);
    this->sliceOverlayXAxis->GetTitleTextProperty()->SetFontSize(16);
    this->sliceOverlayXAxis->GetTitleTextProperty()->SetBold(false);
    this->sliceOverlayYAxis->GetTitleTextProperty()->SetFontSize(16);
    this->sliceOverlayYAxis->GetTitleTextProperty()->SetOrientation(90.);
    this->sliceOverlayYAxis->GetTitleTextProperty()->SetBold(false);
    this->sliceOverlayXTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->sliceOverlayXTitleActor->GetTextProperty()->SetFontSize(14);
    this->sliceOverlayXTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->sliceOverlayXTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->sliceOverlayYTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->sliceOverlayYTitleActor->GetTextProperty()->SetFontSize(14);
    this->sliceOverlayYTitleActor->GetTextProperty()->SetOrientation(90.);
    this->sliceOverlayYTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->sliceOverlayYTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    CubeWcs::applyOverlayFont(this->sliceOverlayXTitleActor->GetTextProperty());   // §3.0c #8
    CubeWcs::applyOverlayFont(this->sliceOverlayYTitleActor->GetTextProperty());
    this->ensureOverlayTickActors(ren, this->sliceOverlayXTickActors, this->sliceOverlayYTickActors);
    this->sliceWcsOverlayInitialized = true;
    this->set2dWcsOverlayVisible(this->showWcsAxes);
    this->sliceWin->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::updateSliceWcsOverlay);
    this->sliceWin->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::updateCatalogueOverlayLabels);

    // ── Loading placeholder text actor ────────────────────────────────────────
    // Shown until the first remote slice tile arrives from the backend.
    // Hidden (VisibilityOff) in applyRemoteSliceResult().
    {
        auto *prop = this->m_slice2dLoadingActor->GetTextProperty();
        prop->SetFontSize(16);
        prop->SetColor(0.72, 0.84, 0.91);   // #B8D6E8 — matches app blue-gray palette
        prop->SetJustificationToCentered();
        prop->SetVerticalJustificationToCentered();
        prop->BoldOff();
        prop->ShadowOff();
        this->m_slice2dLoadingActor->SetInput("Loading 2D view\xe2\x80\xa6");  // UTF-8 ellipsis
        this->m_slice2dLoadingActor->GetPositionCoordinate()
                ->SetCoordinateSystemToNormalizedViewport();
        this->m_slice2dLoadingActor->SetPosition(0.5, 0.5);
        this->m_slice2dLoadingActor->VisibilityOn();
        ren->AddViewProp(this->m_slice2dLoadingActor);
    }

    ren->ResetCamera();                       // keep: establishes a sane camera distance
    this->layout2dPaneFor(/*slice=*/true, true); // then letterbox it into the gutter
    this->sliceWin->Render();
}

void vtkWindowCube::setupMomentRenderer()
{
    this->momentWcsOverlayInitialized = false;
    vtkNew<vtkRenderer> ren;
    ren->SetBackground(0.0, 0.0, 0.0157);   // #000004 — scientific colormaps need black (VisivoTheme.h)
    ren->GetActiveCamera()->ParallelProjectionOn();
    this->momentWin->AddRenderer(ren);

    vtkNew<QVTKInteractor> iren;
    this->momentWin->SetInteractor(iren);
    iren->Initialize();
    iren->AddObserver(vtkCommand::MouseMoveEvent, this, &vtkWindowCube::mouseCallback);
    iren->AddObserver(vtkCommand::LeftButtonPressEvent, this, &vtkWindowCube::toggleProbeFreeze);
    iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, this, &vtkWindowCube::finishRegionInteraction);
    iren->AddObserver(vtkCommand::RightButtonPressEvent, this, &vtkWindowCube::finalizePvInteraction);
    iren->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, this, &vtkWindowCube::finalizePvInteraction);
    iren->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, this, &vtkWindowCube::finalizePvInteraction);

    vtkNew<vtkInteractorStyleImage> style;
    iren->SetInteractorStyle(style);

    if (!vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0))) {
        this->momentDisplaySource->SetOutput(createPlaceholderImageData());
    }

    const auto momentImage =
            vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0));
    if (momentImage) {
        this->lutMoment->SetTableRange(momentImage->GetScalarRange());
    } else {
        this->lutMoment->SetTableRange(0., 0.);
    }
    // Masked-out (NaN) pixels read as ABSENT, not full-scale: transparent so the
    // #000004 viewport shows through. Opaque white made the ≈3σ-masked frame read
    // as a white sheet — the max end of most scientific colormaps (§3.0b).
    this->lutMoment->SetNanColor(0., 0., 0., 0.);
    ColorMaps::SetColorMap(this->lutMoment);
    if (momentImage) {
        this->momentColors->SetInputData(momentImage);
    } else {
        this->momentColors->SetInputData(createPlaceholderImageData());
    }
    this->momentColors->SetLookupTable(this->lutMoment);
    vtkNew<vtkImageSliceMapper> momentMapper;
    momentMapper->SetInputConnection(this->momentColors->GetOutputPort());
    momentMapper->BorderOn();
    vtkNew<vtkImageSlice> momentActor;
    momentActor->SetMapper(momentMapper);
    momentActor->GetProperty()->SetInterpolationTypeToNearest();
    ren->AddViewProp(momentActor);

    // Color bar — parked in the right gutter, laid out by layout2dPane().
    this->m_momentColorbar->SetLookupTable(this->lutMoment);
    CubeWcs::applyOverlayFont(this->m_momentColorbar->GetLabelTextProperty());   // §3.0c #8
    ren->AddViewProp(this->m_momentColorbar);
    // Placeholder layout; layout2dPaneFor(moment) below re-lays it with the real data area.
    CubeGeom::layoutColorbarInGutter(this->m_momentColorbar, ren,
                                     this->gutter2dFor(/*slice=*/false), CubeGeom::ImageRect{},
                                     this->dprFor(/*slice=*/false));

    // Legend: local AstroUtils-based legend not used in backend mode; remote WCS overlay actors are used instead.
    ren->AddViewProp(this->momentOverlayXAxis);
    ren->AddViewProp(this->momentOverlayYAxis);
    ren->AddViewProp(this->momentOverlayXTitleActor);
    ren->AddViewProp(this->momentOverlayYTitleActor);
    vtkNew<vtkPolyDataMapper> momentProbeHorizontalMapper;
    momentProbeHorizontalMapper->SetInputConnection(this->momentProbeHorizontalLine->GetOutputPort());
    this->momentProbeHorizontalActor->SetMapper(momentProbeHorizontalMapper);
    this->momentProbeHorizontalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->momentProbeHorizontalActor->GetProperty()->SetLineWidth(1.5);
    this->momentProbeHorizontalActor->VisibilityOff();
    ren->AddActor(this->momentProbeHorizontalActor);
    vtkNew<vtkPolyDataMapper> momentProbeVerticalMapper;
    momentProbeVerticalMapper->SetInputConnection(this->momentProbeVerticalLine->GetOutputPort());
    this->momentProbeVerticalActor->SetMapper(momentProbeVerticalMapper);
    this->momentProbeVerticalActor->GetProperty()->SetColor(1.0, 0.85, 0.1);
    this->momentProbeVerticalActor->GetProperty()->SetLineWidth(1.5);
    this->momentProbeVerticalActor->VisibilityOff();
    ren->AddActor(this->momentProbeVerticalActor);
    vtkNew<vtkPolyDataMapper> momentCatalogueOverlayMapper;
    momentCatalogueOverlayMapper->SetInputData(this->catalogueOverlayData);
    this->momentCatalogueOverlayActor->SetMapper(momentCatalogueOverlayMapper);
    this->momentCatalogueOverlayActor->GetProperty()->SetColor(1.0, 0.45, 0.15);
    this->momentCatalogueOverlayActor->GetProperty()->SetLineWidth(1.5);
    this->momentCatalogueOverlayActor->VisibilityOff();
    ren->AddActor(this->momentCatalogueOverlayActor);
    vtkNew<vtkPolyDataMapper> momentCatalogueHoverMapper;
    momentCatalogueHoverMapper->SetInputData(this->catalogueHoverOverlayData);
    this->momentCatalogueHoverOverlayActor->SetMapper(momentCatalogueHoverMapper);
    this->momentCatalogueHoverOverlayActor->GetProperty()->SetColor(1.0, 0.95, 0.45);
    this->momentCatalogueHoverOverlayActor->GetProperty()->SetLineWidth(2.5);
    this->momentCatalogueHoverOverlayActor->VisibilityOff();
    ren->AddActor(this->momentCatalogueHoverOverlayActor);
    vtkNew<vtkPolyDataMapper> momentCatalogueSelectionMapper;
    momentCatalogueSelectionMapper->SetInputData(this->catalogueSelectionOverlayData);
    this->momentCatalogueSelectionOverlayActor->SetMapper(momentCatalogueSelectionMapper);
    this->momentCatalogueSelectionOverlayActor->GetProperty()->SetColor(0.15, 0.95, 1.0);
    this->momentCatalogueSelectionOverlayActor->GetProperty()->SetLineWidth(3.2);
    this->momentCatalogueSelectionOverlayActor->VisibilityOff();
    ren->AddActor(this->momentCatalogueSelectionOverlayActor);
    this->momentPvData->SetPoints(this->momentPvPoints);
    this->momentPvData->SetLines(this->momentPvCells);
    vtkNew<vtkPolyDataMapper> momentPvMapper;
    momentPvMapper->SetInputData(this->momentPvData);
    this->momentPvActor->SetMapper(momentPvMapper);
    this->momentPvActor->GetProperty()->SetColor(1.0, 0.4, 0.1);
    this->momentPvActor->GetProperty()->SetLineWidth(2.5);
    this->momentPvActor->VisibilityOff();
    ren->AddActor(this->momentPvActor);
    vtkRenderer *momentRenderer = ren;
    const auto configureMomentPvBoundaryActor =
            [momentRenderer](vtkPoints *points, vtkCellArray *cells, vtkPolyData *polyData,
                             vtkActor *actor) {
        polyData->SetPoints(points);
        polyData->SetLines(cells);
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(polyData);
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(1.0, 0.72, 0.2);
        actor->GetProperty()->SetLineWidth(1.5);
        actor->GetProperty()->SetOpacity(0.8);
        actor->VisibilityOff();
        momentRenderer->AddActor(actor);
    };
    configureMomentPvBoundaryActor(this->momentPvUpperPoints, this->momentPvUpperCells,
                                   this->momentPvUpperData, this->momentPvUpperActor);
    configureMomentPvBoundaryActor(this->momentPvLowerPoints, this->momentPvLowerCells,
                                   this->momentPvLowerData, this->momentPvLowerActor);
    const auto configureRegionLineActor = [momentRenderer](vtkLineSource *line, vtkActor *actor) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.2, 0.9, 1.0);
        actor->GetProperty()->SetLineWidth(2.0);
        actor->VisibilityOff();
        momentRenderer->AddActor(actor);
    };
    configureRegionLineActor(this->momentRegionTopLine, this->momentRegionTopActor);
    configureRegionLineActor(this->momentRegionBottomLine, this->momentRegionBottomActor);
    configureRegionLineActor(this->momentRegionLeftLine, this->momentRegionLeftActor);
    configureRegionLineActor(this->momentRegionRightLine, this->momentRegionRightActor);
    this->momentRegionCircleSource->SetNumberOfSides(96);
    this->momentRegionCircleSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> momentRegionCircleMapper;
    momentRegionCircleMapper->SetInputConnection(this->momentRegionCircleSource->GetOutputPort());
    this->momentRegionCircleActor->SetMapper(momentRegionCircleMapper);
    this->momentRegionCircleActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionCircleActor->GetProperty()->SetLineWidth(2.0);
    this->momentRegionCircleActor->VisibilityOff();
    ren->AddActor(this->momentRegionCircleActor);
    this->momentRegionAnnulusOuterSource->SetNumberOfSides(96);
    this->momentRegionAnnulusOuterSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> momentRegionAnnulusOuterMapper;
    momentRegionAnnulusOuterMapper->SetInputConnection(this->momentRegionAnnulusOuterSource->GetOutputPort());
    this->momentRegionAnnulusOuterActor->SetMapper(momentRegionAnnulusOuterMapper);
    this->momentRegionAnnulusOuterActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionAnnulusOuterActor->GetProperty()->SetLineWidth(2.0);
    this->momentRegionAnnulusOuterActor->GetProperty()->SetRepresentationToWireframe();
    this->momentRegionAnnulusOuterActor->VisibilityOff();
    ren->AddActor(this->momentRegionAnnulusOuterActor);
    this->momentRegionAnnulusInnerSource->SetNumberOfSides(96);
    this->momentRegionAnnulusInnerSource->GeneratePolygonOff();
    vtkNew<vtkPolyDataMapper> momentRegionAnnulusInnerMapper;
    momentRegionAnnulusInnerMapper->SetInputConnection(this->momentRegionAnnulusInnerSource->GetOutputPort());
    this->momentRegionAnnulusInnerActor->SetMapper(momentRegionAnnulusInnerMapper);
    this->momentRegionAnnulusInnerActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionAnnulusInnerActor->GetProperty()->SetLineWidth(1.5);
    this->momentRegionAnnulusInnerActor->GetProperty()->SetRepresentationToWireframe();
    this->momentRegionAnnulusInnerActor->VisibilityOff();
    ren->AddActor(this->momentRegionAnnulusInnerActor);
    vtkNew<vtkPolyDataMapper> momentRegionAnnulusFillMapper;
    momentRegionAnnulusFillMapper->SetInputData(this->momentRegionAnnulusFillData);
    this->momentRegionAnnulusFillActor->SetMapper(momentRegionAnnulusFillMapper);
    this->momentRegionAnnulusFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionAnnulusFillActor->GetProperty()->SetOpacity(0.18);
    this->momentRegionAnnulusFillActor->GetProperty()->SetRepresentationToSurface();
    this->momentRegionAnnulusFillActor->VisibilityOff();
    ren->AddActor(this->momentRegionAnnulusFillActor);
    this->momentRegionPolygonTriangulator->SetInputData(this->momentRegionPolygonFillData);
    vtkNew<vtkPolyDataMapper> momentRegionPolygonMapper;
    momentRegionPolygonMapper->SetInputData(this->momentRegionPolygonData);
    this->momentRegionPolygonActor->SetMapper(momentRegionPolygonMapper);
    this->momentRegionPolygonActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionPolygonActor->GetProperty()->SetLineWidth(2.0);
    this->momentRegionPolygonActor->VisibilityOff();
    ren->AddActor(this->momentRegionPolygonActor);
    vtkNew<vtkPolyDataMapper> momentRegionPolygonFillMapper;
    momentRegionPolygonFillMapper->SetInputConnection(this->momentRegionPolygonTriangulator->GetOutputPort());
    this->momentRegionPolygonFillActor->SetMapper(momentRegionPolygonFillMapper);
    this->momentRegionPolygonFillActor->GetProperty()->SetColor(0.2, 0.9, 1.0);
    this->momentRegionPolygonFillActor->GetProperty()->SetOpacity(0.18);
    this->momentRegionPolygonFillActor->VisibilityOff();
    ren->AddActor(this->momentRegionPolygonFillActor);
    this->momentOverlayXAxis->GetTitleTextProperty()->SetFontSize(16);
    this->momentOverlayXAxis->GetTitleTextProperty()->SetBold(false);
    this->momentOverlayYAxis->GetTitleTextProperty()->SetFontSize(16);
    this->momentOverlayYAxis->GetTitleTextProperty()->SetOrientation(90.);
    this->momentOverlayYAxis->GetTitleTextProperty()->SetBold(false);
    this->momentOverlayXTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->momentOverlayXTitleActor->GetTextProperty()->SetFontSize(14);
    this->momentOverlayXTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->momentOverlayXTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    this->momentOverlayYTitleActor->GetTextProperty()->SetColor(1., 1., 1.);
    this->momentOverlayYTitleActor->GetTextProperty()->SetFontSize(14);
    this->momentOverlayYTitleActor->GetTextProperty()->SetOrientation(90.);
    this->momentOverlayYTitleActor->GetTextProperty()->SetJustificationToCentered();
    this->momentOverlayYTitleActor->GetTextProperty()->SetVerticalJustificationToCentered();
    CubeWcs::applyOverlayFont(this->momentOverlayXTitleActor->GetTextProperty());   // §3.0c #8
    CubeWcs::applyOverlayFont(this->momentOverlayYTitleActor->GetTextProperty());
    this->ensureOverlayTickActors(ren, this->momentOverlayXTickActors, this->momentOverlayYTickActors);
    this->momentWcsOverlayInitialized = true;
    this->set2dWcsOverlayVisible(this->showWcsAxes);
    this->momentWin->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::updateMomentWcsOverlay);
    this->momentWin->AddObserver(vtkCommand::EndEvent, this, &vtkWindowCube::updateCatalogueOverlayLabels);

    ren->ResetCamera();                        // keep: establishes a sane camera distance
    this->layout2dPaneFor(/*slice=*/false, true); // then letterbox it into the gutter
}

void vtkWindowCube::resetCubeCamera()
{
    auto ren = ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer();
    auto camera = ren->GetActiveCamera();
    camera->SetPosition(this->initialCameraPosition);
    camera->SetFocalPoint(this->initialCameraFocalPoint);
    camera->SetViewUp(0., 1., 0.);
    ren->ResetCamera();
}

void vtkWindowCube::setCameraAzimuth(double az)
{
    this->resetCubeCamera();
    ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActiveCamera()->Azimuth(az);
    ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::setCameraElevation(double el)
{
    this->resetCubeCamera();
    ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActiveCamera()->Elevation(
            el);
    ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::resetCameraFront()
{
    this->setCameraAzimuth(0.);
}

void vtkWindowCube::resetCameraBack()
{
    this->setCameraAzimuth(-180.);
}

void vtkWindowCube::resetCameraTop()
{
    this->setCameraElevation(90.);
}

void vtkWindowCube::resetCameraRight()
{
    this->setCameraAzimuth(90.);
}

void vtkWindowCube::resetCameraBottom()
{
    this->setCameraElevation(-90.);
}

void vtkWindowCube::resetCameraLeft()
{
    this->setCameraAzimuth(-90.);
}
