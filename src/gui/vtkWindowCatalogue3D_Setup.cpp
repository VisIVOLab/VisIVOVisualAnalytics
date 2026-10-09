#include "vtkWindowCatalogue3D.h"
#include "ui_vtkWindowCatalogue3D.h"

#include "LutScale.h"
#include "app/DiagnosticsManager.h"
#include "DiagnosticsWindow.h"
#include "Catalogue3DTableModel.h"
#include "Catalogue3DParser.h"
#include "theme/VisivoTheme.h"
#include "WorkspaceChrome.h"
#include "InspectorPanel.h"
#include <QToolButton>
#include <QTableWidget>

// ── VTK ──────────────────────────────────────────────────────────────────────
#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkAppendPolyData.h>
#include <vtkAxesActor.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCubeSource.h>
#include <vtkCubeAxesActor.h>
#include <vtkCoordinate.h>
#include <vtkFloatArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGlyph3D.h>
#include <vtkIntArray.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkLookupTable.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSphereSource.h>
#include <vtkTextProperty.h>

// ── Qt ───────────────────────────────────────────────────────────────────────
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QKeySequence>
#include <QMetaObject>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableView>
#include <QTextEdit>
#include <QVBoxLayout>
#include "AssistantPanel.h"
#include "CopilotActionRouter.h"
#include <QtConcurrent>

#include <optional>

/// Returns true when @p errorText looks like a backend session-expiry error.
// ── std ───────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "CatalogueWcsUtil.h"

using namespace Qt::StringLiterals;

// ── Setup ──
// Extracted verbatim from vtkWindowCatalogue3D.cpp.

