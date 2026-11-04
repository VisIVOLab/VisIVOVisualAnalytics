#ifndef RAYTRACEVIEWPORT_H
#define RAYTRACEVIEWPORT_H

// A ray-traced view on this machine's GPU (issue #101), for the point viewer's
// "Ray-traced" shape and the cube viewer's "Ray-traced" style (issue #106):
// what the backend prepared (particles, /v1/particle_render/particles, or a
// cube's grid, /v1/volume_render/grid), fetched again only when what shapes it
// changes, drawn by ParticleGLView, turned with the mouse as the 3-D views are
// (drag: turn; Shift-drag or right button: move; wheel: zoom).

#include "ParticleGLView.h"

#include <QImage>
#include <QJsonObject>
#include <QPoint>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <utility>
#include <vector>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QStackedLayout;
class QTimer;

class RaytraceViewport : public QWidget
{
    Q_OBJECT

public:
    struct Source
    {
        QString backendUrl;
        QString token;
        QString sessionId;
        /// The render request without cameras (the view's state and the
        /// settings): what the particles are prepared from.
        std::function<QJsonObject()> request;
        /// The GPU tone of the settings.
        std::function<ParticleGLView::Tone()> tone;
        /// The tone fields over an exposure base, for the backend's frames
        /// when this machine has no GPU view.
        std::function<QJsonObject(double)> toneJson;
        /// A cube's grid instead of particles.
        bool grid = false;
        /// What the GPU view is drawn from, and the backend's frame without
        /// one (empty: none, the view says so).
        QString dataPath = QStringLiteral("/v1/particle_render/particles");
        QString previewPath = QStringLiteral("/v1/particle_render/preview");
        /// What in a request shapes the data: a new key, a new fetch.
        std::function<QString(const QJsonObject &)> key;
    };

    explicit RaytraceViewport(Source source, QWidget *parent = nullptr);

    void setCamera(const QJsonObject &camera);
    QJsonObject camera() const { return m_camera; }
    /// The request or the settings changed: fetches the particles when what
    /// shapes them changed, else draws them again.
    void refresh();
    /// Only the tone changed.
    void retone();
    double autoExposure() const;
    void setOverlayShown(bool box, bool colourbar);
    /// The box and colour bar over the view: the GPU view's, or without one
    /// a grid's from the backend's frames.
    visivo::RaytraceOverlay overlay() const;
    /// A picture (a still) in place of the view, until it is turned.
    void showStill(const QImage &image);
    /// A grid's colour table and its quantity's range, as the backend made
    /// them (the GPU tone's colours and colour bar).
    const std::vector<QVector3D> &gridLut() const { return m_gridLut; }
    std::pair<double, double> gridRange() const { return m_gridRange; }
    bool gridLog() const { return m_gridLog; }

signals:
    /// A line for the status bar.
    void message(const QString &text);
    /// The still shown was put away (the view was turned).
    void stillDismissed();
    /// A grid arrived: its colour table and range (gridLut, gridRange).
    void gridReady();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void fetch(const QString &key, const QJsonObject &request);
    void redraw(bool settle);
    void showImage(const QImage &image);
    void showGl();
    /// No GPU view: a frame of the backend's (/v1/particle_render/preview).
    void fallbackFrame();
    /// The backend's last frame, with the overlay drawn over it.
    void showFallback();
    bool takeGrid(QNetworkReply *reply);
    /// A grid's box and colours from a backend frame's headers, read with the
    /// settings the frame was asked with.
    void takeFallbackGrid(QNetworkReply *reply, bool projected,
                          const ParticleGLView::Tone &tone);

    Source m_source;
    ParticleGLView *m_gl{ nullptr };
    QNetworkAccessManager *m_nam{ nullptr };
    QTimer *m_settle{ nullptr };
    QJsonObject m_camera;
    QString m_key;          ///< the particles on the GPU
    QString m_loadingKey;   ///< the particles being fetched
    QPointer<QNetworkReply> m_reply;
    QPoint m_dragFrom;
    bool m_failed{ false };
    QStackedLayout *m_pages{ nullptr };
    QLabel *m_image{ nullptr };     ///< a still, or the backend's frame without a GPU view
    bool m_showingStill{ false };
    QImage m_shown;
    QPointer<QNetworkReply> m_fallbackReply;
    bool m_fallbackPending{ false };
    std::vector<QVector3D> m_gridLut;
    std::pair<double, double> m_gridRange{ 0.0, 1.0 };
    bool m_gridLog{ false };
    // Without a GPU view: the backend's last frame, its camera and overlay,
    // and what the overlay shows.
    QImage m_fallbackImage;
    QJsonObject m_fallbackCamera;
    visivo::RaytraceOverlay m_fallbackOverlay;
    bool m_showBox{ true };
    bool m_showBar{ true };
};

#endif // RAYTRACEVIEWPORT_H
