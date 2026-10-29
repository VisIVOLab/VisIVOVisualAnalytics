#ifndef BrokerClient_h
#define BrokerClient_h

#include <atomic>
#include <functional>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/// A job on a Computing Broker (the SRCNet-style job API: ``POST /v1/jobs``,
/// ``GET /v1/jobs/{id}``, ``POST /v1/jobs/{id}/cancel``, ``/logs``). The
/// broker asks SKAVA where the dataset is and which node can run the finder,
/// then runs it as a task on that node's VisIVO backend: the desktop never
/// chooses the node, and never moves the data. The products stay on the
/// node; ``maskPath`` and ``catalogPath`` are where, on its filesystem.
struct BrokerJob
{
    bool    valid{ false };     ///< the answer was a job
    QString error;              ///< a failed request, or the job's own error
    QString id;
    QString status;             ///< PENDING, RUNNING, COMPLETE, FAILED, CANCELLED
    QString node;               ///< the node the broker placed the job on
    QString backend;            ///< that node's VisIVO backend
    QString rankReason;         ///< SKAVA's reason for the placement
    QString taskId;             ///< the task on the node
    QString finder;
    int     nSources{ -1 };
    QString maskPath;
    QString catalogPath;
    QJsonObject raw;            ///< the whole job, for anything else
    bool ended() const { return status == "COMPLETE" || status == "FAILED" || status == "CANCELLED"; }
};

class BrokerClient
{
public:
    /// ``baseUrl`` is the broker (``http://host:port``); ``bearerToken`` goes
    /// in ``Authorization: Bearer`` when set.
    BrokerClient(const QString &baseUrl, const QString &bearerToken = {});

    /// Submit a finder job for ``did`` (the dataset's identifier in SKAVA:
    /// its obs_id) and return it as created (PENDING).
    BrokerJob submit(const QString &did, const QString &finder, const QJsonObject &params,
                     int cpu = 1, const QString &memory = {}) const;
    BrokerJob status(const QString &jobId) const;
    /// Ask the broker to cancel the job. The broker's answer: the job as it
    /// stands (its status says whether the cancellation took), or, when the
    /// request failed, an invalid job whose ``error`` says why.
    BrokerJob cancel(const QString &jobId) const;
    /// The job's log, one line per entry.
    QStringList logs(const QString &jobId) const;
    /// The same, cancellable: ``cancelled`` says the read was cut short.
    QStringList logs(const QString &jobId, std::atomic_bool *cancel, bool &cancelled) const;

    /// Submit and wait for the job to end, polling. ``cancel``, when set,
    /// cancels the job on the broker and returns its final state (``error``
    /// reads "cancelled" when the broker confirmed it, else says what it
    /// answered). ``onLog`` receives each new log line as it appears. Every
    /// request is bounded in time; a broker that keeps failing is given up.
    BrokerJob run(const QString &did, const QString &finder, const QJsonObject &params,
                  std::atomic_bool *cancel = nullptr,
                  const std::function<void(const QString &)> &onLog = {},
                  int cpu = 1, const QString &memory = {}) const;

    /// A job document → BrokerJob (valid only with a status). Factored out for unit tests.
    static BrokerJob parseJob(const QByteArray &body);

private:
    struct Exchange
    {
        QByteArray body;
        int        status{ 0 };
        QString    error;
        bool       cancelled{ false };   ///< aborted because ``cancel`` was set
    };
    /// One request, given up after ``deadlineMs``; ``cancel``, when given,
    /// aborts it when set.
    Exchange request(const QByteArray &verb, const QString &path, const QByteArray &json,
                     std::atomic_bool *cancel, int deadlineMs) const;
    /// Cancel ``jobId`` and wait, within one time budget, for the broker to
    /// settle it.
    BrokerJob cancelAndSettle(const QString &jobId, const BrokerJob &lastKnown) const;

    QString m_baseUrl;
    QString m_token;
};

#endif  // BrokerClient_h