void vtkWindowCatalogue3D::buildControlPages()
{
    // The pages are built here and MOUNTED LATER, in setupWorkspaceDocks(): the
    // docks are installed once the catalogue has been read, because the window's
    // Session Data row and the Inspector's properties describe what was loaded.
    // Until then each page is a hidden child of the window.
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->sidebarContainer);
    ui->sidebarContainer->hide();

    // Shared card styles (same palette as cube viewer).
    const QString hdrStyle    = VisivoTheme::sidebarHeaderStyleSheet();
    const QString cardStyle   = VisivoTheme::sidebarCardStyleSheet();
    const QString subHdrStyle = VisivoTheme::sidebarSubHeaderStyleSheet();

    // ── Visualization page ────────────────────────────────────────────────────
    // Built programmatically so we own the layout cleanly.  ui->pageViz (from
    // the .ui file) is left in the hidden sidebarContainer and not used.
    {
        auto *pageViz    = new QWidget;
        auto *pageLayout = new QVBoxLayout(pageViz);
        pageLayout->setContentsMargins(8, 8, 8, 8);
        pageLayout->setSpacing(8);

        auto *vizHdr = new QLabel(u"VISUALIZATION"_s, pageViz);
        vizHdr->setStyleSheet(hdrStyle);
        pageLayout->addWidget(vizHdr);

        // ── Source Representation card ────────────────────────────────────────
        auto *reprCard   = new QFrame(pageViz);
        reprCard->setStyleSheet(cardStyle);
        auto *reprLayout = new QVBoxLayout(reprCard);
        reprLayout->setContentsMargins(8, 8, 8, 8);
        reprLayout->setSpacing(6);
        auto *reprHdr = new QLabel(u"SOURCE REPRESENTATION"_s, reprCard);
        reprHdr->setStyleSheet(subHdrStyle);
        reprLayout->addWidget(reprHdr);

        this->comboGeometry = new QComboBox(reprCard);
        this->comboGeometry->addItems({ u"Ellipsoid"_s, u"Sphere"_s, u"Point"_s, u"Cross"_s });
        auto *reprForm = new QFormLayout;
        reprForm->setContentsMargins(0, 0, 0, 0);
        reprForm->addRow(u"Shape"_s, this->comboGeometry);
        reprLayout->addLayout(reprForm);
        reprLayout->addWidget(ui->chkLabels);
        reprLayout->addWidget(ui->chkMorphColors);
        pageLayout->addWidget(reprCard);

        // ── Axis Mapping card ─────────────────────────────────────────────────
        auto *axisCard   = new QFrame(pageViz);
        axisCard->setStyleSheet(cardStyle);
        auto *axisLayout = new QVBoxLayout(axisCard);
        axisLayout->setContentsMargins(8, 8, 8, 8);
        axisLayout->setSpacing(6);
        auto *axisHdr = new QLabel(u"AXIS MAPPING"_s, axisCard);
        axisHdr->setStyleSheet(subHdrStyle);
        axisLayout->addWidget(axisHdr);

        this->comboMapX = new QComboBox(axisCard);
        this->comboMapY = new QComboBox(axisCard);
        this->comboMapZ = new QComboBox(axisCard);
        auto *axisForm = new QFormLayout;
        axisForm->setContentsMargins(0, 0, 0, 0);
        axisForm->addRow(u"X"_s, this->comboMapX);
        axisForm->addRow(u"Y"_s, this->comboMapY);
        axisForm->addRow(u"Z"_s, this->comboMapZ);
        axisLayout->addLayout(axisForm);
        pageLayout->addWidget(axisCard);

        // ── Visual Encoding card ──────────────────────────────────────────────
        auto *encCard   = new QFrame(pageViz);
        encCard->setStyleSheet(cardStyle);
        auto *encLayout = new QVBoxLayout(encCard);
        encLayout->setContentsMargins(8, 8, 8, 8);
        encLayout->setSpacing(6);
        auto *encHdr = new QLabel(u"VISUAL ENCODING"_s, encCard);
        encHdr->setStyleSheet(subHdrStyle);
        encLayout->addWidget(encHdr);

        // Size combo doubles as comboSizeMode (same widget, two member aliases).
        this->comboSizeMode = new QComboBox(encCard);
        this->comboMapSize  = this->comboSizeMode;
        this->comboMapColor = new QComboBox(encCard);
        this->comboColorScale = new QComboBox(encCard);
        for (const char *label : LutScale::modeLabels()) {
            this->comboColorScale->addItem(QString::fromUtf8(label));
        }
        this->spinColorGamma = new QDoubleSpinBox(encCard);
        this->spinColorGamma->setRange(0.1, 10.0);
        this->spinColorGamma->setSingleStep(0.1);
        this->spinColorGamma->setDecimals(2);
        this->spinColorGamma->setValue(2.0);
        this->spinColorGamma->setToolTip(u"Exponent γ for the Power stretch (t^γ)."_s);
        this->spinColorGamma->setEnabled(false);  // enabled only in Power mode
        auto *encForm = new QFormLayout;
        encForm->setContentsMargins(0, 0, 0, 0);
        encForm->addRow(u"Size"_s,  this->comboSizeMode);
        encForm->addRow(u"Color"_s, this->comboMapColor);
        encForm->addRow(u"Color scale"_s, this->comboColorScale);
        encForm->addRow(u"Power γ"_s, this->spinColorGamma);
        encForm->addRow(u"Scale"_s, ui->sliderScale);

        // Render budget (GPU point LOD): cap how many points the mappers draw so
        // glyph modes stay smooth on million-point catalogues. Picking/stats use
        // the full data. userData holds the point cap (0 = unlimited).
        this->comboRenderBudget = new QComboBox(encCard);
        this->comboRenderBudget->addItem(u"500k"_s, 500000);
        this->comboRenderBudget->addItem(u"1M"_s, 1000000);
        this->comboRenderBudget->addItem(u"2M"_s, 2000000);
        this->comboRenderBudget->addItem(u"5M"_s, 5000000);
        this->comboRenderBudget->addItem(u"Unlimited"_s, 0);
        this->comboRenderBudget->setCurrentIndex(1);   // 1M default (matches m_renderBudget)
        this->comboRenderBudget->setToolTip(
                u"Maximum points drawn on the GPU. Above this a density-preserving "
                u"random subsample is shown; picking and stats still use every source."_s);
        encForm->addRow(u"Render budget"_s, this->comboRenderBudget);
        encLayout->addLayout(encForm);

        QObject::connect(this->comboColorScale, &QComboBox::currentIndexChanged, this,
                         [this](int) { this->updateColorMapping(); });
        QObject::connect(this->spinColorGamma, &QDoubleSpinBox::valueChanged, this,
                         [this](double) { this->updateColorMapping(); });
        QObject::connect(this->comboRenderBudget, &QComboBox::currentIndexChanged, this,
                         [this](int) {
                             if (!this->comboRenderBudget)
                                 return;
                             this->m_renderBudget =
                                     this->comboRenderBudget->currentData().toInt();
                             if (this->entries.empty())
                                 return;
                             this->applyRenderBudget();
                             this->renderWindow->Render();
                             if (this->filterStatusLabel)
                                 this->filterStatusLabel->setText(
                                         this->renderBudgetStatusText());
                         });
        pageLayout->addWidget(encCard);

        // ── Scene card ────────────────────────────────────────────────────────
        auto *sceneCard   = new QFrame(pageViz);
        sceneCard->setStyleSheet(cardStyle);
        auto *sceneLayout = new QVBoxLayout(sceneCard);
        sceneLayout->setContentsMargins(8, 8, 8, 8);
        sceneLayout->setSpacing(6);
        auto *sceneHdr = new QLabel(u"SCENE"_s, sceneCard);
        sceneHdr->setStyleSheet(subHdrStyle);
        sceneLayout->addWidget(sceneHdr);

        this->chkShowAxes        = new QCheckBox(u"Show axes"_s,          sceneCard);
        this->chkShowBoundingBox = new QCheckBox(u"Show bounding box"_s,  sceneCard);
        this->chkShowShells      = new QCheckBox(u"Show redshift shells"_s, sceneCard);
        this->chkShowAxes->setChecked(true);
        sceneLayout->addWidget(this->chkShowAxes);
        sceneLayout->addWidget(this->chkShowBoundingBox);
        sceneLayout->addWidget(this->chkShowShells);

        this->comboFrame = new QComboBox(sceneCard);
        this->comboFrame->addItems(
                { u"FK5 / J2000"_s, u"Galactic (l, b)"_s, u"Supergalactic (SGL, SGB)"_s });
        this->comboFrame->setToolTip(
                u"Coordinate frame for 3D scene positions.\n"
                u"FK5 / J2000: standard equatorial (RA, Dec).\n"
                u"Galactic (l, b): galactic longitude and latitude.\n"
                u"Supergalactic (SGL, SGB): de Vaucouleurs frame of the local "
                u"supercluster plane (the natural frame for cosmic-flow figures)."_s);
        this->comboCosmology = new QComboBox(sceneCard);
        this->comboCosmology->addItems(
                { u"Planck18"_s, u"Planck15"_s, u"Planck13"_s, u"WMAP9"_s });
        this->comboCosmology->setToolTip(
                u"Cosmology model for redshift → comoving distance.\n"
                u"Distances are computed on the backend via astropy."_s);

        // R-14: real-space (comoving) vs redshift-space (naive cz/H0) placement.
        this->comboDistanceMode = new QComboBox(sceneCard);
        this->comboDistanceMode->addItems(
                { u"Real-space (comoving)"_s, u"Redshift-space (cz/H₀)"_s });
        this->comboDistanceMode->setToolTip(
                u"How a source's radial distance is derived from its redshift.\n"
                u"Real-space: measured / model comoving distance (default).\n"
                u"Redshift-space: naive Hubble distance cz/H₀ from redshift alone.\n"
                u"When a source also has an INDEPENDENT real-space distance, the\n"
                u"shift between the two modes is its line-of-sight peculiar velocity\n"
                u"(redshift-space distortion). For redshift-only sources the shift\n"
                u"is just cz/H₀ − comoving D_C(z), not a peculiar velocity."_s);

        auto *sceneForm = new QFormLayout;
        sceneForm->setContentsMargins(0, 0, 0, 0);
        sceneForm->addRow(u"Frame"_s, this->comboFrame);
        sceneLayout->addLayout(sceneForm);
        pageLayout->addWidget(sceneCard);

        // ── Cosmology card ────────────────────────────────────────────────────
        // The model and the distance convention decide WHERE every source is,
        // not how it looks, so they are not scene decoration and no longer sit
        // under "Scene" between the axes and the bounding box. They also say
        // what they did: the model changed and 300 positions moved, which the
        // status line at the bottom of a different page used to report.
        auto *cosmoCard = new QFrame(pageViz);
        cosmoCard->setStyleSheet(cardStyle);
        auto *cosmoLayout = new QVBoxLayout(cosmoCard);
        cosmoLayout->setContentsMargins(8, 8, 8, 8);
        cosmoLayout->setSpacing(6);
        auto *cosmoHdr = new QLabel(u"COSMOLOGY"_s, cosmoCard);
        cosmoHdr->setStyleSheet(subHdrStyle);
        cosmoLayout->addWidget(cosmoHdr);

        auto *cosmoForm = new QFormLayout;
        cosmoForm->setContentsMargins(0, 0, 0, 0);
        cosmoForm->addRow(u"Model"_s, this->comboCosmology);
        cosmoForm->addRow(u"Distance"_s, this->comboDistanceMode);
        cosmoLayout->addLayout(cosmoForm);

        this->m_cosmologyStatus = new QLabel(cosmoCard);
        this->m_cosmologyStatus->setWordWrap(true);
        this->m_cosmologyStatus->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
        cosmoLayout->addWidget(this->m_cosmologyStatus);
        pageLayout->addWidget(cosmoCard);

        pageLayout->addStretch(1);
        this->m_pageViz = pageViz;
        pageViz->setParent(this);
        pageViz->hide();
    }

    // ── Source Info page (from UI file) ───────────────────────────────────────
    ui->groupInfo->setStyleSheet(cardStyle);
    ui->labelInfoHdr->setStyleSheet(hdrStyle);

    // ── Filters page ─────────────────────────────────────────────────────────
    // Widgets assigned to members here; signal/slot connections wired later
    // in the constructor body after the schema is known.
    {
        auto *pageFilters = new QWidget;
        auto *pageLayout  = new QVBoxLayout(pageFilters);
        pageLayout->setContentsMargins(8, 8, 8, 8);
        pageLayout->setSpacing(8);

        auto *hdr = new QLabel(u"FILTERS"_s, pageFilters);
        hdr->setStyleSheet(hdrStyle);
        pageLayout->addWidget(hdr);

        // Active Filters card.
        auto *filtersCard   = new QFrame(pageFilters);
        filtersCard->setStyleSheet(cardStyle);
        auto *filtersCardLayout = new QVBoxLayout(filtersCard);
        filtersCardLayout->setContentsMargins(8, 8, 8, 8);
        filtersCardLayout->setSpacing(6);

        auto *listLabel = new QLabel(u"ACTIVE FILTERS"_s, filtersCard);
        listLabel->setStyleSheet(subHdrStyle);
        filtersCardLayout->addWidget(listLabel);

        this->filterList = new QListWidget(filtersCard);
        this->filterList->setSelectionMode(QAbstractItemView::SingleSelection);
        this->filterList->setToolTip(
                u"Active filters combined with AND. Double-click a row to edit it."_s);
        filtersCardLayout->addWidget(this->filterList);

        this->filterStatusLabel = new QLabel(filtersCard);
        this->filterStatusLabel->setWordWrap(true);
        this->filterStatusLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; font-size: 9px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        filtersCardLayout->addWidget(this->filterStatusLabel);

        this->btnLoadMore = new QPushButton(u"Load more…"_s, filtersCard);
        this->btnLoadMore->setVisible(false);
        this->btnLoadMore->setToolTip(
                u"Fetch the next page of results and append them to the current scene."_s);
        filtersCardLayout->addWidget(this->btnLoadMore);
        QObject::connect(this->btnLoadMore, &QPushButton::clicked,
                         this, &vtkWindowCatalogue3D::loadMoreEntries);

        this->btnLoadAll = new QPushButton(u"Load all"_s, filtersCard);
        this->btnLoadAll->setVisible(false);
        this->btnLoadAll->setToolTip(
                u"Load every remaining source at once (no paging). May use significant "
                u"memory for very large catalogues."_s);
        filtersCardLayout->addWidget(this->btnLoadAll);
        QObject::connect(this->btnLoadAll, &QPushButton::clicked,
                         this, &vtkWindowCatalogue3D::loadAllEntries);

        pageLayout->addWidget(filtersCard);

        // Actions card.
        auto *actionsCard   = new QFrame(pageFilters);
        actionsCard->setStyleSheet(cardStyle);
        auto *actionsCardLayout = new QVBoxLayout(actionsCard);
        actionsCardLayout->setContentsMargins(8, 8, 8, 8);
        actionsCardLayout->setSpacing(4);

        this->btnFilterAdd      = new QPushButton(u"Add…"_s,      actionsCard);
        this->btnFilterRemove   = new QPushButton(u"Remove"_s,    actionsCard);
        this->btnFilterClearAll = new QPushButton(u"Clear All"_s, actionsCard);
        this->btnFilterApply    = new QPushButton(u"Apply"_s,     actionsCard);
        actionsCardLayout->addWidget(this->btnFilterAdd);
        actionsCardLayout->addWidget(this->btnFilterRemove);
        actionsCardLayout->addWidget(this->btnFilterClearAll);
        actionsCardLayout->addWidget(this->btnFilterApply);

        pageLayout->addWidget(actionsCard);
        pageLayout->addStretch(1);
        this->m_pageFilters = pageFilters;
        pageFilters->setParent(this);
        pageFilters->hide();
    }

    // ── Scientific copilot page ───────────────────────────────────────────────
    // Grounded Q&A over the loaded catalogue (catalogue_summary + cosmology_distance
    // tools) plus reversible camera control. The dataset id is set later, once the
    // catalogue open request returns (see the open handlers in the .cpp).
    // The copilot is the Inspector's own tab now (installCopilot creates it), so
    // nothing is built here — see setupWorkspaceDocks().

    // View menu: the same two entries as the image and cube viewers, naming the
    // docks rather than a "Controls Sidebar" that exists nowhere else.
    auto *actionSession = new QAction(u"Session Data Panel"_s, this);
    actionSession->setCheckable(true);
    actionSession->setChecked(true);
    this->actionShowControlsSidebar = new QAction(u"Inspector Panel"_s, this);
    this->actionShowControlsSidebar->setCheckable(true);
    this->actionShowControlsSidebar->setChecked(true);
    QAction *before = ui->menuView->actions().isEmpty() ? nullptr
                                                        : ui->menuView->actions().first();
    ui->menuView->insertAction(before, actionSession);
    ui->menuView->insertAction(before, this->actionShowControlsSidebar);
    ui->menuView->insertSeparator(before);
    QObject::connect(actionSession, &QAction::toggled, this, [this](bool checked) {
        if (this->m_workspaceChrome)
            this->m_workspaceChrome->setSessionCollapsed(!checked);
    });
    QObject::connect(this->actionShowControlsSidebar, &QAction::toggled, this,
                     [this](bool checked) {
                         if (this->m_workspaceChrome)
                             this->m_workspaceChrome->setInspectorCollapsed(!checked);
                     });
    this->m_actionSessionPanel = actionSession;
}

