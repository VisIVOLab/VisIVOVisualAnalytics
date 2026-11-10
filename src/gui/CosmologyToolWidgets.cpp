// CosmologyToolWidgets.cpp
// See CosmologyToolWidgets.h.

#include "CosmologyToolWidgets.h"

#include "qcustomplot.h"
#include "theme/VisivoTheme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

// ── Angular power spectrum: settings ─────────────────────────────────────────

AngularPowerSettings::AngularPowerSettings(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_source = new QLabel(this);
    m_source->setWordWrap(true);
    m_source->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    layout->addWidget(m_source);

    auto *form = new QFormLayout;
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    m_bins = new QSpinBox(this);
    m_bins->setRange(8, 256);
    m_bins->setValue(64);   // the route's own default
    m_bins->setToolTip(u"Number of logarithmic |k| bins from one cycle per map to the "
                       u"Nyquist frequency. Bins left empty by the pixel grid are dropped."_s);
    form->addRow(u"Bins"_s, m_bins);
    layout->addLayout(form);

    m_run = new QPushButton(u"Compute P(ℓ)"_s, this);
    layout->addWidget(m_run);
    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    m_hint->setText(u"A relative texture spectrum: Hann-windowed |FFT|², binned in |k|. "
                    u"No beam, pixel-window or mask deconvolution, so compare maps with it; "
                    u"do not read it as a calibrated Cℓ. Restrict the fit range to "
                    u"exclude the beam / pixel-window fall-off and any noise floor at "
                    u"high ℓ."_s);
    layout->addWidget(m_hint);
    QObject::connect(m_run, &QPushButton::clicked, this, &AngularPowerSettings::runRequested);
}

int AngularPowerSettings::nBins() const { return m_bins ? m_bins->value() : 64; }

void AngularPowerSettings::setSourceText(const QString &text)
{
    if (m_source)
        m_source->setText(text);
}

void AngularPowerSettings::setAvailable(bool available, const QString &reason)
{
    m_available = available;
    if (m_run)
        m_run->setToolTip(available ? QString() : reason);
    refreshRun();
}

void AngularPowerSettings::setBusy(bool busy)
{
    m_busy = busy;
    if (m_run)
        m_run->setText(busy ? u"Computing…"_s : u"Compute P(ℓ)"_s);
    refreshRun();
}

void AngularPowerSettings::refreshRun()
{
    if (m_run)
        m_run->setEnabled(m_available && !m_busy);
}

// ── Skeleton / voids: settings ───────────────────────────────────────────────

