#include "ChannelMapsWindow.h"

#include "qcustomplot/qcustomplot.h"
#include "theme/VisivoTheme.h"
#include "vtk/ColorMaps.h"

#include <vtkLookupTable.h>
#include <vtkNew.h>

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

// ── LUT → QCPColorGradient conversion ──────────────────────────────────────
// Samples the VTK lookup table at 256 points and builds a matching QCP
// gradient so the channel-map panels use the same palette as the slice
// viewer without depending on the VTK rendering pipeline.

static QCPColorGradient lutToGradient(const QString &lutName)
{
    vtkNew<vtkLookupTable> lut;
    lut->SetNumberOfTableValues(256);
    ColorMaps::SetColorMap(lut, lutName.toStdString());
    lut->Build();

    QCPColorGradient grad;
    grad.clearColorStops();
    for (int i = 0; i < 256; ++i) {
        const double *rgba = lut->GetTableValue(i);
        const double pos = static_cast<double>(i) / 255.0;
        grad.setColorStopAt(pos, QColor::fromRgbF(rgba[0], rgba[1], rgba[2]));
    }
    return grad;
}

// ── Construction ────────────────────────────────────────────────────────────

ChannelMapsWindow::ChannelMapsWindow(
        const ChannelMapsConfig &config,
        const QString &backendUrl, const QString &token,
        const QString &sessionId, const QString &datasetId,
        int cubeWidth, int cubeHeight, int cubeDepth,
        const QString &filepath, QWidget *parent)
    : QMainWindow(parent)
    , m_config(config)
    , m_backendUrl(backendUrl), m_token(token)
    , m_sessionId(sessionId), m_datasetId(datasetId)
    , m_cubeWidth(cubeWidth), m_cubeHeight(cubeHeight), m_cubeDepth(cubeDepth)
    , m_filepath(filepath)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window);
    setWindowTitle(u"Channel Maps — %1"_s
            .arg(QFileInfo(filepath).fileName()));
    resize(1200, 800);

    const QString bg = QString::fromLatin1(VisivoTheme::kSurface());
    setStyleSheet(u"ChannelMapsWindow { background: %1; }"_s.arg(bg));

    buildUi();
    fetchAndRender();
}

void ChannelMapsWindow::buildUi()
{
    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header
    auto *header = new QFrame(central);
    header->setObjectName(u"cmHeader"_s);
    header->setStyleSheet(
            u"QFrame#cmHeader {"
            u"  background: %1; border-bottom: 1px solid %2;"
            u"}"_s.arg(QString::fromLatin1(VisivoTheme::kSurfaceContainerLow()),
                       QString::fromLatin1(VisivoTheme::kOutline())));
    auto *headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(14, 8, 14, 8);
    auto *title = new QLabel(
            u"Channel Maps  ·  CH %1–%2  stride %3  ·  %4"_s
            .arg(m_config.channelStart).arg(m_config.channelEnd)
            .arg(m_config.stride).arg(m_config.lutName),
            header);
    title->setStyleSheet(
            u"font-size: 13px; font-weight: bold; color: %1; "
            u"background: transparent;"_s
            .arg(QString::fromLatin1(VisivoTheme::kOnSurface())));
    headerLay->addWidget(title, 1);
    m_statusLabel = new QLabel(u"Loading…"_s, header);
    m_statusLabel->setStyleSheet(
            u"font-size: 11px; color: %1; background: transparent;"_s
            .arg(QString::fromLatin1(VisivoTheme::kOnSurfaceVariant())));
    headerLay->addWidget(m_statusLabel);
    root->addWidget(header);

    // Scroll area for the grid
    m_scroll = new QScrollArea(central);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet(u"QScrollArea { background: %1; border: none; }"_s
            .arg(QString::fromLatin1(VisivoTheme::kBackground())));
    auto *container = new QWidget(m_scroll);
    container->setStyleSheet(u"background: transparent;"_s);
    m_grid = new QGridLayout(container);
    m_grid->setContentsMargins(8, 8, 8, 8);
    m_grid->setSpacing(4);
    m_scroll->setWidget(container);
    root->addWidget(m_scroll, 1);

    // Footer
    auto *footer = new QFrame(central);
    footer->setObjectName(u"cmFooter"_s);
    footer->setStyleSheet(
            u"QFrame#cmFooter { background: %1; border-top: 1px solid %2; }"_s
            .arg(QString::fromLatin1(VisivoTheme::kSurfaceContainerLow()),
                 QString::fromLatin1(VisivoTheme::kOutline())));
    auto *footerLay = new QHBoxLayout(footer);
    footerLay->setContentsMargins(12, 8, 12, 8);
    auto *saveBtn = new QPushButton(u"Save mosaic as PNG…"_s, footer);
    QObject::connect(saveBtn, &QPushButton::clicked, this,
                     &ChannelMapsWindow::saveMosaicPng);
    footerLay->addStretch();
    footerLay->addWidget(saveBtn);
    root->addWidget(footer);
}

