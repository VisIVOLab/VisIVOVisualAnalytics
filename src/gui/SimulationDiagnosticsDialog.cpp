#include "SimulationDiagnosticsDialog.h"

#include "CatalogueSelectionState.h"
#include "app/BackendTaskFollow.h"

#include "qcustomplot.h"
#include "theme/VisivoTheme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;
namespace profiles = visivo::profiles;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

QCustomPlot *newPlot(QWidget *parent)
{
    auto *plot = new QCustomPlot(parent);
    plot->setMinimumHeight(300);
    return plot;
}

QLabel *newSummary(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QDoubleSpinBox *coordinateBox(QWidget *parent)
{
    auto *box = new QDoubleSpinBox(parent);
    box->setRange(-1e12, 1e12);
    box->setDecimals(6);
    return box;
}

/// A length in the table's units; 0 stands for "the backend's default".
QDoubleSpinBox *radiusBox(QWidget *parent)
{
    auto *box = new QDoubleSpinBox(parent);
    box->setRange(0.0, 1e12);
    box->setDecimals(6);
    box->setSpecialValueText(u"automatic"_s);
    return box;
}

QPen primaryPen(double width = 1.6)
{
    return QPen(QColor(QString::fromLatin1(VisivoTheme::kPrimary())), width);
}

}   // namespace

SimulationDiagnosticsDialog::SimulationDiagnosticsDialog(
        const QString &backendUrl, const QString &backendToken, const QString &sessionId,
        const QString &datasetId, const QStringList &numericFields,
        const std::vector<BackendTabularFilter> &filters, QWidget *parent)
    : QDialog(parent), m_backendUrl(backendUrl), m_backendToken(backendToken),
      m_sessionId(sessionId), m_datasetId(datasetId), m_filters(filters)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(u"Selection diagnostics"_s);
    this->resize(820, 700);
    auto *outer = new QVBoxLayout(this);

    m_population = new QLabel(this);
    m_population->setWordWrap(true);
    m_population->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    outer->addWidget(m_population);
    this->updatePopulation();

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(this->buildHistogramTab(numericFields), u"Histogram"_s);
    m_tabs->addTab(this->buildProfileTab(numericFields), u"Radial profile"_s);
    m_tabs->addTab(this->buildPhaseTab(numericFields), u"Phase diagram"_s);
    outer->addWidget(m_tabs, 1);

    auto *runRow = new QHBoxLayout;
    m_run = new QPushButton(u"Compute"_s, this);
    m_run->setToolTip(u"Compute the current tab over the complete selected population."_s);
    QObject::connect(m_run, &QPushButton::clicked, this, &SimulationDiagnosticsDialog::run);
    runRow->addWidget(m_run);
    // Passes over every particle of a large snapshot take minutes: a backend
    // task, whose progress the bar shows and which Cancel stops.
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setVisible(false);
    runRow->addWidget(m_progress, 1);
    m_cancel = new QPushButton(u"Cancel"_s, this);
    m_cancel->setVisible(false);
    QObject::connect(m_cancel, &QPushButton::clicked, this, &SimulationDiagnosticsDialog::cancel);
    runRow->addWidget(m_cancel);
    outer->addLayout(runRow);
    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    QObject::connect(m_tick, &QTimer::timeout, this, &SimulationDiagnosticsDialog::showProgress);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_saveCsv = buttons->addButton(u"Save CSV…"_s, QDialogButtonBox::ActionRole);
    m_savePng = buttons->addButton(u"Save PNG…"_s, QDialogButtonBox::ActionRole);
    QObject::connect(m_saveCsv, &QPushButton::clicked, this, &SimulationDiagnosticsDialog::saveCsv);
    QObject::connect(m_savePng, &QPushButton::clicked, this, &SimulationDiagnosticsDialog::savePng);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);
    QObject::connect(m_tabs, &QTabWidget::currentChanged, this,
                     [this](int) { this->updateSaveButtons(); });
    this->updateSaveButtons();
}

QWidget *SimulationDiagnosticsDialog::buildHistogramTab(const QStringList &numericFields)
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    m_field = new QComboBox(page);
    m_field->addItems(numericFields);
    m_weight = new QComboBox(page);
    m_weight->addItem(u"None (particle count)"_s, QString());
    for (const QString &field : numericFields)
        m_weight->addItem(field, field);
    m_bins = new QSpinBox(page);
    m_bins->setRange(4, 512);
    m_bins->setValue(64);
    form->addRow(u"Quantity"_s, m_field);
    form->addRow(u"Weight"_s, m_weight);
    form->addRow(u"Bins"_s, m_bins);
    layout->addLayout(form);
    m_summary = newSummary(u"Choose a quantity and compute."_s, page);
    layout->addWidget(m_summary);
    m_plot = newPlot(page);
    m_plot->xAxis->setLabel(u"Quantity"_s);
    m_plot->yAxis->setLabel(u"Count"_s);
    layout->addWidget(m_plot, 1);
    return page;
}