SkeletonSettings::SkeletonSettings(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_source = new QLabel(this);
    m_source->setWordWrap(true);
    m_source->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    layout->addWidget(m_source);

    auto *form = new QFormLayout;
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    m_mode = new QComboBox(this);
    m_mode->addItem(u"Filaments"_s, u"skeleton"_s);
    m_mode->addItem(u"Voids"_s, u"void"_s);
    m_mode->setToolTip(u"Filaments: the morphological skeleton (thinning) of the regions "
                       u"above the threshold.\nVoids: the skeleton of the regions below it."_s);
    form->addRow(u"Trace"_s, m_mode);

    m_thresholdAuto = new QCheckBox(u"Mean of the map"_s, this);
    m_thresholdAuto->setChecked(true);
    m_thresholdAuto->setToolTip(u"Let the server use the mean of the finite pixels (after "
                                u"smoothing, which averages over the finite pixels only). "
                                u"The value it used is recorded with the result."_s);
    m_threshold = new QLineEdit(this);
    // C locale on both sides: the value is read with QString::toDouble, which
    // would turn an Italian "0,5" accepted by a system-locale validator into 0.
    auto *thresholdValidator = new QDoubleValidator(m_threshold);
    thresholdValidator->setLocale(QLocale::c());
    m_threshold->setValidator(thresholdValidator);
    m_threshold->setPlaceholderText(u"value, in the map's units"_s);
    m_threshold->setEnabled(false);
    auto *thrRow = new QHBoxLayout;
    thrRow->setContentsMargins(0, 0, 0, 0);
    thrRow->addWidget(m_thresholdAuto);
    thrRow->addWidget(m_threshold, 1);
    form->addRow(u"Threshold"_s, thrRow);

    m_smooth = new QDoubleSpinBox(this);
    m_smooth->setRange(0.0, 50.0);
    m_smooth->setDecimals(1);
    m_smooth->setSingleStep(0.5);
    m_smooth->setSuffix(u" px"_s);
    m_smooth->setSpecialValueText(u"none"_s);
    m_smooth->setToolTip(u"Gaussian σ applied before thresholding, in pixels. Smoothing "
                         u"suppresses noise spurs at the cost of merging close structures."_s);
    form->addRow(u"Smoothing σ"_s, m_smooth);

    m_prune = new QCheckBox(u"Drop where map <"_s, this);
    m_prune->setToolTip(u"Removes individual skeleton pixels where the (smoothed) map is "
                        u"below this value. This is an intensity cut, not a "
                        u"topological-persistence cut: it can split a filament and add end "
                        u"points. Any value is a cut, 0 and negative included. Filaments only: "
                        u"a void skeleton lies below the threshold by construction."_s);
    m_pruneValue = new QLineEdit(this);
    auto *pruneValidator = new QDoubleValidator(m_pruneValue);
    pruneValidator->setLocale(QLocale::c());
    m_pruneValue->setValidator(pruneValidator);
    m_pruneValue->setEnabled(false);
    auto *pruneRow = new QHBoxLayout;
    pruneRow->setContentsMargins(0, 0, 0, 0);
    pruneRow->addWidget(m_prune);
    pruneRow->addWidget(m_pruneValue, 1);
    form->addRow(u"Skeleton pixels"_s, pruneRow);
    layout->addLayout(form);

    auto *buttons = new QHBoxLayout;
    m_run = new QPushButton(u"Trace"_s, this);
    m_clear = new QPushButton(u"Clear"_s, this);
    m_clear->setToolTip(u"Remove the overlay from the view (the product stays in Session Data)."_s);
    buttons->addWidget(m_run, 1);
    buttons->addWidget(m_clear);
    layout->addLayout(buttons);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    layout->addWidget(m_hint);
    m_result = new QLabel(this);
    m_result->setWordWrap(true);
    layout->addWidget(m_result);

    QObject::connect(m_thresholdAuto, &QCheckBox::toggled, this, [this](bool on) {
        if (m_threshold)
            m_threshold->setEnabled(!on);
        refreshRun();
    });
    QObject::connect(m_threshold, &QLineEdit::textChanged, this, [this]() { refreshRun(); });
    QObject::connect(m_prune, &QCheckBox::toggled, this, [this](bool on) {
        if (m_pruneValue)
            m_pruneValue->setEnabled(on);
        refreshRun();
    });
    // The intensity cut would delete a void skeleton outright (it sits below
    // the threshold by construction), so it is a filaments-only option.
    QObject::connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
        const bool voids = m_mode && m_mode->currentData().toString() == u"void"_s;
        if (m_prune) {
            if (voids)
                m_prune->setChecked(false);
            m_prune->setEnabled(!voids);
        }
        refreshRun();
    });
    QObject::connect(m_pruneValue, &QLineEdit::textChanged, this, [this]() { refreshRun(); });
    QObject::connect(m_run, &QPushButton::clicked, this, &SkeletonSettings::runRequested);
    QObject::connect(m_clear, &QPushButton::clicked, this, &SkeletonSettings::clearRequested);
}

BackendSkeletonRequest SkeletonSettings::request() const
{
    BackendSkeletonRequest r;
    r.channel = 0;
    r.mode = m_mode ? m_mode->currentData().toString() : u"skeleton"_s;
    r.thresholdAuto = !m_thresholdAuto || m_thresholdAuto->isChecked();
    r.threshold = m_threshold ? QLocale::c().toDouble(m_threshold->text()) : 0.0;
    r.smoothSigmaPix = m_smooth ? m_smooth->value() : 0.0;
    r.prune = m_prune && m_prune->isChecked() && m_prune->isEnabled();
    r.persistenceThreshold = m_pruneValue ? QLocale::c().toDouble(m_pruneValue->text()) : 0.0;
    return r;
}

void SkeletonSettings::setSourceText(const QString &text)
{
    if (m_source)
        m_source->setText(text);
}

void SkeletonSettings::setAvailable(bool available, const QString &reason)
{
    m_available = available;
    if (m_hint)
        m_hint->setText(available ? QString() : reason);
    refreshRun();
}

void SkeletonSettings::setBusy(bool busy)
{
    m_busy = busy;
    if (m_run)
        m_run->setText(busy ? u"Tracing…"_s : u"Trace"_s);
    refreshRun();
}

void SkeletonSettings::setResultText(const QString &text)
{
    if (m_result)
        m_result->setText(text);
}

