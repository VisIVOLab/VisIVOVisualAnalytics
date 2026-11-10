#ifndef RAYTRACEOUTPUT_H
#define RAYTRACEOUTPUT_H

// The pictures of a ray-traced view (issues #101, #106): a still or an orbit
// movie of it, rendered by the backend (/v1/tasks/particle_render, or
// /v1/tasks/volume_render for a cube) at up to 8K, graded
// again without rendering it again, saved as PNG, as the light before the tone
// curve (FITS), or encoded as an MP4. The box and colour bar shown over the
// view are drawn onto them. Lives in the point viewer's Ray-traced card.

#include "RaytraceOverlay.h"
#include "app/TaskQuitDrain.h"

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QSize>
#include <QWidget>

#include <functional>
#include <memory>

class QComboBox;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QProgressBar;
class QPushButton;
class QTemporaryDir;
class QTimer;

namespace visivo {
class RenderTaskHandle;
}

class RaytraceOutput : public QWidget
{
    Q_OBJECT

public:
    struct Source
    {
        QString backendUrl;
        QString token;
        QString sessionId;
        /// The render request without cameras, size and tone: the view's
        /// state and the settings.
        std::function<QJsonObject()> request;
        /// The tone fields over an exposure base (0: the backend's automatic one).
        std::function<QJsonObject(double)> tone;
        /// The view's camera now.
        std::function<QJsonObject()> camera;
        /// The view's automatic exposure (a movie without a still takes it).
        std::function<double()> viewExposure;
        /// The box and colour bar over the view.
        std::function<visivo::RaytraceOverlay()> overlay;
        /// The view's shape, for the "this view" size.
        std::function<QSize()> viewSize;
        /// The task a render is: particles, or a cube (issue #106). Its
        /// frames, tone and light are served at /v1/particle_render/{id}/….
        QString taskPath = QStringLiteral("/v1/tasks/particle_render");
    };

    explicit RaytraceOutput(Source source, QWidget *parent = nullptr);
    ~RaytraceOutput() override;

    /// The tone changed: a still on show is graded again.
    void toneChanged();
    /// The view was turned: the still on show is put away.
    void stillDismissed();
    bool showingStill() const { return m_showingStill; }

signals:
    /// A still (graded, with its overlay) to show in place of the view.
    void showStill(const QImage &image);
    /// A line for the status bar.
    void message(const QString &text);

private:
    enum class Stage { Idle, Rendering, Downloading, Encoding };

    QSize imageSize() const;
    void renderStill();
    void renderMovie();
    void submit(const QList<QJsonObject> &cameras, bool movie);
    void poll();
    void finished(const QJsonObject &result);
    void regrade();
    void fetchFrame(int index);
    void encode();
    void saveImage();
    void saveLight();
    void cancel();
    void forgetTask();
    void setStage(Stage stage, const QString &message = QString());
    /// ``png`` with the box and colour bar of ``camera``'s view drawn on it.
    QImage annotated(const QByteArray &png, const QJsonObject &camera) const;
    QNetworkReply *send(const QString &verb, const QString &path, const QByteArray &body = {});

    Source m_source;
    QNetworkAccessManager *m_nam{ nullptr };
    QTimer *m_pollTimer{ nullptr };
    QTimer *m_regradeTimer{ nullptr };
    Stage m_stage{ Stage::Idle };

    /// The task held, shared with the worker creating it and the quit drain,
    /// which cancels it (#149); registered with the drain for this widget's life.
    std::shared_ptr<visivo::RenderTaskHandle> m_task;
    visivo::TaskQuitDrain::Registration m_quitRegistration;
    QString m_taskId;               ///< m_task's, once its create has answered
    quint64 m_generation{ 0 };      ///< the task's: a reply of an earlier one is dropped
    quint64 m_taskGeneration{ 0 };  ///< m_task's, for the create under way
    bool m_taskIsMovie{ false };
    double m_autoExposure{ 0.0 };   ///< the still's automatic exposure, 0 before one
    QList<QJsonObject> m_cameras;   ///< of the frames rendered
    visivo::RaytraceOverlay m_overlay;   ///< as shown when the render was asked for
    QImage m_still;                 ///< the still as graded and annotated now
    bool m_showingStill{ false };
    int m_frames{ 0 };
    int m_fps{ 30 };
    QString m_moviePath;
    QTemporaryDir *m_frameDir{ nullptr };
    QPointer<QNetworkReply> m_regradeReply;
    QPointer<QProcess> m_ffmpeg;

    QComboBox *m_resolution{ nullptr };
    QPushButton *m_stillButton{ nullptr };
    QPushButton *m_movieButton{ nullptr };
    QPushButton *m_saveImageButton{ nullptr };
    QPushButton *m_saveLightButton{ nullptr };
    QPushButton *m_cancelButton{ nullptr };
    QProgressBar *m_progress{ nullptr };
};

#endif // RAYTRACEOUTPUT_H
