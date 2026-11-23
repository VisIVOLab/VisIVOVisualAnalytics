#ifndef SIMULATIONDIAGNOSTICSDIALOG_H
#define SIMULATIONDIAGNOSTICSDIALOG_H

#include "app/BackendClient.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>

#include <atomic>
#include <memory>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class QCustomPlot;

class SimulationDiagnosticsDialog : public QDialog
{
    Q_OBJECT
public:
    /// @p filters: the filters the window's rows were last queried with (the
    /// applied ones, not the list being edited).
    SimulationDiagnosticsDialog(const QString &backendUrl, const QString &backendToken,
                                const QString &sessionId, const QString &datasetId,
                                const QStringList &numericFields,
                                const std::vector<BackendTabularFilter> &filters,
                                QWidget *parent = nullptr);
    ~SimulationDiagnosticsDialog() override;

    /// The window applied other filters: the population line follows, and a
    /// result on show (or a run under way) is computed again for them.
    void setFilters(const std::vector<BackendTabularFilter> &filters);

private:
    void run();
    void cancel();
    void finishRun();
    void clearResult();
    void updatePopulation();
    void present(const BackendCatalogueDiagnosticsResponse &result);
    void saveCsv();

    QString m_backendUrl;
    QString m_backendToken;
    QString m_sessionId;
    QString m_datasetId;
    std::vector<BackendTabularFilter> m_filters;
    BackendCatalogueDiagnosticsResponse m_result;
    bool m_hasResult = false;
    /// Bumped by every run and by Cancel: an answer for an older one is
    /// dropped.
    quint64 m_runId = 0;
    /// A run is shown as under way (the bar, Cancel).
    bool m_running = false;
    /// A worker is still out, maybe one given up on: the next run waits for
    /// it, so the backend is never asked for two passes at once from here.
    bool m_inFlight = false;
    bool m_rerunQueued = false;
    /// The out worker's cancel flag (shared: it outlives this dialog).
    std::shared_ptr<std::atomic_bool> m_abort;
    QElapsedTimer m_elapsed;
    QPointer<QTimer> m_tick;
    QPointer<QComboBox> m_field;
    QPointer<QComboBox> m_weight;
    QPointer<QSpinBox> m_bins;
    QPointer<QLabel> m_population;
    QPointer<QLabel> m_summary;
    QPointer<QProgressBar> m_progress;
    QPointer<QPushButton> m_run;
    QPointer<QPushButton> m_cancel;
    QPointer<QPushButton> m_saveCsv;
    QPointer<QPushButton> m_savePng;
    QPointer<QCustomPlot> m_plot;
};

#endif
