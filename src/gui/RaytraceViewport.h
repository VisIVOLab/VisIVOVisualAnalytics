#ifndef RAYTRACEVIEWPORT_H
#define RAYTRACEVIEWPORT_H

// A ray-traced particle view on this machine's GPU (issue #101), for the
// point viewer's "Ray-traced" shape: the particles the backend prepared
// (/v1/particle_render/particles), fetched again only when what shapes them
// changes, drawn by ParticleGLView, turned with the mouse as the 3-D views
// are (drag: turn; Shift-drag or right button: move; wheel: zoom).

#include "ParticleGLView.h"

#include <QImage>
#include <QJsonObject>
#include <QPoint>
#include <QPointer>
#include <QWidget>

#include <functional>

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
    visivo::RaytraceOverlay overlay() const { return m_gl->overlay(); }
    /// A picture (a still) in place of the view, until it is turned.
    void showStill(const QImage &image);

signals:
    /// A line for the status bar.
    void message(const QString &text);
    /// The still shown was put away (the view was turned).
    void stillDismissed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void fetch(const QString &key, const QJsonObject &request);
    void redraw(bool settle);
    void showImage(const QImage &image);
    void showGl();
    /// No GPU view: a frame of the backend's (/v1/particle_render/preview).
    void fallbackFrame();

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
};

#endif // RAYTRACEVIEWPORT_H