void SkeletonSettings::setClearVisible(bool visible)
{
    if (m_clear)
        m_clear->setVisible(visible);
}

void SkeletonSettings::refreshRun()
{
    if (!m_run)
        return;
    // A typed value is required where the box asks for one: an empty field
    // would go out as 0, a threshold nobody chose.
    const bool thresholdOk = !m_thresholdAuto || m_thresholdAuto->isChecked()
            || (m_threshold && m_threshold->hasAcceptableInput());
    const bool pruneOk = !m_prune || !m_prune->isChecked()
            || (m_pruneValue && m_pruneValue->hasAcceptableInput());
    m_run->setEnabled(m_available && !m_busy && thresholdOk && pruneOk);
}

// ── Angular power spectrum: result window ────────────────────────────────────

PowerSpectrumWindow::PowerSpectrumWindow(const BackendAngularPowerResult &spectrum,
                                         const QString &sourceLabel,
                                         const visivo::PowerSlopeOptions &options,
                                         QWidget *parent)
    : QWidget(parent, Qt::Window), m_spectrum(spectrum), m_sourceLabel(sourceLabel)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(u"Angular Power Spectrum — %1"_s.arg(sourceLabel));
    resize(720, 560);

    auto *layout = new QVBoxLayout(this);
    m_plot = new QCustomPlot(this);
    m_plot->setMinimumHeight(320);
    for (QCPAxis *axis : { m_plot->xAxis, m_plot->yAxis }) {
        axis->setScaleType(QCPAxis::stLogarithmic);
        axis->setTicker(QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
        axis->setNumberFormat(u"eb"_s);
        axis->setNumberPrecision(0);
    }
    m_plot->xAxis->setLabel(spectrum.ellIsAngular
                                    ? u"Angular multipole ℓ = 2π k / θ_pix "
                                      u"(k in cycles/pixel, θ_pix in rad)"_s
                                    : u"Wavenumber k (pixel⁻¹): no celestial pixel "
                                      u"scale, ℓ not defined"_s);
    m_plot->yAxis->setLabel(u"P (relative, map units²)"_s);
    m_plot->legend->setVisible(true);
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    layout->addWidget(m_plot, 1);

    auto *controls = new QHBoxLayout;
    const bool angular = spectrum.ellIsAngular;
    const auto makeEll = [this, angular]() {
        auto *box = new QDoubleSpinBox(this);
        box->setDecimals(angular ? 1 : 5);
        box->setRange(0.0, 1e9);
        box->setSpecialValueText(u"auto"_s);
        return box;
    };
    m_ellMin = makeEll();
    m_ellMax = makeEll();
    m_ellMin->setValue(options.ellMin);
    m_ellMax->setValue(options.ellMax);
    m_ellMin->setToolTip(u"Lowest bin the slope is fitted over (auto: from the first)."_s);
    m_ellMax->setToolTip(u"Highest bin the slope is fitted over (auto: to the last)."_s);
    m_minModes = new QSpinBox(this);
    m_minModes->setRange(1, 10000);
    m_minModes->setValue(options.minModes);
    m_minModes->setToolTip(u"Leave out of the fit any bin built from fewer Fourier modes. "
                           u"The window couples neighbouring modes, so a bin's effective "
                           u"count is n / %1²."_s
                                   .arg(std::isfinite(spectrum.modeCoupling)
                                                ? QString::number(spectrum.modeCoupling, 'f', 2)
                                                : u"1.94"_s));
    const QString axisName = angular ? u"ℓ"_s : u"k"_s;
    controls->addWidget(new QLabel(u"Fit %1 from"_s.arg(axisName), this));
    controls->addWidget(m_ellMin);
    controls->addWidget(new QLabel(u"to"_s, this));
    controls->addWidget(m_ellMax);
    controls->addWidget(new QLabel(u"min. modes"_s, this));
    controls->addWidget(m_minModes);
    controls->addStretch(1);
    auto *csv = new QPushButton(u"Export CSV…"_s, this);
    auto *png = new QPushButton(u"Save PNG…"_s, this);
    controls->addWidget(csv);
    controls->addWidget(png);
    layout->addLayout(controls);

    m_slope = new QLabel(this);
    m_slope->setWordWrap(true);
    m_slope->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_slope);
    auto *note = new QLabel(this);
    note->setWordWrap(true);
    note->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    QString noteText = spectrum.note.isEmpty()
            ? u"Relative (uncalibrated) power spectrum."_s
            : spectrum.note.left(1).toUpper() + spectrum.note.mid(1) + u'.';
    if (std::isfinite(spectrum.pixelScaleArcsec))
        noteText += u" Pixel scale %1″; %2 × %3 pixels."_s
                            .arg(spectrum.pixelScaleArcsec, 0, 'g', 5)
                            .arg(spectrum.width)
                            .arg(spectrum.height);
    note->setText(noteText);
    layout->addWidget(note);

    const auto changed = [this]() { refit(); };
    QObject::connect(m_ellMin, &QDoubleSpinBox::editingFinished, this, changed);
    QObject::connect(m_ellMax, &QDoubleSpinBox::editingFinished, this, changed);
    QObject::connect(m_minModes, &QSpinBox::editingFinished, this, changed);
    QObject::connect(csv, &QPushButton::clicked, this, &PowerSpectrumWindow::exportCsv);
    QObject::connect(png, &QPushButton::clicked, this, &PowerSpectrumWindow::savePng);

    m_fit = visivo::fitPowerSlope(m_spectrum.ell, m_spectrum.power, m_spectrum.nModes, options,
                                  m_spectrum.clErr);
    redraw();
}

