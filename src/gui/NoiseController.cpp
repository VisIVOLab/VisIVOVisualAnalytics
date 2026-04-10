// NoiseController.cpp
// Owns the noise-estimation flow that was previously split across
// vtkWindowCube.cpp: fetchRemoteNoise() free function, noiseEstimateWatcher
// connection, startNoiseEstimation(), and showNoiseEstimateResult().

#include "NoiseController.h"

#include "CubeNoiseTypes.h"
#include "NoiseRegionDialog.h"
#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"

#include <QDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Free worker function – called off the UI thread by QtConcurrent::run.
// No captures that could dangle; all inputs are value-copied by the lambda.
// ─────────────────────────────────────────────────────────────────────────────
static CubeNoiseEstimateResult runNoiseRequest(const QString &backendUrl,
                                               const QString &datasetId,
                                               const QString &sessionId,
                                               const QString &backendToken,
                                               int x0, int x1,
                                               int y0, int y1,
                                               int channelStart,
                                               int channelEnd)
{
    CubeNoiseEstimateResult result;

    BackendClient client(backendUrl, backendToken);
    client.setSessionId(sessionId);

    const auto response =
            client.requestNoise(datasetId, x0, x1, y0, y1, channelStart, channelEnd);

    if (!response.valid) {
        result.errorMessage =
                response.error.isEmpty() ? u"Backend noise estimation failed."_s
                                         : response.error;
        return result;
    }

    result.valid        = true;
    result.channelStart = response.channelStart;
    result.channelEnd   = response.channelEnd;
    result.numChannels  = response.numChannels;
    result.mad          = response.mad;
    result.sigma        = response.sigma;
    result.x0           = response.x0;
    result.x1           = response.x1;
    result.y0           = response.y0;
    result.y1           = response.y1;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// NoiseController
// ─────────────────────────────────────────────────────────────────────────────

NoiseController::NoiseController(const QString &backendUrl, const QString &datasetId,
                                 const QString &sessionId, const QString &backendToken,
                                 int width, int height, int depth,
                                 QWidget *parentWindow,
                                 std::function<void()> onRunFinished,
                                 QObject *parent)
    : QObject(parent)
    , m_backendUrl(backendUrl)
    , m_datasetId(datasetId)
    , m_sessionId(sessionId)
    , m_backendToken(backendToken)
    , m_width(width)
    , m_height(height)
    , m_depth(depth)
    , m_parentWindow(parentWindow)
    , m_onRunFinished(std::move(onRunFinished))
{
    QObject::connect(&m_watcher, &QFutureWatcher<CubeNoiseEstimateResult>::finished,
                     this, &NoiseController::onWatcherFinished);
}

bool NoiseController::isBusy() const
{
    return m_watcher.isRunning();
}

void NoiseController::startEstimation()
{
    if (m_watcher.isRunning()) {
        return; // guard against re-entrancy
    }

    NoiseRegionDialog dlg(m_width, m_height, m_depth, m_parentWindow);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const int x0           = dlg.x0();
    const int x1           = dlg.x1();
    const int y0           = dlg.y0();
    const int y1           = dlg.y1();
    const int channelStart = dlg.channelStart();
    const int channelEnd   = dlg.channelEnd();

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
            QStringLiteral("vtkWindowCube"),
            QStringLiteral("Noise estimation started (x:%1-%2 y:%3-%4 ch:%5-%6).")
                    .arg(x0).arg(x1).arg(y0).arg(y1).arg(channelStart).arg(channelEnd),
            m_datasetId, m_sessionId,
            QStringLiteral("noise"));

    if (auto *win = qobject_cast<QMainWindow *>(m_parentWindow)) {
        win->statusBar()->showMessage(u"Estimating backend noise..."_s, 2000);
    }

    // Notify immediately so vtkWindowCube can disable the action while busy.
    if (m_onRunFinished) {
        // Not a finished callback here — we call it now to trigger an
        // availability refresh. The same callback is also called on completion.
        // Use a direct single-shot call; no need for a separate "started" signal.
        m_onRunFinished();
    }

    // Capture values to avoid dangling refs if the window closes (very unlikely
    // during an in-flight request, but defensive is cheap here).
    const QString backendUrl   = m_backendUrl;
    const QString datasetId    = m_datasetId;
    const QString sessionId    = m_sessionId;
    const QString backendToken = m_backendToken;

    m_watcher.setFuture(QtConcurrent::run(
            [backendUrl, datasetId, sessionId, backendToken,
             x0, x1, y0, y1, channelStart, channelEnd]() -> CubeNoiseEstimateResult {
                return runNoiseRequest(backendUrl, datasetId, sessionId, backendToken,
                                      x0, x1, y0, y1, channelStart, channelEnd);
            }));
}

