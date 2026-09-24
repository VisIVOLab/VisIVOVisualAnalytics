// SedWindow.cpp — see SedWindow.h.

#include "SedWindow.h"

#include "qcustomplot.h"
#include "theme/VisivoTheme.h"

#include <QCheckBox>
#include <QToolTip>

#include <limits>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace {

enum BandColumn {
    ColUse = 0,
    ColUpperLimit,
    ColBand,        ///< wavelength in µm, with the catalogue's tag when it differs
    ColFlux,
    ColError,
    ColSize,
    ColLabel,
    BandColumnCount,
};

const QColor kThinColour(80, 160, 255);
const QColor kThickColour(255, 150, 60);
const QColor kModelColour(140, 220, 140);
const QColor kDataColour(235, 235, 235);

/// Human name for a fit parameter, with its unit. The backend speaks in stable
/// keys; the table should not.
QString parameterLabel(const QString &key)
{
    static const QMap<QString, QString> kNames{
        { u"mass_msun"_s, u"Mass (M☉)"_s },
        { u"temperature_k"_s, u"Temperature (K)"_s },
        { u"beta"_s, u"β"_s },
        { u"lambda0_um"_s, u"λ₀ (µm)"_s },
        { u"size_arcsec"_s, u"Size (arcsec)"_s },
        { u"scale"_s, u"Size scale"_s },
        { u"luminosity_lsun"_s, u"L_bol (L☉)"_s },
        { u"l_over_m"_s, u"L/M (L☉/M☉)"_s },
    };
    return kNames.value(key, key);
}

/// Order the fit parameters are worth reading in, rather than QMap's alphabet.
QStringList parameterOrder(const QMap<QString, double> &parameters)
{
    static const QStringList kPreferred{
        u"mass_msun"_s, u"temperature_k"_s, u"beta"_s, u"lambda0_um"_s,
        u"size_arcsec"_s, u"scale"_s, u"luminosity_lsun"_s, u"l_over_m"_s,
    };
    QStringList out;
    for (const QString &key : kPreferred) {
        if (parameters.contains(key))
            out.append(key);
    }
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
        if (!out.contains(it.key()))
            out.append(it.key());
    }
    return out;
}

QString formatValue(double value)
{
    if (!std::isfinite(value))
        return u"—"_s;
    const double magnitude = std::abs(value);
    if (magnitude != 0.0 && (magnitude < 1e-3 || magnitude >= 1e5))
        return QString::number(value, 'e', 3);
    return QString::number(value, 'g', 5);
}

/// Distinct wavelengths among the selected DETECTIONS.
///
/// Counting points instead lets two branches of one band — two counterparts at
/// 250 µm — stand in for two bands, and a "SED" of one wavelength is not one a
/// greybody can be fitted to.
int distinctDetectionWavelengths(const QVector<BackendSedPoint> &points)
{
    QVector<double> seen;
    for (const BackendSedPoint &point : points) {
        if (point.upperLimit)
            continue;
        bool known = false;
        for (double wavelength : seen)
            known = known || qFuzzyCompare(wavelength, point.wavelengthUm);
        if (!known)
            seen.append(point.wavelengthUm);
    }
    return static_cast<int>(seen.size());
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

/// A measured number: right-aligned, so magnitudes line up down the column.
QTableWidgetItem *numericItem(const QString &text)
{
    QTableWidgetItem *item = readOnlyItem(text);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

/// Four significant figures. A flux quoted as 0.035743 Jy claims a precision
/// its own error bar does not have, and six digits per cell is what made the
/// table hard to read across.
QString formatQuantity(double value)
{
    if (!std::isfinite(value))
        return u"—"_s;
    return QString::number(value, 'g', 4);
}

/// What to call a band. For a Hi-GAL catalogue the tag *is* the wavelength, and
/// showing both put the same number in two adjacent columns; the tag only earns
/// its own text when it says something different (the MSX sub-bands, whose
/// columns are all named after 21 µm).
QString bandLabel(const visivo::SedPoint &point)
{
    const QString wavelength = QString::number(point.wavelengthUm, 'g', 4);
    bool numeric = false;
    const double tag = point.band.trimmed().toDouble(&numeric);
    if (numeric && qFuzzyCompare(tag, point.wavelengthUm))
        return wavelength;
    if (point.band.trimmed().isEmpty())
        return wavelength;
    return u"%1  (%2)"_s.arg(wavelength, point.band.trimmed());
}

}   // namespace

SedWindow::SedWindow(const visivo::SedBuildResult &sed, const QString &backendUrl,
                     const QString &backendToken, QWidget *parent)
    : QWidget(parent, Qt::Window),
      m_sed(sed),
      m_backendUrl(backendUrl),
      m_backendToken(backendToken)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(m_sed.sourceLabel.isEmpty() ? u"SED"_s
                                               : u"SED — %1"_s.arg(m_sed.sourceLabel));
    resize(1080, 680);

    m_rawPoints = m_sed.points;
    resetRowBranches();
    buildUi();
    refreshBandTable();

    connect(m_plot, &QCustomPlot::selectionChangedByUser,
            this, &SedWindow::onPlotSelectionChanged);
    // Which dot is which row? With eleven bands, four of them at the same
    // wavelength, the plot and the table were two lists you had to match by
    // eye. Hovering now says it outright.
    m_plot->setMouseTracking(true);
    connect(m_plot, &QCustomPlot::mouseMove, this, &SedWindow::showPointTooltip);
    connect(&m_fitWatcher, &QFutureWatcher<BackendSedFitResult>::finished,
            this, &SedWindow::onGreybodyFitFinished);
    connect(&m_modelWatcher, &QFutureWatcher<BackendSedModelsResult>::finished,
            this, &SedWindow::onModelFitFinished);

    // Ask the backend what ranges it suggests, so the dialog and the fit cannot
    // drift apart. Asynchronously: this runs while showCatalogueSourceSed() is
    // still on the stack, and a synchronous round trip would hold the window
    // closed for as long as the backend took to answer. A backend that never
    // answers leaves the spin boxes as built — the fit still runs, on the
    // server's own defaults.
    if (!m_backendUrl.isEmpty()) {
        const QString url = m_backendUrl;
        const QString token = m_backendToken;
        auto *watcher = new QFutureWatcher<BackendSedDefaults>(this);
        connect(watcher, &QFutureWatcher<BackendSedDefaults>::finished, this,
                [this, watcher]() {
                    watcher->deleteLater();
                    const BackendSedDefaults defaults = watcher->result();
                    // Do not overwrite something the user has already typed
                    // while the request was in flight.
                    if (defaults.valid && !m_parametersTouched)
                        applyDefaults(defaults);
                    else if (defaults.valid)
                        m_modelsServiceUrl = defaults.modelsServiceUrl;
                });
        watcher->setFuture(QtConcurrent::run([url, token]() {
            BackendClient client(url, token);
            return client.requestSedDefaults();
        }));
    }

    if (!m_sed.skippedBands.isEmpty()) {
        m_log->append(u"Bands without a usable flux, left out of the SED: %1"_s
                              .arg(m_sed.skippedBands.join(u", "_s)));
    }
    updateIdleStatus();
    replot();
}

SedWindow::~SedWindow()
{
    // Nothing to wait for. Every worker lambda captures by value, so a fit still
    // in flight is self-contained and simply finishes into a watcher whose
    // connections Qt has already dropped. Waiting here would freeze the whole
    // GUI for as long as the backend takes to answer — up to the transport's
    // two-minute timeout — because the user closed a window.
    m_fitWatcher.disconnect(this);
    m_modelWatcher.disconnect(this);
}

// ── construction ──────────────────────────────────────────────────────────────

