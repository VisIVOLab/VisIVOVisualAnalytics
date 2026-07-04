#include "ProfileWidget.h"
#include "ui_ProfileWidget.h"

#include "vtkInteractorStyleProfile.h"
#include "theme/VisivoTheme.h"

#include <vtkImageData.h>

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>
#include <QStandardPaths>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <QFutureWatcher>

#include "app/BackendClient.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

using namespace Qt::StringLiterals;

// ── Theme accessors ──────────────────────────────────────────────────────────
// Use VisivoTheme tokens so the profile window shares the exact same surface
// ramp as ToolResultDialog (Noise) and PvDiagramWidget. Hardcoded hex codes
// used to drift away from the global theme — these stay in sync forever.
namespace {
QString tBg()         { return QString::fromLatin1(VisivoTheme::kSurface()); }
QString tHeaderBg()   { return QString::fromLatin1(VisivoTheme::kSurfaceContainerLow()); }
QString tBorder()     { return QString::fromLatin1(VisivoTheme::kOutline()); }
QString tTitle()      { return QString::fromLatin1(VisivoTheme::kOnSurface()); }
QString tKey()        { return QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()); }
QString tValue()      { return QString::fromLatin1(VisivoTheme::kOnSurface()); }
QString tPrimary()    { return QString::fromLatin1(VisivoTheme::kPrimary()); }
QString tWarn()       { return QStringLiteral("#E8A840"); }   // freeze badge
QString tLive()       { return QStringLiteral("#5DA87A"); }   // live badge
}

/// Build the save footer bar (PNG buttons + CSV button) using the same
/// pattern as ToolResultDialog: a QDialogButtonBox with ActionRole buttons
/// lets the platform style size the buttons correctly without any manual
/// width calculation.
///
/// The footer is appended to @p widget's existing QVBoxLayout (installed by
/// setupUi), and the plot rows are given stretch=1 so the footer stays compact.
static void buildSaveFooter(QWidget *widget, QPushButton *&btn1Out,
                            QPushButton *&btn2Out, QPushButton *&btnCsvOut)
{
    auto *footer = new QFrame(widget);
    footer->setObjectName(u"profileFooter"_s);
    footer->setStyleSheet(
            u"QFrame#profileFooter {"
            u"  background: %1;"
            u"  border-top: 1px solid %2;"
            u"}"_s.arg(tHeaderBg(), tBorder()));

    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 8, 12, 8);

    auto *buttonBox = new QDialogButtonBox(footer);
    btn1Out   = new QPushButton(u"Save plot 1 as PNG…"_s);
    btn2Out   = new QPushButton(u"Save plot 2 as PNG…"_s);
    btnCsvOut = new QPushButton(u"Save spectrum as CSV…"_s);
    buttonBox->addButton(btn1Out,   QDialogButtonBox::ActionRole);
    buttonBox->addButton(btn2Out,   QDialogButtonBox::ActionRole);
    buttonBox->addButton(btnCsvOut, QDialogButtonBox::ActionRole);
    // Hidden by default; setupSpectrumPlot() shows it. Image-profile mode
    // doesn't have a single spectrum to export.
    btnCsvOut->hide();
    footerLayout->addWidget(buttonBox);

    // Attach to the outer VBoxLayout installed by setupUi.
    // Layout indices coming out of .ui: [0]=checkLive, [1]=plot1, [2]=plot2.
    auto *vLayout = qobject_cast<QVBoxLayout *>(widget->layout());
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);
    vLayout->setStretch(1, 1);  // plot1 — expand
    vLayout->setStretch(2, 1);  // plot2 — expand (0-height when hidden)
    vLayout->addWidget(footer);
}

ProfileWidget::ProfileWidget(vtkInteractorStyleProfile *style, vtkImageData *dataset,
                             const std::string &filepath, QWidget *parent)
    : QWidget(parent),
      ui(new Ui::ProfileWidget),
      dataset(dataset),
      astro(std::make_unique<AstroUtils>(filepath)),
      interactor(style)
{
    ui->setupUi(this);
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowFlag(Qt::Window);
    this->setStyleSheet(u"ProfileWidget { background: %1; }"_s.arg(tBg()));

    buildSaveFooter(this, m_btnSavePlot1, m_btnSavePlot2, m_btnSaveCsv);
    QObject::connect(m_btnSavePlot1, &QPushButton::clicked, this, &ProfileWidget::savePlot1Png);
    QObject::connect(m_btnSavePlot2, &QPushButton::clicked, this, &ProfileWidget::savePlot2Png);
    QObject::connect(m_btnSaveCsv,   &QPushButton::clicked, this, &ProfileWidget::saveSpectrumCsv);

    // The .ui-defined "Live Update" checkbox is hidden in every mode: the
    // freeze state is now driven by the cube viewer (click on slice to
    // toggle) and visualised via the Pin button in the spectrum header,
    // not via this checkbox. Kept in the .ui only to avoid invalidating
    // any saved layout state.
    ui->checkLive->hide();

    // Get info from dataset
    this->ScalarPointer = static_cast<float *>(this->dataset->GetScalarPointer());
    this->dataset->GetIncrements(this->Increments);
    this->dataset->GetDimensions(this->Dimensions);

    // Setup Interactor
    this->interactor->SetLengths(this->Dimensions[0] - 1, this->Dimensions[1] - 1);
}

ProfileWidget::ProfileWidget(QWidget *parent)
    : QWidget(parent),
      ui(new Ui::ProfileWidget),
      dataset(nullptr),
      astro(nullptr),
      ScalarPointer(nullptr),
      Dimensions{ 0, 0, 0 },
      Increments{ 0, 0, 0 },
      interactor(nullptr),
      refLineX(nullptr),
      refLineY(nullptr)
{
    ui->setupUi(this);
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowFlag(Qt::Window);
    this->setStyleSheet(u"ProfileWidget { background: %1; }"_s.arg(tBg()));

    buildSaveFooter(this, m_btnSavePlot1, m_btnSavePlot2, m_btnSaveCsv);
    QObject::connect(m_btnSavePlot1, &QPushButton::clicked, this, &ProfileWidget::savePlot1Png);
    QObject::connect(m_btnSavePlot2, &QPushButton::clicked, this, &ProfileWidget::savePlot2Png);
    QObject::connect(m_btnSaveCsv,   &QPushButton::clicked, this, &ProfileWidget::saveSpectrumCsv);

    // See note in the other constructor.
    ui->checkLive->hide();
}

ProfileWidget::~ProfileWidget()
{
    delete ui;
}

void ProfileWidget::setUsageMode(UsageMode mode, const QString &windowTitle)
{
    if (m_dialogInProgress) return;   // setWindowTitle would dismiss the sheet
    this->usageMode = mode;
    this->applyUsageModeUi(windowTitle);
}

void ProfileWidget::applyUsageModeUi(const QString &windowTitle)
{
    // The Live Update checkbox stays hidden in every mode now (replaced by
    // the Pin button in the spectrum header). Keep its underlying state false
    // for any remaining listeners.
    ui->checkLive->hide();
    if (this->interactor) {
        this->interactor->SetLiveMode(false);
    }

    if (!windowTitle.isEmpty()) {
        this->setWindowTitle(windowTitle);
    } else {
        const bool liveMode = this->usageMode == UsageMode::ProbeLive;
        this->setWindowTitle(liveMode ? u"Spectral Profile"_s : u"Region Spectral Profile"_s);
    }
}

void ProfileWidget::setupImagePlots()
{
    this->applyUsageModeUi();
    if (!this->interactor || !this->astro) {
        return;
    }
    this->interactor->SetCallback(
            [this](double x, double y, bool live) { this->plotProfile(x, y, live); });

    const std::string unit = this->astro->getPhysicalUnit();
    const QString yLabel = unit.empty() ? u"Intensity"_s : QString::fromStdString(unit);

    ui->plot1->addGraph();
    ui->plot1->setInteractions(QCP::iRangeDrag);
    ui->plot1->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plot1->plotLayout()->insertRow(0);
    ui->plot1->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot1, u"X Profile"_s));
    ui->plot1->xAxis->setLabel(u"X Coordinate"_s);
    ui->plot1->yAxis->setLabel(yLabel);
    this->refLineX = new QCPItemLine(ui->plot1);
    this->refLineX->setPen({ Qt::red });

    ui->plot2->addGraph();
    ui->plot2->setInteractions(QCP::iRangeDrag);
    ui->plot2->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plot2->plotLayout()->insertRow(0);
    ui->plot2->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot2, u"Y Profile"_s));
    this->refLineY = new QCPItemLine(ui->plot2);
    this->refLineY->setPen({ Qt::red });
    ui->plot2->xAxis->setLabel(u"Y Coordinate"_s);
    ui->plot2->yAxis->setLabel(yLabel);
}

