#include "RaytraceOutput.h"

#include "OrbitPath.h"
#include "app/BackendClient.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace {

/// A manager owned by the application, for the requests that make and delete
/// renders: the widget may be going, and a render made for nobody would run on.
QNetworkAccessManager *appManager()
{
    static QNetworkAccessManager *nam = new QNetworkAccessManager(QCoreApplication::instance());
    return nam;
}

/// Removes a render's files on the backend (and stops it, if it still runs).
void deleteRender(const QString &backendUrl, const QString &token, const QString &sessionId,
                  const QString &taskId)
{
    QNetworkAccessManager *nam = appManager();
    QUrl url(backendUrl);
    url.setPath(u"/v1/particle_render/"_s + taskId);
    QNetworkRequest req(url);
    // The token BackendClient resolves: with the local backend the window may
    // hold none, and it is in ~/.visivo_token.
    BackendClient::applySessionHeaders(req, backendUrl, BackendClient(backendUrl, token).token(),
                                       sessionId);
    QNetworkReply *reply = nam->deleteResource(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

QString errorOf(QNetworkReply *reply)
{
    const QString detail =
            QJsonDocument::fromJson(reply->readAll()).object().value(u"detail"_s).toString();
    return detail.isEmpty() ? reply->errorString() : detail;
}

QString ffmpegPath()
{
    // An application started from the Finder has a short PATH: look where
    // Homebrew and MacPorts put it too.
    QString path = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (path.isEmpty())
        path = QStandardPaths::findExecutable(
                u"ffmpeg"_s, { u"/opt/homebrew/bin"_s, u"/usr/local/bin"_s, u"/opt/local/bin"_s });
    return path;
}

} // namespace

RaytraceOutput::RaytraceOutput(Source source, QWidget *parent)
    : QWidget(parent), m_source(std::move(source)), m_nam(new QNetworkAccessManager(this))
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(500);
    connect(m_pollTimer, &QTimer::timeout, this, &RaytraceOutput::poll);
    m_regradeTimer = new QTimer(this);
    m_regradeTimer->setSingleShot(true);
    m_regradeTimer->setInterval(200);
    connect(m_regradeTimer, &QTimer::timeout, this, &RaytraceOutput::regrade);

    m_resolution = new QComboBox;
    m_resolution->addItem(u"1920 × 1080"_s, QSize(1920, 1080));
    m_resolution->addItem(u"3840 × 2160 (4K)"_s, QSize(3840, 2160));
    m_resolution->addItem(u"7680 × 4320 (8K)"_s, QSize(7680, 4320));
    m_resolution->addItem(u"2048 × 2048"_s, QSize(2048, 2048));
    m_resolution->addItem(u"The view's shape"_s, QSize());
    m_resolution->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_resolution->setMinimumContentsLength(4);
    m_resolution->setToolTip(u"The size of a still or a movie's frames; the view's shape is "
                             u"1920 pixels wide"_s);

    m_stillButton = new QPushButton(u"Render Still"_s);
    m_stillButton->setToolTip(u"The view, rendered by the backend at the size chosen, with "
                              u"absorption; shown in place of the view until it is turned"_s);
    m_movieButton = new QPushButton(u"Movie…"_s);
    m_movieButton->setToolTip(u"An orbit about the view's up axis, encoded as MP4 with ffmpeg "
                              u"(or saved as PNG frames)"_s);
    m_saveImageButton = new QPushButton(u"Save Image…"_s);
    m_saveLightButton = new QPushButton(u"Save Light…"_s);
    m_saveLightButton->setToolTip(u"The still's light before the tone curve, a floating-point "
                                  u"FITS, to grade it elsewhere"_s);
    m_cancelButton = new QPushButton(u"Cancel"_s);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(8);
    connect(m_stillButton, &QPushButton::clicked, this, &RaytraceOutput::renderStill);
    connect(m_movieButton, &QPushButton::clicked, this, &RaytraceOutput::renderMovie);
    connect(m_saveImageButton, &QPushButton::clicked, this, &RaytraceOutput::saveImage);
    connect(m_saveLightButton, &QPushButton::clicked, this, &RaytraceOutput::saveLight);
    connect(m_cancelButton, &QPushButton::clicked, this, &RaytraceOutput::cancel);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(u"Size"_s, m_resolution);
    auto *buttons = new QGridLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addWidget(m_stillButton, 0, 0);
    buttons->addWidget(m_movieButton, 0, 1);
    buttons->addWidget(m_saveImageButton, 1, 0);
    buttons->addWidget(m_saveLightButton, 1, 1);
    auto *busy = new QHBoxLayout;
    busy->addWidget(m_progress, 1);
    busy->addWidget(m_cancelButton);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addLayout(busy);
    setStage(Stage::Idle);
}

