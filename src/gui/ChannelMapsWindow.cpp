#include "ChannelMapsWindow.h"
#include "ChannelMapsLayout.h"
#include "WindowCloseShortcut.h"

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
#include <QGuiApplication>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <cstring>
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
    // Cmd+W closes it, as the title bar's button does (WindowCloseShortcut.h).
    visivo::installCloseWindowShortcut(this);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window);
    setWindowTitle(u"Channel Maps - %1"_s
            .arg(QFileInfo(filepath).fileName()));
    // Wide enough for the columns asked for, as far as the screen allows; the
    // panels then shrink to what is left (relayoutCells).
    {
        int width = 1200;
        if (const QScreen *screen = QGuiApplication::primaryScreen()) {
            const int wanted = visivo::cmlayout::viewportWidthFor(std::max(1, m_config.columns));
            width = std::min(wanted + 40, int(screen->availableGeometry().width() * 0.96));
        }
        resize(std::max(width, 640), 800);
    }

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
            .arg(m_config.channelStart + 1).arg(m_config.channelEnd + 1)   // as on the channel bar
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
    // Always there: a scrollbar that comes and goes changes the viewport's width,
    // and with it the panels' size.
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_scroll->viewport()->installEventFilter(this);
    m_scroll->setStyleSheet(u"QScrollArea { background: %1; border: none; }"_s
            .arg(QString::fromLatin1(VisivoTheme::kBackground())));
    auto *container = new QWidget(m_scroll);
    container->setStyleSheet(u"background: transparent;"_s);
    m_grid = new QGridLayout(container);
    m_grid->setContentsMargins(visivo::cmlayout::kGridMargin, visivo::cmlayout::kGridMargin,
                               visivo::cmlayout::kGridMargin, visivo::cmlayout::kGridMargin);
    m_grid->setSpacing(visivo::cmlayout::kGridSpacing);
    m_grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
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

namespace {
// Sky pixels are square: one data unit spans the same number of screen pixels on
// both axes, so a 196 × 280 cube is drawn taller than wide instead of stretched
// into the cell.
void keepSquarePixels(QCustomPlot *plot, int w, int h, bool interactive)
{
    // Only when the plot area's size changes: afterLayout also follows every
    // replot. A cell always shows the whole image; the zoomable enlarged view is
    // fitted once and afterwards only kept square around the current centre and
    // zoom (its area also changes when a zoom widens the tick labels).
    const auto fit = [plot, w, h, interactive, last = QSize()]() mutable {
        const QRect r = plot->axisRect()->rect();
        // The enlarged view fits once, so not before it is on screen: until its
        // layout places it the size is a placeholder it would then keep. (A cell
        // refits on every size change, so an early fit corrects itself.)
        if ((interactive && !plot->isVisible()) || r.width() <= 0 || r.height() <= 0 || w <= 0
            || h <= 0 || r.size() == last)
            return;
        const bool first = last.isEmpty();
        last = r.size();
        double cx = 0.5 * (w - 1), cy = 0.5 * (h - 1);
        double scale = std::min(r.width() / double(w), r.height() / double(h));   // px per pixel
        if (interactive && !first) {
            cx = plot->xAxis->range().center();
            cy = plot->yAxis->range().center();
            scale = r.width() / plot->xAxis->range().size();
        }
        const double halfX = 0.5 * r.width() / scale, halfY = 0.5 * r.height() / scale;
        plot->xAxis->setRange(QCPRange(cx - halfX, cx + halfX));
        plot->yAxis->setRange(QCPRange(cy - halfY, cy + halfY));
        plot->replot(QCustomPlot::rpQueuedReplot);
    };
    QObject::connect(plot, &QCustomPlot::afterLayout, plot, fit);
}
}  // namespace

// ── Grid population ─────────────────────────────────────────────────────────

