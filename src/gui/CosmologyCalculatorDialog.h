#ifndef COSMOLOGYCALCULATORDIALOG_H
#define COSMOLOGYCALCULATORDIALOG_H
// CosmologyCalculatorDialog.h
// Tools ▸ Cosmology Calculator (issue #29): distances and times for one or many
// redshifts, under a preset or a custom ΛCDM, all computed by the backend.
//
// Which route answers depends on the model, because the backend has two:
//   * a preset → POST /v1/cosmology/distance (D_C, D_L, kpc/″); Planck18 also
//     → GET /v1/cosmology/distances (D_A, lookback time, age), which computes
//     the server's Planck18 when given no parameters;
//   * a custom model → GET /v1/cosmology/distances with H0, Om0, Ode0 (D_C,
//     D_L, D_A, lookback time, age); kpc/″ is that D_A in other units.
// Planck15, Planck13 and WMAP9 therefore leave D_A, lookback time and age
// empty: no route computes them for those presets, and the client does not
// fill the gap with its own integral.
//
// The custom model the user hands to the 3-D catalogue viewer is stored in
// QSettings (CosmologyModel.h) and announced through CosmologyChoiceHub, so
// every catalogue window using "Custom" re-places its sources.

#include "CosmologyModel.h"

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>

#include <atomic>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QWidget;

/// Process-wide notice that the stored custom cosmology changed.
class CosmologyChoiceHub : public QObject
{
    Q_OBJECT
public:
    static CosmologyChoiceHub &instance();
    /// Store @p model as the custom cosmology and tell every listener.
    void setCustomModel(const visivo::cosmology::Model &model);
    visivo::cosmology::Model customModel() const;

signals:
    void customModelChanged();

private:
    CosmologyChoiceHub() = default;
};

class CosmologyCalculatorDialog : public QDialog
{
    Q_OBJECT
public:
    /// Raise the calculator, creating it on first use. One per application:
    /// a second Tools entry brings the same window back with what it holds.
    /// @p initial, when given, selects that model (the catalogue viewer opens
    /// it on the model its sources are placed with).
    static CosmologyCalculatorDialog *showCalculator(QWidget *parent, const QString &backendUrl,
                                                     const QString &backendToken,
                                                     const visivo::cosmology::Model *initial = nullptr);

    /// The most redshifts one run asks for. The custom-model route answers one
    /// redshift per request; a catalogue goes through the viewer, not here.
    static constexpr int kMaxRedshifts = 1000;

    ~CosmologyCalculatorDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    CosmologyCalculatorDialog(const QString &backendUrl, const QString &backendToken,
                              QWidget *parent);
    void setBackend(const QString &backendUrl, const QString &backendToken);
    void selectModel(const visivo::cosmology::Model &model);
    visivo::cosmology::Model currentModel() const;
    void refreshModelState();
    void compute();
    void showRows(const visivo::cosmology::Model &model,
                  const QList<visivo::cosmology::Row> &rows);
    void copyTable();
    void exportCsv();
    void useForCatalogues();

    QString m_backendUrl;
    QString m_backendToken;

    QPointer<QComboBox> m_modelCombo;
    QPointer<QWidget> m_customBox;
    QPointer<QDoubleSpinBox> m_h0;
    QPointer<QDoubleSpinBox> m_om0;
    QPointer<QDoubleSpinBox> m_ode0;
    QPointer<QCheckBox> m_flat;
    QPointer<QLabel> m_modelLine;
    QPointer<QLabel> m_routeNote;
    QPointer<QPlainTextEdit> m_redshifts;
    QPointer<QPushButton> m_computeButton;
    QPointer<QPushButton> m_useButton;
    QPointer<QPushButton> m_copyButton;
    QPointer<QPushButton> m_exportButton;
    QPointer<QLabel> m_status;
    QPointer<QTableWidget> m_table;

    /// What the table shows, so Copy and Export write the run on screen and
    /// not whatever the form says now.
    visivo::cosmology::Model m_shownModel;
    QList<visivo::cosmology::Row> m_shownRows;

    /// Set when the window goes away: the worker stops asking for the rest.
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_runId{ 0 };
    bool m_running{ false };
};

#endif   // COSMOLOGYCALCULATORDIALOG_H
