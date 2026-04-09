#include "HiPSViewportWidget.h"

#include <QFutureWatcher>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>
#include <QWheelEvent>
#include <QtConcurrent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

// ── Anonymous helpers ─────────────────────────────────────────────────────────
namespace {

// Optimal HEALPix order for the given viewport:
// We want tiles that project to roughly 256–512 screen pixels wide.
// At order N, each tile covers ≈ 90 / 2^N degrees.
int optimalOrder(double fovDeg, int viewportWidth, int maxOrder)
{
    if (viewportWidth <= 0 || fovDeg <= 0.0)
        return 3;
    // Desired angular tile size in degrees (target: ~512 px per tile)
    const double targetTileAngleDeg = fovDeg * 512.0 / viewportWidth;
    // At order N: tile ≈ 90 / 2^N degrees ⟹ N = log2(90/tile)
    const int order = static_cast<int>(std::log2(90.0 / std::max(0.01, targetTileAngleDeg)));
    return std::clamp(order, 0, maxOrder);
}

// Normalise RA to [0, 360).
double normaliseRa(double ra)
{
    ra = std::fmod(ra, 360.0);
    if (ra < 0.0) ra += 360.0;
    return ra;
}

// Format RA as "HHh MMm SS.Ss"
QString formatRa(double raDeg)
{
    const double h = raDeg / 15.0;
    const int    H = static_cast<int>(h);
    const double m = (h - H) * 60.0;
    const int    M = static_cast<int>(m);
    const double s = (m - M) * 60.0;
    return QStringLiteral("%1h %2m %3s")
            .arg(H, 2, 10, QLatin1Char('0'))
            .arg(M, 2, 10, QLatin1Char('0'))
            .arg(s, 4, 'f', 1, QLatin1Char('0'));
}

// Format Dec as "±DD° MM' SS""
QString formatDec(double decDeg)
{
    const char sign = decDeg < 0 ? '-' : '+';
    const double abs = std::abs(decDeg);
    const int    D = static_cast<int>(abs);
    const double m = (abs - D) * 60.0;
    const int    M = static_cast<int>(m);
    const double s = (m - M) * 60.0;
    return QStringLiteral("%1%2° %3' %4\"")
            .arg(sign)
            .arg(D, 2, 10, QLatin1Char('0'))
            .arg(M, 2, 10, QLatin1Char('0'))
            .arg(s, 4, 'f', 1, QLatin1Char('0'));
}

} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

HiPSViewportWidget::HiPSViewportWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(400, 300);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(u"background-color: #000010;"_s);

    // Debounce timer: fires 250 ms after the last view change to request tiles.
    m_viewChangeTimer = new QTimer(this);
    m_viewChangeTimer->setSingleShot(true);
    m_viewChangeTimer->setInterval(250);
    QObject::connect(m_viewChangeTimer, &QTimer::timeout,
                     this, &HiPSViewportWidget::onViewChanged);

    // AllSky watcher: fires when the AllSky image download completes.
    QObject::connect(&m_allskyWatcher, &QFutureWatcher<QByteArray>::finished, this, [this]() {
        m_allskyLoading = false;
        const QByteArray data = m_allskyWatcher.result();
        if (!data.isEmpty()) {
            QPixmap pm;
            pm.loadFromData(data);
            if (!pm.isNull()) {
                m_allskyPixmap = pm;
                update();
            }
        }
        emit loadingChanged(!m_pendingKeys.isEmpty());
    });

    // Tile-list watcher: fires when query_tiles returns.
    QObject::connect(&m_tileListWatcher, &QFutureWatcher<BackendHiPSViewResponse>::finished,
                     this, [this]() {
                         const auto resp = m_tileListWatcher.result();
                         m_currentTiles.clear();
                         if (resp.valid) {
                             m_currentTiles.reserve(resp.tiles.size());
                             for (const auto &t : resp.tiles) {
                                 HiPSTileDescriptor td;
                                 td.key           = { t.order, t.pix };
                                 td.raCenterDeg   = t.raCenterDeg;
                                 td.decCenterDeg  = t.decCenterDeg;
                                 td.cornersRaDeg  = t.cornersRaDeg;
                                 td.cornersDecDeg = t.cornersDecDeg;
                                 m_currentTiles.push_back(td);
                             }
                         }
                         update();
                         emit orderChanged(currentOrder());
                     });
}

