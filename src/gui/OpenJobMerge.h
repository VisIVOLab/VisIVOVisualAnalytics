#ifndef OPEN_JOB_MERGE_H
#define OPEN_JOB_MERGE_H
// OpenJobMerge.h
// "Last 5 jobs" in the Data Hub: the backend's list, with the client's own
// record of the files it opened merged in.
//
// The backend times each request from its arrival to its response. For an
// open, that is the `datasets/open` request alone: a file refused at once (a
// truncated FITS) read "1ms", although the user had waited through the health
// check, the classification and maybe a decompression first. And an open the
// client refused before asking the backend (a file not found, a format nothing
// opens, a decompression cancelled before the task existed) was not in the
// list at all (#133).
//
// So the client keeps a short record of its opens: when the user asked, when
// the open ended (viewer shown, error reported, cancelled), how, and whether
// it reached the backend. mergeOpenJobs() then:
//   * gives an open the backend did see the client's end-to-end time and
//     outcome. The backend does not say which file a job was about, so the
//     match is by time: the backend's open job (an "…open" route, or a
//     "decompress" task when that is all there is) that FINISHED inside the
//     client's open, from request to result, with some slack. Pairs are chosen
//     best first across all opens, each job and each open once: inside the
//     open's own interval before inside its slack, an open route before a
//     decompression (which stays a job of its own), then the one that finished
//     closest to the open's end. The row then takes the open's end time;
//   * adds an open that never reached the backend as an entry of its own;
//   * leaves an open the backend saw but no longer lists alone: its job fell
//     out of the backend's five, and inventing it would put it back;
//   * keeps every backend job without a client record as the backend gave it;
//   * orders the whole by when each finished, newest first, and keeps @p limit.
//
// Ages are the backend's (seconds before its answer), so its clock never has
// to agree with this one: a job finished at answerTime − age, on this clock.
//
// Header-only and widget-free (TestOpenJobMerge).

#include "app/BackendClient.h"

#include <QFileInfo>
#include <QList>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <vector>

