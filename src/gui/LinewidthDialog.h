#pragma once

#include <QDialog>
#include <QPointer>

class BackendClient;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QProgressDialog;
class QSpinBox;

/**
 * S-02 — Line-width map dialog.
 *
 * Calls BackendClient::fetchLinewidthBinary() asynchronously and opens a
 * ScienceMapWindow for the requested map type (FWHM or Equivalent Width).
 */
class LinewidthDialog : public QDialog
{
    Q_OBJECT

public:
    LinewidthDialog(BackendClient *client,
                    const QString &sessionId,
                    const QString &datasetId,
                    int depth,
                    double initialThreshold,
                    QWidget *parent = nullptr);

private slots:
    void onCompute();

private:
    void setUiBusy(bool busy);

    BackendClient           *m_client{ nullptr };
    QString                  m_sessionId;
    QString                  m_datasetId;
    int                      m_depth{ 1 };

    QComboBox               *m_methodCombo{ nullptr };
    QDoubleSpinBox          *m_restFreqSpin{ nullptr };
    QSpinBox                *m_chanStartSpin{ nullptr };
    QSpinBox                *m_chanEndSpin{ nullptr };
    QCheckBox               *m_maskCheck{ nullptr };
    QDoubleSpinBox          *m_thresholdSpin{ nullptr };
    QDialogButtonBox        *m_buttons{ nullptr };
    QPointer<QProgressDialog> m_progress;
};
