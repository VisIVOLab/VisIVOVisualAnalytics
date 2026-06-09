#include "RemoteRenderWidget.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWheelEvent>

using namespace std::chrono_literals;

namespace {
// VTK's vtkInteractorStyleTrackballCamera uses delta_azimuth =
// -20 / window_width * MotionFactor (default MotionFactor = 10) per
// drag pixel, applied INCREMENTALLY to the current camera. For a
// typical 1000-pixel viewport that's ~0.2°/px. With the server now
// running the same incremental model (see render_session.py
// `_apply_command_on_thread` → `cam.Azimuth(delta)`), this constant
// gives a one-to-one match with the local VTK widget feel.
constexpr double kRotateSensitivity = 0.2;
constexpr double kPanSensitivity    = 1.0;
constexpr double kZoomPerNotch      = 1.1;
} // namespace

RemoteRenderWidget::RemoteRenderWidget(QWidget *parent)
    : QWidget(parent),
      m_lastFrameTime(std::chrono::steady_clock::now())
{
    // Black is the right starting backdrop for an astronomical volume
    // — matches the backend's default background and prevents a flash
    // of system-grey before the first frame lands.
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setMouseTracking(false);   // no commands until the user clicks
    setFocusPolicy(Qt::StrongFocus);
}

RemoteRenderWidget::~RemoteRenderWidget()
{
    disconnectFromSession();
}

// ── Connection lifecycle ────────────────────────────────────────────────────

#ifdef VISIVO_HAS_WEBRTC
#include "RemoteRenderWebRTCClient.h"
#endif

