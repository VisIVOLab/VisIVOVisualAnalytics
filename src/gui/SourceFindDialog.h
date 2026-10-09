#pragma once

#include <QCloseEvent>
#include <QDialog>
#include <QPointer>

#include <memory>

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
    /// The client is shared: the async SoFiA run captures a copy of the
    /// shared_ptr, so the client outlives an early dialog close (no use-after-
    /// free) and the dialog needn't block on the run to be destroyed.
    SourceFindDialog(std::shared_ptr<BackendClient> client,
                     const QString &datasetId,
                     QWidget *parent = nullptr);

signals:
    /// Emitted when the user asks to open the SoFiA-2 binary mask cube. The
    /// path is on the backend filesystem; MainWindow opens it in the 3D cube
    /// viewer (mask → 3D isosurface inspection of the detections).
    void openMaskCubeRequested(const QString &maskPath);

private slots:
    void onRun();

protected:
    /// Ask before abandoning a running computation (ToolDialogClose.h).
    void closeEvent(QCloseEvent *event) override;

private:
    void setUiBusy(bool busy);
    /// True while a compute is in flight. Set in setUiBusy, the single place
    /// that already knew — closeEvent needs it to warn instead of discarding.
    bool m_busy{ false };
    void populateTable(const QVector<QVariantMap> &rows);

    std::shared_ptr<BackendClient> m_client;
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
    QPushButton             *m_openMaskBtn{ nullptr };   ///< "Open mask in 3D"
    QString                  m_maskPath;                 ///< backend mask-cube path
    QPointer<QProgressDialog> m_progress;
};
