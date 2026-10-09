#ifndef McutoutJobDialog_h
#define McutoutJobDialog_h

// McutoutJobDialog.h
// One asynchronous VLKB job for a batch of cutouts.
//
// A single SODA request cuts one dataset; twenty means twenty requests, and the
// archive's own answer to that is a job you submit once and collect once. This
// window is the job: it shows what was asked for, where the job has got to, and
// what became of each dataset — then unpacks the results and opens them.
//
// Ported from the legacy client's MCutoutSummary (ViaLactea Visual Analytics
// 1.7.4), with the protocol moved to the backend (app/vlkb_mcutout.py) and one
// behaviour added: the legacy handed the user a `.tar.gz` and stopped, which
// left the cutouts a manual unpacking away from the viewer that had asked for
// them.

#include "BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;

class McutoutJobDialog : public QDialog
{
    Q_OBJECT

public:
    /// @param items      what to cut, in the order the inventory listed them
    /// @param labels     a human-readable name per item, for the table
    /// @param serviceUrl the VLKB base URL ("" = the backend's configured one)
    McutoutJobDialog(const QVector<BackendMcutoutItem> &items, const QStringList &labels,
                     const QString &serviceUrl, const QString &vlkbBearer,
                     const QString &backendUrl, const QString &backendToken,
                     QWidget *parent = nullptr);
    ~McutoutJobDialog() override;

signals:
    /// One extracted cutout, ready to open. Emitted per file when the user asks.
    void cutoutReady(const QString &backendPath);

private slots:
    void onSubmitFinished();
    void onPhaseFinished();
    void onReportFinished();
    void onFetchFinished();
    void poll();
    void collect();
    void requestReport();

private:
    void buildUi(const QStringList &labels);
    void setStatus(const QString &text);
    void setRowStatus(int row, const QString &text, bool ok, const QString &detail = QString());

    QVector<BackendMcutoutItem> m_items;
    QString m_serviceUrl;
    QString m_vlkbBearer;
    QString m_backendUrl;
    QString m_backendToken;
    QString m_jobId;
    bool m_collected{ false };

    QFutureWatcher<BackendMcutoutSubmitResult> m_submitWatcher;
    QFutureWatcher<BackendMcutoutPhaseResult> m_phaseWatcher;
    QFutureWatcher<BackendMcutoutReportResult> m_reportWatcher;
    QFutureWatcher<BackendMcutoutFetchResult> m_fetchWatcher;
    QPointer<QTimer> m_pollTimer;

    QPointer<QTableWidget> m_table;
    QPointer<QLabel> m_status;
    QPointer<QProgressBar> m_progress;
    QPointer<QPushButton> m_collectButton;
    /// Shown when the report could not be read: the job is finished on the
    /// archive, so the only thing that failed is the asking.
    QPointer<QPushButton> m_retryButton;
};

#endif   // McutoutJobDialog_h
