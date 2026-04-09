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

int optimalOrder(double fovDeg, int viewportWidth, int maxOrder)
{
    if (viewportWidth <= 0 || fovDeg <= 0.0)
        return 3;
    const double targetTileAngleDeg = fovDeg * 512.0 / viewportWidth;
    const int order = static_cast<int>(std::log2(90.0 / std::max(0.01, targetTileAngleDeg)));
    return std::clamp(order, 0, maxOrder);
}

double normaliseRa(double ra)
{
    ra = std::fmod(ra, 360.0);
    if (ra < 0.0) ra += 360.0;
    return ra;
}

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

// Task 1: adaptive grid spacing
// Aim for 5–9 grid lines across the FOV; returns degrees.
double gridSpacingDeg(double fovDeg)
{
    static const double kSpacings[] = {
        60.0, 30.0, 20.0, 15.0, 10.0, 5.0, 2.0, 1.0,
        0.5, 1.0/6.0, 1.0/12.0, 1.0/30.0, 1.0/60.0 // 30', 10', 5', 2', 1'
    };
    for (double s : kSpacings) {
        if (fovDeg / s <= 9.0 && fovDeg / s >= 3.0)
            return s;
    }
    return 1.0 / 60.0; // 1 arcminute floor
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

    // ── View-change debounce (primary + secondary) ────────────────────────
    m_viewChangeTimer = new QTimer(this);
    m_viewChangeTimer->setSingleShot(true);
    m_viewChangeTimer->setInterval(250);
    QObject::connect(m_viewChangeTimer, &QTimer::timeout,
                     this, &HiPSViewportWidget::onViewChanged);

    // ── Catalogue refresh debounce (Task 3) ───────────────────────────────
    m_catalogueRefreshTimer = new QTimer(this);
    m_catalogueRefreshTimer->setSingleShot(true);
    m_catalogueRefreshTimer->setInterval(600);
    QObject::connect(m_catalogueRefreshTimer, &QTimer::timeout, this, [this]() {
        if (m_cataloguePath.isEmpty() || m_catalogueWatcher.isRunning()) return;
        const QString path      = m_cataloguePath;
        const QString raField   = m_catalogueRaField;
        const QString decField  = m_catalogueDecField;
        const QString url       = m_backendUrl;
        const QString token     = m_backendToken;
        const double  raDeg     = m_view.raDeg;
        const double  decDeg    = m_view.decDeg;
        const double  fovDeg    = m_view.fovDeg;
        m_catalogueWatcher.setFuture(QtConcurrent::run(
                [url, token, path, raDeg, decDeg, fovDeg, raField, decField]()
                        -> BackendHiPSCatalogueOverlayResult {
                    BackendClient client(url, token);
                    return client.requestHiPSCatalogueOverlay(
                            path, raDeg, decDeg, fovDeg, raField, decField);
                }));
    });

    // ── Allsky watcher (primary) ──────────────────────────────────────────
    QObject::connect(&m_allskyWatcher, &QFutureWatcher<QByteArray>::finished, this, [this]() {
        m_allskyLoading = false;
        const QByteArray data = m_allskyWatcher.result();
        if (!data.isEmpty()) {
            QPixmap pm;
            if (pm.loadFromData(data)) {
                m_allskyPixmap = pm;
                update();
            }
        }
        emit loadingChanged(m_allskyLoading || m_allskyLoadingSec
                            || !m_pendingKeys.isEmpty() || !m_pendingKeysSec.isEmpty());
    });

    // ── Tile-list watcher (primary) ───────────────────────────────────────
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

    // ── Allsky watcher (secondary — Task 2) ───────────────────────────────
    QObject::connect(&m_allskyWatcherSec, &QFutureWatcher<QByteArray>::finished, this, [this]() {
        m_allskyLoadingSec = false;
        const QByteArray data = m_allskyWatcherSec.result();
        if (!data.isEmpty()) {
            QPixmap pm;
            if (pm.loadFromData(data)) {
                m_allskyPixmapSec = pm;
                update();
            }
        }
        emit loadingChanged(m_allskyLoading || m_allskyLoadingSec
                            || !m_pendingKeys.isEmpty() || !m_pendingKeysSec.isEmpty());
    });

    // ── Tile-list watcher (secondary — Task 2) ────────────────────────────
    QObject::connect(&m_tileListWatcherSec, &QFutureWatcher<BackendHiPSViewResponse>::finished,
                     this, [this]() {
                         const auto resp = m_tileListWatcherSec.result();
                         m_currentTilesSec.clear();
                         if (resp.valid) {
                             m_currentTilesSec.reserve(resp.tiles.size());
                             for (const auto &t : resp.tiles) {
                                 HiPSTileDescriptor td;
                                 td.key           = { t.order, t.pix };
                                 td.raCenterDeg   = t.raCenterDeg;
                                 td.decCenterDeg  = t.decCenterDeg;
                                 td.cornersRaDeg  = t.cornersRaDeg;
                                 td.cornersDecDeg = t.cornersDecDeg;
                                 m_currentTilesSec.push_back(td);
                             }
                         }
                         update();
                     });

    // ── Catalogue watcher (Task 3) ────────────────────────────────────────
    QObject::connect(&m_catalogueWatcher,
                     &QFutureWatcher<BackendHiPSCatalogueOverlayResult>::finished,
                     this, [this]() {
                         const auto r = m_catalogueWatcher.result();
                         if (r.valid) {
                             m_catalogueSources = r.sources;
                             emit catalogueOverlayRefreshed(r.totalInView, r.truncated);
                             update();
                         }
                     });
}

