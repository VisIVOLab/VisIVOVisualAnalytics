#include "DatasetWindowFactory.h"

#include "vtkWindowCube.h"
#include "vtkWindowImage.h"

#include <QString>
#include <QWidget>

QWidget *DatasetWindowFactory::createWindow(const DatasetOpenInfo &dataset, const QString &backendUrl,
                                            const QString &backendToken, QWidget *parent) const
{
    switch (dataset.kind) {
    case DatasetKind::Image:
        return new vtkWindowImage(dataset.filepath, parent);
    case DatasetKind::Cube:
        return new vtkWindowCube(dataset.filepath, backendUrl, backendToken, parent);
    case DatasetKind::Unknown:
    default:
        return nullptr;
    }
}