QWidget *SimulationDiagnosticsDialog::buildProfileTab(const QStringList &numericFields)
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;

    m_centreMode = new QComboBox(page);
    m_centreMode->addItem(u"Shrinking sphere"_s, static_cast<int>(profiles::CentreMode::ShrinkingSphere));
    m_centreMode->addItem(u"Point"_s, static_cast<int>(profiles::CentreMode::Point));
    m_centreMode->addItem(u"Selected SubFind target"_s, static_cast<int>(profiles::CentreMode::Target));
    m_centreMode->setToolTip(
            u"Shrinking sphere: the mass-weighted centre found by nested shrinking spheres "
            u"(Power et al. 2003), the default. Point: coordinates in the table's units, or "
            u"the particle selected in the window. SubFind target: the group or subhalo "
            u"selected in a SubFind catalogue window of this session."_s);
    form->addRow(u"Centre"_s, m_centreMode);

    auto *pointRow = new QWidget(page);
    auto *pointLayout = new QHBoxLayout(pointRow);
    pointLayout->setContentsMargins(0, 0, 0, 0);
    m_pointX = coordinateBox(pointRow);
    m_pointY = coordinateBox(pointRow);
    m_pointZ = coordinateBox(pointRow);
    pointLayout->addWidget(m_pointX);
    pointLayout->addWidget(m_pointY);
    pointLayout->addWidget(m_pointZ);
    m_usePicked = new QPushButton(u"Selected particle"_s, pointRow);
    m_usePicked->setToolTip(u"Fill in the position of the particle selected in the window."_s);
    pointLayout->addWidget(m_usePicked);
    form->addRow(u"Point (x, y, z)"_s, pointRow);
    m_targetLabel = new QLabel(page);
    m_targetLabel->setWordWrap(true);
    form->addRow(u"Target"_s, m_targetLabel);

    auto *rangeRow = new QWidget(page);
    auto *rangeLayout = new QHBoxLayout(rangeRow);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    m_rMin = radiusBox(rangeRow);
    m_rMax = radiusBox(rangeRow);
    rangeLayout->addWidget(m_rMin);
    rangeLayout->addWidget(new QLabel(u"to"_s, rangeRow));
    rangeLayout->addWidget(m_rMax);
    rangeRow->setToolTip(u"Radii in the table's length unit. Automatic: from the larger of the "
                         u"smallest distance and r_max/1000, out to the largest distance, half "
                         u"the box, or the edge of a box or sphere selection."_s);
    form->addRow(u"Radius range"_s, rangeRow);
    m_profileBins = new QSpinBox(page);
    m_profileBins->setRange(4, 256);
    m_profileBins->setValue(32);
    m_profileBins->setToolTip(u"Logarithmically spaced shells."_s);
    form->addRow(u"Shells (log)"_s, m_profileBins);
    m_profileAxis = new QComboBox(page);
    m_profileAxis->addItem(u"Angular momentum of the centre"_s, QString());
    m_profileAxis->addItem(u"x"_s, u"x"_s);
    m_profileAxis->addItem(u"y"_s, u"y"_s);
    m_profileAxis->addItem(u"z"_s, u"z"_s);
    m_profileAxis->setToolTip(
            u"The polar axis of the polar and azimuthal velocity components. By default the "
            u"angular momentum of the particles in the smallest sphere about the centre that "
            u"holds 1000 of them; the radial velocity does not depend on it."_s);
    form->addRow(u"Polar axis"_s, m_profileAxis);

    m_profileColumns = new QListWidget(page);
    m_profileColumns->setMaximumHeight(110);
    m_profileColumns->setToolTip(
            u"Quantities profiled per shell besides the density: mass-weighted mean, σ and "
            u"16/50/84th percentiles. Velocity dispersions come with the profile whenever the "
            u"table has velocities."_s);
    QStringList columnChoices = numericFields;
    columnChoices << u"radial_velocity"_s;
    for (const QString &field : columnChoices) {
        auto *item = new QListWidgetItem(field, m_profileColumns);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    form->addRow(u"Quantities"_s, m_profileColumns);
    layout->addLayout(form);

    auto *showRow = new QHBoxLayout;
    showRow->addWidget(new QLabel(u"Show"_s, page));
    m_profileShow = new QComboBox(page);
    showRow->addWidget(m_profileShow, 1);
    layout->addLayout(showRow);
    QObject::connect(m_profileShow, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->plotProfile(); });

    m_profileSummary = newSummary(u"Choose a centre and compute."_s, page);
    layout->addWidget(m_profileSummary);
    m_profilePlot = newPlot(page);
    layout->addWidget(m_profilePlot, 1);

    QObject::connect(m_centreMode, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateCentreControls(); });
    QObject::connect(m_usePicked, &QPushButton::clicked, this, [this]() {
        const auto point = m_pickedPoint ? m_pickedPoint() : std::nullopt;
        if (!point) {
            m_profileSummary->setText(u"No particle is selected in the window."_s);
            return;
        }
        m_pointX->setValue((*point)[0]);
        m_pointY->setValue((*point)[1]);
        m_pointZ->setValue((*point)[2]);
    });
    this->updateCentreControls();
    return page;
}

