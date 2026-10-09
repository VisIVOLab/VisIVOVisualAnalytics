#include "MomentMapComputeTask.h"

#include "MomentProcessingService.h"

#include <QDebug>
#include <QElapsedTimer>
#include <vtkImageData.h>

namespace {
bool isSupportedMomentOrder(int order)
{
    switch (order) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}
}

MomentMapComputeResult computeMomentMap(const MomentMapComputeRequest &request)
{
    QElapsedTimer totalTimer;
    totalTimer.start();
    MomentMapComputeResult result;

    if (request.filepath.isEmpty() && request.datasetId.isEmpty()) {
        result.errorMessage = "Empty dataset path.";
        return result;
    }

    if (!isSupportedMomentOrder(request.momentOrder)) {
        result.errorMessage = "Unsupported moment order.";
        return result;
    }

    MomentProcessingService processing;
    QElapsedTimer computeTimer;
    computeTimer.start();
    const auto processed = processing.computeMoment(
            MomentRequest { request.filepath, request.datasetId, request.backendUrl,
                            request.sessionId, request.backendToken,
                            request.momentOrder, request.channelStart, request.channelEnd,
                            request.maskEnabled, request.thresholdValue,
                            request.thresholdAuto });
    qDebug().noquote()
            << QStringLiteral("[perf][moment] backend compute: %1 ms").arg(computeTimer.elapsed());
    if (!processed.valid || !processed.image) {
        result.errorMessage = processed.error.isEmpty() ? QStringLiteral("Backend moment failed.")
                                                        : processed.error;
        return result;
    }

    result.valid = true;
    result.imageRange = processed.imageRange;
    result.imageData = processed.image;
    result.momentUnit = processed.momentUnit;
    result.bunit = processed.bunit;
    result.spectralAxisType = processed.spectralAxisType;
    result.spectralAxisUnit = processed.spectralAxisUnit;
    result.wcsStatus = processed.wcsStatus;
    result.wcsWarningMessage = processed.wcsWarningMessage;
    qDebug().noquote()
            << QStringLiteral("[perf][moment] worker total: %1 ms").arg(totalTimer.elapsed());
    return result;
}