void SedWindow::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter, 1);

    m_plot = new QCustomPlot(splitter);
    m_plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_plot->setMinimumWidth(420);
    // Selection on the plot is OFF until asked for, clicks included. It used to
    // be that the box governed only the rubber-band drag while a plain click
    // always toggled a band in or out of the fit — which meant a click intended
    // to bring the window forward, or a slightly-moved drag, silently changed
    // the data going into the fit. A toggle that governs one of the two gestures
    // it looks like it governs is worse than no toggle (user report).
    //
    // So: off, the plot is for looking — drag pans, wheel zooms, nothing you do
    // there changes the fit. On, clicking toggles a band and dragging rubber-
    // bands a group. iMultiSelect lets one band take in both the detections and
    // the upper limits, which are separate plottables.
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->xAxis->setLabel(u"Wavelength (µm)"_s);
    m_plot->yAxis->setLabel(u"Flux (Jy)"_s);
    m_plot->legend->setVisible(true);
    applyPlotTheme();
    splitter->addWidget(m_plot);

    m_tabs = new QTabWidget(splitter);
    m_tabs->setMinimumWidth(380);
    splitter->addWidget(m_tabs);
    // The SED *is* the window; the tables serve it. Stretch factors alone left
    // the plot at barely a third of the width, which is where a curve and its
    // error bars stop being readable.
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 4);
    splitter->setSizes({ 620, 460 });

    // ── Photometry ───────────────────────────────────────────────────────────
    auto *bandsPage = new QWidget(m_tabs);
    auto *bandsLayout = new QVBoxLayout(bandsPage);
    bandsLayout->setContentsMargins(6, 6, 6, 6);
    auto *bandsHint = new QLabel(
            u"<b>Use</b> — include the band in the fit.   "
            u"<b>UL</b> — treat its flux as an upper limit: it constrains the fit "
            u"without entering the χ²."_s,
            bandsPage);
    bandsHint->setWordWrap(true);
    bandsLayout->addWidget(bandsHint);
    m_bandTable = new QTableWidget(bandsPage);
    m_bandTable->setColumnCount(BandColumnCount);
    m_bandTable->setHorizontalHeaderLabels(
            { u"Use"_s, u"UL"_s, u"λ (µm)"_s, u"Flux (Jy)"_s,
              u"σ (Jy)"_s, u"Size (\")"_s, u"Designation"_s });
    m_bandTable->horizontalHeader()->setStretchLastSection(true);
    m_bandTable->verticalHeader()->setVisible(false);
    m_bandTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bandTable->setAlternatingRowColors(true);
    m_bandTable->setShowGrid(false);
    bandsLayout->addWidget(m_bandTable, 1);
    connect(m_bandTable, &QTableWidget::cellChanged, this, &SedWindow::onBandTableChanged);
    m_tabs->addTab(bandsPage, u"Photometry"_s);

    // ── Fit parameters ───────────────────────────────────────────────────────
    auto *fitPage = new QWidget(m_tabs);
    buildParameterPanel(fitPage);
    m_tabs->addTab(fitPage, u"Fit"_s);

    // ── Results ──────────────────────────────────────────────────────────────
    auto *resultPage = new QWidget(m_tabs);
    auto *resultLayout = new QVBoxLayout(resultPage);
    resultLayout->setContentsMargins(6, 6, 6, 6);
    m_resultTable = new QTableWidget(resultPage);
    m_resultTable->setColumnCount(4);
    m_resultTable->setHorizontalHeaderLabels(
            { u"Model"_s, u"Parameter"_s, u"Value"_s, u"± (Δχ²=1)"_s });
    m_resultTable->horizontalHeader()->setStretchLastSection(true);
    m_resultTable->verticalHeader()->setVisible(false);
    resultLayout->addWidget(m_resultTable, 2);
    m_log = new QTextBrowser(resultPage);
    m_log->setMinimumHeight(110);
    resultLayout->addWidget(m_log, 1);
    m_tabs->addTab(resultPage, u"Results"_s);

    // ── Theoretical models ───────────────────────────────────────────────────
    auto *modelPage = new QWidget(m_tabs);
    auto *modelLayout = new QVBoxLayout(modelPage);
    modelLayout->setContentsMargins(6, 6, 6, 6);
    auto *modelHint = new QLabel(
            u"Models from the VLKB grid, ranked by χ². Select a row to overplot it."_s, modelPage);
    modelHint->setWordWrap(true);
    modelLayout->addWidget(modelHint);

    auto *weights = new QGroupBox(u"Band weights"_s, modelPage);
    auto *weightForm = new QFormLayout(weights);
    const auto weightBox = [&](double value) {
        auto *box = new QDoubleSpinBox(weights);
        box->setRange(0.0, 100.0);
        box->setDecimals(2);
        box->setValue(value);
        return box;
    };
    m_weightMidIr = weightBox(1.0);
    m_weightFarIr = weightBox(1.0);
    m_weightSubmm = weightBox(1.0);
    m_prefilter = weightBox(0.0);
    m_deltaChi2 = weightBox(1.0);
    weightForm->addRow(u"Mid-IR"_s, m_weightMidIr);
    weightForm->addRow(u"Far-IR"_s, m_weightFarIr);
    weightForm->addRow(u"Sub-mm"_s, m_weightSubmm);
    weightForm->addRow(u"Pre-filter"_s, m_prefilter);
    weightForm->addRow(u"Δχ²"_s, m_deltaChi2);
    modelLayout->addWidget(weights);

    m_modelTable = new QTableWidget(modelPage);
    m_modelTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_modelTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_modelTable->verticalHeader()->setVisible(false);
    modelLayout->addWidget(m_modelTable, 1);
    connect(m_modelTable, &QTableWidget::itemSelectionChanged,
            this, &SedWindow::onModelSelected);
    m_tabs->addTab(modelPage, u"Models"_s);

    // ── Action bar ───────────────────────────────────────────────────────────
    auto *actions = new QWidget(this);
    auto *actionLayout = new QHBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);

    m_logAxes = new QCheckBox(u"Log axes"_s, actions);
    m_logAxes->setChecked(true);
    connect(m_logAxes, &QCheckBox::toggled, this, [this](bool) { replot(); });
    actionLayout->addWidget(m_logAxes);

    // Picking the bands on the plot is how the legacy tool did it, and it is
    // the natural gesture: the SED is what you are looking at. Off by default
    // so a drag still pans; the two gestures are the same mouse button.
    m_selectOnPlot = new QCheckBox(u"Select on plot"_s, actions);
    m_selectOnPlot->setToolTip(
            u"Click a point to put its band in or out of the fit, or drag a rectangle over "
            u"several. While this is off the plot only pans and zooms, and nothing you do "
            u"on it changes the fit — the two gestures share a mouse button."_s);
    connect(m_selectOnPlot, &QCheckBox::toggled, this, [this](bool on) {
        // Both gestures, not just the drag: see the interaction flags above.
        QCP::Interactions flags = m_plot->interactions();
        flags.setFlag(QCP::iSelectPlottables, on);
        flags.setFlag(QCP::iMultiSelect, on);
        m_plot->setInteractions(flags);
        m_plot->setSelectionRectMode(on ? QCP::srmSelect : QCP::srmNone);
    });
    actionLayout->addWidget(m_selectOnPlot);

    m_collapseBranches = new QCheckBox(u"Collapse branches"_s, actions);
    m_collapseBranches->setToolTip(
            u"A band-merged source can have more than one counterpart in the same band. "
            u"Collapsed, they are summed into a single point per band; expanded, each is a "
            u"separate branch you can choose between."_s);
    connect(m_collapseBranches, &QCheckBox::toggled, this, &SedWindow::setBranchesCollapsed);
    actionLayout->addWidget(m_collapseBranches);

    m_statusLabel = new QLabel(QString(), actions);
    actionLayout->addWidget(m_statusLabel, 1);

    m_fitButton = new QPushButton(u"Fit greybody"_s, actions);
    m_fitButton->setDefault(true);
    m_fitButton->setAutoDefault(true);
    connect(m_fitButton, &QPushButton::clicked, this, &SedWindow::runGreybodyFit);
    actionLayout->addWidget(m_fitButton);

    m_modelButton = new QPushButton(u"Fit theoretical models"_s, actions);
    connect(m_modelButton, &QPushButton::clicked, this, &SedWindow::runModelFit);
    actionLayout->addWidget(m_modelButton);

    auto *clearButton = new QPushButton(u"Clear fits"_s, actions);
    connect(clearButton, &QPushButton::clicked, this, &SedWindow::clearFits);
    actionLayout->addWidget(clearButton);

    auto *exportButton = new QPushButton(u"Export…"_s, actions);
    connect(exportButton, &QPushButton::clicked, this, &SedWindow::exportSed);
    actionLayout->addWidget(exportButton);

    layout->addWidget(actions);
}

void SedWindow::applyPlotTheme()
{
    // QCustomPlot defaults to a white sheet. Dropped into this application that
    // is not merely inconsistent: the photometry is drawn in a light ink chosen
    // for a dark panel, so on white the points were all but invisible.
    const QColor ink = QColor(QString::fromLatin1(VisivoTheme::kOnSurface()));
    const QColor muted = QColor(QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()));
    const QColor panel = QColor(QString::fromLatin1(VisivoTheme::kSurface()));

    m_plot->setBackground(QBrush(panel));
    m_plot->axisRect()->setBackground(QBrush(panel));
    for (QCPAxis *axis : { m_plot->xAxis, m_plot->yAxis, m_plot->xAxis2, m_plot->yAxis2 }) {
        axis->setBasePen(QPen(muted));
        axis->setTickPen(QPen(muted));
        axis->setSubTickPen(QPen(muted));
        axis->setTickLabelColor(ink);
        axis->setLabelColor(ink);
        axis->grid()->setPen(QPen(QColor(muted.red(), muted.green(), muted.blue(), 60),
                                  1, Qt::DotLine));
        axis->grid()->setSubGridVisible(true);
        axis->grid()->setSubGridPen(QPen(QColor(muted.red(), muted.green(), muted.blue(), 25),
                                         1, Qt::DotLine));
    }
    // A SED is read across four decades; the top and right edges stop the eye
    // from running off the plot at the ends of the curve. They are a FRAME:
    // their ticks are switched off entirely, because replot() only ever updates
    // the primary axes' scale and range, so opposite ticks would drift out of
    // alignment as soon as the user zoomed.
    for (QCPAxis *axis : { m_plot->xAxis2, m_plot->yAxis2 }) {
        axis->setVisible(true);
        axis->setTicks(false);
        axis->setSubTicks(false);
        axis->setTickLabels(false);
    }

    m_plot->legend->setBrush(QBrush(QColor(panel.red(), panel.green(), panel.blue(), 200)));
    m_plot->legend->setBorderPen(QPen(muted));
    m_plot->legend->setTextColor(ink);
    // Top-left: a SED rises to a far-IR peak and falls, so that corner is the
    // one reliably free of data. elementCount() guards the index — the legend
    // is element 0 of the inset layout only while it lives in the axis rect.
    if (m_plot->axisRect()->insetLayout()->elementCount() > 0)
        m_plot->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignTop | Qt::AlignLeft);
}

