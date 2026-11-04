// vtkWindowCube_Raytrace.cpp
// The cube viewer's "Ray-traced light" (issue #106): the 3-D view as HDR light,
// ray-marched on this machine's GPU (RaytraceViewport, ParticleGLView) from the
// grid the backend prepares (/v1/volume_render/grid), with stills and movies
// rendered by the backend (/v1/tasks/volume_render, RaytraceOutput). Its
// settings and output sit under the check in the 3-D display section.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "CubeUiAssembler.h"
#include "RaytraceOutput.h"
#include "RaytraceViewport.h"
#include "VolumeRaytracePanel.h"
#include "app/BackendClient.h"

#include <vtkCamera.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

using namespace Qt::Literals::StringLiterals;

void vtkWindowCube::raytraceCamera(
        std::function<void(const QJsonObject &, const QString &)> done)
{
    // The view on screen: the backend's when it is shown, else this window's.
    const bool remote = this->m_renderStack && this->m_renderStack->currentIndex() == 1
            && !this->m_renderSessionId.isEmpty();
    if (!remote) {
        vtkRenderWindow *win = ui->vtkCube->renderWindow();
        vtkRenderer *ren = win ? win->GetRenderers()->GetFirstRenderer() : nullptr;
        if (!ren) {
            done({}, u"no 3-D view"_s);
            return;
        }
        vtkCamera *cam = ren->GetActiveCamera();
        const double *p = cam->GetPosition(), *f = cam->GetFocalPoint(), *u = cam->GetViewUp();
        this->m_rtFromRemote = false;
        done(QJsonObject{ { u"position"_s, QJsonArray{ p[0], p[1], p[2] } },
                          { u"focal_point"_s, QJsonArray{ f[0], f[1], f[2] } },
                          { u"view_up"_s, QJsonArray{ u[0], u[1], u[2] } },
                          { u"view_angle"_s, cam->GetViewAngle() },
                          { u"parallel"_s, cam->GetParallelProjection() != 0 },
                          { u"parallel_scale"_s, cam->GetParallelScale() } },
             QString());
        return;
    }
    auto *nam = this->findChild<QNetworkAccessManager *>(u"raytraceCamera"_s);
    if (!nam) {
        nam = new QNetworkAccessManager(this);
        nam->setObjectName(u"raytraceCamera"_s);
    }
    QUrl url(this->remoteBackendUrl);
    url.setPath(u"/v1/render/%1/camera"_s.arg(this->m_renderSessionId));
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(
            req, this->remoteBackendUrl,
            BackendClient(this->remoteBackendUrl, this->remoteBackendToken).token(),
            this->remoteSessionId);
    QNetworkReply *reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
        reply->deleteLater();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, body.value(u"detail"_s).toString(reply->errorString()));
            return;
        }
        // The backend view's coordinates: the cube's own voxels.
        this->m_rtFromRemote = true;
        done(body, QString());
    });
}

void vtkWindowCube::setDisplayStride(int stride)
{
    stride = std::max(1, stride);
    const int before = std::exchange(this->m_displayStride, stride);
    if (before == stride || !this->m_rtView || this->m_rtFromRemote
        || this->m_rtView->camera().isEmpty())
        return;
    // The displayed coordinates are the cube's over the stride: the same
    // view, scaled by the ratio of the strides.
    const double k = double(before) / stride;
    QJsonObject cam = this->m_rtView->camera();
    for (const QString &key : { u"position"_s, u"focal_point"_s }) {
        QJsonArray v = cam.value(key).toArray();
        for (int i = 0; i < 3; ++i)
            v[i] = v.at(i).toDouble() * k;
        cam.insert(key, v);
    }
    cam.insert(u"parallel_scale"_s, cam.value(u"parallel_scale"_s).toDouble(1.0) * k);
    this->m_rtView->setCamera(cam);
    if (this->m_renderStack && this->m_renderStack->currentWidget() == this->m_rtView)
        this->m_rtView->refresh();   // the grid's box is in the new coordinates
}

QJsonArray vtkWindowCube::raytraceBox() const
{
    // The backend view places the cube at its voxel coordinates: the
    // backend's default box. This window places what it shows there, a
    // strided preview at the preview's coordinates (voxel i of it is the
    // cube's i × stride), so the whole cube spans (n - 1) / stride.
    if (this->m_rtFromRemote)
        return {};
    const double s = std::max(1, this->m_displayStride);
    const int n[3] = { this->remoteDatasetWidth, this->remoteDatasetHeight,
                       this->remoteDatasetDepth };
    QJsonArray box;
    for (int a = 0; a < 3; ++a)
        box << 0.0 << std::max(0, n[a] - 1) / s;
    return box;
}

