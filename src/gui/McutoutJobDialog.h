#ifndef McutoutJobDialog_h
#define McutoutJobDialog_h

// McutoutJobDialog.h
// One asynchronous VLKB job for a batch of cutouts.
//
// A single SODA request cuts one dataset; twenty means twenty requests, and the
// archive's own answer to that is a job you submit once and collect once. This
// window is the job: it shows what was asked for, where the job has got to, and
// what became of each dataset.
//
// Ported from the legacy client's MCutoutSummary (ViaLactea Visual Analytics
// 1.7.4), with the protocol moved to the backend (app/vlkb_mcutout.py), and
// beyond it:
//   * the results are unpacked, not handed over as a .tar.gz;
//   * Download keeps them without opening anything; Open… opens them, the
//     images of one source as layers of one viewer;
//   * Abort job stops the job on the archive, and closing the window while it
//     runs asks whether to stop it or leave it running;
//   * the elapsed time is shown, since the archive reports a phase only.

#include "BackendClient.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QVector>

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;

/// One unpacked cutout, with what is needed to open it sensibly.
struct McutoutResultFile
{
    QString path;    ///< on the backend
    QString label;   ///< the row's label
    QString type;    ///< "image", "cube" or empty if unknown
    QString group;   ///< the source it belongs to; images of one group share a viewer
};

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
    /// The user asked to open the unpacked cutouts.
    void openRequested(const QVector<McutoutResultFile> &files);

public slots:
    void reject() override;

private slots:
    void onSubmitFinished();
    void onPhaseFinished();
    void onReportFinished();
    void onFetchFinished();
    void onAbortFinished();
    void poll();
    void requestReport();
    void download();
    void openResults();
    void abortJob();
    void showFolder();
    void tick();

private:
    void buildUi(const QStringList &labels);
    void setStatus(const QString &text);
    void setRowStatus(int row, const QString &text, bool ok, const QString &detail = QString());
    void fetch(bool openAfter);
    QVector<McutoutResultFile> resultFiles() const;
    bool jobRunning() const;
    void updateButtons();

    QVector<BackendMcutoutItem> m_items;
    QStringList m_labels;
    QString m_serviceUrl;
    QString m_vlkbBearer;
    QString m_backendUrl;
    QString m_backendToken;
    QString m_jobId;
    QString m_phase;
    bool m_terminal{ false };
    bool m_reportOk{ false };
    bool m_openAfterFetch{ false };
    bool m_closeAfterAbort{ false };
    /// Closing was asked for while the job was still being submitted: what to
    /// do once the archive has named it.
    enum class PendingClose { None, Stop, Leave } m_pendingClose{ PendingClose::None };
    QElapsedTimer m_elapsed;
    QStringList m_reportFiles;   ///< per row: the file the archive named for it
    QStringList m_files;         ///< unpacked cutouts, on the backend
    QString m_directory;         ///< where they were unpacked, on the backend

    QFutureWatcher<BackendMcutoutSubmitResult> m_submitWatcher;
    QFutureWatcher<BackendMcutoutPhaseResult> m_phaseWatcher;
    QFutureWatcher<BackendMcutoutReportResult> m_reportWatcher;
    QFutureWatcher<BackendMcutoutFetchResult> m_fetchWatcher;
    QFutureWatcher<BackendMcutoutPhaseResult> m_abortWatcher;
    QPointer<QTimer> m_pollTimer;
    QPointer<QTimer> m_tickTimer;

    QPointer<QTableWidget> m_table;
    QPointer<QLabel> m_status;
    QPointer<QProgressBar> m_progress;
    QPointer<QPushButton> m_abortButton;
    QPointer<QPushButton> m_downloadButton;
    QPointer<QPushButton> m_openButton;
    QPointer<QPushButton> m_folderButton;
    /// Shown when the report could not be read: the job is finished on the
    /// archive, so the only thing that failed is the asking.
    QPointer<QPushButton> m_retryButton;
};

#endif   // McutoutJobDialog_h
