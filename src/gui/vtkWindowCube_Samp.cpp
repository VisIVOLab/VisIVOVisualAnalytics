// vtkWindowCube_Samp.cpp – SAMP actions of the cube viewer (issue #130)
//
// This window sends ITS cube (the file it opened on the backend), and the sky
// position of the last pixel clicked on the 2-D channel map or the moment
// map, as coord.pointAt.sky; with "Send Positions of Clicked Pixels" on,
// every click sends one. A click is a left press and release a few pixels
// apart at most, with no tool that takes the click for itself
// (isInteractionToolActive), and not on a catalogue source: a drag pans. The
// position comes from the backend, which has the file's full WCS
// (pixel_to_sky); an older backend gets the client's own conversion.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "MainWindow.h"
#include "SampViewerMenu.h"
#include "app/DatasetName.h"
#include "app/SampSkyPosition.h"

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QStatusBar>

#include <vtkAlgorithm.h>
#include <vtkCommand.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkTrivialProducer.h>

#include <cmath>
#include <cstdlib>

using namespace Qt::StringLiterals;

namespace {
/// A press and release this close (display pixels) are a click, not a drag.
constexpr int kClickSlopPx = 4;
}  // namespace

void vtkWindowCube::setupSamp()
{
    visivo::installSampMenu(this, [this](QMenu *menu, MainWindow *host) {
        this->fillSampMenu(menu, host);
    });
    // The channel map and the moment map each have their own interactor.
    if (auto *iren = this->sliceWin->GetInteractor()) {
        // Ahead of the tools' own press handlers (see the image viewer).
        iren->AddObserver(vtkCommand::LeftButtonPressEvent, this, &vtkWindowCube::sampSlicePressed,
                          10.0f);
        iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                          &vtkWindowCube::sampSliceReleased);
    }
    if (auto *iren = this->momentWin->GetInteractor()) {
        iren->AddObserver(vtkCommand::LeftButtonPressEvent, this,
                          &vtkWindowCube::sampMomentPressed, 10.0f);
        iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, this,
                          &vtkWindowCube::sampMomentReleased);
    }
}

std::optional<visivo::samp::SkyPosition> vtkWindowCube::sampSkyAt(double pixelX,
                                                                  double pixelY) const
{
    visivo::samp::ImageSkyWcs wcs;
    for (std::size_t i = 0; i < 2; ++i) {
        wcs.ctype[i] = this->remoteDatasetCtype[i];
        wcs.crval[i] = this->remoteDatasetCrval[i];
        wcs.crpix[i] = this->remoteDatasetCrpix[i];
        wcs.cdelt[i] = this->remoteDatasetCdelt[i];
    }
    return visivo::samp::pixelToIcrs(wcs, pixelX, pixelY);
}

void vtkWindowCube::sampSlicePressed()
{
    this->sampPressIn(this->sliceWin);
}

void vtkWindowCube::sampMomentPressed()
{
    this->sampPressIn(this->momentWin);
}

void vtkWindowCube::sampSliceReleased()
{
    this->sampReleaseIn(this->sliceWin,
                        vtkImageData::SafeDownCast(
                                this->remoteSliceDisplaySource->GetOutputDataObject(0)));
}

void vtkWindowCube::sampMomentReleased()
{
    this->sampReleaseIn(this->momentWin,
                        vtkImageData::SafeDownCast(this->momentDisplaySource->GetOutputDataObject(0)));
}

void vtkWindowCube::sampPressIn(vtkRenderWindow *win)
{
    auto *iren = win ? win->GetInteractor() : nullptr;
    const int *p = iren ? iren->GetEventPosition() : nullptr;
    this->m_sampPressPos = p ? std::array<int, 2>{ p[0], p[1] } : std::array<int, 2>{ -1, -1 };
    this->m_sampPressTaken = this->isInteractionToolActive();
}

bool vtkWindowCube::isInteractionToolActive() const
{
    // Every tool whose clicks place or pick something: regions, PV paths, the
    // lasso (seed double-click, mask edits), the spectrum probe's pin.
    return this->regionMode != RegionMode::None || this->pvModeActive || this->m_lassoActive
           || this->m_lassoEditActive || this->probeModeActive || this->isBusyForPvInteraction();
}

