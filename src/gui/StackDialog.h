#pragma once

#include <QCloseEvent>
#include <QDialog>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <memory>
#include <vector>

class BackendClient;
class QDialogButtonBox;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressDialog;
class QPushButton;

/**
 * S-04 — Spectral stacking dialog.
 *
 * Shows a checkable list of session datasets, lets the user select ≥ 2 cubes,
 * and calls BackendClient::fetchStackBinary() async. The resulting 1-D
 * spectrum is displayed in a dedicated QCustomPlot window.
 */
class StackDialog : public QDialog
{
    Q_OBJECT

public:
    /// Minimal description of a session dataset for the selection list. The
    /// shape fields are used to disable cubes incompatible with the current
    /// one (different spatial/spectral grid → cannot be stacked together).
    struct DatasetEntry
    {
        QString datasetId;
        QString kind;     ///< "cube" | "image" | "" (unknown)
        QString label;    ///< human-readable name (falls back to datasetId)
        int width{ 0 };
        int height{ 0 };
        int depth{ 0 };
        QString path;     ///< backend file path; identity for de-duplication
    };

    /**
     * Legacy overload (kept for back-compat): receives a flat list of dataset
     * IDs and assumes they are all stackable. Prefer the @ref DatasetEntry
     * overload so the dialog can filter incompatible cubes.
     */
    StackDialog(std::shared_ptr<BackendClient> client,
                const QString &sessionId,
                const QStringList &sessionDatasets,
                QWidget *parent = nullptr);

    /**
     * @param client      Shared BackendClient — the async stack run captures a
     *                    copy of the shared_ptr, so it survives an early dialog
     *                    close (no use-after-free).
     * @param sessionId   Backend session identifier — set on client before requests.
     * @param datasets    Datasets currently open in the session (with metadata).
     * @param currentId   Dataset to pre-check on open (empty = none).
     * @param parent      Optional parent widget.
     */
    StackDialog(std::shared_ptr<BackendClient> client,
                const QString &sessionId,
                const std::vector<DatasetEntry> &datasets,
                const QString &currentId,
                QWidget *parent = nullptr);

Q_SIGNALS:
    /// Emitted when the stacked spectrum is computed, so the caller promotes it to a first-class
    /// SPEC pane product (#5) instead of a standalone QCustomPlot window. Payload = the stacked
    /// channel values, a descriptive label (method, N cubes), and a provenance map (method,
    /// weighting, source cube ids, channel count) so the product carries a real recipe.
    void spectrumReady(const QVector<double> &values, const QString &label,
                       const QVariantMap &meta);

private slots:
    void onItemChanged();
    void onCompute();

protected:
    /// Ask before abandoning a running computation (ToolDialogClose.h).
    void closeEvent(QCloseEvent *event) override;

private:
    void setUiBusy(bool busy);
    /// True while a compute is in flight. Set in setUiBusy, the single place
    /// that already knew — closeEvent needs it to warn instead of discarding.
    bool m_busy{ false };
    void updateSelectionLabel();

    std::shared_ptr<BackendClient> m_client;
    QString                  m_sessionId;

    QListWidget             *m_datasetList{ nullptr };
    QLabel                  *m_selectionLabel{ nullptr };
    QComboBox               *m_methodCombo{ nullptr };
    QComboBox               *m_weightByCombo{ nullptr };
    QPushButton             *m_stackBtn{ nullptr };
    QPointer<QProgressDialog> m_progress;
};