HiPSViewportWidget::~HiPSViewportWidget()
{
    if (m_allskyWatcher.isRunning())    m_allskyWatcher.cancel();
    if (m_tileListWatcher.isRunning())  m_tileListWatcher.cancel();
    if (m_allskyWatcherSec.isRunning()) m_allskyWatcherSec.cancel();
    if (m_tileListWatcherSec.isRunning()) m_tileListWatcherSec.cancel();
    if (m_catalogueWatcher.isRunning()) m_catalogueWatcher.cancel();
    for (auto *w : m_activeWatchers) { w->cancel(); w->deleteLater(); }
    for (auto *w : m_activeWatchersSec) { w->cancel(); w->deleteLater(); }
}

// ── Public API ────────────────────────────────────────────────────────────────

void HiPSViewportWidget::setSurvey(const BackendHiPSSurveyInfo &survey,
                                   const QString &backendUrl, const QString &backendToken)
{
    m_survey       = survey;
    m_backendUrl   = backendUrl;
    m_backendToken = backendToken;
    m_hasSurvey    = true;

    m_view = {};
    m_tileCache.clear();
    m_pendingKeys.clear();
    m_fetchQueue.clear();
    m_currentTiles.clear();
    m_allskyPixmap = {};

    update();
    requestAllsky();
    m_viewChangeTimer->start();
    m_catalogueRefreshTimer->start();
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
    m_catalogueRefreshTimer->start();
    emit centerChanged(m_view.raDeg, m_view.decDeg);
}

void HiPSViewportWidget::setFov(double fovDeg)
{
    m_view.fovDeg = std::clamp(fovDeg, 0.01, 180.0);
    update();
    m_viewChangeTimer->start();
    m_catalogueRefreshTimer->start();
    emit fovChanged(m_view.fovDeg);
}

void HiPSViewportWidget::resetView()
{
    m_view = {};
    m_tileCache.clear();
    m_pendingKeys.clear();
    m_fetchQueue.clear();
    m_currentTiles.clear();
    m_tileCacheSec.clear();
    m_pendingKeysSec.clear();
    m_fetchQueueSec.clear();
    m_currentTilesSec.clear();
    update();
    m_viewChangeTimer->start();
    m_catalogueRefreshTimer->start();
    emit centerChanged(m_view.raDeg, m_view.decDeg);
    emit fovChanged(m_view.fovDeg);
}

int HiPSViewportWidget::currentOrder() const
{
    if (!m_hasSurvey) return 0;
    return optimalOrder(m_view.fovDeg, width(), m_survey.maxOrder);
}

// ── Task 1 — Grid ─────────────────────────────────────────────────────────────

void HiPSViewportWidget::setGridVisible(bool visible)
{
    if (m_showGrid == visible) return;
    m_showGrid = visible;
    update();
}

// ── Task 2 — Secondary survey ─────────────────────────────────────────────────

void HiPSViewportWidget::setSecondarySurvey(const BackendHiPSSurveyInfo &survey,
                                             const QString &backendUrl,
                                             const QString &backendToken)
{
    m_surveySec      = survey;
    m_backendUrlSec  = backendUrl;
    m_backendTokenSec = backendToken;
    m_hasSurveySec   = true;

    m_tileCacheSec.clear();
    m_pendingKeysSec.clear();
    m_fetchQueueSec.clear();
    m_currentTilesSec.clear();
    m_allskyPixmapSec = {};

    update();
    requestAllskySec();
    m_viewChangeTimer->start();
}

