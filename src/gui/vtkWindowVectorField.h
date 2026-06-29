#ifndef vtkWindowVectorField_h
#define vtkWindowVectorField_h

#include "app/BackendClient.h"

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <QMainWindow>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QVTKOpenGLNativeWidget;
class vtkActor;
class vtkImageData;
class vtkLookupTable;
class vtkOrientationMarkerWidget;
class vtkScalarBarActor;

/// 3-D viewer for a cosmic-flow velocity field (F1): glyph arrows + streamlines
/// over a 3-component vector grid, coloured by speed. The grid is fetched from
/// the backend (/v1/velocity/grid_bin) and rendered locally with VTK so seeding
/// and the physical box scale stay interactive.
class vtkWindowVectorField : public QMainWindow
{
    Q_OBJECT

public:
    explicit vtkWindowVectorField(const QString &path,
                                  BackendVelocityGridResult grid,
                                  QWidget *parent = nullptr);
    ~vtkWindowVectorField() override = default;

private:
    void setupUi();
    void setupRenderer();
    void buildImageData();        // grid bytes -> vtkImageData with "velocity" vectors
    void rebuildGlyphs();
    void rebuildStreamlines();
    void applyBoxSize(double boxMpc);   // rescale spacing/origin in place
    void applyColorMap(const QString &name);
    void resetCamera();

    QString m_path;
    BackendVelocityGridResult m_grid;
    double m_boxMpc = 0.0;        // 0 -> grid units

    QPointer<QVTKOpenGLNativeWidget> vtkWidget;
    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    vtkNew<vtkRenderer> renderer;
    vtkSmartPointer<vtkOrientationMarkerWidget> axesWidget;
    vtkSmartPointer<vtkScalarBarActor> scalarBar;
    vtkSmartPointer<vtkLookupTable> lut;

    vtkSmartPointer<vtkImageData> imageData;
    vtkSmartPointer<vtkActor> glyphActor;
    vtkSmartPointer<vtkActor> streamActor;

    // Sidebar controls
    QPointer<QCheckBox> chkGlyphs;
    QPointer<QCheckBox> chkStreamlines;
    QPointer<QSpinBox> spinSeeds;
    QPointer<QSpinBox> spinStride;
    QPointer<QDoubleSpinBox> spinGlyphScale;
    QPointer<QDoubleSpinBox> spinBoxSize;
    QPointer<QComboBox> comboLut;
    QPointer<QLabel> summaryLabel;
};

#endif
