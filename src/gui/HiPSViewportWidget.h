#ifndef HiPSViewportWidget_h
#define HiPSViewportWidget_h

#include "app/BackendClient.h"

#include <QFutureWatcher>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <array>
#include <vector>

// ---------------------------------------------------------------------------
// HiPSViewportWidget — Milestone 2
//
// Extends the MVP viewer with:
//   • Coordinate grid overlay (Task 1)
//   • Secondary survey (alpha blending / side-by-side) (Task 2)
//   • Catalogue overlay — marker rendering for sky sources (Task 3)
//   (Target resolver lives in HiPSWindow; the viewport only exposes setCenter)
// ---------------------------------------------------------------------------

struct HiPSTileKey
{
    int order;
    int pix;

    bool operator==(const HiPSTileKey &o) const noexcept
    {
        return order == o.order && pix == o.pix;
    }
};

inline size_t qHash(const HiPSTileKey &k, size_t seed = 0) noexcept
{
    return qHashMulti(seed, k.order, k.pix);
}

struct HiPSTileDescriptor
{
    HiPSTileKey key;
    double raCenterDeg{ 0.0 };
    double decCenterDeg{ 0.0 };
    std::array<double, 4> cornersRaDeg{};
    std::array<double, 4> cornersDecDeg{};
};

struct HiPSTileFetchResult
{
    HiPSTileKey key;
    QByteArray imageData;
    QString error;
};

class HiPSViewportWidget : public QWidget
{
    Q_OBJECT

public:
    // ── Blend mode for secondary survey ──────────────────────────────────────
    enum class BlendMode { Alpha, SideBySide };

    explicit HiPSViewportWidget(QWidget *parent = nullptr);
    ~HiPSViewportWidget() override;

    // ── Primary survey ────────────────────────────────────────────────────────
    void setSurvey(const BackendHiPSSurveyInfo &survey,
                   const QString &backendUrl, const QString &backendToken);
    void clearSurvey();

    // View control
    void setCenter(double raDeg, double decDeg);
    void setFov(double fovDeg);
    void resetView();

    double centerRa()  const { return m_view.raDeg; }
    double centerDec() const { return m_view.decDeg; }
    double fov()       const { return m_view.fovDeg; }
    int    currentOrder() const;

    // ── Task 1 — Coordinate grid ──────────────────────────────────────────────
    void setGridVisible(bool visible);
    bool isGridVisible() const { return m_showGrid; }

    // ── Task 2 — Secondary survey (blending / side-by-side) ──────────────────
    void setSecondarySurvey(const BackendHiPSSurveyInfo &survey,
                            const QString &backendUrl, const QString &backendToken);
    void clearSecondarySurvey();
    bool hasSecondarySurvey() const { return m_hasSurveySec; }
    void setOverlayOpacity(double opacity);
    double overlayOpacity() const { return m_overlayOpacity; }
    void setBlendMode(BlendMode mode);
    BlendMode blendMode() const { return m_blendMode; }

    // ── Task 3 — Catalogue overlay ────────────────────────────────────────────
    void setCatalogueOverlay(const QString &cataloguePath,
                             const QString &raField = {},
                             const QString &decField = {});
    void clearCatalogueOverlay();
    void setCatalogueOverlayVisible(bool visible);
    bool hasCatalogueOverlay() const { return !m_cataloguePath.isEmpty(); }
    bool isCatalogueOverlayVisible() const { return m_showCatalogue; }

signals:
    void cursorCoordinatesChanged(double raDeg, double decDeg);
    void centerChanged(double raDeg, double decDeg);
    void fovChanged(double fovDeg);
    void orderChanged(int order);
    void loadingChanged(bool loading);
    // Emitted when catalogue overlay is refreshed (totalInView, truncated)
    void catalogueOverlayRefreshed(int totalInView, bool truncated);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    struct ViewState {
        double raDeg  = 180.0;
        double decDeg = 0.0;
        double fovDeg = 60.0;
    };

    // ── Projection ────────────────────────────────────────────────────────────
    QPointF project(double raDeg, double decDeg) const;
    void    unproject(QPointF screen, double &raDeg, double &decDeg) const;
    double  scalePixPerRad() const;

