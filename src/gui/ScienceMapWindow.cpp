#include "ScienceMapWindow.h"
#include "ColorMaps.h"

#include <QVTKOpenGLNativeWidget.h>
#include <vtkCamera.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageMapToColors.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkInteractorStyleImage.h>
#include <vtkLookupTable.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkSmartPointer.h>
#include <vtkTrivialProducer.h>

#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

ScienceMapWindow::ScienceMapWindow(const QString &title,
                                   const QByteArray &data, int width, int height,
                                   float rangeMin, float rangeMax,
                                   QWidget *parent)
    : QMainWindow(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    resize(800, 600);

    // ── Central VTK widget ────────────────────────────────────────────────────
    auto *central = new QWidget(this);
    auto *layout  = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    m_vtkWidget = new QVTKOpenGLNativeWidget(central);
    layout->addWidget(m_vtkWidget);
    setCentralWidget(central);

    m_renderWindow->AddRenderer(m_renderer);
    m_renderer->SetBackground(0.21, 0.23, 0.25);
    m_renderer->GetActiveCamera()->ParallelProjectionOn();
    m_vtkWidget->setRenderWindow(m_renderWindow);
    m_vtkWidget->setEnableTouchEventProcessing(false);

    vtkNew<vtkInteractorStyleImage> style;
    m_renderWindow->GetInteractor()->SetInteractorStyle(style);

    // ── Fill vtkImageData from raw float32 bytes ──────────────────────────────
    auto imageData = vtkSmartPointer<vtkImageData>::New();
    imageData->SetDimensions(width, height, 1);
    imageData->AllocateScalars(VTK_FLOAT, 1);
    if (data.size() >= width * height * static_cast<int>(sizeof(float))) {
        std::memcpy(imageData->GetScalarPointer(), data.constData(),
                    static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
                            * sizeof(float));
    }

    m_source->SetOutput(imageData);

    // ── LUT ──────────────────────────────────────────────────────────────────
    m_lut->SetNanColor(0.2, 0.2, 0.2, 1.0);
    m_lut->SetTableRange(static_cast<double>(rangeMin), static_cast<double>(rangeMax));
    ColorMaps::SetColorMap(m_lut);

    // ── Pipeline ──────────────────────────────────────────────────────────────
    m_colors->SetInputData(imageData);
    m_colors->SetLookupTable(m_lut);

    vtkNew<vtkImageSliceMapper> mapper;
    mapper->SetInputConnection(m_colors->GetOutputPort());
    vtkNew<vtkImageSlice> actor;
    actor->SetMapper(mapper);
    actor->GetProperty()->SetInterpolationTypeToNearest();
    m_renderer->AddViewProp(actor);

    // ── Colourbar ─────────────────────────────────────────────────────────────
    m_colorbar->SetLookupTable(m_lut);
    m_colorbar->SetMaximumWidthInPixels(100);
    m_colorbar->SetPosition(0.88, 0.1);
    const QByteArray barTitle = title.toUtf8();
    m_colorbar->SetTitle(barTitle.constData());
    m_renderer->AddViewProp(m_colorbar);

    m_renderer->ResetCamera();
    m_renderWindow->Render();
}