void RemoteRenderWidget::connectToSession(const QString &baseUrl,
                                          const QString &sessionId,
                                          const QString &token)
{
    disconnectFromSession();

#ifdef VISIVO_HAS_WEBRTC
    // ── H.264-over-WebRTC branch ─────────────────────────────────
    if (m_transport == Transport::H265WebRTC) {
        m_webrtc = new RemoteRenderWebRTCClient(this);
        // The widget's frame buffer is shared between transports — the
        // paint event doesn't care if the QImage came from JPEG decode
        // or H.264 decode. We just copy on the signal.
        //
        // VERTICAL FLIP: the WebRTC backend sends pixels bottom-up
        // (OpenGL framebuffer order) without flipping server-side —
        // doing that flip in numpy used to cost ~20 ms per frame at
        // 1080p on Power9 because of the negative stride + sliced
        // last axis. Qt's ``QImage::mirrored(false, true)`` is the
        // SIMD-vectorised equivalent and runs in roughly 1-3 ms on
        // Apple Silicon. Setting ``m_currentFrame`` to the mirrored
        // copy keeps the rest of paintEvent simple.
        connect(m_webrtc, &RemoteRenderWebRTCClient::frameReady,
                this, [this](const QImage &img) {
            m_currentFrame = img.mirrored(false, true);
            if (!m_live) {
                m_live = true;
            }
            update();
        });
        connect(m_webrtc, &RemoteRenderWebRTCClient::framesPerSecondChanged,
                this, &RemoteRenderWidget::framesPerSecondChanged);
        connect(m_webrtc, &RemoteRenderWebRTCClient::connectionStateChanged,
                this, [this](const QString &state) {
            m_status = state;
            // Flip ``m_live`` here, BEFORE emitting connectionStateChanged.
            // vtkWindowCube hooks the same signal and runs the bootstrap
            // (LUT / threshold / blend / cutting-plane) when it sees
            // "Live". That bootstrap calls sendStateCommand, which guards
            // on isLive(). If we waited for the first frameReady to flip
            // m_live, every bootstrap SEND would race-drop and the user
            // would see a galaxy at the wrong threshold / wrong LUT /
            // wrong cutting plane until they wiggled the controls.
            if (state == QLatin1String("Live")) {
                m_live = true;
            }
            emit connectionStateChanged(state);
            update();
            // On Live transition, push an initial ``resize`` so the
            // backend renders at our PHYSICAL pixel count instead of
            // the SessionViewState default (1024×768). Fire TWICE
            // with a delay so the second one catches up to the dock's
            // deferred layout: ``setupWorkspaceDocks`` queues a
            // ``resizeDocks`` call on the next event-loop turn, so the
            // widget's width() returned right at Live transition can
            // still be the smaller pre-layout value. Re-firing 250 ms
            // later lets that settle.
            if (state == QLatin1String("Live") && m_webrtc) {
                // Track the last size we sent so the 250 ms-delayed
                // re-fire below is a no-op when the dock layout didn't
                // actually change the viewport. Without this guard we
                // send a SECOND resize with the same dimensions, which
                // aiortc treats as a frame-size change and rebuilds
                // the NVENC encoder context — that costs ~300 ms +
                // forces a fresh IDR, killing first-second FPS for
                // nothing.
                auto sendResize = [this]() {
                    if (!m_webrtc) return;
                    const qreal dpr = devicePixelRatioF();
                    const int physW = qRound(width()  * dpr);
                    const int physH = qRound(height() * dpr);
                    if (physW == m_lastResizeSent.width()
                            && physH == m_lastResizeSent.height()) {
                        qDebug().noquote() << QStringLiteral(
                                "[webrtc] skipping resize: %1×%2 == last sent")
                                .arg(physW).arg(physH);
                        return;
                    }
                    m_lastResizeSent = QSize(physW, physH);
                    QVariantMap sz;
                    sz[QStringLiteral("type")]   = QStringLiteral("resize");
                    sz[QStringLiteral("width")]  = physW;
                    sz[QStringLiteral("height")] = physH;
                    qDebug().noquote() << QStringLiteral(
                            "[webrtc] sending resize: %1×%2 logical -> "
                            "%3×%4 physical (dpr=%5)")
                            .arg(width()).arg(height()).arg(physW)
                            .arg(physH).arg(dpr);
                    m_webrtc->sendStateCommand(sz);
                };
                sendResize();   // immediate
                QTimer::singleShot(250, this, sendResize);   // post-layout
            }
        });
        m_status = QStringLiteral("Connecting…");
        emit connectionStateChanged(m_status);
        update();
        m_webrtc->connectToSession(baseUrl, sessionId, token);
        return;
    }
#endif

    // ── JPEG-over-WebSocket branch (legacy / always-available) ───
    // Build ws://host:port/v1/render/<id>/ws?token=...
    QUrl wsUrl(baseUrl);
    const QString scheme = (wsUrl.scheme() == QLatin1String("https"))
                                   ? QStringLiteral("wss")
                                   : QStringLiteral("ws");
    wsUrl.setScheme(scheme);
    wsUrl.setPath(QStringLiteral("/v1/render/") + sessionId + QStringLiteral("/ws"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("token"), token);
    wsUrl.setQuery(q);

    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::connected,
            this, &RemoteRenderWidget::onWebSocketConnected);
    connect(m_ws, &QWebSocket::disconnected,
            this, &RemoteRenderWidget::onWebSocketDisconnected);
    connect(m_ws, &QWebSocket::textMessageReceived,
            this, &RemoteRenderWidget::onTextMessageReceived);
    connect(m_ws, &QWebSocket::binaryMessageReceived,
            this, &RemoteRenderWidget::onBinaryMessageReceived);
    // Qt6 unified error signal:
    connect(m_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        const QString msg = m_ws ? m_ws->errorString() : QStringLiteral("(no socket)");
        m_status = QStringLiteral("Error: ") + msg;
        emit connectionStateChanged(m_status);
        update();
    });

    m_status = QStringLiteral("Connecting…");
    emit connectionStateChanged(m_status);
    update();
    m_ws->open(wsUrl);
}

