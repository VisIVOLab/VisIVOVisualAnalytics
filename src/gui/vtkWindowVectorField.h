#ifndef vtkWindowVectorField_h
#define vtkWindowVectorField_h

#include "app/BackendClient.h"

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <QMainWindow>
#include <QPointer>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QVTKOpenGLNativeWidget;
class vtkActor;
class vtkBoxWidget2;
class vtkImageData;
class vtkLookupTable;
class vtkOrientationMarkerWidget;
class vtkScalarBarActor;

/// 3-D viewer for a cosmic-flow velocity field (F1): glyph arrows + streamlines
/// over a 3-component vector grid, coloured by speed. The display grid is a
/// backend LOD (<=128³); the user can box a region and pull it at FULL
/// resolution (ROI on demand). Rendering is local VTK so seeding + box scale
/// stay interactive.
class vtkWindowVectorField : public QMainWindow
{
    Q_OBJECT

public:
    explicit vtkWindowVectorField(const QString &path,
                                  BackendVelocityGridResult grid,
                                  const QString &datasetId,
                                  const QString &backendUrl,
                                  const QString &backendToken,
                                  const QString &sessionId,
                                  QWidget *parent = nullptr);
    ~vtkWindowVectorField() override = default;

private:
    void setupUi();
    void setupRenderer();
    void buildImageData(const BackendVelocityGridResult &grid,
                        vtkSmartPointer<vtkImageData> &out, double sx, double sy, double sz,
                        double ox, double oy, double oz);   // grid bytes -> vtkImageData(vectors+speed)
    // Build glyph / streamline actors for a given grid; returns a new actor.
    vtkSmartPointer<vtkActor> makeGlyphActor(vtkImageData *img, double cell);
    vtkSmartPointer<vtkActor> makeStreamActor(vtkImageData *img);
    void rebuildLodActors();
    void applyBoxSize(double boxMpc);   // rescale display spacing/origin in place
    void applyColorMap(const QString &name);
    void resetCamera();
    void toggleRoiWidget(bool on);
    void loadFullResRoi();
    void showBasins(bool on);   // F1 phase-2: gravitational-basin (watershed) regions
    void placeBasins();         // (re)build the basin actor from m_basins at the current scale

    // Per-axis full-resolution cell size + centred origin (point-centred model:
    // box>0 → box/(n-1) and -box/2; else unit spacing, -(n-1)/2). The display
    // (LOD) cell is fullCell * downsampleFactor.
    void fullCells(double &sx, double &sy, double &sz) const;
    void originVec(double &ox, double &oy, double &oz) const;

    QString m_path;
    BackendVelocityGridResult m_grid;    // the LOD display grid
    QString m_datasetId;
    QString m_backendUrl;
    QString m_backendToken;
    QString m_sessionId;
    double m_boxMpc = 0.0;               // 0 -> grid units

    QPointer<QVTKOpenGLNativeWidget> vtkWidget;
    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    vtkNew<vtkRenderer> renderer;
    vtkSmartPointer<vtkOrientationMarkerWidget> axesWidget;
    vtkSmartPointer<vtkScalarBarActor> scalarBar;
    vtkSmartPointer<vtkLookupTable> lut;
    vtkSmartPointer<vtkBoxWidget2> roiWidget;

    vtkSmartPointer<vtkImageData> imageData;     // LOD display grid
    vtkSmartPointer<vtkActor> glyphActor;
    vtkSmartPointer<vtkActor> streamActor;
    vtkSmartPointer<vtkImageData> roiImageData;  // full-res ROI grid
    vtkSmartPointer<vtkActor> roiGlyphActor;
    vtkSmartPointer<vtkActor> roiStreamActor;
    vtkSmartPointer<vtkActor> basinActor;        // gravitational-basin surfaces
    vtkSmartPointer<vtkLookupTable> basinLut;    // categorical basin colours
    BackendVelocityBasinsResult m_basins;        // cached labels (re-placed on scale change)

    // Sidebar controls
    QPointer<QCheckBox> chkGlyphs;
    QPointer<QCheckBox> chkStreamlines;
    QPointer<QSpinBox> spinSeeds;
    QPointer<QSpinBox> spinMaxArrows;
    QPointer<QDoubleSpinBox> spinGlyphScale;
    QPointer<QDoubleSpinBox> spinBoxSize;
    QPointer<QComboBox> comboLut;
    QPointer<QCheckBox> chkRoi;
    QPointer<QPushButton> btnLoadRoi;
    QPointer<QCheckBox> chkBasins;
    QPointer<QComboBox> comboBasinDir;   // Attraction (superclusters) / Repulsion (voids)
    QPointer<QLabel> summaryLabel;
};

#endif
