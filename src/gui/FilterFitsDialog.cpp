// FilterFitsDialog.cpp — see FilterFitsDialog.h.

#include "FilterFitsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;

FilterFitsDialog::FilterFitsDialog(const QString &path, double pixelArcsec, int width, int height,
                                   const QString &backendUrl, const QString &backendToken,
                                   QWidget *parent)
    : QDialog(parent),
      m_path(path),
      m_pixelArcsec(pixelArcsec),
      m_width(width),
      m_height(height),
      m_backendUrl(backendUrl),
      m_backendToken(backendToken)
{
    setWindowTitle(u"Smooth / Regrid"_s);
    buildUi();
    connect(&m_watcher, &QFutureWatcher<BackendFilterFitsResult>::finished, this,
            &FilterFitsDialog::onFinished);
    updateSummary();
}

FilterFitsDialog::~FilterFitsDialog()
{
    // The work runs on the backend and finishes there whether or not this
    // dialog is listening; nothing is waited for, only disconnected.
    m_watcher.disconnect(this);
}

void FilterFitsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *source = new QLabel(
            u"%1\n%2 × %3 pixels%4"_s.arg(QFileInfo(m_path).fileName())
                    .arg(m_width)
                    .arg(m_height)
                    .arg(m_pixelArcsec > 0 ? u", %1″/pixel"_s.arg(m_pixelArcsec, 0, 'f', 2)
                                           : QString()),
            this);
    source->setWordWrap(true);
    layout->addWidget(source);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);

    m_smoothBox = new QCheckBox(u"Gaussian smoothing"_s, this);
    m_smoothBox->setChecked(true);
    form->addRow(m_smoothBox);

    m_fwhm = new QDoubleSpinBox(this);
    m_fwhm->setRange(0.1, 500.0);
    m_fwhm->setDecimals(2);
    m_fwhm->setValue(3.0);
    m_fwhm->setSuffix(u" px"_s);
    m_fwhm->setToolTip(u"Kernel FWHM in pixels. The beam in the output header is "
                       u"widened by this in quadrature."_s);
    form->addRow(u"FWHM"_s, m_fwhm);

    m_regridBox = new QCheckBox(u"Regrid (shrink)"_s, this);
    form->addRow(m_regridBox);

    m_factor = new QSpinBox(this);
    m_factor->setRange(2, 64);
    m_factor->setValue(2);
    m_factor->setPrefix(u"÷ "_s);
    form->addRow(u"Factor"_s, m_factor);

    m_combine = new QComboBox(this);
    // The default reads BUNIT on the backend, which is the only place that
    // knows it. Averaging a Jy/pixel map loses flux by the square of the
    // factor, and the legacy always averaged.
    m_combine->addItem(u"Automatic (from BUNIT)"_s, u"auto"_s);
    m_combine->addItem(u"Mean — surface brightness (Jy/beam, MJy/sr)"_s, u"mean"_s);
    m_combine->addItem(u"Sum — per pixel (Jy/pixel)"_s, u"sum"_s);
    form->addRow(u"Combine"_s, m_combine);

    m_outputName = new QLineEdit(this);
    m_outputName->setPlaceholderText(u"derived from the input name"_s);
    form->addRow(u"Save as"_s, m_outputName);

    layout->addLayout(form);

    m_summary = new QLabel(QString(), this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_status = new QLabel(QString(), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(u"Apply"_s);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &FilterFitsDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    connect(m_smoothBox, &QCheckBox::toggled, this, &FilterFitsDialog::updateSummary);
    connect(m_regridBox, &QCheckBox::toggled, this, &FilterFitsDialog::updateSummary);
    connect(m_fwhm, &QDoubleSpinBox::valueChanged, this, &FilterFitsDialog::updateSummary);
    connect(m_factor, &QSpinBox::valueChanged, this, &FilterFitsDialog::updateSummary);
}

void FilterFitsDialog::updateSummary()
{
    const bool smooth = m_smoothBox && m_smoothBox->isChecked();
    const bool regrid = m_regridBox && m_regridBox->isChecked();
    if (m_fwhm)
        m_fwhm->setEnabled(smooth);
    if (m_factor)
        m_factor->setEnabled(regrid);
    if (m_combine)
        m_combine->setEnabled(regrid);
    if (m_buttons && m_buttons->button(QDialogButtonBox::Ok))
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(smooth || regrid);

    QStringList parts;
    if (smooth && m_pixelArcsec > 0) {
        parts << u"kernel %1″ FWHM"_s.arg(m_fwhm->value() * m_pixelArcsec, 0, 'f', 1);
    }
    if (regrid) {
        const int factor = m_factor->value();
        parts << u"%1 × %2 pixels"_s.arg(m_width / factor).arg(m_height / factor);
        if (m_pixelArcsec > 0)
            parts << u"%1″/pixel"_s.arg(m_pixelArcsec * factor, 0, 'f', 2);
    }
    if (!smooth && !regrid)
        parts << u"Nothing selected."_s;

    if (m_summary)
        m_summary->setText(u"Result: %1"_s.arg(parts.join(u", "_s)));
}

void FilterFitsDialog::accept()
{
    if (m_watcher.isRunning())
        return;
    const bool smooth = m_smoothBox && m_smoothBox->isChecked();
    const bool regrid = m_regridBox && m_regridBox->isChecked();
    if (!smooth && !regrid) {
        QMessageBox::warning(this, windowTitle(), u"Select at least one action."_s);
        return;
    }

    m_buttons->setEnabled(false);
    m_status->setText(u"Working on the backend…"_s);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString path = m_path;
    const double fwhm = smooth ? m_fwhm->value() : 0.0;
    const int factor = regrid ? m_factor->value() : 1;
    const QString combine = m_combine->currentData().toString();
    const QString name = m_outputName->text().trimmed();
    m_watcher.setFuture(QtConcurrent::run([url, token, path, fwhm, factor, combine, name]() {
        BackendClient client(url, token);
        return client.filterFits(path, fwhm, factor, combine, name);
    }));
}

void FilterFitsDialog::onFinished()
{
    const BackendFilterFitsResult result = m_watcher.result();
    if (!result.valid) {
        m_buttons->setEnabled(true);
        m_status->setText(QString());
        QMessageBox box(QMessageBox::Critical, windowTitle(),
                        result.error.isEmpty() ? u"The filter failed."_s : result.error,
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }

    // Say what was actually done, not what was asked: "combine" in particular
    // may have been decided from BUNIT rather than by the user.
    QStringList done;
    if (result.smoothFwhmPixels > 0) {
        done << (result.smoothFwhmArcsec > 0
                         ? u"smoothed with a %1″ FWHM kernel"_s.arg(result.smoothFwhmArcsec, 0,
                                                                    'f', 1)
                         : u"smoothed with a %1 px FWHM kernel"_s.arg(result.smoothFwhmPixels, 0,
                                                                      'f', 2));
    }
    if (result.regridFactor > 1) {
        done << u"regridded ÷%1 (%2 of each block)"_s.arg(result.regridFactor).arg(result.combine);
    }
    m_status->setText(u"%1 — %2"_s.arg(result.workspaceFilename, done.join(u", "_s)));

    if (result.beamMissing) {
        // Not a failure, but not something to discover later either: the map is
        // in Jy/beam, the beam just grew, and nothing in the header says by how
        // much — so its fluxes no longer compare with the input's.
        QMessageBox box(QMessageBox::Warning, windowTitle(),
                        u"%1 was smoothed, but its header carries no beam "
                        u"(BMAJ/BMIN) while its unit is per beam.\n\nThe values "
                        u"could not be rescaled for the wider beam, so fluxes "
                        u"measured on the result are not comparable with the "
                        u"original."_s.arg(result.workspaceFilename),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
    }

    emit filtered(result.outputPath);
    QDialog::accept();
}