HiPSViewportWidget::~HiPSViewportWidget()
{
    // Cancel any in-flight watchers so they don't fire after destruction.
    if (m_allskyWatcher.isRunning())
        m_allskyWatcher.cancel();
    if (m_tileListWatcher.isRunning())
        m_tileListWatcher.cancel();
    for (auto *w : m_activeWatchers) {
        w->cancel();
        w->deleteLater();
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void HiPSViewportWidget::setSurvey(const BackendHiPSSurveyInfo &survey,
                                   const QString &backendUrl, const QString &backendToken)
{
    m_survey      = survey;
    m_backendUrl  = backendUrl;
    m_backendToken = backendToken;
    m_hasSurvey   = true;

    // Reset to a sensible starting view and clear state.
    m_view        = {};
    m_tileCache.clear();
    m_pendingKeys.clear();
    m_fetchQueue.clear();
    m_currentTiles.clear();
    m_allskyPixmap = {};

    update();

    // Kick off AllSky download for the initial overview (order 3 or lower).
    requestAllsky();
    // Also request the tile list for the initial view.
    m_viewChangeTimer->start();
}

void HiPSViewportWidget::clearSurvey()
{
    m_hasSurvey = false;
    m_tileCache.clear();
    m_pendingKeys.clear();
    m_fetchQueue.clear();
    m_currentTiles.clear();
    m_allskyPixmap = {};
    update();
}

void HiPSViewportWidget::setCenter(double raDeg, double decDeg)
{
    m_view.raDeg  = normaliseRa(raDeg);
    m_view.decDeg = std::clamp(decDeg, -89.9, 89.9);
    update();
    m_viewChangeTimer->start();
    emit centerChanged(m_view.raDeg, m_view.decDeg);
}

void HiPSViewportWidget::setFov(double fovDeg)
{
    m_view.fovDeg = std::clamp(fovDeg, 0.01, 180.0);
    update();
    m_viewChangeTimer->start();
    emit fovChanged(m_view.fovDeg);
}

void HiPSViewportWidget::resetView()
{
    m_view = {};
    m_tileCache.clear();
    m_pendingKeys.clear();
    m_fetchQueue.clear();
    m_currentTiles.clear();
    update();
    m_viewChangeTimer->start();
    emit centerChanged(m_view.raDeg, m_view.decDeg);
    emit fovChanged(m_view.fovDeg);
}

int HiPSViewportWidget::currentOrder() const
{
    if (!m_hasSurvey) return 0;
    return optimalOrder(m_view.fovDeg, width(), m_survey.maxOrder);
}

// ── Projection ────────────────────────────────────────────────────────────────

double HiPSViewportWidget::scalePixPerRad() const
{
    return width() / qDegreesToRadians(m_view.fovDeg);
}

QPointF HiPSViewportWidget::project(double raDeg, double decDeg) const
{
    const double ra0  = qDegreesToRadians(m_view.raDeg);
    const double dec0 = qDegreesToRadians(m_view.decDeg);
    const double ra1  = qDegreesToRadians(raDeg);
    const double dec1 = qDegreesToRadians(decDeg);

    // Gnomonic (tangential) projection.
    const double cos_c = std::sin(dec0) * std::sin(dec1)
                       + std::cos(dec0) * std::cos(dec1) * std::cos(ra1 - ra0);
    if (cos_c <= 0.0) {
        // Point is on or behind the projection plane — off-screen sentinel.
        return { -1e7, -1e7 };
    }

    const double x = std::cos(dec1) * std::sin(ra1 - ra0) / cos_c;
    const double y = (std::cos(dec0) * std::sin(dec1)
                      - std::sin(dec0) * std::cos(dec1) * std::cos(ra1 - ra0)) / cos_c;

    const double scale = scalePixPerRad();
    // Screen y is flipped relative to celestial y (Dec increases upward).
    return { width()  * 0.5 + x * scale,
             height() * 0.5 - y * scale };
}

void HiPSViewportWidget::unproject(QPointF screen, double &raDeg, double &decDeg) const
{
    const double ra0  = qDegreesToRadians(m_view.raDeg);
    const double dec0 = qDegreesToRadians(m_view.decDeg);
    const double scale = scalePixPerRad();

    const double x = (screen.x() - width()  * 0.5) / scale;
    const double y = -(screen.y() - height() * 0.5) / scale; // un-flip

    const double rho = std::sqrt(x * x + y * y);
    if (rho < 1e-12) {
        raDeg  = m_view.raDeg;
        decDeg = m_view.decDeg;
        return;
    }

    const double c    = std::atan(rho);
    const double dec1 = std::asin(std::cos(c) * std::sin(dec0)
                                   + y * std::sin(c) * std::cos(dec0) / rho);
    const double ra1  = ra0 + std::atan2(x * std::sin(c),
                                          rho * std::cos(dec0) * std::cos(c)
                                          - y * std::sin(dec0) * std::sin(c));
    raDeg  = normaliseRa(qRadiansToDegrees(ra1));
    decDeg = std::clamp(qRadiansToDegrees(dec1), -90.0, 90.0);
}

// ── Tile management ───────────────────────────────────────────────────────────

void HiPSViewportWidget::onViewChanged()
{
    if (!m_hasSurvey) return;

    // If FOV is very wide, the AllSky is good enough — skip individual tiles.
    if (m_view.fovDeg > 90.0) {
        if (m_allskyPixmap.isNull() && !m_allskyLoading) {
            requestAllsky();
        }
        return;
    }
    requestTileList();
}

void HiPSViewportWidget::requestTileList()
{
    if (m_tileListWatcher.isRunning()) return;

    const QString backendUrl  = m_backendUrl;
    const QString backendToken = m_backendToken;
    const QString surveyId    = m_survey.surveyId;
    const QString sessionId   = m_survey.sessionId;
    const double  raDeg       = m_view.raDeg;
    const double  decDeg      = m_view.decDeg;
    const double  fovDeg      = m_view.fovDeg;
    const int     order       = optimalOrder(fovDeg, width(), m_survey.maxOrder);

    m_tileListWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, surveyId, sessionId, raDeg, decDeg, fovDeg, order]()
                    -> BackendHiPSViewResponse {
                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty())
                    client.setSessionId(sessionId);
                return client.requestHiPSTilesForView(surveyId, raDeg, decDeg, fovDeg, order);
            }));
}