void RemoteRenderWidget::disconnectFromSession()
{
    if (m_ws) {
        m_ws->close();
        m_ws->deleteLater();
        m_ws = nullptr;
    }
#ifdef VISIVO_HAS_WEBRTC
    if (m_webrtc) {
        m_webrtc->disconnectFromSession();
        m_webrtc->deleteLater();
        m_webrtc = nullptr;
    }
#endif
    m_live = false;
    m_frameInFlight = false;
    m_status = QStringLiteral("Disconnected");
    emit connectionStateChanged(m_status);
}

void RemoteRenderWidget::setJpegQuality(int q)
{
    m_jpegQuality = qBound(1, q, 100);
    if (m_live) {
        QJsonObject cmd;
        cmd[QStringLiteral("type")]    = QStringLiteral("jpeg_quality");
        cmd[QStringLiteral("quality")] = m_jpegQuality;
        sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
    }
}

void RemoteRenderWidget::sendStateCommand(const QVariantMap &command)
{
#ifdef VISIVO_HAS_WEBRTC
    // WebRTC path: forward to the DataChannel client. The WebRTC
    // transport pushes frames continuously at server-paced FPS, so we
    // don't issue a request_frame after the command — the next frame
    // in the stream will already reflect the new state.
    if (m_transport == Transport::H265WebRTC && m_webrtc) {
        m_webrtc->sendStateCommand(command);
        return;
    }
#endif
    if (!m_ws || m_ws->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    const QJsonObject obj = QJsonObject::fromVariantMap(command);
    sendCommand(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    // Mutating state without re-rendering would leave the user staring
    // at a stale frame, so always nudge the server for a fresh one.
    // requestFrame() is throttled (one in-flight at a time) so this is
    // safe to call after every command.
    requestFrame();
}

// ── WebSocket callbacks ─────────────────────────────────────────────────────

void RemoteRenderWidget::onWebSocketConnected()
{
    m_status = QStringLiteral("Connected, awaiting first frame…");
    emit connectionStateChanged(m_status);
    // Tell the server our REAL render size (physical pixels) BEFORE we
    // ask for the first frame — otherwise the bootstrap frame arrives
    // at the SessionViewState default (1024×768) and gets upscaled,
    // looking blurry until the next resize event.
    const qreal dpr = devicePixelRatioF();
    QJsonObject sz;
    sz[QStringLiteral("type")]   = QStringLiteral("resize");
    sz[QStringLiteral("width")]  = qRound(width()  * dpr);
    sz[QStringLiteral("height")] = qRound(height() * dpr);
    sendCommand(QJsonDocument(sz).toJson(QJsonDocument::Compact));
    requestFrame();
    update();
}

void RemoteRenderWidget::onWebSocketDisconnected()
{
    m_live = false;
    m_frameInFlight = false;
    m_status = QStringLiteral("Disconnected");
    emit connectionStateChanged(m_status);
    update();
}

void RemoteRenderWidget::onTextMessageReceived(const QString &message)
{
    // Diagnostic JSON from the server (e.g. {"error": "…"}). We don't
    // expose it to the UI directly yet; future commit can wire it.
    qDebug().noquote() << "[remote-render] server text:" << message;
}

void RemoteRenderWidget::onBinaryMessageReceived(const QByteArray &bytes)
{
    QImage img;
    if (!img.loadFromData(bytes, "JPEG")) {
        qWarning() << "[remote-render] received invalid JPEG, size=" << bytes.size();
        return;
    }
    m_currentFrame = std::move(img);

    // FPS EMA (alpha ~ 0.2, smoothing window ~ 5 frames).
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - m_lastFrameTime).count();
    m_lastFrameTime = now;
    if (dt > 0.0 && dt < 10.0) {
        const double instant = 1.0 / dt;
        m_fpsEma = (m_fpsEma == 0.0) ? instant : (0.8 * m_fpsEma + 0.2 * instant);
        emit framesPerSecondChanged(m_fpsEma);
    }

    if (!m_live) {
        m_live = true;
        m_status = QStringLiteral("Live");
        emit connectionStateChanged(m_status);
    }
    m_frameInFlight = false;
    update();
}

// ── Painting ────────────────────────────────────────────────────────────────

void RemoteRenderWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (!m_currentFrame.isNull()) {
        // We may receive frames at the backend's default render size
        // (1024×768) before our resize-on-Live event has propagated.
        // Scale-to-fit ensures the frame ALWAYS fills the widget
        // area with the correct aspect ratio — at worst the backend
        // catches up to our resize on the next frame and we drop the
        // up-scaling cost. ``Qt::SmoothTransformation`` is bilinear;
        // good enough at the typical 30-60 FPS render cadence.
        //
        // When the backend already renders at our physical pixel
        // count (post-resize, Retina display), this becomes a no-op
        // 1:1 paint with no upscale blur.
        const QImage scaled = m_currentFrame.scaled(
                size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const int x = (width()  - scaled.width())  / 2;
        const int y = (height() - scaled.height()) / 2;
        painter.drawImage(QPoint(x, y), scaled);
    }

    if (!m_live) {
        // Status overlay until the first frame arrives.
        painter.setPen(Qt::white);
        painter.setFont(QFont(QStringLiteral("Sans-Serif"), 14, QFont::Light));
        painter.drawText(rect(), Qt::AlignCenter, m_status);
    }
}

// ── Input → commands ────────────────────────────────────────────────────────

void RemoteRenderWidget::mousePressEvent(QMouseEvent *event)
{
    qDebug().noquote() << QStringLiteral(
            "[remote-render] mousePressEvent button=%1 live=%2 transport=%3")
            .arg(int(event->button())).arg(m_live ? 1 : 0)
            .arg(int(m_transport));
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_dragActive = true;
        m_lastMouseX = event->position().x();
        m_lastMouseY = event->position().y();
        // Interactive LOD: tell the backend to drop the volume + cutting
        // plane and render only the bounding-box wireframe while we drag.
        // VTK volume render is the dominant frame cost (~30 ms at 1080p
        // on V100); wireframe is sub-millisecond. The encoder + RTP send
        // budget is plenty for 60 FPS, so the rotation feels instant.
        // ``mouseReleaseEvent`` re-enables full quality with an IDR.
        if (m_live) {
            QJsonObject cmd;
            cmd[QStringLiteral("type")]  = QStringLiteral("set_interactive");
            cmd[QStringLiteral("value")] = true;
            sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
        }
    }
    event->accept();
}

void RemoteRenderWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragActive || !m_live) {
        event->ignore();
        return;
    }
    const int dx = event->position().x() - m_lastMouseX;
    const int dy = event->position().y() - m_lastMouseY;
    m_lastMouseX = event->position().x();
    m_lastMouseY = event->position().y();

    if (dx == 0 && dy == 0) {
        return;
    }

    // Throttle to 60 Hz max. Qt fires mouseMoveEvent at the platform's
    // pointer poll rate — on a Retina trackpad that's often 120 Hz, and
    // a fast drag can hit 70-100 events/s. Sending each one as its own
    // ``rotate`` command floods the data channel and stacks tiny camera
    // updates ahead of the renderer, which has no benefit visually
    // (we already pace frames at 60 FPS) and just adds executor
    // contention with render_rgb_frame. Coalesce deltas into a
    // pending buffer and send at most once per ~16 ms.
    m_pendingAzimuthDelta   += -dx * kRotateSensitivity;
    m_pendingElevationDelta +=  dy * kRotateSensitivity;
    m_pendingPanDx          += dx * kPanSensitivity;
    m_pendingPanDy          += dy * kPanSensitivity;
    m_pendingPanButton       = bool(event->buttons() & Qt::MiddleButton);

    if (!m_dragSendTimer) {
        m_dragSendTimer = new QTimer(this);
        m_dragSendTimer->setSingleShot(true);
        connect(m_dragSendTimer, &QTimer::timeout, this,
                &RemoteRenderWidget::flushPendingDrag);
    }
    if (!m_dragSendTimer->isActive()) {
        m_dragSendTimer->start(16);   // ~60 Hz
    }
}

