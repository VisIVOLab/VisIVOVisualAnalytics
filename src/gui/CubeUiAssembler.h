#pragma once

// QPointer<T> requires the full type definition (uses static_cast<T*> internally).
#include <QComboBox>
#include <QLabel>
#include <QPointer>
#include <QString>
#include <QToolButton>
#include <QWidget>

class QAction;

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
                         QAction *cameraRoi);

    // ── Bidirectional sync helpers ────────────────────────────────────────────
    void sync2dModeSidebar(bool sliceActive);
    void syncVolumeModeSidebar(int idx);   ///< idx: 0=Composite, 1=MIP, 2=MinIP

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
    // ── Tools page buttons ────────────────────────────────────────────────────
    QPointer<QToolButton> m_btnEstimateNoise;
    QPointer<QToolButton> m_btnDataProbe;
    QPointer<QToolButton> m_btnPvDiagram;
    QPointer<QToolButton> m_btnCameraRoi;
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
};
