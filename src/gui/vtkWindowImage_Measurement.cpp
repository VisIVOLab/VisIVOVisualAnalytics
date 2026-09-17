#include "vtkWindowImage.h"

#include "vtk/LutScale.h"
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
#include <functional>
#include <memory>
#include <cstring>
#include <sstream>

using namespace Qt::StringLiterals;
using namespace CubeGeom;  // shared viewport-geometry helpers (deduplicated)

// ── Measurement ──
// Extracted verbatim from vtkWindowImage.cpp. Ruler / angle measurement overlay + histogram panel.

void vtkWindowImage::setMeasurementMode(MeasurementMode mode)
{
    if (m_measMode == mode) return;

    if (this->probeModeActive && ui->actionProfile->isChecked())
        ui->actionProfile->setChecked(false);
    if (this->regionMode != RegionMode::None) {
        QAction *actions[] = { actionBoxRegion, actionCircleRegion,
                               actionPolygonRegion, actionAnnulusRegion };
        for (auto *a : actions) {
            if (a && a->isChecked()) a->setChecked(false);
        }
    }

    m_measMode = mode;
    clearMeasurement();
    if (mode == MeasurementMode::None) {
        setInteractorStyleImage();
        ui->vtk->setCursor(Qt::ArrowCursor);
    } else {
        setInteractorStyleRegion();
        ui->vtk->setCursor(Qt::CrossCursor);
    }
}

