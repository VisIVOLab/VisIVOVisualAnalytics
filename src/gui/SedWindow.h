#ifndef SedWindow_h
#define SedWindow_h

// SedWindow.h
// The SED of one compact source: its photometry, greybody fits (optically thin
// and thick) and fits against the VLKB grid of theoretical models.
//
// The window owns presentation only. The photometry is assembled by
// visivo::SedBuilder from catalogue rows, and every fit runs on the backend
// (POST /v1/sed/fit, /v1/sed/models) off the GUI thread, so a slow or remote
// backend cannot freeze the interface and a result can be reproduced from the
// API alone.

#include "BackendClient.h"
#include "SedBuilder.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QCustomPlot;
class QCPGraph;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTextBrowser;

class SedWindow : public QWidget
{
    Q_OBJECT

public:
    /// @param sed           the photometry, already built from the catalogue
    /// @param backendUrl     base URL of the backend that runs the fits
    /// @param backendToken   its auth token
    SedWindow(const visivo::SedBuildResult &sed, const QString &backendUrl,
              const QString &backendToken, QWidget *parent = nullptr);
    ~SedWindow() override;

signals:
    /// A fit finished. The viewer that opened this window turns it into a
    /// Session Data product: it owns the dataset key, the window scope and the
    /// backend session this SED belongs to, and this window does not.
    void fitProduced(const QString &label, const QVariantMap &params);

private slots:
    void runGreybodyFit();
    /// The user picked points on the plot: that selection *is* the set of bands
    /// in the fit, so it is written straight back into the photometry table.
    void onPlotSelectionChanged();
    /// Name the band under the cursor, and point the table at it.
    void showPointTooltip(QMouseEvent *event);
    /// Sum the branches at each wavelength into one point, and back again.
    void setBranchesCollapsed(bool collapsed);
    void runModelFit();
    void onGreybodyFitFinished();
    void onModelFitFinished();
    void exportSed();
    void onBandTableChanged(int row, int column);
    void onModelSelected();
    void clearFits();

private:
    void buildUi();
    void refreshBandTable();
    void buildParameterPanel(QWidget *page);
    void applyPlotTheme();
    void replot();
    void plotObservedPoints();
    /// Push the current @c use flags into the plot's selection, without
    /// letting that round-trip back through onPlotSelectionChanged().
    void syncPlotSelection();
    /// Write the displayed rows' Use/UL flags back onto the branches they came
    /// from, so collapsing and expanding never rolls an edit back.
    void propagateEditsToRawPoints();
    /// Recompute @ref m_rowBranches for a one-row-per-branch (expanded) list.
    void resetRowBranches();
    void plotFit(const BackendSedFit &fit, const QColor &colour);
    void plotModel(const BackendSedModel &model, const QColor &colour);
    void showFitResults();
    void markResultsStale();
    void updateIdleStatus();
    void setBusy(bool busy, const QString &message = QString());
    void announceProduct(const BackendSedFit &fit);
    /// Points currently ticked in the band table, as the backend wants them.
    QVector<BackendSedPoint> selectedPoints() const;
    /// Rows of m_sed.points that selectedPoints() would send, in the same order.
    QVector<int> selectedRows() const;
    /// The pristine-list branches behind each selected row, in the same order.
    QVector<QVector<int>> selectedBranches() const;
    /// Which entry of the recorded request a table row corresponds to, matched
    /// on wavelength (and flux, for a band with branches). -1 when the row was
    /// not part of the fit, or when branches make the pairing ambiguous.
    int provenanceIndexFor(int row) const;
    /// Ranges typed in the parameter panel; an empty vector means "not set", so
    /// the backend's own default applies rather than a second copy of it here.
    QVector<double> rangeFrom(QDoubleSpinBox *minBox, QDoubleSpinBox *maxBox,
                              QSpinBox *stepBox) const;
    void applyDefaults(const BackendSedDefaults &defaults);

    visivo::SedBuildResult m_sed;
    /// The SED as built, before any collapsing. Collapsing is reversible, so
    /// the branches have to survive it.
    std::vector<visivo::SedPoint> m_rawPoints;
    QString m_backendUrl;
    QString m_backendToken;
    QString m_modelsServiceUrl;

    QVector<BackendSedFit> m_fits;         ///< greybody fits currently plotted
    QVector<BackendSedModel> m_models;     ///< theoretical models, best first

