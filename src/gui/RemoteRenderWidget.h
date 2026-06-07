#pragma once

#include <QImage>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <QWidget>
#include <chrono>

class QPaintEvent;
class QMouseEvent;
class QWheelEvent;
class QResizeEvent;
class QWebSocket;
class QLabel;

/**
 * @brief Client-side widget that displays JPEG frames streamed from the
 *        VisIVO backend's server-side rendering session.
 *
 * Responsibilities:
 *   - Open a WebSocket to ``ws://<backend>/v1/render/<session_id>/ws``
 *     with the bearer token in the URL query (matches the backend
 *     auth path).
 *   - Decode each incoming binary frame as a JPEG, display it, paint
 *     a status overlay while loading.
 *   - Translate mouse / wheel / resize events into JSON commands sent
 *     to the server (rotate, zoom, pan, resize).
 *   - Throttle frame REQUESTS to the screen refresh rate so the
 *     server doesn't render dozens of frames the user will never see.
 *
 * The widget does NOT own the session lifecycle — that's done via
 * BackendClient REST calls (``POST /v1/render/create_session``,
 * ``DELETE /v1/render/{id}``). The widget only handles the live
 * channel between session-create and session-delete.
 */
class RemoteRenderWidget : public QWidget {
    Q_OBJECT
public:
    explicit RemoteRenderWidget(QWidget *parent = nullptr);
    ~RemoteRenderWidget() override;

    /// Open the WebSocket. Call after :c:`POST /v1/render/create_session`
    /// returns a session_id. ``baseUrl`` is the same URL the operator
    /// configured in Settings → Remote Backends, e.g. ``http://localhost:8000``.
    void connectToSession(const QString &baseUrl,
                          const QString &sessionId,
                          const QString &token);

    /// Cleanly close the WebSocket. Does NOT call the DELETE endpoint;
    /// the caller still owns the session lifetime.
    void disconnectFromSession();

    /// Render-quality preference (passed to the server as a JSON
    /// command). 0..100; sensible default 85 — the backend's default
    /// JPEG quality.
    void setJpegQuality(int q);

    /// Send an arbitrary state-mutation command to the server (LUT
    /// changes, threshold, blend mode, cutting plane visibility, …)
    /// and ask for a fresh frame so the user sees the effect.
    /// No-op if not currently connected — the next reconnect should
    /// re-push the desired state from vtkWindowCube's stored values.
    /// The map is JSON-serialised as ``{ "type": <type>, ... }``;
    /// callers must include the ``type`` key themselves.
    Q_INVOKABLE void sendStateCommand(const QVariantMap &command);

    /// True once the WebSocket has connected AND the first JPEG frame
    /// has been received and drawn. Used by the cube viewer to gate
    /// "loading…" → "ready" UI transitions.
    bool isLive() const { return m_live; }

signals:
    /// Emitted on every successful frame draw — exposes the wall-clock
    /// frame interval as observed by the client so the status bar can
    /// show "Render: X FPS".
    void framesPerSecondChanged(double fps);

    /// Emitted on connection state changes (connecting, connected,
    /// error, closed). String is human-readable; mapped to a UI badge.
    void connectionStateChanged(const QString &state);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onTextMessageReceived(const QString &message);
    void onBinaryMessageReceived(const QByteArray &bytes);

private:
    void sendCommand(const QByteArray &jsonBytes);
    void requestFrame();

    QPointer<QWebSocket> m_ws;
    QImage m_currentFrame;
    QString m_status{ QStringLiteral("Idle") };
    bool m_live{ false };

    // Mouse drag tracking for rotation.
    bool m_dragActive{ false };
    int m_lastMouseX{ 0 };
    int m_lastMouseY{ 0 };

    // FPS measurement (decaying mean over the last ~1 s of frames).
    std::chrono::steady_clock::time_point m_lastFrameTime;
    double m_fpsEma{ 0.0 };

    // Settings.
    int m_jpegQuality{ 85 };

    // Throttle outgoing request_frame messages: at most one in flight
    // at a time. This makes the server skip frames the client wouldn't
    // see anyway, keeping the tunnel from filling up during fast drag.
    bool m_frameInFlight{ false };
};
