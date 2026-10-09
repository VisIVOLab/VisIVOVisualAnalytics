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
#include <functional>
#include <limits>

using namespace Qt::StringLiterals;

// ── Anonymous helpers ─────────────────────────────────────────────────────────
namespace {

int optimalOrder(double fovDeg, int viewportWidth, int maxOrder)
{
    if (viewportWidth <= 0 || fovDeg <= 0.0)
        return 3;
    const double targetTileAngleDeg = fovDeg * 512.0 / viewportWidth;
    // Round *up*: truncating picks an order whose tiles can be up to twice as
    // coarse as the screen, so the sky was rendered below the display's
    // resolution. Ceiling errs towards oversampling instead.
    const int order = static_cast<int>(
            std::ceil(std::log2(90.0 / std::max(0.01, targetTileAngleDeg))));
    return std::clamp(order, 0, maxOrder);
}

double normaliseRa(double ra)
{
    ra = std::fmod(ra, 360.0);
    if (ra < 0.0) ra += 360.0;
    return ra;
}

// Sexagesimal with the carry done before formatting. Rounding the seconds to
// one decimal first is what turns 41d 29' 59.97" into 41d 30' 00.0" instead of
// printing the impossible 41d 29' 60.0".
void sexagesimal(double value, int &whole, int &minutes, double &seconds)
{
    whole   = static_cast<int>(value);
    double m = (value - whole) * 60.0;
    minutes = static_cast<int>(m);
    seconds = std::round((m - minutes) * 60.0 * 10.0) / 10.0;
    if (seconds >= 60.0) { seconds -= 60.0; ++minutes; }
    if (minutes >= 60)   { minutes -= 60;   ++whole;   }
}

QString formatRa(double raDeg)
{
    int h = 0, m = 0; double sec = 0.0;
    sexagesimal(raDeg / 15.0, h, m, sec);
    if (h >= 24) h -= 24;
    return QStringLiteral("%1h %2m %3s")
            .arg(h, 2, 10, QLatin1Char('0'))
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(sec, 4, 'f', 1, QLatin1Char('0'));
}

QString formatDec(double decDeg)
{
    const char sign = decDeg < 0 ? '-' : '+';
    int d = 0, m = 0; double sec = 0.0;
    sexagesimal(std::abs(decDeg), d, m, sec);
    return QStringLiteral("%1%2° %3' %4\"")
            .arg(sign)
            .arg(d, 2, 10, QLatin1Char('0'))
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(sec, 4, 'f', 1, QLatin1Char('0'));
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

// ── Projection maths ─────────────────────────────────────────────────────────
//
// Everything is done in a "native" frame: the sky rotated so the view centre
// sits at the origin, with +X east, +Y north and +Z towards the centre. Each
// projection is then a plane map of that frame, normalised so that all of them
// have unit scale at the centre — which is what lets a single field-of-view
// number mean the same thing whichever projection is selected.

struct NativeVec { double x, y, z; };

NativeVec toNative(double raDeg, double decDeg, double ra0, double dec0)
{
    const double ra  = qDegreesToRadians(raDeg);
    const double dec = qDegreesToRadians(decDeg);
    const double d   = ra - ra0;
    return { std::cos(dec) * std::sin(d),
             std::cos(dec0) * std::sin(dec) - std::sin(dec0) * std::cos(dec) * std::cos(d),
             std::sin(dec0) * std::sin(dec) + std::cos(dec0) * std::cos(dec) * std::cos(d) };
}

void fromNative(const NativeVec &v, double ra0, double dec0,
                double &raDeg, double &decDeg)
{
    const double sinDec = std::sin(dec0) * v.z + std::cos(dec0) * v.y;
    const double cosDecCosD = std::cos(dec0) * v.z - std::sin(dec0) * v.y;
    decDeg = qRadiansToDegrees(std::asin(std::clamp(sinDec, -1.0, 1.0)));
    raDeg  = qRadiansToDegrees(ra0 + std::atan2(v.x, cosDecCosD));
}

constexpr double kSqrt2 = 1.41421356237309504880;

// Where Mercator is cut off; the poles are infinitely far away.
const double kMercatorLatLimit = qDegreesToRadians(85.0);

// Solve 2t + sin 2t = pi sin(lat) for t, the Mollweide auxiliary angle.
// Newton's derivative 2 + 2cos 2t vanishes at the poles and the iteration
// crawls near them, so the poles are taken in closed form and the rest is
// bisection-bracketed Newton, which cannot leave [-pi/2, pi/2].
double mollweideParameter(double lat)
{
    const double s = std::sin(std::clamp(lat, -M_PI * 0.5, M_PI * 0.5));
    if (std::abs(s) >= 1.0 - 1e-15)
        return s > 0.0 ? M_PI * 0.5 : -M_PI * 0.5;

    const double target = M_PI * s;
    double lo = -M_PI * 0.5, hi = M_PI * 0.5, t = lat;
    for (int i = 0; i < 60; ++i) {
        const double f = 2.0 * t + std::sin(2.0 * t) - target;
        if (std::abs(f) < 1e-13) break;
        if (f > 0.0) hi = t; else lo = t;
        const double df = 2.0 + 2.0 * std::cos(2.0 * t);
        double next = (std::abs(df) > 1e-9) ? t - f / df
                                            : 0.5 * (lo + hi);
        if (!(next > lo && next < hi))
            next = 0.5 * (lo + hi);
        if (std::abs(next - t) < 1e-15) { t = next; break; }
        t = next;
    }
    return t;
}

// Forward map. Returns false when the point has no image in this projection.
bool projectNative(SkyProjection proj, const NativeVec &v, double &px, double &py)
{
    switch (proj) {
    case SkyProjection::Tan: {
        if (v.z <= 1e-6) return false;             // the far hemisphere and the horizon
        px = v.x / v.z; py = v.y / v.z;
        return true;
    }
    case SkyProjection::Sin: {
        if (v.z < 0.0) return false;
        px = v.x; py = v.y;
        return true;
    }
    case SkyProjection::Arc: {
        const double r = std::hypot(v.x, v.y);
        const double theta = std::atan2(r, v.z);   // angular distance from centre
        if (r < 1e-12) {
            // On the axis: either the centre or the antipode, where the
            // projection is singular. Mapping the antipode to (0,0) would put
            // the far pole of the sky on the crosshair.
            if (v.z < 0.0) return false;
            px = py = 0.0;
            return true;
        }
        px = theta * v.x / r; py = theta * v.y / r;
        return true;
    }
    case SkyProjection::Ait: {
        // Hammer-Aitoff on the native longitude/latitude.
        const double lon = std::atan2(v.x, v.z);
        const double lat = std::asin(std::clamp(v.y, -1.0, 1.0));
        const double denom = 1.0 + std::cos(lat) * std::cos(lon * 0.5);
        if (denom < 1e-9) return false;
        const double g = std::sqrt(2.0 / denom);
        px = 2.0 * g * std::cos(lat) * std::sin(lon * 0.5);
        py = g * std::sin(lat);
        return true;
    }
    case SkyProjection::Mol: {
        const double lon = std::atan2(v.x, v.z);
        const double lat = std::asin(std::clamp(v.y, -1.0, 1.0));
        const double t = mollweideParameter(lat);
        // The textbook equal-area Mollweide, same outline as Hammer-Aitoff.
        // Being equal-area it cannot also be conformal: at the centre the
        // horizontal scale is 2*sqrt(2)/pi and the vertical pi/(2*sqrt(2)),
        // whose product is 1. The field of view is that geometric mean, so a
        // switch to or from this projection keeps the area on screen and
        // stretches it by about 11% one way and squeezes it the other.
        px = (2.0 * kSqrt2 / M_PI) * lon * std::cos(t);
        py = kSqrt2 * std::sin(t);
        return true;
    }
    case SkyProjection::Mer: {
        const double lon = std::atan2(v.x, v.z);
        const double lat = std::asin(std::clamp(v.y, -1.0, 1.0));
        if (std::abs(lat) > kMercatorLatLimit) return false;   // the poles run off to infinity
        px = lon;
        py = std::log(std::tan(M_PI * 0.25 + lat * 0.5));
        return true;
    }
    }
    return false;
}

// Inverse map. Returns false for plane points outside the projection's outline.
bool unprojectNative(SkyProjection proj, double px, double py, NativeVec &v)
{
    switch (proj) {
    case SkyProjection::Tan: {
        const double n = 1.0 / std::sqrt(1.0 + px * px + py * py);
        v = { px * n, py * n, n };
        return true;
    }
    case SkyProjection::Sin: {
        const double r2 = px * px + py * py;
        if (r2 > 1.0) return false;
        v = { px, py, std::sqrt(1.0 - r2) };
        return true;
    }
    case SkyProjection::Arc: {
        const double theta = std::hypot(px, py);
        if (theta > M_PI) return false;
        if (theta < 1e-12) { v = { 0.0, 0.0, 1.0 }; return true; }
        const double s = std::sin(theta) / theta;
        v = { px * s, py * s, std::cos(theta) };
        return true;
    }
    case SkyProjection::Ait: {
        const double zz = 1.0 - (px / 4.0) * (px / 4.0) - (py / 2.0) * (py / 2.0);
        if (zz < 0.0) return false;
        const double z = std::sqrt(zz);
        const double lon = 2.0 * std::atan2(z * px, 2.0 * (2.0 * z * z - 1.0));
        const double lat = std::asin(std::clamp(z * py, -1.0, 1.0));
        if (std::abs(lon) > M_PI) return false;
        v = { std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon) };
        return true;
    }
    case SkyProjection::Mol: {
        const double sy = py / kSqrt2;
        if (std::abs(sy) > 1.0) return false;
        const double t = std::asin(std::clamp(sy, -1.0, 1.0));
        const double lat = std::asin(std::clamp((2.0 * t + std::sin(2.0 * t)) / M_PI, -1.0, 1.0));
        const double ct = std::cos(t);
        if (std::abs(ct) < 1e-9) {
            // The pole is a single point of the outline, not a rejected
            // region: accept it when the plane point is on the axis.
            if (std::abs(px) > 1e-6) return false;
            v = { 0.0, lat >= 0.0 ? 1.0 : -1.0, 0.0 };
            return true;
        }
        const double lon = px / ((2.0 * kSqrt2 / M_PI) * ct);
        if (std::abs(lon) > M_PI) return false;
        v = { std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon) };
        return true;
    }
    case SkyProjection::Mer: {
        const double lon = px;
        if (std::abs(lon) > M_PI) return false;
        const double lat = 2.0 * std::atan(std::exp(py)) - M_PI * 0.5;
        // Same cut-off as the forward map, or the blank band past it would
        // report coordinates that cannot be projected back.
        if (std::abs(lat) > kMercatorLatLimit) return false;
        v = { std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon) };
        return true;
    }
    }
    return false;
}

