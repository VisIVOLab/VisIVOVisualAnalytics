#pragma once

// QPointer<T> requires the full type definition (uses static_cast<T*> internally).
#include <QComboBox>
#include <QLabel>
#include <QPointer>
#include <QString>
#include <QToolButton>
#include <QWidget>

class QAction;
class QStatusBar;

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
                         QAction *momentMap,       ///< ui->actionMomentMap
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
    QComboBox *renderModeCombo()   const { return m_comboRenderMode; }
    QComboBox *volumeModeCombo()   const { return m_comboVolumeMode; }
    QComboBox *lutCombo()          const { return m_comboLut; }
    QWidget   *volumeModeCard()    const { return m_volumeModeCard; }
    QWidget   *surfaceColorCard()  const { return m_surfaceColorCard; }
    QWidget   *colorMapCard()      const { return m_colorMapCard; }
    // 2D View Settings
    QComboBox *combo2dMode()       const { return m_combo2dMode; }
    QComboBox *sliceLutCombo()     const { return m_comboSliceLut; }
    QComboBox *momentLutCombo()    const { return m_comboMomentLut; }
    QWidget   *sliceControlsCard() const { return m_sliceControlsCard; }
    QWidget   *sliceLutCard()      const { return m_sliceLutCard; }
    QWidget   *sliceContoursCard() const { return m_sliceContoursCard; }
    QWidget   *momentInfoCard()    const { return m_momentInfoCard; }
    QWidget   *momentLutCard()     const { return m_momentLutCard; }
    QLabel    *momentInfoLabel()   const { return m_momentInfoLabel; }
    // Info / Stats
    QLabel    *infoDsLoadState()   const { return m_infoDsLoadState; }
    QLabel    *infoWcsBadge()      const { return m_infoWcsBadge; }
    QLabel    *infoWcsFrame()      const { return m_infoWcsFrame; }
    QLabel    *infoSpectral()      const { return m_infoSpectral; }
    QLabel    *infoRenderMode()    const { return m_infoRenderMode; }
    QLabel    *info2dMode()        const { return m_info2dMode; }
    QLabel    *infoMomentState()   const { return m_infoMomentState; }

private:
    /// Generate a gradient QPixmap for a named color map (used as combo-box icons).
    static QPixmap buildLutPreview(const QString &name, int w, int h);

    Ui::vtkWindowCube *m_ui;

    // ── 3D View Settings widgets ──────────────────────────────────────────────
    QPointer<QComboBox>   m_comboRenderMode;
    QPointer<QComboBox>   m_comboVolumeMode;
    QPointer<QComboBox>   m_comboLut;
    QPointer<QWidget>     m_volumeModeCard;
    QPointer<QWidget>     m_surfaceColorCard;
    QPointer<QWidget>     m_colorMapCard;
    // ── 2D View Settings widgets ──────────────────────────────────────────────
    QPointer<QComboBox>   m_combo2dMode;
    QPointer<QWidget>     m_sliceControlsCard;
    QPointer<QWidget>     m_sliceLutCard;
    QPointer<QWidget>     m_sliceContoursCard;
    QPointer<QComboBox>   m_comboSliceLut;
    QPointer<QWidget>     m_momentInfoCard;
    QPointer<QLabel>      m_momentInfoLabel;
    QPointer<QWidget>     m_momentLutCard;
    QPointer<QComboBox>   m_comboMomentLut;
    // ── Tools page buttons (ANALYSIS) ─────────────────────────────────────────
    QPointer<QToolButton> m_btnEstimateNoise;
    QPointer<QToolButton> m_btnMomentMap;
    QPointer<QToolButton> m_btnPvDiagram;
    QPointer<QToolButton> m_btnBoxRegion;
    QPointer<QToolButton> m_btnCircleRegion;
    QPointer<QToolButton> m_btnPolygonRegion;
    QPointer<QToolButton> m_btnAnnulusRegion;
    // ── Tools page buttons (EXPLORATION) ──────────────────────────────────────
    QPointer<QToolButton> m_btnDataProbe;
    // ── 3D View Settings page buttons ─────────────────────────────────────────
    QPointer<QToolButton> m_btnCameraRoi;
    // ── 2D View Settings page buttons ─────────────────────────────────────────
    QPointer<QToolButton> m_btnWcsAxes;
    // ── Info / Stats labels ───────────────────────────────────────────────────
    QPointer<QLabel>      m_infoDsName;
    QPointer<QLabel>      m_infoDsDimensions;
    QPointer<QLabel>      m_infoDsLoadState;
    QPointer<QLabel>      m_infoWcsBadge;
    QPointer<QLabel>      m_infoWcsFrame;
    QPointer<QLabel>      m_infoSpectral;
    QPointer<QLabel>      m_infoRenderMode;
    QPointer<QLabel>      m_info2dMode;
    QPointer<QLabel>      m_infoMomentState;
    // ── Status-bar labels ─────────────────────────────────────────────────────
    QPointer<QLabel>      m_dataStateLabel;
    QPointer<QLabel>      m_sanityLabel;
    QPointer<QLabel>      m_wcsStatusLabel;
};
