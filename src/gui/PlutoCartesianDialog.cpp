// PlutoCartesianDialog.cpp — see PlutoCartesianDialog.h.

#include "PlutoCartesianDialog.h"

#include "ToolProgressSheet.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;
namespace pv = visivo::plutoview;

PlutoCartesianDialog::PlutoCartesianDialog(const QString &plutoPath, const QString &backendUrl,
                                           const QString &backendToken, QWidget *parent)
    : QDialog(parent), m_path(plutoPath), m_backendUrl(backendUrl), m_backendToken(backendToken)
{
    setWindowTitle(u"Cartesian View of a PLUTO Output"_s);
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
            u"The output is stored on the cells of its own coordinates (polar, cylindrical, "
            u"spherical, or Cartesian cells of growing width). The backend resamples it onto "
            u"a uniform x-y-z grid, interpolating in the native coordinates; the result opens "
            u"like any other volume, with the run's units. The as-stored view stays as it is."_s,
            this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout();
    m_millions = new QSpinBox(this);
    m_millions->setRange(1, 1000);
    m_millions->setValue(64);
    m_millions->setSuffix(u" million voxels at most"_s);
    m_millions->setToolTip(
            u"The grid is as fine as the finest cell of the run, and coarser when that would "
            u"be more voxels than this. Each voxel is 4 bytes per variable."_s);
    form->addRow(u"Resolution"_s, m_millions);

    m_mirror = new QCheckBox(u"Mirror to negative x (axisymmetric runs)"_s, this);
    m_mirror->setToolTip(u"An axisymmetric run is a half plane; this draws its mirror image "
                         u"too, as a full meridional section."_s);
    form->addRow(QString(), m_mirror);
    m_all = new QCheckBox(u"Every variable of the output"_s, this);
    m_all->setToolTip(u"One cube per variable. Off: the variable this file holds."_s);
    form->addRow(QString(), m_all);
    layout->addLayout(form);

    m_planLabel = new QLabel(u"Grid: —"_s, this);
    m_planLabel->setWordWrap(true);
    m_planLabel->setTextFormat(Qt::RichText);
    layout->addWidget(m_planLabel);

    auto *actions = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    actions->addWidget(m_statusLabel, 1);
    m_extractButton = new QPushButton(u"Resample"_s, this);
    m_extractButton->setDefault(true);
    m_extractButton->setEnabled(false);
    actions->addWidget(m_extractButton);
    auto *close = new QPushButton(u"Close"_s, this);
    actions->addWidget(close);
    layout->addLayout(actions);

    connect(m_extractButton, &QPushButton::clicked, this, &PlutoCartesianDialog::extract);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_millions, &QSpinBox::valueChanged, this, &PlutoCartesianDialog::refreshPlan);
    connect(m_mirror, &QCheckBox::toggled, this, &PlutoCartesianDialog::refreshPlan);
    connect(&m_planWatcher, &QFutureWatcher<pv::Plan>::finished, this,
            &PlutoCartesianDialog::onPlanFinished);
    connect(&m_extractWatcher, &QFutureWatcher<pv::Result>::finished, this,
            &PlutoCartesianDialog::onExtractFinished);
    refreshPlan();
}

PlutoCartesianDialog::~PlutoCartesianDialog()
{
    visivo::closeToolProgressSheet(m_progressSheet);
}

void PlutoCartesianDialog::refreshPlan()
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
    const qint64 budget = pv::voxelBudget(m_millions->value());
    const bool mirror = m_mirror->isChecked();
    m_planWatcher.setFuture(QtConcurrent::run([path, url, token, budget, mirror]() {
        BackendClient client(url, token);
        return client.planPlutoCartesian(path, budget, mirror);
    }));
}

void PlutoCartesianDialog::onPlanFinished()
{
    if (m_planDirty) {
        refreshPlan();
        return;
    }
    const pv::Plan plan = m_planWatcher.result();
    if (!plan.valid) {
        m_planLabel->setText(u"<b>Grid:</b> %1"_s.arg(plan.error.toHtmlEscaped()));
        m_extractButton->setEnabled(false);
        return;
    }
    m_mirror->setEnabled(pv::canMirror(plan));
    m_planLabel->setText(pv::summary(plan));
    m_extractButton->setEnabled(true);
}

void PlutoCartesianDialog::extract()
{
    if (m_extractWatcher.isRunning())
        return;
    setBusy(true, u"Resampling…"_s);
    m_progressSheet = visivo::showToolProgressSheet(this, u"Resampling onto a Cartesian grid"_s);

    const QString path = m_path;
    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const qint64 budget = pv::voxelBudget(m_millions->value());
    const bool mirror = m_mirror->isChecked();
    const bool all = m_all->isChecked();
    m_extractWatcher.setFuture(QtConcurrent::run([path, url, token, budget, mirror, all]() {
        BackendClient client(url, token);
        return client.extractPlutoCartesian(path, budget, mirror, all);
    }));
}

void PlutoCartesianDialog::onExtractFinished()
{
    visivo::closeToolProgressSheet(m_progressSheet);
    setBusy(false);
    const pv::Result result = m_extractWatcher.result();
    if (!result.valid) {
        QMessageBox::warning(this, windowTitle(), result.error);
        return;
    }
    // The first variable opens; the others are listed with their paths on the
    // backend, for anyone who ticked "every variable".
    emit cubeReady(result.outputs.first().path);
    m_statusLabel->setText(u"Made %1 cube(s); %2 is open."_s.arg(result.outputs.size())
                                   .arg(result.outputs.first().field));
    if (result.outputs.size() > 1) {
        QStringList names;
        for (const pv::Cube &cube : result.outputs)
            names.append(u"%1: %2"_s.arg(cube.field, cube.path));
        QMessageBox::information(this, windowTitle(),
                                 u"One cube per variable, on the backend:\n\n%1"_s
                                         .arg(names.join(u"\n"_s)));
    }
}

void PlutoCartesianDialog::setBusy(bool busy, const QString &message)
{
    m_extractButton->setEnabled(!busy);
    if (!message.isEmpty())
        m_statusLabel->setText(message);
    else if (!busy)
        m_statusLabel->clear();
    setCursor(busy ? Qt::BusyCursor : Qt::ArrowCursor);
}