void SedWindow::buildParameterPanel(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);

    auto *common = new QGroupBox(u"Source and dust"_s, page);
    auto *commonForm = new QFormLayout(common);

    m_modelCombo = new QComboBox(common);
    m_modelCombo->addItem(u"Optically thin"_s, u"thin"_s);
    m_modelCombo->addItem(u"Optically thick"_s, u"thick"_s);
    m_modelCombo->addItem(u"Both (compare)"_s, u"both"_s);
    commonForm->addRow(u"Model"_s, m_modelCombo);

    m_distance = new QDoubleSpinBox(common);
    m_distance->setRange(1.0, 1.0e7);
    m_distance->setDecimals(1);
    m_distance->setValue(1000.0);
    m_distance->setSuffix(u" pc"_s);
    commonForm->addRow(u"Distance"_s, m_distance);

    m_kappaRef = new QDoubleSpinBox(common);
    m_kappaRef->setRange(1e-4, 100.0);
    m_kappaRef->setDecimals(4);
    m_kappaRef->setValue(0.1);
    m_kappaRef->setSuffix(u" cm²/g"_s);
    commonForm->addRow(u"κ(λ_ref)"_s, m_kappaRef);

    m_lambdaRef = new QDoubleSpinBox(common);
    m_lambdaRef->setRange(1.0, 5000.0);
    m_lambdaRef->setDecimals(1);
    m_lambdaRef->setValue(300.0);
    m_lambdaRef->setSuffix(u" µm"_s);
    commonForm->addRow(u"λ_ref"_s, m_lambdaRef);

    m_sizeArcsec = new QDoubleSpinBox(common);
    m_sizeArcsec->setRange(0.0, 3600.0);
    m_sizeArcsec->setDecimals(2);
    m_sizeArcsec->setValue(visivo::SedBuilder::representativeSizeArcsec(m_sed.points));
    m_sizeArcsec->setSuffix(u" \""_s);
    m_sizeArcsec->setToolTip(
            u"Angular size used by the optically thick model. Taken from the catalogue's "
            u"per-band FWHM (median over the bands in use) when it gives one."_s);
    commonForm->addRow(u"Source size"_s, m_sizeArcsec);

    m_colourCorrection = new QCheckBox(u"Apply PACS colour corrections"_s, common);
    m_colourCorrection->setChecked(true);
    m_colourCorrection->setToolTip(
            u"PACS fluxes are calibrated for a νF_ν = const source; a cold greybody needs a "
            u"temperature-dependent correction at 70, 100 and 160 µm."_s);
    commonForm->addRow(QString(), m_colourCorrection);
    layout->addWidget(common);

    // One row of [min] [max] [steps] per fitted parameter. A step count of 1
    // holds the parameter fixed at its minimum — the same convention the
    // backend uses, so what the panel shows is what is fitted.
    const auto addRange = [&](QGroupBox *box, QFormLayout *form, const QString &label,
                              QPointer<QDoubleSpinBox> &minBox, QPointer<QDoubleSpinBox> &maxBox,
                              QPointer<QSpinBox> &stepBox, double lo, double hi, int steps,
                              double rangeMin, double rangeMax, int decimals) {
        auto *row = new QWidget(box);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        minBox = new QDoubleSpinBox(row);
        maxBox = new QDoubleSpinBox(row);
        for (QDoubleSpinBox *spin : { minBox.data(), maxBox.data() }) {
            spin->setRange(rangeMin, rangeMax);
            spin->setDecimals(decimals);
        }
        minBox->setValue(lo);
        maxBox->setValue(hi);
        stepBox = new QSpinBox(row);
        stepBox->setRange(1, 2000);
        stepBox->setValue(steps);
        stepBox->setPrefix(u"× "_s);
        stepBox->setToolTip(u"Grid steps; 1 holds the parameter fixed at the minimum."_s);
        rowLayout->addWidget(minBox);
        rowLayout->addWidget(maxBox);
        rowLayout->addWidget(stepBox);
        form->addRow(label, row);
    };

    auto *thin = new QGroupBox(u"Optically thin: min / max / steps"_s, page);
    auto *thinForm = new QFormLayout(thin);
    addRange(thin, thinForm, u"Mass (M☉)"_s, m_massMin, m_massMax, m_massSteps,
             1.0, 5000.0, 30, 1e-3, 1e7, 3);
    addRange(thin, thinForm, u"Temperature (K)"_s, m_tempMin, m_tempMax, m_tempSteps,
             5.0, 50.0, 30, 0.1, 1000.0, 2);
    addRange(thin, thinForm, u"β"_s, m_betaMin, m_betaMax, m_betaSteps,
             2.0, 2.0, 1, 0.0, 5.0, 2);
    layout->addWidget(thin);

    auto *thick = new QGroupBox(u"Optically thick: min / max / steps"_s, page);
    auto *thickForm = new QFormLayout(thick);
    addRange(thick, thickForm, u"λ₀ (µm)"_s, m_lambda0Min, m_lambda0Max, m_lambda0Steps,
             10.0, 300.0, 30, 0.1, 5000.0, 2);
    addRange(thick, thickForm, u"Size scale"_s, m_scaleMin, m_scaleMax, m_scaleSteps,
             1.0, 1.0, 1, 0.01, 100.0, 3);
    layout->addWidget(thick);

    layout->addStretch(1);

    // Any edit here means two things: the backend's late defaults must not
    // overwrite what the user typed, and whatever is already plotted no longer
    // corresponds to the settings on screen.
    const auto touched = [this]() {
        m_parametersTouched = true;
        markResultsStale();
    };
    for (QDoubleSpinBox *box : { m_distance.data(), m_kappaRef.data(), m_lambdaRef.data(),
                                 m_sizeArcsec.data(), m_massMin.data(), m_massMax.data(),
                                 m_tempMin.data(), m_tempMax.data(), m_betaMin.data(),
                                 m_betaMax.data(), m_lambda0Min.data(), m_lambda0Max.data(),
                                 m_scaleMin.data(), m_scaleMax.data() }) {
        if (box)
            connect(box, &QDoubleSpinBox::valueChanged, this, touched);
    }
    for (QSpinBox *box : { m_massSteps.data(), m_tempSteps.data(), m_betaSteps.data(),
                           m_lambda0Steps.data(), m_scaleSteps.data() }) {
        if (box)
            connect(box, &QSpinBox::valueChanged, this, touched);
    }
    connect(m_modelCombo, &QComboBox::currentIndexChanged, this, touched);
    connect(m_colourCorrection, &QCheckBox::toggled, this, touched);
}

