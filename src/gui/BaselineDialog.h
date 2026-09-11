#pragma once

#include <QMainWindow>
#include <QPair>
#include <QPointer>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressDialog;
class QLineEdit;
class QPushButton;
class QSpinBox;

/**
 * S-03 — Spectral baseline subtraction tool window.
 *
 * Non-modal scientific tool window.  Computation runs asynchronously on a
 * pooled thread.  The window stays open after a successful run so the user
 * can adjust parameters and rerun without reopening.
 *
 * Lifetime safety
 * ───────────────
 * The worker lambda captures only value-typed copies of backend context
 * (backendUrl, backendToken, sessionId, datasetId and the baseline parameters).
 * A fresh BackendClient is created inside the worker and destroyed when the
 * call returns — there is no shared mutable object across threads.
 *
 * QPointer<BaselineDialog> and a request-ID counter protect the finished()
 * handler: if the window is deleted while a request is in-flight the pointer
 * becomes null and the handler returns without touching any UI.  If the user
 * clicks "Subtract Baseline" a second time the request ID advances and any
 * older in-flight result is silently discarded on arrival.
 *
 * The QFutureWatcher is heap-allocated with no parent so that its lifetime is
 * managed exclusively by the deleteLater() call inside the finished() lambda,
 * independent of the window's lifetime.
 *
 * Output signal
 * ─────────────
 * baselineResultReady() is emitted (on the UI thread, inside finished()) when
 * the backend reports a successful subtraction.  Callers may connect to this
 * signal to load the new dataset into a viewer; the tool window itself does not
 * open any additional window.
 */
class BaselineDialog : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @param backendUrl    Backend base URL, e.g. "http://host:8000".
     * @param backendToken  Auth token (may be empty — BackendClient reads ~/.visivo_token).
     * @param sessionId     Active session identifier.
     * @param datasetId     Source dataset for the subtraction.
     * @param depth         Spectral depth (channel count) of the cube.
     * @param parent        Optional parent widget.
     */
    BaselineDialog(const QString &backendUrl,
                   const QString &backendToken,
                   const QString &sessionId,
                   const QString &datasetId,
                   int depth,
                   QWidget *parent = nullptr);

signals:
    /**
     * Emitted when baseline subtraction succeeds.
     * @param newDatasetId  Backend-assigned ID of the result dataset in this session.
     * @param summary       Human-readable summary string suitable for a status bar or log.
     */
    /// @param outputPath  Where the backend wrote the cube. It is a TEMPORARY
    ///                     file (the backend's temp directory), which the caller
    ///                     needs in order to reopen the result later — the
    ///                     summary string is for humans, not for parsing.
    void baselineResultReady(const QString &newDatasetId, const QString &outputPath,
                             const QString &summary);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onCompute();

private:
    /// Parse "1-50, 200-300" into 0-based [start,end] pairs.  Returns an empty
    /// vector on failure and writes a message to \a errorOut.
    static QVector<QPair<int,int>> parseLineFreeRanges(const QString &text,
                                                        int depth,
                                                        QString &errorOut);

    void setUiBusy(bool busy);

    // Backend context — value copies, safe to capture by value in worker lambdas.
    QString  m_backendUrl;
    QString  m_backendToken;
    QString  m_sessionId;
    QString  m_datasetId;
    int      m_depth{ 1 };

    // Re-entrancy guard and stale-result fence.
    bool     m_busy{ false };
    /// The shared window-modal progress sheet (ToolProgressSheet.h).
    QPointer<QProgressDialog> m_progressSheet;
    int      m_currentRequestId{ 0 };

    // Form widgets.
    QComboBox  *m_methodCombo{ nullptr };
    QLabel     *m_polyOrderLabel{ nullptr };
    QSpinBox   *m_polyOrderSpin{ nullptr };
    QLineEdit  *m_lineFreeEdit{ nullptr };
    QPushButton *m_computeBtn{ nullptr };
};
