#pragma once

// Save View As… (issue #528) for the 3-D views: the small dialog that asks what
// the picture is, and the glue that renders it and writes it. The rules (sizes,
// formats, limits) are in RenderExportPlan.h, the drawing in vtk/RenderExport.h.

#include "RenderExportPlan.h"
#include "vtk/RenderExport.h"

#include <QDialog>
#include <QImage>
#include <QPointer>
#include <QSize>
#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QWidget;
class vtkCubeAxesActor;
class vtkRenderer;
class vtkRenderWindow;

namespace visivo {

/// What a window gives to save its 3-D view.
struct SaveViewSource
{
    /// The dataset's (or the window's) name: the title and the file name.
    QString name;
    /// The view's widget: its size is the "current" size of the picture.
    QPointer<QWidget> view;
    /// The local render. Null for a view rendered on the server: `frame` (what
    /// the stream last showed) is then the picture, at its own size and opaque.
    vtkRenderWindow *window = nullptr;
    vtkRenderer *renderer = nullptr;
    QImage frame;
    /// The axes box of the scene (drawn as vectors in a PDF or SVG).
    vtkCubeAxesActor *axes = nullptr;
    /// Hides (true) and shows (false) what the scene draws itself: the axes,
    /// the scalar bar, the orientation marker.
    std::function<void(bool)> hideInScene;
    /// The colour bar is drawn in the scene, so a PNG already has it.
    bool colourbarInScene = false;
    /// Paints the colour bar over a view of the size given (empty: none).
    std::function<void(QPainter &, const QSize &)> colourbar;
};

class SaveViewDialog : public QDialog
{
    Q_OBJECT
public:
    /// `view`: the view's size in device pixels (zero for a streamed frame,
    /// whose size is fixed); `streamed`: the picture is a frame already
    /// rendered on the server.
    SaveViewDialog(const QSize &view, bool streamed, const QString &title, QWidget *parent);

    renderexport::Request request() const;
    QString title() const;

private:
    void refresh();

    QSize m_view;
    bool m_streamed;
    QPointer<QComboBox> m_format;
    QPointer<QComboBox> m_size;
    QPointer<QSpinBox> m_width;
    QPointer<QLabel> m_sizeNote;
    QPointer<QCheckBox> m_transparent;
    QPointer<QCheckBox> m_annotations;
    QPointer<QLineEdit> m_title;
    QPointer<QPushButton> m_ok;
};

/// Ask, render and write. Returns the file written, or an empty string when
/// the user cancelled or it failed (the failure is shown to the user).
QString saveViewAs(const SaveViewSource &source);

} // namespace visivo
