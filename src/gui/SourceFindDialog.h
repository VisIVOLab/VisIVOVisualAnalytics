#pragma once

#include "BackendClient.h"
#include "FinderTaskRun.h"
#include "ProductRegistry.h"
#include "SofiaSuggest.h"

#include <QCloseEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>

#include <atomic>
#include <memory>

class QComboBox;
class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTableWidgetItem;
class QTimer;

/**
 * SoFiA-2 source finding on the cube a viewer shows (Tools ▸ Source Finding).
 *
 * On opening it asks the backend to suggest parameters from the cube
 * (POST /v1/sofia2/suggest, issue #64) and shows each with its value, the
 * reason for it and SoFiA's default; the user edits what they like and runs.
 * The run is a task of the backend (POST /v1/tasks/sourcefind), the validated
 * route the SKAVA dialog uses too: it can be cancelled, and SoFiA's own counts
 * come back with the catalogue. Each run is announced through runRecorded so
 * the cube viewer records it as a product, with the parameters that ran.
 */
class SourceFindDialog : public QDialog
{
    Q_OBJECT

public:
    /// The client is shared: the async work captures a copy of the
    /// shared_ptr, so the client outlives an early dialog close (no use-after-
    /// free) and the dialog needn't block on the run to be destroyed.
    SourceFindDialog(std::shared_ptr<BackendClient> client,
                     const QString &datasetId,
                     QWidget *parent = nullptr);
    ~SourceFindDialog() override;

signals:
    /// Emitted when the user asks to open the SoFiA-2 binary mask cube. The
    /// path is on the backend filesystem; MainWindow opens it in the 3D cube
    /// viewer (mask → 3D isosurface inspection of the detections).
    void openMaskCubeRequested(const QString &maskPath);
    /// A run finished: what to record as the cube's product (label, the
    /// Provenance lines, the replay step).
    void runRecorded(const QString &label, const QVariantMap &params,
                     const QList<ReplayStep> &replay);

public slots:
    /// Escape: handled as a close, so a running job is not abandoned silently.
    void reject() override;

protected:
    /// Ask before abandoning a running SoFiA-2 task; closing cancels it.
    void closeEvent(QCloseEvent *event) override;

private:
    /// Ask the backend for a suggestion with the user's answers to its
    /// questions (empty = let it deduce).
    void requestSuggestion();
    /// A SoFiA-2 parameter the suggestion does not set (e.g. dilation.*).
    void onAddParameter();
    void applySuggestion(const BackendSofiaSuggestResult &answer);
    void showChoice(QComboBox *combo, QLabel *note, QPushButton *confirm,
                    const visivo::SofiaChoice &choice, const QString &what);
    void fillTable();
    void onCellChanged(QTableWidgetItem *item);
    void onRun();
    void onCancel();
    void showRunOutcome(const visivo::FinderTaskOutcome &outcome, const QJsonObject &params,
                        const QVariantMap &provenance, int edited);
    void populateSources(const QJsonArray &sources);
    /// Enable what the state allows: a run needs no suggestion in flight, no
    /// run in flight, no unanswered question and no refusal of the data.
    void updateControls();

    std::shared_ptr<BackendClient> m_client;
    QString                  m_datasetId;

    // ── Suggestion ──────────────────────────────────────────────────────────
    QComboBox               *m_dataTypeCombo{ nullptr };
    QLabel                  *m_dataTypeNote{ nullptr };
    QPushButton             *m_dataTypeConfirm{ nullptr };
    QComboBox               *m_lineCombo{ nullptr };
    QLabel                  *m_lineNote{ nullptr };
    QPushButton             *m_lineConfirm{ nullptr };
    QLabel                  *m_statusLabel{ nullptr };    ///< measurements, or why there are none
    QLabel                  *m_warningsLabel{ nullptr };
    QTableWidget            *m_paramTable{ nullptr };
    QPushButton             *m_resetBtn{ nullptr };
    QPushButton             *m_addBtn{ nullptr };
    QPushButton             *m_suggestBtn{ nullptr };

    visivo::SofiaParamSet    m_params;
    visivo::SofiaSuggestion  m_suggestion;
    /// The user's answers to the suggester's questions, as asked and as last
    /// answered.
    visivo::SofiaAskState    m_ask;
    bool                     m_suggesting{ false };
    /// A newer request supersedes an answer still on its way.
    int                      m_suggestSerial{ 0 };
    /// The backend refused the data (422): no run is offered.
    bool                     m_dataRefused{ false };
    /// Why the latest request brought no suggestion (old backend, no answer,
    /// busy); empty after a good one. The run is still offered, with SoFiA's
    /// defaults for whatever the table does not set.
    QString                  m_suggestError;
    bool                     m_fillingTable{ false };
    void updateRow(int row);

    // ── Run ─────────────────────────────────────────────────────────────────
    QProgressBar            *m_progressBar{ nullptr };
    QLabel                  *m_runLabel{ nullptr };
    /// The run's outcome: counts and summary, or SoFiA's ERROR lines in full
    /// (they reach 1200 characters), wrapped and scrollable.
    QPlainTextEdit          *m_summaryLabel{ nullptr };
    QTableWidget            *m_table{ nullptr };
    QSplitter               *m_splitter{ nullptr };  ///< parameters above, result below
    QTimer                  *m_elapsedTimer{ nullptr };
    QElapsedTimer            m_runClock;
    bool                     m_busy{ false };
    /// Shared with the worker: set by Cancel or by closing, read by the poll.
    std::shared_ptr<std::atomic_bool> m_cancel;

    // ── Buttons ─────────────────────────────────────────────────────────────
    QPushButton             *m_runBtn{ nullptr };
    QPushButton             *m_cancelBtn{ nullptr };
    QPushButton             *m_openMaskBtn{ nullptr };   ///< "Open mask in 3D"
    QString                  m_maskPath;                 ///< backend mask-cube path
};