void vtkWindowCatalogue3D::setupRenderer()
{
    // Attach render window to the Qt VTK widget.
    ui->vtk->setRenderWindow(renderWindow.Get());
    ui->vtk->setEnableTouchEventProcessing(false);
    qInfo() << "[catalogue3d] widget type:" << ui->vtk->metaObject()->className()
            << "touch-processing disabled to match vtkWindowCube";

    renderer->SetBackground(0.06, 0.06, 0.12);
    renderWindow->AddRenderer(renderer);

    // Trackball camera – standard 3D navigation, matching the cube viewer approach:
    // use the stock VTK style and keep catalogue logic in separate observers.
    auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
    ui->vtk->interactor()->SetInteractorStyle(style);
    qInfo() << "[catalogue3d] installed interactor style:" << style->GetClassName();

    // Orientation axes widget (bottom-left corner, non-interactive).
    vtkNew<vtkAxesActor> axesActor;
    axesWidget->SetOrientationMarker(axesActor);
    axesWidget->SetInteractor(ui->vtk->interactor());
    axesWidget->SetViewport(0.0, 0.0, 0.14, 0.14);
    axesWidget->EnabledOn();
    axesWidget->InteractiveOff();

    // ── Mouse observers ───────────────────────────────────────────────────────
    auto cb = vtkSmartPointer<vtkCallbackCommand>::New();
    cb->SetCallback([](vtkObject *, unsigned long eid, void *clientData, void *) {
        static_cast<vtkWindowCatalogue3D *>(clientData)->onMouseEvent(eid);
    });
    cb->SetClientData(this);

    auto *inter = ui->vtk->interactor();
    inter->AddObserver(vtkCommand::MouseMoveEvent,         cb);
    inter->AddObserver(vtkCommand::LeftButtonPressEvent,   cb);
    inter->AddObserver(vtkCommand::LeftButtonReleaseEvent, cb);
    inter->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, cb);
    qInfo() << "[catalogue3d] observer registration complete;"
            << "hover observer is separate from camera interaction";
}

