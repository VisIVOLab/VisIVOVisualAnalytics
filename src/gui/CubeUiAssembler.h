#pragma once

// All widget members below are raw pointers.  Their lifetime is guaranteed by
// the Qt parent-child hierarchy: every widget is created as a child of a page
// owned by Ui::vtkWindowCube, which is destroyed together with CubeUiAssembler.
// Forward declarations keep this header free of heavyweight Qt widget includes.
#include <QString>

class QAction;
class QComboBox;
class QLabel;
class QStatusBar;
class QToolButton;
class QWidget;
class SegmentedToggle;

namespace Ui {
class vtkWindowCube;
}

/// Assembles and owns the sidebar UI widgets for vtkWindowCube.
///
/// Responsibilities:
///   - Building the four sidebar pages (3D View Settings, 2D View Settings, Tools, Info/Stats)
///   - Storing pointers to all dynamic sidebar widgets
///   - Exposing widget accessors so vtkWindowCube's update/sync methods can reach them
///   - Wiring tool-button actions after they are created in the constructor
///
/// NOT responsible for:
///   - VTK rendering, scene management, or pipeline operations
///   - Backend communication
///   - Reading or modifying rendering state (LUT application, etc.)
class CubeUiAssembler
{
public:
    /// Context passed once to buildInfoStatsPage() for initial population.
    struct InfoStatsInput {
        QString filepath;
        int     datasetWidth  { 0 };
        int     datasetHeight { 0 };
        int     datasetDepth  { 0 };
        QString wcsFrameLabel;
        QString spectralAxisTitle;
        QString wcsStatus;           ///< "ok" / "degraded" / other
        QString wcsWarningMessage;   ///< shown as tooltip on the WCS badge
    };

    explicit CubeUiAssembler(Ui::vtkWindowCube *ui);
    ~CubeUiAssembler();

    // Disable copy
    CubeUiAssembler(const CubeUiAssembler &) = delete;
    CubeUiAssembler &operator=(const CubeUiAssembler &) = delete;

    // ── Page builders (call once from setupSidebar()) ─────────────────────────
    QWidget *buildThresholdPage();
    QWidget *build2dViewPage();
    QWidget *buildToolsPage();
    QWidget *buildInfoStatsPage(const InfoStatsInput &info);

    // ── Late wiring (call after QActions are created in the constructor) ──────
    void wireToolActions(QAction *estimateNoise,
                         QAction *extractSpectrum,
                         QAction *extractPvDiagram,
                         QAction *cameraRoi,
                         QAction *momentMap,       ///< ui->actionComputeMoment
                         QAction *linewidthMap,    ///< ui->actionLinewidthMap
                         QAction *baselineSub,     ///< ui->actionBaselineSubtract
                         QAction *stackCubes,      ///< ui->actionStackCubes
                         QAction *openInVR,        ///< Tools → Open in VR (or nullptr)
                         QAction *wcsAxes,         ///< actionShowWcsAxes
                         QAction *boxRegion,       ///< actionBoxRegion
                         QAction *circleRegion,    ///< actionCircleRegion
                         QAction *polygonRegion,   ///< actionPolygonRegion
                         QAction *annulusRegion);  ///< actionAnnulusRegion

    // ── Bidirectional sync helpers ────────────────────────────────────────────
    void sync2dModeSidebar(bool sliceActive);
    void syncVolumeModeSidebar(int idx);   ///< idx: 0=Composite, 1=MIP, 2=MinIP

    // ── Data structs for UI update methods ───────────────────────────────────

    /// Pre-built data for the Moment Map info card.
    /// vtkWindowCube builds this struct from its own state; assembler only renders.
    struct MomentCardData {
        enum class State { NoMoment, Computing, Ready, Error };
        State   state            { State::NoMoment };
        QString detail;               ///< context string for Computing/Error states
        QString provenanceSummary;    ///< used when state == Ready
        QString provenanceDetails;    ///< tooltip when state == Ready
    };

