#ifndef SkavaSourceFindDialog_h
#define SkavaSourceFindDialog_h

#include "../app/BackendClient.h"   // BackendFinderInfo

#include <QDialog>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

/// What a source finder run next to the data produced: the products on the
/// node's filesystem, and the backend this desktop opens them from.
struct SkavaFinderOutcome
{
    bool    valid{ false };
    QString error;
    QString finder;
    int     nSources{ -1 };
    QString maskPath;          ///< label mask, on the node
    QString catalogPath;       ///< VOTable catalogue, on the node
    QString node;              ///< SKAVA node code the finder ran on
    QString rankReason;        ///< why that node (the placement's reason)
    QString backendUrl;        ///< the node's registered backend, to open the products
    QString backendToken;
};

/// A source finder run where the dataset lives (SKAVA tab ▸ Compute next
/// to data ▸ Source finder…): pick the finder among those the node offers,
/// set its parameters, follow the run's log, open the mask and the
/// catalogue from the node. How the run is made (through the Computing
/// Broker, or as a task on the node's backend) is the ``runner`` the caller
/// hands in; it is called on a worker thread with a cancel flag and a log sink.
class SkavaSourceFindDialog : public QDialog
{
    Q_OBJECT
public:
    using Runner = std::function<SkavaFinderOutcome(const QString &finder, const QJsonObject &params,
                                                    std::atomic_bool *cancel,
                                                    const std::function<void(const QString &)> &log)>;

    /// ``finders`` are the node's (an empty list offers the two VisIVO knows);
    /// ``kind`` ("cube" / "image") preselects the finder; ``where`` says where
    /// the run happens, for the user.
    SkavaSourceFindDialog(const QVector<BackendFinderInfo> &finders, const QString &datasetLabel,
                          const QString &kind, const QString &where, Runner runner,
                          QWidget *parent = nullptr);
    ~SkavaSourceFindDialog() override;

signals:
    void openMaskRequested(const SkavaFinderOutcome &outcome);
    void openCatalogueRequested(const SkavaFinderOutcome &outcome);

public slots:
    /// Escape: handled as a close, so a running job is not abandoned silently.
    void reject() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void fillParameters(int finderIndex);
    QJsonObject parameters() const;
    void run();
    void cancel();
    void appendLog(const QString &line);
    void showOutcome(const SkavaFinderOutcome &outcome);
    void setBusy(bool busy);

    QVector<BackendFinderInfo> m_finders;
    Runner m_runner;
    QComboBox      *m_finderCombo{ nullptr };
    QLabel         *m_finderNote{ nullptr };
    QTableWidget   *m_params{ nullptr };
    QPlainTextEdit *m_log{ nullptr };
    QLabel         *m_summary{ nullptr };
    QPushButton    *m_runBtn{ nullptr };
    QPushButton    *m_cancelBtn{ nullptr };
    QPushButton    *m_openMaskBtn{ nullptr };
    QPushButton    *m_openCatBtn{ nullptr };
    /// Shared with the worker: set by Cancel, read by the runner.
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_busy{ false };
    SkavaFinderOutcome m_outcome;
};

#endif  // SkavaSourceFindDialog_h
