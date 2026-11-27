#pragma once

// RemotePointView.h
// A point table drawn on the backend (issue #83): the render session's life
// (create, connect, release) and the state sent to it, for a viewer that keeps
// its own controls. The cube viewer does the same for cubes inline; point
// viewers share this.
//
// State is sent as named commands, each only when its content changes: some
// (the coordinate fields, the filters) make the backend rebuild every point of
// a table that can hold hundreds of millions.

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include "RemoteRenderQueue.h"

#include "RemoteSessionRecovery.h"

class QJsonObject;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class RemoteRenderWidget;

class RemotePointView : public QObject
{
    Q_OBJECT

public:
    RemotePointView(const QString &backendUrl, const QString &token, const QString &sessionId,
                    const QString &datasetId, RemoteRenderWidget *widget,
                    QObject *parent = nullptr);
    ~RemotePointView() override;

    /// Creates the session and connects the widget; started() or failed().
    /// With every backend slot busy it waits in the backend's queue first
    /// (queued(), issue #4).
    void start();
    /// Disconnects and deletes the session, or gives up the place in the queue.
    void stop();
    bool isActive() const { return !m_renderSessionId.isEmpty(); }
    /// A view that is not on screen (a pane its layout hides) gives the backend's session
    /// and the stream back, and keeps what it would send; resume() asks for a session
    /// again and the view comes back as it was (state, and the camera it had).
    void pause();
    void resume();
    bool isPaused() const { return m_paused; }
    /// The backend's render session, empty before it starts.
    QString renderSessionId() const { return m_renderSessionId; }

    /// The latest value of a state command (keyed by its "type"). Sent now
    /// when the view is live and it differs from what was sent; otherwise
    /// kept, and sent when the view goes live.
    void setState(const QVariantMap &command);
    /// A one-off command (a camera reset): sent only when live.
    void send(const QVariantMap &command);

signals:
    void started();
    /// The colour field's range over the whole table, as the backend colours
    /// it (after the session starts and after each colour change).
    void colourRange(double lo, double hi);
    void failed(const QString &reason);
    void liveChanged(bool live);
    /// Every slot of the backend is busy: the view waits in its queue. The
    /// sentence says where; sent again each time the backend answers.
    void queued(const QString &status);
    /// The stream dropped and the view is being reconnected (issue #411): the
    /// sentence says which attempt; started() or failed() ends it.
    void reconnecting(const QString &status);

private:
    /// Asks create_session (again, with the place in the queue if any).
    void request();
    /// create_session answered for the generation asked.
    void created(const visivo::RenderSessionAnswer &answer, const QJsonObject &doc);
    void flush();
    void fetchLevels();
    /// The session and its stream let go, the reconnection left as it is.
    void release();
    /// One step of the reconnection (RemoteSessionRecovery.h).
    void runStep(visivo::ReconnectStep step);
    /// GET .../levels: is the session still there?
    void checkSession();
    void onReconnectTimer();
    void say(const QString &status);
    /// Keep the session's camera a moment after the user turned the view.
    void scheduleCameraSnapshot();

    const QString m_backendUrl;
    const QString m_token;
    const QString m_sessionId;
    const QString m_datasetId;
    QPointer<RemoteRenderWidget> m_widget;
    QNetworkAccessManager *m_nam{ nullptr };
    QString m_renderSessionId;
    quint64 m_generation{ 0 };
    /// A create_session in flight or a place in the queue: start() waits.
    bool m_requesting{ false };
    visivo::RenderQueuePlace m_place;
    bool m_live{ false };
    /// pause() was called and resume() has not: no session, nothing sent.
    bool m_paused{ false };
    /// A camera state arrived while paused (the group moved): newer than m_camera.
    bool m_cameraSetWhilePaused{ false };
    /// A live stream that dropped is reconnected: the same session if it is
    /// still there, else a new one, with bounded retries; then failed() and the
    /// window shows its loaded rows (RemoteSessionRecovery.h, issue #411).
    visivo::RemoteReconnect m_reconnect;
    QTimer *m_reconnectTimer{ nullptr };
    /// A reconnection's stream is being connected: a failure before Live, or
    /// no Live within kReconnectAttemptTimeoutMs, is a failed attempt.
    bool m_attemptConnecting{ false };
    quint64 m_checkSerial{ 0 };
    QString m_reconnectWhy;
    /// The session's camera as of the user's last turn of the view, given to
    /// a session that replaces a lost one.
    QJsonObject m_camera;
    bool m_cameraRestore{ false };
    QTimer *m_cameraTimer{ nullptr };
    QHash<QString, QVariantMap> m_state;   ///< latest per type
    QHash<QString, QVariantMap> m_sent;    ///< last sent per type
};
