#ifndef NodeSoda_h
#define NodeSoda_h

#include <atomic>
#include <functional>
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

/// A SODA job run on an SRC node's VisIVO backend, where the data are: the
/// node's ``/soda/async`` (UWS 1.1) with the SODA cutout parameters, or the
/// custom ``OPERATION=MOMENT`` / ``OPERATION=PV`` the backend adds. The
/// product stays on the node, in the backend's workspace; the job's JSON view
/// says where (``output_path``), and the desktop opens it there.
///
/// Blocking, for a QtConcurrent worker: the request is made with the
/// thread's own QNetworkAccessManager, the job is polled until it ends, and
/// ``cancel`` (set from the UI thread) aborts it on the node.
struct NodeSodaJobResult
{
    bool    valid{ false };
    QString error;         ///< "cancelled" after a cancel the node confirmed
    QString jobUrl;        ///< the UWS job, once created
    QString phase;         ///< the job's last known UWS phase (COMPLETED, ERROR, ABORTED, …)
    QString outputPath;    ///< the product, on the NODE's filesystem
    QString resultUrl;     ///< the UWS result link (streams the product)
    QString applied;       ///< the subsets that took effect ("pos,band")
    QString noData;        ///< the service's text when the selection was empty
};

class NodeSodaClient
{
public:
    using Params = QList<QPair<QString, QString>>;

    /// ``serviceUrl`` is the job list (``…/soda/async``); ``backendToken`` the
    /// node backend's token (``X-Visivo-Token``), empty for an open backend.
    NodeSodaClient(const QString &serviceUrl, const QString &backendToken);

    /// Create the job with ``params`` and ``PHASE=RUN``, wait for it to end
    /// (polling), and return its outcome. ``cancel``, when set, aborts the job
    /// on the node and returns an error reading "cancelled" (or saying the
    /// node did not confirm the abort). ``progress`` receives the job's
    /// progress (0..1) as it is polled. Every request is bounded in time.
    NodeSodaJobResult run(const Params &params, std::atomic_bool *cancel = nullptr,
                          const std::function<void(double)> &progress = {}) const;

    /// The URL to call for a service SKAVA advertises at ``advertisedUrl``
    /// under the node backend ``advertisedBase``: the same service on
    /// ``backendBaseUrl``, the address this desktop reaches that backend at
    /// (SKAVA names the node as the catalogue sees it; the registered backend
    /// is how the user does). The service's path under its backend and its
    /// query are kept; without ``advertisedBase`` the whole path is. Empty
    /// ``backendBaseUrl`` = as advertised.
    static QString serviceUrlOn(const QString &backendBaseUrl, const QString &advertisedUrl,
                                const QString &advertisedBase = {});

    /// The JSON view of a job (``Accept: application/json``) as a result:
    /// valid when the job COMPLETED. Factored out for unit tests.
    static NodeSodaJobResult parseJobJson(const QByteArray &body);
    /// ``application/x-www-form-urlencoded`` body for ``params``.
    static QByteArray encodeForm(const Params &params);

private:
    /// One request's outcome. ``location`` is a 303's target (not followed).
    struct Exchange
    {
        QByteArray body;
        int        status{ 0 };
        QString    location;
        QString    error;
        bool       cancelled{ false };   ///< aborted because ``cancel`` was set
    };
    /// One request, given up after ``deadlineMs``; ``cancel``, when given,
    /// aborts it when set.
    Exchange request(const QByteArray &verb, const QString &url, const QByteArray &form,
                     std::atomic_bool *cancel, int deadlineMs) const;
    /// Ask the node to abort the job and wait for its phase to say so; the
    /// result's error reads "cancelled" when it did, else says what happened.
    NodeSodaJobResult abortJob(const QString &jobUrl, const QString &lastPhase) const;

    QString m_serviceUrl;
    QString m_token;
};

#endif  // NodeSoda_h
