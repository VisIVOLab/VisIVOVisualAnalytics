#include "RaytraceViewport.h"

#include "OrbitPath.h"
#include "RaytraceSettingsPanel.h"
#include "app/BackendClient.h"
#include "app/BackendHeaders.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPixmap>
#include <QStackedLayout>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

RaytraceViewport::RaytraceViewport(Source source, QWidget *parent)
    : QWidget(parent), m_source(std::move(source)), m_nam(new QNetworkAccessManager(this))
{
    m_gl = new ParticleGLView(this);
    m_gl->setCursor(Qt::OpenHandCursor);
    m_gl->installEventFilter(this);
    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setStyleSheet(u"background: black;"_s);
    m_image->setMinimumSize(64, 64);
    m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_image->setCursor(Qt::OpenHandCursor);
    m_image->installEventFilter(this);
    m_pages = new QStackedLayout(this);
    m_pages->setContentsMargins(0, 0, 0, 0);
    m_pages->addWidget(m_gl);
    m_pages->addWidget(m_image);
    connect(m_gl, &ParticleGLView::failed, this, [this](const QString &reason) {
        m_failed = true;
        emit message(u"No GPU for the ray-traced view (%1): the backend draws it."_s.arg(reason));
        m_pages->setCurrentWidget(m_image);
        fallbackFrame();
    });
    // The statistics a picture is graded by, read when the view stops.
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    m_settle->setInterval(250);
    connect(m_settle, &QTimer::timeout, this, [this]() { redraw(true); });
}

double RaytraceViewport::autoExposure() const
{
    return m_gl->autoExposure();
}

void RaytraceViewport::setOverlayShown(bool box, bool colourbar)
{
    m_gl->setOverlayShown(box, colourbar);
}

void RaytraceViewport::showImage(const QImage &image)
{
    m_shown = image;
    // Fitted to the screen's pixels, shown at its points.
    const double dpr = devicePixelRatioF();
    QPixmap pix = QPixmap::fromImage(image).scaled(m_image->size() * dpr, Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation);
    pix.setDevicePixelRatio(dpr);
    m_image->setPixmap(pix);
    m_pages->setCurrentWidget(m_image);
}

void RaytraceViewport::showStill(const QImage &image)
{
    m_showingStill = true;
    showImage(image);
}

void RaytraceViewport::showGl()
{
    if (m_showingStill) {
        m_showingStill = false;
        emit stillDismissed();
    }
    if (!m_failed)
        m_pages->setCurrentWidget(m_gl);
}

void RaytraceViewport::fallbackFrame()
{
    if (!m_failed || m_camera.isEmpty() || !m_source.request || m_showingStill)
        return;
    if (m_fallbackReply) {   // one at a time, the latest next
        m_fallbackPending = true;
        return;
    }
    QJsonObject req = m_source.request();
    const int width = std::clamp(int(m_image->width() * devicePixelRatioF()), 160, 1280);
    const int height = std::clamp(width * std::max(1, m_image->height())
                                          / std::max(1, m_image->width()), 90, 2048);
    req.insert(u"width"_s, width);
    req.insert(u"height"_s, height);
    req.insert(u"cameras"_s, QJsonArray{ m_camera });
    if (m_source.toneJson)
        req.insert(u"tone"_s, m_source.toneJson(0.0));
    QUrl url(m_source.backendUrl);
    url.setPath(u"/v1/particle_render/preview"_s);
    QNetworkRequest nreq(url);
    BackendClient::applySessionHeaders(nreq, m_source.backendUrl,
                                       BackendClient(m_source.backendUrl, m_source.token).token(),
                                       m_source.sessionId);
    nreq.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    QNetworkReply *reply = m_nam->post(nreq, QJsonDocument(req).toJson(QJsonDocument::Compact));
    m_fallbackReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_fallbackReply.clear();
        if (reply->error() == QNetworkReply::NoError && !m_showingStill) {
            QImage image;
            image.loadFromData(reply->readAll());
            if (!image.isNull())
                showImage(image);
        }
        if (std::exchange(m_fallbackPending, false))
            fallbackFrame();
    });
}

void RaytraceViewport::setCamera(const QJsonObject &camera)
{
    m_camera = camera;
    redraw(true);
}

void RaytraceViewport::refresh()
{
    if (!m_source.request)
        return;
    if (m_failed) {
        fallbackFrame();
        return;
    }
    const QJsonObject request = m_source.request();
    const QString key = RaytraceSettingsPanel::particlesKey(request);
    if (key == m_key && m_gl->hasParticles()) {
        // Back to the settings on the GPU: a fetch for others is not wanted.
        if (m_reply) {
            m_reply->abort();
            m_loadingKey.clear();
        }
        redraw(true);
        return;
    }
    if (key != m_loadingKey)
        fetch(key, request);
}