QWidget *SimulationDiagnosticsDialog::buildPhaseTab(const QStringList &numericFields)
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    QStringList axes = numericFields;
    axes << u"radius"_s << u"radial_velocity"_s;
    const auto axisRow = [&](QPointer<QComboBox> &field, QPointer<QCheckBox> &log,
                             QPointer<QSpinBox> &bins, int preferred) {
        auto *row = new QWidget(page);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        field = new QComboBox(row);
        field->addItems(axes);
        if (preferred < field->count())
            field->setCurrentIndex(preferred);
        log = new QCheckBox(u"log"_s, row);
        log->setChecked(true);
        bins = new QSpinBox(row);
        bins->setRange(4, 512);
        bins->setValue(64);
        bins->setPrefix(u"bins "_s);
        rowLayout->addWidget(field, 1);
        rowLayout->addWidget(log);
        rowLayout->addWidget(bins);
        return row;
    };
    form->addRow(u"x"_s, axisRow(m_phaseX, m_phaseXLog, m_phaseXBins, 0));
    form->addRow(u"y"_s, axisRow(m_phaseY, m_phaseYLog, m_phaseYBins, 1));
    m_phaseWeight = new QComboBox(page);
    m_phaseWeight->addItem(u"None (particle count)"_s, QString());
    for (const QString &field : numericFields)
        m_phaseWeight->addItem(field, field);
    form->addRow(u"Weight"_s, m_phaseWeight);
    auto *note = new QLabel(u"radius and radial_velocity are measured from the centre chosen "
                            u"in the Radial profile tab. Values ≤ 0 on a log axis are dropped "
                            u"and counted."_s,
                            page);
    note->setWordWrap(true);
    note->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    form->addRow(note);
    layout->addLayout(form);
    m_phaseSummary = newSummary(u"Choose two quantities and compute."_s, page);
    layout->addWidget(m_phaseSummary);
    m_phasePlot = newPlot(page);
    layout->addWidget(m_phasePlot, 1);
    return page;
}

void SimulationDiagnosticsDialog::setCentreSources(PointSource pickedPoint,
                                                   TargetSource subfindTarget)
{
    m_pickedPoint = std::move(pickedPoint);
    m_subfindTarget = std::move(subfindTarget);
    this->updateCentreControls();
}

void SimulationDiagnosticsDialog::changeEvent(QEvent *event)
{
    // The SubFind target is another window's selection: back from there,
    // the label says which target a run would use now.
    if (event->type() == QEvent::ActivationChange && this->isActiveWindow())
        this->updateCentreControls();
    QDialog::changeEvent(event);
}

void SimulationDiagnosticsDialog::updateCentreControls()
{
    if (!m_centreMode)
        return;
    const auto mode = static_cast<profiles::CentreMode>(m_centreMode->currentData().toInt());
    const bool point = mode == profiles::CentreMode::Point;
    for (QDoubleSpinBox *box : { m_pointX.data(), m_pointY.data(), m_pointZ.data() })
        if (box)
            box->setEnabled(point);
    if (m_usePicked)
        m_usePicked->setEnabled(point && m_pickedPoint);
    if (!m_targetLabel)
        return;
    if (mode != profiles::CentreMode::Target) {
        m_targetLabel->setText(u"-"_s);
        return;
    }
    const auto target = m_subfindTarget ? m_subfindTarget() : std::nullopt;
    m_targetLabel->setText(target ? target->label
                                  : u"none: select a group or subhalo in a SubFind catalogue "
                                    u"window of this session"_s);
}

std::optional<profiles::CentreChoice> SimulationDiagnosticsDialog::chosenCentre(QString *why)
{
    profiles::CentreChoice centre;
    centre.mode = static_cast<profiles::CentreMode>(m_centreMode->currentData().toInt());
    if (centre.mode == profiles::CentreMode::Point) {
        centre.point = { m_pointX->value(), m_pointY->value(), m_pointZ->value() };
        centre.label = u"point (%1, %2, %3)"_s.arg(centre.point[0], 0, 'g', 8)
                               .arg(centre.point[1], 0, 'g', 8)
                               .arg(centre.point[2], 0, 'g', 8);
    } else if (centre.mode == profiles::CentreMode::Target) {
        // Asked now, not when the tab was built: the selection may have moved.
        const auto target = m_subfindTarget ? m_subfindTarget() : std::nullopt;
        this->updateCentreControls();
        if (!target) {
            if (why)
                *why = u"No SubFind target is selected in a catalogue window of this session."_s;
            return std::nullopt;
        }
        centre = *target;
    } else {
        centre.label = u"shrinking sphere"_s;
    }
    if (!centre.usable()) {
        if (why)
            *why = u"The centre is not complete."_s;
        return std::nullopt;
    }
    return centre;
}

void SimulationDiagnosticsDialog::updatePopulation()
{
    if (m_filters.empty()) {
        m_population->setText(u"Population: complete dataset"_s);
        return;
    }
    QStringList regions;
    for (const BackendTabularFilter &filter : m_filters)
        if (filter.isSpatial() && !filter.name.trimmed().isEmpty())
            regions << filter.name.trimmed();
    QString text = u"Population: %1 applied selection predicate(s), evaluated on the full "
                   u"dataset"_s.arg(m_filters.size());
    if (!regions.isEmpty())
        text += u" (regions: %1)"_s.arg(regions.join(u", "_s));
    m_population->setText(text);
}

SimulationDiagnosticsDialog::~SimulationDiagnosticsDialog()
{
    // Closed while a run is out: its worker cancels the task on the backend
    // and frees its thread.
    if (m_control)
        m_control->requestAbort();
}

SimulationDiagnosticsDialog::Kind SimulationDiagnosticsDialog::currentKind() const
{
    switch (m_tabs ? m_tabs->currentIndex() : 0) {
    case 1:
        return Kind::Profile;
    case 2:
        return Kind::Phase;
    default:
        return Kind::Histogram;
    }
}

