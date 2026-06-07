#include "RemoteRenderWidget.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
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

void RemoteRenderWidget::connectToSession(const QString &baseUrl,
                                          const QString &sessionId,
                                          const QString &token)
{
    disconnectFromSession();

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
        // The JPEG arrives at PHYSICAL pixel resolution (see resizeEvent
        // — we asked for width*DPR pixels). Tagging the QImage with the
        // same devicePixelRatio lets QPainter paint it 1:1 against the
        // physical framebuffer — no scaling, no blur. Painting straight
        // into ``rect()`` then lands every pixel at its native target.
        QImage frame = m_currentFrame;
        frame.setDevicePixelRatio(devicePixelRatioF());
        // Centre the (logical) frame in the widget, preserving aspect.
        const QSize logicalSize = frame.size() / frame.devicePixelRatio();
        const int x = (width()  - logicalSize.width())  / 2;
        const int y = (height() - logicalSize.height()) / 2;
        painter.drawImage(QPoint(x, y), frame);
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
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_dragActive = true;
        m_lastMouseX = event->position().x();
        m_lastMouseY = event->position().y();
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

    QJsonObject cmd;
    if (event->buttons() & Qt::LeftButton) {
        cmd[QStringLiteral("type")]         = QStringLiteral("rotate");
        cmd[QStringLiteral("d_azimuth")]    = -dx * kRotateSensitivity;
        cmd[QStringLiteral("d_elevation")]  =  dy * kRotateSensitivity;
    } else {  // Middle button → pan
        cmd[QStringLiteral("type")] = QStringLiteral("pan");
        cmd[QStringLiteral("dx")]   = dx * kPanSensitivity;
        cmd[QStringLiteral("dy")]   = dy * kPanSensitivity;
    }
    sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
    requestFrame();
}

void RemoteRenderWidget::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragActive = false;
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
    if (m_live) {
        const qreal dpr = devicePixelRatioF();
        QJsonObject cmd;
        cmd[QStringLiteral("type")]   = QStringLiteral("resize");
        cmd[QStringLiteral("width")]  = qRound(width()  * dpr);
        cmd[QStringLiteral("height")] = qRound(height() * dpr);
        sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
        requestFrame();
    }
    QWidget::resizeEvent(event);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

void RemoteRenderWidget::sendCommand(const QByteArray &jsonBytes)
{
    if (!m_ws || m_ws->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    m_ws->sendTextMessage(QString::fromUtf8(jsonBytes));
}

void RemoteRenderWidget::requestFrame()
{
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