void vtkWindowCatalogue3D::ensureCatalogueDock()
{
    if (this->catalogueDock) {
        return;
    }
    this->catalogueDock = new QDockWidget(u"Catalogue"_s, this);
    this->catalogueDock->setObjectName(u"Catalogue3DDock"_s);
    auto *dockWidget = new QWidget(this->catalogueDock);
    auto *dockLayout = new QVBoxLayout(dockWidget);
    dockLayout->setContentsMargins(4, 4, 4, 4);
    dockLayout->setSpacing(0);

    // Table only — mappings live in the Visualization sidebar page;
    // selected-source details live in the Source Info sidebar page.
    this->catalogueTableView = new QTableView(dockWidget);
    this->catalogueTableModel = new Catalogue3DTableModel(this->catalogueDock);
    this->catalogueTableView->setModel(this->catalogueTableModel);
    this->catalogueTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->catalogueTableView->setSelectionMode(QAbstractItemView::SingleSelection);
    this->catalogueTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->catalogueTableView->setAlternatingRowColors(true);
    this->catalogueTableView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->catalogueTableView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->catalogueTableView->verticalHeader()->setVisible(false);
    this->catalogueTableView->horizontalHeader()->setStretchLastSection(false);
    this->catalogueTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    dockLayout->addWidget(this->catalogueTableView, 1);

    this->catalogueDock->setWidget(dockWidget);
    this->addDockWidget(Qt::BottomDockWidgetArea, this->catalogueDock);
    QObject::connect(this, &vtkWindowCatalogue3D::sourceSelectionChanged, this,
                     &vtkWindowCatalogue3D::syncTableSelection);
    QObject::connect(this->catalogueTableView->selectionModel(), &QItemSelectionModel::currentRowChanged,
                     this, [this](const QModelIndex &current) {
        if (this->syncingTableSelection) {
            return;
        }
        this->setSelectedSource(current.isValid() ? current.row() : -1);
    });
    QObject::connect(this->catalogueTableView, &QTableView::doubleClicked, this,
                     [this](const QModelIndex &index) {
        if (index.isValid()) {
            this->setSelectedSource(index.row());
            this->centerOnSource(index.row());
        }
    });
    QObject::connect(this->comboMapX, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!this->comboMapX || this->updatingMappingControls) {
            return;
        }
        this->mapXKey = this->comboMapX->currentData().toString();
        this->updatePointPositions();
        this->renderWindow->Render();
    });
    QObject::connect(this->comboMapY, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!this->comboMapY || this->updatingMappingControls) {
            return;
        }
        this->mapYKey = this->comboMapY->currentData().toString();
        this->updatePointPositions();
        this->renderWindow->Render();
    });
    QObject::connect(this->comboMapZ, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!this->comboMapZ || this->updatingMappingControls) {
            return;
        }
        this->mapZKey = this->comboMapZ->currentData().toString();
        this->updatePointPositions();
        this->renderWindow->Render();
    });
    QObject::connect(this->comboMapColor, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!this->comboMapColor || this->updatingMappingControls) {
            return;
        }
        this->mapColorKey = this->comboMapColor->currentData().toString();
        this->updateColorMapping();
        this->renderWindow->Render();
    });
}

