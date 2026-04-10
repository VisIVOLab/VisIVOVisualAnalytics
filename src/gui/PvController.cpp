// PvController.cpp
// Owns the backend PV fetch + data decoding that was previously inlined in
// vtkWindowCube.cpp as the free function fetchRemotePv() and the body of
// extractCurrentPvDiagram() that is unrelated to rendering.
//
// The async dispatch (QtConcurrent::run) replaces the previous synchronous
// call: fetchRemotePv() did blocking network I/O on the UI thread.

#include "PvController.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"

#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QtConcurrent>

#include <utility>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Free worker function – runs off the UI thread via QtConcurrent::run.
// All inputs are value-copied; no captures that could dangle.
// Moved verbatim from vtkWindowCube.cpp where it was a file-scope free fn.
// ─────────────────────────────────────────────────────────────────────────────

static RemotePvFetchResult fetchRemotePv(const QString &backendUrl,
                                         const QString &datasetId,
                                         const std::vector<std::array<int, 2>> &vertices,
                                         int widthPixels,
                                         const QString &sessionId,
                                         const QString &backendToken)
{
    RemotePvFetchResult result;

    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);

    BackendCubePvResult response;
    const auto taskStatus = client.waitForTaskCompletion(
            client.createPvTask(datasetId, vertices, widthPixels),
            QStringLiteral("[pv][task]"));
    if (taskStatus.status == QStringLiteral("completed")) {
        response = BackendClient::parsePvResultObject(taskStatus.resultObject);
    }

    if (!response.valid) {
        if (!taskStatus.taskId.isEmpty()) {
            qWarning().noquote()
                    << QStringLiteral("[pv][task] fallback to sync task_id=%1")
                               .arg(taskStatus.taskId);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Task,
                    QStringLiteral("PvController"),
                    QStringLiteral("Task path failed, falling back to sync PV request."),
                    datasetId, sessionId, QStringLiteral("pv"));
        }
        response = client.requestPv(datasetId, vertices, widthPixels);
        if (!taskStatus.taskId.isEmpty()) {
            qDebug().noquote()
                    << QStringLiteral("[pv][task] sync fallback completed valid=%1")
                               .arg(response.valid ? QStringLiteral("true")
                                                   : QStringLiteral("false"));
        }
    }

    if (!response.valid) {
        result.errorMessage =
                response.error.isEmpty() ? u"Remote PV request failed."_s : response.error;
        return result;
    }
    if (response.scalarType != u"float32"_s) {
        result.errorMessage = u"Unsupported remote PV scalar type."_s;
        return result;
    }
    if (response.numSamples <= 0 || response.depth <= 0) {
        result.errorMessage = u"Remote PV payload has invalid dimensions."_s;
        return result;
    }

    const qsizetype expectedPositionsBytes =
            static_cast<qsizetype>(response.numSamples) * static_cast<qsizetype>(sizeof(float));
    const qsizetype expectedDataBytes =
            static_cast<qsizetype>(response.numSamples) * response.depth
            * static_cast<qsizetype>(sizeof(float));
    if (response.positions.size() != expectedPositionsBytes
        || response.data.size() != expectedDataBytes) {
        result.errorMessage = u"Invalid remote PV payload."_s;
        return result;
    }

    const auto *rawPositions = reinterpret_cast<const float *>(response.positions.constData());
    result.positions.resize(response.numSamples);
    for (int i = 0; i < response.numSamples; ++i) {
        result.positions[i] = static_cast<double>(rawPositions[i]);
    }

    // Populate physical arcsecond positions when the backend returns them.
    if (!response.positionsArcsec.isEmpty()
        && response.positionsArcsec.size() == expectedPositionsBytes) {
        const auto *rawArcsec =
                reinterpret_cast<const float *>(response.positionsArcsec.constData());
        result.positionsArcsec.resize(response.numSamples);
        for (int i = 0; i < response.numSamples; ++i) {
            result.positionsArcsec[i] = static_cast<double>(rawArcsec[i]);
        }
        result.spatialUnit = response.spatialUnit;
    }

    const auto *rawValues = reinterpret_cast<const float *>(response.data.constData());
    result.values.resize(response.numSamples * response.depth);
    for (int i = 0; i < response.numSamples * response.depth; ++i) {
        result.values[i] = static_cast<double>(rawValues[i]);
    }

    result.valid = true;
    result.numSamples = response.numSamples;
    result.depth = response.depth;
    result.computedOn = response.computedOn;
    result.widthPixels = response.widthPixels;
    result.vertexCount = response.vertexCount;
    result.totalLength = response.totalLength;
    result.validSamples = response.validSamples;
    result.pixelScaleArcsecPerPixel = response.pixelScaleArcsecPerPixel;
    result.spectralAxisType = response.spectralAxisType;
    result.spectralAxisUnit = response.spectralAxisUnit;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// PvController
