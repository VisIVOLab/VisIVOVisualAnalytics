#pragma once

#include <QDialog>
#include <QPointer>
#include <QStringList>

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
 * Shows a checkable list of session datasets (FIX 6 redesign), lets the user
 * select ≥ 2 cubes, and calls BackendClient::fetchStackBinary() async.
 * The resulting 1-D spectrum is displayed in a dedicated QCustomPlot window.
 */
class StackDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @param client          BackendClient instance (not owned).
     * @param sessionId       Backend session identifier — set on client before requests.
     * @param sessionDatasets Dataset IDs currently open in the session.
     * @param parent          Optional parent widget.
     */
    StackDialog(BackendClient *client,
                const QString &sessionId,
                const QStringList &sessionDatasets,
                QWidget *parent = nullptr);

private slots:
    void onItemChanged();
    void onCompute();

private:
    void setUiBusy(bool busy);
    void updateSelectionLabel();

    BackendClient           *m_client{ nullptr };
    QString                  m_sessionId;

    QListWidget             *m_datasetList{ nullptr };
    QLabel                  *m_selectionLabel{ nullptr };
    QComboBox               *m_methodCombo{ nullptr };
    QComboBox               *m_weightByCombo{ nullptr };
    QPushButton             *m_stackBtn{ nullptr };
    QPointer<QProgressDialog> m_progress;
};