void SedWindow::refreshBandTable()
{
    m_updatingTable = true;
    m_bandTable->clearContents();
    m_bandTable->setRowCount(static_cast<int>(m_sed.points.size()));
    for (int row = 0; row < static_cast<int>(m_sed.points.size()); ++row) {
        const visivo::SedPoint &point = m_sed.points[static_cast<std::size_t>(row)];

        auto *use = new QTableWidgetItem();
        use->setFlags((use->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        use->setCheckState(point.use ? Qt::Checked : Qt::Unchecked);
        m_bandTable->setItem(row, ColUse, use);

        auto *ul = new QTableWidgetItem();
        ul->setFlags((ul->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        ul->setCheckState(point.upperLimit ? Qt::Checked : Qt::Unchecked);
        m_bandTable->setItem(row, ColUpperLimit, ul);

        QTableWidgetItem *band = numericItem(bandLabel(point));
        if (point.alternative) {
            // An alternative counterpart in a band that already has one. Said
            // in the row itself, because "why are there two 250 µm rows" is the
            // first question the table otherwise raises.
            band->setToolTip(u"Another counterpart of this source in the same band. "
                             u"Tick it to fit this branch instead."_s);
            band->setForeground(QBrush(QColor(154, 174, 196)));
        }
        m_bandTable->setItem(row, ColBand, band);

        m_bandTable->setItem(row, ColFlux, numericItem(formatQuantity(point.fluxJy)));
        m_bandTable->setItem(row, ColError,
                             numericItem(point.errorJy > 0.0 ? formatQuantity(point.errorJy)
                                                             : u"—"_s));
        m_bandTable->setItem(row, ColSize,
                             numericItem(point.sizeArcsec > 0.0
                                                 ? QString::number(point.sizeArcsec, 'f', 1)
                                                 : u"—"_s));
        m_bandTable->setItem(row, ColLabel, readOnlyItem(point.label));
    }
    m_bandTable->resizeColumnsToContents();
    // Checkbox columns only ever hold a checkbox; leaving them sized to their
    // header text put the ticks off under the label.
    m_bandTable->setColumnWidth(ColUse, 46);
    m_bandTable->setColumnWidth(ColUpperLimit, 46);
    m_updatingTable = false;
}

void SedWindow::setBranchesCollapsed(bool collapsed)
{
    // Legacy behaviour, kept because it answers a real question: when a band
    // has several counterparts, is the source the brightest of them or all of
    // them together? Collapsed says "all of them", by summing the branches at
    // each wavelength into one point.
    if (collapsed == m_branchesCollapsed)
        return;
    propagateEditsToRawPoints();

    if (!collapsed) {
        m_sed.points = m_rawPoints;
        m_branchesCollapsed = false;
        resetRowBranches();
    } else {
        std::vector<visivo::SedPoint> summed;
        std::vector<std::vector<int>> groups;
        QStringList refused;

        // Group by wavelength FIRST, then decide what to do with the whole
        // group. Folding branch by branch into the previous point only ever
        // compares two neighbours, so a group of three — two detections and a
        // limit — came out half-summed.
        for (std::size_t first = 0; first < m_rawPoints.size();) {
            std::size_t last = first;
            while (last + 1 < m_rawPoints.size()
                   && m_rawPoints[last + 1].wavelengthUm == m_rawPoints[first].wavelengthUm) {
                ++last;
            }

            bool anyDetection = false;
            bool anyLimit = false;
            for (std::size_t i = first; i <= last; ++i) {
                anyDetection = anyDetection || !m_rawPoints[i].upperLimit;
                anyLimit = anyLimit || m_rawPoints[i].upperLimit;
            }

            if (anyDetection && anyLimit) {
                // A detection and an upper limit are not two measurements to
                // add up: summing 10 Jy and a "< 5 Jy" would hand the fit a
                // measured 15 Jy that nobody observed. The whole group stays as
                // it is — no partial sums — and the log says where and why.
                refused.append(QString::number(m_rawPoints[first].wavelengthUm));
                for (std::size_t i = first; i <= last; ++i) {
                    summed.push_back(m_rawPoints[i]);
                    groups.push_back({ static_cast<int>(i) });
                }
                first = last + 1;
                continue;
            }

            visivo::SedPoint total = m_rawPoints[first];
            total.alternative = false;
            std::vector<int> members{ static_cast<int>(first) };
            for (std::size_t i = first + 1; i <= last; ++i) {
                const visivo::SedPoint &branch = m_rawPoints[i];
                total.fluxJy += branch.fluxJy;
                // In quadrature, not the legacy's linear sum: the branches are
                // independent measurements, and adding their errors linearly
                // overstates the uncertainty of the total.
                total.errorJy = std::sqrt(total.errorJy * total.errorJy
                                          + branch.errorJy * branch.errorJy);
                // The summed source is at least as large as its largest part.
                total.sizeArcsec = std::max(total.sizeArcsec, branch.sizeArcsec);
                // The aggregate is in the fit if any of its branches was: a band
                // the user had excluded outright stays excluded.
                total.use = total.use || branch.use;
                members.push_back(static_cast<int>(i));
            }
            if (members.size() > 1) {
                total.label = u"%1 (+%2 branches summed)"_s.arg(total.label)
                                      .arg(members.size() - 1);
            }
            summed.push_back(total);
            groups.push_back(std::move(members));
            first = last + 1;
        }

        if (!refused.isEmpty()) {
            m_log->append(u"Not collapsed at %1 µm: the band mixes a detection with an "
                          u"upper limit, and those cannot be added together."_s
                                  .arg(refused.join(u", "_s)));
        }
        m_sed.points = std::move(summed);
        m_rowBranches = std::move(groups);
        m_displayedBaseline = m_sed.points;
        m_branchesCollapsed = true;
    }

    refreshBandTable();
    if (m_sizeArcsec) {
        const double size = visivo::SedBuilder::representativeSizeArcsec(m_sed.points);
        if (size > 0.0)
            m_sizeArcsec->setValue(size);
    }
    markResultsStale();
    updateIdleStatus();
    replot();
}

void SedWindow::applyDefaults(const BackendSedDefaults &defaults)
{
    const auto apply = [](const QVector<double> &range, QDoubleSpinBox *minBox,
                          QDoubleSpinBox *maxBox, QSpinBox *stepBox) {
        if (range.size() != 3 || !minBox || !maxBox || !stepBox)
            return;
        minBox->setValue(range[0]);
        maxBox->setValue(range[1]);
        stepBox->setValue(static_cast<int>(range[2]));
    };
    apply(defaults.massRange, m_massMin, m_massMax, m_massSteps);
    apply(defaults.tempRange, m_tempMin, m_tempMax, m_tempSteps);
    apply(defaults.betaRange, m_betaMin, m_betaMax, m_betaSteps);
    apply(defaults.lambda0Range, m_lambda0Min, m_lambda0Max, m_lambda0Steps);
    apply(defaults.scaleRange, m_scaleMin, m_scaleMax, m_scaleSteps);
    if (m_kappaRef && defaults.kappaRef > 0.0)
        m_kappaRef->setValue(defaults.kappaRef);
    if (m_lambdaRef && defaults.lambdaRefUm > 0.0)
        m_lambdaRef->setValue(defaults.lambdaRefUm);
    if (m_distance && defaults.distancePc > 0.0)
        m_distance->setValue(defaults.distancePc);
    m_modelsServiceUrl = defaults.modelsServiceUrl;
}

// ── interaction ───────────────────────────────────────────────────────────────

void SedWindow::onBandTableChanged(int row, int column)
{
    if (m_updatingTable || row < 0 || static_cast<std::size_t>(row) >= m_sed.points.size())
        return;
    visivo::SedPoint &point = m_sed.points[static_cast<std::size_t>(row)];
    if (column == ColUse)
        point.use = m_bandTable->item(row, ColUse)->checkState() == Qt::Checked;
    else if (column == ColUpperLimit)
        point.upperLimit = m_bandTable->item(row, ColUpperLimit)->checkState() == Qt::Checked;
    else
        return;

    propagateEditsToRawPoints();
    markResultsStale();
    updateIdleStatus();
    syncPlotSelection();
    // The thick model's size follows the bands actually in use.
    if (m_sizeArcsec) {
        const double size = visivo::SedBuilder::representativeSizeArcsec(m_sed.points);
        if (size > 0.0)
            m_sizeArcsec->setValue(size);
    }
    replot();
}

QVector<int> SedWindow::selectedRows() const
{
    QVector<int> rows;
    for (int row = 0; row < static_cast<int>(m_sed.points.size()); ++row) {
        if (m_sed.points[static_cast<std::size_t>(row)].use)
            rows.append(row);
    }
    return rows;
}

QVector<QVector<int>> SedWindow::selectedBranches() const
{
    QVector<QVector<int>> branches;
    for (int row : selectedRows()) {
        QVector<int> members;
        if (row >= 0 && static_cast<std::size_t>(row) < m_rowBranches.size()) {
            for (int member : m_rowBranches[static_cast<std::size_t>(row)])
                members.append(member);
        }
        branches.append(members);
    }
    return branches;
}

QVector<BackendSedPoint> SedWindow::selectedPoints() const
{
    QVector<BackendSedPoint> points;
    for (const visivo::SedPoint &point : m_sed.points) {
        if (!point.use)
            continue;
        BackendSedPoint out;
        out.wavelengthUm = point.wavelengthUm;
        out.fluxJy = point.fluxJy;
        out.errorJy = point.errorJy;
        out.upperLimit = point.upperLimit;
        points.append(out);
    }
    return points;
}

QVector<double> SedWindow::rangeFrom(QDoubleSpinBox *minBox, QDoubleSpinBox *maxBox,
                                     QSpinBox *stepBox) const
{
    if (!minBox || !maxBox || !stepBox)
        return {};
    return { minBox->value(), maxBox->value(), static_cast<double>(stepBox->value()) };
}

void SedWindow::runGreybodyFit()
{
    if (m_fitWatcher.isRunning())
        return;
    const QVector<BackendSedPoint> points = selectedPoints();
    if (distinctDetectionWavelengths(points) < 2) {
        QMessageBox::warning(this, u"SED fit"_s,
                             u"Select at least two detections at different wavelengths. Two "
                             u"branches of the same band are two measurements of one point on "
                             u"the spectrum, and upper limits constrain the fit but cannot "
                             u"define it."_s);
        return;
    }

    BackendSedFitRequest request;
    request.model = m_modelCombo->currentData().toString();
    request.points = points;
    request.distancePc = m_distance->value();
    request.kappaRef = m_kappaRef->value();
    request.lambdaRefUm = m_lambdaRef->value();
    request.colourCorrection = m_colourCorrection->isChecked();
    request.sizeArcsec = m_sizeArcsec->value();
    request.massRange = rangeFrom(m_massMin, m_massMax, m_massSteps);
    request.tempRange = rangeFrom(m_tempMin, m_tempMax, m_tempSteps);
    request.betaRange = rangeFrom(m_betaMin, m_betaMax, m_betaSteps);
    request.lambda0Range = rangeFrom(m_lambda0Min, m_lambda0Max, m_lambda0Steps);
    request.scaleRange = rangeFrom(m_scaleMin, m_scaleMax, m_scaleSteps);
    request.sourceLabel = m_sed.sourceLabel;

    if ((request.model == u"thick"_s || request.model == u"both"_s)
        && request.sizeArcsec <= 0.0) {
        if (request.model == u"thick"_s) {
            QMessageBox::warning(this, u"SED fit"_s,
                                 u"The optically thick model needs a source size, and this "
                                 u"catalogue gives none. Type one in the Fit tab."_s);
            return;
        }
        m_log->append(u"No source size available: only the optically thin model will be fitted."_s);
    }

    m_pending = FitProvenance{ selectedRows(), selectedBranches(), points, request.distancePc,
                               request.kappaRef,
                               request.lambdaRefUm, request.sizeArcsec,
                               request.colourCorrection, request.model, m_inputRevision };

    setBusy(true, u"Fitting…"_s);
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    m_fitWatcher.setFuture(QtConcurrent::run([url, token, request]() {
        if (url.isEmpty()) {
            BackendSedFitResult failure;
            failure.error = u"No backend is configured to run the fit."_s;
            return failure;
        }
        BackendClient client(url, token);
        return client.requestSedFit(request);
    }));
}

void SedWindow::onGreybodyFitFinished()
{
    setBusy(false);
    const BackendSedFitResult result = m_fitWatcher.result();
    if (!result.valid) {
        m_log->append(u"<b>Fit failed:</b> %1"_s.arg(result.error.toHtmlEscaped()));
        updateIdleStatus();
        QMessageBox::warning(this, u"SED fit"_s, result.error);
        return;
    }

    m_fits = result.fits;
    m_provenance = m_pending;
    // The user is free to edit while the fit runs. Comparing the revision the
    // request carried with the current one is the only way to know whether the
    // answer still describes what is on screen.
    m_resultsStale = m_provenance.revision != m_inputRevision;
    showFitResults();
    if (m_resultsStale) {
        m_log->append(u"<i>The inputs changed while this fit was running: it describes the "
                      u"previous selection. Run it again.</i>"_s);
    }
    replot();
    m_tabs->setCurrentIndex(2);
    updateIdleStatus();
    for (const BackendSedFit &fit : m_fits)
        announceProduct(fit);
}

void SedWindow::runModelFit()
{
    if (m_modelWatcher.isRunning())
        return;
    const QVector<BackendSedPoint> points = selectedPoints();
    if (distinctDetectionWavelengths(points) < 2) {
        QMessageBox::warning(this, u"Theoretical models"_s,
                             u"Select at least two bands at different wavelengths."_s);
        return;
    }

    setBusy(true, u"Querying the model grid…"_s);
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString service = m_modelsServiceUrl;
    const QString label = m_sed.sourceLabel;
    const double distance = m_distance->value();
    const double prefilter = m_prefilter->value();
    const double midIr = m_weightMidIr->value();
    const double farIr = m_weightFarIr->value();
    const double submm = m_weightSubmm->value();
    const double deltaChi2 = m_deltaChi2->value();
    m_modelWatcher.setFuture(QtConcurrent::run(
            [url, token, points, distance, prefilter, midIr, farIr, submm, deltaChi2,
             service, label]() {
                if (url.isEmpty()) {
                    BackendSedModelsResult failure;
                    failure.error = u"No backend is configured to run the model fit."_s;
                    return failure;
                }
                BackendClient client(url, token);
                return client.requestSedModels(points, distance, prefilter, midIr, farIr,
                                               submm, deltaChi2, service, label);
            }));
}

void SedWindow::onModelFitFinished()
{
    setBusy(false);
    const BackendSedModelsResult result = m_modelWatcher.result();
    if (!result.valid) {
        m_log->append(u"<b>Model fit failed:</b> %1"_s.arg(result.error.toHtmlEscaped()));
        updateIdleStatus();
        QMessageBox::warning(this, u"Theoretical models"_s, result.error);
        return;
    }

    m_models = result.models;
    m_modelTable->setRowCount(0);
    m_modelTable->setColumnCount(result.columns.size());
    m_modelTable->setHorizontalHeaderLabels(result.columns);
    for (int row = 0; row < m_models.size(); ++row) {
        m_modelTable->insertRow(row);
        for (int column = 0; column < result.columns.size(); ++column) {
            const QVariant value = m_models[row].fields.value(result.columns[column]);
            const QString text = value.typeId() == QMetaType::Double
                    ? formatValue(value.toDouble())
                    : value.toString();
            m_modelTable->setItem(row, column, readOnlyItem(text));
        }
    }
    m_modelTable->resizeColumnsToContents();

    QString summary = u"%1 model(s) returned"_s.arg(result.totalModels);
    if (result.truncated)
        summary += u", showing the %1 best"_s.arg(m_models.size());
    if (result.skippedModels > 0)
        summary += u"; %1 row(s) had no usable χ² and were dropped"_s.arg(result.skippedModels);
    m_log->append(summary + u"."_s);

    m_tabs->setCurrentIndex(3);
    updateIdleStatus();
    if (!m_models.isEmpty())
        m_modelTable->selectRow(0);
}

void SedWindow::onModelSelected()
{
    const QModelIndexList selected = m_modelTable->selectionModel()->selectedRows();
    if (selected.isEmpty())
        return;
    replot();
}

void SedWindow::clearFits()
{
    m_fits.clear();
    m_models.clear();
    m_modelTable->setRowCount(0);
    m_resultTable->setRowCount(0);
    m_log->clear();
    m_resultsStale = false;
    updateIdleStatus();
    replot();
}

// ── plotting ──────────────────────────────────────────────────────────────────

void SedWindow::replot()
{
    if (!m_plot)
        return;
    m_plot->clearPlottables();
    m_plot->clearItems();

    const bool logAxes = m_logAxes && m_logAxes->isChecked();
    // A log axis cannot show a non-positive value; the SED builder already
    // refuses non-positive fluxes, so the only risk is an empty selection.
    if (logAxes) {
        m_plot->xAxis->setScaleType(QCPAxis::stLogarithmic);
        m_plot->yAxis->setScaleType(QCPAxis::stLogarithmic);
        auto ticker = QSharedPointer<QCPAxisTickerLog>::create();
        m_plot->xAxis->setTicker(ticker);
        m_plot->yAxis->setTicker(QSharedPointer<QCPAxisTickerLog>::create());
    } else {
        m_plot->xAxis->setScaleType(QCPAxis::stLinear);
        m_plot->yAxis->setScaleType(QCPAxis::stLinear);
        m_plot->xAxis->setTicker(QSharedPointer<QCPAxisTicker>::create());
        m_plot->yAxis->setTicker(QSharedPointer<QCPAxisTicker>::create());
    }

    plotObservedPoints();

    for (const BackendSedFit &fit : m_fits)
        plotFit(fit, fit.model == u"thick"_s ? kThickColour : kThinColour);

    const QModelIndexList selected =
            m_modelTable ? m_modelTable->selectionModel()->selectedRows() : QModelIndexList();
    if (!selected.isEmpty()) {
        const int row = selected.first().row();
        if (row >= 0 && row < m_models.size())
            plotModel(m_models[row], kModelColour);
    }

    m_plot->rescaleAxes();
    // Give the curve some air; without it the outermost point sits on the frame.
    m_plot->xAxis->scaleRange(1.15, m_plot->xAxis->range().center());
    m_plot->yAxis->scaleRange(1.30, m_plot->yAxis->range().center());
    m_plot->replot();
}

void SedWindow::plotObservedPoints()
{
    m_dataGraph = nullptr;
    m_ulGraph = nullptr;
    m_dataRows.clear();
    m_ulRows.clear();

    // EVERY point is plotted, in the fit or not. The ones left out are the
    // alternative counterparts of a band — the branches of the band-merged
    // tree — and a SED that silently dropped them would hide the fact that
    // there was a choice to make at all. What is *in* the fit is shown by the
    // plot's own selection: filled and bright, against hollow and dim.
    QVector<double> x;
    QVector<double> y;
    QVector<double> err;
    QVector<double> ulX;
    QVector<double> ulY;
    for (int row = 0; row < static_cast<int>(m_sed.points.size()); ++row) {
        const visivo::SedPoint &point = m_sed.points[static_cast<std::size_t>(row)];
        if (point.upperLimit) {
            ulX.append(point.wavelengthUm);
            ulY.append(point.fluxJy);
            m_ulRows.push_back(row);
            continue;
        }
        x.append(point.wavelengthUm);
        y.append(point.fluxJy);
        err.append(point.errorJy);
        m_dataRows.push_back(row);
    }

    const QColor dim(kDataColour.red(), kDataColour.green(), kDataColour.blue(), 110);

    // ── Every band, in grey, connected: the shape of the source ─────────────
    //
    // Two families, the way the legacy viewer drew them, because each answers a
    // different question:
    //
    //   grey  — ALL the photometry there is, joined in wavelength order. Where a
    //           band has several counterparts the line runs up and down between
    //           them, and that vertical spread IS the information: it says how
    //           much the choice of counterpart matters. When the branches have
    //           been collapsed this is still the ORIGINAL set, so the plot shows
    //           what collapsing did rather than only its result.
    //   bright — the working set: the bands that will enter the fit.
    //
    // Before this the plot drew one family and joined only the used points, so
    // the alternatives floated unconnected and the collapsed view hid the
    // measurements it was made from.
    //
    // Added FIRST: within a layer QCustomPlot draws in creation order, so grey
    // ends up behind the markers the user has to click.
    const std::vector<visivo::SedPoint> &allBands =
            m_rawPoints.empty() ? m_sed.points : m_rawPoints;
    {
        QVector<double> rawX;
        QVector<double> rawY;
        for (const visivo::SedPoint &point : allBands) {
            if (point.upperLimit)
                continue;
            rawX.append(point.wavelengthUm);
            rawY.append(point.fluxJy);
        }
        if (rawX.size() >= 2) {
            const QColor raw(150, 155, 165);
            auto *original = m_plot->addGraph();
            original->setName(m_branchesCollapsed ? u"All bands (before collapsing)"_s
                                                  : u"All bands"_s);
            original->setData(rawX, rawY, /*alreadySorted=*/true);
            // Deliberately faint: it is the backdrop, and the eye must land on
            // the spectrum being fitted rather than on the alternatives.
            original->setPen(QPen(QColor(raw.red(), raw.green(), raw.blue(), 70), 1.0));
            original->setScatterStyle(m_branchesCollapsed
                    ? QCPScatterStyle(QCPScatterStyle::ssCircle, QPen(raw, 1.0), Qt::NoBrush, 5)
                    : QCPScatterStyle(QCPScatterStyle::ssNone));
            original->setSelectable(QCP::stNone);
        }
    }

    // The bright line joins the bands actually in the fit, in wavelength order.
    // Solid and in the data colour — it is the spectrum being fitted, and the
    // grey behind it is the context.
    QVector<double> lineX;
    QVector<double> lineY;
    for (const visivo::SedPoint &point : m_sed.points) {
        if (!point.use || point.upperLimit)
            continue;
        lineX.append(point.wavelengthUm);
        lineY.append(point.fluxJy);
    }
    if (lineX.size() >= 2) {
        auto *guide = m_plot->addGraph();
        guide->setData(lineX, lineY, /*alreadySorted=*/true);
        guide->setPen(QPen(kDataColour, 1.8));
        guide->setScatterStyle(QCPScatterStyle::ssNone);
        guide->setSelectable(QCP::stNone);
        guide->removeFromLegend();
    }

    if (!x.isEmpty()) {
        m_dataGraph = m_plot->addGraph();
        m_dataGraph->setName(u"Photometry"_s);
        m_dataGraph->setData(x, y, /*alreadySorted=*/true);
        m_dataGraph->setLineStyle(QCPGraph::lsNone);
        m_dataGraph->setScatterStyle(
                QCPScatterStyle(QCPScatterStyle::ssCircle, QPen(dim, 1.2), Qt::NoBrush, 7));
        m_dataGraph->setSelectable(QCP::stMultipleDataRanges);
        m_dataGraph->selectionDecorator()->setScatterStyle(
                QCPScatterStyle(QCPScatterStyle::ssDisc, kDataColour, 7),
                QCPScatterStyle::spAll);
        m_dataGraph->selectionDecorator()->setPen(QPen(kDataColour));

        bool anyError = false;
        for (double e : err)
            anyError = anyError || e > 0.0;
        if (anyError) {
            auto *bars = new QCPErrorBars(m_plot->xAxis, m_plot->yAxis);
            bars->setDataPlottable(m_dataGraph);
            bars->setData(err);
            bars->setPen(QPen(dim));
            bars->setSelectable(QCP::stNone);
            bars->removeFromLegend();
        }
    }

    if (!ulX.isEmpty()) {
        // Upper limits are drawn as downward triangles: they are not
        // measurements, and a disc would read as one.
        const QColor limitColour(200, 200, 90);
        const QColor limitDim(limitColour.red(), limitColour.green(), limitColour.blue(), 110);
        m_ulGraph = m_plot->addGraph();
        m_ulGraph->setName(u"Upper limits"_s);
        m_ulGraph->setData(ulX, ulY, /*alreadySorted=*/true);
        m_ulGraph->setLineStyle(QCPGraph::lsNone);
        m_ulGraph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssTriangleInverted,
                                                   QPen(limitDim, 1.2), Qt::NoBrush, 9));
        m_ulGraph->setSelectable(QCP::stMultipleDataRanges);
        m_ulGraph->selectionDecorator()->setScatterStyle(
                QCPScatterStyle(QCPScatterStyle::ssTriangleInverted, limitColour, 9),
                QCPScatterStyle::spAll);
        m_ulGraph->selectionDecorator()->setPen(QPen(limitColour));
    }

    syncPlotSelection();
}

void SedWindow::resetRowBranches()
{
    m_rowBranches.clear();
    m_rowBranches.reserve(m_sed.points.size());
    for (int row = 0; row < static_cast<int>(m_sed.points.size()); ++row)
        m_rowBranches.push_back({ row });
    m_displayedBaseline = m_sed.points;
}

void SedWindow::propagateEditsToRawPoints()
{
    // Every edit reaches the pristine branch list, collapsed or not. Tying this
    // to "only while expanded" meant a band unticked in the collapsed view came
    // back ticked the moment the user expanded again — the edit had nowhere to
    // live.
    //
    // Only what the user actually CHANGED is written down, though. A collapsed
    // row's Use is the OR of its branches, so copying it back unconditionally
    // would turn "one branch in, one out" into "both in" on the next expand —
    // a band the user had excluded, silently back in the fit. The baseline is
    // what the row held when the view was built; a row that still matches it
    // has not been edited, and its branches are left alone.
    if (m_rowBranches.size() != m_sed.points.size()
        || m_displayedBaseline.size() != m_sed.points.size()) {
        return;
    }
    for (std::size_t row = 0; row < m_sed.points.size(); ++row) {
        const visivo::SedPoint &point = m_sed.points[row];
        const bool useChanged = point.use != m_displayedBaseline[row].use;
        const bool limitChanged = point.upperLimit != m_displayedBaseline[row].upperLimit;
        if (!useChanged && !limitChanged)
            continue;
        for (int member : m_rowBranches[row]) {
            if (member < 0 || static_cast<std::size_t>(member) >= m_rawPoints.size())
                continue;
            visivo::SedPoint &raw = m_rawPoints[static_cast<std::size_t>(member)];
            if (useChanged)
                raw.use = point.use;
            if (limitChanged)
                raw.upperLimit = point.upperLimit;
        }
        m_displayedBaseline[row] = point;
    }
}

void SedWindow::syncPlotSelection()
{
    // setSelection() emits selectionChanged, not selectionChangedByUser, so
    // this cannot loop back through onPlotSelectionChanged. The flag guards the
    // case anyway: a future connection to the wrong signal would otherwise turn
    // a redraw into an edit of the user's choice.
    m_syncingSelection = true;
    const auto apply = [this](QCPGraph *graph, const std::vector<int> &rows) {
        if (!graph)
            return;
        QCPDataSelection selection;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const int row = rows[static_cast<std::size_t>(i)];
            if (row >= 0 && static_cast<std::size_t>(row) < m_sed.points.size()
                && m_sed.points[static_cast<std::size_t>(row)].use) {
                selection.addDataRange(QCPDataRange(i, i + 1), /*simplify=*/false);
            }
        }
        selection.simplify();
        graph->setSelection(selection);
    };
    apply(m_dataGraph, m_dataRows);
    apply(m_ulGraph, m_ulRows);
    m_syncingSelection = false;
}

void SedWindow::showPointTooltip(QMouseEvent *event)
{
    if (!event || !m_plot)
        return;
    const QPoint pos = event->pos();

    // Nearest point among the two data families, within a few pixels. selectTest
    // reports the distance AND, through the details, which data index it was —
    // which is what maps a dot back to its row in the table.
    const auto nearest = [this, pos](QCPGraph *graph, const std::vector<int> &rows,
                                     double &bestDist, int &bestRow) {
        if (!graph || graph->dataCount() == 0)
            return;
        QVariant details;
        const double d = graph->selectTest(pos, /*onlySelectable=*/false, &details);
        if (d < 0.0 || d > 12.0 || d >= bestDist)
            return;
        const QCPDataSelection sel = details.value<QCPDataSelection>();
        if (sel.dataRangeCount() == 0)
            return;
        const int index = sel.dataRange(0).begin();
        if (index < 0 || index >= static_cast<int>(rows.size()))
            return;
        bestDist = d;
        bestRow = rows[static_cast<std::size_t>(index)];
    };

    double bestDist = std::numeric_limits<double>::max();
    int row = -1;
    nearest(m_dataGraph, m_dataRows, bestDist, row);
    nearest(m_ulGraph, m_ulRows, bestDist, row);

    if (row < 0 || row >= static_cast<int>(m_sed.points.size())) {
        QToolTip::hideText();
        return;
    }

    const visivo::SedPoint &point = m_sed.points[static_cast<std::size_t>(row)];
    QStringList lines;
    // trimmed(), because a collapsed band whose catalogue gave no designation
    // ends up with just " (+2 branches summed)" — a bold empty line.
    const QString name = !point.label.trimmed().isEmpty() ? point.label.trimmed()
                                                          : point.band.trimmed();
    if (!name.isEmpty())
        lines << u"<b>%1</b>"_s.arg(name.toHtmlEscaped());
    lines << u"λ %1 µm"_s.arg(formatValue(point.wavelengthUm));
    if (point.upperLimit)
        lines << u"flux ≤ %1 Jy (upper limit)"_s.arg(formatValue(point.fluxJy));
    else if (point.errorJy > 0.0)
        lines << u"flux %1 ± %2 Jy"_s.arg(formatValue(point.fluxJy), formatValue(point.errorJy));
    else
        lines << u"flux %1 Jy"_s.arg(formatValue(point.fluxJy));
    if (point.sizeArcsec > 0.0)
        lines << u"size %1″"_s.arg(formatValue(point.sizeArcsec));
    // The one thing the dot itself cannot say, and the reason for hovering.
    lines << (point.use ? u"<i>in the fit</i>"_s : u"<i>not in the fit</i>"_s);

    QToolTip::showText(m_plot->mapToGlobal(pos), lines.join(u"<br>"_s), m_plot);

    // Row 3 of the table is not row 3 of the plot: point the table at the same
    // band so the two views agree about what is being pointed at.
    if (m_bandTable && m_bandTable->selectionModel()) {
        const QModelIndex idx = m_bandTable->model()->index(row, 0);
        if (idx.isValid())
            m_bandTable->setCurrentIndex(idx);
    }
}

void SedWindow::onPlotSelectionChanged()
{
    if (m_syncingSelection)
        return;

    // QCustomPlot's selection semantics are "the selection is whatever you last
    // touched": a click replaces it, and a rubber band that catches one point
    // throws the rest away. Copying that straight onto the fit input meant a
    // stray drag silently dropped every other band, and the plot appeared to
    // empty itself. The gesture is interpreted instead:
    //
    //   click a point   → toggle that band, leaving the others alone
    //   drag over points → add them
    //   click empty space → change nothing
    //
    // which is the promise the table's checkboxes already make, and needs no
    // modifier key to be useful.
    QVector<int> picked;
    const auto pick = [&picked](QCPGraph *graph, const std::vector<int> &rows) {
        if (!graph)
            return;
        const QCPDataSelection selection = graph->selection();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            if (selection.contains(QCPDataSelection(QCPDataRange(i, i + 1))))
                picked.append(rows[static_cast<std::size_t>(i)]);
        }
    };
    pick(m_dataGraph, m_dataRows);
    pick(m_ulGraph, m_ulRows);

    if (picked.isEmpty()) {
        // Nothing was hit: put the visual state back to what the fit input is,
        // rather than letting an empty selection read as "no bands chosen".
        syncPlotSelection();
        return;
    }

    const auto valid = [this](int row) {
        return row >= 0 && static_cast<std::size_t>(row) < m_sed.points.size();
    };
    if (picked.size() == 1 && valid(picked.first())) {
        visivo::SedPoint &point = m_sed.points[static_cast<std::size_t>(picked.first())];
        point.use = !point.use;
    } else {
        for (int row : picked) {
            if (valid(row))
                m_sed.points[static_cast<std::size_t>(row)].use = true;
        }
    }

    propagateEditsToRawPoints();
    refreshBandTable();
    if (m_sizeArcsec) {
        const double size = visivo::SedBuilder::representativeSizeArcsec(m_sed.points);
        if (size > 0.0)
            m_sizeArcsec->setValue(size);
    }
    markResultsStale();
    updateIdleStatus();
    // Deferred: this slot runs from QCustomPlot's own mouse handling, and
    // replot() deletes and rebuilds every plottable — including the graph whose
    // selection QCP has just changed and may still touch on the way out.
    QTimer::singleShot(0, this, [this]() { replot(); });
}