    /// Pre-built data for the Info/Stats page.
    /// vtkWindowCube builds this struct from its own state; assembler only renders.
    struct InfoStatsData {
        QString loadStateText;
        QString loadStateStyleSheet;
        QString wcsFrame;
        QString spectralAxis;
        QString wcsBadgeText;
        QString wcsBadgeStyleSheet;
        QString wcsBadgeTooltip;
        QString renderModeText;
        QString imageModeText;
        QString momentStateText;
        QString momentStateStyleSheet;
    };

    // ── UI update methods (pure rendering — no internal state access) ─────────
    void updateMomentCard(const MomentCardData &data);
    void updateInfoStats(const InfoStatsData &data);

    // ── Status-bar data structs ───────────────────────────────────────────────

    /// Pre-built data for the WCS status label in the status bar.
    struct WcsStatusData {
        bool    visible    { true };
        QString text;
        QString styleSheet;
        QString tooltip;
    };

    /// Pre-built data for the sanity-check label in the status bar.
    struct SanityStatusData {
        QString text;
        QString tooltip;
    };

    /// Pre-built data for the data-state badge label in the status bar.
    struct DataStateUiData {
        QString text;
        QString tooltip;
    };

    // ── Status-bar label lifecycle ────────────────────────────────────────────
    /// Creates the three status-bar labels and registers them with @p bar.
    /// Must be called once, after the assembler is constructed.
    void initStatusBarLabels(QStatusBar *bar);

    // ── Status-bar update methods (pure rendering) ────────────────────────────
    void updateWcsStatus(const WcsStatusData &data);
    void updateSanityStatus(const SanityStatusData &data);
    void updateDataState(const DataStateUiData &data);

    // ── Widget accessors (used by vtkWindowCube's update/sync methods) ────────
    // 3D View Settings
    // The "Render Mode" (Isosurface / Volume) and "Volume Rendering"
    // (Composite / MIP / MinIP) selectors are SegmentedToggle widgets — a
    // pair-of-pills control that matches the Observatory Console design.
    // They expose a QComboBox-flavoured API (currentIndex / setCurrentIndex
    // / currentIndexChanged) so the rest of the wiring stayed intact.
    SegmentedToggle *renderModeCombo()   const { return m_comboRenderMode; }
    SegmentedToggle *volumeModeCombo()   const { return m_comboVolumeMode; }
    QComboBox       *lutCombo()          const { return m_comboLut; }
    QWidget    *volumeModeCard()    const { return m_volumeModeCard; }
    QWidget    *surfaceColorCard()  const { return m_surfaceColorCard; }
    QWidget    *colorMapCard()      const { return m_colorMapCard; }
    QToolButton *lutEdit3dButton()  const { return m_btnLutEdit3D; }
    // 2D View Settings
    QComboBox  *combo2dMode()          const { return m_combo2dMode; }
    QComboBox  *sliceLutCombo()        const { return m_comboSliceLut; }
    QComboBox  *momentLutCombo()       const { return m_comboMomentLut; }
    QWidget    *sliceControlsCard()    const { return m_sliceControlsCard; }
    QWidget    *sliceLutCard()         const { return m_sliceLutCard; }
    QWidget    *sliceContoursCard()    const { return m_sliceContoursCard; }
    QWidget    *momentInfoCard()       const { return m_momentInfoCard; }
    QWidget    *momentLutCard()        const { return m_momentLutCard; }
    QLabel     *momentInfoLabel()      const { return m_momentInfoLabel; }
    QToolButton *lutEditSliceButton()  const { return m_btnLutEditSlice; }
    QToolButton *lutEditMomentButton() const { return m_btnLutEditMoment; }
    // Info / Stats
    QLabel    *infoDsLoadState()   const { return m_infoDsLoadState; }
    QLabel    *infoWcsBadge()      const { return m_infoWcsBadge; }
    QLabel    *infoWcsFrame()      const { return m_infoWcsFrame; }
    QLabel    *infoSpectral()      const { return m_infoSpectral; }
    QLabel    *infoRenderMode()    const { return m_infoRenderMode; }
    QLabel    *info2dMode()        const { return m_info2dMode; }
    QLabel    *infoMomentState()   const { return m_infoMomentState; }

private:
    Ui::vtkWindowCube *m_ui;