RaytraceOutput::~RaytraceOutput()
{
    if (m_ffmpeg)
        m_ffmpeg->kill();
    forgetTask();
}

QSize RaytraceOutput::imageSize() const
{
    const QSize size = m_resolution->currentData().toSize();
    if (size.isValid())
        return size;
    const QSize view = m_source.viewSize ? m_source.viewSize() : QSize(16, 9);
    const int height = view.width() > 0 ? 1920 * view.height() / view.width() : 1080;
    return { 1920, std::clamp(height & ~1, 16, 8192) };
}

QNetworkReply *RaytraceOutput::send(const QString &verb, const QString &path,
                                    const QByteArray &body, QNetworkAccessManager *nam)
{
    if (!nam)
        nam = m_nam;
    QUrl url(m_source.backendUrl);
    url.setPath(path);
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(req, m_source.backendUrl,
                                       BackendClient(m_source.backendUrl, m_source.token).token(),
                                       m_source.sessionId);
    if (!body.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    if (verb == u"GET"_s)
        return nam->get(req);
    if (verb == u"DELETE"_s)
        return nam->deleteResource(req);
    return nam->post(req, body);
}

QImage RaytraceOutput::annotated(const QByteArray &png, const QJsonObject &camera) const
{
    QImage image;
    image.loadFromData(png, "PNG");
    if (image.isNull())
        return image;
    image = image.convertToFormat(QImage::Format_RGB32);
    QPainter painter(&image);
    visivo::paintRaytraceOverlay(painter, QRectF(image.rect()), camera, m_overlay);
    return image;
}

void RaytraceOutput::renderStill()
{
    const QJsonObject camera = m_source.camera ? m_source.camera() : QJsonObject();
    if (camera.isEmpty()) {
        emit message(u"No camera yet: the ray-traced view is still starting."_s);
        return;
    }
    submit({ camera }, false);
}

void RaytraceOutput::renderMovie()
{
    const QJsonObject camera = m_source.camera ? m_source.camera() : QJsonObject();
    if (camera.isEmpty()) {
        emit message(u"No camera yet: the ray-traced view is still starting."_s);
        return;
    }
    // The orbit: asked here, only when a movie is made.
    static double turn = 360.0, seconds = 12.0;
    static int fps = 30;
    QDialog ask(this);
    ask.setWindowTitle(u"Movie"_s);
    auto *form = new QFormLayout(&ask);
    auto *turnBox = new QDoubleSpinBox;
    turnBox->setRange(-1080.0, 1080.0);
    turnBox->setValue(turn);
    turnBox->setSuffix(u"°"_s);
    turnBox->setToolTip(u"About the view's up axis"_s);
    auto *secondsBox = new QDoubleSpinBox;
    secondsBox->setRange(0.5, 600.0);
    secondsBox->setValue(seconds);
    secondsBox->setSuffix(u" s"_s);
    auto *fpsBox = new QSpinBox;
    fpsBox->setRange(12, 120);
    fpsBox->setValue(fps);
    fpsBox->setSuffix(u" fps"_s);
    form->addRow(u"Turn"_s, turnBox);
    form->addRow(u"Length"_s, secondsBox);
    form->addRow(u"Rate"_s, fpsBox);
    auto *note = new QLabel(ffmpegPath().isEmpty()
                                    ? u"ffmpeg not found: the frames are saved as PNGs."_s
                                    : u"Encoded as MP4 (H.264) with ffmpeg."_s);
    note->setWordWrap(true);
    form->addRow(note);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText(u"Render…"_s);
    connect(box, &QDialogButtonBox::accepted, &ask, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &ask, &QDialog::reject);
    form->addRow(box);
    if (ask.exec() != QDialog::Accepted)
        return;
    turn = turnBox->value();
    seconds = secondsBox->value();
    fps = fpsBox->value();

    const QString ffmpeg = ffmpegPath();
    QString target;
    if (!ffmpeg.isEmpty()) {
        target = QFileDialog::getSaveFileName(this, u"Save Movie"_s,
                                              QDir::home().filePath(u"render.mp4"_s),
                                              u"MP4 video (*.mp4)"_s);
        if (!target.isEmpty() && !target.endsWith(u".mp4"_s, Qt::CaseInsensitive))
            target += u".mp4"_s;
    } else {
        target = QFileDialog::getExistingDirectory(this, u"Folder for the Movie's Frames"_s);
    }
    if (target.isEmpty())
        return;
    m_moviePath = target;
    m_fps = fps;
    const int frames = std::max(1, static_cast<int>(std::lround(seconds * fps)));
    submit(visivo::orbitCameras(camera, frames, turn), true);
}

void RaytraceOutput::submit(const QList<QJsonObject> &cameras, bool movie)
{
    // A movie keeps the still's automatic exposure (its grade), else the
    // view's; a new still finds its own.
    const double keepExposure = movie ? m_autoExposure : 0.0;
    forgetTask();
    m_autoExposure = keepExposure;
    m_taskIsMovie = movie;
    m_cameras = cameras;
    m_overlay = m_source.overlay ? m_source.overlay() : visivo::RaytraceOverlay();
    QJsonObject req = m_source.request ? m_source.request() : QJsonObject();
    const QSize px = imageSize();
    // The bar as large, against the picture, as it is against the view, for
    // every frame of the render whatever the window does meanwhile.
    const QSize shown = m_source.viewSize ? m_source.viewSize() : QSize();
    if (shown.height() > 0)
        m_overlay.scale = double(px.height()) / shown.height();
    req.insert(u"width"_s, px.width());
    req.insert(u"height"_s, px.height());
    QJsonArray cams;
    for (const QJsonObject &c : cameras)
        cams.append(c);
    req.insert(u"cameras"_s, cams);
    const double view = m_source.viewExposure ? m_source.viewExposure() : 0.0;
    if (m_source.tone)
        req.insert(u"tone"_s, m_source.tone(movie ? (keepExposure > 0.0 ? keepExposure : view)
                                                  : 0.0));
    QNetworkReply *reply = send(u"POST"_s, m_source.taskPath,
                                QJsonDocument(req).toJson(QJsonDocument::Compact), appManager());
    setStage(Stage::Rendering,
             movie ? u"Ray-traced: rendering %1 frames of %2 × %3…"_s.arg(cameras.size())
                             .arg(px.width()).arg(px.height())
                   : u"Ray-traced: rendering %1 × %2…"_s.arg(px.width()).arg(px.height()));
    const QPointer<RaytraceOutput> self(this);
    const quint64 generation = m_generation;
    const QString backendUrl = m_source.backendUrl, token = m_source.token,
                  sessionId = m_source.sessionId;
    connect(reply, &QNetworkReply::finished, reply, [=]() {
        reply->deleteLater();
        const QString taskId = reply->error() == QNetworkReply::NoError
                ? QJsonDocument::fromJson(reply->readAll()).object().value(u"task_id"_s).toString()
                : QString();
        if (!self || generation != self->m_generation) {
            // Cancelled, replaced or closed before it existed: stopped and
            // removed there.
            if (!taskId.isEmpty())
                deleteRender(backendUrl, token, sessionId, taskId);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            self->setStage(Stage::Idle, u"The render could not start: "_s + errorOf(reply));
            return;
        }
        self->m_taskId = taskId;
        self->m_pollTimer->start();
    });
}

void RaytraceOutput::poll()
{
    if (m_taskId.isEmpty())
        return;
    QNetworkReply *reply = send(u"GET"_s, u"/v1/tasks/"_s + m_taskId);
    m_pollTimer->stop();   // the next poll once this one answers
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation = m_generation]() {
        reply->deleteLater();
        if (m_stage != Stage::Rendering || generation != m_generation)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            setStage(Stage::Idle, u"The render was lost: "_s + errorOf(reply));
            return;
        }
        const QJsonObject task = QJsonDocument::fromJson(reply->readAll()).object();
        const QString status = task.value(u"status"_s).toString();
        m_progress->setValue(static_cast<int>(1000 * task.value(u"progress"_s).toDouble()));
        if (status == u"completed"_s) {
            finished(task.value(u"result"_s).toObject());
        } else if (status == u"failed"_s || status == u"cancelled"_s) {
            setStage(Stage::Idle, status == u"failed"_s
                                          ? u"The render failed: "_s + task.value(u"error"_s).toString()
                                          : u"Cancelled."_s);
        } else {
            m_pollTimer->start();
        }
    });
}