    // ── Primary tile management ───────────────────────────────────────────────
    void onViewChanged();
    void requestTileList();
    void scheduleTileFetch(const HiPSTileKey &key);
    void drainFetchQueue();
    void requestAllsky();

    // ── Secondary tile management (Task 2) ────────────────────────────────────
    void requestTileListSec();
    void scheduleTileFetchSec(const HiPSTileKey &key);
    void drainFetchQueueSec();
    void requestAllskySec();

    // ── Rendering ─────────────────────────────────────────────────────────────
    void renderBackground(QPainter &p) const;
    void renderAllsky(QPainter &p) const;
    void renderTiles(QPainter &p);
    void renderAllskySec(QPainter &p) const;   // Task 2
    void renderTilesSec(QPainter &p);           // Task 2
    void renderGrid(QPainter &p) const;         // Task 1
    void renderCatalogueOverlay(QPainter &p) const; // Task 3
    void renderCrosshair(QPainter &p) const;
    void renderNoSurvey(QPainter &p) const;

    // ── Survey state (primary) ────────────────────────────────────────────────
    BackendHiPSSurveyInfo m_survey;
    bool    m_hasSurvey{ false };
    QString m_backendUrl;
    QString m_backendToken;

    // ── View state ────────────────────────────────────────────────────────────
    ViewState m_view;

    // ── Pan interaction ───────────────────────────────────────────────────────
    bool      m_panning{ false };
    QPointF   m_panStartPos;
    ViewState m_panStartView;

    // ── AllSky (primary) ──────────────────────────────────────────────────────
    QPixmap m_allskyPixmap;
    bool    m_allskyLoading{ false };
    QFutureWatcher<QByteArray> m_allskyWatcher;

    // ── Tile list + cache (primary) ───────────────────────────────────────────
    std::vector<HiPSTileDescriptor> m_currentTiles;
    QFutureWatcher<BackendHiPSViewResponse> m_tileListWatcher;
    static constexpr int kMaxConcurrent = 6;
    QHash<HiPSTileKey, QPixmap> m_tileCache;
    QSet<HiPSTileKey>  m_pendingKeys;
    QList<HiPSTileKey> m_fetchQueue;
    QList<QFutureWatcher<HiPSTileFetchResult> *> m_activeWatchers;

    // ── Task 1 — Grid ─────────────────────────────────────────────────────────
    bool m_showGrid{ false };

    // ── Task 2 — Secondary survey ─────────────────────────────────────────────
    BackendHiPSSurveyInfo m_surveySec;
    bool    m_hasSurveySec{ false };
    QString m_backendUrlSec;
    QString m_backendTokenSec;

    QPixmap m_allskyPixmapSec;
    bool    m_allskyLoadingSec{ false };
    QFutureWatcher<QByteArray> m_allskyWatcherSec;

    std::vector<HiPSTileDescriptor> m_currentTilesSec;
    QFutureWatcher<BackendHiPSViewResponse> m_tileListWatcherSec;
    QHash<HiPSTileKey, QPixmap> m_tileCacheSec;
    QSet<HiPSTileKey>  m_pendingKeysSec;
    QList<HiPSTileKey> m_fetchQueueSec;
    QList<QFutureWatcher<HiPSTileFetchResult> *> m_activeWatchersSec;

    double    m_overlayOpacity{ 0.5 };
    BlendMode m_blendMode{ BlendMode::Alpha };

    // ── Task 3 — Catalogue overlay ────────────────────────────────────────────
    QString m_cataloguePath;
    QString m_catalogueRaField;
    QString m_catalogueDecField;
    bool    m_showCatalogue{ true };
    QVector<BackendHiPSCatalogueSource> m_catalogueSources;
    QFutureWatcher<BackendHiPSCatalogueOverlayResult> m_catalogueWatcher;
    QTimer *m_catalogueRefreshTimer{ nullptr };

    // ── Shared debounce timer ─────────────────────────────────────────────────
    QTimer *m_viewChangeTimer{ nullptr };
};

#endif // HiPSViewportWidget_h