QLabel *SimulationDiagnosticsDialog::summaryOf(Kind kind) const
{
    switch (kind) {
    case Kind::Profile:
        return m_profileSummary;
    case Kind::Phase:
        return m_phaseSummary;
    case Kind::Histogram:
        break;
    }
    return m_summary;
}

QCustomPlot *SimulationDiagnosticsDialog::plotOf(Kind kind) const
{
    switch (kind) {
    case Kind::Profile:
        return m_profilePlot;
    case Kind::Phase:
        return m_phasePlot;
    case Kind::Histogram:
        break;
    }
    return m_plot;
}

void SimulationDiagnosticsDialog::showProgress()
{
    if (!m_running || !m_control)
        return;
    const int percent = m_control->currentPercent();
    if (percent >= 0) {
        m_progress->setRange(0, 100);
        m_progress->setValue(percent);
    } else {
        m_progress->setRange(0, 0);
    }
    summaryOf(m_runningKind)->setText(visivo::describeTaskProgress(
            u"Computing over the complete selected population"_s, m_control->currentPhase(),
            m_elapsed.elapsed(), percent));
}

void SimulationDiagnosticsDialog::setFilters(const std::vector<BackendTabularFilter> &filters)
{
    if (visivo::selection::sameFilters(m_filters, filters))
        return;   // the same population: nothing to compute again
    m_filters = filters;
    this->updatePopulation();
    // A result of the old population next to the new population line would
    // be read as the new one's: every result is cleared, and the one under
    // way or on show computed again.
    const bool rerun = m_running || (currentKind() == Kind::Histogram && m_hasHistogram)
            || (currentKind() == Kind::Profile && m_hasProfile)
            || (currentKind() == Kind::Phase && m_hasPhase);
    const Kind kind = m_running ? m_runningKind : currentKind();
    this->clearAllResults();
    if (!rerun)
        return;
    if (const std::optional<Job> job = this->buildJob(kind)) {
        this->start(*job);
        return;
    }
    // The run cannot be set up again (its centre is gone, say). The one
    // under way, or queued, was built for the old population: stopped, so
    // its answer is not shown under the new population line.
    if (m_running) {
        const QString why = summaryOf(kind)->text();
        this->cancel();
        summaryOf(kind)->setText(u"Stopped: the selection changed and this computation could "
                                 u"not be set up again. %1"_s.arg(why));
    }
}

void SimulationDiagnosticsDialog::clearResult(Kind kind)
{
    switch (kind) {
    case Kind::Histogram:
        m_hasHistogram = false;
        break;
    case Kind::Profile:
        m_hasProfile = false;
        break;
    case Kind::Phase:
        m_hasPhase = false;
        break;
    }
    if (QCustomPlot *plot = plotOf(kind)) {
        plot->clearPlottables();
        plot->clearItems();
        // The phase plot's colour scale and axis titles belong to the map
        // cleared: an empty layout, not a bar for nothing.
        if (kind == Kind::Phase) {
            plot->plotLayout()->clear();
            delete m_phaseMargins;
        }
        plot->replot();
    }
    this->updateSaveButtons();
}

void SimulationDiagnosticsDialog::clearAllResults()
{
    for (Kind kind : { Kind::Histogram, Kind::Profile, Kind::Phase })
        this->clearResult(kind);
}

void SimulationDiagnosticsDialog::updateSaveButtons()
{
    const Kind kind = currentKind();
    const bool has = kind == Kind::Histogram ? m_hasHistogram
            : kind == Kind::Profile          ? m_hasProfile
                                             : m_hasPhase;
    if (m_saveCsv)
        m_saveCsv->setEnabled(has);
    if (m_savePng)
        m_savePng->setEnabled(has);
}

void SimulationDiagnosticsDialog::run()
{
    this->runKind(currentKind());
}

std::optional<SimulationDiagnosticsDialog::Job> SimulationDiagnosticsDialog::buildJob(Kind kind)
{
    Job job;
    job.kind = kind;
    job.filters = m_filters;
    if (kind == Kind::Histogram) {
        if (m_field->currentText().isEmpty())
            return std::nullopt;
        job.histogram.datasetId = m_datasetId;
        job.histogram.field = m_field->currentText();
        job.histogram.weightField = m_weight->currentData().toString();
        job.histogram.bins = m_bins->value();
        job.histogram.filters = m_filters;
        return job;
    }
    if (kind == Kind::Profile) {
        QString why;
        const auto centre = this->chosenCentre(&why);
        if (!centre) {
            m_profileSummary->setText(why);
            return std::nullopt;
        }
        profiles::ProfileRequest &profile = job.profile;
        profile.datasetId = m_datasetId;
        profile.centre = *centre;
        profile.rMin = m_rMin->value() > 0.0 ? m_rMin->value() : kNaN;
        profile.rMax = m_rMax->value() > 0.0 ? m_rMax->value() : kNaN;
        if (std::isfinite(profile.rMin) && std::isfinite(profile.rMax)
            && profile.rMin >= profile.rMax) {
            m_profileSummary->setText(u"The inner radius must be smaller than the outer one."_s);
            return std::nullopt;
        }
        profile.bins = m_profileBins->value();
        const QString axis = m_profileAxis->currentData().toString();
        if (axis == u"x"_s)
            profile.axis = std::array<double, 3>{ 1.0, 0.0, 0.0 };
        else if (axis == u"y"_s)
            profile.axis = std::array<double, 3>{ 0.0, 1.0, 0.0 };
        else if (axis == u"z"_s)
            profile.axis = std::array<double, 3>{ 0.0, 0.0, 1.0 };
        for (int i = 0; i < m_profileColumns->count(); ++i)
            if (m_profileColumns->item(i)->checkState() == Qt::Checked)
                profile.columns << m_profileColumns->item(i)->text();
        return job;
    }
    profiles::PhaseRequest &phase = job.phase;
    phase.datasetId = m_datasetId;
    phase.x = { m_phaseX->currentText(), m_phaseXLog->isChecked(), m_phaseXBins->value() };
    phase.y = { m_phaseY->currentText(), m_phaseYLog->isChecked(), m_phaseYBins->value() };
    phase.weightField = m_phaseWeight->currentData().toString();
    if (profiles::needsCentre(phase.x.field) || profiles::needsCentre(phase.y.field)) {
        QString why;
        const auto centre = this->chosenCentre(&why);
        if (!centre) {
            m_phaseSummary->setText(why);
            return std::nullopt;
        }
        phase.centre = *centre;
    }
    return job;
}

