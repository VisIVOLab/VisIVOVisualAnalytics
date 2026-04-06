#include "MomentProcessingService.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"

#include <QByteArray>
#include <QDebug>
#include <vtkImageData.h>

#include <cstring>

MomentResult MomentProcessingService::computeMoment(const MomentRequest &request) const
{
    return this->computeMomentRemote(request);
}

MomentResult MomentProcessingService::computeMomentRemote(const MomentRequest &request) const
{
    BackendClient client = request.backendUrl.isEmpty()
            ? BackendClient()
            : BackendClient(request.backendUrl, request.backendToken);
    if (!request.sessionId.isEmpty()) {
        client.setSessionId(request.sessionId);
    }

    QString datasetId = request.datasetId;
    QString sessionId = request.sessionId;
    if (datasetId.isEmpty()) {
        const QString message = request.datasetPath.isEmpty()
                ? QStringLiteral("Moment computation requires a backend-visible dataset. Open or stage the dataset on the backend first.")
                : QStringLiteral("Moment computation requires a backend that can access this dataset. Stage or upload the dataset to the backend first.");
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote,
                QStringLiteral("MomentProcessingService"), message, {}, sessionId,
                QStringLiteral("moment"));
        return { nullptr, { 0., 0. }, false, message };
    }

    BackendMomentResult response;
    const auto taskStatus = client.waitForTaskCompletion(
            client.createMomentTask(datasetId, request.order, request.channelStart,
                                    request.channelEnd, request.maskEnabled,
                                    request.thresholdValue),
            QStringLiteral("[moment][task]"));
    if (taskStatus.status == QStringLiteral("completed")) {
        response = BackendClient::parseMomentResultObject(taskStatus.resultObject);
    }

    if (!response.valid) {
        if (!taskStatus.taskId.isEmpty()) {
            qWarning().noquote()
                    << QStringLiteral("[moment][task] fallback to sync task_id=%1")
                               .arg(taskStatus.taskId);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Task,
                    QStringLiteral("MomentProcessingService"),
                    QStringLiteral("Task path failed, falling back to sync moment request."),
                    datasetId, sessionId, QStringLiteral("moment"));
        }
        response = client.requestMoment(datasetId, request.order, request.channelStart,
                                        request.channelEnd, request.maskEnabled,
                                        request.thresholdValue);
        if (!taskStatus.taskId.isEmpty()) {
            qDebug().noquote() << QStringLiteral("[moment][task] sync fallback completed valid=%1")
                                          .arg(response.valid ? QStringLiteral("true")
                                                              : QStringLiteral("false"));
        }
    }

    if (!response.valid) {
        return { nullptr, { 0., 0. }, false,
                 response.error.isEmpty() ? QStringLiteral("Remote moment request failed.")
                                          : response.error };
    }

    if (response.scalarType != QStringLiteral("float32")) {
        return { nullptr, { 0., 0. }, false, QStringLiteral("Unsupported remote scalar type.") };
    }

    const qsizetype expectedBytes =
            static_cast<qsizetype>(response.width) * response.height * static_cast<qsizetype>(sizeof(float));
    if (response.width <= 0 || response.height <= 0 || response.data.size() != expectedBytes) {
        return { nullptr, { 0., 0. }, false, QStringLiteral("Invalid remote moment payload.") };
    }

    MomentResult result;
    result.valid = true;
    result.imageRange = { response.rangeMin, response.rangeMax };
    result.image = vtkSmartPointer<vtkImageData>::New();
    result.image->SetExtent(0, response.width - 1, 0, response.height - 1, 0, 0);
    result.image->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(result.image->GetScalarPointer(), response.data.constData(),
                static_cast<std::size_t>(expectedBytes));
    result.momentUnit = response.momentUnit;
    result.bunit = response.bunit;
    result.spectralAxisType = response.spectralAxisType;
    result.spectralAxisUnit = response.spectralAxisUnit;
    result.wcsStatus = response.wcsStatus;
    result.wcsWarningMessage = response.wcsWarningMessage;
    return result;
}