void vtkWindowImage::addMeasurementPoint(int voxelX, int voxelY)
{
    m_measPoints.push_back({ voxelX, voxelY });

    const int needed = (m_measMode == MeasurementMode::Ruler) ? 2 : 3;
    if (static_cast<int>(m_measPoints.size()) >= needed) {
        refreshMeasurementOverlay();
    } else {
        if (m_measLabelActor) {
            const QString hint = (m_measMode == MeasurementMode::Ruler)
                    ? u"Click second point…"_s
                    : QStringLiteral("Click point %1 of 3…").arg(m_measPoints.size() + 1);
            m_measLabelActor->SetInput(hint.toStdString().c_str());
            m_measLabelActor->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
            m_measLabelActor->SetPosition(0.5, 0.96);
            m_measLabelActor->VisibilityOn();
        }
    }
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

void vtkWindowImage::refreshMeasurementOverlay()
{
    // Stored measurement points are 0-based *display* voxel indices; the ruler
    // renders in world coordinates and angularSeparationDeg needs full-res pixel
    // indices. Map voxel → world (== full-res pixel via the image spacing) so
    // both are correct when the displayed image is decimated. No-op for a
    // full-res image (spacing 1, origin 0, extent0 0 → world == voxel).
    double mo[3] = { 0., 0., 0. };
    double ms[3] = { 1., 1., 1. };
    int me[6] = { 0, 0, 0, 0, 0, 0 };
    if (this->layers) {
        if (auto *img = this->layers->getImageData(this->layers->getMasterIndex())) {
            img->GetOrigin(mo);
            img->GetSpacing(ms);
            img->GetExtent(me);
        }
    }
    // me[] = {xmin, xmax, ymin, ymax, zmin, zmax}: Y must use me[2] (ymin), not me[1] (xmax).
    const auto toWorld = [&](const std::array<int, 2> &p) -> std::array<double, 2> {
        return { mo[0] + (p[0] - me[0]) * ms[0], mo[1] + (p[1] - me[2]) * ms[1] };
    };

    if (m_measMode == MeasurementMode::Ruler && m_measPoints.size() >= 2) {
        const auto a = toWorld(m_measPoints[0]);
        const auto b = toWorld(m_measPoints[1]);
        m_measLine1->SetPoint1(a[0], a[1], 0.0);
        m_measLine1->SetPoint2(b[0], b[1], 0.0);
        m_measLineActor1->VisibilityOn();
        m_measLineActor2->VisibilityOff();

        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double pixDist = std::sqrt(dx * dx + dy * dy);
        QString label = QStringLiteral("%1 px").arg(pixDist, 0, 'f', 1);

        const double angDeg = angularSeparationDeg(a[0], a[1], b[0], b[1]);
        if (angDeg >= 0.0) {
            const double arcsec = angDeg * 3600.0;
            if (arcsec < 60.0)
                label += QStringLiteral("  (%1\")").arg(arcsec, 0, 'f', 2);
            else if (arcsec < 3600.0)
                label += QStringLiteral("  (%1')").arg(arcsec / 60.0, 0, 'f', 2);
            else
                label += QStringLiteral("  (%1°)").arg(angDeg, 0, 'f', 4);
        }

        if (m_measLabelActor) {
            m_measLabelActor->SetInput(label.toStdString().c_str());
            m_measLabelActor->GetPositionCoordinate()->SetCoordinateSystemToWorld();
            m_measLabelActor->SetPosition((a[0] + b[0]) / 2.0 + 3.0,
                                           (a[1] + b[1]) / 2.0 + 3.0);
            m_measLabelActor->VisibilityOn();
        }
    } else if (m_measMode == MeasurementMode::Angle && m_measPoints.size() >= 3) {
        const auto a = toWorld(m_measPoints[0]);
        const auto b = toWorld(m_measPoints[1]);
        const auto c = toWorld(m_measPoints[2]);
        m_measLine1->SetPoint1(a[0], a[1], 0.0);
        m_measLine1->SetPoint2(b[0], b[1], 0.0);
        m_measLine2->SetPoint1(b[0], b[1], 0.0);
        m_measLine2->SetPoint2(c[0], c[1], 0.0);
        m_measLineActor1->VisibilityOn();
        m_measLineActor2->VisibilityOn();

        const double bax = a[0] - b[0], bay = a[1] - b[1];
        const double bcx = c[0] - b[0], bcy = c[1] - b[1];
        const double dot = bax * bcx + bay * bcy;
        const double magBA = std::sqrt(bax * bax + bay * bay);
        const double magBC = std::sqrt(bcx * bcx + bcy * bcy);
        double angleDeg = 0.0;
        if (magBA > 0.0 && magBC > 0.0)
            angleDeg = std::acos(std::clamp(dot / (magBA * magBC), -1.0, 1.0)) * 180.0 / M_PI;

        const QString label = QStringLiteral("%1°").arg(angleDeg, 0, 'f', 2);
        if (m_measLabelActor) {
            m_measLabelActor->SetInput(label.toStdString().c_str());
            m_measLabelActor->GetPositionCoordinate()->SetCoordinateSystemToWorld();
            m_measLabelActor->SetPosition(b[0] + 5.0, b[1] + 5.0);
            m_measLabelActor->VisibilityOn();
        }
    }
}

void vtkWindowImage::clearMeasurement()
{
    m_measPoints.clear();
    m_measLineActor1->VisibilityOff();
    m_measLineActor2->VisibilityOff();
    if (m_measLabelActor) m_measLabelActor->VisibilityOff();
    if (ui->vtk->renderWindow()) ui->vtk->renderWindow()->Render();
}

double vtkWindowImage::angularSeparationDeg(double vx1, double vy1, double vx2, double vy2) const
{
    if (!remoteHasCelestialAxes()) return -1.0;
    bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
    const double lon1 = remoteVoxelToWcs(0, vx1, &ok1);
    const double lat1 = remoteVoxelToWcs(1, vy1, &ok2);
    const double lon2 = remoteVoxelToWcs(0, vx2, &ok3);
    const double lat2 = remoteVoxelToWcs(1, vy2, &ok4);
    if (!ok1 || !ok2 || !ok3 || !ok4) return -1.0;

    const double deg2rad = M_PI / 180.0;
    const double dlon = (lon2 - lon1) * deg2rad;
    const double la1 = lat1 * deg2rad;
    const double la2 = lat2 * deg2rad;
    const double a = std::sin(dlon / 2.0);
    const double b = std::sin((la2 - la1) / 2.0);
    const double h = b * b + std::cos(la1) * std::cos(la2) * a * a;
    return 2.0 * std::asin(std::sqrt(std::clamp(h, 0.0, 1.0))) * 180.0 / M_PI;
}

void vtkWindowImage::showHistogramPanel()
{
    if (m_histogramWindow) {
        m_histogramWindow->raise();
        m_histogramWindow->activateWindow();
        return;
    }

    auto *imageData = this->layers
            ? this->layers->getImageData(this->layers->getMasterIndex()) : nullptr;
    if (!imageData) return;

    int extent[6];
    imageData->GetExtent(extent);
    const int width = extent[1] - extent[0] + 1;
    const int height = extent[3] - extent[2] + 1;
    if (width <= 0 || height <= 0) return;

    std::vector<double> values;
    const qint64 pixels = static_cast<qint64>(width) * height;
    const int stride = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(pixels) / 500000.0)));
    values.reserve(static_cast<std::size_t>(pixels / std::max(1, stride * stride)));
    for (int y = extent[2]; y <= extent[3]; y += stride) {
        for (int x = extent[0]; x <= extent[1]; x += stride) {
            const double v = imageData->GetScalarComponentAsDouble(x, y, 0, 0);
            if (std::isfinite(v)) values.push_back(v);
        }
    }
    if (values.empty()) return;

    std::sort(values.begin(), values.end());
    const double dataMin = values.front();
    const double dataMax = values.back();
    if (dataMax <= dataMin) return;

    // Bin over a ROBUST range, not min..max. On a radio map the extremes are a
    // handful of bright pixels while the information is in a band around the
    // noise: binning 0 .. 2 Jy/beam put every pixel of interest in the first of
    // 256 bins and drew what looked like an empty plot (user report). The 0.5 -
    // 99.5 percentile keeps the shape visible; values outside fall into the end
    // bins, so nothing is dropped, and the full range is one click away.
    const auto percentile = [&values](double p) {
        const double pos = p * static_cast<double>(values.size() - 1);
        const auto i = static_cast<std::size_t>(std::clamp(pos, 0.0,
                                                           double(values.size() - 1)));
        return values[i];
    };
    const double robustMin = percentile(0.005);
    const double robustMax = percentile(0.995);

    constexpr int nBins = 256;
    double vmin = (robustMax > robustMin) ? robustMin : dataMin;
    double vmax = (robustMax > robustMin) ? robustMax : dataMax;
    double binWidth = (vmax - vmin) / nBins;
    QVector<double> binCenters(nBins), counts(nBins, 0.0);
    const auto fillBins = [&]() {
        binWidth = (vmax - vmin) / nBins;
        counts.fill(0.0);
        for (int i = 0; i < nBins; ++i)
            binCenters[i] = vmin + (i + 0.5) * binWidth;
        for (const double v : values) {
            int bin = static_cast<int>((v - vmin) / binWidth);
            bin = std::clamp(bin, 0, nBins - 1);   // outliers land in the end bins
            counts[bin] += 1.0;
        }
    };
    fillBins();

    auto *win = new QWidget(nullptr, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(u"Pixel Histogram"_s);
    win->resize(600, 360);
    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *plot = new QCustomPlot(win);
    layout->addWidget(plot);

    auto *bars = new QCPBars(plot->xAxis, plot->yAxis);
    bars->setWidth(binWidth * 0.92);
    bars->setData(binCenters, counts);
    bars->setPen(QPen(QColor(70, 130, 180)));
    bars->setBrush(QColor(70, 130, 180, 120));

    plot->xAxis->setLabel(u"Pixel Value"_s);
    plot->yAxis->setLabel(u"Count"_s);
    plot->xAxis->setRange(vmin, vmax);
    // Counts on a LOG axis: the noise peak is orders of magnitude above the
    // source pixels, which on a linear axis are a flat line along zero.
    auto *logTicker = new QCPAxisTickerLog;
    plot->yAxis->setTicker(QSharedPointer<QCPAxisTicker>(logTicker));
    plot->yAxis->setScaleType(QCPAxis::stLogarithmic);
    const double maxCountNow = *std::max_element(counts.begin(), counts.end());
    plot->yAxis->setRange(0.5, std::max(1.0, maxCountNow) * 1.6);

    // …and the two choices stay the user's.
    auto *controls = new QWidget(win);
    auto *controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(4, 0, 4, 0);
    auto *robustCheck = new QCheckBox(u"Clip to 0.5–99.5 %"_s, controls);
    robustCheck->setChecked(true);
    robustCheck->setToolTip(u"Bin over the robust range instead of the full min..max, "
                            u"so a few bright pixels do not flatten the histogram."_s);
    auto *logCheck = new QCheckBox(u"Log counts"_s, controls);
    logCheck->setChecked(true);
    auto *rangeLabel = new QLabel(controls);
    rangeLabel->setText(u"data %1 … %2"_s.arg(dataMin, 0, 'g', 4).arg(dataMax, 0, 'g', 4));
    controlsLayout->addWidget(robustCheck);
    controlsLayout->addWidget(logCheck);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(rangeLabel);
    layout->addWidget(controls);

    auto *barsPtr = bars;
    auto reBin = std::make_shared<std::function<void()>>();
    *reBin = [=, &binCenters, &counts]() mutable {
        vmin = robustCheck->isChecked() && robustMax > robustMin ? robustMin : dataMin;
        vmax = robustCheck->isChecked() && robustMax > robustMin ? robustMax : dataMax;
        fillBins();
        barsPtr->setWidth(binWidth * 0.92);
        barsPtr->setData(binCenters, counts);
        plot->xAxis->setRange(vmin, vmax);
        const double top = *std::max_element(counts.begin(), counts.end());
        if (logCheck->isChecked()) {
            plot->yAxis->setScaleType(QCPAxis::stLogarithmic);
            plot->yAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTickerLog));
            plot->yAxis->setRange(0.5, std::max(1.0, top) * 1.6);
        } else {
            plot->yAxis->setScaleType(QCPAxis::stLinear);
            plot->yAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
            plot->yAxis->setRange(0, std::max(1.0, top) * 1.05);
        }
        plot->replot();
    };
    QObject::connect(robustCheck, &QCheckBox::toggled, win, [reBin](bool) { (*reBin)(); });
    QObject::connect(logCheck, &QCheckBox::toggled, win, [reBin](bool) { (*reBin)(); });

    auto *lut = this->layers->getLookupTable(this->layers->getMasterIndex());
    const double *lutRange = lut ? lut->GetTableRange() : nullptr;
    const double clipLow  = lutRange ? lutRange[0] : vmin;
    const double clipHigh = lutRange ? lutRange[1] : vmax;

    auto *lowLine = new QCPItemLine(plot);
    lowLine->setPen(QPen(QColor(220, 50, 50), 2, Qt::DashLine));
    lowLine->start->setType(QCPItemPosition::ptPlotCoords);
    lowLine->end->setType(QCPItemPosition::ptPlotCoords);
    lowLine->start->setCoords(clipLow, 0);
    lowLine->end->setCoords(clipLow, *std::max_element(counts.begin(), counts.end()) * 1.05);

    auto *highLine = new QCPItemLine(plot);
    highLine->setPen(QPen(QColor(50, 180, 50), 2, Qt::DashLine));
    highLine->start->setType(QCPItemPosition::ptPlotCoords);
    highLine->end->setType(QCPItemPosition::ptPlotCoords);
    highLine->start->setCoords(clipHigh, 0);
    highLine->end->setCoords(clipHigh, *std::max_element(counts.begin(), counts.end()) * 1.05);

    auto *lowLabel = new QCPItemText(plot);
    lowLabel->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    lowLabel->position->setType(QCPItemPosition::ptPlotCoords);
    lowLabel->position->setCoords(clipLow, *std::max_element(counts.begin(), counts.end()) * 1.02);
    lowLabel->setText(QStringLiteral("Low: %1").arg(clipLow, 0, 'g', 5));
    lowLabel->setColor(QColor(220, 50, 50));

    auto *highLabel = new QCPItemText(plot);
    highLabel->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    highLabel->position->setType(QCPItemPosition::ptPlotCoords);
    highLabel->position->setCoords(clipHigh, *std::max_element(counts.begin(), counts.end()) * 1.02);
    highLabel->setText(QStringLiteral("High: %1").arg(clipHigh, 0, 'g', 5));
    highLabel->setColor(QColor(50, 180, 50));

    QPointer<vtkWindowImage> self(this);
    QPointer<QCPItemLine> lowLinePtr(lowLine);
    QPointer<QCPItemLine> highLinePtr(highLine);
    QPointer<QCPItemText> lowLabelPtr(lowLabel);
    QPointer<QCPItemText> highLabelPtr(highLabel);

    bool *draggingLow = new bool(false);
    bool *draggingHigh = new bool(false);
    const double maxCount = *std::max_element(counts.begin(), counts.end()) * 1.05;

    QObject::connect(plot, &QCustomPlot::mousePress, win,
                     [plot, lowLinePtr, highLinePtr, draggingLow, draggingHigh](QMouseEvent *event) {
        const double x = plot->xAxis->pixelToCoord(event->pos().x());
        if (lowLinePtr && std::abs(x - lowLinePtr->start->coords().x()) < (plot->xAxis->range().size() * 0.015))
            *draggingLow = true;
        else if (highLinePtr && std::abs(x - highLinePtr->start->coords().x()) < (plot->xAxis->range().size() * 0.015))
            *draggingHigh = true;
    });

    QObject::connect(plot, &QCustomPlot::mouseMove, win,
                     [plot, lowLinePtr, highLinePtr, lowLabelPtr, highLabelPtr,
                      draggingLow, draggingHigh, maxCount](QMouseEvent *event) {
        if (!*draggingLow && !*draggingHigh) return;
        const double x = plot->xAxis->pixelToCoord(event->pos().x());
        if (*draggingLow && lowLinePtr) {
            lowLinePtr->start->setCoords(x, 0);
            lowLinePtr->end->setCoords(x, maxCount);
            if (lowLabelPtr) {
                lowLabelPtr->position->setCoords(x, maxCount * 0.98);
                lowLabelPtr->setText(QStringLiteral("Low: %1").arg(x, 0, 'g', 5));
            }
            plot->replot();
        } else if (*draggingHigh && highLinePtr) {
            highLinePtr->start->setCoords(x, 0);
            highLinePtr->end->setCoords(x, maxCount);
            if (highLabelPtr) {
                highLabelPtr->position->setCoords(x, maxCount * 0.98);
                highLabelPtr->setText(QStringLiteral("High: %1").arg(x, 0, 'g', 5));
            }
            plot->replot();
        }
    });

    QObject::connect(plot, &QCustomPlot::mouseRelease, win,
                     [self, lowLinePtr, highLinePtr, draggingLow, draggingHigh](QMouseEvent *) {
        if (*draggingLow || *draggingHigh) {
            if (self && lowLinePtr && highLinePtr) {
                double lo = lowLinePtr->start->coords().x();
                double hi = highLinePtr->start->coords().x();
                if (lo > hi) std::swap(lo, hi);
                if (hi > lo) {
                    const int master = self->layers->getMasterIndex();
                    auto *lut2 = self->layers->getLookupTable(master);
                    if (lut2) {
                        // Through LutScale: dragging the cursors across zero on a
                        // log stretch would otherwise hand VTK a range it rejects.
                        LutScale::setRange(lut2, self->layers->scaleMode(master), lo, hi);
                        lut2->Build();
                        self->markDisplayStateAdjusted("histogram drag");
                        self->vtkRender();
                    }
                }
            }
        }
        *draggingLow = false;
        *draggingHigh = false;
    });

    QObject::connect(win, &QObject::destroyed, win, [draggingLow, draggingHigh]() {
        delete draggingLow;
        delete draggingHigh;
    });

    plot->replot();
    m_histogramWindow = win;
    win->show();
}