void vtkWindowCatalogue3D::refreshCatalogueTable()
{
    if (!this->catalogueTableModel) {
        return;
    }
    this->catalogueTableModel->setCatalogue(&this->entries, &this->schema);
    if (this->catalogueTableView) {
        this->catalogueTableView->resizeColumnsToContents();
    }
}

void vtkWindowCatalogue3D::detectVelocityColumns()
{
    this->m_velocityColIdx[0] = this->m_velocityColIdx[1] = this->m_velocityColIdx[2] = -1;
    for (int i = 0; i < this->rawHeaders.size(); ++i) {
        const QString h = this->rawHeaders.at(i).trimmed().toLower();
        if (h == u"vx"_s)
            this->m_velocityColIdx[0] = i;
        else if (h == u"vy"_s)
            this->m_velocityColIdx[1] = i;
        else if (h == u"vz"_s)
            this->m_velocityColIdx[2] = i;
    }
}

void vtkWindowCatalogue3D::populateMappingControls()
{
    if (!this->comboMapX || !this->comboMapY || !this->comboMapZ || !this->comboMapSize
        || !this->comboMapColor) {
        return;
    }

    this->numericRawFieldIndices.clear();
    for (int column = 0; column < static_cast<int>(this->schema.fields.size()); ++column) {
        const QString type = this->schema.fields[static_cast<std::size_t>(column)].type.toLower();
        if (type == QStringLiteral("float") || type == QStringLiteral("int")) {
            this->numericRawFieldIndices.push_back(column);
        }
    }

    this->updatingMappingControls = true;
    const auto fillAxisCombo = [this](QComboBox *combo, const QString &currentKey, const QString &defaultKey) {
        combo->clear();
        combo->addItem(u"Computed Cartesian X"_s, QStringLiteral("computed:x"));
        combo->addItem(u"Computed Cartesian Y"_s, QStringLiteral("computed:y"));
        combo->addItem(u"Computed Cartesian Z"_s, QStringLiteral("computed:z"));
        for (const int index : this->numericRawFieldIndices) {
            combo->addItem(QStringLiteral("raw:%1").arg(this->rawHeaders.value(index)),
                           QStringLiteral("raw:%1").arg(index));
        }
        int comboIndex = combo->findData(currentKey);
        if (comboIndex < 0) {
            comboIndex = combo->findData(defaultKey);
        }
        combo->setCurrentIndex(std::max(0, comboIndex));
    };
    fillAxisCombo(this->comboMapX, this->mapXKey, QStringLiteral("computed:x"));
    fillAxisCombo(this->comboMapY, this->mapYKey, QStringLiteral("computed:y"));
    fillAxisCombo(this->comboMapZ, this->mapZKey, QStringLiteral("computed:z"));

    this->comboMapSize->clear();
    this->comboMapSize->addItem(u"Fixed"_s, QStringLiteral("builtin:fixed"));
    this->comboMapSize->addItem(u"Major axis"_s, QStringLiteral("builtin:major"));
    this->comboMapSize->addItem(u"LLS"_s, QStringLiteral("builtin:lls"));
    this->comboMapSize->addItem(u"Flux"_s, QStringLiteral("builtin:flux"));
    for (const int index : this->numericRawFieldIndices) {
        this->comboMapSize->addItem(QStringLiteral("raw:%1").arg(this->rawHeaders.value(index)),
                                    QStringLiteral("raw:%1").arg(index));
    }
    int sizeIndex = this->comboMapSize->findData(this->mapSizeKey);
    this->comboMapSize->setCurrentIndex(sizeIndex >= 0 ? sizeIndex : 0);

    // Keep velocity-column indices consistent with the current columns.
    this->detectVelocityColumns();
    const bool hasVelocity = this->m_velocityColIdx[0] >= 0 && this->m_velocityColIdx[1] >= 0
            && this->m_velocityColIdx[2] >= 0;

    // The "Arrows (velocity)" shape is only meaningful with vx/vy/vz — add/remove
    // it to match (it is the 5th item, index 4, handled in updateGeometryMode()).
    if (this->comboGeometry) {
        const QString arrowsLabel = u"Arrows (velocity)"_s;
        const int arrowsIdx = this->comboGeometry->findText(arrowsLabel);
        if (hasVelocity && arrowsIdx < 0) {
            this->comboGeometry->addItem(arrowsLabel);
        } else if (!hasVelocity && arrowsIdx >= 0) {
            QSignalBlocker blk(this->comboGeometry);
            if (this->comboGeometry->currentIndex() == arrowsIdx) {
                this->comboGeometry->setCurrentIndex(0);
                this->geometryMode = GeometryMode::Ellipsoid;   // keep model in sync
            }
            this->comboGeometry->removeItem(arrowsIdx);
        }
    }

    this->comboMapColor->clear();
    this->comboMapColor->addItem(u"Morphology color"_s, QStringLiteral("builtin:morphology"));
    if (hasVelocity)   // peculiar-velocity magnitude √(vx²+vy²+vz²)
        this->comboMapColor->addItem(u"Speed |v|"_s, QStringLiteral("builtin:speed"));
    for (const int index : this->numericRawFieldIndices) {
        this->comboMapColor->addItem(QStringLiteral("raw:%1").arg(this->rawHeaders.value(index)),
                                     QStringLiteral("raw:%1").arg(index));
    }
    int colorIndex = this->comboMapColor->findData(this->mapColorKey);
    this->comboMapColor->setCurrentIndex(colorIndex >= 0 ? colorIndex : 0);
    this->updatingMappingControls = false;
}