void HiPSViewportWidget::clearSecondarySurvey()
{
    m_hasSurveySec = false;
    for (auto *w : m_activeWatchersSec) { w->cancel(); w->deleteLater(); }
    m_activeWatchersSec.clear();
    m_tileCacheSec.clear();
    m_pendingKeysSec.clear();
    m_fetchQueueSec.clear();
    m_currentTilesSec.clear();
    m_allskyPixmapSec = {};
    update();
}

void HiPSViewportWidget::setOverlayOpacity(double opacity)
{
    m_overlayOpacity = std::clamp(opacity, 0.0, 1.0);
    update();
}

void HiPSViewportWidget::setBlendMode(BlendMode mode)
{
    if (m_blendMode == mode) return;
    m_blendMode = mode;
    update();
}

// ── Task 3 — Catalogue overlay ────────────────────────────────────────────────

void HiPSViewportWidget::setCatalogueOverlay(const QString &cataloguePath,
                                              const QString &raField,
                                              const QString &decField)
{
    m_cataloguePath    = cataloguePath;
    m_catalogueRaField  = raField;
    m_catalogueDecField = decField;
    m_catalogueSources.clear();
    m_catalogueRefreshTimer->start();
    update();
}

void HiPSViewportWidget::clearCatalogueOverlay()
{
    m_cataloguePath.clear();
    m_catalogueSources.clear();
    if (m_catalogueWatcher.isRunning()) m_catalogueWatcher.cancel();
    update();
}

void HiPSViewportWidget::setCatalogueOverlayVisible(bool visible)
{
    if (m_showCatalogue == visible) return;
    m_showCatalogue = visible;
    update();
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

    const double cos_c = std::sin(dec0) * std::sin(dec1)
                       + std::cos(dec0) * std::cos(dec1) * std::cos(ra1 - ra0);
    if (cos_c <= 0.0)
        return { -1e7, -1e7 };

    const double x = std::cos(dec1) * std::sin(ra1 - ra0) / cos_c;
    const double y = (std::cos(dec0) * std::sin(dec1)
                      - std::sin(dec0) * std::cos(dec1) * std::cos(ra1 - ra0)) / cos_c;

    const double scale = scalePixPerRad();
    return { width()  * 0.5 + x * scale,
             height() * 0.5 - y * scale };
}

void HiPSViewportWidget::unproject(QPointF screen, double &raDeg, double &decDeg) const
{
    const double ra0  = qDegreesToRadians(m_view.raDeg);
    const double dec0 = qDegreesToRadians(m_view.decDeg);
    const double scale = scalePixPerRad();

    const double x = (screen.x() - width()  * 0.5) / scale;
    const double y = -(screen.y() - height() * 0.5) / scale;

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

// ── Tile management (primary) ─────────────────────────────────────────────────

void HiPSViewportWidget::onViewChanged()
{
    if (!m_hasSurvey) return;

    if (m_view.fovDeg > 90.0) {
        if (m_allskyPixmap.isNull() && !m_allskyLoading)
            requestAllsky();
        if (m_hasSurveySec && m_allskyPixmapSec.isNull() && !m_allskyLoadingSec)
            requestAllskySec();
        return;
    }
    requestTileList();
    if (m_hasSurveySec)
        requestTileListSec();
}

void HiPSViewportWidget::requestTileList()
{
    if (m_tileListWatcher.isRunning()) return;

    const QString backendUrl   = m_backendUrl;
    const QString backendToken = m_backendToken;
    const QString surveyId     = m_survey.surveyId;
    const QString sessionId    = m_survey.sessionId;
    const double  raDeg        = m_view.raDeg;
    const double  decDeg       = m_view.decDeg;
    const double  fovDeg       = m_view.fovDeg;
    const int     order        = optimalOrder(fovDeg, width(), m_survey.maxOrder);

    m_tileListWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, surveyId, sessionId, raDeg, decDeg, fovDeg, order]()
                    -> BackendHiPSViewResponse {
                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) client.setSessionId(sessionId);
                return client.requestHiPSTilesForView(surveyId, raDeg, decDeg, fovDeg, order);
            }));
}