// Half-width of the projection's outline in native plane units, or 0 when the
// projection has no finite extent.
double projectionHalfSpan(SkyProjection proj)
{
    switch (proj) {
    case SkyProjection::Ait: return 2.0 * kSqrt2;
    case SkyProjection::Mol: return 2.0 * kSqrt2;
    case SkyProjection::Mer: return M_PI;
    case SkyProjection::Arc: return M_PI;
    case SkyProjection::Sin: return 1.0;
    case SkyProjection::Tan: break;
    }
    return 0.0;
}

// How far up the tile pyramid to look for something to draw while a tile is
// still in flight. Five levels is a 32x coarser texture — beyond that the
// fallback is too blurry to be worth the lookup.
constexpr int kLodAncestorDepth = 5;

// Neighbouring quads share their edges exactly, so rasterising them
// independently leaves a one-pixel crack along every shared edge. On a field
// showing a handful of tiles that is a faint hairline; on a whole-sky view of
// several hundred it is a grid drawn over the map. Push each corner this far
// outwards so neighbours overlap instead of abutting.
constexpr double kQuadGrowPx = 0.6;

// Tiles held before the cache is trimmed back to what is on screen. A decoded
// 512x512 tile costs about a megabyte, so this is a budget of roughly 1.5 GB
// worst case and far less in practice, where most tiles are smaller.
constexpr int kMaxCachedTiles = 1500;



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
        if (m_cataloguePath.isEmpty()) return;
        if (m_catalogueWatcher.isRunning()) {
            // Remember that the view has moved on; the request in flight is
            // for a field we are no longer looking at.
            m_cataloguePending = true;
            return;
        }
        const QString path      = m_cataloguePath;
        const QString raField   = m_catalogueRaField;
        const QString decField  = m_catalogueDecField;
        const QString url       = m_backendUrl;
        const QString token     = m_backendToken;
        const double  raDeg     = m_view.raDeg;
        const double  decDeg    = m_view.decDeg;
        const double  fovDeg    = m_view.fovDeg;
        const double  radiusDeg = viewportRadiusDeg();
        m_catalogueRequestGeneration = m_catalogueGeneration;
        m_catalogueWatcher.setFuture(QtConcurrent::run(
                [url, token, path, raDeg, decDeg, fovDeg, radiusDeg, raField, decField]()
                        -> BackendHiPSCatalogueOverlayResult {
                    BackendClient client(url, token);
                    return client.requestHiPSCatalogueOverlay(
                            path, raDeg, decDeg, fovDeg, raField, decField, 5000, radiusDeg);
                }));
    });

    // ── Allsky watcher (primary) ──────────────────────────────────────────
    QObject::connect(&m_allskyWatcher, &QFutureWatcher<QByteArray>::finished, this, [this]() {
        m_allskyLoading = false;
        const QByteArray data = m_allskyWatcher.result();
        if (!data.isEmpty()) {
            QPixmap pm;
            if (pm.loadFromData(data)) {
                m_allskyLoaded = true;
                seedCacheFromAllsky(pm, allSkyOrder(false), m_previewCache);
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
                         if (m_tileListPending) {
                             m_tileListPending = false;
                             QTimer::singleShot(0, this, [this]() { requestTileList(); });
                         }
                     });

    // ── Allsky watcher (secondary — Task 2) ───────────────────────────────
    QObject::connect(&m_allskyWatcherSec, &QFutureWatcher<QByteArray>::finished, this, [this]() {
        m_allskyLoadingSec = false;
        const QByteArray data = m_allskyWatcherSec.result();
        if (!data.isEmpty()) {
            QPixmap pm;
            if (pm.loadFromData(data)) {
                m_allskyLoadedSec = true;
                seedCacheFromAllsky(pm, allSkyOrder(true), m_previewCacheSec);
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
                         if (m_tileListPendingSec) {
                             m_tileListPendingSec = false;
                             QTimer::singleShot(0, this, [this]() { requestTileListSec(); });
                         }
                     });

    // ── Catalogue watcher (Task 3) ────────────────────────────────────────
    QObject::connect(&m_catalogueWatcher,
                     &QFutureWatcher<BackendHiPSCatalogueOverlayResult>::finished,
                     this, [this]() {
                         const auto r = m_catalogueWatcher.result();
                         const bool pending = m_cataloguePending;
                         m_cataloguePending = false;
                         const auto scheduleIfPending = [this, pending]() {
                             if (!pending || m_cataloguePath.isEmpty()) return;
                             // Not start(0): that would overwrite the timer's
                             // interval and every later debounce would fire
                             // immediately. Come back on the next turn of the
                             // event loop instead.
                             QTimer::singleShot(0, this, [this]() {
                                 if (!m_cataloguePath.isEmpty())
                                     m_catalogueRefreshTimer->start();
                             });
                         };

                         if (m_catalogueRequestGeneration != m_catalogueGeneration) {
                             // The catalogue was replaced or cleared after this
                             // request went out: applying it now would put the
                             // old sources back, and failing it would drop the
                             // new one. The replacement still needs fetching.
                             scheduleIfPending();
                             return;
                         }
                         if (r.valid) {
                             m_catalogueSources = r.sources;
                             emit catalogueOverlayRefreshed(r.totalInView, r.truncated);
                             update();
                         } else {
                             m_catalogueSources.clear();
                             clearCatalogueOverlay();
                             emit catalogueOverlayFailed(
                                     r.error.isEmpty()
                                             ? u"The catalogue could not be read."_s
                                             : r.error);
                             return;   // nothing to refresh for a catalogue we just dropped
                         }
                         scheduleIfPending();
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
    m_previewCache.clear();      // belongs to the survey we are replacing
    m_allskyLoaded = false;
    m_pendingKeys.clear();
    m_fetchQueue.clear();
    m_currentTiles.clear();

    update();
    requestAllsky();
    m_viewChangeTimer->start();
    m_catalogueRefreshTimer->start();

    // The view was reset to its defaults above; announce it so the status
    // strip shows a centre/FOV/order instead of the "-" placeholders.
    emit centerChanged(m_view.raDeg, m_view.decDeg);
    emit fovChanged(m_view.fovDeg);
    emit orderChanged(currentOrder());
}

void HiPSViewportWidget::clearSurvey()
{
    m_hasSurvey = false;
    m_tileCache.clear();
    m_previewCache.clear();
    m_allskyLoaded = false;
    m_pendingKeys.clear();
    m_fetchQueue.clear();
    m_currentTiles.clear();
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
    m_view.fovDeg = std::clamp(fovDeg, 0.01, projectionMaxFovDeg(m_projection));
    update();
    m_viewChangeTimer->start();
    m_catalogueRefreshTimer->start();
    emit fovChanged(m_view.fovDeg);
}

void HiPSViewportWidget::resetView()
{
    m_view = {};
    // Only the view is reset. The tile caches are still valid for this survey,
    // and the AllSky previews in particular must survive: dropping them left
    // the whole-sky fallback empty with nothing arranging to fetch it again.
    m_currentTiles.clear();
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
    // A whole-sky view is drawn from the same low order the AllSky mosaic
    // carries; picking it by screen resolution there would ask for thousands
    // of tiles to cover the sphere.
    if (m_view.fovDeg > 90.0)
        return allSkyOrder(false);
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
    m_previewCacheSec.clear();
    m_allskyLoadedSec = false;
    m_pendingKeysSec.clear();
    m_fetchQueueSec.clear();
    m_currentTilesSec.clear();

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
    m_previewCacheSec.clear();
    m_allskyLoadedSec = false;
    m_pendingKeysSec.clear();
    m_fetchQueueSec.clear();
    m_currentTilesSec.clear();
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
    // Any request already in flight is for the catalogue being replaced.
    ++m_catalogueGeneration;
    m_cataloguePending = false;
    m_catalogueRefreshTimer->start();
    update();
}

void HiPSViewportWidget::clearCatalogueOverlay()
{
    m_cataloguePath.clear();
    m_catalogueSources.clear();
    ++m_catalogueGeneration;
    m_cataloguePending = false;
    m_catalogueRefreshTimer->stop();
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
    const NativeVec v = toNative(raDeg, decDeg, ra0, dec0);

    double px = 0.0, py = 0.0;
    if (!projectNative(m_projection, v, px, py))
        return { -1e7, -1e7 };

    // RA increases to the LEFT, as in every sky image and as the WCS
    // convention requires (a negative CDELT1). Adding x here mirrored the
    // whole field east-west, so the view disagreed with hips2fits, with
    // Aladin, and with the image viewer's own overlays.
    const double scale = scalePixPerRad();
    return { width()  * 0.5 - px * scale,
             height() * 0.5 - py * scale };
}

void HiPSViewportWidget::unproject(QPointF screen, double &raDeg, double &decDeg) const
{
    const double ra0  = qDegreesToRadians(m_view.raDeg);
    const double dec0 = qDegreesToRadians(m_view.decDeg);
    const double scale = scalePixPerRad();

    const double px = -(screen.x() - width()  * 0.5) / scale;
    const double py = -(screen.y() - height() * 0.5) / scale;

    NativeVec v{};
    if (!unprojectNative(m_projection, px, py, v)) {
        // Off the projection's outline: report the centre rather than a
        // fabricated position.
        raDeg  = m_view.raDeg;
        decDeg = m_view.decDeg;
        return;
    }
    fromNative(v, ra0, dec0, raDeg, decDeg);
    raDeg  = normaliseRa(raDeg);
    decDeg = std::clamp(decDeg, -90.0, 90.0);
}

double HiPSViewportWidget::projectionSpanPx() const
{
    return 2.0 * projectionHalfSpan(m_projection) * scalePixPerRad();
}

double HiPSViewportWidget::viewportRadiusDeg() const
{
    // Measured, not assumed. In the native frame the separation from the view
    // centre is just the angle off the +Z axis, so each probe is one acos.
    //
    // The perimeter is sampled densely rather than at the four corners: in a
    // curved projection the furthest point of a wide viewport is not a corner.
    // A 284-degree Hammer-Aitoff field in a 1200x300 window reaches 154.8
    // degrees part-way up its left edge, where the corners only reach 153.1,
    // and catalogue sources in between were dropped. A probe that falls
    // outside the projection's outline has no sky position at all, and the
    // honest answer is then that the view reaches as far as the projection
    // does.
    const double scale = scalePixPerRad();
    const auto probe = [&](double x, double y, double &sepDeg) {
        NativeVec v{};
        if (!unprojectNative(m_projection,
                             -(x - width() * 0.5) / scale,
                             -(y - height() * 0.5) / scale, v))
            return false;
        sepDeg = qRadiansToDegrees(std::acos(std::clamp(v.z, -1.0, 1.0)));
        return true;
    };

    constexpr int kPerEdge = 64;
    double worst = 0.0, sep = 0.0;
    for (int k = 0; k <= kPerEdge; ++k) {
        const double t = double(k) / kPerEdge;
        const double x = t * width(), y = t * height();
        // The four edges, plus the centre lines so a viewport that straddles
        // the outline is caught wherever it does so.
        const QPointF points[] = {
            { x, 0.0 }, { x, double(height()) }, { 0.0, y }, { double(width()), y },
            { x, height() * 0.5 }, { width() * 0.5, y },
        };
        for (const QPointF &pt : points) {
            if (!probe(pt.x(), pt.y(), sep))
                return 180.0;
            worst = std::max(worst, sep);
        }
    }
    // A little margin: the sampling is dense but it is still sampling.
    return std::clamp(worst * 1.05, 0.0, 180.0);
}

int HiPSViewportWidget::allSkyOrder(bool secondary) const
{
    if (secondary)
        return m_hasSurveySec ? std::min(3, m_surveySec.maxOrder) : 3;
    return m_hasSurvey ? std::min(3, m_survey.maxOrder) : 3;
}

void HiPSViewportWidget::evictTilesIfOverBudget(
        QHash<HiPSTileKey, QPixmap> &cache,
        const std::vector<HiPSTileDescriptor> &inView) const
{
    // Nothing ever removed tiles, so panning around a deep survey grew the
    // cache without bound — a decoded 512x512 tile is about a megabyte.
    if (cache.size() <= kMaxCachedTiles) return;

    QSet<HiPSTileKey> keep;
    keep.reserve(int(inView.size()) * (kLodAncestorDepth + 1));
    for (const auto &td : inView) {
        // Keep what is on screen and the ancestors that back its LOD fallback.
        HiPSTileKey k = td.key;
        keep.insert(k);
        for (int up = 0; up < kLodAncestorDepth && k.order > 0; ++up) {
            k = { k.order - 1, k.pix >> 2 };
            keep.insert(k);
        }
    }

    for (auto it = cache.begin(); it != cache.end(); ) {
        if (keep.contains(it.key())) ++it;
        else it = cache.erase(it);
    }
}

void HiPSViewportWidget::seedCacheFromAllsky(const QPixmap &mosaic, int order,
                                             QHash<HiPSTileKey, QPixmap> &cache) const
{
    if (mosaic.isNull() || order < 0) return;

    // A HiPS AllSky file is every tile of one order laid out in a single grid,
    // row-major from tile 0 at the top left, with floor(sqrt(nTiles)) columns.
    // Drawing it stretched across the viewport, as this used to, shows a
    // scrambled mosaic rather than the sky. Split it back into tiles instead
    // and the ordinary tile renderer places each one correctly.
    const int nTiles = 12 * (1 << (2 * order));
    const int columns = static_cast<int>(std::floor(std::sqrt(double(nTiles))));
    if (columns <= 0) return;

    const int tileSize = mosaic.width() / columns;
    if (tileSize <= 0) return;
    // The width must divide exactly, or the column boundaries we compute are
    // not the ones the file was written with and every tile comes out shifted.
    if (tileSize * columns != mosaic.width()) return;
    const int rows = mosaic.height() / tileSize;
    if (rows * columns < nTiles) return;   // not the layout we expect; leave it alone

    for (int pix = 0; pix < nTiles; ++pix) {
        const int row = pix / columns;
        const int col = pix % columns;
        QPixmap tile = mosaic.copy(col * tileSize, row * tileSize, tileSize, tileSize);
        if (!tile.isNull())
            cache.insert({ order, pix }, tile);
    }
}

// ── Projection selection ──────────────────────────────────────────────────────

double HiPSViewportWidget::projectionMaxFovDeg(SkyProjection projection)
{
    switch (projection) {
    // Gnomonic diverges as the field approaches a hemisphere; orthographic
    // shows exactly one hemisphere and no more.
    case SkyProjection::Tan: return 120.0;
    case SkyProjection::Sin: return 180.0;
    case SkyProjection::Arc:
    case SkyProjection::Ait:
    case SkyProjection::Mol:
    case SkyProjection::Mer: return 360.0;
    }
    return 180.0;
}

QString HiPSViewportWidget::projectionName(SkyProjection projection)
{
    switch (projection) {
    case SkyProjection::Tan: return u"Gnomonic (TAN)"_s;
    case SkyProjection::Sin: return u"Orthographic (SIN)"_s;
    case SkyProjection::Arc: return u"Zenithal equidistant (ARC)"_s;
    case SkyProjection::Ait: return u"Hammer-Aitoff (AIT)"_s;
    case SkyProjection::Mol: return u"Mollweide (MOL)"_s;
    case SkyProjection::Mer: return u"Mercator (MER)"_s;
    }
    return u"Gnomonic (TAN)"_s;
}

QString HiPSViewportWidget::projectionCode(SkyProjection projection)
{
    switch (projection) {
    case SkyProjection::Tan: return u"TAN"_s;
    case SkyProjection::Sin: return u"SIN"_s;
    case SkyProjection::Arc: return u"ARC"_s;
    case SkyProjection::Ait: return u"AIT"_s;
    case SkyProjection::Mol: return u"MOL"_s;
    case SkyProjection::Mer: return u"MER"_s;
    }
    return u"TAN"_s;
}

void HiPSViewportWidget::setProjection(SkyProjection projection)
{
    if (m_projection == projection) return;
    m_projection = projection;

    // A projection that cannot show the current field has to pull it in.
    const double maxFov = projectionMaxFovDeg(projection);
    if (m_view.fovDeg > maxFov) {
        m_view.fovDeg = maxFov;
        emit fovChanged(m_view.fovDeg);
    }

    update();
    m_viewChangeTimer->start();
    m_catalogueRefreshTimer->start();
    emit projectionChanged(projection);
    emit orderChanged(currentOrder());
}

// ── Tile management (primary) ─────────────────────────────────────────────────

void HiPSViewportWidget::onViewChanged()
{
    if (!m_hasSurvey) return;

    // The AllSky mosaic is one request that seeds the cache with every tile of
    // its order, so a whole-sky view needs no per-tile fetching at all, and
    // every deeper view has an ancestor to fall back on while it loads.
    if (m_view.fovDeg > 90.0) {
        if (!m_allskyLoaded && !m_allskyLoading)
            requestAllsky();
        if (m_hasSurveySec && !m_allskyLoadedSec && !m_allskyLoadingSec)
            requestAllskySec();
    }
    requestTileList();
    if (m_hasSurveySec)
        requestTileListSec();
}

void HiPSViewportWidget::requestTileList()
{
    if (m_tileListWatcher.isRunning()) {
        m_tileListPending = true;
        return;
    }
    m_tileListPending = false;

    const QString backendUrl   = m_backendUrl;
    const QString backendToken = m_backendToken;
    const QString surveyId     = m_survey.surveyId;
    const QString sessionId    = m_survey.sessionId;
    const double  raDeg        = m_view.raDeg;
    const double  decDeg       = m_view.decDeg;
    // Past 90 degrees the covering disc is the whole sphere; ask for it
    // explicitly so the backend returns every tile of the order rather than
    // clipping the query to a disc it cannot represent.
    const bool    wholeSky     = m_view.fovDeg > 90.0;
    const double  fovDeg       = wholeSky ? 360.0 : m_view.fovDeg;
    const int     order        = wholeSky
                                       ? allSkyOrder(false)
                                       : optimalOrder(m_view.fovDeg, width(), m_survey.maxOrder);
    // The disc the backend queries has to cover the viewport, corners and all.
    // Past 90 degrees we ask for the whole sphere as a matter of policy rather
    // than geometry: that is the order the AllSky mosaic already covers, so it
    // costs nothing, and it is drawn from previews without fetching.
    const double  radiusDeg    = wholeSky ? 180.0 : viewportRadiusDeg();

    m_tileListWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, surveyId, sessionId, raDeg, decDeg, fovDeg, order,
             radiusDeg]()
                    -> BackendHiPSViewResponse {
                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) client.setSessionId(sessionId);
                return client.requestHiPSTilesForView(surveyId, raDeg, decDeg, fovDeg,
                                                      order, radiusDeg);
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
    const int order = allSkyOrder(false);

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
                             evictTilesIfOverBudget(m_tileCache, m_currentTiles);
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
    if (!m_hasSurveySec) return;
    if (m_tileListWatcherSec.isRunning()) {
        m_tileListPendingSec = true;
        return;
    }
    m_tileListPendingSec = false;

    const QString backendUrl   = m_backendUrlSec;
    const QString backendToken = m_backendTokenSec;
    const QString surveyId     = m_surveySec.surveyId;
    const QString sessionId    = m_surveySec.sessionId;
    const double  raDeg        = m_view.raDeg;
    const double  decDeg       = m_view.decDeg;
    // Past 90 degrees the covering disc is the whole sphere; ask for it
    // explicitly so the backend returns every tile of the order rather than
    // clipping the query to a disc it cannot represent.
    const bool    wholeSky     = m_view.fovDeg > 90.0;
    const double  fovDeg       = wholeSky ? 360.0 : m_view.fovDeg;
    const int     order        = wholeSky
                                       ? allSkyOrder(true)
                                       : optimalOrder(m_view.fovDeg, width(), m_surveySec.maxOrder);
    const double  radiusDeg    = wholeSky ? 180.0 : viewportRadiusDeg();

    m_tileListWatcherSec.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, surveyId, sessionId, raDeg, decDeg, fovDeg, order,
             radiusDeg]()
                    -> BackendHiPSViewResponse {
                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) client.setSessionId(sessionId);
                return client.requestHiPSTilesForView(surveyId, raDeg, decDeg, fovDeg,
                                                      order, radiusDeg);
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
    const int order = allSkyOrder(true);

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
                             evictTilesIfOverBudget(m_tileCacheSec, m_currentTilesSec);
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
        renderTiles(p);

        p.setClipRect(half, 0, width() - half, height());
        renderTilesSec(p);

        p.setClipping(false);

        // Divider line
        p.setPen(QPen(QColor(255, 255, 255, 180), 1.5));
        p.drawLine(half, 0, half, height());

    } else {
        // ── Normal / alpha blend ──────────────────────────────────────────
        renderTiles(p);

        if (m_hasSurveySec) {
            p.setOpacity(m_overlayOpacity);
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
               u"No HiPS survey loaded.\nEnter a survey URL above and press Open."_s);
}

// Unit vector of an equatorial direction (reuses NativeVec as a plain 3-vector).
static NativeVec directionOf(double raDeg, double decDeg)
{
    const double ra = qDegreesToRadians(raDeg), dec = qDegreesToRadians(decDeg);
    return { std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec) };
}

static void renderTilesImpl(QPainter &p,
                             const std::vector<HiPSTileDescriptor> &tiles,
                             QHash<HiPSTileKey, QPixmap> &cache,
                             const QHash<HiPSTileKey, QPixmap> &previews,
                             const std::function<void(const HiPSTileKey &)> &scheduleFetch,
                             const std::function<QPointF(double, double)> &projectFn,
                             double scalePixPerRad, int w, int h,
                             double spanPx, double devicePixelRatio, bool previewOnly)
{
    // Adjacent tiles share their edges exactly, so an antialiased edge blends
    // each one with the background instead of with its neighbour and the whole
    // mosaic is ruled with dark hairlines. Measured on M31: 1647 seam pixels
    // with antialiasing, 132 without, and the match to hips2fits is no worse.
    const bool hadAntialiasing = p.testRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::Antialiasing, false);

    // A quad this wide has wrapped around the far side of an all-sky
    // projection and would be painted as a smear right across the map.
    const double wrapLimitPx = spanPx > 0.0 ? spanPx * 0.4 : std::numeric_limits<double>::max();

    const auto valid = [](const QPointF &q) { return q.x() > -9e6; };

    std::vector<QPointF> mesh;
    std::vector<int> drawable;

    for (const auto &td : tiles) {
        const HiPSTileKey &key = td.key;

        // Corner roles as healpy returns them, and their screen positions.
        QPointF corner[4];
        bool anyVisible = false, allValid = true;
        for (int i = 0; i < 4; ++i) {
            corner[i] = projectFn(td.cornersRaDeg[i], td.cornersDecDeg[i]);
            if (!valid(corner[i])) { allValid = false; continue; }
            if (corner[i].x() > -w && corner[i].x() < 2 * w &&
                corner[i].y() > -h && corner[i].y() < 2 * h)
                anyVisible = true;
        }
        // A tile straddling the projection's edge has corners with no image at
        // all, yet part of it is on screen, so it still has to be examined.
        if (!anyVisible && allValid) continue;

        // How finely to subdivide. One projective transform maps a HEALPix
        // diamond exactly at its four corners and approximately in between,
        // which is invisible for a tile a few tens of pixels across and badly
        // wrong for one that spans a curved projection. Powers of two only, so
        // the texture divides evenly.
        int n = 8;
        if (allValid) {
            double maxEdge = 0.0;
            for (int i = 0; i < 4; ++i) {
                const QPointF d = corner[(i + 1) % 4] - corner[i];
                maxEdge = std::max(maxEdge, std::hypot(d.x(), d.y()));
            }
            n = 1;
            while (n < 8 && maxEdge / n > 96.0) n *= 2;
        }

        // The tile corners arrive as (N, W, S, E); a HiPS tile image is stored
        // with its rows running the other way, so the texture's top edge lies
        // along the diamond's SOUTH-WEST side, not the north-west one. Pairing
        // them in the naive order put each tile's content in the wrong half of
        // its diamond, which read on screen as a checkerboard of mismatched
        // sky. Verified against hips2fits: r=0.993 on M31 with this order.
        // In texture space that makes (0,0)=S, (1,0)=W, (1,1)=N, (0,1)=E.
        const NativeVec cN = directionOf(td.cornersRaDeg[0], td.cornersDecDeg[0]);
        const NativeVec cW = directionOf(td.cornersRaDeg[1], td.cornersDecDeg[1]);
        const NativeVec cS = directionOf(td.cornersRaDeg[2], td.cornersDecDeg[2]);
        const NativeVec cE = directionOf(td.cornersRaDeg[3], td.cornersDecDeg[3]);

        // Screen positions of the (n+1)x(n+1) mesh, interpolating the corner
        // directions on the sphere rather than in the plane.
        mesh.assign(size_t(n + 1) * size_t(n + 1), QPointF(-1e7, -1e7));
        for (int j = 0; j <= n; ++j) {
            const double v = double(j) / n;
            for (int i = 0; i <= n; ++i) {
                const double u = double(i) / n;
                const double a = (1.0 - u) * (1.0 - v), b = u * (1.0 - v);
                const double c = u * v,                 d = (1.0 - u) * v;
                NativeVec q{ a * cS.x + b * cW.x + c * cN.x + d * cE.x,
                             a * cS.y + b * cW.y + c * cN.y + d * cE.y,
                             a * cS.z + b * cW.z + c * cN.z + d * cE.z };
                const double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
                if (len < 1e-12) continue;
                const double raDeg  = qRadiansToDegrees(std::atan2(q.y, q.x));
                const double decDeg = qRadiansToDegrees(std::asin(std::clamp(q.z / len, -1.0, 1.0)));
                mesh[size_t(j) * (n + 1) + i] = projectFn(raDeg, decDeg);
            }
        }

        // Which pieces of this tile actually land on screen, and how big they
        // are there. Deciding this from the mesh rather than from the corners
        // is what keeps a tile on the far side of the sphere — whose corners
        // are all unprojectable — from being fetched at full resolution for a
        // view it can never appear in.
        drawable.clear();
        double maxPieceEdgePx = 0.0;
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const QPointF a = mesh[size_t(j)     * (n + 1) + i];
                const QPointF b = mesh[size_t(j)     * (n + 1) + i + 1];
                const QPointF c = mesh[size_t(j + 1) * (n + 1) + i + 1];
                const QPointF d = mesh[size_t(j + 1) * (n + 1) + i];
                if (!valid(a) || !valid(b) || !valid(c) || !valid(d)) continue;

                const double minX = std::min({ a.x(), b.x(), c.x(), d.x() });
                const double maxX = std::max({ a.x(), b.x(), c.x(), d.x() });
                const double minY = std::min({ a.y(), b.y(), c.y(), d.y() });
                const double maxY = std::max({ a.y(), b.y(), c.y(), d.y() });
                if (maxX - minX > wrapLimitPx) continue;          // wrapped round the map
                if (maxX < 0 || minX > w || maxY < 0 || minY > h) continue;

                maxPieceEdgePx = std::max({ maxPieceEdgePx, maxX - minX, maxY - minY });
                drawable.push_back(j * n + i);
            }
        }
        if (drawable.empty()) continue;      // nothing of this tile is on screen

        // Which texture to use: the tile's own, or the sub-rectangle this tile
        // occupies inside the nearest cached ancestor.
        const QPixmap *tex = nullptr;
        double u0 = 0.0, v0 = 0.0, sub = 1.0;

        const auto own = cache.constFind(key);
        if (own != cache.constEnd()) {
            tex = &own.value();
        } else {
            // Rather than leave a hole, draw the coarsest detail we already
            // have: each ancestor covers this diamond, and the quadrant a child
            // occupies in its parent is (u, v) = ((c >> 1) & 1, c & 1) for the
            // low two bits of the NESTED index. Verified against hips2fits:
            // r=0.994 rendering M31 entirely from order-7 parents.
            // The AllSky previews live at one fixed order, so the walk has to
            // reach it however deep the tile is — a five-level cap would leave
            // deep zooms with nothing while the real tile is in flight.
            int ancOrder = key.order, ancPix = key.pix;
            while (ancOrder > 0) {
                const int child = ancPix & 3;
                u0 = (((child >> 1) & 1) + u0) * 0.5;
                v0 = ((child & 1) + v0) * 0.5;
                sub *= 0.5;
                ancPix >>= 2;
                --ancOrder;
                const HiPSTileKey ancKey{ ancOrder, ancPix };
                if (key.order - ancOrder <= kLodAncestorDepth) {
                    const auto anc = cache.constFind(ancKey);
                    if (anc != cache.constEnd()) { tex = &anc.value(); break; }
                }
                const auto prev = previews.constFind(ancKey);
                if (prev != previews.constEnd()) { tex = &prev.value(); break; }
                if (sub < 1.0 / 65536.0) break;   // the sub-rectangle is sub-pixel
            }
            // A preview at exactly this tile's own order is still a fallback,
            // not the tile: it does not mark the tile as loaded.
            if (!tex) {
                const auto prev = previews.constFind(key);
                if (prev != previews.constEnd()) { tex = &prev.value(); u0 = v0 = 0.0; sub = 1.0; }
            }

            // Fetch only when the fallback is coarser than the screen. The
            // comparison is in DEVICE pixels: projected lengths are in Qt's
            // logical pixels, and on a 2x display a 64-pixel preview stretched
            // over 50 logical pixels is really covering 100, so comparing the
            // two directly left every Retina view undersampled.
            const double haveTexels = tex ? sub * tex->width() : 0.0;
            const double needTexels = (maxPieceEdgePx * n) * devicePixelRatio;
            // In the whole-sky view the AllSky previews are the whole point:
            // its 768 tiles at some 40 screen pixels each would otherwise be
            // fetched at full resolution, about 80 MB, for detail a couple of
            // pixels wide. A tile count would have caught a deep zoom too,
            // where the fetch is exactly what the user is waiting for.
            if (!previewOnly && haveTexels < needTexels)
                scheduleFetch(key);
        }

        if (!tex) {
            // Nothing to show here yet. Outline only: a filled placeholder is
            // an axis-aligned square far larger than the tile's diamond, so it
            // wipes out the neighbours already painted in this pass and the
            // view flashes black on every pan and zoom until the last tile
            // lands.
            if (!allValid) continue;
            const double tileAngDeg = 90.0 / (1 << key.order);
            const int sz = static_cast<int>(scalePixPerRad
                                            * qDegreesToRadians(tileAngDeg) * 0.92);
            if (sz < 2) continue;
            const QPointF centre = projectFn(td.raCenterDeg, td.decCenterDeg);
            if (!valid(centre)) continue;
            p.setPen(QPen(QColor(40, 60, 80, 90), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(centre.x() - sz * 0.5, centre.y() - sz * 0.5, sz, sz));
            continue;
        }

        const QPixmap &pm = *tex;
        const double texX = u0 * pm.width();
        const double texY = v0 * pm.height();
        const double texW = sub * pm.width();
        const double texH = sub * pm.height();
        if (texW <= 0.0 || texH <= 0.0) continue;

        for (int index : drawable) {
            const int i = index % n, j = index / n;
            const QPointF a = mesh[size_t(j)     * (n + 1) + i];
            const QPointF b = mesh[size_t(j)     * (n + 1) + i + 1];
            const QPointF c = mesh[size_t(j + 1) * (n + 1) + i + 1];
            const QPointF d = mesh[size_t(j + 1) * (n + 1) + i];

            // Endpoints rather than a fixed width, so the division cannot drop
            // a column between neighbouring pieces or at the end of a row.
            const double sx = texX + double(i)     * texW / n;
            const double sw = texX + double(i + 1) * texW / n - sx;
            const double sy = texY + double(j)     * texH / n;
            const double sh = texY + double(j + 1) * texH / n - sy;
            if (sw <= 0.0 || sh <= 0.0) continue;

            QPolygonF src;
            src << QPointF(sx, sy) << QPointF(sx + sw, sy)
                << QPointF(sx + sw, sy + sh) << QPointF(sx, sy + sh);

            QPointF quad[4] = { a, b, c, d };
            const QPointF ctr = (a + b + c + d) / 4.0;
            for (QPointF &q : quad) {
                const QPointF r = q - ctr;
                const double len = std::hypot(r.x(), r.y());
                if (len > 1e-9) q = ctr + r * ((len + kQuadGrowPx) / len);
            }
            QPolygonF dst;
            dst << quad[0] << quad[1] << quad[2] << quad[3];

            QTransform t;
            if (!QTransform::quadToQuad(src, dst, t))
                continue;   // degenerate quad; skipping beats pasting it somewhere wrong
            p.save();
            p.setTransform(t, true);
            // Draw only this piece of the texture, in place. Copying it out
            // first would allocate a pixmap per sub-quad per repaint, and
            // drawing the whole pixmap would spill an ancestor's texture over
            // four neighbours and collapse the mosaic.
            const QRectF piece(sx, sy, sw, sh);
            p.drawPixmap(piece, pm, piece);
            p.restore();
        }
    }

    p.setRenderHint(QPainter::Antialiasing, hadAntialiasing);
}