void SimulationDiagnosticsDialog::runKind(Kind kind)
{
    // Validated and built now, from the controls as they are: a run that
    // has to wait for the previous one goes out as it was asked.
    const std::optional<Job> job = this->buildJob(kind);
    if (job)
        this->start(*job);
}

void SimulationDiagnosticsDialog::start(const Job &job)
{
    // One request out at a time: a run given up on is dropped first, and
    // this one goes out when its worker has returned.
    if (m_inFlight) {
        if (m_control)
            m_control->requestAbort();
        ++m_runId;   // the old answer is not this one
        if (m_running && m_runningKind != job.kind)
            summaryOf(m_runningKind)->setText(u"Stopped: another computation was started."_s);
        m_rerunQueued = true;
        m_queuedJob = job;
        m_running = true;
        m_runningKind = job.kind;
        m_run->setEnabled(false);
        m_progress->setRange(0, 0);
        m_progress->setVisible(true);
        m_cancel->setVisible(true);
        m_tick->stop();
        summaryOf(job.kind)->setText(u"Waiting for the previous run to stop…"_s);
        return;
    }

    const quint64 runId = ++m_runId;
    m_running = true;
    m_runningKind = job.kind;
    m_inFlight = true;
    m_control = std::make_shared<visivo::BackendTaskControl>();
    m_run->setEnabled(false);
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    m_cancel->setVisible(true);
    m_elapsed.start();
    m_tick->start();
    summaryOf(job.kind)->setText(u"Computing over the complete selected population…"_s);
    auto *watcher = new QFutureWatcher<Outcome>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, runId]() {
        watcher->deleteLater();
        m_inFlight = false;
        if (runId == m_runId) {
            this->finishRun();
            this->present(watcher->result());
        } else if (!m_rerunQueued) {
            m_run->setEnabled(true);   // cancelled: free to compute again
        }
        if (m_rerunQueued) {
            m_rerunQueued = false;
            this->start(m_queuedJob);
        }
    });
    watcher->setFuture(QtConcurrent::run([url = m_backendUrl, token = m_backendToken,
                                          session = m_sessionId, job,
                                          control = m_control]() {
        BackendClient client(url, token);
        client.setSessionId(session);
        Outcome outcome;
        outcome.kind = job.kind;
        outcome.phaseAsked = job.phase;
        if (job.kind == Kind::Histogram)
            outcome.histogram =
                    client.catalogueDiagnosticsFollowingTask(job.histogram, control.get());
        else if (job.kind == Kind::Profile)
            outcome.profile =
                    client.catalogueProfileFollowingTask(job.profile, job.filters, control.get());
        else
            outcome.phase = client.cataloguePhaseDiagramFollowingTask(job.phase, job.filters,
                                                                      control.get());
        return outcome;
    }));
}

void SimulationDiagnosticsDialog::finishRun()
{
    m_running = false;
    m_tick->stop();
    m_progress->setVisible(false);
    m_cancel->setVisible(false);
    // Enabled again once the worker of a cancelled run has returned.
    m_run->setEnabled(!m_inFlight);
}

void SimulationDiagnosticsDialog::cancel()
{
    if (!m_running)
        return;
    if (m_control)
        m_control->requestAbort();   // the worker sends DELETE to the task
    ++m_runId;                       // and its answer, if one comes, is ignored
    m_rerunQueued = false;
    const Kind kind = m_runningKind;
    this->finishRun();
    this->clearResult(kind);
    summaryOf(kind)->setText(u"Cancelled."_s);
}

void SimulationDiagnosticsDialog::present(const Outcome &outcome)
{
    switch (outcome.kind) {
    case Kind::Histogram:
        this->presentHistogram(outcome.histogram);
        break;
    case Kind::Profile:
        this->presentProfile(outcome.profile);
        break;
    case Kind::Phase:
        if (outcome.phase.valid)
            m_phaseAsked = outcome.phaseAsked;
        this->presentPhase(outcome.phase);
        break;
    }
    this->updateSaveButtons();
}

