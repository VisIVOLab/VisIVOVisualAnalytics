#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QVariantMap>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressDialog;
class QPushButton;
class QSpinBox;

/**
 * S-02 — Line-width map tool window.
 *
 * Non-modal scientific tool window.  Computation runs asynchronously on a
 * pooled thread.  The window may be kept open and reused for multiple compute
 * runs; it is always safe to close it, even mid-computation.
 *
 * Spectral-axis awareness
 * ───────────────────────
 * The constructor accepts the backend-reported spectral axis type (CTYPE3-style
 * string, e.g. "VRAD", "FREQ") and unit (CUNIT3-style string, e.g. "km/s",
 * "GHz").  These are classified into SpectralAxisType to decide:
 *
 *   • Velocity cubes  — "Rest frequency" row is hidden; the backend can
 *     compute FWHM/EW directly in velocity units with no conversion.
 *   • Frequency/Wavelength cubes — "Rest frequency" row is shown and its
 *     value is validated (> 0) before dispatch.  The value is passed to the
 *     backend as rest_freq_hz for velocity-axis conversion.
 *   • Unknown — treated conservatively as frequency/wavelength (rest freq
 *     shown, but zero is accepted so the user can still run without it).
 *
 * Lifetime safety
 * ───────────────
 * The worker lambda captures only value-typed copies of backend context
 * (backendUrl, backendToken, sessionId, datasetId, numeric scalars).  A fresh
 * BackendClient is created inside the worker and destroyed when the call
 * returns — there is no shared mutable object across threads.
 *
 * QPointer<LinewidthDialog> and a request-ID counter protect the finished()
 * handler: if the window is deleted while a request is in-flight, the pointer
 * becomes null and the handler returns without touching any UI.  If the user
 * clicks Compute a second time the request ID advances and the older result is
 * silently discarded.
 *
 * The QFutureWatcher is heap-allocated with no parent so that its lifetime is
 * managed exclusively by the deleteLater() call inside the finished() lambda,
 * independent of the window's lifetime.
 */
class LinewidthDialog : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * Classification of the spectral (third) axis of the cube.
     * Drives whether a rest-frequency input is shown to the user.
     */
    enum class SpectralAxisType {
        Velocity,    ///< VRAD, VOPT, FELO, VELO — no rest-freq conversion needed.
        Frequency,   ///< FREQ — rest-freq required for velocity-axis output.
        Wavelength,  ///< WAVE, AWAV — rest-freq required for velocity-axis output.
        Unknown,     ///< Channels or unrecognised — rest-freq shown but optional.
    };

Q_SIGNALS:
    /// Emitted when the line-width map is computed, so the caller promotes it to a first-class
    /// PANE product (#5) instead of a standalone window. Payload = the raw float map + geometry.
    void mapReady(const QByteArray &floatData, int width, int height, float rangeMin,
                  float rangeMax, const QString &label, const QString &unit,
                  const QVariantMap &recipe);

public:

    /**
     * @param backendUrl            Backend base URL, e.g. "http://host:8000".
     * @param backendToken          Auth token (may be empty — BackendClient reads ~/.visivo_token).
     * @param sessionId             Active session identifier.
     * @param datasetId             Dataset to run the computation on.
     * @param depth                 Spectral depth of the cube (clamps channel spinners).
     * @param initialThreshold      Default value for the threshold mask spinner.
     * @param spectralAxisType      Backend-reported CTYPE3 string (e.g. "VRAD", "FREQ").
     *                              Pass an empty string when unknown.
     * @param spectralAxisUnit      Backend-reported CUNIT3 string (e.g. "km/s", "GHz").
     *                              Pass an empty string when unknown.
     * @param parent                Optional parent widget.
     */
    LinewidthDialog(const QString &backendUrl,
                    const QString &backendToken,
                    const QString &sessionId,
                    const QString &datasetId,
                    int depth,
                    double initialThreshold,
                    const QString &spectralAxisType = QString(),
                    const QString &spectralAxisUnit = QString(),
                    QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

public:
    /// The session's noise estimate: the mask is turned on at 3σ of it, and the
    /// map's provenance says so. Called again on a re-estimate; once the
    /// threshold has been edited by hand it is the user's and is left alone.
    void setNoiseEstimate(double sigma, const QString &where);
    /// The cube's rest frequency (RESTFRQ), so a frequency cube's widths come out
    /// in km/s without the user retyping what the header already says.
    void setRestFrequencyHz(double restFreqHz);

private slots:
    void onCompute();

private:
    void setUiBusy(bool busy);

    /** Classify the raw CTYPE3/CUNIT3 strings into SpectralAxisType. */
    static SpectralAxisType classifySpectralAxis(const QString &axisType,
                                                 const QString &axisUnit);

    /** Human-readable description of \a type, suitable for a UI label. */
    static QString spectralAxisTypeLabel(SpectralAxisType type);

    // Backend context — value copies, safe to capture by value in worker lambdas.
    QString  m_backendUrl;
    QString  m_backendToken;
    QString  m_sessionId;
    QString  m_datasetId;
    int      m_depth{ 1 };

    // Spectral axis classification (derived once in constructor).
    SpectralAxisType m_spectralAxisType{ SpectralAxisType::Unknown };
    QString          m_spectralAxisUnit;   // raw unit string, e.g. "km/s"

    // Re-entrancy guard and stale-result fence.
    bool     m_busy{ false };
    /// The shared window-modal progress sheet (ToolProgressSheet.h). The dialog's
    /// own status bar is not enough on its own: a compute that takes minutes
    /// needs something that says "working" without the user hunting for it.
    QPointer<QProgressDialog> m_progressSheet;
    int      m_currentRequestId{ 0 };

    // Form widgets.
    QLabel         *m_axisInfoLabel{ nullptr };   ///< Read-only "Spectral axis: …" display.
    QComboBox      *m_methodCombo{ nullptr };
    QLabel         *m_restFreqLabel{ nullptr };   ///< Hidden for velocity axes.
    QDoubleSpinBox *m_restFreqSpin{ nullptr };    ///< Hidden for velocity axes.
    QSpinBox       *m_chanStartSpin{ nullptr };
    QSpinBox       *m_chanEndSpin{ nullptr };
    QCheckBox      *m_maskCheck{ nullptr };
    QDoubleSpinBox *m_thresholdSpin{ nullptr };
    QLabel         *m_thresholdSourceLabel{ nullptr };
    QString         m_thresholdSource;   ///< "3σ of the noise estimate, …"; empty when typed
    double          m_estimateThreshold{ 0.0 };   ///< exact 3σ (the box rounds)
    bool            m_thresholdTouched{ false };  ///< edited by hand: estimates no longer apply
    QPushButton    *m_computeBtn{ nullptr };
};