QJsonObject vtkWindowCube::raytraceRequest() const
{
    // max_axis: the GPU view's 3-D textures (2048 voxels along an axis is
    // what OpenGL 4.1 guarantees); a render task ignores it.
    QJsonObject req{ { u"dataset_id"_s, this->remoteDatasetId }, { u"max_axis"_s, 2048 } };
    const QJsonArray box = this->raytraceBox();
    if (!box.isEmpty())
        req.insert(u"box"_s, box);
    if (this->m_rtPanel) {
        const QJsonObject fields = this->m_rtPanel->fields();
        for (auto it = fields.begin(); it != fields.end(); ++it)
            req.insert(it.key(), it.value());
    }
    return req;
}

bool vtkWindowCube::raytracedShown() const
{
    return this->m_rtCheck && this->m_rtCheck->isChecked();
}

void vtkWindowCube::syncRaytracedControls()
{
    const bool on = this->raytracedShown();
    if (this->m_classic3dTop)
        this->m_classic3dTop->setVisible(!on);
    if (this->m_classic3dBottom)
        this->m_classic3dBottom->setVisible(!on);
    if (this->m_rtCard)
        this->m_rtCard->setVisible(on);
    // The 3-D layers follow it (the pane filter asks raytracedShown).
    this->updateDisplaySectionForActivePane();
}

