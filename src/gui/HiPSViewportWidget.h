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
// HiPSViewportWidget
//
// A QWidget that renders a HiPS (Hierarchical Progressive Survey) sky map
// using QPainter with gnomonic (tangential) projection.
//
// Tile images are fetched from the backend asynchronously via a bounded
// concurrent-fetch queue (QtConcurrent::run + QFutureWatcher).  The widget
// never contacts remote servers directly.
//
// Architecture
// ┌─────────────────────────────────────────────┐
// │ HiPSViewportWidget                          │
// │  ┌─────────────┐   ┌─────────────────────┐ │
// │  │ ViewState   │   │ TileCache (QHash)   │ │
// │  │ raDeg/decDeg│   │ TileKey→QPixmap     │ │
// │  │ fovDeg      │   └─────────────────────┘ │
// │  └─────────────┘                            │
// │  TileListWatcher  → query_tiles backend     │
// │  TileFetchWatchers→ tile/{order}/{pix}      │
// └─────────────────────────────────────────────┘
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
    explicit HiPSViewportWidget(QWidget *parent = nullptr);
    ~HiPSViewportWidget() override;

    // Call once after openHiPS() succeeds.
    void setSurvey(const BackendHiPSSurveyInfo &survey,
                   const QString &backendUrl, const QString &backendToken);

    void clearSurvey();

    // View control (also triggered by mouse/wheel internally)
    void setCenter(double raDeg, double decDeg);
    void setFov(double fovDeg);
    void resetView();

    double centerRa()  const { return m_view.raDeg; }
    double centerDec() const { return m_view.decDeg; }
    double fov()       const { return m_view.fovDeg; }
    int    currentOrder() const;

signals:
    void cursorCoordinatesChanged(double raDeg, double decDeg);
    void centerChanged(double raDeg, double decDeg);
    void fovChanged(double fovDeg);
    void orderChanged(int order);
    void loadingChanged(bool loading);

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
        double fovDeg = 60.0;  // full-width FOV in degrees
    };

    // ── Projection ────────────────────────────────────────────────────────
    QPointF project(double raDeg, double decDeg) const;
    void    unproject(QPointF screen, double &raDeg, double &decDeg) const;
    double  scalePixPerRad() const;

    // ── Tile management ──────────────────────────────────────────────────
    void onViewChanged();           // debounced — triggers tile-list query
    void requestTileList();         // async: calls backend query_tiles
    void scheduleTileFetch(const HiPSTileKey &key);
    void drainFetchQueue();
    void requestAllsky();           // async: download AllSky background

    // ── Rendering ─────────────────────────────────────────────────────────
    void renderBackground(QPainter &p) const;
    void renderAllsky(QPainter &p) const;
    void renderTiles(QPainter &p);
    void renderCrosshair(QPainter &p) const;
    void renderNoSurvey(QPainter &p) const;

    // ── Survey state ──────────────────────────────────────────────────────
    BackendHiPSSurveyInfo m_survey;
    bool m_hasSurvey{ false };
    QString m_backendUrl;
    QString m_backendToken;

    // ── View state ────────────────────────────────────────────────────────
    ViewState m_view;

    // ── Pan interaction ───────────────────────────────────────────────────
    bool      m_panning{ false };
    QPointF   m_panStartPos;
    ViewState m_panStartView;

    // ── AllSky background ─────────────────────────────────────────────────
    QPixmap m_allskyPixmap;
    bool    m_allskyLoading{ false };
    QFutureWatcher<QByteArray> m_allskyWatcher;

    // ── Tile list (from query_tiles) ──────────────────────────────────────
    std::vector<HiPSTileDescriptor> m_currentTiles;
    QFutureWatcher<BackendHiPSViewResponse> m_tileListWatcher;

    // ── Tile fetch queue ─────────────────────────────────────────────────
    static constexpr int kMaxConcurrent = 6;
    QHash<HiPSTileKey, QPixmap> m_tileCache;
    QSet<HiPSTileKey> m_pendingKeys;
    QList<HiPSTileKey> m_fetchQueue;
    QList<QFutureWatcher<HiPSTileFetchResult> *> m_activeWatchers;

    // ── View-change debounce ─────────────────────────────────────────────
    QTimer *m_viewChangeTimer{ nullptr };
};

#endif // HiPSViewportWidget_h