void SedWindow::plotFit(const BackendSedFit &fit, const QColor &colour)
{
    if (fit.modelWavelengthUm.isEmpty())
        return;
    auto *graph = m_plot->addGraph();
    graph->setData(fit.modelWavelengthUm, fit.modelFluxJy);
    graph->setPen(QPen(colour, 2));
    const QString name = fit.model == u"thick"_s ? u"Optically thick"_s : u"Optically thin"_s;
    // formatValue renders a NaN as an em dash, but "χ²ᵣ = —" reads as a
    // formatting failure; an under-determined fit has a χ² and no χ²ᵣ.
    graph->setName(std::isfinite(fit.chi2Reduced)
                           ? u"%1  (χ²ᵣ = %2)"_s.arg(name, formatValue(fit.chi2Reduced))
                           : u"%1  (χ² = %2, no χ²ᵣ)"_s.arg(name, formatValue(fit.chi2)));
}

void SedWindow::plotModel(const BackendSedModel &model, const QColor &colour)
{
    if (model.wavelengthUm.isEmpty() || model.wavelengthUm.size() != model.fluxJy.size())
        return;
    auto *graph = m_plot->addGraph();
    graph->setData(model.wavelengthUm, model.fluxJy);
    graph->setPen(QPen(colour, 2, Qt::DashLine));
    graph->setName(u"Theoretical model (χ² = %1)"_s.arg(formatValue(model.chi2)));
}

