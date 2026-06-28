#include "vtkWindowImage.h"
#include "ui_vtkWindowImage.h"
#include "CubeViewportGeometry.h"

#include "app/BackendClient.h"
#include "app/DiagnosticsManager.h"
#include "app/ThemeManager.h"
#include "CatalogueTableModel.h"
#include "ColorMaps.h"
#include "CatalogueOverlayUtils.h"
#include "DiagnosticsWindow.h"
#include "ImageLayerController.h"
#include "ImageLayerImportService.h"
#include "ImageLayerLoadTask.h"
#include "LUTCustomizerDialog.h"
#include "LayerListModel.h"
#include "ProfileWidget.h"
#include "RemoteFileBrowserDialog.h"
#include "qcustomplot.h"
#include "SidebarPanel.h"
#include "theme/SegmentedToggle.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"
#include "ToolResultDialog.h"
#include "vtkInteractorStyleProfile.h"
#include "vtkLegendScaleActorWCS.h"
#include "wcs.h"

#include <vtkCamera.h>
#include <vtkAxisActor2D.h>
#include <vtkContourTriangulator.h>
#include <vtkCoordinate.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageSlice.h>
#include <vtkImageStack.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleUser.h>
#include <vtkLineSource.h>
#include <vtkLookupTable.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkPolyData.h>
#include "vtkFITSReader.h"
#include <vtkContourFilter.h>
#include <vtkFlyingEdges2D.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDebug>
#include <QKeySequence>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QPlainTextEdit>
#include <QDialog>
#include <QSlider>
#include <QStringList>
#include <QToolBar>
#include <QToolButton>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

using namespace Qt::StringLiterals;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

// ── Dynspec ──
// Extracted verbatim from vtkWindowImage.cpp. Dynamic-spectrum tools: dedispersion / pulse profile / RFI mask.

void vtkWindowImage::setDynamicSpectrumMode(bool enabled)
{
    if (m_dynamicSpectrumMode == enabled) return;
    m_dynamicSpectrumMode = enabled;

    // Hide celestial-WCS specific tools and overlays. They make no sense for
    // a time × frequency waterfall: no beam, no Galactic / FK5 frame, no
    // polarimetry workflow, no catalogue overlay against sky coords.
    const auto setActionEnabled = [enabled](QAction *act) {
        if (!act) return;
        act->setEnabled(!enabled);
        act->setVisible(!enabled);
    };
    setActionEnabled(actionWcsSexagesimal);
    setActionEnabled(actionWcsDecimal);
    setActionEnabled(actionLoadCatalogueOverlay);
    setActionEnabled(actionShowCatalogueOverlay);
    setActionEnabled(actionShowCatalogueLabels);
    setActionEnabled(actionClearCatalogueOverlay);
    setActionEnabled(m_actLoadStokesCompanions);
    setActionEnabled(m_actComputeStokesP);
    setActionEnabled(m_actComputeStokesPA);
    setActionEnabled(m_actComputeStokesFrac);
    setActionEnabled(m_actComputeStokesPdebiased);
    setActionEnabled(m_actComputeSpectralIndex);
    setActionEnabled(m_actComputeFaradayRM);
    setActionEnabled(m_actTogglePolVecs);
    if (ui->actionGalactic)  ui->actionGalactic->setEnabled(!enabled);
    if (ui->actionFK5)       ui->actionFK5->setEnabled(!enabled);
    if (ui->actionEcliptic)  ui->actionEcliptic->setEnabled(!enabled);

    // Sidebar cards: hide celestial-only ones and reveal the Time-Series
    // Tools placeholder. Done outside the if/else so the inverse direction
    // (re-enabling celestial mode after a dynspec was loaded) also works.
    for (const auto &card : std::as_const(m_dynspecHiddenCards)) {
        if (card) card->setVisible(!enabled);
    }
    if (m_dynspecToolsCard) {
        m_dynspecToolsCard->setVisible(enabled);
    }

    if (enabled) {
        // Beam indicator is meaningless for beamformed data.
        m_beamMajorDeg = std::numeric_limits<double>::quiet_NaN();
        m_beamMinorDeg = std::numeric_limits<double>::quiet_NaN();
        m_beamPaDeg    = std::numeric_limits<double>::quiet_NaN();
        if (m_beamActor) m_beamActor->VisibilityOff();
        // Status-bar panels: replace sanity messaging with a stable note.
        if (this->sanityLabel) {
            this->sanityLabel->setText(u"Dynamic Spectrum"_s);
            this->sanityLabel->setToolTip(
                    u"This dataset is a beamformed / dynamic-spectrum product. "
                    u"Time × frequency axes; celestial-WCS tools are disabled."_s);
        }
    }

    if (ui->vtk && ui->vtk->renderWindow()) {
        this->invalidateWcsOverlayCache();
        this->requestWcsOverlayRender();
    }
}

