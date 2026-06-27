#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"
#include "CubeViewportGeometry.h"

#include "qcustomplot.h"
#include "ColorMaps.h"
#include "ChannelOverlay.h"
#include "CubeUiAssembler.h"
#include "CubeVRController.h"
#include "DiagnosticsWindow.h"
#include "theme/SegmentedToggle.h"
#include "CatalogueOverlayUtils.h"
#include "CatalogueTableModel.h"
#include "LUTCustomizerDialog.h"
#include "BaselineDialog.h"
#include "LinewidthDialog.h"
#include "MomentMapController.h"
#include "NoiseController.h"
#include "StackDialog.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"
#include "ToolResultDialog.h"
#include "ProfileWidget.h"
#include "PvController.h"
#include "PvDiagramWidget.h"
#include "RemoteRenderWidget.h"
#include "SidebarPanel.h"
#include "Settings.h"
#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "vtkInteractorStyleProfile.h"
#include "vtkLegendScaleActorWCS.h"
#include "wcs.h"

#include <QVTKInteractor.h>
#include <vtkActor.h>
#include <vtkAxisActor2D.h>
#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkColorTransferFunction.h>
#include <vtkContourTriangulator.h>
#include <vtkCoordinate.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges2D.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageMapToColors.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleUser.h>
#include <vtkCubeSource.h>
#include <vtkLineSource.h>
#include <vtkLookupTable.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkCubeAxesActor.h>
#include <vtkOutlineFilter.h>
#include <vtkPropPicker.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkPlaneSource.h>
#include <vtkPiecewiseFunction.h>
#include <vtkTexture.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkRenderWindow.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkDataArray.h>
#include <vtkIdTypeArray.h>
#include <vtkPointData.h>
#include <vtkTextProperty.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <QFileInfo>
#include <QActionGroup>
#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolBar>
#include <QToolButton>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include "ChannelMapsDialog.h"
#include "ChannelMapsWindow.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QList>
#include <QMetaObject>
#include <QMenu>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QGroupBox>
#include <QSlider>
#include <QSpinBox>
#include <QTableView>
#include <QThread>
#include <QtConcurrentRun>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

using namespace Qt::StringLiterals;
using namespace CubeGeom;

// ── Server-side (remote) rendering session control ───────────────────────────
// Extracted verbatim from vtkWindowCube.cpp. Manages the SSR session lifecycle,
// transport wiring, connection/FPS state callbacks and render-mode badges.

