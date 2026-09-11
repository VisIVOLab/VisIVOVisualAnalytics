// NoiseController.cpp
// Owns the noise-estimation flow that was previously split across
// vtkWindowCube.cpp: fetchRemoteNoise() free function, noiseEstimateWatcher
// connection, startNoiseEstimation(), and showNoiseEstimateResult().

#include "NoiseController.h"

#include "ToolProgressSheet.h"

#include "AnalysisParamStore.h"
#include "CubeNoiseTypes.h"
#include "NoiseRegionDialog.h"
#include "ToolResultDialog.h"
// vtkWindowCube is included in the .cpp (not the .h) so the controller's
// header stays decoupled. We need the full type here to qobject_cast the
// parent widget for the live preview-rectangle hookup.
#include "vtkWindowCube.h"
#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QPointer>
#include <QStatusBar>
#include <QtConcurrent>

#include <algorithm>

#include <QMessageBox>
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

    result.valid         = true;
    result.channelStart  = response.channelStart;
    result.channelEnd    = response.channelEnd;
    result.numChannels   = response.numChannels;
    result.mad           = response.mad;
    result.sigma         = response.sigma;
    result.x0            = response.x0;
    result.x1            = response.x1;
    result.y0            = response.y0;
    result.y1            = response.y1;
    result.blankFraction = response.blankFraction;
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
}

bool NoiseController::isBusy() const
{
    return m_busy;
}

void NoiseController::startEstimation()
{
    if (m_busy) {
        return; // guard against re-entrancy
    }

    // The window STAYS OPEN across the estimate, like every other tool dialog:
    // Estimate does not close it, the progress sheet is window-modal on IT, and
    // when the numbers land the same window is there to sample another region.
    if (m_regionDialog) {
        m_regionDialog->show();
        m_regionDialog->raise();
        m_regionDialog->activateWindow();
        return;
    }
    auto *dlgPtr = new NoiseRegionDialog(m_width, m_height, m_depth, m_parentWindow);
    m_regionDialog = dlgPtr;
    dlgPtr->setAttribute(Qt::WA_DeleteOnClose);
    NoiseRegionDialog &dlg = *dlgPtr;

    // Live preview overlays (amber rectangle on the 2D slice + amber semi-
    // transparent box inside the 3D volume): connect the dialog's
    // regionPreviewChanged signal to the cube viewer's preview slot so the
    // user sees their selection in both views while tuning the spinboxes.
    // qobject_cast is safe here — m_parentWindow is the cube viewer that
    // owns this controller. If the parent ever isn't a cube window (no
    // current case), the cast yields null and the connect is a no-op.
    auto *cube = qobject_cast<vtkWindowCube *>(m_parentWindow);
    if (cube) {
        QObject::connect(&dlg, &NoiseRegionDialog::regionPreviewChanged,
                         cube, &vtkWindowCube::setNoiseRegionPreview);
        // Restore the noise region last used for this dataset (if any).
        const QVariantMap saved =
                AnalysisParamStore().load(cube->datasetPersistKey(), u"noise"_s);
        // Require the full set so a truncated/old settings file doesn't silently
        // default missing axes to 0.
        if (saved.contains(u"x0"_s) && saved.contains(u"x1"_s) && saved.contains(u"y0"_s)
            && saved.contains(u"y1"_s) && saved.contains(u"chStart"_s)
            && saved.contains(u"chEnd"_s)) {
            dlg.setRegion(saved.value(u"x0"_s).toInt(), saved.value(u"x1"_s).toInt(),
                          saved.value(u"y0"_s).toInt(), saved.value(u"y1"_s).toInt(),
                          saved.value(u"chStart"_s).toInt(), saved.value(u"chEnd"_s).toInt());
        }
    }

    // Closing the window clears the preview overlays; running an estimate keeps
    // them, because the user is still looking at the region they sampled.
    QObject::connect(dlgPtr, &QObject::destroyed, this, [this]() {
        // Only if nothing newer owns the overlay. With a non-modal settings
        // window the user can run an estimate, close the window, and still have
        // a result window showing the region it sampled.
        this->clearPreviewIfCurrent(m_previewGeneration);
    });
    QObject::connect(dlgPtr, &NoiseRegionDialog::estimateRequested, this,
                     [this, dlgPtr]() { this->runEstimate(dlgPtr); });
    dlgPtr->show();
    dlgPtr->raise();
    dlgPtr->activateWindow();
}

