// PhysicalUnitsControl.cpp — see PhysicalUnitsControl.h.

#include "PhysicalUnitsControl.h"

#include "ElideOnResize.h"
#include "app/BackendClient.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QtConcurrent/QtConcurrent>

#include <cmath>
#include <limits>

using namespace Qt::Literals::StringLiterals;
using visivo::simvol::PhysicalUnits;
using visivo::simvol::UnitsRequest;

namespace {

/// A positive number typed in a field, NaN when it is empty, -1 when it is not
/// a positive number (so the dialog can say which field is wrong).
double typedPositive(const QLineEdit *edit)
{
    const QString text = edit->text().trimmed();
    if (text.isEmpty())
        return std::numeric_limits<double>::quiet_NaN();
    bool ok = false;
    const double v = text.toDouble(&ok);
    return (ok && std::isfinite(v) && v > 0.0) ? v : -1.0;
}

QString shown(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 8) : QString();
}

QString summary(const PhysicalUnits &u, bool physical)
{
    if (!u.hasValue() && !u.hasLength())
        return u"code units"_s;
    if (!physical)
        return u"code units (physical available)"_s;
    QStringList parts;
    if (u.hasValue())
        parts << u"values in %1"_s.arg(u.valueUnit.isEmpty() ? u"(dimensionless)"_s : u.valueUnit);
    if (u.hasLength())
        parts << u"lengths in %1"_s.arg(u.lengthUnit.isEmpty() ? u"?"_s : u.lengthUnit);
    const QString from = (u.valueSource == u"user"_s || u.lengthSource == u"user"_s)
            ? u"typed"_s : (u.origin.isEmpty() ? u"run"_s : u.origin);
    return u"%1 (%2)"_s.arg(parts.join(u", "_s), from);
}

}   // namespace

PhysicalUnitsControl::PhysicalUnitsControl(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_label = new QLabel(u"-"_s, this);
    m_label->setMinimumWidth(1);
    m_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_label, 1);
    setProperty("inspectorFillRow", true);
    m_physicalBox = new QCheckBox(u"Physical"_s, this);
    m_physicalBox->setToolTip(u"Show values, coordinates and time in physical units; off shows "
                              u"the file's code units. The data are not changed."_s);
    connect(m_physicalBox, &QCheckBox::toggled, this, [this](bool on) {
        m_physical = on;
        refresh();
        emit displayChanged(on);
    });
    layout->addWidget(m_physicalBox);
    m_editButton = new QToolButton(this);
    m_editButton->setText(u"Units…"_s);
    m_editButton->setToolTip(u"Type the run's UNIT_DENSITY, UNIT_LENGTH and UNIT_VELOCITY, or a "
                             u"factor and unit for this variable. They are kept with the run, "
                             u"and read from definitions.h when it is next to the data."_s);
    connect(m_editButton, &QToolButton::clicked, this, &PhysicalUnitsControl::askForUnits);
    layout->addWidget(m_editButton);
    refresh();
}

void PhysicalUnitsControl::setDataset(const QString &backendUrl, const QString &token,
                                      const QString &sessionId, const QString &datasetId)
{
    m_backendUrl = backendUrl;
    m_token = token;
    m_sessionId = sessionId;
    m_datasetId = datasetId;
    refresh();
}

void PhysicalUnitsControl::setUnits(const PhysicalUnits &units, bool physical)
{
    m_units = units;
    m_physical = physical;
    refresh();
}

void PhysicalUnitsControl::refresh()
{
    const bool known = m_units.hasValue() || m_units.hasLength();
    if (m_label) {
        const QString text = summary(m_units, m_physical);
        ElideOnResize::apply(m_label, text);
        m_label->setToolTip(text);
    }
    if (m_physicalBox) {
        const QSignalBlocker block(m_physicalBox);
        m_physicalBox->setChecked(m_physical);
        m_physicalBox->setVisible(known);
    }
    if (m_editButton)
        m_editButton->setEnabled(!m_backendUrl.isEmpty() && !m_datasetId.isEmpty() && !m_busy);
}