// ── Data fetch (async, one subvolume call) ──────────────────────────────────

void ChannelMapsWindow::fetchAndRender()
{
    const QString url   = m_backendUrl;
    const QString token = m_token;
    const QString sid   = m_sessionId;
    const QString did   = m_datasetId;
    const int z0 = m_config.channelStart;
    const int z1 = m_config.channelEnd;
    const int w  = m_cubeWidth;
    const int h  = m_cubeHeight;

    auto *watcher = new QFutureWatcher<BackendCubeSubvolumeResult>(this);
    QPointer<ChannelMapsWindow> self(this);
    QObject::connect(watcher,
                     &QFutureWatcher<BackendCubeSubvolumeResult>::finished, this,
                     [self, watcher]() {
                         auto vol = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;
                         if (!vol.valid) {
                             self->m_statusLabel->setText(
                                     u"Error: %1"_s.arg(vol.error));
                             return;
                         }
                         self->populateGrid(vol);
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, sid, did, w, h, z0, z1]() {
                BackendClient c(url, token);
                c.setSessionId(sid);
                return c.requestSubvolume(did, 0, w - 1, 0, h - 1, z0, z1);
            }));
}

// ── Grid population ─────────────────────────────────────────────────────────

void ChannelMapsWindow::populateGrid(const BackendCubeSubvolumeResult &vol)
{
    const int stride  = std::max(1, m_config.stride);
    const int cols    = std::max(1, m_config.columns);
    const int w       = vol.width;
    const int h       = vol.height;
    const int depth   = vol.depth;
    const int planeBytes = w * h * int(sizeof(float));
    const auto *raw   = reinterpret_cast<const float *>(vol.data.constData());
    const int totalFloats = vol.data.size() / int(sizeof(float));

    // Build list of z-offsets relative to the sub-volume start.
    QVector<int> zOffsets;
    for (int z = 0; z < depth; z += stride)
        zOffsets.append(z);

    // Shared finite range across all selected panels.
    double sharedMin = vol.rangeMin;
    double sharedMax = vol.rangeMax;
    if (!std::isfinite(sharedMin) || !std::isfinite(sharedMax)
        || sharedMin >= sharedMax) {
        sharedMin = 0.0;
        sharedMax = 1.0;
    }

    const QCPColorGradient gradient = lutToGradient(m_config.lutName);
    const bool dark = VisivoTheme::isDark();
    const QColor bgColor(QString::fromLatin1(VisivoTheme::kBackground()));
    const QColor axisColor(QString::fromLatin1(VisivoTheme::kOutline()));
    const QColor labelColor(QString::fromLatin1(VisivoTheme::kOnSurface()));

    const int nPanels = zOffsets.size();
    const int nRows = static_cast<int>(
            std::ceil(static_cast<double>(nPanels) / cols));

    m_cells.clear();
    m_cells.reserve(nPanels);

    for (int idx = 0; idx < nPanels; ++idx) {
        const int zOff = zOffsets[idx];
        const int absChannel = m_config.channelStart + zOff;
        const int floatOffset = zOff * w * h;
        if (floatOffset + w * h > totalFloats) break;

        const int row = idx / cols;
        const int col = idx % cols;
        const bool leftEdge   = (col == 0);
        const bool bottomEdge = (row == nRows - 1);

        // Build one QCustomPlot with a QCPColorMap
        auto *plot = new QCustomPlot();
        plot->setMinimumSize(160, 140);
        plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        plot->setBackground(bgColor);
        plot->axisRect()->setBackground(bgColor);

        // Axis visibility: ticks/labels only on edges
        for (auto *axis : { plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2 }) {
            axis->setBasePen(QPen(axisColor));
            axis->setTickPen(QPen(axisColor));
            axis->setSubTickPen(QPen(axisColor));
            axis->setTickLabelColor(labelColor);
            axis->setLabelColor(labelColor);
        }
        plot->xAxis->setTicks(bottomEdge);
        plot->xAxis->setTickLabels(bottomEdge);
        plot->yAxis->setTicks(leftEdge);
        plot->yAxis->setTickLabels(leftEdge);
        plot->xAxis2->setVisible(false);
        plot->yAxis2->setVisible(false);

        // Title element: "CH 42"
        plot->plotLayout()->insertRow(0);
        auto *titleEl = new QCPTextElement(plot,
                u"CH %1"_s.arg(absChannel));
        titleEl->setTextColor(labelColor);
        QFont titleFont;
        titleFont.setPixelSize(13);
        titleFont.setWeight(QFont::DemiBold);
        titleEl->setFont(titleFont);
        titleEl->setMargins(QMargins(4, 4, 4, 2));
        plot->plotLayout()->addElement(0, 0, titleEl);

        // Color map
        auto *colorMap = new QCPColorMap(plot->xAxis, plot->yAxis);
        colorMap->data()->setSize(w, h);
        colorMap->data()->setRange(QCPRange(0, w - 1), QCPRange(0, h - 1));
        const float *plane = raw + floatOffset;
        for (int py = 0; py < h; ++py) {
            for (int px = 0; px < w; ++px) {
                const float v = plane[py * w + px];
                colorMap->data()->setCell(px, py,
                        std::isfinite(v) ? static_cast<double>(v)
                                         : sharedMin);
            }
        }
        colorMap->setDataRange(QCPRange(sharedMin, sharedMax));
        colorMap->setGradient(gradient);
        colorMap->setInterpolate(false);

        plot->xAxis->setRange(0, w - 1);
        plot->yAxis->setRange(0, h - 1);
        plot->setInteractions(QCP::Interactions());

        plot->replot(QCustomPlot::rpQueuedReplot);

        // Click-to-enlarge: double-click (or single click) any cell to
        // open a standalone dialog showing that single channel at full
        // resolution with a colour bar and axes.
        QObject::connect(plot, &QCustomPlot::mouseDoubleClick, this,
                         [this, idx, absChannel, sharedMin, sharedMax, gradient]() {
                             this->showEnlargedCell(idx, absChannel,
                                                   sharedMin, sharedMax,
                                                   gradient);
                         });
        plot->setCursor(Qt::PointingHandCursor);

        m_grid->addWidget(plot, row, col);
        m_cells.append(plot);
    }

    m_statusLabel->setText(
            u"%1 panels  ·  %2 × %3 px per panel  ·  range [%4, %5]"_s
            .arg(nPanels).arg(w).arg(h)
            .arg(sharedMin, 0, 'g', 5).arg(sharedMax, 0, 'g', 5));
}