void vtkWindowCube::setRaytraced(bool on)
{
    // The card and the controls follow the check, whichever way this returns.
    const auto sync = qScopeGuard([this]() { this->syncRaytracedControls(); });
    if (!on) {
        if (this->m_rtView && this->m_renderStack
            && this->m_renderStack->currentWidget() == this->m_rtView) {
            // Back to the view the camera was turned to (this window's; the
            // backend view keeps its own).
            const QJsonObject cam = this->m_rtView->camera();
            vtkRenderWindow *win = ui->vtkCube->renderWindow();
            vtkRenderer *ren = win ? win->GetRenderers()->GetFirstRenderer() : nullptr;
            if (!cam.isEmpty() && ren && !this->m_rtFromRemote) {
                const auto v = [&](const QString &k, int i) {
                    return cam.value(k).toArray().at(i).toDouble();
                };
                vtkCamera *c = ren->GetActiveCamera();
                c->SetPosition(v(u"position"_s, 0), v(u"position"_s, 1), v(u"position"_s, 2));
                c->SetFocalPoint(v(u"focal_point"_s, 0), v(u"focal_point"_s, 1),
                                 v(u"focal_point"_s, 2));
                c->SetViewUp(v(u"view_up"_s, 0), v(u"view_up"_s, 1), v(u"view_up"_s, 2));
                c->SetViewAngle(cam.value(u"view_angle"_s).toDouble(30.0));
                c->SetParallelProjection(cam.value(u"parallel"_s).toBool() ? 1 : 0);
                if (cam.value(u"parallel_scale"_s).toDouble() > 0.0)
                    c->SetParallelScale(cam.value(u"parallel_scale"_s).toDouble());
                ren->ResetCameraClippingRange();
                win->Render();
            }
            this->m_renderStack->setCurrentIndex(this->m_rtPreviousPage);
        }
        return;
    }
    if (this->remoteDatasetId.isEmpty() || !this->m_renderStack || !this->m_rtCard) {
        this->statusBar()->showMessage(
                u"The ray-traced light is prepared by the backend, and this cube is not open "
                u"there."_s, 6000);
        if (this->m_rtCheck) {
            const QSignalBlocker block(this->m_rtCheck);
            this->m_rtCheck->setChecked(false);
        }
        return;
    }
    if (!this->m_rtView) {
        auto *cardLayout = qobject_cast<QVBoxLayout *>(this->m_rtCard->layout());
        this->m_rtPanel = new VolumeRaytracePanel(this->m_rtCard);
        this->m_rtPanel->setFields(this->m_rtFields);
        if (QComboBox *lut = this->m_uiAssembler ? this->m_uiAssembler->lutCombo() : nullptr)
            this->m_rtPanel->setMap(lut->currentText());
        cardLayout->addWidget(this->m_rtPanel);
        // What the picture is read by, over the view and on what is saved.
        auto *boxCheck = new QCheckBox(u"Box"_s);
        boxCheck->setChecked(true);
        boxCheck->setToolTip(u"The cube's bounding box"_s);
        auto *barCheck = new QCheckBox(u"Colour bar"_s);
        barCheck->setChecked(true);
        barCheck->setToolTip(u"The colours' scale: the colour's quantity, or a map's column"_s);
        auto *showRow = new QHBoxLayout;
        showRow->addWidget(boxCheck);
        showRow->addWidget(barCheck);
        showRow->addStretch(1);
        cardLayout->addLayout(showRow);
        auto *outHdr = new QLabel(u"Output"_s, this->m_rtCard);
        outHdr->setStyleSheet(u"font-weight: 600; color: #64748b;"_s);
        cardLayout->addWidget(outHdr);

        QPointer<vtkWindowCube> self(this);
        RaytraceViewport::Source source;
        source.backendUrl = this->remoteBackendUrl;
        source.token = this->remoteBackendToken;
        source.sessionId = this->remoteSessionId;
        source.grid = true;
        source.dataPath = u"/v1/volume_render/grid"_s;
        source.previewPath.clear();   // no backend frames without a GPU view (yet)
        source.key = &VolumeRaytracePanel::gridKey;
        source.request = [self]() { return self ? self->raytraceRequest() : QJsonObject(); };
        source.tone = [self]() {
            if (!self || !self->m_rtPanel || !self->m_rtView)
                return ParticleGLView::Tone();
            const auto [lo, hi] = self->m_rtView->gridRange();
            return self->m_rtPanel->gpuTone(self->m_rtView->gridLut(), lo, hi,
                                            self->m_rtView->gridLog(), self->m_rtFieldName);
        };
        source.toneJson = [self](double base) {
            return self && self->m_rtPanel ? self->m_rtPanel->tone(base) : QJsonObject();
        };
        this->m_rtView = new RaytraceViewport(source, this->m_renderStack);
        this->m_renderStack->addWidget(this->m_rtView);   // after Local (0) and Remote (1)
        const auto overlayShown = [self, boxCheck, barCheck]() {
            if (self && self->m_rtView)
                self->m_rtView->setOverlayShown(boxCheck->isChecked(), barCheck->isChecked());
        };
        QObject::connect(boxCheck, &QCheckBox::toggled, this, overlayShown);
        QObject::connect(barCheck, &QCheckBox::toggled, this, overlayShown);

        RaytraceOutput::Source out;
        out.backendUrl = this->remoteBackendUrl;
        out.token = this->remoteBackendToken;
        out.sessionId = this->remoteSessionId;
        out.taskPath = u"/v1/tasks/volume_render"_s;
        out.request = source.request;
        out.tone = source.toneJson;
        out.camera = [self]() {
            return self && self->m_rtView ? self->m_rtView->camera() : QJsonObject();
        };
        out.viewExposure = [self]() {
            return self && self->m_rtView ? self->m_rtView->autoExposure() : 0.0;
        };
        out.overlay = [self]() {
            return self && self->m_rtView ? self->m_rtView->overlay() : visivo::RaytraceOverlay();
        };
        out.viewSize = [self]() {
            return self && self->m_renderStack ? self->m_renderStack->size() : QSize(16, 9);
        };
        this->m_rtOutput = new RaytraceOutput(std::move(out), this->m_rtCard);
        cardLayout->addWidget(this->m_rtOutput);
        QObject::connect(this->m_rtOutput, &RaytraceOutput::showStill, this->m_rtView,
                         &RaytraceViewport::showStill);
        QObject::connect(this->m_rtOutput, &RaytraceOutput::message, this,
                         [this](const QString &text) {
                             this->statusBar()->showMessage(text, 10000);
                         });
        QObject::connect(this->m_rtView, &RaytraceViewport::stillDismissed, this->m_rtOutput,
                         &RaytraceOutput::stillDismissed);
        QObject::connect(this->m_rtView, &RaytraceViewport::message, this,
                         [this](const QString &text) {
                             this->statusBar()->showMessage(text, 8000);
                         });
        // A grid's colours arrive with it: the tone takes them.
        QObject::connect(this->m_rtView, &RaytraceViewport::gridReady, this->m_rtView,
                         &RaytraceViewport::retone);
        QObject::connect(this->m_rtPanel, &VolumeRaytracePanel::gridChanged, this->m_rtView,
                         &RaytraceViewport::refresh);
        QObject::connect(this->m_rtPanel, &VolumeRaytracePanel::toneChanged, this, [this]() {
            this->m_rtView->retone();
            this->m_rtOutput->toneChanged();
        });
        // Another page shown (the backend view chosen meanwhile): the style
        // is off, its check too.
        QObject::connect(this->m_renderStack, &QStackedWidget::currentChanged, this,
                         [this](int index) {
                             if (!this->m_rtView || !this->m_rtCheck
                                 || this->m_renderStack->widget(index) == this->m_rtView
                                 || !this->m_rtCheck->isChecked())
                                 return;
                             const QSignalBlocker block(this->m_rtCheck);
                             this->m_rtCheck->setChecked(false);
                             this->syncRaytracedControls();
                         });
    }
    const bool alreadyOn = this->m_renderStack->currentWidget() == this->m_rtView;
    if (alreadyOn) {
        this->m_rtView->refresh();
        return;
    }
    this->m_rtPreviousPage = this->m_renderStack->currentIndex();
    this->raytraceCamera([self = QPointer<vtkWindowCube>(this)](const QJsonObject &camera,
                                                                const QString &error) {
        // Cleared while the backend view's camera was on its way: stay off.
        if (!self || !self->m_rtView || !self->m_rtCheck || !self->m_rtCheck->isChecked())
            return;
        if (camera.isEmpty()) {
            self->statusBar()->showMessage(u"No camera for the ray-traced view: "_s + error, 8000);
            self->m_rtCheck->setChecked(false);   // the analysis view and its controls back
            return;
        }
        self->m_rtView->setCamera(camera);
        self->m_rtView->refresh();
        self->m_renderStack->setCurrentWidget(self->m_rtView);
    });
}