QStringList ChannelMapsWindow::panelTitleLines(int absChannel) const
{
    // The channel as on the channel bar, and its value on the axis the cube
    // window shows, with its unit and frame (issue #496).
    QStringList lines{ u"CH %1"_s.arg(absChannel + 1) };
    const int i = absChannel - m_config.channelStart;
    if (i >= 0 && i < m_config.channelCaptions.size() && !m_config.channelCaptions.at(i).isEmpty())
        lines << m_config.channelCaptions.at(i);
    return lines;
}

QString ChannelMapsWindow::panelTitle(int absChannel) const
{
    return panelTitleLines(absChannel).join(u"  ·  "_s);
}

bool ChannelMapsWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_scroll && watched == m_scroll->viewport() && event->type() == QEvent::Resize)
        relayoutCells();
    return QMainWindow::eventFilter(watched, event);
}

void ChannelMapsWindow::relayoutCells()
{
    if (m_cells.isEmpty() || m_slices.isEmpty() || !m_scroll)
        return;
    namespace cm = visivo::cmlayout;
    const int cols = std::max(1, m_config.columns);
    const int nRows = (m_cells.size() + cols - 1) / cols;
    const int imageW = cm::imageWidth(m_scroll->viewport()->width(), cols);
    const int imageH = cm::imageHeight(imageW, m_slices[0].width, m_slices[0].height);
    for (int idx = 0; idx < m_cells.size(); ++idx) {
        QCustomPlot *plot = m_cells[idx];
        if (!plot)
            continue;
        const cm::PanelSize size = cm::panelSize(idx % cols, idx / cols, nRows, imageW, imageH,
                                                 m_cellTitleHeight);
        plot->axisRect()->setAutoMargins(QCP::msNone);
        plot->axisRect()->setMargins(
                QMargins(size.marginLeft, 0, size.marginRight, size.marginBottom));
        plot->setFixedSize(size.width, size.height);
        // The caption fits the panel: a smaller font first (down to 7 px), then
        // the middle elided; the whole text stays in the tooltip. Twelve
        // columns cut "268.291623e+11 Hz BARYCENT" at both ends (issue #502).
        if (auto *title = qobject_cast<QCPTextElement *>(plot->plotLayout()->element(0, 0))) {
            const QStringList lines = title->property("fullTitle").toStringList();
            if (!lines.isEmpty()) {
                const int avail = std::max(20, size.width - 4);
                QFont font = title->font();
                int px = 10;
                const auto widest = [&lines](const QFontMetrics &fm) {
                    int w = 0;
                    for (const QString &l : lines)
                        w = std::max(w, fm.horizontalAdvance(l));
                    return w;
                };
                for (; px > 7; --px) {
                    font.setPixelSize(px);
                    if (widest(QFontMetrics(font)) <= avail)
                        break;
                }
                font.setPixelSize(px);
                const QFontMetrics fm(font);
                QStringList fitted;
                for (const QString &l : lines)
                    fitted << fm.elidedText(l, Qt::ElideMiddle, avail);
                title->setFont(font);
                title->setText(fitted.join(u"\n"_s));
            }
        }
    }
}

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
    m_cellTitleHeight = 0;
    m_cells.reserve(nPanels);
    m_slices.clear();
    m_slices.reserve(nPanels);
    m_sharedMin = sharedMin;
    m_sharedMax = sharedMax;

    for (int idx = 0; idx < nPanels; ++idx) {
        const int zOff = zOffsets[idx];
        const int absChannel = m_config.channelStart + zOff;
        const int floatOffset = zOff * w * h;
        if (floatOffset + w * h > totalFloats) break;

        // Store raw data for the PNG export path.
        {
            ChannelSlice slice;
            slice.channel = absChannel;
            slice.width   = w;
            slice.height  = h;
            slice.data.resize(w * h);
            std::memcpy(slice.data.data(), raw + floatOffset,
                        w * h * sizeof(float));
            m_slices.append(std::move(slice));
        }

        const int row = idx / cols;
        const int col = idx % cols;
        const bool leftEdge   = (col == 0);
        const bool bottomEdge = (row == nRows - 1);

        // Build one QCustomPlot with a QCPColorMap
        auto *plot = new QCustomPlot();
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
        // Few, small ticks: a panel is ~140 px wide, and the default five labels
        // ran into each other.
        QFont tickFont;
        tickFont.setPixelSize(9);
        for (auto *axis : { plot->xAxis, plot->yAxis }) {
            axis->setTickLabelFont(tickFont);
            axis->setTickLength(3, 0);
            axis->setSubTickLength(0, 0);
            axis->setTickLabelPadding(2);
            axis->ticker()->setTickCount(3);
        }

        // Title: the channel, and under it its value on the spectral axis. The
        // text is as tall as its lines; the panel is sized from the same number.
        plot->plotLayout()->insertRow(0);
        const QStringList titleLines = panelTitleLines(absChannel);
        auto *titleEl = new QCPTextElement(plot, titleLines.join(u"\n"_s));
        // The full caption: relayoutCells() fits it to the panel's width.
        titleEl->setProperty("fullTitle", titleLines);
        plot->setToolTip(titleLines.join(u"  ·  "_s));
        titleEl->setTextColor(labelColor);
        QFont titleFont;
        titleFont.setPixelSize(10);
        titleFont.setWeight(QFont::DemiBold);
        titleEl->setFont(titleFont);
        titleEl->setMargins(QMargins(2, 3, 2, 1));
        plot->plotLayout()->addElement(0, 0, titleEl);
        m_cellTitleHeight = std::max(m_cellTitleHeight,
                                     QFontMetrics(titleFont).height() * int(titleLines.size()) + 4);
        plot->plotLayout()->setRowSpacing(0);

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
        keepSquarePixels(plot, w, h, /*interactive=*/false);
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

        m_grid->addWidget(plot, row, col, Qt::AlignLeft | Qt::AlignTop);
        m_cells.append(plot);
    }
    relayoutCells();

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
    dlg->setWindowTitle(u"Channel %1"_s.arg(absChannel + 1));
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
            panelTitle(absChannel));
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
    keepSquarePixels(plot, nx, ny, /*interactive=*/true);
    plot->replot();

    root->addWidget(plot);
    dlg->show();
}

