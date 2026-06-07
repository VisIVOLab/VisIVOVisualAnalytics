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
// VTK's vtkInteractorStyleTrackballCamera converts ~1° per drag pixel
// at default sensitivity and then scales by the renderer's
// MotionFactor (default 10) divided by the window height/width — a
// sensible empirical match for a 1024 px wide window is ~0.15 °/px.
// The previous 0.5 felt "stuck on a fast-spin axis" against the local
// VTK widget's feel.
constexpr double kRotateSensitivity = 0.15;
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

// ── WebSocket callbacks ─────────────────────────────────────────────────────

void RemoteRenderWidget::onWebSocketConnected()
{
    m_status = QStringLiteral("Connected, awaiting first frame…");
    emit connectionStateChanged(m_status);
    // Ask for an initial frame so the user sees the volume immediately.
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
        // Stretch the JPEG to widget size with smooth scaling.
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
    // resolution. Throttled implicitly because Qt only fires one
    // resizeEvent per geometry change.
    if (m_live) {
        QJsonObject cmd;
        cmd[QStringLiteral("type")]   = QStringLiteral("resize");
        cmd[QStringLiteral("width")]  = width();
        cmd[QStringLiteral("height")] = height();
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