// ── results ───────────────────────────────────────────────────────────────────

void SedWindow::showFitResults()
{
    m_resultTable->setRowCount(0);
    for (const BackendSedFit &fit : m_fits) {
        const QString modelName = fit.model == u"thick"_s ? u"Thick"_s : u"Thin"_s;
        for (const QString &key : parameterOrder(fit.parameters)) {
            const int row = m_resultTable->rowCount();
            m_resultTable->insertRow(row);
            m_resultTable->setItem(row, 0, readOnlyItem(modelName));
            m_resultTable->setItem(row, 1, readOnlyItem(parameterLabel(key)));
            m_resultTable->setItem(row, 2, readOnlyItem(formatValue(fit.parameters.value(key))));
            QString uncertainty = u"—"_s;
            if (fit.uncertainties.contains(key)) {
                uncertainty = formatValue(fit.uncertainties.value(key));
                // ">" reads as "at least this wide": the Δχ² = 1 region ran
                // off the end of the searched range, so the real interval is
                // wider than the number.
                if (fit.unbounded.contains(key))
                    uncertainty = u"> %1"_s.arg(uncertainty);
            }
            m_resultTable->setItem(row, 3, readOnlyItem(uncertainty));
        }
        const int row = m_resultTable->rowCount();
        m_resultTable->insertRow(row);
        m_resultTable->setItem(row, 0, readOnlyItem(modelName));
        m_resultTable->setItem(row, 1, readOnlyItem(u"χ² / χ²ᵣ (dof)"_s));
        m_resultTable->setItem(row, 2, readOnlyItem(u"%1 / %2 (%3)"_s
                                                            .arg(formatValue(fit.chi2),
                                                                 formatValue(fit.chi2Reduced))
                                                            .arg(fit.dof)));
        if (!std::isfinite(fit.chi2Reduced))
            m_resultTable->item(row, 1)->setText(u"χ² (dof) — no reduced χ²"_s);
        m_resultTable->setItem(row, 3, readOnlyItem(u"%1 band(s), %2 UL"_s
                                                            .arg(fit.bandsUsed)
                                                            .arg(fit.bandsUpperLimit)));
    }
    m_resultTable->resizeColumnsToContents();

    for (const BackendSedFit &fit : m_fits) {
        for (const QString &warning : fit.warnings)
            m_log->append(u"<b>%1:</b> %2"_s.arg(fit.model, warning.toHtmlEscaped()));
    }
}