// ── Mosaic PNG export ──────────────────────────────────────────────────────
// Bypass QCustomPlot's toPixmap entirely — its internal layout at the
// export size doesn't match the on-screen layout, causing truncated "CH"
// labels and shifted images. Instead we paint each channel directly from
// the stored raw float data + the VTK lookup table, and draw the labels
// with QPainter. Result is pixel-perfect and identical on every platform.

void ChannelMapsWindow::saveMosaicPng()
{
    if (m_slices.isEmpty()) {
        QMessageBox::information(this, u"Save mosaic"_s,
                u"No panels to export, generate the channel maps first."_s);
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

    // Build a 256-entry color table from the VTK LUT once.
    vtkNew<vtkLookupTable> lut;
    lut->SetNumberOfTableValues(256);
    ColorMaps::SetColorMap(lut, m_config.lutName.toStdString());
    lut->Build();
    QVector<QRgb> colorTable(256);
    for (int i = 0; i < 256; ++i) {
        const double *rgba = lut->GetTableValue(i);
        colorTable[i] = qRgb(
                std::clamp(int(rgba[0] * 255. + .5), 0, 255),
                std::clamp(int(rgba[1] * 255. + .5), 0, 255),
                std::clamp(int(rgba[2] * 255. + .5), 0, 255));
    }

    const int cols    = std::max(1, m_config.columns);
    const int nPanels = m_slices.size();
    const int nRows   = static_cast<int>(
            std::ceil(static_cast<double>(nPanels) / cols));

    // Full source resolution per cell with 2× DPR for retina-quality
    // exports. For a 512×512 cube with 64 panels (8 cols) this produces
    // an ~8200×8700 physical-pixel image — large but publication-grade.
    const int scale   = 2;
    const int imgW    = m_slices[0].width;
    const int imgH    = m_slices[0].height;
    const int pad     = 4  * scale;
    const int labelH  = 20 * scale;
    // A panel is as wide as its caption ("CH 16  ·  32.15 km/s LSRK"), so a small
    // image does not cut it.
    QFont captionFont;
    captionFont.setPixelSize(11 * scale);
    captionFont.setWeight(QFont::DemiBold);
    int captionW = 0;
    for (const auto &slice : std::as_const(m_slices))
        captionW = std::max(captionW,
                            QFontMetrics(captionFont).horizontalAdvance(panelTitle(slice.channel)));
    const int cellW   = std::max(imgW * scale, captionW) + pad;
    const int cellH   = imgH * scale + labelH + pad;
    const int headerH = 30 * scale;

    const QSize mosaicSize(cols * cellW, nRows * cellH + headerH);
    QImage mosaic(mosaicSize, QImage::Format_ARGB32);
    mosaic.fill(QColor(QString::fromLatin1(VisivoTheme::kBackground())));

    QPainter painter(&mosaic);
    painter.setRenderHint(QPainter::Antialiasing);

    // Title bar
    painter.setPen(QColor(QString::fromLatin1(VisivoTheme::kOnSurface())));
    QFont hdrFont;
    hdrFont.setPixelSize(14 * scale);
    hdrFont.setWeight(QFont::DemiBold);
    painter.setFont(hdrFont);
    painter.drawText(QRect(8 * scale, 0, mosaicSize.width() - 16 * scale, headerH),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     u"Channel Maps  ·  %1  ·  CH %2–%3  stride %4"_s
                     .arg(QFileInfo(m_filepath).fileName())
                     .arg(m_config.channelStart + 1).arg(m_config.channelEnd + 1)
                     .arg(m_config.stride));

    // Cell label font
    QFont cellFont;
    cellFont.setPixelSize(11 * scale);
    cellFont.setWeight(QFont::DemiBold);
    painter.setFont(cellFont);

    const double range = m_sharedMax - m_sharedMin;
    const double invRange = (range > 0.) ? 255.0 / range : 0.;

    for (int i = 0; i < nPanels; ++i) {
        const int r = i / cols;
        const int c = i % cols;
        const auto &slice = m_slices[i];

        const int cellX = c * cellW;
        const int cellY = headerH + r * cellH;

        // Label: "CH N" centred above the image
        painter.setPen(QColor(QString::fromLatin1(VisivoTheme::kOnSurface())));
        painter.drawText(QRect(cellX, cellY, cellW, labelH),
                         Qt::AlignCenter,
                         panelTitle(slice.channel));

        // Rasterise at full source resolution × scale factor.
        // Source data is row-major [y][x] with origin at bottom-left
        // (FITS convention); QImage row 0 is top, so we flip Y.
        const int outW = imgW * scale;
        const int outH = imgH * scale;
        QImage cellImg(outW, outH, QImage::Format_RGB32);
        const float *src = slice.data.constData();
        const int srcW = slice.width;
        const int srcH = slice.height;
        for (int py = 0; py < outH; ++py) {
            const int srcY = py * srcH / outH;
            const int flippedY = srcH - 1 - srcY;
            auto *scanline = reinterpret_cast<QRgb *>(cellImg.scanLine(py));
            for (int px = 0; px < outW; ++px) {
                const int srcX = px * srcW / outW;
                const float v = src[flippedY * srcW + srcX];
                int idx;
                if (!std::isfinite(v) || range <= 0.) {
                    idx = 0;
                } else {
                    idx = std::clamp(int((v - m_sharedMin) * invRange + .5),
                                     0, 255);
                }
                scanline[px] = colorTable[idx];
            }
        }
        painter.drawImage(cellX + (cellW - outW) / 2, cellY + labelH + pad / 2, cellImg);
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