namespace visivo {

/// One open as the client saw it. Times in ms since the epoch, client clock.
struct ClientOpen
{
    QString path;
    QString status;              ///< "done", "failed" or "cancelled"
    qint64 requestedMs{ 0 };     ///< when the user asked
    qint64 finishedMs{ 0 };      ///< when the viewer showed, or the open ended otherwise
    bool reachedBackend{ false };   ///< a backend open route or decompression was asked
    QString backendUrl;             ///< the backend the open was made on
    QString backendToken;
};

/// One row of "Last 5 jobs".
struct HubJobRow
{
    QString operation;
    QString status;
    double durationSeconds{ 0.0 };
    double ageSeconds{ 0.0 };
    bool cacheHit{ false };
    /// The duration is the client's, from request to result.
    bool clientMeasured{ false };
    /// The backend's own time for its part, when known (-1: none).
    double backendSeconds{ -1.0 };
    QString path;   ///< the file, for a client-measured open
};

/// Whether a backend job is the backend's side of an open.
inline bool isOpenOperation(const QString &operation)
{
    return operation == QLatin1String("decompress") || operation.endsWith(QLatin1String("/open"))
            || operation.startsWith(QLatin1String("datasets/open"));
}

/// @p answeredMs: when the backend's answer (and its ages) arrived, client clock.
inline QVector<HubJobRow> mergeOpenJobs(const std::vector<BackendRecentTask> &backend,
                                        const QList<ClientOpen> &client, qint64 answeredMs,
                                        int limit = 5, qint64 slackMs = 1500)
{
    struct Row
    {
        HubJobRow row;
        qint64 finishedMs{ 0 };
        bool claimed{ false };
    };
    std::vector<Row> rows;
    rows.reserve(backend.size() + static_cast<std::size_t>(client.size()));
    for (const BackendRecentTask &t : backend) {
        Row r;
        r.row.operation = t.operation;
        r.row.status = t.status;
        r.row.durationSeconds = t.durationSeconds;
        r.row.ageSeconds = t.ageSeconds;
        r.row.cacheHit = t.cacheHit;
        r.finishedMs = answeredMs - static_cast<qint64>(std::llround(std::max(0.0, t.ageSeconds) * 1000.0));
        rows.push_back(r);
    }
    const std::size_t backendCount = rows.size();

    // Every (open, backend job) pair that may belong together, then the best
    // pairs first across ALL opens: a job inside an open's own interval
    // before one only inside its slack (so an earlier open never takes, by
    // its slack, the job of the open that followed it), an open route before
    // a decompression, then the job that finished closest to the open's end.
    struct Pair
    {
        int open{ 0 };
        std::size_t job{ 0 };
        bool strict{ false };
        bool openRoute{ false };
        qint64 gap{ 0 };
    };
    std::vector<Pair> pairs;
    for (int k = 0; k < client.size(); ++k) {
        const ClientOpen &c = client[k];
        if (!c.reachedBackend || c.finishedMs > answeredMs + slackMs)
            continue;
        for (std::size_t i = 0; i < backendCount; ++i) {
            const Row &r = rows[i];
            if (!isOpenOperation(r.row.operation))
                continue;
            if (r.finishedMs < c.requestedMs - slackMs || r.finishedMs > c.finishedMs + slackMs)
                continue;
            pairs.push_back({ k, i, r.finishedMs >= c.requestedMs && r.finishedMs <= c.finishedMs,
                              r.row.operation != QLatin1String("decompress"),
                              std::llabs(r.finishedMs - c.finishedMs) });
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair &x, const Pair &y) {
        if (x.strict != y.strict)
            return x.strict;
        if (x.openRoute != y.openRoute)
            return x.openRoute;
        return x.gap < y.gap;
    });
    std::vector<bool> openMatched(static_cast<std::size_t>(client.size()), false);
    for (const Pair &pair : pairs) {
        Row &r = rows[pair.job];
        if (r.claimed || openMatched[static_cast<std::size_t>(pair.open)])
            continue;
        const ClientOpen &c = client[pair.open];
        r.claimed = true;
        openMatched[static_cast<std::size_t>(pair.open)] = true;
        r.row.backendSeconds = r.row.durationSeconds;
        r.row.durationSeconds = std::max<qint64>(0, c.finishedMs - c.requestedMs) / 1000.0;
        r.row.status = c.status;
        r.row.clientMeasured = true;
        r.row.path = c.path;
        // The open ended when the client presented its result, not when the
        // backend answered: ordered and aged by that.
        r.finishedMs = c.finishedMs;
        r.row.ageSeconds = std::max<qint64>(0, answeredMs - c.finishedMs) / 1000.0;
    }

    // An open that never reached the backend is an entry of its own. One that
    // did but found no job (it fell out of the backend's five) is left out.
    for (const ClientOpen &c : client) {
        if (c.reachedBackend || c.finishedMs > answeredMs + slackMs)
            continue;
        Row r;
        r.row.operation = QStringLiteral("open %1").arg(QFileInfo(c.path).fileName());
        r.row.status = c.status;
        r.row.durationSeconds = std::max<qint64>(0, c.finishedMs - c.requestedMs) / 1000.0;
        r.row.ageSeconds = std::max<qint64>(0, answeredMs - c.finishedMs) / 1000.0;
        r.row.clientMeasured = true;
        r.row.path = c.path;
        r.finishedMs = c.finishedMs;
        rows.push_back(r);
    }

    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &a, const Row &b) { return a.finishedMs > b.finishedMs; });
    QVector<HubJobRow> out;
    for (const Row &r : rows) {
        if (out.size() >= limit)
            break;
        out.push_back(r.row);
    }
    return out;
}

}   // namespace visivo

#endif   // OPEN_JOB_MERGE_H