void RaytraceOutput::finished(const QJsonObject &result)
{
    const QString summary = u"%1 particles, %2 × %3, in %4 s"_s
                                    .arg(QLocale().toString(result.value(u"particles"_s).toInt()))
                                    .arg(result.value(u"width"_s).toInt())
                                    .arg(result.value(u"height"_s).toInt())
                                    .arg(result.value(u"seconds"_s).toDouble());
    if (m_taskIsMovie) {
        m_frames = result.value(u"frames"_s).toInt();
        delete m_frameDir;
        m_frameDir = new QTemporaryDir;
        setStage(Stage::Downloading, u"Ray-traced: %1. Fetching the frames…"_s.arg(summary));
        fetchFrame(0);
        return;
    }
    m_autoExposure = result.value(u"exposure"_s).toDouble();
    QNetworkReply *reply = send(u"GET"_s, u"/v1/particle_render/%1/frames/0"_s.arg(m_taskId));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, summary, generation = m_generation]() {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            setStage(Stage::Idle, u"The still could not be fetched: "_s + errorOf(reply));
            return;
        }
        m_still = annotated(reply->readAll(), m_cameras.value(0));
        m_showingStill = true;
        emit showStill(m_still);
        setStage(Stage::Idle, u"Ray-traced still: %1. Change the tone to grade it again; turn "
                              u"the view to go back to it."_s.arg(summary));
    });
}