void vtkWindowImage::runDynspecDedispersion()
{
    if (!m_dynamicSpectrumMode) return;
    auto *edit = this->findChild<QLineEdit *>(u"dynspecDmEdit"_s);
    if (!edit) return;
    const double dm = edit->text().toDouble();
    if (dm <= 0.0) {
        QMessageBox::information(this, u"Dedispersion"_s,
                u"Set a positive Dispersion Measure (pc cm⁻³) before computing."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Dedispersing at DM=%1 pc cm⁻³…").arg(dm), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecDedisperseResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecDedisperseResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Dedispersion"_s,
                    u"Dedispersion failed:\n\n%1"_s.arg(r.error));
            return;
        }
        QMessageBox::information(self, u"Dedispersion"_s,
                u"Dedispersed dynspec saved.\n\n"
                u"Filename: %1\nDM: %2 pc cm⁻³\nRef. freq.: %3 MHz\n"
                u"Max channel shift: %4 samples\n\n"
                u"Open the file in a new viewer or as a layer to inspect "
                u"the result."_s
                .arg(r.workspaceFilename)
                .arg(r.dmPcCm3, 0, 'f', 4)
                .arg(r.refFreqMhz, 0, 'f', 3)
                .arg(r.maxShiftSamples));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, dm]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.dedisperseDynspec(dsId, dm, /*refFreqMhz=*/0.0, QString());
    }));
}

