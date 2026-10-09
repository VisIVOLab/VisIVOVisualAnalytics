// Hips2FitsDialog.cpp — see Hips2FitsDialog.h.

#include "Hips2FitsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;

Hips2FitsDialog::Hips2FitsDialog(const QString &backendUrl, const QString &backendToken,
                                 QWidget *parent)
    : QDialog(parent), m_backendUrl(backendUrl), m_backendToken(backendToken)
{
    setWindowTitle(u"HiPS cutout (hips2fits)"_s);
    buildUi();

    connect(&m_optionsWatcher, &QFutureWatcher<BackendHips2FitsOptions>::finished, this,
            &Hips2FitsDialog::onOptionsFinished);
    connect(&m_cutoutWatcher, &QFutureWatcher<BackendHips2FitsResult>::finished, this,
            &Hips2FitsDialog::onCutoutFinished);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    m_optionsWatcher.setFuture(QtConcurrent::run([url, token]() {
        BackendClient client(url, token);
        return client.hips2fitsOptions();
    }));
}

Hips2FitsDialog::~Hips2FitsDialog()
{
    m_optionsWatcher.disconnect(this);
    m_cutoutWatcher.disconnect(this);
}

void Hips2FitsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_survey = new QComboBox(this);
    m_survey->setEditable(true);   // any id from the CDS registry is accepted
    m_survey->setMinimumWidth(360);
    m_survey->setToolTip(u"A HiPS id, e.g. CDS/P/DSS2/color. The list is a starting "
                         u"point — any id the CDS registry knows works."_s);
    form->addRow(u"Survey"_s, m_survey);

    const auto degrees = [this](double minimum, double maximum, double value, int decimals) {
        auto *box = new QDoubleSpinBox(this);
        box->setRange(minimum, maximum);
        box->setDecimals(decimals);
        box->setValue(value);
        box->setSuffix(u"°"_s);
        return box;
    };
    m_ra = degrees(-360.0, 360.0, 0.0, 6);
    m_dec = degrees(-90.0, 90.0, 0.0, 6);
    m_fov = degrees(0.0001, 180.0, 1.0, 4);
    m_rotation = degrees(-360.0, 360.0, 0.0, 2);
    form->addRow(u"RA"_s, m_ra);
    form->addRow(u"Dec"_s, m_dec);
    form->addRow(u"Field of view"_s, m_fov);
    form->addRow(u"Rotation"_s, m_rotation);

    m_width = new QSpinBox(this);
    m_width->setRange(1, 20000);
    m_width->setValue(512);
    m_width->setSuffix(u" px"_s);
    m_height = new QSpinBox(this);
    m_height->setRange(1, 20000);
    m_height->setValue(512);
    m_height->setSuffix(u" px"_s);
    form->addRow(u"Width"_s, m_width);
    form->addRow(u"Height"_s, m_height);

    m_projection = new QComboBox(this);
    form->addRow(u"Projection"_s, m_projection);

    m_coordsys = new QComboBox(this);
    m_coordsys->addItem(u"ICRS (equatorial)"_s, u"icrs"_s);
    m_coordsys->addItem(u"Galactic"_s, u"galactic"_s);
    form->addRow(u"Coordinates"_s, m_coordsys);

    m_format = new QComboBox(this);
    m_format->addItem(u"FITS (data)"_s, u"fits"_s);
    m_format->addItem(u"PNG (picture)"_s, u"png"_s);
    m_format->addItem(u"JPEG (picture)"_s, u"jpg"_s);
    form->addRow(u"Format"_s, m_format);

    m_minCut = new QDoubleSpinBox(this);
    m_minCut->setRange(0.0, 100.0);
    m_minCut->setValue(0.5);
    m_minCut->setSuffix(u" %"_s);
    m_maxCut = new QDoubleSpinBox(this);
    m_maxCut->setRange(0.0, 100.0);
    m_maxCut->setValue(99.5);
    m_maxCut->setSuffix(u" %"_s);
    m_stretch = new QComboBox(this);
    form->addRow(u"Min cut"_s, m_minCut);
    form->addRow(u"Max cut"_s, m_maxCut);
    form->addRow(u"Stretch"_s, m_stretch);

    m_outputName = new QLineEdit(this);
    m_outputName->setPlaceholderText(u"named after the survey"_s);
    form->addRow(u"Save as"_s, m_outputName);

    layout->addLayout(form);

    m_status = new QLabel(u"Loading the survey list…"_s, this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(u"Fetch"_s);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &Hips2FitsDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    connect(m_format, &QComboBox::currentIndexChanged, this,
            &Hips2FitsDialog::updateFormatDependentFields);
    updateFormatDependentFields();
}