void HiPSViewportWidget::requestAllsky()
{
    if (m_allskyLoading || !m_hasSurvey) return;
    m_allskyLoading = true;
    emit loadingChanged(true);

    const QString backendUrl  = m_backendUrl;
    const QString backendToken = m_backendToken;
    const QString surveyId    = m_survey.surveyId;
    const QString sessionId   = m_survey.sessionId;
    // Use a low order for AllSky (order 3 = 192 tiles, manageable mosaic).
    const int order = std::min(3, m_survey.maxOrder);

    m_allskyWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, surveyId, sessionId, order]() -> QByteArray {
                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty())
                    client.setSessionId(sessionId);
                return client.requestHiPSAllsky(surveyId, order);
            }));
}

void HiPSViewportWidget::scheduleTileFetch(const HiPSTileKey &key)
{
    if (m_pendingKeys.contains(key) || m_tileCache.contains(key))
        return;
    m_pendingKeys.insert(key);
    m_fetchQueue.append(key);
    drainFetchQueue();
}

void HiPSViewportWidget::drainFetchQueue()
{
    while (m_activeWatchers.size() < kMaxConcurrent && !m_fetchQueue.isEmpty()) {
        const HiPSTileKey key = m_fetchQueue.takeFirst();

        const QString backendUrl  = m_backendUrl;
        const QString backendToken = m_backendToken;
        const QString surveyId    = m_survey.surveyId;
        const QString sessionId   = m_survey.sessionId;
        const int order = key.order;
        const int pix   = key.pix;

        auto *watcher = new QFutureWatcher<HiPSTileFetchResult>(this);
        m_activeWatchers.append(watcher);

        QObject::connect(watcher, &QFutureWatcher<HiPSTileFetchResult>::finished, this,
                         [this, watcher, key]() {
                             m_activeWatchers.removeOne(watcher);
                             m_pendingKeys.remove(key);
                             watcher->deleteLater();

                             const auto result = watcher->result();
                             if (result.error.isEmpty() && !result.imageData.isEmpty()) {
                                 QPixmap pm;
                                 pm.loadFromData(result.imageData);
                                 if (!pm.isNull()) {
                                     m_tileCache.insert(key, pm);
                                 }
                             }
                             drainFetchQueue();
                             update();
                             emit loadingChanged(m_allskyLoading || !m_pendingKeys.isEmpty());
                         });

        watcher->setFuture(QtConcurrent::run(
                [backendUrl, backendToken, surveyId, sessionId, order, pix, key]()
                        -> HiPSTileFetchResult {
                    BackendClient client(backendUrl, backendToken);
                    if (!sessionId.isEmpty())
                        client.setSessionId(sessionId);
                    const QByteArray data = client.requestHiPSTile(surveyId, order, pix);
                    HiPSTileFetchResult r;
                    r.key       = key;
                    r.imageData = data;
                    return r;
                }));
    }
}

