#pragma once

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageMapToColors.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkTrivialProducer.h>

#include <QMainWindow>
#include <QPointer>

class QVTKOpenGLNativeWidget;

/**
 * Minimal non-modal window for displaying a 2-D float32 map received from
 * the backend as a raw byte array (float32 LE, row-major).
 *
 * Used by LinewidthDialog and any future tool that produces a 2-D image result.
 */
class ScienceMapWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @param title     Window title and colourbar label.
     * @param data      Raw float32 LE bytes, \c width × height pixels, row-major.
     * @param width     Image width in pixels.
     * @param height    Image height in pixels.
     * @param rangeMin  Scalar range minimum (for LUT).
     * @param rangeMax  Scalar range maximum (for LUT).
     * @param parent    Optional parent widget.
     */
    ScienceMapWindow(const QString &title,
                     const QByteArray &data, int width, int height,
                     float rangeMin, float rangeMax,
                     QWidget *parent = nullptr);

private:
    QPointer<QVTKOpenGLNativeWidget>   m_vtkWidget;
    vtkNew<vtkGenericOpenGLRenderWindow> m_renderWindow;
    vtkNew<vtkRenderer>                m_renderer;
    vtkNew<vtkTrivialProducer>         m_source;
    vtkNew<vtkImageMapToColors>        m_colors;
    vtkNew<vtkLookupTable>             m_lut;
    vtkNew<vtkScalarBarActor>          m_colorbar;
};