void ProfileWidget::setupImagePlots(const QString &xLabel, const QString &yLabel)
{
    this->applyUsageModeUi();
    ui->plot1->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->plot2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    this->clearFitOverlay();   // drop fit refs before clearGraphs() frees them
    ui->plot1->clearGraphs();
    ui->plot2->clearGraphs();
    ui->plot1->addGraph();
    ui->plot2->addGraph();
    ui->plot1->setInteractions(QCP::iRangeDrag);
    ui->plot1->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plot2->setInteractions(QCP::iRangeDrag);
    ui->plot2->axisRect()->setRangeDrag(Qt::Horizontal);
    if (!ui->plot1->plotLayout()->element(0, 0)) {
        ui->plot1->plotLayout()->insertRow(0);
        ui->plot1->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot1, u"X Profile"_s));
    }
    if (!ui->plot2->plotLayout()->element(0, 0)) {
        ui->plot2->plotLayout()->insertRow(0);
        ui->plot2->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot2, u"Y Profile"_s));
    }
    ui->plot1->xAxis->setLabel(u"X Coordinate"_s);
    ui->plot1->yAxis->setLabel(yLabel);
    ui->plot2->xAxis->setLabel(u"Y Coordinate"_s);
    ui->plot2->yAxis->setLabel(yLabel);
    if (!this->refLineX) {
        this->refLineX = new QCPItemLine(ui->plot1);
        this->refLineX->setPen({ Qt::red });
    }
    if (!this->refLineY) {
        this->refLineY = new QCPItemLine(ui->plot2);
        this->refLineY->setPen({ Qt::red });
    }
    Q_UNUSED(xLabel);
}

void ProfileWidget::setupSpectrumPlot()
{
    this->applyUsageModeUi();
    if (!this->astro || !this->interactor) {
        return;
    }
    ui->plot2->hide();
    this->buildSpectrumHeader();

    this->interactor->SetCallback(
            [this](double x, double y, bool live) { this->plotSpectrum(x, y, live); });

    const std::string unit = this->astro->getPhysicalUnit();
    const QString yLabel = unit.empty() ? u"Intensity"_s : QString::fromStdString(unit);

    const std::string spectralUnit = this->astro->getAxisUnit(2);
    const QString xLabel = spectralUnit.empty() ? u"Spectral axis"_s
                                                : QString::fromStdString(spectralUnit);

    ui->plot1->addGraph();
    ui->plot1->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    ui->plot1->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    ui->plot1->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    ui->plot1->plotLayout()->insertRow(0);
    ui->plot1->plotLayout()->addElement(0, 0,
            new QCPTextElement(ui->plot1, u"Spectral Profile"_s));
    ui->plot1->xAxis->setLabel(xLabel);
    ui->plot1->yAxis->setLabel(yLabel);
}

void ProfileWidget::setupSpectrumPlot(const QString &xLabel, const QString &yLabel)
{
    this->applyUsageModeUi();
    ui->plot2->hide();
    if (m_btnSavePlot2) m_btnSavePlot2->hide();
    if (m_btnSavePlot1)
        m_btnSavePlot1->setText(u"Save spectrum as PNG…"_s);
    if (m_btnSaveCsv)
        m_btnSaveCsv->show();
    // Remember the axis labels for the CSV header (so columns aren't just
    // "x" / "y" — they say e.g. "velocity (m/s)" / "intensity (K)").
    m_lastSpectrumXLabel = xLabel.isEmpty() ? u"spectral_axis"_s : xLabel;
    m_lastSpectrumYLabel = yLabel.isEmpty() ? u"intensity"_s     : yLabel;

    this->buildSpectrumHeader();

    ui->plot1->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    this->clearFitOverlay();   // drop fit refs before clearGraphs() frees them
    ui->plot1->clearGraphs();
    m_pinnedGraphs.clear();    // clearGraphs() just freed the pinned graph pointers
    refreshPinButtons();
    ui->plot1->addGraph();
    ui->plot1->legend->setVisible(false);
    // Allow both axes to drag/zoom — astronomers regularly need to zoom
    // into a faint line tail without losing the rest of the spectrum.
    ui->plot1->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    ui->plot1->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    ui->plot1->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    ui->plot1->plotLayout()->insertRow(0);
    auto *titleEl = new QCPTextElement(ui->plot1, u"Spectral Profile"_s);
    ui->plot1->plotLayout()->addElement(0, 0, titleEl);
    ui->plot1->xAxis->setLabel(xLabel.isEmpty() ? u"Spectral axis"_s : xLabel);
    ui->plot1->yAxis->setLabel(yLabel.isEmpty() ? u"Intensity"_s : yLabel);
}

// ─────────────────────────────────────────────────────────────────────────────
// Spectrum header & stats bar (built lazily on first setupSpectrumPlot)
// ─────────────────────────────────────────────────────────────────────────────

