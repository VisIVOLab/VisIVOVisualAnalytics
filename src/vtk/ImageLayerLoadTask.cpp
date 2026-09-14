#include "ImageLayerLoadTask.h"

#include "AstroUtils.h"
#include "vtkFITSReader.h"

#include "wcs.h"

#include <QDebug>
#include <QFile>
#include <QElapsedTimer>

#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkNew.h>

#include <cmath>

ImageLayerLoadResult loadImageLayer(const ImageLayerLoadRequest &request)
{
    QElapsedTimer totalTimer;
    totalTimer.start();
    ImageLayerLoadResult result;
    result.filepath = request.layerFilepath;

    if (request.masterFilepath.empty() || request.layerFilepath.empty()) {
        result.errorMessage = "Empty dataset path.";
        return result;
    }

    // The reader reports nothing on a missing or unreadable file, so check here:
    // callers (the Stokes companion queue in particular) rely on `valid` being
    // false to stop and tell the user, instead of adding an empty layer.
    if (!QFile::exists(QString::fromStdString(request.layerFilepath))) {
        result.errorMessage = "File not found: " + request.layerFilepath;
        return result;
    }

    vtkNew<vtkFITSReader> reader;
    reader->SetFileName(request.layerFilepath.c_str());
    QElapsedTimer readTimer;
    readTimer.start();
    reader->Update();
    qDebug().noquote()
            << QStringLiteral("[perf][layer] FITS read: %1 ms").arg(readTimer.elapsed());

    result.imageData = vtkSmartPointer<vtkImageData>::New();
    QElapsedTimer deepCopyTimer;
    deepCopyTimer.start();
    result.imageData->DeepCopy(reader->GetOutput());
    qDebug().noquote()
            << QStringLiteral("[perf][layer] DeepCopy: %1 ms").arg(deepCopyTimer.elapsed());
    result.scalarRange = { reader->GetMin(), reader->GetMax() };

    int dims[3] = { 0, 0, 0 };
    result.imageData->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || result.imageData->GetNumberOfPoints() <= 0) {
        result.imageData = nullptr;
        result.errorMessage = "Not a readable 2-D FITS image: " + request.layerFilepath;
        return result;
    }

    QElapsedTimer payloadTimer;
    payloadTimer.start();
    AstroUtils astroMaster(request.masterFilepath);
    AstroUtils astroNewLayer(request.layerFilepath);

    const double scalingFactor = astroNewLayer.getSecPix() / astroMaster.getSecPix();
    result.spacing = { scalingFactor, scalingFactor, 1. };

    double posNewLayer[2];
    double pix[2] = { 0., 0. };
    astroNewLayer.xy2sky(pix, posNewLayer, WCS_J2000);
    astroMaster.sky2xy(posNewLayer, pix, WCS_J2000);
    result.origin = { pix[0], pix[1], 0. };

    pix[0] = 0.;
    pix[1] = 100.;
    astroNewLayer.xy2sky(pix, posNewLayer, WCS_J2000);
    astroMaster.sky2xy(posNewLayer, pix, WCS_J2000);
    const double m = std::abs((pix[1] - result.origin[1]) / (pix[0] - result.origin[0]));
    result.rotationDegrees = 90. - std::atan(m) * 180. / vtkMath::Pi();
    qDebug().noquote() << QStringLiteral("[perf][layer] payload preparation: %1 ms").arg(
            payloadTimer.elapsed());

    result.valid = true;
    qDebug().noquote()
            << QStringLiteral("[perf][layer] load total: %1 ms").arg(totalTimer.elapsed());
    return result;
}