void Hips2FitsDialog::setPosition(double ra, double dec, double fovDeg)
{
    if (m_ra)
        m_ra->setValue(ra);
    if (m_dec)
        m_dec->setValue(dec);
    if (m_fov && fovDeg > 0)
        m_fov->setValue(fovDeg);
}

void Hips2FitsDialog::updateFormatDependentFields()
{
    // Cuts and stretch turn numbers into pixels; a FITS carries the numbers, so
    // they mean nothing for it. (The legacy greyed these out the same way.)
    const bool picture = m_format && m_format->currentData().toString() != u"fits"_s;
    for (QWidget *w : { static_cast<QWidget *>(m_minCut.data()),
                        static_cast<QWidget *>(m_maxCut.data()),
                        static_cast<QWidget *>(m_stretch.data()) }) {
        if (w)
            w->setEnabled(picture);
    }
}

void Hips2FitsDialog::onOptionsFinished()
{
    const BackendHips2FitsOptions options = m_optionsWatcher.result();
    if (!options.valid) {
        m_status->setText(options.error.isEmpty()
                                  ? u"The survey list could not be loaded; type an id."_s
                                  : options.error);
    } else {
        m_survey->addItems(options.surveys);
        m_projection->addItems(options.projections);
        m_stretch->addItems(options.stretches);
        m_status->setText(u"%1"_s.arg(options.endpoint));
    }
    if (m_projection->count() == 0)
        m_projection->addItems({ u"TAN"_s, u"SIN"_s, u"AIT"_s, u"CAR"_s });
    if (m_stretch->count() == 0)
        m_stretch->addItems({ u"linear"_s, u"sqrt"_s, u"log"_s, u"asinh"_s });
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void Hips2FitsDialog::accept()
{
    if (m_cutoutWatcher.isRunning())
        return;

    QVariantMap parameters;
    parameters.insert(u"hips"_s, m_survey->currentText().trimmed());
    parameters.insert(u"width"_s, m_width->value());
    parameters.insert(u"height"_s, m_height->value());
    parameters.insert(u"ra"_s, m_ra->value());
    parameters.insert(u"dec"_s, m_dec->value());
    parameters.insert(u"fov"_s, m_fov->value());
    parameters.insert(u"projection"_s, m_projection->currentText());
    parameters.insert(u"coordsys"_s, m_coordsys->currentData().toString());
    parameters.insert(u"rotation_angle"_s, m_rotation->value());
    parameters.insert(u"format"_s, m_format->currentData().toString());
    parameters.insert(u"min_cut"_s, m_minCut->value());
    parameters.insert(u"max_cut"_s, m_maxCut->value());
    parameters.insert(u"stretch"_s, m_stretch->currentText());
    parameters.insert(u"output_basename"_s, m_outputName->text().trimmed());

    m_buttons->setEnabled(false);
    m_status->setText(u"Asking the service for the cutout…"_s);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    m_cutoutWatcher.setFuture(QtConcurrent::run([url, token, parameters]() {
        BackendClient client(url, token);
        return client.hips2fits(parameters);
    }));
}

void Hips2FitsDialog::onCutoutFinished()
{
    const BackendHips2FitsResult result = m_cutoutWatcher.result();
    m_buttons->setEnabled(true);
    if (!result.valid) {
        m_status->setText(QString());
        QMessageBox box(QMessageBox::Critical, windowTitle(),
                        result.error.isEmpty() ? u"The cutout failed."_s : result.error,
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }

    m_status->setText(u"Saved as %1 (%2 kB)."_s.arg(result.workspaceFilename)
                              .arg(result.bytes / 1024));
    if (!result.isFits) {
        // A picture is not something the viewers open; saying where it is is
        // the whole of what can be offered.
        QMessageBox::information(this, windowTitle(),
                                 u"Saved in the Workspace as %1."_s.arg(result.workspaceFilename));
        QDialog::accept();
        return;
    }
    emit cutoutReady(result.outputPath);
    QDialog::accept();
}