void NoiseController::onWatcherFinished()
{
    const auto result = m_watcher.result();

    if (!result.valid) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote,
                QStringLiteral("vtkWindowCube"),
                result.errorMessage.isEmpty() ? QStringLiteral("Noise estimation failed.")
                                              : result.errorMessage,
                m_datasetId, m_sessionId,
                QStringLiteral("noise"));

        if (auto *win = qobject_cast<QMainWindow *>(m_parentWindow)) {
            win->statusBar()->showMessage(
                    result.errorMessage.isEmpty() ? u"Noise estimation failed."_s
                                                  : result.errorMessage,
                    4000);
        }
    } else {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                QStringLiteral("vtkWindowCube"),
                QStringLiteral("Noise estimation completed."),
                m_datasetId, m_sessionId,
                QStringLiteral("noise"));

        showResult(result);
    }

    // Always call back so vtkWindowCube can re-evaluate action availability.
    if (m_onRunFinished) {
        m_onRunFinished();
    }
}

void NoiseController::showResult(const CubeNoiseEstimateResult &result)
{
    // Compute per-array statistics (mean, median, min, max) over finite values.
    auto summarize = [](const QVector<double> &values) -> std::array<double, 4> {
        QVector<double> finite;
        finite.reserve(values.size());
        for (double v : values) {
            if (std::isfinite(v)) {
                finite.push_back(v);
            }
        }
        if (finite.isEmpty()) {
            constexpr double nan = std::numeric_limits<double>::quiet_NaN();
            return { nan, nan, nan, nan };
        }
        std::sort(finite.begin(), finite.end());
        const double sum  = std::accumulate(finite.begin(), finite.end(), 0.0);
        const double mean = sum / static_cast<double>(finite.size());
        const double med  = finite[finite.size() / 2];
        return { mean, med, finite.front(), finite.back() };
    };

    const auto sigmaStats = summarize(result.sigma);
    const auto madStats   = summarize(result.mad);

    const QString details =
            u"Channels analyzed: %1 (%2..%3)\n"
            "Region: x=%4..%5, y=%6..%7\n\n"
            "Sigma summary\n"
            "Mean: %8\nMedian: %9\nMin: %10\nMax: %11\n\n"
            "MAD summary\n"
            "Mean: %12\nMedian: %13\nMin: %14\nMax: %15"_s
                    .arg(result.numChannels)
                    .arg(result.channelStart)
                    .arg(result.channelEnd)
                    .arg(result.x0).arg(result.x1)
                    .arg(result.y0).arg(result.y1)
                    .arg(sigmaStats[0], 0, 'g', 8)
                    .arg(sigmaStats[1], 0, 'g', 8)
                    .arg(sigmaStats[2], 0, 'g', 8)
                    .arg(sigmaStats[3], 0, 'g', 8)
                    .arg(madStats[0], 0, 'g', 8)
                    .arg(madStats[1], 0, 'g', 8)
                    .arg(madStats[2], 0, 'g', 8)
                    .arg(madStats[3], 0, 'g', 8);

    QMessageBox::information(m_parentWindow, u"Noise Estimate"_s, details);

    if (auto *win = qobject_cast<QMainWindow *>(m_parentWindow)) {
        win->statusBar()->showMessage(u"Noise estimation completed."_s, 3000);
    }
}