void RaytraceOutput::toneChanged()
{
    if (m_showingStill && m_stage == Stage::Idle)
        m_regradeTimer->start();
}

void RaytraceOutput::stillDismissed()
{
    m_showingStill = false;
}

void RaytraceOutput::regrade()
{
    if (m_taskId.isEmpty() || m_taskIsMovie || m_autoExposure <= 0.0 || !m_source.tone)
        return;
    if (m_regradeReply)
        m_regradeReply->abort();
    // The grade and the overlay as they are now.
    const double scale = m_overlay.scale;   // the still's size has not changed
    m_overlay = m_source.overlay ? m_source.overlay() : m_overlay;
    m_overlay.scale = scale;
    QNetworkReply *reply = send(u"POST"_s, u"/v1/particle_render/%1/tone"_s.arg(m_taskId),
                                QJsonDocument(m_source.tone(m_autoExposure))
                                        .toJson(QJsonDocument::Compact));
    m_regradeReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation = m_generation]() {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError)
                emit message(u"Could not grade the still: "_s + errorOf(reply));
            return;
        }
        m_still = annotated(reply->readAll(), m_cameras.value(0));
        if (m_showingStill)
            emit showStill(m_still);
    });
}

void RaytraceOutput::fetchFrame(int index)
{
    if (m_stage != Stage::Downloading)
        return;
    if (index >= m_frames) {
        encode();
        return;
    }
    m_progress->setValue(1000 * index / std::max(1, m_frames));
    QNetworkReply *reply = send(u"GET"_s, u"/v1/particle_render/%1/frames/%2"_s.arg(m_taskId)
                                                  .arg(index));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, index, generation = m_generation]() {
        reply->deleteLater();
        if (m_stage != Stage::Downloading || generation != m_generation)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            setStage(Stage::Idle, u"Frame %1 could not be fetched: "_s.arg(index) + errorOf(reply));
            return;
        }
        // The frame's own box and colour bar, as its camera sees them.
        const QImage frame = annotated(reply->readAll(), m_cameras.value(index));
        const QString path = m_frameDir->filePath(u"frame_%1.png"_s.arg(index, 5, 10, QLatin1Char('0')));
        if (!frame.save(path, "PNG")) {
            setStage(Stage::Idle, u"Could not write frame %1 to %2."_s.arg(index)
                                          .arg(m_frameDir->path()));
            return;
        }
        fetchFrame(index + 1);
    });
}

