#pragma once

#include "app/LteCoRequest.h"

#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QVariantMap>
#include <QVector>

#include <optional>

class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressDialog;
class QPushButton;
class QTabWidget;
class QTableWidget;

/**
 * The LTE analysis of CO isotopologues (issue #416): ¹³CO and C¹⁸O, optionally
 * ¹²CO, as cubes or integrated maps open in the session, sent to
 * POST /v1/products/lte_co. The answer comes back as a table per region, the
 * assumptions it rests on, and FITS maps the host opens in a viewer.
 *
 * Opened from a cube or a map window, which hands over its backend, its
 * dataset, its regions and the spectral range it is showing. Non-modal; the
 * request runs on a pooled thread with its own BackendClient, and a result
 * that arrives after the dialog is gone is dropped.
 */
class LteCoDialog : public QDialog
{
    Q_OBJECT

public:
    struct RegionChoice
    {
        QString name;
        QJsonObject canonical;   ///< 0-based pixels of the host's dataset
    };

    struct Context
    {
        QString backendUrl;
        QString backendToken;
        QString sessionId;
        QString hostDatasetId;
        double hostRestFreqHz{ 0.0 };
        std::optional<double> vMinKms;
        std::optional<double> vMaxKms;
        QString windowSource;   ///< what the window was taken from, said under it
        QVector<RegionChoice> regions;
    };

    explicit LteCoDialog(const Context &context, QWidget *parent = nullptr);
    /// The host's regions and spectral range as they are now, for a dialog
    /// raised again: a region drawn since is offered, a deleted one is gone. A
    /// window the user typed is kept. Ignored while a run is in flight.
    void refreshHostContext(const Context &context);

signals:
    /// Open one of the maps the run wrote, in a viewer of its own.
    void openMapRequested(const QString &path, const QString &label);
    /// A run finished: the host records it in its Session Data.
    void runRecorded(const QString &label, const QVariantMap &params);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct DatasetChoice
    {
        QString id;
        QString kind;
        QString path;
        QString label;
    };

    void listDatasets();
    void fillDatasetCombos(const QVector<DatasetChoice> &datasets);
    void refreshRegionsEnabled();
    void run();
    void showResult(const visivo::lte::Response &response, const visivo::lte::Inputs &inputs);
    void showFailure(const QString &error);
    void markEtaRequired(const QStringList &species);
    void saveCsv();
    void openSelectedMap();
    void setBusy(bool busy);
    DatasetChoice chosen(QComboBox *combo) const;
    QStringList provenanceLines() const;

    Context m_ctx;
    QVector<DatasetChoice> m_datasets;

    QTabWidget *m_tabs{ nullptr };
    QComboBox *m_transition{ nullptr };
    QComboBox *m_ds13{ nullptr };
    QComboBox *m_ds18{ nullptr };
    QComboBox *m_ds12{ nullptr };
    QLabel *m_datasetNote{ nullptr };
    QLineEdit *m_vMin{ nullptr };
    QLineEdit *m_vMax{ nullptr };
    QLabel *m_windowNote{ nullptr };
    QLineEdit *m_eta13{ nullptr };
    QLineEdit *m_eta18{ nullptr };
    QLineEdit *m_eta12{ nullptr };
    QLabel *m_etaNote{ nullptr };
    QLineEdit *m_distance{ nullptr };
    QLineEdit *m_rGc{ nullptr };
    QLineEdit *m_xRatio{ nullptr };
    QComboBox *m_gradient{ nullptr };
    QComboBox *m_h2From{ nullptr };
    QLineEdit *m_x12co{ nullptr };
    QLineEdit *m_tEx{ nullptr };
    QDoubleSpinBox *m_snr{ nullptr };
    QListWidget *m_regionList{ nullptr };
    QLabel *m_regionNote{ nullptr };
    QPushButton *m_runBtn{ nullptr };
    QPushButton *m_csvBtn{ nullptr };
    QPushButton *m_openMapBtn{ nullptr };

    QTableWidget *m_table{ nullptr };
    QPlainTextEdit *m_assumptions{ nullptr };
    QListWidget *m_mapList{ nullptr };
    QLabel *m_resultNote{ nullptr };

    bool m_busy{ false };
    int m_requestId{ 0 };
    QPointer<QProgressDialog> m_progressSheet;

    visivo::lte::Response m_last;
    visivo::lte::Inputs m_lastInputs;
    QStringList m_lastRegionNames;
    QStringList m_lastProvenance;
};
