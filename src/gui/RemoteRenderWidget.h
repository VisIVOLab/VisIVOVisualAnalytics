#pragma once

#include <QImage>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <QWidget>
#include <chrono>

class QPaintEvent;
class QMouseEvent;
class QWheelEvent;
class QResizeEvent;
class QTimer;
class QWebSocket;
class QLabel;
class RemoteRenderWebRTCClient;

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
    /// Transport selector. Affects how the widget receives frames AND
    /// sends camera commands:
    ///
    ///   * ``JpegWS``       — existing WebSocket + JPEG path. Always
    ///                        available. Used when the backend has no
    ///                        H.265/NVENC stack or as a fallback.
    ///   * ``H265WebRTC``   — H.264 over WebRTC via libdatachannel.
    ///                        Requires VISIVO_HAS_WEBRTC at build time
    ///                        and ``h265-webrtc`` advertised on
    ///                        ``/v1/render/diagnostics::transports``.
    ///   * ``Auto``         — try H265WebRTC if VISIVO_HAS_WEBRTC AND
    ///                        the diagnostics probe (done elsewhere)
    ///                        confirmed availability; otherwise JpegWS.
    ///                        The caller controls the auto policy by
    ///                        explicitly calling ``setTransport`` with
    ///                        the resolved choice before connect.
    enum class Transport { Auto, JpegWS, H265WebRTC };

    explicit RemoteRenderWidget(QWidget *parent = nullptr);
    ~RemoteRenderWidget() override;

    /// Set the transport BEFORE calling :c:`connectToSession`. After
    /// connect, switching transports requires disconnect/reconnect.
    void setTransport(Transport t) { m_transport = t; }
    Transport transport() const { return m_transport; }

    /// Open a session channel. The branch — JPEG-WS vs H.264-WebRTC
    /// — is chosen by the current :c:`transport` setting at the time
    /// of this call. Both branches share the same paint pipeline so
    /// the widget code outside this class doesn't care which is live.
    /// ``baseUrl`` is the same URL the operator configured in Settings
    /// → Remote Backends, e.g. ``http://localhost:8000``.
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
    /// Fired by ``m_resizeDebounceTimer`` once the user has stopped
    /// dragging the window edge for 250 ms; sends a single resize
    /// command to the backend at the final size.
    void flushPendingResize();
    /// Fired by ``m_dragSendTimer`` to coalesce mouseMove deltas into
    /// one rotate/pan command per ~16 ms (60 Hz). Without this the
    /// data channel saw 70-100 commands/s on a fast Retina trackpad,
    /// which stacked apply_command calls in the executor and stole
    /// time from render_rgb_frame.
    void flushPendingDrag();

private:
    // JPEG/WS path (existing).
    void sendCommand(const QByteArray &jsonBytes);
    void requestFrame();

    // Transport selection.
    Transport m_transport{ Transport::JpegWS };

    QPointer<QWebSocket> m_ws;
    // WebRTC client — owned by this widget when m_transport ==
    // H265WebRTC. nullptr otherwise (and ``RemoteRenderWebRTCClient``
    // header is only included in builds with VISIVO_HAS_WEBRTC, but
    // the forward declaration keeps this header self-contained).
    RemoteRenderWebRTCClient *m_webrtc{ nullptr };

    QImage m_currentFrame;
    QString m_status{ QStringLiteral("Idle") };
    bool m_live{ false };
    // Last viewport size we POSTed via the WebRTC "resize" command. We
    // dedupe the double-fire (immediate + 250 ms deferred) against this
    // so a no-op size doesn't restart the NVENC encoder server-side.
    QSize m_lastResizeSent;
    // Coalesces bursts of ``resizeEvent`` from an interactive drag
    // into a single send 250 ms after the user lets go of the handle.
    // Saves the backend ~300 ms per spurious NVENC re-open.
    QTimer *m_resizeDebounceTimer{ nullptr };
    // Wheel-zoom LOD: there's no press/release pair to bracket a
    // scroll gesture, so we open with the first notch and close on
    // 250 ms of wheel inactivity.
    bool    m_zoomLodActive{ false };
    QTimer *m_zoomLodIdleTimer{ nullptr };

    // Drag throttling: accumulate camera deltas and ship one batched
    // rotate/pan command per 16 ms tick instead of one per mouseMove.
    QTimer *m_dragSendTimer{ nullptr };
    double  m_pendingAzimuthDelta{ 0.0 };
    double  m_pendingElevationDelta{ 0.0 };
    double  m_pendingPanDx{ 0.0 };
    double  m_pendingPanDy{ 0.0 };
    bool    m_pendingPanButton{ false };

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
