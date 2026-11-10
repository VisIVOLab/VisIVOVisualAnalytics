// vtkWindowImage_Samp.cpp – SAMP actions of the image viewer (issue #130)
//
// This window sends ITS image (the master layer's file on the backend), and
// the sky position of the last pixel clicked, as coord.pointAt.sky; with
// "Send Positions of Clicked Pixels" on, every click sends one. A click is a
// left press and release a few pixels apart at most, with no tool that takes
// the click for itself (isInteractionToolActive), and not on a catalogue
// source: a drag pans or stretches. The position comes from the backend,
// which has the file's full WCS (pixel_to_sky); an older backend gets the
// client's own conversion (SampSkyPosition.h).

#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"

#include "LayerListModel.h"
#include "MainWindow.h"
#include "SampViewerMenu.h"
#include "app/DatasetName.h"
#include "app/SampSkyPosition.h"

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QStatusBar>

#include <vtkCommand.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

#include <cmath>
#include <cstdlib>

using namespace Qt::StringLiterals;

namespace {
/// A press and release this close (display pixels) are a click, not a drag.
constexpr int kClickSlopPx = 4;
}  // namespace

void vtkWindowImage::setupSamp()
{
    visivo::installSampMenu(this, [this](QMenu *menu, MainWindow *host) {
        this->fillSampMenu(menu, host);
    });
    auto *iren = ui->vtk->renderWindow() ? ui->vtk->renderWindow()->GetInteractor() : nullptr;
    if (!iren)
        return;
    // Ahead of the other press handlers: a tool that ends on this press (an
    // annotation placed, a polygon closed) is still active when it is seen.
    iren->AddObserver(vtkCommand::LeftButtonPressEvent, this, &vtkWindowImage::sampMousePressed,
                      10.0f);
    iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                      &vtkWindowImage::sampMouseReleased);
}

std::optional<visivo::samp::SkyPosition> vtkWindowImage::sampSkyAt(double fullX,
                                                                   double fullY) const
{
    visivo::samp::ImageSkyWcs wcs;
    for (int i = 0; i < 2; ++i) {
        wcs.ctype[static_cast<std::size_t>(i)] = this->remoteDatasetCtype[static_cast<std::size_t>(i)];
        wcs.crval[static_cast<std::size_t>(i)] = this->remoteDatasetCrval[static_cast<std::size_t>(i)];
        wcs.crpix[static_cast<std::size_t>(i)] = this->remoteDatasetCrpix[static_cast<std::size_t>(i)];
        wcs.cdelt[static_cast<std::size_t>(i)] = this->remoteDatasetCdelt[static_cast<std::size_t>(i)];
    }
    return visivo::samp::pixelToIcrs(wcs, fullX, fullY);
}

void vtkWindowImage::sampMousePressed()
{
    auto *iren = ui->vtk->renderWindow()->GetInteractor();
    const int *p = iren ? iren->GetEventPosition() : nullptr;
    this->m_sampPressPos = p ? std::array<int, 2>{ p[0], p[1] } : std::array<int, 2>{ -1, -1 };
    this->m_sampPressTaken = this->isInteractionToolActive();
}

bool vtkWindowImage::isInteractionToolActive() const
{
    // Every tool whose clicks place or pick something: regions (and closing a
    // polygon), the VLKB area, the probe's pin, measurements, annotations.
    return this->regionMode != RegionMode::None || this->m_vlkbSelectActive
           || this->probeModeActive || this->m_measMode != MeasurementMode::None
           || this->m_annotPlaceMode != AnnotPlaceMode::None;
}

void vtkWindowImage::sampMouseReleased()
{
    auto *iren = ui->vtk->renderWindow()->GetInteractor();
    const int *p = iren ? iren->GetEventPosition() : nullptr;
    const std::array<int, 2> press = this->m_sampPressPos;
    this->m_sampPressPos = { -1, -1 };
    if (!p || press[0] < 0 || std::abs(p[0] - press[0]) > kClickSlopPx
        || std::abs(p[1] - press[1]) > kClickSlopPx)
        return;   // a drag
    // A click a tool took, or one that picked a catalogue source (the press
    // picked it: the selection is set only on a hit), is not a position the
    // user means to point at.
    if (this->m_sampPressTaken || this->isInteractionToolActive())
        return;
    if (this->catalogueOverlayLoaded && this->actionShowCatalogueOverlay
        && this->actionShowCatalogueOverlay->isChecked() && this->selectedCatalogueSourceIndex >= 0)
        return;
    auto *renderer = ui->vtk->renderWindow()->GetRenderers()->GetFirstRenderer();
    auto *imageData = this->layers ? this->layers->getImageData(this->layers->getMasterIndex())
                                   : nullptr;
    if (!renderer || !imageData)
        return;
    this->coordinate->SetValue(p[0], p[1]);
    const double *world = this->coordinate->GetComputedWorldValue(renderer);
    if (!world)
        return;
    // World coordinates ARE full-resolution pixel coordinates here (the
    // displayed image's origin and spacing map it onto the source pixels),
    // as for the probe readout.
    double bounds[6];
    imageData->GetBounds(bounds);
    double spacing[3];
    imageData->GetSpacing(spacing);
    const double halfX = 0.5 * spacing[0];
    const double halfY = 0.5 * spacing[1];
    if (world[0] < bounds[0] - halfX || world[0] > bounds[1] + halfX || world[1] < bounds[2] - halfY
        || world[1] > bounds[3] + halfY)
        return;   // off the image
    this->m_sampClickPixel = { world[0], world[1] };
    this->m_sampHasClick = true;
    if (this->m_sampBroadcastClicks)
        this->sendClickedPosition(QString(), /*quiet=*/true);
}