void SimulationDiagnosticsDialog::presentHistogram(const BackendCatalogueDiagnosticsResponse &result)
{
    if (result.cancelled) {
        m_summary->setText(u"Cancelled."_s);
        this->clearResult(Kind::Histogram);
        return;
    }
    if (!result.valid) {
        m_summary->setText(u"Diagnostics failed: %1"_s.arg(result.error));
        this->clearResult(Kind::Histogram);   // what is plotted is not this population's
        return;
    }
    m_histogram = result;
    m_hasHistogram = true;
    QString text = u"%L1 selected rows; %L2 finite. %3 [%4]: min %5, max %6, mean %7, σ %8. %9."_s
            .arg(result.selectedRows).arg(result.finiteRows).arg(result.field)
            .arg(result.fieldUnit).arg(result.minimum, 0, 'g', 6)
            .arg(result.maximum, 0, 'g', 6).arg(result.mean, 0, 'g', 6)
            .arg(result.stddev, 0, 'g', 6).arg(result.weighting);
    if (result.weightedMean)
        text += u" Weighted mean %1; total weight %2 [%3]."_s
                        .arg(*result.weightedMean, 0, 'g', 6)
                        .arg(result.weightSum.value_or(0.0), 0, 'g', 6)
                        .arg(result.weightUnit);
    text += u" Selection: %1."_s.arg(result.selection.join(u" AND "_s));
    m_summary->setText(text);
    m_plot->clearPlottables();
    auto *bars = new QCPBars(m_plot->xAxis, m_plot->yAxis);
    QVector<double> centres, widths;
    centres.reserve(result.counts.size());
    widths.reserve(result.counts.size());
    for (int i = 0; i < result.counts.size(); ++i) {
        centres.append(0.5 * (result.binEdges[i] + result.binEdges[i + 1]));
        widths.append(result.binEdges[i + 1] - result.binEdges[i]);
    }
    bars->setData(centres, result.counts);
    if (!widths.isEmpty())
        bars->setWidth(widths.front());
    bars->setPen(primaryPen(1.0));
    bars->setBrush(QColor(QString::fromLatin1(VisivoTheme::kPrimary())).lighter(125));
    m_plot->xAxis->setLabel(u"%1 [%2]"_s.arg(result.field, result.fieldUnit));
    m_plot->yAxis->setLabel(result.weightField.isEmpty()
                                   ? u"Particle count"_s
                                   : u"Sum of %1 [%2]"_s.arg(result.weightField, result.weightUnit));
    m_plot->rescaleAxes();
    m_plot->replot();
}

void SimulationDiagnosticsDialog::presentProfile(const profiles::ProfileResult &result)
{
    if (result.cancelled) {
        m_profileSummary->setText(u"Cancelled."_s);
        this->clearResult(Kind::Profile);
        return;
    }
    if (!result.valid) {
        m_profileSummary->setText(u"Profile failed: %1"_s.arg(result.error));
        this->clearResult(Kind::Profile);
        return;
    }
    m_profile = result;
    m_hasProfile = true;
    const QString lengthUnit = profiles::unitOf(result.units, u"radius"_s);
    QString text = u"%L1 selected particles; centre %2 (%3, %4, %5)"_s
                           .arg(result.selectedRows)
                           .arg(result.centreMode)
                           .arg(result.centre[0], 0, 'g', 8)
                           .arg(result.centre[1], 0, 'g', 8)
                           .arg(result.centre[2], 0, 'g', 8);
    if (!lengthUnit.isEmpty())
        text += u" [%1]"_s.arg(lengthUnit);
    text += u"; %1 shells from %2 to %3"_s.arg(result.rMid.size())
                    .arg(result.rMin, 0, 'g', 5)
                    .arg(result.rMax, 0, 'g', 5);
    if (std::isfinite(result.completeRadius))
        text += u", complete out to %1 (outer shells, hollow, reach past the selection or half "
                u"the box)"_s.arg(result.completeRadius, 0, 'g', 5);
    text += u". %L1 inside r_min, %L2 outside r_max."_s.arg(result.insideRMinCount)
                    .arg(result.outsideRMaxCount);
    if (result.velocity.present) {
        text += u" Velocities are peculiar (no Hubble flow), relative to the centre velocity %1;"
                u" polar axis %2."_s
                        .arg(profiles::vectorText(result.velocity.centreVelocity),
                             profiles::vectorText(result.velocity.axis));
    }
    text += u" Values in the table's units as it states them (comoving and /h where the run "
            u"uses them); the CSV gives the factors to physical units."_s;
    const QString note = result.provenance.value(u"code_units_note"_s).toString();
    m_profileSummary->setToolTip(note);
    m_profileSummary->setText(text);

    const QString previous = m_profileShow->currentData().toString();
    const QSignalBlocker blocker(m_profileShow);
    m_profileShow->clear();
    m_profileShow->addItem(u"Density"_s, u"density"_s);
    m_profileShow->addItem(u"Enclosed mass"_s, u"enclosed_mass"_s);
    for (const profiles::ColumnProfile &c : result.columns)
        m_profileShow->addItem(u"%1 (median, 16–84%)"_s.arg(c.name), u"column:"_s + c.name);
    if (result.velocity.present) {
        m_profileShow->addItem(u"Velocity dispersion σ_r, σ_θ, σ_φ"_s, u"sigma"_s);
        m_profileShow->addItem(u"Anisotropy β"_s, u"beta"_s);
    }
    const int keep = m_profileShow->findData(previous);
    m_profileShow->setCurrentIndex(keep >= 0 ? keep : 0);
    this->plotProfile();
}

