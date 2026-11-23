#ifndef SIMULATIONDIAGNOSTICSDIALOG_H
#define SIMULATIONDIAGNOSTICSDIALOG_H

#include "app/BackendClient.h"
#include "app/ParticleProfiles.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace visivo {
struct BackendTaskControl;
}

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTimer;
class QCustomPlot;
class QCPMarginGroup;

/// Exact diagnostics of the particles a catalogue window has selected, over
/// the complete population on the backend: a histogram, a radial profile and
/// a phase diagram (issues #448, #426). One run at a time, as a backend task
/// with progress and Cancel.
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

    /// Where a profile can be centred besides the shrinking sphere: the
    /// position of the particle selected in the window, and the SubFind
    /// target selected in another window of the same session. Asked each
    /// time they are needed, so they follow the selections.
    using PointSource = std::function<std::optional<std::array<double, 3>>()>;
    using TargetSource = std::function<std::optional<visivo::profiles::CentreChoice>()>;
    void setCentreSources(PointSource pickedPoint, TargetSource subfindTarget);

private:
    enum class Kind { Histogram, Profile, Phase };

    struct Outcome
    {
        Kind kind{ Kind::Histogram };
        BackendCatalogueDiagnosticsResponse histogram;
        visivo::profiles::ProfileResult profile;
        visivo::profiles::PhaseResult phase;
        visivo::profiles::PhaseRequest phaseAsked;
    };

    /// A run, validated and with everything it needs, as asked when the
    /// user asked: it can wait for the previous worker unchanged.
    struct Job
    {
        Kind kind{ Kind::Histogram };
        std::vector<BackendTabularFilter> filters;
        BackendCatalogueDiagnosticsRequest histogram;
        visivo::profiles::ProfileRequest profile;
        visivo::profiles::PhaseRequest phase;
    };
    std::optional<Job> buildJob(Kind kind);
    void start(const Job &job);
    void changeEvent(QEvent *event) override;

    QWidget *buildHistogramTab(const QStringList &numericFields);
    QWidget *buildProfileTab(const QStringList &numericFields);
    QWidget *buildPhaseTab(const QStringList &numericFields);
    Kind currentKind() const;
    void run();
    void runKind(Kind kind);
    void cancel();
    void finishRun();
    void clearResult(Kind kind);
    void clearAllResults();
    void updatePopulation();
    void updateSaveButtons();
    void updateCentreControls();
    std::optional<visivo::profiles::CentreChoice> chosenCentre(QString *why);
    void present(const Outcome &outcome);
    void presentHistogram(const BackendCatalogueDiagnosticsResponse &result);
    void presentProfile(const visivo::profiles::ProfileResult &result);
    void presentPhase(const visivo::profiles::PhaseResult &result);
    void plotProfile();
    void saveCsv();
    void savePng();
    QLabel *summaryOf(Kind kind) const;
    QCustomPlot *plotOf(Kind kind) const;

    QString m_backendUrl;
    QString m_backendToken;
    QString m_sessionId;
    QString m_datasetId;
    std::vector<BackendTabularFilter> m_filters;
    PointSource m_pickedPoint;
    TargetSource m_subfindTarget;

    BackendCatalogueDiagnosticsResponse m_histogram;
    visivo::profiles::ProfileResult m_profile;
    visivo::profiles::PhaseResult m_phase;
    bool m_hasHistogram = false;
    bool m_hasProfile = false;
    bool m_hasPhase = false;
    /// What the phase diagram on show was asked for: its axes, scales and
    /// weight, which the controls may no longer say.
    visivo::profiles::PhaseRequest m_phaseAsked;
    QPointer<QCPMarginGroup> m_phaseMargins;

    /// Bumped by every run and by Cancel: an answer for an older one is
    /// dropped.
    quint64 m_runId = 0;
    /// A run is shown as under way (the bar, Cancel), and of which kind.
    bool m_running = false;
    Kind m_runningKind = Kind::Histogram;
    /// A worker is still out, maybe one given up on: the next run waits for
    /// it, so the backend is never asked for two passes at once from here.
    bool m_inFlight = false;
    bool m_rerunQueued = false;
    Job m_queuedJob;
    /// The out worker's task: its progress, and the abort that cancels it on
    /// the backend (shared: it outlives this dialog).
    std::shared_ptr<visivo::BackendTaskControl> m_control;
    void showProgress();
    QElapsedTimer m_elapsed;
    QPointer<QTimer> m_tick;

    QPointer<QTabWidget> m_tabs;
    QPointer<QLabel> m_population;
    QPointer<QProgressBar> m_progress;
    QPointer<QPushButton> m_run;
    QPointer<QPushButton> m_cancel;
    QPointer<QPushButton> m_saveCsv;
    QPointer<QPushButton> m_savePng;

    // Histogram
    QPointer<QComboBox> m_field;
    QPointer<QComboBox> m_weight;
    QPointer<QSpinBox> m_bins;
    QPointer<QLabel> m_summary;
    QPointer<QCustomPlot> m_plot;

    // Radial profile
    QPointer<QComboBox> m_centreMode;
    QPointer<QDoubleSpinBox> m_pointX, m_pointY, m_pointZ;
    QPointer<QPushButton> m_usePicked;
    QPointer<QLabel> m_targetLabel;
    QPointer<QDoubleSpinBox> m_rMin, m_rMax;
    QPointer<QSpinBox> m_profileBins;
    QPointer<QComboBox> m_profileAxis;
    QPointer<QListWidget> m_profileColumns;
    QPointer<QComboBox> m_profileShow;
    QPointer<QLabel> m_profileSummary;
    QPointer<QCustomPlot> m_profilePlot;

    // Phase diagram
    QPointer<QComboBox> m_phaseX, m_phaseY, m_phaseWeight;
    QPointer<QCheckBox> m_phaseXLog, m_phaseYLog;
    QPointer<QSpinBox> m_phaseXBins, m_phaseYBins;
    QPointer<QLabel> m_phaseSummary;
    QPointer<QCustomPlot> m_phasePlot;
};

#endif