void ProfileWidget::buildSpectrumHeader()
{
    if (m_specHeader) return;   // already built

    // ── HEADER BAND ─────────────────────────────────────────────────────────
    // Two-row layout: bold title left + Pin button right (row 1), meta info
    // + status badge (row 2). Matches the ToolResultDialog header pattern.
    m_specHeader = new QFrame(this);
    m_specHeader->setObjectName(u"profileSpecHeader"_s);
    m_specHeader->setStyleSheet(
            u"QFrame#profileSpecHeader {"
            u"  background: %1;"
            u"  border-bottom: 1px solid %2;"
            u"}"_s.arg(tHeaderBg(), tBorder()));

    auto *col = new QVBoxLayout(m_specHeader);
    col->setContentsMargins(14, 10, 14, 10);
    col->setSpacing(4);

    // Row 1: title + status badge (live / pinned). The Pin button is
    // intentionally GONE: the freeze toggle is owned by the click on the
    // 2D slice (first click pins at that pixel, second click unpins and
    // resumes live updates). A read-only badge reflects the current state.
    auto *row1 = new QHBoxLayout();
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(8);

    m_specTitleLabel = new QLabel(u"Spectral Profile"_s);
    m_specTitleLabel->setStyleSheet(
            u"font-size: 14px; font-weight: bold; color: %1; background: transparent;"_s
                    .arg(tTitle()));
    row1->addWidget(m_specTitleLabel, /*stretch=*/1);

    m_specStatusLabel = new QLabel();
    m_specStatusLabel->setStyleSheet(
            u"font-size: 11px; font-weight: 600; background: transparent; "
            u"padding: 3px 8px; border-radius: 4px;"_s);
    m_specStatusLabel->setMinimumWidth(70);
    m_specStatusLabel->setAlignment(Qt::AlignCenter);
    row1->addWidget(m_specStatusLabel);
    col->addLayout(row1);

    // Row 2: meta info — filename, pixel coords, optional WCS, hint about
    // the click-to-pin interaction so users discover it without docs.
    m_specMetaLabel = new QLabel(u"—"_s);
    m_specMetaLabel->setStyleSheet(
            u"font-size: 11px; color: %1; background: transparent; "
            u"font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"_s.arg(tKey()));
    m_specMetaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_specMetaLabel->setWordWrap(true);
    col->addWidget(m_specMetaLabel);

    // The Pin button is no longer used — keep the pointer null so the
    // refresh helper skips it (cheaper than null-checking everywhere).
    m_specPinBtn = nullptr;

    // Row 3: smoothing + line ID controls on a single compact row.
    // Keeps the header height minimal while giving access to both features.
    auto *toolRow = new QHBoxLayout();
    toolRow->setContentsMargins(0, 2, 0, 0);
    toolRow->setSpacing(8);

    // Smoothing combo — label embedded as first item so it reads naturally
    // as "Smooth: [Hanning ▼]". No maxWidth: let the dropdown size itself
    // from its longest item so nothing is clipped.
    auto *smoothLabel = new QLabel(u"Smooth:"_s);
    smoothLabel->setStyleSheet(
            u"font-size: 11px; color: %1; background: transparent;"_s
                    .arg(tKey()));
    toolRow->addWidget(smoothLabel);
    m_smoothCombo = new QComboBox();
    m_smoothCombo->addItem(u"None"_s,        0);
    m_smoothCombo->addItem(u"Hanning"_s,     1);
    m_smoothCombo->addItem(u"Boxcar 3"_s,    3);
    m_smoothCombo->addItem(u"Boxcar 5"_s,    5);
    m_smoothCombo->addItem(u"Boxcar 7"_s,    7);
    m_smoothCombo->addItem(u"Gaussian σ=1"_s, 10);
    m_smoothCombo->addItem(u"Gaussian σ=2"_s, 20);
    m_smoothCombo->setToolTip(
            u"Apply a spectral smoothing kernel for display only.\n"
            u"CSV export always saves the raw (unsmoothed) data."_s);
    m_smoothCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    toolRow->addWidget(m_smoothCombo);

    // Separator + Line ID buttons — small pill-style, matching the header
    // chrome so they don't look like out-of-place default buttons.
    const QString pillCss = QStringLiteral(
            "QPushButton { padding: 3px 10px; font-size: 10px;"
            " border: 1px solid %1; border-radius: 4px;"
            " background: transparent; color: %2; }"
            "QPushButton:hover { background: %3; }")
            .arg(tBorder(), tValue(),
                 QString::fromLatin1(VisivoTheme::kSurfaceContainerHigh()));
    toolRow->addStretch(1);
    auto *loadLinesBtn = new QPushButton(u"Load Lines…"_s);
    loadLinesBtn->setStyleSheet(pillCss);
    loadLinesBtn->setCursor(Qt::PointingHandCursor);
    loadLinesBtn->setToolTip(
            u"Load a CSV file of spectral lines (columns: frequency, label).\n"
            u"Draws vertical annotated markers on the plot at each frequency.\n"
            u"Frequencies must be in the same unit as the X axis."_s);
    auto *clearLinesBtn = new QPushButton(u"Clear Lines"_s);
    clearLinesBtn->setStyleSheet(pillCss);
    clearLinesBtn->setCursor(Qt::PointingHandCursor);
    m_btnFit = new QPushButton(u"Fit Gaussian"_s);
    m_btnFit->setStyleSheet(pillCss);
    m_btnFit->setCursor(Qt::PointingHandCursor);
    m_btnFit->setToolTip(
            u"Fit a single Gaussian (+ linear baseline) to the current spectrum\n"
            u"via the backend; overlays the fitted curve and reports peak, centre,\n"
            u"FWHM and integrated flux in the axis units. Enabled once connected."_s);
    m_btnFit->setEnabled(!m_backendUrl.isEmpty());
    // Pin / Clear-pins: overlay several regions' spectra for comparison.
    m_btnPin = new QPushButton(u"Pin Spectrum"_s);
    m_btnPin->setStyleSheet(pillCss);
    m_btnPin->setCursor(Qt::PointingHandCursor);
    m_btnPin->setToolTip(
            u"Pin the current spectrum as a comparison curve. Draw another region\n"
            u"(or move the probe) and its spectrum overlays on top, so several\n"
            u"regions can be compared on the same axes. Each pin gets its own colour."_s);
    m_btnClearPins = new QPushButton(u"Clear Pins"_s);
    m_btnClearPins->setStyleSheet(pillCss);
    m_btnClearPins->setCursor(Qt::PointingHandCursor);
    m_btnClearPins->setToolTip(u"Remove all pinned comparison spectra."_s);
    toolRow->addWidget(loadLinesBtn);
    toolRow->addWidget(clearLinesBtn);
    toolRow->addWidget(m_btnFit);
    toolRow->addWidget(m_btnPin);
    toolRow->addWidget(m_btnClearPins);
    col->addLayout(toolRow);

    QObject::connect(m_smoothCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                     this, [this](int) { this->applySmoothAndReplot(); });
    QObject::connect(loadLinesBtn,  &QPushButton::clicked,
                     this, &ProfileWidget::loadLineIdFile);
    QObject::connect(clearLinesBtn, &QPushButton::clicked,
                     this, &ProfileWidget::clearLineIdOverlay);
    QObject::connect(m_btnFit,      &QPushButton::clicked,
                     this, &ProfileWidget::requestLineFit);
    QObject::connect(m_btnPin,      &QPushButton::clicked,
                     this, [this]() { this->pinCurrentSpectrum(); });
    QObject::connect(m_btnClearPins, &QPushButton::clicked,
                     this, &ProfileWidget::clearPinnedSpectra);
    refreshPinButtons();

    // Insert the header at the very top of the widget's outer layout.
    auto *vLayout = qobject_cast<QVBoxLayout *>(this->layout());
    vLayout->insertWidget(0, m_specHeader);

    // ── STATS BAR (below the plot, above the footer) ────────────────────────
    m_specStatsBar = new QFrame(this);
    m_specStatsBar->setObjectName(u"profileSpecStats"_s);
    m_specStatsBar->setStyleSheet(
            u"QFrame#profileSpecStats {"
            u"  background: %1;"
            u"  border-top: 1px solid %2;"
            u"}"_s.arg(tHeaderBg(), tBorder()));
    auto *statsLayout = new QHBoxLayout(m_specStatsBar);
    statsLayout->setContentsMargins(14, 6, 14, 6);

    m_specStatsLabel = new QLabel(u"—"_s);
    m_specStatsLabel->setStyleSheet(
            u"font-size: 11px; color: %1; background: transparent; "
            u"font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"_s.arg(tValue()));
    m_specStatsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsLayout->addWidget(m_specStatsLabel, /*stretch=*/1);

    // Insert immediately before the footer (which was appended last by
    // buildSaveFooter); index -1 doesn't work in QVBoxLayout, so compute it.
    const int footerIdx = vLayout->count() - 1;
    vLayout->insertWidget(footerIdx, m_specStatsBar);

    refreshSpectrumHeader();
}

void ProfileWidget::refreshSpectrumHeader()
{
    if (!m_specHeader) return;

    // Region mode: no Live/Pinned concept (the spectrum is a one-shot mean
    // computed over a fixed region) and no "click to pin" hint. Show the
    // region description instead.
    if (this->usageMode == UsageMode::RegionStatic) {
        if (m_specStatusLabel) m_specStatusLabel->hide();
        QString text = m_regionMetaLine.isEmpty() ? u"Region spectrum"_s : m_regionMetaLine;
        if (!m_regionExtraLine.isEmpty()) {
            text += u"\n"_s + m_regionExtraLine;
        }
        m_specMetaLabel->setText(text);
        return;
    }

    // ── Probe-live (pixel) mode ────────────────────────────────────────────
    if (m_specStatusLabel) m_specStatusLabel->show();

    // Status badge — coloured pill: 🟢 Live | 🔒 Pinned
    if (m_frozen) {
        m_specStatusLabel->setText(u"\U0001F512 Pinned"_s);
        m_specStatusLabel->setStyleSheet(
                u"font-size: 11px; font-weight: 600; padding: 3px 8px; border-radius: 4px;"
                u"background: rgba(232,168,64,0.18); color: %1;"_s.arg(tWarn()));
    } else {
        m_specStatusLabel->setText(u"\U0001F7E2 Live"_s);
        m_specStatusLabel->setStyleSheet(
                u"font-size: 11px; font-weight: 600; padding: 3px 8px; border-radius: 4px;"
                u"background: rgba(93,168,122,0.18); color: %1;"_s.arg(tLive()));
    }

    // Meta row — filename, pixel coords, optional WCS, plus a one-line
    // hint that documents the click-to-pin interaction. Hint disappears
    // once a pixel has been pinned (it's just for discoverability).
    QStringList parts;
    if (!m_fileLabel.isEmpty()) parts << m_fileLabel;
    parts << u"pixel (%1, %2)"_s.arg(m_pixelX).arg(m_pixelY);
    if (!m_wcsLabel.isEmpty()) parts << m_wcsLabel;
    QString text = parts.join(u"  ·  "_s);
    if (m_frozen) {
        text += u"\nClick the slice again to unpin and resume live updates."_s;
    } else {
        text += u"\nHover the slice for a live preview; click a pixel to pin its spectrum."_s;
    }
    m_specMetaLabel->setText(text);
}

// ── Spectral smoothing (display only) ────────────────────────────────────────
// Applies a 1-D convolution kernel to m_lastSpectrumValues and replots.
// The raw data is not modified; CSV export always saves the original.