void SedWindow::updateIdleStatus()
{
    // An empty status bar under an unfitted SED says nothing about what the
    // window is for. Say what is loaded, and what the next step is.
    //
    // setBusy(false) clears the label, so every completion path — including the
    // failures — has to come back through here, or the window ends up with a
    // blank status line and no explanation.
    if (!m_statusLabel || m_fitWatcher.isRunning() || m_modelWatcher.isRunning())
        return;
    if (m_resultsStale) {
        m_statusLabel->setText(u"Results are out of date — run the fit again"_s);
        return;
    }
    int used = 0;
    int limits = 0;
    double shortest = 0.0;
    double longest = 0.0;
    for (const visivo::SedPoint &point : m_sed.points) {
        if (!point.use)
            continue;
        ++used;
        if (point.upperLimit)
            ++limits;
        if (shortest == 0.0 || point.wavelengthUm < shortest)
            shortest = point.wavelengthUm;
        longest = std::max(longest, point.wavelengthUm);
    }
    QString text = u"%1 band(s) selected"_s.arg(used);
    if (used > 0)
        text += u", %1–%2 µm"_s.arg(QString::number(shortest, 'g', 4),
                                    QString::number(longest, 'g', 4));
    if (limits > 0)
        text += u" (%1 upper limit(s))"_s.arg(limits);
    text += m_fits.isEmpty() ? u" · no fit yet"_s
                             : u" · %1 fit(s) plotted"_s.arg(m_fits.size());
    m_statusLabel->setText(text);
}