void HiPSViewportWidget::requestAllsky()
{
    if (m_allskyLoading || !m_hasSurvey) return;
    m_allskyLoading = true;
    emit loadingChanged(true);

    const QString backendUrl   = m_backendUrl;
    const QString backendToken = m_backendToken;
    const QString surveyId     = m_survey.surveyId;
    const QString sessionId    = m_survey.sessionId;
    const int order = std::min(3, m_survey.maxOrder);

    m_allskyWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, surveyId, sessionId, order]() -> QByteArray {
                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) client.setSessionId(sessionId);
                return client.requestHiPSAllsky(surveyId, order);
            }));
}

void HiPSViewportWidget::scheduleTileFetch(const HiPSTileKey &key)
{
    if (m_pendingKeys.contains(key) || m_tileCache.contains(key)) return;
    m_pendingKeys.insert(key);
    m_fetchQueue.append(key);
    drainFetchQueue();
}

void HiPSViewportWidget::drainFetchQueue()
{
    while (m_activeWatchers.size() < kMaxConcurrent && !m_fetchQueue.isEmpty()) {
        const HiPSTileKey key = m_fetchQueue.takeFirst();

        const QString backendUrl   = m_backendUrl;
        const QString backendToken = m_backendToken;
        const QString surveyId     = m_survey.surveyId;
        const QString sessionId    = m_survey.sessionId;
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
                                 if (pm.loadFromData(result.imageData))
                                     m_tileCache.insert(key, pm);
                             }
                             drainFetchQueue();
                             update();
                             emit loadingChanged(m_allskyLoading || m_allskyLoadingSec
                                                 || !m_pendingKeys.isEmpty()
                                                 || !m_pendingKeysSec.isEmpty());
                         });

        watcher->setFuture(QtConcurrent::run(
                [backendUrl, backendToken, surveyId, sessionId, order, pix, key]()
                        -> HiPSTileFetchResult {
                    BackendClient client(backendUrl, backendToken);
                    if (!sessionId.isEmpty()) client.setSessionId(sessionId);
                    HiPSTileFetchResult r;
                    r.key       = key;
                    r.imageData = client.requestHiPSTile(surveyId, order, pix);
                    return r;
                }));
    }
}

// ── Tile management (secondary — Task 2) ─────────────────────────────────────

void HiPSViewportWidget::requestTileListSec()
{
    if (!m_hasSurveySec || m_tileListWatcherSec.isRunning()) return;

    const QString backendUrl   = m_backendUrlSec;
    const QString backendToken = m_backendTokenSec;
    const QString surveyId     = m_surveySec.surveyId;
    const QString sessionId    = m_surveySec.sessionId;
    const double  raDeg        = m_view.raDeg;
    const double  decDeg       = m_view.decDeg;
    const double  fovDeg       = m_view.fovDeg;
    const int     order        = optimalOrder(fovDeg, width(), m_surveySec.maxOrder);

    m_tileListWatcherSec.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, surveyId, sessionId, raDeg, decDeg, fovDeg, order]()
                    -> BackendHiPSViewResponse {
                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) client.setSessionId(sessionId);
                return client.requestHiPSTilesForView(surveyId, raDeg, decDeg, fovDeg, order);
            }));
}

void HiPSViewportWidget::requestAllskySec()
{
    if (m_allskyLoadingSec || !m_hasSurveySec) return;
    m_allskyLoadingSec = true;
    emit loadingChanged(true);

    const QString backendUrl   = m_backendUrlSec;
    const QString backendToken = m_backendTokenSec;
    const QString surveyId     = m_surveySec.surveyId;
    const QString sessionId    = m_surveySec.sessionId;
    const int order = std::min(3, m_surveySec.maxOrder);

    m_allskyWatcherSec.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, surveyId, sessionId, order]() -> QByteArray {
                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) client.setSessionId(sessionId);
                return client.requestHiPSAllsky(surveyId, order);
            }));
}

void HiPSViewportWidget::scheduleTileFetchSec(const HiPSTileKey &key)
{
    if (m_pendingKeysSec.contains(key) || m_tileCacheSec.contains(key)) return;
    m_pendingKeysSec.insert(key);
    m_fetchQueueSec.append(key);
    drainFetchQueueSec();
}