namespace {
QVector<double> convolve1D(const QVector<double> &data,
                           const QVector<double> &kernel)
{
    const int n = data.size();
    const int k = kernel.size();
    const int half = k / 2;
    QVector<double> out(n);
    for (int i = 0; i < n; ++i) {
        double sum = 0.0;
        double wsum = 0.0;
        for (int j = 0; j < k; ++j) {
            const int idx = i + j - half;
            if (idx >= 0 && idx < n && std::isfinite(data[idx])) {
                sum  += kernel[j] * data[idx];
                wsum += kernel[j];
            }
        }
        out[i] = (wsum > 0.0) ? sum / wsum : data[i];
    }
    return out;
}

QVector<double> gaussianKernel(double sigma)
{
    const int half = static_cast<int>(std::ceil(3.0 * sigma));
    const int size = 2 * half + 1;
    QVector<double> k(size);
    for (int i = 0; i < size; ++i) {
        const double x = i - half;
        k[i] = std::exp(-0.5 * x * x / (sigma * sigma));
    }
    return k;
}
} // namespace

void ProfileWidget::applySmoothAndReplot()
{
    if (m_dialogInProgress) return;
    if (m_lastSpectrumKey.isEmpty() || m_lastSpectrumValues.isEmpty()) return;
    if (!m_smoothCombo) return;

    const QVector<double> smoothed = displayedSpectrumValues();

    // Replot with smoothed data; keep the original in m_lastSpectrum* for
    // the next smooth-change and for CSV export.
    if (ui->plot1->graphCount() > 0) {
        ui->plot1->graph(0)->setData(m_lastSpectrumKey, smoothed);
        ui->plot1->rescaleAxes();
        ui->plot1->replot();
    }
    refreshSpectrumStats(m_lastSpectrumKey, smoothed);
}

QVector<double> ProfileWidget::displayedSpectrumValues() const
{
    const int mode = m_smoothCombo ? m_smoothCombo->currentData().toInt() : 0;
    if (mode == 1) {                       // Hanning (3-tap)
        return convolve1D(m_lastSpectrumValues, { 0.25, 0.5, 0.25 });
    } else if (mode >= 3 && mode < 10) {   // Boxcar of width N
        return convolve1D(m_lastSpectrumValues, QVector<double>(mode, 1.0));
    } else if (mode == 10) {
        return convolve1D(m_lastSpectrumValues, gaussianKernel(1.0));
    } else if (mode == 20) {
        return convolve1D(m_lastSpectrumValues, gaussianKernel(2.0));
    }
    return m_lastSpectrumValues;           // "None" — raw
}

void ProfileWidget::setRegionContext(const QString &shapeLabel,
                                     int validPixels, int totalPixels,
                                     const QString &extraInfo)
{
    if (m_dialogInProgress) return;
    QStringList parts;
    if (!shapeLabel.isEmpty()) parts << shapeLabel + u" region"_s;
    if (totalPixels > 0) {
        parts << u"%1 / %2 valid pixels"_s.arg(validPixels).arg(totalPixels);
    } else if (validPixels > 0) {
        parts << u"%1 valid pixels"_s.arg(validPixels);
    }
    m_regionMetaLine  = parts.join(u"  ·  "_s);
    m_regionExtraLine = extraInfo;
    refreshSpectrumHeader();
}

void ProfileWidget::refreshSpectrumStats(const QVector<double> &spectralKey,
                                         const QVector<double> &values)
{
    if (!m_specStatsLabel || values.isEmpty()) {
        if (m_specStatsLabel) m_specStatsLabel->setText(u"—"_s);
        return;
    }

    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    double sumSq = 0.0;
    int n = 0;
    for (double v : values) {
        if (std::isfinite(v)) {
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            sum += v;
            sumSq += v * v;
            ++n;
        }
    }
    if (n == 0) {
        m_specStatsLabel->setText(u"N=0 (all NaN)"_s);
        return;
    }
    const double mean = sum / n;
    // Population RMS (root-mean-square of the values themselves — useful as
    // a quick noise indicator on line-free spectra; for the "RMS of residuals
    // around the mean" use sqrt(variance) instead).
    const double rms = std::sqrt(sumSq / n);

    // Integrated flux: rectangular sum f * dv, where dv comes from the median
    // channel spacing on the X axis. This is a useful science readout
    // (column density proxy, line flux) and matches what moment-0 would
    // give over the same range.
    double integral = std::numeric_limits<double>::quiet_NaN();
    if (spectralKey.size() == values.size() && spectralKey.size() >= 2) {
        QVector<double> deltas;
        deltas.reserve(spectralKey.size() - 1);
        for (int i = 1; i < spectralKey.size(); ++i) {
            deltas.push_back(std::abs(spectralKey[i] - spectralKey[i - 1]));
        }
        std::sort(deltas.begin(), deltas.end());
        const double dv = deltas[deltas.size() / 2];
        double s = 0.0;
        for (double v : values) {
            if (std::isfinite(v)) s += v;
        }
        integral = s * dv;
    }

    auto fmt = [](double v) -> QString {
        return QString::number(v, 'g', 6);
    };

    QString text = u"N=%1   Min=%2   Max=%3   Mean=%4   RMS=%5"_s
                           .arg(n).arg(fmt(mn)).arg(fmt(mx)).arg(fmt(mean)).arg(fmt(rms));
    if (std::isfinite(integral)) {
        text += u"   ∫=%1"_s.arg(fmt(integral));
    }
    m_specStatsLabel->setText(text);
}

void ProfileWidget::setSpectrumContext(const QString &fileLabel,
                                        int pixelX, int pixelY,
                                        const QString &wcsLabel)
{
    if (m_dialogInProgress) return;   // suppress label mutations while saving
    m_fileLabel = fileLabel;
    m_pixelX    = pixelX;
    m_pixelY    = pixelY;
    m_wcsLabel  = wcsLabel;
    refreshSpectrumHeader();
}

void ProfileWidget::setSpectrumFrozen(bool frozen)
{
    if (m_dialogInProgress) return;   // suppress badge mutations while saving
    if (this->usageMode == UsageMode::RegionStatic) return; // no Live/Pinned in region mode
    if (m_frozen == frozen) return;
    m_frozen = frozen;
    refreshSpectrumHeader();
}

void ProfileWidget::setSpectrumCurrentChannel(double spectralValueAtChannel)
{
    // Draw a vertical reference line at the current slice's spectral value
    // so the user always knows which channel the slice viewer is showing.
    // Pass NaN to remove the marker.
    if (m_dialogInProgress) return;   // suppress replot while saving
    if (!ui->plot1) return;
    if (!std::isfinite(spectralValueAtChannel)) {
        if (m_channelMarker) {
            ui->plot1->removeItem(m_channelMarker);
            m_channelMarker = nullptr;
            ui->plot1->replot();
        }
        return;
    }
    if (!m_channelMarker) {
        m_channelMarker = new QCPItemLine(ui->plot1);
        m_channelMarker->setPen(QPen(QColor(tPrimary()), 1.0, Qt::DashLine));
    }
    m_channelMarker->start->setCoords(spectralValueAtChannel,
                                      ui->plot1->yAxis->range().lower);
    m_channelMarker->end->setCoords(spectralValueAtChannel,
                                    ui->plot1->yAxis->range().upper);
    ui->plot1->replot();
}

// ── Gaussian line fit (backend /v1/spectral/linefit) ─────────────────────────

void ProfileWidget::setBackend(const QString &url, const QString &token)
{
    m_backendUrl = url;
    m_backendToken = token;
    if (m_btnFit && !m_fitInProgress) m_btnFit->setEnabled(!url.isEmpty());
}

void ProfileWidget::clearFitOverlay()
{
    if (!ui->plot1) return;
    if (m_fitGraph) { ui->plot1->removeGraph(m_fitGraph); m_fitGraph = nullptr; }
    if (m_fitLabel) { ui->plot1->removeItem(m_fitLabel);  m_fitLabel = nullptr; }
}