void SedWindow::markResultsStale()
{
    ++m_inputRevision;
    if (m_fits.isEmpty() || m_resultsStale)
        return;
    m_resultsStale = true;
    m_log->append(u"<i>The photometry changed after the last fit: the curves and results "
                  u"shown belong to the previous selection. Run the fit again.</i>"_s);
    updateIdleStatus();
}

void SedWindow::setBusy(bool busy, const QString &message)
{
    if (m_fitButton)
        m_fitButton->setEnabled(!busy);
    if (m_modelButton)
        m_modelButton->setEnabled(!busy);
    if (m_statusLabel)
        m_statusLabel->setText(message);
    setCursor(busy ? Qt::BusyCursor : Qt::ArrowCursor);
}

void SedWindow::announceProduct(const BackendSedFit &fit)
{
    QVariantMap params;
    params.insert(u"model"_s, fit.model);
    params.insert(u"distance_pc"_s, m_provenance.distancePc);
    params.insert(u"kappa_ref"_s, m_provenance.kappaRef);
    params.insert(u"lambda_ref_um"_s, m_provenance.lambdaRefUm);
    params.insert(u"colour_correction"_s, m_provenance.colourCorrection);
    params.insert(u"chi2"_s, fit.chi2);
    if (std::isfinite(fit.chi2Reduced))
        params.insert(u"chi2_reduced"_s, fit.chi2Reduced);
    params.insert(u"dof"_s, fit.dof);
    params.insert(u"bands_used"_s, fit.bandsUsed);
    params.insert(u"bands_upper_limit"_s, fit.bandsUpperLimit);
    for (auto it = fit.parameters.constBegin(); it != fit.parameters.constEnd(); ++it)
        params.insert(it.key(), it.value());
    for (auto it = fit.uncertainties.constBegin(); it != fit.uncertainties.constEnd(); ++it)
        params.insert(u"err_%1"_s.arg(it.key()), it.value());
    if (!fit.warnings.isEmpty())
        params.insert(u"warnings"_s, fit.warnings);

    emit fitProduced(u"SED fit (%1) — %2"_s.arg(
                             fit.model,
                             m_sed.sourceLabel.isEmpty() ? u"source"_s : m_sed.sourceLabel),
                     params);
}

// ── export ────────────────────────────────────────────────────────────────────

int SedWindow::provenanceIndexFor(int row) const
{
    // Which entry of the recorded request this row IS, by the branches behind
    // it. Matching on wavelength (and flux) instead cannot tell a branch that
    // was fitted from one that was not, and after a collapse it paired the
    // summed point with one of its own parts.
    if (row < 0 || static_cast<std::size_t>(row) >= m_rowBranches.size())
        return -1;
    const std::vector<int> &members = m_rowBranches[static_cast<std::size_t>(row)];
    for (int i = 0; i < m_provenance.sentBranches.size(); ++i) {
        const QVector<int> &sent = m_provenance.sentBranches[i];
        if (sent.size() != static_cast<int>(members.size()))
            continue;
        bool same = true;
        for (int j = 0; j < sent.size() && same; ++j)
            same = sent[j] == members[static_cast<std::size_t>(j)];
        if (same)
            return i;
    }
    return -1;
}

void SedWindow::exportSed()
{
    const QString path = QFileDialog::getSaveFileName(
            this, u"Export SED"_s,
            m_sed.sourceLabel.isEmpty() ? u"sed.csv"_s : u"%1_sed.csv"_s.arg(m_sed.sourceLabel),
            u"CSV files (*.csv)"_s);
    if (path.isEmpty())
        return;

    // QSaveFile: a failed or interrupted write leaves the previous file intact
    // rather than a truncated one.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, u"Export SED"_s,
                             u"Could not write %1: %2"_s.arg(path, file.errorString()));
        return;
    }

    QTextStream out(&file);
    out << "# SED";
    if (!m_sed.sourceLabel.isEmpty())
        out << " of " << m_sed.sourceLabel;
    out << "\n";
    if (!m_fits.isEmpty()) {
        // The fit's own settings, not the widgets': the user may have changed
        // them since, and a file that says one thing and holds another is worse
        // than one that says nothing.
        out << "# fitted with: model=" << m_provenance.model
            << " distance_pc=" << m_provenance.distancePc
            << " kappa_ref=" << m_provenance.kappaRef
            << " lambda_ref_um=" << m_provenance.lambdaRefUm
            << " size_arcsec=" << m_provenance.sizeArcsec
            << " colour_correction=" << (m_provenance.colourCorrection ? 1 : 0) << "\n";
        if (m_resultsStale) {
            out << "# NOTE: the photometry below changed after this fit ran; the "
                   "fitted_* columns describe the selection recorded in fit_* instead.\n";
        }
    }
    out << "wavelength_um,flux_jy,error_jy,upper_limit,used,size_arcsec,designation";
    if (!m_fits.isEmpty())
        out << ",fit_flux_jy,fit_error_jy,fit_upper_limit";
    for (const BackendSedFit &fit : m_fits)
        out << ",model_" << fit.model << "_jy";
    out << "\n";

    for (int i = 0; i < static_cast<int>(m_sed.points.size()); ++i) {
        const visivo::SedPoint &point = m_sed.points[static_cast<std::size_t>(i)];
        out << point.wavelengthUm << ',' << point.fluxJy << ',' << point.errorJy << ','
            << (point.upperLimit ? 1 : 0) << ',' << (point.use ? 1 : 0) << ','
            << point.sizeArcsec << ',' << '"' << QString(point.label).replace(u'"', u"\"\""_s)
            << '"';
        // fitted_flux_jy is aligned with the bands that were SENT when the fit
        // ran. Row indices cannot carry that across: collapsing the branches
        // renumbers the rows, so row 2 can be 160 µm before the fit and 250 µm
        // after it, and the export would put the model of one band next to the
        // photometry of another. Match on the wavelength — and, where a band
        // has branches, on the flux as well.
        const int sent = provenanceIndexFor(i);
        if (!m_fits.isEmpty()) {
            if (sent >= 0 && sent < m_provenance.points.size()) {
                const BackendSedPoint &used = m_provenance.points[sent];
                out << ',' << used.fluxJy << ',' << used.errorJy << ','
                    << (used.upperLimit ? 1 : 0);
            } else {
                out << ",,,";
            }
        }
        for (const BackendSedFit &fit : m_fits) {
            if (sent >= 0 && sent < fit.fittedFluxJy.size())
                out << ',' << fit.fittedFluxJy[sent];
            else
                out << ',';
        }
        out << "\n";
    }

    for (const BackendSedFit &fit : m_fits) {
        out << "# fit " << fit.model << ":";
        for (const QString &key : parameterOrder(fit.parameters)) {
            out << ' ' << key << '=' << fit.parameters.value(key);
            if (fit.uncertainties.contains(key))
                out << "+/-" << fit.uncertainties.value(key);
        }
        out << " chi2=" << fit.chi2 << " dof=" << fit.dof;
        if (std::isfinite(fit.chi2Reduced))
            out << " chi2_red=" << fit.chi2Reduced;
        out << "\n";
    }

    out.flush();
    if (!file.commit()) {
        QMessageBox::warning(this, u"Export SED"_s,
                             u"Could not finish writing %1: %2"_s.arg(path, file.errorString()));
        return;
    }
    m_log->append(u"Exported to %1"_s.arg(path.toHtmlEscaped()));
}