// ── Shared viewer chrome ─────────────────────────────────────────────────────
//
// The image and cube viewers open with a command bar across the top — the file's
// path, a tag naming what the file IS, the ⌘K search, the live backend chip —
// and a status rail along the bottom. This window had none of it: the only clue
// to which catalogue was on screen was the title bar, and the only sign that the
// backend was alive was that things happened when you clicked. It is the same
// kind of window; it gets the same chrome.

void vtkWindowCatalogue3D::setupViewerChrome()
{
    VisivoTheme::ViewerToolbarSpec spec;
    spec.filePath = this->filepath;
    spec.primaryTag = QStringLiteral("CAT · %1 sources").arg(this->entries.size());
    // A concatenated catalogue is several files behaving as one, which is worth
    // saying here: the path pill can only show the first of them.
    spec.secondaryTag = this->m_cataloguePaths.size() > 1
            ? QStringLiteral("%1 files · remote").arg(this->m_cataloguePaths.size())
            : QStringLiteral("remote");

    VisivoTheme::ViewerToolbarWidgets widgets;
    this->m_workspaceChrome = WorkspaceChrome::installCommandBar(
            this, spec, this->backendUrl, this->backendToken, this->remoteSessionId, &widgets);
    if (!this->m_workspaceChrome)
        return;

    this->m_kindTag = widgets.primaryTag;
    if (widgets.primaryTag) {
        widgets.primaryTag->setToolTip(u"Columns in this catalogue, and which of "
                                       u"them are its coordinates."_s);
        QObject::connect(widgets.primaryTag, &QToolButton::clicked, this,
                         [this]() { this->showCatalogueFieldsDialog(); });
    }
    this->m_workspaceChrome->installStatusRail();
    this->setupWorkspaceDocks();
    this->m_workspaceChrome->start();

    // Say what the positions on screen ARE before the user touches anything: a
    // panel that only speaks once something changes leaves the first — and most
    // common — state unexplained.
    this->refreshViewerChrome();
}

