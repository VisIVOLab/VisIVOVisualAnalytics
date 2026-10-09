// Movie export (F3) — partial class of vtkWindowCube.
//
// Capture an animation of the cube as a video. Two modes:
//   * Channel scan — step through the velocity channels, rendering each 2-D
//     channel map (with any kinematic-model contour overlaid), the classic HI
//     "data movie".
//   * Camera orbit — rotate the 3-D camera a full turn around the volume.
//
// Frames are grabbed straight from the Qt GL widget (QOpenGLWidget::
// grabFramebuffer → QImage), which is FBO-reliable across platforms, and
// assembled into an MP4 (H.264) with ffmpeg when it is on PATH; otherwise the
// PNG frame sequence is written to a folder the user chooses. The capture loop
// runs on the GUI thread (it must render the on-screen window) under a
// cancellable, window-modal QProgressDialog, guarded against re-entry.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "app/BackendClient.h"

#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QOpenGLWidget>
#include <QProcess>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

namespace {
// Grab the current contents of a Qt GL widget to a PNG. grabFramebuffer() reads
// the widget's own framebuffer object, which is reliable across Qt/VTK/platform
// combinations (unlike reading GL front/back buffers directly). The widget must
// have been rendered immediately beforehand.
bool captureWidgetToPng(QWidget *w, const QString &path)
{
    auto *gl = qobject_cast<QOpenGLWidget *>(w);
    if (!gl)
        return false;
    const QImage img = gl->grabFramebuffer();
    return !img.isNull() && img.save(path, "PNG");
}

QString framePath(const QString &dir, int index)
{
    return QStringLiteral("%1/frame_%2.png").arg(dir).arg(index, 5, 10, QLatin1Char('0'));
}
}  // namespace

