#ifndef SIMULATIONDIAGNOSTICSDIALOG_H
#define SIMULATIONDIAGNOSTICSDIALOG_H

#include "app/BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QCustomPlot;

class SimulationDiagnosticsDialog : public QDialog
{
    Q_OBJECT
public:
    SimulationDiagnosticsDialog(const QString &backendUrl, const QString &backendToken,
                                const QString &sessionId, const QString &datasetId,
                                const QStringList &numericFields,
                                const QJsonObject &units,
                                const std::vector<BackendTabularFilter> &filters,
                                QWidget *parent = nullptr);

private:
    void run();
    void present(const BackendCatalogueDiagnosticsResponse &result);
    void saveCsv();

    QString m_backendUrl;
    QString m_backendToken;
    QString m_sessionId;
    QString m_datasetId;
    QJsonObject m_units;
    std::vector<BackendTabularFilter> m_filters;
    BackendCatalogueDiagnosticsResponse m_result;
    QPointer<QComboBox> m_field;
    QPointer<QComboBox> m_weight;
    QPointer<QSpinBox> m_bins;
    QPointer<QLabel> m_summary;
    QPointer<QPushButton> m_run;
    QPointer<QPushButton> m_saveCsv;
    QPointer<QPushButton> m_savePng;
    QPointer<QCustomPlot> m_plot;
    QFutureWatcher<BackendCatalogueDiagnosticsResponse> m_watcher;
};

#endif