void vtkWindowCatalogue3D::setupWorkspaceDocks()
{
    // Session Data on the left, Inspector on the right — the same two docks, in
    // the same places, with the same collapse-to-a-rail behaviour as the image
    // and cube viewers. This window used to carry a control rail of its own
    // design instead, so moving between a cube and a catalogue meant learning
    // the window again.
    WorkspaceChrome *chrome = this->m_workspaceChrome;
    if (!chrome)
        return;

    const QFileInfo info(this->filepath);
    chrome->installSessionDock(this->filepath, info.fileName(), u"catalogue"_s);
    // Design #1d: what is drawn and how it is drawn lives in the LEFT column,
    // under the dataset it belongs to.
    if (this->m_pageViz) {
        this->m_pageViz->show();
        chrome->addSessionLayers(this->m_pageViz);
    }

    InspectorPanel *panel = chrome->installInspectorDock();
    if (!panel)
        return;
    // Built row by row rather than with setProperties(), because two of them
    // have to be updated later and re-running setProperties() clears the page —
    // taking the Source Info section mounted below it with it.
    // Styled by the Inspector itself, so a row this window updates later looks
    // and behaves exactly like the ones it owns — elided to the column, with the
    // whole value in the tooltip.
    const auto valueLabel = [](const QString &text) {
        auto *label = new QLabel;
        InspectorPanel::applyValueStyle(label, text);
        return label;
    };
    panel->clearProperties();
    panel->addPropertyRow(u"Dataset"_s, info.fileName());
    panel->addPropertyRow(u"Type"_s, u"CATALOGUE"_s);
    this->m_propSources = valueLabel(QString::number(this->entries.size()));
    panel->addPropertyRow(u"Sources"_s, this->m_propSources);
    panel->addPropertyRow(u"Columns"_s, QString::number(this->schema.fields.size()));
    panel->addPropertyRow(u"Path"_s, this->filepath);
    this->m_propSession = valueLabel(this->remoteSessionId);
    panel->addPropertyRow(u"Session"_s, this->m_propSession);
    panel->addPropertyRow(u"Backend"_s, this->backendUrl);
    // The selected source's details sit under those properties: both answer
    // "what am I looking at", which is what the Properties tab is for.
    if (ui->pageInfo)
        panel->addPropertiesSection(ui->pageInfo);
    // Filtering is an analysis of the catalogue, not a view setting.
    if (this->m_pageFilters) {
        this->m_pageFilters->show();
        panel->addAnalysisSection(this->m_pageFilters);
    }

    // The copilot becomes the Inspector's own tab, as in the other viewers.
    if (!this->backendUrl.isEmpty()) {
        this->m_copilot = chrome->installCopilot(this->backendUrl, this->backendToken,
                                                 this->remoteDatasetId);
        if (this->m_copilot) {
            this->m_copilot->setSuggestions({
                    u"What columns does this catalogue have?"_s,
                    u"What's the redshift range?"_s,
                    u"Comoving distance at redshift 0.1?"_s });
            CopilotActionRouter::instance()->registerTarget(
                    this->remoteDatasetId, this,
                    [this](const QList<QJsonObject> &acts) { this->applyCopilotActions(acts); },
                    [this]() { this->undoCopilotActions(); });
        }
    }

    // Seed the two checkmarks from the docks' ACTUAL state: the Inspector opens
    // as a rail by design (§4.1.4), so an action that starts checked means the
    // first click asks for a collapse that has already happened and nothing
    // moves — which is exactly what it did.
    if (this->m_actionSessionPanel) {
        QSignalBlocker block(this->m_actionSessionPanel);
        this->m_actionSessionPanel->setChecked(!chrome->sessionCollapsed());
    }
    if (this->actionShowControlsSidebar) {
        QSignalBlocker block(this->actionShowControlsSidebar);
        this->actionShowControlsSidebar->setChecked(!chrome->inspectorCollapsed());
    }

    // Keep the View-menu checkmarks in step with a collapse done by clicking the
    // dock's own chevron.
    QObject::connect(chrome, &WorkspaceChrome::dockCollapseChanged, this, [this]() {
        if (!this->m_workspaceChrome)
            return;
        if (this->m_actionSessionPanel) {
            QSignalBlocker block(this->m_actionSessionPanel);
            this->m_actionSessionPanel->setChecked(!this->m_workspaceChrome->sessionCollapsed());
        }
        if (this->actionShowControlsSidebar) {
            QSignalBlocker block(this->actionShowControlsSidebar);
            this->actionShowControlsSidebar->setChecked(
                    !this->m_workspaceChrome->inspectorCollapsed());
        }
    });
}