void PowerSpectrumWindow::setFitCallback(
        std::function<void(const visivo::PowerSlopeFit &, const visivo::PowerSlopeOptions &)> onFit)
{
    m_onFit = std::move(onFit);
}

visivo::PowerSlopeOptions PowerSpectrumWindow::options() const
{
    visivo::PowerSlopeOptions o;
    o.ellMin = m_ellMin ? m_ellMin->value() : 0.0;
    o.ellMax = m_ellMax ? m_ellMax->value() : 0.0;
    o.minModes = m_minModes ? m_minModes->value() : 8;
    return o;
}

QString PowerSpectrumWindow::slopeSummary(const visivo::PowerSlopeFit &fit, bool ellIsAngular)
{
    const QString axis = ellIsAngular ? u"ℓ"_s : u"k"_s;
    if (!fit.valid)
        return u"No slope: fewer than 3 bins left in the fit range."_s;
    return u"Slope n = %1 ± %2 (P ∝ %3ⁿ) over %3 %4–%5, %6 bins, %7"_s
            .arg(fit.slope, 0, 'f', 3)
            .arg(fit.slopeError, 0, 'f', 3)
            .arg(axis)
            .arg(fit.ellLow, 0, 'g', 4)
            .arg(fit.ellHigh, 0, 'g', 4)
            .arg(fit.binsUsed)
            .arg(fit.weighted ? u"weighted by the bin errors"_s
                              : u"unweighted (the server sent no errors)"_s);
}

void PowerSpectrumWindow::refit()
{
    const visivo::PowerSlopeOptions o = options();
    m_fit = visivo::fitPowerSlope(m_spectrum.ell, m_spectrum.power, m_spectrum.nModes, o,
                                  m_spectrum.clErr);
    redraw();
    if (m_onFit)
        m_onFit(m_fit, o);
}