void ProfileWidget::requestLineFit()
{
    if (m_backendUrl.isEmpty()) {
        QMessageBox::information(this, u"Line Fit"_s, u"Not connected to a backend."_s);
        return;
    }
    // Keep only finite (key,value) pairs: region spectra contain NaN channels,
    // and Qt serialises non-finite doubles to JSON null, which the backend
    // cannot fit. Filtering here guarantees a clean, equal-length payload.
    QVector<double> key, values;
    const int n = std::min(m_lastSpectrumKey.size(), m_lastSpectrumValues.size());
    key.reserve(n);
    values.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double k = m_lastSpectrumKey.at(i), v = m_lastSpectrumValues.at(i);
        if (std::isfinite(k) && std::isfinite(v)) { key.append(k); values.append(v); }
    }
    if (key.size() < 4) {
        QMessageBox::information(this, u"Line Fit"_s,
                u"Need at least 4 finite spectral channels to fit a Gaussian."_s);
        return;
    }
    const QString url = m_backendUrl, token = m_backendToken;
    const int gen = m_spectrumGeneration;
    m_fitInProgress = true;
    if (m_btnFit) m_btnFit->setEnabled(false);
    QPointer<ProfileWidget> self(this);
    auto *watcher = new QFutureWatcher<BackendLineFitResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendLineFitResult>::finished, this, [self, watcher, gen]() {
        const BackendLineFitResult fit = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        self->m_fitInProgress = false;
        if (self->m_btnFit) self->m_btnFit->setEnabled(!self->m_backendUrl.isEmpty());
        if (self->m_spectrumGeneration != gen) return;  // spectrum changed → stale fit
        if (!fit.valid) {
            QMessageBox::warning(self, u"Line Fit"_s, u"Fit failed:\n%1"_s.arg(fit.error));
            return;
        }
        self->overlayLineFit(fit);
    });
    watcher->setFuture(QtConcurrent::run([url, token, key, values]() -> BackendLineFitResult {
        BackendClient c(url, token);
        return c.fitSpectralLine(values, key, /*withBaseline=*/true);
    }));
}

void ProfileWidget::overlayLineFit(const BackendLineFitResult &fit)
{
    if (!ui->plot1 || m_lastSpectrumKey.isEmpty()) return;
    this->clearFitOverlay();

    // Sample the fitted model (linear baseline + Gaussian) across the spectrum.
    double xmin = std::numeric_limits<double>::infinity();
    double xmax = -std::numeric_limits<double>::infinity();
    for (double k : m_lastSpectrumKey) {
        if (!std::isfinite(k)) continue;
        xmin = std::min(xmin, k);
        xmax = std::max(xmax, k);
    }
    if (!std::isfinite(xmin) || !std::isfinite(xmax) || xmax <= xmin) return;
    const int n = 300;
    double sig = std::abs(fit.sigma);
    if (!std::isfinite(sig) || sig < 1e-12) sig = (xmax > xmin) ? (xmax - xmin) / 100.0 : 1.0;
    QVector<double> fx(n), fy(n);
    for (int i = 0; i < n; ++i) {
        const double x = xmin + (xmax - xmin) * static_cast<double>(i) / (n - 1);
        const double g = fit.peak * std::exp(-((x - fit.center) * (x - fit.center)) / (2.0 * sig * sig));
        fx[i] = x;
        fy[i] = fit.baselineIntercept + fit.baselineSlope * x + g;
    }
    m_fitGraph = ui->plot1->addGraph();
    m_fitGraph->setPen(QPen(QColor(255, 140, 0), 1.6, Qt::DashLine));
    m_fitGraph->setData(fx, fy);
    m_fitGraph->removeFromLegend();

    const QString summary =
            u"Gaussian fit · peak %1 · centre %2 · FWHM %3 · ∫ %4 · rms %5"_s
                    .arg(fit.peak, 0, 'g', 4).arg(fit.center, 0, 'g', 6)
                    .arg(fit.fwhm, 0, 'g', 4).arg(fit.integratedFlux, 0, 'g', 4)
                    .arg(fit.rms, 0, 'g', 3);
    if (m_specStatsLabel) m_specStatsLabel->setText(summary);

    m_fitLabel = new QCPItemText(ui->plot1);
    m_fitLabel->setPositionAlignment(Qt::AlignTop | Qt::AlignRight);
    m_fitLabel->position->setType(QCPItemPosition::ptAxisRectRatio);
    m_fitLabel->position->setCoords(0.98, 0.04);
    m_fitLabel->setColor(QColor(255, 140, 0));
    m_fitLabel->setTextAlignment(Qt::AlignRight);
    m_fitLabel->setText(u"FWHM %1\ncentre %2"_s
                                .arg(fit.fwhm, 0, 'g', 4).arg(fit.center, 0, 'g', 6));

    ui->plot1->replot();
}

// ─────────────────────────────────────────────────────────────────────────────

void ProfileWidget::plotProfile(double x, double y, bool live)
{
    Q_UNUSED(live);
    const long pixX = std::lround(x);
    const long pixY = std::lround(y);

    // X Profile
    QVector<double> keyX(this->Dimensions[0]), valuesX(this->Dimensions[0]);
    for (vtkIdType i = 0; i < this->Dimensions[0]; ++i) {
        const vtkIdType idx = i * this->Increments[0] + pixY * this->Increments[1];
        keyX[i] = i;
        valuesX[i] = this->ScalarPointer[idx];
    }
    ui->plot1->graph()->setData(keyX, valuesX);
    ui->plot1->rescaleAxes();
    ui->plot1->xAxis->setRange(-1., this->Dimensions[0] + 1.);
    this->refLineX->start->setCoords(pixX, ui->plot1->yAxis->range().lower);
    this->refLineX->end->setCoords(pixX, ui->plot1->yAxis->range().upper);
    ui->plot1->replot();

    // Y Profile
    QVector<double> keyY(this->Dimensions[1]), valuesY(this->Dimensions[1]);
    for (vtkIdType j = 0; j < this->Dimensions[1]; ++j) {
        const vtkIdType idx = pixX * this->Increments[0] + j * this->Increments[1];
        keyY[j] = j;
        valuesY[j] = this->ScalarPointer[idx];
    }
    ui->plot2->graph()->setData(keyY, valuesY);
    ui->plot2->rescaleAxes();
    ui->plot2->xAxis->setRange(-1., this->Dimensions[1] + 1.);
    this->refLineY->start->setCoords(pixY, ui->plot2->yAxis->range().lower);
    this->refLineY->end->setCoords(pixY, ui->plot2->yAxis->range().upper);
    ui->plot2->replot();

    showAndRaiseFirstTime();
}

void ProfileWidget::plotSpectrum(double x, double y, bool live)
{
    Q_UNUSED(live);
    if (m_dialogInProgress) return;   // suppress live updates while saving
    this->clearFitOverlay();          // a new spectrum invalidates any prior fit
    ++this->m_spectrumGeneration;
    const long pixX = std::lround(x);
    const long pixY = std::lround(y);

    if (!this->astro) {
        return;
    }

    const double initSpectral = this->astro->getInitialSpectralValue();
    const double *axesInc = this->astro->getIncrements();

    QVector<double> key(this->Dimensions[2]), values(this->Dimensions[2]);
    for (vtkIdType k = 0; k < this->Dimensions[2]; ++k) {
        const vtkIdType idx = 1ll * pixX * this->Increments[0] + pixY * this->Increments[1]
                + k * this->Increments[2];
        key[k] = initSpectral + k * axesInc[2];
        values[k] = this->ScalarPointer[idx];
    }
    if (ui->plot1->graphCount() == 0) ui->plot1->addGraph();
    ui->plot1->graph(0)->setData(key, values);   // graph(0) is always the live series
    if (!m_pinnedGraphs.isEmpty()) {
        ui->plot1->graph(0)->setName(u"Current"_s);
        ui->plot1->legend->setVisible(true);
        applyPinnedLegendTheme();
    }
    ui->plot1->rescaleAxes();
    // The pixel coords drive the header meta row — refresh through the
    // public path so the title bar and stats stay in sync.
    m_pixelX = static_cast<int>(pixX);
    m_pixelY = static_cast<int>(pixY);
    m_lastSpectrumKey    = key;     // cached for CSV export
    m_lastSpectrumValues = values;
    refreshPinButtons();
    refreshSpectrumHeader();
    // If a smoothing kernel is active, apply it to the display; otherwise
    // show raw data. Stats are computed on the (possibly smoothed) data.
    if (m_smoothCombo && m_smoothCombo->currentData().toInt() != 0) {
        applySmoothAndReplot();
    } else {
        refreshSpectrumStats(key, values);
        ui->plot1->replot();
    }

    showAndRaiseFirstTime();
}