void vtkWindowCatalogue3D::refreshViewerChrome()
{
    const int count = static_cast<int>(this->entries.size());
    if (this->m_kindTag)
        this->m_kindTag->setText(u"CAT · %1 sources"_s.arg(count));
    // The Inspector counts the same sources as the command bar, and names the
    // session the window is actually talking to — which changes when an expired
    // one is recovered.
    if (this->m_propSources)
        InspectorPanel::applyValueStyle(this->m_propSources, QString::number(count));
    if (this->m_propSession)
        InspectorPanel::applyValueStyle(this->m_propSession, this->remoteSessionId);
    if (this->m_workspaceChrome)
        this->m_workspaceChrome->setSessionId(this->remoteSessionId);

    if (!this->m_cosmologyStatus)
        return;
    // What the line must not do is claim a cosmology that had no part in the
    // positions. A Cartesian catalogue carries its own X/Y/Z; the redshift-space
    // mode uses cz/H₀ and ignores the model entirely; and a catalogue with its
    // own distance column uses THAT in preference to any conversion, which is
    // why the column is named rather than implied.
    if (this->schema.isCartesian()) {
        this->m_cosmologyStatus->setText(
                u"Positions are the catalogue's own X/Y/Z — no redshift conversion."_s);
        return;
    }
    // The wording follows entryDistanceMpc()'s own order of precedence:
    // redshift-space (with a usable z) → a distance computed by the backend for
    // the selected model → the catalogue's distance column → the local Planck18
    // integral → the 300 Mpc default. Saying anything simpler would be saying
    // something false for some of the rows.
    const QString distanceField = Catalogue3DParser::detail::detectedDistanceField(this->schema);
    if (this->schema.distanceMode == CatalogueDistanceMode::RedshiftSpace) {
        this->m_cosmologyStatus->setText(
                u"Redshift-space cz/H₀ · %1 sources · the model is not used; a source "
                u"without a usable redshift keeps its catalogue distance"_s.arg(count));
        return;
    }
    const QString model = this->m_cosmologyModel.isEmpty() ? u"Planck18"_s : this->m_cosmologyModel;
    const bool backendComputed = model != u"Planck18"_s;
    const QString source = backendComputed ? model : u"Planck18 (local)"_s;
    QString line = u"%1 · %2 sources"_s.arg(source).arg(count);
    if (!distanceField.isEmpty()) {
        line += backendComputed
                // Once the backend has answered for a redshift, that distance is
                // the one used: the column only gets a say where it did not.
                ? u" · \"%1\" where the model produced nothing"_s.arg(distanceField)
                : u" · \"%1\" is used where a source has one"_s.arg(distanceField);
    }
    this->m_cosmologyStatus->setText(line);
}

void vtkWindowCatalogue3D::showCatalogueFieldsDialog()
{
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setWindowTitle(u"Catalogue Fields - %1"_s.arg(QFileInfo(this->filepath).fileName()));
    dlg->resize(560, 460);

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *header = new QLabel(u"Catalogue Fields"_s, dlg);
    header->setStyleSheet(VisivoTheme::sidebarHeaderStyleSheet());
    layout->addWidget(header);

    // Which column is which coordinate is the thing that decides where every
    // source is drawn, so it is stated first and by name — a catalogue whose
    // l/b were read as RA/Dec looks perfectly plausible and is 90° wrong.
    const auto coordinateLine = [this]() -> QString {
        if (!this->schema.raFieldName.isEmpty() || !this->schema.decFieldName.isEmpty())
            return u"Sky: %1 / %2"_s.arg(this->schema.raFieldName, this->schema.decFieldName);
        if (!this->schema.xFieldName.isEmpty())
            return u"Cartesian: %1 / %2 / %3"_s.arg(this->schema.xFieldName,
                                                    this->schema.yFieldName,
                                                    this->schema.zFieldName);
        return u"No coordinate columns were recognised."_s;
    };

    auto *summary = new QLabel(
            u"%1\n%2 sources · %3 columns"_s.arg(coordinateLine())
                    .arg(this->entries.size())
                    .arg(this->schema.fields.size()),
            dlg);
    summary->setStyleSheet(VisivoTheme::infoValueStyleSheet());
    layout->addWidget(summary);

    auto *table = new QTableWidget(static_cast<int>(this->schema.fields.size()), 2, dlg);
    table->setHorizontalHeaderLabels({ u"Column"_s, u"Type"_s });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 320);
    for (int row = 0; row < static_cast<int>(this->schema.fields.size()); ++row) {
        table->setItem(row, 0, new QTableWidgetItem(this->schema.fields[row].name));
        table->setItem(row, 1, new QTableWidgetItem(this->schema.fields[row].type));
    }
    layout->addWidget(table, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg->show();
}