void HiPSViewportWidget::renderTiles(QPainter &p)
{
    if (m_currentTiles.empty()) return;
    const double spr = scalePixPerRad();
    const int w = width(), h = height();
    renderTilesImpl(p, m_currentTiles, m_tileCache, m_previewCache,
                    [this](const HiPSTileKey &k) { scheduleTileFetch(k); },
                    [this](double ra, double dec) { return project(ra, dec); },
                    spr, w, h, projectionSpanPx(), devicePixelRatioF(),
                    m_view.fovDeg > 90.0);
}

void HiPSViewportWidget::renderTilesSec(QPainter &p)
{
    if (m_currentTilesSec.empty()) return;
    const double spr = scalePixPerRad();
    const int w = width(), h = height();
    renderTilesImpl(p, m_currentTilesSec, m_tileCacheSec, m_previewCacheSec,
                    [this](const HiPSTileKey &k) { scheduleTileFetchSec(k); },
                    [this](double ra, double dec) { return project(ra, dec); },
                    spr, w, h, projectionSpanPx(), devicePixelRatioF(),
                    m_view.fovDeg > 90.0);
}

// ── Task 1 — Coordinate grid rendering ────────────────────────────────────────

void HiPSViewportWidget::renderGrid(QPainter &p) const
{
    // project() handles every projection now, so the graticule follows the
    // sphere in the all-sky ones too instead of being switched off past 90°.
    double spacing = gridSpacingDeg(m_view.fovDeg);
    if (spacing <= 0.0) return;

    const double ra0  = qDegreesToRadians(m_view.raDeg);
    const double dec0 = qDegreesToRadians(m_view.decDeg);
    const auto onScreen = [this](const QPointF &q) {
        return q.x() > -1e6 && q.x() >= 0 && q.x() <= width()
                            && q.y() >= 0 && q.y() <= height();
    };

    // ── What part of the sky is on screen ────────────────────────────────
    // Sampling the whole sphere at the field's own spacing was some 34 million
    // projections per repaint at a one-degree field, which is what made the
    // grid unusable when zoomed in.
    double decMin = 90.0, decMax = -90.0, dRaMin = 180.0, dRaMax = -180.0;
    bool fullLongitude = false;

    if (m_view.fovDeg > 90.0) {
        // The whole sphere can be on screen and the spacing is coarse here, so
        // there is nothing to gain by narrowing it — and a probe grid over a
        // viewport that is mostly outside the projection's outline would miss
        // most of the sky it is meant to find.
        decMin = -90.0; decMax = 90.0;
        fullLongitude = true;
    } else {
        constexpr int kProbe = 12;
        for (int j = 0; j <= kProbe; ++j) {
            for (int i = 0; i <= kProbe; ++i) {
                double ra = 0.0, dec = 0.0;
                unproject(QPointF(double(i) * width() / kProbe,
                                  double(j) * height() / kProbe), ra, dec);
                const double dRa = std::fmod(ra - m_view.raDeg + 540.0, 360.0) - 180.0;
                decMin = std::min(decMin, dec);   decMax = std::max(decMax, dec);
                dRaMin = std::min(dRaMin, dRa);   dRaMax = std::max(dRaMax, dRa);
            }
        }
        // A pole on screen means every meridian passes through the view, which
        // no range of sampled right ascensions can tell you: at the pole they
        // all meet, so the probe returns whatever few the samples happened to
        // land on and the rest of the sky would go undrawn.
        if (onScreen(project(m_view.raDeg,  90.0))) { decMax =  90.0; fullLongitude = true; }
        if (onScreen(project(m_view.raDeg, -90.0))) { decMin = -90.0; fullLongitude = true; }

        decMin = std::max(-90.0, decMin - spacing);
        decMax = std::min( 90.0, decMax + spacing);
        dRaMin -= spacing;
        dRaMax += spacing;
        if (dRaMax - dRaMin >= 360.0) fullLongitude = true;
    }
    if (fullLongitude) { dRaMin = -180.0; dRaMax = 180.0; }
    if (decMax <= decMin) return;

    // Meridians converge towards the poles, so a spacing chosen in right
    // ascension packs them tighter and tighter as the view climbs: at
    // Polaris a one-degree field spans nearly 60 degrees of RA and the sky
    // disappeared under a hatch of them. Space them by angular distance
    // instead, snapped to a power-of-two multiple so the values stay round.
    const double cosDec = std::cos(qDegreesToRadians(std::clamp(m_view.decDeg, -89.9, 89.9)));
    double raSpacing = spacing / std::max(0.02, cosDec);
    raSpacing = spacing * std::pow(2.0, std::ceil(std::log2(raSpacing / spacing)));
    raSpacing = std::min(raSpacing, 45.0);

    // Coarsen rather than truncate: cutting the loop short at one end drew a
    // grid over part of the view and left the rest bare.
    constexpr int kMaxLines = 200;
    while ((dRaMax - dRaMin) / raSpacing > kMaxLines) raSpacing *= 2.0;
    while ((decMax - decMin) / spacing   > kMaxLines) spacing   *= 2.0;

    const QPen gridPen(QColor(80, 160, 230, 110), 1.0);
    const QPen labelPen(QColor(160, 210, 255, 200), 1.0);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(QFont(u"Lato"_s, 8));

    // Where the projection wraps in longitude. Both sides of that seam are
    // perfectly valid points, so only the native longitude itself shows the
    // crossing — a distance threshold misses it wherever the seam narrows,
    // which it does towards the poles.
    const bool wrapsInLongitude = (m_projection == SkyProjection::Ait
                                   || m_projection == SkyProjection::Mol
                                   || m_projection == SkyProjection::Mer);
    // Zenithal equidistant has no longitude seam but does have an antipode,
    // where a curve leaves one edge of the disc and re-enters at the opposite
    // one. Both samples are valid, so only the size of the jump shows it.
    const double span = projectionSpanPx();
    const double jumpLimit = span > 0.0 ? span * 0.4
                                        : std::numeric_limits<double>::max();

    // One pass per curve: draw it and pick the label anchor as we go.
    struct Anchor { bool found = false; QPointF at; };
    constexpr int kCurveSamples = 256;

    const auto sweep = [&](bool meridian, double fixed,
                           double from, double to,
                           const std::function<bool(const QPointF &)> &wantsLabel,
                           Anchor &anchor) {
        QPainterPath path;
        bool inPath = false;
        double prevLon = 0.0;
        QPointF prev;
        for (int k = 0; k <= kCurveSamples; ++k) {
            const double t = from + (to - from) * k / kCurveSamples;
            const double ra  = meridian ? fixed : t;
            const double dec = meridian ? t : fixed;
            const QPointF sp = project(ra, dec);

            const NativeVec v = toNative(ra, dec, ra0, dec0);
            const double lon = std::atan2(v.x, v.z);

            if (sp.x() < -1e6) { inPath = false; prevLon = lon; continue; }
            if (inPath
                && ((wrapsInLongitude && std::abs(lon - prevLon) > M_PI)
                    || std::hypot(sp.x() - prev.x(), sp.y() - prev.y()) > jumpLimit))
                inPath = false;                      // crossed the seam or the antipode
            if (!inPath) { path.moveTo(sp); inPath = true; }
            else          { path.lineTo(sp); }
            prevLon = lon;
            prev = sp;

            if (!anchor.found && wantsLabel(sp)) { anchor.found = true; anchor.at = sp; }
        }
        if (!path.isEmpty()) {
            p.setPen(gridPen);
            p.drawPath(path);
        }
    };

    // ── Meridians (constant RA), labelled along the bottom ───────────────
    const double raStart = std::ceil((m_view.raDeg + dRaMin) / raSpacing) * raSpacing;
    const double raEnd   = m_view.raDeg + dRaMax;
    double lastLabelX = -1e9;
    for (double ra = raStart; ra <= raEnd; ra += raSpacing) {
        Anchor a;
        sweep(true, ra, decMin, decMax,
              [this](const QPointF &sp) {
                  return sp.x() >= 4 && sp.x() <= width() - 70
                      && sp.y() >= height() - 44 && sp.y() <= height() - 6;
              }, a);
        // Skip a label that would land on top of the previous one — near a
        // pole the meridians crowd together and the labels ran into an
        // unreadable smear along the bottom of the view.
        // Absolute distance: right ascension normally increases to the LEFT,
        // so a signed comparison rejected every label after the first.
        if (a.found && std::abs(a.at.x() - lastLabelX) > 90.0) {
            lastLabelX = a.at.x();
            p.setPen(labelPen);
            p.drawText(a.at + QPointF(3, -4), formatRa(normaliseRa(ra)));
        }
    }

    // ── Parallels (constant Dec), labelled down the left edge ────────────
    const double decStart = std::ceil(decMin / spacing) * spacing;
    double lastLabelY = -1e9;
    for (double dec = decStart; dec <= decMax + 1e-9; dec += spacing) {
        // The poles are a point, not a circle; everything short of them is a
        // perfectly good parallel, including the 89-degree one.
        if (std::abs(dec) >= 90.0 - 1e-6) continue;
        Anchor a;
        sweep(false, dec, m_view.raDeg + dRaMin, m_view.raDeg + dRaMax,
              [this](const QPointF &sp) {
                  return sp.x() >= 4 && sp.x() <= 70
                      && sp.y() >= 12 && sp.y() <= height() - 4;
              }, a);
        if (a.found && std::abs(a.at.y() - lastLabelY) > 22.0) {
            lastLabelY = a.at.y();
            p.setPen(labelPen);
            p.drawText(a.at + QPointF(3, -3), formatDec(dec));
        }
    }
}

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
    // The radius the overlay is fetched for depends on the aspect ratio, so a
    // taller window reveals sky the last query did not cover.
    if (!m_cataloguePath.isEmpty()) m_catalogueRefreshTimer->start();
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

        // RA increases to the LEFT on screen (see project()), so raising the
        // centre's RA moves the sky right: a rightward drag needs a POSITIVE
        // dRa. The minus that used to be here belonged to the old, mirrored
        // projection and would have inverted horizontal panning.
        const double cosDec = std::cos(qDegreesToRadians(m_panStartView.decDeg));
        const double dRa  = qRadiansToDegrees(dx / scale) / std::max(0.01, cosDec);
        const double dDec = qRadiansToDegrees(dy / scale);

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
    // Through setFov, so the wheel obeys the same per-projection limit as the
    // buttons: clamping to a hard 180 here stopped the all-sky projections
    // short of the whole sky and let gnomonic past the point it diverges.
    const double newFov = std::clamp(m_view.fovDeg * factor,
                                     0.01, projectionMaxFovDeg(m_projection));
    if (qFuzzyCompare(newFov, m_view.fovDeg)) return;
    setFov(newFov);
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
