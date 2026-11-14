#ifndef VRSPECTATORFEED_H
#define VRSPECTATORFEED_H

// VrSpectatorFeed — the desktop's end of the spectator view (issue #281), on
// the GUI thread's event loop: it watches one headset page of a backend
// session and hands over each new frame.
//
// start() asks the backend to watch the page (POST /v1/vr/headset/spectator
// on), then reads frames one at a time (GET .../frame, waiting up to 3 s for
// a newer one). There is never more than one read out: a slow desktop gets
// the latest frame, frames in between are skipped, nothing queues. Every
// read renews the backend's lease. stop() (or the destructor) says the watch
// is over (on: false), best effort. A 409 (the lease ran out, say after the
// laptop slept) starts the watch again; a 404 (the page went) ends it. No
// frame for kRewatchMs while the backend says it is watched (the page paused
// on a bad link, or was reloaded and missed its command): the watch is sent
// again, which the page takes as a fresh start.
//
// Frames are handed over as the JPEG the headset made (decoded by the window,
// so this stays free of QtGui).
//
// snapshot() saves the latest frame as a PNG in the Workspace Exports (the
// backend writes it: it is the backend's workspace, local or remote).
//
// The requests carry the session's token and id in their headers
// (BackendClient::applySessionHeaders); nothing here logs either.

#include "VrSpectator.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

class VrSpectatorFeed : public QObject
{
    Q_OBJECT

public:
    VrSpectatorFeed(const QString &baseUrl, const QString &token, const QString &session,
                    const QString &client, QObject *parent = nullptr);
    ~VrSpectatorFeed() override;

    void start(int fps = VrSpectator::kDefaultFps, int width = VrSpectator::kDefaultWidth);
    /// The watch is over: the backend (and so the page) is told, best effort.
    void stop();
    void snapshot();

    bool running() const { return m_running; }
    QString client() const { return m_client; }
    /// The latest frame (its JPEG and numbers); kind None before the first.
    const VrSpectator::FrameReply &latest() const { return m_info; }
    double fps() const;
    qint64 framesShown() const { return m_frames; }
    /// No frame for this long while watched: the watch is sent again (ms).
    static constexpr int kRewatchMs = 10000;
    void setRewatchMs(int ms) { m_rewatchMs = ms; }   ///< for the tests

signals:
    /// A new frame: its JPEG and its numbers.
    void frameReady(const VrSpectator::FrameReply &frame);
    /// Watching started (the backend accepted it).
    void watching(int fps, int width);
    /// It ended: the page went, the backend has no such route, or it refused.
    void ended(const QString &why);
    /// A passing problem (a failed read): it reads again by itself.
    void trouble(const QString &why);
    void snapshotSaved(const VrSpectator::SnapshotResult &result);
    void snapshotFailed(const QString &why);

private:
    /// @p thenRead: read frames once it is accepted (false: a read is already going on).
    void sendWatch(bool on, bool thenRead = true);
    void readNext();
    void onFrame(QNetworkReply *reply);
    void finish(const QString &why);
    QNetworkReply *post(const QString &path, const QJsonObject &body, int timeoutMs);

    QNetworkAccessManager *m_nam{ nullptr };
    QString m_baseUrl, m_token, m_session, m_client;
    int m_fps{ VrSpectator::kDefaultFps };
    int m_width{ VrSpectator::kDefaultWidth };
    bool m_running{ false };
    quint64 m_generation{ 0 };
    qint64 m_after{ 0 };
    QString m_instance;
    int m_fails{ 0 };
    int m_restarts{ 0 };
    qint64 m_lastProgressMs{ 0 };
    int m_rewatchMs{ kRewatchMs };   ///< m_clock at the start, the last frame or the last rewatch
    qint64 m_frames{ 0 };
    QPointer<QNetworkReply> m_read;
    QTimer m_retry;
    VrSpectator::FrameReply m_info{ VrSpectator::FrameReply::Kind::None };
    VrSpectator::RateMeter m_rate;
    QElapsedTimer m_clock;
};

#endif // VRSPECTATORFEED_H