void HiPSViewportWidget::drainFetchQueueSec()
{
    while (m_activeWatchersSec.size() < kMaxConcurrent && !m_fetchQueueSec.isEmpty()) {
        const HiPSTileKey key = m_fetchQueueSec.takeFirst();

        const QString backendUrl   = m_backendUrlSec;
        const QString backendToken = m_backendTokenSec;
        const QString surveyId     = m_surveySec.surveyId;
        const QString sessionId    = m_surveySec.sessionId;
        const int order = key.order;
        const int pix   = key.pix;

        auto *watcher = new QFutureWatcher<HiPSTileFetchResult>(this);
        m_activeWatchersSec.append(watcher);

        QObject::connect(watcher, &QFutureWatcher<HiPSTileFetchResult>::finished, this,
                         [this, watcher, key]() {
                             m_activeWatchersSec.removeOne(watcher);
                             m_pendingKeysSec.remove(key);
                             watcher->deleteLater();
                             const auto result = watcher->result();
                             if (result.error.isEmpty() && !result.imageData.isEmpty()) {
                                 QPixmap pm;
                                 if (pm.loadFromData(result.imageData))
                                     m_tileCacheSec.insert(key, pm);
                             }
                             drainFetchQueueSec();
                             update();
                             emit loadingChanged(m_allskyLoading || m_allskyLoadingSec
                                                 || !m_pendingKeys.isEmpty()
                                                 || !m_pendingKeysSec.isEmpty());
                         });

        watcher->setFuture(QtConcurrent::run(
                [backendUrl, backendToken, surveyId, sessionId, order, pix, key]()
                        -> HiPSTileFetchResult {
                    BackendClient client(backendUrl, backendToken);
                    if (!sessionId.isEmpty()) client.setSessionId(sessionId);
                    HiPSTileFetchResult r;
                    r.key       = key;
                    r.imageData = client.requestHiPSTile(surveyId, order, pix);
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

    if (m_hasSurveySec && m_blendMode == BlendMode::SideBySide) {
        // ── Side-by-side: primary left, secondary right ───────────────────
        const int half = width() / 2;

        p.setClipRect(0, 0, half, height());
        renderAllsky(p);
        renderTiles(p);

        p.setClipRect(half, 0, width() - half, height());
        renderAllskySec(p);
        renderTilesSec(p);

        p.setClipping(false);

        // Divider line
        p.setPen(QPen(QColor(255, 255, 255, 180), 1.5));
        p.drawLine(half, 0, half, height());

    } else {
        // ── Normal / alpha blend ──────────────────────────────────────────
        renderAllsky(p);
        renderTiles(p);

        if (m_hasSurveySec) {
            p.setOpacity(m_overlayOpacity);
            renderAllskySec(p);
            renderTilesSec(p);
            p.setOpacity(1.0);
        }
    }

    // ── Overlays (always on top) ──────────────────────────────────────────
    if (m_showGrid)
        renderGrid(p);
    if (m_showCatalogue && !m_catalogueSources.isEmpty())
        renderCatalogueOverlay(p);
    renderCrosshair(p);
}

void HiPSViewportWidget::renderBackground(QPainter &p) const
{
    p.fillRect(rect(), QColor(0, 0, 16));
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
    if (m_allskyPixmap.isNull() || m_view.fovDeg < 90.0) return;
    const QPixmap scaled = m_allskyPixmap.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                                  Qt::SmoothTransformation);
    const int ox = (width()  - scaled.width())  / 2;
    const int oy = (height() - scaled.height()) / 2;
    p.setOpacity(0.85);
    p.drawPixmap(ox, oy, scaled);
    p.setOpacity(1.0);
}

void HiPSViewportWidget::renderAllskySec(QPainter &p) const
{
    if (m_allskyPixmapSec.isNull() || m_view.fovDeg < 90.0) return;
    const QPixmap scaled = m_allskyPixmapSec.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                                     Qt::SmoothTransformation);
    const int ox = (width()  - scaled.width())  / 2;
    const int oy = (height() - scaled.height()) / 2;
    p.drawPixmap(ox, oy, scaled);
}

static void renderTilesImpl(QPainter &p,
                             const std::vector<HiPSTileDescriptor> &tiles,
                             QHash<HiPSTileKey, QPixmap> &cache,
                             const std::function<void(const HiPSTileKey &)> &scheduleFetch,
                             const std::function<QPointF(double, double)> &projectFn,
                             double scalePixPerRad, int w, int h)
{
    for (const auto &td : tiles) {
        const HiPSTileKey &key = td.key;

        if (!cache.contains(key)) {
            const QPointF centre = projectFn(td.raCenterDeg, td.decCenterDeg);
            if (centre.x() < -w * 2 || centre.x() > w * 3) continue;

            const double tileAngDeg = 90.0 / (1 << key.order);
            const int sz = static_cast<int>(scalePixPerRad
                                            * qDegreesToRadians(tileAngDeg) * 0.92);
            if (sz < 2) continue;

            p.setPen(QPen(QColor(40, 60, 80), 1));
            p.setBrush(QColor(10, 20, 30));
            p.drawRect(QRectF(centre.x() - sz * 0.5, centre.y() - sz * 0.5, sz, sz));

            scheduleFetch(key);
            continue;
        }

        const QPixmap &pm = cache[key];
        QPolygonF dst;
        dst.reserve(4);
        bool anyVisible = false;
        for (int i = 0; i < 4; ++i) {
            const QPointF sp = projectFn(td.cornersRaDeg[i], td.cornersDecDeg[i]);
            dst.append(sp);
            if (sp.x() > -w && sp.x() < 2 * w && sp.y() > -h && sp.y() < 2 * h)
                anyVisible = true;
        }
        if (!anyVisible) continue;

        const qreal W = pm.width();
        const qreal H = pm.height();
        QPolygonF src;
        src << QPointF(0, 0) << QPointF(W, 0) << QPointF(W, H) << QPointF(0, H);

        QTransform t;
        if (QTransform::quadToQuad(src, dst, t)) {
            p.save();
            p.setTransform(t, true);
            p.drawPixmap(0, 0, pm);
            p.restore();
        } else {
            const QPointF ctr = projectFn(td.raCenterDeg, td.decCenterDeg);
            p.drawPixmap(ctr.toPoint() - QPoint(pm.width() / 2, pm.height() / 2), pm);
        }
    }
}

void HiPSViewportWidget::renderTiles(QPainter &p)
{
    if (m_currentTiles.empty()) return;
    const double spr = scalePixPerRad();
    const int w = width(), h = height();
    renderTilesImpl(p, m_currentTiles, m_tileCache,
                    [this](const HiPSTileKey &k) { scheduleTileFetch(k); },
                    [this](double ra, double dec) { return project(ra, dec); },
                    spr, w, h);
}

void HiPSViewportWidget::renderTilesSec(QPainter &p)
{
    if (m_currentTilesSec.empty()) return;
    const double spr = scalePixPerRad();
    const int w = width(), h = height();
    renderTilesImpl(p, m_currentTilesSec, m_tileCacheSec,
                    [this](const HiPSTileKey &k) { scheduleTileFetchSec(k); },
                    [this](double ra, double dec) { return project(ra, dec); },
                    spr, w, h);
}

// ── Task 1 — Coordinate grid rendering ────────────────────────────────────────

void HiPSViewportWidget::renderGrid(QPainter &p) const
{
    // Only render grid in tile mode (gnomonic projection is valid for FOV < 90°)
    if (m_view.fovDeg >= 90.0) return;

    const double spacing = gridSpacingDeg(m_view.fovDeg);
    // Arc sample step: small enough for smooth curves, no finer than 0.01°
    const double arcStep = std::max(spacing / 10.0, 0.01);

    const QPen gridPen(QColor(80, 160, 230, 110), 1.0);
    p.setPen(gridPen);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Helper: draw a sequence of sky points as a QPainterPath,
    // breaking segments whenever a point is behind the projection.
    const auto drawArc = [&](const QVector<QPointF> &pts) {
        QPainterPath path;
        bool inPath = false;
        for (const QPointF &sp : pts) {
            if (sp.x() < -1e6) { inPath = false; continue; }
            if (!inPath) { path.moveTo(sp); inPath = true; }
            else          { path.lineTo(sp); }
        }
        if (!path.isEmpty())
            p.drawPath(path);
    };

    // ── Meridians (constant RA) — great circles → straight lines in gnomonic
    //    Sample only 3 points: enough because gnomonic maps great circles to lines.
    const double raStart = std::ceil(0.0 / spacing) * spacing;
    for (double ra = raStart; ra < 360.0; ra += spacing) {
        QVector<QPointF> pts;
        // Oversample slightly for numerical safety at high declinations
        for (double dec = -88.0; dec <= 88.0 + arcStep * 0.5; dec += arcStep * 2.0)
            pts.append(project(ra, dec));
        drawArc(pts);
    }

    // ── Parallels (constant Dec) — small circles, need dense sampling
    const double decStart = std::ceil(-88.0 / spacing) * spacing;
    for (double dec = decStart; dec <= 88.0 + 1e-9; dec += spacing) {
        if (std::abs(dec) > 88.5) continue; // skip poles
        QVector<QPointF> pts;
        for (double ra = 0.0; ra <= 360.0 + arcStep * 0.5; ra += arcStep)
            pts.append(project(std::min(ra, 360.0), dec));
        drawArc(pts);
    }

    // ── Labels at grid intersections (minimal, near viewport edges) ──────
    const QPen labelPen(QColor(160, 210, 255, 200), 1.0);
    p.setPen(labelPen);
    const QFont labelFont(u"Lato"_s, 8);
    p.setFont(labelFont);

    // Label only the Dec parallels that are visible
    for (double dec = decStart; dec <= 88.0 + 1e-9; dec += spacing) {
        if (std::abs(dec) > 88.5) continue;
        // Try to place label near left edge of viewport
        for (double ra = 0.0; ra < 360.0; ra += arcStep) {
            const QPointF sp = project(ra, dec);
            if (sp.x() >= 4 && sp.x() <= 60 && sp.y() >= 4 && sp.y() <= height() - 4) {
                p.drawText(sp + QPointF(3, -3), formatDec(dec));
                break;
            }
        }
    }
}

// ── Task 3 — Catalogue overlay rendering ─────────────────────────────────────

void HiPSViewportWidget::renderCatalogueOverlay(QPainter &p) const
{
    if (m_catalogueSources.isEmpty()) return;

    p.setRenderHint(QPainter::Antialiasing, true);
    // Bright cyan circle markers, semi-transparent
    const QPen markerPen(QColor(0, 230, 200, 200), 1.2);
    p.setPen(markerPen);
    p.setBrush(Qt::NoBrush);

    const double r = 4.0; // marker radius in pixels
    const QRectF vp = QRectF(rect()).adjusted(-r, -r, r, r);

    for (const auto &src : m_catalogueSources) {
        const QPointF sp = project(src.raDeg, src.decDeg);
        if (sp.x() < -1e6) continue;        // behind projection
        if (!vp.contains(sp))  continue;     // off-screen
        p.drawEllipse(sp, r, r);
    }
}

// ── Crosshair ─────────────────────────────────────────────────────────────────

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
    p.drawEllipse(QPointF(cx, cy), 3.0, 3.0);
}

// ── Event handlers ────────────────────────────────────────────────────────────

void HiPSViewportWidget::resizeEvent(QResizeEvent *)
{
    if (m_hasSurvey)
        m_viewChangeTimer->start();
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
        const double scale = scalePixPerRad();
        const double dx = e->position().x() - m_panStartPos.x();
        const double dy = e->position().y() - m_panStartPos.y();

        const double cosDec = std::cos(qDegreesToRadians(m_panStartView.decDeg));
        const double dRa  = -qRadiansToDegrees(dx / scale) / std::max(0.01, cosDec);
        const double dDec =  qRadiansToDegrees(dy / scale);

        m_view.raDeg  = normaliseRa(m_panStartView.raDeg + dRa);
        m_view.decDeg = std::clamp(m_panStartView.decDeg + dDec, -89.9, 89.9);

        update();
        emit centerChanged(m_view.raDeg, m_view.decDeg);
        m_viewChangeTimer->start();
        m_catalogueRefreshTimer->start();
    }

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
    const double factor = (e->angleDelta().y() > 0) ? 0.75 : 1.333;
    const double newFov = std::clamp(m_view.fovDeg * factor, 0.01, 180.0);
    if (qFuzzyCompare(newFov, m_view.fovDeg)) return;
    m_view.fovDeg = newFov;
    update();
    m_viewChangeTimer->start();
    m_catalogueRefreshTimer->start();
    emit fovChanged(m_view.fovDeg);
    emit orderChanged(currentOrder());
}

void HiPSViewportWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    double ra, dec;
    unproject(e->position(), ra, dec);
    m_view.raDeg  = ra;
    m_view.decDeg = dec;
    update();
    m_viewChangeTimer->start();
    m_catalogueRefreshTimer->start();
    emit centerChanged(m_view.raDeg, m_view.decDeg);
}