void RaytraceViewport::retone()
{
    redraw(true);
}

void RaytraceViewport::redraw(bool settle)
{
    if (m_failed) {
        if (settle)
            fallbackFrame();
        return;
    }
    if (!m_gl->hasParticles() || m_camera.isEmpty())
        return;
    const double aspect = double(std::max(1, width())) / std::max(1, height());
    m_gl->setImageAspect(aspect);
    if (m_source.tone)
        m_gl->setTone(m_source.tone());
    m_gl->setCamera(m_camera);
    if (settle)
        m_gl->settle();
}

void RaytraceViewport::fetch(const QString &key, const QJsonObject &request)
{
    if (m_reply)
        m_reply->abort();
    m_loadingKey = key;
    QUrl url(m_source.backendUrl);
    url.setPath(u"/v1/particle_render/particles"_s);
    QNetworkRequest req(url);
    // The token BackendClient resolves: with the local backend the window may
    // hold none, and it is in ~/.visivo_token.
    BackendClient::applySessionHeaders(req, m_source.backendUrl,
                                       BackendClient(m_source.backendUrl, m_source.token).token(),
                                       m_source.sessionId);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    QNetworkReply *reply = m_nam->post(req, QJsonDocument(request).toJson(QJsonDocument::Compact));
    m_reply = reply;
    emit message(u"Ray-traced view: preparing the particles…"_s);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
        reply->deleteLater();
        if (m_reply == reply)
            m_reply.clear();
        if (m_loadingKey == key)
            m_loadingKey.clear();
        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError) {
                const QString detail = QJsonDocument::fromJson(reply->readAll())
                                               .object().value(u"detail"_s).toString();
                emit message(u"Ray-traced view: "_s
                             + (detail.isEmpty() ? reply->errorString() : detail));
            }
            return;
        }
        const QByteArray rows = reply->readAll();
        const int count = reply->rawHeader(VisivoHeaders::PARTICLE_COUNT).toInt();
        const bool map = reply->rawHeader(VisivoHeaders::PARTICLE_FIELDS).endsWith(",v");
        const int step = std::max(1, reply->rawHeader(VisivoHeaders::PARTICLE_STEP).toInt());
        if (count <= 0 || rows.size() != qsizetype(count) * (map ? 6 : 8) * 4)
            return;
        m_gl->setParticles(rows, count, map ? ParticleGLView::Mode::Projected
                                             : ParticleGLView::Mode::Light);
        m_key = key;
        if (!m_showingStill)
            m_pages->setCurrentWidget(m_gl);
        redraw(true);
        QString note = u"Ray-traced view: %1 particles on the GPU"_s.arg(QLocale().toString(count));
        if (step > 1)
            note += u", one in %1 of the table carrying their light"_s.arg(step);
        emit message(note);
    });
}

bool RaytraceViewport::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched != m_gl && watched != m_image) || m_camera.isEmpty())
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::Resize && watched == m_image && !m_shown.isNull())
        showImage(m_shown);   // fitted again
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        // A still on show: put away, the view comes back to be turned.
        showGl();
        m_dragFrom = static_cast<QMouseEvent *>(event)->position().toPoint();
        m_gl->setCursor(Qt::ClosedHandCursor);
        return true;
    case QEvent::MouseMove: {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->buttons() == Qt::NoButton)
            return true;
        const QPoint d = e->position().toPoint() - m_dragFrom;
        m_dragFrom = e->position().toPoint();
        const double h = std::max(1, m_gl->height());
        if ((e->buttons() & Qt::LeftButton) && !(e->modifiers() & Qt::ShiftModifier))
            m_camera = visivo::turnCamera(m_camera, -d.x() * 0.4, d.y() * 0.4);
        else
            m_camera = visivo::panCamera(m_camera, d.x() / h, d.y() / h);
        redraw(false);
        m_settle->start();
        return true;
    }
    case QEvent::MouseButtonRelease:
        m_gl->setCursor(Qt::OpenHandCursor);
        m_settle->start();
        return true;
    case QEvent::Wheel: {
        showGl();
        // A wheel notch (120) is about ×1.2; a burst is capped.
        const double factor = std::clamp(
                std::pow(1.0015, static_cast<QWheelEvent *>(event)->angleDelta().y()), 0.8, 1.25);
        m_camera = visivo::dollyCamera(m_camera, factor);
        redraw(false);
        m_settle->start();
        return true;
    }
    default:
        return QWidget::eventFilter(watched, event);
    }
}