void NoiseController::runEstimate(NoiseRegionDialog *dialog)
{
    using namespace Qt::StringLiterals;
    if (!dialog || m_busy)
        return;
    auto *cube = qobject_cast<vtkWindowCube *>(m_parentWindow);
    NoiseRegionDialog &dlg = *dialog;

    // Normalise here rather than trusting the widgets' order, and refuse an
    // inverted range outright: the dialog's own Estimate button is gated on
    // validity, but this is the function that PERSISTS the region and sends it
    // to the backend, so it does its own checking.
    const int x0 = std::min(dlg.x0(), dlg.x1());
    const int x1 = std::max(dlg.x0(), dlg.x1());
    const int y0 = std::min(dlg.y0(), dlg.y1());
    const int y1 = std::max(dlg.y0(), dlg.y1());
    const int channelStart = std::min(dlg.channelStart(), dlg.channelEnd());
    const int channelEnd   = std::max(dlg.channelStart(), dlg.channelEnd());
    if (x0 > x1 || y0 > y1 || channelStart > channelEnd) {
        QMessageBox::warning(dialog, u"Estimate Noise"_s,
                             u"The selected region is empty. Check the X, Y and channel "
                             u"ranges."_s);
        return;
    }

    // Remember this region for the dataset so the dialog re-opens pre-filled.
    if (cube) {
        AnalysisParamStore().save(cube->datasetPersistKey(), u"noise"_s, {
                { u"x0"_s, x0 }, { u"x1"_s, x1 }, { u"y0"_s, y0 }, { u"y1"_s, y1 },
                { u"chStart"_s, channelStart }, { u"chEnd"_s, channelEnd } });
    }

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
            QStringLiteral("NoiseController"),
            QStringLiteral("Noise estimation started (x:%1-%2 y:%3-%4 ch:%5-%6).")
                    .arg(x0).arg(x1).arg(y0).arg(y1).arg(channelStart).arg(channelEnd),
            m_datasetId, m_sessionId,
            QStringLiteral("noise"));

    if (auto *win = qobject_cast<QMainWindow *>(m_parentWindow)) {
        win->statusBar()->showMessage(u"Estimating backend noise..."_s, 2000);
    }
    ++m_previewGeneration;   // this run now owns the overlay
    // On the dialog the user pressed Estimate in, not on the viewer behind it.
    m_progressSheet = visivo::showToolProgressSheet(dialog, u"Estimating the noise"_s);
    dialog->setBusy(true);

    m_busy = true;

    // Notify immediately so vtkWindowCube can disable the action while busy.
    if (m_onRunFinished) {
        m_onRunFinished();
    }

    // Capture values by value – safe across thread boundary and if window closes.
    const QString backendUrl   = m_backendUrl;
    const QString datasetId    = m_datasetId;
    const QString sessionId    = m_sessionId;
    const QString backendToken = m_backendToken;

    QPointer<NoiseController> self(this);
    auto *watcher = new QFutureWatcher<CubeNoiseEstimateResult>();
    QObject::connect(watcher, &QFutureWatcher<CubeNoiseEstimateResult>::finished,
                     watcher, [self, watcher, datasetId, sessionId]() {
        const auto result = watcher->result();
        watcher->deleteLater();

        if (!self) {
            return; // controller was destroyed while request was in-flight
        }

        self->m_busy = false;
        visivo::closeToolProgressSheet(self->m_progressSheet);
        if (self->m_regionDialog)
            self->m_regionDialog->setBusy(false);

        if (!result.valid) {
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote,
                    QStringLiteral("NoiseController"),
                    result.errorMessage.isEmpty() ? QStringLiteral("Noise estimation failed.")
                                                  : result.errorMessage,
                    datasetId, sessionId,
                    QStringLiteral("noise"));

            if (auto *win = qobject_cast<QMainWindow *>(self->m_parentWindow)) {
                win->statusBar()->showMessage(
                        result.errorMessage.isEmpty() ? u"Noise estimation failed."_s
                                                      : result.errorMessage,
                        4000);
            }
        } else {
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                    QStringLiteral("NoiseController"),
                    QStringLiteral("Noise estimation completed."),
                    datasetId, sessionId,
                    QStringLiteral("noise"));

            self->showResult(result);
        }

        // Always call back so vtkWindowCube can re-evaluate action availability.
        if (self->m_onRunFinished) {
            self->m_onRunFinished();
        }
    });

    watcher->setFuture(QtConcurrent::run(
            [backendUrl, datasetId, sessionId, backendToken,
             x0, x1, y0, y1, channelStart, channelEnd]() -> CubeNoiseEstimateResult {
                return runNoiseRequest(backendUrl, datasetId, sessionId, backendToken,
                                      x0, x1, y0, y1, channelStart, channelEnd);
            }));
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

    auto fmt = [](double v) -> QString {
        return QString::number(v, 'g', 8);
    };

    QString subtitle =
            u"Channels: %1..%2  (%3 planes)  ·  region x=%4..%5, y=%6..%7"_s
                    .arg(result.channelStart)
                    .arg(result.channelEnd)
                    .arg(result.numChannels)
                    .arg(result.x0).arg(result.x1)
                    .arg(result.y0).arg(result.y1);

    // If the region overlaps a lot of off-source blanked pixels, MAD/σ get
    // pulled down toward the value of the blank marker (typically 0). Make
    // that explicit so the user doesn't read a misleading "σ=0" — and so
    // they know to retry with a tighter spatial box.
    if (result.blankFraction > 0.20) {
        const int pct = static_cast<int>(std::round(result.blankFraction * 100.0));
        subtitle += u"\n⚠ %1 % of the selected pixels are blank (=0). "_s.arg(pct);
        if (result.blankFraction > 0.50) {
            subtitle += u"σ excluding zeros - pick a tighter region inside "
                        u"the data footprint for a clean estimate."_s;
        } else {
            subtitle += u"Result still computed over all finite pixels."_s;
        }
    }

    // Record it in Session Data before showing the dialog: the dialog is
    // WA_DeleteOnClose, and σ is what the user reaches for next (moment-map
    // threshold, isosurface level, mask cut-off). The product carries no
    // renderer — it is a measurement, not something to draw in a pane — so it
    // shows as a statistics row whose Provenance holds these numbers.
    {
        QVariantMap params;
        params.insert(u"Region"_s, u"x=%1..%2, y=%3..%4"_s
                                           .arg(result.x0).arg(result.x1)
                                           .arg(result.y0).arg(result.y1));
        params.insert(u"Channels"_s, u"%1..%2 (%3 planes)"_s
                                             .arg(result.channelStart)
                                             .arg(result.channelEnd)
                                             .arg(result.numChannels));
        params.insert(u"σ mean"_s,   fmt(sigmaStats[0]));
        params.insert(u"σ median"_s, fmt(sigmaStats[1]));
        params.insert(u"σ min"_s,    fmt(sigmaStats[2]));
        params.insert(u"σ max"_s,    fmt(sigmaStats[3]));
        params.insert(u"MAD mean"_s,   fmt(madStats[0]));
        params.insert(u"MAD median"_s, fmt(madStats[1]));
        params.insert(u"MAD min"_s,    fmt(madStats[2]));
        params.insert(u"MAD max"_s,    fmt(madStats[3]));
        if (result.blankFraction > 0.0) {
            params.insert(u"Blank (=0) pixels"_s,
                          u"%1 %"_s.arg(std::round(result.blankFraction * 100.0)));
        }
        const QString stableKey = u"noise-%1_%2_%3_%4_ch%5_%6"_s
                                          .arg(result.x0).arg(result.x1)
                                          .arg(result.y0).arg(result.y1)
                                          .arg(result.channelStart).arg(result.channelEnd);
        emit noiseProvenanceReady(
                stableKey,
                u"Noise σ=%1 (x %2..%3, y %4..%5)"_s.arg(fmt(sigmaStats[1]))
                        .arg(result.x0).arg(result.x1).arg(result.y0).arg(result.y1),
                params);
    }

    auto *dlg = new ToolResultDialog(m_parentWindow);
    dlg->setTitle(u"Noise Estimation"_s);
    dlg->setSubtitle(subtitle);
    dlg->addSection(u"Sigma"_s, {
        { u"Mean"_s,   fmt(sigmaStats[0]) },
        { u"Median"_s, fmt(sigmaStats[1]) },
        { u"Min"_s,    fmt(sigmaStats[2]) },
        { u"Max"_s,    fmt(sigmaStats[3]) },
    });
    dlg->addSection(u"MAD"_s, {
        { u"Mean"_s,   fmt(madStats[0]) },
        { u"Median"_s, fmt(madStats[1]) },
        { u"Min"_s,    fmt(madStats[2]) },
        { u"Max"_s,    fmt(madStats[3]) },
    });

    // Tear down the noise-region preview overlays (2D rect + 3D box) when
    // the user closes the result dialog. QObject::destroyed fires after
    // close() because ToolResultDialog uses WA_DeleteOnClose; using a
    // QPointer guards against the cube viewer being destroyed first.
    {
        // Tied to the generation this result belongs to: an older result window
        // closing must not wipe the overlay of a newer estimate.
        const quint64 generation = m_previewGeneration;
        QObject::connect(dlg, &QObject::destroyed, this,
                         [this, generation]() { this->clearPreviewIfCurrent(generation); });
    }

    dlg->show();

    if (auto *win = qobject_cast<QMainWindow *>(m_parentWindow)) {
        win->statusBar()->showMessage(u"Noise estimation completed."_s, 3000);
    }
}

void NoiseController::clearPreviewIfCurrent(quint64 generation)
{
    if (generation != m_previewGeneration)
        return;   // something newer drew over it
    if (auto *cube = qobject_cast<vtkWindowCube *>(m_parentWindow))
        cube->clearNoiseRegionPreview();
}