void SimulationDiagnosticsDialog::plotProfile()
{
    QCustomPlot *plot = m_profilePlot;
    if (!plot)
        return;
    plot->clearPlottables();
    plot->clearItems();
    if (!m_hasProfile) {
        plot->replot();
        return;
    }
    const profiles::ProfileResult &r = m_profile;
    const QString show = m_profileShow->currentData().toString();
    const QString lengthUnit = profiles::unitOf(r.units, u"radius"_s);
    plot->xAxis->setLabel(profiles::axisTitle(u"r"_s, lengthUnit));
    plot->xAxis->setScaleType(QCPAxis::stLogarithmic);
    plot->xAxis->setTicker(QSharedPointer<QCPAxisTickerLog>::create());

    // Complete shells drawn solid, the outer ones past the selection hollow:
    // their density is missing the volume outside it.
    const auto addCurve = [&](const QVector<double> &y, const QColor &colour,
                              const QString &name) {
        QVector<double> xs, ys, xo, yo;
        for (qsizetype i = 0; i < r.rMid.size() && i < y.size(); ++i) {
            if (!std::isfinite(y[i]))
                continue;
            const bool complete = i < r.complete.size() && r.complete[i];
            (complete ? xs : xo).append(r.rMid[i]);
            (complete ? ys : yo).append(y[i]);
        }
        auto *solid = plot->addGraph();
        solid->setName(name);
        solid->setData(xs, ys);
        solid->setPen(QPen(colour, 1.6));
        solid->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, 5));
        if (!xo.isEmpty()) {
            auto *open = plot->addGraph();
            open->setName(name + u" (incomplete)"_s);
            open->setData(xo, yo);
            open->setPen(QPen(colour, 1.0, Qt::DashLine));
            open->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, colour,
                                                  Qt::NoBrush, 5));
        }
    };
    const QColor primary(QString::fromLatin1(VisivoTheme::kPrimary()));
    bool logY = true;
    QString yTitle;
    if (show == u"density"_s) {
        addCurve(r.density, primary, u"density"_s);
        yTitle = profiles::axisTitle(u"ρ"_s, profiles::unitOf(r.units, u"density"_s));
    } else if (show == u"enclosed_mass"_s) {
        addCurve(r.enclosedMass, primary, u"M(<r)"_s);
        yTitle = profiles::axisTitle(u"M(<r)"_s, profiles::unitOf(r.units, u"mass"_s));
    } else if (show == u"sigma"_s) {
        addCurve(r.velocity.sigmaR, primary, u"σ_r"_s);
        addCurve(r.velocity.sigmaTheta, QColor(230, 140, 40), u"σ_θ"_s);
        addCurve(r.velocity.sigmaPhi, QColor(90, 180, 90), u"σ_φ"_s);
        yTitle = profiles::axisTitle(u"σ, peculiar"_s, profiles::unitOf(r.units, u"velocity"_s));
        logY = false;
    } else if (show == u"beta"_s) {
        addCurve(r.velocity.beta, primary, u"β"_s);
        yTitle = u"β = 1 − (σ_θ² + σ_φ²) / 2σ_r²"_s;
        logY = false;
    } else if (show.startsWith(u"column:"_s)) {
        const QString name = show.mid(7);
        for (const profiles::ColumnProfile &c : r.columns) {
            if (c.name != name)
                continue;
            addCurve(c.median, primary, u"median"_s);
            addCurve(c.p16, primary.lighter(140), u"16th percentile"_s);
            addCurve(c.p84, primary.lighter(140), u"84th percentile"_s);
            yTitle = profiles::axisTitle(
                    name == u"radial_velocity"_s ? u"radial velocity, peculiar"_s : name,
                    profiles::unitOf(r.units, name));
            // Log only for a positive quantity spanning decades.
            double lo = std::numeric_limits<double>::infinity(), hi = 0.0;
            for (double v : c.median)
                if (std::isfinite(v)) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            logY = lo > 0.0 && hi / lo > 100.0;
        }
    }
    plot->yAxis->setLabel(yTitle);
    if (logY) {
        plot->yAxis->setScaleType(QCPAxis::stLogarithmic);
        plot->yAxis->setTicker(QSharedPointer<QCPAxisTickerLog>::create());
    } else {
        plot->yAxis->setScaleType(QCPAxis::stLinear);
        plot->yAxis->setTicker(QSharedPointer<QCPAxisTicker>::create());
    }
    plot->legend->setVisible(plot->graphCount() > 1);
    plot->rescaleAxes();
    plot->replot();
}