void RemoteRenderWidget::flushPendingDrag()
{
    if (!m_live) return;
    QJsonObject cmd;
    if (m_pendingPanButton) {
        if (m_pendingPanDx == 0.0 && m_pendingPanDy == 0.0) return;
        cmd[QStringLiteral("type")] = QStringLiteral("pan");
        cmd[QStringLiteral("dx")]   = m_pendingPanDx;
        cmd[QStringLiteral("dy")]   = m_pendingPanDy;
    } else {
        if (m_pendingAzimuthDelta == 0.0 && m_pendingElevationDelta == 0.0) return;
        cmd[QStringLiteral("type")]        = QStringLiteral("rotate");
        cmd[QStringLiteral("d_azimuth")]   = m_pendingAzimuthDelta;
        cmd[QStringLiteral("d_elevation")] = m_pendingElevationDelta;
    }
    m_pendingAzimuthDelta   = 0.0;
    m_pendingElevationDelta = 0.0;
    m_pendingPanDx          = 0.0;
    m_pendingPanDy          = 0.0;
    sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
    requestFrame();
}

void RemoteRenderWidget::mouseReleaseEvent(QMouseEvent *event)
{
    const bool wasDragging = m_dragActive;
    m_dragActive = false;
    // Make sure any in-flight deltas reach the backend before the
    // wireframe LOD is turned off, so the final still-frame
    // orientation matches what the user let go on.
    if (wasDragging && m_dragSendTimer && m_dragSendTimer->isActive()) {
        m_dragSendTimer->stop();
        flushPendingDrag();
    }
    // Interactive LOD off — backend re-enables volume + cutting plane
    // and emits an IDR so the very first still frame is clean.
    if (wasDragging && m_live) {
        QJsonObject cmd;
        cmd[QStringLiteral("type")]  = QStringLiteral("set_interactive");
        cmd[QStringLiteral("value")] = false;
        sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
    }
    event->accept();
}

void RemoteRenderWidget::wheelEvent(QWheelEvent *event)
{
    if (!m_live) {
        event->ignore();
        return;
    }
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) {
        return;
    }
    const double factor = (notches > 0)
                                  ? std::pow(kZoomPerNotch, notches)
                                  : std::pow(1.0 / kZoomPerNotch, -notches);

    // Wheel-zoom LOD: there's no press/release pair to bracket a
    // wheel gesture, so we drive it on an inactivity timer. First
    // notch flips the backend into wireframe; every subsequent notch
    // restarts the timer; 250 ms of idle wheel = restore full quality
    // with an IDR. Same UX as Paraview / VTK's default interactor.
    if (!m_zoomLodActive) {
        m_zoomLodActive = true;
        QJsonObject lodOn;
        lodOn[QStringLiteral("type")]  = QStringLiteral("set_interactive");
        lodOn[QStringLiteral("value")] = true;
        sendCommand(QJsonDocument(lodOn).toJson(QJsonDocument::Compact));
    }
    if (!m_zoomLodIdleTimer) {
        m_zoomLodIdleTimer = new QTimer(this);
        m_zoomLodIdleTimer->setSingleShot(true);
        connect(m_zoomLodIdleTimer, &QTimer::timeout, this, [this]() {
            if (!m_zoomLodActive || !m_live) return;
            m_zoomLodActive = false;
            QJsonObject lodOff;
            lodOff[QStringLiteral("type")]  = QStringLiteral("set_interactive");
            lodOff[QStringLiteral("value")] = false;
            sendCommand(QJsonDocument(lodOff).toJson(QJsonDocument::Compact));
        });
    }
    m_zoomLodIdleTimer->start(250);

    QJsonObject cmd;
    cmd[QStringLiteral("type")]   = QStringLiteral("zoom");
    cmd[QStringLiteral("factor")] = factor;
    sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
    requestFrame();
    event->accept();
}