void vtkWindowCube::requestRemoteRenderSession()
{
    if (!this->m_renderNam) {
        this->m_renderNam = new QNetworkAccessManager(this);
    }
    if (this->remoteBackendUrl.isEmpty() || this->remoteDatasetId.isEmpty()) {
        qWarning() << "[remote-render] cannot create session: backendUrl or datasetId empty";
        this->applyRenderMode(RenderMode::Local, false);
        return;
    }

    // Probe /v1/render/diagnostics::transports first so we know which
    // transport to set on the widget before connectToSession fires.
    // Result is cached on m_remoteBackendSupportsWebRTC and reused on
    // subsequent Local↔Remote flips within the same backend session.
    {
        QUrl diagUrl(this->remoteBackendUrl);
        diagUrl.setPath(u"/v1/render/diagnostics"_s);
        QNetworkRequest diagReq(diagUrl);
        if (!this->remoteBackendToken.isEmpty()) {
            diagReq.setRawHeader(QByteArrayLiteral("X-Visivo-Token"),
                                 this->remoteBackendToken.toUtf8());
        }
        QNetworkReply *diagReply = this->m_renderNam->get(diagReq);
        QObject::connect(diagReply, &QNetworkReply::finished, this,
                         [this, diagReply]() {
            diagReply->deleteLater();
            if (diagReply->error() != QNetworkReply::NoError) {
                qDebug().noquote() << "[remote-render] diagnostics probe failed; "
                                       "assuming JPEG-WS only";
                this->m_remoteBackendSupportsWebRTC = false;
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(diagReply->readAll());
            const QJsonArray transports = doc.object().value(u"transports"_s).toArray();
            bool haveWebRtc = false;
            for (const auto &t : transports) {
                if (t.toString() == QLatin1String("h265-webrtc")) {
                    haveWebRtc = true;
                    break;
                }
            }
            this->m_remoteBackendSupportsWebRTC = haveWebRtc;
            QStringList tlist;
            for (const auto &t : transports) tlist << t.toString();
            qDebug().noquote() << QStringLiteral(
                    "[remote-render] backend transports: [%1] (h265-webrtc=%2)")
                    .arg(tlist.join(u", "_s))
                    .arg(haveWebRtc ? u"yes"_s : u"no"_s);
        });
    }

    QUrl url(this->remoteBackendUrl);
    url.setPath(u"/v1/render/create_session"_s);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    if (!this->remoteBackendToken.isEmpty()) {
        req.setRawHeader(QByteArrayLiteral("X-Visivo-Token"),
                         this->remoteBackendToken.toUtf8());
    }
    QJsonObject body;
    body[u"dataset_id"_s] = this->remoteDatasetId;
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    qDebug().noquote() << "[remote-render] POST" << url.toString();
    QNetworkReply *reply = this->m_renderNam->post(req, payload);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        this->m_renderBackendProbed = true;
        if (reply->error() != QNetworkReply::NoError) {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qWarning().noquote()
                    << "[remote-render] create_session failed:" << reply->errorString()
                    << "(HTTP" << code << ")";
            this->m_renderBackendSupportsSsr = false;
            // Stay on Local (current page) and tell the user why.
            if (this->statusBar()) {
                this->statusBar()->showMessage(
                        u"Remote rendering unavailable on this backend — staying local"_s,
                        5000);
            }
            this->m_effectiveRenderMode = RenderMode::Local;
            this->updateRenderModeBadges();
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QString sid = doc.object().value(u"session_id"_s).toString();
        if (sid.isEmpty()) {
            qWarning() << "[remote-render] create_session returned no session_id";
            return;
        }
        this->m_renderSessionId = sid;
        this->m_renderBackendSupportsSsr = true;
        qDebug().noquote() << "[remote-render] session created:" << sid;

        if (this->m_remoteRenderWidget) {
            this->connectRemoteRenderWidgetWithTransport(sid);
        }
        if (this->m_renderStack) {
            this->m_renderStack->setCurrentIndex(1);
        }
        this->m_effectiveRenderMode = RenderMode::Remote;
        this->updateRenderModeBadges();
    });
}

void vtkWindowCube::connectRemoteRenderWidgetWithTransport(const QString &sessionId)
{
    // Pick the best transport based on backend capability. The
    // policy ("H.265 always when available") was set during the Phase
    // 3 architecture sign-off — see project_h265_backend_e2e.md. If
    // the build wasn't compiled with VISIVO_HAS_WEBRTC, the widget's
    // ``setTransport`` is a no-op so we just fall through to JPEG.
#ifdef VISIVO_HAS_WEBRTC
    if (this->m_remoteBackendSupportsWebRTC) {
        this->m_remoteRenderWidget->setTransport(
                RemoteRenderWidget::Transport::H265WebRTC);
        qDebug().noquote()
                << "[remote-render] transport: H.264 WebRTC (h265-webrtc capable)";
    } else
#endif
    {
        this->m_remoteRenderWidget->setTransport(
                RemoteRenderWidget::Transport::JpegWS);
        qDebug().noquote()
                << "[remote-render] transport: JPEG WebSocket (legacy / fallback)";
    }
    this->m_remoteRenderWidget->connectToSession(
            this->remoteBackendUrl, sessionId, this->remoteBackendToken);
}