void PhysicalUnitsControl::askForUnits()
{
    // On the heap and watched, as the beam dialog: the window can close while
    // the dialog's own event loop runs.
    QPointer<PhysicalUnitsControl> self(this);
    QPointer<QDialog> dialog = new QDialog(this);
    dialog->setWindowTitle(u"Physical units"_s);
    auto *form = new QFormLayout(dialog);
    auto *hint = new QLabel(
            u"PLUTO writes code units. Give the run's units (cgs) and the pressure, time and "
            u"magnetic field follow as PLUTO derives them, or type a factor and unit for this "
            u"variable. Leave a field empty to keep what the file gives."_s,
            dialog);
    hint->setWordWrap(true);
    form->addRow(hint);

    const PhysicalUnits &u = m_units;
    auto *density = new QLineEdit(shown(u.unitDensity), dialog);
    auto *length = new QLineEdit(shown(u.unitLength), dialog);
    auto *velocity = new QLineEdit(shown(u.unitVelocity), dialog);
    density->setPlaceholderText(u"UNIT_DENSITY, g/cm^3"_s);
    length->setPlaceholderText(u"UNIT_LENGTH, cm"_s);
    velocity->setPlaceholderText(u"UNIT_VELOCITY, cm/s"_s);
    form->addRow(u"Density unit (g/cm^3)"_s, density);
    form->addRow(u"Length unit (cm)"_s, length);
    form->addRow(u"Velocity unit (cm/s)"_s, velocity);
    if (!u.origin.isEmpty())
        form->addRow(new QLabel(u"Read from %1."_s.arg(u.origin), dialog));

    auto *valueFactor = new QLineEdit(dialog);
    auto *valueUnit = new QLineEdit(dialog);
    valueFactor->setPlaceholderText(u.hasValue() ? shown(u.valueFactor) : u"factor"_s);
    valueUnit->setPlaceholderText(u.hasValue() ? u.valueUnit : u"unit, e.g. Msun/pc^3"_s);
    auto *valueRow = new QHBoxLayout();
    valueRow->addWidget(valueFactor);
    valueRow->addWidget(valueUnit);
    form->addRow(u"This variable: value ×"_s, valueRow);

    auto *lengthFactor = new QLineEdit(dialog);
    auto *lengthUnit = new QLineEdit(dialog);
    lengthFactor->setPlaceholderText(u.hasLength() ? shown(u.lengthFactor) : u"factor"_s);
    lengthUnit->setPlaceholderText(u.hasLength() ? u.lengthUnit : u"unit, e.g. pc"_s);
    auto *lengthRow = new QHBoxLayout();
    lengthRow->addWidget(lengthFactor);
    lengthRow->addWidget(lengthUnit);
    form->addRow(u"Coordinates ×"_s, lengthRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                                 | QDialogButtonBox::Reset,
                                         dialog);
    buttons->button(QDialogButtonBox::Reset)->setText(u"Forget typed"_s);
    form->addRow(buttons);
    bool forget = false;
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, dialog,
            [&forget, dialog]() {
                forget = true;
                dialog->accept();
            });
    const int answer = dialog->exec();
    if (!self || !dialog)
        return;

    UnitsRequest request;
    request.clear = forget;
    bool bad = false;
    const auto take = [&bad](const QLineEdit *edit, double &into) {
        const double v = typedPositive(edit);
        if (v < 0.0)
            bad = true;
        else if (std::isfinite(v))
            into = v;
    };
    if (answer == QDialog::Accepted && !forget) {
        take(density, request.unitDensity);
        take(length, request.unitLength);
        take(velocity, request.unitVelocity);
        take(valueFactor, request.valueFactor);
        take(lengthFactor, request.lengthFactor);
        request.valueUnit = valueUnit->text().trimmed();
        request.lengthUnit = lengthUnit->text().trimmed();
    }
    delete dialog;
    if (answer != QDialog::Accepted)
        return;
    if (bad) {
        QMessageBox::information(this, u"Physical units"_s,
                                 u"Every factor must be a positive number."_s);
        return;
    }
    // The run's own numbers, left as the dialog showed them, are not a change.
    const auto same = [](double typed, double current) {
        return std::isfinite(typed) && std::isfinite(current)
                && std::abs(typed - current) <= 1e-9 * std::abs(current);
    };
    if (same(request.unitDensity, u.unitDensity))
        request.unitDensity = std::numeric_limits<double>::quiet_NaN();
    if (same(request.unitLength, u.unitLength))
        request.unitLength = std::numeric_limits<double>::quiet_NaN();
    if (same(request.unitVelocity, u.unitVelocity))
        request.unitVelocity = std::numeric_limits<double>::quiet_NaN();
    send(request);
}

void PhysicalUnitsControl::send(const UnitsRequest &request)
{
    if (m_busy || m_backendUrl.isEmpty() || m_datasetId.isEmpty())
        return;
    m_busy = true;
    refresh();
    const QString url = m_backendUrl;
    const QString token = m_token;
    const QString session = m_sessionId;
    const QString dataset = m_datasetId;
    auto *watcher = new QFutureWatcher<visivo::simvol::UnitsResult>(this);
    QPointer<PhysicalUnitsControl> self(this);
    connect(watcher, &QFutureWatcher<visivo::simvol::UnitsResult>::finished, this,
            [self, watcher]() {
                const visivo::simvol::UnitsResult result = watcher->result();
                watcher->deleteLater();
                if (!self)
                    return;
                self->m_busy = false;
                if (!result.valid) {
                    self->refresh();
                    QMessageBox::warning(self, u"Physical units"_s, result.error);
                    return;
                }
                self->setUnits(result.units, self->m_physical);
                emit self->unitsChanged(result.units);
            });
    watcher->setFuture(QtConcurrent::run([url, token, session, dataset, request]() {
        BackendClient client(url, token);
        if (!session.isEmpty())
            client.setSessionId(session);
        return client.setDatasetUnits(dataset, request);
    }));
}
