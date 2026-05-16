#pragma once

#include <QDialog>
#include <QPointer>

class BackendClient;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QProgressDialog;
class QPushButton;
class QSpinBox;
class QTableWidget;

/**
 * S-05 — SoFiA-2 source-finding dialog.
 *
 * Presents basic SoFiA parameters and calls BackendClient::runSourceFind()
 * asynchronously.  Results are shown in an embedded QTableWidget.
 * 503 responses (sofia2 not on PATH) are reported as a user-friendly error.
 */
class SourceFindDialog : public QDialog
{
    Q_OBJECT

public:
    SourceFindDialog(BackendClient *client,
                     const QString &datasetId,
                     QWidget *parent = nullptr);

private slots:
    void onRun();

private:
    void setUiBusy(bool busy);
    void populateTable(const QVector<QVariantMap> &rows);

    BackendClient           *m_client{ nullptr };
    QString                  m_datasetId;

    // ── Parameters ─────────────────────────────────────────────────────────
    QDoubleSpinBox          *m_thresholdSpin{ nullptr };  ///< S+C threshold (σ)
    QSpinBox                *m_kernelXSpin{ nullptr };
    QSpinBox                *m_kernelYSpin{ nullptr };
    QSpinBox                *m_kernelZSpin{ nullptr };
    QComboBox               *m_mergeCombo{ nullptr };

    // ── Results ─────────────────────────────────────────────────────────────
    QTableWidget            *m_table{ nullptr };

    // ── Buttons ─────────────────────────────────────────────────────────────
    QPushButton             *m_runBtn{ nullptr };
    QPointer<QProgressDialog> m_progress;
};