void RemoteRenderWidget::resizeEvent(QResizeEvent *event)
{
    // Send a resize command so the server renders at the new aspect /
    // resolution. We send PHYSICAL pixel dimensions (logical × DPR) so
    // the server renders at the native display resolution — on a 2×
    // Retina display, width()=1024 logical means we want a 2048-wide
    // frame back. Without this scale-up the JPEG arrives at 1024 and
    // gets up-sampled to 2048 by Qt → blurry compared to the local
    // QVTKOpenGLNativeWidget which always paints at native DPI.
    //
    // Debounce: during an interactive drag-resize Qt fires
    // ``resizeEvent`` for EVERY pixel of movement. Sending each one to
    // the backend triggers aiortc to rebuild the NVENC encoder context
    // (~300 ms each) because the frame size of the next ``VideoFrame``
    // differs from the codec's. We saw bursts of 8 NVENC re-opens
    // during a single drag, which is what caused the FPS to collapse
    // to single digits while resizing. Delay sending until the user
    // stops dragging for 250 ms.
    if (m_live) {
        if (!m_resizeDebounceTimer) {
            m_resizeDebounceTimer = new QTimer(this);
            m_resizeDebounceTimer->setSingleShot(true);
            connect(m_resizeDebounceTimer, &QTimer::timeout,
                    this, &RemoteRenderWidget::flushPendingResize);
        }
        m_resizeDebounceTimer->start(250);
    }
    QWidget::resizeEvent(event);
}

void RemoteRenderWidget::flushPendingResize()
{
    if (!m_live) return;
    const qreal dpr = devicePixelRatioF();
    const int physW = qRound(width()  * dpr);
    const int physH = qRound(height() * dpr);
    if (m_lastResizeSent.width() == physW
            && m_lastResizeSent.height() == physH) {
        return;
    }
    m_lastResizeSent = QSize(physW, physH);
    QJsonObject cmd;
    cmd[QStringLiteral("type")]   = QStringLiteral("resize");
    cmd[QStringLiteral("width")]  = physW;
    cmd[QStringLiteral("height")] = physH;
    sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
    requestFrame();
}

// ── Helpers ─────────────────────────────────────────────────────────────────

void RemoteRenderWidget::sendCommand(const QByteArray &jsonBytes)
{
#ifdef VISIVO_HAS_WEBRTC
    // WebRTC dispatch: mouse drag / wheel / resize event handlers call
    // sendCommand() directly with a serialised JSON byte array. Under
    // the WebRTC transport we re-parse the JSON back into a
    // QVariantMap (cheap — payloads are tiny) and forward through
    // the data channel. Same JSON shape, different transport.
    if (m_transport == Transport::H265WebRTC && m_webrtc) {
        const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
        qDebug().noquote() << QStringLiteral(
                "[remote-render] sendCommand (WebRTC) -> %1")
                .arg(QString::fromUtf8(jsonBytes));
        m_webrtc->sendStateCommand(doc.object().toVariantMap());
        return;
    }
#endif
    if (!m_ws || m_ws->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    m_ws->sendTextMessage(QString::fromUtf8(jsonBytes));
}

void RemoteRenderWidget::requestFrame()
{
#ifdef VISIVO_HAS_WEBRTC
    // Under WebRTC the server pushes frames continuously at a fixed
    // FPS — the request_frame poke that the JPEG path uses is not
    // needed and would be silently ignored by the server. No-op here
    // keeps the rest of the call-site code (mouse, wheel, resize) the
    // same across transports.
    if (m_transport == Transport::H265WebRTC) {
        return;
    }
#endif
    if (!m_ws || m_ws->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    if (m_frameInFlight) {
        // Don't pile up multiple in-flight requests — let the current
        // one come back first. The server reads the latest view state
        // when it renders, so the dropped request_frame doesn't lose
        // any user intent; only frame cadence.
        return;
    }
    m_frameInFlight = true;
    m_ws->sendTextMessage(
            QStringLiteral("{\"type\":\"request_frame\"}"));
}
