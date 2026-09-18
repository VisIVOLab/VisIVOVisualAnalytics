// SedWindow.cpp — see SedWindow.h.

#include "SedWindow.h"

#include "qcustomplot.h"

#include <QCheckBox>
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
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace {

enum BandColumn {
    ColUse = 0,
    ColUpperLimit,
    ColBand,
    ColWavelength,
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

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
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

    buildUi();
    buildBandTable();

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
    m_plot->setMinimumWidth(360);
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->xAxis->setLabel(u"Wavelength (µm)"_s);
    m_plot->yAxis->setLabel(u"Flux (Jy)"_s);
    m_plot->legend->setVisible(true);
    splitter->addWidget(m_plot);

    m_tabs = new QTabWidget(splitter);
    m_tabs->setMinimumWidth(380);
    splitter->addWidget(m_tabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    // ── Photometry ───────────────────────────────────────────────────────────
    auto *bandsPage = new QWidget(m_tabs);
    auto *bandsLayout = new QVBoxLayout(bandsPage);
    bandsLayout->setContentsMargins(6, 6, 6, 6);
    auto *bandsHint = new QLabel(
            u"Untick a band to leave it out of the fit. Tick “UL” to treat its flux as an "
            "upper limit: the band then constrains the fit without entering the χ²."_s,
            bandsPage);
    bandsHint->setWordWrap(true);
    bandsLayout->addWidget(bandsHint);
    m_bandTable = new QTableWidget(bandsPage);
    m_bandTable->setColumnCount(BandColumnCount);
    m_bandTable->setHorizontalHeaderLabels(
            { u"Use"_s, u"UL"_s, u"Band"_s, u"λ (µm)"_s, u"Flux (Jy)"_s,
              u"σ (Jy)"_s, u"Size (\")"_s, u"Designation"_s });
    m_bandTable->horizontalHeader()->setStretchLastSection(true);
    m_bandTable->verticalHeader()->setVisible(false);
    m_bandTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    bandsLayout->addWidget(m_bandTable, 1);
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

    m_statusLabel = new QLabel(QString(), actions);
    actionLayout->addWidget(m_statusLabel, 1);

    m_fitButton = new QPushButton(u"Fit greybody"_s, actions);
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
            "per-band FWHM (median over the bands in use) when it gives one."_s);
    commonForm->addRow(u"Source size"_s, m_sizeArcsec);

    m_colourCorrection = new QCheckBox(u"Apply PACS colour corrections"_s, common);
    m_colourCorrection->setChecked(true);
    m_colourCorrection->setToolTip(
            u"PACS fluxes are calibrated for a νF_ν = const source; a cold greybody needs a "
            "temperature-dependent correction at 70, 100 and 160 µm."_s);
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

void SedWindow::buildBandTable()
{
    m_updatingTable = true;
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

        m_bandTable->setItem(row, ColBand, readOnlyItem(point.band));
        m_bandTable->setItem(row, ColWavelength, readOnlyItem(formatValue(point.wavelengthUm)));
        m_bandTable->setItem(row, ColFlux, readOnlyItem(formatValue(point.fluxJy)));
        m_bandTable->setItem(row, ColError,
                             readOnlyItem(point.errorJy > 0.0 ? formatValue(point.errorJy) : u"—"_s));
        m_bandTable->setItem(row, ColSize,
                             readOnlyItem(point.sizeArcsec > 0.0 ? formatValue(point.sizeArcsec)
                                                                 : u"—"_s));
        m_bandTable->setItem(row, ColLabel, readOnlyItem(point.label));
    }
    m_bandTable->resizeColumnsToContents();
    m_updatingTable = false;

    connect(m_bandTable, &QTableWidget::cellChanged, this, &SedWindow::onBandTableChanged);
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

    markResultsStale();
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
    int detections = 0;
    for (const BackendSedPoint &point : points) {
        if (!point.upperLimit)
            ++detections;
    }
    if (detections < 2) {
        QMessageBox::warning(this, u"SED fit"_s,
                             u"Select at least two detections. Upper limits constrain the fit "
                             "but cannot define it."_s);
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
                                 "catalogue gives none. Type one in the Fit tab."_s);
            return;
        }
        m_log->append(u"No source size available: only the optically thin model will be fitted."_s);
    }

    m_pending = FitProvenance{ selectedRows(), points, request.distancePc, request.kappaRef,
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
                      "previous selection. Run it again.</i>"_s);
    }
    replot();
    m_tabs->setCurrentIndex(2);
    for (const BackendSedFit &fit : m_fits)
        announceProduct(fit);
}

void SedWindow::runModelFit()
{
    if (m_modelWatcher.isRunning())
        return;
    const QVector<BackendSedPoint> points = selectedPoints();
    if (points.size() < 2) {
        QMessageBox::warning(this, u"Theoretical models"_s,
                             u"Select at least two bands."_s);
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
    QVector<double> x, y, err;
    QVector<double> ulX, ulY;
    for (const visivo::SedPoint &point : m_sed.points) {
        if (!point.use)
            continue;
        if (point.upperLimit) {
            ulX.append(point.wavelengthUm);
            ulY.append(point.fluxJy);
            continue;
        }
        x.append(point.wavelengthUm);
        y.append(point.fluxJy);
        err.append(point.errorJy);
    }

    if (!x.isEmpty()) {
        auto *dataGraph = m_plot->addGraph();
        dataGraph->setName(u"Photometry"_s);
        dataGraph->setData(x, y);
        dataGraph->setLineStyle(QCPGraph::lsNone);
        dataGraph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, kDataColour, 7));

        bool anyError = false;
        for (double e : err)
            anyError = anyError || e > 0.0;
        if (anyError) {
            auto *bars = new QCPErrorBars(m_plot->xAxis, m_plot->yAxis);
            bars->setDataPlottable(dataGraph);
            bars->setData(err);
            bars->setPen(QPen(kDataColour));
            bars->removeFromLegend();
        }
    }

    if (!ulX.isEmpty()) {
        // Upper limits are drawn as downward triangles: they are not
        // measurements, and a disc would read as one.
        auto *ulGraph = m_plot->addGraph();
        ulGraph->setName(u"Upper limits"_s);
        ulGraph->setData(ulX, ulY);
        ulGraph->setLineStyle(QCPGraph::lsNone);
        ulGraph->setScatterStyle(
                QCPScatterStyle(QCPScatterStyle::ssTriangleInverted, QColor(200, 200, 90), 9));
    }
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

void SedWindow::markResultsStale()
{
    ++m_inputRevision;
    if (m_fits.isEmpty() || m_resultsStale)
        return;
    m_resultsStale = true;
    m_log->append(u"<i>The photometry changed after the last fit: the curves and results "
                  "shown belong to the previous selection. Run the fit again.</i>"_s);
    if (m_statusLabel)
        m_statusLabel->setText(u"Results are out of date"_s);
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
        // ran — which is the recorded selection, not whatever is ticked now.
        const int sent = m_provenance.sentRows.indexOf(i);
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