void vtkWindowCube::sampReleaseIn(vtkRenderWindow *win, vtkImageData *imageData)
{
    auto *iren = win ? win->GetInteractor() : nullptr;
    const int *p = iren ? iren->GetEventPosition() : nullptr;
    const std::array<int, 2> press = this->m_sampPressPos;
    this->m_sampPressPos = { -1, -1 };
    if (!p || press[0] < 0 || std::abs(p[0] - press[0]) > kClickSlopPx
        || std::abs(p[1] - press[1]) > kClickSlopPx)
        return;   // a drag
    // A click a tool took, or one that picked a catalogue source, is not a
    // position the user means to point at.
    if (this->m_sampPressTaken || this->isInteractionToolActive())
        return;
    if (this->catalogueOverlayLoaded && this->selectedCatalogueSourceIndex >= 0)
        return;
    auto *renderer = win->GetRenderers()->GetFirstRenderer();
    if (!renderer || !imageData)
        return;
    this->coordinate->SetValue(p[0], p[1]);
    const double *world = this->coordinate->GetComputedWorldValue(renderer);
    if (!world)
        return;
    // The voxel the probe readout names for this point (same mapping).
    int extent[6];
    imageData->GetExtent(extent);
    double origin[3];
    double spacing[3];
    imageData->GetOrigin(origin);
    imageData->GetSpacing(spacing);
    const double voxelX = extent[0] + (world[0] - origin[0]) / spacing[0];
    const double voxelY = extent[2] + (world[1] - origin[1]) / spacing[1];
    if (std::lround(voxelX) < extent[0] || std::lround(voxelX) > extent[1]
        || std::lround(voxelY) < extent[2] || std::lround(voxelY) > extent[3])
        return;   // off the map
    this->m_sampClickPixel = { voxelX, voxelY };
    this->m_sampClickChannel = this->clampRemoteSliceIndex(ui->spinSlice->value() - 1);
    this->m_sampHasClick = true;
    if (this->m_sampBroadcastClicks)
        this->sendClickedPosition(QString(), /*quiet=*/true);
}

void vtkWindowCube::sendClickedPosition(const QString &recipientId, bool quiet)
{
    MainWindow *host = visivo::sampHost(this);
    QString problem;
    if (!this->m_sampHasClick)
        problem = u"click a pixel of the channel or moment map first"_s;
    else if (!host || !host->sampConnected())
        problem = u"not connected to a SAMP hub"_s;
    if (!problem.isEmpty()) {
        if (!quiet)
            this->statusBar()->showMessage(u"SAMP: no position sent: %1."_s.arg(problem), 6000);
        return;
    }
    const std::array<double, 2> pixel = this->m_sampClickPixel;
    const quint64 serial = ++this->m_sampClickSerial;   // a later click supersedes this one
    QPointer<vtkWindowCube> self = this;
    visivo::resolvePixelSky(
            this,
            { this->remoteBackendUrl, this->remoteBackendToken, this->remoteSessionId,
              this->remoteDatasetId, pixel[0], pixel[1],
              static_cast<double>(this->m_sampClickChannel) },
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

void vtkWindowCube::fillSampMenu(QMenu *menu, MainWindow *host)
{
    QPointer<vtkWindowCube> self = this;
    const QString path = this->filepath;
    const bool field = visivo::isFieldReference(path);
    const QString name = visivo::datasetDisplayName(path);
    QString cubeWhy;
    if (field)
        cubeWhy = u"A field of a VBT volume is not a FITS file other tools can load."_s;
    else if (host && host->sampSendBusy())
        cubeWhy = u"A file is still being prepared for sending."_s;
    visivo::addSampSendTo(menu, host, u"Send Cube To"_s, QString::fromLatin1(Samp::kImageLoadFits),
                          cubeWhy, [self, path, name](const QString &to) {
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
        positionWhy = u"The cube has no sky coordinates (RA/Dec, l/b or ecliptic axes)."_s;
    else if (!this->m_sampHasClick)
        positionWhy = u"Click a pixel of the channel or moment map first."_s;
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
    broadcast->setToolTip(u"While on, every pixel you click on the channel or moment map is "
                          u"sent to all SAMP tools as coord.pointAt.sky."_s);
    QObject::connect(broadcast, &QAction::toggled, this, [self](bool on) {
        if (self)
            self->m_sampBroadcastClicks = on;
    });
}