void SimulationDiagnosticsDialog::presentPhase(const profiles::PhaseResult &result)
{
    if (result.cancelled) {
        m_phaseSummary->setText(u"Cancelled."_s);
        this->clearResult(Kind::Phase);
        return;
    }
    if (!result.valid) {
        m_phaseSummary->setText(u"Phase diagram failed: %1"_s.arg(result.error));
        this->clearResult(Kind::Phase);
        return;
    }
    m_phase = result;
    m_hasPhase = true;
    QString text = profiles::closureText(result, !m_phaseAsked.weightField.isEmpty());
    if (profiles::closureGap(result) != 0)
        text += u" The totals do not close: %L1 rows unaccounted for."_s.arg(
                profiles::closureGap(result));
    m_phaseSummary->setText(text);

    QCustomPlot *plot = m_phasePlot;
    plot->clearPlottables();
    plot->clearItems();
    plot->plotLayout()->clear();
    delete m_phaseMargins;
    auto *rect = new QCPAxisRect(plot);
    plot->plotLayout()->addElement(0, 0, rect);
    const qsizetype nx = result.xEdges.size() - 1;
    const qsizetype ny = result.yEdges.size() - 1;
    const bool xLog = m_phaseAsked.x.log;
    const bool yLog = m_phaseAsked.y.log;
    // On a log axis the edges are uniform in log10: the map is drawn there
    // and the axis says so. The range is that of the cell centres.
    const auto coord = [](double v, bool log) { return log ? std::log10(v) : v; };
    const auto centre = [&coord](const QVector<double> &edges, qsizetype i, bool log) {
        return 0.5 * (coord(edges[i], log) + coord(edges[i + 1], log));
    };
    auto *map = new QCPColorMap(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
    map->data()->setSize(int(nx), int(ny));
    map->data()->setRange(QCPRange(centre(result.xEdges, 0, xLog), centre(result.xEdges, nx - 1, xLog)),
                          QCPRange(centre(result.yEdges, 0, yLog), centre(result.yEdges, ny - 1, yLog)));
    // Phase diagrams span decades: shown as log10 of the weight (or count),
    // with empty bins blank.
    const bool weighted = !m_phaseAsked.weightField.isEmpty();
    const auto &values = weighted ? result.weights : result.counts;
    for (qsizetype i = 0; i < nx; ++i)
        for (qsizetype j = 0; j < ny; ++j) {
            const double v = values[i][j];
            map->data()->setCell(int(i), int(j), v > 0.0 ? std::log10(v) : kNaN);
        }
    map->setInterpolate(false);
    auto *scale = new QCPColorScale(plot);
    plot->plotLayout()->addElement(0, 1, scale);
    scale->setType(QCPAxis::atRight);
    map->setColorScale(scale);
    map->setGradient(QCPColorGradient::gpThermal);
    map->rescaleDataRange(true);
    scale->axis()->setLabel(weighted
                                    ? u"log10 Σ %1%2"_s.arg(
                                              m_phaseAsked.weightField,
                                              result.weightUnit.isEmpty()
                                                      ? QString()
                                                      : u" [%1]"_s.arg(result.weightUnit))
                                    : u"log10 particle count"_s);
    m_phaseMargins = new QCPMarginGroup(plot);
    rect->setMarginGroup(QCP::msBottom | QCP::msTop, m_phaseMargins);
    scale->setMarginGroup(QCP::msBottom | QCP::msTop, m_phaseMargins);
    const QString xField = m_phaseAsked.x.field;
    const QString yField = m_phaseAsked.y.field;
    rect->axis(QCPAxis::atBottom)->setLabel(
            (xLog ? u"log10 "_s : QString()) + profiles::axisTitle(xField, result.xUnit));
    rect->axis(QCPAxis::atLeft)->setLabel(
            (yLog ? u"log10 "_s : QString()) + profiles::axisTitle(yField, result.yUnit));
    map->rescaleAxes();
    plot->replot();
}

void SimulationDiagnosticsDialog::savePng()
{
    const Kind kind = currentKind();
    const QString name = kind == Kind::Histogram ? u"selection-diagnostics.png"_s
            : kind == Kind::Profile             ? u"radial-profile.png"_s
                                                : u"phase-diagram.png"_s;
    const QString path = QFileDialog::getSaveFileName(this, u"Save diagnostics plot"_s, name,
                                                      u"PNG image (*.png)"_s);
    if (!path.isEmpty() && plotOf(kind))
        plotOf(kind)->savePng(path, 1600, 1000, 1.0, -1);
}

void SimulationDiagnosticsDialog::saveCsv()
{
    const Kind kind = currentKind();
    const QString name = kind == Kind::Histogram ? u"selection-diagnostics.csv"_s
            : kind == Kind::Profile             ? u"radial-profile.csv"_s
                                                : u"phase-diagram.csv"_s;
    const QString path = QFileDialog::getSaveFileName(this, u"Save diagnostics table"_s, name,
                                                      u"CSV table (*.csv)"_s);
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, u"Save diagnostics"_s, file.errorString());
        return;
    }
    QTextStream out(&file);
    if (kind == Kind::Profile) {
        out << profiles::profileCsv(m_profile);
        return;
    }
    if (kind == Kind::Phase) {
        out << profiles::phaseCsv(m_phase, !m_phaseAsked.weightField.isEmpty());
        return;
    }
    out << "# field," << m_histogram.field << '\n';
    out << "# field_unit," << m_histogram.fieldUnit << '\n';
    out << "# weighting," << m_histogram.weighting << '\n';
    out << "# selection," << m_histogram.selection.join(" AND ") << '\n';
    out << "bin_low,bin_high,count\n";
    for (int i = 0; i < m_histogram.counts.size(); ++i)
        out << QString::number(m_histogram.binEdges[i], 'g', 17) << ','
            << QString::number(m_histogram.binEdges[i + 1], 'g', 17) << ','
            << QString::number(m_histogram.counts[i], 'g', 17) << '\n';
}