void vtkWindowCube::showExportMovieDialog()
{
    if (this->m_exportMovieInProgress)
        return;
    const int nz = this->remoteDatasetDepth;
    if (nz <= 0) {
        QMessageBox::warning(this, u"Export Movie"_s, u"Cube geometry is not available yet."_s);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(u"Export Movie"_s);
    auto *form = new QFormLayout(&dlg);

    auto *mode = new QComboBox(&dlg);
    mode->addItems({ u"Channel scan (2D channel maps)"_s, u"Camera orbit (3D view)"_s });
    form->addRow(u"Mode"_s, mode);

    auto *chStart = new QSpinBox(&dlg);
    chStart->setRange(1, nz);
    chStart->setValue(1);
    auto *chEnd = new QSpinBox(&dlg);
    chEnd->setRange(1, nz);
    chEnd->setValue(nz);
    form->addRow(u"First channel"_s, chStart);
    form->addRow(u"Last channel"_s, chEnd);

    auto *orbitFrames = new QSpinBox(&dlg);
    orbitFrames->setRange(8, 1440);
    orbitFrames->setValue(120);
    form->addRow(u"Orbit frames (360°)"_s, orbitFrames);

    auto *fps = new QSpinBox(&dlg);
    fps->setRange(1, 60);
    fps->setValue(15);
    form->addRow(u"Frames per second"_s, fps);

    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    auto *note = new QLabel(
            ffmpeg.isEmpty()
                    ? u"ffmpeg not found, frames will be saved as a PNG sequence."_s
                    : u"Output: MP4 (H.264) via ffmpeg."_s);
    note->setWordWrap(true);
    form->addRow(note);

    auto syncRows = [&]() {
        const bool scan = (mode->currentIndex() == 0);
        chStart->setEnabled(scan);
        chEnd->setEnabled(scan);
        orbitFrames->setEnabled(!scan);
    };
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dlg, [syncRows](int) { syncRows(); });
    syncRows();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Export…"_s);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const bool scan = (mode->currentIndex() == 0);
    const int fpsVal = fps->value();
    const int c0 = std::min(chStart->value(), chEnd->value()) - 1;   // 0-based
    const int c1 = std::max(chStart->value(), chEnd->value()) - 1;
    const int nFrames = scan ? (c1 - c0 + 1) : orbitFrames->value();
    if (nFrames < 1)
        return;

    // Pick the output: MP4 if ffmpeg is available, else a folder for the frames.
    QString outputMp4, framesDir;
    if (!ffmpeg.isEmpty()) {
        const QString base = QFileInfo(this->filepath).completeBaseName();
        outputMp4 = QFileDialog::getSaveFileName(
                this, u"Export movie as"_s,
                QStringLiteral("%1_%2.mp4").arg(base, scan ? u"channels"_s : u"orbit"_s),
                u"MP4 video (*.mp4)"_s);
        if (outputMp4.isEmpty())
            return;
    } else {
        framesDir = QFileDialog::getExistingDirectory(this, u"Save PNG frames to folder"_s);
        if (framesDir.isEmpty())
            return;
    }

    QTemporaryDir tmp;
    QString workDir = framesDir;
    if (!ffmpeg.isEmpty()) {
        if (!tmp.isValid()) {
            QMessageBox::warning(this, u"Export Movie"_s, u"Could not create a temporary folder."_s);
            return;
        }
        workDir = tmp.path();
    }

    // Re-entry / lifetime guard: block a second export, disable the action, and
    // restore both on every exit path. The window-modal progress dialog also
    // blocks closing the viewer while frames render.
    this->m_exportMovieInProgress = true;
    if (this->m_actExportMovie)
        this->m_actExportMovie->setEnabled(false);
    auto guard = qScopeGuard([this]() {
        this->m_exportMovieInProgress = false;
        if (this->m_actExportMovie)
            this->m_actExportMovie->setEnabled(true);
    });

    QProgressDialog progress(scan ? u"Rendering channel frames…"_s : u"Rendering orbit frames…"_s,
                             u"Cancel"_s, 0, nFrames, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    bool cancelled = false;
    int written = 0;   // contiguous frame index → no gaps for ffmpeg's %05d
    if (scan) {
        const int origChannel = ui->spinSlice ? ui->spinSlice->value() - 1 : -1;   // restore after
        BackendClient client(this->remoteBackendUrl, this->remoteBackendToken);
        client.setSessionId(this->remoteSessionId);
        for (int i = 0; i < nFrames; ++i) {
            progress.setValue(i);
            qApp->processEvents();
            if (progress.wasCanceled()) { cancelled = true; break; }

            const int ch = c0 + i;
            const BackendCubeSliceResult resp = client.requestSlice(this->remoteDatasetId, u"z"_s, ch);
            if (!resp.valid || resp.scalarType != u"float32"_s
                || resp.data.size()
                        != qint64(resp.width) * resp.height * qint64(sizeof(float))) {
                continue;   // skip a bad channel rather than abort the whole movie
            }
            auto img = vtkSmartPointer<vtkImageData>::New();
            img->SetExtent(0, resp.width - 1, 0, resp.height - 1, 0, 0);
            img->SetOrigin(0., 0., 0.);
            img->SetSpacing(1., 1., 1.);
            img->AllocateScalars(VTK_FLOAT, 1);
            std::memcpy(img->GetScalarPointer(), resp.data.constData(),
                        static_cast<std::size_t>(resp.data.size()));

            RemoteCubeSliceResult r;
            r.valid = true;
            r.imageData = img;
            r.index = ch;
            r.imageRange = { resp.rangeMin, resp.rangeMax };
            this->applyRemoteSliceResult(r);   // renders sliceWin (+ 2-D model contour)
            if (captureWidgetToPng(ui->vtkImage, framePath(workDir, written)))
                ++written;
        }
        // Restore the channel the user was viewing before the scan.
        if (origChannel >= 0)
            this->requestRemoteSlice(origChannel);
    } else {
        auto *rw = ui->vtkCube->renderWindow();
        auto *ren = rw ? rw->GetRenderers()->GetFirstRenderer() : nullptr;
        auto *cam = ren ? ren->GetActiveCamera() : nullptr;
        if (!cam) {
            QMessageBox::warning(this, u"Export Movie"_s, u"3-D view is not available."_s);
            return;
        }
        // Snapshot the camera so cancel/partial export leaves the view unchanged.
        double pos[3], foc[3], up[3];
        cam->GetPosition(pos);
        cam->GetFocalPoint(foc);
        cam->GetViewUp(up);
        const double step = 360.0 / nFrames;
        for (int i = 0; i < nFrames; ++i) {
            progress.setValue(i);
            qApp->processEvents();
            if (progress.wasCanceled()) { cancelled = true; break; }

            cam->Azimuth(step);
            ren->ResetCameraClippingRange();
            rw->Render();
            if (captureWidgetToPng(ui->vtkCube, framePath(workDir, written)))
                ++written;
        }
        // Restore the original camera.
        cam->SetPosition(pos);
        cam->SetFocalPoint(foc);
        cam->SetViewUp(up);
        ren->ResetCameraClippingRange();
        rw->Render();
    }
    progress.setValue(nFrames);

    if (cancelled) {
        this->statusBar()->showMessage(u"Movie export cancelled."_s, 5000);
        return;   // never encode/report a partial movie the user cancelled
    }
    if (written == 0) {
        QMessageBox::warning(this, u"Export Movie"_s, u"No frames were captured."_s);
        return;
    }

    if (ffmpeg.isEmpty()) {
        this->statusBar()->showMessage(
                u"Saved %1 PNG frames to %2."_s.arg(written).arg(workDir), 8000);
        return;
    }

    // Assemble the frames into an MP4. pad=…*2 keeps even dimensions for yuv420p.
    QStringList args;
    args << u"-y"_s << u"-framerate"_s << QString::number(fpsVal)
         << u"-i"_s << QStringLiteral("%1/frame_%05d.png").arg(workDir)
         << u"-c:v"_s << u"libx264"_s << u"-pix_fmt"_s << u"yuv420p"_s
         << u"-crf"_s << u"18"_s
         << u"-vf"_s << u"pad=ceil(iw/2)*2:ceil(ih/2)*2"_s
         << outputMp4;
    QProcess proc;
    proc.start(ffmpeg, args);   // QStringList args → no shell, injection-safe
    const bool finished = proc.waitForFinished(600000);
    if (!finished || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (!finished) {
            proc.kill();
            proc.waitForFinished(5000);   // reap before reading stderr / returning
        }
        const QString err = QString::fromUtf8(proc.readAllStandardError());
        QMessageBox::warning(
                this, u"Export Movie"_s,
                u"ffmpeg failed to encode the video.\n\n%1"_s.arg(err.left(800)));
        return;
    }
    this->statusBar()->showMessage(
            u"Movie exported: %1 (%2 frames @ %3 fps)."_s.arg(outputMp4).arg(written).arg(fpsVal),
            8000);
}