void PowerSpectrumWindow::redraw()
{
    if (!m_plot)
        return;
    m_plot->clearPlottables();
    const visivo::PowerSlopeOptions o = options();

    QVector<double> inX, inY, outX, outY;
    QVector<double> errX, errY, errLo, errHi;
    const int n = static_cast<int>(std::min(m_spectrum.ell.size(), m_spectrum.power.size()));
    for (int i = 0; i < n; ++i) {
        const double l = m_spectrum.ell[i];
        const double p = m_spectrum.power[i];
        if (!(l > 0.0 && p > 0.0 && std::isfinite(l) && std::isfinite(p)))
            continue;   // a log axis has no place for it
        const double e = i < m_spectrum.clErr.size() ? m_spectrum.clErr[i] : NAN;
        // The same test fitPowerSlope applies, so the colours say which bins
        // the line was fitted to: a bin without an error has no weight.
        const bool weighted = m_spectrum.clErr.size() >= n;
        const bool inRange = (o.ellMin <= 0.0 || l >= o.ellMin) && (o.ellMax <= 0.0 || l <= o.ellMax)
                && (i >= m_spectrum.nModes.size() || m_spectrum.nModes[i] >= o.minModes)
                && (!weighted || (std::isfinite(e) && e > 0.0));
        (inRange ? inX : outX).append(l);
        (inRange ? inY : outY).append(p);
        if (std::isfinite(e) && e > 0.0) {
            errX.append(l);
            errY.append(p);
            // On a log axis the lower bar cannot reach zero: stop it at a
            // thousandth of the value rather than draw it to minus infinity.
            errLo.append(std::min(e, p * 0.999));
            errHi.append(e);
        }
    }

    auto *errGraph = m_plot->addGraph();
    errGraph->setData(errX, errY, true);
    errGraph->setLineStyle(QCPGraph::lsNone);
    errGraph->setScatterStyle(QCPScatterStyle::ssNone);
    errGraph->removeFromLegend();
    auto *bars = new QCPErrorBars(m_plot->xAxis, m_plot->yAxis);
    bars->setDataPlottable(errGraph);
    bars->setData(errLo, errHi);
    bars->setPen(QPen(QColor(120, 120, 120)));
    bars->removeFromLegend();

    auto *used = m_plot->addGraph();
    used->setName(u"Bins in the fit"_s);
    used->setData(inX, inY, true);
    used->setLineStyle(QCPGraph::lsNone);
    used->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, QColor(37, 99, 235), 6));

    auto *left = m_plot->addGraph();
    left->setName(u"Left out"_s);
    left->setData(outX, outY, true);
    left->setLineStyle(QCPGraph::lsNone);
    left->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, QColor(148, 163, 184), 6));

    if (m_fit.valid) {
        auto *line = m_plot->addGraph();
        line->setName(u"P ∝ %1^%2"_s.arg(m_spectrum.ellIsAngular ? u"ℓ"_s : u"k"_s)
                              .arg(m_fit.slope, 0, 'f', 2));
        const double a = std::pow(10.0, m_fit.intercept);
        line->setData({ m_fit.ellLow, m_fit.ellHigh },
                      { a * std::pow(m_fit.ellLow, m_fit.slope),
                        a * std::pow(m_fit.ellHigh, m_fit.slope) });
        line->setPen(QPen(QColor(220, 38, 38), 2));
    }
    m_plot->rescaleAxes();
    m_plot->replot();

    if (m_slope)
        m_slope->setText(slopeSummary(m_fit, m_spectrum.ellIsAngular));
}

void PowerSpectrumWindow::exportCsv()
{
    const QString path = QFileDialog::getSaveFileName(
            this, u"Export Power Spectrum"_s, u"power_spectrum.csv"_s, u"CSV (*.csv)"_s);
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, u"Export Power Spectrum"_s,
                             u"Could not write %1:\n%2"_s.arg(path, file.errorString()));
        return;
    }
    QTextStream out(&file);
    out << "# source: " << m_sourceLabel << '\n';
    out << "# " << (m_spectrum.note.isEmpty() ? u"relative power spectrum"_s : m_spectrum.note)
        << '\n';
    out << "# ell_is_angular: " << (m_spectrum.ellIsAngular ? "true" : "false") << '\n';
    if (m_fit.valid)
        out << "# slope: " << QString::number(m_fit.slope, 'g', 8) << " +- "
            << QString::number(m_fit.slopeError, 'g', 8) << " over " << m_fit.ellLow << " to "
            << m_fit.ellHigh << " (" << m_fit.binsUsed << " bins)\n";
    out << "k_pix_inv,ell,power,power_err,n_modes\n";
    const int n = static_cast<int>(m_spectrum.power.size());
    for (int i = 0; i < n; ++i) {
        const double e = i < m_spectrum.clErr.size() ? m_spectrum.clErr[i] : NAN;
        out << (i < m_spectrum.kBins.size() ? QString::number(m_spectrum.kBins[i], 'g', 10)
                                            : QString())
            << ','
            << (i < m_spectrum.ell.size() ? QString::number(m_spectrum.ell[i], 'g', 10)
                                          : QString())
            << ',' << QString::number(m_spectrum.power[i], 'g', 10) << ','
            << (std::isfinite(e) ? QString::number(e, 'g', 10) : QString()) << ','
            << (i < m_spectrum.nModes.size() ? QString::number(m_spectrum.nModes[i])
                                             : QString())
            << '\n';
    }
}

void PowerSpectrumWindow::savePng()
{
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save Power Spectrum Plot"_s, u"power_spectrum.png"_s, u"PNG (*.png)"_s);
    if (path.isEmpty() || !m_plot)
        return;
    if (!m_plot->savePng(path, 0, 0, 2.0))
        QMessageBox::warning(this, u"Save Power Spectrum Plot"_s,
                             u"Could not write %1."_s.arg(path));
}