void vtkWindowImage::sendClickedPosition(const QString &recipientId, bool quiet)
{
    MainWindow *host = visivo::sampHost(this);
    QString problem;
    if (!this->m_sampHasClick)
        problem = u"click a pixel of the image first"_s;
    else if (!host || !host->sampConnected())
        problem = u"not connected to a SAMP hub"_s;
    if (!problem.isEmpty()) {
        if (!quiet)
            this->statusBar()->showMessage(u"SAMP: no position sent: %1."_s.arg(problem), 6000);
        return;
    }
    const std::array<double, 2> pixel = this->m_sampClickPixel;
    // A later click supersedes a position still on its way.
    const quint64 serial = ++this->m_sampClickSerial;
    QPointer<vtkWindowImage> self = this;
    visivo::resolvePixelSky(
            this,
            { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
              this->remoteDatasetId, pixel[0], pixel[1], 0.0 },
            [self, pixel]() { return self ? self->sampSkyAt(pixel[0], pixel[1])
                                          : std::optional<visivo::samp::SkyPosition>(); },
            [self, pixel, serial, recipientId, quiet](const visivo::PixelSky &sky) {
                if (!self || serial != self->m_sampClickSerial)
                    return;
                MainWindow *mw = visivo::sampHost(self);
                QString why = sky.problem;
                if (sky.position
                    && (!mw || !mw->sendSkyPositionViaSamp(sky.position->raDeg,
                                                           sky.position->decDeg, recipientId)))
                    why = u"not connected to a SAMP hub, or not a sky position"_s;
                if (!sky.position || !why.isEmpty()) {
                    if (!quiet)
                        self->statusBar()->showMessage(
                                u"SAMP: no position sent: %1."_s.arg(why), 6000);
                    return;
                }
                self->statusBar()->showMessage(
                        u"SAMP: sent pixel %1, %2 (%3)%4."_s.arg(std::lround(pixel[0]))
                                .arg(std::lround(pixel[1]))
                                .arg(visivo::samp::describe(*sky.position),
                                     sky.local ? u"; the backend is older than this client, so "
                                                 u"without the header's rotation or distortion"_s
                                               : QString()),
                        8000);
            });
}

void vtkWindowImage::fillSampMenu(QMenu *menu, MainWindow *host)
{
    QPointer<vtkWindowImage> self = this;
    const QString path = this->filepath;
    const bool field = visivo::isFieldReference(path);
    const QString name = visivo::datasetDisplayName(path);
    QString imageWhy;
    if (field)
        imageWhy = u"A field of a VBT volume is not a FITS file other tools can load."_s;
    else if (host && host->sampSendBusy())
        imageWhy = u"A file is still being prepared for sending."_s;
    visivo::addSampSendTo(menu, host, u"Send Image To"_s, QString::fromLatin1(Samp::kImageLoadFits),
                          imageWhy, [self, path, name](const QString &to) {
                              MainWindow *mw = self ? visivo::sampHost(self) : nullptr;
                              if (mw)
                                  mw->sendFitsViaSamp(path, name, to, self);
                          });

    menu->addSeparator();
    const bool sky = visivo::samp::skyFrameOf({ this->remoteDatasetCtype[0],
                                                this->remoteDatasetCtype[1] })
                             .has_value();
    QString positionWhy;
    if (!sky)
        positionWhy = u"The image has no sky coordinates (RA/Dec, l/b or ecliptic axes)."_s;
    else if (!this->m_sampHasClick)
        positionWhy = u"Click a pixel of the image first."_s;
    visivo::addSampSendTo(menu, host, u"Send Clicked Position To"_s,
                          QString::fromLatin1(Samp::kCoordPointAtSky), positionWhy,
                          [self](const QString &to) {
                              if (self)
                                  self->sendClickedPosition(to, /*quiet=*/false);
                          });
    QAction *broadcast = menu->addAction(u"Send Positions of Clicked Pixels"_s);
    broadcast->setCheckable(true);
    broadcast->setChecked(this->m_sampBroadcastClicks);
    broadcast->setEnabled(sky);
    broadcast->setToolTip(u"While on, every pixel you click is sent to all SAMP tools as "
                          u"coord.pointAt.sky (Aladin and DS9 recentre on it)."_s);
    QObject::connect(broadcast, &QAction::toggled, this, [self](bool on) {
        if (self)
            self->m_sampBroadcastClicks = on;
    });
}