// ── Enlarge a single cell (double-click) ────────────────────────────────────

void ChannelMapsWindow::showEnlargedCell(int cellIdx, int absChannel,
                                         double rangeMin, double rangeMax,
                                         const QCPColorGradient &gradient)
{
    if (cellIdx < 0 || cellIdx >= m_cells.size()) return;

    auto *src = m_cells[cellIdx];
    if (!src) return;
    auto *srcMap = qobject_cast<QCPColorMap *>(src->plottable(0));
    if (!srcMap) return;

    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(u"Channel %1"_s.arg(absChannel));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(640, 560);

    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(0, 0, 0, 0);

    auto *plot = new QCustomPlot(dlg);
    plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    const QColor bg(QString::fromLatin1(VisivoTheme::kBackground()));
    const QColor axis(QString::fromLatin1(VisivoTheme::kOutline()));
    const QColor lbl(QString::fromLatin1(VisivoTheme::kOnSurface()));
    plot->setBackground(bg);
    plot->axisRect()->setBackground(bg);
    for (auto *a : { plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2 }) {
        a->setBasePen(QPen(axis));
        a->setTickPen(QPen(axis));
        a->setSubTickPen(QPen(axis));
        a->setTickLabelColor(lbl);
        a->setLabelColor(lbl);
    }
    plot->xAxis->setLabel(u"X (pixels)"_s);
    plot->yAxis->setLabel(u"Y (pixels)"_s);
    plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    // Title
    plot->plotLayout()->insertRow(0);
    auto *title = new QCPTextElement(plot,
            u"CH %1"_s.arg(absChannel));
    title->setTextColor(lbl);
    QFont titleFont;
    titleFont.setPixelSize(14);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    plot->plotLayout()->addElement(0, 0, title);

    // Copy the color map data from the source
    auto *colorMap = new QCPColorMap(plot->xAxis, plot->yAxis);
    const int nx = srcMap->data()->keySize();
    const int ny = srcMap->data()->valueSize();
    colorMap->data()->setSize(nx, ny);
    colorMap->data()->setRange(srcMap->data()->keyRange(),
                                srcMap->data()->valueRange());
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
            colorMap->data()->setCell(x, y, srcMap->data()->cell(x, y));
    colorMap->setInterpolate(false);

    // Color scale (bar on the right) — link BEFORE setting the gradient,
    // because setColorScale() synchronises the scale's own (default)
    // gradient into the color map, overwriting any previously set one.
    auto *colorScale = new QCPColorScale(plot);
    plot->plotLayout()->addElement(1, 1, colorScale);
    colorScale->setType(QCPAxis::atRight);
    colorMap->setColorScale(colorScale);
    colorScale->axis()->setTickLabelColor(lbl);
    colorScale->axis()->setLabelColor(lbl);

    // Now set data range + gradient — AFTER the color scale link so the
    // gradient propagates to both the map and the scale bar correctly.
    colorMap->setDataRange(QCPRange(rangeMin, rangeMax));
    colorMap->setGradient(gradient);

    plot->xAxis->setRange(0, nx - 1);
    plot->yAxis->setRange(0, ny - 1);
    plot->replot();

    root->addWidget(plot);
    dlg->show();
}