    // ── 3D View Settings widgets ──────────────────────────────────────────────
    SegmentedToggle      *m_comboRenderMode{ nullptr };
    SegmentedToggle      *m_comboVolumeMode{ nullptr };
    QComboBox            *m_comboLut{ nullptr };
    QWidget              *m_volumeModeCard{ nullptr };
    QWidget              *m_surfaceColorCard{ nullptr };
    QWidget              *m_colorMapCard{ nullptr };
    // ── 2D View Settings widgets ──────────────────────────────────────────────
    QComboBox            *m_combo2dMode{ nullptr };
    QWidget              *m_sliceControlsCard{ nullptr };
    QWidget              *m_sliceLutCard{ nullptr };
    QWidget              *m_sliceContoursCard{ nullptr };
    QComboBox            *m_comboSliceLut{ nullptr };
    QWidget              *m_momentInfoCard{ nullptr };
    QLabel               *m_momentInfoLabel{ nullptr };
    QWidget              *m_momentLutCard{ nullptr };
    QComboBox            *m_comboMomentLut{ nullptr };
    // ── Tools page buttons (EXPLORATION) ──────────────────────────────────────
    QToolButton          *m_btnDataProbe{ nullptr };
    QToolButton          *m_btnOpenInVR{ nullptr };
    // ── Tools page buttons (ANALYSIS) ─────────────────────────────────────────
    QToolButton          *m_btnEstimateNoise{ nullptr };
    QToolButton          *m_btnMomentMap{ nullptr };
    QToolButton          *m_btnLineWidth{ nullptr };
    QToolButton          *m_btnBaselineSub{ nullptr };
    QToolButton          *m_btnStackCubes{ nullptr };
    QToolButton          *m_btnPvDiagram{ nullptr };
    // ── Tools page buttons (REGIONS) ──────────────────────────────────────────
    QToolButton          *m_btnBoxRegion{ nullptr };
    QToolButton          *m_btnCircleRegion{ nullptr };
    QToolButton          *m_btnPolygonRegion{ nullptr };
    QToolButton          *m_btnAnnulusRegion{ nullptr };
    // ── 3D View Settings page buttons ─────────────────────────────────────────
    QToolButton          *m_btnCameraRoi{ nullptr };
    QToolButton          *m_btnLutEdit3D{ nullptr };
    // ── 2D View Settings page buttons ─────────────────────────────────────────
    QToolButton          *m_btnWcsAxes{ nullptr };
    QToolButton          *m_btnLutEditSlice{ nullptr };
    QToolButton          *m_btnLutEditMoment{ nullptr };
    // ── Info / Stats labels ───────────────────────────────────────────────────
    QLabel               *m_infoDsName{ nullptr };
    QLabel               *m_infoDsDimensions{ nullptr };
    QLabel               *m_infoDsLoadState{ nullptr };
    QLabel               *m_infoWcsBadge{ nullptr };
    QLabel               *m_infoWcsFrame{ nullptr };
    QLabel               *m_infoSpectral{ nullptr };
    QLabel               *m_infoRenderMode{ nullptr };
    QLabel               *m_info2dMode{ nullptr };
    QLabel               *m_infoMomentState{ nullptr };
    // ── Status-bar labels ─────────────────────────────────────────────────────
    QLabel               *m_dataStateLabel{ nullptr };
    QLabel               *m_sanityLabel{ nullptr };
    QLabel               *m_wcsStatusLabel{ nullptr };
};