// ─────────────────────────────────────────────────────────────────────────────

PvController::PvController(
        const QString &backendUrl,
        const QString &datasetId,
        const QString &sessionId,
        const QString &backendToken,
        QWidget *parentWindow,
        std::function<void()> onRunFinished,
        std::function<void(const RemotePvFetchResult &)> onCompleted,
        QObject *parent)
    : QObject(parent)
    , m_backendUrl(backendUrl)
    , m_datasetId(datasetId)
    , m_sessionId(sessionId)
    , m_backendToken(backendToken)
    , m_parentWindow(parentWindow)
    , m_onRunFinished(std::move(onRunFinished))
    , m_onCompleted(std::move(onCompleted))
{
    QObject::connect(&m_watcher, &QFutureWatcher<RemotePvFetchResult>::finished,
                     this, &PvController::onWatcherFinished);
}

bool PvController::isBusy() const
{
    return m_watcher.isRunning();
}

void PvController::startExtraction(const std::vector<std::array<int, 2>> &vertices,
                                   int widthPixels)
{
    if (m_watcher.isRunning()) {
        return; // guard against re-entrancy
    }

    if (auto *win = qobject_cast<QMainWindow *>(m_parentWindow)) {
        win->statusBar()->showMessage(u"Requesting PV diagram..."_s);
    }

    // Notify immediately so vtkWindowCube can disable the PV action.
    if (m_onRunFinished) {
        m_onRunFinished();
    }

    // Capture everything by value – safe across thread boundary.
    const QString backendUrl   = m_backendUrl;
    const QString datasetId    = m_datasetId;
    const QString sessionId    = m_sessionId;
    const QString backendToken = m_backendToken;

    m_watcher.setFuture(QtConcurrent::run(
            [backendUrl, datasetId, sessionId, backendToken,
             vertices, widthPixels]() -> RemotePvFetchResult {
                auto result = fetchRemotePv(backendUrl, datasetId, vertices, widthPixels,
                                            sessionId, backendToken);
                // Embed the original request context so the callback can build
                // provenance strings without touching shared mutable state.
                result.capturedVertices    = vertices;
                result.capturedWidthPixels = widthPixels;
                return result;
            }));
}

void PvController::onWatcherFinished()
{
    // Re-enable the PV action first (same ordering as MomentMapController).
    if (m_onRunFinished) {
        m_onRunFinished();
    }

    const auto result = m_watcher.result();
    if (!result.valid) {
        const QString message = result.errorMessage.isEmpty()
                ? u"PV computation failed on the backend."_s
                : result.errorMessage;

        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote,
                QStringLiteral("PvController"), message,
                m_datasetId, m_sessionId, QStringLiteral("pv"));
        QMessageBox::warning(m_parentWindow, u"PV Diagram"_s, message);
        if (auto *win = qobject_cast<QMainWindow *>(m_parentWindow)) {
            win->statusBar()->showMessage(message, 4000);
        }
        return;
    }

    if (m_onCompleted) {
        m_onCompleted(result);
    }
}