void vtkWindowCube::releaseRemoteRenderSession()
{
    if (this->m_remoteRenderWidget) {
        this->m_remoteRenderWidget->disconnectFromSession();
    }
    if (this->m_renderSessionId.isEmpty()) {
        return;
    }
    if (!this->m_renderNam) {
        this->m_renderNam = new QNetworkAccessManager(this);
    }
    QUrl url(this->remoteBackendUrl);
    url.setPath(u"/v1/render/"_s + this->m_renderSessionId);
    QNetworkRequest req(url);
    if (!this->remoteBackendToken.isEmpty()) {
        req.setRawHeader(QByteArrayLiteral("X-Visivo-Token"),
                         this->remoteBackendToken.toUtf8());
    }
    qDebug().noquote() << "[remote-render] DELETE" << url.toString();
    QNetworkReply *reply = this->m_renderNam->deleteResource(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    this->m_renderSessionId.clear();
}

void vtkWindowCube::onRemoteRenderConnectionStateChanged(const QString &state)
{
    if (this->m_remoteRenderBannerLabel) {
        if (state == QLatin1String("Live")) {
            this->m_remoteRenderBannerLabel->setText(
                    u"Remote rendering (GPU on backend) — click ✕ to switch to Local"_s);
        } else {
            this->m_remoteRenderBannerLabel->setText(
                    u"Remote rendering — " + state);
        }
    }
    // Clear the switch-in-progress guard on any terminal/failure
    // state too, so the user can retry instead of being stuck if the
    // negotiation broke (network blip, backend error, timeout). Also
    // surface the failure reason in the recommendation banner so the
    // user gets honest feedback instead of a frozen "Connecting…".
    if (state == QLatin1String("Disconnected")
        || state.startsWith(QLatin1String("Error"))) {
        this->m_remoteSwitchInProgress = false;
        if (this->m_remoteSwitchProgressTimer) {
            this->m_remoteSwitchProgressTimer->stop();
        }
        if (this->m_localRecommendRemoteBanner
            && this->m_localRecommendRemoteLabel) {
            this->m_localRecommendRemoteLabel->setText(
                    u"Remote rendering: " + state
                    + u" — click Switch to Remote to retry."_s);
            this->m_localRecommendRemoteBanner->setVisible(true);
        }
        if (this->m_localRecommendRemoteSwitchBtn) {
            this->m_localRecommendRemoteSwitchBtn->setEnabled(true);
            this->m_localRecommendRemoteSwitchBtn->setText(u"Switch to Remote"_s);
        }
    }
    // When the WebSocket turns Live (first frame received), the server
    // is up but still running on its initial defaults. Push the user's
    // current state — LUT, threshold, blend, cutting plane — so the
    // very next frame reflects what they see locally.
    if (state == QLatin1String("Live")) {
        // The user IS now seeing full-resolution data, just streamed
        // from the V100 instead of computed locally. Update the cube
        // open state label so it stops saying "Preview" / "Loading
        // full resolution" once the remote stream is up — that text
        // is misleading when the displayed frames are already
        // full-quality remote renders.
        if (this->m_effectiveRenderMode == RenderMode::Remote) {
            this->setRemoteCubeDisplayState(RemoteCubeDisplayState::FullResolution);
            this->clearPersistentStatusMessage();
        }
        // Connection is up — clear the switch-in-progress guard, stop
        // the elapsed-time ticker, hide the "Connecting…" banner, and
        // restore the Switch button to its idle text/state in case
        // the user goes back to Local and then wants to switch again.
        this->m_remoteSwitchInProgress = false;
        if (this->m_remoteSwitchProgressTimer) {
            this->m_remoteSwitchProgressTimer->stop();
        }
        if (this->m_localRecommendRemoteBanner) {
            this->m_localRecommendRemoteBanner->setVisible(false);
        }
        if (this->m_localRecommendRemoteSwitchBtn) {
            this->m_localRecommendRemoteSwitchBtn->setEnabled(true);
            this->m_localRecommendRemoteSwitchBtn->setText(u"Switch to Remote"_s);
        }
        this->pushRenderStateToRemote();

        // First-time Remote entry race: the Auto-trigger calls
        // applyRenderMode at +150 ms, but the channel scrubber overlay
        // is created LATER (in applyCubeOpenResult, when the preview
        // lands). On that first switch the reparenting in
        // applyRenderMode runs against a null channelOverlay and does
        // nothing — so the user sees no slider on the remote view
        // until they manually flip Local→Remote again. Retry the
        // reparenting here: by the time the WS is Live, the cube
        // preview has long since finished, channelOverlay exists, and
        // we're already on the Remote stack page.
        if (this->channelOverlay
            && this->m_effectiveRenderMode == RenderMode::Remote
            && this->m_remoteRenderWidget
            && this->channelOverlay->parentWidget() != this->m_remoteRenderWidget) {
            QWidget *oldParent = this->channelOverlay->parentWidget();
            if (oldParent) {
                oldParent->removeEventFilter(this->channelOverlay);
            }
            this->channelOverlay->setParent(this->m_remoteRenderWidget);
            this->m_remoteRenderWidget->installEventFilter(this->channelOverlay);
            this->channelOverlay->show();
            QResizeEvent ev(this->m_remoteRenderWidget->size(),
                            this->m_remoteRenderWidget->size());
            QApplication::sendEvent(this->m_remoteRenderWidget, &ev);
        }
    }
}

void vtkWindowCube::onRemoteRenderFpsChanged(double fps)
{
    if (this->m_renderFpsBadge) {
        this->m_renderFpsBadge->setText(QStringLiteral("%1 FPS").arg(fps, 0, 'f', 1));
    }
}

void vtkWindowCube::resendThresholdToRemote()
{
    // Idempotent re-push of the slider's CURRENT value. Cheap on the
    // server (just rebuilds the opacity TF + flags a render). Used as
    // a defensive companion to other stateful commands so MIP/MinIP
    // switches, LUT changes, etc. always render with the threshold
    // the user is actually looking at on the local slider — bypasses
    // any race where the backend's TF could be stale (e.g. the
    // bootstrap state push happened before applyCubeOpenResult set
    // m_initialThresholdApplied=true).
    if (!this->m_initialThresholdApplied) return;
    QVariantMap m;
    m[u"type"_s]  = u"set_threshold"_s;
    // Read from the line-edit, NOT from this->lowerBound — the latter
    // is the LOWER EDGE of the cube's visible range (cubeRange[0] +
    // 3 % span, set once in applyCubeOpenResult and never modified).
    // The actual threshold the user is looking at lives in
    // ui->lineThreshold (kept in sync by applyThreshold). Reading
    // lowerBound here used to push a negative dummy on bootstrap,
    // forcing the user to click "Enter" on the threshold field to
    // resync the backend.
    m[u"value"_s] = ui->lineThreshold->text().toDouble();
    this->sendRemoteRenderStateCommand(m);
}

void vtkWindowCube::sendRemoteRenderStateCommand(const QVariantMap &command)
{
    const QString type = command.value(u"type"_s).toString();
    const bool widgetExists = (this->m_remoteRenderWidget != nullptr);
    const bool isLive = widgetExists && this->m_remoteRenderWidget->isLive();
    const bool inRemote = (this->m_effectiveRenderMode == RenderMode::Remote);
    if (!widgetExists || !isLive || !inRemote) {
        qDebug().noquote() << QStringLiteral(
                "[remote-render] DROP %1 — widget=%2 live=%3 mode=%4")
                                      .arg(type)
                                      .arg(widgetExists)
                                      .arg(isLive)
                                      .arg(inRemote ? u"Remote"_s : u"Local"_s);
        return;
    }
    qDebug().noquote() << QStringLiteral("[remote-render] SEND %1: %2")
                                  .arg(type, QString::fromUtf8(
                                          QJsonDocument(QJsonObject::fromVariantMap(command))
                                                  .toJson(QJsonDocument::Compact)));
    this->m_remoteRenderWidget->sendStateCommand(command);
}

void vtkWindowCube::pushRenderStateToRemote()
{
    qDebug().noquote() << QStringLiteral(
            "[remote-render] BOOTSTRAP state push — widget=%1 live=%2 mode=%3 LUT=%4 threshold=%5")
                                  .arg(this->m_remoteRenderWidget != nullptr)
                                  .arg(this->m_remoteRenderWidget
                                               && this->m_remoteRenderWidget->isLive())
                                  .arg(this->m_effectiveRenderMode == RenderMode::Remote
                                               ? u"Remote"_s : u"Local"_s)
                                  .arg(this->m_activeColorMapName.isEmpty()
                                               ? u"<empty>"_s : this->m_activeColorMapName)
                                  .arg(this->m_initialThresholdApplied
                                               ? QString::number(this->lowerBound)
                                               : u"<not-applied>"_s);

    // Route every bootstrap command through sendRemoteRenderStateCommand so
    // each one is logged with the same SEND/DROP envelope as user-driven
    // changes. Easier to spot which value the server is actually being
    // told to use when something looks wrong.
    if (!this->m_activeColorMapName.isEmpty()) {
        QVariantMap m;
        m[u"type"_s] = u"set_lut"_s;
        m[u"name"_s] = this->m_activeColorMapName;
        this->sendRemoteRenderStateCommand(m);
    }
    if (!this->m_activeSliceLutName.isEmpty()) {
        QVariantMap m;
        m[u"type"_s] = u"set_slice_lut"_s;
        m[u"name"_s] = this->m_activeSliceLutName;
        this->sendRemoteRenderStateCommand(m);
    }
    if (this->m_initialThresholdApplied) {
        QVariantMap m;
        m[u"type"_s]  = u"set_threshold"_s;
        // See resendThresholdToRemote() for why lineThreshold is the
        // right source — lowerBound is the cube's lower edge, not the
        // user's current threshold.
        m[u"value"_s] = ui->lineThreshold->text().toDouble();
        this->sendRemoteRenderStateCommand(m);
    }
    int blend = 0;
    if (this->m_actionMIP   && this->m_actionMIP->isChecked())   blend = 1;
    if (this->m_actionMinIP && this->m_actionMinIP->isChecked()) blend = 2;
    {
        QVariantMap m;
        m[u"type"_s] = u"set_blend_mode"_s;
        m[u"mode"_s] = blend;
        this->sendRemoteRenderStateCommand(m);
    }
    if (this->actionShowCuttingPlane) {
        QVariantMap m;
        m[u"type"_s]    = u"set_cutting_plane"_s;
        m[u"visible"_s] = this->actionShowCuttingPlane->isChecked();
        m[u"slice_index"_s] = ui->sliderSlice ? ui->sliderSlice->value() : 0;
        this->sendRemoteRenderStateCommand(m);
    }
    {
        QVariantMap m;
        m[u"type"_s] = u"set_cube_render_mode"_s;
        const bool iso = ui->actionIsosurface && ui->actionIsosurface->isChecked();
        m[u"mode"_s] = iso ? u"isosurface"_s : u"volume"_s;
        this->sendRemoteRenderStateCommand(m);
    }
}

void vtkWindowCube::updateRenderModeBadges()
{
    if (!this->m_renderModeBadge) return;
    const bool remote = (this->m_effectiveRenderMode == RenderMode::Remote);
    this->m_renderModeBadge->setText(remote
                                             ? u"Render: REMOTE"_s
                                             : u"Render: LOCAL"_s);
    this->m_renderModeBadge->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  padding: 1px 8px; margin-right: 6px;"
                    "  border-radius: 9px;"
                    "  background: rgba(%1, 0.22);"
                    "  color: palette(window-text);"
                    "  font-size: 10px; font-weight: 600;"
                    "  text-transform: uppercase; letter-spacing: 0.04em;"
                    "}")
                    .arg(remote ? u"245, 158, 11"_s : u"120, 120, 120"_s));
    if (this->m_renderFpsBadge) {
        this->m_renderFpsBadge->setVisible(remote);
    }
}