// ── Rendering ─────────────────────────────────────────────────────────────────

void HiPSViewportWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::Antialiasing, true);

    renderBackground(p);

    if (!m_hasSurvey) {
        renderNoSurvey(p);
        return;
    }

    renderAllsky(p);
    renderTiles(p);
    renderCrosshair(p);
}

void HiPSViewportWidget::renderBackground(QPainter &p) const
{
    p.fillRect(rect(), QColor(0, 0, 16)); // Deep space black-blue
}

void HiPSViewportWidget::renderNoSurvey(QPainter &p) const
{
    p.setPen(QColor(100, 120, 140));
    p.setFont(QFont(u"Lato"_s, 14));
    p.drawText(rect(), Qt::AlignCenter,
               u"No HiPS survey loaded.\nUse File → Open HiPS Survey to begin."_s);
}

void HiPSViewportWidget::renderAllsky(QPainter &p) const
{
    if (m_allskyPixmap.isNull()) return;

    // The AllSky image is a full-sky map.  For FOV > 90° we show it
    // as a simple scaled image spanning the widget (approximate, no accurate
    // projection — this is the "zoom-out" overview mode).
    if (m_view.fovDeg < 90.0) return; // tile renderer takes over at lower FOV

    // Scale the AllSky to fill the widget, preserving aspect ratio.
    const QPixmap scaled = m_allskyPixmap.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                                  Qt::SmoothTransformation);
    const int ox = (width()  - scaled.width())  / 2;
    const int oy = (height() - scaled.height()) / 2;
    p.setOpacity(0.85);
    p.drawPixmap(ox, oy, scaled);
    p.setOpacity(1.0);
}

void HiPSViewportWidget::renderTiles(QPainter &p)
{
    if (m_currentTiles.empty()) return;

    for (auto &td : m_currentTiles) {
        const HiPSTileKey &key = td.key;

        if (!m_tileCache.contains(key)) {
            // Placeholder: dark tile at approximate screen position.
            const QPointF centre = project(td.raCenterDeg, td.decCenterDeg);
            if (centre.x() < -width() * 2 || centre.x() > width() * 3) continue;

            // Approximate tile angular size at this order.
            const double tileAngDeg = 90.0 / (1 << key.order);
            const int sz = static_cast<int>(scalePixPerRad()
                                            * qDegreesToRadians(tileAngDeg) * 0.92);
            if (sz < 2) continue;

            p.setPen(QPen(QColor(40, 60, 80), 1));
            p.setBrush(QColor(10, 20, 30));
            p.drawRect(QRectF(centre.x() - sz * 0.5, centre.y() - sz * 0.5, sz, sz));

            scheduleTileFetch(key);
            continue;
        }

        const QPixmap &pm = m_tileCache[key];

        // Project the 4 HEALPix tile corners to screen space.
        QPolygonF dst;
        dst.reserve(4);
        bool anyVisible = false;
        for (int i = 0; i < 4; ++i) {
            const QPointF sp = project(td.cornersRaDeg[i], td.cornersDecDeg[i]);
            dst.append(sp);
            if (sp.x() > -width() && sp.x() < 2 * width()
                    && sp.y() > -height() && sp.y() < 2 * height()) {
                anyVisible = true;
            }
        }
        if (!anyVisible) continue;

        // Source rectangle: the tile image is a square bitmap.
        const qreal W = pm.width();
        const qreal H = pm.height();
        QPolygonF src;
        src << QPointF(0, 0) << QPointF(W, 0) << QPointF(W, H) << QPointF(0, H);

        // Map tile image (rectangle) → screen quadrilateral.
        QTransform t;
        if (QTransform::quadToQuad(src, dst, t)) {
            p.save();
            p.setTransform(t, true); // combine with existing transform
            p.drawPixmap(0, 0, pm);
            p.restore();
        } else {
            // Fallback: draw at centre point.
            const QPointF ctr = project(td.raCenterDeg, td.decCenterDeg);
            p.drawPixmap(ctr.toPoint() - QPoint(pm.width() / 2, pm.height() / 2), pm);
        }
    }
}