void ProfileWidget::updateSpectrumPlot(const QVector<double> &key, const QVector<double> &values,
                                       const QString &title, bool live)
{
    if (m_dialogInProgress) return;   // suppress live updates while saving
    this->clearFitOverlay();          // a new spectrum invalidates any prior fit
    ++this->m_spectrumGeneration;
    m_lastSpectrumTitle = title;
    if (ui->plot1->graphCount() == 0) {
        ui->plot1->addGraph();
    }
    // Keep graph(0) (live) and the pinned comparison curves; drop only any
    // other leftover graph (e.g. a secondary series from a prior two-series
    // update). Pinned curves must survive so several regions can be compared.
    for (int i = ui->plot1->graphCount() - 1; i >= 1; --i) {
        QCPGraph *g = ui->plot1->graph(i);
        if (!m_pinnedGraphs.contains(g)) {
            ui->plot1->removeGraph(g);
        }
    }

    Q_UNUSED(live);

    const bool havePins = !m_pinnedGraphs.isEmpty();
    QCPGraph *live0 = ui->plot1->graph(0);
    live0->setPen(QPen(QColor(0, 170, 255), 1.5));
    live0->setData(key, values);
    live0->setName(havePins ? u"Current"_s : QString());
    ui->plot1->legend->setVisible(havePins);
    if (havePins) applyPinnedLegendTheme();
    ui->plot1->rescaleAxes();
    if (auto *titleElement = qobject_cast<QCPTextElement *>(ui->plot1->plotLayout()->element(0, 0))) {
        titleElement->setText(title);
    }
    m_lastSpectrumKey    = key;     // cached for CSV export
    m_lastSpectrumValues = values;
    refreshPinButtons();
    refreshSpectrumHeader();
    if (m_smoothCombo && m_smoothCombo->currentData().toInt() != 0) {
        applySmoothAndReplot();
    } else {
        refreshSpectrumStats(key, values);
        ui->plot1->replot();
    }
    showAndRaiseFirstTime();
}

void ProfileWidget::updateSpectrumPlotSeries(const QVector<double> &key,
                                             const QVector<double> &primaryValues,
                                             const QString &primaryLabel,
                                             const QVector<double> &secondaryValues,
                                             const QString &secondaryLabel,
                                             const QString &title, bool live)
{
    if (m_dialogInProgress) return;   // suppress live updates while saving
    this->clearFitOverlay();          // a new spectrum invalidates any prior fit
    // Pinned comparison curves are single-series; a two-series update owns
    // graph(0)+graph(1), so drop any pins first to avoid clobbering them.
    this->clearPinnedSpectra();
    ++this->m_spectrumGeneration;
    // Cache the primary series so a Gaussian fit uses it (not stale data).
    m_lastSpectrumKey = key;
    m_lastSpectrumValues = primaryValues;
    while (ui->plot1->graphCount() < 2) {
        ui->plot1->addGraph();
    }
    while (ui->plot1->graphCount() > 2) {
        ui->plot1->removeGraph(ui->plot1->graphCount() - 1);
    }

    Q_UNUSED(live);

    ui->plot1->graph(0)->setData(key, primaryValues);
    ui->plot1->graph(0)->setName(primaryLabel);
    ui->plot1->graph(0)->setPen(QPen(QColor(0, 170, 255), 2.0));
    ui->plot1->graph(1)->setData(key, secondaryValues);
    ui->plot1->graph(1)->setName(secondaryLabel);
    ui->plot1->graph(1)->setPen(QPen(QColor(255, 170, 0), 2.0));
    ui->plot1->legend->setVisible(true);
    if (m_darkTheme) {
        ui->plot1->legend->setBrush(QColor(tHeaderBg()));
        ui->plot1->legend->setBorderPen(QPen(QColor(tBorder())));
        ui->plot1->legend->setTextColor(QColor(tValue()));
    } else {
        ui->plot1->legend->setBrush(Qt::white);
        ui->plot1->legend->setBorderPen(QPen(Qt::black));
        ui->plot1->legend->setTextColor(Qt::black);
    }
    ui->plot1->rescaleAxes();
    if (auto *titleElement = qobject_cast<QCPTextElement *>(ui->plot1->plotLayout()->element(0, 0))) {
        titleElement->setText(title);
    }
    refreshSpectrumHeader();
    refreshSpectrumStats(key, primaryValues);
    ui->plot1->replot();
    showAndRaiseFirstTime();
}

// ── Multi-region comparison: pinned spectra ─────────────────────────────────

QColor ProfileWidget::pinColour(int index)
{
    static const QColor palette[] = {
        QColor(255, 158,  32),   // orange
        QColor( 53, 196, 106),   // green
        QColor(226,  86, 200),   // magenta
        QColor(229, 208,  32),   // yellow
        QColor( 64, 208, 208),   // cyan
        QColor(229,  80,  60),   // red
        QColor(150, 130, 255),   // violet
        QColor(120, 200, 120),   // lime
    };
    const int n = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    return palette[((index % n) + n) % n];
}

void ProfileWidget::applyPinnedLegendTheme()
{
    if (m_darkTheme) {
        ui->plot1->legend->setBrush(QColor(tHeaderBg()));
        ui->plot1->legend->setBorderPen(QPen(QColor(tBorder())));
        ui->plot1->legend->setTextColor(QColor(tValue()));
    } else {
        ui->plot1->legend->setBrush(Qt::white);
        ui->plot1->legend->setBorderPen(QPen(Qt::black));
        ui->plot1->legend->setTextColor(Qt::black);
    }
}

void ProfileWidget::pinCurrentSpectrum(const QString &label)
{
    if (m_dialogInProgress) return;
    if (m_lastSpectrumKey.isEmpty() || m_lastSpectrumValues.isEmpty()) return;   // nothing to pin

    const int idx = static_cast<int>(m_pinnedGraphs.size());
    QString name = label.isEmpty() ? m_lastSpectrumTitle : label;
    if (name.isEmpty()) name = u"Spectrum"_s;
    name = u"%1. %2"_s.arg(idx + 1).arg(name);

    auto *g = ui->plot1->addGraph();
    g->setData(m_lastSpectrumKey, displayedSpectrumValues());   // match what's on screen
    QPen pen(pinColour(idx));
    pen.setWidthF(1.5);
    g->setPen(pen);
    g->setName(name);
    m_pinnedGraphs.push_back(g);

    // With at least one pin the legend becomes meaningful; label the live curve.
    if (ui->plot1->graphCount() > 0) ui->plot1->graph(0)->setName(u"Current"_s);
    ui->plot1->legend->setVisible(true);
    applyPinnedLegendTheme();
    ui->plot1->replot();
    refreshPinButtons();
}

void ProfileWidget::clearPinnedSpectra()
{
    if (m_pinnedGraphs.isEmpty()) { refreshPinButtons(); return; }
    for (QCPGraph *g : m_pinnedGraphs) {
        ui->plot1->removeGraph(g);
    }
    m_pinnedGraphs.clear();
    ui->plot1->legend->setVisible(false);
    if (ui->plot1->graphCount() > 0) ui->plot1->graph(0)->setName(QString());
    ui->plot1->replot();
    refreshPinButtons();
}

void ProfileWidget::refreshPinButtons()
{
    if (m_btnPin) m_btnPin->setEnabled(!m_lastSpectrumKey.isEmpty());
    if (m_btnClearPins) {
        const bool any = !m_pinnedGraphs.isEmpty();
        m_btnClearPins->setEnabled(any);
        m_btnClearPins->setText(any ? u"Clear Pins (%1)"_s.arg(m_pinnedGraphs.size())
                                    : u"Clear Pins"_s);
    }
}

// X-axis (coordinate) range from the actual key values, with a small margin.
static QCPRange coordinateRange(const QVector<double> &key)
{
    if (key.isEmpty()) {
        return QCPRange(-1.0, 1.0);
    }
    const double lo = std::min(key.first(), key.last());
    const double hi = std::max(key.first(), key.last());
    const double pad = std::max(1.0, (hi - lo) * 0.01);
    return QCPRange(lo - pad, hi + pad);
}

// Robust value-axis range: clip to the [loPct, hiPct] percentiles of the finite
// values so a few bright point sources don't squash the whole profile against
// zero (same idea as the image LUT's percentile clip). NaN/blank samples ignored.
static QCPRange robustValueRange(const QVector<double> &values, double loPct = 1.0,
                                 double hiPct = 99.5)
{
    QVector<double> finite;
    finite.reserve(values.size());
    for (const double v : values) {
        if (std::isfinite(v)) {
            finite.append(v);
        }
    }
    if (finite.isEmpty()) {
        return QCPRange(-1.0, 1.0);
    }
    std::sort(finite.begin(), finite.end());
    const auto pct = [&](double p) {
        const double frac = std::clamp(p / 100.0, 0.0, 1.0);
        const int idx = static_cast<int>(std::lround(frac * (finite.size() - 1)));
        return finite[std::clamp(idx, 0, static_cast<int>(finite.size()) - 1)];
    };
    double lo = pct(loPct);
    double hi = pct(hiPct);
    if (!(hi > lo)) {  // flat profile → give it a tiny visible span
        const double eps = std::max(1e-9, std::abs(hi) * 1e-3);
        lo -= eps;
        hi += eps;
    }
    const double pad = (hi - lo) * 0.08;  // headroom so peaks aren't flush to the edge
    return QCPRange(lo - pad, hi + pad);
}

