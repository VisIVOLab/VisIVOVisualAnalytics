// AmrUniformDialog.cpp — see AmrUniformDialog.h.

#include "AmrUniformDialog.h"

#include "ToolProgressSheet.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;
namespace av = visivo::amrview;

AmrUniformDialog::AmrUniformDialog(const QString &outputPath, const QString &backendUrl,
                                   const QString &backendToken, QWidget *parent)
    : QDialog(parent), m_path(outputPath), m_backendUrl(backendUrl), m_backendToken(backendToken)
{
    setWindowTitle(u"Uniform Cube of an AMR Output"_s);
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(560);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
            u"The mesh is refined where the gas needs it, so no viewer can draw it as a grid. "
            u"The backend resamples one field onto a uniform grid at the level you choose, "
            u"with the volume-weighted mean over the cells finer than a voxel, so the mass is "
            u"conserved. The cube opens like any volume, with the run's units; the "
            u"refinement blocks can be drawn over its slices (Inspector, AMR)."_s,
            this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout();
    m_field = new QComboBox(this);
    m_field->setToolTip(u"A hydro variable, the mass of each cell, or its refinement level."_s);
    form->addRow(u"Field"_s, m_field);

    m_level = new QComboBox(this);
    m_level->setToolTip(
            u"A voxel is the size of a cell of this level: each level halves it. The finest "
            u"level that fits is the finest one whose cube stays within the voxel limit."_s);
    form->addRow(u"Level"_s, m_level);

    m_millions = new QSpinBox(this);
    m_millions->setRange(1, 4000);
    m_millions->setValue(64);
    m_millions->setSuffix(u" million voxels at most"_s);
    // The suffix is long: without a floor the form layout squeezes the box to
    // a few pixels and the number cannot be read.
    m_millions->setMinimumWidth(240);
    m_millions->setToolTip(u"The limit the level is checked against. Each voxel is 4 bytes."_s);
    form->addRow(u"Limit"_s, m_millions);

    m_regionOn = new QCheckBox(u"Only a region of the box"_s, this);
    m_regionOn->setToolTip(u"The cube covers these corners, in fractions of the box (0 to 1), "
                           u"rounded out to whole voxels."_s);
    form->addRow(QString(), m_regionOn);
    static const char *const names[3] = { "x", "y", "z" };
    for (int i = 0; i < 3; ++i) {
        auto *row = new QWidget(this);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        m_lo[i] = new QDoubleSpinBox(row);
        m_hi[i] = new QDoubleSpinBox(row);
        for (QDoubleSpinBox *box : { m_lo[i].data(), m_hi[i].data() }) {
            box->setRange(0.0, 1.0);
            box->setDecimals(4);
            box->setSingleStep(0.05);
            box->setEnabled(false);
        }
        m_lo[i]->setValue(0.0);
        m_hi[i]->setValue(1.0);
        h->addWidget(m_lo[i], 1);
        h->addWidget(new QLabel(u"to"_s, row));
        h->addWidget(m_hi[i], 1);
        form->addRow(QString::fromLatin1(names[i]), row);
        connect(m_regionOn, &QCheckBox::toggled, m_lo[i], &QWidget::setEnabled);
        connect(m_regionOn, &QCheckBox::toggled, m_hi[i], &QWidget::setEnabled);
        connect(m_lo[i], &QDoubleSpinBox::valueChanged, this, &AmrUniformDialog::refreshPlan);
        connect(m_hi[i], &QDoubleSpinBox::valueChanged, this, &AmrUniformDialog::refreshPlan);
    }
    layout->addLayout(form);

    m_planLabel = new QLabel(u"Grid: —"_s, this);
    m_planLabel->setWordWrap(true);
    m_planLabel->setTextFormat(Qt::RichText);
    // Tall enough for the whole summary at the dialog's width, whatever the
    // label's hint was when it held only "Grid: —".
    m_planLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    layout->addWidget(m_planLabel);

    auto *actions = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    actions->addWidget(m_statusLabel, 1);
    m_extractButton = new QPushButton(u"Resample"_s, this);
    m_extractButton->setDefault(true);
    m_extractButton->setEnabled(false);
    actions->addWidget(m_extractButton);
    auto *close = new QPushButton(u"Close"_s, this);
    actions->addWidget(close);
    layout->addLayout(actions);

    connect(m_extractButton, &QPushButton::clicked, this, &AmrUniformDialog::extract);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_millions, &QSpinBox::valueChanged, this, &AmrUniformDialog::refreshPlan);
    connect(m_regionOn, &QCheckBox::toggled, this, &AmrUniformDialog::refreshPlan);
    connect(m_field, &QComboBox::currentIndexChanged, this, &AmrUniformDialog::refreshPlan);
    connect(m_level, &QComboBox::currentIndexChanged, this, &AmrUniformDialog::refreshPlan);
    connect(&m_planWatcher, &QFutureWatcher<av::Plan>::finished, this,
            &AmrUniformDialog::onPlanFinished);
    connect(&m_extractWatcher, &QFutureWatcher<av::Result>::finished, this,
            &AmrUniformDialog::onExtractFinished);
    refreshPlan();
}