void vtkWindowImage::runDynspecPulseProfile()
{
    if (!m_dynamicSpectrumMode) return;
    auto *pEdit = this->findChild<QLineEdit *>(u"dynspecPeriodEdit"_s);
    auto *dmEdit = this->findChild<QLineEdit *>(u"dynspecDmEdit"_s);
    if (!pEdit) return;
    const double period = pEdit->text().toDouble();
    const double dm = dmEdit ? dmEdit->text().toDouble() : 0.0;
    if (period <= 0.0) {
        QMessageBox::information(this, u"Pulse Profile"_s,
                u"Set a positive period (seconds) before folding."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Folding at P=%1 s (DM=%2)…")
                    .arg(period, 0, 'g', 6).arg(dm, 0, 'f', 3), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecPulseProfileResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecPulseProfileResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"Pulse Profile"_s,
                    u"Pulse profile computation failed:\n\n%1"_s.arg(r.error));
            return;
        }

        // Floating QCustomPlot window with the folded profile
        auto *win = new QWidget(nullptr, Qt::Window);
        win->setAttribute(Qt::WA_DeleteOnClose);
        win->setWindowTitle(
                QStringLiteral("Pulse Profile — P=%1 s · DM=%2 · SNR=%3")
                        .arg(r.periodS, 0, 'g', 6)
                        .arg(r.dmPcCm3, 0, 'f', 3)
                        .arg(r.snrPeak, 0, 'f', 1));
        win->resize(640, 360);
        auto *layout = new QVBoxLayout(win);
        layout->setContentsMargins(4, 4, 4, 4);

        auto *info = new QLabel(QStringLiteral(
                "Period: %1 s    DM: %2 pc cm⁻³    Bins: %3    "
                "Peak: %4    σ_MAD: %5    SNR (peak/σ): %6")
                        .arg(r.periodS, 0, 'g', 6).arg(r.dmPcCm3, 0, 'f', 3)
                        .arg(r.nBins)
                        .arg(r.peak, 0, 'g', 5).arg(r.sigmaMad, 0, 'g', 5)
                        .arg(r.snrPeak, 0, 'f', 2), win);
        layout->addWidget(info);

        auto *plot = new QCustomPlot(win);
        layout->addWidget(plot);
        plot->addGraph();
        plot->graph(0)->setData(r.phase, r.profile);
        plot->graph(0)->setPen(QPen(QColor(70, 130, 180), 1.5));
        plot->graph(0)->setBrush(QBrush(QColor(70, 130, 180, 40)));
        plot->xAxis->setLabel(u"Pulse phase"_s);
        plot->yAxis->setLabel(u"Mean intensity"_s);
        if (!r.phase.isEmpty()) {
            plot->xAxis->setRange(0.0, 1.0);
            double minY = std::numeric_limits<double>::infinity();
            double maxY = -std::numeric_limits<double>::infinity();
            for (double v : r.profile) {
                if (std::isfinite(v)) {
                    minY = std::min(minY, v);
                    maxY = std::max(maxY, v);
                }
            }
            if (std::isfinite(minY) && std::isfinite(maxY) && minY < maxY) {
                const double pad = (maxY - minY) * 0.10;
                plot->yAxis->setRange(minY - pad, maxY + pad);
            }
        }
        plot->replot();
        win->show();
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, period, dm]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.pulseProfile(dsId, period, /*nBins=*/64, dm,
                              /*fLo*/ 0.0, /*fHi*/ 0.0);
    }));
}

void vtkWindowImage::runDynspecRfiMask()
{
    if (!m_dynamicSpectrumMode) return;
    auto *edit = this->findChild<QLineEdit *>(u"dynspecSigmaEdit"_s);
    if (!edit) return;
    const double sigma = edit->text().toDouble();
    if (sigma <= 0.0) {
        QMessageBox::information(this, u"RFI Mask"_s,
                u"Set a positive σ threshold before flagging."_s);
        return;
    }

    const QString url   = this->remoteBackendUrl;
    const QString token = this->remoteBackendToken;
    const QString sid   = this->remoteSessionId;
    const QString dsId  = this->remoteDatasetId;

    statusBar()->showMessage(
            QStringLiteral("Flagging RFI at %1 σ…").arg(sigma), 0);

    QPointer<vtkWindowImage> self(this);
    auto *watcher = new QFutureWatcher<BackendDynspecRfiMaskResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDynspecRfiMaskResult>::finished,
                     this, [self, watcher]() {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->statusBar()->clearMessage();
        if (!r.valid) {
            QMessageBox::warning(self, u"RFI Mask"_s,
                    u"RFI mask computation failed:\n\n%1"_s.arg(r.error));
            return;
        }
        QMessageBox::information(self, u"RFI Mask"_s,
                u"RFI mask saved to Workspace Exports.\n\n"
                u"Filename: %1\n"
                u"Flagged fraction: %2 %\n"
                u"σ threshold: %3 (across %4 axis)\n\n"
                u"Open it as a layer over the source dynspec to highlight "
                u"the flagged pixels."_s
                .arg(r.workspaceFilename)
                .arg(r.flaggedFraction * 100.0, 0, 'f', 2)
                .arg(r.nSigma, 0, 'f', 1)
                .arg(r.axis));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sid, dsId, sigma]() {
        BackendClient c(url, token);
        c.setSessionId(sid);
        return c.rfiMask(dsId, sigma, QStringLiteral("channel"), QString());
    }));
}