void ProfileWidget::updateImageProfiles(const QVector<double> &keyX, const QVector<double> &valuesX,
                                        const QVector<double> &keyY, const QVector<double> &valuesY,
                                        double probeX, double probeY, bool live)
{
    Q_UNUSED(live);
    if (m_dialogInProgress) return;   // suppress live updates while saving
    if (ui->plot1->graphCount() == 0 || ui->plot2->graphCount() == 0) {
        this->setupImagePlots(u"X"_s, u"Value"_s);
    }

    ui->plot1->graph()->setData(keyX, valuesX);
    ui->plot1->xAxis->setRange(coordinateRange(keyX));
    ui->plot1->yAxis->setRange(robustValueRange(valuesX));
    if (this->refLineX) {
        this->refLineX->start->setCoords(probeX, ui->plot1->yAxis->range().lower);
        this->refLineX->end->setCoords(probeX, ui->plot1->yAxis->range().upper);
    }
    if (auto *titleX = qobject_cast<QCPTextElement *>(ui->plot1->plotLayout()->element(0, 0))) {
        titleX->setText(u"X Profile"_s);
    }
    ui->plot1->replot();

    ui->plot2->graph()->setData(keyY, valuesY);
    ui->plot2->xAxis->setRange(coordinateRange(keyY));
    ui->plot2->yAxis->setRange(robustValueRange(valuesY));
    if (this->refLineY) {
        this->refLineY->start->setCoords(probeY, ui->plot2->yAxis->range().lower);
        this->refLineY->end->setCoords(probeY, ui->plot2->yAxis->range().upper);
    }
    if (auto *titleY = qobject_cast<QCPTextElement *>(ui->plot2->plotLayout()->element(0, 0))) {
        titleY->setText(u"Y Profile"_s);
    }
    ui->plot2->replot();

    showAndRaiseFirstTime();
}

void ProfileWidget::applyPlotTheme(bool dark)
{
    m_darkTheme = dark;
    // Plot canvas pulls colours from VisivoTheme tokens so it matches the
    // surrounding window chrome and the rest of the app's dialogs.
    auto applyToPlot = [dark](QCustomPlot *plot) {
        if (!plot) return;
        if (dark) {
            plot->setBackground(QColor(tBg()));
            plot->axisRect()->setBackground(
                    QColor(QString::fromLatin1(VisivoTheme::kBackground())));
            if (auto *te = qobject_cast<QCPTextElement *>(plot->plotLayout()->element(0, 0)))
                te->setTextColor(QColor(tTitle()));
            const QColor axisLine(tBorder());
            const QColor subTick(QString::fromLatin1(VisivoTheme::kOutlineVariant()));
            for (auto *axis : { plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2 }) {
                axis->setBasePen(QPen(axisLine));
                axis->setTickPen(QPen(axisLine));
                axis->setSubTickPen(QPen(subTick));
                axis->setTickLabelColor(QColor(tValue()));
                axis->setLabelColor(QColor(tPrimary()));
            }
            if (plot->legend->visible()) {
                plot->legend->setBrush(QColor(tHeaderBg()));
                plot->legend->setBorderPen(QPen(QColor(tBorder())));
                plot->legend->setTextColor(QColor(tValue()));
            }
        } else {
            plot->setBackground(Qt::white);
            plot->axisRect()->setBackground(QColor(u"#f8f8f8"_s));
            if (auto *te = qobject_cast<QCPTextElement *>(plot->plotLayout()->element(0, 0)))
                te->setTextColor(Qt::black);
            for (auto *axis : { plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2 }) {
                axis->setBasePen(QPen(Qt::black));
                axis->setTickPen(QPen(Qt::black));
                axis->setSubTickPen(QPen(QColor(u"#888888"_s)));
                axis->setTickLabelColor(Qt::black);
                axis->setLabelColor(Qt::black);
            }
            if (plot->legend->visible()) {
                plot->legend->setBrush(Qt::white);
                plot->legend->setBorderPen(QPen(Qt::black));
                plot->legend->setTextColor(Qt::black);
            }
        }
        plot->replot();
    };
    applyToPlot(ui->plot1);
    applyToPlot(ui->plot2);
}

// Helper: run a native (macOS Finder, Windows Explorer, etc.) Save dialog
// as a standalone application-modal window.
//
// Why standalone (parent=nullptr) and not parent=spectrumWindow?
// On macOS, parenting a QFileDialog makes it an NSSavePanel sheet attached
// to the parent NSWindow, and macOS dismisses such sheets on *any* parent-
// window mutation (a label stylesheet refresh triggered by a network reply
// is enough). Standalone is the only way to keep the native picker stable
// while DataHub does its background work — which we also pause via the
// activeModalWidget() check in MainWindow::pollBackendDiagnosticsOnce().
//
// Why raise/activate the @p anchor widget BEFORE opening?
// When a parent-less native dialog closes, macOS restores focus to whoever
// was the "key window" at the moment the dialog opened. Without the
// activate, that's whichever window happened to be key when the user
// clicked Save — usually NOT the spectrum/PV window, because clicking the
// Save button doesn't necessarily make its host window key on macOS.
// Forcing the anchor key before opening, plus a processEvents flush so
// macOS observes the change, makes the OS return focus to the right
// window on close.
static QString runAppModalSaveDialog(QWidget *anchor, const QString &title,
                                     const QString &defaultName,
                                     const QString &defaultSuffix,
                                     const QStringList &filters)
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) dir = QDir::homePath();
    if (anchor) {
        anchor->raise();
        anchor->activateWindow();
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    }
    QFileDialog dlg(nullptr, title);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    dlg.setDefaultSuffix(defaultSuffix);
    dlg.setNameFilters(filters);
    dlg.setDirectory(dir);
    dlg.selectFile(defaultName);
    dlg.setWindowModality(Qt::ApplicationModal);
    if (dlg.exec() != QDialog::Accepted) return {};
    return dlg.selectedFiles().value(0);
}

void ProfileWidget::savePlot1Png()
{
    // QPointer-aware: even though the Qt-rendered dialog blocks input
    // during exec(), nothing prevents some other code from calling
    // deleteLater() on this widget while the nested event loop runs.
    // The guard still applies — defence in depth.
    QPointer<ProfileWidget> alive(this);
    m_dialogInProgress = true;
    auto _guard = qScopeGuard([&alive]() { if (alive) alive->m_dialogInProgress = false; });

    const QString path = runAppModalSaveDialog(
            this, u"Save plot as PNG"_s, u"profile.png"_s, u"png"_s,
            { u"PNG image (*.png)"_s, u"All files (*)"_s });
    if (!alive) return;
    if (!path.isEmpty())
        ui->plot1->savePng(path, 0, 0, 2.0, 95, 300);
}

void ProfileWidget::savePlot2Png()
{
    QPointer<ProfileWidget> alive(this);
    m_dialogInProgress = true;
    auto _guard = qScopeGuard([&alive]() { if (alive) alive->m_dialogInProgress = false; });

    const QString path = runAppModalSaveDialog(
            this, u"Save plot as PNG"_s, u"profile2.png"_s, u"png"_s,
            { u"PNG image (*.png)"_s, u"All files (*)"_s });
    if (!alive) return;
    if (!path.isEmpty())
        ui->plot2->savePng(path, 0, 0, 2.0, 95, 300);
}

void ProfileWidget::showAndRaiseFirstTime()
{
    // On the very first call we make sure the window is visible AND in
    // front (so the user actually notices it appeared). On every subsequent
    // call we only call show(), which is a no-op if we're already visible
    // — and crucially we do NOT call raise() / activateWindow(), because
    // those steal focus from any modal child (file picker, message box)
    // and on macOS that causes such children to dismiss themselves.
    if (!m_initiallyRaised) {
        this->show();
        this->raise();
        this->activateWindow();
        m_initiallyRaised = true;
    } else if (!this->isVisible()) {
        this->show();
    }
}

