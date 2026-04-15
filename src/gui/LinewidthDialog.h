#pragma once

#include <QMainWindow>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QSpinBox;

/**
 * S-02 — Line-width map tool window.
 *
 * Non-modal scientific tool window.  Computation runs asynchronously on a
 * pooled thread.  The window may be kept open and reused for multiple compute
 * runs; it is always safe to close it, even mid-computation.
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
     * @param backendUrl        Backend base URL, e.g. "http://host:8000".
     * @param backendToken      Auth token (may be empty — BackendClient reads ~/.visivo_token).
     * @param sessionId         Active session identifier.
     * @param datasetId         Dataset to run the computation on.
     * @param depth             Spectral depth of the cube (clamps channel spinners).
     * @param initialThreshold  Default value for the threshold mask spinner.
     * @param parent            Optional parent widget.
     */
    LinewidthDialog(const QString &backendUrl,
                    const QString &backendToken,
                    const QString &sessionId,
                    const QString &datasetId,
                    int depth,
                    double initialThreshold,
                    QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onCompute();

private:
    void setUiBusy(bool busy);

    // Backend context — value copies, safe to capture by value in worker lambdas.
    QString  m_backendUrl;
    QString  m_backendToken;
    QString  m_sessionId;
    QString  m_datasetId;
    int      m_depth{ 1 };

    // Re-entrancy guard and stale-result fence.
    bool     m_busy{ false };
    int      m_currentRequestId{ 0 };

    // Form widgets.
    QComboBox      *m_methodCombo{ nullptr };
    QDoubleSpinBox *m_restFreqSpin{ nullptr };
    QSpinBox       *m_chanStartSpin{ nullptr };
    QSpinBox       *m_chanEndSpin{ nullptr };
    QCheckBox      *m_maskCheck{ nullptr };
    QDoubleSpinBox *m_thresholdSpin{ nullptr };
    QPushButton    *m_computeBtn{ nullptr };
};
