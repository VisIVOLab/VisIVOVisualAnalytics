#pragma once

#include <QDialog>
#include <QPointer>
#include <QPair>
#include <QVector>

class BackendClient;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressDialog;
class QSpinBox;

/**
 * S-03 — Spectral baseline subtraction dialog.
 *
 * Calls BackendClient::requestBaseline() asynchronously.  On success the
 * backend registers a new session dataset; the dialog reports the new dataset
 * ID and RMS values to the user via a summary QMessageBox.
 */
class BaselineDialog : public QDialog
{
    Q_OBJECT

public:
    BaselineDialog(BackendClient *client,
                   const QString &sessionId,
                   const QString &datasetId,
                   int depth,
                   QWidget *parent = nullptr);

private slots:
    void onCompute();

private:
    /// Parse a comma/space-separated list of channel ranges like "1-50, 200-300"
    /// into a vector of 0-based [start, end] pairs.  Returns an empty vector on
    /// parse failure and sets \c errorOut.
    static QVector<QPair<int,int>> parseLineFreeRanges(const QString &text,
                                                        int depth,
                                                        QString &errorOut);

    void setUiBusy(bool busy);

    BackendClient           *m_client{ nullptr };
    QString                  m_sessionId;
    QString                  m_datasetId;
    int                      m_depth{ 1 };

    QComboBox               *m_methodCombo{ nullptr };
    QSpinBox                *m_polyOrderSpin{ nullptr };
    QLabel                  *m_polyOrderLabel{ nullptr };
    QLineEdit               *m_lineFreeEdit{ nullptr };
    QLabel                  *m_lineFreeHint{ nullptr };
    QDialogButtonBox        *m_buttons{ nullptr };
    QPointer<QProgressDialog> m_progress;
};