    /// Exactly what produced @ref m_fits. Export and Session Data must describe
    /// the fit that ran, not whatever the spin boxes and checkboxes say now: the
    /// user is free to change them afterwards, and reading the widgets then
    /// wrote a provenance that never happened and mis-aligned the model fluxes
    /// with the bands.
    struct FitProvenance
    {
        QVector<int> sentRows;              ///< index into m_sed.points, in the order sent
        /// For each sent point, the branches of the pristine list behind it —
        /// one entry when the branches were expanded, the whole group when they
        /// were collapsed. Row numbers cannot do this job: collapsing renumbers
        /// the rows, so a row index recorded before a collapse names a
        /// different band afterwards.
        QVector<QVector<int>> sentBranches;
        QVector<BackendSedPoint> points;    ///< exactly the photometry that was sent
        double distancePc{ 0. };
        double kappaRef{ 0. };
        double lambdaRefUm{ 0. };
        double sizeArcsec{ 0. };
        bool colourCorrection{ true };
        QString model;
        /// Value of @ref m_inputRevision when the request was sent. If the
        /// inputs moved on while the fit was running, the answer describes a
        /// SED that is no longer on screen.
        quint64 revision{ 0 };
    };
    FitProvenance m_pending;     ///< the request in flight
    FitProvenance m_provenance;  ///< the request that produced m_fits
    /// The inputs changed after the fit ran: results on screen are no longer of
    /// this SED. Said out loud instead of silently redrawing stale curves.
    bool m_resultsStale{ false };
    /// The user typed in the parameter panel before the backend's defaults
    /// arrived; the late answer must not overwrite their numbers.
    bool m_parametersTouched{ false };
    /// Bumped by every change to the photometry or the fit parameters.
    quint64 m_inputRevision{ 0 };

    QFutureWatcher<BackendSedFitResult> m_fitWatcher;
    QFutureWatcher<BackendSedModelsResult> m_modelWatcher;

    QPointer<QCustomPlot> m_plot;
    QPointer<QTableWidget> m_bandTable;
    QPointer<QTableWidget> m_resultTable;
    QPointer<QTableWidget> m_modelTable;
    QPointer<QTextBrowser> m_log;
    QPointer<QTabWidget> m_tabs;
    QPointer<QLabel> m_statusLabel;

    QPointer<QComboBox> m_modelCombo;
    QPointer<QDoubleSpinBox> m_distance;
    QPointer<QDoubleSpinBox> m_kappaRef;
    QPointer<QDoubleSpinBox> m_lambdaRef;
    QPointer<QDoubleSpinBox> m_sizeArcsec;
    QPointer<QCheckBox> m_colourCorrection;
    QPointer<QCheckBox> m_logAxes;
    QPointer<QCheckBox> m_selectOnPlot;
    QPointer<QCheckBox> m_collapseBranches;

    QPointer<QDoubleSpinBox> m_massMin, m_massMax;
    QPointer<QSpinBox> m_massSteps;
    QPointer<QDoubleSpinBox> m_tempMin, m_tempMax;
    QPointer<QSpinBox> m_tempSteps;
    QPointer<QDoubleSpinBox> m_betaMin, m_betaMax;
    QPointer<QSpinBox> m_betaSteps;
    QPointer<QDoubleSpinBox> m_lambda0Min, m_lambda0Max;
    QPointer<QSpinBox> m_lambda0Steps;
    QPointer<QDoubleSpinBox> m_scaleMin, m_scaleMax;
    QPointer<QSpinBox> m_scaleSteps;

    QPointer<QDoubleSpinBox> m_weightMidIr, m_weightFarIr, m_weightSubmm;
    QPointer<QDoubleSpinBox> m_prefilter, m_deltaChi2;

    QPointer<QPushButton> m_fitButton;
    QPointer<QPushButton> m_modelButton;

    bool m_updatingTable{ false };
    bool m_syncingSelection{ false };
    bool m_branchesCollapsed{ false };
    /// For each displayed row, which entries of @ref m_rawPoints it stands for.
    std::vector<std::vector<int>> m_rowBranches;
    /// The displayed rows as the view built them. A row still equal to its
    /// baseline has not been edited, and its branches must keep their own
    /// flags rather than inherit the aggregate's.
    std::vector<visivo::SedPoint> m_displayedBaseline;

    /// The two photometry graphs and, per graph, which row of m_sed.points each
    /// of their data points came from — the map the plot selection is written
    /// back through.
    QCPGraph *m_dataGraph{ nullptr };
    QCPGraph *m_ulGraph{ nullptr };
    std::vector<int> m_dataRows;
    std::vector<int> m_ulRows;
};

#endif   // SedWindow_h