void ProfileWidget::saveSpectrumCsv()
{
    // QPointer-aware: macOS native sheets run a nested event loop; if
    // anything deletes this widget mid-dialog (cube viewer disposing the
    // spectrum window, app shutdown, …) `this` becomes invalid when exec()
    // returns. The scope guard only clears m_dialogInProgress when we're
    // still alive — and it runs on every return path so the flag is never
    // left stuck "true" on early returns.
    QPointer<ProfileWidget> alive(this);
    m_dialogInProgress = true;
    auto _guard = qScopeGuard([&alive]() { if (alive) alive->m_dialogInProgress = false; });

    // Snapshot the cached data first thing: the user can keep hovering
    // over the slice while the file picker is open, which mutates the
    // m_lastSpectrum* members through the live-probe path. Without this
    // snapshot the CSV would race against the latest hover spectrum.
    const QVector<double> key       = m_lastSpectrumKey;
    const QVector<double> values    = m_lastSpectrumValues;
    const QString         xLbl      = m_lastSpectrumXLabel;
    const QString         yLbl      = m_lastSpectrumYLabel;
    const QString         fileLabel = m_fileLabel;
    const QString         wcsLabel  = m_wcsLabel;
    const int             px        = m_pixelX;
    const int             py        = m_pixelY;

    if (key.isEmpty() || values.isEmpty() || key.size() != values.size()) {
        QMessageBox::information(this, u"Save spectrum as CSV"_s,
                                 u"No spectrum to export yet — pick a pixel on the slice first."_s);
        return;
    }

    // Build a proper default path: <Documents>/spectrum_x{px}_y{py}.csv.
    // On macOS passing only a bare filename makes the native file picker
    // open at $HOME and behave oddly (the user reported "fa cose strane").
    // A full default path makes the picker land where the user actually
    // expects to save data files, and remembers the directory afterwards.
    QString defaultDir =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (defaultDir.isEmpty() || !QFileInfo(defaultDir).isDir()) {
        defaultDir = QDir::homePath();
    }
    const QString suggested = QDir(defaultDir).filePath(
            u"spectrum_x%1_y%2.csv"_s.arg(px).arg(py));

    // Construct the file dialog explicitly so we can make it *application
    // modal* — the static convenience getSaveFileName() returns a window-
    // modal dialog (a sheet on macOS), which blocks input to ProfileWidget
    // only. The underlying cube viewer keeps firing live probe updates as
    // the mouse passes over the slice, each of which mutates this widget's
    // labels / plot — and that mutation is enough to dismiss the macOS
    // sheet by itself. Application-modal stops cube hover from reaching
    // here while the picker is up.
    // Native standalone (parent=nullptr) Save dialog. Make ourselves the
    // key window first so macOS restores focus to us on close — see
    // runAppModalSaveDialog() for the full rationale.
    this->raise();
    this->activateWindow();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    QFileDialog dlg(nullptr, u"Save spectrum as CSV"_s);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    dlg.setDefaultSuffix(u"csv"_s);
    dlg.setNameFilters({ u"CSV files (*.csv)"_s,
                         u"Text files (*.txt)"_s,
                         u"All files (*)"_s });
    dlg.setDirectory(defaultDir);
    dlg.selectFile(QFileInfo(suggested).fileName());
    dlg.setWindowModality(Qt::ApplicationModal);
    const int rc = dlg.exec();
    if (!alive) return;   // widget destroyed during the dialog
    if (rc != QDialog::Accepted) {
        return;  // user cancelled — entirely normal, not an error
    }
    QString chosen = dlg.selectedFiles().value(0);
    if (chosen.isEmpty()) {
        return;
    }
    // The native macOS picker doesn't always append the filter extension if
    // the user typed a name without one. Add .csv defensively when there is
    // no extension at all (don't override the user's explicit .txt etc.).
    if (QFileInfo(chosen).suffix().isEmpty()) {
        chosen += u".csv"_s;
    }

    QFile out(chosen);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, u"Save spectrum as CSV"_s,
                             u"Could not open file for writing:\n%1\n\n%2"_s
                                     .arg(chosen, out.errorString()));
        return;
    }

    QTextStream w(&out);
    // Provenance preamble using '#' so the file stays readable as a plain
    // 2-column CSV in pandas / astropy / TOPCAT (those tools skip lines
    // starting with '#' by convention).
    w << u"# VisIVO spectrum export\n"_s;
    if (!fileLabel.isEmpty()) w << u"# dataset: "_s << fileLabel << u"\n"_s;
    w << u"# pixel:   ("_s << px << u", "_s << py << u")\n"_s;
    if (!wcsLabel.isEmpty())  w << u"# wcs:     "_s << wcsLabel  << u"\n"_s;
    w << u"# samples: "_s << key.size() << u"\n"_s;
    // Header row + data. Sanitise label commas so the column count stays
    // exactly 2 (axis labels often contain units like "km/s" which are
    // safe, but a defensive replace future-proofs us).
    auto sanitiseLabel = [](const QString &s) {
        QString out = s;
        out.replace(u',', u';');
        return out;
    };
    w << sanitiseLabel(xLbl) << u","_s << sanitiseLabel(yLbl) << u"\n"_s;
    // Force C locale for numeric formatting so the decimal separator is
    // always '.' regardless of the user's regional settings — otherwise
    // an Italian (or other "," decimal) locale would emit "0,123" which
    // turns the file into a 3-column CSV that pandas / astropy can't read.
    const QLocale cLocale = QLocale::c();
    for (int i = 0; i < key.size(); ++i) {
        w << cLocale.toString(key[i],    'g', 12)
          << u","_s
          << cLocale.toString(values[i], 'g', 12)
          << u"\n"_s;
    }
    // _focusGuard restores spectrum-window focus on scope exit.
}

// ── Line identification overlay ──────────────────────────────────────────────

void ProfileWidget::loadLineIdFile()
{
    const QString path = QFileDialog::getOpenFileName(
            this, u"Load spectral line list"_s, QString(),
            u"CSV files (*.csv *.txt);;All files (*)"_s);
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, u"Load Lines"_s,
                u"Could not open:\n%1"_s.arg(path));
        return;
    }

    clearLineIdOverlay();

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) continue;
        // Expected format: frequency,label (or frequency<tab>label)
        const QStringList parts = line.contains(u',')
                ? line.split(u',') : line.split(u'\t');
        if (parts.size() < 2) continue;
        bool ok = false;
        const double freq = parts[0].trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(freq)) continue;
        m_lineIds.append({ freq, parts[1].trimmed() });
    }

    if (m_lineIds.isEmpty()) {
        QMessageBox::information(this, u"Load Lines"_s,
                u"No valid lines found in the file.\n\n"
                u"Expected format: frequency,label (one per line).\n"
                u"Lines starting with # are ignored."_s);
        return;
    }

    drawLineIdOverlay();
    if (ui->plot1) ui->plot1->replot();
}

void ProfileWidget::drawLineIdOverlay()
{
    if (!ui->plot1 || m_lineIds.isEmpty()) return;

    const QColor lineColor(u"#E8A840"_s);   // warm amber, distinct from data
    const QColor textColor(u"#F0C060"_s);

    for (const auto &sl : m_lineIds) {
        // Vertical line spanning the full Y range
        auto *marker = new QCPItemLine(ui->plot1);
        marker->setPen(QPen(lineColor, 1.0, Qt::DashLine));
        marker->start->setTypeX(QCPItemPosition::ptPlotCoords);
        marker->start->setTypeY(QCPItemPosition::ptViewportRatio);
        marker->start->setCoords(sl.frequency, 0.0);
        marker->end->setTypeX(QCPItemPosition::ptPlotCoords);
        marker->end->setTypeY(QCPItemPosition::ptViewportRatio);
        marker->end->setCoords(sl.frequency, 1.0);
        m_lineIdMarkers.append(marker);

        // Label above the line
        auto *text = new QCPItemText(ui->plot1);
        text->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
        text->position->setTypeX(QCPItemPosition::ptPlotCoords);
        text->position->setTypeY(QCPItemPosition::ptViewportRatio);
        text->position->setCoords(sl.frequency, 0.02);
        text->setText(sl.label);
        text->setColor(textColor);
        QFont f;
        f.setPixelSize(9);
        text->setFont(f);
        text->setRotation(-90);
        m_lineIdLabels.append(text);
    }
}

void ProfileWidget::clearLineIdOverlay()
{
    if (ui->plot1) {
        for (auto *m : m_lineIdMarkers) ui->plot1->removeItem(m);
        for (auto *t : m_lineIdLabels)  ui->plot1->removeItem(t);
        if (!m_lineIdMarkers.isEmpty() || !m_lineIdLabels.isEmpty())
            ui->plot1->replot();
    }
    m_lineIdMarkers.clear();
    m_lineIdLabels.clear();
    m_lineIds.clear();
}