void HiPSViewportWidget::renderCrosshair(QPainter &p) const
{
    const int cx = width()  / 2;
    const int cy = height() / 2;
    const int r  = 12;
    const QPen crossPen(QColor(255, 220, 0, 180), 1.5);
    p.setPen(crossPen);
    p.drawLine(cx - r, cy, cx - 3, cy);
    p.drawLine(cx + 3, cy, cx + r, cy);
    p.drawLine(cx, cy - r, cx, cy - 3);
    p.drawLine(cx, cy + 3, cx, cy + r);

    // Tick circle
    p.drawEllipse(QPointF(cx, cy), 3.0, 3.0);
}

// ── Event handlers ────────────────────────────────────────────────────────────

void HiPSViewportWidget::resizeEvent(QResizeEvent *)
{
    if (m_hasSurvey) {
        m_viewChangeTimer->start();
    }
}

void HiPSViewportWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_panning      = true;
        m_panStartPos  = e->position();
        m_panStartView = m_view;
        setCursor(Qt::ClosedHandCursor);
    }
}

void HiPSViewportWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_panning) {
        // Linear approximation: convert pixel delta → angular delta.
        const double scale = scalePixPerRad();
        const double dx = e->position().x() - m_panStartPos.x();
        const double dy = e->position().y() - m_panStartPos.y();

        // RA increases to the left on the sky; divide by cos(Dec) for great-
        // circle correction.
        const double cosDec = std::cos(qDegreesToRadians(m_panStartView.decDeg));
        const double dRa  = -qRadiansToDegrees(dx / scale) / std::max(0.01, cosDec);
        const double dDec =  qRadiansToDegrees(dy / scale);

        m_view.raDeg  = normaliseRa(m_panStartView.raDeg + dRa);
        m_view.decDeg = std::clamp(m_panStartView.decDeg + dDec, -89.9, 89.9);

        update();
        emit centerChanged(m_view.raDeg, m_view.decDeg);
        // Debounced tile refresh after pan settles.
        m_viewChangeTimer->start();
    }

    // Always emit cursor coordinates.
    double ra, dec;
    unproject(e->position(), ra, dec);
    emit cursorCoordinatesChanged(ra, dec);
}

void HiPSViewportWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_panning) {
        m_panning = false;
        setCursor(Qt::CrossCursor);
    }
}

void HiPSViewportWidget::wheelEvent(QWheelEvent *e)
{
    // Zoom around the cursor position (not the centre).
    const double factor = (e->angleDelta().y() > 0) ? 0.75 : 1.333;
    const double newFov = std::clamp(m_view.fovDeg * factor, 0.01, 180.0);
    if (qFuzzyCompare(newFov, m_view.fovDeg)) return;
    m_view.fovDeg = newFov;
    update();
    m_viewChangeTimer->start();
    emit fovChanged(m_view.fovDeg);
    emit orderChanged(currentOrder());
}

void HiPSViewportWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Double-click to centre on the clicked sky position.
    double ra, dec;
    unproject(e->position(), ra, dec);
    m_view.raDeg  = ra;
    m_view.decDeg = dec;
    update();
    m_viewChangeTimer->start();
    emit centerChanged(m_view.raDeg, m_view.decDeg);
}
