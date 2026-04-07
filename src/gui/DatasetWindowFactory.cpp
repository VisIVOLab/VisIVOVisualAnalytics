#include "DatasetWindowFactory.h"

#include "vtkWindowCube.h"
#include "vtkWindowImage.h"

#include <QString>
#include <QWidget>

// DatasetWindowFactory is no longer used. All dataset opens go through
// MainWindow::openData() → BackendClient::openDataset(), which always obtains
// a backend datasetId before creating a viewer window.
// This file is kept as a build stub to avoid removing it from CMakeLists.
QWidget *DatasetWindowFactory::createWindow(const DatasetOpenInfo &, const QString &,
                                            const QString &, QWidget *) const
{
    return nullptr;
}
