// DatasetBeamControl.cpp — see DatasetBeamControl.h.

#include "DatasetBeamControl.h"

#include "app/BackendClient.h"
#include "ElideOnResize.h"
#include "app/DatasetBeam.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QToolButton>
#include <QtConcurrent/QtConcurrent>

#include <cmath>

using namespace Qt::Literals::StringLiterals;

DatasetBeamControl::DatasetBeamControl(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_label = new QLabel(u"-"_s, this);
    // Elided to the room the row has, the whole beam in the tooltip: a wrapped
    // label asked the row for a width it did not have and was cut (issue #502).
    m_label->setMinimumWidth(1);
    m_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_label, 1);
    setProperty("inspectorFillRow", true);
    m_setButton = new QToolButton(this);
    m_setButton->setText(u"Set beam…"_s);
    m_setButton->setToolTip(
            u"The header has no beam (BMAJ/BMIN). Type one so per-beam quantities and their "
            u"errors can be computed; it is kept with the file and used by every later "
            u"request on it."_s);
    connect(m_setButton, &QToolButton::clicked, this, &DatasetBeamControl::askForBeam);
    layout->addWidget(m_setButton);
    m_clearButton = new QToolButton(this);
    m_clearButton->setText(u"Clear"_s);
    m_clearButton->setToolTip(u"Forget the typed beam."_s);
    connect(m_clearButton, &QToolButton::clicked, this, &DatasetBeamControl::clearBeam);
    layout->addWidget(m_clearButton);
    refresh();
}

void DatasetBeamControl::setDataset(const QString &backendUrl, const QString &token,
                                    const QString &datasetId)
{
    m_backendUrl = backendUrl;
    m_token = token;
    m_datasetId = datasetId;
    refresh();
}

void DatasetBeamControl::setBeam(double majorDeg, double minorDeg, double paDeg,
                                 const QString &source)
{
    m_majorDeg = majorDeg;
    m_minorDeg = minorDeg;
    m_paDeg = paDeg;
    m_source = source;
    refresh();
}

void DatasetBeamControl::refresh()
{
    if (m_label) {
        const QString text = visivo::beam::summary(m_majorDeg, m_minorDeg, m_paDeg, m_source);
        ElideOnResize::apply(m_label, text);
        m_label->setToolTip(text);
    }
    const bool connected = !m_backendUrl.isEmpty() && !m_datasetId.isEmpty();
    const bool settable = visivo::beam::canSetBeam(m_source);
    if (m_setButton) {
        m_setButton->setVisible(settable);
        m_setButton->setEnabled(connected && !m_busy);
        m_setButton->setText(m_source == u"user"_s ? u"Edit…"_s : u"Set beam…"_s);
    }
    if (m_clearButton) {
        m_clearButton->setVisible(m_source == u"user"_s);
        m_clearButton->setEnabled(connected && !m_busy);
    }
}

void DatasetBeamControl::askForBeam()
{
    // On the heap and watched: the window can close while the dialog's own
    // event loop runs, and a stack dialog owned by it would be deleted twice.
    QPointer<DatasetBeamControl> self(this);
    QPointer<QDialog> dialog = new QDialog(this);
    dialog->setWindowTitle(u"Set beam"_s);
    auto *form = new QFormLayout(dialog);
    auto *hint = new QLabel(u"FWHM of the restoring beam. A circular beam needs only the "
                            u"major axis; the minor axis then equals it."_s,
                            dialog);
    hint->setWordWrap(true);
    form->addRow(hint);
    QDialog *owner = dialog;
    const auto arcsecBox = [owner](double value, double minimum) {
        auto *box = new QDoubleSpinBox(owner);
        box->setRange(minimum, 36000.0);
        box->setDecimals(4);   // an ALMA beam is 0.0228″, not 0.02″
        box->setSuffix(u" ″"_s);
        box->setValue(value);
        return box;
    };
    const bool has = visivo::beam::hasBeam(m_majorDeg, m_minorDeg);
    auto *major = arcsecBox(has ? m_majorDeg * 3600.0 : 15.0, 0.0001);
    auto *minor = arcsecBox(has ? m_minorDeg * 3600.0 : 0.0, 0.0);
    minor->setSpecialValueText(u"same as major"_s);
    auto *pa = new QDoubleSpinBox(dialog);
    pa->setRange(-180.0, 180.0);
    pa->setDecimals(1);
    pa->setSuffix(u"°"_s);
    pa->setValue(has ? m_paDeg : 0.0);
    form->addRow(u"Major axis"_s, major);
    form->addRow(u"Minor axis"_s, minor);
    form->addRow(u"Position angle (E of N)"_s, pa);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const int answer = dialog->exec();
    if (!self || !dialog)
        return;
    const double majorArcsec = major->value();
    const double minorArcsec = minor->value();
    const double paDeg = pa->value();
    delete dialog;
    if (answer != QDialog::Accepted)
        return;
    if (minorArcsec > majorArcsec) {
        QMessageBox::information(this, u"Set beam"_s,
                                 u"The minor axis cannot be larger than the major axis."_s);
        return;
    }
    send(majorArcsec, minorArcsec, paDeg, false);
}

void DatasetBeamControl::clearBeam()
{
    send(0.0, 0.0, 0.0, true);
}

void DatasetBeamControl::send(double majorArcsec, double minorArcsec, double paDeg, bool clear)
{
    if (m_busy || m_backendUrl.isEmpty() || m_datasetId.isEmpty())
        return;
    visivo::beam::BeamRequest request;
    request.datasetId = m_datasetId;
    request.majorArcsec = majorArcsec;
    request.minorArcsec = minorArcsec;
    request.paDeg = paDeg;
    request.clear = clear;
    m_busy = true;
    refresh();
    const QString url = m_backendUrl;
    const QString token = m_token;
    auto *watcher = new QFutureWatcher<visivo::beam::BeamResult>(this);
    QPointer<DatasetBeamControl> self(this);
    connect(watcher, &QFutureWatcher<visivo::beam::BeamResult>::finished, this,
            [self, watcher]() {
                const visivo::beam::BeamResult result = watcher->result();
                watcher->deleteLater();
                if (!self)
                    return;
                self->m_busy = false;
                if (!result.valid) {
                    self->refresh();
                    QMessageBox::warning(self, u"Beam"_s, result.error);
                    return;
                }
                self->setBeam(result.majorDeg, result.minorDeg, result.paDeg, result.source);
                if (!result.note.isEmpty() && self->m_label)
                    self->m_label->setToolTip(result.note);
                emit self->beamChanged(result.majorDeg, result.minorDeg, result.paDeg,
                                       result.source);
            });
    watcher->setFuture(QtConcurrent::run([url, token, request]() {
        BackendClient client(url, token);
        return client.setDatasetBeam(request);
    }));
}