void RaytraceOutput::encode()
{
    const QString ffmpeg = ffmpegPath();
    if (!m_moviePath.endsWith(u".mp4"_s, Qt::CaseInsensitive) || ffmpeg.isEmpty()) {
        // No encoder: the frames, in the folder chosen.
        int copied = 0;
        for (int i = 0; i < m_frames; ++i) {
            const QString name = u"frame_%1.png"_s.arg(i, 5, 10, QLatin1Char('0'));
            QFile::remove(QDir(m_moviePath).filePath(name));
            copied += QFile::copy(m_frameDir->filePath(name), QDir(m_moviePath).filePath(name));
        }
        setStage(Stage::Idle, u"%1 frames saved in %2."_s.arg(copied).arg(m_moviePath));
        forgetTask();
        return;
    }
    setStage(Stage::Encoding, u"Ray-traced: encoding %1 frames…"_s.arg(m_frames));
    auto *proc = new QProcess(this);
    m_ffmpeg = proc;
    const QStringList args{ u"-y"_s, u"-framerate"_s, QString::number(m_fps),
                            u"-i"_s, m_frameDir->filePath(u"frame_%05d.png"_s),
                            u"-c:v"_s, u"libx264"_s, u"-preset"_s, u"slow"_s,
                            u"-crf"_s, u"16"_s, u"-pix_fmt"_s, u"yuv420p"_s,
                            // yuv420p needs even sides.
                            u"-vf"_s, u"pad=ceil(iw/2)*2:ceil(ih/2)*2"_s,
                            u"-movflags"_s, u"+faststart"_s, m_moviePath };
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            setStage(Stage::Idle, u"ffmpeg failed: "_s
                                          + QString::fromLocal8Bit(proc->readAllStandardError())
                                                    .right(600));
            return;
        }
        setStage(Stage::Idle, u"Movie saved: %1 (%2 frames at %3 fps)."_s.arg(m_moviePath)
                                      .arg(m_frames).arg(m_fps));
        forgetTask();
    });
    proc->start(ffmpeg, args);   // arguments, no shell
}

void RaytraceOutput::saveImage()
{
    if (m_still.isNull())
        return;
    const QString path = QFileDialog::getSaveFileName(this, u"Save Image"_s,
                                                      QDir::home().filePath(u"render.png"_s),
                                                      u"PNG image (*.png)"_s);
    if (!path.isEmpty() && !m_still.save(path, "PNG"))
        QMessageBox::warning(this, u"Save Image"_s, u"Could not write %1."_s.arg(path));
}

void RaytraceOutput::saveLight()
{
    if (m_taskId.isEmpty() || m_taskIsMovie)
        return;
    const QString path = QFileDialog::getSaveFileName(this, u"Save Light"_s,
                                                      QDir::home().filePath(u"render_light.fits"_s),
                                                      u"FITS (*.fits)"_s);
    if (path.isEmpty())
        return;
    QNetworkReply *reply = send(u"GET"_s, u"/v1/particle_render/%1/light.fits"_s.arg(m_taskId));
    connect(reply, &QNetworkReply::finished, this, [this, reply, path]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QMessageBox::warning(this, u"Save Light"_s, errorOf(reply));
            return;
        }
        const QByteArray data = reply->readAll();
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
            QMessageBox::warning(this, u"Save Light"_s, u"Could not write %1."_s.arg(path));
    });
}

void RaytraceOutput::cancel()
{
    m_pollTimer->stop();
    if (m_ffmpeg) {   // encoding: its frames are about to go
        m_ffmpeg->disconnect(this);
        m_ffmpeg->kill();
        m_ffmpeg->deleteLater();
    }
    // Deleting the render stops it on the backend (before or after it began).
    const bool movie = m_taskIsMovie;
    setStage(Stage::Idle, u"Ray-traced render cancelled."_s);
    if (movie || m_still.isNull())
        forgetTask();
}

void RaytraceOutput::forgetTask()
{
    ++m_generation;   // the replies on their way are of the task forgotten
    if (!m_taskId.isEmpty())
        deleteRender(m_source.backendUrl, m_source.token, m_source.sessionId, m_taskId);
    m_taskId.clear();
    m_still = QImage();
    m_showingStill = false;
    m_autoExposure = 0.0;
    delete m_frameDir;
    m_frameDir = nullptr;
}

void RaytraceOutput::setStage(Stage stage, const QString &text)
{
    m_stage = stage;
    const bool idle = stage == Stage::Idle;
    m_stillButton->setEnabled(idle);
    m_movieButton->setEnabled(idle);
    m_resolution->setEnabled(idle);
    m_saveImageButton->setEnabled(idle && !m_still.isNull());
    m_saveLightButton->setEnabled(idle && !m_taskId.isEmpty() && !m_taskIsMovie);
    m_progress->setVisible(!idle);
    m_cancelButton->setVisible(!idle);
    if (stage == Stage::Rendering)
        m_progress->setValue(0);
    if (!text.isNull())
        emit message(text);
}