AmrUniformDialog::~AmrUniformDialog()
{
    visivo::closeToolProgressSheet(m_progressSheet);
}

av::Region AmrUniformDialog::region() const
{
    av::Region r;
    if (!m_regionOn->isChecked())
        return r;
    r.set = true;
    for (int i = 0; i < 3; ++i) {
        r.lo[std::size_t(i)] = m_lo[i]->value();
        r.hi[std::size_t(i)] = m_hi[i]->value();
    }
    return r;
}

void AmrUniformDialog::populateChoices(const av::Plan &plan)
{
    // Once, from the first plan: the lists are the mesh's, not the settings'.
    m_populated = true;
    const QSignalBlocker blockField(m_field.data());
    const QSignalBlocker blockLevel(m_level.data());
    m_field->clear();
    for (const QString &name : plan.fields)
        m_field->addItem(name);
    const int density = m_field->findText(u"density"_s);
    m_field->setCurrentIndex(density >= 0 ? density : 0);
    m_level->clear();
    m_level->addItem(u"Finest that fits"_s, -1);
    for (int level : plan.levels)
        m_level->addItem(u"Level %1"_s.arg(level), level);
}

void AmrUniformDialog::refreshPlan()
{
    if (m_planWatcher.isRunning()) {
        m_planDirty = true;
        return;
    }
    m_planDirty = false;
    m_extractButton->setEnabled(false);
    const QString path = m_path;
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString field = m_field->count() > 0 ? m_field->currentText() : m_firstField;
    const int level = m_level->count() > 0 ? m_level->currentData().toInt() : -1;
    const qint64 budget = av::voxelBudget(m_millions->value());
    const av::Region region = this->region();
    m_planWatcher.setFuture(QtConcurrent::run([path, url, token, field, level, budget, region]() {
        BackendClient client(url, token);
        return client.planAmrUniform(path, field, level, region, budget);
    }));
}

void AmrUniformDialog::onPlanFinished()
{
    if (m_planDirty) {
        refreshPlan();
        return;
    }
    const av::Plan plan = m_planWatcher.result();
    // A mesh with no "density" refuses the first plan, and the lists are filled
    // from it: ask again for the refinement level, which every mesh has.
    if (!plan.valid && !m_populated && m_firstField != u"level"_s) {
        m_firstField = u"level"_s;
        refreshPlan();
        return;
    }
    if (!plan.valid) {
        m_planLabel->setText(u"<b>Grid:</b> %1"_s.arg(plan.error.toHtmlEscaped()));
        m_extractButton->setEnabled(false);
        fitToContents();
        return;
    }
    if (!m_populated) {
        populateChoices(plan);
        // The first plan may have been for the refinement level (no density):
        // plan again for the field the list settled on.
        if (m_field->currentText() != plan.field) {
            refreshPlan();
            return;
        }
    }
    m_planLabel->setText(av::summary(plan));
    m_extractButton->setEnabled(plan.fits);
    fitToContents();
}

void AmrUniformDialog::fitToContents()
{
    // The summary arrives after the dialog is shown and is several lines long:
    // grow to hold it instead of clipping its last line.
    layout()->activate();
    const int width = qMax(this->width(), minimumWidth());
    const int height = qMax(layout()->totalHeightForWidth(width), layout()->totalMinimumSize().height());
    resize(width, qMax(this->height(), height));
}

void AmrUniformDialog::extract()
{
    if (m_extractWatcher.isRunning())
        return;
    setBusy(true, u"Resampling…"_s);
    m_progressSheet = visivo::showToolProgressSheet(this, u"Resampling the AMR mesh"_s);

    const QString path = m_path;
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString field = m_field->currentText();
    const int level = m_level->currentData().toInt();
    const qint64 budget = av::voxelBudget(m_millions->value());
    const av::Region region = this->region();
    m_extractWatcher.setFuture(
            QtConcurrent::run([path, url, token, field, level, budget, region]() {
                BackendClient client(url, token);
                return client.extractAmrUniform(path, field, level, region, budget);
            }));
}

void AmrUniformDialog::onExtractFinished()
{
    visivo::closeToolProgressSheet(m_progressSheet);
    setBusy(false);
    const av::Result result = m_extractWatcher.result();
    if (!result.valid) {
        QMessageBox::warning(this, windowTitle(), result.error);
        return;
    }
    emit cubeReady(result.outputs.first().path);
    m_statusLabel->setText(u"Made the %1 cube at level %2; it is open."_s
                                   .arg(result.outputs.first().field)
                                   .arg(result.plan.level));
}

void AmrUniformDialog::setBusy(bool busy, const QString &message)
{
    m_extractButton->setEnabled(!busy);
    if (!message.isEmpty())
        m_statusLabel->setText(message);
    else if (!busy)
        m_statusLabel->clear();
    setCursor(busy ? Qt::BusyCursor : Qt::ArrowCursor);
}