// ── Mosaic PNG export ──────────────────────────────────────────────────────

void ChannelMapsWindow::saveMosaicPng()
{
    if (m_cells.isEmpty()) {
        QMessageBox::information(this, u"Save mosaic"_s,
                u"No panels to export — generate the channel maps first."_s);
        return;
    }

    const QString defaultDir =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString suggested =
            QDir(defaultDir.isEmpty() ? QDir::homePath() : defaultDir)
                    .filePath(u"%1_channel_maps.png"_s
                              .arg(QFileInfo(m_filepath).completeBaseName()));
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save channel maps mosaic"_s, suggested,
            u"PNG image (*.png);;All files (*)"_s);
    if (path.isEmpty()) return;

    const int cols    = std::max(1, m_config.columns);
    const int nPanels = m_cells.size();
    const int nRows   = static_cast<int>(
            std::ceil(static_cast<double>(nPanels) / cols));
    // Generous per-cell size so the "CH N" title text and axis labels
    // render at readable resolution (the old 256×220 was too small for
    // the QCPTextElement to lay out the channel number).
    const int cellW   = 400;
    const int cellH   = 340;
    const int headerH = 48;
    const qreal dpr   = 2.0;   // retina-quality export
    const QSize mosaicSize(cols * cellW, nRows * cellH + headerH);

    QImage mosaic(mosaicSize * dpr, QImage::Format_ARGB32);
    mosaic.setDevicePixelRatio(dpr);
    mosaic.fill(QColor(QString::fromLatin1(VisivoTheme::kBackground())));

    QPainter painter(&mosaic);
    painter.setRenderHint(QPainter::Antialiasing);

    // Title bar
    painter.setPen(QColor(QString::fromLatin1(VisivoTheme::kOnSurface())));
    QFont hdrFont;
    hdrFont.setPixelSize(18);
    hdrFont.setWeight(QFont::DemiBold);
    painter.setFont(hdrFont);
    painter.drawText(QRect(12, 0, mosaicSize.width() - 24, headerH),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     u"Channel Maps  ·  %1  ·  CH %2–%3  stride %4"_s
                     .arg(QFileInfo(m_filepath).fileName())
                     .arg(m_config.channelStart).arg(m_config.channelEnd)
                     .arg(m_config.stride));

    for (int i = 0; i < nPanels; ++i) {
        const int r = i / cols;
        const int c = i % cols;
        // Force the cell to lay out at the export size before rendering
        // so QCPTextElement ("CH 28") and axis ticks get proper space.
        // Without this, toPixmap uses the cell's on-screen size for text
        // metrics, which may be much smaller than the export resolution.
        auto *cell = m_cells[i];
        const QSize oldSize = cell->size();
        cell->resize(cellW, cellH);
        cell->replot(QCustomPlot::rpImmediateRefresh);
        const QPixmap pix = cell->toPixmap(cellW, cellH, dpr);
        cell->resize(oldSize);
        painter.drawPixmap(c * cellW, headerH + r * cellH, pix);
    }
    painter.end();

    if (!mosaic.save(path)) {
        QMessageBox::warning(this, u"Save mosaic"_s,
                u"Could not write:\n%1"_s.arg(path));
        return;
    }
    QMessageBox::information(this, u"Save mosaic"_s,
            u"Saved to:\n%1"_s.arg(path));
}
